# CARMIX Proof-Core Research Packet (R1 to R4)

## 0. Method and vocabulary lock

Vocabulary is locked. "Machine-checked" and "proven" are reserved for what
proofs/Carmix.v actually contains (Coq source, every theorem closed by Qed,
nothing Admitted). Everything else is model-proven (holds in an abstract model
not tied to the running binary), C-tested (exercised by runtime attack tables),
or paper-argued (asserted in the two synthesis documents). seL4-scale
source-to-abstract or source-to-binary refinement is out of reach for this team
and is named as a boundary, never crossed. No result below claims it.

This is a verdict exercise. Each front returns exactly one of PROVABLE,
PROVABLE-UNDER-NAMED-ASSUMPTIONS (assumptions named), or NOT-AT-REASONABLE-COST
(reason given), with the reasoning that forces the verdict.

Files are named repo-relative: proofs/Carmix.v, carmix/swcap.c,
store/object_store.c, kernel/ACADEMIC_SYNTHESIS.md,
kernel/MEMBRANE_ARCHITECTURE.md, kernel/U3_LOG.md, kernel/kernel.c.

---

## 1. The exact current proof boundary (what proofs/Carmix.v machine-checks)

proofs/Carmix.v is 357 lines. It models one pure function and one finite
inductive datatype. It has no notion of a machine, a wire, a thread, a
schedule, Ed25519, an epoch, or a second party. The machine-checked content is
exactly three stages plus one corollary, over the seven-element permission enum
{Read, Write, Execute, Seal, Unseal, Invoke, AccessSysRegs} (Carmix.v:29-36).

STAGE C1, anti_amplification (Carmix.v:139). For all source cap s and request
(breq, lreq, preq), if valid (re_mint s breq lreq preq) = true then
re_mint s breq lreq preq <=cap s. Here re_mint (Carmix.v:129-133) is the clamp
lo = max breq (base s), hi = min lreq (limit s), the permission intersect
Pinter preq (perms s), and validity = valid s && negb (reject ps), where reject
= has_wx || has_forbidden. The order cap_le (Carmix.v:105-108) is the
conjunction (valid d -> valid s) and (base s <= base d and limit d <= limit s)
and Psubset (perms d) (perms s). So the theorem is a monotonicity fact that
holds by the definition of re_mint. It is real and narrow.

STAGE C2, two theorems. valid_dest_no_wx (Carmix.v:206) says a valid re_mint
output never holds both Write and Execute. valid_dest_no_forbidden
(Carmix.v:217) says a valid re_mint output holds no member of {Seal, Unseal,
Invoke, AccessSysRegs}.

STAGE C3', acyclic_graph (Carmix.v:342). Objects are the finite inductive tree
Obj := node nat (list Obj) (Carmix.v:254-255). A child is a strict subterm, so
children_smaller (Carmix.v:276) is a proven lemma, not an assumption, using the
structural measure osize. The content edge is edge u v := In (Identity v)
(map Identity (obj_children u)) (Carmix.v:307), path is its transitive closure,
and the result is forall u, ~ path u u. The corollary no_id_fixpoint
(Carmix.v:349) says path u v -> Identity u <> Identity v.

The single Hypothesis in the whole file is Identity_injective : forall a b,
Identity a = Identity b -> a = b (Carmix.v:301). It is used twice, at
id_edge_iff_child (Carmix.v:317) to make the id-edge coincide with the
structural-child edge, and again inside no_id_fixpoint (Carmix.v:352) to collapse
equal ids to equal objects. It is used nowhere to supply well-foundedness. The
well-foundedness is carried entirely by the structural osize measure that the
inductive Obj type provides for free. This distinction is load-bearing for R1.

That is the entire proof boundary. Anti-amplification of one clamp-and-intersect
function, W^X and forbidden-freedom of its valid outputs, and acyclicity of a
finite inductive tree under one injectivity hypothesis. Everything the rest of
the system asserts is C-tested or paper-argued (ACADEMIC_SYNTHESIS.md:11-17).

---

## R1. Operational acyclicity as a theorem (hash-consed shared DAG)

VERDICT: PROVABLE-UNDER-NAMED-ASSUMPTIONS. The crypto-only absolute variant is
NOT-AT-REASONABLE-COST and, in its absolute form, not even true.

