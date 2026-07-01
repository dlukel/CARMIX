# CU_LOG.md - CYCLE 5: CORE USERLAND COMPLETION (CU1-CU6)

Capability-mediated, content-addressed userland with NO POSIX ambient authority.
`run_cu()` in `kernel/kernel.c` composes the proven pieces into the programmer-visible
model. It invents nothing and touches nothing under the proven core; it only calls
existing primitives.

## What it reuses (no reimplementation)

- U-1 spawn / bounded authority / capability IPC: `u1_spawn`, `u1_cap_send`, `u1_cap_recv`.
- U-2 real heap + GC-coupled free: `u2_malloc`, `u2_free`, `u2_mem_release`, and the
  committed refcount/tombstone path (`gc_obj_put`, `gc_rc_apply/get/find`, `gc_freeblk`).
- FS over the Merkle namespace: `fs_dir_put`, `fs_resolve`, `fs_read`.
- The content store: `cvsasx_store_put/get`, `cvsasx_blake3`, `cvsasx_hash_eq`.
- The gated capability handlers over the bounded CSpace: `u0_console_write`,
  `u0_store_read`, `u0_store_write` over `u0_cspace`, `conc_make_pir`, the `CAP_*` model.
- U-3 / SMP cross-core dispatch: `ap_kick`, `ap_wait`, `u3_drf` on the AP, `percpu`.

## Placement (honest)

`run_cu()` is called in `kmain` immediately after `run_smp()` and before `run_u3u4()`.
That is the only window where a second core is live: `run_smp` leaves the AP in its
worker loop, and `run_u3u4` parks it (`g_ap_shutdown`) at its end so every single-CPU
stage after it runs unchanged. CU therefore reuses the U-* functions and data, not their
print-order. The ring-3 hardware crossing is demonstrated in U-0/U-1 and is not re-run
here; CU works at the in-kernel process/cap level (the level U-3/U-4 also use), with
every mutating/security branch showing a real refusal path.

## Seams (verified against real serial output, single boot)

- CU-1 runtime + environment: the program's WORLD = its environment subtree (a content-
  addressed root) + its granted cap set. args/config are content-addressed objects named
  by hash inside the env dir. The program "reaches main": it resolves `cfg` within its
  world, reads it through the granted read cap, and re-hash-verifies it. A name OUTSIDE
  the subtree (`etc`) is unreachable - there is NO ambient global namespace.
  Observed: `granted caps=2/8 (rest EMPTY); reached main ... =y; name OUTSIDE the world
  unreachable (NO ambient namespace)=y`.

- CU-2 resource management under the ceiling: a resource limit IS a capability bound. An
  over-quota write (129 B against a 128 B write cap) is REFUSED. An over-limit spawn
  (requesting an un-held cap) is REFUSED at birth (anti-amp). Lifecycle: three GC-heap
  regions are acquired (refcount 1 each) and at exit are reclaimed via `u2_mem_release`
  -> the committed `gc_rc` decrement + tombstone + `gc_freeblk`, refcounts -> 0.
  Observed: `over-quota write REFUSED=y; over-limit spawn ... REFUSED=y; ... refcounts->0=y,
  bytes reclaimed=12288 (no exit-leak)`.

- CU-3 capability-mediated services: a registry (a Merkle name->hash directory) is looked
  up only via a capability over its root; a no-cap open is REFUSED. Storage/FS read is
  gated by a store-read cap; a no-cap read is REFUSED. Console/IO is gated by CAP_CONSOLE;
  a no-cap console write is REFUSED. Every service is reached ONLY through a cap.
  Observed: all three services succeed via a cap and all three refuse without one.

- CU-4 a real multi-process program, end-to-end: parent spawns a bounded child
  {console, store-read}; coordinates via capability IPC (data + a cap crossed; an un-held
  cap send REFUSED) and across the two SMP cores via `ap_kick`/`u3_drf` producing a
  content-addressed coordinated result; uses FS + services via caps; allocates/frees over
  the real heap and reclaims a GC-heap region at exit; and enforces authority - the child
  (no write cap) attempting `store_write` is REFUSED.
  Observed: `child spawned bounded=y; cap-IPC coordinate ... =y; cross-core coordinated
  result (content-addressed)=<hash>; ... child over-reach ... REFUSED=y; end-to-end
  authority enforced=y`.

- CU-5 the programmer model, honestly. WHAT DIFFERS: name by HASH (not a path), hold
  explicit CAPABILITIES (no ambient authority/root/cwd), immutable content (a write
  creates a new object + re-roots). WHERE STRICTLY BETTER: no confused-deputy, free
  versioning + snapshots, dedup by construction, every read re-hash-verified. THE REAL
  FRICTION (surfaced, not hidden): names are opaque 32-byte hashes; there is no in-place
  mutation - every write RE-ROOTS the tree (measured, this run); caps are carried
  explicitly with no ambient fallback; a growing history retains old roots until GC.

- CU-6 measure (rdtsc, per run): startup (build CSpace + env resolve), service-call
  (registry lookup + FS re-verify read), spawn+coordinate+reclaim, and cross-core
  dispatch+join. Numbers are rdtsc-measured each boot and printed on the CU-6 line; the
  re-root friction cost is printed on the CU-5 friction line.

## Disproved

- Ambient authority: every resource and service is reached via a capability, and each
  unauthorized reach is REFUSED (registry, FS read, console, spawn, store_write).
- POSIX-for-convenience: names are hashes; no ambient POSIX syscalls were added.
- Amplification: over-quota write and over-limit spawn are both REFUSED.
- Friction hidden: opaque hash names and the measured re-root cost are surfaced.
- Exit-leak: exit drops held caps through the committed refcount path; refcounts reach 0
  and reclaimed bytes are reported.

## Scope

Single-CPU per core. Composes only existing primitives. Demonstrated at the in-kernel
process/cap level (the level U-3/U-4 use); the ring-3 hardware crossing is demonstrated
in U-0/U-1 and not re-run here. Every mutating/security branch shows a real refusal path.
