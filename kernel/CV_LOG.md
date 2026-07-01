# CV_LOG.md - content-addressed convergence, Stage 1 (CV-CORE, CV1-CV7)

Honest record of run_cv() in kernel/kernel.c: canonical form over the reachable acyclic object
graph, with authority folded into the name. Two executions that build the same reachable state
get the same name no matter how they got there. The claim is reachable-state STRUCTURAL
equivalence only. Observational/behavioral equivalence is undecidable and is permanently out of
scope (shown as CV6, a recorded limit, not a bug).

All figures rdtsc-measured this run.

## Baseline (P0)

- Built on committed HEAD 589705f (dev+daily tools 1e2a3d3 and user-model docs 589705f under it,
  and beneath those U-3/U-4 7689692, distrusting migration 920a43f, hardware cycle 1 e9243a3,
  the content-addressed filesystem, the versioned gate, GC, dedup scoping, and SMP).
- Working tree clean against that HEAD before the first new line; full regression green to the
  last stage with the single accepted PM0 stall.
- The nine proven modules are byte-for-byte identical (comment-stripped diff empty) pre-flight
  and post-build. run_cv is new code in kernel/kernel.c only; no proven-core file was touched.

## The CARMIX-specific constraint: authority is in the name

The security core of this build is CV2. If authority were not hashed into the canonical form,
convergence would fabricate identity across privilege: a low-authority state could canonicalize
to the same name as a high-authority one, which is amplification by convergence. CV2 demonstrates
the refusal directly: two states with an identical data graph but one differing capability
canonicalize to DIFFERENT names. That refusal firing is the most important result here.

## Reachability reuses the committed GC, not a new pass

CV3 determines reachability with the committed refcount oracle gc_rc_get(hash, domain) > 0, the
same code path GC and the U-2 heap already use. No parallel mark or reachability pass was written
(that would be the forbidden dodge D4). An object the committed refcount reports dead is excluded
from the canonical form, so dead scratch does not change the name.

## What was built and shown (observed on real serial)

```
CV0 reachability oracle = the COMMITTED refcount path gc_rc_get(hash,domain)>0 (NOT a parallel mark pass); gc_rc entries in use=64/256
CV1 3-node chain root->mid->leaf canonicalized (bottom-up): name=66dbb06bbb30f91daff7 visited=3 nodes (terminated)
CV1 re-canonicalize SAME state -> 66dbb06bbb30f91daff7 byte-identical=y -> DETERMINISTIC (disproves D2) OK
CV2 same data graph, ONE capability differs (root perms LOAD vs LOAD|STORE): 66dbb06bbb30f91d vs ee447bbda434e30d
CV2 -> DIFFERENT NAMES: authority is IN the name, one-cap difference REFUSED OK
CV3 dead-scratch(gc_rc_get==0) excluded; reachable state canonicalizes identically to the no-scratch state OK
CV4 build-A order[leaf,mid,root] vs build-B order[root,scratch,mid,leaf]+transient scratch -> ONE name e9d8e35e990c4991 OK
CV5 two byte-distinct records (opposite child order+placement): canonical-equal=y byte-equal=n
CV5 intra-domain dedup: first=MISS second=HIT  cross-domain(same content)=MISS OK
CV5 corpus K=4: distinct canonical names=1 vs distinct byte-identity names=4 -> canonical:byte dedup ratio 4:1
CV6 two graphs, SAME observable (eval=42 vs 42), DISTINCT structure: names 9ea57700428bb1e5 vs a5c0176f55b5cb6f -> DO NOT converge
CV7 capture boundary: raw-capture(1 obj)=78787 cyc  canonical-capture(2-node graph)=165602 cyc
CV7 INTEGRATION CHOICE (forced by the number): 165602 cyc is 2x a raw capture; captures are frequent -> canonical pass at RECLAMATION-time (amortized), NOT per-capture
CV7 two byte-distinct equivalent captures -> one canonical name; new stored objects=0 dedup-hits=2; rematerialize+re-verify(BLAKE3)=y OK
```

- CV1 the canonical form is bottom-up over the reachable acyclic graph (leaves first, child
  references replaced by canonical child hashes, deterministic field ordering, then BLAKE3 of the
  canonical bytes), well-founded by the same acyclicity the GC relies on. It terminates on real
  captured state and is deterministic: the same state canonicalized twice yields a byte-identical
  canonical form and identical hash.
