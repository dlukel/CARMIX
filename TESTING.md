# Testing CARMIX

This is the shortest path for someone with no prior context to build CARMIX, run its
self-tests, and watch the framebuffer desktop. Every command below was run during
verification and its output observed.

## 1. Dependencies / toolchain

| Tool | Version used | Purpose |
|------|--------------|---------|
| Clang/LLVM | 17 (freestanding x86-64) | compile the kernel + linked modules |
| ld.lld | 17 | link the kernel ELF |
| llvm-nm | 17 | post-link symbol audit (0 undefined / 0 libc) |
| llvm-objdump | 17 | post-link instruction audit (0 CHERI) |
| Limine | 9.6.7 | boot protocol + BIOS/UEFI boot files + `limine` install tool |
| QEMU | 10.2.1 (`qemu-system-x86_64`) | emulate the machine |
| xorriso | any recent | build the El-Torito ISO (lives under `X86TOOLS/usr/bin`) |
| Coq (Rocq) | 8.20.1 | re-verify the proofs (optional, see docs/PROOFS.md) |

Any Clang 17 that can target `--target=x86_64-unknown-none-elf -ffreestanding -nostdlib
-mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-sse` works. The BLAKE3 portable
(no-SIMD) path is used so `-mno-sse` builds cleanly. The CHERI capability-mode module tests
are separate and are NOT needed to build or run this kernel.

## 2. Build + headless self-test boot

The build scripts read every tool location from environment variables and fail loudly
(`set VAR to ...`) if any is unset. There are no `$HOME` defaults baked into the shipped
scripts. Set all six variables, including `CVSASX_NM` and `CVSASX_OBJDUMP`:

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

`build.sh` compiles the kernel and the linked-in modules, prints the link audit, builds a
hybrid BIOS+UEFI bootable ISO at `kernel/carmix.iso`, then boots it headless with serial
output. The default boot window is 40 seconds, which only reaches the early stages. Set
`BOOT_SECS=180` (as above) to observe every self-test through the live desktop shell.

The ISO is also bootable on real hardware from a USB stick. That path has not been verified
on physical hardware here.

## 3. What green output looks like

The link audit prints three zeros:

```
E0 undefined symbols : 0  (expect 0)
E0 libc/host symbols : 0  (expect 0; whole-name match)
E0 CHERI instructions: 0  (expect 0)
```

Then the boot streams self-test stages. Every passing stage ends in `OK` or `HOLDS` or
`PROVEN`. A deliberate fault that traps prints its dump instead of triple-faulting, and nothing
prints `FAIL`. Representative green lines:

```
B1  long mode: CS=0x...28 EFER.LMA=1 -> 64-bit CONFIRMED
B4  FB 1280x800 bpp=32 ... -> DRAWN TO SCREEN OK
E1 wasm validate->check->exec in kernel: result=51 (expect 51) -> OK
E2 anti-amplification in kernel: control=ACCEPT; 6/6 attacks rejected ... -> HOLDS
R0 BLAKE3 KAT digest("")=af1349b9...41f3262 ... -> MATCH
R3 rematerialize: bit-identical=y ... -> REMAT OK
R4 ... -> PROVEN (O(changed), no over/under-report)
S2 ... -> CONSOLE SCROLLS OK
S3 ... -> WINDOW DRAWN OK
A7/A8/A9/E1/E2/E3 ... -> MOUSE/DRAG/Z-ORDER/FOCUS/FRONT-DRAG/RESIZE OK
C2 ... -> DESCHEDULE->DEMATERIALIZE->REMATERIALIZE->RESUME, COUNTER SURVIVED OK
PM2 forced access (serviced via the #PF core) -> rematerialized by hash (BLAKE3-verified) ... bit-identical=y ... OK
```

The boot ends at the live desktop:

```
=== STEP 10b E4: live desktop - click=focus, titlebar=drag, grip=resize, keys=type ===
```

After that banner the kernel sits in `run_shell`, polling real PS/2 keyboard + mouse input
forever. That is the correct terminal state, not a hang. There is no triple-fault and no
panic in the whole run.

## 4. See the framebuffer desktop (graphical)

The self-test run above is HEADLESS (`-display none`): the desktop, windows, cursor, focus,
drag, and resize are all driven by SCRIPTED self-tests inside the kernel and PROVEN by
pixel-readback over serial. No human is clicking in that run.

To actually watch the framebuffer and drive it by hand, run QEMU with a display instead of
`-display none`. The prebuilt QEMU used here was compiled WITHOUT the gtk/sdl backends
(`-display help` lists only `none`), but it DOES include the VNC server, which serves the
framebuffer on a port any VNC client can attach to:

```
# VNC path (works on the headless prebuilt QEMU; connect a VNC client to localhost:5901)
"$X86TOOLS/usr/bin/qemu-system-x86_64" -M q35 -m 512M \
   -cdrom kernel/carmix.iso \
   -vnc 0.0.0.0:1 -serial stdio -no-reboot \
   -L "$X86TOOLS/usr/share/seabios" -L "$X86TOOLS/usr/share/qemu"
# then in another terminal: vncviewer localhost:5901
```

If you have a standard distro `qemu-system-x86_64` that was built with a window backend, you
can open a real window directly instead of VNC:

```
# native-window path (needs a QEMU built with gtk or sdl)
qemu-system-x86_64 -M q35 -m 512M -cdrom kernel/carmix.iso \
   -display gtk -serial stdio -no-reboot
# (substitute -display sdl if gtk is unavailable)
```

### What you WILL see

CARMIX boots, the self-tests stream over serial, and the kernel draws its OWN framebuffer
desktop: a colored background, windows with titlebars and a resize grip, a yellow cursor,
scrolling text consoles inside windows, and the WASM/REMAT result panels. Once the boot
reaches `run_shell` (the E4 desktop), it takes LIVE input from the QEMU-provided PS/2 mouse
and keyboard: move the cursor, click a window to focus/raise it, drag it by the titlebar,
resize it by the bottom-right grip, and type into the focused console.

### What you WILL NOT see

No third-party GUI applications, no browser, no GPU-accelerated graphics. Everything on
screen is software-rendered by CARMIX's own code (the "software-render ceiling" is by
design: no vendor drivers). The desktop is the kernel's own apps, nothing else.

## 5. Two-machine migration + proofs (optional)

```
cd kernel && bash net_repro.sh     # ivshmem two-VM migration + signed-authz tables
cd proofs && make && make check && make audit   # Coq re-verification (needs Coq 8.20.1)
```

Expected numbers and tables are in docs/REPRODUCE.md and docs/PROOFS.md.
