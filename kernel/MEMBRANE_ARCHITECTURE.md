# CARMIX Membrane Architecture

A build-ready, honesty-audited design document. It states one law, applies it to five permanent boundaries, sketches the next-era primitives that reuse the built spine, gives the composition algebra, draws the mature picture, and names the single first build. Every claim carries its class. Where a result is gated, emulated, prior-art, or unbuilt, the prose says so inline rather than at the bottom.

A note on standing, stated once and honored throughout. "Machine-checked" and "proven" are reserved for the seven-permission `re_mint` monotonicity lemma plus finite-tree acyclicity in kernel/Carmix.v. Every runtime gate that leans on those lemmas (kernel/cvsasx_swcap.c and its callers) is tested against attack tables, not refined to its C. So wherever a construct re-runs the anti-amplification gate, the checked object is an abstract model and the running object is tested C. This document uses the phrase "gate-checked (model proven, C tested)" for that state and does not shorten it to "machine-checked."

---

## 0. The Membrane Law, operationalized

The law is a design-time classifier, not a runtime service. Run it on any proposed construct X before any code exists. The output class fixes exactly what X may claim. Run the gates in order. The first gate that fires decides the side.

### 0.1 The membrane sort (decision procedure)

The gates below are not narrative. Each is a yes/no question a designer answers about X from X's own definition, and each has a worked example so the classifier is falsifiable rather than post-hoc.

**G0 (existence).** At the moment X's identity is decided, does X's value depend only on bytes that ALREADY EXIST? If X's meaning references not-yet-existing bytes, wall-clock "now", "eventually", a deadline, or a remote party's future conduct, X is LIVE. A hash cannot name it. Stop.
Worked: "the hash of the current process state" passes G0 (the bytes exist). "a token that is valid until 5pm" fails G0 (its meaning references a future clock reading), so it is LIVE.

**G1 (internal vs external).** Does X stay inside a party that runs the gate? If X involves a non-CARMIX peer OR an external effect (a packet on the NIC, a pixel on the framebuffer, a device register, a human gesture), X is LIVE-EXTERNAL. Only unilateral integrity applies (re-hash received bytes). No authority claim binds the far side. Stop.
Worked: "verify the migrated state B sent" is LIVE-EXTERNAL, integrity is unilateral, B's honesty is not bindable.

**G2 (physical fact).** Does X's correctness rest on freshness, environment genuineness, real energy, or a real clock? Then X is PHYSICAL. It needs a hardware root. Today the software AUTH keypair EMULATES that root, so X may claim the protocol, never the security. Stop.
Worked: "this quote proves a genuine machine signed it now" is PHYSICAL, emulated today, so it demonstrates the protocol against no real adversary.

**G3 (determinism).** X is internal and over existing bytes. Is X on the deterministic, data-race-free spine (reproducible by re-run at a synchronization-point cut)? If NOT (RNG, races, GPU float, device input), X is FROZEN-RETAINED-ONLY. It is nameable-by-hash and therefore integrity-checkable, dedupable, diffable, and time-travelable by RETENTION, but it may NOT claim reproduction-by-rerun and may NOT anchor a transition-correctness proof.
Worked: a task holding an RNG cap is FROZEN-RETAINED-ONLY, you can pin and rematerialize it bit-exactly, you cannot re-run it to the same hash.

**G4 (default).** X survived G0 to G3. X is FROZEN-REPRODUCIBLE, still gated on DRCC being validated for the SMP spine (see the first-build recommendation).

### 0.2 What each class may claim

- **FROZEN-REPRODUCIBLE:** name-by-hash, integrity, intrinsic dedup, structural diff, state-provenance, anti-amplification bounding, time-travel by retention, AND reproduction-by-rerun / transition-correctness-by-remat. The last two are labeled "gated on DRCC".
- **FROZEN-RETAINED-ONLY:** everything above EXCEPT reproduction and transition-correctness. The handle is retention (pin the hash), not re-derivation.
- **LIVE-EXTERNAL:** unilateral content integrity only. No order, no time, no bound on the peer. Downgrade the party to a data source.
- **PHYSICAL:** emulated attestation only until a hardware root exists.
- **LIVE (G0):** not nameable at all. The honest deliverable is the boundary statement plus its partial escape (attested-tick for time, join-semilattice for multi-writer, hardware root for freshness), never a claimed solution.

### 0.3 The cross-cut after the sort

If X crosses an authority boundary, the AUTHORITY axis reaches exactly one step past the content axis. It gates the reachability edge (may this be instantiated here?) and bounds it (dest <= source), but it still cannot impose order, name time, or bind an ungated party. So a FROZEN object crossing a domain is content-verified by re-hash AND authority-re-minted at the crossing. These are two separate checks on two separate axes, never conflated.

### 0.4 Anti-overclaim tie-break

"Content-addressed X" is not automatically novel. The commitment covers its bytes, which is true of any commitment. If X's only claimed novelty is "authority is in the hash", ship the discipline, not a novelty claim. Any X leaning on "the gate is correct" inherits the model-proven / C-tested gap. Any X leaning on DRCC inherits the unvalidated-premise gap. Both must be labeled in X's claim.

### 0.5 Architectural patterns (each an invariant a build must satisfy)

1. **Freeze-then-name.** Never hash a value mid-mutation. Drive the mutators to a quiescent synchronization-point cut FIRST, snapshot there, then hash via the store put / BLAKE3 path and fold diff-proportionally through kernel/store/epoch_tree.c so only changed leaf-to-root paths re-hash. On SMP the cut is the multi-core release-cut capture. Only at COARSE boundaries (epoch, pipeline, window subtree, app closure). Per-switch / per-keystroke / per-frame state stays on the direct blit/register-swap fast path. Output class is FROZEN-REPRODUCIBLE only if the cut is data-race-free, otherwise FROZEN-RETAINED-ONLY.
2. **Capability-gate-the-reachability-not-the-content.** Do not gate reads of the bytes. Bytes are immutable and self-verifying by re-hash, so integrity is unilateral and free. Gate the EDGE that instantiates the object in a new domain. At that edge, verify the referent hash (fail CVSASX_ERR_REFERENT_MISMATCH), then mint a fresh local cap bounded by dest <= source, clamped to canonical perms/bounds. The wire carries a signed ceiling (a bound), never a bearer token. The same dest <= source relation is the one theorem reused from swcap down to the emulated-VT-d DMA window.
3. **Rematerialize-and-compare.** A distrusting party re-derives from named inputs and hash-compares instead of trusting a shipped token or an unproven claim. Sound ONLY on the DRF spine and gated on DRCC validation. Exclude any stage holding clock/RNG/net/device caps. The succinct (zkVM/IVC) version that avoids re-execution is needs-research and collides with BLAKE3 arithmetization-hostility plus per-step prover cost. Do not promise it near-term.
4. **Explicit-order-structure-over-content-objects.** Content-addressing yields a SET, not a sequence. Never infer order from hashes. Build an explicit ordered structure (append-only Merkle log, ROOT-block epoch chain, MVCC version word) and sign its root. Residue that must be in the claim: this gives tamper-evident order for a SINGLE writer, not total order under concurrency. The double-spend problem stays irreducible. Even a signed order is not consensus.
5. **Unilateral-integrity-at-the-external-edge.** Downgrade every external party to a data SOURCE. Re-hash everything received before use. Treat any capability the peer presents as a REQUEST the local gate re-mints from LOCAL grants, never as authority, because the peer never ran the gate. For external effects obey output-commit: time-travel/remat recovers internal state and can never un-send a packet or un-show a frame, so state the frozen root as "up to the last externalized output".
6. **Authority-in-the-hashed-bytes.** Fold authority metadata INTO the content BEFORE hashing, so two states of differing authority cannot alias to one name and flipping any ceiling changes the root. Claim only the DISCIPLINE, not cryptographic novelty. A folded label buys a monotone-label lemma at best, NOT noninterference. Do not borrow seL4's standing.
7. **Retain-don't-reproduce.** For non-DRF state (held RNG, races, device input, GPU float) the only honest handle is RETENTION: pin the task-hash as a GC root (durable refcount, tombstone, resurrection window). You may rematerialize it bit-exactly if retained and diff/dedup it, but you may NOT claim to reproduce it by re-running. Every use carries an OPEN retention-policy cost.

### 0.6 Invariants (violation is a build bug)

