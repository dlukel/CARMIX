# Security model

This document states the threat model for the two-machine migration, what each mechanism
guarantees, and what is not yet provided. docs/THEORY.md states the same three layers, identity,
provenance, and authority, as a formal model and gives the exact theorem Coq checks for the
authority layer.

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

The signing key itself is a target.

- Unknown key. A source signs a well-formed record under a key the destination was never told to
  trust.
- Rotation forgery. An attacker forges a key-rotation message the destination did not receive from
  the trusted authority.
- Rotation replay or downgrade. An old, once-valid rotation or an old key is replayed to force the
  destination back to a superseded key.
- Use after revoke. A migration is signed by a key that was valid but has since been revoked.
- Revocation forgery. A forged revocation of a still-good key, to deny service.

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

## The key lifecycle

The destination holds a trust store rather than a baked source key. The store has an authority
public key, the current source public key, a monotonic lifecycle epoch, and a permanent revoked
set. Three record types, each signed by the authority key and stamped with an epoch, drive it. An
enrollment sets the first source key. A rotation replaces it with a new key. A revocation adds a
key to the revoked set. A lifecycle record is applied only if its signature verifies under the
authority key and its epoch is strictly greater than the stored epoch, which makes rotation and
revocation monotonic and rejects replay and downgrade.

Before the five authorization checks above, the destination resolves the migration's signing key
through the trust store. The key must equal the current source key, else the migration is rejected
as an unknown key, and it must not be in the revoked set, else it is rejected as a revoked key.
These two reasons are distinct. A downgrade to an old key reads as unknown-key, while a migration
under a killed key reads as revoked-key. A forged rotation or revocation under a non-authority key
is rejected as bad-authority, and an old lifecycle record is rejected as a stale epoch.

## What is not provided

The keys in the demo are deterministic test keys. The lifecycle above provides enrollment,
authority-signed rotation, revocation, and monotonic epochs. The following are not built, and a
real deployment requires them.

- The root of trust. The destination's first authority public key is baked in. How a destination
  obtains and proves that first key, through a certificate authority, identity proofing, or an
  out-of-band channel, is the one irreducible bootstrap assumption. The lifecycle is designed and
  tested against attack tables given a trustworthy authority key, not how that key is established.
- Persistent trust and nonce state. The trust store and the used-nonce set live in memory. The
  kernel has no disk, so a restart re-bootstraps from the baked authority key at epoch zero. A real
  system must persist the current key, the epoch, the revoked set, and the seen nonces without
  rollback.
- A trustworthy clock. The expiry check needs a monotonic, trustworthy time source. The kernel has
  a tick counter, not a verified clock.
- Revocation scale and a single authority. The revoked set is a small fixed in-memory list with no
  propagation, and there is one authority key whose compromise is unrecoverable. Distribution and a
  multi-authority quorum are not built.

These are listed in docs/ROADMAP.md.
