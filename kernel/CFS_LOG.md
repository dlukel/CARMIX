# CFS: Full content-addressed filesystem (Cycle 2)

Stage `run_cfs()` in `kernel/kernel.c`, called from `kmain` right after `run_fs()`.
It extends the committed FS (run_fs, fs_dir_put/fs_resolve/fs_read) into a full
filesystem and, critically, wires deletion into the COMMITTED garbage collector
(gc_rc refcount + tombstone reclaim, the same path U-2 and DS6 use).

Every object lives in two places on purpose:

* `u0_store` is the logical content-address space. Reads re-hash the bytes and
  reject a mismatch, so the medium is untrusted. Bytes are never evicted here;
  this is the address space, not the backing store.
* the GC arena (`gc_obj_put` blocks, refcounted by `gc_rc` under a new
  `CFS_DOM=8`) is the reclaimable backing. When a tree goes unreachable its
  blocks are tombstoned and returned to the allocator free list.

Refcounting is done on edges at construction time, exactly once per object:
`cfs_mat` gives a newly-live object a block and, if it is a directory, adds
`+1` to each child (its outgoing edges). A root is pinned with an extra `+1`
(`cfs_pin`). `cfs_unref` removes one reference; at zero it tombstones, frees the
block, and recurses into the directory's children (each dropped edge cascades).
Because the content-addressed graph is acyclic by construction (proven in the GC
stage: an object's hash is computed over its children's hashes, so no object can
name an ancestor), plain refcounting is complete without a cycle collector.

## Scope (honest)

* Single-CPU. No concurrent mutator, no cross-core refcount races.
* Demonstrated on small fixed trees; the re-thread / reclaim / diff mechanisms
  are uniform over depth and fan-out (fan-out capped at `FS_MAXENT=8` per dir).
* Large files are shown as file-level Merkle trees of up to 8 chunks
  (`CFS_CHUNK=64`); the chunk-index is an ordinary directory object, so chunks
  are refcounted edges and GC-reclaim on it for free.
* Structural diff assumes a shared name set between the two roots (adds/removes
  on one side are counted, but full three-way name reconciliation is not built).
* Crash-consistency of the root is argued by the in-kernel ordered-prefix
  discipline (allowed alternative to the 3-boot persist/revive): the root is
  made durable with `dur_put` only AFTER the whole tree is written and flushed.
* NO POSIX layer, NO in-place mutation, NO ambient access, NO unverified read.

## Seams, verified against real serial output (one representative boot)

Numbers are `rdtsc`-measured every boot and vary with QEMU jitter.

* CFS-1 full file ops. create + read re-hash-verified; write, append (len
  48->64), truncate (len 48->24) each produce a NEW content hash, never an
  in-place edit. A 200-byte file becomes 4 chunks under a Merkle index; read
  reassembles and re-verifies every chunk; a tampered chunk address is rejected.
    ```
    CFS-1 create: file object bdce9cbf331675b4 len 48; read re-hash-verified=y
    CFS-1 write->new hash=y (NOT in-place); append->new hash & len 48->64=y; truncate->new hash & len 48->24=y
    CFS-1 large file = 4 chunks under a Merkle index 6c729a2bbbde0185; read reassembles+re-verifies all chunks=y; a tampered chunk address is rejected=y
    ```
* CFS-2 directory ops. nested resolve to depth 3 (each step re-hash-verified);
  mkdir / rename / move / unlink each RE-THREAD to new dir and root hashes; an
  open is gated by a capability naming a root, an un-granted root is refused (no
  ambient access).
    ```
    CFS-2 nested resolve root/a/b/file (depth 3, each step re-hash-verified)=y
    CFS-2 mkdir new dir+root hash=y rename new dir+root=y move new dirs+root=y unlink new dir=y (each RE-THREADS, no in-place edit)
    CFS-2 open gated by a root capability=y; open of an un-granted root refused (no ambient access)=y
    ```
