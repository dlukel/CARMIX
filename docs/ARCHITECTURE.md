# Architecture

This walks the whole system from the lowest layer up. Each module section says what the module
does, the invariant it holds, and the file that implements it.

## Capability model (cap/)

A capability is a token of authority. In this model it is a record of a base address, a length,
a permission set, and a validity tag. The capability model defines two operations.

- Strip. Given a live capability and a reference to the object it points at, produce a plain
  descriptor called a PIR (a position-independent reference). The descriptor carries the offset
  within the object, the length, the permissions, and the content hash of the referent. It does
  not carry a live authority tag. cap/cap_strip.c.
- Re-mint. Given a destination root capability, a descriptor, and the destination's instantiation
  of the referent object, mint a fresh local capability. The re-mint is fail-closed. It checks,
  in order, that the descriptor is well formed, that its bounds fit within the source region,
  that its permissions are a subset of the source permissions, that write and execute are not
  both requested, and that no permission from a forbidden set is present. Any failure returns an
  error and produces an invalid capability. cap/cap_custodian.c, cap/cap_remint.S.

Invariant. A re-minted capability's base is no lower and its limit no higher than the source
region, and its permissions are a subset of the source permissions. This is the anti-amplification
property, proven abstractly in proofs/Carmix.v.

The model is target-independent. On a capability machine the bounds are enforced in hardware. The
software backend in carmix/ enforces the same model with no capability hardware.

## Content-addressed object store (store/)

The store maps a BLAKE3 hash to bytes. Putting bytes returns their hash and stores one copy.
Putting identical bytes again returns the same hash and stores nothing new. There are no paths,
no keys, and no overwrite. Changing data means producing a new object with a new hash.

BLAKE3 is vendored. The store hashes only at commit and epoch boundaries, never inside an
execution loop. store/object_store.c, store/blake3_wrap.c, store/blake3/.

The hash function is correctness and safety critical. A wrong digest would break deduplication and
let two distinct objects collide to one address. The vendored BLAKE3 is validated against the
official known-answer vectors. The kernel re-runs that known-answer check at startup before it
trusts any content-addressed result.

## Single-level store and structural diff (sls/)

The single-level store builds an object graph on top of the store. An object's identity is the
hash of its content, including its children's identities. A leaf holds opaque bytes. A node holds
an ordered list of child identities. Mutation never edits in place. It produces a new object and a
new epoch, and unchanged sub-objects keep their identities, which is structural sharing.

The structural diff takes two epoch roots and returns the set of changed objects. It walks the
graph, prunes any subtree whose hash is unchanged, and memoizes visited identity pairs so a shared
changed subtree costs constant work rather than work proportional to the number of paths to it.
The diff calls no hash function and resolves only as many objects as actually changed. sls/sls.c.

Invariant. The diff between two epochs is exactly the set of changed objects, computed in time
proportional to the change.

## WebAssembly safety gate (gate/)

The gate runs untrusted WebAssembly under software-fault isolation. It parses and validates a
WebAssembly subset, lowers it to a small typed intermediate representation, and runs a linear-time
checker that rejects any module that could escape its linear memory or forge a capability. A
backend performs the bounded memory accesses. gate/wasm_frontend.c, gate/sfi_checker.c,
gate/executor.c.

The checker is the trusted computing base of the gate. Its guarantee holds relative to the set of
operations it models. The static elision pass (gate/optimize.c) drops a runtime bounds check only
for an access it has proven in bounds, and the checker rejects the elided opcodes in untrusted
input so they cannot be injected.

## Software capability backend (carmix/)

The software backend enforces the capability model with no capability hardware. A software
capability is the record of base, length, permissions, and a validity tag held in trusted runtime
state. The guest reaches an object only through a bounds-checked access, not through a raw token.

The re-mint logic here is the same decision logic as cap/, ported to software records. The bounds
and permission checks are integer comparisons. There is no rounding, so a length that a hardware
capability would have to round is minted exactly. carmix/swcap.c, carmix/backend_sw_gate.c.

This backend is what the bootable kernel uses. It runs on commodity hardware with no capability
instructions in the binary.

## Kernel (kernel/)

The kernel boots on x86-64 through the Limine boot protocol, which hands it long mode, page
tables, a memory map, and a linear framebuffer. Everything above the entry point is written here.
kernel/kernel.c.

- Serial. A 16550 UART driver on COM1 for debug output, brought up first.
- Interrupts. An interrupt descriptor table with handlers for the CPU exception vectors and a
  catch-all over the 0 to 31 range, so a fault prints a register dump and halts rather than
  triple-faulting silently.
