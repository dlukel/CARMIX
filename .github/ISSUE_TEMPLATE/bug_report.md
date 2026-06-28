---
name: Bug report
about: A self-test that should be green is not, or observed behavior does not match a claim
title: "[bug] "
labels: bug
---

## What happened

State the observed behavior. If a self-test is involved, name the stage code (for example E2, R3,
US3, PM2) and quote the serial line verbatim.

## What you expected

State the expected behavior. Where a doc or a self-test claims it, link the file and line.

## Reproduction

The exact command. For most things this is one of:

```
cd kernel && BOOT_SECS=180 bash build.sh        # boot + self-tests
cd kernel && bash net_repro.sh                  # two-machine migration + authz tables
cd proofs && make && make check && make audit   # Coq re-verification
```

## Environment

- Toolchain versions (Clang/LLVM, Limine, QEMU, Coq). See TESTING.md for the versions used.
- Host OS.
- Whether this is the emulator path (the only verified path) or something else.

## Serial transcript

Paste the relevant lines. If the build failed, paste the link audit (the three E0 lines). If a
fault triggered, paste the register dump.
