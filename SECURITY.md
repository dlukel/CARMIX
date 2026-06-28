# Security policy

CARMIX is a research operating system prototype verified in an emulator. It is not a
general-purpose operating system and is not hardened for production use. This document states how
to report a vulnerability, what the enforced security property is, the known security-relevant
limits, and what is in scope.

## Reporting a vulnerability

Report security issues privately. Do not open a public issue for an unpatched vulnerability.

- Contact: <SECURITY CONTACT, fill in, for example security@example.org>

Include the affected file and the self-test or reproduction command, the observed behavior, and
the behavior you expected. If a self-test is involved, name the stage code (for example E2, US3,
or the migration table in kernel/net_repro.sh) and quote the serial line.

The repository owner reviews reports and responds as warranted. There is no bug bounty.

## The enforced authority property

The one authority property the system enforces is anti-amplification monotonicity at the
capability re-mint gate. A re-minted capability's bounds are no wider and its permissions no
greater than the source held. Over-authority is rejected fail-closed, not clamped silently. The
same gate bounds a cross-machine migration, a page fault, a user and kernel crossing, and a
process load, so these are one authority model.

This property is machine-checked in Coq in proofs/Carmix.v. coqc and coqchk both accept it, and
it is axiom-free except one stated collision-resistance hypothesis. The runtime correspondence to
the C implementation is demonstrated by the kernel attack self-tests (E2, US3, L3, C1), not yet
proven by a refinement. See docs/PROOFS.md and docs/SECURITY_MODEL.md.

## Known security-relevant limits

These are stated plainly so the proven parts stay credible. They are tracked in docs/ROADMAP.md
and docs/SECURITY_MODEL.md.

- Cross-process dedup side channel. Two processes that write independently identical bytes
  deduplicate to one physical frame by hash. The timing of that deduplication is observable, so it
  is a side channel that can leak whether another process holds given content. This is a known
  hazard. It is named, not solved. No dedup-scan mitigation is built.
- Nested-fault gap (F5). The page-fault handler's own code, stack, and binding metadata are
  resident, so servicing a miss does not itself fault. A fault during fault handling fails closed
  rather than recovering. Making nested faults recoverable needs a separate interrupt stack and a
  per-fault service context (IST and TSS), which is not built.
- Trust-root bootstrap. The destination's first authority public key is baked in. A certificate
  authority, identity proofing, or an out-of-band channel to obtain and prove that first key is
  the one irreducible bootstrap assumption, and it is not built. The key lifecycle, enrollment,
  authority-signed rotation, revocation, and monotonic epochs against replay and downgrade, is
  built and tested, but it is proven only given a trustworthy authority key, not given a way to
  establish that key.
- No persistent trust or nonce state. The trust store, the lifecycle epoch, the revoked set, and
  the seen nonces live in memory. A restart re-bootstraps from the baked authority key at epoch
  zero. A real system must persist this state without rollback.
- No trustworthy clock. The expiry check uses a tick counter, not a monotonic verified time
  source.
- Single authority, small revocation set. There is one authority key whose compromise is
  unrecoverable, and the revoked set is a small fixed in-memory list with no propagation.
- Cryptographic implementations are vendored, not formally verified. BLAKE3 is vendored and
  validated against the official known-answer vectors, which the kernel re-runs at startup before
  it trusts any content-addressed result. Ed25519 is vendored tweetnacl. See
  kernel/TWEETNACL_PROVENANCE.txt. Neither is a formally verified implementation.

## Scope

In scope:

- A way to amplify authority across the re-mint gate (a re-minted capability wider or more
  permissive than the source) in the kernel, the software capability backend, or the cross-machine
  migration.
- A way to install content that does not match its requested BLAKE3 hash (a hash-verification
  bypass on the migration, fault, syscall, or load path).
- A signed-authorization or key-lifecycle bypass (accept an unauthorized migration, accept a
  revoked or unknown key, replay or downgrade).
- A WebAssembly guest escaping its linear memory or forging a capability past the gate checker.
- A flaw in the Coq model or its statement of the property it claims to prove.

Out of scope:

- The known limits listed above. They are documented gaps, not vulnerabilities to report.
- Anything requiring real hardware, USB input, persistent storage, a filesystem, GPU drivers, or a
  real network transport. None of these are built. See docs/ROADMAP.md.
- The desktop compositor and input handling beyond the authority boundary, since the desktop draws
  only CARMIX's own windows.
- Denial of service from a malformed input that fails closed without violating an authority
  property.
