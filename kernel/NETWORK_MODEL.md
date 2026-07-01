# NETWORK_MODEL.md - can a network endpoint be a CARMIX capability?

Research note. No build, no code change. This adjudicates one question: whether a
network endpoint can be expressed as a CARMIX capability under content-addressing
and fail-closed anti-amplification, or whether networking is a boundary of the
thesis. A precise boundary is treated as a result of equal value to a native model.

## Evidence grading (used throughout)

- [E] Established in CARMIX. Built and shown, with the anchor file named.
- [C] CARMIX-specific design claim. Follows from established properties, not
  separately built.
- [O] Open. Not built, not yet decided.
- [P] In-principle. Would hold given a component that does not exist in the tree
  yet (a real NIC, a real clock, a hardware root of trust). Labelled as such.
- [PA] Prior art. Named, not a CARMIX contribution.

## Established properties this note builds on (not re-derived)

- Content-addressing. An object's address is BLAKE3 of its bytes. Content equals
  address. A changed object is a new name. [E] kernel/U4_LOG.md, kernel/nettest.c.
- Capability gate with fail-closed anti-amplification. Destination authority is at
  most source authority, enforced structurally at every crossing (spawn birth,
  IPC send, delegation hop, migration re-mint), not by convention. [E]
  kernel/U1_LOG.md, kernel/U4_LOG.md, kernel/cvsasx_swcap.* via nettest.c.
- Rematerialization. A process is a content-addressed live execution re-created
  from content-addressed state plus a bounded authority set. [E] kernel/U1_LOG.md,
  kernel/U4_LOG.md.
- Capability IPC. A synchronous endpoint carries data and capabilities. A send of
  an un-held cap is refused. [E] kernel/U1_LOG.md U1-3.
- Merkle-DAG namespace. A directory is a name-to-hash object with its own content
  hash. Delegation attenuates at every hop. [E] kernel/U4_LOG.md.
- Two-machine migration. Trusted-cluster and distrusting variants over ivshmem
  (shared memory, not a network), real Ed25519, content-addressed state
  re-verified by re-hash, authority re-mint bounded by the same anti-amp gate. The
  transport is explicitly not a network and is not claimed to be. [E]
  kernel/MIGRATION_AUTHSET_LOG.md, kernel/MIGRATION_DISTRUST_LOG.md, kernel/nettest.c.

One structural fact from the anchors is load-bearing below. CARMIX already carries
two flavours of capability, not one. The first names an immutable object by its
content hash (the namespace, the process image, the migrated state). The second
grants bounded authority over a mutable region (the swcap gate over a base and
length, and the U-1 IPC endpoint, whose identity is a handle and not a hash of the
data flowing through it). Content-addressing is a property of the first flavour.
Anti-amplification is a property of both. Keeping these two axes apart is what
makes a clean boundary possible.

---

## N1 ENDPOINT MODEL

Is a network endpoint a coherent CARMIX capability?

### (a) Endpoint as a capability to a mutable channel

If the endpoint is named by the content flowing through it, it has no stable
identity. Content-addressing assigns a name by hashing bytes. A channel whose
bytes change every message would hash to a different address every message, so it
cannot be a fixed name. Under strict content-addressing, interpretation (a) is
incoherent: a mutable channel has no content-address. [C]

But CARMIX capabilities are not all content-addressed. The swcap flavour and the
U-1 IPC endpoint grant authority over mutable state, and their identity is a
handle, not a hash of the payload. [E] kernel/U1_LOG.md, kernel/cvsasx_swcap.*.
Read through that flavour, interpretation (a) is coherent as an authority
capability: a handle that grants the bounded right to read and write a live
channel, exactly as swcap grants bounded read and write over a region. What it is
not is a content-addressed capability. The mutability is real, and it lives in the
authority axis, where CARMIX already tolerates mutable referents. Content
-addressing simply does not reach the live bytes.

### (b) Endpoint content-addressed by an immutable endpoint descriptor

Separate the immutable identity of a channel from its mutable data flow. Define an
endpoint descriptor as a fixed tuple: peer identity or public key, protocol,
connection parameters, and keying material. That tuple is frozen content. Its
BLAKE3 hash is a stable name for the endpoint's configuration. [C]

