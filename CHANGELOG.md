# Changelog

A factual record of what each verified milestone added. Newest first. Each line states one thing
that was built and observed. Stage codes (B, E, R, S, A, U, M, F, US, L, C, PP, FA, PM) match the
serial self-test names in kernel/VERIFY_PUBLIC.md and docs/REPRODUCE.md. Dates are approximate
where the milestone groups several days of work.

## Per-process heap (PM) - 2026-06

- A ring-3 process grows its own memory through an authority-bounded extend-heap that crosses the
  re-mint gate, with an over-ceiling request refused by the same gate (PM0, PM1).
- The kernel grants a page-granular not-present range and the fault handler backs it demand-zero on
  first write, while the kernel stays blind to the userspace bump allocator's free lists (PM0).
- An idle stable heap page dematerializes to a hash and rematerializes on fault by hash, serviced
  through the page-fault core and verified bit-identical (PM2).
- Two processes that independently write the same bytes deduplicate to one frame by hash with
  copy-on-write, which a shared-ancestor copy-on-write cannot do (PM3).
- A hot churning page is excluded from dematerialization because re-hashing it on every attempt is
  the measured fine-granularity cost (PM4).

## Fairness control (FA) - 2026-06

- A repeatedly-rematerialized process accumulates a measured per-process progress deficit relative
  to an always-resident peer (FA0).
- A bounded control detects the deficit and biases the policy to keep the deficit-carrying process
  resident so its progress rate recovers, capped so it never starves the peer (FA1, FA2).
- Stated honestly as a first measured control on a lightly-researched dimension, not a
  proven-optimal fairness algorithm.

## Rematerialization-aware scheduling policy (PP) - 2026-06

- A descheduled process is kept resident by default and dematerialized only under memory pressure
  (PP0, PP1).
- The dematerialize decision uses cheap signals (quantum preemption keeps resident, a block or long
  sleep makes a candidate, repeated wakeups keep a hot process resident) with measured mispredict
  count (PP2).
- The break-even duration is computed live from the measured resume cost, so a longer absence nets
  a gain and a shorter one nets a loss (PP3).
- An anti-thrash backoff keeps a just-rematerialized process resident so it is not dematerialized
  again immediately (PP4).

## Concurrent content-addressed processes (C) - 2026-06

- Two ring-3 processes run interleaved on the timer, each in its own address space under its own
  re-minted ceiling, with no cross-process authority leak (C0, C1).
- A descheduled process is dematerialized to a hash and its frames released, then rematerialized
  with verification and resumed, its counter surviving the round trip (C2).
- The cost is measured, resume-resident against resume-from-hash, showing dematerializing on every
  deschedule would be wasteful (C3).

## Content-addressed process loader (L) - 2026-06

- A freestanding ring-3 ELF64 is a content-addressed object, parsed by program header, with a
  malformed image rejected fail-closed (L0).
- Each loadable segment is materialized by hash with BLAKE3 verification into a fresh ring-3 space,
  with W^X enforced and no segment both writable and executable (L1).
- The loaded program runs in ring 3 from materialized-by-hash code rather than code linked into the
  kernel (L2).
- The process runs under a capability re-minted through the anti-amplification gate at load time,
  with over-ceiling and added-permission requests each refused by a distinct reason (L3).
- Two processes from one image share read-only code as one physical frame by hash while data and
  stacks stay private (L4).

## Authority-bounded user and kernel crossing (US) - 2026-06

- A ring-3 process enters the kernel through a real ring transition, and a ring-3 access to a
  kernel address is classified as a protection violation, not a miss (US0, US1).
- A privileged crossing re-mints the caller's bounded authority through the same anti-amplification
  gate that bounds migration and faults (US2).
- Six userspace amplification attempts are each refused by their own distinct reason, the same
  status codes the migration table uses, with replay refused and a legitimate crossing accepted
  (US3).
- A large syscall argument is passed as a hash and rematerialized by hash with verification, which
  closes the copy-at-use class of time-of-check to time-of-use race and relocates trust to the
  address-to-hash binding (US4).

## Rematerializing fault handler (F) - 2026-06

- A not-present access traps through the page-fault vector and is serviced by a BLAKE3-verified
  materialize-by-hash that gates resume, with a protection fault classified as not a miss (F0, F1).
- Content that fails verification is refused at the fault boundary, no mapping installed and no
  resume (F2).
- Two virtual addresses bound to the same hash share one resident frame, so deduplication happens
  on the fault path (F3).
- Two address-to-hash bindings were measured head to head and the choice does not move
  fault-service latency, a null finding dominated by the materialize and the hash (F4).
- Re-entrancy is guarded fail-closed and the recoverable nested-fault gap is named (needs IST and
  TSS) (F5).

## Residency manager (M) - 2026-06

- Physical memory is a content-addressed cache over the proven store, with a frame database
  (reserve, free, reuse, conservation, fail-closed exhaustion) and per-task four-level page tables
  (M0, M1).
- Fault-in materializes a frame from a hash and verifies it, and identical content is shared by
  hash as one frame with measured dedup cost (M2).
- Eviction writes a dirty victim back to the store before reuse under a temporal policy (M3).
- The hardware page-table dirty bit is measured against the software chunk diff, they agree and the
  hardware scan is six to seven times cheaper (M4).
- Fragmentation is measured, near 29 percent internal slack on the mix, with a frame-sized request
  still serviceable (M5).

## Unified content-addressed task substrate and measured crossover (U) - 2026-06

- A task's registers and stack hash into the proven store, so a task is a content-addressed object,
  the same state hashing to the same handle and a one-byte change to a different handle (U0).
- Activate-by-hash dematerializes and rematerializes a task through that hash with the counter
  surviving (U1).
