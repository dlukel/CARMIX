# PERSISTENCE_LOG.md - durable content-addressed persistence (S1-S7)

This is the honest record of the first durability substrate: a BLAKE3 content store
made durable on a virtio-blk disk so that a content-addressed object AND a whole
dematerialized process survive a genuine cold reboot, where every on-disk hash names
complete verifiable content (never torn), authority is re-minted through the proven
gate and never amplified, and the disk is treated as untrusted media re-verified on
every load. Durable garbage collection is DEFERRED to its own later cycle and is out
of scope here; the store grows without reclamation.

Every number below was measured by rdtsc inside CARMIX on CARMIX's own virtio-blk
path. No number is taken from Linux, from published BLAKE3 throughput, or from any
report. Figures are from the recorded boots (`BOOT_SECS`, file-backed virtio-blk),
and vary run to run under TCG because they are real.

The cold reboot is genuine: each boot is a SEPARATE qemu process on the SAME host
disk image. RAM is empty on the next boot; only the disk file carries state. The
in-RAM hash-to-location index is rebuilt purely by scanning the disk, which is the
evidence that RAM started empty (D6).

## The seven pieces, what ran, observed

### S1 - the stable-media abstraction (virtio-blk, new bring-up)

A minimal legacy/transitional virtio-blk driver over the PCI I/O BAR, polled, no
interrupts, no MSI-X, no IOMMU (q35 default, so a descriptor address is a guest
physical address). PCI enumerate 1af4:1001, enable I/O + bus-master, reset, negotiate
only VIRTIO_BLK_F_FLUSH, one virtqueue, 4 KiB block read/write/flush.

```
DUR S1 virtio-blk up: io=0x000000000000c000 qsz=256 flush=y
DUR S1 write rc=0 read rc=0 block read-back matches written=y  -> STABLE-MEDIA READ/WRITE OK
DUR S1 measured per-object durable write+flush = 3561112 cyc (CARMIX rdtsc on the virtio-blk path)
```

The per-object durable write+flush cost (3,561,112 cyc) is CARMIX-measured by rdtsc
around the actual two-block write plus the VIRTIO_BLK_T_FLUSH on this virtio-blk path.

### S2 - the append-only object log

Immutable hash-named objects are appended, never overwritten. Each record is two
4 KiB blocks: a header block (magic, version, claimed length, claimed BLAKE3) written
FIRST, then a payload block written SECOND, then a flush. The hash IS the integrity
check: no data journal. Reuses cvsasx_blake3 and the existing object bytes.

```
DUR S2 append-only write: object hash ab2141b3c6e18505c53ac2e9ed3793984dc34d609db63e3ab05f4dffbc0766f0 rc=0 at block 1
```

Header-first ordering is deliberate: under an ordered-prefix truncation a partially
written record has its claim present but its payload incomplete, so recovery
re-hashes, mismatches, and REJECTS it (the torn-tail branch). Object size <= 4096 B
(one payload block) is the named limit of this build.

### S3 - the crash-consistent index (rebuild-by-scan, the recovery floor)

The hash-to-location index is rebuilt on boot purely by scanning the log and
re-verifying each record. This is choice (a) from the scope: simplest and correct, and
it is the recovery floor. Stops at the first block without a valid claim, or at a
record whose re-hash does not match its claim (rejected, the tail).

```
DUR S3 boot recovery (fresh disk): indexed 0 objects in 996200 cyc; rejected-tail=0
DUR S3 boot recovery (post-cold-reboot): re-verified + indexed 6 objects in 46884349 cyc; rejected-tail=0
```

The 6-object re-verifying scan cost 46,884,349 cyc (CARMIX rdtsc). The index came
purely from disk, so RAM started empty (D6).

### S4 - the crash-consistency proof (truncate at every offset)

The durable write of a two-record log was read back from disk into its exact byte
image, and the recovery parser (the SAME accept rule boot recovery uses) was run at
EVERY byte offset of the write, modelling an ordered-prefix truncation at each point.

```
DUR S4 crash proof: truncated at EVERY byte offset 0..16384 (16385 points); invariant violations=0; rejection branch fired at 8896 offsets; proof cost 609903419 cyc
DUR S4 D2 example: truncate inside record-2 payload -> record-1 KEPT, record-2 REJECTED (absent=y)
DUR S4 tamper variant: flip 1 payload byte -> re-hash MISMATCH -> REJECTED (fired=y, accepted-before-it=1)
```

