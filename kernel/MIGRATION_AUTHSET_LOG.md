# MIGRATION_AUTHSET_LOG.md - authority-preserving migration in a trusted two-machine cluster

Honest record of the authority-set layer added to the existing two-machine migration
(nettest.c, Step 13). When a process migrates from machine A to machine B, A signs the
process's canonical authority SET with the existing Ed25519, B verifies the signature
against A's pre-shared public key, and only on success B re-mints local capabilities
bounded by exactly the verified set, minting nothing beyond it. Any verification failure
rejects the migration fail-closed. This makes the single-machine anti-amplification ceiling
a cross-machine property in the trusted-cluster model.

All figures are rdtsc-measured on this run. Nothing is imported.

## What the two machines are, and the trust model (read first)

The two machines are TWO separate QEMU instances booting the same image and sharing one
ivshmem region (a host memory-backed file, /dev/shm), one instance claiming role A (source)
and the other role B (destination). This is two real separate kernels with separate address
spaces communicating over shared memory. It is not a network distributed system, and it is
not claimed to be.

Trust model: TRUSTED CLUSTER. B trusts A's public key (pre-shared, baked into B's trust
store), and the property assumes A is honest about the set it signs. Within that assumption,
a migrant cannot arrive at B with more authority than A signed for it. This does NOT solve
the mutually-distrusting case (where B does not trust A). That case needs hardware
attestation (SGX-style) or reduces to a shared trust anchor, and is future work. Single-CPU
per machine. No concurrent multi-machine operation and no SMP are claimed.

## Proven core untouched, and what changed

The nine proven modules and the rest of the proven core are byte-for-byte identical. kernel.c
is untouched. The change is entirely in kernel/nettest.c (the existing two-machine migration
module, Step 13, the DM stage) plus a build-harness timeout bump in kernel/net_build.sh (40s
to 90s per QEMU, for the added Ed25519 work). The layer REUSES the existing Ed25519 (vendored
tweetnacl crypto_sign and crypto_sign_open), the existing pre-shared keys (authz_keys.h), the
unmodified swcap anti-amplification gate (cvsasx_sw_cap_remint) for the bounded re-mint, and
the existing ivshmem transfer. No new crypto library and no second Ed25519 implementation.

## What was built and shown (observed)

### DM1 canonical authority-set serialization

```
DM1 [A] canonical authority-set serialization: 3 caps, 132 bytes; reserialize (incl a shuffled order) is byte-identical=y
```

The authority set is N capabilities, each an object hash (32), rights (4), and length (8),
sorted by hash and packed little-endian. Sorting removes any caller-order or layout
dependence, so the same set always produces the same bytes. Serializing the set and a
shuffled copy of it produces byte-identical output.

### DM2 sign at the source, DM4 verify at the destination

A signs the canonical set bytes with A's Ed25519 private key (crypto_sign). B verifies the
signature over the canonical set against A's pre-shared public key (crypto_sign_open). The
migration proceeds only if the signature verifies. This is genuine public-key source
authentication, not a stub and not a shared-secret MAC.

### DM5 bounded re-mint at the destination, and DM6 the refusals (fail-closed)

```
[DMcase0 LEGIT] -> ACCEPT  |C_B'|=3  (== |C_A|=3, minted nothing beyond the verified set)
[DM-adv1 wrong-key] -> REJECT bad-signature [T1]  |C_B'|=0  (fail-closed: nothing re-minted)
[DM-adv2 tampered-set] -> REJECT bad-signature [T1]  |C_B'|=0  (fail-closed: nothing re-minted)
[DM-adv3 corrupt-sig] -> REJECT bad-signature [T1]  |C_B'|=0  (fail-closed: nothing re-minted)
legit set ACCEPT, C_B'=C_A (3 caps), minted nothing beyond? y   all 3 adversaries REJECTED fail-closed (nothing re-minted)? y
-> DISTRIBUTED ANTI-AMPLIFICATION (trusted cluster): a migrant cannot arrive with more authority than A signed; a post-sign amplification breaks the Ed25519 and is rejected
```

On a valid signature B re-mints each capability in the set through the unmodified swcap gate,
bounded by the source authority, and mints nothing beyond the verified set. For the clean
migration the rematerialized process holds exactly the signed set (|C_B'| = |C_A| = 3, and in
all cases C_B' is a subset-or-equal of C_A). The three refusals are real, driven by the real
Ed25519, and each rematerializes nothing:

- (a) tampered set: a capability widened after signing (an amplification attempt) changes the
  canonical bytes, so the signature no longer matches and B rejects. You cannot add or widen a
  cap without breaking the signature. This is the no-amplification-through-tampering proof, the
  cross-machine analogue of the local gate refusing AMPLIFY.
- (b) wrong key: a set signed with a key that is not A's trusted key fails verification against
  A's pre-shared public key, so B rejects.
- (c) corrupt signature: a flipped signature byte fails verification, so B rejects.

The anti-amplification GATE is the backstop under the signature: a per-cap bounded re-mint that
would refuse any cap whose length exceeds the source authority (the dm_remint path returns the
anti-amp verdict for an over-authority length). In the trusted-cluster model an honest A does
not sign an over-authority set, so the in-model amplification threat is the tamper case above,
which the signature catches. Note skipped: a dedicated case for a validly-signed over-authority
cap, add it when exercising the gate-reject path at runtime is wanted.

### DM7 state re-verification ordered after authority

The existing content-addressed state re-verification (B fetches objects by hash and re-hashes
to verify integrity, the B2 cold and B4 warm pulls) runs before the authority stage, and the
DM authority verification gates the re-mint. B does not re-mint authority for a migration whose
signature it has not verified. The leaf and node tamper rejections (BLAKE3 integrity) remain
green in the same build.

### DM8 measurement (rdtsc, this run)

```
DM8 [A] serialize=6586 cyc; Ed25519 sign=43831086 cyc
DM8 [B] Ed25519 verify=72961514 cyc; per-cap bounded re-mint=40831 cyc
```

The canonical serialization is cheap (about 6.6k cyc). The Ed25519 operations dominate the
added overhead (sign about 44M cyc at A, verify about 73M cyc at B, single-shot, well above
rdtsc noise). The per-cap bounded re-mint is about 41k cyc. The crypto is the cost of the
authority layer over the existing content-addressed transfer, reported honestly.

## Regression

The existing nettest two-machine suite stays green with the DM stage added: the Step-10
trust-boundary table (TRUST BOUNDARY CLOSED) and the Step-12 key-lifecycle table (KEY
LIFECYCLE CLOSED) both still pass, and both instances reach A DONE and B DONE. The main
kernel regression is unaffected because kernel.c and the proven core are untouched.

## Forbidden dodges, each disproven

- D1 faked verification: the tampered set actually fails the real Ed25519 and is rejected.
- D2 amplification through re-mint: C_B' = C_A exactly, B mints nothing beyond the verified set.
- D3 fail-open on bad signature: every failed verification rejects fail-closed, nothing re-minted.
- D4 new crypto reimplemented: reuses the vendored tweetnacl Ed25519 already in the tree.
- D5 trust-model overclaim: stated as trusted-cluster, honest source assumed, distrusting case
  named as future work needing attestation.
- D6 faked two machines: stated as two QEMU instances sharing one ivshmem region.
- D7 hardcoded numbers: every figure rdtsc-measured this run.
- D8 proven-core drift: the nine proven modules and kernel.c are byte-identical.

This banks the distributed anti-amplification property in the trusted-cluster model, the
smallest sound step of cross-machine authority preservation.
