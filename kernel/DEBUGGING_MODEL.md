# DEBUGGING_MODEL.md - debugging and observability when every past state is content-addressed and re-materializable

Research note. No build, no code. This document reasons about what debugging and
observability become once execution state is content-addressed by BLAKE3, named by its
hash, and re-materializable by that name. It does not re-derive CARMIX's established
properties. It cites them and builds on them.

The one-line thesis: if a past execution state is a hash, and re-materialization is the
kernel's ordinary way of turning a hashed state back into a running one, then reaching a
past state is direct addressing, not replay. That is the novel capability. The honest
edge is that direct addressing recovers internal deterministic state and nothing else:
it cannot un-send a packet, cannot reproduce a race, and lands outside wall-clock time.

## How to read this (evidence grades)

Every claim carries a grade. Do not read an in-principle claim as a shipped feature.

- `[EST-CARMIX]` shown in a boot log in this repository. Cited to the log.
- `[EST-THEORY]` a banked CARMIX theory result (DRCC, convergence). Stated in the interim
  whitepaper, not yet externally reviewed. Flagged as interim wherever used.
- `[EST-EXTERNAL]` established prior art outside CARMIX. Named in BORROWED VS NEW.
- `[CARMIX-INPRINCIPLE]` follows from primitives that already exist in the tree, but is NOT
  built as debugging tooling. An in-principle capability, labeled as such every time.
- `[OPEN]` unresolved. Future work or a genuine limit.

Established properties this note stands on, by reference, not re-derived:

- Content-addressing by BLAKE3 over content INCLUDING referents' hashes, so the object
  graph is a Merkle DAG that is acyclic by construction. `[EST-CARMIX]` kernel/GC_LOG.md
  (GC2), kernel/U4_LOG.md (U4-1).
- Rematerialization is the existing mechanism: context switch, fault, and migration ARE
  rematerialization. Migration fetches objects by hash, re-hashes to verify, and re-mints
  bounded authority. `[EST-CARMIX]` kernel/U3_LOG.md (U3-1), kernel/MIGRATION_AUTHSET_LOG.md
  (DM7, DM5).
- Persistence: state survives a genuine cold reboot under an ordered-prefix crash
  discipline. `[EST-CARMIX]` kernel/GC_LOG.md (GC1, GC4).
- DRCC: a data-race-free program reaches a reproducible content hash at a
  synchronization-point cut. Undefined for racy state. `[EST-THEORY]` with one first
  exercise, kernel/U3_LOG.md (U3-3).
- Convergence: a computable canonical form for reachable-state structural equivalence.
  Observational equivalence is undecidable. `[EST-THEORY]`, interim whitepaper.
- GC: reference-counted plus tombstone reclamation over the acyclic graph, root set =
  durable roots + volatile roots, resurrection window on a dedup hit. `[EST-CARMIX]`
  kernel/GC_LOG.md (GC1 to GC6).

---

## DB1 TIME-TRAVEL BY HASH (headline)

Claim: a debugger can re-materialize a PAST execution state directly by its hash,
deterministically, without replay. `[CARMIX-INPRINCIPLE]`

### (a) History as a DAG of content-addressed states

Model an execution as a directed graph. A node is a whole execution-state hash (the
address space, registers, and capability set hashed together, the same content that
migration transfers and re-verifies, kernel/MIGRATION_AUTHSET_LOG.md DM7). An edge is one
execution step from a predecessor state to its successor state. Because each state is a
content address over its own contents, two runs that pass through the identical internal
state land on the identical node. The history is therefore not a linear tape but a DAG in
which structurally identical states coincide by construction. This is the same Merkle-DAG
shape already shown for the namespace, applied to execution state over time rather than to
directory trees. `[EST-CARMIX]` for the DAG shape (kernel/U4_LOG.md U4-1). `[OPEN]` for
recording the step edges: nothing in the tree records a per-step history DAG today.

### (b) Re-materializing state H is direct addressing, not replay

Re-materialization already exists as the kernel's context-switch, fault, and migration
mechanism. `[EST-CARMIX]` kernel/U3_LOG.md (U3-1). To reach a historical state H, apply
that same mechanism to a historical hash. The kernel fetches the objects named by H,
re-hashes to verify them, re-mints the bounded authority, and the process is that past
state. Nothing is re-executed. The distinction is exact: a re-materialized state is the
actual bytes that were live then, recovered by naming them, not a state re-derived by
running instructions forward again.

