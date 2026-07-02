# CONVERGENT COMPUTATION RESEARCH PACKET

Read-only verdict exercise over the committed CARMIX spine. Three fronts, each with an
RC0 nearest-neighbor table, findings, and one of four verdicts (BUILDABLE-NOW,
BUILDABLE-GATED, RESEARCH-ONLY, KILL). Then RC4 the unified claim, one honest delta per
nearest neighbor, and the kill-list. The honesty critic has been folded in: four flagged
items are fixed or downgraded in place, marked CRITIC-FIX where they change a claim.

Grounding note. All grounding was read-only against committed files. No code was run. The
one load-bearing fix (RC3-b) was re-read against proofs/Carmix.v:105-133 and is corrected
below. Every bibliography entry is recall, not fetch, and is flagged UNVERIFIED per row. No
fetch tool exists in this environment.

Notation. Authority order is cap_le (proofs/Carmix.v:105-108). MEET is the greatest lower
bound in that order. The anti-amplification discipline (D4) takes the MEET for any
identity, merge, or reuse rule, never the join.

============================================================================
RC1  CONVERGENT COMPUTATION (durable authority-keyed memoization)
============================================================================

Claim under test. Durable memoization keyed by (canonical code hash Hc, canonical input
hash Hi, authority ceiling C, domain D), servable from the content store instead of
re-execution, under the anti-amplification lattice. Nothing memo-shaped exists today (every
`memo` token in kernel.c is `memory`). The roadmap placeholder is Cycle 4 (CARSH, the
authority-routed memoizing shell). Primitives it reuses are all committed: store put/get
plus BLAKE3 dedup, domain-partitioned dedup (kernel/DEDUP_SCOPING_LOG.md DS1-DS6), the SFI
checker (gate/sfi_checker.c I1-I5), the swcap gate plus proofs/Carmix.v C1
anti_amplification, cv_canon (kernel/CV_LOG.md CV1-CV8), the epoch tree, persistence,
refcount GC, and the migration re-hash / re-mint path.

RC0 nearest-neighbor table (each row UNVERIFIED, recall not fetch)

| Work | What it does | Delta vs this front |
|------|--------------|---------------------|
| Nix / Bazel / Buck2 / Shake / redo, input-addressed build memoization [UNVERIFIED, recall; core BORROWED] | Memoize impure ambient-authority processes, input-completeness approximated by sandbox convention, so a loose sandbox silently poisons the cache. | CARMIX's granted capability set IS the declared input set, so effect-closure is a kernel invariant established statically by the SFI checker (I1-I5), not a sandbox promise, and the key carries the authority ceiling plus domain (CV2), which build systems never model. |
| Unison, content-addressed code [UNVERIFIED, recall; core BORROWED] | Content-addresses pure code definitions and hashes references in a pure language. | Unison gets effect-freedom from the language by construction, CARMIX admits effectful native/wasm code via capability effect-closure plus a static load-time checker, and binds authority into the memoized identity. |
| Self-adjusting computation / Adapton (Acar et al.) [UNVERIFIED, recall; mechanism BORROWED] | Memoize plus change-propagate over a dynamic dependence graph for incremental recompute. | Orthogonal aim (incrementality). New here is authority-lattice keying, anti-amplification of the served identity (meet not join), and the cross-domain timing analysis those works do not address. |
| Deterministic-parallelism runtimes (Determinator, dOS, DPJ) [UNVERIFIED, recall; substrate BORROWED] | Enforce execution determinism so parallel results are reproducible. | CARMIX uses DRCC (DRF implies one canonical hash) as the admission substrate but adds a durable, content-addressed, authority-keyed memo store and the domain-partition timing remedy. |
| Harnik / Armknecht cross-user dedup side-channel [UNVERIFIED, recall; remedy BORROWED] | Shows client-side dedup leaks whether content exists somewhere, and that zero cross-user leakage with cross-user sharing is impossible. | CARMIX generalizes the leak from "content exists" to "computation was performed" and applies the same committed domain-partition remedy (DS1-DS6). Borrowed remedy, new target. |
| FaaS result caching / memcached / RPC response cache [UNVERIFIED, recall; core BORROWED] | Cache outputs keyed by request for fast reuse across callers or tenants. | No authority model, no anti-amplification, no effect-closure guarantee, and directly vulnerable to the cross-tenant hit/miss timing channel CARMIX closes by putting domain in the key. |
| Certified compilation / translation validation / PCC (Necula, CompCert) [UNVERIFIED, recall; orthogonal] | Prove a produced output correctly corresponds to its input. | Orthogonal. PCC proves the transform correct, CARMIX memo proves only serve==recompute for an admitted (asserted-pure) region and that serve cannot amplify authority. A PCC certificate could later discharge admission-soundness, that composition is unbuilt. |
| TxOS system transactions / speculative-execution OS caches [UNVERIFIED, recall; orthogonal] | Provide atomicity, isolation, rollback of OS effects, or speculatively cache syscall effects. | Those give effect isolation, not content-addressed pure-region reuse. CARMIX memo is about not re-executing an admitted pure region, keyed by authority, with the meet discipline and durable content-store backing. |

Novelty outcome: NOVEL-AS-FUSION. Exact delta sentence. The only new content is placing the
authority ceiling and the dedup domain INTO the memo key so the served identity takes the
MEET (the CV2 refusal applied to reuse), plus capability-effect-closure admission that
reuses the existing SFI checker's static guarantee as the admission oracle. Everything else
(the memoization mechanism, DRF-determinacy, the dedup timing channel and its
domain-partition remedy) is borrowed.

RC1-a ADMISSION (the exact predicate, and what the gate can and cannot check).
A region R is ADMISSIBLE iff its output is a pure function of (Hc, Hi) under a fixed
serialization. This is sufficient, never necessary, because purity is undecidable (CV6 /
Rice). Two sufficient conditions. (i) EFFECT-CLOSURE by capability. R's granted cap set
holds only frozen-side caps (content-store read of declared inputs, its own private
linmem), and NO clock cap, no MMIO/device cap, no IPC-receive from a mutable peer, no RNG,
no shared-writable window aliased by a live writer. This is the PROOF_CORE A4 /
FROZEN-REPRODUCIBLE vs FROZEN-RETAINED-ONLY split. (ii) Either R writes no external effect,
or every effect input is captured into Hi (record-replay).

What the SFI/gate path CAN check today, statically, at load time, in linear cost (I1-I5):
all memory access goes through the single linmem cap slot (I1), no cap forge (I2), control
flow in range (I3), no otype/unknown ops (I4 default-reject), bounded stack (I5). The
load-bearing consequence, confirmed by reading gate/sfi_checker.c, is that the permitted
GIR opcode set contains NO host-call, import, syscall, clock, or random opcode, and the
switch default rejects. So a module that passes gate_check has no effect surface except
LOAD/STORE on that one linmem slot plus deterministic arithmetic on locals and args, and
CALL_INDIRECT dispatches only to range-checked in-code targets. The checker therefore
ESTABLISHES effect-closure to {linmem, locals, args} for free, which is the hard half of
admission. For the gate/wasm execution model the admission oracle already exists in-tree.

