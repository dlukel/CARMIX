# MIGRATION_DISTRUST_LOG.md - distrusting cross-machine migration (attestation-gated, EMULATED root)

Honest record of Step 14 in nettest.c: the distrusting migration protocol, where B does NOT
trust A. B honors the migrant's authority only if an attestation quote verifies, the
content-addressed state re-verifies by re-hash, and the authority re-mint is bounded by the
anti-amplification gate.

All figures rdtsc-measured this run.

## The cardinal honesty point: the root is EMULATED

The attestation ROOT is EMULATED. The measurement register and the quoting key are SOFTWARE
(the quoting key is the existing AUTH keypair standing in for a hardware root). This
demonstrates the PROTOCOL and the VERIFICATION FLOW, NOT hardware attestation security. A
real distrusting adversary who controls A's machine could forge a software root, which is
exactly why real security needs a HARDWARE root of trust (TPM, SGX, SEV, TDX), and that needs
physical hardware CARMIX cannot boot on yet. Real hardware-anchored distrusting security is
FUTURE WORK gated on physical bring-up. This build banks the distrusting PROTOCOL with an
emulated root, not achieved hardware-anchored distrusting security. That boundary is stated in
the code, the boot output, and the commit message.

## The CARMIX-specific strength

Because the migrant's STATE is content-addressed, B independently re-verifies every byte of
state by re-hashing it (the existing cold and warm sync re-verification), needing zero trust
in A for the state. That collapses the distrusting problem to a single residue: the AUTHORITY.
Attestation only has to anchor the authority, which is the exact gap the research isolated.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical. Step 14 is
in kernel/nettest.c, reusing the Step 13 canonical authority set and bounded re-mint (dm_remint,
via the unmodified swcap gate), the existing Ed25519 (crypto_sign and crypto_sign_open), and the
content-addressed state re-verification. No new crypto.

## What was built and shown (observed)

The quote binds A's measured code identity, the canonical authority set, and the
content-addressed state root, signed with the emulated quoting key.

```
[AMcase0 LEGIT] -> ACCEPT  |C_B'|=3  (attested authority re-minted; state re-verified by re-hash, zero trust in A for state)
[AM-adv1 forged-quote] -> REJECT forged-quote [bad quoting sig]  |C_B'|=0
[AM-adv2 unrecognized-measurement] -> REJECT unrecognized-measurement [distrust: code identity not accepted]  |C_B'|=0
[AM-adv3 mismatched-binding] -> REJECT mismatched-binding [attested root != transferred root]  |C_B'|=0
legit quote ACCEPT (C_B'=3)? y   forged + unrecognized + mismatched all REJECTED fail-closed? y
unrecognized-measurement rejected (the distrusting property trusted-cluster LACKED)? y
```

- DA-B1/DA-B2: A produces a signed quote; B verifies the quoting signature under the trusted
  quoting key, fail-closed. A forged quote (wrong quoting key) is rejected.
- The distrusting heart: B rejects a quote from a code identity it does not accept
  (unrecognized measurement). This is the property the trusted-cluster model did not have. In
  the trusted cluster B trusted A's key outright. Here B accepts the migrant only if the
  measured identity is one it recognizes.
- DA-B3: B re-verifies the state independently by re-hash (the content-addressed cold and warm
  sync), needing no trust in A for the state. A quote whose attested state root does not match
  the transferred root is rejected (mismatched binding).
- DA-B4: on a verified quote B re-mints the authority bounded by the anti-amp gate, C_B' equals
  the attested set (3 caps), nothing more. Every rejection re-mints nothing.

### DA-B6 measurement (rdtsc, this run)

```
AM6 [A] measurement=159735 cyc; quote generation (Ed25519 sign)=76786583 cyc
AM6 [B] quote verification (Ed25519 open)=71287156 cyc
```

The Ed25519 quote generation and verification dominate the added overhead over trusted
migration, about 77M and 71M cyc, reported honestly. Attestation adds cost.

## Regression

The nettest two-machine suite stays green with Step 14 added: the trust-boundary table (Step
10), the key-lifecycle table (Step 12), the trusted authority-set table (Step 13), and now the
distrusting table (Step 14) all pass, and both instances reach A DONE and B DONE. The main
kernel regression is unaffected (kernel.c and the proven core are untouched).

## Forbidden dodges, each disproven

- D1 emulated-root presented as real: the emulated-root boundary is stated in code, log, and
  commit; no hardware-anchored distrusting security is claimed.
- D2 unrecognized measurement accepted: rejected (the distrusting heart).
- D3 forged quote accepted: rejected by the real Ed25519.
- D4 state trusted from A: state re-verified by re-hash (the CARMIX advantage); a mismatched
  attested root is rejected.
- D5 amplification through re-mint: C_B' equals the attested set, nothing more.
- D6 new crypto: reuses the existing Ed25519.
- D8 proven-core drift: the nine proven modules are byte-identical.

This banks the distrusting migration PROTOCOL with an emulated root. The protocol and the
verification flow, including the distrusting refusal of an unrecognized identity, are real.
Hardware-anchored distrusting security is future work on physical bring-up.
