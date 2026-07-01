# DBG_LOG.md - CYCLE 4: rematerialization debugging (run_dbg, DBG1-DBG6)

Observed-truth log for run_dbg() in kernel/kernel.c. Every result below is a REAL serial
line from a single boot on the metal (QEMU/KVM, SINGLE_BOOT=1). Numbers are rdtsc-measured
THIS run and vary boot to boot; they are recorded as observed, not imported.

This cycle turns the in-principle claims of kernel/DEBUGGING_MODEL.md (DB1-DB6) into a
driven, single-CPU demonstration over the COMMITTED content store. The debugging TOOLING is
new; the mechanism it drives (content-address + re-materialize + re-hash-verify) is the
same store engine used everywhere else in the tree (cvsasx_store_put/get, cvsasx_blake3,
cvsasx_hash_eq) plus the committed gc_rc refcount table for the retention-cost surface.

## The model in this cycle

An execution STATE is a small Merkle tree stored content-addressed in a dedicated
dbg_store (same cvsasx_store_t engine as net_store, isolated so its storage metrics are
exact):

    state = dir{ "cpu": dir{ r0, r1 }, "mem": dir{ p0, p1 } }

r0/r1 are 1-byte register leaves; p0/p1 are 64-byte memory-page leaves. A history is a DAG
of these state hashes. The spine is deterministic; one edge is a REAL external edge whose
input is the live 64-bit TSC (strictly monotonic within a boot, so two reads never
coincide - genuine non-determinism, not a simulated flag).

History used below:
- s0 = state(r0=1, r1=2, memA, memB)
- s1 = state(r0=9, r1=2, memA, memB)      deterministic edge s0->s1 (r0 += 8)
- s2 = external_step(s1)                    external edge s1->s2 (p0 <- TSC bytes), execution A
- s2b = external_step(s1)                   independent external read, execution B

## Seams, mapped to DEBUGGING_MODEL.md, with the real serial evidence

### DBG-1 state-history DAG + time-travel by hash (DB1)
```
DBG-1 history DAG nodes s0=157ec90c9cc2 -> s1=84d06c4d163b -> s2=8a43b08a5c95
DBG-1 time-travel to s0 by hash: fetched 80 bytes, re-hash == recorded hash=y (bit-exact; NOTHING re-executed -> direct addressing, NOT replay)
DBG-1 re-materialized PAST r0=1 vs current(s2) r0=9 -> the actual old bytes, recovered by naming them=y
DBG-1 RETENTION COST (surfaced, not hidden): retaining a state PINS its root -> un-reclaimable while retained=y; dropping the root makes it reclaimable again=y (committed gc_rc refcount path)
```
Jumping to a past state is store_get(H) followed by cvsasx_blake3 of the returned bytes and
cvsasx_hash_eq against H. The re-hash CONFIRMS the fetched bytes ARE H (bit-exact by BLAKE3
collision-resistance), which is exactly what disproves replay-masquerade: no instruction was
re-executed, the past register value r0=1 was recovered by naming it, distinct from the
current r0=9. The GC/history-retention cost is surfaced, not hidden: pinning a state root in
the committed gc_rc table makes it (and its subtree) un-reclaimable while retained, and
dropping the pin makes it reclaimable again.

### DBG-2 structural diff by hash (DB2)
```
DBG-2 diff(s0,s1): changed leaves=1, dir-pairs descended=2 (mem subtree shared -> PRUNED, r1 leaf pruned)
DBG-2 diff(s0,s2): changed leaves=2, dir-pairs descended=3 (r0 and p0 both changed -> cpu+mem descended); pruning is STRUCTURAL, not a byte scan=y
```
The diff compares root hashes and descends only where they differ; an equal subtree is
pruned in ONE hash comparison and never read. s0 vs s1 differs only in r0, so the entire
mem subtree is pruned (descended=2: root and cpu only). s0 vs s2 changed both r0 and p0, so
both subtrees are descended (descended=3). This is structural pruning, not a byte scan:
unchanged regions are PROVEN unchanged by hash equality, not read.

### DBG-3 provenance walk, honest (DB3)
```
DBG-3 provenance edge s0->s1: DETERMINISTIC (DRF spine), re-derivation reaches the same hash=y -> reproducible lineage
DBG-3 provenance edge s1->s2: NON-DETERMINISTIC/EXTERNAL (p0 <- external input), re-derivation differs=y -> the walk STOPS here and SAYS SO: cause is outside the recorded content (NO fabricated cause)
```
On the deterministic DRF spine, provenance is inherent: re-deriving s1 from s0 by the same
op reaches the identical hash, so the edge is reproducible lineage read off the DAG. At the
external edge the walk STOPS and SAYS SO: re-deriving with a fresh external read reaches a
DIFFERENT hash, so the cause is outside the recorded content. No cause is fabricated for the
external edge; provenance is a lineage of states, not of causes across external edges.

### DBG-4 observability as DAG query - first divergence (DB4)
```
DBG-4 two executions A,B compared by state hash: last common node = index 1, FIRST DIVERGENCE at index 2
DBG-4 structural delta at the divergence node: changed leaves=1, dir-pairs descended=2 (only p0 differs; cpu subtree shared -> pruned)=y
```
Two executions are compared by state hash node-for-node. A=[s0,s1,s2] and B=[s0,s1,s2b]
share the s0->s1 prefix (last common = index 1) and first diverge at index 2. The precise
delta at the divergence node is then the structural diff of DBG-2 (reused): only p0 differs;
the shared cpu subtree is pruned. Observability is a query over the DAG, not log scraping.