CRITIC-FIX (backstop dependency, surfaced). The sfi_checker.c header states its guarantee
holds only relative to the modeled opcode set, and that the independent CHERI runtime
backstop is MANDATORY (CVE-2021-32629 lineage). Effect-closure therefore inherits both the
model-incompleteness of the opcode set AND the assumption that the runtime backstop is
present in the deployed configuration. Adding any future opcode reopens admission and must
re-trigger this analysis.

What the gate path CANNOT check: (1) that the linmem backing object is a private input
snapshot and not a shared-mutable window another party writes (Boundary 2, mutable shared
state, irreducible for the raw case, and CV8 racy divergence is the live hazard), so the
memo layer must pin linmem-in as content and put linmem-out in the value, (2) it does not
itself capture linmem into the key (memo-layer job), (3) it cannot decide semantic
determinism, observational equivalence, or termination (CV6, permanently out of scope), so
admission is only ever a SUFFICIENT structural condition. The NATIVE INT-0x80 task model has
no equivalent static bound (syscalls reach TSC via SYS_GETTIME, devices, IPC). There
admission reduces to the capability-set flag (task holds only frozen caps), which is
checkable, but the underlying DRF / purity premise is ASSERTED not decided (A2,
undecidable).

Verdict RC1-a: BUILDABLE-NOW for the gate model (checker plus "capture linmem-in into key,
linmem-out into value"). BUILDABLE-GATED on the frozen-cap-set flag for native tasks, DRF
asserted.

RC1-b THE REUSE RULE (the rule that provably cannot amplify).
Two distinct amplification vectors, the rule kills both.
(1) Authority-carrying. Does the served result grant authority the caller lacked? No. The
memo value Hv is a content object, serving is a store-get returning BYTES, re-hashed before
use (the membrane "re-hash before use" invariant). It carries zero capability. Access to
those bytes is gated by the caller re-minting read(Hi) plus exec(Hc) from its OWN grants
through the committed gate (dest authority <= source, proofs/Carmix.v C1). The producer's
ceiling is IRRELEVANT to authorization and must not be trusted. So serve authorization is
NOT dominance and NOT exact-ceiling matching, it is a per-caller re-mint from local grants.
Dominance-based serving is rejected for the same reason SR4 rejected immutable-authority
restore and CV2 refused authority-blind convergence.
(2) Convergence / result-confusion. Could a low-authority caller receive a result that
depends on higher authority (the CV2 refusal)? Prevented by putting (C, D) in the KEY so
entries never cross partitions. An entry produced at (C_prod, D_prod) is a distinct memo
entry from (C_call, D_call), and a caller only ever hits an entry keyed to its own (C, D).
This is exact-partition keying, the identical discipline to dedup domain-partitioning.
Net rule: key = (Hc, Hi, C, D) with C the caller's re-derived ceiling and D its domain,
serve = store-get of Hv gated by the caller's own re-mint of read(Hi) plus exec(Hc), value
carries bytes only. Served identity's authority = caller's own authority MEET producer's =
caller's own (same-partition hits only), never the join. Anti-amplification applied to
identity itself (D4).

RC1-c THE TIMING CHANNEL (Armknecht/Harnik generalized, dedup-verdict honesty).
A shared memo cache leaks that SOMEONE already ran (Hc, Hi). A fast serve is a hit
(computed-before), a slow path is a miss (never-run). This generalizes the cross-user dedup
channel from "content exists" to "computation was performed". Mitigations and cost:
- Domain scoping (put D in the key, look up only within the caller's domain). A cross-domain
  probe becomes a MISS at recompute cost, indistinguishable from never-run. Closes the
  cross-domain channel, exactly as DEDUP_SCOPING measured for content (global-dedup fast hit
  42873 cyc vs domain-scoped miss ~1285349 cyc == genuinely-new, kernel/DEDUP_SCOPING_LOG.md).
  Cost is N-fold duplicate computation and storage for cross-domain-shared results (N
  physical objects for N domains). Within-domain hit timing remains, an accepted residual
  because the observer is already in that domain.
- Constant-time serve (pad every serve to recompute cost). Self-defeating, if serve costs a
  recompute the memo has no benefit. Padding to a fixed constant much greater than a hit but
  much less than a recompute only narrows the channel. Partial only.
- Hit padding / randomized threshold. Bounded-residual, adds latency variance, reduces
  per-query bandwidth, does not close (attacker averages over repeated probes).
Is zero-leakage cross-domain reuse achievable at all? Provably not. The benefit IS the leak.
A cross-domain reuse benefit (the caller is faster because another domain computed it) is
itself a cross-domain observable. You cannot both share the result across the boundary
(faster) and leak nothing (timing identical to no-share). This is the Armknecht/Harnik
impossibility restated for computation. The honest answer is domain-partitioning again, the
identical trade to committed dedup. The cross-CORE concurrent attacker remains SMP-blocked
and out of scope, same standing as dedup.

RC1-d DURABILITY plus MIGRATION.
Across reboot: the entry is (key -> Hv) in the durable content store, Hv persists via the
committed store plus persistence plus refcount GC (memo entries pin Hv as GC roots while
retained, retention policy open, per Cycle 4). An entry MEANS "under (Hc, Hi, C, D) the
admitted region's output is object Hv", a content-verified fact. Hc and Hi are content
hashes and survive reboot unchanged, Hv is re-hashed (BLAKE3) on read before trust. But C
and D are re-derived from CURRENT authority/domain state, NOT restored from the pinned
entry. This is exactly SR4's revocation FLOOR at rematerialization. An entry produced under
authority since revoked must not serve if the caller's current re-minted ceiling no longer
authorizes read(Hi) or exec(Hc). A memo entry is content-durable but AUTHORITY-EPHEMERAL,
the data is immutable history, the serve-right is floored to today's revocation state.
Across the migration wire: the VALUE transfers as content (diff-proportional transfer of the
Hv the destination lacks, re-hashed on arrival). Authority does NOT transfer, the
destination re-mints the serve-right from LOCAL grants (peer capability is a request
re-minted locally, never authority). Trusted cluster (both ends run the gate, share the
emulated root, Boundary 4): the destination re-derives C in the shared authority space and
reuses Hv after re-hash, BUILDABLE-GATED on the shared root. To reuse WITHOUT recompute
across the wire the consumer must trust the producer's assertion "Hv = output(Hc, Hi) at C".
SR5 emulated Ed25519 attestation can sign that (key -> Hv) binding, but this is an
emulated-platform self-reported attestation (not hardware-rooted), and the consumer still
cannot verify the producer's ADMISSION discipline (was the region really effect-free). So
distrusting-migration memo reuse without recompute is RESEARCH-ONLY / trust-delegating,
trusted-cluster reuse is buildable-gated.

RC1-e FORMAL TARGET (theorem shape).
T-CEILING (serve preserves the ceiling): for every admitted region and every caller,
authority(serve(key, caller)) <= authority(recompute(Hc, Hi, caller)). Serve's only
authority act is the caller's re-mint (bytes carry none), recompute uses the same re-mint,
so both equal the caller's own authority, both <= the caller's ceiling. PROVABLE, it reduces
to committed proofs/Carmix.v C1 anti_amplification plus a definitional lemma "served bytes
carry no authority". Cheap.
T-EQUIV (serve is observationally equivalent to recompute for admitted regions):
serve(key) = recompute(R). Requires (i) determinism of R given (Hc, Hi), the DRCC statement
(DRF implies one canonical output, CV8), PROVABLE-UNDER-NAMED-ASSUMPTIONS with A1
(SC-for-DRF / x86-TSO, not refined to HW), A4 (no live caps, checkable via cap set), A5
(canonical serialization = cv_canon), and (ii) soundness of the admission predicate ADM (no
uncaptured effect input), which is exactly A2, decidable only as a SUFFICIENT structural
condition (gate effect-closure), never as full purity (CV6 / Rice). So T-EQUIV is
PROVABLE-UNDER-NAMED-ASSUMPTIONS, the load-bearing assumption being ADM-soundness, the same
asserted-not-decided premise as DRCC A2. CARMIX-specific content: authority is a KEY
component (CV2) so the memoized function includes the ceiling, and admission is by
capability-effect-closure using the SFI checker's static guarantee as the oracle.

VERDICT RC1: BUILDABLE-NOW.
Safe subset named: the gate/wasm execution model only. Build (1) the memo table keyed by
(Hc, Hi, C, D), (2) capture linmem-in into Hi and linmem-out into Hv, (3) serve as a
store-get re-hashed before use and gated by the caller's own re-mint, (4) domain in the key
to close the cross-domain timing channel. This reuses only committed code and the admission
oracle already exists as the SFI checker. Native INT-0x80 memoization is BUILDABLE-GATED on
the frozen-cap-set flag with DRF asserted. Distrusting cross-wire reuse without recompute is
RESEARCH-ONLY.

CRITIC-FIX (verdictReason, was the placeholder "See verdictReason"). The BUILDABLE-NOW
rests on one confirmed fact: gate/sfi_checker.c's default-reject switch leaves no effect
opcode, so effect-closure to a single linmem slot is a static kernel guarantee, not a
promise. The memo layer adds only keying and store plumbing over committed primitives. The
verdict is scoped to the gate model precisely because the native model has no such static
bound.

Honest boundary (RC1). Grounding is read-only, no code was run. The SFI-effect-closure claim
is only as strong as the permitted GIR opcode set present today, and depends on the
mandatory CHERI runtime backstop being deployed (surfaced above). Admission is a sufficient
structural condition, never a decision of purity (CV6 / Rice, permanent). ADM-soundness and
DRF are ASSERTED per region, never decided (PROOF_CORE A2, the single largest honesty risk),
a region with an uncaptured effect input silently violates T-EQUIV and the system cannot
catch it. All cited dedup / timing numbers (42873 vs ~1285349 cyc, N-fold storage) are
QEMU-virtual and single-CPU, the cross-core concurrent memo-probe attacker is SMP-blocked
and unexercised. Migration transport is ivshmem-local and attestation is emulated, so
trusted-cluster reuse is a simulation against no real adversary. Bibliography is recall-only
and UNVERIFIED by fetch.

============================================================================
RC2  CERTIFICATE-CARRYING CONVERGENCE
============================================================================

Claim under test. The kernel CHECKS an individual equivalence witness (never SEARCHES for
one, never DECIDES the relation). An accepted witness authorizes a content-addressed
identity-class MERGE whose authority is the anti-amp MEET (never the join), domain-scoped,
tagged as a new tier strictly between committed T1 (structural-canonical) and T2
(behavioral, a permanent refusal branch). Minimal class to build first: a bounded
rewrite-trace checker over the cv_canon algebra.

RC0 nearest-neighbor table (each row UNVERIFIED, recall not fetch)

| Work | What it does | Delta vs this front |
|------|--------------|---------------------|
| Proof-Carrying Code (Necula, recalled POPL 1997) [UNVERIFIED, recall; core BORROWED] | Untrusted code ships a machine-checkable proof of a safety policy, the host CHECKS it in linear time, never searches. | PCC checks a safety policy on one program, RC2 checks an EQUIVALENCE between two content-addressed names and, on accept, MERGES their identity class under an authority MEET. PCC has no identity, merge, or authority-lattice notion. |
| Translation validation / credible compilation (Pnueli-Siegel-Singerman TACAS 1998, Necula PLDI 2000) [UNVERIFIED, recall; core BORROWED] | After each compiler run a validator checks source==target for this instance via a simulation witness, sidestepping proving the compiler correct in general. | Identical checker-not-decider discipline (closest neighbor). RC2 adds the content-addressed identity-merge consequence plus the anti-amp authority MEET and domain-scoping. TV has no capability or dedup dimension. |
| E-graphs plus equality saturation with explanations (egg, Willsey et al., recalled POPL 2021) [UNVERIFIED, recall; witness format BORROWED] | Congruence-closed e-classes, saturate rules to DECIDE equality within a theory, explanations emit a rewrite proof between two equal terms. | egg SEARCHES (saturates) and merges are authority-blind unions. RC2 only CHECKS a supplied trace (no search) and merges under authority MEET with cross-domain forbidden. RC2's certificate IS an egg explanation, checked in-kernel rather than produced there. |
| Bisimulation-relation checking / Hennessy-Milner, Hopcroft DFA minimization (already cited kernel/MEMBRANE_ARCHITECTURE.md:265) [UNVERIFIED, recall; core BORROWED] | A supplied relation certifies behavioral equivalence, checking the coinductive closure condition per pair is decidable over finite state. | This IS the RC2 bisimulation sub-case. Delta over the in-repo citation: bind the accepted equivalence to a content-addressed identity merge under MEET, and totality holds ONLY for finite-state gate objects, undecidable to even check for Turing-complete ones. |
| Unison content-addressed code (recalled) [UNVERIFIED, recall; structural tier BORROWED] | A definition's identity is the hash of its normalized alpha-canonical AST, structural identity decided by one hash compare. | Unison decides identity STRUCTURALLY, exactly CARMIX's committed cv_canon / T1. No checked semantic-equivalence layer, no authority. RC2 is the tier ABOVE Unison's structural identity. |
| Nix / Vesta input-addressed builds (recalled) [UNVERIFIED, recall; orthogonal] | A build output is named by a hash of its inputs, equal inputs share the output. | Input-addressing decides reuse by input identity and never proves two different builds produced equivalent outputs. RC2 checks equivalence of the OUTPUT names and merges them. Orthogonal axis, they compose. |
| Certificate Transparency / verifiable logs (RFC 6962, recalled) [UNVERIFIED, recall; discipline BORROWED] | Append-only Merkle log, an auditor CHECKS inclusion and consistency proofs (a checker, not a searcher). | Same cheap-checkable-witness discipline, different predicate (log membership, not object equivalence), no identity merge and no authority lattice. |
| CRDTs / delta-CRDTs (Shapiro et al., recalled 2011) [UNVERIFIED, recall; contrast] | Convergent replicated state merges by the lattice JOIN (least upper bound), giving strong eventual consistency. | CRDT merges DATA by JOIN, RC2 merges IDENTITY authority by MEET (D4), the opposite lattice direction, deliberately. The contrast is the point of the front. |

Novelty outcome: NOVEL-AS-FUSION. Exact delta sentence. The checker-not-decider technique
(verifier cheap, prover hard, an individual checkable witness for an undecidable relation)
is borrowed wholesale from PCC, translation validation, bisimulation-relation checking, and
e-graph explanations. The only new element is applying anti-amplification to IDENTITY
ITSELF: an accepted equivalence authorizes collapsing two content-addressed names into one
class whose authority is the MEET (never the join) and whose merge is REFUSED across dedup
domains, wired into the already-committed cv_canon / T0-T1 tier framework (INV-1, INV-4,
INV-5).

Where this sits in the committed spine. kernel/MEMBRANE_ARCHITECTURE.md:239-259 already fixes
three sameness tiers: T0 bit-exact (built), T1 structural-canonical (decidable, cv_canon is
the built slice), T2 behavioral (UNDECIDABLE by Rice, a refusal branch forever, :242, :269).
INV-1 mandates every sameness result carry a tag and refuses bare T2 queries, INV-4 forbids
upgrading CF-equal to behavior-equal, INV-5 domain-scopes CF-hashes exactly like dedup. RC2
adds ONE tier CERT_EQUIV between T1 and T2. A pair is promoted out of the default T2 refusal
ONLY when a certificate is presented and CHECKED. Absent a certificate the T2 refusal is
untouched. One tag plus one checker on top of INV-1.

RC2-a THE CHECKER CONTRACT (spelled to the invariant). verify(A, B, pi) is: TOTAL
(terminates on every input, bounded by |pi|), BOUNDED (cost = sum over steps of
O(term_size), no fixpoint, no saturation, no normalization-to-completion, no search, because
searching for pi is the undecidable half and the forbidden dodge), MEASURED (rdtsc like
every CV figure), SOUND (accept implies A and B are equal in the pinned rule-class),
INCOMPLETE by construction (silent / refuse on equivalent pairs with no witness in the
class), FAIL-CLOSED (any malformed step, out-of-range redex path, unknown rule id, arena
overflow, or endpoint-hash mismatch refuses and performs no merge). Trusted base = the
checker code plus the OFFLINE per-rule soundness of the pinned table. The certificate
PRODUCER is untrusted. A wrong certificate is refused because every step is re-applied and
re-hashed against the pinned table and the endpoints must hash to A and B.

RC2-b THE AUTHORITY RULE FOR IDENTITY (the one genuinely new bit, D4). cv_canon folds the
authority SHAPE (perms, length, offset) into the hashed bytes, so CV2 already proves two
names differing by one capability are DIFFERENT names, authority-blind convergence refused.
RC2 carries that into the MERGE: the merged class's authority = MEET(auth(A), auth(B)),
enforced as a guard that REFUSES the merge if auth(A) != auth(B). Under an
authority-PRESERVING rule table (the safe subset) auth(A) == auth(B) always, so MEET is a
no-op and the guard is defense-in-depth. The MEET is stated for the general case (a
bisimulation cert legitimately relating two objects of different authority) where taking the
join would launder the higher authority onto the lower name, amplification by identity.
Cross-domain class merges are FORBIDDEN: a merge is a dedup event, and CV5 already showed
cross-domain canonical dedup reopens the Harnik channel domain-partitioning closed, so the
merge reuses gc_rc's domain tag exactly like cv_dedup (INV-5).

RC2-c THE BOUNDARY RESTATED (D3, non-negotiable). CERT_EQUIV makes INDIVIDUAL equivalences
checkable, the RELATION stays undecidable (Rice, kernel/ACADEMIC_SYNTHESIS.md:274, boundary
3.5). accept implies "equal in the pinned finite rule-class", a DECIDABLE congruence
STRICTLY WEAKER than observational equivalence, never "behaviorally the same". No-certificate
stays refused. The kernel never enumerates or searches candidate certificates. Observational
equivalence remains a refusal branch forever (INV-4). Any prose that reads CERT_EQUIV as
behavioral sameness is the failed front.

RC2-d THE MINIMAL CLASS plus consumable spec. Pick the BOUNDED REWRITE TRACE over the
cv_canon algebra (not the bisimulation class, see verdict). Certificate = {version,
ruleset_id (= BLAKE3 of the pinned rule table), domain, A (start canonical hash), B (end
canonical hash), steps[]}, each step = {rule_id, redex_path (child-index path from root),
binding}. Checker: (1) refuse if ruleset_id != the kernel's pinned table hash, (2) refuse if
the two names are not same-domain, (3) cur := cv_store fetch(A), refuse if hash(cur) != A,
(4) for each step, locate redex at redex_path, look up rule_id in the pinned table, check its
LHS pattern matches with binding, rewrite to RHS, re-canonicalize the touched subtree with
the EXISTING cv_canon, cur := new hash, store_put the bytes, refuse on any mismatch / OOB /
overflow, (5) accept iff cur == B, (6) on accept do the domain-scoped class-union
(representative = min(A, B) by hash for determinism) with the MEET guard from RC2-b. Reuses
only committed code: cv_canon serialization plus BLAKE3 plus cv_store plus gc_rc domain tags
plus the CV2 authority-in-name fact plus the INV-1 tier tag. New surface is roughly 100-200
lines of loop-free-modulo-trace-length C.

Rule table v1 (safe subset, zero new trust). Seed the pinned table with ONLY the two
rewrites cv_canon already embodies, written explicitly: R-SORT (reorder a node's child
multiset, sound because cv_canon sorts children) and R-DEADPRUNE (drop an edge to a
gc_rc_get == 0 node, sound by CV3). With this table the checker's ACCEPT set is EXACTLY
cv_canon's structural-equal set, so v1 is a certificate-carrying RE-DERIVATION of structural
equivalence and a strict regression against CV5: the checker can NEVER accept a pair cv_canon
calls different. Adding any rule beyond cv_canon is gated one rule at a time on that rule's
offline soundness proof (Coq beside proofs/Carmix.v, or a CBMC harness per R3), blast radius
one proof.

CRITIC-FIX (Finding 2, value inflation of the NOW headline). The honest niche below already
says: for confluent, cheap-to-normalize rule theories, do NOT use a checker, extend cv_canon
and DECIDE via a hash, because a checker is strictly worse there. But rule table v1
reproduces exactly cv_canon's structural-equal set. So v1 IS that negative-value case, a
witness-checker that re-derives a hash the kernel already computes (textbook YAGNI). The
mechanism is buildable now, but v1 has ZERO standalone utility. Its only value is as a proved
seam (tier tag, MEET-merge, domain guard, fail-closed refusal, rdtsc measurement) with no new
soundness burden, ready for the first non-confluent rule. The verdict below is worded to say
exactly that, not "a useful checker ships now".

Honest niche / where NOT to use the checker. If a semantic rule is confluent and cheap to
normalize, do NOT add it to the checker, fold it into cv_canon (extend T1) and keep DECIDING
via a canonical hash. The checker earns its place ONLY for rule theories with no
computable/confluent normal form, where there is no canonical representative to hash and
checking a supplied finite trace A ->* B is the only bounded option. That is the precise,
narrow scope beyond the committed structural tier.

Borrowed vs new (D1/D6, all recall, UNVERIFIED). BORROWED: the checker discipline (PCC,
translation validation), rewrite-trace-as-proof (egg explanations),
bisimulation-relation-as-certificate (Hennessy-Milner / Hopcroft, cited in-repo), tier-tagging
(committed INV-1), domain-scoping (committed INV-5), cv_canon plus BLAKE3 plus store
(committed). NEW: only the merge-by-authority-MEET rule with its refuse-on-auth-mismatch
guard, and the placement of CERT_EQUIV strictly below T2's permanent refusal. Modest but real.

VERDICT RC2: BUILDABLE-NOW (mechanism/seam only, honestly de-inflated per CRITIC-FIX).
Safe subset named: the RC2-a bounded rewrite-trace checker delivered as one new INV-1 tier
tag (CERT_EQUIV) with rule table v1 = {R-SORT, R-DEADPRUNE}, merge under the authority MEET,
domain-scoped (INV-5), fail-closed, rdtsc-measured. It reuses only committed code, adds
~100-200 lines, and ships with a zero-new-trust regression (never accepts a pair CV5 calls
different). Explicit honesty: v1 carries ZERO standalone utility because it re-derives a hash
the kernel already computes. What ships now is the proved SEAM, not a useful checker. The
useful case (a non-confluent rule with no normal form) is BUILDABLE-GATED, one rule at a time,
on that rule's offline soundness proof. RC2-b bisimulation certificates for gate-executable
objects are BUILDABLE-GATED only for declared FINITE-STATE gate objects with an enumerable
transition table, and are RESEARCH-ONLY / KILL for Turing-complete gate objects because even
CHECKING a claimed bisimulation is not total when the transition relation cannot be
enumerated.

Honest boundary (RC2). D3, non-negotiable: certificates make individual equivalences
checkable, the RELATION stays undecidable (Rice, kernel/ACADEMIC_SYNTHESIS.md:274). The
checker is SOUND and INCOMPLETE, accept means "equal in the pinned finite rule-class",
strictly weaker than observational, never "behaviorally the same" (INV-4). No-certificate
stays refused, the committed T2 permanent-refusal branch is preserved not replaced. The
kernel never searches for a certificate, it only checks supplied ones, total and bounded by
|pi|. Two further limits: (1) the checker earns its place over extending cv_canon only for
non-confluent or expensive-to-normalize theories, for confluent cheap rules extend T1 and
decide, (2) the only new security claim is anti-amplification applied to identity (merge by
MEET, cross-domain forbidden), which under the safe authority-preserving table is a no-op
guard, and the offline per-rule soundness proofs remain a trusted base the kernel does not
itself discharge.

============================================================================
RC3  BRANCH-MERGE CALCULUS OVER SYSTEM ROOTS
============================================================================

Claim under test. SR (kernel/SR_LOG.md) already banks branch (SR3: two divergent roots
retained, both rematerializable). The missing operation is merge: fold two System Roots that
forked from a common ancestor back into one. This front defines merge in four rungs over
Merkle roots, states the ceiling rule (per-component MEET plus SR4 revocation floor at merge
time), and states the confluence condition tied explicitly to DRF/DRCC.

RC0 nearest-neighbor table (each row UNVERIFIED, recall not fetch)

| Work | What it does | Delta vs this front |
|------|--------------|---------------------|
| Aviram et al. OSDI 2010, Determinator [UNVERIFIED, recall; confluence idea BORROWED] | Deterministic parallel OS, threads mutate private workspaces, merge deterministically at explicit reconcile barriers, a concurrent write to the same location by two threads is a runtime fault. | Closest neighbor to rungs 2 and 4. Private-workspace plus deterministic-merge-at-barrier IS the disjoint-subtree confluence, and its fault-on-overlap IS rung-4 refusal. New in CARMIX: merge is over content-addressed Merkle roots (structural, diff-proportional, order-free) with a per-component authority MEET plus SR4 revocation floor folded in. |
| CRDTs / delta-state CRDTs (Shapiro et al. 2011, Almeida et al. 2018), Automerge/Yjs [UNVERIFIED, recall; LUB merge BORROWED] | Coordination-free merge of concurrent replicas via join-semilattice LUB, strong eventual consistency, no total order. | Adjudicated hard: for the monotone fragment CRDTs COVER rung 3a entirely. New in CARMIX: only gating the merge edge behind the anti-amp ceiling (MEET) and reading SEC off as hash-equality. Authority-scoping is the sole delta, the merge itself is not novel. |
| Porter et al. SOSP 2009, TxOS system transactions [UNVERIFIED, recall; contrast] | OS-level transactions over system objects with conflict detection and commit/abort, resolving conflicts by serialization (a total order). | Adjudicated hard: TxOS SOLVES the rung-4 conflicted case CARMIX declares RESEARCH-ONLY, by paying exactly the coordination cost CALM says non-monotone merge requires. CARMIX's rung-4 non-answer is a deliberate refusal to impose order, not a novelty. |
| git merge (merge-base, 3-way, fast-forward) [UNVERIFIED, recall; core BORROWED] | 3-way tree/text merge of content-addressed commits against a common ancestor, fast-forward when one side is an ancestor, heuristic/user conflict resolution. | Rung 1 and the disjoint case of rung 2 ARE git's fast-forward and tree merge. New in CARMIX: authority ceilings folded into hashed nodes so merge carries a per-component MEET plus revocation floor, and conflicts are refused, never heuristically resolved, because a wrong resolve could amplify authority. |
| Darcs / Pijul patch theory [UNVERIFIED, recall; commutation BORROWED] | Merge by patch commutation, independent patches commute to a canonical result regardless of order, overlapping patches conflict. | Patch commutation is precisely the disjoint-write-set confluence, over patches rather than content roots and with no authority semantics. New in CARMIX: the commutation predicate is tied explicitly to DRF/DRCC and the merged object carries the authority MEET. |
| Burckhardt & Leijen, Concurrent Revisions [UNVERIFIED, recall; typed merge BORROWED] | Fork-join isolation types where each shared type supplies a deterministic per-type merge function applied at join. | Per-type merge functions ARE rung-3 certificates. New in CARMIX: the resolver runs over content-addressed roots and its merge edge is authority-gated by the anti-amp MEET. |
| Nix/Guix generations, OSTree [UNVERIFIED, recall; re-point BORROWED] | Content-addressed system generations with rollback by re-pointing a durable root, generations are linear, no branch merge. | Covers rung 1 (fast-forward/rollback by re-point) only. New in CARMIX: rungs 2-4 (an actual merge of two divergent roots) which Nix/OSTree do not attempt, plus the authority calculus. |
| Self-adjusting computation / Adapton (Acar) [UNVERIFIED, recall; substrate BORROWED] | Incremental recomputation with change propagation over a dependency DAG, recomputing only affected nodes. | Not a competitor, this is the SUBSTRATE that makes merge cheap (the epoch tree's diff-proportional re-fold is the same only-changed-paths idea). Borrowed as mechanism, not as a merge calculus. |

Novelty outcome: NOVEL-AS-FUSION. Exact delta sentence. Every rung's merge mechanism has
prior art (fast-forward, 3-way tree merge, patch commutation, CRDT LUB, typed resolvers,
fault-on-overlap). The only new content is folding the swcap authority ceiling into the
merged node as a per-component MEET plus the SR4 revocation floor applied at merge time, and
tying the confluence condition explicitly to the built DRF/DRCC predicate (CV8).

The four rungs (merge of branch roots R_A, R_B forked from last-common root M0, M0 found by
the DBG-4 last-common-node walk, per-component change-set by the DBG-2 hash-pruned structural
diff).

Rung 1 FAST-FORWARD. M0 == R_A, so merge = adopt R_B. Decided by hash-ancestry over the
retained epoch-root chain. This is git / Nix generation / OSTree fast-forward, latent in SR3
plus persistence plus the epoch chain. No conflict, no ceiling arithmetic beyond identity.

Rung 2 DISJOINT-SUBTREE. WriteSet(A) and WriteSet(B) over the reachable component leaves are
disjoint (DBG-2 structural diff against M0, pruned one hash-compare per equal subtree). Merge
= 3-way recombine: for each of the 8 SR leaves take M0's leaf unless exactly one branch
changed it, then diff-proportionally re-fold through store/epoch_tree.c (incremental == full,
verified). Recombination is merge-order-independent because disjoint edits commute (a
Church-Rosser diamond). Per-component ceiling by the MEET rule below.

Rung 3 CERTIFICATE-ASSISTED (consumes RC2 = the lineage log / attestation / delegation
record). WriteSet(A) and WriteSet(B) OVERLAP on a leaf, but the overlap resolves canonically
because either (3a) the leaf carries a join-semilattice/CRDT type whose LUB is a pure
function of the delivered delta-set (Boundary 2 escape), so merge = rematerialize both,
compute LUB, store_put, re-root, or (3b) an RC2 certificate attests the transition that
produced the overlap (transition-correctness for the DRF spine, or authority-monotone edge
A_post subset A_pre from the lineage-log leaf), so the merge adopts the certified value. The
certificate is itself a content-addressed object re-verified by re-hash before it is trusted.

Rung 4 CONFLICTED. WriteSet overlap on a leaf with two order-dependent, non-monotone values
and no lattice/certificate (a read-modify-write cell, two competing balances). Structural
diff DETECTS this (same leaf, two hashes, no common LUB). There is no canonical merged root:
content-addressing yields a SET not a sequence (the double-spend negative restated). The
honest operation is fail-closed REFUSAL with a distinct reason (mirroring the DBG-5 refusal
branches and the SM4 amplify/bounds refusals), never a heuristic resolve.

THE CEILING RULE AT MERGE (anti-amplification applied to identity itself, D4, take the MEET).

CRITIC-FIX (Finding 1, D4 VIOLATION, confirmed against proofs/Carmix.v:129-133). The packet
as first drafted claimed the merged ceiling "= MEET ... valid = valid_A && valid_B" AND that
"re_mint(C_A ; base_B, limit_B, perms_B) IS the glb". That is FALSE and is the exact D4 trap
this exercise exists to catch. re_mint (proofs/Carmix.v:133) produces
`valid = valid_A && negb(reject(Pinter(perms_B, perms_A)))`. It NEVER references valid_B.
Consequences: (a) internal inconsistency, the cited mechanism contradicts the stated formula,
(b) order-dependent validity, re_mint(C_A ; B).valid = valid_A && k versus
re_mint(C_B ; A).valid = valid_B && k differ whenever valid_A != valid_B, which breaks
Rung 2's "unique canonical root independent of merge order", (c) join-in-disguise on the
validity dimension, if valid_B = false but valid_A = true then re_mint(C_A ; B) yields a
VALID cap descended from an invalid branch, and the committed anti_amplification theorem only
proves result <=cap C_A (cap_le:106 requires valid d -> valid s, which fails for s = C_B), so
the merged cap is NOT a lower bound of C_B, and the separate revoked-now floor does not
necessarily catch a minted-invalid or W^X-rejected branch, (d) perm-list order, Pinter
preserves the requester's list order (proofs/Carmix.v:81), so the two mint directions give
the same SET in different ORDER, and since cv_canon folds the perms bytes into the hashed
authority shape (and child-sort covers children not the perms list) the merged canonical name
is merge-order-dependent unless perms are separately sorted.

Corrected merge ceiling (the actual glb in cap_le, order-independent by construction). For
each component:
  base_m  = max(base_A, base_B)
  limit_m = min(limit_A, limit_B)
  perms_m = SORT(Pinter(perms_A, perms_B)) canonicalized before folding
  valid_m = valid_A && valid_B && negb(reject(perms_m))
This is symmetric in A and B on every field, so it is a true meet and the merged canonical
name is merge-order-independent. re_mint ALONE is not the glb because it drops valid_B, so
the merge cannot be a single existing gate call, it needs an explicit "AND both valids, sort
perms" step (a few lines) on top of the gate's clamp. Under an authority-PRESERVING branch
pair (auth_A == auth_B, the common quiescent case) the correction is a no-op, but for the
general different-authority merge the correction is load-bearing and without it Rung 2/3
confluence and the no-amplification claim are false. Taking the JOIN instead (min base, max
limit, union perms, OR of valids) would let a branch merge back authority its fork never
held, amplification-by-merge, so the MEET is forced, not chosen.

SR4 REVOCATION FLOOR AT MERGE TIME. Merge is a rematerialization, so SR4's revocation FLOOR
applies: the merged authority is the corrected MEET AND (NOT revoked-now). A component
revoked after the fork but before the merge is floored to nothing in the merged root.
Revocation stays forward-only, merge cannot resurrect it.

A BRANCH NEVER MERGES BACK MORE AUTHORITY THAN IT FORKED WITH (the load-bearing invariant).
Let C0 be the fork ceiling. Mint only moves down, so anti_amplification gives C_A <=cap C0
and C_B <=cap C0 along each branch. The corrected MEET is a lower bound of BOTH (this is what
the fix restores, the old re_mint-only form was a lower bound of C_A only), so
MEET(C_A, C_B) <=cap C_A <=cap C0, and the revocation floor only lowers it further. Result
<=cap C0. This reuses the existing anti_amplification lemma composed with the glb property,
standing is gate-checked for the COMPOSITION, never machine-checked.

THE CONFLUENCE CONDITION, tied explicitly to DRF/DRCC. Two branches provably merge to a
UNIQUE canonical root independent of merge order IFF WriteSet(A) intersect WriteSet(B) is
empty (rungs 1-2), OR every leaf in the intersection carries a canonical LUB or an RC2
certificate (rung 3). This is the inter-branch LIFT of the intra-run DRCC/DRF condition: DRCC
proves that when two threads write DISJOINT slots the memory value at the cut is
schedule-invariant (Church-Rosser diamond over the interleaving lattice) hence one canonical
hash. Replace threads/interleaving with branches/merge-order and slots with Merkle subtrees
and it is the identical argument. The mergeable case == the DRF case, the conflicted case ==
the racy case. CV8 is the BUILT, MEASURED evidence at the intra-run level on 2 real cores:
the DRF disjoint-slot workload converged to ONE canonical name across 12 runs, the racy
same-slot control diverged to 12 names. Rung-2 disjoint-subtree merge is that same predicate
at the branch level. HONEST GATING: the confluence condition over branches produced
SEQUENTIALLY/QUIESCENTLY needs no DRCC (it is the classical commuting-edit 3-way merge,
decidable by structural diff). DRCC is required only to claim confluence over branches
produced by CONCURRENT SMP execution, or to claim each branch's internal state is
reproducible-by-rerun. DRCC is paper-argued and unvalidated on SMP, so the branch-merge
confluence THEOREM inherits that gate, the mechanism and the single-machine confluence do not.

Per-rung verdicts.
Rung 1 fast-forward: BUILDABLE-NOW. Safe subset: single machine, quiescent epoch boundary,
both roots retained. Reuses SR3 branch plus persistence plus epoch chain. Only new code is a
hash-ancestry check.
Rung 2 disjoint-subtree: BUILDABLE-NOW for the MECHANISM on the quiescent single-machine
subset (3-way structural recombine = DBG-2 plus DBG-4 diff already built, re-fold via
epoch_tree verified, ceiling MEET = the corrected AND-both-valids/sort-perms step).
BUILDABLE-GATED (on DRCC) for the confluence THEOREM over concurrently-produced SMP branches.
CV8 already supplies measured evidence for the predicate.
Rung 3 certificate-assisted: BUILDABLE-GATED. Gate = RC2 certificates existing, plus DRCC for
the reproducible / transition-correctness variants. Sub-case 3a (semilattice/CRDT LUB) is
BUILDABLE-NOW on the quiescent subset but is prior-art COVERED except for the authority-MEET
on the merge edge. Sub-case 3b (succinct proof) is RESEARCH-ONLY (BLAKE3
arithmetization-hostility plus per-step prover cost).
Rung 4 conflicted: DETECTION is BUILDABLE-NOW (structural diff plus fail-closed refusal).
Canonical automatic RESOLUTION as a pure content-addressed primitive is KILL: irreducible by
the double-spend negative (a hash names a set, ordering two competing mutations needs
consensus CARMIX deliberately lacks). Policy/consensus-assisted resolution via an explicit
signed order structure is RESEARCH-ONLY and, even built, is not consensus and not canonical.

Reuse map (nothing new invented for rungs 1-2 and 3a beyond glue and the ceiling correction):
last-common-node plus structural diff (DBG-2/DBG-4), diff-proportional re-fold
(store/epoch_tree.c), ceiling MEET (the corrected step over the swcap clamp), revocation
floor (SR4 path), retained branch roots (SR3), durable re-point (persistence), fail-closed
refusal (DBG-5 / SM4). CRDT LUB merge (3a) reuses the SHAREDMAP per-owner ceiling plus SM4
refusal harness.

VERDICT RC3: BUILDABLE-GATED.
Net verdict is BUILDABLE-GATED because the front's headline claim (confluent authority-safe
merge of divergent System Roots) holds now only for the quiescent single-machine rungs 1-2
and 3a, while the confluence THEOREM over concurrent SMP branches is gated on DRCC (which is
paper-argued, unvalidated on SMP), and rung 4 automatic content-addressed resolution is KILL.

CRITIC-FIX (verdictReason, was the placeholder "see above"). The gating is real and not
cosmetic: (1) the no-amplification and order-independence claims are TRUE only with the
corrected MEET (AND both valids, sort perms), not with re_mint alone, (2) confluence over
concurrently-produced branches inherits the unvalidated DRCC SMP gate, (3) rung 4 is
irreducible. Detection ships now, resolution never (KILL). The single-machine quiescent
subset (rungs 1, 2, 3a) is genuinely buildable now once the ceiling step is corrected.

Honest boundary (RC3). Standing: the merge COMPOSITION is C-tested/gate-checked, never
machine-checked. Only the one-clamp anti_amplification, W^X/forbidden-freedom, and
finite-tree acyclicity in proofs/Carmix.v are proven, and merge reuses the first WITHOUT
extending the proof, and the corrected two-sided MEET is NOT yet a proven lemma (it is the
composition of the proven one-clamp with a hand-argued glb symmetry, this is the honest
standing after the fix, not a machine-checked meet). The confluence tie to DRF/DRCC rests on
DRCC, paper-argued and unvalidated on SMP. CV8 evidences the predicate for the exercised
2-core workloads only, not universally. All rungs assume a QUIESCENT epoch cut on one
machine, concurrent capture and real-network merge are out of scope, same as SR. Rung 4 is
irreducible: content-addressing solves tamper, not double-spend. Structural not observational
merge only: two behaviorally-equal but structurally-different branch states will be reported
CONFLICTED correctly and can never be recognized as mergeable (CV6 / boundary 5). Every merge
across a domain is two separate checks (re-hash content AND re-mint authority), never
conflated.

============================================================================
RC4  THE UNIFIED CLAIM
============================================================================

THE FUSED SYSTEM CLAIM (stated precisely). CARMIX can support a convergent-computation layer
in which (i) the output of an admitted pure region is memoized durably in the content store
under a key that includes the authority ceiling and the dedup domain, and served by re-hash
instead of re-execution, (ii) an individual, checked equivalence certificate can collapse two
content-addressed names into one identity class, and (iii) two divergent System Roots forked
from a common ancestor can be merged back into one root. Across all three the single
governing discipline is anti-amplification applied to IDENTITY ITSELF: any reuse, merge, or
identity rule takes the authority MEET (greatest lower bound in cap_le), never the join, and
is refused across dedup domains. Served bytes carry no authority, every serve or merge
re-mints the right from the caller's own local grants and re-hashes content before trust, and
every cross-domain reuse benefit is treated as a cross-domain observable and priced as such.

What is genuinely new, in one sentence, is not any mechanism but the fusion: authority (the
swcap ceiling) and domain are made components of computational identity, so memoization
reuse, certified merge, and root merge all take the authority MEET and are domain-partitioned,
turning the committed CV2 refusal (authority is in the name) and the committed dedup
domain-partition remedy into a general law over reuse, convergence, and merge.

ONE HONEST DELTA SENTENCE PER NEAREST NEIGHBOR (all recall, UNVERIFIED by fetch).
- Nix / Bazel / input-addressed builds: they memoize impure processes with sandbox-approximated
  input sets, CARMIX makes the capability set the input set via a static kernel checker and
  puts the authority ceiling in the key, but CARMIX has no build-graph scheduler and this is
  simulated single-machine only.
- Unison: it gets purity from the language for free, CARMIX admits effectful gate code via
  capability effect-closure and binds authority into identity, but CARMIX's admission is only
  a sufficient structural condition and can silently miss an uncaptured effect.
- Adapton / self-adjusting computation: it does incremental change-propagation, CARMIX does
  not do incrementality at all and only borrows the diff-proportional re-fold as substrate,
  the delta is authority-keying and the timing analysis, not faster recompute.
- Determinator / deterministic-parallelism runtimes: they give reproducibility and
  fault-on-overlap, CARMIX reuses that predicate as DRCC and adds a durable authority-keyed
  memo/merge object, but CARMIX's DRCC is unvalidated on real SMP beyond the CV8 2-core
  workloads.
- Harnik / Armknecht dedup side-channel: they prove zero-leak cross-user sharing impossible,
  CARMIX restates the same impossibility for "computation was performed" and applies the same
  domain-partition remedy, this is a borrowed bound with a new target, not a new result.
- PCC / translation validation / CompCert: they prove a transform correct or check one
  instance, CARMIX borrows the checker-not-decider discipline and adds an identity-merge under
  MEET, but CARMIX proves nothing about the transform, only serve==recompute for an
  asserted-pure region.
- e-graphs / equality saturation (egg): it searches and merges authority-blind, CARMIX only
  checks a supplied explanation and merges under MEET with cross-domain forbidden, and CARMIX
  never searches.
- CRDTs: they merge data by the lattice JOIN, CARMIX merges identity authority by the opposite
  lattice MEET, and for the monotone fragment CRDTs fully cover CARMIX's LUB rung so the only
  CARMIX delta there is authority-gating the merge edge.
- TxOS system transactions: it resolves conflicts by imposing a total order, CARMIX
  deliberately refuses to impose order and returns fail-closed on the conflicted case, so TxOS
  is the prior art that shows the price of the answer CARMIX declines to pay.
- git merge / Darcs-Pijul / Concurrent Revisions / Nix generations: they provide 3-way merge,
  patch commutation, typed resolvers, and root re-point respectively, CARMIX reuses each and
  adds only the per-component authority MEET plus revocation floor folded into the hashed node.

THE KILL-LIST (what CARMIX still cannot claim after this program, even if everything lands).
1. Not a decision of purity, determinism, or observational equivalence. Admission and merge
   equivalence are SUFFICIENT structural conditions only. A region with an uncaptured effect
   input silently violates the memo equivalence and the system cannot catch it (CV6 / Rice /
   PROOF_CORE A2). This is the single largest standing risk.
2. Not machine-checked at the composition level. Only the one-clamp anti_amplification,
   W^X/forbidden-freedom, and finite-tree acyclicity are proven in proofs/Carmix.v. The
   memo-serve theorem, the certificate-merge MEET, and the corrected two-sided branch-merge
   MEET are gate-checked/C-tested compositions of that lemma, not proven lemmas themselves.
3. Not zero-leakage cross-domain reuse. Provably impossible (the benefit is the leak). The
   only remedy is domain-partitioning, which costs N-fold duplicate computation and storage,
   and even then within-domain hit timing remains.
4. Not validated against a real adversary or real SMP. All numbers are QEMU-virtual and
   single-CPU, the cross-core concurrent probe is SMP-blocked, migration is ivshmem-local, and
   attestation is emulated Ed25519, not hardware-rooted. Distrusting cross-wire reuse without
   recompute stays RESEARCH-ONLY / trust-delegating.
5. Not a resolver of genuine conflicts. Rung-4 automatic content-addressed merge resolution is
   KILL. Content-addressing solves tamper, not double-spend. Ordering two competing
   non-monotone mutations needs consensus CARMIX deliberately lacks, so the honest operation is
   detection plus fail-closed refusal.
6. Not behavioral sameness, ever. CERT_EQUIV and merge are STRUCTURAL. Two behaviorally-equal
   but structurally-different objects stay refused (T2 permanent-refusal branch, INV-4). No
   certificate is ever searched for, only checked.
7. Not a useful checker on day one (RC2). The buildable-now rule table v1 re-derives a hash the
   kernel already computes and has zero standalone utility. Only the proved seam ships now, the
   useful non-confluent rules are each individually gated on an offline soundness proof.
8. Not free of the runtime backstop assumption (RC1). Effect-closure holds only relative to the
   modeled opcode set AND assumes the mandatory CHERI runtime backstop is deployed.

BIBLIOGRAPHY (every entry UNVERIFIED, recall not fetch, no fetch tool exists here,
borrowed-vs-new stated per entry).
- Dolstra et al., Nix / the purely functional software deployment model. [UNVERIFIED, recall]
  Borrowed: input-addressed memoization. New here: capability-as-input-set plus authority in
  the key.
- Bazel / Buck2 / Shake / redo build systems. [UNVERIFIED, recall] Borrowed: hermetic action
  caching. New here: kernel-enforced effect-closure rather than sandbox convention.
- Chiusano & Bjarnason, Unison content-addressed code. [UNVERIFIED, recall] Borrowed:
  content-addressing of definitions. New here: effectful admission plus authority-in-identity.
- Acar et al., self-adjusting computation, and Hammer et al., Adapton. [UNVERIFIED, recall]
  Borrowed: diff-proportional re-fold as substrate. New here: authority-keying, not
  incrementality.
- Aviram et al., Determinator (OSDI 2010), Bergan et al. dOS, Bocchino et al. DPJ.
  [UNVERIFIED, recall] Borrowed: DRF-determinism predicate and fault-on-overlap. New here:
  durable authority-keyed memo/merge object.
- Harnik, Pinkas, Shulman-Peleg, side channels in cloud dedup, and Armknecht et al.
  cross-user dedup analysis. [UNVERIFIED, recall] Borrowed: the impossibility bound and the
  domain-partition remedy. New here: same bound restated for "computation performed".
- Necula, Proof-Carrying Code (POPL 1997), and CompCert (Leroy). [UNVERIFIED, recall]
  Borrowed: cheap-checker-not-decider discipline. New here: identity-merge under MEET.
- Pnueli, Siegel, Singerman, translation validation (TACAS 1998), Necula credible compilation
  (PLDI 2000). [UNVERIFIED, recall] Borrowed: per-instance witness checking. New here:
  content-addressed merge consequence plus authority MEET.
- Willsey et al., egg / equality saturation with explanations (POPL 2021). [UNVERIFIED, recall]
  Borrowed: rewrite-trace explanation as witness. New here: check-not-search plus MEET merge.
- Hennessy & Milner, Hopcroft DFA minimization (bisimulation checking, cited in-repo).
  [UNVERIFIED, recall] Borrowed: relation-as-certificate over finite state. New here:
  identity merge plus finite-state-only totality boundary.
- Shapiro, Preguica, Baquero, Zawirski, CRDTs (2011), Almeida et al. delta-state CRDTs (2018),
  Automerge/Yjs. [UNVERIFIED, recall] Borrowed: LUB merge for the monotone fragment. New here:
  opposite lattice (MEET) on the authority edge.
- Porter et al., TxOS system transactions (SOSP 2009). [UNVERIFIED, recall] Contrast: shows
  the coordination price of the rung-4 answer CARMIX declines to pay.
- git merge-base / 3-way merge, Darcs and Pijul patch theory, Burckhardt & Leijen Concurrent
  Revisions. [UNVERIFIED, recall] Borrowed: merge mechanisms (fast-forward, commutation, typed
  resolver). New here: authority MEET plus revocation floor in the hashed node.
- Nix/Guix generations, OSTree. [UNVERIFIED, recall] Borrowed: root re-point rollback. New
  here: actual branch merge (rungs 2-4) plus authority calculus.
- Hellerstein & Alvaro, the CALM theorem. [UNVERIFIED, recall] Borrowed: monotone==
  coordination-free framing that justifies the rung-3a vs rung-4 split.
- RFC 6962 Certificate Transparency. [UNVERIFIED, recall] Borrowed: checkable-witness log
  discipline, different predicate, no identity or authority.

END OF PACKET.
