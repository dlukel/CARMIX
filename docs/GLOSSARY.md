# Glossary

Anti-amplification. The property that a capability minted at a destination grants no more authority
than the source held. Its bounds are no wider and its permissions no greater. The re-mint gate
enforces it, and proofs/Carmix.v proves it for the abstract model.

Capability. A token of authority over a region of memory with a set of permissions. In this system
it is a record of a base, a length, a permission set, and a validity tag.

Content-addressed. Named by the hash of the content. In this system an object's identity is the
BLAKE3 hash of its bytes, including its children's identities for a node. Identical content has one
identity and one stored copy.

Destination-pull. The two-machine protocol where the destination drives the transfer. The source
advertises a root, the destination requests the object identities it lacks, and the source streams
those blocks. The destination verifies each block's hash before installing it.

Diff-proportional. A transfer whose size tracks the size of the change rather than the size of the
total state. A migration after a small change moves only the changed objects.

Epoch. A version of the object graph identified by its root hash. Mutating an object produces a new
epoch with a new root, and unchanged sub-objects keep their identities across epochs.

Gate. The WebAssembly load-time safety checker and the re-mint authority check. The WebAssembly
gate rejects a module that could escape its linear memory. The re-mint gate rejects a capability
request that would exceed the source authority.

ivshmem. A QEMU device that maps a host file as shared memory visible to two virtual machines. It
is used here as a byte channel between the source and destination instances.

PIR, position-independent reference. The plain descriptor a capability is stripped to before
migration. It carries the offset, length, permissions, and content hash of the referent, with no
live authority tag.

Rematerialization. Reconstructing a running computation from its content address rather than
copying its bytes wholesale. The state is content-addressed, transferred diff-proportionally, and
re-minted under the anti-amplification gate.

Re-mint. Minting a fresh local capability at the destination from a stripped descriptor, through
the anti-amplification gate.

SFI, software-fault isolation. Enforcing memory safety on untrusted code by software checks rather
than hardware, used by the WebAssembly gate.

Single-level store. The object graph layer that gives every object a content identity and supports
the structural diff. Mutation produces new objects with structural sharing of the unchanged parts.

Strip. Producing a PIR descriptor from a live capability, dropping the live authority tag, before
migration.

Structural diff. The operation that returns the set of changed objects between two epochs in time
proportional to the change, by pruning subtrees with unchanged hashes.
