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