This is coherent and it is the U-4 pattern applied unchanged. A directory is a
name-to-hash object with its own content hash. [E] kernel/U4_LOG.md. An endpoint
descriptor is a parameters-to-value object with its own content hash, and a
capability can name it, be delegated for it, and be attenuated over it exactly as
U-4 delegation attenuates. It also mirrors the migration design, where an
immutable record (a root hash, a scope, a ceiling) is signed and verified while
the live thing moves separately. [E] kernel/MIGRATION_AUTHSET_LOG.md.

What (b) gives: a content-addressed name for the identity of a channel, and a
capability to reach that specific channel. What (b) does not give: any
content-addressing of the bytes in flight. That is N2.

### Adjudication

Interpretation (b) is coherent and CARMIX-native. Interpretation (a) is coherent
only as an authority capability, not as a content-addressed one. The mutability is
not irreducible at the level of endpoint identity, because the descriptor resolves
it by separating the frozen configuration from the live flow. The mutability is
irreducible at the level of the data flow, and content-addressing does not claim
the flow. The endpoint therefore splits cleanly into a content-addressed
identity half and an authority-gated live half.

---

## N2 DATA-FLOW MODEL

Does content-addressing extend to the byte stream?

### (a) Each message as a content-addressed immutable object

This is not hypothetical. It is the transfer model already running on the migration
wire. Each object crossing the wire is BLAKE3-verified, the receiver demands that
BLAKE3(received) equals the requested address, and a forged block fails by
construction because tampered bytes hash to a different address. [E]
kernel/nettest.c (b_sync, the leaf and node tamper rejections). Deduplication is
also shown: the destination prunes any object it already holds and never re-fetches
it. [E] kernel/nettest.c (store_exists pruning). So a connection modelled as an
ordered sequence of hash-named messages gives integrity and dedup at message
granularity, and both are demonstrated, over shared memory rather than a socket.

Where message-as-object holds:

- Integrity. Each completed message self-verifies by hash. [E]
- Deduplication. Identical messages and identical subtrees are pruned. [E]
- Per-message immutability. A sent message is frozen content with a fixed name. [E]
- Content-addressed request and response. Pull-by-hash (WANT then BLOCK). [E]

Where it breaks:

- Ordering. Content-addressing yields a set of hash-named messages, not a
  sequence. Hashes carry no order. An ordered stream needs an external ordering
  structure: a monotonic counter, or a chain in which each message names the hash
  of its predecessor. nettest hit this directly. A tri-state flag had a toggle
  ambiguity where the second block was indistinguishable from the first, and it
  was replaced by a monotonic sequence counter per side. [E] kernel/nettest.c
  (SEQ_A_OFF, SEQ_B_OFF, the comment recording the toggle ambiguity). This is
  concrete evidence that content hashes alone did not provide ordering and a
  separate sequence axis was required.
- Liveness. A hash names content that already exists. A stream produces content
  that does not yet exist. You cannot address a message before it is produced.
  Content-addressing is retrospective, a stream is prospective. This is the deep
  tension carried into N4(a).
- Backpressure and flow control. A rate property of the live channel with no
  content-addressing analogue. It is mutable session state (windows, buffers).
- Partial messages. A hash names a complete object. BLAKE3 of a prefix is not
  BLAKE3 of the whole, so a half-received message cannot be hash-verified.
  nettest verifies only after a full block arrives and rejects a bad length. [E]
  kernel/nettest.c. Content-addressing therefore operates at message granularity,
  not byte granularity.

### (b) Byte-stream fundamentally non-content-addressable

True of the stream taken as one object. An ever-growing history has no fixed
content, so it has no fixed hash. False once the stream is quantized into messages,
because each message is fixed content. [C]

### Adjudication

The stream is non-content-addressable as a single object and content-addressable
as a sequence of message objects joined by an external ordering structure
(sequence numbers or a hash chain). Message-as-object holds for integrity, dedup,
and per-message identity, all shown. It needs an added ordering axis for sequence,
shown to be needed in nettest. It does not apply at all to liveness, backpressure,
or sub-message partial delivery. Content-addressing lives at the message
granularity and stops at the live and the not-yet-existing.