* CFS-3 GC integration (the key seam). v1 is a tree {sub->{f:fA}, keep->{g:gy}}.
  A write replaces f, producing v2 {sub->{f:fB}, keep} that SHARES `keep`
  (refcount 2). A snapshot of v1 is a second retained root reference. Dropping
  the live handle reclaims nothing (the snapshot still pins v1); releasing the
  snapshot reclaims exactly the 3 now-dead objects (rootV1, sub, fA) via the
  committed tombstone + `gc_freeblk` path, while the shared `keep` (still
  referenced by v2) is refcount-protected and survives.
    ```
    CFS-3 snapshot pins v1 (refs=2): dropping the live handle reclaims 0 objects, blocks freed=0; v1 tree still pinned & intact=y
    CFS-3 snapshot released: DEAD tree reclaimed=3 objects (blocks freed=3), all tombstoned=y
    CFS-3 SHARED subtree keep referenced by the retained v2 NOT reclaimed (refcount=1, live=y, v2 still resolves keep=y)
    CFS-3 uses the COMMITTED gc_rc refcount + tombstone reclaim (gc_rc_apply + gc_freeblk), the SAME path as U-2/DS6
    ```
* CFS-4 durable crash-consistent root. The root is `dur_put` only after the
  whole tree is written and flushed, so the durable root always names a complete
  tree (a crash before the root write leaves the previous complete root; a crash
  after leaves the new complete one).
    ```
    CFS-4 root persisted durably AFTER the whole tree was written+flushed (root update = the commit point)=y
    ```
* CFS-5 snapshots, history, structural diff by hash. Two roots differing only in
  sub/f, with an identical sibling `keep`: the diff descends root and sub only
  (2 dir-pairs), finds 1 changed leaf, and PRUNES the identical `keep` (never
  descended, because its hash matches). An older root still resolves its own
  version (history is free).
    ```
    CFS-5 structural diff of two roots by hash: changed leaves=1, dir-pairs descended=2 (the identical sibling 'keep' was PRUNED, never descended)=y
    CFS-5 history: the older root b246723db5f8956f still resolves its own version of sub/f=y (snapshots + history are free)
    ```
* CFS-6 measurement (rdtsc, this run). file-op, dir-op, re-root by depth (grows
  with depth), GC-reclaim, one genuine 2-block root-persist, structural-diff.
    ```
    CFS-6 rdtsc: file-op(create+read)=81298 dir-op=92233 re-root depth2=379650 depth3=581103 (cost grows with tree depth)
    CFS-6 rdtsc: GC-reclaim(tombstone+free a 3-object tree)=58664 root-persist(dur_put)=1678172 structural-diff=188202 cyc
    ```

## What is disproved (each with a real branch, not a claim)

* In-place mutation: every write/append/truncate/rename/mkdir/move yields a NEW
  hash; the old root still resolves the old version (CFS-1, CFS-2, CFS-5).
* Unverified read: a corrupted/tampered content address is rejected by re-hash
  (CFS-1 tamper, and the FS3 corrupt-reject it builds on).
* Ambient access: an open is gated by a root capability; an un-granted root is
  refused (CFS-2).
* GC-not-integrated: dead trees ARE reclaimed and snapshots ARE pinned, both via
  the committed refcount + tombstone path; shared subtrees are protected
  (CFS-3, 3 objects reclaimed, 3 blocks freed, shared `keep` survives).
* Root-not-crash-consistent: the durable root is the ordered commit point after
  all tree writes (CFS-4).
* Hidden cost: the re-root cost (grows with tree depth) and the GC-reclaim cost
  are both measured and printed (CFS-6).

## Proven core

Untouched. New code is confined to `kernel/kernel.c` (the `run_cfs` stage and
its helpers) and this log. Full regression stays green except the one
pre-existing accepted PM0 stall (`*** FAIL`); the M0 exhaustion and F2 verify
negatives reach their fail-closed / OK lines.
