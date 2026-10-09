# Replication agent: a Patroni-shaped operator for ByteCaskDB tables

Status: design, not implemented. Tracks
[#422](https://github.com/gustavoamigo/bytecaskdb/issues/422), whose plugin
control surface this keeps, and whose "external operator" this defines. Where
this document and #422 differ, this document wins; the differences are listed
at the end.

## Purpose

#422 delegates replication of ByteCaskDB tables to the engine and leaves four
decisions to something outside the plugin: who leads, when to fail over, where
clients go, and when a dead leader is re-bootstrapped. This document is that
something.

It is shaped after [Patroni](https://github.com/patroni/patroni): a small agent
next to every server, one lease in a store that already has consensus, and a
server that stops writing on its own when the lease is not renewed. Patroni's
design has run PostgreSQL clusters for a decade with no proof and a written
failure envelope. This design borrows the envelope and the rules that produced
it, and writes its own envelope the way the engine's correctness docs do: each
failure class, the layer that stops it, the test that would notice if it did
not.

No consensus is built here. The one consensus the design needs, "who holds the
lease", is rented from the Kubernetes API server, whose etcd has a TLA+
specification and trace validation. Everything the agent and plugin add is a
compare-and-swap on one object, a monotonic timer, and a comparison of terms.

## What is borrowed

| From | Taken | Left behind |
|---|---|---|
| Patroni | Lease loop and timing rule (`loop_wait + 2·retry_timeout ≤ ttl`), self-demotion on failed renewal, the watchdog armed below the lease, DCS failsafe mode, pause mode, the health endpoints load balancers route by, switchover and failover with candidate checks, the `/sync` set that makes synchronous-mode promotion safe | PostgreSQL: timelines (terms replace them), `pg_rewind` (re-bootstrap only), replication slots (`retain_after`), the Postgres-specific bootstrap methods |
| Kubernetes | `coordination.k8s.io/v1 Lease` as the lock, pod annotations as member state, a ConfigMap as dynamic configuration, StatefulSet identity, label-selected Services for routing, probes answered by the agent; client-go's `resourcelock.LeaseLock` for the Lease reads and writes | client-go's leader election loop: it disclaims fencing, and the term must travel in the same update as the holder, so the loop is ours |
| CloudNativePG | The same shape in production for PostgreSQL: a per-pod manager, a Kubernetes Lease the instance must hold before it promotes, no external store. Two rules: a candidate takes an expired Lease only after watching it unchanged for a full `ttl` on its own clock, and a primary that can reach neither the store nor its peers fails its own liveness | The operator deciding the target, and fencing by shutting the process down: the plugin lease is stronger |
| mariadb-operator | The agent infrastructure for a MariaDB pod, imported as Go packages: HTTP router and probe server, TokenReview and basic auth, TLS from the pod environment, rate limiting | Its failover, which is GTID-based, and its controller |
| Vitess | Durability policies: a `cross_cell` synchronous mode whose acknowledging followers must sit in another zone than the leader | — |
| medik8s | Node-level remediation with a watchdog, as the Kubernetes form of Patroni's watchdog | — |
| LiteFS | The shape of the lease interface: acquire, renew, hand off | Consul, which is BSL |

## Components

```
 pod bytecaskdb-0                     pod bytecaskdb-1                 pod bytecaskdb-2
 ┌──────────────────────┐            ┌──────────────────────┐         ┌──────────────┐
 │ mariadb + plugin     │◄── SQL ───►│ mariadb + plugin     │         │     ...      │
 │  leader, lease 25 s  │ pull thread│  follower            │         │              │
 ├──────────────────────┤            ├──────────────────────┤         ├──────────────┤
 │ agent                │            │ agent                │         │ agent        │
 └──────────┬───────────┘            └──────────┬───────────┘         └──────┬───────┘
            │ renew (CAS)                       │ watch, publish state         │
            ▼                                   ▼                             ▼
     ┌─────────────────────────────── Kubernetes API server ───────────────────────────┐
     │ Lease  bytecaskdb-leader   holder=bytecaskdb-0  ttl=30s  term=7                  │
     │ Pods   annotations: role, term, applied, durable, source, lag, reported_at       │
     │ ConfigMap  bytecaskdb-config: ttl, loop_wait, retry_timeout, sync mode, pause    │
     │ Services  bytecaskdb-primary → role=primary   bytecaskdb-replica → role=replica  │
     └──────────────────────────────────────────────────────────────────────────────────┘
```

**The plugin** replicates and enforces. It pulls `changes_since` from its
source over a client connection and applies it with `ingest`; it refuses writes
without an unexpired lease; it refuses a promotion with a term it has already
seen; it refuses to pull from a source on an older term; it comes up refusing
to serve after an unclean shutdown as leader. All of this is #422's
*the plugin enforces* list, with the additions in *Plugin control surface*
below. The plugin knows nothing of Kubernetes. It is driven over SQL.

**The agent** decides. One per server, as a sidecar container in the same pod.
It speaks SQL to its own server and HTTPS to the API server, and serves a small
HTTP API for probes, peers and operators. It holds no state that survives a
restart: everything it needs is in the API server or in its server's status
table.

It is a Go program, `bytecaskdb-agent`, in its own module and directory
outside `bytecaskdb-mariadb-plugin/`, MIT-licensed since it includes no
MariaDB headers. It imports `k8s.io/client-go` for the Lease and the pod
annotations, and from `github.com/mariadb-operator/mariadb-operator/v26/pkg`
the packages its own agent is built from: `agent/router`, `agent/server` and
`agent/handler` for the HTTP API and the probe server, `http` for responses,
and `environment` for the pod environment. Those give the TokenReview and
basic-auth paths, TLS from the pod's mounted certificates and rate limiting
without writing them. The acquire-and-renew loop is not client-go's: the term
has to change in the same update as the holder, and client-go's loop owns the
update. The loop is a few hundred lines on top of `resourcelock.LeaseLock`.
The MariaDB operator's controller is not used; the manifests are ours. A mode
in that operator can come later and would own nothing the agents rely on.

**The store** is the Kubernetes API server, behind a `Leaser` interface
(acquire, renew, release, hand off, read members) so that an etcd backend can be
added for deployments outside Kubernetes without touching the loop.

**No central controller.** Correctness does not need one, and a process that
all nodes depend on is the availability cost #422 worried about. The agents are
symmetric. A controller can come later for convenience (a `ByteCaskDBCluster`
CRD that stamps out the StatefulSet), and would own nothing the agents rely on.

## State in the store

| Object | Written by | Holds |
|---|---|---|
| `Lease bytecaskdb-leader` | the holder, and a candidate on acquisition, always by CAS on `resourceVersion` | `holderIdentity` (pod name), `leaseDurationSeconds` (`ttl`), `renewTime`, `acquireTime`, `leaseTransitions`; annotation `bytecaskdb.io/term`; annotation `bytecaskdb.io/sync` (synchronous mode: the followers whose acknowledgement commits wait for) |
| each pod's annotations | that pod's agent only | `role`, `term` (highest seen), `applied_sequence`, `durable_sequence`, `source`, `lag_seconds`, `state` (`running`, `bootstrapping`, `crashed_leader`, `diverged`, `stopped`), `reported_at`, `bootstrap_from` (a joining node: the manifest's `through_sequence`) |
| `ConfigMap bytecaskdb-config` | operators | the dynamic configuration below, and `pause` |

The term is an annotation on the Lease rather than `leaseTransitions` because
the latter is a client-maintained counter with no enforced meaning. Both are
written in the same CAS, so they cannot disagree.

A member's state is fresh if `reported_at` is within `ttl`. Only fresh members
take part in a promotion. A stale member is treated as absent, which is what
the data-file rules below need.

## The agent loop

Every `loop_wait` seconds, the agent reads the configuration, the Lease and the
members, reads its server's status table, and takes one branch.

**I hold the Lease.** Renew it by CAS. On success, grant the plugin its lease:
`SET GLOBAL bytecaskdb_lease_ms = (ttl − safety_margin) · 1000`. Update the
`sync` annotation (synchronous mode, below). Set `bytecaskdb_retain_after` to the
lowest `durable_sequence` and `bootstrap_from` among fresh members.

If the CAS fails because another holder is recorded, demote at once:
`SET GLOBAL bytecaskdb_demote = 1`, which makes every acknowledged write durable,
switches the engine to follower mode and sets `@@read_only`. The agent never
waits for the plugin lease to expire when it already knows it has lost.

If the CAS fails because the store is unreachable, retry until `retry_timeout`
has passed. Then, with `failsafe_mode` on, ask every known member's agent over
HTTP whether it still follows this node. If every member answers yes, keep
granting the plugin lease; the primary cannot have been replaced, because
every node that could replace it has just said it is a follower. If any member
is silent, stop granting. The plugin lease runs out `ttl − safety_margin` after
the last grant and the plugin demotes itself. Patroni checks every member
rather than a quorum for the same reason this design does: members are not
placed for a quorum to mean anything, and a node on the losing side of a
partition must not keep writing.

**Another node holds an unexpired Lease.** Be a follower of it. If
`bytecaskdb_replication_source` does not name the holder, or the holder's term
is above the source's, repoint it. If the status table shows `crashed_leader`
or `diverged`, re-bootstrap from the holder. Publish state.

**The Lease is absent, or expired on this agent's clock.** Expiry is never
read off the Lease's `renewTime`, which is the holder's wall clock. The agent
records on its own monotonic clock when it first saw the current
`resourceVersion`, and the Lease counts as expired once that version has stood
unchanged for `ttl`. A renewal changes the version and restarts the count.
This is how client-go and CloudNativePG read a Lease, and it is what makes the
design free of any comparison between two nodes' clocks. Then decide whether
to run:

1. Not while `pause` is set, not with the `nofailover` tag, not from
   `crashed_leader`, `diverged` or `bootstrapping`.
2. In synchronous mode, only if this node is in the `sync` annotation.
3. Lag within `maximum_lag_on_failover`, measured against the highest
   `durable_sequence` any fresh member reported.
4. The highest `applied_sequence` among fresh members, ties broken by pod name.
   A node that sees a higher one does nothing this iteration. This is #422's
   "promote the most advanced follower", decided by each candidate from the
   same data instead of by a coordinator.

Then acquire: one CAS that sets `holderIdentity`, `acquireTime`, `renewTime`,
`leaseTransitions + 1` and `term + 1`. A lost race is a normal outcome; the
loser becomes a follower on the next iteration. The winner grants the plugin
lease, then promotes: `SET GLOBAL bytecaskdb_promote = term`. The plugin's
preconditions run (the pull loop stopped, everything received applied, the
term above any seen). If promotion fails, the agent releases the Lease so
another candidate can try, and reports the error in its state.

The old leader has stopped writing before the new one starts, without any
message between them: the old plugin lease was granted at the last renewal
for `ttl − safety_margin`; a candidate acquires no sooner than `ttl` after it
observed that renewal's version, which is after the renewal itself; so the
plugin lease expired at least `safety_margin` before any acquisition.
`safety_margin` covers the SQL round trip of the grant and the drift between
the two nodes' clock rates over one `ttl`, which is microseconds.

**On every branch**, the agent labels its pod with its role, answers probes
from its last loop, and publishes its state.

### Timing

| Parameter | Default | Meaning |
|---|---|---|
| `ttl` | 30 s | Lease duration. Also the longest a dead leader can go unreplaced. |
| `loop_wait` | 10 s | Loop period. |
| `retry_timeout` | 10 s | How long a store or SQL failure is retried before it counts. |
| `safety_margin` | 5 s | How much shorter the plugin lease is than the Lease. |
| `maximum_lag_on_failover` | unlimited | Highest lag, in sequences, a candidate may have. |
| `failsafe_mode` | off | Keep leading through a store outage while every member confirms. |
| `synchronous_mode` | off | `on`: commits wait for `synchronous_node_count` followers. `cross_zone`: the same, and the followers counted must be in another `topology.kubernetes.io/zone` than the leader. |
| `synchronous_node_count` | 1 | |
| `pause` | off | The agent renews and reports but takes no action. |

Constraint, from Patroni: `loop_wait + 2 · retry_timeout ≤ ttl`, so a leader
that hits one full retry on the store and one on SQL still renews before the
Lease expires. `safety_margin < ttl − loop_wait`, so a leader is never without a
plugin lease between two successful renewals.

### What the plugin lease measures

The plugin keeps a deadline on `CLOCK_BOOTTIME`, which keeps counting through a
system suspend, and checks it inside the write path before every commit, not
only in a background thread. A process stopped with `SIGSTOP` and resumed past
its deadline refuses the next write. A virtual machine paused by its hypervisor
sees its clock jump on resume and refuses too. What no clock in the guest can
see is a kernel that is hung and resumes later with the clock unchanged; that
is the case the node watchdog is for.

## Plugin control surface

#422's table, with these changes:

| Mechanism | Change |
|---|---|
| `bytecaskdb_lease_ms` | Granted on `CLOCK_BOOTTIME`, checked on every commit. `0` revokes. Accepted on a leader or during promotion. Not optional when the agent runs; `0` is for manual operation. |
| `bytecaskdb_promote = term` | Unchanged, plus: refused unless a lease is granted, so a leader with lease checking on never runs without one. |
| `bytecaskdb_demote` | New. `set_mode(Follower)` (which makes every acknowledged write durable), `@@read_only = 1`, lease revoked. The step-down for switchover and for a lost renewal. |
| `bytecaskdb_retain_after` | New. Set by the agent; the plugin passes it to every vacuum. Replaces "the leader passes the lowest sequence its followers hold": the plugin would only know connected pullers, and a bootstrapping node is not one yet. |
| `bytecaskdb_sync_followers`, `bytecaskdb_sync_count` | New, synchronous mode only. A commit returns once `sync_count` of the named followers report `durable_sequence() ≥ seq`. |
| `information_schema.BYTECASKDB_REPLICATION` | Adds `state` (`running`, `crashed_leader`, `diverged`), `lease_remaining_ms`, `source_term`. |
| Term-start entry | Unchanged: `(term, start_seq)` in the admin namespace, the new leader's first write. Adds the rule below. |

**Diverged followers.** A follower pulling from a source on term T reads T's
term-start entry. If the follower's own `durable_sequence()` is above
`start_seq`, it holds writes the old leader made after the new leader's
history branched. It stops pulling, reports `diverged`, and is re-bootstrapped
by its agent. This is the fork the Elle cluster check found when a follower
ahead of the promoted node was re-targeted; the promotion rule makes it
unlikely, and this rule makes it visible instead of silent when a stale member
reappears.

**Crashed leader.** As in #422: a server restarting after an unclean shutdown
as leader comes up as a follower that refuses to serve until re-bootstrapped.
The agent sees `crashed_leader` and runs the bootstrap. A clean restart of the
leader (a rolling update) is a switchover first: the container's `preStop`
hook calls the agent's `/switchover`, which hands the Lease to the most
advanced follower and demotes.

## Bootstrap and re-bootstrap

The joining node's agent, with an empty data directory or a `crashed_leader` or
`diverged` state:

1. Writes `state = bootstrapping` on its pod.
2. Asks the leader's agent for a manifest. The leader's agent calls
   `create_manifest()` through the plugin, records the manifest's
   `through_sequence`, and holds the leader's vacuum off while the manifest is
   checked out. The joining agent writes `bootstrap_from = through_sequence` on
   its pod before copying a byte, so the leader's agent counts it in
   `retain_after` from then on. This is the ordering the Elle cluster harness
   settled on.
3. Copies the manifest's files from the leader's agent over HTTP into a fresh
   directory, swaps it in, starts the server as a follower, and sets its source
   to the leader. The pull loop continues from `through_sequence`.
4. Clears `bootstrapping`. Readiness stays false until lag is under the probe's
   limit.

Table discovery (#422 milestone 1) is what lets the joined server see the
tables; it is unchanged by this document.

## Routing and probes

The agent serves Patroni's endpoint set, so existing proxy configurations
transfer:

| Endpoint | 200 when |
|---|---|
| `/primary`, `/read-write` | this server is leader and holds a granted plugin lease |
| `/replica?lag=N` | follower, not `noloadbalance`, lag ≤ N |
| `/read-only` | either of the above |
| `/health` | the server answers SQL |
| `/liveness` | the loop ran within `ttl` (leader) or `2·ttl` (follower); no SQL |
| `/readiness?lag=N` | leader, or a follower within lag N |
| `/failsafe` | this node follows the asking node (used by failsafe mode) |
| `POST /switchover`, `POST /failover`, `POST /pause` | operator actions, with Patroni's parameters and candidate checks |

Services: `bytecaskdb-primary` selects `bytecaskdb.io/role=primary`,
`bytecaskdb-replica` selects `role=replica`. The pod's readiness probe is
`/readiness`, so a lagging or bootstrapping follower leaves the replica Service
by itself, and a demoted leader leaves the primary Service within one probe
period. The liveness probe is `/liveness`; it restarts a stuck agent, which is
not fencing and is not relied on for it.

## Fencing

Layers, from the one that is always on to the one that needs the deployment's
help:

| Layer | Stops | Needs |
|---|---|---|
| Plugin lease, checked per commit | a leader partitioned from the store, a paused process or VM, a dead agent | nothing: it is in the plugin |
| Term in the data file, checked by followers and at promotion | an old leader's writes reaching followers; a stale node promoting itself | nothing |
| Readiness probe and Services | clients that go through the Services reaching a demoted leader | nothing |
| Node remediation (medik8s Self Node Remediation and Node Healthcheck Operator, or a cloud-API power-off) | a hung kernel or VM that resumes with its clock unchanged; the window between a node's death and its pods being freed | the deployment installs it, with its timeouts set so a node that lost the API server reboots within `ttl` |
| Out-of-service taint | the same, by hand, after a human confirms the node is off | a runbook |

A leader pod is never force-deleted. A StatefulSet runs at most one pod per
ordinal unless someone force-deletes, and a replacement with the same name on
another node while the old one still runs breaks every layer above at once.
The runbook fences the node and lets the StatefulSet replace the pod.

Clients that bypass the Services and connect to a pod directly are stopped by
the plugin lease and nothing else. That is the same envelope as #422's.

## Synchronous mode

Off by default; the default is asynchronous and can lose writes acknowledged
in the last `ttl` on an unplanned failover, bounded by
`maximum_lag_on_failover`.

With `synchronous_mode` on, the leader's agent keeps a set of followers whose
acknowledgement commits wait for, and records it in the Lease's `sync`
annotation. Promotion is restricted to that set, so no acknowledged write is
lost on an automatic failover. The ordering that makes this hold is Patroni's:
a follower is made synchronous on the leader (`bytecaskdb_sync_followers`)
*before* it is added to the annotation, and removed from the annotation
*before* it is removed on the leader. Any node the annotation names has been
synchronous for every commit since it was named.

With no healthy follower to be synchronous, the leader keeps accepting writes,
and the annotation is emptied, so no automatic promotion happens until a
follower catches up and is named again. A manual failover can still be forced,
with the loss that implies. A strict variant that blocks writes instead is a
later option.

`cross_zone` is Vitess's `cross_cell` policy: the followers that count are in
a different zone from the leader, read from the node label through the pod's
`spec.nodeName`. A zone outage then loses no acknowledged write, at the cost
of a cross-zone round trip on every commit.

## Failure envelope

What each scenario does, which layer stops it, and the check that would notice
if it did not. The checks are the Elle cluster harness
(`docs/replication_checking_design.md`) driven by the agent loop in-process
against a fake store, plus mutations of the agent that each remove one rule.

| Scenario | Outcome | Stopped by | Checked by |
|---|---|---|---|
| Leader pod dies | No writes for up to `ttl + loop_wait`. Most advanced follower promotes. Writes above its `durable_sequence` are lost (async) or none (sync). | Lease expiry, promotion rule | topology run: unplanned promotion |
| Leader node loses the API server, keeps its clients | Writes stop within `ttl − safety_margin` of the last renewal. A follower promotes after `ttl`. | plugin lease | nemesis: store unreachable from one node |
| Leader node loses the API server, failsafe on, members reachable | Leader keeps writing; no follower can promote while it answers them. | failsafe check | nemesis: store unreachable from the leader only |
| Whole store unreachable, failsafe off | Leader demotes; cluster is read-only until the store returns. Availability lost, nothing else. | plugin lease | nemesis: store unreachable from all |
| Leader's agent dies | Plugin lease expires; plugin demotes. The restarted agent, same pod name, resumes renewing if within `ttl`, else becomes a follower. | plugin lease, holder identity | nemesis: agent paused |
| Leader process or VM paused past `ttl` | First commit after resume is refused. | `CLOCK_BOOTTIME` deadline per commit | nemesis: plugin paused past lease; a mutation moves the check off the commit path |
| Two candidates race | One CAS wins; the other follows. | store CAS | nemesis: synchronized candidates |
| Lagging follower promotes | Impossible if a fresher, more advanced member exists; bounded by `maximum_lag_on_failover`; impossible in sync mode. | promotion rule, `sync` set | `--promote-least`: must be refused |
| Stale member reappears ahead of the leader | It reports `diverged` and is re-bootstrapped. | term-start rule | nemesis: member hidden through a promotion |
| Old leader's pull repointed by a zombie agent | Followers refuse an older term. | term check | mutation: drop the follower term check |
| Clock skew between nodes | None: no wall clock is compared across nodes. Only clock *rate* matters, within `safety_margin`. | design | nemesis: skewed clocks, no effect expected |
| Plugin lease configured longer than `ttl` | Split brain. The agent refuses to start with `safety_margin ≤ 0`. | configuration check | mutation: drop the check, expect caught |
| Switchover | Old leader demotes and hands the Lease to the named candidate; candidate waits until caught up, then promotes. No loss. | `bytecaskdb_demote` syncs first | topology run: planned transfer |

The last column is the acceptance criterion: a row without a check is not done.

## Testing

1. **Agent loop against a model.** The loop is a Go package with the store,
   the clock and the plugin as interfaces. A test runs N loops against an
   in-memory CAS store with injectable unreachability and latency, a fake
   clock, and a model plugin that keeps each node's lease deadline, term and
   sequence and asserts the safety properties on every step: never two nodes
   with an unexpired plugin lease, never a promotion with a term at or below
   one already seen, never a follower pulling from an older term. Random
   interleavings with a printed seed, in the form of the engine's model-based
   tests. Every nemesis in the envelope is a few lines there. This is where
   the design is validated, before any Kubernetes.
2. **Agent mutations.** `tests/agent_mutations/`, one patch per rule in the
   envelope, each with `Expected: caught` and what the test then sees, in the
   form of the engine's mutation sets.
3. **The Elle cluster harness** keeps checking the data: its orchestrator
   already runs promotions, transfers and re-bootstraps against real engines.
   It gains the diverged-follower case and a promotion chosen by the agent's
   rule, so what step 1 proves about the agent and what the harness proves
   about the engine meet on the same protocol.
4. **Kubernetes end to end.** A `kind` cluster, the real StatefulSet, two or
   three failover rounds with pods killed and the API server blocked by a
   network policy. A smoke test, not the proof: the interleavings are covered
   in steps 1 and 3.

## Milestones

Replacing #422's list:

1. Table discovery. Unchanged.
2. Replication from SQL: `HA_HAS_OWN_BINLOGGING`, the source sysvar, the pull
   thread, follower read-only, the status table. Unchanged, plus
   `bytecaskdb_demote` and `bytecaskdb_retain_after`.
3. Plugin lease on `CLOCK_BOOTTIME` checked per commit, terms, term-start
   entries, follower term check, the diverged rule, crashed-leader refusal.
4. The agent loop as a Go package, the model test, the agent mutations, and
   the Elle harness extended. The envelope's check column filled in.
5. The Kubernetes store, the agent container, the manifests, bootstrap over
   HTTP, the `kind` test.
6. Synchronous mode, `on` and `cross_zone`.
7. An etcd store, for deployments outside Kubernetes. Tracked as an issue when
   milestone 5 lands.

## Open questions

1. **Settled: Go.** The agent is a Go program on client-go and the MariaDB
   operator's agent packages, as described under *Components*. Python would
   have shared the bindings and the Elle harness; Go gets the Lease client,
   the probe and auth plumbing and a static binary for free, and the harness
   is met through the protocol rather than shared code.
2. **File transfer.** The leader's agent serving data and hint files over HTTP
   is the simplest thing; it needs a throttle so a bootstrap does not starve
   the leader's disk.
3. **Membership.** The StatefulSet's pods are the members. Scaling down leaves
   a stale pod annotation, which goes stale after `ttl` and is ignored; a
   `sync` entry naming it is removed by the leader's agent. Whether a removed
   member needs an explicit tombstone is open.
4. **Standby clusters** (a whole cluster following another, Patroni's
   `standby_cluster`) are out of scope.
5. **Packaging.** Milestone 5 ships plain manifests: a StatefulSet, the two
   Services, the ConfigMap, RBAC for the Lease and pod annotations. Two
   frameworks could own that layer later, and neither replaces the agent:
   - A **KubeBlocks addon**: a `ComponentDefinition` whose lifecycle actions
     (`roleProbe`, `switchover`, `memberJoin`, `memberLeave`) call the agent's
     HTTP API, plus a chart. KubeBlocks then owns provisioning, scaling,
     backups and the `OpsRequest` for a planned switchover. Unplanned failover
     stays with the agent: KubeBlocks leaves it to the engine's own HA layer,
     as it does with Patroni and Sentinel. The operator is AGPL-3.0, but it
     runs unmodified as a separate program, the addons repository is
     Apache-2.0, and an addon is configuration the operator consumes, not a
     derived work, so nothing here or in the addon takes on AGPL terms.
   - A **mariadb-operator mode**, in their MIT codebase, which would also own
     the Services' lag exclusion and their backup tooling. It needs a
     replication type that does not read `SHOW SLAVE STATUS`, which is a
     larger upstream change.
   Both are additive. The agent's API is the seam, so the choice can wait
   for a deployment that wants one of them.

## Changes from #422

- The write lease is not optional when the agent runs, and it is granted for
  `ttl − safety_margin` by an agent that renews a Kubernetes Lease, not
  renewed by an operator on a health check.
- The operator does not have to be highly available. The API server does, and
  already is. Failsafe mode covers its outage.
- `bytecaskdb_demote`, `bytecaskdb_retain_after` and the synchronous-mode
  sysvars are added. `retain_after` moves from "the leader knows its
  followers" to "the agent sets it from the members".
- The diverged-follower rule is added to the term-start entry.
- The promotion target is chosen by the candidates from published state, not
  by an operator.
- The failure envelope and its checks are the acceptance criteria, in place of
  the multi-node harness milestone, which becomes milestone 4.