This is what makes it time-travel by hash rather than by replay. A record-replay debugger
holds a checkpoint plus a log of non-determinism and RE-EXECUTES forward from the
checkpoint, applying the log, to APPROXIMATE the state at a target point. CARMIX addresses
the state. Two consequences follow. First, cost is a store fetch plus hash verification,
independent of how far in the past H is, whereas replay cost grows with distance from the
nearest checkpoint. Second, correctness is by construction: a re-verified H is bit-exact by
the collision resistance of BLAKE3, not by the fidelity of a re-execution. `[CARMIX-INPRINCIPLE]`
The primitives exist. A debugger that drives them to a chosen historical hash does not.

### The requirement: history retention couples to GC, and this is a real cost

The capability is only as good as the retention of H. GC reclaims unreachable
content-addressed objects by reference count and tombstone. `[EST-CARMIX]` kernel/GC_LOG.md
(GC6). A past state H is reachable only while some root names it. The root set is the union
of durable roots and volatile roots. `[EST-CARMIX]` kernel/GC_LOG.md (GC3). So to guarantee
that H can be re-materialized later, a history layer must pin H as a root. Pinning H makes H
and its entire referent subtree un-reclaimable for as long as the history is retained. That
is the cost, stated plainly: retained history is un-reclaimable storage. Structural sharing
softens it (see DB2: unchanged subtrees share one hash and are stored once) but does not
remove it. The size of retained history is bounded below by the size of the distinct state
that history spans, not by zero. `[OPEN]` the retention policy (retain every step, retain
sync-point cuts only, retain a sliding window) is a design choice with a direct storage
price and is not decided in the tree. Note the tension with the resurrection window
(kernel/GC_LOG.md GC5): a history root removes the object from the reclamation path
entirely, so the zombie or dedup-hit dynamics do not arise for pinned history.

### The exact difference from record-replay

Record-replay re-executes to approximate a past state from a checkpoint plus a
non-determinism log. CARMIX re-materializes the actual past state by addressing its hash. It
does not re-execute. This is the single sharpest structural difference in this document and
everything downstream (DB2 to DB4) depends on it: because the past state is a named object,
it can be diffed, walked, and navigated as data, not reconstructed as a side effect of
running.

---

## DB2 STRUCTURAL STATE DIFF BY HASH

Claim: two execution states, each a content-addressed Merkle tree, are diffed by comparing
hashes top-down. Identical subtrees share one hash and are pruned unvisited. `[CARMIX-INPRINCIPLE]`

### The mechanism

Compare the two root hashes. If equal, the two states are bit-identical and the diff is
empty, decided in one comparison regardless of state size. If they differ, descend into the
children and recurse, and at each level skip any child subtree whose hash matches its
counterpart. `[EST-CARMIX]` the Merkle property that a changed referent changes the
referrer's hash (kernel/GC_LOG.md GC2, kernel/U4_LOG.md U4-1) is exactly what makes an
unchanged subtree detectable by a single hash equality. The walk visits only the subtrees
that actually changed. Cost is proportional to the size of the difference, not the size of
the state.

### What it reveals

The exact set of addresses, register slots, and capabilities that changed between the two
states, down to the leaf, with unchanged regions proven unchanged rather than assumed
unchanged. Applied to two adjacent history nodes it is the precise effect of one execution
step. Applied to two divergent runs it is the precise structural delta between them.

### How it differs from conventional memory-diffing

Conventional memory-diffing compares two byte images region by region and must scan both in
full to know what is equal. It has no way to prove a region unchanged short of reading it.
Structural diff by hash inverts this: equality is a hash comparison and unchanged subtrees
are pruned before they are read. Shared hashes prune the diff. The saving is the whole
unchanged fraction of the state, which for one execution step is typically almost all of it.

### Connection to convergence and to GC

There are two notions of "same" here and the distinction is load-bearing. `[EST-THEORY,
interim]`

- Raw content-address equality is bit-exact identity. Diff-by-hash as described prunes
  bit-identical subtrees.
- Structural equivalence is coarser. Convergence gives a computable canonical form so that
  two states that are structurally equivalent but not bit-identical (a different but
  equivalent layout) canonicalize to the same form. A diff run over canonical forms prunes
  structurally-equivalent subtrees too, at the cost of computing the canonical form.
  Observational equivalence is undecidable, so this is the strongest equivalence a diff can
  mechanically exploit.