1. **NO BEARER TOKEN CROSSES.** Only a signed authority ceiling crosses. The destination re-mints a fresh local cap. Violated the moment any wire format carries a usable capability.
2. **ANTI-AMP HOLDS AT EVERY AUTHORITY CROSSING.** The single anti_amplification relation is reused at spawn, IPC, delegation, cross-core, the migration wire, and the emulated IOMMU DMA window. No crossing may bypass the gate. (Maturity is not uniform across this list, see the mature-architecture note in section 5.)
3. **RE-HASH BEFORE USE ACROSS ANY TRUST BOUNDARY.** Every transferred, fetched, or resumed byte is re-hashed before it is trusted. Every re-mint verifies the referent hash and fails CVSASX_ERR_REFERENT_MISMATCH on mismatch.
4. **NOTHING IS HASHED MID-MUTATION.** A name may only be minted for a quiesced value at a synchronization-point cut.
5. **AUTHORITY IS FOLDED INTO THE HASHED BYTES.** A ceiling flip always changes the root.
6. **NO REPRODUCTION CLAIM OFF THE DRF SPINE.** Non-deterministic state may be named and rematerialized only by retention, never claimed reproducible-by-rerun, never used to anchor a transition proof.
7. **NO HASH NAMES A TEMPORAL OR LIVENESS PREDICATE.** Freshness, deadlines, "eventually", and liveness require a real timer or hardware root and are labeled EMULATED until one exists. Real-time queries are refused, not answered.
8. **NO AUTHORITY BOUND IS ASSERTED ON A NON-CARMIX PEER.** The external side of any edge gets content integrity only.
9. **ORDER IS NEVER INFERRED FROM CONTENT IDENTITY.** Ordering comes only from an explicit signed append-only structure, and even then total order under concurrency stays out of scope.
10. **EVERY GATE CHECK FAILS CLOSED WITH A DISTINCT REASON.** Default is refusal.
11. **CONTENT-ADDRESSED ACTIVATION STAYS AT COARSE BOUNDARIES.** Rematerialize-by-hash is 2 to 3 orders costlier than the direct path on the measured (QEMU-virtual) workload, so it is forbidden on the interactive path.
12. **CLAIM STANDING IS LABELED, NOT BORROWED.** Anything leaning on "the gate is correct" carries the model-proven / C-tested label. Anything leaning on DRCC carries the unvalidated-premise label. Any freshness/genuineness claim carries the emulated-root label.

---

## 1. Boundary 1: Real-time and liveness (a double boundary, both edges permanent)

**Edge 1 (naming).** A BLAKE3 hash is a function of bytes that already exist, so it can never encode "now", "eventually", or "within deadline D". This edge is permanent by Rice's theorem, independent of any measurement.

**Edge 2 (cost).** Activate-by-hash is a measured latency floor. On the current QEMU-virtual run, from-hash activation exceeds a register swap above a small dirty-chunk threshold (the C3 crossover, `g_c3_fromhash_cyc` vs `g_c3_resident_cyc` in kernel/kernel.c). Honesty note that outranks the confidence of this edge: BOTH clocks are virtual on QEMU, so the 2-to-3-order figure is a proxy. The cost-edge exclusion of hard real-time is strongly indicated by the measured shape and by the architecture (remat touches more state than a register swap), but it is asserted from virtual cycle counts and must be re-measured on real hardware before it is called a proven permanent ceiling. The naming edge is what makes hard real-time permanent without qualification.

Honest scope: SOFT real-time at COARSE boundaries with a-posteriori deadline VERDICTS. Never a-priori schedulability. CARMIX offers none of the Liu and Layland rate-monotonic/EDF analysis.

### 1.1 Within-strategy (five patterns)

- **A, two-clock capture.** Every temporal reading captures BOTH `rdtsc()` (fine-grained, TCG-noisy, not wall-clock) AND the calibrated LAPIC-timer 100Hz `ticks` counter (the real periodic anchor, calibrated against the PIT at boot, roughly 63,145,600 ticks/sec this run). The cycles-to-wall conversion is that boot-calibrated figure kept as a live CALIBRATION KNOB, re-measured per boot, never hardcoded, because a real TSC may not be invariant or constant-rate.
- **B, coarse-boundary-only invariant.** The remat path may sit on a deadline only when deadline_budget is much larger than the measured C3 from-hash cost. Route any deadline-adjacent scheduling decision through the existing `should_dematerialize` / `pp_breakeven_cyc` gate. Interactive paths stay on the direct register-swap fast path.
- **C, clock-independent primary bounds.** Every progress/liveness-adjacent bound has an attempt-counted form (count scheduling attempts, IPI round-trips, epochs) as the invariant OF RECORD. The wall-clock figure is a secondary, calibration-gated refinement. This confines temporal-undecidability residue to one labeled code path.
- **D, deadline as a VALUE.** A deadline check is `met = (rdtsc_end - tick_start.tsc) <= budget_cyc`, an a-posteriori boolean, content-addressable and re-checkable by pure arithmetic over content-addressed inputs. The system EMITS verdicts, it never promises the next run meets the deadline.
- **E, liveness stays outside by refusal.** The existing DBG-5(c) branch REFUSES real-time/liveness queries over rematerialized state. Hang detection needs non-content-addressed live watchdog/heartbeat state, added as explicitly live, non-addressed state. Never infer liveness from the state DAG.

### 1.2 Partial escape: attested-tick annotation

Object: `{ tsc, lapic_ticks, core, epoch }`, content-addressed `h = BLAKE3(record)`. A captured tick is a VALUE, therefore on the frozen side, therefore nameable. Two shippable consequences:

1. Time-of-capture becomes part of content identity. Embed the tick-hash into a task-hash so a rematerialized state carries the instant it was frozen as immutable, diffable content. Extend the existing migration authrec (root, scope, ceiling, nonce, expiry) with a capture-tick field.
2. Deadline CHECKING against a real timer. A start-tick plus an end rdtsc yields a re-checkable met/missed verdict. Fold `(H_pre, P, H_post, tick_start, tick_end, deadline_met)` into the transition-log leaf.

SMP extension (scaffold): an attested-tick CUT is the tuple of per-core ticks captured on a coordination IPI, the temporal companion to the DRCC release-cut. The cut's timestamp is a bounded-uncertainty interval `[min_tsc, max_tsc]`, precisely Spanner-TrueTime's honest shape.

Honesty caveat carried inline: on the emulated root the tick's SIGNATURE demonstrates the protocol, not security. "Signable" here means the byte layout is signed and re-verifiable, not that the signature binds a genuine instant. A signed stale tick replays.

### 1.3 Residue

- **R1 liveness prediction stays UNDECIDABLE** (Rice). CARMIX only reports, after the fact, whether P finished. PERMANENT.
- **R2 fine-grained content-addressed preemption is excluded by the C3 crossover.** Strongly indicated as permanent by architecture and the measured shape, asserted from QEMU-virtual numbers, so re-measure on hardware before calling it proven. Soft real-time at coarse boundaries is the working ceiling.
- **R3 the wall-clock anchor is only as trustworthy as its boot calibration and the hardware clock,** and on the emulated root the signature is protocol-only. Retiring this needs a hardware root plus a trustworthy monotonic clock. OPEN, hardware-blocked.
- **R4 cross-core "now" is skew-bounded, an interval not a point,** and its soundness as a consistent cut is the DRCC line, unvalidated on the built SMP. CURRENT.
- **R5 every number here is a proxy until real hardware.** This is why the attempt-count is the invariant of record and the wall-clock figure is never load-bearing.

Net for a builder: content-address a captured tick, sign its layout, embed it, emit re-checkable deadline verdicts at coarse boundaries on the real LAPIC timer. Do none of predict liveness, meet a fine-grained deadline through remat, prove freshness on an emulated root, or name a single global instant across cores.

### 1.4 Buildable-now / needs-research / open

Buildable-now on the existing SMP + LAPIC timer: the attested_tick record + BLAKE3 + Ed25519 signature over its layout, capture-tick embedded in task-hash and migration authrec, the a-posteriori deadline verdict, the coarse-boundary invariant via the existing break-even gate, the attempt-counted bound as invariant of record, the per-core attested-tick cut as MECHANISM only (reported as a skew interval).
Needs-research / open: soundness of the cross-core cut (DRCC), a hardware root plus monotonic clock for freshness, any hard-real-time or schedulability guarantee (excluded, not deferred).

