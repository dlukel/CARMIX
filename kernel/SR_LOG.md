# PART SR: THE SYSTEM ROOT (SR1-SR7)

One content address that folds the WHOLE running system through the COMMITTED
epoch tree at a quiescent epoch boundary. SR banks a **composition** of committed
primitives; it upgrades **no** proof claim. Every number below is rdtsc / counter
measured on the real serial console this run (QEMU q35, tcg, 2 vCPU).

## What the System Root folds (SR1)

Eight component leaves, one 32-byte content hash each, folded through
`store/epoch_tree.c` (the committed diff-proportional Merkle epoch tree, whose
VERIFIED invariant is "incremental root == full recompute"):

| leaf | component | source primitive |
|------|-----------|------------------|
| taskA | process A page-table-INCLUSIVE task hash | `ts_canonicalize` (placement-independent) folded with regs+stack+ceiling |
| taskB | process B page-table-inclusive task hash | same |
| capver | capability slot versions | the versioned gate (`vgate_slot_t.version`) |
| storeman | content-store manifest | BLAKE3 over the store index (every distinct object's hash) |
| fsroot | content-addressed FS root | `fs_dir_put` Merkle name->hash tree |
| ceilA / ceilB | per-component authority ceilings | re-mintable ceiling descriptor (ceilH \|\| len \|\| perms) |
| sysceil | global authority ceiling | the root custodian over the 256B authority object |

Observed root this run:
`70c98e311ece3477ab578228dae686d505a4be16c78ace972d741a18777774f1`
(first fold = 8 leaf-hashes + 7 node-hashes).

- Consecutive QUIESCENT epoch, nothing changed -> **identical root** (re-hash work
  = 0 leaves + 0 nodes; the epoch tree touches nothing when no chunk is dirty).
- One component changed (cap slot version bump) -> **changed root**
  (`7af9e515...`), then restoring that leaf returns to the pinned root (time-travel
  of a single component). **SR1 OK.**

## SR2 diff-proportional fold (the D1 refusal)

- Incremental fold of ONE changed leaf: re-hashed **1 leaf + 3 nodes = 4 of 15**
  total tree nodes.
- Full recompute (all leaves dirty): re-hashed **8 leaf + 7 nodes = 15 nodes**.
- **Shared-subtree reuse = 11 nodes NOT re-hashed** (unchanged subtrees kept their
  prior hashes).
- **incremental-root == full-root = y.** The fold is provably NOT a secret full
  recompute (had it been, SR2 would report FAILED per D1). The node-hash counts are
  the epoch tree's own `last_leaf_hashes` / `last_node_hashes`, so the reuse is
  counted, not asserted.
- rdtsc this run: full 404467 cyc vs incremental 706148 cyc (single-shot; at 15
  nodes the raw cycle count is first-touch/cold-cache dominated, so the load-bearing
  diff-proportionality evidence is the node-hash COUNT 4 vs 15, not the cycle delta).
  **SR2 OK.**

## SR3 whole-system time-travel + branch (the D2 refusal)

Pinned root N, process A task hash `ce9f55c421f16bf3349c4641...`.

1. Dematerialize A: content-address registers + heap page + **stack page**, free
   the 2 private frames (backing frames freed = 2; registers+heap no longer
   resident). Stack is included, unlike `conc_dematerialize`, so the
   placement-independent task hash round-trips exactly.
2. Mutate the system to root N+k (`c89db051...`, differs from N).
3. Rematerialize A into FRESH frames (BLAKE3-verified): **restored task hash ==
   pinned = y**, registers match (rip `0x4000000e`), heap match (counter = 3),
   root N re-derived = y. This is NOT files-only: the actual register + heap +
   stack state is rematerialized (D2 defeated).
4. BRANCH: continued A from the restored root to counter = 5 -> line N' root
   `e81d405d...`, diverges from N; BOTH roots retained and BOTH task states
   rematerializable from the store. **SR3 OK.**

## SR4 revocation vs time-travel: THE DECISION

**CHOSEN: (b) a REVOCATION FLOOR at rematerialization.** A restored capability is
intersected with the CURRENT revocation state; the pinned root remains immutable
HISTORY (state is faithfully restored), but AUTHORITY is re-minted through today's
revocation state.

**Reason:** revocation is a forward-only, safety-critical security action. If a
replayed root could resurrect revoked authority, an attacker who can trigger a
restore bypasses every revocation. That is unacceptable for a capability OS whose
entire thesis is bounded authority.

**REJECTED: (a) immutable-authority restore** (restoring a root restores its
authority). Rejected because it makes revocation REVERSIBLE by replay, a security
regression that voids the anti-amplification guarantee the rest of the system
enforces.

Real revoked-capability restore attempt this run:

- Revoke A's ceiling AFTER pinning root N. Restore A's authority from the pinned
  ceiling descriptor: the anti-amp gate alone returns status 0 (it WOULD grant, the
  pinned authority is legitimate), but `revoked = y` -> **authority GRANTED = n**
  (floored to nothing). A's STATE still rematerialized in SR3 (state faithful,
  authority denied).
