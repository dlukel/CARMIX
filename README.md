# CARMIX

CARMIX is a research operating system kernel that migrates a live computation between machines
by content-addressing its state, transferring only the parts the destination lacks, and minting
fresh local capabilities at the destination under a gate that never grants more authority than
the source held.

The system boots on x86-64 through a standard bootloader, runs a capability-guarded WebAssembly
executor in the kernel, rematerializes computation state on the running machine and across two
machines, and ships with a machine-checked Coq proof of its core authority guarantee (the proof
is of the abstract re-mint model; the C gate is tested against attack tables, not machine-checked). It is a
research prototype verified in an emulator, not a general-purpose operating system. The sections
below state precisely what is proven, what is measured, and what is not built yet.

## The idea

Rematerialization is the act of reconstructing a running computation somewhere else from its
content address rather than copying its bytes wholesale.

Three pieces combine.

1. Content-addressed state. Every object in the computation's memory graph is named by the
   BLAKE3 hash of its content. Identical content has one name and one stored copy. Changing an
   object produces a new object with a new name, and the unchanged objects keep their names.

2. Diff-proportional transfer. Because the destination already knows the names of objects it
   holds, a migration sends only the objects whose names the destination is missing. The bytes
   on the wire track the size of the change, not the size of the state. This follows directly
   from the content-addressed immutable model. It is not a separate deduplication pass.

3. Monotonic capability re-mint. A capability is a token of authority over a region of memory
   with a set of permissions. CARMIX never moves a live capability across the boundary. It
   strips the capability to a plain descriptor, transfers the content-addressed state, and mints
   a fresh local capability at the destination through a re-mint gate. The gate is fail-closed.
   The minted capability's bounds are no wider and its permissions no greater than the source
   held. Over-authority is rejected, not clamped silently.

What is novel here is the synthesis. Content-addressed migration of program state exists in prior
work. Capability systems exist. The new part is fusing them so that the diff-proportional transfer
and the monotonic re-mint emerge together from executing live state inside a content-addressed
immutable memory model, with the authority guarantee enforced on each re-mint. The nearest active
project in the space content-addresses program binaries. CARMIX content-addresses live computation
state and re-mints capabilities during the migration.

The deeper claim is that one mechanism does several jobs. A context switch, a page fault, a
migration, a user-to-kernel crossing, a program load, and a deschedule are not separate
subsystems here. Each is the same operation, rematerialize an object identified by its BLAKE3
hash under a freshly re-minted authority ceiling that the anti-amplification gate refuses to let
exceed the source. docs/THE_CONTRIBUTION.md states the claim as it would be stated to a reviewer,
docs/THEORY.md states the authority property precisely, and docs/ARCHITECTURE.md walks the path.

## Where this sits in the literature

The axis CARMIX occupies, content-addressing live execution state and tying the capability
re-mint to it, appears unoccupied in the surveyed systems. This is a positioning statement, not a
claim of superiority. Each of the systems below is strong on its own axis.

- Theseus makes safety a language property, with intralingual state and cell-based modules. Its
  axis is language safety, not content-addressed live state.
- Twizzler content-addresses data at rest, persistent objects in a single global address space.
  Its objects are storage, not a running computation's live memory graph under a migrating
  capability.
- seL4 is the verified microkernel and capability system. It verifies and capability-izes time
  and access, not the content of live state, and does not migrate state by content address.
- MITOSIS provides fast remote process fork. It is remote creation, not a content-addressed
  time-slice of an already-running computation, and it does not re-mint capabilities by content.
- EROS and KeyKOS take a system-wide periodic checkpoint for persistence. The checkpoint is
  decoupled from scheduling and is not a per-operation content-addressed rematerialization gated
  by an authority ceiling.
- Plan 9 unifies through namespaces, and Unix through files. CARMIX unifies through
  rematerialization of content-addressed live execution. The unifying noun is different.

The CARMIX axis is content-addressed live-execution state with the capability re-mint fused to
it. docs/THE_CONTRIBUTION.md gives the precise prior-art difference.

## What is proven, what is measured, what is demonstrated