- CV2 authority is in the name, the one-capability difference is refused (different names).
- CV3 dead scratch is excluded via the committed refcount oracle.
- CV4 two materially different construction sequences (different order, different placement, a
  transient scratch object that later dies) converge to one identical name.
- CV5 canonical dedup within a domain is strictly stronger than byte identity (a 4:1 ratio on a
  small corpus), and cross-domain equivalence never dedups (a genuine miss), so the domain
  channel closed by the committed dedup-scoping work is not reopened.
- CV6 two states that are observationally equivalent but structurally distinct do NOT converge.
  This is the proven limit (observational equivalence is undecidable), recorded as a result the
  same way the dedup impossibility was, not a bug.
- CV7 canonicalization is wired at a real capture boundary. The measured per-capture cost
  (165602 cyc, about twice a raw capture) forced the honest integration choice: run the canonical
  pass at reclamation-time (amortized over the GC sweep), not on every capture. Equivalent
  captures still rematerialize under one name and re-verify by BLAKE3.

## Measurement (rdtsc, this run)

```
CV canonicalization per full graph=1778237 cyc / 3 nodes = 592745 cyc/obj
CV canonical-hash(graph)=1778237 cyc  vs  raw-hash(1 obj)=101745 cyc
CV convergence-check(compare)=46825 cyc
CV5 canonical:byte dedup ratio on the K=4 corpus = 4:1
CV7 raw-capture=78787 cyc  canonical-capture=165602 cyc (2x, forced the reclamation-time choice)
```

Canonicalization is not free: it costs roughly the whole subgraph re-hashed in canonical order,
which is why the integration choice is amortized reclamation-time rather than per-capture. The
number is reported plainly rather than hidden.

## Regression

The full single boot re-ran green with exactly one fail-marker, the accepted pre-existing PM0
stall. The M0 exhaustion negative reached its fail-closed line and F2 reached its fail-closed OK.
All prior stages (store, gate, GC, dedup, persistence, versioned gate, U-0..U-4, migration, FS,
networking, debugging, userland, drivers, hardware, graphics, integration, tools) still pass.

## Forbidden dodges, each disproven

- D1 authority-blind canonical form: CV2 refusal fires, one-cap difference yields different names.
- D2 nondeterministic canonicalization: CV1 re-run is byte-identical.
- D3 fake path independence: CV4 uses materially different construction sequences, not one run twice.
- D4 parallel reachability code: CV3 calls the committed gc_rc_get refcount oracle, not a new mark.
- D5 cross-domain canonical dedup: CV5 shows the cross-domain miss.
- D6 semantic overclaim: only reachable-state structural equivalence is claimed anywhere; CV6
  records the observational-equivalence boundary as undecidable.
- D7 imported numbers: every figure above is rdtsc-measured this run.
- D8 proven-core drift: the nine proven modules are byte-identical.
- D10 seam: this is Stage 1 alone, committed at the CV-CORE seam, separable from Stage 2.

## Scope

Reachable-state structural equivalence only. Observational equivalence is undecidable and
permanently out of scope. Single machine, single CPU. Stage 2 (CV8, concurrent convergence on the
two real cores, the first measured exercise of the multi-core DRCC theory) is gated on committed
U-3, which is present (7689692), and follows on top of this committed Stage 1.

# CV8 - CV-DRCC (Stage 2): first measured exercise of the multi-core DRCC theory

Stage 2 runs run_cv's SAME canonical form (cv_canon) over the reachable graph left by a workload
that runs on BOTH real cores. It is placed in kmain in the LIVE-AP window - after run_cu, before
run_u3u4 parks the AP - the only window where a second core actually executes. The cross-core
dispatch is the committed real 2-core path (ap_kick / ap_wait + u3_drf, the same U-3/CU-4 use),
under MTTCG (-accel tcg,thread=multi, -smp 2). Nothing new invented; no proven-core file touched
(kernel/kernel.c only). AP startup latency this run = 434204 cyc (rdtsc), second core confirmed
executing.

## The two workloads (real concurrency, one converges, one diverges)

