# DEDUP_SCOPING_LOG.md - deduplication domain-scoping (DS1-DS6, single-CPU)

Honest record of domain-partitioned deduplication: content dedups only within an authority
domain, so an observer in one domain cannot probe (via store hit/miss timing) for content
held by another domain. This is the leak-free option the dedup side-channel research proved
for the cross-domain channel (zero cross-domain leakage with cross-domain sharing is
impossible, the Armknecht et al. bound). The cost is duplicate storage for content shared
across domains, measured here.

All figures are rdtsc-measured in CARMIX on this run. The research gave no CARMIX numbers,
so every figure is new measurement.

## Precise claim (read first)

This implements domain-PARTITIONED dedup. The CROSS-domain store-hit/miss timing channel is
CLOSED by partitioning. This is NOT a general zero-leakage result: the WITHIN-domain timing
channel still exists and is acceptable (the observer is already in that domain, so it learns
nothing new). It does NOT implement the alternative bounded-residual mitigations (timing
normalization, randomized threshold), which the research priced as different options. The
cost is duplicate storage for cross-domain-shared content, measured below. Single-CPU; a
concurrent cross-core attacker is SMP-blocked and not exercised.

## Built against the committed GC baseline

This build EXTENDS the committed GC infrastructure (commit 40c99cd): the refcount side
table's key gains an authority-domain tag, and the resurrection check becomes domain-aware.
It does not fork a parallel table or dedup path. Diffed against the committed GC snapshot,
this build's delta is only the domain-scoping extension (kernel/kernel.c, +155/-22), so the
GC table and resurrection logic are the given and the attribution is clean.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical after
comment-stripping. Only kernel/kernel.c changed (the gc_rc key gains a domain field; the RC
record carries the domain; the new DS stage). No proven module was edited.

## What was built and shown (observed)

### DS1 - authority-domain tag on the refcount table

```
DS1 same content stored in domain A and domain B: blocks 705 and 706 (distinct=y), counts A=1 B=1 (independent)
DS1 -> TWO PHYSICAL OBJECTS, INDEPENDENT COUNTS (domain-tagged key) OK
```

The refcount table key is now (hash, authority-domain). Identical content stored in domain A
and domain B produces two distinct physical objects (blocks 705 and 706) with independent
counts, not one shared object. The GC build reserved the key for exactly this; domain 0 is
the GC/system domain and every GC caller passes it, so GC behaves as before.

### DS2 - domain-scoped dedup on the store path

```
DS2 same-domain re-store (X in A) hit=y; cross-domain store (Y exists in A, stored in B) hit=n (miss = new object)
DS2 -> DEDUP SCOPED TO DOMAIN (same-domain hit, cross-domain miss) OK
```

A store in domain D dedups only against existing objects in D. Re-storing content already in
the same domain is a hit (count increment, no new object). Storing content that exists in a
different domain is a miss (a new physical object is created in the storing domain).

### DS3 - the cross-domain channel closure, measured (the security result)

```
DS3 store latency (rdtsc avg of 8): genuinely-new(miss)=1275657 within-domain-existing(hit)=2245 cyc
DS3 cross-domain probe for content held by ANOTHER domain: GLOBAL-dedup baseline=42873 (a fast HIT, LEAKS existence) vs DOMAIN-SCOPED=1285349 cyc (a MISS)
DS3 -> CROSS-DOMAIN TIMING CHANNEL CLOSED=y; within-domain hit remains (acceptable residual)
```

The channel is the store latency: a fast store means a dedup hit (the content already
exists), a slow store means a miss (genuinely new). Measured this run:

- Genuinely-new content (a miss): 1,275,657 cyc (includes the object write and flush).
- Within-domain existing content (a hit): 2,245 cyc (count increment, no write).
- A cross-domain probe (probing from another domain for content domain A holds):
  - Under the GLOBAL-dedup baseline: 42,873 cyc, a fast HIT. This LEAKS that the content
    exists somewhere, even though the probing domain does not hold it.
  - Under DOMAIN-SCOPED dedup (this build): 1,285,349 cyc, a MISS, the same order as a
    genuinely-new store (1,275,657). The timing no longer distinguishes "exists in another
    domain" from "does not exist anywhere".