### R1 statement

Model the real store as a finite partial map S : Hash -> Bytes. This matches
store/object_store.c, which keys opaque bytes by BLAKE3 and never parses the
bytes (find_entry at object_store.c:33, store_put at object_store.c:43). Overlay
a node set N with payload and children functions, a naming function
name(n) = H(encode(payload n, map name (children n))), and require S(name n)
resident for every n. Define the content edge as name(m) occurring in the
child-name slots of S(name n), and path as its transitive closure. This is
exactly the shape of edge and path in Carmix.v. The operational claim to promote
from comment to theorem is that this edge graph has no cycle, so a hash cannot
name a not-yet-existing object.

Provable form. Assume (H-WF) a well-founded rank t : N -> nat with
edge n m -> t m < t n. Then forall u, ~ path u u. The proof is isomorphic to
Carmix.v path_size_decreasing and acyclic_graph, with the derived osize replaced
by the assumed rank t, about 30 lines. So operational acyclicity is provable and
cheap once H-WF is admitted.

### R1 named assumptions

1. H-WF (the load-bearing one). The content edge respects a well-founded
   creation order, so a parent name cannot be formed until every child name
   already exists. This is the formal content of hash-before-name. It is a
   modeling axiom about the minting API, not a cryptographic fact. Git, IPFS,
   and Nix make the same operational assumption in prose.
2. Encoding faithfulness. encode is such that child-name slots are recoverable
   from the bytes, so edge is well-defined on S. Minor.
3. Deliberately NOT assumed: injectivity or collision-freedom of BLAKE3. It is
   neither necessary nor sufficient for the absolute theorem (see reasoning).
   Naming it as the acyclicity assumption would be a misattribution.

### R1 reasoning

Why H-WF is the real hypothesis and crypto is not. Carmix.v proves acyclicity of
Obj := node nat (list Obj), a finite inductive type whose child is literally a
strict subterm, so children_smaller is a proven lemma and osize strictly
decreases along edges. The well-foundedness is baked into the datatype.
Identity_injective does not supply it. Read operationally, the inductive Obj is
itself a model of hash-before-name, because to build a parent value you must
already hold the child values. So Carmix.v proves acyclicity of a structure that
has H-WF built into its type. The operational gap is exactly discharging H-WF for
the real flat store, which has no inductive structure to donate it.

The obstruction to a crypto-only proof (force acyclicity from BLAKE3 alone with
no order axiom) is NOT-AT-REASONABLE-COST for two independent reasons. First, it
is false as an absolute statement. A cycle a to b to a forces
name(a) = H(pa, [H(pb, [name(a)])]), a fixed point of a composed hash map.
Injectivity forbids collisions H(u) = H(v) with u not equal v. It does not forbid
fixed points. The identity map is injective and all-fixed. The subterm-size
argument that carries the inductive proof dies at the hash, because a name is 256
bits regardless of what it names, so H collapses the very measure the
well-foundedness rode on. Hence injectivity is not sufficient, and H-WF alone
gives acyclicity with no crypto at all, so injectivity is also not necessary.
Second, Carmix.v Identity_injective is false of real BLAKE3 as a mathematical
function, because an arbitrary-length domain maps into a 256-bit codomain, so
collisions provably exist and are merely infeasible to find. The only true
statement of the crypto flavor is computational: no feasible adversary can craft
bytes committing to their own or a future name (preimage and collision
resistance in a random-oracle model). Formalizing that is a game-based
cryptographic development at the scale of FCF, SSProve, or EasyCrypt, an order of
magnitude beyond the current pure-functional Coq, and it would yield a hardness
bound rather than the clean acyclic the comment asserts. Out of reach for this
team at reasonable cost.

C-code gap (why H-WF is not even enforced today). store/object_store.c stores
opaque bytes. find_entry and store_put never parse child-name slots, never check
that a referenced child hash is resident, and never enforce hash-before-name. A
buggy or adversarial client can store_put bytes embedding a hash of a
not-yet-existing or self object. The only thing stopping a genuine self-loop B
with BLAKE3(B) inside B is a preimage search, that is computational hardness, not
any store invariant.

