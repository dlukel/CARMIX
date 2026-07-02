# FOUNDATIONS RESEARCH  -  F1 to F4 plus the unified claim (F5)

Scope. Four fronts on top of the committed spine. Each front returns a nearest-neighbor
table (5 to 8 works, one line what-it-does plus one line delta), findings, and exactly one
verdict of BUILDABLE-NOW, BUILDABLE-GATED, RESEARCH-ONLY, or KILL. F2 carries the
triangle-inequality result proved-or-refuted. F5 states the unified claim, one honest delta
sentence per neighbor across the whole program, and the kill-list. Every bibliography entry is
UNVERIFIED (recall, not fetch). Settled verdicts (Armknecht partitioning, the network hybrid,
CV6 undecidability) are restated as boundaries and never reopened.

Honesty pass applied. F1 headline is stated conditionally (the identity holds once the typed
derivation edge is built, not today). F2 drops the hollow reason slot and states the metric as
proved on paper and Coq-tractable, not yet machine-checked. F2 neighbor entries are each
UNVERIFIED-tagged to match the other three fronts. No hype framing anywhere.

---

## F1 THE PROVENANCE CALCULUS

Claim under test. A per-store-put derivation record (parent hashes plus producing rule or
admitted-computation flag plus declared entropy-leaf hashes) turns every reachable state into a
checkable derivation DAG rooted at named sources, with flow queries as DAG reachability and
authority-never-amplifies doubling as provenance-monotonicity.

### F0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta |
|---|---|---|
| Green, Karvounarakis, Tannen, provenance semirings (PODS 2007) | Annotate relational query outputs with polynomials over source variables. Plus and times are union and join, homomorphisms give lineage, why, how. | CARMIX has no relational algebra. Over opaque admitted computations the polynomial degenerates to Boolean why/lineage, how-provenance is unrecoverable, and the only real plus is dedup (two derivations collapsing to one hash). |
| W3C PROV-DM (W3C Rec 2013, already synth:189) | Standard provenance data model: entity, activity, agent, wasDerivedFrom, used. | CARMIX edges are BLAKE3-named and re-verifiable, not asserted metadata. PROV supplies only the vocabulary. |
| PASS provenance-aware storage (Muniswamy-Reddy et al., USENIX ATC 2006) | OS captures file ancestry by intercepting syscalls at the storage layer. | F1 records at the single content-addressed put chokepoint with tamper-evidence, complete-by-construction for internal edges, but inherits PASS's undecidable-completeness gap for edges hidden inside opaque computations. |
| HiStar/DStar plus Flume DIFC (Zeldovich OSDI 2006 / NSDI 2008, Krohn SOSP 2007) | Kernel or user labels enforce information flow at runtime (no read-high-write-low). | HiStar prevents flow via runtime labels. F1 records and queries flow as offline DAG reachability over immutable objects. Audit not enforcement, a tamper-evident history DIFC lacks, but no runtime read/write mediation. |
| Nix/Guix content-addressed derivations plus in-toto/SLSA (recalled USENIX Sec 2019) | Content-addressed derivation names inputs and builder, output hash. in-toto/SLSA sign supply-chain steps. | Closest prior art. F1 extends derivation-names-inputs from batch builds to per-put live execution state and couples the edge to the capability ceiling. Nix and in-toto have no authority model. |
| Dynamic taint tracking, TaintDroid/DTA (recalled OSDI 2010) | Propagate taint through execution to catch data and implicit flows. | Taint sees inside the computation. F1 reachability sees only declared parent edges, so it misses exactly the implicit and data flows inside an admitted computation that taint exists to catch. |
| seL4 intransitive noninterference (Murray et al., IEEE S&P 2013) | Machine-checked whole-system noninterference including covert-channel treatment. | F1-b is reachability noninterference in the derivation model only. seL4's hard part (implicit and covert flow everywhere) is precisely what F1 does not cover, the same gap the synthesis books at A2 and section 3. |
| Eidetic/Arnold plus rr record-replay (Devecsery et al. OSDI 2014, Mozilla rr) | Record nondeterministic inputs so execution replays deterministically. | F1-d captures each nondeterministic input as a content-addressed un-parented external-tagged leaf feeding DRCC-conditional replay. Draw-timing stays a real off-DAG channel (D3/D4). |

Novelty outcome: PARTIALLY-COVERED.

### Findings

Grounding. store_put records only (hash, offset, len) at kernel/object_store.c:67-71, with no
parents, no rule, no entropy. The store is opaque bytes keyed by BLAKE3
(kernel/object_store.c:33,43,50). The synthesis books "No per-step history DAG is recorded, so
there is nothing to audit yet" (kernel/ACADEMIC_SYNTHESIS.md:126) and names the confinement/IFC
gap as "the single largest distance from the state of the art" (:36). kernel/proofs/CarmixDag.v
already proves acyclicity of the shared content-addressed DAG under H_WF (hash-before-name), the
well-founded order any reachability induction rides on. Adjacent pieces are already scoped: A2
label-in-hash monotone flow (needs-research, explicitly not the star-property and not
noninterference, :205), B4 computation transparency log (buildable-now, :237), B5
model-plus-data-plus-run triple with seed-in-the-hash (:240), and provenance-native inspection
via Whyline and PROV (:189). So F1 is a fusion of already-named repo items, not a new primitive.

F1-a the algebra (semiring provenance), largely covered and degenerate. The Green
Karvounarakis Tannen power comes from the algebraic structure of relational operators. Plus is
union (alternative derivation), times is join (joint use), and homomorphisms specialize N[X] down
to why-provenance, lineage/Boolean, Trio probability, and tropical min-cost. CARMIX has no
relational algebra. Its operations are admitted computations, opaque functions f(parents) to
bytes. For an opaque f, provenance collapses to the coarsest semiring, Boolean why/lineage
(which parents were used), the black-box case. What the DAG buys operationally: which-provenance
(transitive source set) and why-provenance (minimal parent/witness sets) are both free
reverse-reachability queries. What it does not buy: how-provenance and the full polynomial
(derivation multiplicity, used twice) requires transparent operators the admitted-computation
model hides by definition. Claiming it would be laundering opaque f into fake algebraic
structure. One genuine exception where plus is meaningful: the store's dedup event is exactly
"two alternative derivations reaching identical bytes collapse to one hash", which is precisely
the provenance plus. So the semiring framing is real but degenerate. It buys Boolean lineage plus
the dedup-as-plus slice. The polynomial richness is killed for the general opaque case.

F1-b the flow theorem shape (noninterference as reachability). Model: DAG D with nodes equal to
content hashes, edge parent to child from each object's recorded parent-hash list, A-sources
equal to leaf nodes authored or tagged by domain A. Statement (in the model): if no directed
path in D from any A-source to S, then S's bytes are a function of non-A sources only, so varying
A's inputs with all non-A sources fixed cannot change S. Proof shape: well-founded induction on D
(acyclicity from kernel/proofs/CarmixDag.v), each node equals f(recorded parents), S
unreachable-from-A implies every transitive parent of S is non-A implies S equals g(non-A
leaves), so changing A-leaves leaves S fixed. Standing: a monotone reachability lemma, the same
class as anti_amplification and the A2 monotone-label lemma. Cheap, proof-directive, rides on the
already-proven acyclicity.

What F1-b does not cover, permanent. (1) Soundness equals completeness of parent-declaration. If
an admitted f reads an A-source but the record omits it, the theorem is vacuously wrong.
Deciding whether a declared parent set is complete equals detecting all data dependencies of an
arbitrary computation, which is undecidable (A2 class). So it is noninterference relative to a
declared, unverifiable parent set. (2) Timing and covert channels live off the DAG. The dedup
hit/miss timing channel (kernel/DEDUP_SCOPING_LOG, Armknecht cross-domain bound) is the standing
example. A learns about S via store-timing with no derivation edge. The DAG has no edge, the flow
is real, it flows through shared physical presence plus wall-clock, not lineage. "No path implies
independent" is therefore true only in the model that excludes timing. On the real machine it is
false, and this is permanent, the exact settled Armknecht/dedup verdict (not relitigated, cited
as the boundary). (3) Implicit flows inside an admitted f (control-dependent writes) are
invisible to reachability, which is exactly what dynamic taint exists to catch and what the DAG
cannot. Net: the same honest ceiling the synthesis already draws at A2 (bounds flow direction,
not covert timing or termination) and at section 3.6 (the membrane names the frozen half, the
timing/behavioral half is off it).