- DRF: each core writes ITS OWN disjoint 32-byte slot (u3_drf, racy=0), concurrently across the
  two cores - no shared mutable state, no data race. The final reachable state (both slots) is
  loaded into a 3-node graph (root -> slot0-leaf, slot1-leaf), the leaves are marked reachable via
  the committed gc_rc oracle, and the graph is canonicalized with cv_canon.
- Racy control: both cores race the SAME slot UNLOCKED (u3_drf, racy=1) - the SMP lost-update
  proof. The torn slot is canonicalized the same way.
- Each workload is repeated N=12 times (N >= 10). The full N-run canonical-hash SET is printed for
  both.

## What was observed (real serial, this run)

```
CV8 DRF  workload (each core mutates its OWN disjoint slot, concurrent on 2 cores), N=12 runs -> distinct canonical names=1
CV8   DRF  set[0]=fe59feafdda3f9141125bec12c5cb43f
CV8 RACY workload (both cores race the SAME slot UNLOCKED - the SMP lost-update proof), N=12 runs -> distinct canonical names=12
CV8   RACY set[0]=b9c490b37594c3a64d2a5ad1ee4b7aa0
CV8   RACY set[1]=2b7f8fdbe931e9767e8d0e65468a5d00
CV8   RACY set[2]=db8baac0eaf7b4382e8531582f3dc404
CV8   RACY set[3]=4abc7888fb628f8ea7a57b6b55d6140a
CV8   RACY set[4]=ce2db9986e2854611584af3dad236a86
CV8   RACY set[5]=b09c6d37d3727a3b16cd89f8bf7b7ba3
CV8   RACY set[6]=431015a58294c6ae2148d7b9ad0fcf17
CV8   RACY set[7]=cb19aec618734c625a4349954634f975
CV8   RACY set[8]=f605e108adcd14cf5122f9ea08bf6cd0
CV8   RACY set[9]=c629b081e59df05d9e4c01db94ac98b8
CV8   RACY set[10]=0def5b0741cd0e06e9302d4e399bcec3
CV8   RACY set[11]=da57fe38dfde981d5db708ef480ab974
CV8 MEASURE (rdtsc this run): per-run canonicalization DRF=285383 cyc  racy=273623 cyc (final reachable graph, 3 nodes)
CV8 -> DRF converges to ONE canonical name across all 12 runs; the racy control DIVERGES (12 names) -> real 2-core concurrency confirmed OK
```

- The DRF workload converged to ONE canonical name across all 12 runs (hash set size 1). The whole
  N-run DRF hash set is exactly {fe59feafdda3f9141125bec12c5cb43f}.
- The racy control DIVERGED to 12 distinct canonical names (hash set size 12, every run different).
  The divergence is what proves the harness is exercising REAL cross-core concurrency: a single
  core could not tear the shared slot into 12 different results. Had the racy control not diverged,
  CV8 would have been reported failed (harness not real concurrency) and nothing committed.

## Measurement (rdtsc, this run)

```
CV8 per-run canonicalization DRF = 285383 cyc  racy = 273623 cyc  (3-node reachable graph)
AP startup latency = 434204 cyc
```

Per-run canonicalization is measured on the final reachable graph each run; DRF and racy cost the
same order (the racy torn slot is the same graph shape), so the divergence is purely in the CONTENT
the two cores raced to, not in canonicalization cost.

## Regression

The full single boot re-ran green with exactly one fail-marker, the accepted pre-existing PM0
stall (INT-1 self-check confirms "exactly ONE accepted fail-marker line (the PM0 stall) OK"). The
M0 pool-exhaustion negative and the F2 materialize-verify negative reached their fail-closed lines.
U-3 (U3-1, U3-3, U-3 COMPLETE) and CV-CORE (Stage 1, CV1-CV7) still pass unchanged, and gc_rc
stayed at 63/256 after all stages.

## DRCC scope (D9, non-negotiable)

This is the FIRST measured exercise of the multi-core DRCC theory. It is VALID FOR THE EXERCISED
WORKLOADS AND SCHEDULES ONLY - not a universal proof. It shows, on the two real cores under this
run's schedules, that a data-race-free workload reaches ONE content-addressed reachable state
(convergence) while a genuinely racy workload does not (divergence). The claim remains
reachable-state STRUCTURAL equivalence with authority in the name, NEVER observational/behavioral
equivalence (that boundary is CV6, undecidable, permanently out of scope).
