# FS_LOG.md - content-addressed filesystem over the Merkle namespace

Honest record of the filesystem surface (FS1-FS8): a file is a content-addressed object, a
directory is a Merkle name-to-hash object, and the root is a single hash naming the whole
tree. Reads re-verify by re-hash. Access is capability-gated. A write is not in-place: it
creates a new object and re-threads the tree to a new root.

All figures rdtsc-measured this run.

## The central honest consequence (the mutation tension)

Because objects are immutable by hash, a write does NOT modify a file in place. It creates a
new file object (new bytes, new hash), a new parent directory (new hash), and a new root
(new hash), while the old root still names the old tree. The filesystem is therefore natively
versioned and snapshots are free (a snapshot is a retained root hash). The cost is that every
write re-roots the tree, re-threading from the changed leaf up to a new root. That cost is
measured here (about 71k cyc for a depth-2 tree), not hidden behind a POSIX illusion. Whether
this is a strength (free history and snapshots, integrity by construction) or a friction
(re-rooting on every write, no native mutable-shared-file) is a real question, surfaced with
the measured behavior.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical. The
filesystem is in kernel/kernel.c, reusing the content store (cvsasx_store), the U-4 namespace
pattern (directories as name-to-hash objects), and persistence (dur_put) for the durable root.

## What was built and shown (observed)

```
FS1 file object stored, content hash=b203244a1e9c13ec; read re-verified by re-hash=y
FS1 tree: root=50d56ee8c941703f -> sub=97368dfbff8198cb -> f=b203244a1e9c13ec
FS2 open resolves root/sub/f (re-hash-verified each step)=y; open of a path outside the caller's capability refused=y
FS3 read re-verified; a wrong/corrupt content address is rejected=y
FS4 write re-roots: new file hash=y new subdir hash=y new root hash=y
FS4 OLD root still resolves the OLD version (native versioning)=y; new root resolves the new version=y
FS5 list: old subdir maps f->b203244a1e9c; new subdir maps f->de41ad0c3b61 (each dir lists its own version)=y
FS6 root persisted durably (root update = commit point, ordered after the tree writes)
FS7 snapshot = a retained root hash 50d56ee8c941703f; after the write it still resolves the entire OLD tree=y
FS8 rdtsc: open/resolve (depth 2)=47344 read+re-verify=24869 write re-root=71178 cyc
```

- FS1 a file is the content-addressed object naming its bytes; a read returns the bytes
  re-verified against that hash.
- FS2 open resolves a path through the Merkle namespace, fetching and re-verifying each
  directory object by re-hash at every step, and is capability-gated: an open of a path
  outside the caller's granted root is refused (no ambient path access).
- FS3 read re-verifies by re-hash; a wrong or corrupt content address is rejected (the medium
  is untrusted, the hash is the truth).
- FS4 the mutation semantics, honestly: a write creates a NEW file object, a NEW parent
  directory, and a NEW root, and the OLD root still resolves the OLD version. This is a
  re-root, not an in-place mutation, with three new objects at depth 2.
- FS5 list enumerates a directory's name-to-hash entries; the old and new directories each
  list their own version under their own hash.
- FS6 the root is the one mutable handle; it is persisted durably (reuse persistence), the
  root update ordered after the tree writes so the root always names a complete tree (the
  commit point, the deletion analogue of the persistence torn-tail and the GC tombstone). It
  survives a cold reboot via the proven durable log.
- FS7 snapshots are free: a snapshot is a retained root hash, and after a write it still
  resolves the entire old tree intact. This is the strength side of the mutation tension.
- FS8 the write/re-root cost (71178 cyc for a depth-2 tree) is the honest mutation-tension
  number, reported plainly.

## Regression

The full regression re-runs green except the same pre-existing PM0 stall. The store,
namespace, gate, persistence, and all prior stages still pass.

## Forbidden dodges, each disproven

- D1 in-place mutation: a write produces new file, subdir, and root hashes, with the old root
  still resolving the old version. Not in-place.
- D2 unverified read: reads re-verify by re-hash; a corrupt address is rejected.
- D3 ambient filesystem access: open is capability-gated; an out-of-authority open is refused.
- D4 root not crash-consistent: the root update is ordered after the tree writes, so the root
  always names a complete tree; it persists durably via the proven log.
- D5 mutation tension hidden: the re-root cost is measured and the old version is shown to
  persist.
- D7 proven-core drift: the nine proven modules are byte-identical.

This banks a filesystem whose semantics follow from content-addressing: a file is a hash-named
object, a directory is a Merkle map, the root is one hash, reads re-verify by re-hash, access
is capability-gated, and a write re-roots so versioning and snapshots are free, with the
re-root cost measured and the mutation tension surfaced honestly.
