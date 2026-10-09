# Replication operator: a `bytecaskdb` replication type for mariadb-operator

Status: design, not implemented. Implements the "external operator" of
[#422](https://github.com/gustavoamigo/bytecaskdb/issues/422) by forking
[mariadb-operator](https://github.com/mariadb-operator/mariadb-operator) (MIT)
and adding a replication type whose SQL is the plugin's control surface instead
of the binlog's. Where this document and #422 differ, this document wins; the
differences are listed at the end.

## Shape

#422 as written: **the plugin enforces, the operator decides.** The plugin
refuses to write without an unexpired lease, refuses a promotion with a term it
has seen, refuses to pull from an older term, and comes up refusing to serve
after an unclean shutdown as leader. The operator renews the leader's lease,
notices a dead primary, picks the most advanced follower, promotes it, repoints
the others, and re-bootstraps the old one.

mariadb-operator already does the second half for binlog replication: a
reconciler that configures a primary and its replicas, a pod controller that
fails over when the primary pod stops being Ready, an eight-phase switchover
driven by `spec.replication.primary.podIndex`, a per-pod agent that answers the
probes from replication state, primary and secondary Services, replica
recovery, and MaxScale, Users, Grants and Databases as resources. The fork
keeps all of it and changes what the SQL says.

A central operator is enough. CloudNativePG runs the same shape in production.
The operator's own availability is controller-runtime's leader election over a
Deployment; if every replica of it is down, the leader's lease runs out and the
cluster goes read-only until the operator returns. That is the cost #422
accepted, and the plugin lease is what keeps an operator outage, or an operator
mistake, from corrupting anything.

## What the plugin provides

#422's control surface, with three additions:

| Mechanism | Notes |
|---|---|
| `SET GLOBAL bytecaskdb_replication_source = 'host:port'` | Starts or stops the pull thread. The pull carries the follower's highest term; a source on an older term is refused. |
| `SET GLOBAL bytecaskdb_promote = <term>` | Refused unless the pull loop is stopped, everything received is applied, the term is above any seen, and a lease is granted. Its first write is the term-start entry `(term, start_seq)`. |
| `SET GLOBAL bytecaskdb_demote = 1` | **New.** `set_mode(Follower)`, which makes every acknowledged write durable, then `@@read_only = 1` and the lease revoked. |
| `SET GLOBAL bytecaskdb_lease_ms = <ms>` | A deadline on `CLOCK_BOOTTIME`, **checked on every commit**, not in a background thread, so a process or VM paused past it refuses its next write. `0` revokes. |
| `SET GLOBAL bytecaskdb_retain_after = <seq>` | **New.** Passed to every vacuum. The operator sets it from the lowest sequence any follower or bootstrap needs. |
| `bytecaskdb_sync_followers`, `bytecaskdb_sync_count` | **New, later milestone.** A commit returns once `sync_count` of the named followers report `durable_sequence() ≥ seq`. |
| `information_schema.BYTECASKDB_REPLICATION` | `role`, `term`, `applied_sequence`, `durable_sequence`, `source`, `source_term`, `lag_seconds`, `lease_remaining_ms`, `state` (`running`, `crashed_leader`, `diverged`), `last_error`. |
| `BYTECASKDB_WAIT_SEQUENCE(seq, timeout_ms)` | UDF over `durable_sequence(seq, timeout)`. |

**Diverged.** A follower reads its source's term-start entry. If its own
`durable_sequence()` is above that `start_seq`, it holds writes from a branch
the new leader never had: it stops pulling and reports `diverged`. The
operator recovers it like any faulty replica. This is the fork the Elle cluster
check found, made visible when a stale member reappears.

**Crashed leader.** A server restarting after an unclean shutdown as leader
comes up as a follower reporting `crashed_leader` and refuses to serve until
re-bootstrapped. The operator recovers it. A clean restart of the leader is a
switchover first, as the operator's update strategy already does.

## The fork

A new value `spec.replication.type: bytecaskdb` (default `binlog`), read by
the places below. Everything not listed is untouched.

| Operator piece | Does today | With `type: bytecaskdb` |
|---|---|---|
| `pkg/controller/replication/topology.go` `ConfigurePrimary` | `STOP SLAVE`, `RESET SLAVE`, reset `gtid_slave_pos` | `bytecaskdb_promote = status.replication.term + 1`, then `read_only = 0`; records the term in status |
| `ConfigureReplica` | `CHANGE MASTER TO …`, `START SLAVE`, `read_only = 1` | `read_only = 1`, `bytecaskdb_replication_source = <primary FQDN>:3306` |
| `pkg/sql` `ReplicaStatus`, `IsReplicationPrimary`, `GtidCurrentPos` | `SHOW REPLICA STATUS`, `SHOW MASTER STATUS`, `@@gtid_current_pos` | One query on `BYTECASKDB_REPLICATION`; a `ReplicationStatus` interface with the binlog and bytecaskdb implementations behind it |
| `failover.go` `FurthestAdvancedReplica` | Ready, IO and SQL threads running, no relay-log events, highest GTID | Ready, `state = running`, highest `applied_sequence`; ties to the lowest pod name, as today |
| `switchover.go`, eight phases | `FLUSH TABLES WITH READ LOCK`, `read_only`, `MASTER_GTID_WAIT`, configure new primary, connect replicas, demote old, semi-sync, `read_only = 0` | Phase 1 is a no-op. Phase 2 is `bytecaskdb_demote`, which syncs. Phase 3 waits with `BYTECASKDB_WAIT_SEQUENCE(old.durable_sequence)` on the target. Phases 4 to 6 as above. Phase 7 is the sync-followers sysvars. Phase 8 unchanged. |
| `internal/controller/pod_replication_controller.go` `ReconcilePodNotReady` | Waits `autoFailoverDelay`, picks the replica, patches `podIndex` | Unchanged, except `autoFailoverDelay` is floored at `lease_ms + margin` so the old primary's lease has run out before the new one is promoted |
| `reconcileReplication`, primary branch | Converges semi-sync | **Also renews the lease:** `bytecaskdb_lease_ms = ttl`, and requeues every `ttl / 3`. Sets `bytecaskdb_retain_after`. |
| `pkg/agent/handler/replication/probe.go` | Liveness from IO and SQL thread errors, readiness from `Seconds_Behind_Master ≤ maxLagSeconds` | Liveness: `state = running` or the pod is primary with a lease. Readiness: `lag_seconds ≤ maxLagSeconds`; a `bootstrapping`, `diverged` or `crashed_leader` node is not ready |
| `mariadb_controller_replica_recovery.go`, `bootstrapFrom` | Error 1236 or a persistent error triggers a `PhysicalBackup` restore or a PVC from a `VolumeSnapshot` | `diverged` or `crashed_leader` triggers the same flow. The **VolumeSnapshot path works unchanged**: a crash-consistent copy of the data directory is a valid database, and the newest file's torn tail is truncated at open. The copy is opened as a follower and tails from its own `durable_sequence`. The `PhysicalBackup` job path becomes a `create_manifest` copy served by the primary's agent over its existing HTTP API, for clusters without a snapshot class. |
| `pkg/controller/replication/config.go` | Renders `log_bin`, `server_id`, GTID and semi-sync settings | Renders none of them; the plugin sets `HA_HAS_OWN_BINLOGGING` |
| `semi_sync.go` | `rpl_semi_sync_*` | The sync-followers sysvars, later milestone |
| Services, StatefulSet, TLS, Users, Grants, Databases, Connections, MaxScale resources | | Untouched |

Vacuum retention: before taking a snapshot or manifest for a bootstrap, the
operator sets `retain_after` on the primary to its current `durable_sequence`
and keeps it there until the new replica reports its own; afterwards it is the
lowest `durable_sequence` across replicas. This is the ordering the Elle
harness settled on.

## Timing

| Setting | Default | Meaning |
|---|---|---|
| `spec.replication.lease.ttl` | 30 s | The lease granted on each renewal. `0` turns the lease off, for manual operation only. |
| renew period | `ttl / 3` | The primary branch's requeue. Two missed renewals still leave a third. |
| `autoFailoverDelay` | floored at `ttl + 5 s` | A promotion never starts before the old primary's lease has expired. |
| `syncTimeout` | 10 s | Phase 3 of a switchover, as today. |
| `maxLagSeconds` | 0 | As today. |

An operator outage longer than `ttl` makes the primary read-only until the
operator is back and renews. That is the trade: no failsafe mode, no quorum,
one controller.

## Fencing

| Layer | Stops |
|---|---|
| Plugin lease, per commit | an old primary partitioned from the operator, a paused process or VM, an operator that promoted too early |
| Term in the data file | an old primary's writes reaching replicas; a stale node promoting itself |
| Readiness and the Services | clients that go through the Services reaching a demoted primary |
| The operator's own rule: no force-delete of a primary pod; recovery recreates the PVC | two pods with one identity |
| Node remediation (medik8s) or the out-of-service taint | a hung node, by the deployment's choice; optional |

Clients that connect to a pod directly are stopped by the lease alone, as in
#422.

## Failure envelope

| Scenario | Outcome | Checked by |
|---|---|---|
| Primary pod dies | Read-only for `autoFailoverDelay`, then the most advanced Ready replica is promoted with the next term. Writes above its `durable_sequence` are lost (async). | the operator's envtest suite for `pod_replication_controller`, extended; the Elle topology run for the data |
| Primary partitioned from the operator, reachable by clients | Writes stop within `ttl`; a replica is promoted after `autoFailoverDelay`. | envtest with a stalled renewal; Elle nemesis: leader paused past its lease |
| Operator down | Primary goes read-only after `ttl`; nothing else. | envtest |
| Primary paused past `ttl` and resumed | First commit refused. | plugin test: per-commit deadline; a mutation moving the check off the commit path |
| Stale replica reappears ahead of the primary | Reports `diverged`, recovered by the operator. | Elle: member hidden through a promotion |
| Old primary's pull repointed to an older term | Refused. | plugin test; mutation dropping the check |
| Switchover | `demote` syncs, target waits to the old primary's `durable_sequence`, promotes. No loss. | Elle planned transfer; envtest for the phases |
| Lease configured above `autoFailoverDelay` | Refused by the webhook. | envtest |

## Milestones

1. Table discovery. Unchanged from #422.
2. Plugin surface: `HA_HAS_OWN_BINLOGGING`, the sysvars above, the status
   table, the UDF, terms, the per-commit lease, the diverged and
   crashed-leader states.
3. The fork: `type: bytecaskdb` through topology, status, failover,
   switchover, probes and config; lease renewal and retention in the
   reconciler; envtest coverage of the rows above.
4. Bootstrap: the VolumeSnapshot path verified end to end; the manifest copy
   over the agent for the job path.
5. Sync followers.
6. A conversation with upstream about a replication-type seam. Until then
   the fork tracks upstream by merge; the touched files are the ones in the
   table.

## Limitations

- MaxScale's MariaDB Monitor reads `SHOW SLAVE STATUS` and sees no
  replication. Routing by `@@read_only` still works; its automatic failover
  must stay off, and the operator's `MaxScale` resource is used without it.
- Replication is asynchronous until milestone 5, and loses writes acknowledged
  after the promoted replica's `durable_sequence`.
- A deployment outside Kubernetes has no operator here; the plugin surface is
  the same and a script can drive it by hand.

## Changes from #422

- The lease is not optional under the operator, and it is checked on every
  commit, not expired by a thread.
- `bytecaskdb_demote`, `bytecaskdb_retain_after` and the sync-followers
  sysvars are added. `retain_after` is set by the operator from what the
  replicas and bootstraps need, not by the leader from its connected pullers.
- The diverged-follower rule is added to the term-start entry.
- The operator is a fork of mariadb-operator, not a bespoke one; the
  multi-node harness becomes the operator's envtest suite plus the existing
  Elle cluster harness.
