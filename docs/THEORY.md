# Theory

This document states the authority model precisely. It defines the three-layer trust model, states
the anti-amplification property and the exact theorems machine-checked in proofs/Carmix.v, and is
explicit about what the proof does not cover. docs/SECURITY_MODEL.md gives the threat model in
operational terms, and docs/PROOFS.md gives the re-verification commands. This document is the
formal statement those two refer to.

## The three-layer trust model

A rematerialization is admitted only when three independent questions are answered. They are
separated on purpose, because each fails closed for a different reason and a confusion of one for
another would be a hole.

### Layer 1, identity by content hash

An object's identity is the BLAKE3 hash of its content. For a node the hashed content includes the
identities of its children, so the identity of any object commits to its entire subtree. The
destination recomputes the hash of every received block and installs it only if the hash equals the
identity that was requested. A block that was tampered with or forged hashes to a different
identity and cannot be installed. Identity answers what the content is. It does not answer who
served it.

### Layer 2, provenance by signature over the hash

The source signs an authorization record with Ed25519. The record binds the epoch root hash, a
computation identifier, a destination identifier, an authority ceiling, a nonce, and an expiry. The
signature is over the hash, so it commits to exactly the state being installed, because the root
hash commits to the whole graph by Layer 1. The destination verifies the signature under the
trusted source key, resolved through a trust store that rejects an unknown or a revoked key by
distinct reasons, and checks that the signed root equals the root it actually pulled, that the
destination and computation identifiers match, and that the nonce is unseen and the record is not
expired. Provenance answers who authorized this rematerialization. The cryptography is real
public-key authentication, not a shared secret. The one assumption left open is the bootstrap of
the first authority key, stated in docs/SECURITY_MODEL.md.

### Layer 3, authority by the re-minted ceiling

The destination never receives a live authority token. It receives a stripped descriptor and the
content-addressed state, and mints a fresh local capability through the anti-amplification gate,
capped at the signed authority ceiling. The gate refuses any request that would widen bounds or add
permissions. Authority answers how much the rematerialized computation may do, and the answer is
bounded by what the source held. This layer is the one the Coq proof is about.

## The anti-amplification property, in prose

A capability is a half-open range, a base and a limit, plus a finite set of permissions, plus a
validity tag. One capability is below another in the authority order when three conditions hold
together. Its validity implies the other's. Its range sits inside the other's range, its base no
lower and its limit no higher. Its permissions are a subset of the other's permissions.

Re-mint takes a source capability, a requested range, and a requested permission set. It clamps the
requested range to the source range, the low end raised to the source base and the high end lowered
to the source limit. It intersects the requested permissions with the source permissions. It
rejects, by setting the result invalid, any request whose surviving permission set holds write and
execute together or holds any permission from a forbidden set. It sets the result valid only when
the source was valid and the request was not rejected. This mirrors the C in carmix/swcap.c,
function cvsasx_sw_cap_remint, the invalid-source rejection, the bounds clamp, the forbidden-mask
rejection, the write-execute rejection, and the permission subset.

The anti-amplification property is that the result of re-mint, whenever it is valid, is below the
source in the authority order. The authority granted at the destination never exceeds the authority
the source held. Over-authority is refused, not silently clamped to a weaker grant.

## What Coq proves, exactly

The source is proofs/Carmix.v, checked with Coq (Rocq) 8.20.1. The development contains no Axiom,
no Admitted, and no admit. The only Hypothesis is the collision-resistance assumption named below,
used only by the acyclicity result. Three headline results are checked.

- anti_amplification. For all sources, all requested ranges, and all requested permission sets, if
  the re-minted capability is valid then it is below the source in the authority order. This is the
  formal anti-amplification guarantee.
- valid_dest_no_wx and valid_dest_no_forbidden. A valid re-minted capability does not hold write
  and execute together, and holds no forbidden permission. The forbidden set modeled is the four
  object-capability permissions a re-mint of an ordinary memory capability must never confer, seal,
  unseal, invoke, and access to system registers.
- acyclic_graph. The content-addressed object graph is acyclic, no object reaches itself along the
  child relation.

## How the acyclicity result is scoped

acyclic_graph does not derive acyclicity from hashing alone. Objects are modeled as a finite
inductive tree, a node carrying a payload and a list of child objects, so a child is literally a
strict subterm of its parent. A structural size measure therefore strictly decreases along the
child relation, and acyclicity follows from that well-founded order. The size measure is a proven
lemma, children_smaller, not an assumption.

The single stated assumption is collision resistance, written in the proof as Identity_injective,
that the identity function is injective on objects. It is what connects the hash-graph edge, v is a
child of u when v's identity appears among u's children's identities, back to the structural child
relation of the finite tree. Under collision resistance the id-graph is faithful to the finite
tree, and a finite tree is well-founded, so the graph is acyclic. The result does not claim that
hashing by itself, without the finite-structure model, forces acyclicity. A direct corollary,
no_id_fixpoint, states the fixed-point impossibility, no object's identity can equal the identity of
one of its own proper descendants.

## What the proof does not cover

The proof is about the abstract capability algebra. It is not a machine-checked refinement of the C
implementation against this model. The runtime correspondence, that carmix/swcap.c actually
behaves as the abstract re_mint, is demonstrated, not proven. The kernel runs six attack classes
against the software backend and observes each rejected by its intended check, the in-kernel
anti-amplification table, and the two-machine harness rejects seven adversaries each by a distinct
reason. Those runtime attack tables are the evidence for the C, and a machine-checked refinement is
named as future work in docs/ROADMAP.md.

The proof also says nothing about the cryptography or the key bootstrap. It is the authority
algebra of Layer 3 only. Layer 1 rests on BLAKE3 collision resistance, which the kernel checks
against the official known-answer vectors at startup but does not prove, and Layer 2 rests on
Ed25519 and on the bootstrap of the first authority key, which is the one irreducible assumption in
docs/SECURITY_MODEL.md.

## Re-verify

```
cd proofs
make          # compiles Carmix.v, fails if any theorem does not close with Qed
make check    # runs coqc, then coqchk re-verifies the compiled object independently
make audit    # prints the assumptions of each headline theorem
```

make check prints coqchk OK. make audit prints Closed under the global context for
anti_amplification, valid_dest_no_wx, and valid_dest_no_forbidden, meaning zero axioms and zero
admitted goals, and prints the single collision-resistance hypothesis for acyclic_graph. See
docs/PROOFS.md for the same commands with the expected output described line by line.