Reuses: `rdtsc()`, the 100Hz `ticks` timer, `SYS_GETTIME`, `g_boot_tsc`, the calibrated LAPIC timer, the SMP real LAPIC-IPI path, `should_dematerialize` / `pp_breakeven_cyc` and the attempt-counted cooldown, the C3 crossover measurement, the DBG-5(c) refusal branch, `cvsasx_blake3`, `crypto_sign` with the AUTH keypair, the migration authrec, and kernel/store/epoch_tree.c.

---

## 2. Boundary 2: Mutable shared state (irreducible for the raw case)

Content-addressing names immutable values. A write is a NEW name, so two independent writers diverge into two hashes with no canonical merge, and re-rooting is O(depth) per write (surfaced by FS8 at roughly 71178 cycles at depth 2, a QEMU-virtual figure). Delivered scope today is single-writer-shared and quiescent (SHAREDMAP: one physical frame survives a round trip with per-owner ceilings). The construct the thesis cannot express is a canonical reconciliation of two concurrent, order-dependent mutations. This design does not dissolve that. It carves out the sub-case content-addressing CAN name.

### 2.1 Within-strategy: restrict to a join-semilattice

Admit only shared mutable state whose values form a join-semilattice and whose mutation is monotone. For a semilattice there IS a canonical merge, the least upper bound, and because the LUB is a deterministic pure function of its inputs it has a canonical hash. That converts "no canonical merge" (true of arbitrary values) into "one canonical merge with one canonical name" (true of a semilattice). MERGE = REMATERIALIZATION: materialize the two divergent states by hash, compute the LUB, store_put the result. The merged hash is a pure function of the SET of delivered deltas, not their arrival order.

Prior-art anchor that makes both the escape and its limit honest: the CALM theorem (Hellerstein, Alvaro). Monotone computations need no coordination, non-monotone ones provably do. CARMIX's restriction to a join-semilattice IS the monotone half of CALM. The CARMIX-side additions are narrow and are named as such, not sold as new results: (a) the LUB is stored in the content store and re-rooted diff-proportionally, which is storing a CRDT state in a content store, and (b) strong eventual consistency is READ OFF as hash equality, which is applying content-addressing to an already-canonical value. The load-bearing content (CALM monotonicity, delta-state join, the CvRDT SEC theorem) is entirely prior art. Delta-state CRDTs (Almeida, Shoker, Baquero 2018) are chosen over op-based because op-based needs the causal-ordered delivery CARMIX lacks, while delta-state needs only unordered at-least-once delivery.

Invariants a build must satisfy:
- INV-1 semilattice law: merge is commutative, associative, idempotent, enforced by exposing ONLY a fixed typed library (G-Counter, PN-Counter, OR-Set, delta-map, LWW-register with content-addressed tie-break). A non-lattice type is refused at registration.
- INV-2 deterministic join to canonical hash: all tie-breaks total and deterministic, LWW ties broken by BLAKE3 comparison of the competing value hashes.
- INV-3 monotone merge-authority: the merge-cap is re-minted <= its ceiling via the anti-amp gate, structurally identical to the SHAREDMAP per-owner rule. An amplifying delta is refused CVSASX_ERR_AMPLIFY_PERMS, a wider-bounds delta CVSASX_ERR_BAD_BOUNDS.
- INV-4 delta is a content-addressed object, re-verified on read, fail-closed. No token crosses.
- INV-5 batch-to-amortize: join a batch, re-root ONCE per epoch via the epoch tree, commit via the single-word MVCC version bump so a concurrent reader sees pre- or post-canonical, never torn.
- INV-6 unordered idempotent delivery: because join is idempotent and commutative, the channel needs no causal ordering.

### 2.2 Partial escape

TWO independent writers may each mutate concurrently, each ships a delta, and they CONVERGE with no coherence protocol, because the join is the reconciliation and its result is a third canonical hash. This escapes SHAREDMAP's stated frontier for the monotone fragment only. The escape is exactly the CALM-monotone half of the state space, no more.

### 2.3 Residue

1. **Raw multi-writer coherence stays irreducible.** Any value requiring order-dependent reconciliation (a read-modify-write cell, an account balance after two concurrent withdrawals) has no canonical merge. This is the double-spend negative restated. CRDTs buy convergence by surrendering general-purpose mutation. That surrender is the permanent cost.
2. **Quiescence / causal-stability detection is a liveness property CARMIX lacks.** "Have all deltas arrived?", "is this merge final?", "is this OR-Set tombstone safe to GC?" are temporal predicates a hash cannot name. So the honest statement is sharper than "SEC delivered": you get strong EVENTUAL consistency, but the system can never DECIDE it has reached the eventual state, and tombstone/metadata GC is blocked on this undecidable stability, so retention grows monotonically. The escape DEPENDS ON boundary 1 and cannot outrun it.
3. **Distrusting multi-writer inherits boundary 3.** A non-CARMIX peer never runs the gate, so its delta is downgraded to a data source and its claimed authority re-minted from local grants only.
4. **Proof-vs-code and DRCC.** The semilattice laws and merge-authority rule are runtime-tested (the SM4 adversarial-refusal pattern), not machine-checked. The "same delta-set to same hash" claim is single-CPU demonstrable but on SMP inherits the unvalidated DRCC premise.

### 2.4 Buildable-now slice

A single-CPU, quiescent slice on built plumbing: a `crdt_state` object content-addressed via store put/get, starting with a delta-map of G-Counters. `merge(a,b)` = per-key max, re-rooted diff-proportionally through the epoch tree. Merge gated by a `merge_cap` swcap driving the SM4 adversarial vectors verbatim (amplify returns status 12, wider-bounds returns status 9). Batch N deltas, one re-root per epoch, commit into the MVCC versioned slot. Headline demonstration: feed two replicas the same delta-set in different orders, show the final canonical hash is EQUAL. Measure re-root cost per batch in rdtsc (never hardcoded). Multi-core convergence and quiescence-driven metadata GC are explicitly NOT in this slice.

Reuses: store put/get, `cvsasx_sw_cap_remint`, the SHAREDMAP per-owner-ceiling attach + SM4 refusal harness, the epoch-tree re-root machinery, the MVCC versioned slot, the rematerialize path, the diff-proportional delta channel, domain-partitioned dedup, durable refcount + tombstone GC. Prior art (named, not invented here): CRDTs (Shapiro et al. 2011), delta-state CRDTs (Almeida et al. 2018), the CvRDT SEC theorem, CALM and Bloom (Hellerstein, Alvaro), deployed systems Riak/Automerge/Yjs.

---

## 3. Boundary 3: External non-CARMIX party (a shared boundary, not a CARMIX win on the authority half)

The inexpressible construct: an authority bound on the CONDUCT of a party that does not run the gate. A foreign party never calls re_mint, so the ceiling binds only the local endpoint. This is a hard ASYMMETRY:
(a) content integrity is UNILATERALLY enforceable (re-hash received bytes, tamper fails by construction, zero trust in the sender);
(b) authority on the peer's side is NOT enforceable (any capability it presents is unbacked by a local re-mint).
Prior art placing this correctly: CapTP and Cap'n Proto share this exact limit, so unilateral re-hash integrity is the surviving CARMIX advantage, while the authority half is a SHARED boundary, not a CARMIX win. SPKI/SDSI (authority rechecked locally against local grants) is the closest prior art to the discipline. The one genuine lead over confidential-VM migration (SEV-SNP/TDX opaque blob) is that the destination re-verifies every byte independently.

### 3.1 Within-strategy: the Foreign Ingress Gate (FIG)

One structural choke point, the generalization of the distrusting-migration verifier `am_dest` lifted off the two-QEMU ivshmem harness. Do not scatter trust decisions across callers. One gate, distinct fail-closed verdict per check, re-mints nothing on failure.

LAYER 1, unilateral-integrity discipline (works against ANY peer, zero shared root):
- INV-1 data-source downgrade: a foreign object is admitted ONLY through the store put, whose returned name IS BLAKE3(bytes). A foreign-supplied hash is at most a WANT hint. Reuse the `b_sync` loop: WANT then BLOCK then demand BLAKE3(received) == requested else REJECT, `store_exists` prune so a held subtree is never re-fetched.
- INV-2 capability-as-request: any presented authority is parsed into an authrec and treated as a REQUEST, honored only by re-minting from LOCAL grants bounded by the local ceiling. A presented cap with no backing local grant re-mints nothing.
- INV-3 no ambient outward reach: outbound reach is itself a swcap over an immutable content-addressed endpoint descriptor.

