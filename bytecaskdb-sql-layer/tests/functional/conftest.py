# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The functional suite's fixtures and run modes
(docs/functional_testing_design.md, *How a run works*).

The gate is the default: a case marked not implemented is skipped, the rest
must pass. `--conformance` runs every case, marked or not, records each
outcome and doesn't fail the run on a failed case.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "server"))
import pgserver  # noqa: E402  (tests/server/pgserver.py)

SUITES = ("sqltest", "postgresql", "bytecaskdb", "tprocc")
DATABASE = "functional"


def pytest_addoption(parser):
    group = parser.getgroup("functional")
    group.addoption("--conformance", action="store_true",
                    help="run every case, marked not implemented or not; failed cases don't fail the run")
    group.addoption("--suite", action="append", choices=SUITES,
                    help="run only this suite (repeatable); default all")
    group.addoption("--results", metavar="FILE", help="write each case's outcome to FILE as JSON")


def pytest_configure(config):
    # A feature's id is a marker, so `-k F041` runs a feature's cases. Our
    # own ids have a colon, which a registered marker's name can't have.
    config.addinivalue_line("filterwarnings", "ignore::pytest.PytestUnknownMarkWarning")
    if config.getoption("results"):
        config.pluginmanager.register(_Results(config), "functional-results")


def pytest_collection_modifyitems(config, items):
    wanted = config.getoption("suite")
    if not wanted:
        return
    kept, dropped = [], []
    for item in items:
        case = _case_of(item)
        (kept if case is None or case.suite in wanted else dropped).append(item)
    config.hook.pytest_deselected(items=dropped)
    items[:] = kept


def pytest_sessionfinish(session, exitstatus):
    # A conformance run measures: a failed case is a result, not a failure.
    if session.config.getoption("conformance") and exitstatus == pytest.ExitCode.TESTS_FAILED:
        session.exitstatus = pytest.ExitCode.OK


def _case_of(item):
    callspec = getattr(item, "callspec", None)
    return callspec.params.get("case") if callspec else None


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    report = yield
    report.case = _case_of(item)
    handle = getattr(item, "server_handle", None)
    if report.failed and handle is not None:
        report.sections.append(("sql_layer_server log", "".join(handle.server.output[-300:])))
    return report


class _Results:
    """results.json: one entry per case with its suite, id, features, whether
    it is marked not implemented, and its outcome: pass, fail or skip."""

    def __init__(self, config):
        self.config = config
        self.entries: dict[str, dict] = {}

    def pytest_runtest_logreport(self, report):
        case = getattr(report, "case", None)
        if case is None:
            return
        entry = self.entries.setdefault(report.nodeid, {
            "suite": case.suite,
            "id": case.id,
            "features": list(case.features),
            "marked": case.unimplemented,
            "outcome": "pass",
        })
        # A failure in any phase, a server that didn't start included, is the case's failure.
        if report.failed:
            entry["outcome"] = "fail"
        elif report.skipped and entry["outcome"] != "fail":
            entry["outcome"] = "skip"

    def pytest_sessionfinish(self, session):
        out = {"conformance": bool(self.config.getoption("conformance")),
               "results": list(self.entries.values())}
        pathlib.Path(self.config.getoption("results")).write_text(json.dumps(out, indent=1) + "\n")


class ServerHandle:
    """A sql_layer_server on a directory of its own."""

    def __init__(self, directory: pathlib.Path, flags: tuple[str, ...]):
        self.directory = directory
        self.flags = list(flags)
        self.server = pgserver.Server(str(directory), database=DATABASE, extra_args=self.flags)

    def dsn(self) -> str:
        return self.server.dsn(user=DATABASE)

    def restart(self) -> None:
        status = self.server.stop()
        assert status == 0, f"sql_layer_server did not stop cleanly: exit status {status}"
        self.server = pgserver.Server(str(self.directory), database=DATABASE, extra_args=self.flags)

    def stop(self) -> None:
        self.server.stop()


class Servers:
    """One server per file, in order; a case with `restart` or `server_flags`
    gets one of its own."""

    def __init__(self, base: pathlib.Path):
        self.base = base
        self.count = 0
        self.shared: tuple[str, ServerHandle] | None = None

    def _start(self, flags: tuple[str, ...]) -> ServerHandle:
        self.count += 1
        directory = self.base / f"db{self.count}"
        directory.mkdir()
        return ServerHandle(directory, flags)

    def acquire(self, case) -> ServerHandle:
        if case.own_server:
            return self._start(case.server_flags)
        if self.shared is not None and self.shared[0] != case.group:
            self.close()
        if self.shared is None:
            self.shared = (case.group, self._start(()))
        return self.shared[1]

    def release(self, case, handle: ServerHandle) -> None:
        if case.own_server:
            handle.stop()
            shutil.rmtree(handle.directory, ignore_errors=True)

    def close(self) -> None:
        if self.shared is not None:
            self.shared[1].stop()
            shutil.rmtree(self.shared[1].directory, ignore_errors=True)
            self.shared = None


@pytest.fixture(scope="session")
def servers(tmp_path_factory):
    if pgserver.server_binary() is None:
        pytest.skip(f"{pgserver.SERVER_ENV} is not set")
    pool = Servers(tmp_path_factory.mktemp("servers"))
    yield pool
    pool.close()
