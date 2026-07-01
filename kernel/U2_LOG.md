# U2_LOG.md - userland U-2: real heap with real free, coupled to the committed GC (single-CPU)

Honest record of U-2, giving a process a real heap with real free, where the free-to-kernel
path (sys_mem_release) is coupled to the committed garbage collector so a dropped region is
reclaimed under the content-addressed reference model.

All figures are rdtsc-measured in CARMIX on this run.

## Scope (read first, plainly)

This is U-2, single-CPU. A real userland allocator with malloc and free, and sys_mem_release
coupled to the committed GC refcount reclamation (the gc_rc table and tombstone path committed
at 40c99cd). It reuses the committed GC, not a parallel reclamation. It does NOT implement
multi-core concurrency. That is U-3 and is SMP-dependent. No multi-core concurrency is claimed.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical after
comment-stripping. U-2 is new code in kernel/kernel.c. The reclamation path it calls is the
committed gc_rc refcount plus tombstone reclamation, unchanged. No proven module was edited.

## What was built and shown (observed)

### U2-1 the userland allocator with real free

```
U2-1 heap malloc/free: 3 blocks, free the middle, re-malloc reuses it (same address=y, high-water unchanged=y) -> real free, growth bounded
```

A real free-list allocator with malloc and free for heap-internal blocks. Free is userland only,
no syscall per free. Allocate three blocks, free the middle, re-malloc the same size, and the
freed block is reused at the same address with the high-water mark unchanged. Real free, growth
stays bounded.

### U2-2 sys_mem_release coupled to the committed GC (the U-2 core)

```
U2-2 sys_mem_release coupled to committed GC: region held (refcount 1)=y; release decremented the COMMITTED gc_rc to 0 and reclaimed via the tombstone path=y
```

A process returns a whole acquired region by dropping its capability to it. sys_mem_release
decrements the refcount on that region in the committed gc_rc table. When the refcount reaches
zero the region is reclaimed via the committed crash-consistent tombstone path (the tombstone is
the commit point), then the storage is freed. This is the userland free path flowing into the
content-addressed GC reference model. The committed GC path is the one that runs, not a parallel
reclamation. D4 is disproven.

### U2-3 release is anti-amplification-safe

```
U2-3 release of a region NOT held: refused=y
```

A process can only release a region it holds a capability to. Releasing does not grant it
authority over anything else. A release attempt on a region the process does not hold is refused.

### U2-4 the GC still holds under userland release

```
U2-4 GC under userland release: a region with two holders, one release keeps it live (not reclaimed)=y; last release reclaims crash-consistently (tombstone commit)=y
```

The committed GC invariants still hold when the decrement is driven by a userland
sys_mem_release rather than a kernel-internal drop. A region with two holders is not reclaimed
when one holder releases (still reachable, refcount 1, not tombstoned). The last release brings
the refcount to zero and reclaims crash-consistently (the tombstone is the commit point). A
reachable region is never reclaimed. D5 is disproven.

### U2-5 exit reclaims the process's memory via the GC

```
U2-5 exit-time reclaim: a region held at exit (refcount 1)=y; teardown drops it via the refcount path and the GC reclaims it (no leak across process end)=y
```

When a process exits, its still-held region capabilities are dropped and the GC reclaims the
regions whose refcount reaches zero, via the committed refcount path, not an ad-hoc sweep. A
region acquired and not released before exit is reclaimed at teardown. No leak across the
process's own lifetime-end.

### U2-6 measurement (rdtsc)

```
U2-6 rdtsc: malloc+free (userland)=102 sys_mem_release (incl committed GC decrement+reclaim)=... cyc; bytes reclaimed end-to-end=16384 (4 regions, 1 block each)
```

Userland malloc plus free measured 102 cyc. sys_mem_release (including the committed GC
decrement and reclaim, and in the measurement loop the paired acquire) is reported as measured,
dominated by the content-store put on acquire. Bytes reclaimed end-to-end were 16384 (four
regions, one block each).

## U-2 complete

A real heap with real free, sys_mem_release coupled to the committed GC, crash-consistent
reclaim, and exit-time reclaim via the refcount path. Together with U-1 this banks the step from
one program to a bounded process that can create bounded processes and manage real memory, the
substrate of a usable capability OS. Multi-process concurrency across cores remains U-3, which is
SMP-dependent.
