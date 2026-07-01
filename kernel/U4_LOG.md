# U4_LOG.md - the namespace as a Merkle DAG, delegation as an attenuation chain

Honest record of U-4: the namespace is a content-addressed Merkle structure (a directory is
a name-to-hash object), and delegation is a kernel-enforced attenuation chain (authority
strictly decreases at every hop, revocation walks the chain).

All figures rdtsc-measured on this run.

## Scope

U-4 builds on the existing content store and the U-1 derivation records. It is single-CPU.
It reuses cvsasx_store (put/get), cvsasx_blake3, and the U-1 delegation model. Directories
are real content-addressed objects, not a POSIX name table.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical. U-4 is
in kernel/kernel.c, reusing the store and the U-1 derivation.

## What was built and shown (observed)

### U4-1 the content-addressed namespace (directories ARE name-to-hash maps)

```
U4-1 directory object stored, own content hash=09eb8864e22843c4; resolve 'b' re-hash-verified=y; changing an entry yields a DIFFERENT directory hash=y
U4-1 -> NAMESPACE IS A CONTENT-ADDRESSED MERKLE DAG OK
```

A directory is a stored object mapping names to the hashes of other objects, and it gets its
own content hash. Resolving a name walks the name-to-hash entries and fetches the target
object, re-verified by re-hashing it against the stored hash. Changing an entry produces a
different directory hash, so the namespace is content-addressed and immutable-by-hash: a
changed tree is a new name, exactly the Merkle-DAG shape of a version-control tree.

### U4-2 delegation chains with kernel-enforced attenuation

```
U4-2 delegation chain A(256)->B(128)->C(64): strictly attenuates=y; over-delegation (C grant 200 > its 64) refused=y
U4-2 -> DELEGATION ATTENUATES AT EVERY HOP (anti-amp transitive) OK
```

A capability is delegated along a chain A to B to C, each hop producing a derived capability
whose authority is a strict subset of its parent (256 to 128 to 64). An attempt to delegate
more authority than the delegator holds (C granting 200 over its own 64) is refused. The
anti-amplification ceiling extends transitively across the chain at arbitrary depth.

### U4-3 revocation walking the chain

```
U4-3 revoke A->B: B revoked=y C (descendant) revoked=y unrelated cap untouched=y
U4-3 -> REVOCATION WALKS THE CHAIN OK
```

Because the delegation chain is recorded, revoking a capability reaches its descendants.
Revoking the A-to-B delegation revokes B and its descendant C, while an unrelated capability
is unaffected. Revocation walks the derivation subtree.

### U4-4 richer spawn from a content-addressed image

```
U4-4 spawn from image: process identity = image hash e0e79fc31609c817..; initial authority 128 is a delegated subset of the spawner's 256 =y
U4-4 -> SPAWN FROM CONTENT-ADDRESSED IMAGE, DELEGATED BOUNDED AUTHORITY OK
```

A process is spawned from an image named by its content hash, so the process identity IS the
hash of its image, and its initial capability set is a delegation-derived subset of the
spawner's authority.

### U4-5 measurement

```
U4-5 rdtsc: namespace resolve (store fetch)=1118 cyc; spawn-from-image (hash+store)=25125 cyc
```

Resolving a name (a content-store fetch) measured about 1118 cyc. Spawn from a content-addressed
image (hashing the image plus the store put) measured about 25125 cyc.

## Regression

The full regression re-runs green except the same pre-existing PM0 stall. All prior stages,
including U-3, still pass.

## Forbidden dodges, each disproven

- D4 namespace not content-addressed: a directory is a real content-addressed object whose hash
  changes when an entry changes.
- D5 delegation amplification: every hop strictly attenuates and an over-delegation is refused.
- D7 proven-core drift: the nine proven modules are byte-identical.
- D8 separability: U-4 follows the U-3 COMPLETE checkpoint.

This banks the namespace as the Merkle DAG and delegation as a kernel-enforced attenuation chain.
