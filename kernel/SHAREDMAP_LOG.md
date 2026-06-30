# SHAREDMAP_LOG.md - shared mappings in the canonical form (SM0-SM5)

This log records the SITVOSH cycle that makes a content-addressed SHARED physical
frame survive a whole-process dematerialize/rematerialize round trip as ONE physical
frame for BOTH owners, with per-owner authority re-minted through the proven gate and
never widened.

The capability requirement, stated as the bar: two processes sharing ONE physical
frame by content hash must STILL share ONE physical frame after BOTH round-trip,
proven by the actual physical frame INDEX being EQUAL for both owners after rebuild,
the shared page's content hash unchanged, each owner's authority re-minted under its
OWN ceiling.

Headline result (SM3, the capability gate): the shared frame SURVIVED. After both
processes round-trip, the writer and the reader both resolve the shared VPN to the
SAME physical frame (frame index EQUAL), refcount 2, content hash unchanged, and a
coherence write/read through each owner's OWN page tables confirms it is genuinely
one physical frame, not two private copies. The forbidden dodge (two private frames
with identical content, relabeled shared) did NOT occur.

All numbers below are COMPUTED on the metal (rdtsc / counters), never hardcoded, and
vary run to run under TCG. Figures are from the recorded boot (`BOOT_SECS=180`, full
log `kernel/boot_sharedmap.log`).

## A. The representational gap (why sharing breaks without this)

The TS canonical leaf is `(VPN, page-content-hash, perms)`. It captures CONTENT
identity (same hash) but NOT SHARING identity (same physical frame, one object with
more than one owner). On rebuild, TS allocates a FRESH frame per content hash per
process (writable copied private, read-only materialized), so two processes that
shared one physical frame each rebuild a PRIVATE copy: content preserved, sharing
destroyed, the shared identity silently forks into two frames. The missing notion is
a SHARED OBJECT with an identity and a refcount that the per-process canonical trees
REFERENCE rather than each inlining a private copy. This is the content-addressed-
storage / single-level-store distinction between one object referenced by many
holders versus copying it per holder; CARMIX already has the resident-by-hash side
(rm_materialize over a resident-set, the residency refcount used for read-only code,
PM3 dedup), but the TS canonical form did not carry the reference, so the round trip
forked it.

## B. The shared-object layer (the new machinery, on the proven plumbing)

- A leaf carries a SHARED bit. `MM_SHARED` is a software-available PTE bit (AVL bit
  9, ignored by hardware). It is folded into the canonical perm mask, so
  `ts_canonicalize` records it in the leaf and the descriptor. A PRIVATE leaf never
  sets it, so TS canonicalization and rebuild are unchanged (the bit is 0 on every
  TS leaf, so no TS hash changes).
- The shared object is identified by its content hash. Its physical frame, refcount,
  and residency live in the EXISTING residency plumbing: the resident-set maps hash
  to frame, and the frame database holds the refcount. The per-owner authority
  ceiling lives in each owner's OWN task object (not pooled). The SM1 proof surfaces
  this as a table row: `hash -> frame -> refcount -> per-owner ceiling`.
- On rebuild, a SHARED leaf takes the ATTACH-or-materialize path
  (`rm_materialize` over the resident-set): if the object is already resident (the
  first sharer rebuilt it), the second sharer's page table is pointed at the SAME
  physical frame and the refcount is bumped; otherwise the first sharer materializes
  it once. This is the extension that makes a WRITABLE shared object survive a round
  trip shared, not just read-only code deduped at one instant: the SHARED bit
  overrides the TS "writable leaf is copied private" rule, so a writer's mapping
  attaches to the shared frame instead of copying it.

Honest note on an invariant relaxed deliberately: M2's "a content-addressed (shared)
frame is read-only by policy" is relaxed for the single-writer case, where one owner
maps the shared frame read-write. This is the named scope, not a silent change.

## C. The cross-round-trip ownership protocol (the messy part, solved not routed)