LAYER 2, attested delegation, admissible ONLY where a shared root exists:
- INV-4 authority forks on the trust anchor: the measurement step is the fork. If the peer's attested identity is recognized (or its key chains to AUTH_PK via the enroll/rotate/revoke trust store), take the attested path (quote sig, measurement accepted, attested root equals the root re-verified by re-hash, bounded re-mint). Else the peer is a PURE DATA SOURCE with authority = empty set.
- INV-5 freshness is checked, not trusted: replay/expiry via nonce-seen plus a counter, which is a proxy not a clock (see residue).

### 3.2 Partial escape

Buildable-now: a general FIG delivering FULL unilateral integrity for any peer and bounded authority for a peer sharing the emulated root. It reuses `b_sync` re-hash-and-prune as the integrity membrane, the 4-step `am_dest` gate as the authority path forked on the measurement trust store, the AUTH_PK-rooted enroll/rotate/revoke store as the shared-root test, and `dm_remint` over the anti-amp gate so a foreign token amplifies nothing. No new crypto, no proven-core edit.

### 3.3 Residue

1. **Authority from a truly ungated party is UNBINDABLE.** Layer 2 requires a shared root, and "needs a shared root" IS boundary 4. Today the anchor is EMULATED, so the authority half is a simulation against no real adversary. Only the integrity half is really unilateral. Hardware-blocked.
2. **Freshness is a counter, not time.** A stale-but-valid quote replayed within the counter window is not closed. Needs a real clock. Hardware-blocked.
3. **Equivocation / double-spend.** Re-hash proves each state untampered but cannot order two conflicting states. No local mechanism escapes this without consensus, which CARMIX deliberately lacks.
4. **Confidentiality.** Re-hash gives integrity, not secrecy.
5. **Real transport.** The gate runs over ivshmem shared memory. A real NIC/socket layer is OPEN. The re-hash membrane and gate reuse unchanged, only the transport is missing.

Honest one line: you can unilaterally trust a stranger's BYTES forever, and you can never unilaterally trust a stranger's AUTHORITY, and closing even the conditional authority path down to real security collapses into the hardware-root boundary.

Reuses: `b_sync`, `am_dest`/`am_src`, `am_accepted`, `authz_checks_2to5` + authrec, `antiamp_remint`/`dm_remint` over the swcap gate, the AUTH_PK trust store, nonce-seen/mark + counter, the content-addressed endpoint descriptor, vendored tweetnacl. Named prior art: SPKI/SDSI, CapTP/Cap'n Proto, NDN, SEV-SNP/TDX, Nix rebuild-and-compare.

---

## 4. Boundary 4: Emulated vs hardware root of trust

A BLAKE3 hash, and equally a Coq or zk proof, attests WHAT the bytes are. Neither attests that a genuine, non-simulated environment produced them (genuineness) or that it did so now (freshness). Both are timeless and replayable. Freshness reduces to genuineness. CARMIX today emulates this: the quoting key is the software AUTH keypair and the measurement register is a constant (`AM_MEASURE`). So the distrusting-migration path banks the PROTOCOL, not the security. This is irreducible for the physical half. The honest deliverable is the smallest attested-root interface plus its residue.

### 4.1 Within-strategy: shrink the residue to the theoretical floor

CORE MOVE. The current signed body is meas(32) + authset(132) + state_root(32) = 196 bytes. But authset and state_root are ALREADY content-addressed. So collapse the entire body to ONE 32-byte content hash and prepend the one thing a hash cannot supply, a freshness nonce. The hardware-facing message becomes `(nonce || body_root)` = 40 bytes. All 196 bytes of structure move OUT of the TCB into re-verifiable content-addressed land. `body_root` is naturally the System Root, so residue-shrink and the capstone share the same 32-byte value.

Invariants:
- RT-INV-1 single quoting primitive: the whole hardware-root TCB is `quote(nonce, body_root) -> sig` plus `measure() -> meas`.
- RT-INV-2 content-collapse: nothing in the signed body except measurement and nonce.
- RT-INV-3 freshness external and mandatory: every quote consumes a verifier nonce, missing/reused/expired fails closed. Concrete gap to close today: the distrusting quote OMITS the nonce the authrec carries, so a stale quote currently replays. Unify the quote body with the authrec nonce and expiry.
- RT-INV-4 measurement binds CODE, never DATA: data genuineness re-verifies by re-hash.
- RT-INV-5 emulated/hardware behind ONE symbol: a `root_backend_t` with two impls. The swap changes SECURITY, not PROTOCOL.
- RT-INV-6 fail-closed with distinct reason, add AV_STALE for nonce/expiry.

Layered build: L0 hardware root (future, irreducible TCB) maps 1:1 onto TPM2_Quote qualifying-data, SGX EREPORT REPORT_DATA + MRENCLAVE, SEV-SNP report, TDX TDREPORT, all of which expose a report-data field that fits 40 bytes. L1 protocol (buildable-now, emulated) refactors the quote body. L2 content re-verify (built) re-hashes every byte. L3 authority re-mint (gate-checked, model proven / C tested) bounds via `dm_remint`. Residue after L0 to L3 is exactly {genuineness of the key bound to meas, freshness of the clock}, two facts, about 40 bytes.

### 4.2 Partial escape

The design shrinks the hardware TCB to one signing key bound to one code-measurement plus one monotonic clock. Concretely: the 196-byte body collapses to a single 32-byte content hash, data genuineness leaves the TCB entirely, and emulated-now/hardware-later is a backend swap. The verifier's trusted set stays tiny: one measurement, one quote key, one nonce+expiry.

Honesty note carried at the point of the claim: today `meas` is a constant and the quote key is software, so whole-system ATTESTATION as a SECURITY property is not delivered. Only the WIRE FORMAT and the protocol are real. Real self-measurement (measured-boot binding `meas` to the actual loaded CARMIX image) is needs-research. Until it lands, System-Root attestation demonstrates the protocol against no adversary.

### 4.3 Residue (irreducible, never claim solved)

1. Genuineness is physical, only a hardware anchor supplies it, and even that carries side-channel and TCB-recovery caveats.
2. Freshness reduces to genuineness, so shrinking relocates the residue to the one signing primitive, it does not remove it.
3. The emulated backend is a simulation against no real adversary.
4. The clock is a tick-count proxy, so expiry is a proxy.
5. The baked first-authority key is the one irreducible out-of-band bootstrap trusted base, and the machine owner controlling the raw store with no persistent anti-rollback state is a de-facto trusted base.

### 4.4 Buildable-now / needs-research / open

Buildable-now: the `root_backend_t` indirection wrapping the AUTH keypair, the 40-byte body collapse, folding the nonce+expiry freshness gate into the distrusting path (closes the current stale-quote replay), the residue partition as an enforced invariant, `body_root` = System Root.
Needs-research: real self-measurement via a measured-boot chain, mapping `(nonce || body_root)` onto a specific hardware report-data field and validating the endorsement/quote chain.
Open / hardware-blocked: the genuine hardware root, a trustworthy monotonic clock, persistent anti-rollback state, the TEE's own side-channel exposure.

Reuses: tweetnacl `crypto_sign`/`crypto_sign_open`, AUTH_PK/AUTH_SK, `AM_MEASURE` + `am_accepted`, the distrusting-migration gate with distinct verdicts, `dm_set_pack` + `dm_remint`, authrec nonce+expiry + nonce-seen + tick clock, `cvsasx_blake3`, the cold+warm re-hash sync, kernel/store/epoch_tree.c and the System Root manifest, the two-QEMU ivshmem substrate. Prior art: TPM2_Quote + measured boot / IMA / DRTM, SGX EREPORT + DCAP/EPID, SEV-SNP + VCEK, TDX, Flicker, Haven/SCONE.

---

## 5. Boundary 5: Observational-equivalence undecidability (three notions of "same")

Three sameness relations, and the build's job is to keep them separate:
- **T0 bit-exact:** BLAKE3 root equality, one comparison, ALREADY BUILT and load-bearing (store dedup, the `sls_diff` prune, migration re-hash).
- **T1 structural-canonical:** equality of a canonical form CF(state) quotienting out layout freedoms (child order, address relabeling, unreachable garbage). DECIDABLE but UNBUILT.
- **T2 behavioral:** same behavior. UNDECIDABLE by Rice, permanently. A refusal branch forever.

Membrane placement: T0 and T1 are frozen/nameable, T2 is the live/behavioral half the capability axis cannot reach. Load-bearing code fact: `CVSASX_SLS_MAX_CHILDREN == 16` bounds Merkle OUT-degree only. It does NOT bound fan-IN of a shared subtree nor the semantic pointer valence of an address space, cap graph, or namespace, which is what a real canonical form ranges over.

