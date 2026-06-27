# Reproduce

Every number below is printed by the listed command. The values are what the development observed
in QEMU.

## Boot, in-kernel executor, and in-kernel rematerialization

```
cd kernel
bash build.sh
```

The boot window defaults to 40 seconds, which prints through the early milestones. The later stages
(the scheduler, residency manager, fault handler, userspace, loader, concurrency, and policy) print
after that under the emulator's slower timing, so set a longer window to observe them all:

```
cd kernel
BOOT_SECS=180 bash build.sh
```

The serial output includes the following self-test results.

- Boot and long mode. The kernel reports the code segment and the long-mode bit, confirming
  64-bit mode.
- Memory. A frame is allocated, mapped, written, and read back. The allocator fails closed when
  exhausted.
- Framebuffer. A background, a rectangle, and text are drawn and confirmed by pixel readback.
- E0. The capability backend and the WebAssembly gate link into the kernel with zero undefined
  symbols, zero libc symbols, and zero capability-hardware instructions.
- E1. A WebAssembly task runs under the gate. With input 10 it returns 51.
- E2. Six anti-amplification attacks are each rejected by their intended check. The status codes
  print as wider-bounds, added capability permission, write-execute, forbidden permission, version
  mismatch, and referent mismatch.
- E3. An out-of-bounds guest access raises a software fault and the kernel stays intact.
- R0. The BLAKE3 digest of the empty input equals the official value
  af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262.
- R3. A computation's state checkpoints and rematerializes bit-identically.
- R4. After a one-leaf change to a 256-leaf state the structural diff reports one changed object
  and resolves only a handful of objects, not the whole graph. The same holds at 1024 leaves.
- Desktop. The console scrolls, a window draws, the cursor tracks the mouse, the window raises on
  click, drags by the titlebar, and resizes by the grip, each confirmed by pixel readback.

## Two-machine migration and signed authorization

```
cd kernel
bash net_repro.sh
```

This boots two QEMU instances sharing memory through ivshmem and prints two tables.

Measured bytes on the wire.

```
cold sync   blocks=1093  bytes=44505
warm hop 1  (one leaf changed)        blocks=4  bytes=1176
warm hop 2  (a different leaf)        blocks=4  bytes=1176
warm hop 3  (two leaves)             blocks=7  bytes=2219
```

Hop 3, which changes two leaves, transfers more than hop 1, which changes one leaf. The warm
transfer tracks the change set, not the total state. The leaf-tamper and node-tamper variants each
show the destination rejecting a forged block on a hash mismatch and not installing it.

Signed-authorization accept and reject table.

```
legitimate migration          ACCEPT
wrong signing key             REJECT bad-signature
signed root, different graph  REJECT root-mismatch
wrong destination identifier  REJECT wrong-scope
wrong computation identifier  REJECT wrong-scope
ceiling above source          REJECT anti-amp-ceiling
replayed nonce                REJECT replay-or-expired
expired record                REJECT replay-or-expired
```

One legitimate migration is accepted. Seven adversaries are each rejected by a distinct reason.

Key-lifecycle accept and reject table. The same reproducer prints this from the clean run.

```
enroll under the authority key            ACCEPT
migrate under the enrolled key            ACCEPT
migrate under a never-enrolled key        REJECT unknown-key
rotate signed by a non-authority key      REJECT bad-authority
rotate under the authority key            ACCEPT
migrate under the new key                 ACCEPT
migrate under the old key after rotation  REJECT unknown-key
replay an old rotation                    REJECT stale-epoch
revoke under the authority key            ACCEPT
migrate under the revoked key             REJECT revoked-key
revoke signed by a non-authority key      REJECT bad-authority
```

The legitimate enroll, rotate, and revoke sequence accepts. Each lifecycle attack is rejected by
its own distinct reason, and revoked-key is distinct from unknown-key.

## Proof re-verification

```
cd proofs
make && make check && make audit
```

`make check` prints `coqchk OK`. `make audit` prints `Closed under the global context` for
anti_amplification, valid_dest_no_wx, and valid_dest_no_forbidden, and prints the single
collision-resistance hypothesis for acyclic_graph. See docs/PROOFS.md.