Dematerializing two sharers and rematerializing them is not atomic. The protocol,
built on `refdrop` / `rm_materialize` / the resident-set:

- Dematerialize: each owner's SHARED leaf calls `dematerialize_frame` (content into
  the store, idempotent) then `refdrop` (refcount-aware). The frame is freed ONLY
  when the LAST owner releases it (refcount reaches 0). When one owner is
  dematerialized and the other is not, the frame is resident with refcount 1, which
  is correct.
- Rematerialize: order-independent. Whichever owner rebuilds first calls
  `rm_materialize`, which MISSES the resident-set and MATERIALIZES the frame once
  (refcount 1); the second calls `rm_materialize`, HITS the resident-set, and
  ATTACHES to the SAME frame (refcount 2). The refcount is reconstructed by the
  attach.
- Hazards named and prevented: double-materialize (the FORBIDDEN dodge) is prevented
  by the resident-set hit on the second rebuild (it attaches, never re-creates);
  double-free is prevented by `refdrop` freeing only at refcount 0 (the first
  owner's demat does NOT free the frame because the second still maps it); refcount
  desync is prevented by using the single residency refcount as the source of truth.

## D. The per-owner authority rule (sound under sharing, never pooled)

Each owner holds its OWN ceiling to the shared object (writer LOAD|STORE, reader
LOAD). On rebuild, each owner's authority is re-minted INDEPENDENTLY through the
proven gate (`cvsasx_sw_cap_remint`) under its own ceiling; the shared-object layer
holds physical identity plus content, while AUTHORITY stays per-owner and
gate-mediated, never pooled. The page is mapped read-write ONLY if the gate grants
STORE, so a read-only owner attaching to a frame a read-write owner also maps comes
back READ-ONLY, and an attempt to attach with wider authority than that owner's
ceiling is REFUSED with a distinct reason. This is the anti-amplification property
holding in the presence of sharing.

## E. The limits (honest scope, distinct from the forbidden dodge)

- Sharing mode delivered: SINGLE-WRITER-SHARED (one owner read-write, one read-only,
  ONE physical frame). Read-shared is the special case where both map read-only.
- QUIESCENT: the round trip is a deschedule-point operation; there is no write during
  the round trip. A post-rebuild write by the writer is visible to the reader (real
  shared memory, demonstrated by the coherence test), but it changes the content
  hash, which is the content-addressing/coherence frontier.
- OUT of scope, flagged not faked: full multi-writer coherence (two writers,
  write-shared) and live (non-quiescent) sharing.
- The line: single-writer-shared and saying so is HONEST scope of the hard problem.
  Two private frames with identical content called shared is the FORBIDDEN dodge,
  and SM3 proves it did not happen (equal frame index plus a coherence write/read).

## SM stage -> proof map (recorded boot)

### SM0 - establish real sharing

```
SM0 shared object hash = ffcad60cfaaae98d9f040e4300370180c3f68851125d297b5ddfac639caa3265
SM0 writer(W) sees shared frame#1  reader(R) sees shared frame#1  EQUAL=y refcount=2
SM0 private data: W frame#0 R frame#2 distinct=y
SM0 -> ONE PHYSICAL FRAME, TWO OWNERS, refcount 2 (real sharing established) OK
```

The writer and reader resolve the shared VPN to the SAME frame (refcount 2) before
any round trip; their private data pages are distinct frames (privacy preserved
alongside sharing).

### SM1 - the shared-object layer

```
SM1 shared-object table: hash ffcad60cfaaae98d9f040e43.. frame#1 refcount=2  owner ceilings: W perms=0x...c (LOAD|STORE)  R perms=0x...4 (LOAD), independent (not pooled)
SM1 leaf VPN=0x0000000042000000 shared-bit=0 (PRIVATE)
SM1 leaf VPN=0x0000000044000000 shared-bit=1 (SHARED object)
SM1 -> SHARED-OBJECT LAYER: shared leaf carries the bit, private leaf does not, per-owner ceilings distinct OK
```

The shared leaf carries the bit; the private leaf does not; the per-owner ceilings
are distinct (not pooled). The private mapping is unchanged from TS behavior.

### SM2 - dematerialize both, lifetime correct

```
SM2 after W dematerializes: shared refcount=1 shared frame still resident=y (NOT freed: R still maps it; double-free avoided)
SM2 after R dematerializes: shared frame resident=n (freed at last release) content-in-store=y
SM2 pool free 253 -> 256 (both processes' frames returned)
SM2 -> LIFETIME CORRECT: refcount 2->1->0, frame freed only at last release, content persists OK
```

Refcount drops 2 to 1 when the first owner dematerializes (frame still resident,
mapped by the second), then to 0 when the second dematerializes (frame freed); the
shared object's content persists in the store. Double-free avoided.

### SM3 - the capability gate (one shared frame survives the round trip)

```
SM3 serviced via the CORE materialize/attach path directly (rm_materialize over the resident-set), NOT a live #PF (the PM2/F2/TS3 precedent)
SM3 after W rebuilt (first): shared frame#2 refcount=1 (materialized once)
SM3 after R rebuilt (second): W sees frame#2 R sees frame#2  EQUAL=y refcount=2
SM3 shared content hash now = ffcad60cfaaae98d9f040e4300370180c3f68851125d297b5ddfac639caa3265  unchanged-from-SM0=y
SM3 coherence: wrote sentinel via W's RW mapping, read it back via R's RO mapping -> R saw it=y (GENUINELY one physical frame, not two)
SM3 round-trip cost MEASURED = 8776018 cyc
SM3 -> CAPABILITY GATE PASSED: the shared frame SURVIVED the round trip as ONE physical frame for BOTH owners OK
```

SHARED FRAME SURVIVED: YES. The first owner to rebuild materialized the frame once
(refcount 1); the second attached to the SAME frame (refcount 2). Both owners resolve
the shared VPN to the same physical frame index. The content hash is unchanged from
SM0. The coherence sub-proof (write a sentinel through the writer's read-write
mapping, read it back through the reader's read-only mapping, in their OWN page
tables) confirms it is genuinely one physical frame. The service used the core
materialize/attach path directly, not a live page fault, stated honestly (the
PM2/F2/TS3 precedent).

### SM4 - per-owner authority on the shared frame

```
SM4 W (writer) re-mint LOAD|STORE under its OWN ceiling: status=0 minted-perms=0x...c -> ACCEPT, READ-WRITE (its own authority, not pooled) OK
SM4 R (reader) re-mint LOAD under its OWN ceiling: status=0 minted-perms=0x...4 -> ACCEPT, READ-ONLY (stays read-only) OK
SM4 page perms: W shared PTE writable=y R shared PTE read-only=y
SM4 ADVERSARIAL reader attaches READ-WRITE (LOAD|STORE) under its LOAD-only ceiling: status=12 (DISTINCT: CVSASX_ERR_AMPLIFY_PERMS) REFUSED -> sharing the frame NEVER widens authority
SM4 ADVERSARIAL wider bounds (4097>4096): status=9 (DISTINCT: CVSASX_ERR_BAD_BOUNDS) REFUSED
SM4 -> PER-OWNER AUTHORITY SOUND UNDER SHARING: each re-minted under its OWN ceiling, read-only stays read-only, wider REFUSED OK
```

Each owner's authority is re-minted under its own ceiling: the writer comes back
read-write, the reader read-only, mapped accordingly in their page tables. The
reader attaching with wider authority (request STORE) under its read-only ceiling is
refused with `CVSASX_ERR_AMPLIFY_PERMS` (status 12); a wider-bounds request is
refused with `CVSASX_ERR_BAD_BOUNDS` (status 9). Sharing the frame never pools or
widens authority.

### SM5 - limits and cost

```
SM5 sharing mode delivered: SINGLE-WRITER-SHARED (W read-write, R read-only, ONE frame), QUIESCENT
SM5 round-trip cost = 8776018 cyc (attach reconstructs the refcount via the residency resident-set)
SM5 W address-space root = 3b9b4b29ed439785122a76a2590d33ef.. contains shared hash=y
SM5 R address-space root = ec825f05406e9869af22d532a6c6bdae.. contains shared hash=y (both roots reflect the shared content; roots differ because per-owner perms differ)
SM5 -> LIMITS NAMED; costs measured; shared hash in BOTH roots; honest scope distinct from the dodge OK
```

The shared object's content hash enters BOTH processes' address-space roots, so both
sharers' roots reflect the shared content; the roots differ only because the
per-owner permissions differ (writer read-write, reader read-only), which is correct
per-owner authority, not a content divergence.

## Proven core byte-identical

`git status` shows only `kernel/kernel.c` modified (the SM0-SM5 section,
`run_sharedmap` and its helpers, the `MM_SHARED` bit added to the canonical perm
mask, and the call site) plus the new `kernel/SHAREDMAP_LOG.md` /
`kernel/boot_sharedmap.log`. The comment-stripped diff against `HEAD` is empty for
every source file under `gate/ cap/ carmix/ store/ sls/`:

```
verdict: PROVEN CORE BYTE-IDENTICAL (all 23 files)
```

Link-time identity checks on this build:

```
E0 link OK: kernel.elf = 392336 bytes (backend linked in)
E0 undefined symbols : 0  (expect 0)
E0 libc/host symbols : 0  (expect 0; whole-name match)
E0 CHERI instructions: 0  (expect 0)
```

## Regression suite

The full suite (B / E / R / desktop S/A/E / P / U / M / F / US / L / C / PP / FA /
PM / TS) plus the new SM group was re-run at `BOOT_SECS=180`. All TS0-TS5 verdicts
remain OK (no regression from the `MM_SHARED` mask extension, which is 0 on every TS
leaf). The only `*** FAIL` in the whole run is PM0:

```
PM0 -> *** FAIL (see stall report) ***
```

This is a PRE-EXISTING stall (present in the unmodified baseline; PM0 runs before
`run_taskstate` and `run_sharedmap`, so this work neither caused nor can affect it).
The software desktop (E4) comes up after SM, so the kernel is intact past the SM
group. Zero NEW FAILs were introduced.

## OPEN items (named, not hidden)

- Full multi-writer coherence (two writers, write-shared). OUT of scope. A write
  changes the content hash, so two independent writers diverge; reconciling that with
  content-addressing (or a coherence protocol over the shared object) is the frontier.
- Live (non-quiescent) sharing. The round trip is a deschedule-point operation; a
  live shared-frame round trip needs the quiesce-and-snapshot work named in the TS
  cycle.
- A machine-checked Coq extension of anti-amplification to the shared-object attach.
  Not done; the runtime per-owner refusal (SM4) is the evidence, the Coq extension is
  the named follow-up. No proof is claimed that was not done.
- Refcount-reconstruction is done by the attach over the residency resident-set; a
  table sized for two owners is shown here. Many owners and a production resident-set
  scaling are not exercised.

## Honest notes

- SM3 is serviced through the core materialize/attach path directly, not a live page
  fault; the serial line says so (the PM2/F2/TS3 precedent).
- M2's read-only-shared-frame policy is relaxed for the single-writer case (one owner
  maps the shared frame read-write); stated as scope, not a silent change.
- The pair is built quiescent (descheduled) and the round trip does not execute ring-3
  code; the shared frame's survival is proven by equal frame index plus a coherence
  write/read through each owner's own page tables, which is a direct physical-sharing
  proof.
- No behavior elsewhere in the kernel changed; nothing about the public repository is
  pushed. This lands as a verified dev-tree snapshot.