- Memory. A physical frame allocator over the usable memory map, fail-closed when exhausted, and
  a four-level page mapper.
- Framebuffer. Direct pixel writes to the firmware-provided linear framebuffer, with a bitmap
  font for text.
- Timer. A periodic interrupt from the PIT driving a tick counter.
- Input. PS/2 keyboard and mouse, polled. The mouse decodes the standard three-byte packets.
- Console. A scrolling text region that shifts the framebuffer rows up when the cursor passes the
  bottom.
- Desktop. A software compositor that draws a background and windows back to front, with a
  save-under cursor. Windows support focus raise on click, drag by the titlebar, and resize by a
  corner grip.
- In-kernel executor. The gate and the software capability backend, linked into the kernel, run a
  WebAssembly task and draw its result.
- In-kernel rematerialization. The store and the single-level store, linked into the kernel,
  checkpoint a counter, rematerialize it, and demonstrate the diff-proportional structural diff.

## Task substrate and scheduler hook (kernel/kernel.c)

The kernel runs and switches between tasks. A task is a saved execution context, an id, a stack, a
saved stack pointer, a state of ready, running, or done, and a reserved content-address handle
named remat_root that is unused today. A small assembly routine, switch_to, saves the running
task's callee-saved registers and stack pointer and restores the next task's, which is a real
context switch, not a function call. A cooperative yield calls it directly. The timer interrupt
calls it too, so a task that never yields is still preempted on a tick.

The scheduling policy is isolated as a single function, pick_next, which is round-robin. It and the
save and restore in switch_to are the seam, and the policy is a deliberate placeholder.

On top of this the kernel makes a task a content-addressed object. The remat_root handle now holds
the BLAKE3 hash of a task's state, its registers and the used part of its stack, serialized into the
proven store. A context switch can then run as activate-by-hash, the task is dematerialized to a
hash and later materialized back from the store, which is rematerialization applied to task state.
The same state hashes to the same handle and different state to a different handle. Persistence falls
out of this, a task dropped and resumed from only its hash and the store resumes correctly, the live
and durable forms being one object.

The cost is measured, not assumed. A raw register switch is a flat cost near 1400 cycles. The
activate-by-hash path costs two to three orders of magnitude more even for a single dirty chunk,
because hashing dominates, and there is no dirty-set size at which it beats the register switch. So
the fast switch_to stays the path for rapid preemption, and activate-by-hash is reserved for the
coarse, infrequent boundaries that already touch the store, yield-to-migrate, checkpoint, and
persistence-as-noop, where deduplication, integrity by construction, and diff-proportional transfer
pay for the hash. The page table and capability slots are not yet part of the task state object, and
the scheduling policy itself stays a placeholder, the measurement shows it does not collapse into
rematerialization at switch frequency.

## Residency manager (kernel/kernel.c)

Physical memory is managed as a content-addressed cache over the proven store. The classification of
each operation is fixed. Fresh allocation stays conventional. Fault-in and sharing collapse into
rematerialization. Free, eviction, and defragmentation partially collapse. Mapping stays
conventional.

The conventional floor is a physical frame database (every frame free, resident, or pinned, with a
refcount and a dirty flag), per-task four-level page tables with map, unmap, and TLB invalidation,
and a temporal eviction policy. frame_reserve is the one conventional allocator, since a fresh frame
has no content identity until it is written.

The collapsed operations are rematerialization. materialize(hash) fills a frame from the store and
verifies that the BLAKE3 of what it loaded equals the requested hash, so integrity holds by
construction. A request for a hash that is already resident maps the existing frame and bumps its
refcount, so identical content is one physical frame shared read-only. refdrop releases a logical
reference, and the last drop makes the frame reclaimable. Eviction writes a dirty victim back to the
store before its frame is reused, and victim selection is temporal, not keyed on content.

The findings were measured in the emulator and vary run to run because they are real:

- Sharing a hash at admission re-materializes zero bytes and costs far less than a second fault, so
  deduplicating on the way in beats a post-hoc merge.
- The temporal eviction policy retained the hot working set under oversubscription while the cold
  stream missed.
- The hardware page-table dirty bit and the software chunk diff agree on the dirty set, and the
  hardware scan is roughly six to seven times cheaper. This closes the question the unified substrate
  left open. The hardware bit is the cheap coarse filter and content-addressing is the fine
  mechanism, and they compose.
- Fixed chunks trade external fragmentation for internal slack, near 29 percent on the measured mix,
  and a frame-sized request stays serviceable.

The page table and the capability slots are not yet part of the content-addressed task state object.

