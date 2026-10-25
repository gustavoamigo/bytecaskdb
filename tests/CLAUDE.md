# Tests (`tests/`)

> `AGENTS.md` is a symlink to this file. The root `CLAUDE.md` has the rules for the whole repository (testing basics, workflow, commits); this file adds the ones specific to the engine's tests, and is read when you work in this folder.

## Fault sweeps must reach every structural transition

A fault sweep proves only the paths its workload reaches with the fault armed. Line and MC/DC coverage show a branch ran, not that it ran with each of its allocations failed. The engine's sweep missed #580 because its seed was one leaf no operation filled (#588).

- A sweep over a tree, or over anything else that restructures itself (split, rebuild, collapse, a join of parts built in parallel), includes writes that make every such transition while the structure owns the nodes involved, and writes that make it on nodes it must first copy.
- The sweep asserts its own reach rather than trusting the seed: it counts the transitions its writes make (`btree_detail::Transition`) and requires each to happen in a write that an injected failure then ended (`sweep_partial_writes` in `tests/alloc_sweep_test.cpp`). A workload that stops reaching one fails the test instead of passing silently.
- A new structural transition gets a `Transition` value and joins a sweep's required set.

## Model-based recovery tests

The `[model]` test cases in `tests/bytecask_test.cpp` (random, batch-heavy, delete-heavy, range-delete, and many-frame-hint workloads, plus the vacuum tombstone model) are the primary soundness checks for the DB engine. They run thousands of random operations, close the DB, then reopen under serial and parallel recovery and verify that all key/values and per-file `file_stats` (live_bytes, total_bytes) match a serial baseline.

Update or extend the model-based tests when a change could affect what a recovered DB contains or reports. Common examples:
- Alters recovery logic (parsing, merging, fan-in, file_stats computation).
- Adds a new write-path feature (new operation type, new on-disk format field) that must survive recovery.
- Changes how `file_stats` are computed or propagated.
- Modifies the parallel merge strategy (thread counts, merge order, tombstone handling).

This list is not exhaustive — use judgment. If a change touches anything that could make serial and parallel recovery diverge, add or update coverage.

When extending, follow the existing pattern: collect a serial baseline outside the SECTIONs, copy the sealed DB to an isolated directory per SECTION, reopen with the target thread count, and CHECK both key/values and `collect_stats(db) == serial_stats_vals`.

## Mutation sets

`tests/soak_mutations/` and `tests/chaos_mutations/` hold deliberate engine breaks that prove the soak and the chaos rig still catch what they should (`scripts/soak_mutation_check.sh`, `scripts/chaos_mutation_check.sh`). `tests/durability_mutations/` holds breaks of durability sites the rig does not reach. `docs/correctness_validation.md`, *Durability sites*, lists every site with the test that guards it and its mutation. Keep them current:
- A new durability site (an `fdatasync`, directory sync, rename, truncate or unlink of a data or hint file, a sync-before-publish ordering, a degrade on an I/O error) gets a row in that table: the test that fails if it breaks and a mutation proving it, or why neither is needed.
- Every mutation names its test with a `Guarded-by: "<Catch2 test spec>"` header (a comma in a test name is written `\,`: Catch2 reads a bare one as OR; `scripts/check_guarded_by.py` checks in CI that every spec selects a test), and the test is run once with the patch applied to show it fails; say so in the PR. A durability fix adds that test, a patch reverting the fix (in `tests/durability_mutations/`, or `tests/chaos_mutations/` if the rig reaches it) and a table row.
- A fix for a bug the soak or the chaos rig found adds a patch that reverts the fix, with `Expected: caught.` and one line on what the rig then sees.
- Patches apply by three-way merge (`scripts/mutation_patch.sh`, used by both check scripts and `ci.yml`), so a change elsewhere in the file, or to lines a little way off, merges. A patch needs regenerating only when the merge conflicts: when the targeted code changed, or a line directly next to it did. Regenerate it by making the same break and saving `git diff` (the `index` line is what lets it merge later), or delete it if its target is gone; `ci.yml` fails on a conflicting patch. When code moves verbatim to another file, the merge cannot follow it: `scripts/mutation_follow_move.sh <base> <from> <to> <patch>...` regenerates the stranded patches.
- Run the mutation check when a change could alter what it proves, and put its summary in the PR description:
  - a change to the rig (`tests/chaos/`, `tests/crash/crash_model.h`) or to the mutations themselves runs the whole set: `scripts/chaos_mutation_check.sh`;
  - a change to engine code a mutation patches runs that mutation: `scripts/chaos_mutation_check.sh 5 tests/chaos_mutations/<name>.patch` (a soak mutation: `scripts/soak_mutation_check.sh <address|thread> 120 tests/soak_mutations/<name>.patch`).
  Other changes need no run: the set only goes stale when the rig or the code it mutates changes, which is why it is not scheduled.
- A mutation whose result changes gets its `Expected:` header updated with the reason. One that other code now covers is kept as `Expected: NOT caught` with that reason, not deleted.