---

## N3 ANTI-AMPLIFICATION AS A NETWORK PROPERTY

This is the strongest CARMIX-native angle.

Conventional ambient model. Any code opens any socket to any address. A process
holds ambient network authority, so a compromised process reaches the whole
internet. There is no ceiling on reachability and no anti-amplification. [PA]
Berkeley sockets.

CARMIX model. Network access is a bounded capability. A capability names exactly
which peers or channels a process may reach, by naming their endpoint descriptors
(N1b). There is no ambient reachability. A child spawned with a subset cannot
reach a peer its parent could not, because spawn refuses any cap the parent does
not hold. [E] kernel/U1_LOG.md U1-1. A delegated channel cap attenuates at every
hop and cannot be widened. [E] kernel/U4_LOG.md U4-2. Holding the cap for an
endpoint descriptor hash is the right to communicate on that one channel and
nothing more. [C]

Connection to the distributed anti-amplification already banked. The migration work
made the single-machine ceiling a cross-machine property: a migrant cannot arrive
at the destination with more authority than the source signed, and any post-sign
widening breaks the Ed25519 and is rejected fail-closed, re-minting nothing. [E]
kernel/MIGRATION_AUTHSET_LOG.md (DM5, DM6). If network endpoints are entries in
the authority set, the same unmodified gate bounds network reach across migration.
The ceiling "destination authority is at most source authority" reads, for the
network case, as "destination reachability is at most source reachability." A
migrated process cannot acquire network reach it did not carry. [C]

Honesty on maturity. Anti-amplification over synchronous IPC and over the ivshmem
migration wire is established. [E]. Network reachability as a capability is a design
extension that reuses the same gate but is not built, because there is no NIC and
no socket layer. [O]. The cross-machine reachability ceiling is in-principle: it
follows from the proven migration gate applied to endpoint caps, given a real
transport. [P].

---

## N4 THE BOUNDARY (honest negative)

Where does networking force the thesis to break?

### (a) Liveness and time

A BLAKE3 hash is a function of bytes that already exist. Content-addressing is a
statement about a fixed value. A connection is defined by real-time liveness, that
is by the peer answering now. There is no content hash for "the peer is alive,"
because liveness is a temporal predicate, not a value. This is the first
irreducible break. Content-addressing cannot express "now." nettest shows the
shape of the workaround and its limit: it has no timer, so "now" is a counter the
destination advances each case, and expiry is a tick deadline against that counter.
[E] kernel/nettest.c (b_now). That is a proxy for time, not time. Real liveness
needs a real clock and real timeouts, which sit outside the content-addressed
model.

### (b) External parties

A non-CARMIX peer does not honor CARMIX capabilities. The anti-amplification
ceiling is enforced by the CARMIX kernel at its own crossings. A remote peer
running a conventional stack has no CARMIX gate, so the ceiling cannot be enforced
on the far side of a real link. This is exactly the distrusting migration boundary.
The cardinal point of the distrust work is that content-addressed state re-verifies
with zero trust in the sender by re-hash, which collapses distrust to a single
residue, the authority, and anchoring that residue needs a hardware root of trust,
which the current build only emulates. [E] kernel/MIGRATION_DISTRUST_LOG.md. The
split carries onto a real network cleanly. Content integrity you can enforce
unilaterally, because you can re-hash any bytes you receive regardless of who sent
them, and this is the CARMIX advantage that survives contact with an arbitrary
peer. Authority and reachability on the peer's side you cannot enforce, because the
peer sends what it wants. The ceiling is a property of the local endpoint's grants,
not of the remote party. Attesting an external party without a shared hardware root
or PKI is out of reach, and even with a root it is future work gated on physical
bring-up. [O].

### (c) Mutable session state

Sequence numbers, windows, buffers, retransmit and congestion state. These are
mutable, per-connection, and have no fixed content, so they are the swcap-style
authority-over-mutable-region flavour, not the content-addressed flavour. The
nettest sequence counters, the mailbox records, and the b_now clock proxy are all
instances. [E] kernel/nettest.c. They are expressible as authority capabilities
over mutable state. They are not expressible as content-addressed objects.

