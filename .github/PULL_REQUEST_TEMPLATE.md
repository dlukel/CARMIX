# Pull request

## What this changes

One paragraph describing the change and the files it touches.

## Verification checklist

Check each box only after you have run the command and observed the result. Evidence before
assertion.

### Build and link

- [ ] The kernel builds and links via `cd kernel && BOOT_SECS=180 bash build.sh`.
- [ ] The link audit prints three zeros: 0 undefined symbols, 0 libc/host symbols, 0 CHERI
      instructions.

### Self-tests stay green

- [ ] The boot streams the self-test stages and every terminal line is `OK`, `HOLDS`, or `PROVEN`.
      Nothing prints `FAIL`, there is no triple-fault, and the boot reaches the live E4 desktop
      shell. Stages: B (boot), E (in-kernel Wasm under the gate), R (in-kernel remat), S/A/E-10b
      (desktop), P (task substrate), U (unified substrate + crossover), M (residency), F (fault
      handler), US (user/kernel crossing), L (loader), C (concurrency), PP (policy), FA (fairness),
      PM (per-process heap).
- [ ] The two-machine migration is green: `cd kernel && bash net_repro.sh` prints the byte table,
      the signed-authorization table, and the key-lifecycle table, with one accept and each
      adversary rejected by its distinct reason.
- [ ] The proofs re-verify: `cd proofs && make && make check && make audit`. `make check` prints
      `coqchk OK`.

### The proven core is untouched

- [ ] The proven modules (cap/, store/, sls/, gate/, carmix/) are byte-identical, or if one had to
      change, the PR says which, why, and how it was re-verified. (Comment-stripped identity check
      as in kernel/VERIFY_PUBLIC.md.)

### Writing discipline

- [ ] No em-dash or en-dash as punctuation, no semicolons joining independent clauses in prose, no
      colon as a dramatic break (colons only introduce a list, a code block, or a key-value pair).
- [ ] The banned-token scrub is clean: no degraded-mode substitute word, and no stray marker
      string. Grep the diff against the project's banned-token list before submitting.
- [ ] No host user-path leaks (no /home/, /Users/, or C:\\Users in tracked files, no $HOME default
      baked into a shipped .sh script). ASCII only.

### Claims are code-backed

- [ ] Every new claim in code, a comment, or the docs is backed by observed evidence: a gated
      self-test predicate, an rdtsc-measured number, or a machine-checked proof. No print-only
      "verified". A direct-service stage says so on the wire (see PM2). Honest limits are named, not
      smoothed.
- [ ] A `// SAFETY:` comment justifies each unsafe or invariant-critical step, in the discipline
      described in CONTRIBUTING.md.

## Notes

Anything a reviewer should know, including measured numbers this PR changes and any named limit it
adds to docs/ROADMAP.md.