## Rematerializing fault handler (kernel/kernel.c)

Demand paging is automatic. A not-present access traps through the page-fault vector, the handler
classifies the fault (a protection fault is not a miss), resolves the faulting address to a content
hash, materializes the object from the store with BLAKE3 verification, installs the mapping, and
returns so the faulting instruction re-executes and now succeeds. A miss is a verified
materialize-by-hash.

Fail-closed is enforced at the fault boundary. If the loaded bytes do not hash to the expected hash,
the handler installs no mapping and does not resume, it fails loud. Two virtual addresses bound to
the same hash share one resident frame, so deduplication happens on the fault path, not only on an
explicit call.

The one new decision, how a faulting address resolves to a hash, was settled by measurement. Two
bindings are built, a side table keyed by address range and a not-present page-table entry that
points at a descriptor, and their fault-service latency was measured head to head. The result is a
null finding. The binding choice does not move fault-service latency, because the cost is dominated by
the materialize and the BLAKE3 verification, a page copy plus a full hash, and the resolution step
sits in the measurement noise.

The handler's own code, stack, and binding metadata are resident, so servicing a miss does not itself
fault. Recoverable nested faults are not supported. A fault during fault handling fails closed rather
than recovering, and making it recoverable needs a separate interrupt stack and a per-fault service
context, which is named as future work.

## Authority-bounded domain crossing (kernel/kernel.c)

The kernel runs a process in ring 3 on its own page tables. The hardware privilege floor is
conventional, GDT user segments, a task-state segment with a kernel stack, a ring transition through
a trap gate, and the user and supervisor bit on the page tables. A ring-3 access to a kernel address
faults and is classified as a protection violation, not a miss.

Entering the kernel is not an ambient privilege. A process carries a capability re-minted under the
authority ceiling, and a privileged crossing re-mints the caller's bounded authority for the
requested service through the same anti-amplification gate that bounds a migration and a fault. The
authority that reaches the kernel is the re-minted bounded one, not the caller's claim. A confused
deputy that hands the kernel a wider claim is held to the re-minted authority.

The proof mirrors the migration adversarial table. Six userspace amplification attempts, an unheld
capability, an over-ceiling request, a forged epoch, a confused deputy, a write-execute violation,
and a forbidden permission, are each refused by their own distinct reason, and these are the same
status codes the in-kernel anti-amplification table and the cross-machine migration table reject by.
A replay is refused and a legitimate crossing within the ceiling is accepted. The same gate that
refuses amplification at the migration boundary refuses it at the user and kernel boundary, so
crossing, migrating, and faulting are one authority model.

A large syscall argument is passed as a hash and rematerialized by hash with verification, rather
than copied from user memory. This closes the copy-at-use class of time-of-check to time-of-use race,
since a mutated object is a different hash and fails verification. It does not eliminate trust, it
relocates it to the address-to-hash binding and the store admission, which is stated honestly rather
than claimed away.

## Process loader (kernel/kernel.c, kernel/user_prog.S)

A program is a content-addressed object, and loading a process is materialization. A freestanding
ring-3 ELF64 is stored in the content-addressed store, the loader parses its program headers, and for
each loadable segment it materializes the bytes by hash with BLAKE3 verification into a fresh ring-3
address space at the segment's virtual address, with the segment's permissions and no segment both
writable and executable. The user stack is mapped, and the process enters ring 3 at the ELF entry
point and runs its own code, which was materialized from a hash rather than linked into the kernel.

The process runs under a capability re-minted through the anti-amplification gate at load time, so
its authority is bounded at birth. An over-ceiling or unauthorized request is refused by the same
gate that refuses a migration. Loading the same image twice gives two processes whose read-only code
resolves to one physical frame by hash while their writable data and stacks are private. Loading
therefore joins faulting, migrating, and crossing as one operation, rematerialization under the
proven gate, with integrity gated by construction. The image is embedded in the kernel because there
is no filesystem yet, and the loaded program is a static executable.

## Concurrent processes and descheduling as rematerialization (kernel/kernel.c)

The scheduler runs more than one ring-3 process at once. Each process carries its own page-table
root, its own user instruction and stack pointers, and its own re-minted ceiling, and the timer
preempts a running process and restores another exactly. Two processes run interleaved, each bounded
by its own ceiling, and neither can exercise the other's authority.

A descheduled process is a content-addressed object. Instead of keeping a descheduled process
resident, the scheduler dematerializes its schedulable state to a hash in the store and releases its
frames, so that not running and not resident become different states. When the scheduler selects it
again, the state is rematerialized from the hash with verification and the process resumes where it
left off, its counter surviving the round trip. Descheduling joins faulting, migrating, crossing, and
loading as rematerialization under the proven path.