F1-c fusion adjudication (confirm/kill). Candidate delta: content-addressed tamper-evident
derivation certificates unified with capability ceilings in one store, flow queries as DAG
reachability. Piece by piece: the record format is Nix/Guix content-addressed derivations plus
in-toto/SLSA plus W3C PROV (covered). Flow-as-a-graph is DIFC (HiStar/Flume) plus taint plus
DB/workflow provenance (covered). The transparency log of (H_pre, P, H_post) transitions is
B4/Certificate-Transparency (covered, already counted). The semiring richness is
covered-and-degenerate (F1-a). The one surviving novel-as-fusion sentence, stated conditionally
because the edge is not built yet: once a typed derivation edge is recorded per put, CARMIX would
be the store where the provenance edge and the capability-derivation edge are the same edge, so a
child's derivation record is at once its provenance certificate (parent hashes plus rule) and its
authority certificate (re-mint from re-verified parent content), and the already-proven
anti_amplification (kernel/proofs/Carmix.v) would be at once authority-monotonicity and
provenance-monotonicity, with flow queries reducing to reachability over the very DAG the
GC/dedup/acyclicity already maintain. No single neighbor has this. Nix and in-toto have
content-addressed derivations but no authority model. HiStar has capability-flavored labels plus
runtime flow but no content-addressed re-verifiable derivation history. PROV and PASS record
ancestry but neither content-bind nor gate it. That identity (authority-DAG equals
provenance-DAG) is the confirmed delta once built. Everything else in the candidate sentence is
borrowed. Because the algebra claim is killed and the flow-theorem standing is already booked at
A2, the per-front outcome is PARTIALLY-COVERED, not first-of-its-kind.

Honesty note on tense. Today kernel/proofs/Carmix.v proves authority-monotonicity only. It says
nothing about provenance because no provenance edge is in the proof or in the store. The identity
above is a fusion-to-be-built, correctly booked BUILDABLE-GATED below, and the flow lemma is
proof-directive, not proven.

F1-d entropy honesty (D4, declared, never laundered). Randomness, timers, device, and net inputs
enter as sources. Each nondeterministic read is captured, at the moment of read, as a
content-addressed un-parented leaf (a DAG root) tagged external/nondeterministic, and any object
consuming it lists that leaf's hash as a parent. This generalizes the synthesis attested-tick
pattern (section 3.1 partial escape, a captured tick is a value hence content-addressable) and
the B5 seed-in-the-hash. Prior art: rr and Eidetic/Arnold record-replay (record the
nondeterminism, replay is deterministic). The non-laundering rule: the leaf is a root with no
computed in-edges and an explicit external tag, and the DAG never claims to derive it.
Determinism and replay are conditional on the recorded leaves (this is the A4
FROZEN-REPRODUCIBLE vs FROZEN-RETAINED-ONLY line, DRCC-adjacent, hence gated). The failure mode
to reject: hashing the RNG output and presenting the state as derived from nothing, which is
exactly what D4 forbids. Boundary (D3): capturing the leaf makes replay reproducible but does not
make the future draw predictable, and the latency/timing of the draw is not in the DAG, a real
off-DAG channel consistent with F1-b(2).

### Metric/axiom result

Flow lemma (in-model, proof-directive, cheap on kernel/proofs/CarmixDag.v): for derivation DAG D
and domain-A source set, if no directed path from any A-source to node S, then S's content is
invariant under changes to A's inputs holding non-A sources fixed. Proof: well-founded induction
on D (acyclicity already Qed'd in CarmixDag.v), each node equals f(recorded parents). Scope
(permanent, per D3 and the settled Armknecht/dedup verdict): sound only relative to a declared
parent set whose completeness is undecidable (A2 class). Does not cover the dedup hit/miss timing
channel (real flow with zero DAG edge), draw-timing of entropy leaves, or implicit flows inside
an admitted computation (taint-visible, reachability-invisible). Standing equals
anti_amplification-class monotone lemma, not seL4-class noninterference. Semiring result: over
opaque admitted computations N[X] degenerates to the Boolean/why-lineage homomorphism, and the
only algebraically meaningful plus is the store's dedup event (two derivations of identical bytes
collapsing to one hash).

### Verdict: BUILDABLE-GATED

The record-and-query mechanism is a mechanism-engineering slice with no new theory: a typed
derivation record appended per put and a reverse-reachability walk. It is gated, not free, on two
named items. Gate 1 (engineering): the store is opaque bytes recording only (hash, offset, len)
at kernel/object_store.c:67-71. A derivation record needs typed objects with a parent-hash plus
rule plus entropy-leaf layout the store lacks, the identical typed-child-layout step R1 already
priced as a real modest engineering plus spec step, not free. Gate 2 (permanent, on any security
reading): the F1-b noninterference claim is proof-directive (a cheap monotone reachability lemma
on the acyclic DAG, same standing as anti_amplification) but its security meaning is bounded by
(a) undecidable completeness of the declared parent set (A2 class, you cannot decide a
computation declared all its dependencies) and (b) timing/covert/implicit channels that live off
the DAG (dedup/Armknecht, settled, permanent, plus taint-visible implicit flows). So the calculus
records and queries declared derivation now (gated on typed records) and does not deliver
noninterference-as-a-guarantee (research-only, proof-directive). The semiring how-provenance
richness is killed for opaque admitted computations, surviving only as Boolean lineage plus
dedup-as-plus.

Build subset. Derivation record per store put equals {parent hashes, rule-id or
admitted-computation flag, declared-entropy leaf hashes}, appended in a typed object. Flow query
equals reverse-reachability over that DAG. Nondeterministic inputs captured as un-parented
external-tagged content-addressed leaves. It rides on the BLAKE3 already computed at put
(kernel/object_store.c:50) and the acyclic DAG the GC/dedup/CarmixDag.v already maintain. It is
exactly B4 transparency-log (buildable-now) plus B5 lineage/seed plus A2 label, fused. The safe
subset explicitly excludes any enforcement (this is audit, not prevention), any soundness beyond
declared edges, and any timing/covert-channel closure.

Borrowed vs new (from CARMIX itself). Borrowed: CarmixDag.v acyclicity (the induction skeleton),
A2 label-in-hash (the second gate check), B4 transparency log (the record substrate), B5
seed-in-hash (entropy leaf), DEDUP_SCOPING_LOG/Armknecht (the permanent off-DAG channel). New
(once built): the identity provenance-edge equals authority-edge in one content-addressed DAG so
anti_amplification is simultaneously provenance-monotonicity.

### Bibliography (all UNVERIFIED, recall not fetch, borrowed vs new)

- Green, Karvounarakis, Tannen, "Provenance Semirings" (recalled PODS 2007). UNVERIFIED.
  Borrowed: the semiring/polynomial framework. New: nothing usable, degenerates to Boolean
  lineage over opaque admitted computations (F1-a).
- Buneman/Cheney/Tan why-and-where provenance, Trio/ULDB (Widom, recalled CIDR 2005). UNVERIFIED.
  Borrowed: why/lineage query shapes. New: computed by DAG reachability rather than query
  rewriting.
- W3C PROV-DM (recalled W3C Rec 2013). UNVERIFIED. Borrowed: entity/activity/wasDerivedFrom
  vocabulary (already cited synth:189). New: BLAKE3-named, re-verifiable edges vs asserted
  metadata.
- Muniswamy-Reddy et al., PASS (recalled USENIX ATC 2006). UNVERIFIED. Borrowed: OS-level
  provenance capture at the storage layer. New: single content-addressed put chokepoint plus
  tamper-evidence, but shares PASS's undecidable-completeness gap for edges hidden in opaque code.
- Zeldovich et al., HiStar (recalled OSDI 2006) plus DStar (recalled NSDI 2008), Krohn et al.
  Flume (recalled SOSP 2007), Efstathopoulos et al. Asbestos. UNVERIFIED. Borrowed:
  flow-as-a-graph, label lattice. New: record plus query (audit) vs runtime enforce (prevent).
  F1 keeps a tamper-evident content-addressed history DIFC lacks but gives no runtime read/write
  mediation.
- Nix/Guix content-addressed derivations, in-toto (recalled USENIX Sec 2019) / SLSA. UNVERIFIED.
  Borrowed: derivation-names-inputs, signed supply-chain provenance. New: per-put live execution
  state plus capability-ceiling coupling (Nix and in-toto have no authority model). Closest prior
  art.
- Enck/TaintDroid (recalled OSDI 2010), dynamic taint analysis. UNVERIFIED. Borrowed:
  information-flow-as-propagation. New (as a boundary): reachability sees only declared edges, so
  it misses exactly the implicit/data flows inside admitted computations taint catches.
- Murray et al., seL4 intransitive noninterference (recalled IEEE S&P 2013). UNVERIFIED.
  Borrowed: the noninterference target. New (as a boundary): F1-b is in-model reachability only,
  the hard part (covert/implicit flow everywhere) is uncovered, booked at A2/section 3, not
  claimed.
- Devecsery et al., Eidetic/Arnold (recalled OSDI 2014), Mozilla rr. UNVERIFIED. Borrowed: record
  nondeterministic inputs for deterministic replay. New: inputs become first-class
  content-addressed DAG leaves feeding DRCC-conditional replay, draw-timing stays off-DAG.
- Denning lattice (recalled CACM 1976), CT (RFC 6962), Nova/IVC. UNVERIFIED. Underlying frames
  already counted in R4/B4/B5, not re-earned here.

---