So the cross-domain timing difference that existed under global dedup (a fast 42,873-cyc hit
versus a slow ~1.28M-cyc miss) is GONE under scoping: the cross-domain probe now costs what a
new store costs. The cross-domain channel is closed. The within-domain hit (2,245 cyc)
remains, which is the acceptable residual (the observer is already in that domain). D1
(channel-closure faked) is disproven: the latencies are measured and shown.

### DS4 - domain-aware resurrection

```
DS4 zombie in A (count 0)=y; store same content in B -> new object, A NOT resurrected=y; store same content in A -> A RESURRECTED (count 1, no new storage)=y
DS4 -> RESURRECTION IS DOMAIN-AWARE OK
```

The GC resurrection window resurrects a zombie object on a dedup hit. Made domain-aware: a
zombie object in domain A is NOT resurrected by a same-content store in domain B (B gets a
new object, A's object proceeds to reclamation), and IS resurrected by a same-content store
in domain A (count restored, no new storage, the same storage block intact and load-verifies).
This keeps the partition consistent through reclamation.

### DS5 - the storage cost, measured (the honest price)

```
DS5 same content across 3 domains -> 3 physical objects (global dedup would be 1); duplicate-storage cost=8192 bytes
DS5 -> STORAGE COST IS N-FOLD FOR N-DOMAIN-SHARED CONTENT (measured, not hidden) OK
```

Storing the same content across 3 domains produces 3 physical objects (global dedup would
store 1), a duplicate-storage cost of 8192 bytes (the 2 extra copies) this run. This is the
unavoidable price the research proved for cross-domain leak-freedom. Reported, not hidden. D3
(storage cost hidden) is disproven.

### DS6 - GC still works under the domain tag

```
DS6 GC under domain tag: domain-A object reclaimed (tombstone+free)=y; reachable domain-A object intact + load-verifies=y
DS6 -> GC RECLAMATION STILL WORKS WITH DOMAIN-TAGGED COUNTS OK
```

End-to-end reclamation still works with domain-tagged counts: a domain-A object dropped to
zero is reclaimed (tombstone path intact, storage freed) while a reachable domain-A object is
untouched and load-verifies. The domain tag does not break GC. D4 (GC broken by scoping) is
disproven.

## Regression

The full 3-boot regression (persist, cold-reboot revive, host-tamper) re-ran green except the
same pre-existing PM0 stall (one per boot, byte-identical). Each boot: GC (GC1-GC7, including
the cold-reboot refcount revive on boot2), the versioned gate (VG1-VG5), the persistence
stages, and TS/SM all pass; the M0 and F2 negatives reach their lines; the desktop comes up;
DS1-DS6 pass. Domain-scoping did not break the existing path.

## Forbidden dodges, each disproven

- D1 channel-closure faked: latencies measured; scoped cross-domain == genuinely-new, not the
  fast global hit.
- D2 zero-leakage overclaim: the precise claim is cross-domain closure; the within-domain
  residual is stated and kept.
- D3 storage cost hidden: the N-fold duplicate-storage price is measured and reported.
- D4 GC broken by scoping: GC reclamation shown still working under the domain tag.
- D5 imported numbers: every figure rdtsc-measured this run.
- D6 proven-core drift: nine proven modules byte-identical; only kernel/kernel.c changed.
- D7 not-extending-GC: extends the committed GC table key and resurrection check; no fork.
- D8 built-before-GC-commits: built against the committed GC baseline (40c99cd); the delta is
  only the domain-scoping extension, cleanly attributable.

This closes the cross-domain dedup timing channel by partitioning, at the measured cost of
duplicate cross-domain storage, exactly the trade the research proved unavoidable. It is not a
general zero-leakage result, and the within-domain residual and the cross-core attacker
(SMP-blocked) remain out of scope.