Honest bridge from comment to theorem (buildable, not free). Add referential
integrity on insert. store_put parses the object child-name slots and rejects any
put referencing a non-resident hash. Then insertion order is a provable
well-founded rank witnessing H-WF, and operational acyclicity becomes a theorem
about the actual store. This needs typed objects with a child-name layout, which
the opaque-byte model of store/object_store.c lacks, so it is a real modest
engineering plus spec step, not free.

Net: operational acyclicity is provable-under-named-assumptions, the assumption
being well-foundedness of the naming order (hash-before-name), not
collision-resistance. The crypto-only version is not-at-reasonable-cost and in
its absolute form not true.

### R1 borrowed vs new

- BORROWED from Carmix.v: the whole skeleton edge, path, path_size_decreasing,
  acyclic_graph, no_id_fixpoint transfers unchanged. Only the measure changes
  from the derived osize to an assumed rank t.
- NEW (correction): replace structural-subterm well-foundedness (an artifact of
  the inductive Obj) with an explicit H-WF creation-order axiom on the flat store
  graph, because the flat store has no inductive structure to supply it.
- NEW (boundary): identify Identity_injective as transport, not the source of
  acyclicity, note it is false of real BLAKE3, and note the operationally
  meaningful crypto statement is computational preimage-resistance, not absolute
  injectivity.
- NEW (C bridge): referential integrity on insert makes insertion order a
  provable H-WF witness, contingent on giving objects a typed child-name layout
  the store currently lacks.

### R1 relation to CV8

R1 owns the well-foundedness framing (acyclicity rides on hash-before-name
creation order, not on injectivity) and the fixed-point-under-injectivity
obstruction. The observation that Carmix.v Identity_injective is false of real
BLAKE3 and that the true property is computational is a general crypto-assumption
critique any BLAKE3-auditing front raises. To avoid double counting, cite that
injectivity-versus-computational-hardness point once, attributed to whichever
front owns the crypto-assumption modeling. R1 uses it only to justify not naming
collision-resistance as the acyclicity assumption.

---

## R2. DRCC as a theorem

VERDICT: PROVABLE-UNDER-NAMED-ASSUMPTIONS.

### R2 smallest operational model

1. Threads. A finite set T = {1..n} over shared memory M : Loc -> Val. Each
   thread is a deterministic finite action sequence whose next action is a
   function only of values read. In U3-3, n = 2, one thread per real core (BSP,
   AP), M is the 64-byte u3_slot array.
2. Data-race-freedom, CARMIX-shaped. Between two consecutive synchronization
   barriers each shared location is written by at most one thread (the own-slot
   discipline). In code, core idx writes only u3_slot[idx and 1], so write-sets
   are disjoint and no conflicting pair exists.
3. Synchronization cut B. A barrier at which every thread has completed all
   pre-B actions and started none of its post-B actions. In U3-3 the BSP reads
   the slots only after the AP has finished (ap_wait after ap_kick).
4. Canonical-state function CF : State -> Hash. CF(M) = BLAKE3 of a serialization
   that enumerates locations in a fixed order independent of the interleaving. In
   code, cut[0..31] = slot0, cut[32..63] = slot1, then BLAKE3 over the 64 bytes.
   The general CF is cv_canon over the reachable acyclic graph, sorted-child
   bottom-up (kernel/kernel.c near 8259).

### R2 the shared statement (stated once, named identically by theory and CV8)

Fix P = two deterministic threads under the own-slot discipline (core idx writes
only u3_slot[idx and 1]), M0 = zeroed slots, B = the ap_wait join, and
CF(M) = BLAKE3(slot0 || slot1). Let exec(P, sigma, M0) be the memory produced by
schedule sigma. Define R(sigma) := CF(exec(P, sigma, M0) at B). Theory and
measurement both range over exactly this R.

- Theory names the universal: for all schedules sigma, R(sigma) = h, a single
  value determined by (P, M0).
- Measurement (U3-3, U3_LOG.md:56, site kernel/kernel.c:6681-6689) names three
  reruns that all produced R = d05014af13f5df61, plus a negative control where
  the racy variant P' (both cores writing slot0 unlocked) produced R' differing
  across runs (U3_LOG.md:57).