| Claim | How strong | Where observed |
| --- | --- | --- |
| Anti-amplification monotonicity. A re-minted capability's authority never exceeds the source. | Machine-checked in Coq. coqc and coqchk both accept it. Axiom-free except one stated collision-resistance hypothesis. | proofs/Carmix.v |
| W^X and forbidden-permission cleanliness of a valid re-minted capability. | Machine-checked in Coq. | proofs/Carmix.v |
| Object graph acyclicity. | Machine-checked in Coq, with objects modeled as finite trees and collision-resistance connecting the hash-graph to that structure. It does not derive acyclicity from hashing alone. See docs/PROOFS.md. | proofs/Carmix.v |
| Anti-amplification holds at runtime with no capability hardware. Six attack classes rejected, each by its intended check. | Observed in the kernel on x86-64 in an emulator. | kernel test E2 |
| In-kernel rematerialization. A computation's state checkpoints and rematerializes bit-identically. | Observed in the kernel. | kernel test R3 |
| Two-machine diff-proportional migration, hash-verified. Cold sync 44505 bytes over 1093 objects. Three warm hops after small changes move 1176, 1176, and 2219 bytes. | Measured bytes on the wire between two emulator instances. | kernel/net_repro.sh |
| Signed cross-machine authorization with real Ed25519. One legitimate migration accepted, seven adversaries each rejected by a distinct reason. | Observed between two emulator instances. | kernel/net_repro.sh |
| Key lifecycle. A source key is learned by authority-signed enrollment, rotated and revoked under the authority key, monotonic epochs against replay and downgrade. Six lifecycle attacks each rejected by a distinct reason. | Observed between two emulator instances. | kernel/net_repro.sh |
| Software-rendered desktop. Boot, framebuffer, console with scrolling, windows with focus, drag, and resize, driven by PS/2 keyboard and mouse. | Observed by framebuffer pixel readback in an emulator. | kernel self-tests |
| Task substrate. The kernel creates tasks, switches between them with a real assembly context switch, runs the content-addressed remat path inside a task across a switch, and preempts a non-yielding task on the timer interrupt. The scheduling policy is an isolated round-robin placeholder. | Observed by serial trace in an emulator. | kernel scheduler self-test |
| Task as a content-addressed object. A task's registers and stack hash into the content-addressed store, activate-by-hash dematerializes and rematerializes it through that hash, and a task resumes from its hash alone, so persistence is a no-op. | Observed by serial trace in an emulator. | kernel unified-substrate self-test |
| The cost crossover, measured. A raw register switch is a flat cost near 1400 cycles, while activate-by-hash is two to three orders of magnitude more at every dirty-set size, so the fast path stays for rapid switching and content-addressing is used only at coarse boundaries. | Measured with rdtsc in an emulator. | kernel unified-substrate self-test |
| Residency manager. Physical memory is a content-addressed cache over the store. Fault-in materializes a frame from a hash and verifies it, identical content is shared by hash as one frame, eviction writes a dirty victim back to the store, and the hardware page-table dirty bit is measured against the software diff (it agrees and is six to seven times cheaper). | Observed and measured in an emulator. | kernel residency self-test |
| Rematerializing fault handler. A not-present access traps through the page-fault vector and is serviced by a BLAKE3-verified materialize-by-hash that gates resume. Content that fails verification is refused at the fault boundary, and identical content is shared on the fault path. | Observed by serial trace in an emulator. | kernel fault-handler self-test |
| Authority-bounded user and kernel crossing. A ring-3 process enters the kernel through a re-mint of its bounded capability under the same anti-amplification gate that bounds migration and faults. Six userspace amplification attempts are each refused by their own distinct reason, the same status codes the migration table uses. | Observed by serial trace in an emulator. | kernel userspace self-test |
| Content-addressed process loader. A program image is an object in the store. Loading a process materializes its ELF segments by hash with BLAKE3 verification into a fresh ring-3 space under a re-minted ceiling, enforces W^X, and two processes from one image share read-only code by hash. | Observed by serial trace in an emulator. | kernel loader self-test |
| Concurrent content-addressed processes. Two ring-3 processes run interleaved on the timer, each in its own address space under its own re-minted ceiling. A descheduled process is dematerialized to a hash and rematerialized to resume, its counter surviving the round trip. | Observed and measured in an emulator. | kernel concurrency self-test |
| Rematerialization-aware scheduling policy. A descheduled process is kept resident by default and dematerialized only under memory pressure when predicted to stay gone long enough to beat the measured rematerialize cost, with the break-even computed live from that cost and an anti-thrash backoff. | Observed and measured in an emulator. | kernel policy self-test |
| Fairness under rematerialization. A process repeatedly paying the rematerialize cost accumulates a measured progress deficit, and a bounded control keeps it resident to recover its progress rate without starving its resident peer. | Observed and measured in an emulator. | kernel fairness self-test |
| Content-addressed process heap. A process grows memory through an authority-bounded extend-heap, backed demand-zero. An idle heap page dematerializes and rematerializes by hash, two processes deduplicate an independently-identical heap page by hash with copy-on-write, and a hot churning page is excluded from dematerialization. | Observed and measured in an emulator. | kernel heap self-test |

The two-machine byte counts and the proof results are reproducible. See docs/REPRODUCE.md.

## Status and maturity

CARMIX is a working research prototype, roughly 40 to 45 percent of the way to a general-purpose
operating system in the sense of the subsystems a Linux-like system needs. It is not finished and
it is not production software. The estimate is a self-assessment, not a benchmark.

