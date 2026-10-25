> `AGENTS.md` is a symlink to this file, so every coding agent reads the same instructions. Edit `CLAUDE.md`; don't replace the symlink with a copy.

# SQL layer (`bytecaskdb-sql-layer/`)

These instructions add to the repository's [`CLAUDE.md`](../CLAUDE.md), which still applies: its workflow, commit rules, and documentation guidelines. Work here updates this directory's documents, not the engine's (`bytecaskdb-sql-layer/docs/` and `CONTRACT.md`).

## Keep `README.md` current

[`README.md`](README.md) is this layer's entry point: what it is, what it does today, what it refuses. **Update it in every PR that adds, removes or changes a feature**, in the same PR:

- **Current state:** move an item between *Built* and *Not built yet* when the work changes it: a statement, a clause, a type, an aggregate, a refusal turned into support, an interface, a flag.
- **Conformance table:** refresh it from a `--conformance` run (`tests/functional/report.py`) whenever cases, marks or the layer change what passes. Copy the report's numbers; don't estimate them.
- **Run it** and **Tests:** a new server flag, build target or test suite goes in the table or the flag list.
- **Where things are:** a new directory or document gets a row.

This is the exception to the repository rule that the root `README.md` changes only when the pitch does: this one is a status page, and a stale status page is wrong. Check it against the code, not the PR description: the *Not built yet* list is the dialect's *Refused* list in `docs/frontend_design.md` and #534, plus the gaps `tests/functional/` marks. Say what is true today, name no number a run has not measured, and keep it short: detail belongs in `docs/`, which the README links.

## Where the rest is

- Design, by area: `docs/*.md`. Each states its status at the top; keep that line true.
- Guarantees: `CONTRACT.md` says what the layer guarantees, with the test proving each row. A change to a guarantee is named in the PR: added, widened, narrowed or removed. CI checks its vocabulary against `algebra/bc_plan.h` and `api/database.cppm`.
- Tests: `docs/testing_design.md`. The functional suite in `tests/functional/` has its own design, `docs/functional_testing_design.md`.
- A statement the layer can't run yet is a gap, not a decision: mark the functional case `unimplemented: true` (with the refusal as the reason) and don't open an issue per case.
