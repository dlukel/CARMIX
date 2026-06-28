# Proofs

The proofs/ directory holds a Coq development that machine-checks the core authority guarantee.
The development is axiom-free except for one stated collision-resistance hypothesis used only by
the acyclicity result. docs/THEORY.md states the authority property and the three-layer trust model
in precise prose. This document is the re-verification reference for it.

## What is proven

The source is proofs/Carmix.v. It models a capability as a record of a base, a limit, a finite
permission set, and a validity flag, with a forbidden permission subset. It defines an authority
order, where one capability is below another when its validity implies the other's, its range sits
inside the other's range, and its permissions are a subset. It defines a re-mint function that
clamps the requested range to the source, intersects the requested permissions with the source,
rejects a request that asks for write and execute together or for any forbidden permission, and
sets validity accordingly.

Three results are checked.

- anti_amplification. For all sources and all requests, if the re-minted capability is valid then
  it is below the source in the authority order. This is the anti-amplification guarantee. The
  destination authority never exceeds the source authority.
- valid_dest_no_wx and valid_dest_no_forbidden. A valid re-minted capability does not hold write
  and execute together, and holds no forbidden permission.
- acyclic_graph. The content-addressed object graph is acyclic.

## How the acyclicity result is scoped

State this result honestly. acyclic_graph does not derive acyclicity from hashing alone. Objects
are modeled as a finite inductive tree, a node carrying a payload and a list of child objects. In
that model a child is literally a strict subterm of its parent, so a structural size measure
strictly decreases along the child relation, and acyclicity follows from that well-founded order.

The collision-resistance hypothesis, that the identity function is injective, is what connects the
hash-graph back to that finite tree structure. It is the single stated assumption. The size
measure that orders the tree is a proven lemma, not an assumption.

So the result reads as follows. Under collision resistance, the content-addressed object graph is
acyclic, because content addressing makes the graph faithful to a finite tree, and a finite tree
is well-founded. The result does not claim that hashing by itself, without the finite-structure
model, forces acyclicity.

## How to re-verify

Install Coq (Rocq) 8.20.1 so that coqc and coqchk are on PATH. Then:

```
cd proofs
make          # compiles Carmix.v, fails if any theorem does not close with Qed
make check    # runs coqc, then coqchk re-verifies the compiled object independently
make audit    # prints the assumptions of each headline theorem
```

A successful run prints `coqchk OK` from `make check`. From `make audit`, anti_amplification,
valid_dest_no_wx, and valid_dest_no_forbidden print `Closed under the global context`, meaning the
kernel found zero axioms and zero admitted goals. acyclic_graph prints its single
collision-resistance hypothesis as its only assumption.

The development contains no `Admitted`, no `admit`, and no `Axiom`. The only `Hypothesis` is the
collision-resistance assumption named above.

## What the proof does not cover

The proof is about the abstract capability algebra. It is not a proof that the C implementation in
carmix/swcap.c refines this model. The runtime correspondence is demonstrated by the kernel test
that runs six attack classes against the software backend and observes each rejected by its
intended check. A machine-checked refinement of the C code against this model is future work,
listed in docs/ROADMAP.md.