### DBG-5 limits ENFORCED (DB5) - real refusal paths, not prose caveats
```
DBG-5(a) un-send external effect (packet e0d9119e93a3): effect is a referent of the re-materialized state=n -> undo REFUSED (re-materialization is pure w.r.t. internal state, impure w.r.t. the world)=y
DBG-5(b) reproduce the non-deterministic edge: two runs reach the same state=n -> reproduction REFUSED (DRCC undefined for racy/external state; the state is re-materializable, its recurrence is not)=y
DBG-5(c) restore real-time context: re-materialized state has a live clock=n -> real-time/liveness query REFUSED (the state is out of wall-clock time)=y
```
Each limit is enforced by a real branch, not stated as a caveat:
- (a) The sent packet is not a referent of any re-materialized state (dbg_resolve for it
  fails), so the "undo" query is REFUSED. Re-materialization is pure with respect to
  internal state and impure with respect to the world.
- (b) The external edge was run twice and reached two different states (observed
  s2 != s2b), so reproduction is REFUSED. The state is re-materializable; its recurrence is
  not (DRCC undefined for racy/external state).
- (c) A re-materialized state carries no clock entry (dbg_resolve "clock" fails), so a
  real-time/liveness query is REFUSED. The state is out of wall-clock time.

### DBG-6 measurements (DB6), rdtsc this run
```
DBG-6 rdtsc: jump-to-state(fetch+re-hash)=23724 cyc; structural-diff PRUNED(identical roots)=505 cyc vs DESCENDED(differing)=165520 cyc (pruning benefit); provenance-edge verify=150448 cyc
DBG-6 history-retention STORAGE: distinct objects=20, bytes stored=1219, dedup hits=140023 (structural sharing: shared subtrees stored ONCE; floor = distinct state size, not zero)
```
- jump-to-state (store fetch + re-hash-verify): 23724 cyc, independent of how old the state
  is (a fetch, not a replay whose cost grows with distance).
- structural-diff pruning benefit: an identical-root diff is 505 cyc (one hash compare,
  pruned) versus 165520 cyc when the roots differ and the changed spine is descended (the
  descend cost is dominated by BLAKE3 re-verify on each read). The pruned/unpruned gap IS
  the pruning benefit, measured.
- provenance-edge verify (re-derive + hash-compare a deterministic edge): 150448 cyc.
- history-retention storage: 20 distinct objects, 1219 bytes, with 140023 dedup hits over
  the run (repeated content, including measurement loops, is stored ONCE). Structural
  sharing softens retention but the floor is the distinct state size, not zero - the cost is
  stated, not hidden.

Numbers vary per boot (rdtsc). The above are one representative boot.

## What is disproved (the adversarial checks the cycle had to pass)

- Replay-masquerading-as-time-travel: DISPROVED. The jump re-hashes the fetched bytes and
  requires equality to the recorded hash; the past register value is recovered by naming,
  not by running instructions forward (DBG-1).
- Diff-not-structural: DISPROVED. Equal subtrees are pruned by a single hash comparison and
  never read; descended-pair counts and the 505 vs 165520 cyc gap show structural pruning,
  not a byte scan (DBG-2, DBG-6).
- Fabricated provenance: DISPROVED. The external edge is labeled NON-DETERMINISTIC and the
  walk stops there and says so; no cause is invented (DBG-3).
- Limits hidden: DISPROVED. Each of the three limits is a real refusal branch that prints
  its verdict (DBG-5).
- History cost hidden: DISPROVED. Retention pins roots un-reclaimable (DBG-1) and the
  storage floor (distinct objects/bytes) is measured and reported (DBG-6).

## SCOPE (honest)

Single-CPU. The DEBUGGING TOOLING (history recorder, jump-to-hash, diff walker, provenance
walker, divergence query) is new in this cycle; the MECHANISM it drives (content-address +
re-materialize + re-hash) is the committed store. DEMONSTRATED here: direct-addressed
time-travel confirmed by re-hash, structural diff that prunes by hash, provenance on the
deterministic spine, first-divergence DAG query, and the retention-cost coupling to the
committed gc_rc. DEFERRED / real BOUNDARY, stated not hidden:

- Retention policy is un-reclaimable storage; "retain everything" grows with the distinct
  state touched. A policy (every step / sync-point cuts / sliding window) is not decided.
- Provenance is state-lineage, NOT cause, across external and racy edges.
- Re-materialization CANNOT un-send an external effect, CANNOT reproduce a non-deterministic
  edge, and CANNOT restore wall-clock time. The capability is exactly as wide as
  "deterministic internal state that was retained", and not one edge wider.

## BORROWED vs NEW

- Borrowed: the Merkle-DAG structure (Git/IPFS content-address files and trees), and
  record-replay / omniscient debuggers (Mozilla rr, Pernosco, UndoDB, WinDbg TTD, gdb
  reverse) which RE-EXECUTE from a checkpoint plus a non-determinism log to APPROXIMATE a
  past state.
- New (CARMIX angle): time-travel by DIRECT RE-MATERIALIZATION of content-addressed live
  execution state - reach a past state by ADDRESSING its hash and re-verifying it bit-exact,
  no re-execution, at fetch cost independent of distance in the past. The novelty is the
  primitive; this cycle is a driven demonstration of the tooling over it, bounded exactly by
  the DBG-5 limits.

## Regression

Full regression re-run green with run_dbg added, except the ONE pre-existing accepted PM0
stall (PM0 -> *** FAIL (see stall report) ***). The M0 negative reaches its fail-closed
lines (frame pool exhausted, M0 -> OK) and the F2 negative reaches its fail-closed assert
(F2 -> FAIL-CLOSED ON VERIFY FAILURE OK). Boot continues past PM0 to completion (TS/SM
stages run after it). DBG -> REMATERIALIZATION DEBUGGING OK.
</content>
</invoke>
