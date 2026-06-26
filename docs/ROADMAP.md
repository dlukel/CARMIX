# Roadmap

This is the honest list of what is not built. It is the consolidated set of open items that used
to live as inline markers in the code. An item here means the work is not done, not that it is
hidden.

## Hardware and platform

- Physical hardware. Everything runs in QEMU. The bootable image runs the same path from a USB
  stick, but that has not been verified on a real machine.
- USB input. Input is PS/2, which the emulator provides. Real modern hardware needs a USB HID
  stack over xHCI.
- Persistent storage. There is no storage driver. NVMe or AHCI is needed.
- A filesystem, a process model, and a userspace. None of these exist. The kernel runs a single
  flow of self-tests and a desktop loop.
- GPU acceleration. The desktop is software-rendered and draws CARMIX's own windows. Running
  third-party graphical applications would require the vendor GPU driver stack, which is out of
  scope.

## Input and desktop

- Interrupt-driven input. Keyboard and mouse are polled. An interrupt-driven ring buffer is the
  next step.
- Console that follows focus and per-window consoles. The console is bound to the window that was
  in front when it started.
- Window close, minimize, more than two windows, and a window list.
- Dirty-rectangle redraw. The compositor does a full recompose on each change.
- A real cursor layer. The cursor is drawn with a save-under buffer, which can go stale if the
  area under the cursor changes between moves.
- Backspace across a wrapped line edge. Backspace steps back within the current line only.

## Two-machine migration

- Real key distribution, a public-key infrastructure, and revocation. The signed-authorization
  demo uses baked test keys with the source public key hard-coded. There is no mechanism for a
  destination to learn, rotate, or revoke a trust root.
- Persistent nonce state. The replay defense holds used nonces in memory and would forget them
  across a restart.
- A trustworthy clock. The expiry check needs a monotonic, trustworthy time source. The kernel
  has a tick counter.
- Real network transport. The transport is ivshmem shared memory between two local instances.
  A real NIC, virtio-net, RDMA, or CXL memory is future work.

## Proofs

- A machine-checked refinement of the C implementation against the abstract capability algebra.
  The proof covers the abstract model. The runtime correspondence is demonstrated by the kernel
  attack tests, not yet proven.
- Binding a verified hash implementation. A future phase could replace the vendored BLAKE3 with a
  formally verified hash. Before that, confirm that a verified BLAKE3 exists. Verified BLAKE2 is
  available in existing verified crypto libraries, but CARMIX uses BLAKE3 specifically.
- Temporal-safety with generational handles. This would modify the capability handle structure
  and therefore needs review against the proven core. It is a separate piece of work, not folded
  into the current model.