At all 16,385 offsets the invariant held: the recovered store contains EITHER the
complete object whose bytes hash to its name OR no such hash, never a hash naming torn
bytes (0 violations). The rejection branch is real, not glossed: it fired at 8,896
offsets (every offset where a record's claim was present but its payload incomplete),
and the D2 example shows record 2 rejected with its hash provably absent while record
1 is kept. The tamper variant (flip one payload byte, re-hash mismatch) fires the same
rejection branch.

CLAIM, stated precisely (D3): this is RECOVERY-LOGIC crash consistency under an
ORDERED-PREFIX (write-order) failure model, demonstrated exhaustively in QEMU. It does
NOT prove real-hardware crash consistency, which additionally depends on writes
reaching stable media in issued order (cache-flush and write-barrier semantics) and on
the absence of sub-sector tearing. A byte-prefix truncation models neither; those are
named out of scope. This is the analogue of the single-CPU TLB scope on prior work:
the demonstration is real, the gap is named, not buried.

### S5 - the cold-reboot object round-trip

An object was written and flushed, the qemu machine cold-rebooted (separate process,
RAM empty), the store revived from disk, and the object rematerialized by its BLAKE3
hash with the on-disk bytes RE-HASHED before being served.

```
DUR S5 cold-reboot object round-trip: dur_get rc=0 re-verified-on-load=y len=256 content-matches-original=y round-trip 3099346 cyc
DUR S5 -> OBJECT SURVIVED A COLD REBOOT, re-hashed-on-load, content intact OK
```

End-to-end round-trip (lookup + read + re-hash + compare) was 3,099,346 cyc
(CARMIX rdtsc). The object's hash is derived from its known content on both boots, so
no trusted side channel carried it across the reboot.

### S6 - the persisted-process round-trip under the gate (the headline)

A process was built and dematerialized via the existing task-state canonical form (its
page-table-inclusive task-hash). Its object graph (manifest, registers, descriptor,
pages) was persisted, with the authority ceiling INSIDE the hashed manifest (D4), plus
a ROOT block naming the task-hash. The machine was cold-rebooted. The store was revived
from disk; every durable object was loaded and RE-HASHED on load (D5); the process was
rematerialized and its authority re-minted through the gate under the persisted ceiling.

```
DUR S6 persisted task-hash 68fa715209c5a6f20886d1a497cde370423d354c8fada299e7e74aedca9206c7 graph-objects=5 (ceiling INSIDE the hashed manifest)
DUR S6 loaded + re-verified 6 objects from disk into the in-RAM stores (D5: every load re-hashed)
DUR S6 rematerialize via the CORE materialize path DIRECTLY (NOT a live #PF): rc=0 writer-authority=RW
DUR S6 address-space root re-derived from disk-loaded state == persisted root: y
DUR S6 D4 wider-authority load (+STORE_CAP beyond persisted ceiling): status=12 (DISTINCT: AMPLIFY_PERMS) REFUSED
DUR S6 -> PROCESS SURVIVED A COLD REBOOT: rebuilt from disk under the gate, authority at the persisted ceiling, wider REFUSED OK
```

The whole process survived a genuine cold reboot. The address-space root re-derived
from the disk-loaded, re-verified state equals the persisted root, so the page-table-
inclusive state is intact. The authority was re-minted at the persisted ceiling; a
wider-authority load was REFUSED with the gate's real status code 12
(CVSASX_ERR_AMPLIFY_PERMS), so a persisted process can never come back wider than it
left (D4). Because the ceiling is inside the hashed manifest, tampering it changes the
task-hash and is caught by re-verification.

HONEST NAMING OF THE PATH (0.7): the rematerialize used the CORE materialize/attach
path DIRECTLY, not a live page fault, exactly as the PM2/F2/TS3/SM precedent. The
process is quiescent (built, dematerialized, rebuilt); it is not ring-3-resumed here.
State integrity is shown by the matching address-space root and the gate-mediated
authority, the same evidence the TS and SM cycles used.

### S7 - tamper detection on load

```
DUR S7 (in-kernel) tamper-on-load (manifest byte flipped): re-hash != task-hash detected=y -> REJECTED, bytes never served OK
DUR S7 (host-side) on boot of a host-tampered image: rejected-tail=1 on the recovery scan -> REJECTED, not indexed, bytes never served
```

Two demonstrations. In-kernel: a byte of the on-disk manifest is flipped in the loaded
buffer, the re-hash on load does not equal the task-hash, and it is rejected. Host-side:
between boots a byte was flipped in an on-disk object payload from the host; on the next
boot the recovery scan re-hashed the record, found the mismatch, and REJECTED it (it
was not indexed and its bytes were never served). The disk is never trusted blindly
(D5).

## How the forbidden dodges were actively disproven

- D1 (safe-offset crash proof): the truncation is exhaustive at EVERY byte offset
  (16,385 points over the two-record write), not hand-picked boundaries.
- D2 (silently-indexed torn object): the rejection branch fired at 8,896 offsets and on
  the tamper variant; the D2 example shows the torn record's hash provably absent from
  the recovered store.
- D3 (crash-consistency overclaim): the claim is stated as recovery-logic consistency
  under an ordered-prefix model, with write-reordering, cache/barrier semantics, and
  sub-sector tearing named as the gap to real-hardware crash consistency.
- D4 (amplification pass-through): the ceiling is inside the hashed manifest; a wider
  load was refused with status 12 (AMPLIFY_PERMS).
- D5 (trusted disk): every load re-hashes the on-disk bytes; the tamper was rejected
  and never served, on both the in-kernel and host-side paths.
- D6 (fake reboot): the reboot is a separate qemu process; the index is rebuilt purely
  from disk (6 objects re-verified), which proves RAM started empty.

## Scope and limits (named, not used to scope away the core)

- Single durable writer. There is no concurrent durable write; the honest scope.
- Object size <= 4096 B (one payload block). Larger objects would need multi-block
  payloads; not built.
- Crash consistency is recovery-logic consistency under an ordered-prefix model in
  QEMU (S4 / D3). Real-hardware crash consistency (write ordering, cache/barrier,
  sub-sector tearing) is out of scope and named.
- The log grows without reclamation. Durable garbage collection is DEFERRED to its own
  research-and-build cycle, the correct decomposition (exactly what Venti did:
  a durable content-addressed store, reclamation handled separately). GC is named as
  the next cycle, not half-built to look complete.
- Confidentiality / encryption-at-rest is out of scope; only the integrity model is in
  scope. Real-hardware media beyond virtio-blk (NVMe/SATA/PMEM) is out of scope.

## Borrowed vs new

The storage mechanisms are borrowed: the append-only log (LFS), self-verifying
content-addressed recovery and dedup (Venti), the crash-consistent rebuild-by-scan
index (LFS/ZFS recovery). The CONTRIBUTION is the unified substrate: a whole
content-addressed PROCESS (its page-table-inclusive canonical form, its pages, its
authority ceiling) persisted and rematerialized from disk under the proven
anti-amplification gate, with the disk treated as untrusted and re-verified on every
load.

The EROS / CapROS differentiator (capability systems persist via periodic orthogonal
SNAPSHOTS of the whole machine): CARMIX persists at content-addressed OBJECT
granularity, deduplicates by hash, and verifies each object by re-hashing on load,
and the authority that comes back is re-minted through the gate at the persisted
ceiling, not restored as ambient snapshot state. That object-granular,
hash-verified, gate-re-minted persistence is the difference.

## Proven core and regression

Proven core code-identical: the nine named proven-core modules (carmix/swcap.c,
carmix/backend_sw_gate.c, sls/sls.c, gate/sfi_checker.c, gate/executor.c,
gate/optimize.c, store/object_store.c, cap/cvsasx_cap.h, sls/cvsasx_sls.h) and the rest
of the proven core are byte-for-byte identical after comment-stripping. Persistence was
built BESIDE them: the new code is the virtio-blk driver, the durable log/index/recovery,
the crash proof, and the round-trip wiring, all in kernel/kernel.c, plus the qemu disk
and multi-boot orchestration in kernel/build.sh. Only kernel/kernel.c and
kernel/build.sh changed.

Full regression re-run (B / E / R / desktop / P / U / M / F / US / L / C / PP / FA / PM
/ TS / SM): green, with the SAME pre-existing PM0 stall (a ring-3 readback in the
per-process-memory stage that predates this work and is reported, not hidden). TS0-TS5
and SM0-SM5 remain green; persistence runs early each boot and does not disturb them.

GC is the named next cycle. The proven core is untouched. Nothing is pushed public;
this lands as a dev-tree snapshot.
