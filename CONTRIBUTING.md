# Contributing

## Build and verify before a pull request

- Build and boot the kernel and confirm the self-tests pass. See docs/BUILD.md and
  docs/REPRODUCE.md.
- Run the two-machine migration and confirm the byte table and the authorization table. See
  docs/REPRODUCE.md.
- Re-verify the proofs with `cd proofs && make && make check`. The check must print `coqchk OK`.

A change must not break the proven core. The capability model, the content-addressed store, the
single-level store, the WebAssembly gate, and the software capability backend are verified. If a
change requires modifying one of them, say so in the pull request and re-run the full suite.

## Two rules for contributions

These keep the code and the documentation honest.

1. No degraded modes. The system commits to the real mechanism or it fails closed. There is no
   path that silently substitutes a weaker guarantee for the intended one. For example, signature
   verification either passes or the migration is rejected. It does not accept the state under a
   weaker check. A guard that refuses to run when a precondition is absent, such as skipping a
   draw when no framebuffer is present, is correct and is not a degraded mode. A construct that
   silently weakens a guarantee is not allowed.

2. Plain technical writing. Documentation and comment sentences use plain ASCII punctuation. No
   em-dashes or en-dashes as punctuation. No semicolons in prose. No colon as a dramatic break, a
   colon introduces a list, a code block, or a key-value pair only. No emoji. Write like an
   engineer documenting the system.

## Contributor agreement

The repository owner's choice of license and contributor agreement is recorded in
docs/LICENSE_CHOICE.md. Follow the agreement noted there when it is in place.
