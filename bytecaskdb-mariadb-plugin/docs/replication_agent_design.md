# Replication: the plugin and the operator as one design

Status: design, not implemented. One project with two halves:
[#422](https://github.com/gustavoamigo/bytecaskdb/issues/422) is the plugin
half, and a `type: bytecaskdb` replication mode in a fork of
[mariadb-operator](https://github.com/mariadb-operator/mariadb-operator) (MIT)
is the operator half. This document defines both from one table, so that each
call the operator makes has exactly one plugin counterpart and the plugin
exposes nothing the operator does not call. Where #422's control surface
differs, this document supersedes it; the differences are listed at the end.

## Principle

The operator already drives a MariaDB node through `SET @@global`, one status
query, `read_only`, a wait function and probes. ByteCaskDB tables replicate
through the engine instead of the binlog, so the plugin answers those same
calls with the engine's primitives. The operator's code paths, state machine,
status fields, Services, recovery and resources are kept; only what the SQL
says changes, behind one interface. The plugin is shaped by the operator's
contract, not the other way round.

Safety stays where #422 put it, in the plugin: a write lease checked on every
commit, terms in the data file, followers that refuse an older term. The
operator can be late or wrong without corrupting anything. There is no
sidecar state anywhere: every node boots as a read-only follower, and the
operator tells it what to be, which is the operator's existing
`semiSyncBootAsReplica` pattern made the only mode.

## The contract

Left: the operator's call and where it lives. Middle: what it runs for binlog
replication. Right: what it runs for `type: bytecaskdb`, and what the plugin
does.

### Boot

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `init` renders `0-replication.cnf` (`pkg/controller/replication/config.go`) | `log_bin`, `server_id`, `gtid_*`, `rpl_semi_sync_*`, `read_only=ON` with `semiSyncBootAsReplica` | `read_only=ON`, `bytecaskdb_replication=ON`. No binlog. **Plugin:** sets `HA_HAS_OWN_BINLOGGING`; boots in follower mode with no source and no lease; serves reads. Nothing about replication is persisted, so a restarted pod is a follower with no source until the operator configures it. |
| `init` cleans `master.info`, `relay-log.info` | state files on disk | nothing to clean |

### Configure a replica

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `TopologyManager.ConfigureReplica` | `STOP SLAVE; SET gtid_slave_pos; SET read_only=1; CHANGE MASTER TO MASTER_HOST, MASTER_PORT, MASTER_USER, MASTER_PASSWORD, MASTER_SSL_*, MASTER_CONNECT_RETRY, MASTER_USE_GTID; START SLAVE` | `SET read_only=1;` then the `ChangeMasterOpts` fields one to one: `bytecaskdb_source_host`, `_port`, `_user`, `_password`, `_ssl_ca`, `_ssl_cert`, `_ssl_key`, `_connect_retry`; then `bytecaskdb_source_enabled=1`. **Plugin:** a pull thread connects with those credentials as an ordinary client, reads the leader's entries from its own `durable_sequence`, applies them with `ingest`, cut at batch boundaries. Setting the same values again is a no-op; a changed host restarts the pull. The request carries the follower's highest term; a source on an older term is refused with `last_source_errno = 4001`. |
| `getReplicaOpts`: `gtid_slave_pos` from a snapshot annotation or the agent's `GET /replication/gtid` | needed, the position is outside the data | **not needed**: the position is the follower's own `durable_sequence`, in its data files |

### Configure the primary

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `TopologyManager.ConfigurePrimary` | `STOP SLAVE; RESET SLAVE ALL; SET gtid_slave_pos=''`; replication user DDL | `bytecaskdb_source_enabled=0; SET GLOBAL bytecaskdb_promote=1`. **Plugin:** refuses unless the pull is stopped, `received_sequence = applied_sequence`, and a lease is granted (when `bytecaskdb_lease_ms > 0`). Takes term = highest term seen in the log + 1 and appends the term-start entry `(term, start_seq)` as its first write. An explicit `bytecaskdb_promote=<term>` is accepted for manual operation and refused if not above the highest seen. The operator keeps no term: the most advanced follower, which is the one it promotes, has seen every term any other node has, because term-start entries are log entries. |
| `DisableReadOnly` / `EnableReadOnly` | `SET read_only` | unchanged |

### Status

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `sql.Client.ReplicaStatus` → `ReplicaStatusVars` | `SHOW REPLICA STATUS` + `@@gtid_current_pos` | `SELECT * FROM information_schema.BYTECASKDB_REPLICATION`, one row, columns named for the struct fields: `source_running`, `apply_running` (bools; a reconnecting pull reports `source_running = 1`, as `Connecting` does today), `last_source_errno`, `last_source_error`, `last_apply_errno`, `last_apply_error`, `seconds_behind_source` (**NULL when the pull is not running**, as the code's comment on `Seconds_Behind_Master` requires), `received_sequence`, `applied_sequence`, `durable_sequence`, `term`, `role` (`primary`, `replica`), `source_host`, `source_port`, `lease_remaining_ms`. `ReplicaStatusVars` becomes an interface with a binlog and a bytecaskdb implementation; positions are `uint64` behind a `Position` with `GreaterThan`. |
| `IsReplicationPrimary` | `SHOW MASTER STATUS` | `role = 'primary'` |
| `IsReplicationRunning` | both threads running | `source_running AND apply_running` |
| `status.replication.roles` | `Primary`, `Replica`, `PrimaryReplica`, `Unknown` | unchanged. A pod that restarts goes to `Unknown`, which is what makes the reconciler configure it again. |

Error numbers the recovery controller acts on, mirroring 1236:

| `last_source_errno` | Meaning | Recovery |
|---|---|---|
| 4001 | source is on an older term than this node has seen | retry: the operator is still repointing |
| 4002 | **diverged**: this node's `durable_sequence` is above the source's term-start `start_seq`, so it holds writes from a branch the leader never had | immediate, like 1236 |
| 4003 | history gone: the source has vacuumed below this node's cursor | immediate, like 1236 |
| other | connection and auth errors | after `errorDurationThreshold`, as today |

### Failover

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `ReconcilePodNotReady` (`pod_replication_controller.go`) | waits `autoFailoverDelay`, patches `podIndex` | unchanged. The webhook floors `autoFailoverDelay` at `lease.ttl + 5 s`, so the old primary's lease has expired before the new one is promoted. |
| `FurthestAdvancedReplica` (`failover.go`) | Ready; IO and SQL running; no relay-log events; highest `gtid_current_pos`; ties to the lowest name | Ready; `source_running AND apply_running`; `received_sequence = applied_sequence`; highest `durable_sequence`; ties to the lowest name |

### Switchover

The eight phases of `reconcileSwitchover`, in order:

| Phase | Binlog | ByteCaskDB |
|---|---|---|
| 1 `lockPrimaryWithReadLock` | `FLUSH TABLES WITH READ LOCK` | no-op |
| 2 `setPrimaryReadOnly` | `SET read_only=1` | `SET read_only=1; SET GLOBAL bytecaskdb_demote=1`. **Plugin:** `set_mode(Follower)`, which makes every acknowledged write durable first, and revokes the lease. |
| 3 `waitSync` | `gtid_binlog_pos` on the primary, `MASTER_GTID_WAIT(pos, timeout)` on each replica | `durable_sequence` on the primary, `SELECT BYTECASKDB_WAIT_SEQUENCE(seq, timeout_s)` on each replica. **Plugin:** the UDF is `durable_sequence(seq, timeout)` and returns `0` reached, `-1` timeout, the contract `WaitForReplicaGtid` parses. When the primary is not Ready, `waitForNewPrimarySync` polls until `received_sequence = applied_sequence`. |
| 4 `configureNewPrimary` | `ConfigurePrimary` | `ConfigurePrimary` |
| 5 `connectReplicasToNewPrimary` | `ConfigureReplica` with the new `gtid_binlog_pos` | `ConfigureReplica`, no position |
| 6 `changePrimaryToReplica` | `UNLOCK TABLES`, `ConfigureReplica` with `MASTER_DEMOTE_TO_SLAVE` | `ConfigureReplica` |
| 7 `reconcileSemiSyncSwitchover` | see below | see below |
| 8 `disableNewPrimaryReadOnly` | `SET read_only=0` | unchanged |

### Lease

No binlog counterpart. The one thing the operator does that it did not do
before.

| Operator | ByteCaskDB |
|---|---|
| `reconcileReplication`, primary branch: `SET GLOBAL bytecaskdb_lease_ms = lease.ttl`, requeue every `lease.ttl / 3` | **Plugin:** a deadline on `CLOCK_BOOTTIME`, checked inside the write path before every commit, so a process or VM paused past it refuses its next write. On expiry: `set_mode(Follower)`, `read_only=1`. `0` means no lease, for a node run without the operator. |

The renewal is a lightweight reconcile of its own so a slow phase elsewhere
(a `syncTimeout`, a backup) cannot starve it. A lease missed twice still
renews on the third attempt before `ttl`.

### Semi-sync

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| config | `rpl_semi_sync_slave_enabled=ON` everywhere, `master_enabled=OFF` | nothing: followers always acknowledge |
| `reconcileSemiSync`: master-side on for the primary only, off for replicas, converged every reconcile | `rpl_semi_sync_master_enabled`, `_timeout`, `_wait_no_slave` | `bytecaskdb_sync_enabled`, `bytecaskdb_sync_timeout_ms`, `bytecaskdb_sync_wait_no_follower`, same semantics. **Plugin:** each pull request carries the follower's `durable_sequence`, which is the acknowledgement. A commit waits until some follower has reported at least its sequence, or `sync_timeout_ms`, after which it falls back to asynchronous and reports `bytecaskdb_sync_status = 'async'` in the status table, as `Rpl_semi_sync_master_status` does. |

### Recovery and bootstrap

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `mariadb_controller_replica_recovery.go`: 1236 at once, other errors after `errorDurationThreshold` | | 4002 and 4003 at once, others after the threshold |
| `bootstrapFrom.volumeSnapshotRef`: delete the PVC, recreate it from the snapshot | needs the GTID from the snapshot annotation | **unchanged and position-free**: a crash-consistent copy of the data directory is a valid database, the newest file's torn tail is truncated at open, and the follower tails from its own `durable_sequence`. |
| `bootstrapFrom.physicalBackupTemplateRef`: a job runs `mariadb-backup` into the PVC | | the job calls `BYTECASKDB_CREATE_MANIFEST()` on the primary and copies the listed files from the primary agent's HTTP API into the PVC. **Plugin:** the UDF wraps `create_manifest()`, and the agent serves the manifest's files. |
| nothing | | **`retain_after`.** Before a snapshot or manifest, the operator sets `SET GLOBAL bytecaskdb_retain_after = <primary durable_sequence>` and keeps it until the new replica reports its own; otherwise it is the lowest `durable_sequence` across replicas, converged every reconcile like semi-sync. **Plugin:** passes it to every vacuum. |

### Probes

`pkg/agent/handler/replication/probe.go`, reading the status table through the
same interface:

| Probe | Binlog | ByteCaskDB |
|---|---|---|
| liveness, replica | a stopped thread with errno ≠ 0 fails; a stop with errno 0 is administrative | `source_running` or `apply_running` false with its errno ≠ 0 fails |
| liveness, primary | `SHOW MASTER STATUS` has a row | `role = 'primary'` |
| readiness, replica | `Seconds_Behind_Master` NULL fails; `> maxLagSeconds` fails | `seconds_behind_source` NULL fails; `> maxLagSeconds` fails |
| readiness, primary | as liveness | as liveness |

### Users, grants, databases, SQL jobs

| Operator | Binlog | ByteCaskDB |
|---|---|---|
| `pkg/controller/sql`: one client through `NewClientWithMariaDB` | applied on the primary, replicated by the binlog | applied on **every pod** through `NewInternalClientWithPodIndex`, in index order, since nothing else carries `mysql.*` to a follower. The superuser bypasses `read_only`. `CREATE DATABASE` on every pod also gives table discovery (#422 milestone 1) the directory it needs before it can create a follower's `.frm`. |

### Services, StatefulSet, TLS, MaxScale, Connections

Untouched. The primary and secondary Services follow `currentPrimaryPodIndex`
as today. MaxScale's monitor reads `SHOW SLAVE STATUS` and sees no replication
here: routing by `@@read_only` works, its automatic failover stays off.

### Spec and webhook

`spec.replication.type: binlog | bytecaskdb` (default `binlog`).
`spec.replication.lease.ttl` (default 30 s) for `bytecaskdb`. The webhook
refuses `gtid*`, `syncBinlog`, `serverIdStartIndex` and `semiSyncWaitPoint`
with `type: bytecaskdb`, requires `lease.ttl > 0`, and floors
`autoFailoverDelay` at `lease.ttl + 5 s`.

## What is not persisted, and why that is fine

The plugin keeps no `master.info`, no mode file, no term file. Source settings
live in the operator's spec and status; the term lives in the data file as
term-start entries; a node's position is its `durable_sequence`. A restarted
pod is a read-only follower with no source until the reconciler, seeing its
role as `Unknown`, configures it again. That is one reconcile later, within
`loop_wait`, and the pod is not Ready until it is. This is what makes "a
crashed leader is re-bootstrapped, never reopened in place" need no special
state: it is repointed like any replica, and if it holds a branch, it reports
4002 and recovery handles it.

## Fencing

| Layer | Stops |
|---|---|
| Plugin lease, checked per commit | an old primary partitioned from the operator, a paused process or VM, an operator that promoted early |
| Term in the data file | an old primary's writes reaching replicas; a stale node promoting itself |
| Readiness and the Services | clients through the Services reaching a demoted primary |
| The operator never force-deletes a primary pod; recovery recreates the PVC | two pods with one identity |
| Node remediation (medik8s) or the out-of-service taint | a hung node, by the deployment's choice; optional |

Clients that connect to a pod directly are stopped by the lease alone.

## Failure envelope

| Scenario | Outcome | Checked by |
|---|---|---|
| Primary pod dies | read-only for `autoFailoverDelay`, then the most advanced Ready replica is promoted; writes above its `durable_sequence` are lost (async) | operator envtest for `pod_replication_controller`; Elle topology run |
| Primary partitioned from the operator, reachable by clients | writes stop within `ttl`; promotion after `autoFailoverDelay` | envtest with a stalled renewal; Elle nemesis: leader paused past its lease |
| Operator down | primary read-only after `ttl`; nothing else | envtest |
| Primary paused past `ttl`, resumed | first commit refused | plugin test; a mutation moving the check off the commit path |
| Stale replica reappears ahead of the primary | 4002, recovered | Elle: member hidden through a promotion |
| Pull repointed to an older term | 4001, refused | plugin test; a mutation dropping the check |
| Vacuum ran below a replica's cursor | 4003, recovered; does not happen while `retain_after` is converged | Elle `cluster-vacuum-unretained` finds it, `cluster-vacuum` does not |
| Switchover | `demote` syncs, replicas wait to its `durable_sequence`, promote; no loss | Elle planned transfer; envtest for the phases |
| `lease.ttl` above `autoFailoverDelay` | refused by the webhook | envtest |

## Milestones

One list for both halves; each item is plugin work and operator work that
land together, with the row above as the test.

1. **Table discovery** (#422 milestone 1), plus the `Database` fan-out.
2. **Boot, configure, status, probes:** `HA_HAS_OWN_BINLOGGING`, follower
   boot, the `bytecaskdb_source_*` sysvars and pull thread, the status table,
   the error numbers; `type: bytecaskdb` through config, `ConfigureReplica`,
   `ReplicaStatusVars`, the probes, the SQL fan-out.
3. **Promote, terms, lease, failover:** `bytecaskdb_promote`, term-start
   entries, 4001 and 4002, the per-commit lease, `bytecaskdb_demote`;
   `ConfigurePrimary`, `FurthestAdvancedReplica`, the lease reconcile, the
   webhook rules.
4. **Switchover and retention:** `BYTECASKDB_WAIT_SEQUENCE`,
   `bytecaskdb_retain_after`, 4003; the eight phases, the retention
   reconcile.
5. **Bootstrap:** the VolumeSnapshot path end to end; `BYTECASKDB_CREATE_MANIFEST`
   and the agent's file endpoint for the job path.
6. **Semi-sync:** the three sysvars and the acknowledgement in the pull; the
   semi-sync reconcile.
7. A conversation with upstream about a replication-type seam; the fork
   tracks upstream by merge until then.

## Changes from #422

- Every node boots as a read-only follower; there is no `crashed_leader`
  state and no lease or mode persisted anywhere. The data files cannot say
  which node was leader, and the operator knows.
- `bytecaskdb_promote` takes no term by default; the plugin uses the highest
  term seen plus one. An explicit term stays for manual operation.
- `bytecaskdb_replication_source = 'host:port'` becomes the
  `bytecaskdb_source_*` sysvars mirroring `CHANGE MASTER`'s options,
  credentials and TLS included, plus `bytecaskdb_source_enabled`.
- The status table mirrors `SHOW REPLICA STATUS`'s shape: running flags,
  error numbers and text, `seconds_behind_source` NULL when not running,
  and `received_sequence` so the operator waits instead of `promote`
  refusing.
- Semi-sync has a timeout and falls back to asynchronous, as
  `rpl_semi_sync_master_timeout` does; "K followers" becomes "a follower",
  which is what the operator converges.
- `bytecaskdb_demote` and `bytecaskdb_retain_after` are added;
  `retain_after` is set by the operator, which knows about bootstraps.
- The error numbers 4001 to 4003 and the `diverged` rule are added.
- Users, grants, databases and SQL jobs are applied to every pod by the
  operator; "users and grants keep the normal binlog" is withdrawn, since
  there is no binlog.
- The multi-node harness is the operator's envtest suite plus the existing
  Elle cluster harness.