### 5.1 Within-strategy: the decidable ladder T0 to T1

CF, decomposed into exactly three quotients that differ sharply in cost:
1. **Child-order canon:** where a node's children are a SET, sort by their own hash before hashing the parent. O(n log n), leaf-local.
2. **Label canon (alpha-equivalence):** quotient out consistent renaming of virtual addresses / cap-slot indices / handle numbers. THIS is graph canonization under vertex relabeling, GI-hard in general, polynomial only for bounded valence (Luks 1982). GLOBAL, not leaf-local.
3. **Reachable-restrict:** CF ranges only over the GC-reachable subgraph. Unreachable garbage is quotiented for free.

Invariants:
- INV-1 tier honesty: no API returns a bare bool "same". Every result carries a tag in {BIT_EXACT, STRUCTURAL_CANONICAL, UNKNOWN_BEHAVIORAL}. T2 queries are refused.
- INV-2 CF purity: CF depends only on reachable content and structure. Permute layout, CF-hash must not move.
- INV-3 refinement: BIT_EXACT implies STRUCTURAL_CANONICAL-equal.
- INV-4 no-behavioral: CF-equal does NOT imply behavior-equal, and the system never asserts it.
- INV-5 domain partition: CF-hashes are domain-scoped exactly like dedup.
- INV-6 incremental-only-where-local: child-order canon is leaf-local so incremental equals full holds. Label-canon is global, so if enabled it BREAKS diff-proportional CF for the relabeled scope. Do not advertise diff-proportional canonical roots while relabeling is on.

### 5.2 Partial escape: T1

Build CF and get everything T0 gives (dedup, diff, merge, convergence, whole-system naming) over the coarser but still-decidable structural quotient. It folds states differing only in layout (same allocator run twice, an address space and its relocated copy, a namespace built in a different insertion order) that today get distinct T0 hashes.
Buildable-now slice: quotients 1 and 3 are roughly 90 percent already present (content-keyed store, hash-equality prune, GC reachability). Shipping them as an explicit tier-tagged CF is mechanism engineering, no new theory.
Needs-research slice: quotient 2 (label/address canonization) is the graph-canonicalization frontier, adapt nauty/Traces or a Luks bounded-valence canonizer, but ONLY after the valence measurement below. Prior art: Luks 1982, Babai 2016 (the quasipolynomial fallback), McKay/Piperno nauty/Traces, Hopcroft DFA minimization and Hennessy-Milner bisimulation (canonical forms for a decidable equivalence over finite structures, the shape frozen state fits and behavior does not), Unison (content-addressed code keyed by hash-of-normalized-AST).

### 5.3 Residue

1. **The Rice residue (permanent).** Behaviorally-equal-but-structurally-different pairs are invisible to CF and must be. CF reports them DIFFERENT correctly, and the system never upgrades that to "behaves differently". T2 is a refusal branch forever.
2. **The unmeasured-valence residue (current, decisive).** Even decidability-to-tractability is unestablished for CARMIX. Label canon is polynomial only if the real object graphs are bounded-valence, and the 16-arity out-degree does not supply that bound. THE HIGHEST-VALUE near-term item for this boundary is a MEASUREMENT, not a proof: instrument real remat/cap/namespace graphs and record the valence distribution. Until measured, T1's benefit is unquantified and could be negative.
3. **The incremental residue.** Turning on label canon forfeits diff-proportionality for the relabeled scope. A fully-canonical System Root cannot also be claimed diff-proportional.
4. **The merge residue (cross-dependency on boundary 2).** Structural merge toward CF is well-defined only for join-semilattice state.
5. **The capstone residue.** Today's System Root folds T0 bit-exact hashes, so two structurally-equivalent-but-differently-laid-out systems get DIFFERENT roots. Whole-system dedup and attestation are T0-only until CF is built.

### 5.4 Buildable-now, ordered

STEP 0 (MEASURE, gates everything): instrument the remat graph, the cap graph, and the namespace to emit valence histograms. Reuses the store walk + GC reachability walk. No new subsystem.
STEP 1 (T0 tier-tagging): add the tier tag to the existing `sls_diff` / store-equality returns. Tiny diff.
STEP 2 (CF quotients 1+3): a canonical serializer that restricts to the GC-reachable subgraph and sorts set-children by hash, then store_put. Testable via INV-2 and INV-3.
STEP 3 (canonical diff): re-point the `sls_diff` Merkle walk at CF-hashed nodes.
STEP 4 (domain-scope the CF store).
DEFERRED to research (do NOT build until STEP 0 says polynomial): quotient 2 via a nauty/Traces or Luks canonizer.

Reuses: store put/get + `cvsasx_blake3`, the single-level store + `sls_diff` (arity 16), the GC reachability walk + refcount/tombstone, domain-partitioned dedup, kernel/store/epoch_tree.c, the Merkle namespace, the existing "two notions of same" grading and the DBG refusal branches, the migration whole-state re-hash.

---

## 6. Next-era primitive design sketches

Each is labeled buildable-now / needs-research / open, states what it reuses, how it strengthens the thesis, and which boundary it respects.

### 6.1 Authority-scoped content-defined chunking (CDC)  -  buildable-now (core), open (boundary-doubling)

Idea: replace the fixed 256-byte chunking with content-defined (Rabin/FastCDC) boundaries whose chunk identity, dedup, AND boundary positions are scoped by authority domain. Fuses the domain-tagged refcount/dedup table (keys dedup by (hash, domain), closing the cross-domain store-hit timing channel) with the anti-amp re-mint gate. The result shares chunks WITHIN a domain (diff-proportional, insertion-robust) and NOTHING across domains. The subtle part named by the synthesis: with content-defined boundaries, the boundary set is itself a content side channel, so the domain partition must cover boundary positions, not just chunk identity. A per-domain salt on the rolling hash diverges even the boundary positions at zero dedup cost, because cross-domain dedup is already zero.

