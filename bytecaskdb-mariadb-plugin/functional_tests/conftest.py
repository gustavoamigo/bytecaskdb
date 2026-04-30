# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# conftest.py — pytest session fixtures for ha_bytecaskdb functional tests.
#
# Provisions a real mariadbd on port 3309 for the duration of the test session,
# loads the bytecaskdb plugin, and exposes a make_connection() factory fixture.

import os
import shutil
import signal
import subprocess
import time
import pytest
import pymysql


MARIADB_PORT = 3309
TEST_DIR_NAME = ".mariadb_functional_test"


def _find_bytecask_root():
    root = os.environ.get("BYTECASK_ROOT")
    if root:
        return root
    d = os.path.dirname(os.path.abspath(__file__))
    while d != os.path.dirname(d):
        if os.path.exists(os.path.join(d, "xmake.lua")) and \
           os.path.exists(os.path.join(d, "bytecaskdb-mariadb-plugin")):
            return d
        d = os.path.dirname(d)
    raise RuntimeError("Cannot find BYTECASK_ROOT; set the env variable")


def _symlink_provider_plugins(plugin_dir):
    """Symlink compression-provider .so files so mariadbd doesn't abort."""
    for system_dir in ("/usr/lib64/mariadb/plugin", "/usr/lib/mariadb/plugin"):
        if not os.path.isdir(system_dir):
            continue
        for name in os.listdir(system_dir):
            if name.startswith("provider_"):
                src = os.path.join(system_dir, name)
                dst = os.path.join(plugin_dir, name)
                if not os.path.exists(dst):
                    try:
                        os.symlink(src, dst)
                    except OSError:
                        pass
        break


class MariaDBServer:
    def __init__(self):
        self.root = _find_bytecask_root()
        self.test_dir = os.path.join(self.root, TEST_DIR_NAME)
        self.data_dir = os.path.join(self.test_dir, "data")
        self.socket_path = os.path.join(self.test_dir, "mysql.sock")
        self.pid_file = os.path.join(self.test_dir, "mariadbd.pid")
        self.log_file = os.path.join(self.test_dir, "error.log")
        self.plugin_dir = os.path.join(
            self.root, "bytecaskdb-mariadb-plugin", "build"
        )

    def start(self):
        self._cleanup_previous()
        os.makedirs(self.data_dir, exist_ok=True)
        os.makedirs(os.path.join(self.test_dir, "tmp"), exist_ok=True)

        if not os.path.exists(os.path.join(self.plugin_dir, "ha_bytecaskdb.so")):
            raise RuntimeError(
                f"Plugin not found: {self.plugin_dir}/ha_bytecaskdb.so — run cmake --build first"
            )

        subprocess.run(
            [
                "mariadb-install-db",
                f"--datadir={self.data_dir}",
                "--auth-root-authentication-method=normal",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        _symlink_provider_plugins(self.plugin_dir)

        subprocess.Popen(
            [
                "mariadbd",
                f"--datadir={self.data_dir}",
                f"--socket={self.socket_path}",
                f"--port={MARIADB_PORT}",
                f"--pid-file={self.pid_file}",
                "--skip-grant-tables",
                f"--tmpdir={self.test_dir}/tmp",
                f"--plugin-dir={self.plugin_dir}",
                f"--plugin-load-add=bytecaskdb=ha_bytecaskdb.so",
                f"--log-error={self.log_file}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        self._wait_for_connection()

    def stop(self):
        if os.path.exists(self.pid_file):
            with open(self.pid_file) as f:
                pid = int(f.read().strip())
            try:
                os.kill(pid, signal.SIGTERM)
                time.sleep(2)
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def _cleanup_previous(self):
        if os.path.exists(self.pid_file):
            try:
                with open(self.pid_file) as f:
                    pid = int(f.read().strip())
                os.kill(pid, signal.SIGTERM)
                time.sleep(2)
                os.kill(pid, signal.SIGKILL)
            except (OSError, ValueError):
                pass
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def _wait_for_connection(self, timeout=30):
        for _ in range(timeout):
            time.sleep(1)
            try:
                conn = pymysql.connect(
                    unix_socket=self.socket_path,
                    user="root",
                    password="",
                    connect_timeout=1,
                )
                conn.close()
                return
            except Exception:
                pass
        raise RuntimeError(
            f"mariadbd did not start within {timeout}s — check {self.log_file}"
        )


@pytest.fixture(scope="session")
def mariadb_server():
    server = MariaDBServer()
    server.start()
    yield server
    server.stop()


@pytest.fixture(scope="session")
def make_connection(mariadb_server):
    """Returns a factory that opens a fresh autocommit=True PyMySQL connection."""
    def _make():
        return pymysql.connect(
            unix_socket=mariadb_server.socket_path,
            user="root",
            password="",
            autocommit=True,
            charset="utf8mb4",
        )
    return _make


@pytest.fixture
def connection(make_connection):
    conn = make_connection()
    yield conn
    conn.close()