## F2 EXECUTION GEOMETRY

Claim under test. d(s1, s2) equals the byte-weight of the symmetric difference of the two
canonical Merkle closures. d(s1, s2) equals w(C(s1) triangle C(s2)) where C(s) is the set of
distinct canonical content-addressed objects reachable from s's canonical root and w(o) equals
len(o), the object's byte length.

### F0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta |
|---|---|---|
| Frechet-Nikodym measure-algebra symmetric-difference metric, classical (UNVERIFIED) | d(A,B) equals mu(A triangle B) is a pseudometric on a measure algebra, triangle inequality is a standard result. | CARMIX instantiates mu equal to byte-weight counting measure over content-addressed dedup sets. The metric axioms are covered/classical, the only new work is confirming the closure construction keeps weights context-free. |
| git have/want thin-pack plus IPFS Bitswap want-list (Torvalds/Hamano, Benet 2014) (UNVERIFIED) | Peer sync transfers only objects the other side lacks, the directed set difference of DAG closures. | This is d_pull operationally. git and IPFS never name it a metric nor reuse it for GC/branch/memo, and their closures carry no authority leaf. |
| Quincy, fair scheduling as min-cost flow (Isard et al., SOSP 2009) (UNVERIFIED) | Encodes data-transfer bytes as scheduler graph edge costs to trade locality against fairness. | Closest prior to distance-equals-bytes, but over opaque file blocks with no dedup/content-addressing and no metric guarantee. CARMIX edge weight is the deduped content-closure symmetric difference, provably a metric. |
| Delay Scheduling (Zaharia et al., EuroSys 2010) (UNVERIFIED) | Briefly delays a task so it runs on a node already holding its HDFS input blocks. | Binary node/rack locality heuristic vs CARMIX byte-exact continuous metric. CARMIX also declares the scheduling decision gated on the liveness/timing edge it cannot content-address. |
| LBFS low-bandwidth NFS / rsync content-defined chunking (Muthitacharoen et al., SOSP 2001) (UNVERIFIED) | Transfers only changed chunks between two versions, the chunk-set difference for bandwidth. | Measures inter-version byte diff for bandwidth only. CARMIX lifts it to object-granular hash-consed DAG closures and a placement/GC/branch/memo metric. |
| ZFS/Btrfs send-receive incremental snapshots (Bonwick/Ahrens) (UNVERIFIED) | Sends the block-diff between two snapshots, the symmetric difference of block sets. | Block-level snapshot diff equals branch distance operationally. CARMIX adds metric framing, object (not block) granularity, authority-in-closure, and the memo/GC readouts. |
| MinHash / Jaccard distance plus LSH (Broder 1997) (UNVERIFIED) | Estimates normalized set similarity for near-neighbor search. | Jaccard is normalized (also a metric) over unweighted sets. CARMIX uses unnormalized byte-weighted symmetric difference because absolute transfer cost is the operational quantity, and normalization would destroy the additive-cost meaning. |
| Data Gravity principle (McCrory 2010) (UNVERIFIED) | Informal: data attracts services and apps as it grows (mass proportional to data size). | CARMIX replaces the metaphor with a formula (closure-difference byte weight) that is a proved metric, honestly scoped to data-movement cost only, not compute or the liveness edge. |

Novelty outcome: PARTIALLY-COVERED.

### Triangle-inequality result: PROVED ON PAPER (Coq-tractable, not yet Qed'd)

The triangle inequality HOLDS and no counterexample exists. This is a paper proof in the
finite-list-set style of kernel/proofs/CarmixDag.v, delivered here as instructed and not yet
machine-checked, so it does not carry the same standing as the Qed'd authority-never-amplifies
law in kernel/proofs/Carmix.v.

Setup. Content-addressing makes w(o) equal len(o) a pure function of the object's hash (H_CF
collision-freedom, CarmixDag.v), so an object shared by C(s1) and C(s2) carries the same weight
in both. This context-freedom of the weight is the single load-bearing property. Hash-consing
(CarmixDag D1: one hash equals one resident node under many parents) makes C(s) a genuine set
(each shared object counted once), matching the deduped store.

Axioms on finite subsets X, Y, Z of the content universe with D(X,Y) equal w(X triangle Y), w
strictly positive. (1) Non-negativity and D(X,X) equal 0, immediate. (2) Symmetry, immediate (X
triangle Y equals Y triangle X). (3) Identity of indiscernibles: D equals 0 iff w(X triangle Y)
equals 0 iff X triangle Y empty (needs strictly-positive weights, which holds because every sls
object is [tag(1)][...] so len at least 1) iff X equals Y. (4) Triangle: the classical set
identity X triangle Z subset of (X triangle Y) union (Y triangle Z) (any o in exactly one of X,
Z lands in one of the two differences by casing on o in Y), then w(X triangle Z) at most
w((X triangle Y) union (Y triangle Z)) at most w(X triangle Y) plus w(Y triangle Z) by
monotonicity and subadditivity of a non-negative weight (w(P union Q) equals w(P) plus w(Q) minus
w(P intersect Q) at most w(P) plus w(Q)).

Do closures break it. No. The proof runs on the three resulting sets and cannot be broken by how
they were produced. The only way it could break is context-dependent per-parent weights, which
content-addressing forbids.

Two honest caveats. (a) On raw pre-canonical states d is a pseudometric whose kernel is exactly
canonical equivalence (two build orders converging to one name, CV4), a feature not a defect. It
is a true metric on canonical states and equivalence classes (CV1 determinism plus CV2
authority-in-name give injectivity of the canonical root, which holds because the canonical root
object is itself a member of C(s), so distinct states force distinct closures). (b) A
pathological 0-byte raw store object would demote identity-of-indiscernibles to a pseudometric
but never touches the triangle inequality.

### Findings

Who covers what (honest split, D1). The metric-axiom claim is covered. d(A,B) equal
mu(A triangle B) is the textbook Frechet-Nikodym measure-algebra pseudometric. The work here is
verifying it survives the closure construction rather than inventing it, and saying so. The
transfer quantity is covered. git have/want thin-packs, IPFS Bitswap want-lists, ZFS/Btrfs send,
LBFS/rsync all already compute the directed closure/chunk difference for bandwidth. The
scheduling use is covered. Quincy uses transfer-bytes as scheduler edge cost, Delay Scheduling
uses block locality. Novel-as-fusion sliver (exact delta): one proved metric equals the
byte-weight of the deduped, authority-inclusive (CV2 folds caps into the closure, so two states
differing only by a capability are not distance-0) content-closure symmetric difference, reused
as a single geometry across migration, GC, branch, and memo on a live-execution rematerialization
substrate, where content-addressing is precisely the property that gives the classical proof its
context-free weight and that opaque-block systems (Quincy, ZFS) lack.

Directed pull-distance (the migration-relevant half). Migration cost is not the symmetric d but
the directed d_pull(s, N) equal w(C(s) minus store(N)), the bytes N lacks. It is a quasi-metric:
X minus Z subset of (X minus Y) union (Y minus Z) gives w(X minus Z) at most w(X minus Y) plus
w(Y minus Z) (relay via Y never beats direct in total unique bytes). The symmetric metric
decomposes exactly: w(X triangle Y) equals d_pull(X, Y) plus d_pull(Y, X). Grounded:
kernel/nettest.c a_serve does a_bytes_sent += len (payload/hashed bytes) per pulled object, B2
cold equals d_pull(s, empty) equals full closure, B4 warm hops equal d_pull after a 1-leaf or
2-leaf diff. The committed load-bearing check already asserts hop3 (2-leaf) greater than hop1
(1-leaf) and hop far less than cold, meaning distance tracks the change set and is
diff-proportional. The two-node cluster is already a distance meter.

Four operational readouts (all backed by existing counters). (a) Migration placement, move to
nearest node: pick argmin over N of d_pull(s, N), already the a_bytes_sent readout, one value per
candidate node. (b) Branch cost: d(branch_a_root, branch_b_root) equal w(C(a) triangle C(b))
equal unique bytes to keep both, the epoch tree's shared-vs-new accounting (SR2: 11 of 15 nodes
reused, SR7: checkpoint bytes shared/reused 352B, new 128B). Cleanest fit, the epoch tree already
emits it. (c) GC pressure: reference counting already gives the exact reachable/unreachable
boolean, so d adds nothing to the reclamation decision. Where it adds a knob: the eviction value
of dropping a cold branch root r equals its exclusive closure weight w(C(r) minus union over live
roots C(root_i)), the otherwise-reclaimable bytes r alone pins. The per-root closure walk plus
per-object len already exist. Far from every live root formalizes to not in any live closure, and
among those the metric ranks by re-fetch weight. (d) Memo value: a canonical/dedup hit saves
w(o) equal len(o), total equals (would-store bytes) minus store.bytes_stored, from
store.dedup_hits already tracked. CV5's 4:1 canonical-to-byte ratio is exactly distance saved,
the weight of the 3 collapsed copies.

