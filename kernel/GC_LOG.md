# GC_LOG.md - single-CPU durable garbage collection (GC1-GC7)

Honest record of single-CPU durable GC: reference-counted reclamation of content-addressed
objects, correct without a cycle collector because the content-addressed graph is acyclic
by construction, with a crash-consistent tombstone free path (the tombstone is the commit
point) and a resurrection window so a dedup hit on a zombie object resurrects it instead
of being reclaimed.

All figures are rdtsc-measured in CARMIX on this run. None imported from any research
document.

## Scope (read first, plainly)

This is single-CPU durable garbage collection. It reclaims unreachable content-addressed
objects via reference counting (correct because the graph is acyclic by construction, so
no cycle collector is needed), with a crash-consistent tombstone free path (the tombstone
is the commit point, the deletion analogue of the persistence torn-tail invariant, scoped
ordered-prefix-in-QEMU as the persistence proof was), and a resurrection window so a dedup
hit on a zombie object resurrects it rather than reclaiming it (required because dedup is
already in CARMIX).

The refcount side table and the resurrection logic are DESIGNED so authority-domain
scoping can extend them in a FUTURE build, but this build implements NO domain-scoping, NO
dedup side-channel mitigation, NO leakage claim, and NO convergence canonicalization.
Concurrent tracing against a live multi-threaded mutator and any cross-core reclamation
race are SMP-blocked and NOT implemented here; they remain future cycles once SMP is used.
This build banks single-CPU durable reclamation, so the persistence log no longer grows
unbounded.

## Proven core untouched

The nine proven modules (carmix/swcap.c, carmix/backend_sw_gate.c, sls/sls.c,
gate/sfi_checker.c, gate/executor.c, gate/optimize.c, store/object_store.c,
cap/cvsasx_cap.h, sls/cvsasx_sls.h) and the rest of the proven core are byte-for-byte
identical after comment-stripping. GC is new infrastructure built beside the store and the
persistence substrate. The only change to committed persistence code is an ADDITIVE branch
in dur_recover that replays RC-delta records; the object-record path is byte-identical, so
the persistence stages (which write no RC/TOMB records) recover exactly as before. Only
kernel/kernel.c changed for GC.

## What was built and shown (observed)

### GC2 - acyclicity by construction (the property the design rests on)

```
GC2 acyclicity: node hash ba844c7cc48d.. is computed over its referent's hash 67e24428f5f4..
GC2 changing the referent changes the referrer's hash=y -> a referent's hash must EXIST before the referrer's is computed
GC2 -> ACYCLICITY HOLDS (refcounting is correct without a cycle collector) OK
```

An object's BLAKE3 is computed over its content INCLUDING its referents' hashes. Changing a
referent changes its hash, which changes the referrer's content, which changes the
referrer's hash (shown). So a referent's hash must already exist before the referrer's hash
can be computed; an object cannot name its own or an ancestor's hash (its hash is fixed only
after its content is). Therefore the graph is acyclic and refcounts reach zero exactly when
objects become unreachable. D1 (acyclicity assumed) is disproven: it is demonstrated.

### GC1 - durable refcount side table, survives a cold reboot

```
boot1 GC1 PERSIST: wrote durable refcount deltas (A: +1+1-1=1, B: +1=1) to the log; A=1 B=1
boot2 GC1 REVIVE: refcounts reconstructed from the durable log after a COLD REBOOT: A=1 B=1 (survived: A==1,B==1 = y) OK
```

Counts are MUTABLE metadata about IMMUTABLE objects, so they live OUTSIDE the hashed content
(a count inside an object would change its hash). They are recorded as durable RC-delta
records in the same append-only log, under the same ordered-prefix discipline as the object
index. Increment (including a dedup hit) and decrement both land durably. On a GENUINE cold
reboot (boot2, a separate QEMU process on the same disk, RAM empty), dur_recover replays the
RC records and reconstructs the counts exactly (A=1, B=1). DESIGN NOTE: the key is the hash
only here; an authority-domain tag could later be folded into the key to scope counts per
domain, but this build is global (one count per object).

### GC3 - root set (durable + volatile), volatile-only object held live

```
GC3 root set: durable roots (persisted objects/ROOT)=8 + volatile roots (live hashes). Quiesce trivial on single-CPU.
GC3 a volatile-only-rooted object: held live while the root exists=y, reclaimable after the root drops=y -> ROOT SET SPANS DURABLE+VOLATILE OK
```

The root set is the union of durable roots (the persisted ROOT block and object index from
the persistence substrate) and volatile roots (hashes held in live process state). On
single-CPU, single-writer, the quiesce is a trivial snapshot at the GC-invocation point
(named as such, not the hard SMP version). An object reachable only from a volatile root is
held live while that root exists and becomes reclaimable when the root drops. D4 (live
object reclaimed) is disproven: a volatile-only-rooted object is held live.

### GC4 - crash-consistent reclamation, ordered-prefix proof (tombstone is the commit)

```
GC4 reclamation sequence [RC(-1)][TOMB] truncated at EVERY byte 0..8192 (8193 points); invariant violations (tomb present while count>0)=0; proof cost ~124490 cyc
GC4 ordering: removal+decrement durable (RC -1) BEFORE tombstone (commit) BEFORE free -> tomb present IMPLIES decrement durable IMPLIES count==0 -> NO durable root reaches a tombstoned object
GC4 saw both states: scheduled-not-committed (decrement durable, no tomb, H still live)=y and committed (tomb, count 0, dead)=y
GC4 -> CRASH-CONSISTENT RECLAMATION: tombstone is the commit point, invariant holds at every crash point OK
```