Buildable-now: the CDC boundary function, routing chunks through the domain-scoped store path, the domain-scoped manifest, the per-domain salt, the re-minted sub-cap (build a PIR from a manifest entry and re-mint bounded by the holder's authority). Invariants: partition safety (INV-1), boundary scoping (INV-2), within-domain determinism (INV-3), sub-share re-minted (INV-4), GC preserved because chunks are acyclic content objects (INV-5). Checks: front-insert dirties O(1) chunks vs all-downstream under fixed chunking, identical content in two domains gives identical hashes but independent refcount entries, salt-on gives different boundary sets, an over-broad re-mint is refused, dropping a shared version reclaims only unique chunks.

Reuses: the domain-scoped store path + (hash, domain) refcount table, the fixed-chunk diff path (replaced), the anti-amp gate + PIR, durable refcount GC, `cvsasx_blake3` + store put/get. Prior art: LBFS Rabin CDC, FastCDC, restic/borg, IPFS UnixFS, prolly trees (Noms/Dolt) and Merkle Search Trees for the tree extension, the dedup side-channel literature (Harnik et al.).

Strengthens the thesis: lives entirely on the frozen/deterministic side, and it EXTENDS anti-amplification from whole-object caps to sub-object structure, so sharing a chunk is gated by the same (hash, domain) key and re-mint clamp. Honesty on the dedup channel: this closes the CROSS-domain boundary-position instance, the WITHIN-domain store-hit timing channel is unchanged (the accepted residual, the observer is already in the domain). "Measured" applies to the QEMU-virtual DS numbers.

Respects boundary 2 (mutable/dedup side) and the anti-amp membrane. OPEN: making a single CDC boundary DOUBLE as a capability re-mint bound. Dedup-boundaries and authority-boundaries are two independent partitions over the same byte stream, and there is no reason they coincide. Keep them independent, a re-mintable sub-cap exists only where a domain explicitly opts to align a chunk boundary with an authority edge. The balanced content-defined TREE (prolly / MST) is needs-research because fanout tuning is unvalidated and object arity is unmeasured (see boundary 5). Concurrent chunkers racing on the refcount table are SMP-blocked, same status as the DS/GC builds.

### 6.2 Proof-Carrying Rematerialization (PCR)  -  needs-research (the load-bearing succinct half)

Idea: a rematerialization edge H_pre --P--> H_post ships one content-addressed witness W a distrusting destination checks with zero trust. W fuses identity (BLAKE3(B) == H_post, the demand-pager check, free), an authority-preserving edge (dest_cap re-minted <= source, the ceiling authenticated by a signed authrec, never a shipped token), and transition correctness in one of two modes. W is itself a store object named by its own hash, so witnesses dedup and chain into a transition log.

Buildable-now slice honestly labeled: S1 witness-self (recompute the witness hash), S2 identity (the demand-pager re-hash, free), S3 ceiling-auth (verify the signed authrec, nonce unseen, not expired), S4 authority-edge (re-run the anti-amp gate and require equality). S4 is gate-checked in the sense that the ABSTRACT `re_mint` behavior is model proven while the running C is tested. Then S5(a) re-execute-and-compare: rematerialize H_pre, run P to its next DRCC release-cut, hash, require == H_post. This slice is BUILDABLE-SHAPED BUT BLOCKED, not buildable-now: S5(a) cannot be VALIDATED until the DRCC release-cut mechanism ships and is validated. Label it "buildable-shaped, gated on DRCC" rather than "buildable-now", to stay consistent with the roadmap.
S5(b) SUCCINCT (needs-research): an IVC/folding proof checked in polylog. S6 purity: if H_prog holds clock/RNG/net/device caps, REFUSE the transition claim (W_ERR_IMPURE).

Dual-commitment bridge (needs-research feasibility, honest about its own bottleneck): BLAKE3 is arithmetization-hostile, so per state object mint a SNARK-friendly companion `C_snark = Poseidon(O)` and one bridge proof that both commit the same O. Transition proofs run over the cheap companion. The bridge AMORTIZES the one-time BLAKE3-in-circuit equality across every transition touching O. It does NOT eliminate it, and per-step prover cost stays external and untouched. This matters for honesty: content-addressing does NOTHING for the actual bottleneck of the one genuinely new capability PCR claims. The headline strength (naming the transition) does not touch its hardest cost.

Reuses: the anti-amp gate + the Coq monotonicity lemma (authority edge, model proven), the authrec verify path + tweetnacl (ceiling auth), `cvsasx_blake3` + store + the demand-pager re-hash (identity, free), the page-table-inclusive task-hash + page-tree Merkle build (H_pre/H_post), the DRCC release-cut (the well-posed sync cut for S5(a), today theory), domain-partitioned dedup (witness reuse), the acyclic store + transition log, Ed25519.

Strengthens the thesis: it is the thesis applied to the TRANSITION itself, making a remat step a first-class content-addressed object with a re-minted authority edge. It closes the transition-correctness residue for the DRF spine only, at re-execution cost, and only once DRCC lands.

Respects boundaries 1 and 4: PCR retires exactly transition-correctness and NOTHING more. It cannot retire freshness/genuineness (a witness is timeless and replayable, INV-6 pushes "happened now" onto a nonce + trustworthy clock + hardware root, all open/emulated). Its authority half is model-proven / C-tested. Correctness is DRF-only and gated on DRCC. The succinct mode is needs-research and gated on external prover cost. It says nothing about ordering/double-spend or binding an ungated peer.

### 6.3 Lineage Log  -  buildable-now for the CT-shaped part, gated for its one novel tier

Idea: a content-addressed transition LEAF naming (H_pre, P, H_post) plus canonical authority-set hashes (A_pre, A_post) and the derivation record the authority came from, appended into the incremental Merkle tree, with the root Ed25519-signed as a Signed Tree Head chained to its predecessor. Certificate-Transparency-shaped, over live execution. It fuses the state-DAG, the signed authority set, and derivation-chain revocation into one audit surface.

Honesty on what is new versus ported: the buildable-now artifact (integrity re-hash, subset check, STH signing, hash-chained heads) IS Certificate Transparency with content-addressed leaves. The doc's own admission stands: split-view detection "transfers UNCHANGED from CT". The genuinely CARMIX-native claim is T3, that leaves are re-derivable rather than merely trusted, and T3 is defined ONLY for kind=DET and is GATED on DRCC. So the correct label is: the CT-shaped log is buildable-now, its one distinguishing property is blocked on DRCC. Do not call the whole thing "operational".

Leaf schema, append via `commit_transition` (store_put then feed the leaf-hash as an arity-16 balanced-Merkle leaf, diff-proportional), STH = {epoch, log_root, prev_sth, revoke_root} signed with the domain AUTH key. Invariants: append-only (I1), authority-monotone edge (I2, A_post subset A_pre, refused at commit), referent-closed (I3), STH-chained (I4). Auditor tiers: T1 integrity (free), T2 authority (cheap, subset check + revocation non-membership), T3 transition correctness (DRF-spine only, gated on DRCC, defined only for kind=DET). Transitive revocation over lineage reuses the derivation chain-walk + a content-addressed accumulator, forward-reachability over the acyclic state-DAG flags downstream states, which fail on their NEXT rematerialization.

Reuses: kernel/store/epoch_tree.c, `dm_set_pack` canonical serialization, tweetnacl + AUTH keypair, the anti-amp gate + subset check, the derivation chain-walk + accumulator, the whole-state hash, the remat path (T3 auditor and revocation enforcement point), GC durable roots + ROOT-block persistence.

Strengthens the thesis: it makes the frozen half auditable end to end, turning three isolated mechanisms into one verifiable surface where authority preservation is checkable per historical edge, and it keeps state-identity, authority-provenance, and transition-correctness as three separately-attested tiers, so it shows exactly where content-addressing stops.

Respects boundaries 1, 3, 4. Residue: external effects are recorded but not reversible (output-commit) and not explained if uncaptured, non-determinism is recorded honestly but not re-derivable, the correctness residue is retired for the DRF spine only, freshness and split-view are NOT closed (the STH is signed by the emulated root, and split-view needs gossip/consistency exchange inherited unchanged from CT), and revocation is EVENTUAL, not prompt. On that last point be sharper than "eventual": combined with the coarse-boundary rule forbidding a re-hash per switch, a long-resident holder has NO UPPER BOUND on revocation latency, materially weaker than CHERI Cornucopia sweeps. Retention is a growing storage floor, and the retention policy is OPEN.

### 6.4 Content-Addressed Delegation Records  -  buildable-now

Idea: a delegation record is a content-addressed object holding {parent_record_hash, attenuated ceiling as a PIR, delegate identity, delegator Ed25519 signature}. Records form a Merkle chain. The CARMIX move: the wire carries the RECORD (a bound), never a usable token, so this is the SPKI/SDSI model (offline authorization certificates rechecked locally), NOT the Macaroon bearer-token model. A receiver re-mints a fresh local cap bounded by the leaf ceiling. Verification of the whole chain is UNILATERAL for integrity (re-hash every record, check parent links) and monotonicity (re-run the anti-amp gate once per hop). Only two facts need the shared/attested root: provenance of the chain root and genuineness/freshness of a delegate (emulated today).

Buildable-now: the verifiable chain, cross-machine bounded re-mint, and eventual revocation, generalizing the flat distrusting-migration set to an attenuated chain. Verify order (all fail-closed, zero caps on any failure): integrity, provenance (root key in trust store or measurement accepted), per-hop signature, per-hop monotonicity, materialize by local re-mint. Invariants INV-1 to INV-5 as listed. Revocation via a per-domain accumulator whose root rides the signed epoch record, re-checked at each re-mint, EVENTUAL not prompt.

Reuses: the anti-amp gate + PIR + strip, the delegation chain + chain-walk revocation, the distrusting-migration flow + accepted-measurement check, `dm_set_pack`, tweetnacl + trust store, store + `cvsasx_blake3`, diff-proportional Merkle sync, the epoch accumulator.

Strengthens the thesis: it shows the three axes compose into one third-party-auditable artifact, and it demonstrates the capability axis reaching exactly one step past content-addressing (gating and bounding a cross-boundary channel) while honestly failing to order, name time, or bind an ungated party.

Respects boundaries 1, 3, 4, and the double-spend negative. Residue: provenance needs a trusted first key (assumed), delegate genuineness/freshness needs a hardware root (emulated today, so the delegate-binding security is a simulation), a non-CARMIX delegate cannot be bound (the record is a verifiable OFFER), and prompt/single-use/atomic-remote revocation is IRREDUCIBLE (revocation is eventual, latency bounded below by re-check cadence). Standing: the per-hop gate call is backed by the model-proven monotonicity lemma, but the CHAIN composition and the whole cross-machine flow are C + Ed25519 exercised by attack tables, not machine-checked. The defensible novelty is the content-address binding of the ceiling plus the gate-per-hop re-check of a foreign chain, NOT delegation-across-a-boundary as such (SPKI/SDSI, Macaroons, X.509 proxy certs, KeyKOS/EROS are the prior art).

---

## 7. Cross-boundary composition algebra (meet/join over content-addressed names)  -  needs-research

This is the algebra that lets one live process be assembled from sub-graphs authored and signed by mutually-distrusting domains. It is presented as prior art plus one increment, not as a new algebra invented here.

Carrier: an authority set is a canonically-serialized set of content-addressed caps ({object hash, rights, length}, sorted by hash). Order: A <= B iff every cap in A has a cap in B over the same hash with rights_A subset rights_B and length_A <= length_B, which is the pointwise lift of the order the swcap gate already enforces per object. Bottom is the empty set. There is NO top (no ambient root).

Two operators:
- **MEET (A wedge B):** for each hash in BOTH verified sets, emit rights = rights_A AND rights_B, length = min. Absent hash drops out. Honest framing: MEET is capability intersection, and SPKI/SDSI 5-tuple authorization reduction is the CLOSEST prior art, it also computes effective authority locally by intersecting certificate tuples. The CARMIX increment is that the shared element is a content-addressed object HASH (re-verifiable by re-hash, not just name-matched) and the intersection IS the anti-amp `re_mint`. This is one increment on SPKI, not a new algebra.
- **JOIN (A vee B):** canonical-merge the caps, admit cap c from source i ONLY if source i's Ed25519 quote over c verified. JOIN adapts CapTP three-party handoff and DStar/HiStar distributed DIFC.

Load-bearing asymmetry: MEET is monotone-DOWN, so a fully-malicious source can only SHRINK the meet, and integrity of the meet is unilateral like state re-hash. JOIN is monotone-UP, so it is only as strong as the WEAKEST attested contributor. Composite authority = union of attested per-source ceilings (no single source amplified). Composite ASSURANCE = min over contributing roots. Composition is closed: the composite state root is a BLAKE3 DAG whose children are the source roots, and canonical serialization makes wedge and vee commutative and associative. Honesty note: those lattice laws are argued from canonical serialization and are C + tested, not machine-checked, and the MEET gate itself is model-proven / C-tested.

Ingest per source: verify the quote, on fail set the source to bottom. Independently re-hash the sub-graph, a byte tamper drops that source, the composite still forms from the rest. Invariants I1 to I5 (no-amplification, meet monotone-down under malice, join gated by attestation, canonical determinism/closure, weakest-root ceiling recorded and non-forgeable), each with a runnable check mirroring an existing attack table.

Reuses: `dm_cap_t` + `dm_set_pack`, the anti-amp `re_mint`/`dm_remint` (MEET), the attestation quote + tweetnacl, the trust-store measurement check + forged-quote reject, cold/warm re-verification, the BLAKE3 store for the composite DAG, the two-QEMU ivshmem harness.

Strengthens the thesis: it shows the anti-amp gate is the MEET operator of a lattice with no top, and the attested authority set is the JOIN's admission ticket, so the same `re_mint` that bounds one hop composes into an algebra that assembles many distrusting parties without any of them gaining authority. Integrity of every sub-graph is unilateral, the meet is safe even against a malicious source, and only the join requires attestation and is honestly capped by the weakest root.

Respects the double-spend and liveness boundaries. It does NOT cross them. Residue: fork DETECTION is free (two conflicting mutations of one logical slot present two hashes, and meet/join yields the set, never a winner), fork RESOLUTION is out of scope and needs an external total order CARMIX lacks. Composition is defined only at a quiescent cut, and detecting all-sources-quiesced is the same liveness gap as CRDT quiescence. A non-CARMIX source is downgraded to a data source. JOIN admission is only as strong as the emulated root, so composite assurance is a real protocol with simulated security until a hardware root exists. There is no confidentiality (composing means the composer sees every source's bytes).

---

## 8. The mature architecture

A mature CARMIX is one law applied uniformly, realized as four planes plus a capstone, with every construct pre-classified before it is built.

**The law (the OS's conscience, run at design time).** Every construct passes the membrane sort G0 to G4 BEFORE code exists, and its class fixes what it may claim. This is what makes CARMIX coherent rather than a feature list: the same sort that admits the content store REFUSES hard real-time, raw multi-writer coherence, and binding an ungated peer, and it says so in the same breath. To keep this from being an unfalsifiable narrative device, the sort is stated in section 0.1 as concrete yes/no gates with worked examples, so a reader can independently classify a construct and check the result against its residue rather than take the class on assertion.

**Plane 1, the frozen spine.** BLAKE3 content store + diff-proportional epoch tree + domain-partitioned dedup + durable refcount/tombstone/resurrection GC. Invariant: NOTHING IS HASHED MID-MUTATION. Here identity, integrity, dedup, structural diff are cheap to compute. Time-travel is NOT free: it is cheap to NAME a past state and expensive-forever to KEEP it namable, because a pinned root is un-reclaimable while retained and the store grows monotonically. GC is acyclic-by-construction (content-addressing removes cycle collection) but NOT "complete": it still owes shared-block reachability bookkeeping and is structurally forbidden from reclaiming the growing pinned set. Domain-partitioned dedup closes the CROSS-domain instance of the Harnik timing channel (measured on QEMU-virtual numbers) and accepts the within-domain residual.

**Plane 2, the authority membrane.** The single anti_amplification relation (dest <= source) is reused at spawn, IPC, delegation, cross-core, the migration wire, and the emulated-VT-d IOMMU DMA window. Maturity across that list is NOT uniform: spawn and IPC are built crossings, while the IOMMU/DMA window is an EMULATED PHYSICAL-class crossing (attestation is emulated until a hardware root), so "the same relation is reused" is a design statement, not a claim of equal maturity. Two invariants define the membrane: NO BEARER TOKEN CROSSES (a signed ceiling crosses, the destination re-mints from re-verified content, failing CVSASX_ERR_REFERENT_MISMATCH) and AUTHORITY IS FOLDED INTO THE HASHED BYTES. The membrane gates and bounds reachability, it structurally CANNOT impose order, name time, or bind an ungated party. Content-verify and authority-re-mint are always two separate checks.

**Plane 3, the activation plane.** Rematerialization is the single mechanism at COARSE boundaries only (page fault, service restart, hibernate, migration, undo, app closure). It is NOT the mechanism for ordinary preemptive context switch: per-quantum preempt, cursor, keystroke, and per-frame paths stay on the direct blit/register-swap fast path because of the measured U3 crossover. So the honest claim is "one activation mechanism at coarse boundaries", not "one mechanism behind context-switch". Trust-by-re-derivation replaces trust-by-token (Nix rebuild-and-compare lifted to live state), sound only on the DRF spine and gated on DRCC.

**Plane 4, the boundary layer.** Time: attested-tick records yield content-addressable, layout-signable, embeddable instants and a-posteriori deadline VERDICTS against the calibrated LAPIC timer, never liveness prediction or fine-grained preemption, and on the emulated root the signature is protocol-only. External party: the Foreign Ingress Gate gives full unilateral integrity to ANY peer and bounded authority ONLY where a shared/attested root exists. Root of trust: everything hardware-facing collapses behind one `root_backend_t` symbol whose quote signs `(nonce || 40-byte body_root)`, and the same `body_root` IS the System Root, so emulated-now/hardware-later is a backend swap, with the standing caveat that the emulated backend's security is a simulation until real self-measurement and a hardware root land. Sameness: T0 bit-exact (built), T1 structural-canonical (decidable, mostly unbuilt, gated on a valence measurement), T2 behavioral (a Rice refusal branch forever). Order: only from an explicit signed append-only structure, and total order under concurrency stays out of scope. Merge: only over join-semilattices, where SEC is READ OFF as hash equality, with the honest rider that convergence CANNOT be DETECTED (quiescence is a liveness property CARMIX lacks) and metadata GC is blocked on it.

**Capstone, the System Root.** One 32-byte hash naming the SOFTWARE object graph at a quiescent epoch boundary (each task's page-table-inclusive hash + MVCC slot versions + store manifest + FS root, folded diff-proportionally). It yields whole-system time-travel/branch and whole-system attestation whose novel bit is per-component capability-ceiling binding inside the hashed manifest. Scope honesty carried at the claim: today it folds T0 bit-exact hashes (so structurally-equivalent systems get different roots), `meas` is a constant on the emulated root (so attestation is protocol-only until self-measurement), the single-CPU slice names a single-CPU system, and the whole-live-system-across-cores framing is not delivered until the SMP consistent cut lands, because that cut's soundness IS the DRCC line. It names software at an epoch boundary only, not devices/DMA/firmware, not the instants between epochs, and it obeys output-commit.

One sentence: make the name carry both content and authority, activate by re-derivation at coarse boundaries, and route every live-side need through a labeled boundary escape whose residue is stated, not hidden.

---

## 9. The tiered roadmap

Framing honesty stated once and applied to every "gated on DRCC" item below: DRCC ("a data-race-free program reaches a reproducible content hash at a synchronization-point cut") cannot be validated in the strong sense, because the OS cannot DECIDE whether a workload is actually data-race-free, that is the same undecidability wall respected elsewhere. So DRCC validation means "reproducibility on workloads we ASSERT are race-free", and every downstream claim is conditional on a premise the OS cannot enforce. This single dependency, not any individual primitive, is the largest honesty risk in the whole design.

### Tier 1, buildable-now (led by the keystone)

1. **KEYSTONE, multi-core DRCC release-cut capture.** A hash-barrier with no global stop. Each core, at its next release-consistency sync point, publishes its local task-hash into a per-core MVCC slot over the built LAPIC-IPI path. The cut is the tuple of per-core hashes plus the shared-heap root, validated by rematerialize-and-compare across N runs on workloads ASSERTED data-race-free. Moves DRCC from paper-argued to validated-for-the-asserted-DRF-spine.
2. **Lineage / Computation Transparency Log.** The CT-shaped log (integrity, subset check, signed hash-chained heads) is buildable-now, its one novel tier (T3 re-derivable leaves) is gated on DRCC. Split-view/gossip/freshness inherited from CT, unchanged.
3. **Revocation by epoch accumulator.** A content-addressed Merkle accumulator whose root rides the signed record, stale caps fail on next rematerialization. Honest: EVENTUAL, and for a long-resident holder there is NO UPPER BOUND on revocation latency, weaker than CHERI Cornucopia sweeps.
4. **Authority-scoped content-defined chunking.** Chunking + per-domain salt buildable-now, making a CDC boundary double as a re-mint bound stays OPEN.
5. **Domain-scoped deadlist GC at scale** (adapt ZFS async_destroy), carrying shared-block reachability explicitly.
6. **Merkle property witnesses** (inclusion/exclusion). Membership/non-membership buildable-now, succinct whole-subtree-clean is research.
7. **Verifiable rematerialization, re-execute-and-compare slice.** Buildable-SHAPED but gated on the keystone landing before it can be validated.
8. **Attested-tick annotation + a-posteriori deadline verdicts** via the existing break-even gate. Emits verdicts, never guarantees, signature protocol-only on the emulated root.
9. **Foreign Ingress Gate.** Unilateral integrity for any peer buildable-now over ivshmem, bounded authority only on a shared root, real NIC transport OPEN.
10. **Content-addressed delegation records** (SPKI/SDSI-shaped, not bearer tokens), third-party verifiable by re-hash + gate-per-hop.
11. **`root_backend_t` indirection + 40-byte quote collapse**, folding the nonce+expiry freshness gate into the distrusting path (closes the current stale-quote replay), with a distinct AV_STALE verdict. Shrinks the TCB to its floor, security still emulated.
12. **Content-addressed Powerbox substrate** (a designating gesture returns an immutable snapshot hash + a freshly minted attenuated capability). Substrate buildable-now, gesture/frame are external edges.
13. **System Root object.** Single-CPU slice buildable-now, the SMP whole-system version unblocked by the keystone.

### Tier 2, needs-research

- **Proof-Carrying Rematerialization + dual-commitment bridge.** Identity + authenticated anti-amp ceiling near-term, the succinct half needs-research (BLAKE3 arithmetization-hostility, per-step prover cost content-addressing does not touch).
- **Brand-by-hash sealing / content-addressed otypes.** Needs a symmetric-crypto TCB the design deliberately lacks.
- **Label-in-the-hash monotone flow.** Ceiling is a monotone-label lemma, NOT the star-property, NOT seL4 noninterference.
- **Capability-scoped delta-state CRDTs.** The CALM-monotone half. Quiescence detection is a liveness property CARMIX lacks, raw multi-writer coherence and double-spend stay irreducible, and the additions (LUB in the store, SEC as hash equality) are restatements of content-addressing, not new results.
- **Meet/join authority algebra + multi-source distrusting composition.** One increment on SPKI/SDSI, fork detection free, fork resolution needs consensus CARMIX deliberately lacks.
- **Structural canonical form / T1.** MEASURE FIRST (valence distributions), which gates whether label-canonization is Luks-polynomial, Babai-quasipolynomial, or GI-hard for CARMIX.
- **seL4-style refinement of swcap.c, or a source-to-binary certificate.** Retires the model-proven / C-tested gap, today all cross-machine/composition claims are tested against attack tables.
- **Hardware root of trust + trustworthy monotonic clock.** Retires the emulated-root and freshness residues. The 40-byte body already fits TPM2_Quote/SGX/SEV-SNP/TDX report-data, needs measured-boot self-measurement. Hardware-blocked.
- **Content-addressed ML lineage + attestable inference.** Content-addressed provenance/authority/revocation, NOT bit-exact reproduction (GPU float is off the DRF spine).
- **Real transport (NIC/socket).** The re-hash membrane and gate reuse unchanged, only the transport is missing.
- **Mutable data-plane separation** (a rollback-able code/authority root vs a forward-only data store). OPEN.
- **Global retention policy across every pinned root.** Every-edge vs sync-cut-only vs sliding-window is OPEN, and this is what makes "time-travel is free" false.

### Permanently irreducible (never claim solved, ship boundary + partial escape)

Liveness/deadline prediction (Rice). Hard real-time via remat (the naming edge is permanent, the cost-edge ceiling is strongly indicated but rests on virtual measurements pending hardware). Raw multi-writer coherence. Binding an ungated peer's conduct. Behavioral equivalence (a T2 refusal branch forever). Double-spend / total order. Genuineness + freshness on the emulated root.

---

## 10. The single recommended first build

**BUILD THE MULTI-CORE DRCC RELEASE-CUT CAPTURE FIRST.**

WHAT: a hash-barrier with no global stop. On a capture request, each core, at its NEXT release-consistency synchronization point (not on a stop-the-world interrupt), publishes BLAKE3 of its local page-table-inclusive task-hash into a per-core slot. The consistent cut is the tuple of per-core hashes plus the shared-heap root, committed via the single-word MVCC version bump so a concurrent reader sees pre- or post-cut, never torn. Add the attested-tick companion: a per-core tick captured on the same IPI, reported as a `[min_tsc, max_tsc]` skew interval. Validate by rematerialize-and-compare across N runs on a workload ASSERTED data-race-free: same delivered execution gives the same cut hash.

REUSE (no new theory, no proven-core edit): the built SMP LAPIC-IPI path + per-CPU state, the MVCC versioned gate, the per-task page-table-inclusive hash + page-tree Merkle build, the epoch tree (incremental equals full), the rematerialization/demand-paging re-hash path for the compare, `rdtsc` + the calibrated LAPIC counter for the tick cut.

WHY THIS ONE: dependency leverage, not novelty. DRCC is today PAPER-ARGUED AND UNVALIDATED ON THE BUILT SMP, yet roughly a third of the forward roadmap is gated on it: verifiable rematerialization, Proof-Carrying Rematerialization, the transition-correctness tier of the Lineage Log, delta-CRDT strong-eventual-consistency, memoization soundness, and the SMP consistent cut the System Root capstone needs. B3 is the one buildable-now mechanism that converts that premise from theory to validated-for-the-asserted-DRF-spine, the highest ratio of unblocked roadmap to lines of code, entirely on green primitives.

HONEST RESIDUE, stated not buried: B3 validates the DRF spine ONLY, and even that validation is conditional, because the OS cannot DECIDE that a workload is race-free, so B3 validates reproducibility on workloads we ASSERT are DRF, not reproducibility as a checked property. Every downstream "gated on DRCC" claim inherits this unverifiable input assumption. Racy/non-deterministic state stays FROZEN-RETAINED-ONLY (nameable and rematerializable-if-retained, never reproducible-by-rerun). The cross-core "now" is a skew-bounded interval, not a point. B3 touches neither the model-proven / C-tested boundary nor the emulated-root/freshness boundary. Until B3 exhibits reproducible cuts on asserted-DRF workloads, every downstream reproduction and transition-correctness claim remains labeled "gated on DRCC".