- Contrast B (NOT revoked): status 0, `revoked = n` -> **authority GRANTED = y**,
  never widened (len 128 <= 128). **SR4 OK.**

## SR5 whole-system attestation (the D3 claim)

Signed the System Root with the COMMITTED Ed25519 path (tweetnacl
`crypto_sign`/`crypto_sign_open`, keys from `authz_keys.h`). A verifier re-derives
the root by re-folding the manifest and checks ONE signature to learn what code
runs (the task-hash leaves) AND every component's authority ceiling (the ceiling
leaves): one signature, both facts. This ceiling binding is the CARMIX-specific
claim (D3).

- sign+msg = 96 bytes; verifier re-derives root `70c98e31...` + one signature ->
  valid = y.
- **Refusal 1 (tamper):** flip one byte of A's ceiling post-sign -> re-derived root
  `b556250e...` != signed root `70c98e31...` -> **verification FAILS = y**.
- **Refusal 2 (unknown signer):** sign with WRONG_SK, verifier trusts only SRC_PK
  -> `crypto_sign_open` returns nonzero -> **REFUSED = y**. **SR5 OK.**

rdtsc this run: sign = 37121891 cyc, verify = 49638092 cyc (Ed25519 is tens of
Mcyc, well above rdtsc noise; portable no-SIMD path).

**Honest boundary (no overclaim):** this is a signature over a SELF-REPORTED
manifest on an EMULATED platform. It is NOT hardware-rooted attestation (no TPM, no
measured boot); it attests what THIS kernel reports, not an independent hardware
measurement.

## SR6 quiescence is real (the D5 refusal)

The epoch boundary quiesces mutation via the versioned gate. The fold captures the
ACTIVE (version-selected) capability buffer at version V; a racing mutation writes
the INACTIVE buffer and commits with a single atomic version bump.

- Fold at version V = 2: captures = 5 (of which 4 mid-inactive-write, the ones that
  would tear a naive single-buffer slot), TORN captures = **0** (every capture read
  a complete capability).
- The racing mutation committed at version 3 (V+1): **EXCLUDED from epoch V,
  DEFERRED into epoch V+1** (its authority differs). The epoch-V root never folded a
  torn capability. **SR6 OK.**

## SR7 measure (rdtsc this run)

| quantity | value |
|----------|-------|
| fold, full recompute | 404467 cyc (15 node-hashes) |
| fold, incremental | 706148 cyc (4 node-hashes) |
| restore, dematerialize | 1273533 cyc |
| restore, rematerialize (regs+heap+task-hash) | 2508169 cyc |
| branch (run + re-fold) | 76109473 cyc |
| attestation, Ed25519 sign | 37121891 cyc |
| attestation, verify (open) | 49638092 cyc |
| checkpoint bytes, incremental | 128 B (4 changed nodes) |
| checkpoint bytes, full | 480 B (15 nodes) |
| checkpoint bytes, shared/reused | 352 B not re-stored |

(Numbers vary run to run; they are rdtsc-measured this run, never imported.)

## Reuse (READ first, not reimplemented)

- Epoch tree: `store/epoch_tree.c` (`cvsasx_epoch_init/write/commit`), pulled into
  the kernel translation unit by textual `#include` so kernel.o carries it with no
  `build.sh` change (its one external, `cvsasx_blake3_tagged`, is already linked
  from `blake3_wrap.c`). Chunk size overridden to 32B so 1 component hash = 1 leaf.
- Versioned gate: `vgate_slot_t`, `vgate_commit`, `vg_capture` (unchanged).
- Content store: `cvsasx_store_*`. FS root: `fs_dir_put`.
- Canonical page-table hash: `ts_canonicalize` (placement-independent, page-table
  inclusive).
- Process demat/remat: `sr_demat`/`sr_remat` reuse the committed store + frame
  reserve/free + BLAKE3-verify path (`dematerialize_frame`/`rm_materialize`
  precedent), extended to include the stack page so the task hash round-trips.
- Anti-amp gate: `conc_remint` / `cvsasx_sw_cap_remint`.
- Ed25519: the COMMITTED tweetnacl `crypto_sign`/`crypto_sign_open`. Keypair
  generation is host-only (commented out), so there is no randombytes/libc
  dependency; the baked TEST keypair lives in `authz_keys.h`.

Nothing under `carmix/ gate/ sls/ store/ cap/ proofs/` was edited (byte-identical).

## Scope (stated plainly, not upgraded)

Single machine, QEMU, QUIESCENT-epoch capture only. Concurrent capture is out of
scope. The System Root banks a composition of committed primitives; it does not
upgrade any proof claim. SR5 attestation is an emulated-platform signature over a
self-reported manifest, NOT hardware-rooted attestation.

## Regression

Full regression green except the accepted PM0 stall. All prior stages (B0-B5,
STEP4/5, S2-S6, A7-A9, E1-E4, run_sched..run_cv, CV1-CV8, TS0-TS5, VG1-VG5,
DBG1-DBG6, FS/CFS, SM0-SM5, INT1-INT5) still pass on the same boot; the one F2
`materialize ASSERT` line is the intentional fail-closed negative test that
resolves to `F2 OK`.
