---
name: Feature request / design RFC
about: Propose a new mechanism or a change to an existing one
title: "[rfc] "
labels: rfc
---

## Summary

One paragraph. What is being proposed and why it belongs in a research OS built around
content-addressed rematerialization under a re-mint gate.

## Motivation

What is missing today. If this is a known open item, link the entry in docs/ROADMAP.md. State the
problem before the solution.

## Design

How it would work. Name the files it touches. State the invariant it would hold and how that
invariant is checked (a self-test, a measured number, or a proof).

## Effect on the proven core

The proven core is cap/, store/, sls/, gate/, carmix/, and proofs/. State plainly whether this
proposal touches any of them. If it does, say which invariant could be affected and how the change
would be re-verified. A change that keeps the proven modules byte-identical is far easier to accept.

## What is measured vs claimed

Every new claim must be backed by observed evidence, a gated self-test predicate, an rdtsc-measured
number, or a machine-checked proof. State what evidence this feature would produce. No claim
without a number or a proof behind it.

## Honest limits

State what the proposal would not do, in the same register as docs/ROADMAP.md. A named limit is
expected, not a weakness.

## Alternatives

What else was considered, and why this approach.
