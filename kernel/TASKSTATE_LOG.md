# TASKSTATE_LOG.md - full task-state dematerialization (TS0-TS5)

This log records the build that makes ONE whole live (descheduled) process a
content-addressed object, page-table hierarchy and capability authority included,
dematerialized so that ALL its frames are freed (page-table frames too), then
rematerialized bit-exact with authority re-minted through the proven
anti-amplification gate under the same ceiling. It closes the standing limit named
in every prior milestone (C2 / PP / `docs/ROADMAP.md`: "the page-table root stays
resident").

It consumes the paired research report (canonical Merkle page-table form, full
task-object enumeration, bit-exact rebuild, break-even vs C3, authority safety) and
realizes that seam in the kernel. The research mapped the seam; this build makes a
whole process rematerialize. Neither fakes the other's result.

All numbers below are COMPUTED on the metal (rdtsc / counters), never hardcoded,
and vary run to run under TCG because they are real. The pasted figures are from
the recorded boot (`BOOT_SECS=180`); re-running reproduces the structure, not the
exact cycle counts.

## What is new, and what is reused (the proven core is untouched)

The proven modules ship byte-identical. The comment-stripped diff against `HEAD`
is empty for every file under `gate/ cap/ carmix/ store/ sls/`; only
`kernel/kernel.c` changed (the TS0-TS5 section, `run_taskstate`, plus a one-line
capture of the clean kernel cr3 and the call site). Per-file confirmation is in the
"Proven core byte-identical" section.

REUSED, not reimplemented:

- `conc_make_proc` / `conc_enter` / `conc_dematerialize` - the ring-3 process model
  and the C2 descheduled-process serialization style.
- `mm_new_space` / `mm_walk` / `mm_map` / `mm_resolve` - the four-level page tables.
- `frame_reserve` / `frame_release_physical` / `falloc` / `ffree` - the frame pools
  (the page-table frames come from `falloc`, so `ffree` reclaims them).
- `rm_materialize` / `dematerialize_frame` / `cvsasx_blake3` / the store - content
  addressing and the PM2 backing-page demat/remat path. Backing-page CONTENT lives
  in `rm_store` so `rm_materialize` rebuilds it verbatim; the canonical tree,
  descriptor, registers, metadata and manifest live in a dedicated `ts_store`.
- `cvsasx_sw_cap_remint` + the custodian / region machinery (`conc_remint`) - the
  proven gate, for re-minting the capability authority on rebuild.

## Scope lock (from the research report)

- IN scope: a QUIESCENT (descheduled) process with ordinary CONTENT-BACKED pages.
- OUT of scope, EXCLUDED and flagged, never hashed: device/MMIO mappings (no
  meaningful content hash) and live / non-quiescent address spaces
  (canonicalization needs a consistent snapshot).

The process's content is its USER (ring-3) page-table subtree - the entries with
the USER bit set at every level. The shared kernel/HHDM half is supervisor (USER
bit clear) and is never canonicalized or freed. This USER-bit rule is what makes
the canonicalization and the frame-freeing independent of the boot's page-table
layout (the kernel's low PML4 slot, and which cr3 happens to be active, do not
matter).

## Stage -> proof map

### TS0 - the canonical page-table form (placement-independent Merkle root)

Implements the research report's canonical form:

- leaf = `BLAKE3( VPN || page-content-hash || perm-bits )`, where the
  page-content-hash is the existing `BLAKE3` of the 4 KiB backing page (the store's
  hashing) and perm-bits is the exact permission mask (present / write / user / NX;
  the hardware ACCESSED and DIRTY status bits are masked out so an accessed page and
  an untouched one with the same rights canonicalize identically).
- interior = `BLAKE3( ordered child hashes )`.
- address-space root = hash of the canonical PML4-level node.

The walk goes over the REAL page table (`mm_walk`'s own PML4), classifying each
present USER mapping: content-backed (include) versus device/MMIO/non-content
(EXCLUDE + flag). The tree is built over included mappings only.

Proof (observed): two processes with an IDENTICAL logical mapping (same VPNs, same
page contents, same perms) in DIFFERENT physical frames canonicalize to the SAME
root (placement-independence - the hash never sees a physical frame number).
Changing one page's content, and separately changing one permission bit, each makes
the root DIFFER; restoring returns it to the baseline root. A scratch space with one
RAM page and one device/MMIO page shows the MMIO page EXCLUDED and flagged, never
hashed.

Recorded boot:

```
TS0 proc A root = 7ee548327ac6099e24d159826f4a21968208bf8e89ae8445cc592a4437f2431e
TS0 proc B root = 7ee548327ac6099e24d159826f4a21968208bf8e89ae8445cc592a4437f2431e
TS0 A frames: code#0 data#1 stack#2  B frames: code#0 data#3 stack#4
TS0 identical logical space, DIFFERENT physical frames (data/stack distinct=y; code deduped-by-hash to one frame) -> roots EQUAL=y (PLACEMENT-INDEPENDENT) OK
TS0 one CONTENT byte changed -> root 20a745e8225bb72d7799f3bd.. differs=y
TS0 one PERM bit changed (RW->RO) -> root af346c98796d93c917e1a854.. differs=y; restored-to-baseline=y
TS0 device/MMIO exclusion: content-backed leaves INCLUDED=1, non-content EXCLUDED=1 (flagged @0x0000000060000000, never hashed) OK
TS0 -> CANONICAL MERKLE FORM: placement-independent, content/perm sensitive, device-excluded OK
```

A and B map the same VPNs to the same page contents and perms in DIFFERENT physical
frames (data #1/#3, stack #2/#4; the read-only code page deduplicates to one frame
by hash) and produce the SAME root - placement-independence. One content byte and
one permission bit each move the root; restoring returns it to the baseline. The
device/MMIO page is classified out and flagged, never hashed.

### TS1 - the complete task object (reduce a process to one hash)

`task-hash = BLAKE3( registers-hash || stack-hash || address-space-root-hash ||
capability-ceiling-descriptor || metadata-hash )`.

The five-component concatenation is the manifest; the task-hash is `BLAKE3` of the
manifest, so the whole process is reduced to one 32-byte content address.

- registers-hash, stack-hash: reuse the C2 register/stack content-addressing.
- address-space-root-hash: from TS0.
- capability-ceiling-descriptor: the process's authority as a RE-MINTABLE descriptor
  - the referent content-hash, the length, and the canonical perms (LOAD|GLOBAL).
  It carries NO raw kernel pointers and NO raw capability bits; it is exactly what
  the gate re-mints on rebuild.
- metadata-hash: the per-task metadata for a correct rebuild (id/tag, the residency
  PP/FA counters, the local ceiling base offset, and the descriptor hash), hashed.

Proof (observed): the process is run in ring 3 then descheduled (quiescent), and its
task-hash is printed with every component hash shown.

Recorded boot:

```
TS1 process ran in ring 3 then descheduled (quiescent) at counter=3 (full context saved in its trapframe)
TS1 registers-hash         = 475ce4a4555f1ab4395b76891f55caaee7bf87828f82bcdd9ca54487f363ff9e
TS1 stack-hash             = b6fb73fc46938c981e2b0b4b1ef282adcfc89854d01bfe3972fdc4785b41b2c7
TS1 address-space-root     = ab4a4be8e90b77a323d4c2f95c3fa5bec6d7e2182b1d877e7e8da28619cbf8d9
TS1 capability-ceiling-desc= ceilH d424e1784521d175acadaa3d.. len=128 perms=0x0000000000000005 (LOAD|GLOBAL; re-mintable, NO raw pointers/cap-bits)
TS1 metadata-hash          = 2c23f7017c25cf54f645fa3d9aa83e55aa7fc61f934d7c341633acd97ea37240
TS1 ==> TASK-HASH          = d6214203e1d398c4eaaf2627c2a6723e5d1ebc0f0d5a9c8a82d4a89b7691944f
TS1 -> WHOLE PROCESS REDUCED TO ONE HASH; capability carried as the re-mintable ceiling descriptor OK
```

The capability component is the ceiling descriptor (referent hash `d424e178…`, length
128, perms `0x5` = LOAD|GLOBAL), not raw pointers - exactly what the gate re-mints in
TS4.

### TS2 - full dematerialization (free ALL frames, page-table frames included)

- The canonical page-table tree (all nodes) is stored in the content store by hash.
- Every backing page is dematerialized via the PM2 path (`dematerialize_frame` ->
  content into the store, then the frame is freed), deduplicated where a hash is
  already resident (the shared read-only code frame is dropped by `refdrop`).
- THE NEW THING: the page-table frames themselves (PML4 / PDPT / PD / PT) are freed
  with `ffree` once the canonical tree is in the store. After TS2 the process holds
  NO frames at all, only its task-hash.

Proof (observed): the frame-pool free count before and after, with the delta split
into backing frames and page-table frames, and the line "process now exists only as
task-hash …, frames held = 0". The dematerialize cost is measured (rdtsc), with the
page-table canonicalization cost broken out so the new cost is visible.

Recorded boot:

```
TS2 backing content in store: code-resident-by-hash=y; data+stack dematerialized via the PM2 path (hash->store, frame freed)
TS2 pool free BEFORE=253 AFTER-backing=256 (backing frames freed=3)
TS2 PAGE-TABLE frames freed (PML4+PDPT+PD+PT, cr3=0x00000000002db000) = 6 (the standing 'page-table root stays resident' caveat is now CLOSED)
TS2 total physical footprint = backing(3) + page-table(6) = 9 frames freed; frames held = 0
TS2 process now exists only as task-hash d6214203e1d398c4eaaf2627c2a6723e5d1ebc0f0d5a9c8a82d4a89b7691944f, frames held = 0
TS2 cost MEASURED = 3492528 cyc total (of which canonicalization = 1440718 cyc, the NEW page-table cost broken out)
TS2 -> FULL DEMATERIALIZATION: every frame freed (page-table frames included), footprint zero OK
```

The whole physical footprint goes to zero: 3 backing pages (code, data, stack  - 
returned to the pool, pool free 253 -> 256) PLUS the 6 page-table frames (one PML4,
one PDPT, one PD, three PTs - returned via `ffree`). After TS2 the process holds NO
frames, only its 32-byte task-hash. The new page-table canonicalization cost
(1,440,718 cyc) is broken out from the total demat cost (3,492,528 cyc).

### TS3 - bit-exact rematerialization (rebuild the whole process from the hash)

From ONLY the task-hash: resolve the manifest, then the metadata, then the canonical
descriptor; allocate fresh frames and materialize each page content by hash
(`rm_materialize` for shared read-only code, a fresh private frame with a BLAKE3
re-verify for each writable page); construct a NEW four-level page-table hierarchy in
fresh frames at the recorded VPNs with the exact perm bits; restore registers;
re-mint the capability (TS4); restore the metadata counters; make the process
runnable.

This rebuild is serviced through the CORE materialize path directly
(`rm_materialize` / `frame_reserve` + BLAKE3-verify), NOT through a live page fault.
This is stated honestly in the serial line; it is the PM2/F2 precedent.

Proof (observed): after rebuild the live process is RE-CANONICALIZED (its NEW page
tables walked, every backing page re-hashed, the Merkle tree rebuilt, registers and
ceiling re-collected) and the recomputed task-hash EQUALS the original from TS1. The
physical frames differ (old vs new PFNs printed; the rebuild holds spacer frames so
it cannot reuse the just-freed LIFO slots, forcing fresh placement) while the logical
mapping and contents are identical. The process is then resumed to completion to show
it runs. The rebuild cost is measured.

Recorded boot:

```
TS3 rebuilt via the CORE materialize path directly (rm_materialize / frame_reserve+BLAKE3-verify), NOT a live #PF (the PM2/F2 precedent)
TS3 original task-hash  = d6214203e1d398c4eaaf2627c2a6723e5d1ebc0f0d5a9c8a82d4a89b7691944f
TS3 recomputed task-hash= d6214203e1d398c4eaaf2627c2a6723e5d1ebc0f0d5a9c8a82d4a89b7691944f  MATCH (BIT-EXACT)
TS3 address-space root re-derived from the NEW page tables matches=y
TS3 frames DIFFER (old data#1 -> new#5; old stack#2 -> new#4; old PML4 0x00000000002db000 -> new 0x00000000002d7000) distinct=y yet contents+mapping IDENTICAL
TS3 rebuild cost MEASURED = 5372368 cyc
TS3 -> BIT-EXACT REMATERIALIZATION: recomputed task-hash EQUALS the original, frames differ, mapping identical OK
...
TS3 resumed-to-completion final counter=8 (target 8), exited=y -> the REBUILT process ran correctly OK
```

The recomputed task-hash equals the original from TS1, bit for bit; the
address-space root re-derived from the brand-new page tables matches; the physical
frames are all different (data #1 -> #5, stack #2 -> #4, PML4 `0x2db000` -> `0x2d7000`)
while the logical mapping and contents are identical; and the rebuilt process resumes
in ring 3 and runs to completion (its counter reaches the target and it exits).

### TS4 - authority safety at process granularity (the non-negotiable property)

On rebuild each capability slot is re-minted via `cvsasx_sw_cap_remint` under the
EXACT ceiling from the dematerialized descriptor. The re-mint under the same ceiling
succeeds with authority identical (not wider). An adversarial rebuild with a WIDER
authority request is REFUSED by the gate with its distinct reason: wider bounds ->
`CVSASX_ERR_BAD_BOUNDS`; an added permission -> `CVSASX_ERR_AMPLIFY_PERMS` - exactly
as US3 / L3 / C1 / PM1 refuse at their granularities.

Proof (observed): the re-mint success under the same ceiling, then the two
adversarial wider-authority attempts each refused with their distinct code. A
rematerialized process can NEVER come back with wider authority than it left; this is
the C1 anti-amplification invariant holding at PROCESS granularity.

// OPEN: a machine-checked Coq extension of `anti_amplification` (proofs/Carmix.v) to
whole-process rebuild is NOT done this cycle. The runtime adversarial refusal is the
evidence; the Coq extension is the named follow-up. No proof is claimed that was not
done.

Recorded boot:

```
TS4 re-mint under the SAME ceiling: status=0 minted-len=128 (ceiling=128) -> ACCEPT, authority IDENTICAL (not wider) OK
TS4 ADVERSARIAL wider bounds (len=129>ceiling): status=9 (DISTINCT: CVSASX_ERR_BAD_BOUNDS) REFUSED, no amplification
TS4 ADVERSARIAL added perm (+STORE_CAP): status=12 (DISTINCT: CVSASX_ERR_AMPLIFY_PERMS) REFUSED, no amplification
TS4 -> AUTHORITY SAFETY AT PROCESS GRANULARITY OK
```

Re-minting the full ceiling succeeds (status 0, minted length 128 == ceiling 128  - 
authority identical, not wider). A wider-bounds request is refused with status 9
(`CVSASX_ERR_BAD_BOUNDS`); an added permission is refused with status 12
(`CVSASX_ERR_AMPLIFY_PERMS`) - the same distinct codes US3 / L3 / C1 / PM1 reject by.
A rematerialized process can never come back with wider authority than it left.

### TS5 - the break-even (is whole-process collapse ever worth it?)

On the same workload: (a) the TS2+TS3 full canonicalize + dematerialize +
rematerialize cost, and (b) the existing C3 keep-the-page-table-root-resident
resume cost (`g_c3_fromhash_cyc`). Full collapse frees more frames (the page-table
frames too) but pays more to rebuild them. The break-even absence D* is computed with
the same linear model PP3 uses (a freed frame is worth one avoided resume-from-hash
per `PP_REUSE_WINDOW` cycles it is held): `D* = extra_cost * window / (extra_frames *
fromhash)`.

Proof (observed): the measured (a) and (b), the extra frames freed and extra cost
paid, and the computed D*. Honest conclusion: full collapse pays ONLY for a deeply
idle process (descheduled longer than D*); for a process that wakes soon, keeping the
root resident (C3) is cheaper. This is the process-level analogue of the PP page
policy; collapse is not claimed to be the common case.

Recorded boot:

```
TS5 (a) full collapse  cost = 8864896 cyc (demat 3492528 + remat 5372368), frees 9 frames (incl. the page-table frames)
TS5 (b) keep-root C3   cost = 443385 cyc (resume-from-hash, page-table root resident), frees 2 frames
TS5 extra frames freed by full collapse = 7; extra cost paid = 8421511 cyc
TS5 break-even absence D* = 2713382 cyc (= extra_cost*window/(extra_frames*fromhash); MEASURED inputs, window=1000000)
TS5 -> BREAK-EVEN MEASURED; honest 'deeply idle only' conclusion OK
```

Full collapse costs 8.86M cyc and frees 9 frames; keeping the page-table root
resident (C3) costs 0.44M cyc and frees 2. Full collapse therefore frees 7 extra
frames (the code and page-table frames C3 keeps) at 8.42M extra cyc, for a break-even
absence of about 2.71M cyc: only a process descheduled longer than that nets a gain.
For a process that wakes soon, keeping the root resident is cheaper. (rdtsc under TCG
is noisy, so the exact figures move run to run; the ordering and the conclusion do
not.)

## Proven core byte-identical

`git status` shows only `kernel/kernel.c` modified (the TS0-TS5 section plus a
one-line capture of the clean kernel cr3 and the call site) and the new
`kernel/TASKSTATE_LOG.md` / `kernel/boot_taskstate.log`. The comment-stripped diff of
every source file under `gate/ cap/ carmix/ store/ sls/` against `HEAD` is empty:

```
identical: gate/backend_cheri.c        identical: cap/cap_custodian.c     identical: store/blake3_wrap.c
identical: gate/cvsasx_gate.h          identical: cap/cap_remint.S        identical: store/cvsasx_store.h
identical: gate/executor.c             identical: cap/cap_strip.c         identical: store/epoch_tree.c
identical: gate/optimize.c             identical: cap/cvsasx_cap.h        identical: store/epoch_tree.h
identical: gate/sfi_checker.c          identical: cap/cvsasx_pir.h        identical: store/object_store.c
identical: gate/wasm_frontend.c        identical: carmix/backend_sw_gate.c identical: store/store_mem.c
                                       identical: carmix/cvsasx_swcap.h   identical: sls/backend_cheri.c
                                       identical: carmix/swcap.c          identical: sls/cvsasx_sls.h
                                                                          identical: sls/sls.c
verdict: PROVEN CORE BYTE-IDENTICAL (all 23 files)
```

The link-time identity checks also hold on this build:

```
E0 link OK: kernel.elf = 363408 bytes (backend linked in)
E0 undefined symbols : 0  (expect 0)
E0 libc/host symbols : 0  (expect 0; whole-name match)
E0 CHERI instructions: 0  (expect 0)
```

## Regression suite

The full suite (B / E / R / desktop S/A/E / P / U / M / F / US / L / C / PP / FA /
PM) plus the new TS group was re-run at `BOOT_SECS=180` (full log:
`kernel/boot_taskstate.log`). 77 stage verdicts print OK, including all six TS
stages. The only `*** FAIL` in the whole run is **PM0**:

```
PM0 -> *** FAIL (see stall report) ***
```

This is a PRE-EXISTING stall: it appears identically in the unmodified baseline boot,
and PM0 runs before `run_taskstate`, so this work neither caused nor can affect it. It
is named here honestly rather than papered over. The `M0 exhaustion ... FAILED LOUDLY
(fail-closed)` and `F2 -> FAIL-CLOSED ON VERIFY FAILURE OK` lines are intended
fail-closed assertions (passing), not stage failures. The software desktop (E4) comes
up after TS, so the kernel is intact past the TS group.

Zero NEW FAILs were introduced.

## OPEN items (named, not hidden)

- Coq extension. The machine-checked `anti_amplification` proof covers the abstract
  capability algebra; extending it to whole-process rebuild is future work. TS4
  presents the runtime adversarial refusal as the evidence and names the Coq
  extension as the follow-up.
- Shared mappings across processes. TS0-TS5 canonicalize and collapse a single
  process. Two processes that share a read-only page by hash already deduplicate at
  the backing-frame level (the code frame is one physical frame, refcounted), but a
  canonical form that represents cross-process page-table SHARING (so a shared subtree
  is stored once and re-installed into multiple rebuilt spaces) is deferred.
- Sparse / large address spaces. The canonicalization walks the USER subtree and
  costs work proportional to the number of present mappings; a very sparse but
  wide address space pays the walk per present mapping, and a production form would
  want a skip-structure over empty index ranges. The measured cost here is for the
  small PM-style process used in the proofs.
- Live (non-quiescent) collapse. Only a descheduled, quiescent process is collapsed,
  because canonicalization needs a consistent snapshot. Collapsing a running address
  space would need a quiesce-and-snapshot protocol, out of scope.

## Honest notes

- The TS3 rebuild is serviced through the core materialize path directly, not a live
  fault; the serial line says so (the PM2/F2 precedent).
- The "frames differ" demonstration in TS3 holds spacer frames so the LIFO allocator
  cannot hand back the just-freed slots; placement-independence itself is proven in
  TS0, and the spacer use is logged.
- No behavior elsewhere in the kernel changed; nothing about the public repository is
  pushed. This lands as a verified dev-tree snapshot.