### The exact construct the thesis cannot express as a content-addressed capability

A live connection, specifically the conjunction of two things. First, liveness, the
temporal predicate that the peer is responding now. Second, open-endedness, an
ever-growing ordered byte stream with mutable per-connection control state. No
BLAKE3 hash can name an ongoing responsive stream, because there is no fixed byte
string to hash and part of the string does not exist yet.

The boundary is precise. Content-addressing extends to every part of a connection
that is frozen, meaning the descriptor and each completed message. It stops exactly
at the live, meaning the temporal predicate, the not-yet-existing future bytes, and
the mutable control state. The capability model reaches one step further than
content-addressing, because the authority and anti-amp axis can gate the live
channel (who may reach it, bounded so authority never amplifies) even in the region
where content-addressing cannot name anything.

---

## N5 DEMONSTRABILITY PARTITIONED

### Core, testable now on single-CPU or loopback or shared memory

- Message-as-object with hash integrity. Already shown on the ivshmem wire,
  including leaf-site and node-site tamper rejection. [E] kernel/nettest.c. A
  loopback channel abstraction on one machine reuses it directly.
- Capability-gated channel access. An endpoint descriptor as a content-addressed
  object (U-4 pattern), a capability naming it, and spawn, IPC, and delegation
  anti-amp already enforced. [E] kernel/U1_LOG.md, kernel/U4_LOG.md. A loopback
  channel gated by a capability is testable now. [O for the specific channel glue,
  E for every gate it would reuse.]
- Ordered message sequence via a monotonic counter. Already the nettest model. [E].
- Dedup via prune-if-resident. Already shown. [E] kernel/nettest.c.
- Descriptor immutability and re-hash verification. Already the namespace model. [E]
  kernel/U4_LOG.md.

### Hardware-blocked, gated on physical bring-up

- Real NIC driver. Explicitly deferred in nettest, which chose ivshmem over
  virtio-net because a full NIC plus ring plus TCP/IP stack was too heavy for one
  pass. [E note] kernel/nettest.c header. No NIC means no real packets. [O].
- Real interrupts and asynchronous I/O. The build is single-CPU and cooperative,
  with no device interrupts driving a live socket. [O].
- External peers. A non-CARMIX endpoint over a real link needs a NIC and a network
  and raises N4(b). [O].
- Real time. Real timeouts, backpressure, and liveness need a real timer. nettest
  uses a counter proxy. [O].
- Hardware root of trust for distrusting external peers. Emulated today, needs
  TPM, SGX, SEV, or TDX on physical hardware. [O] kernel/MIGRATION_DISTRUST_LOG.md.

The partition is clean. The content-addressing and capability-gating half is
demonstrable now, and most of it is already demonstrated over shared memory. The
liveness, external-party, and real-transport half is hardware-blocked.

---

## VERDICT

Honest hybrid. This much is native, this specific part is an irreducible boundary.

Native model, shape stated. A connection decomposes into four parts.

1. Channel identity is an immutable content-addressed endpoint descriptor. [C],
   reusing [E] U-4.
2. Frozen history is an ordered sequence of content-addressed message objects,
   giving integrity and dedup, with an external ordering axis (a monotonic counter
   or a hash chain) supplying the sequence. Integrity, dedup, and the need for the
   ordering axis are all [E] on the ivshmem wire.
3. Reachability is a bounded anti-amplification capability. Holding a channel cap
   is the right to reach exactly that endpoint, attenuating on delegation and
   bounded across migration by the same gate. [C] and [P], reusing [E] the proven
   spawn, IPC, delegation, and migration ceilings.
4. The live part, meaning the liveness predicate, the future bytes, and the mutable
   session state, is authority-gated but not content-addressed.