GC ties in through reachability: the diff descends only reachable subtrees, because an
unreachable subtree is not named by the state and is not part of its Merkle tree. What GC
would reclaim, the diff never visits. `[EST-CARMIX]` reachability as the liveness criterion,
kernel/GC_LOG.md (GC3, GC6).

---

## DB3 INHERENT PROVENANCE

Claim: lineage is partly inherent in the content-addressed DAG, because a state hash is
derived from prior states and inputs, and identical states coincide by hash. `[CARMIX-INPRINCIPLE]`
The word "partly" is the whole content of this section.

### What causality it captures

Within deterministic execution, the history DAG records genuine lineage. An edge from
predecessor P to successor S means S was produced from P by one step, and because both are
content addresses the relation is exact and shareable. Under DRF, that edge is reproducible:
re-running the same DRF computation from P reaches the same S, and a synchronization-point
cut over DRF processes reaches a reproducible content hash. `[EST-THEORY]` DRCC, with one
first exercise, kernel/U3_LOG.md (U3-3). Convergence sharpens this: two lineages that reach
structurally equivalent states are recognizable as convergent by canonical form.
`[EST-THEORY, interim]`. So for the deterministic, DRF part of an execution, provenance is
not an added log. It is the DAG itself, and identical intermediate states are automatically
identified rather than re-derived.

### What it does NOT capture

Content-addressing records WHAT a state is. It does not, by itself, record WHY that state
arose when the transition was not a function of the recorded predecessor alone.

- Non-deterministic edges. If a step depends on a race, the same predecessor P can yield
  different successors on different runs. The content hash of S is honest about which S
  occurred, but the edge P to S is not reproducible and does not encode the scheduling that
  chose it. This is exactly the boundary DRCC draws: undefined for racy state, and shown as
  a hash that differs across runs. `[EST-CARMIX]` kernel/U3_LOG.md (U3-3, racy pair).
- External inputs. A step that consumes an external input (a device read, a network
  message, a real-time value) produces an S that the predecessor alone does not determine.
  Unless that input is itself captured as a content-addressed object and folded into the
  edge, the DAG shows the resulting state without the cause. `[OPEN]` capturing external
  inputs as content-addressed edge annotations is a design option, not a built feature, and
  it converts the input into internal state at the moment of capture.

The honest summary: provenance is inherent for the deterministic internal spine of an
execution and absent for its non-deterministic and external edges. It is a lineage of
states, not a lineage of causes, wherever the cause is outside the recorded content.

---

## DB4 OBSERVABILITY AS DAG NAVIGATION

Claim: observability becomes query and navigation over the state DAG rather than scraping
of logs and metrics. `[CARMIX-INPRINCIPLE]`

### The queries the DAG answers directly

- Which states did a computation pass through. Enumerate the nodes on its path.
- Which states share structure. Nodes that share a hash, or share a subtree hash, are the
  same state or share that substructure by construction (DB2). Shared history across
  processes is a shared hash, not a heuristic match.
- Where did two executions diverge. Walk both paths and find the last common node, then the
  first pair of differing nodes. The divergence point is exact, and the structural diff at
  that pair (DB2) is the precise delta that separated them.

### Versus conventional log and metric scraping

Conventional observability samples. A log line or a metric is emitted at a point the
programmer chose, at a rate the system can afford, and the past state is RECONSTRUCTED
after the fact by inference from those samples. It is lossy by design and the reconstruction
can be wrong. DAG navigation is different on three axes.

- Structural, not textual. The unit is a state and its Merkle structure, not a formatted
  message about a state.
- Exact, not sampled. Every retained node is the actual state, not a sample near it.
  Coverage is set by the retention policy (DB1), not by log verbosity.
- Content-addressed, not reconstructed. Identity and sharing are decided by hash, so "the
  same state" and "this subtree is unchanged" are facts, not inferences.

The honest boundary carries straight over from DB1 and DB3. This observability sees only
what is retained (a storage cost) and only the deterministic internal state (DB5). It does
not see external effects or the non-deterministic reason a particular edge was taken.

---

## DB5 THE HONEST LIMITS (non-negotiable)

These are not caveats. They delimit exactly where time-travel by hash works and where it
stops. Time-travel by hash works for deterministic internal state. It stops at every one of
the following.

### (a) External I/O and side effects are outside the model

