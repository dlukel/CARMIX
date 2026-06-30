# U0_LOG.md - userland Phase U-0 (single-CPU, GC-independent, SMP-independent)

Honest record of Phase U-0, the smallest real CARMIX userland: one process that starts
holding an explicit bounded capability set (no ambient authority), allocates from an
acquire-only bump allocator over a pre-granted pool, reads and writes the content-addressed
store and writes the console through five gated syscalls (each an authority-mediated
crossing with a real refusal path and anti-amplification on acquire), and exits with its
memory reclaimed by teardown.

All figures are rdtsc-measured in CARMIX on this run. The research gave no CARMIX numbers.

## Scope (read first, plainly)

This is Phase U-0 only. ONE process. Five gated syscalls. An acquire-only bump allocator.
Startup and exit. It REUSES the existing capability gate, the US3 re-mint-at-crossing path,
the proven content store, the ring-3 crossing, and the PM machinery (mm_new_space, mm_map,
ld_enter_ring3). It does NOT implement spawn (U-1), does NOT implement free-returned-to-
kernel / sys_mem_release (U-2, GC-dependent), and does NOT implement IPC or multi-process
concurrency (U-3, SMP-dependent). U-0 is independent of GC and SMP and was built as such.
This banks the first real program running on CARMIX's capability model.

## The capability principle (the whole point)

Every syscall is an authority-mediated crossing. The process presents a capability it
already holds (a CSpace slot index), the kernel validates the slot (presence and type, the
unforgeable token) and services under that authority. There is no ambient authority. A
process that does not hold the required capability is refused with a capability fault. Each
of the five syscalls demonstrates both the authorized path and the refusal.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical after
comment-stripping. U-0 is new code (the CSpace, the five gated handlers, the ring-3 program,
the harness) entirely in kernel/kernel.c. The syscalls reuse the existing gate
(cvsasx_sw_cap_remint) and the proven store (cvsasx_store_put / cvsasx_store_get). No proven
module was edited.

## What was built and shown (observed)

### U0-1 initial CSpace from a boot descriptor

```
U0-1 initial CSpace: console@0 pool@1 store-read@2 store-write@3, granted=4 of 8 slots; the rest are EMPTY (no ambient authority)
```

The process starts holding exactly four capabilities (console-write, memory-pool,
store-read for a known object, store-write-quota) and nothing more. The remaining slots are
empty. The descriptor-to-CSpace mapping is observable, and the process holds nothing it was
not granted (its use of slots 0 to 3 succeeds and its use of an empty slot is refused).

### U0-2 the syscall / gate ABI, and the hash representation

```
U0-2 ABI: syscall carries (call#, cap-slot, args); hash representation = (c) the read cap ENCODES the object hash (no separate hash arg). store_write returns the new hash + installs a read cap.
```

A syscall presents a capability slot index plus data arguments. The hash-in-syscall
representation chosen is (c): the read capability itself encodes the object hash (in its PIR
referent hash), so there is no separate hash argument. Tradeoff: a process can only read
objects it holds a capability for, the capability is the name, there is no hash guessing and
no ambient naming. The cost is that to read a freshly written object the process uses the
read capability that store_write installs for it. store_write returns the new content hash
and installs a read capability naming it.

### U0-3 sys_exit

The process exits cleanly. Its address space and acquired pages are released by the existing
process-teardown path. U-0 is acquire-only with no sys_mem_release, so memory is reclaimed at
exit by teardown, not by a userland free. Stated honestly: there is no free-to-kernel in U-0.

### U0-4 sys_console_write (gated I/O)

```
U0-4 console_write: authorized ok=y; no-cap write refused (nothing written)=y
```

The process writes to the console only by presenting its console-write capability. With the
cap the bytes reach the serial console. Without it the write is refused (capability fault,
nothing written). I/O is authority-mediated, not ambient.

### U0-5 sys_mem_acquire (acquire only, anti-amplification through the gate)

```
U0-5 mem_acquire: in-pool acquire ok=y; over-pool refused (anti-amp via the gate)=y; no-cap acquire refused=y
```

The process acquires memory by presenting its memory-pool capability. The requested size is
routed through the proven anti-amplification gate (the same cvsasx_sw_cap_remint the kernel
gate uses) against the pool ceiling. An acquisition within the pool authority succeeds and
the region is usable. An acquisition exceeding the pool authority is refused (the gate
refuses the amplification). A process without the pool cap cannot acquire at all. There is no
release, acquired memory returns at process exit.

