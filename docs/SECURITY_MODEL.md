# Security model

This document states the threat model for the two-machine migration, what each mechanism
guarantees, and what is not yet provided.

## Threat model

A destination machine pulls content-addressed state from a source and rematerializes it. The
threats are the following.

- Tamper. A block is corrupted in transit.
- Impersonation. A party that is not the legitimate source presents an object graph and induces
  the destination to rematerialize it. The graph's public hashes can be replayed by anyone.
- Replay. An old, legitimately signed migration is presented again later to force the destination
  back to a stale epoch.
- Scope escalation. A source authorized for one computation, one destination, or one authority
  ceiling presents state for a different computation, a different destination, or a higher ceiling.

## What each mechanism guarantees

Hash verification guards integrity. Each block's identity is the BLAKE3 hash of its content. The
destination recomputes the hash of every received block and rejects any block whose hash does not
match the requested identity. A tampered or forged block hashes to a different identity, so it
cannot be installed. This closes tamper. It does not, by itself, prove who served the graph.

The signed authorization record guards provenance and scope. The record binds the migration to a
specific epoch root, computation, destination, authority ceiling, nonce, and expiry, and the
source signs it with Ed25519. Before the destination installs and re-mints, it checks, each
check failing closed with a distinct reason:

1. The signature is valid under the expected source public key. This closes impersonation.
2. The signed epoch root equals the root the destination actually pulled. This binds the
   authorization to exactly the state being installed.
3. The destination and computation identifiers match this machine and this computation. This
   closes wrong-destination and wrong-computation escalation.
4. The nonce is unseen and the record is not expired. This closes replay.
5. The re-mint is capped at the authority ceiling and runs through the anti-amplification gate.
   This closes authority escalation, and ties to the anti_amplification proof.

The cryptography is real Ed25519, vendored tweetnacl. It is public-key source authentication, not
a shared-secret scheme.

## What is not provided

The demo uses baked test keys. The source public key is hard-coded in the destination. The
following are not built, and a real deployment requires them.

- Key distribution and a public-key infrastructure. There is no mechanism for a destination to
  learn the legitimate source key, to rotate it, or to revoke it. The demo proves the protocol
  and the binding logic under a known key, not how trust roots are managed.
- Persistent nonce state. The replay defense relies on remembering used nonces. The demo holds
  this in memory. A real system must survive a restart without forgetting, or an attacker replays
  across a reboot.
- A trustworthy clock. The expiry check needs a monotonic, trustworthy time source. The kernel
  has a tick counter, not a verified clock.

These are listed in docs/ROADMAP.md.
