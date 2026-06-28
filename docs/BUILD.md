# Build

## Toolchain and pinned versions

- A freestanding x86-64 C compiler and the LLD linker. The development used LLVM/Clang 17
  configured to target x86-64 with `--target=x86_64-unknown-none-elf -ffreestanding -nostdlib
  -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-sse`. Any Clang 17 that can target
  freestanding x86-64 works.
- Limine 9.6.7, the boot protocol and bootloader. Provides `limine.h`, the BIOS and UEFI boot
  files, and the `limine` install tool.
- QEMU 10.2.1, `qemu-system-x86_64`, for the x86-64 demos.
- BLAKE3, vendored in store/blake3/. The pinned upstream commit is recorded in
  store/blake3/UPSTREAM.txt. The portable, no-SIMD path is used so the code builds with
  `-mno-sse`.
- TweetNaCl, the Ed25519 implementation, vendored in kernel/tweetnacl.c. The provenance is
  recorded in kernel/TWEETNACL_PROVENANCE.txt.
- Coq (Rocq) 8.20.1, for the proofs. coqc and coqchk must be on PATH.

The CHERI capability-mode module tests, which are separate from the x86-64 kernel, used a
CHERI-LLVM Clang and a CHERI build of QEMU. They are not required to build or run the kernel.

## Build scripts

The build scripts read every tool location from environment variables so they can be pointed at a
local install, and fail loudly if one is unset. There are no defaults baked into the shipped
scripts.

- `CVSASX_CLANG` path to the freestanding x86-64 clang.
- `CVSASX_LLD` path to ld.lld.
- `CVSASX_NM` path to llvm-nm, used for the post-link symbol audit (zero undefined, zero libc).
- `CVSASX_OBJDUMP` path to llvm-objdump, used for the post-link instruction audit (zero CHERI).
- `LIMINE_DIR` path to the Limine 9.6.x directory that holds limine.h, the boot files, and the
  limine tool.
- `X86TOOLS` path to a prefix that holds qemu-system-x86_64 and xorriso under usr/bin, plus the
  SeaBIOS firmware.
- `BOOT_SECS` the headless boot window in seconds. The default is 40, which reaches only the early
  stages. Set 180 to observe every self-test through the live desktop shell.

## Build and boot the kernel

```
export CVSASX_CLANG=/path/to/clang \
       CVSASX_LLD=/path/to/ld.lld \
       CVSASX_NM=/path/to/llvm-nm \
       CVSASX_OBJDUMP=/path/to/llvm-objdump \
       LIMINE_DIR=/path/to/limine \
       X86TOOLS=/path/to/qemu-prefix \
       BOOT_SECS=180
cd kernel
bash build.sh
```

This compiles the kernel and the linked-in modules, prints the link audit, builds a hybrid BIOS and
UEFI bootable ISO at kernel/carmix.iso, and boots it headless in QEMU with serial output. The boot
prints a sequence of self-test results over serial. The expected results are listed in
docs/REPRODUCE.md. The ISO is also bootable on a real machine from a USB stick, which has not been
verified on physical hardware.

## Watch the desktop

The headless build above drives the desktop, windows, cursor, focus, drag, and resize by scripted
self-tests and proves them by pixel readback over serial. To watch the framebuffer and drive it by
hand, boot the ISO under the prebuilt QEMU's VNC server and attach a VNC client.

```
"$X86TOOLS/usr/bin/qemu-system-x86_64" -M q35 -m 512M \
   -cdrom kernel/carmix.iso \
   -vnc 0.0.0.0:1 -serial stdio -no-reboot \
   -L "$X86TOOLS/usr/share/seabios" -L "$X86TOOLS/usr/share/qemu"
# then attach a VNC client to localhost:5901
```

A standard distro qemu-system-x86_64 built with a window backend can open a native window with
`-display gtk` or `-display sdl` instead of the VNC server. What you will and will not see, and what
the live run_shell stage accepts, is described in full in TESTING.md.

## Build and run the two-machine migration

```
cd kernel
bash net_repro.sh
```

This builds the network test, boots two QEMU instances that share memory through an ivshmem
device, runs the migration in three variants (clean, leaf tamper, node tamper), and prints the
measured byte table and the signed-authorization accept and reject table. The expected numbers are
in docs/REPRODUCE.md.

## Verify the proofs

```
cd proofs
make && make check && make audit
```

This requires Coq 8.20.1. The expected output is in docs/PROOFS.md.