- Incremental dematerialization tracks the dirty set, small dirty set transferring far less than a
  large one (U2).
- The cost crossover is measured with rdtsc, a raw register switch flat near 1400 cycles against
  activate-by-hash two to three orders of magnitude more at every dirty-set size, so the fast path
  stays for rapid switching and content-addressing is reserved for coarse boundaries (U3).
- Persistence is a no-op, a task dropped and resumed from only its hash and the store resumes
  correctly (U4).

## Task substrate and scheduler hook (P) - 2026-06

- The kernel creates tasks and switches between them with a real assembly context switch that saves
  and restores callee-saved registers and the stack pointer, driven by a cooperative yield (P0).
- The proven remat path runs inside a task across a switch, a counter checkpointed via the store
  surviving yield and resume (P1).
- A non-yielding task is preempted on the timer interrupt (P2).
- The scheduling policy is isolated as a single round-robin pick_next placeholder.

## Key lifecycle (KA) - 2026-06

- The destination holds a trust store (authority key, current source key, monotonic lifecycle
  epoch, permanent revoked set) instead of a single baked source key.
- Three authority-signed records drive it, enrollment sets the first source key, rotation replaces
  it, revocation adds a key to the revoked set, applied only if the signature verifies under the
  authority key and the epoch strictly exceeds the stored epoch, which rejects replay and
  downgrade.
- Each migration resolves its signing key through the trust store, rejected as unknown-key if it is
  not the current source key and as revoked-key if it is in the revoked set, distinct from a bad
  signature.
- Six lifecycle attacks are each rejected by a distinct reason in kernel/net_repro.sh.

## Two-machine migration and signed authorization (B-track, D2) - 2026-06

- Two emulator instances share memory through an ivshmem device, the kernel enumerating PCI,
  mapping the shared region, and using it as a byte channel.
- A destination-pull protocol over the object graph syncs cold (1093 objects, 44505 bytes) and
  warm after small changes (three hops moving 1176, 1176, and 2219 bytes), tracking the change set
  not the total state.
- A forged or corrupted block is rejected fail-closed on a hash mismatch and not installed.
- A signed authorization record (epoch root, computation id, destination id, authority ceiling,
  nonce, expiry) with real Ed25519 (vendored tweetnacl) gates installation, one legitimate
  migration accepted and seven adversaries each rejected by a distinct reason.

## In-kernel rematerialization (R) - 2026-06

- BLAKE3, the store, and the single-level store are recompiled freestanding and linked into the
  booting kernel, with the official known-answer vector for the empty input matching at startup
  (R0).
- A computation's live state is checkpointed to the content-addressed store (R2) and rematerialized
  bit-identically with a fresh capability re-mint (R3).
- The structural diff is diff-proportional, after a one-leaf change to a 256-leaf state it reports
  one changed object and resolves only a handful, and the same holds at 1024 leaves (R4).
- The result is drawn to the framebuffer and pixel-readback confirmed (R5).

## In-kernel Wasm under the anti-amplification gate (E) - 2026-06

- The software capability backend and the WebAssembly gate link into the kernel with zero undefined
  symbols, zero libc symbols, and zero capability-hardware instructions (E0).
- A WebAssembly task runs under the gate in the kernel, returning 51 for input 10 (E1).
- Anti-amplification holds at runtime with no capability hardware, control accepted and six attack
  classes each rejected by their intended check (E2).
- An out-of-bounds guest access raises a software fault and the kernel stays intact (E3), and the
  result is drawn to the framebuffer (E4).

## Software desktop (S, A, E-10b) - 2026-06

- A bitmap-font text console scrolls by shifting framebuffer rows up (S2), and a window draws with
  background, frame, titlebar, title, and content rect (S3).
- A window moves and a console renders inside the window content rect (S4, S5), and a counter
  checkpoints and rematerializes through the store inside the GUI (S6).
- PS/2 mouse tracking moves a save-under cursor (A7), titlebar drag moves a window (A8), and a
  second window proves z-order by distinct background colors at the overlap (A9).
- Click focus raises a window (E1-10b), front-window drag moves the focused window (E2-10b), and a
  corner grip resizes with a minimum clamp (E3-10b), each pixel-readback proven.
- The boot ends in a live shell polling real PS/2 keyboard and mouse input (E4-10b).

## Bare-metal boot (B) - 2026-06

- The kernel boots on x86-64 through the Limine boot protocol into long mode, confirmed by reading
  the code segment and the EFER long-mode bit (B0, B1).
- A physical frame allocator and a four-level page mapper allocate, map, write, read back, and
  reuse a frame, fail-closed on exhaustion (B3).
- The firmware-provided linear framebuffer is drawn with a background, a rectangle, and bitmap-font
  text, confirmed by pixel readback (B4).
- A periodic PIT timer interrupt advances a tick counter (B5), and an interrupt descriptor table
  prints a register dump on a fault rather than triple-faulting.

## Core modules and proofs - 2026-06

- Capability model (cap/): strip a live capability to a position-independent descriptor and re-mint
  a fresh local capability fail-closed under the anti-amplification gate.
- Content-addressed object store (store/) with vendored BLAKE3 validated against the official
  known-answer vectors.
- Single-level store and structural diff (sls/) computing the changed set in time proportional to
  the change.
- WebAssembly safety gate (gate/) with a linear-time load-time checker, a typed intermediate
  representation, and a static bounds-elision pass whose elided opcodes the checker rejects in
  untrusted input.
- Software capability backend (carmix/) enforcing the same model with no capability hardware.
- Coq proof (proofs/Carmix.v) of anti-amplification monotonicity, W^X and forbidden-permission
  cleanliness, and object-graph acyclicity, accepted by coqc and coqchk, axiom-free except one
  stated collision-resistance hypothesis.