What is demonstrated, each item verified by the evidence cited in the table above: boot on x86-64
through a standard bootloader, a physical frame allocator and a four-level page mapper, an
interrupt and fault path that dumps and halts rather than triple-faulting, a software-rendered
desktop with windows, focus, drag, and resize, an in-kernel WebAssembly executor under the
software capability gate, content-addressed in-kernel rematerialization, a task substrate with a
real assembly context switch and timer preemption, a residency manager that treats physical
memory as a content-addressed cache, a rematerializing page-fault handler, an authority-bounded
ring-3 userspace and re-minting syscall boundary, a content-addressed process loader, two
concurrent processes with descheduling as rematerialization, a rematerialization-aware scheduling
policy with a fairness control, a per-process heap, two-machine diff-proportional migration over
ivshmem, signed cross-machine authorization with real Ed25519, a signed key lifecycle, and a
machine-checked Coq proof of the core authority guarantee (of the abstract re-mint model; the C
gate is tested against attack tables, not machine-checked).

What is not built, the gap to general purpose: there is no filesystem and no persistent storage
(the store is in RAM), no full heap allocator (the userspace allocator is bump-only with no free),
no dynamic linking (the one loaded program is a static executable), no real-hardware bringup
(everything runs in QEMU), no USB input (input is PS/2), no GPU acceleration (the desktop is
software-rendered and runs only CARMIX's own windows), no interrupt-driven input, no NIC (the
two-machine transport is ivshmem shared memory), no learned scheduling predictor, and no
established root of trust for keys (the first authority key is baked in). The full list is in
docs/ROADMAP.md.

## What this is not, yet

CARMIX is not a general-purpose operating system. The following are not built. They are listed
in full in docs/ROADMAP.md.

- Real hardware. Everything here runs in QEMU. The bootable image runs the same path on a real
  machine from a USB stick, but that has not been verified on physical hardware.
- USB input. Input is PS/2, which the emulator provides. Modern hardware needs a USB HID stack.
- Persistent storage and a filesystem. The store is in RAM. A process model, a ring-3 userspace, a
  re-minting syscall boundary, a content-addressed process loader, concurrent scheduling, and a
  rematerialization-aware scheduling policy, and a per-process heap exist, but a full allocator,
  dynamic linking, and a learned predictor do not. See
  docs/ROADMAP.md.
- GPU acceleration. The desktop is software-rendered. It draws CARMIX's own windows. It does not
  run third-party graphical applications, which would require the vendor GPU driver stack.
- The root of trust for keys. The key lifecycle, enrollment, rotation, revocation, and monotonic
  epochs, is built, but the destination's first authority key is baked in. A certificate authority,
  identity proofing, or an out-of-band bootstrap of that first key, and persistence of the trust
  state across a reboot, are not built. See docs/SECURITY_MODEL.md.

The honesty of this list is what makes the machine-checked and tested parts credible.

## Build and run

TESTING.md is the shortest path from a clean machine to a running kernel. docs/BUILD.md has the
toolchain and pinned versions, and docs/REPRODUCE.md lists each demo, its exact command, and the
number it prints.

The build scripts read every tool location from environment variables and fail loudly if one is
unset. Set all six, then build. Set BOOT_SECS=180 so the later self-test stages have time to run
under the emulator's slower timing.

```
export CVSASX_CLANG=/path/to/clang \
       CVSASX_LLD=/path/to/ld.lld \
       CVSASX_NM=/path/to/llvm-nm \
       CVSASX_OBJDUMP=/path/to/llvm-objdump \
       LIMINE_DIR=/path/to/limine \
       X86TOOLS=/path/to/qemu-prefix \
       BOOT_SECS=180
cd kernel
bash build.sh
```

That boots headless with serial output and streams the self-test results. To watch the desktop
draw its own windows and drive it by hand, run the prebuilt QEMU with its VNC server and attach a
VNC client.

```
"$X86TOOLS/usr/bin/qemu-system-x86_64" -M q35 -m 512M \
   -cdrom kernel/carmix.iso \
   -vnc 0.0.0.0:1 -serial stdio -no-reboot \
   -L "$X86TOOLS/usr/share/seabios" -L "$X86TOOLS/usr/share/qemu"
# then attach a VNC client to localhost:5901
```

You will see CARMIX boot, the self-tests stream over serial, and the kernel draw its own
framebuffer desktop, with live PS/2 mouse and keyboard at the run_shell stage. You will not see
any third-party GUI application, browser, or GPU-accelerated graphics, because the desktop is
software-rendered by design. The full walk-through is in TESTING.md.

## How it is organized

```
cap/      capability model: strip a capability to a descriptor, re-mint under the gate
store/    content-addressed object store, BLAKE3 hashing (vendored)
sls/      single-level store and the structural diff over the object graph
gate/     WebAssembly load-time safety checker and stack-machine executor
carmix/   software capability backend that enforces the model with no capability hardware
kernel/   the bootable x86-64 kernel, desktop, and the two-machine authorization harness
proofs/   the Coq sources and a one-command verify
paper/    the interim whitepaper, LaTeX source and the built PDF
docs/     architecture, theory, proofs, security model, glossary, build, reproduce, roadmap
```

## License and contributing

CARMIX is released under the GNU Affero General Public License version 3, copyright 2026 Loucas
Louka. See LICENSE. docs/LICENSE_CHOICE.md records why AGPL-3.0. See CONTRIBUTING.md to build, run
the proofs, and run the no-regression suite before a pull request.
