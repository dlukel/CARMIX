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

## Code style discipline

These are the rules the codebase already follows. Match them.

- Comment for safety, not for narration. A step that depends on an invariant or does something the
  type system does not guard carries a `// SAFETY:` comment stating the invariant it relies on. A
  comment explains why a thing is correct, not what the next line obviously does.
- Mark a deliberate simplification where it is made, naming the known ceiling and the upgrade path,
  so a shortcut reads as intent rather than an oversight. The open ceilings are collected in
  docs/ROADMAP.md.
- No em-dash or en-dash as punctuation. No semicolon joining two independent clauses in prose. No
  colon as a dramatic break, a colon introduces a list, a code block, or a key-value pair only. No
  emoji. ASCII only.
- No degraded-mode wording. Do not name a weaker substitute path, the system commits to the real
  mechanism or it fails closed. The banned-token scrub also rejects a specific stray marker string,
  run the scrub in the PR template before you submit.
- No host user-path leaks. No /home/, /Users/, or C:\\Users in a tracked file. No $HOME, /root/, or
  ~/ default baked into a shipped .sh script, the build scripts read every tool location from a
  required environment variable and fail loudly when one is unset.
- Observed-truth-only proof discipline. Every PASS line, comment, and doc claim maps to a gated,
  computed predicate, an rdtsc-measured number, or a machine-checked proof. There are no print-only
  "verified" claims. A stage that drives a core routine directly rather than through a live event
  says so on the wire (see PM2 in kernel/VERIFY_PUBLIC.md). A named limit is stated as plainly as a
  capability.

## How verification works here

The whole system is verified by building it, booting it in the emulator, and reading the serial
self-tests, plus re-checking the Coq proof. The numbers are the proof.

1. Build and boot. The build scripts read every tool path from an environment variable and fail
   loudly if one is unset. Set all six (CVSASX_CLANG, CVSASX_LLD, CVSASX_NM, CVSASX_OBJDUMP,
   LIMINE_DIR, X86TOOLS) plus BOOT_SECS, then run the kernel build:

   ```
   cd kernel && BOOT_SECS=180 bash build.sh
   ```

   The default 40-second boot window only reaches the early stages. Set BOOT_SECS=180 to observe
   every self-test through the live desktop. See TESTING.md for the toolchain versions and the
   exact six-variable invocation, and docs/BUILD.md for the toolchain detail.

2. Read the link audit. The build prints three zeros, 0 undefined symbols, 0 libc/host symbols, and
   0 CHERI instructions. Any non-zero is a regression.

3. Read the serial self-tests. Every passing stage ends in OK, HOLDS, or PROVEN. Nothing prints
   FAIL, a deliberate fault prints its dump rather than triple-faulting, and the boot reaches the
   live E4 desktop shell and parks there polling real PS/2 input. The stage codes and what each one
   gates are listed in kernel/VERIFY_PUBLIC.md and docs/REPRODUCE.md.

4. Run the two-machine migration. `cd kernel && bash net_repro.sh` boots two QEMU instances over
   ivshmem and prints the byte table, the signed-authorization table, and the key-lifecycle table.
   The expected numbers are in docs/REPRODUCE.md.

5. Re-verify the proof. `cd proofs && make && make check && make audit`. `make check` must print
   `coqchk OK`. `make audit` reports the closed-context properties and the one stated
   collision-resistance hypothesis. See docs/PROOFS.md.

A change must not break the proven core. The capability model (cap/), the content-addressed store
(store/), the single-level store (sls/), the WebAssembly gate (gate/), and the software capability
backend (carmix/) are verified. If a change requires modifying one of them, say so in the pull
request and re-run the full suite. Keeping these modules byte-identical is the easiest path to
acceptance, the comment-stripped identity check in kernel/VERIFY_PUBLIC.md shows how to confirm it.

## Where to start

Good first issues, drawn from the real open items in docs/ROADMAP.md. Each is self-contained and
does not require touching the proven core.

- Add free() to the bump allocator. The per-process userspace allocator runs on granted heap pages
  but has no free. A real free list over the granted range is a contained piece of userspace work.
- A storage or block driver. There is no persistent storage. An NVMe or AHCI driver is the
  prerequisite for a filesystem and for persisting the trust and nonce state.
- Dynamic linking for the loader. The loader runs a static freestanding ELF. Supporting a dynamic
  image and shared objects is a defined extension of the content-addressed loader.
- Huge-page promotion. Mapping is 4K only. Promoting a contiguous run to a 2M page is a contained
  page-table change with a measurable benefit.
- A dedup-scan policy. Deduplication happens on the way in. A background scan that finds already-
  resident identical content is open work, and a real one must address the named dedup side channel
  in SECURITY.md, not just ignore it.

Pick one, open an RFC issue using the feature request template, and say what evidence the change
will produce before you write it.

## Developer certificate of origin

Sign off every commit. The Signed-off-by line certifies that you wrote the change or have the right
to submit it under the project license, per the Developer Certificate of Origin. Add it with:

```
git commit -s
```

which appends a line of the form:

```
Signed-off-by: Your Name <your.email@example.org>
```

A pull request whose commits are not signed off will be asked to add the sign-off before review.

## Contributor agreement

The repository owner's choice of license and contributor agreement is recorded in
docs/LICENSE_CHOICE.md. Follow the agreement noted there when it is in place.