The cost is measured. Resuming a resident process is a register and page-table-root restore near ten
thousand cycles, resuming a dematerialized process is a store fetch, a verification, and a remap near
half a million cycles, about fifty times more. So dematerializing on every deschedule would be
wasteful. The mechanism is built, the policy that decides when to use it, based on how long a process
is expected to stay descheduled against this cost, is a named future seam, and pick_next stays a
round-robin placeholder. Today the register context and the private data page dematerialize, the
page-table root stays resident, dematerializing the full page-table state is the open wider
task-state object.

The policy that decides when to dematerialize is rematerialization-aware. By default a descheduled
process is kept resident, since resuming it is far cheaper. Dematerialization happens only when the
residency manager signals memory pressure and the process is predicted to stay descheduled long
enough to repay the rematerialize cost. The prediction uses cheap signals, a quantum preemption
predicts a short absence and keeps the process resident, a block or a long sleep predicts a long
absence and makes it a candidate, and repeated wakeups keep a hot process resident. The break-even
duration is computed live from the measured resume cost rather than a guessed constant, so a process
descheduled longer than the break-even nets a gain and a shorter one nets a loss. An anti-thrash
backoff keeps a just-rematerialized process resident so it is not dematerialized again immediately.
This is the local measured rule. It does not make scheduling free and it is not a unified scheduling
model.

A fairness control sits on top of the policy. A process that is repeatedly dematerialized pays the
rematerialize cost on each return, so over equal scheduling turns it makes less progress than an
always-resident peer, a measured per-process deficit. The control detects the deficit and biases the
policy to keep the deficit-carrying process resident, which removes the penalty source so its progress
rate recovers, bounded by a cap so it never starves the peer or overshoots. This bounds future
unfairness and recovers the rate. It does not refund the penalty already paid, and it accounts for one
peer rather than many, so it is a first measured control on a lightly-researched dimension, not a
proven-optimal fairness algorithm.

## Two-machine migration (kernel/nettest.c)

Two emulator instances share memory through an ivshmem device, a PCI device whose second base
address register maps a host file shared between both instances. The kernel enumerates PCI,
finds the device, maps the shared region, and uses it as a byte channel.

The migration uses a destination-pull protocol over the object graph.

1. The source advertises its epoch root hash.
2. The destination walks the root's children top down, marking the hashes it lacks.
3. The destination requests the missing hashes in a batch.
4. The source streams the raw blocks.
5. The destination verifies each block's BLAKE3 hash before installing it, and prunes any subtree
   whose hash it already holds.

A cold sync transfers the full graph. A warm sync after a small change transfers only the changed
objects. A block whose recomputed hash does not match the requested hash is rejected fail-closed,
so a corrupted or forged block cannot be installed.

## Signed authorization (kernel/nettest.c, kernel/tweetnacl.c)

Hash verification proves that bytes match their advertised hashes. It does not prove that the
source was authorized to make the destination rematerialize this state. The authorization record
closes that gap.

The record holds the epoch root hash, a computation identifier, a destination identifier, an
authority ceiling, a nonce, and an expiry. The source signs it with Ed25519. The destination,
before it installs and re-mints, checks in order that the signature is valid under the expected
source key, that the signed root matches the root it actually pulled, that the destination and
computation identifiers match, that the nonce is unseen and not expired, and finally re-mints
capped at the authority ceiling through the anti-amplification gate. Each check fails closed with
its own reason. The Ed25519 implementation is vendored tweetnacl. See kernel/TWEETNACL_PROVENANCE.txt.

## Key lifecycle (kernel/nettest.c)

The destination does not trust a single baked source key. It holds a trust store of an authority
public key, the current source public key, a monotonic lifecycle epoch, and a permanent revoked
set. Three authority-signed records drive it. An enrollment sets the first source key, a rotation
replaces it, and a revocation adds a key to the revoked set. A lifecycle record applies only if it
verifies under the authority key and its epoch strictly exceeds the stored epoch, which is what
rejects replay and downgrade.

Each migration carries its signing key. Before the five authorization checks, the destination
checks that the signing key is the current source key, else unknown-key, and that it is not
revoked, else revoked-key. These two reasons are distinct from each other and from a bad signature.
The authority key is the one baked root of trust whose bootstrap is out of scope. See
docs/SECURITY_MODEL.md.

The proven modules ship exactly as verified. The kernel, the network harness, the proofs, and the
documentation are the new code that uses them.
