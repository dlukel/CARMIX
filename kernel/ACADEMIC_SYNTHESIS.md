# CARMIX: An Academic Synthesis

Tier-labeled, honesty-calibrated. This document states what the repository actually proves, what it tests, what it designs, and what it cannot express. Where the earlier synthesis prose borrowed the standing of the machine-checked core to cover mechanisms that core does not model, this version restricts the words "proven" and "machine-checked" to what kernel/proofs/Carmix.v actually contains and relabels the rest as "designed and tested against attack tables" or "ported and adapted."

## 0. What the machine-checked core actually is (read this first)

Everything below rests on a precise reading of kernel/proofs/Carmix.v. Three facts bound every claim in this document.

1. The object type is a finite inductive tree, `node : nat -> list Obj -> Obj`, with no sharing. A finite inductive type is acyclic by construction in the proof assistant, so `children_smaller` and the acyclicity result are true of every inductive type. The operational claim that matters for the thesis, that a content hash cannot name a not-yet-existing object so cycles cannot form in the store, appears as a comment, not as a proof. The datatype has no sharing, so it cannot represent the deduplicated directed acyclic graph the storage thesis depends on. Correct statement: CARMIX proves a finite tree is acyclic (trivial) and assumes in prose that the hash construction yields such a structure. That is the same operational assumption Git, IPFS, and Nix make, restated in a proof assistant.

2. `anti_amplification` is a monotonicity result that holds by the definition of `re_mint`. `re_mint` is defined to intersect permissions and clamp the range, and the theorem confirms the intersection is a subset and the clamp stays in bounds. It is a consistency check that `re_mint` is not self-contradictory over a seven-element permission enum. It is real and useful. It is not a deep result, and its small assumption budget follows from its small scope.

3. The proof models one pure function. It has no notion of a machine, a wire, Ed25519, migration, an epoch, or a second party. Every claim of the form "carried across the machine boundary," "post-sign widening breaks Ed25519," or "enforced at spawn, IPC, delegation, cross-core, and on the migration wire" is C code plus prose, exercised by runtime attack tables, not machine-checked. The C custodian (kernel/cap/cap_custodian.c) additionally notes that anti-amplification is claimed only within the modelled hosted lattice, not over arbitrary permission bits, which are fail-closed by C rather than covered by the theorem.

The honest one-line standing of the whole system is proven-model plus tested-code, where the model is a seven-permission `re_mint` monotonicity lemma plus finite-tree acyclicity under one injectivity hypothesis, and everything else is designed and tested. The code-level honesty in the tree (stubs labeled, assumptions named, `SEALED_UNSUPPORTED`, the revocation non-tracking note, `otype_class` marked unsolved) is better than the earlier prose-level honesty, and this document aligns the prose down to the code.

DRCC (a data-race-free program reaches a reproducible content hash at a synchronization-point cut) is a paper-argued premise. It is not a theorem in kernel/proofs/Carmix.v. Six of the forward-looking proposals rest on it. Wherever DRCC is invoked below, read "argued on paper and unvalidated on the built SMP," not "machine-checked."

---

## 1. Per-domain gap analysis

Each domain gives four things: where CARMIX genuinely leads, where it trails, which prior-art mechanisms it can adapt rather than invent, and the precise boundary of the contribution.

### Domain 1: Capability-based systems (KeyKOS/EROS, seL4, CHERI, Fuchsia/Zircon)

**Advantages.**
- Re-mint replaces the bearer token. KeyKOS/EROS keys, seL4 CNode slots, and Zircon handles ship a live token that is copied or moved. CARMIX transfers content-addressed state plus a stripped machine-independent descriptor (referent hash, offset, canonical perms) and an Ed25519-signed authority ceiling, and the destination mints a fresh local cap through the gate, bounded by that ceiling (kernel/cap/swcap.c, `cvsasx_sw_cap_remint`). The wire carries a bound, not authority. This is a design property enforced by C and tested, not a theorem.
- Machine-independent capabilities. A cap references (content hash, offset) rather than a local virtual address, so re-mint re-derives it wherever the object is currently instantiated. seL4 caps are node-local, Zircon handles are per-process indices, CHERI caps are local virtual addresses. None survive migration without translation.
- Content-addressed immutability closes the target-staleness class that CHERI sweeps for. A stale cap re-minted against a reused region fails the referent-hash check (`CVSASX_ERR_REFERENT_MISMATCH`) rather than silently aliasing. This addresses target staleness only, not authority revocation.
- The anti-amplification lemma is machine-checked with zero admits over the seven-permission model. This is a genuine, if narrow, verified core with an explicit and small assumption budget.
- Clean three-layer separation with a distinct fail-closed reason per check.

**Gaps.**
- No functional-correctness refinement of the implementation. seL4 has an abstract-spec-to-C refinement (Klein et al., SOSP 2009) and down to the binary (Sewell et al., PLDI 2013). CARMIX proves an abstract `re_mint` and does not prove kernel/cap/swcap.c refines it. The divergence is concrete: the Coq `re_mint` clamps bounds, the C rejects out-of-bounds (`CVSASX_ERR_BAD_BOUNDS`). Correspondence rests on six runtime attack tables, not a proof.
- No confinement or information-flow guarantee. Anti-amplification bounds how much authority a cap holds. It says nothing about leakage. EROS proved constructor confinement (Shapiro and Weber, IEEE S&P 2000) and seL4 proved intransitive noninterference (Murray et al., IEEE S&P 2013). CARMIX has no analogue. This is the single largest distance from the state of the art.
- No hardware per-instruction enforcement in the software backend. CHERI enforces bounds on every load, store, and derive. kernel/cap/swcap.c checks bounds only at explicit call sites, so it is a coarse authority gate, not fine-grained memory safety.
- No sealing, no object-type invocation, no protected domain transition. Re-mint of a sealed cap returns `CVSASX_ERR_SEALED_UNSUPPORTED`, and Seal/Unseal/Invoke are in the forbidden mask. Content-addressing gives identity opacity, not use opacity. A hash is not a sealing substitute.
- Revocation-by-derivation is seL4's Capability Derivation Tree revoke with content-addressed targets. The novelty is the content binding, not the mechanism. Cost at depth is unmeasured, and the trust store, revoked set, and nonce set are in-memory only with a single authority key.
- Maturity gap. The others are complete, deployed capability systems with rich object models. CARMIX is a research kernel with content blocks and re-minted memory caps.