Re-materializing a past state recovers the internal state. It does NOT un-happen anything
that state did to the outside world. A network packet that was sent stays sent. A device
write that occurred stays occurred. The model can return the process to the state it was in
before the send, but it cannot retract the send. `[EST-CARMIX]` by direct consequence of
what state is: the hashed content is the address space, registers, and capabilities
(kernel/MIGRATION_AUTHSET_LOG.md DM7), and an emitted external effect is none of those.
Any debugger built on this must treat re-materialization as pure with respect to internal
state and impure with respect to the world, and must not present "go back" as "undo".

### (b) Non-determinism is out of scope for reproduction

DRCC and convergence are defined only for data-race-free execution. `[EST-THEORY]` U3-3 and
the interim whitepaper. A racy edge produces a hash that differs across runs, shown directly
in kernel/U3_LOG.md (U3-3, racy pair). `[EST-CARMIX]`. The consequences for debugging:

- A racy state that occurred can be re-materialized if it was retained (it is a hash like
  any other). But it cannot be reliably REPRODUCED by re-running, because the race need not
  land on the same state again.
- Real-time inputs are non-deterministic edges (DB3). The state after such an edge is
  observable if retained but its recurrence is not guaranteed.

So the model re-materializes any retained state, deterministic or not, but only guarantees
reproduction for the DRF, deterministic spine.

### (c) Real-time and liveness are gone

A re-materialized state is out of wall-clock time. It is the state as it was, with no clock
advancing and no external world in step with it. Two failures follow.

- Anything timing-dependent or deadline-driven cannot be observed live from a
  re-materialized state, because there is no live wall clock in it.
