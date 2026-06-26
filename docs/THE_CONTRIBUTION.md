# The contribution

This document states the novelty as carefully as for a reviewer, including what would falsify it
and how it differs from the nearest prior work.

## The claim

Diff-proportional migration of a live computation and monotonic capability re-mint at the
destination are consequences of executing live state inside a content-addressed immutable memory
model, enforced together on each migration.

Stated operationally. When a computation's state is a graph of content-addressed immutable
objects, migrating it to a destination that already holds part of that graph moves only the
objects the destination lacks, and the authority granted at the destination is minted fresh under
a gate that cannot exceed the source authority. The first property is the structural diff. The
second is the anti-amplification re-mint. The synthesis is enforcing both during a real migration
on real machines.

## Why each property follows from the model

Diff-proportional transfer. In a content-addressed store an object's name is the hash of its
content. A destination that holds an object holds its name. A migration therefore sends an object
only when the destination does not already know its name. Unchanged sub-objects keep their names
across an epoch, so they are never resent. The bytes on the wire track the change, not the total
state. This is a property of the naming scheme, not an added deduplication step.

Monotonic re-mint. A migration never carries a live authority token. It carries a stripped
descriptor and content-addressed state. The destination mints a fresh local capability through a
gate that checks the requested bounds and permissions against the source region and rejects any
request that would widen bounds or add permissions. The destination authority is therefore bounded
by the source authority by construction.

## What would falsify it

- A migration whose warm-hop bytes do not track the change set. If a small change forced a transfer
  proportional to total state, the diff-proportional claim is false. The measurement in
  docs/REPRODUCE.md tests this with three hops of different change sizes.
- A re-mint that produces a capability exceeding the source authority. If any bounds-widening or
  permission-adding request were accepted, the anti-amplification claim is false. The Coq proof
  rules this out for the abstract model, and the kernel test runs six attack classes against the
  software backend.
- A signature-gated migration that installs unsigned, mis-scoped, or replayed state. The
  adversarial table in docs/REPRODUCE.md tests this.

## Prior art and the precise difference

Content-addressed virtual machine migration exists. Systems that snapshot and deduplicate memory
pages by content hash have been built and measured. Capability systems exist, both in hardware and
in software-fault-isolation form. CARMIX does not claim either of these alone.

The difference is the fusion. Prior content-addressed migration does not re-mint capabilities under
an authority-monotonic gate as part of the transfer. Prior capability systems do not migrate live
state diff-proportionally by content address. CARMIX enforces both in one path, and ties the
authority bound to a machine-checked proof.

The nearest active project in the space content-addresses program binaries, so that a function is
named by the hash of its code and shared across a cluster. CARMIX content-addresses live
computation state, the running memory graph, and re-mints the capabilities that guard it during
the migration. The unit of content addressing is different, state rather than code, and the
capability re-mint is not present in that project.

## Scope of the claim

The claim is about the migration primitive and its authority guarantee, demonstrated on emulated
machines. It is not a claim about raw compute speed, about cold-start migration, or about being a
complete operating system. The measured advantage is repeated migration of a large state with a
small change, where the transfer tracks the change. See docs/ROADMAP.md for what remains open.