**Deep adaptations.**
- seL4 CDT revoke, recast so derivation targets are content-addressed and re-mint replaces mint-from-slot with mint-from-re-verified-region. Mechanism inherited, content binding adapted.
- CHERI monotonic derivation, reused directly on a CHERI purecap target, plus a post-mint verification (`CVSASX_ERR_VERIFY_LENGTH`) against the bounds-compression rounding-outward vector and a content-hash referent binding.
- KeyKOS/EROS single-level persistent store, adapted so capabilities persist as re-mintable descriptors rather than saved live tokens.
- Miller-style attenuation (Robust Composition), adapted so each hop is a subset by the anti-amp definition and revocation walks the recorded subtree instead of using caretaker interposition.

**Boundary.** CARMIX recasts authority as a rebuildable bound over content-addressed state rather than a transferable token, and it machine-checks the monotonicity of that rebuild over a seven-permission model. Three edges. (1) Proof versus code: the Coq result is the abstract authority algebra, the C rejects where the model clamps, so implementation correctness is tested, not proven. (2) Magnitude versus flow: anti-amplification bounds the size of granted authority and is categorically weaker than the confinement and information-flow theorems of EROS and seL4. (3) Structure versus opacity: content-addressing kills target staleness but provides no sealing or invoke and no fine-grained hardware enforcement off a CHERI target. Honest placement: a migration-native authority model with a small verified core, trailing seL4 on verified-code depth, EROS/seL4 on confinement, CHERI on enforcement granularity and sealing. Note also that certificate-as-offline-authority-statement systems (SPKI/SDSI) are closer prior art to "authority rechecked locally" than a claim of zero prior art would suggest. The defensible novelty is the content-address binding plus the machine-checked ceiling, not the idea of rebuilding authority as such.

### Domain 2: Content-addressable and immutable systems (Git, IPFS/IPLD, Venti, ZFS, Nix/Guix, NDN, Merkle/verkle)

The framing: prior art content-addresses inert data whose bytes are present at rest. CARMIX aims to content-address live execution state with authority folded into the hashed content. That aim is genuine and orthogonal to the file/block CAS literature. The honest caveat is that the machine-checked model represents a tree, not the shared DAG the dedup story requires, so dedup is a design property of the C store, not a proven property of the model.

**Advantages.**
- Names a live process, not files or blocks. Git checks out a tree, ZFS rolls back to an uberblock, CRIU checkpoints opaque bytes. None give a running process a content hash. This is the thesis and it is orthogonal to all prior art here.
- Dedup is intrinsic to the naming scheme rather than a bolt-on side table like the ZFS DDT. Measured store-level dedup exists in the C implementation. The extension to process/task state is a design goal, not a proven one.
- Acyclicity is established for the tree model, so refcounting is complete for that model and no cycle collector is needed. Git/IPFS/Nix assume acyclicity operationally. CARMIX proves it for a finite tree and assumes in prose that the hash construction produces such a structure. This is a smaller gap over the prior art than "we prove what they assume" would claim.
- Authority-coupled naming: authority metadata is part of the hashed content, so two states of differing authority cannot alias to one name. No CAS in this domain has any authority notion. The mechanism is "the commitment covers the bytes it commits to," which is true of any commitment, so the contribution is the discipline of putting authority in those bytes, not a special cryptographic property.
- Domain-partitioned dedup closes the cross-user dedup timing side channel (Harnik, Pinkas, Shulman-Peleg, 2010) with measured cycle counts (a cross-domain probe moves from a fast hit to a full miss). Honest note: disabling cross-domain dedup at N-fold storage cost is exactly the countermeasure that paper recommends. The contribution is measuring and integrating it, not discovering it.
- DRCC states as a formal cut what Nix/Guix pursue empirically for build outputs. DRCC is paper-argued and unvalidated on SMP, so this is a stated design boundary, not a delivered result.
- Re-rooting gives snapshots as retained root hashes over the same store as process state, plus structural diff by hash.

**Gaps.**
- Fixed chunking, no rolling-hash content-defined chunking. Fixed 256-byte and 64-byte chunks are alignment-fragile. LBFS (Muthitacharoen, SOSP 2001), IPFS UnixFS, restic, and borg survive insertions. Venti and ZFS share the weakness.
- CAS GC at scale is unbuilt. Single-CPU refcount plus tombstone on small graphs. Production CAS uses reachability bitmaps (Git) and deadlists (ZFS async_destroy). The recursive cascade unref will stall on large dead subtrees.
- No delta or pack compression. Whole-object storage. Re-root shares unchanged subtrees but stores the changed leaf whole.
- No succinct authenticated-structure proofs. The DAG is Merkle-shaped so it could serve inclusion/exclusion proofs, but it exposes none. Attestation re-hashes the whole graph.
- Mutation tension: re-root is O(depth) per write, unbatched. ZFS amortizes the same copy-on-write cost across a transaction group. CARMIX does not batch.
- No distributed content routing or discovery. IPFS has DHT provider records, Bitswap, graphsync. CARMIX migration is point-to-point pull.
- No hash agility. Hardwired to BLAKE3 with no stated migration path. IPFS multihash and Git's SHA-256 migration have this.