Adjudication of locality-aware scheduling / data-gravity. Split verdict. The data-gravity
measurement (the metric plus the four readouts) is real and buildable. It turns McCrory's
metaphor into a formula (gravity equals closure-difference byte weight) and Quincy's opaque-block
edge cost into a proved, dedup-aware, authority-aware metric. The scheduler decision policy is not
buildable from d alone and is out of the safe subset. d scores the content plane (bytes to move)
only. A real scheduler also needs compute cost and the ordering/liveness/timing inputs that are
the settled irreducible non-content-addressed edge (network verdict, restated not relitigated) and
the timing channels that stay real everywhere (D3). d cannot see time. Nondeterministic task
arrival is the declared liveness source (D4), never laundered into the metric. So gravity is
measurable now, and gravity-driven scheduling is gated on the edge CARMIX already declared
irreducible.

Domain interaction (restated, not relitigated). The store key is (authority-domain, hash)
(DEDUP_SCOPING DS1), so the metric universe is domain-tagged pairs. Identical content in two
domains is two objects contributing to two closures independently, consistent with partitioning.
d is a system-internal scheduler/GC metric over the system's own store. It is not exposed to a
domain-local observer, so it opens no cross-domain channel (the Armknecht partitioning verdict
stands untouched).

### Verdict: BUILDABLE-NOW

Reason. Both halves the claim needs are already present as counters on the two real QEMU plus
ivshmem nodes, and the triangle-inequality check reduces to three subtractions and an assert over
already-measured byte counts. The content-plane metric and its four readouts read straight off
existing instrumentation (a_bytes_sent, the epoch tree shared/new accounting, store.dedup_hits,
the per-root closure walk) with no change to the proven core. The only thing d cannot do is
choose where to run, and that is explicitly excluded and restated as the settled liveness/timing
edge. So the geometry is measurable and self-checkable now, the scheduling policy is not part of
the buildable subset.

