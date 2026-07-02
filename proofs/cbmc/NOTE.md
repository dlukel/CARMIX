# P3 - CBMC bounded decision of the real swcap gate

## What this is (and is not)

A COMPLETE SAT/SMT decision, WITHIN THE STATED BOUNDS, of the bounds +
permission (anti-amplification) property of the REAL committed enforcement gate
`cvsasx_swcap_check` in `carmix/swcap.c`.

It is NOT a Coq/Qed machine-checked proof (see `proofs/Carmix.v` for that
lineage), and it is NOT an seL4-scale functional-correctness refinement of the
whole system. It is a bit-precise, bounded decision over one function's real C.

`carmix/swcap.c` is byte-identical: the harness only `#include`s it (relative
path `../../carmix/swcap.c`), it is never retyped and never edited.

## Files

- `swcap_check_harness.c`  - the harness (nondet capability fields + nondet
  requested access; calls the real gate; asserts `grant == spec`).
- `swcap_cbmc_output.txt`  - full verbatim CBMC output, recorded this session.

## Pinned tool

cbmc 6.6.0 (cbmc-6.6.0), 64-bit x86_64 linux.

## Property

The gate grants IFF the access is valid, the requested permission bits are a
subset of the capability's (no permission amplification), and the requested
window `[offset, offset+size)` lies within `[0, length)` (i.e. within
`[base, base+length)` in absolute terms; the gate is base-relative). Asserting
the IFF pins both directions: grant => safe (the safety property) and
safe => grant (exactness, so the pass is not vacuous).

## Bounds / unwind justification (D4)

- `--drop-unused-functions`: only `main` + `cvsasx_swcap_check` remain, so every
  reported property is REACHABLE - no vacuous SUCCESS from uncalled functions.
- Unwind bounds: NONE. The reachable code has no loops and no recursion, so no
  unwind bound is needed and the decision is COMPLETE/EXHAUSTIVE over the whole
  nondet input domain (all 2^64 offset, 2^64 size, 2^32 need_perms, 2^64 base,
  2^64 length, 2^8 valid). Nothing is excluded.
- `--unsigned-overflow-check`: substantiates that `length - offset` never
  underflows (the source's overflow-safe comment).
- `--pointer-check --bounds-check`: memory safety of the `&c` dereferences.

## Result

`VERIFICATION SUCCESSFUL` - 0 of 27 properties failed; the core property
`[main.assertion.1] assertion grant == spec: SUCCESS`.