**Deep adaptations.**
- Authority-scoped content-defined chunking. Adopt LBFS/Rabin, partitioned per authority domain so a naive global port does not reopen the timing channel. The proposal that chunk boundaries also serve as re-mint bounds is asserted, not designed: content-defined boundaries have no intrinsic reason to align with capability sub-ranges, and forcing alignment trades away either dedup or granularity. Flag this tension rather than treat it as a feature.
- Vector-commitment attestation. Replace root-signature-plus-full-rehash with a succinct proof of an authority-critical fact. Native only in the weak sense that any Merkle proof commits to the bytes under it.
- ZFS-deadlist-style deferred cascade reclamation, keyed by content hash and domain. The claim that acyclicity makes deferred decrements trivially safe understates the shared-block problem: on a shared structure, deferred decrements still must avoid freeing a block a live root reaches, which is the exact problem ZFS birth-txg bookkeeping solves. "Structural hash-diff detects still-reachable subtrees" is the design sketch for that, not a dismissal of it.
- Delta-against-predecessor driven by the re-root diff. Store the changed leaf as predecessor-hash plus delta while identity stays the full content hash. This is Git's decouple-storage-from-identity, driven by the diff CARMIX already computes.
- DRCC as a distributed verification oracle (rematerialize and hash-compare a peer's claimed result). Bounded to the DRF spine and gated on DRCC validation.

**Boundary.** The advantages are consequences of putting content and authority in one name (a tree-model acyclicity result, intrinsic dedup in the C store, authority-coupled naming, the measured domain partition, the paper-level DRCC cut). The gaps are bulk-storage engineering the file/block CAS community has optimized for two decades. The two sets are largely orthogonal: the thesis lives in what the name means, the gaps in how bytes are stored and moved, so CDC, packing, deadlist GC, and succinct proofs can be adopted as storage-layer work provided each preserves two invariants, authority-in-the-hash and the domain partition. The one place the boundary is not clean is mutation tension: re-rooting is intrinsic to immutability-by-hash, so its O(depth)-per-write cost is a permanent structural cost shared with ZFS copy-on-write and can be amortized, never eliminated. Behavioral convergence is separately impossible by Rice's theorem.

### Domain 3: Live migration, checkpoint/restore, rematerialization

**Advantages.**
- Integrity by hash across the boundary. The destination re-hashes every transferred byte, so it needs zero trust in the source for state. This collapses distrusting migration to the authority residue. Real today. Confidential-VM migration (SEV-SNP, TDX) protects an opaque blob rather than letting the destination independently re-verify byte by byte.
- Delta migration is intrinsic. An unchanged subtree is detected by one hash equality and pruned. Pre-copy tracks dirty pages at page granularity and cannot prove a region unchanged without reading it.
- Anti-amplification is carried across the boundary by C plus signature, not by the proof. A migrant cannot arrive with more authority than the source held. This is tested cross-core and cross-machine, not machine-checked. No other migration or SSI system carries a capability-monotonicity bound across machines.
- Restore is the ordinary mechanism. Context switch, page fault, and migration are one rematerialization path in principle. Honest counterweight in Domain 3 itself: the U3 crossover benchmark shows activate-by-hash is slower than a register swap above a small dirty-chunk threshold, so this unification does not hold for fine-grained context switching. It holds at coarse boundaries. CRIU already checkpoints live state. "Content-addressed" is a representation choice, not the opposite of CRIU.
- Global sharing by construction, cold-start as restore-by-hash, and hash-verifiable reproduction for the DRF spine (the last gated on DRCC validation).

**Gaps.**
- Live external connections and side effects are outside the model. Rematerialization recovers internal state only. It cannot un-send a packet or follow a live TCP flow. Xen/KVM live migration and CRIU (TCP_REPAIR) solve this in production.
- No low-downtime story. The hash is a latency floor. Attestation adds tens of millions of cycles of Ed25519. Pre-copy and post-copy target sub-second downtime under a running writer. CARMIX does not.
- Migration is stop-and-transfer, not iterative pre-copy under a live writer and not post-copy demand paging across a network.
- Non-determinism reproduction is out of scope. DRCC is defined only for DRF execution. Racy state is re-materializable if retained but not reproducible by re-running.
- The distrusting-migration root of trust is emulated. The quoting key is the software AUTH keypair. This guts the security (not the protocol) of every distrusting-migration claim until a hardware root exists.
- The captured state is address space, registers, and capabilities only. A real process holds fds, sockets, mmaps, signals, timers, ptrace state, cgroups, namespaces. CRIU captures these. CARMIX has not demonstrated it.
- Retained history pins GC roots. No retention policy decided.
- Scale is two QEMU instances over one shared-memory region, not a network.

**Deep adaptations.**
- Post-copy demand paging becomes demand rematerialization: the resumable fault handler fetches each demanded page by hash and re-verifies it. A content-addressed page is re-verifiable from any replica, which breaks post-copy's single-source dependency.
- Catalyzer/REAP snapshot restore maps onto activate-by-hash with a Merkle DAG in place of a per-host copy-on-write image.
- Eidetic/Arnold lineage: adopt the goal, replace the replay engine with DAG navigation for deterministic internal edges only.
- CRaC / Lambda SnapStart coordination becomes a DRCC synchronization-point cut, making the ad-hoc "close your sockets before snapshot" a stated model boundary.
- Content-based dedup in VM migration is subsumed and passes through the gate with domain partitioning.

**Boundary.** CARMIX rematerialization leads checkpoint/restore and migration only on internal, deterministic, DRF, retained state. On that domain it delivers integrity by hash, delta transfer that proves unchanged subtrees unvisited, anti-amplification carried across the boundary (tested), and hash-verifiable reproduction for the DRF spine (gated on DRCC). It does not compete on the three problems production migration solves: live external connections, minimized wall-clock downtime under a writer, and rich external OS resource state. The gate follows from the thesis: a hash names internal content, so everything external is out of scope and the hash is a cost floor, not a downtime optimizer.

### Domain 4: Verifiable and auditable computation (PCC, seL4 refinement, CompCert, ZK/zkVM, SNARK/STARK, TEE attestation, transparency logs)

**Advantages.**
- Content-addressed code/proof binding. In PCC the consumer must trust the proof is about the code it runs. In CARMIX a proof is an immutable object whose content names the code/state hash it certifies, and the kernel re-hashes on install, so the binding collapses to hash equality. Only proof generation stays hard. Design property, not built.
- State hashes are free public I/O for a transition statement. A statement "H_post follows from H_pre under P" has its public inputs and outputs already named.
- DRCC scopes where a transition proof is well-posed (the DRF fragment). Paper-argued.
- Proof reuse by content address. A verified transition keys on (H_pre, P, H_post), and shared sub-states share the proof object. This amortizes but does not lower per-step prover cost.
- State integrity is free today, collapsing distrusting execution to a smaller residue.
- The object graph is Merkle-shaped, so a transition log is transparency-log-shaped. Design property.
- A culture of small re-checkable artifacts (coqchk re-verification, printed assumption set).

**Gaps.**
- No verifiable-computation machinery exists in the tree. No SNARK, STARK, or zkVM, no proof of any state transition. Everything here is designed, not built.
- The Coq proof is an abstract capability algebra, not a whole-system refinement. seL4 is strictly more rigorous on this axis. CARMIX's contribution is the content-addressed-migration framing and the tree-acyclicity result, not out-rigoring seL4.
- Proof-generation cost is untouched and is the field's central obstacle. zkVM proving runs orders of magnitude slower than native. Content-addressing makes the statement cheap and does nothing for the prover.
- BLAKE3 is arithmetization-hostile inside a circuit. The hash that makes the store fast is a concrete cost multiplier for any proof over CARMIX state.
- The attestation root is emulated. Environment genuineness and freshness cannot be attested today.
- No per-step history DAG is recorded, so there is nothing to audit yet.
- No CompCert-style source-to-binary preservation, so a proof about a source hash need not carry to the binary hash that rematerializes.

**Deep adaptations.**
- IVC/PCD (Valiant; Chiesa-Tromer; Nova) over the rematerialization DAG. Honest calibration: the improvement claimed (message-binding is the content hash) is on a minor axis. The cost driver is per-step prover cost, which stays external and untouched, so this is not near-term "at scale."
- Proof-carrying rematerialization replacing emulated attestation for the correctness residue. Retires "did A run honestly" with no hardware, leaving freshness and genuineness to a real root.
- A computation transparency log adapted from Certificate Transparency, Go checksum DB, Sigstore/Rekor. Honest carry-over: split-view, gossip, and log-completeness/freshness transfer unchanged. Those are the hard parts of a transparency log, so the residual delta (re-derivable leaves) is DRCC's contribution, already counted, not a new one.
- Content-addressed CompCert-style compilation certificates linking source-hash to binary-hash in the same DAG.

**Boundary.** A content hash attests exactly one thing, state identity, what the bytes are. It does not attest three orthogonal things. (1) Provenance, who authorized this, needs a signature (real Ed25519, first-key bootstrap assumed). (2) Transition correctness, that H_post was reached by honestly running P, needs a proof. A hash of a fabricated state is a valid hash, so this is the residue a hash structurally cannot fill and CARMIX has no mechanism for today. (3) Environment genuineness and freshness need a hardware root, which CARMIX emulates. Content-addressing gives integrity for free and collapses distrusting migration to residues (2) and (3). A proof can retire (2) with no hardware. A proof cannot retire (3), because a proof, like a hash, is timeless and replayable. That external-world, wall-clock boundary is the same one limiting DRCC, time-travel debugging, and the network model.

### Domain 5: Secure distributed and capability-native systems (CapTP / Cap'n Proto, BFT consensus, CRDTs, MPC, auditable ledgers)

**Advantages.**
- Zero-trust integrity is unilateral. A distrusting receiver re-verifies by re-hash with zero trust in the sender. Tested on the migration wire (leaf and node tamper rejection). CapTP and Cap'n Proto rely on vat integrity plus a transport. CARMIX moves integrity into the object, so it survives a hostile relay.
- A quantified anti-amplification ceiling as a structural invariant, enforced (in C, tested) at spawn, IPC, delegation, and across machines. No object-capability network carries this. The ceiling is the monotonicity lemma applied by C code, not a distributed theorem.
- Agreement on content collapses to a hash comparison. Equal hash means equal bytes, so the "same value" half of replication is free and only order remains.
- Content-addressing forces an acyclic immutable object graph for the frozen half, making distributed GC of that half tractable. Applies only to content-addressed references, not the live endpoint graph, which can still cycle.
- A distributed-authority model that is an alternative to global consensus for authenticity: content re-verification, bounded reachability on the local side, attestation for the authority residue.
- Binding commitments for free (with the standard caveat that a bare hash of a low-entropy value is guessable and hiding needs salting).
- State-based/delta CRDT merge maps onto rematerialization, with DRCC and convergence supplying the reproducibility CRDTs assert. Gated on DRCC.

**Gaps.**
- No ordering, no total order, no fork-choice. Content-addressing yields a set, not a sequence. Consensus establishes order and liveness. CARMIX has none. Double-spend is an ordering problem, so content-addressing kills tamper-evidence but not double-spend.
- No liveness, no operation over a real network. Everything distributed is over shared memory, single-CPU, cooperative, no real clock. Promise pipelining is prospective and content-addressing cannot name it.
- The anti-amp ceiling cannot be enforced on a non-CARMIX peer. On a real link only content integrity is unilaterally enforceable. This is a shared boundary with CapTP, not a CARMIX win.
- No confidentiality or information-flow control. Holding a capability means seeing the bytes.
- Multi-writer convergence is unbuilt (the shared-map frontier). No CRDT merge, no join-semilattice type, no anti-entropy, no causal delivery.
- No distributed GC across live references, no three-party handoff.
- No Sybil resistance or open-membership trust. The distrusting-party root is emulated.

**Deep adaptations.**
- Content-addressed delta-state CRDTs as rematerialization. Prefer delta-state over op-based because op-based needs causal ordered delivery CARMIX lacks.
- Capability-scoped CRDTs: gate merge-authority behind the anti-amp ceiling.
- A local-authority ledger: per-domain tamper-evident content-addressed history, cross-domain trust bounded by anti-amp plus attestation, consciously not solving total order.
- Hash-as-commitment bridge to MPC: binding-plus-hiding commitments only, not a secret-sharing substrate.

**Boundary.** Content-addressing is retrospective and order-free. For the frozen half it delivers integrity, dedup, agreement on content, binding commitment, tamper-evident history, and coordinator-free convergence via join-semilattice merge. Every remaining hard problem lives in the live, prospective, ordering half a hash cannot name: total order, liveness under partition, double-spend, promise pipelining, distributed cycle GC over live references, causal-delivery quiescence, input confidentiality. The capability axis reaches one step further, it can gate the live channel and bound reachability, but it cannot impose order and cannot be enforced on a non-CARMIX peer. The first-class negative: content-addressing solves the tamper problem and not the double-spend problem, because tamper-evidence is a property of a fixed value and double-spend is a property of which of two competing mutations happens first.

### Domain 6: Interaction, observability, HCI

Status: the interaction substrate primitives (content-addressing, capabilities, rematerialization) are built at the kernel layer. Every UI-facing model below is designed, not built, because no display, input, or window subsystem exists in the tree. The I/O path they would ride is exactly the external-effects boundary the thesis excludes and kernel/DBG_LOG.md enforces as real refusal branches.

**Advantages.**
- Undo and history as direct addressing. Every retained state is a hash, so "go back" is store-get plus re-hash-verify, bit-exact, at fetch cost independent of history depth (kernel/DEBUGGING_MODEL.md DBG-1). Contrast record-replay debuggers whose cost grows with distance.
- Zero-hidden-state notebooks at the execution layer. A cell output content-addressed over (code hash, input hashes) makes staleness a hash mismatch. Reproducibility of the DRF spine is gated on DRCC.
- Structural diff proportional to the change, not the state size (DBG-2), with exact last-common-node and first-differing-node queries (DBG-4).
- A content-addressed Powerbox with a kernel-enforced attenuation floor. The delta over Sandstorm/CapDesk (attenuation is a kernel invariant) is a property of having kernel capabilities, not of content-addressing. The snapshot-hash half is an orthogonal addition.
- Sharing and forking a session is sharing and forking a hash.
- Provenance-native display for the deterministic spine, halting honestly at the first non-deterministic edge (DBG-3).

**Gaps.**
- No display, input, window, or event subsystem exists. The entire HCI surface is on the far side of the external-effects boundary.
- The human is not content-addressable. Every user input is a non-deterministic external edge. An interactive system is defined by exactly the edges CARMIX cannot reproduce.
- Observational equivalence is undecidable. The strongest mechanical relation is structural equivalence, so the system can dedup structurally-equal states but cannot recognize two structurally-different states that render identically.
- Real-time interactivity is a liveness property invisible to the state DAG (DBG-5c refuses real-time queries).
- Retention cost is acute for interactive apps, which churn distinct state fast, at a storage floor of distinct state touched.
- Rendering is an external effect, so time-travel cannot un-show what the user already saw.

**Deep adaptations.**
- Content-addressed Powerbox (CapDesk/Sandstorm plus user-driven access control), with attenuation as a kernel invariant and transitive revocation. Adopt only because the cap model is already the substrate.
- Zero-hidden-state reactive notebook (Marimo/Pluto plus Nix), pushed under arbitrary processes, inheriting DRCC and convergence, both gated.
- Structural editor with content-addressed document and evaluation (Unison plus Hazel plus free-snapshot writes).
- Provenance-native value inspection (Whyline plus W3C PROV), distinguishing lineage-of-states from lineage-of-causes and refusing to fabricate the latter across external edges.

**Boundary.** The dividing line is the content-addressing membrane. Everything hashed (address space, registers, capabilities, namespace, cell outputs) is time-travelable, diffable, and shareable by name. Everything unhashed (the rendered frame, the gesture, wall-clock timing, perceptual sameness) is a non-deterministic external edge that rematerialization cannot reproduce or un-happen, and observational-equivalence undecidability forbids deciding perceptual sameness. CARMIX turns application state into a navigable, diffable, shareable, reproducible object, and it leaves the act of interacting with a human where every OS leaves it, outside the machine's model of itself.

---

## 2. Radical redesign and next-era design proposals

Each proposal is labeled buildable-now, needs-research, or open, with the prior art it adapts. Two standing corrections apply to all of them. First, "content-addressed X" is not automatically novel. The recurring justification "native because authority is folded into the hash" reduces to "the commitment covers the bytes it commits to," which is true of any commitment, so it is stated once here and not re-earned per proposal. Second, any proposal that leans on "the gate is correct" inherits the proof-versus-code gap, and any proposal that leans on DRCC inherits the unvalidated-premise gap.

### Track A: Core subsystem redesign

**A1. Brand-by-hash sealing (content-addressed otypes). needs-research.**
Replace the wholesale `SEALED_UNSUPPORTED` refusal with a sealing primitive whose otype is a content hash. Seal object O under brand B as ciphertext C = Enc_k(O) named by its hash, with a sealed descriptor recording referent and otype as hashes. Unseal needs a re-mintable cap to the brand key object bounded by the gate. Invoke rematerializes the brand's code object with sealed data attached read-only in a fresh authority domain. This resolves the `otype_class` reconciliation marked unsolved in kernel/cap/cvsasx_pir.h, because two machines agree on otype identity with no shared namespace when the otype is a hash. Adapts CHERI otypes plus CInvoke and KeyKOS opaque keys. Boundary: needs a symmetric-crypto TCB the design deliberately lacks. A hash brands the seal but a hash is not a secret, so use-opacity arrives only with encryption.

**A2. Label-in-the-hash monotone flow check. needs-research.**
Fold an information-flow lattice label into every object's hashed content and add a second gate check structurally identical to the anti-amp definition, admitting a rematerialization edge only when the destination clearance dominates the object label, with a derived object's label being the lattice join of its inputs. Honesty correction, load-bearing: this delivers a monotone-label lemma with the same standing as `anti_amplification`, namely that one gate function by its definition emits a dominating label. It is not the *-property and not noninterference. seL4's result (Murray et al. 2013) is whole-system intransitive noninterference over the entire kernel's execution, and its difficulty is proving the absence of implicit and covert flow everywhere, not that one join is monotone. The earlier framing "content-addressed analogue of seL4's *-property" trades on seL4's reputation and is dropped. The DS3 partition closes one channel, dedup timing, and does not close arithmetic, implicit, or termination channels, which dominate real IFC. Adapts Denning's lattice model and OS-level DIFC (HiStar/Flume). Boundary: bounds flow direction, not covert timing or termination.

**A3. Revocation by epoch accumulator. buildable-now.**
kernel/cap/cap_custodian.c has an explicit note that a re-minted cap is not tracked for later sweeping revocation. Make the live-referent set a content-addressed authenticated (Merkle) accumulator whose root already rides the Layer-2 signed record. A cap is live iff its referent hash is a member of the current epoch root. Revoke by publishing a new accumulator excluding the hash, so stale caps fail on their next rematerialization. Adapts cryptographic revocation accumulators and RevocationTransparency, with seL4 CDT revoke and CHERI Cornucopia sweeps as cost baselines. Honesty correction: this is eventual revocation, not prompt temporal safety. Latency is bounded below by rematerialization cadence, and the U3 crossover forbids re-hashing on every switch, so a long-resident cap has no upper bound on revocation delay. CHERI Cornucopia sweeps deliver prompt temporal safety that this scheme does not. State the comparison as prompt versus eventual, not as a strict win.

**A4. Authority-scoped content-defined chunking. buildable-now (chunking) / open (bound alignment).**
Replace fixed chunks with LBFS/Rabin boundaries, partitioned per authority domain so a naive global port does not reopen the DS3 timing channel. Adapts LBFS, IPFS UnixFS, restic, borg. Honesty correction: the claim that each CDC boundary doubles as a re-mint bound is an unexamined tension, not a feature. Content-defined boundaries have no reason to align with capability sub-ranges, and forcing alignment defeats either the dedup or the granularity. The chunking is buildable now, the bound alignment is open.

**A5. Merkle property witnesses for distrusting migration. buildable-now (membership) / needs-research (aggregate).**
Emit the O(log n) inclusion/exclusion proofs the Merkle store could serve. A destination verifies a specific authority fact against the signed root without pulling the subtree, for example non-membership in the revocation accumulator or membership of a cap set in a subtree witnessed to hold no write-plus-execute cap (the predicate `valid_dest_no_wx` proves). Adapts Certificate Transparency inclusion/consistency proofs. Membership and non-membership are pure hash paths and buildable now. A succinct proof that a whole subtree is clean is the research frontier and collides with BLAKE3 being arithmetization-hostile.

**A6. Domain-scoped deadlist GC at scale. buildable-now.**
Adapt ZFS async_destroy so dropping a large root records its hash on a per-domain deadlist reclaimed incrementally, replacing the synchronous recursive cascade unref. Honesty correction: acyclicity removes cycle collection but does not remove the shared-block problem. Deferred decrements must still avoid freeing a block a live root reaches, which is the exact problem ZFS birth-txg bookkeeping solves. The design carries that problem as structural-hash-diff reachability detection, it does not dismiss it. Domain tags scope the deadlist to respect DS3.

**A7. Verifiable rematerialization. buildable-now (re-execute-and-compare) / needs-research (succinct proof).**
Ship evidence that H_post was honestly reached from H_pre under P, retiring the correctness residue with no hardware. Buildable-now slice: a destination holding the inputs rematerializes and hash-compares, which is Nix rebuild-and-compare for live execution state, gated on DRCC. Research frontier: a succinct zkVM/IVC proof so the destination need not re-execute, colliding with BLAKE3 in-circuit cost and with per-step prover cost, which is external and untouched. Adapts PCC for the binding and IVC/PCD for the DAG recursion.

**A8. Multi-source distrusting live composition. needs-research.**
Assemble one live process from content-addressed sub-graphs authored and signed by mutually distrusting domains, each re-verified by hash, each carrying its own anti-amp ceiling and (with A2) its own flow label. Composite authority is the anti-amp meet, composite label is the lattice join. Lifts the two-owner shared-frame case to a distrusting-assembled execution graph. Adapts CapTP three-party handoff and DStar/HiStar distributed DIFC. Inherits the double-spend negative: composition orders no concurrent mutations, so live multi-writer coherence stays the frontier.

### Track B: Next-era foundations

**B1. Proof-Carrying Rematerialization. needs-research.**
Fold an IVC/folding proof, alongside the authority ceiling, that H_post honestly followed from H_pre under P. State hashes are the proof's public I/O for free, DRCC makes each node's statement well-posed on the DRF spine (gated), and dedup shares node proofs. Improves on generic PCD on the message-binding axis. Honesty correction: message-binding is minor. The cost driver is per-step prover cost, which stays external, so this is not near-term at scale.

**B2. Dual-commitment bridge. needs-research.**
For any object entering a proof, mint a SNARK-friendly companion commitment (Poseidon/Rescue-style) as a content-addressed sibling and prove once that BLAKE3 and the companion commit to the same bytes. Amortizes the BLAKE3-in-circuit cost across every transition that touches the object. Honesty correction: amortizes, does not eliminate. A one-time BLAKE3-in-circuit equality proof per object remains, and per-step prover cost is untouched.

**B3. Multi-core DRCC release-cut capture. buildable-now.**
A hash-barrier with no global stop. On a capture request each core, at its next release-consistency synchronization point (built IPI path plus versioned MVCC gate), publishes the hash of its local task state into a per-core slot, and the cut is the tuple of per-core hashes plus the shared-heap root. Validate by rematerialize-and-compare across N runs. Adapts Chandy-Lamport/Mattern consistent snapshots plus Adve-Hill release consistency. This is the buildable step that would move DRCC from paper-argued to validated for the DRF spine. Racy state stays undefined by construction. Until exhibited it is theory.

**B4. Computation transparency log. buildable-now.**
An append-only Merkle log of (H_pre, P, H_post, proof) transition tuples over the acyclic store, so an auditor checks hashes plus log consistency and re-derives DRF-spine entries. Adapts Certificate Transparency, Go checksum DB, Sigstore/Rekor. Honesty correction: split-view, gossip, and log-completeness/freshness transfer unchanged, and those are the hard parts of a transparency log. The residual delta (re-derivable leaves) is DRCC's, already counted, not a second contribution.

**B5. Content-addressed model+data+run triple. needs-research.**
Name an ML run by a hash over (weights, dataset, code/hyperparams, seed). Data-use authority becomes an anti-amp-bounded capability so a fine-tune cannot exceed its base's data-authority, the delegation chain is the lineage, and revocation-by-derivation retracts a poisoned base and every descendant. Adapts Nix/Guix derivations and seL4 CDT revoke. Honest scope: GPU float non-determinism is outside the DRF spine, so the claim is content-addressed provenance, authority, and transitive revocation, not bit-exact reproduction of a GPU run. No accelerator in-tree.

**B6. Attestable inference. needs-research (proof) / buildable-now (log floor).**
An inference response ships (input, model, output, proof) so a client verifies the output came from the claimed model on the claimed input. The buildable floor is the emulated-attestation path plus a transparency-log entry. The ZK-over-forward-pass version is at the cost frontier. Adapts zkML. Honest: the emulated root guts the security of the floor to a simulation against no real adversary.

**B7. Content-addressed Powerbox. buildable-now (substrate).**
A designating gesture returns a pair (immutable snapshot hash, freshly minted anti-amp-attenuated capability). Attenuation is a kernel invariant, revocation reaches descendants. Adapts Powerbox/CapDesk/Sandstorm and user-driven access control. The gesture and rendered frame are external edges outside the model.

**B8. Omniscient state navigator / reactive notebook. needs-research.**
Undo as store-get plus re-hash-verify, "what changed" as hash-pruned structural diff, merge as structural diff, cell outputs content-addressed over inputs. Pushes Marimo/Pluto/Nix reproducibility under arbitrary processes, gated on DRCC and convergence. Needs a display and input subsystem that does not exist, retention pins GC roots, perceptual sameness is undecidable.

**B9. Local-authority ledger. needs-research.**
Per-domain tamper-evident content-addressed history, cross-domain trust bounded by anti-amp plus attestation, shipped with its own negative: solves tamper, not double-spend. Adapts Git/IPFS DAGs and CT-style audit, minus consensus and Sybil resistance.

**B10. Capability-scoped delta-state CRDTs. needs-research.**
Content-address each CvRDT state, the join becomes a rematerialization step deduped via the store, and DRCC plus convergence give strong eventual consistency as "same delivered delta-set means same canonical hash" (gated). Gate merge-authority behind the anti-amp ceiling. Adapts Shapiro et al. and Almeida et al. Open residue: causal stability and quiescence (knowing when all deltas arrived) is a liveness property CARMIX lacks.

**B11. Vector-commitment attestation. needs-research.**
A vector-commitment (verkle/KZG/IPA) root committing to the folded authority metadata, so a destination verifies a bounded safety property succinctly. Flag: verkle/KZG/IPA are a moving research target, and the property must be expressible over the committed structure.

---

## 3. Irreducible boundaries as first-class results

These are stated as results, not as omissions. For each: the construct the thesis cannot express, why it is irreducible, and whether a partial escape exists. One caution, from the critic: the "membrane" framing below is a genuine organizing principle and also a rhetorical risk. It can make inability read as elegance. The honest reading is that a system which cannot do hard real-time, cannot do raw multi-writer coherence, cannot bind an external party's authority, emulates its root of trust, and has not validated its central reproducibility premise has real limits, some permanent and some merely current. The membrane explains which is which. It does not convert a current gap into a virtue.

**3.1 Real-time and liveness. Irreducible (double boundary).** A hash is a function of bytes that already exist, so it cannot name a temporal predicate (now, eventually, within a deadline). And the hash is a latency floor: the U3 crossover shows activate-by-hash exceeds a register swap above a small dirty-chunk threshold, so content-addressed activation is viable only at coarse boundaries. Hard real-time is excluded on both counts. Partial escape: attested-tick annotation makes a captured tick (a value) content-addressable and supports deadline checking against a real timer. It cannot supply liveness prediction (undecidable) or fine-grained content-addressed preemption (crossover cost).

**3.2 Mutable shared state. Irreducible for the raw case.** Content-addressing names immutable values, so a write is a new name and two independent writers diverge into two hashes with no canonical merge. Delivered scope is single-writer-shared and quiescent, and re-rooting is O(depth) per write, amortizable, not eliminable. Partial escape: restrict shared mutable state to a join-semilattice (delta-state CRDT) whose merge is a rematerialization step. Residue: raw multi-writer coherence stays irreducible, and quiescence detection is a liveness property CARMIX lacks, so the escape depends on 3.1.

**3.3 External non-CARMIX party. Irreducible asymmetry.** Anti-amplification is enforced by the kernel at its own crossings. A foreign party never re-mints, so the ceiling binds only the local side. Content integrity is unilaterally enforceable (re-hash any received bytes, zero trust in the sender). Authority on the peer's side is not, because the peer sends what it wants. The inexpressible construct is an authority bound on the conduct of a party that does not run the gate. Discipline that follows: downgrade every external party to a data source, re-hash everything, and treat any presented capability as a request the local gate re-mints from local grants.

**3.4 Emulated versus hardware root of trust. Irreducible for the physical half.** Content-addressing attests state identity only. Two facts remain that neither a hash nor a proof can supply, because both are timeless and replayable: environment genuineness and freshness. Freshness reduces to genuineness (a verifier nonce needs a genuine root to sign the response), and genuineness is irreducibly physical, so only a hardware root anchors it, with its own side-channel and TCB-recovery caveats. The current build emulates the root (the quoting key is the software AUTH keypair), which demonstrates the protocol and not the security. This corollary of 3.1 (freshness is temporal) is load-bearing: it guts the security of every distrusting-migration and attestable-inference claim down to a simulation against no real adversary until a real anchor exists.

**3.5 Observational-equivalence undecidability. Irreducible.** The strongest sameness a content-addressed system can decide is structural equivalence. Behavioral equivalence (two structurally-different states that behave, render, or compute the same) is undecidable by Rice's theorem. So the system can dedup, diff, and merge structurally-equal states and can never recognize behaviorally-equal but structurally-different states. Honest caveats: the structural canonical form is unbuilt, and its tractability is in-principle only (polynomial for bounded-valence graphs by Luks, but CARMIX object arity is unmeasured, so even the polynomial claim is not established for CARMIX graphs).

**3.6 The unifying membrane.** All five reduce to one line: the membrane between the frozen, internal, deterministic half (nameable by hash, hence integrity-checkable, dedupable, diffable, provable, time-travelable, anti-amp-bounded) and the live, external, temporal, behavioral half (the peer's conduct, not-yet-existing bytes, wall-clock now, physical genuineness, behavioral sameness, none nameable). The capability axis reaches exactly one controlled step past content-addressing, it can gate the live channel and bound reachability, but it cannot impose order, name time, or bind an ungated party. Usage: for any proposed feature, decide which side of the membrane it lives on before building, and if it is on the live side, the honest deliverable is the boundary and its partial escape.

**3.7 Proof-versus-code standing (a boundary on the claims themselves).** All results are proven-model plus tested-code. The Coq proof is a seven-permission `re_mint` monotonicity lemma plus finite-tree acyclicity under one injectivity hypothesis. The C rejects out-of-bounds where the model clamps, and correspondence rests on six runtime attack tables, not an seL4-style refinement down to the binary. Every proposal that leans on "the gate is correct" inherits this until a refinement or a content-addressed source-to-binary certificate exists.

**3.8 Prover cost and BLAKE3 arithmetization-hostility (external, unsolved).** Content-addressing makes the verifiable-computation statement cheap and does nothing for per-step prover cost, which runs orders of magnitude slower than native, and the store's own hash is a cost multiplier in-circuit. The dual-commitment bridge amortizes but leaves a one-time in-circuit equality proof per object. Verifiable execution at scale remains gated on an external cost problem the thesis does not address.

---

## 4. Tiered synthesis roadmap

### Tier 1: Buildable now (mechanism engineering, no new theory required)

- Revocation by epoch accumulator (kernel/cap/cap_custodian.c revocation-not-tracked note). Unifies GC with revocation. Boundary: eventual, not prompt.
- Authority-scoped content-defined chunking (chunking now, bound alignment deferred to research).
- Domain-scoped deadlist GC at scale, carrying the shared-block reachability problem explicitly.
- Merkle property witnesses for membership and non-membership facts.
- Multi-core DRCC release-cut capture on the built SMP. This is the single highest-value near-term item, because it is the step that would move DRCC from paper-argued to validated for the DRF spine, on which roughly a third of the forward roadmap rests.
- Computation transparency log (leaves re-derivable, split-view/gossip/freshness unchanged from CT).
- Content-addressed Powerbox capability substrate.
- Verifiable rematerialization, re-execute-and-compare slice (gated on DRCC validation).
- Attested-tick annotation.
- Unilateral-integrity external-party discipline.

### Tier 2: Needs research (extends the model or fuses formal methods)

- Brand-by-hash sealing, resolving `otype_class` (kernel/cap/cvsasx_pir.h), needs a symmetric-crypto TCB.
- Label-in-the-hash monotone flow check. Delivers a monotone-label lemma with the standing of `anti_amplification`, not the *-property and not noninterference.
- Proof-Carrying Rematerialization and the dual-commitment bridge, both gated on external prover cost.
- Vector-commitment attestation (moving research target).
- Content-addressed ML lineage and attestable inference (provenance and revocation, not bit-exact reproduction).
- Multi-source distrusting composition (inherits the double-spend negative).
- Capability-scoped delta-state CRDTs (open residue: quiescence detection).
- Omniscient state navigator and reactive notebook (needs a display and input subsystem).
- Structural convergence canonical form (unbuilt, tractability in-principle only).
- seL4-style refinement of kernel/cap/swcap.c, or a content-addressed source-to-binary certificate. This is the item that would retire boundary 3.7 and raise the standing of everything that leans on "the gate is correct."

### Tier 3: Defensible contributions, stated at their true standing

These are the claims worth publishing, each trimmed to what the repository supports.

- Authority as a rebuildable bound over content-addressed state rather than a transferable token. The wire carries a bound, not authority. Defensible novelty is the content-address binding plus the machine-checked ceiling, with SPKI/SDSI acknowledged as closer prior art than "no system does this."
- Content-addressing of live execution state, with restore as the ordinary mechanism at coarse boundaries. Honest counterweight: the U3 crossover refutes the unification for fine-grained context switching, and CRIU already checkpoints live state, so this is a representation choice with real consequences, not the opposite of CRIU.
- A machine-checked anti-amplification monotonicity lemma over a seven-permission model with a small explicit assumption budget, applied by C code (tested) across spawn, IPC, delegation, cross-core, and the migration wire. The cross-machine part is tested, not machine-checked.
- Finite-tree acyclicity under one injectivity hypothesis, giving cycle-collector-free refcounting for the model. The operational DAG-acyclicity claim (a hash cannot name a not-yet-existing object) is asserted in prose, the same operational assumption Git/IPFS/Nix make.
- Authority-coupled naming, the discipline of putting authority in the hashed bytes so differing-authority states cannot alias. The cryptographic content is ordinary (a commitment covers its bytes). The contribution is the discipline and its integration with the capability model.
- Domain-partitioned dedup that closes the cross-user dedup timing channel, measured in cycles. This implements the mitigation the Harnik et al. paper itself recommends, and the contribution is the measurement and integration, not the discovery.
- DRCC and content-addressed convergence as stated boundaries for reproducibility of live execution, with observational equivalence proven impossible by Rice. DRCC is paper-argued and unvalidated on SMP, so it is a stated cut, not a delivered theorem.
- Sweep-free eventual revocation piggybacked on the re-hash the design already performs, contrasted honestly with CHERI's prompt sweep-based temporal safety.
- The residue-shrinking placement of what a hardware root is irreducibly for: content-addressing retires state identity for free, a proof can retire transition correctness with no hardware, and only freshness and genuineness need hardware. The current emulated root demonstrates the protocol, not the security.
- Content-addressing as a consensus-free authenticity model that solves tamper and explicitly not double-spend.

### Summary

The roadmap follows from one sentence: make the name carry semantics. Putting content and authority in one identity buys, as consequences, a tree-model acyclicity result, intrinsic dedup in the C store, authority-coupled naming, a measured closure of the cross-user dedup timing channel, and the paper-level DRCC and convergence cuts. The authority-as-bound model and the rematerialization of live execution state are genuinely orthogonal to prior file/block CAS and to token-based capability systems. The near-term work is mechanism engineering the thesis makes tractable, with multi-core DRCC capture and an seL4-style refinement of the C gate as the two items that would most raise the standing of everything else. The research frontier fuses formal methods with the thesis without pretending the fusion is free of external cost. And every contribution is bounded by one membrane and by the honest reading that the machine-checked core is a small monotonicity-and-acyclicity result, the surrounding system is designed and tested against attack tables, and the words proven, machine-checked, native, and new have been restricted throughout to what kernel/proofs/Carmix.v actually contains.