Build subset. On the two real QEMU plus ivshmem nodes, four measurements plus one self-check, all
from existing counters (no proven-core change, kernel/kernel.c and kernel/nettest.c only). (1)
Directed pull-distance d_pull(s, N) equals a_bytes_sent from a destination-pull Merkle sync of
C(s) against store(N), already emitted as B2 cold (pull from empty equals full closure) and B4
warm hops (pull after a diff). Run it against each candidate node and pick argmin. (2) Symmetric
metric d(s1, s2) equals d_pull(s1 to s2) plus d_pull(s2 to s1) by pulling both directions and
summing the two a_bytes_sent totals. (3) Branch cost d(a, b) equals sum of len over
C(a) triangle C(b), read straight off the epoch tree's shared/new node accounting
(last_leaf_hashes/last_node_hashes, SR2 counts, SR7 checkpoint bytes shared vs new). (4) Memo
value equals (bytes that would have been stored) minus store.bytes_stored, from store.dedup_hits
times len (CV5's 4:1 ratio in byte terms). (5) Triangle-inequality runnable check, the one check
the metric claim needs, minimal: pick three real captured states s1, s2, s3, measure d(s1, s3),
d(s1, s2), d(s2, s3) with (2)/(3), assert d(s1, s3) at most d(s1, s2) plus d(s2, s3), three
subtractions and an assert over already-measured byte counts, in the CarmixDag.v exercise style.
GC exclusive-weight (value of evicting a cold branch root equals w(C(r) minus union of live
closures)) is a light extension of the per-root closure walk that already exists and can ride the
reclamation-time pass (CV7's amortization choice). Not in the subset: any policy that lets d
choose where to run, gated on the liveness/ordering/timing edge, restated as settled.

### Metric/axiom result summary

Proved on paper and Coq-tractable, not yet Qed'd: d(X, Y) equal w(X triangle Y) is a metric on
canonical states (a pseudometric on raw pre-canonical states, kernel equals canonical
equivalence). The load-bearing property is context-free weight from content-addressing. The
directed d_pull is a quasi-metric and decomposes the symmetric metric exactly. No new substrate
conservation law beyond the reuse of H_CF and hash-consing already in CarmixDag.v.

### Bibliography (all UNVERIFIED, recall not fetch, borrowed vs new)

1. Frechet / Nikodym / Aronszajn-Panitchpakdi measure-algebra symmetric-difference metric
   (classical, e.g. Halmos, Measure Theory). UNVERIFIED. Borrowed: pseudometric-from-measure
   including triangle inequality. New: instantiation on content-addressed dedup sets with byte
   weight, plus verification that closures preserve it.
2. Broder 1997, "On the resemblance and containment of documents" (MinHash / Jaccard distance).
   UNVERIFIED. Borrowed: set distance as metric. New: unnormalized byte-weighted symmetric
   difference (absolute transfer cost, normalizing would destroy the additive-cost meaning).
3. Zaharia, Borthakur, Sen Sarma, Elmeleegy, Shenker, Stoica 2010, "Delay Scheduling", EuroSys.
   UNVERIFIED. Borrowed: locality-aware placement. New: byte-exact content metric vs binary
   node/rack locality, plus declared liveness gate.
4. Isard, Prabhakaran, Currey, Wieder, Talwar, Goldberg 2009, "Quincy: Fair Scheduling for
   Distributed Computing Clusters", SOSP. UNVERIFIED. Borrowed: transfer-bytes as scheduler edge
   weight (closest neighbor to distance-equals-bytes). New: deduped content-closure symmetric
   difference, metric-guaranteed, reused across GC/branch/memo not just scheduling.
5. Muthitacharoen, Chen, Mazieres 2001, "A Low-Bandwidth Network File System (LBFS)", SOSP.
   UNVERIFIED. Borrowed: transfer only differing chunks. New: object-granular hash-consed DAG
   closure plus placement metric plus authority.
6. McCrory 2010, "Data Gravity" (coinage) and follow-ups. UNVERIFIED. Borrowed: gravity
   metaphor. New: a formula (closure-difference byte weight) with honest data-movement-only scope.
7. Benet 2014, "IPFS" plus Bitswap want-list, git have/want packfile negotiation
   (Torvalds/Hamano). UNVERIFIED. Borrowed: directed closure-difference transfer. New: naming it
   a metric, authority-in-closure, four unified readouts.
8. Bonwick/Ahrens ZFS send/receive incremental snapshots, Btrfs send. UNVERIFIED. Borrowed:
   snapshot block-diff equals branch distance. New: object-level, authority-aware, metric-framed
   with GC/memo readouts.

---

## F3 SELF-DERIVATION (the honest fixed point)

Claim under test. A boot-time self-measurement of the immutable kernel image folded into the
System Root, resolving through the store to a host-recorded derivation from named source hashes.
Adjudicated against DICE/measured boot, Nix, bootstrappable builds / GNU Mes,
reproducible-builds.org, in-toto/SLSA.

### F0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta |
|---|---|---|
| TCG DICE / DICE Layering (UNVERIFIED) | Each boot layer measures the next and derives its identity/CDI from a hardware Unique Device Secret. | F3 is the same fold-next-layer's-code-hash pattern but software-only with no UDS, so its measurement is self-reported. DICE names F3's missing hardware anchor (boundary 3.4). |
| TPM SRTM/PCR measured boot plus IMA (UNVERIFIED) | Firmware/bootloader extend hardware PCRs with per-stage code hashes, a hardware EK/AK quotes them. | F3 folds the measurement into a content-addressed root and additionally binds capability ceilings, but quotes with the software AUTH key, not a hardware key. |
| Intel TXT / AMD SKINIT DRTM (UNVERIFIED) | Dynamic root of trust: a late launch measures a known-state environment into PCRs. | Same hardware-anchor gap. F3 provides no late-launch isolation, only a self-measured leaf. |
| Nix / Guix derivations (UNVERIFIED) | Names build output by hash of (inputs plus build recipe), stores the derivation and rebuilds from it. | F3 stores a Nix-shaped derivation in the CARMIX content store and folds the built code-hash into the live running root under the anti-amp attestation. Nix names artifacts at rest only. |
| Bootstrappable Builds / GNU Mes (UNVERIFIED) | Reduces the compiler binary seed to a tiny auditable seed (hex0 to Mes to tinycc to gcc), defeating trusting-trust. | F3 names sources but treats clang/lld as an opaque unbootstrapped seed. This is a gap F3 does not close, not a claim it makes. |
| reproducible-builds.org techniques (UNVERIFIED) | SOURCE_DATE_EPOCH, build-id normalization, path remapping for byte-identical rebuilds. | F3 already neutralizes build-id and __DATE__/__TIME__/.comment and sidesteps archive timestamps by hashing only loaded .text plus .rodata. It borrows these, invents none. |
| in-toto / SLSA supply-chain attestation (UNVERIFIED) | Signed out-of-band statements binding an artifact to its builder and inputs. | F3 folds provenance into the same content root that names running state and authority, so one signature covers code plus provenance plus ceiling, re-derivably, not a separate attestation artifact. |
| CompCert / seL4 binary correspondence (UNVERIFIED) | Proves the running binary refines the audited source semantics. | F3 attests which sources produced the code (provenance) but not that the binary matches their semantics. That correspondence stays under boundary 3.7, out of reach here. |

Novelty outcome: NOVEL-AS-FUSION (the binding only, not any mechanism).

### Findings

Grounding, what exists vs what F3 adds.
- The System Root fold (kernel/kernel.c:8620-8790, run_sr) currently folds 8 leaves: taskA,
  taskB, capver, storeman, fsroot, ceilA, ceilB, sysceil. It does not fold any hash of the
  kernel's own loaded text. So today the running root does not contain the name of its own code.
  F3 adds a 9th leaf.
- The kernel already links in BLAKE3 (cvsasx_blake3, e.g. kernel/kernel.c:747) and the epoch tree
  (kernel/store/epoch_tree.c, incremental equals full VERIFIED), and SR5 already signs the root
  with vendored Ed25519 (kernel/SR_LOG.md:94-116). So the crypto plus fold machinery for F3 is
  present and reused, not new.
- Missing primitive for the boot-time self-hash: kernel.c requests no Limine executable-address
  feature, and kernel/linker.ld exports no _stext/_etext/_srodata/_erodata (only Limine markers).
  To hash its own image the kernel needs one small addition, either PROVIDE(_stext/_etext/
  _srodata/_erodata) in linker.ld or the Limine executable-address request. Trivial, nameable,
  unbuilt.
- The host derivation record is entirely new. kernel/build.sh today executes the toolchain and
  produces carmix.iso but records no store object mapping code-hash to {source hashes, toolchain,
  flags}. F3 requires the host to write a Nix-shaped derivation object into the content store.

Construction scope (stated so it cannot be faked). The code-identity leaf must be
H(.text concat .rodata), the immutable read-only loaded image. .data and .bss are runtime-mutable
and must be excluded (they differ every boot). They are already covered by the task-hash and
store-manifest leaves. This is the honest scope: the leaf names the code, not the memory. D4: no
nondeterminism laundered, mutable state stays in its own leaves, the code leaf is a build-time
constant only.

Q1 reproducibility, byte-identity is achievable, the exact blockers named. Because F3 hashes
loaded .text concat .rodata (not the ELF file, not the ISO), all archive/timestamp
nondeterminism is out of scope by construction (xorriso mkisofs timestamps, El-Torito, Limine
blobs never enter the leaf). That is the cleanest possible reproducibility surface, compiler plus
linker output only. On that surface the classic blockers are already neutralized: build.sh links
with --build-id=none (kills the GNU build-id note), linker.ld /DISCARD/ drops *(.comment)
(compiler-version string) and *(.note.*), kernel.c contains no __DATE__/__TIME__/__FILE__/
__TIMESTAMP__ (only two _Static_assert, which bake no runtime string), no -g means no DWARF
absolute paths, and -fno-pie -static -mcmodel=kernel fixes the load base at
0xffffffff80000000 so layout is deterministic. Residual blockers to record in the derivation: (1)
exact clang plus ld.lld version and flag string (a different clang version re-codegens .text to a
different hash), (2) link input ordering, build.sh links "$OUT"/*.o via glob, so the derivation
must pin the explicit object order not rely on shell glob order. Both are recordable, not
physical. Verdict on Q1: byte-identity of the hashed image is achievable once toolchain-version
and link-order are pinned in the derivation, no unfixable toolchain blocker remains on the
.text concat .rodata surface.

Q2 circularity, none, and why precisely. H(.text concat .rodata) is a pure function of read-only
bytes fixed at build time. The System Root is computed at runtime into mutable memory and is never
written back into .text/.rodata, so there is no equation root equals f(..., root, ...) to solve.
The apparent self-reference (the hashing routine's own bytes are inside the region it hashes) is
quine-style self-reference over fixed data, not a fixed point. Hashing the bytes does not alter
the bytes. The code hash is fully determined before the machine boots, so an offline party can
compute it from the ELF and predict the leaf. Folding it into the root is therefore inserting a
known constant, no bootstrap paradox. This is exactly why the code hash is fixed before any root
is computed. The leaf is a build-time invariant, independent of every runtime value.

Q3 attestation gain, real at the protocol layer, unchanged at the security layer. SR5 today lets
a verifier re-derive the root and check one signature to learn what code runs (task-hash leaves)
plus what authority each component holds (ceiling leaves). F3 adds a code-provenance dimension:
the code-hash leaf resolves in the store to a derivation from named sources, so one signature over
one 32-byte root now also attests where the code came from (its build inputs). The gain is
genuine and composes with the existing ceiling binding. But the security is unchanged and remains
gated by boundary 3.4 (kernel/ACADEMIC_SYNTHESIS.md:272, kernel/SR_LOG.md:113-116): the
measurement is taken by the kernel of the kernel and quoted with the software AUTH keypair. A
compromised kernel folds a benign code-hash leaf and signs it, so the self-report is forgeable
without an external measurer. Online, F3 demonstrates the protocol, not the security, exactly as
the repo already concedes for the emulated root. This is the DICE point restated: a
self-measurement is only trustworthy if an independent layer (hardware UDS, TPM PCR, DRTM)
measures it.

Honest decomposition of the full chain running-text to named-sources to audited-semantics. (a)
text to measurement-is-honest needs a hardware root (boundary 3.4). F3 supplies the fold pattern
but not the anchor. (b) text to named-sources needs reproducibility, buildable now, offline, no
hardware (rebuild-and-compare). (c) sources to semantics needs verified compilation
(CompCert-class, boundary 3.7), out of reach for this team, F3 does not touch it. F3 delivers (b),
gestures at (a), ignores (c). Stating this split is the contribution's honest boundary.

Adjudication of the named works.
- DICE (TCG Device Identifier Composition Engine): layered measured boot where each layer
  measures the next and derives its CDI from a hardware Unique Device Secret. F3 is the same
  fold-next-layer's-code-hash pattern, software-only, with no UDS, so its measurement is
  self-reported. DICE both validates F3's shape and names its missing piece (the hardware anchor
  of 3.4).
- Measured boot (TPM SRTM/PCR plus IMA, Intel TXT/AMD SKINIT DRTM): firmware/bootloader extend
  PCRs with stage hashes, a hardware EK/AK quotes them. F3 folds the measurement into a
  content-addressed root and additionally binds capability ceilings but has no hardware quoting
  key. Covers the trust anchor F3 lacks.
- Nix / Guix (reproducible-builds.org context): build output named by hash of (inputs plus
  recipe), derivation stored and rebuildable. Closest prior art for F3's derivation-record half.
  F3 stores a Nix-shaped derivation in the CARMIX content store. Delta: Nix names artifacts at
  rest, F3 additionally folds the built code-hash into the live running root and binds it under
  the anti-amp attestation.
- Bootstrappable builds / GNU Mes (hex0 to Mes to tinycc to gcc): shrink the compiler binary seed
  to a tiny auditable seed, defeating Thompson trusting-trust. F3's derivation names sources but
  treats clang/lld as an opaque, unbootstrapped, unhashed-provenance seed. This is a gap F3 does
  not close: derivation from named sources bottoms out at a large unaudited toolchain binary. Mes
  is the neighbor showing the residual trusted base.
- reproducible-builds.org (SOURCE_DATE_EPOCH, build-id normalization, path remapping): F3 already
  neutralizes build-id and __DATE__/__TIME__/.comment and sidesteps archive timestamps by hashing
  loaded .text concat .rodata. It borrows these techniques, invents none. Covered.
- in-toto / SLSA (supply-chain provenance attestations): out-of-band signed statements binding
  artifact to builder plus inputs. F3's delta is folding provenance into the same
  content-addressed root that names running state and authority, so one Ed25519 signature covers
  code plus provenance plus ceiling together, re-derivable, rather than a separate attestation
  artifact.

Novelty. NOVEL-AS-FUSION. Every mechanism is borrowed: the measure-next-layer fold (DICE), the
derivation record (Nix), reproducibility hygiene (reproducible-builds.org), provenance
attestation (in-toto/SLSA), and the content-addressed root plus Ed25519 quote (the existing SR).
Delta sentence: F3 folds a boot-time self-measurement of the immutable kernel image into the same
re-derivable System Root that already names live process state and capability ceilings, and
resolves that leaf through the store to a Nix-shaped derivation from named source hashes, so one
signature over one 32-byte root attests what-runs plus where-it-came-from plus what-authority
jointly and re-derivably, a binding no single neighbor provides. The novelty is strictly the
binding/fusion, not any mechanism, and it does not cross boundary 3.4 (the online measurement
stays self-reported) or 3.7 (no source-to-binary correspondence).

### Metric/axiom result

No new substrate conservation law. The only invariant F3 relies on is trivial:
H(.text concat .rodata) is a build-time constant, invariant across boots and independent of the
runtime root computation, which is precisely what makes circularity absent (a quine-style
self-reference over fixed data, not a fixed point requiring solution). This is a property of the
construction, not a conservation law of the substrate on the order of authority-never-amplifies
(kernel/proofs/Carmix.v).

### Verdict: BUILDABLE-NOW

The core construction is buildable now on existing primitives (in-kernel BLAKE3, the epoch-tree
fold, Ed25519, the content store) plus one trivial addition (export _stext/_etext/_srodata/
_erodata via linker.ld PROVIDE, or add the Limine executable-address request). The safe subset
delivers real hardware-free value: an offline verifier rebuilds .text concat .rodata
byte-identically from the named sources with the pinned toolchain and confirms it equals the
folded code-hash leaf, and that the leaf resolves to that derivation. Circularity is genuinely
none, the code-hash is a build-time constant fixed before any root is computed. The extension
beyond the safe subset, treating the online self-reported quote as a trusted measurement, does
not add a new blocker. It is bounded by the already-owned boundary 3.4 (no hardware root of
trust: a compromised kernel forges its own code-hash leaf), and full source-to-binary
correspondence stays under boundary 3.7 (CompCert-class, out of reach). So BUILDABLE-NOW for the
provenance/reproducibility subset, and the trusted-online-attestation is 3.4-gated, not newly
blocked.

Build subset (needs no hardware, buildable now). (1) Host build.sh writes a Nix-shaped derivation
object into the content store equal to {clang plus ld.lld version hashes, exact flag string,
source-file BLAKE3s, explicit link order} mapping to the predicted code-hash. (2) Kernel exports
_stext/_etext/_srodata/_erodata (linker.ld PROVIDE) or requests Limine executable-address,
computes H(.text concat .rodata) with the in-kernel cvsasx_blake3, and folds it as a 9th
System-Root leaf (SR_CODEHASH) through the existing epoch tree. (3) SR5's existing Ed25519 quote
now covers the code-hash leaf. (4) An offline verifier rebuilds from the named sources with the
pinned toolchain, gets byte-identical .text concat .rodata, and checks it equals the folded leaf
and resolves to the stored derivation. Excluded from the safe subset (the 3.4 gate): trusting
that the online self-reported code-hash leaf reflects what physically ran, which needs an
independent measurer (DICE UDS, TPM PCR, DRTM). Also excluded: source-to-semantics correspondence
(boundary 3.7, CompCert-class), and shrinking the opaque clang/lld trusted seed
(bootstrappable-builds, GNU Mes).

### Bibliography (all UNVERIFIED, recall not fetch, borrowed vs new)

- TCG DICE / DICE Layering Architecture (recalled TCG spec). UNVERIFIED. Borrowed: the per-layer
  measure-and-derive pattern. New in F3: none of the mechanism, F3 lacks the UDS anchor.
- TCG TPM 2.0 plus SRTM/PCR measured boot, IMA (Linux, recalled). UNVERIFIED. Borrowed: load-time
  measurement extended into hardware registers. New: F3 folds measurement into a content root and
  binds ceilings, but no hardware quote.
- Intel TXT / AMD SKINIT DRTM (recalled). UNVERIFIED. Borrowed: late-launch measured root. New:
  nothing, F3 has no isolation primitive.
- Ken Thompson, "Reflections on Trusting Trust" (recalled CACM 1984). UNVERIFIED. Names the
  toolchain-seed problem F3 leaves open.
- Nix (Dolstra et al., recalled PhD 2006, purely functional deployment) and Guix. UNVERIFIED.
  Borrowed: derivation equals hash(inputs plus recipe). New: F3 stores it in-kernel-store and
  folds the output into the live root.
- Bootstrappable Builds / GNU Mes plus stage0/hex0 (recalled). UNVERIFIED. Names the residual
  trusted-compiler-seed gap F3 does not close.
- reproducible-builds.org (SOURCE_DATE_EPOCH, recalled). UNVERIFIED. Borrowed: determinism
  hygiene. New: none, F3 already applies the subset it needs.
- in-toto (Torres-Arias et al., recalled USENIX Security 2019) and SLSA (recalled). UNVERIFIED.
  Borrowed: build provenance attestation. New: F3 co-locates provenance with running-state naming
  under one anti-amp-bound signature.
- CompCert (Leroy) / seL4 binary correspondence (Sewell et al., recalled PLDI 2013). UNVERIFIED.
  Names the source-to-semantics gap (boundary 3.7) F3 does not touch.

---

## F4 THE DELTA WIRE

Claim under test. A theory of content-addressed transfer between two domain-scoped stores: move
exactly the receiver-missing subset of a Merkle closure, re-verify every object by BLAKE3 before
ingest, carry an attenuating authority ceiling with the names, and keep ordering plus liveness on
the declared non-content-addressed transport edge.

### F0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta |
|---|---|---|
| rsync (Tridgell & Mackerras 1996) (UNVERIFIED) | Rolling weak checksum plus strong hash finds byte-shifted block matches, about 2 round trips, ships literal plus copy instructions. | Byte-granular and shift-resistant (beats CARMIX on minimality), but has no content-addressed store, no cross-file identity dedup, no authority model. Its checksum is a similarity heuristic, not a namespace. CARMIX moves object-granular but each object is its name, re-verifies, and is gated by an anti-amp ceiling. |
| casync (Poettering 2016) (UNVERIFIED) | Content-defined chunking (buzhash) into a hash-addressed chunk store, ships a caibx index then only the missing chunks. | Sharpest content-side neighbor. CDC gives strictly finer minimality than CARMIX's fixed sls leaves, and index-exchange beats b_sync's per-object walk on round trips. Lacks any anti-amplification capability ceiling or destination-at-most-source re-mint, casync has no authority-on-the-wire concept at all. |
| IPFS Bitswap (Protocol Labs) (UNVERIFIED) | Destination-pull want-list of blocks by CID over a Merkle DAG, multi-peer, want-have/want-block (1.2.0) cuts duplicate transfers. | Almost exactly b_sync's shape (pull-by-hash over a DAG, dedup by CID) but public/multi-peer with no capability model by design. CARMIX adds the anti-amp reach ceiling plus authority-bounded re-mint gating which blocks may be pulled and with what authority. Restates the NETWORK_MODEL IPFS adjudication, consistent. |
| Git pack negotiation (smart/pack protocol) (UNVERIFIED) | Ref advertisement plus multi-round want/have with exponential have back-off to find the common frontier, server computes a thin, delta-compressed packfile of missing objects. | The negotiation-cost prior art (CARMIX should borrow its want/have back-off). Git objects are content-addressed with a DAG and dedup by id, but there is no per-object authority ceiling and no anti-amp re-mint. Repo access fetches the whole reachable closure. |
| Nix copy / closure substitution (Dolstra 2006) (UNVERIFIED) | Copies exactly the receiver-missing paths of a store closure between two stores (valid-paths query equals have-negotiation), substitutes carry narinfo signatures. | Closest neighbor on closure sync between two stores, missing subset, signed. Trust is a signing-key allowlist and install authority is ambient (a package does anything the builder can). CARMIX's delta is the attenuating capability ceiling re-minted at install (destination-at-most-source), gating what the installed closure may do, not just who signed it. |
| Dat / Hypercore (Ogden et al.) (UNVERIFIED) | Append-only signed content-addressed log with Merkle-tree-verified incremental sync, a feed is authenticated by its author key. | Closer to authority-on-names than casync/Git because the feed is signed, but the signature authenticates the author, not a bounded attenuating ceiling that re-mints destination-at-most-source. Dat equals author-authenticated append log, CARMIX equals anti-amp ceiling on the transfer. |
| Set reconciliation: IBLT (Eppstein et al. SIGCOMM'11), Erlay/minisketch (Naumenko et al. CCS'19), Graphene (SIGCOMM'19) (UNVERIFIED) | Reconcile a set difference of size d in about O(d) bandwidth and about 1 round trip, independent of total set size. | Names the negotiation-cost lower bound (learning a difference of size d costs Omega(d) bits). CARMIX's per-object walk is nowhere near this. This is borrowed prior art marking the optimum, not a CARMIX claim. |
| bup / restic / borg (CDC dedup backup) (UNVERIFIED) | Content-defined chunking plus content-addressed chunk store plus dedup, restic/borg add repo-key encryption/authentication. | Confirms the field is crowded on content-addressed-dedup-transfer and empty on the attenuating-authority-ceiling axis. Repo-key auth is a static credential, not a destination-at-most-source re-mint through a structural gate. |

Novelty outcome: NOVEL-AS-FUSION (the attenuating ceiling on the transfer, itself a reuse of the
proven migration gate).

### Findings

Negotiation cost. b_sync is destination-pull with local pruning. The sender is stateless about
receiver contents, the receiver decides via store_exists. Cost equals 1 round trip per
transferred object (serial WANT to BLOCK, a node's children hashes are unknown until its block
arrives) equal O(|T|) round trips with zero pre-negotiation bandwidth, the chatty corner. The
spectrum, with the real asymptotic tradeoff of round-trips vs summary-bandwidth: (a)
manifest/index exchange (casync caibx, Git thin pack) equals O(1) round trips plus O(|closure|)
manifest bytes, delta computed sender-side, (b) want/have (Git) equals O(log) rounds via
exponential have back-off, (c) set reconciliation (IBLT, minisketch/Erlay) equals O(1) round
trips plus O(d) bandwidth for difference size d, with a false-positive residue needing a fallback
pass. Information-theoretic floor: learning a difference of size d costs Omega(d) bits, so IBLT
and minisketch are bandwidth-optimal and round trips compress to O(1) but never below the
summary-exchange RTT. b_sync sits at (O(|T|) RTT, 0 summary). Cheap fix inside the existing model,
no new theory: each sls node already is a manifest of its children's hashes, so after a node
arrives the receiver can batch-WANT all its non-resident children in one round, giving O(depth)
rounds instead of O(|T|). Optimization, not mechanism.

Authority on the wire (restate, do not relitigate). The network verdict stands. Endpoint identity
is a content-addressed descriptor, reachability is a bounded anti-amp capability, ordering plus
liveness are the irreducible non-content-addressed edge. Closure sync is the favorable case for
the content half. The Merkle DAG supplies its own topological/dependency order (a parent names its
children), so the object set needs no external ordering counter. The only non-CA residue is the
transport rendezvous (the SEQ_A/SEQ_B monotonic counters, the WAIT_BUDGET timeout), which is
declared liveness/nondeterminism, not laundered into fake determinism. Authority ceilings on names
are already built. The migration DM stage binds a signed canonical authority set to the
transferred root, re-verifies it (real Ed25519), and re-mints bounded destination-at-most-source
through the unmodified swcap gate, fail-closed on any post-sign widening (DM5/DM6). The
distrusting variant (Step 14) adds attestation of code identity with an emulated root (real
security needs a hardware root, future work, stated). Timing channel stays real: the negotiation
walk and the re-hash both take measurable, content-dependent cycles (b_sync round-trip about 60k
cyc, Ed25519 sign about 44M / verify about 73M, rdtsc-reported). Nothing here erases the side
channel.

Re-verification. Every received object recomputes BLAKE3(bytes) and demands equality with the
requested address before store_put. A mismatch is fail-closed (dropped, never inserted).
Adversarially exercised at two sites: a flipped leaf byte and a flipped mid-tree node byte are
both rejected by the same hash check. Forgery fails by construction because tampered bytes hash to
a different address. The child-hint header is untrusted (walk hint only). Security rides the
payload hash, checked against the address the trusted parent named, so trust reduces to the root:
the advertised root is authenticated by the signature stage, every object below by the Merkle
chain. Received objects enter the domain-scoped store (partitioned dedup, the Armknecht/Harnik
cross-domain side channel is closed by partitioning, SETTLED, not relitigated) only after
re-verification.

Novelty adjudication equals NOVEL-AS-FUSION. The nearest-neighbor table shows the
content-addressed-missing-subset-with-hash-verify transfer is covered many times over (casync,
IPFS, Git, Nix copy, bup). The negotiation-cost theory is covered (Git want/have, IBLT/Erlay).
Even signed closure authority is prior art (Nix narinfo, Dat feed keys). The one cell no single
named neighbor occupies: closure sync where the transferred names carry an attenuating
anti-amplification ceiling, the received closure's re-minted authority bounded
destination-at-most-source through a structural gate, re-minted from re-verified content, every
object re-verified into a domain-scoped store. Exact delta sentence: casync/Nix-copy transfer
(borrowed, and done better than CARMIX on CDC plus manifest exchange) placed behind the proven
migration anti-amp ceiling (the only new cell), which is a reuse of the DM-stage gate, not a new
transfer mechanism.

Borrowed vs new (per D6). Borrowed: destination-pull Merkle walk (IPFS/Git), missing-subset
closure copy (Nix copy), hash-verify-on-receive (all content-addressed transports),
dedup-by-identity (all such stores), content-defined chunking as the minimality ideal
(casync/rsync, CARMIX does not have this, a real limit), set-reconciliation optimum (IBLT/Erlay,
CARMIX does not have this). New (as fusion only): the anti-amp ceiling on the transfer
(destination-at-most-source, re-minted from re-verified content) fused with domain-scoped
re-verified ingest, narrow, and reuses the proven gate.

### Metric/axiom result

Minimal-transfer statement (correct-sync axiom): sync(H, R) moves exactly
T equals { o in Closure(H) : hash(o) not in R }. b_sync realizes it via a receiver-driven
top-down walk: worklist seeded with root H, for each oid if store_exists(R, oid) then prune and
do not enqueue its children (a resident node's whole subtree is resident by the DAG invariant that
a node's hash covers its children's hashes), else want, re-verify, insert, enqueue non-resident
children. Exact minimality at object granularity from two guards: no over-fetch (store_exists on
both the pop and the child-push) and no under-fetch (every accepted node's children are examined).
Subtree-prune-on-resident makes it diff-proportional. Status: runtime-demonstrated (B2 cold pull
moves the full closure, B3/B4 warm hops move only each hop's own changed leaves plus re-rooted
spine, hop-with-2-changed-leaves greater than hop-with-1, leaf-site and mid-tree-node-site tamper
both rejected). Not machine-checked: the proven Coq core (kernel/proofs/Carmix.v) covers
authority-never-amplifies (the re-mint ceiling), not this transfer-minimality statement. Honest
gap: minimality is over a fixed chunking (sls leaf at most 1024 B, caller-imposed boundaries, no
content-defined chunking in kernel/sls.c), so it is minimal-in-objects, not
minimal-in-bytes-under-shift. A one-byte insert at the front of a leaf re-transfers that whole
leaf and re-roots the nodes above it. Matching casync/rsync on arbitrary edits would require
content-defined chunking, which is not present.

### Verdict: BUILDABLE-NOW

The content half (minimal transfer, subtree-prune dedup, BLAKE3 re-verify, domain-scoped ingest)
and the authority half (signed canonical set, anti-amp bounded re-mint destination-at-most-source)
are both already built and adversarially tested over ivshmem. F4's candidate delta is their fusion
into one closure-sync-with-inline-ceiling pass, which is wiring over proven primitives, not
research. The only research residue (registry/revocation, real NIC, CDC) sits off the delta wire
itself and is separately gated, so the wire is BUILDABLE-NOW for the loopback subset. Overclaim
guard: the transfer mechanism is borrowed (casync/Nix-copy/IPFS/Git), the sole fused delta is the
attenuating ceiling on the transfer (itself a reuse of the proven migration gate), and minimality
is object-granular over a fixed chunking, not byte-granular under shift.

Build subset. Single-CPU, ivshmem loopback. Fuse the already-green b_sync closure pull (worklist
plus store_exists prune plus BLAKE3 re-verify before insert plus domain-scoped store_put) with
the already-green DM-stage signed-authority re-mint (Ed25519 verify plus swcap bounded re-mint,
destination-at-most-source, fail-closed on widening) into one closure-sync pass gated by the
root's ceiling: diff-proportional (receiver-missing objects only), naive per-object negotiation.
Zero new mechanism, every part is banked (B2 cold, B3/B4 warm, leaf plus node tamper rejection,
DM5/DM6). This is the theory layer plus buildable slice under PKG-2 of FRONT SD (the C2
completion build). Named gates / out-of-scope, separable from the wire: (1) real network fetch of
a remote closure, no NIC, hardware-blocked (NET-5), (2) global registry / discovery / revocation
index, the open residue where supply-chain trust re-concentrates, (3) negotiation-cost
optimization, batched-child (O(depth) rounds) or IBLT reconciliation, buildable but optimization
not the minimal correct thing, (4) content-defined chunking for byte-shift minimality, absent, a
real limit vs casync/rsync.

Anchors. kernel/nettest.c (b_sync worklist plus store_exists prune plus BLAKE3 re-verify plus
tamper rejection, a_serve, SEQ_A/SEQ_B rendezvous, b_sync about lines 393-437, a_serve about
334-384), kernel/NET_LOG.md, kernel/NETWORK_MODEL.md (the standing network verdict),
kernel/MIGRATION_AUTHSET_LOG.md (DM5/DM6 signed-set bounded re-mint),
kernel/MIGRATION_DISTRUST_LOG.md (emulated-root attestation), kernel/sls/cvsasx_sls.h
(CVSASX_SLS_MAX_LEAF equals 1024, fixed leaf, no CDC), kernel/MEGAPLAN.md FRONT SD / PKG-2 (the
completion build this feeds).

### Bibliography (all UNVERIFIED, recall not fetch, on-subject, borrowed unless noted)

- Tridgell & Mackerras, "The rsync algorithm", 1996. UNVERIFIED. Rolling checksum delta
  (borrowed).
- Poettering, casync, 2016. UNVERIFIED. CDC plus caibx index (borrowed).
- Benet, "IPFS", 2014, plus Bitswap spec plus GraphSync. UNVERIFIED. Pull-by-CID DAG sync
  (borrowed).
- Git Documentation/technical/pack-protocol. UNVERIFIED. want/have negotiation (borrowed).
- Dolstra, "The Purely Functional Software Deployment Model" (PhD), 2006, plus nix copy/narinfo.
  UNVERIFIED. Closure substitution, signed (borrowed).
- Ogden et al., Dat/Hypercore. UNVERIFIED. Signed content-addressed append log (borrowed).
- Eppstein, Goodrich, Uyeda, Varghese, "What's the Difference?", SIGCOMM 2011. UNVERIFIED. IBLT
  set reconciliation (borrowed).
- Naumenko et al., "Erlay", CCS 2019. UNVERIFIED. minisketch reconciliation (borrowed).
- Ozisik et al., "Graphene", SIGCOMM 2019. UNVERIFIED. Interactive set reconciliation (borrowed).
- borg/restic/bup docs. UNVERIFIED. CDC dedup backup (borrowed).

Keyword-collision guard: Bloom above means the Bloom filter (Burton Bloom 1970, approximate set
membership). It is not the CALM/Bloom declarative language (Alvaro/Hellerstein) referenced
elsewhere in the tree. Distinct, do not conflate (the PROOF_CORE_RESEARCH note already flags
this).

---

## F5 THE UNIFIED CLAIM

### What CARMIX becomes if F1 to F4 land on top of RC1 to RC3

Every reachable state gets four attributes at once. A name (the BLAKE3 content address, already
proven collision-free and acyclic in kernel/proofs/CarmixDag.v). A derivation (F1: a typed
per-put record of parent hashes plus rule-or-admitted-flag plus declared entropy leaves, so the
state sits in a checkable DAG rooted at named sources). A distance (F2: the byte-weight of the
symmetric difference of canonical closures, proved on paper to be a metric on canonical states,
Coq-tractable but not yet Qed'd). And a wire that moves only differences (F4: destination-pull of
exactly the receiver-missing subset of the closure, every object re-verified, carrying an
attenuating authority ceiling). F3 closes the loop on the substrate itself. The running kernel
image folds its own build-time code-hash into the System Root and resolves it to a Nix-shaped
derivation from named sources, so the machine that names, derives, measures, and moves state also
names and derives itself.

The one binding that ties them is the identity CARMIX already half-owns. The provenance edge and
the authority edge would be the same edge (F1, once the typed derivation record is built), so the
proven anti_amplification law reads simultaneously as authority-monotonicity and
provenance-monotonicity. The distance metric (F2) is measured off the very same closures the
delta wire (F4) transfers, so distance is not an abstract quantity but literally the byte count
the wire will move. The wire carries the authority ceiling (F4) that the proven gate mints, so a
transfer cannot amplify what the derivation records. This is a single geometry: name, derivation,
distance, and wire are four readings of one content-addressed DAG plus one proven gate, not four
subsystems bolted together. That coherence is the claim. It is a fusion of borrowed mechanisms
bound by one already-proven law, not a new primitive.

### One honest delta sentence per nearest neighbor, whole program

- Green-Karvounarakis-Tannen semirings (UNVERIFIED): CARMIX has no relational algebra, so
  provenance degenerates to Boolean lineage and the only real plus is store dedup.
- W3C PROV-DM (UNVERIFIED): CARMIX edges are BLAKE3-named and re-verifiable, PROV supplies only
  the vocabulary.
- PASS (UNVERIFIED): F1 records at one content-addressed chokepoint with tamper-evidence but
  inherits PASS's undecidable-completeness gap for edges hidden in opaque code.
- HiStar/Flume DIFC (UNVERIFIED): F1 audits flow as offline DAG reachability, it does not enforce
  it at runtime as DIFC does.
- Nix/Guix plus in-toto/SLSA (UNVERIFIED): F1 and F3 extend content-addressed derivations from
  batch artifacts to live per-put state and the live running root, and couple them to an authority
  ceiling Nix lacks.
- TaintDroid (UNVERIFIED): F1 reachability sees only declared edges and misses the implicit flows
  inside a computation that taint catches.
- seL4 noninterference (UNVERIFIED): F1-b is in-model reachability, seL4's covert and implicit
  flow coverage is exactly what F1 does not claim.
- Eidetic/Arnold plus rr (UNVERIFIED): F1-d makes recorded nondeterministic inputs first-class
  content-addressed leaves, draw-timing stays off-DAG.
- Frechet-Nikodym metric (UNVERIFIED): CARMIX instantiates the classical symmetric-difference
  pseudometric on content-addressed dedup sets, the only new work is confirming closures keep
  weights context-free.
- Quincy / Delay Scheduling (UNVERIFIED): CARMIX turns transfer-bytes edge cost into a proved
  dedup-aware authority-aware metric, but declares the scheduling decision gated on the liveness
  edge it cannot content-address.
- LBFS/rsync, casync, ZFS/Btrfs send (UNVERIFIED): CARMIX lifts chunk/block diff to
  object-granular hash-consed closures reused across GC, branch, and memo, but is minimal in
  objects not bytes-under-shift because it has no content-defined chunking.
- git have/want plus IPFS Bitswap (UNVERIFIED): CARMIX names the directed closure difference a
  metric and gates the pulled closure by an anti-amp ceiling neither carries.
- MinHash/Jaccard (UNVERIFIED): CARMIX uses unnormalized byte-weighted difference because absolute
  transfer cost is the operational quantity.
- Data Gravity (UNVERIFIED): CARMIX replaces the metaphor with a formula scoped honestly to
  data-movement cost only.
- DICE / TPM measured boot / DRTM (UNVERIFIED): F3 reuses the measure-and-fold pattern software-
  only with no hardware secret, so its online quote is self-reported and 3.4-gated.
- reproducible-builds.org (UNVERIFIED): F3 borrows the determinism hygiene and invents none of it.
- Bootstrappable Builds / GNU Mes (UNVERIFIED): F3 names sources but leaves the opaque clang/lld
  seed unaudited, a gap it does not close.
- CompCert / seL4 binary correspondence (UNVERIFIED): F3 attests which sources produced the code
  but not that the binary matches their semantics, which stays under boundary 3.7.
- Nix copy / Dat (UNVERIFIED): F4 adds an attenuating destination-at-most-source ceiling re-minted
  through a structural gate, not just a signing-key allowlist or an author key.
- IBLT / Erlay / Graphene (UNVERIFIED): these mark the O(d) negotiation optimum, CARMIX's
  per-object walk is nowhere near it and does not claim to be.
- bup/restic/borg (UNVERIFIED): confirm the field is full on dedup transfer and empty on the
  authority-ceiling axis F4 occupies.

### KILL-LIST, what still cannot be claimed

- Noninterference as a security guarantee (F1). Killed by undecidable parent-set completeness (A2
  class), off-DAG timing channels (dedup/Armknecht, settled and permanent), and implicit flows
  inside admitted computations. F1-b is a proof-directive reachability lemma only.
- How-provenance / the provenance polynomial (F1). Killed for opaque admitted computations,
  survives only as Boolean lineage plus dedup-as-plus.
- The provenance-edge-equals-authority-edge identity as an achieved fact (F1). Not built yet. The
  store records only (hash, offset, len), so this is BUILDABLE-GATED, stated conditionally, not
  today.
- Byte-minimal transfer under arbitrary edits (F4). Killed by fixed chunking (no CDC), CARMIX is
  minimal in objects not in bytes-under-shift.
- Bandwidth-optimal negotiation (F4). Not claimed, the per-object walk is O(|T|) round trips, far
  from the Omega(d) floor.
- Trusted online attestation of what physically ran (F3). Killed without a hardware root of trust
  (boundary 3.4). A compromised kernel forges its own code-hash leaf. Only offline
  rebuild-and-compare is buildable now.
- Source-to-semantics correspondence (F3). Out of reach, CompCert-class (boundary 3.7). F3
  attests provenance, not that the binary refines the sources.
- A shrunk trusted toolchain base (F3). The clang/lld seed stays opaque and unaudited
  (bootstrappable-builds / GNU Mes gap).
- The metric as a machine-checked law (F2). Proved on paper and Coq-tractable, not yet Qed'd, so
  it does not carry the standing of authority-never-amplifies.
- A gravity-driven scheduler (F2). The metric scores content bytes only. The decision needs
  compute cost plus the liveness/ordering/timing edge, which is the settled non-content-addressed
  boundary.
- Physical hardware (all fronts). No NIC (real network fetch is hardware-blocked, NET-5), no
  hardware root of trust (no UDS/TPM/DRTM), no real attestation, no cross-machine clock. Timing
  channels stay real everywhere (D3). Nondeterminism (entropy leaves, task arrival, SEQ/
  WAIT_BUDGET rendezvous) is declared as a source, never laundered into fake determinism (D4). The
  settled verdicts (Armknecht partitioning, the network hybrid, CV6 observational-equivalence
  undecidability) stand as boundaries and are not reopened.