- Liveness properties (does the system eventually make progress) are not decidable by
  looking at any single re-materialized state or any finite retained set of them. This
  matches the theory limit that observational equivalence is undecidable. `[EST-THEORY,
  interim]`. Time-travel by hash is a tool for safety-style questions ("what state was the
  system in, and how did it get there structurally") and not for liveness questions.

Where it works: deterministic internal state, retained, re-materialized and diffed by hash.
Where it stops: the external world, the non-deterministic edge, and wall-clock time.

---

## DB6 DEMONSTRABILITY PARTITIONED

Separating what is demonstrable on the committed single-CPU core now from what is coupling
cost and what is later tooling.

### Demonstrable now, single-CPU, from existing primitives

Each of these reuses mechanisms already shown green in this tree.

- A state history as a DAG of content-addressed nodes, because whole-state hashing and the
  Merkle-DAG shape exist. `[EST-CARMIX]` kernel/U4_LOG.md (U4-1), kernel/GC_LOG.md (GC2).
- Re-materializing a chosen past state by its hash, because re-materialization by fetch,
  re-hash, and bounded re-mint exists. `[EST-CARMIX]` kernel/U3_LOG.md (U3-1),
  kernel/MIGRATION_AUTHSET_LOG.md (DM5, DM7).
- Structural diff of two states by hash comparison, because the store and the Merkle
  property exist. `[EST-CARMIX]` kernel/GC_LOG.md (GC2), store fetch in kernel/U4_LOG.md
  (U4-5).
- A provenance walk over retained deterministic nodes, because the DAG edges (if retained)
  and content identity exist. `[EST-CARMIX]` for the pieces, `[OPEN]` for the edge
  recording.

The clarification: the PRIMITIVES are demonstrated. The DEBUGGING TOOLING that drives them
(a history recorder, a re-materialize-to-hash command, a diff walker, a provenance walker,
a navigation query surface) is `[CARMIX-INPRINCIPLE]` and not built. This section is a claim
about reachability from what exists, not a claim that a debugger exists.

### The GC and history-retention coupling (a cost to bank, not a feature)

Retaining the history DAG pins past states as roots and makes them un-reclaimable while
retained (DB1). This is single-CPU-visible today: it is a direct interaction with the
existing refcount-plus-tombstone GC and its root set. `[EST-CARMIX]` kernel/GC_LOG.md (GC3,
GC6). It needs a retention policy before any history feature is sound, because "retain
everything" grows storage with the distinct state the execution touches. `[OPEN]`.

### Later tooling and later scope

- A navigation and query surface over the DAG (DB4). Later tooling.
- Canonical-form diff exploiting convergence (DB2), which depends on the convergence result
  landing as running code rather than interim theory. `[EST-THEORY, interim]` today.
- Cross-core and multi-process history. The concurrency and DRCC pieces have a first
  exercise only (kernel/U3_LOG.md U3-3), and racy history is re-materializable but not
  reproducible (DB5). Later scope, gated on DRCC moving from one exercise to validation.
- Capturing external inputs as content-addressed edges to extend provenance across external
  edges (DB3). Design option, later.

---

## BORROWED VS NEW

Prior art named honestly, then the one CARMIX-specific angle, with no overclaim.

### Borrowed (established prior art) `[EST-EXTERNAL]`

- Record-replay and time-travel debuggers. Mozilla rr (record and deterministic replay of a
  process by recording non-determinism, peer-reviewed, O'Callahan et al., USENIX ATC 2017).
  Pernosco (an omniscient query layer built on rr recordings, commercial tool). WinDbg Time
  Travel Debugging and Undo.io UndoDB and TotalView ReplayEngine (commercial record-replay).
  All of these RE-EXECUTE from a checkpoint plus a recorded log to reconstruct a past state.
- Omniscient and back-in-time debuggers. The Omniscient Debugger (Bil Lewis, circa 2003) and
  later trace-oriented and "DVR for code" tools record all state or all events and let the
  developer navigate backward. They store a trace and index it.
- Reversible debugging. GNU gdb reverse execution (reverse-continue, reverse-step) via its
  record target. This is replay-based reverse execution, not direct addressing of a stored
  state.
- Deterministic replay systems. A long line of virtual-machine and OS replay work (for
  example ReVirt-style logging of non-determinism for later faithful re-execution). Same
  re-execution shape as rr at coarser grain.
- Content-addressed DAGs of data. Git (Torvalds, 2005), a Merkle DAG of blobs, trees, and
  commits addressed by hash. It versions FILES and directory trees. It is the closest
  structural analogue to CARMIX's state DAG and namespace. IPFS shares this content-addressed
  Merkle-DAG structure for data.
- Provenance and lineage systems. W3C PROV data model, scientific-workflow provenance, and
  data-lineage tracking in data systems. These record lineage as ADDED metadata alongside the
  data.

Preprint and review status: the external tools above are shipped software and, for rr, peer
reviewed. CARMIX's own DRCC and convergence results are `[EST-THEORY]` from the interim
whitepaper and are NOT externally reviewed. This document flags them as interim everywhere
they are used and does not present them as settled.

### New (the CARMIX-specific angle)

Time-travel by DIRECT RE-MATERIALIZATION of content-addressed LIVE EXECUTION state. Three
sharp distinctions:

- Against record-replay. rr, Pernosco, UndoDB, WinDbg TTD, gdb reverse, and VM replay reach
  a past state by RE-EXECUTING from a checkpoint under a recorded non-determinism log. CARMIX
  reaches a past state by ADDRESSING its hash and re-materializing it, the same mechanism the
  kernel already uses for context switch, fault, and migration. No re-execution. The past
  state is recovered as the actual bytes, verified bit-exact by BLAKE3, not approximated by a
  forward run. Cost is a fetch, independent of distance in the past.
- Against Git and IPFS. Those content-address FILES and data trees. You cannot run a Git
  object as a process. CARMIX content-addresses LIVE EXECUTION state (address space,
  registers, capabilities) such that the addressed object IS a re-materializable running
  process. The Merkle-DAG structure is borrowed. The object being addressed is the
  distinguishing point.
- Against provenance systems. Those attach lineage as metadata. Here lineage is partly
  inherent in the content-addressed derivation itself (DB3), for the deterministic DRF spine
  only, not as a separate annotation layer.

The overclaim guard. The novelty is the primitive (address-and-re-materialize live state),
not a debugger. The debugger is `[CARMIX-INPRINCIPLE]`. And the primitive's reach is bounded
exactly by DB5: it beats record-replay on internal deterministic state and does not compete
with anything on external effects, non-determinism reproduction, or real-time. It is a
better mechanism for a strictly smaller, precisely delimited question.

---

## Bottom line

Novel capability, in-principle from existing primitives: reach any RETAINED past internal
state by naming its hash and re-materializing it, with no replay, bit-exact by construction,
at fetch cost independent of how old the state is. Diff any two such states in time
proportional to their difference, not their size, because identical subtrees share a hash.
Read lineage off the DAG for the deterministic DRF spine without a separate provenance log.

Sharpest honest limit: re-materialization recovers internal deterministic state and only
that. It cannot un-send a packet or undo any external effect, it cannot reproduce a race
(DRCC and convergence are defined only for data-race-free execution), and a re-materialized
state is out of wall-clock time so it answers safety-shaped questions and not liveness. The
capability is exactly as wide as "deterministic internal state that was retained," and not
one edge wider.