Honest gap between them. The measurement does not vary sigma. Because the
own-slot discipline makes the two write-sets disjoint, exec(P, sigma, M0) is the
same memory for every sigma by construction, so the three reruns cannot evidence
schedule-independence even in principle. They evidence run-to-run repeatability
of a program whose output is schedule-independent by construction. The value of
U3-3 is twofold. It confirms the DRF program is repeatable, and its negative
control confirms the DRF hypothesis is load-bearing, since the racy program is
not repeatable. So theory and measurement name the same function R, but the
measurement supplies zero schedule-variation evidence for the disjoint-slot case,
not a sample of the schedule space. U3-3 self-labels FIRST EXERCISE, one
instance, NOT a full validation (U3_LOG.md:56), which is the correct standing. It
tests R, it does not prove the for-all. The model theorem below is the object
that would establish schedule-independence for the general DRF-synchronized case
(distinct locations reachable under a real barrier), where interleavings genuinely
differ and confluence does the work.

### R2 theorem and proof sketch (in the model)

For a DRF program P with cut B, for all schedules sigma, sigma' reaching B,
CF(exec(P, sigma, M0) at B) = CF(exec(P, sigma', M0) at B). Sketch: DRF makes
adjacent actions from different threads on distinct locations independent, hence
commuting (Bernstein conditions). Any two schedules reaching B are connected by a
finite sequence of adjacent transpositions of independent actions (a
Church-Rosser diamond over the interleaving lattice), so exec is
schedule-invariant as a memory value. CF is a function, so equal memory gives
equal hash with no injectivity needed. This is the classical
determinacy-of-DRF-programs result plus a free hash step.

### R2 named assumptions

- A1 SC-for-DRF memory model. The commuting argument assumes atomic sequentially
  consistent per-location accesses. On two real cores this is the
  hardware-plus-compiler DRF-SC guarantee (x86-TSO with store buffers, plus no
  compiler reordering across the sync point). That is a separate deep theorem for
  x86-TSO, not re-proven here and not connected to the C. Discharging it against
  real hardware is the forbidden seL4-scale refinement.
- A2 DRF is an asserted premise, never a decided one (permanent). Deciding
  whether an arbitrary P is DRF reduces to reachability and is undecidable, so
  the OS cannot check it. DRF implies reproducible is provable, its antecedent is
  unverifiable for any real workload. The repo already concedes this
  (ACADEMIC_SYNTHESIS.md:17, MEMBRANE_ARCHITECTURE.md section 9, workloads we
  ASSERT are race-free). This is the single largest honesty risk.
- A3 Genuine quiescent cut. B must be taken only when every thread has finished
  its pre-B actions and started none after. The ap_wait join satisfies this for
  the one instance. The general no-global-stop release-cut (B3 in
  ACADEMIC_SYNTHESIS.md:233, a Chandy-Lamport-style consistent snapshot) is
  unbuilt and its consistency unproven, so the theorem cut is realized only for
  the stop-and-join case.
- A4 Per-thread determinism (no live caps). Each thread must be a deterministic
  function of values read, with no RNG, clock, device, net, or uninitialized
  read. CARMIX enforces this by excluding tasks holding such caps
  (FROZEN-REPRODUCIBLE versus FROZEN-RETAINED-ONLY).
- A5 Schedule-independent canonical serialization. CF must traverse state in a
  fixed order. slot0 || slot1 then BLAKE3 satisfies it. Appending in
  completion-order would break the theorem.

### R2 verdict rationale

Not plain PROVABLE, because the front binds the statement to U3-3 on two real
cores and under that binding A2 is a permanent undecidable premise no CARMIX code
can discharge, while A1 and A3 are real model-versus-machine gaps the theorem
cannot cross without forbidden refinement. Not NOT-AT-REASONABLE-COST, because
the model implication (DRF implies determinacy) is a standard textbook result
whose mechanization is bounded and well understood. Honest cost caveat: this
mechanization is a concurrency-semantics development (threads, shared memory,
schedules, action independence, an adjacent-transposition confluence argument
over the interleaving lattice). That is a materially harder class than anything
in proofs/Carmix.v, whose 357 lines are clamp-and-intersect monotonicity plus
finite-tree acyclicity. The team has demonstrated the clamp-monotonicity class,
not the concurrency-semantics class, so any line estimate is uncertain and larger
than Carmix.v, and I decline the earlier within-demonstrated-capability framing.
The mathematics is standard and the result is not out of reach, but it is a step
up in proof-engineering class the repo has not yet exhibited. The expensive part,
refinement down to running C and to x86-TSO, is fenced into A1 and A3 as named
assumptions rather than claimed.

### R2 relation to Carmix.v

DRCC is not in Carmix.v and shares almost nothing with it. Carmix.v has no
thread, schedule, memory, or determinism. The only reusable fragment is
Identity_injective, and only for the converse (soundness) half, where same hash
implies same state needs CF injective. The forward direction DRCC actually
asserts, same state implies same hash, needs only that CF is a function and
borrows nothing from Carmix.v.

### R2 borrowed vs new

- BORROWED (prior art, must not be claimed new): determinacy of DRF programs.
  Independent actions commute (Bernstein 1966). DRF implies sequential
  consistency (Adve and Hill DRF0, 1990). The Church-Rosser confluence argument.
  This is the entire mathematical core. CARMIX invents none of it.
- NEW but minor (a restatement of content-addressing, not a theorem): using CF =
  BLAKE3 canonical form as the equality oracle, so same state is read off as a
  single hash comparison. The synthesis makes the identical concession about SEC
  and CRDTs.
- SEPARATE, do not conflate: cv_canon folding layout-distinct-but-equal states to
  one name is the structural-canonical direction, decidable, orthogonal to the
  schedule-independence claim DRCC makes.

---

## R3. The smallest real-C rung

VERDICT: PROVABLE-UNDER-NAMED-ASSUMPTIONS.

### R3 statement

The most tractable rung is a bounded model check with CBMC run directly on
carmix/swcap.c. Over the actual C source it would establish, for
cvsasx_sw_cap_remint on every CVSASX_OK return, that out_cap perms are a bitwise
subset of region->object_cap.perms, that [base, base+length) lies wholly inside
the region, that no Write-plus-Execute pair and no SW_FORBIDDEN_MASK bit is set,
that out_cap.valid is 0 on every error return (the fail-closed zeroing at
swcap.c:59 is never reneged), that cvsasx_swcap_check (swcap.c:98) grants access
only when in-bounds and need_perms present with the subtraction
c->length - offset never wrapping, and that there is no unsigned overflow, OOB
read, or null-deref in the gate arithmetic or the 32-byte hash loops.

The technical fact that forces this as the answer: every loop in
cvsasx_sw_cap_remint, cvsasx_sw_cap_strip, sw_check_region, and
cvsasx_swcap_check has a compile-time-constant trip count (CVSASX_BLAKE3_LEN = 32
in hash_eq at swcap.c:15-17 and in the strip copy at swcap.c:37, and
sizeof(pir) in the strip zeroing at swcap.c:29). There are no data-dependent
loops. Both bounds checks guard their subtractions (swcap.c:71-72 and
swcap.c:101-102). So full unwinding with --unwinding-assertions is exhaustive and
the bounded check becomes a complete decision over all inputs, discharged by a
SAT or SMT engine. Under the vocabulary lock this is a C-tested-to-completeness
result, an exhaustive bounded model check, not a Coq-and-Qed machine-checked
proof. It closes exactly the nat-versus-machine-word and clamp-versus-reject gap
that Carmix.v structurally cannot express (Coq nat never overflows), on the
running C rather than an abstraction.

### R3 named assumptions

1. Tool trust. Soundness of the CBMC C front-end and its bit-precise memory
   model. The checked object shifts from a small Coq kernel closed by Qed to a
   SAT or SMT discharge behind CBMC. Larger trusted base than Carmix.v, smaller
   than trusting a hand-written model.
2. No miscompilation. The freestanding kernel toolchain binary must match CBMC
   source-level semantics. Not closed here. Source-to-binary certification is the
   seL4 and Sewell result (ACADEMIC_SYNTHESIS.md:310) and is out of reach for
   this team. CBMC verifies C-source semantics only, so boundary 3.7 stays open
   for the compiler gap.
3. BLAKE3 stubbed. The referent-hash security still rests on collision-resistance,
   the same class as Carmix.v Identity_injective and the store S0 note
   (object_store.c:52-55). CBMC proves the arithmetic and permission logic around
   the hash_eq compare, never the hash.
4. Harness faithfulness. pir, region, custodian modeled as fully nondeterministic
   with no over-constraint. An over-tight harness yields false confidence, so the
   harness is a review artifact.
5. Isolation scope. Functions verified standalone. It assumes, does not prove,
   that callers route every authority crossing through the gate
   (MEMBRANE_ARCHITECTURE.md invariant 2). No concurrency, no information-flow,
   no temporal safety of the handle.

### R3 reasoning

Ruling out the alternatives. Option (a) proof-carrying extraction is the wrong
tool for the actual gate C, because Coq extraction produces a verified different
implementation with a managed runtime and GC that cannot run in a freestanding
no-malloc kernel, and connecting it back to swcap.c is forbidden refinement. It
yields a statement about a substitute, not the real C. Option (b) a hand-written
bit-level model re-encodes swcap.c in a prover and closes the width gap, but it
leaves the same model-versus-code gap Carmix.v already has
(ACADEMIC_SYNTHESIS.md:35), one abstraction lower, at comparable or greater
effort, and still never touches the source. Option (c) CBMC is the only one whose
input is carmix/swcap.c verbatim.

Why (c) is tractable, not aspirational. The gate is tiny and loop-free modulo
constant trip counts, so complete unwinding converts bounded checking into a
decision procedure over all inputs. The predicates (swcap.c:70-78 bounds and
permission anti-amplification, plus the post-mint verify at swcap.c:89-92) are
pure integer and bitmask relations, the sweet spot for SMT bit-vector solving.
Cost for a CBMC-literate engineer is roughly 1 to 3 weeks to a first proven
harness, most of it stubbing cvsasx_blake3 and getting the freestanding headers
and CVSASX_PERM_ and PIR structs to parse. Solve time is seconds to minutes. That
is within reach, so the verdict is not not-at-reasonable-cost.

Why not plain PROVABLE. Two reasons force the qualifier. First, the vocabulary
lock reserves proven and machine-checked for Carmix.v (Coq, Qed). A CBMC result
is discharged by a SAT or SMT engine behind a C front-end, so the trusted base is
the tool, a genuine assumption (assumption 1). Second, CBMC verifies C-source
semantics, so the source-to-binary gap stays open (assumption 2), the exact seL4
and Sewell item at ACADEMIC_SYNTHESIS.md:310 that would retire boundary 3.7
fully, out of reach for this team. Add the BLAKE3 abstraction (3) and isolation
scope (5) and the result is real and conditional.

### R3 borrowed vs new

- BORROWED: CBMC and the AWS s2n and FreeRTOS style of proving loop-free C is
  off-the-shelf. Nothing new invented. Applying a mature tool to about 100 lines
  of loop-free C. No proof-engineering research.
- NEW for CARMIX: only the harness and the assertion set encoding the anti-amp
  postcondition against the real uint64 and uint32 types. The property that makes
  the model check complete (loop-free modulo constant) is a fact about this
  specific code, not a contribution.

### R3 relation to CV8

The load-bearing predicate is identical to Carmix.v C1 anti_amplification (dest
authority <= source) plus the C2 W^X and forbidden-free lemmas. Carmix.v proves
it over the nat and list-Perm model with clamp semantics. This rung proves the
same predicate over the real {uint64 base, uint64 length, uint32 perms}
reject-semantics C. If CV8 is the abstract-re_mint front, a CBMC pass is the
bridge that lets a model-level lemma be legitimately asserted for the running
gate arithmetic and permission logic, retiring boundary 3.7 for that logic though
not the compiler gap.

---

## R4. Bibliography discipline

VERDICT: NOT-AT-REASONABLE-COST for live citation verification, because no fetch
was performed. Every entry below is recalled from training knowledge of
title and venue, not fetch-verified, so the whole set is flagged UNVERIFIED
against a live source. What IS deliverable at reasonable cost, and is delivered
here, is the borrowed-versus-new reduction cross-checked against what the C
actually contains, plus a load-bearing short list and the specific attribution
errors. This front deliberately does not survey the roughly seventy-name pool.
Reproducing that catalogue under a VERIFIED banner would be a literature tour and
would stamp recall as verification, which is the specific dishonesty the exercise
warns against.

### R4 what the code grounding actually shows (this part is verified against files)

- BLAKE3 and Ed25519 are BORROWED unchanged because they are vendored in-tree
  (store/blake3/, kernel/tweetnacl.c).
- Poseidon, verkle, KZG, and IVC are DESIGN-ONLY. grep finds them only as prose
  in kernel/ACADEMIC_SYNTHESIS.md and kernel/MEMBRANE_ARCHITECTURE.md (for
  example ACADEMIC_SYNTHESIS.md:231), never in code.
- store/object_store.c confirms the dedup story rests on BLAKE3
  collision-resistance with no byte re-compare (object_store.c:52-55).

### R4 the reduction (the real work)

CARMIX novelty across the whole pool reduces to exactly two items, matching what
the repo itself claims (ACADEMIC_SYNTHESIS.md:48). First, the content-address
binding of authority, so re-mint rebuilds from re-verified content instead of a
slot or token. Second, the seven-permission machine-checked anti_amplification
lemma in proofs/Carmix.v. Everything else is borrowed mechanism plus integration
plus honest boundary-drawing.

Per-category borrowed vs new:

- Capability systems. BORROWED: the authority-attenuation machinery (seL4 CDT
  revoke, CHERI monotonic derivation and otypes and Cornucopia baseline, EROS
  single-level store, Miller attenuation, SPKI/SDSI local recheck). NEW: (a)
  content-address binding of the referent, (b) the machine-checked anti_amp
  lemma. NOT new: rebuilding authority as such, since SPKI/SDSI is acknowledged
  prior art (ACADEMIC_SYNTHESIS.md:48).
- Content-addressable stores. BORROWED: all byte storage and movement (content
  chunking, packing, deadlist GC, multihash agility, Merkle proofs, and the
  operational DAG-acyclicity ASSUMPTION Git, IPFS, and Nix make). NEW: naming
  live execution state, orthogonal to file and block CAS, plus authority-coupled
  naming. NOT new: the acyclicity result, since Carmix.v proves a finite
  inductive tree is acyclic, which is exactly the operational assumption those
  systems already make.
- DRF and memory-model reproducibility. BORROWED: essentially everything (Adve
  and Hill DRF, Chandy-Lamport and Mattern cuts, CALM monotonicity, CRDT and
  delta-CRDT SEC, TrueTime interval shape). NEW: nothing formal. DRCC is a
  paper-argued restatement applied to content hashes, unvalidated on the built
  SMP. Weakest-novelty category. Claim integration, not a theorem.
- Verified compilation and PCC. BORROWED: PCC framing, CompCert source-to-binary
  (cited as a GAP CARMIX lacks, ACADEMIC_SYNTHESIS.md:35 and :310), IVC, PCD,
  Nova recursion, Certificate Transparency structure. NEW: only the design
  observation that content-addressing makes the transition statement cheap. Does
  nothing for per-step prover cost. No verifiable-computation machinery is built
  in-tree.
- BLAKE3 and Merkle. BORROWED unchanged: BLAKE3 and Ed25519 (both vendored),
  Merkle trees, vector commitments, graph-canonicalization theory, Rice. NEW:
  folding authority into the hashed bytes, which reduces to a commitment covers
  its bytes, integration discipline, no cryptographic novelty.

### R4 load-bearing short list (about 30, all UNVERIFIED by fetch, recall only)

seL4 refinement (Klein et al., recalled SOSP 2009), seL4 binary correspondence
(Sewell et al., recalled PLDI 2013), seL4 intransitive noninterference (Murray et
al., recalled IEEE S&P 2013), EROS confinement (Shapiro and Weber, recalled IEEE
S&P 2000), CHERI and Cornucopia (Filardo et al., recalled S&P 2020), Miller
Robust Composition 2006, SPKI/SDSI (RFC 2693), Git, ZFS, Nix, LBFS
(Muthitacharoen et al., recalled SOSP 2001), Harnik et al. cross-user dedup
timing (recalled 2010), Chandy-Lamport 1985, CALM (Hellerstein and Alvaro,
recalled CACM 2020), CRDTs (Shapiro et al., recalled 2011) and delta-CRDTs,
Necula PCC (recalled POPL 1997), CompCert (Leroy), IVC (P. Valiant, recalled TCC
2008), PCD, Nova folding (recalled CRYPTO 2022), Certificate Transparency (RFC
6962), BLAKE3 (vendored), Ed25519 (vendored), Luks 1982 and Babai (recalled STOC
2016), Rice, Denning lattice (recalled CACM 1976), Liu and Layland (recalled JACM
1973). A published bibliography should be trimmed toward this list rather than the
full pool, and every venue here must be fetch-checked before publication.

### R4 flagged attribution issues (none load-bearing)

1. Adve and Hill release consistency. The phrase Adve-Hill release consistency
   occurs at ACADEMIC_SYNTHESIS.md:234 (not in kernel/MEMBRANE_ARCHITECTURE.md,
   which contains no Adve-Hill string). It is imprecise. Adve and Hill own weak
   ordering and data-race-free, which is the concept DRCC actually needs.
   Release consistency as a named model is Gharachorloo, Lenoski, and colleagues.
   Fix the label to Adve-Hill DRF plus Gharachorloo release consistency, or drop
   release consistency from the Adve-Hill attribution.
2. Bloom (CALM context) is the Alvaro et al. declarative language, not a Bloom
   filter. Disambiguate in any published bibliography.
3. Valiant (IVC) is Paul Valiant, not Leslie Valiant. Note to prevent collision.
4. RevocationTransparency is a Ben Laurie and Google white paper, not
   peer-reviewed. Cite as an informal draft. Cryptographic revocation
   accumulators are the solid co-cited anchor.

### R4 relation to CV8

The single artifact every front shares is proofs/Carmix.v, the anti_amplification
monotonicity theorem over the seven-permission re_mint model plus the finite-tree
acyclic_graph result under the one Identity_injective hypothesis. That file is the
only thing in the whole bibliography carrying the words machine-checked and
proven, and it is the source of the one genuinely new citation-backed
contribution. Everything else is borrowed, model-proven, C-tested, or
paper-argued.

---

## Appendix: honesty-critic disposition

The seven critic findings are incorporated as follows.

- F1 (R4 survey dodge and non-conforming verdict): FIXED. R4 verdict is now
  NOT-AT-REASONABLE-COST for live verification. The seventy-entry roll-call is
  dropped in favor of the reduction plus a roughly thirty-entry load-bearing list.
- F2 (VERIFIED overstated recall as verification): FIXED. Every entry is labeled
  recalled and UNVERIFIED by fetch. No entry is stamped VERIFIED GENUINE.
- F3 (Adve-Hill mis-anchored to MEMBRANE 234): FIXED. Re-anchored to
  ACADEMIC_SYNTHESIS.md:234. The substantive Adve-Hill versus Gharachorloo
  correction stands.
- F4 (R2 inflated 3 runs into 3 schedules): FIXED. The shared statement now says
  the measurement does not vary sigma, that disjoint slots make exec identical
  for every sigma by construction, and that the reruns witness repeatability plus
  a negative control, not a sample of the schedule space.
- F5 (R2 under-costed the model theorem): FIXED. The within-demonstrated-capability
  framing is withdrawn. The mechanization is named a concurrency-semantics
  development materially harder than the Carmix.v clamp-monotonicity class, with
  an uncertain and larger estimate, still not out of reach because the
  mathematics is standard.
- F6 (R1 said Identity_injective used only in id_edge_iff_child): FIXED. It is
  used at Carmix.v:317 and again at :352, never to supply well-foundedness.
- F7 (R3 COMPLETE, a real proof brushed the lock): FIXED. The CBMC result is
  described as a complete decision over all inputs discharged by SAT or SMT, a
  C-tested-to-completeness bounded model check, explicitly not a Coq-and-Qed
  machine-checked proof.

What survives scrutiny across R1 to R3: no front claims seL4-scale ambition, no
front dresses a borrowed result as new, no front overstates what Carmix.v
machine-checks, and R2 A2 (undecidable DRF) is grounded in the repo's own
concession at ACADEMIC_SYNTHESIS.md:17 and MEMBRANE_ARCHITECTURE.md section 9.