Irreducible boundary, stated exactly. Part 4 is the boundary. A live connection
cannot be expressed as a content-addressed capability, because content-addressing
names a fixed value that already exists and a live connection is defined by
time-varying behavior over bytes that do not yet exist. Part 4 is expressible in
the authority dimension, the same dimension as swcap and the U-1 endpoint, but not
in the content-addressing dimension. Separately, the anti-amplification ceiling
cannot be enforced on a non-CARMIX peer, so on a real network only content
integrity is unilaterally enforceable (re-hash), while the reachability ceiling
binds only the local side.

The result is symmetric and worth stating plainly. Content-addressing reaches
exactly as far as the frozen. The capability and anti-amplification model reaches
one step further, over the live channel, but stops at the machine boundary because
a foreign peer honors no CARMIX gate. The union of the two covers everything about
a connection except the peer's own conduct, which no local mechanism can bound.

---

## BORROWED VS NEW

None of the ingredients is individually novel. The contribution, where there is
one, is the combination, and it is flagged as such.

- Sockets and TCP/IP. [PA] and the anti-model. Berkeley sockets grant ambient
  network authority, any process to any address. CARMIX rejects this. Not a CARMIX
  contribution.
- Object-capability networking and CapTP (the E language, Mark Miller and others).
  [PA]. No ambient authority, communication only through capabilities you were
  granted, reference passing. CARMIX shares the no-ambient-authority stance. What
  CARMIX adds on top is the quantified anti-amplification ceiling, destination at
  most source, enforced structurally and shown transitively along delegation chains
  and across machines in migration. CapTP passes capabilities. The attenuation
  ceiling as a structural invariant is the overlay.
- Cap'n Proto capability RPC (Kenton Varda). [PA]. Capability-based RPC with
  promise pipelining and capabilities over the wire, same object-capability
  lineage. CARMIX differs by content-addressing the transported objects and state
  by BLAKE3 and by the anti-amp ceiling. Cap'n Proto is a serialization and RPC
  system, not a content-addressed capability operating system.
- IPFS and libp2p (Protocol Labs). [PA] for content-addressing network data at
  scale, with content identifiers, a DHT, and dedup. CARMIX shares
  content-addressing of objects and dedup. IPFS has no capability or
  anti-amplification model by design, since it is a public content-distribution
  network where anyone may fetch any block. The capability and anti-amp dimension
  is absent there.
- Named Data Networking, NDN (Van Jacobson and others). [PA], and the closest
  prior art, so the distinction must be sharp. NDN names network data. You request
  data by name, routers cache it, and integrity travels with the data by signature.
  At the data layer this is the same shape as message-as-named-object in N2. The
  sharp distinction: NDN has no capability authority model and no
  anti-amplification, because anyone may request any named data, which is the
  intended property of a data-dissemination architecture. CARMIX's contribution is
  orthogonal to what NDN provides. NDN answers how to name and fetch data by
  content. CARMIX answers who is authorized to reach a channel, bounded so
  authority never amplifies, and puts the content-addressed data behind that gate.
  In one line, NDN is content-addressing without capabilities, and the CARMIX angle
  is content-addressing with the anti-amplification capability gate.
- Actor and message models (Hewitt actors, Erlang). [PA]. Isolated actors,
  addresses, asynchronous messages, no shared state. CARMIX per-process isolation
  with capability IPC is actor-shaped. Actors address a mutable mailbox rather than
  content-addressing the message body, and classic actors carry no attenuation
  ceiling on address acquisition.

Overclaim guard. Content-addressing, object-capabilities, anti-amplification as
attenuation under the principle of least authority, message-as-named-data, and
attestation-gated transfer each pre-exist CARMIX. The claimed contribution is the
combination: content-addressed objects behind a structurally enforced
anti-amplification ceiling, unified across spawn, IPC, namespace, and migration,
and, for networking, extended in-principle to reachability and to message data. The
one near-original cell worth naming carefully is content-addressed capability
networking with an attenuation ceiling, which is the NDN-plus-capabilities-with
-attenuation position. To the author's knowledge that cell is not occupied by the
named prior art, but this is a claim about the combination and not a proof of
first-ness, and no exhaustive survey was done. Flagged as in-principle and as a
claim about novel combination, not standalone discovery. No preprint is cited in
this note. Any external work referenced above is published prior art by name, not a
preprint.
