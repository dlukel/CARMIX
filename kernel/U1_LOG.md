# U1_LOG.md - userland U-1: spawn with bounded authority + capability IPC (single-CPU)

Honest record of U-1, the step from one program to a bounded process that can create bounded
processes. Spawn with bounded authority transfer (a child born holding a strict subset of its
parent's authority, the anti-amplification ceiling enforced at birth), derivation-recorded
revocation, and capability-carrying synchronous IPC.

All figures are rdtsc-measured in CARMIX on this run.

## Scope (read first, plainly)

This is U-1, single-CPU. Spawn, revocation, and capability IPC. It reuses the committed
capability gate, the US3 re-mint-at-crossing path, the content store, the loader
(ld_enter_ring3), and the address-space machinery (mm_new_space, mm_map). Children run
cooperatively on the one core. It does NOT implement multi-core concurrent multi-process
execution. That is U-3 and is SMP-dependent. No multi-core concurrency is claimed here.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical after
comment-stripping. U-1 is new code in kernel/kernel.c, built beside the proven core. No proven
module was edited.

## The thesis

A CARMIX process is a content-addressed live execution with an address space and a bounded
capability set. Spawn is not fork. A child is created with an authority set that is a strict
subset of its parent's, and the anti-amplification ceiling (destination authority is at most
source authority) is enforced structurally at the crossing, not by convention. This is the
capability-security core made multi-process.

## What was built and shown (observed)

### U1-1 the child's bounded CSpace at birth

```
U1-1 spawn child with subset {console,store-read}: granted=y; child holds exactly {console,store-read}=y (nothing more)
U1-1 D1 spawn requesting an un-held cap (parent slot 5 empty): refused (birth-time anti-amp)=y
```

sys_spawn verifies the caller holds a capability for every element of the requested set, then
creates a new CSpace populated only with that subset. The child starts holding exactly the
granted subset and no more. An attempt to grant a capability the parent does not hold (an
empty parent slot) is refused at spawn. The birth-time anti-amplification fault is real. D1
is disproven.

### U1-2 the derivation record (revocation-ready)

```
U1-2 derivation-recorded revocation: child had store-read=y; revoke parent store-read reached the child cap=y
```

When a child cap is derived from a parent cap, the parent-child relationship is recorded. A
revoke of the parent cap walks the derivation records and revokes the derived child cap. This
is the structural basis for authority revocation. The child loses the authority.

### U1-3 capability-carrying synchronous IPC

```
U1-3 IPC: data crossed=y; a capability sent in a message arrived usable=y; sending an un-held cap refused (IPC anti-amp)=y
```

A synchronous endpoint carries both data and capabilities. Data crosses. A capability placed
in a message arrives usable by the receiver. A send that attempts to transfer a capability the
sender does not hold is refused. The IPC anti-amplification fault is real. D2 is disproven.

### U1-4 a real spawned child runs bounded

```
U1-4 bounded child ran: store-read (granted) len=64; store-write (NOT granted) refused across the crossing (fault=255)
```

A real ring-3 child runs via the loader with a bounded CSpace of console-write and store-read
but not store-write. It uses the caps it was granted (reads 64 bytes, writes the console) and
is refused when it attempts store-write, a capability it was not granted. The over-reach faults
across the real INT 0x80 crossing (low byte 255 is the fault). The child cannot exceed its birth
authority. D3 is disproven.

### U1-5 measurement (rdtsc)

```
U1-5 rdtsc: spawn CSpace build+birth-check=597 cyc; IPC round-trip (send+recv)=11 cyc (the ring-3 child run is functional, U1-4, not a clean microbench)
```

Spawn (building the child CSpace with the birth-time anti-amplification check) measured 597 cyc.
An IPC round trip (send plus recv) measured 11 cyc. The ring-3 child run is reported as a
functional result rather than a clean microbench, because it includes a full ring-3 excursion
and the child's own serial output, which dominate and are not a meaningful per-operation number.

## U-1 complete

Spawn is bounded, revocation walks the derivation, capability IPC enforces anti-amplification,
and a real bounded child faults on over-reach. U-1 is a complete, separable stage. The next
stage U-2 (the heap coupled to the GC) follows.