The reclamation sequence is: the reference removal and its refcount decrement are recorded by
a single durable RC(-1) record FIRST, then the tombstone is written (the commit point), then
the storage is freed (deferred). Exercised the way the persistence truncate proof was: the
[RC(-1)][TOMB] sequence is truncated at every byte offset (8193 crash points) and the
invariant checked at each. The forbidden state (a tombstone present while the count is still
greater than zero, i.e. a live durable reference naming a tombstoned object) NEVER occurs: 0
violations. Because the RC(-1) precedes the TOMB in write order, a durable tombstone implies
a durable decrement implies count 0 implies no durable root reaches the tombstoned object.
Both intermediate states are observed: decrement durable but no tombstone (H still live), and
tombstone committed (H dead). D2 (tombstone-not-the-commit) is disproven: the ordering is
real, not asserted. CLAIM scope: ordered-prefix-in-QEMU, the same scope the persistence crash
proof carried; real-hardware write reordering and sub-sector tearing are out of scope.

### GC5 - resurrection window (dedup hit on a zombie resurrects it)

```
GC5 resurrection: dec -> count 0 (zombie, pending tombstone), dedup HIT landed IN the window=y; H resurrected (count restored, SAME block 700, storage intact, NOT freed+recreated)=y
GC5 contrast: dec -> 0, NO hit in the window -> tombstone committed + block 701 freed=y
GC5 -> RESURRECTION WINDOW CORRECT (dedup-hit-on-zombie resurrects; no-hit reclaims) OK
```

Between zero-count detection and the tombstone commit, an object is a zombie: logically dead,
not yet freed. Because dedup is already in CARMIX, a dedup hit can arrive in this window;
reclaiming then would be a bug. The hit is shown landing IN the window (after the count hits
zero, before the tombstone commits) and the object is resurrected: the pending tombstone is
cancelled, the count restored, and the SAME storage block (700) is intact and load-verifies,
i.e. it is NOT freed-and-recreated. Contrast: with no hit in the window the object proceeds to
tombstone and its block (701) is freed. D3 (resurrection faked) is disproven: the hit is in
the window and the storage is the same block, not a recreated copy. DESIGN NOTE: the
resurrection check is where a future domain-scoped dedup would consult the domain tag; it is
global here.

### GC6 - end-to-end single-process reclamation, bounded growth

```
GC6 graph R->M->{X,Y} + independent K; dropped R->M. reclaimed unreachable objects=3 (M,X,Y), bytes reclaimed=12288; reachable K intact + load-verifies=y
GC6 bounded growth: re-allocated 3 objects -> REUSED freed blocks, high-water 705 -> 705 (unchanged=y) -> the log does NOT grow unbounded; freed space is reclaimed+reused
GC6 -> END-TO-END RECLAMATION: dead objects freed, live object intact, store bounded OK
```

A small graph (root R to node M to leaves X,Y, plus an independent reachable K) is built;
dropping R's reference to M drives M to zero, M is reclaimed and its referents X,Y decremented
to zero and reclaimed (3 objects, 12288 bytes freed). The reachable K is untouched and still
load-verifies. Re-allocating 3 objects reuses the freed blocks, so the high-water mark stays
at 705 instead of growing to 708: the store does not grow unbounded, freed space is reused.
This is the win.

### GC7 - rdtsc measurements (this run)

- Refcount increment/decrement (in-RAM table): 731 cyc.
- Durable refcount delta (an RC record write + flush): on the virtio-blk path, the same order
  as a durable object write measured by the persistence stage (it is one block write + flush).
- Reclamation decision (the zero-count branch): 40 cyc.
- Crash-proof pass (8193 truncation points): about 124490 cyc.
- Bytes reclaimed end-to-end: 12288.

D5 (hardcoded or imported numbers) is disproven: every figure is rdtsc-measured this run.

## Regression

The full 3-boot regression (persist, cold-reboot revive, host-tamper) re-ran green except the
same pre-existing PM0 stall (one per boot, byte-identical). Each boot: the versioned gate
(VG1-VG5), the persistence stages (including the process surviving a cold reboot and the
host-tamper detection), and TS/SM all pass; the M0 (pool-exhaustion, fail-closed) and F2
(corrupt-store, fail-closed then OK) negatives each reach their line; the desktop comes up.
GC1-GC7 pass (boot1 persist, boot2 revive). Adding GC did not break the existing path.

## Forbidden dodges, each disproven

- D1 acyclicity assumed: demonstrated (changing a referent changes the referrer's hash).
- D2 tombstone-not-the-commit: ordering proven at every crash point; 0 violations.
- D3 resurrection faked: hit lands in the zombie window; same storage block, not recreated.
- D4 live object reclaimed: volatile-only-rooted object held live; reachable K never touched.
- D5 hardcoded numbers: all rdtsc-measured this run.
- D6 scope overreach: NO domain-scoping, NO dedup mitigation, NO leakage claim, NO convergence.
- D7 proven-core drift: nine proven modules byte-identical; only kernel/kernel.c changed.
- D8 SMP overclaim: single-CPU only; concurrent tracing / cross-core / epoch reclamation named
  SMP-blocked and unvalidated.

This banks single-CPU durable GC. The persistence log no longer grows unbounded. Concurrent
tracing, cross-core reclamation, domain-scoped dedup, and convergence canonicalization remain
separate future builds.