### U0-6 sys_store_read and sys_store_write (gated content-addressed access)

```
U0-6 store_read: authorized len=64 re-hash==named=y; no-cap read refused=y
U0-6 store_write: returned hash names the bytes (re-hash==returned=y); no-cap refused=y; over-quota refused=y
```

store_read reads a known object by presenting a store-read capability, the bytes returned
re-hash to the name the capability encodes. store_write writes a new immutable object and
returns its content hash, the returned hash re-hash-verifies against the bytes written (a
content-addressed write, new content gets a new name). A read without the read cap is
refused, a write without the write-quota cap is refused, and a write exceeding the quota is
refused.

### U0-7 startup, U0-9 the end-to-end program (a REAL ring-3 process)

```
U0-9 ring-3 program ran end-to-end: read len=64 wrote len=64 (hash-correct in store=y), unauthorized console refused (fault=255)
U0-7/U0-9 -> REAL PROCESS: started, gated-syscalled, store hash-correct, refusal real across the crossing, exited OK
```

A single real ring-3 program runs the whole path through actual INT 0x80 crossings: it starts
(reaches its entry with a working stack and its capability set), acquires a region from the
pool, bump-allocates a buffer, reads the known object from the store, transforms it, writes a
result object to the store and the kernel returns its hash (hash-correct, re-verified in the
store), writes a line to the console, attempts one unauthorized console write (refused with a
real capability fault across the actual crossing, low byte 255 = the fault), and exits. This
is not a kernel-internal simulation, it is a process crossing the real gate. The process
loader reused is the existing inline-blob plus page-table loader (mm_new_space, mm_map,
ld_enter_ring3), not a separate ELF toolchain, stated honestly.

### U0-8 the bump allocator (acquire only, userland, no syscall, no free)

```
U0-8 bump allocator: two blocks at +0 and +32, pointer advanced to +48, within the acquired region=y (no syscall per alloc, no free)
```

A userland bump allocator over the acquired pool region: it advances a pointer and hands out
aligned blocks with no per-allocation syscall and no free, consistent with U-0's
no-sys_mem_release. It stays within the acquired region.

### U0-10 rdtsc measurements (this run)

```
U0-10 rdtsc (gated handler, this run): console=8 store_read=1485 store_write=36312 mem_acquire=1379 bump-alloc=14 cyc
```

Measured this run (the gated-handler path, the INT and IRETQ hardware crossing is on top of
these):

- console_write: 8 cyc.
- store_read: 1485 cyc.
- store_write: 36312 cyc (dominated by the content-store put, hashing plus insert).
- mem_acquire: 1379 cyc (the anti-amplification gate re-mint plus the bump).
- bump-alloc: 14 cyc (portable pointer arithmetic, no crossing).

store_write is the most expensive, reported honestly, it is the hash-and-store cost.

## Forbidden dodges, each disproven

- D1 ambient syscall: each of the five syscalls refuses an absent or wrong capability (the
  refusal paths are real, shown both in the harness and across the real ring-3 crossing).
- D2 anti-amplification skipped: mem_acquire refuses an over-acquire beyond the pool
  authority via the proven gate.
- D3 free-to-kernel smuggled in: there is no sys_mem_release, memory is reclaimed at exit.
- D4 spawn or IPC smuggled in: there is no spawn, no IPC, one process only.
- D5 store write not content-addressed: the write returns the hash that names the content and
  it re-hash-verifies.
- D6 hardcoded numbers: every figure is rdtsc-measured this run.
- D7 proven-core drift: the nine proven modules and the rest are byte-identical.
- D8 GC or SMP coupling: U-0 uses no GC and no SMP, no free-to-kernel, no IPC, no concurrency.
- D9 hash-width hand-waved: the representation is chosen and stated (the read cap encodes the
  hash, choice c, with its tradeoff).

## Regression

The full single-CPU regression re-ran green except the same pre-existing PM0 stall. The
versioned gate, persistence, GC, dedup-scoping, and TS/SM all still pass, the M0 and F2
negatives reach their lines, and the desktop comes up. Adding U-0 did not break the existing
path.

This banks Phase U-0, the first real program on CARMIX's capability model. Spawn,
free-to-kernel, IPC, and multi-process concurrency remain Phases U-1, U-2, and U-3.
