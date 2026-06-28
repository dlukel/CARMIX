# Glossary

Anti-amplification. The property that a capability minted at a destination grants no more authority
than the source held. Its bounds are no wider and its permissions no greater. The re-mint gate
enforces it, and proofs/Carmix.v proves it for the abstract model.

Capability. A token of authority over a region of memory with a set of permissions. In this system
it is a record of a base, a length, a permission set, and a validity tag.

Content-addressed. Named by the hash of the content. In this system an object's identity is the
BLAKE3 hash of its bytes, including its children's identities for a node. Identical content has one
identity and one stored copy.

Binding, address to hash. The mapping from a virtual address to the content hash of the object that
address holds, used by the fault handler to resolve a miss to a materialize-by-hash. Two bindings
were built and measured head to head, binding A a side table keyed by address range, and binding B a
not-present page-table entry pointing at a descriptor. Their fault-service latency was the same,
because the cost is dominated by the materialize and the BLAKE3 verification, so the binding choice
is a null finding.

Dematerialization. Releasing a resident object's frames and reducing it to its content hash, a hash
token in the store, so that not running and not resident become different states. The inverse of
materialize. An idle page, a descheduled process's register and data state, or a yielded task can be
dematerialized and later rematerialized on demand.

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

Materialize. Filling a frame from the store by content hash and verifying that the BLAKE3 of the
loaded bytes equals the requested hash before the frame is used, so integrity holds by construction.
The inverse of dematerialize. A fault-in, a sharing admission, and a process load are all
materializations.

PIR, position-independent reference. The plain descriptor a capability is stripped to before
migration. It carries the offset, length, permissions, and content hash of the referent, with no
live authority tag.

Rematerialization. Reconstructing a running computation from its content address rather than
copying its bytes wholesale. The state is content-addressed, transferred diff-proportionally, and
re-minted under the anti-amplification gate.

Re-mint. Minting a fresh local capability at the destination from a stripped descriptor, through
the anti-amplification gate.

Residency. Whether an object currently occupies a physical frame. The residency manager treats
physical memory as a content-addressed cache over the store, where a resident object can be shared
by hash, evicted by writing a dirty victim back to the store, or dematerialized to a hash token
under memory pressure, and a non-resident object is materialized on the next fault.

SFI, software-fault isolation. Enforcing memory safety on untrusted code by software checks rather
than hardware, used by the WebAssembly gate.

Single-level store. The object graph layer that gives every object a content identity and supports
the structural diff. Mutation produces new objects with structural sharing of the unchanged parts.

Strip. Producing a PIR descriptor from a live capability, dropping the live authority tag, before
migration.

Structural diff. The operation that returns the set of changed objects between two epochs in time
proportional to the change, by pruning subtrees with unchanged hashes.

swcap. The software capability backend in carmix/. It enforces the capability model with no
capability hardware, holding base, length, permissions, and a validity tag in trusted runtime state
and reaching an object only through a bounds-checked access. Its re-mint logic, carmix/swcap.c
function cvsasx_sw_cap_remint, is the same decision logic as the cap/ model ported to software
records, with integer comparisons and no rounding. This backend is what the bootable kernel links.
