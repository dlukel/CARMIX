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
- A learned predictor and a proven-optimal fairness algorithm. A rematerialization-aware scheduling
  policy exists, and a fairness control over the rematerialization penalty exists. The policy keeps a
  descheduled process resident by default and dematerializes only under memory pressure when the
  process is predicted to stay descheduled long enough to beat the measured rematerialize cost, with a
  break-even computed live from that cost and an anti-thrash backoff. The fairness control measures the
  per-process progress deficit a repeatedly-rematerialized process accumulates and biases the policy to
  keep a deficit-carrying process resident so it stops paying the penalty and its progress rate
  recovers, bounded so it never starves the resident peer. See docs/ARCHITECTURE.md. What does not
  exist is a learned predictor (the heuristic uses cheap fixed signals, and the live two-process
  workload only ever quantum-preempts, so the blocked and long-sleep signals are exercised by probes),
  a proven-optimal fairness algorithm (the control bounds future deficit growth and recovers the
  progress rate but does not refund the penalty already paid, and it accounts for one peer, not many),
  dematerializing the page-table root as well (only the register and data state dematerialize today), a
  brk or mmap process heap, dynamic linking, more than two concurrent processes under real load,
  blocking and waking on input and output, and binding the carried capability to a trustworthy source
  through signing.
- A complete content-addressed task object. The task state object today is the registers and the
  stack only. The page table and the capability slots are not yet part of it. A residency manager now
  gives tasks per-task page mappings, and dirty tracking is measured both ways, the x86-64 page-table
  dirty bit and the software chunk diff agree on the dirty set and the hardware scan is six to seven
  times cheaper. See docs/ARCHITECTURE.md. The wider state object remains future work.
- Recoverable nested faults. Demand paging is now automatic. A not-present access traps through the
  page-fault vector, the handler materializes the object by hash with BLAKE3 verification and
  resumes, and a verification failure fails closed. See docs/ARCHITECTURE.md. The handler's own
  memory is resident so servicing a miss does not itself fault, but a fault during fault handling
  fails closed rather than recovering. Making nested faults recoverable needs a separate interrupt
  stack and a per-fault service context, which is not built.
- A scheduling policy that uses content-addressing per switch. The measured crossover shows
  activate-by-hash costs two to three orders of magnitude more than a register switch at every
  dirty-set size, so it is used only at coarse boundaries. A policy that schedules over content
  hashes at switch frequency is not viable on this hardware and is not built.
- A filesystem and persistent process state. There is no filesystem, and a task's state lives only
  in RAM.
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

The key lifecycle is built. A destination learns a source key by authority-signed enrollment,
rotates and revokes it under the authority key, and rejects replay and downgrade by monotonic
epochs. See docs/SECURITY_MODEL.md. What remains open is below.

- The root of trust. The destination's first authority public key is baked in. A certificate
  authority, identity proofing, or an out-of-band channel to obtain and prove that first key is the
  irreducible bootstrap assumption, not built.
- Persistent trust and nonce state. The trust store, the lifecycle epoch, the revoked set, and the
  used nonces live in memory. The kernel has no disk, so a restart re-bootstraps from the baked
  authority key at epoch zero. A real system must persist this state without rollback.
- A trustworthy clock. The expiry check needs a monotonic, trustworthy time source. The kernel has
  a tick counter.
- Revocation scale and a single authority. The revoked set is a small fixed in-memory list with no
  propagation, and one authority key whose compromise is unrecoverable. Distribution and a
  multi-authority quorum are not built.
- Real network transport. The transport is ivshmem shared memory between two local instances. A
  real NIC, virtio-net, RDMA, or CXL memory is future work.

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
