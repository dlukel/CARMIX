# DRV_LOG.md - CYCLE 6: DRIVER FRAMEWORK + a second driver

Stage: `run_drv()` in `kernel/kernel.c` (called in `kmain` after `run_net`, before
`run_loader`; the real framebuffer and PS/2 controller are up by then). Reuses ONLY
proven primitives: the content store (`cvsasx_store_init/put`, `cvsasx_blake3`,
`cvsasx_hash_eq`) and the software capability gate (`cvsasx_sw_custodian_init`,
`cvsasx_sw_cap_remint`, `cvsasx_swcap_check`, `conc_make_pir`). No proven-core file
is touched.

## Thesis

A driver must not hold ambient hardware access. It holds a DEVICE CAPABILITY: a
bounded MMIO region + one IRQ line + a bounded DMA window. The MMIO/DMA bounds are a
`cvsasx_swcap_t` MINTED through the proven anti-amplification gate
(`cvsasx_sw_cap_remint`), so a driver granted authority WIDER than its device is
refused at bind. Every runtime access is bounds-checked by `cvsasx_swcap_check`; an
out-of-region address (the driver's own over-run OR another device's region) is
refused. One registration/binding model (`drv_bind` + `drv_access`) hosts more than
one driver.

## Two real devices

- DISPLAY driver over the REAL Limine framebuffer (`FB->address`, length
  `height*pitch`). Functions: draws pixels through its cap, snapshots them back.
- INPUT driver over the REAL i8042 (PS/2) controller ports, modeled as the bounded
  window `[0x60, 0x65)` (data 0x60 + status/cmd 0x64), IRQ line 1. Functions: reads
  the controller status register and drains pending scancodes through its cap.

## Seams (OBSERVED serial, single-boot run this cycle)

- DRV-1 framework: `bound 2 drivers via ONE model; display cap[base=0xffff8000fd000000
  len=4096000 irq=0 dma=256B] keyboard cap[ports 0x60..0x64 irq=1]`;
  `anti-amp bind (grant WIDER than device) REFUSED at gate=y; in-region access OK
  (disp=y kbd=y); out-of-region REFUSED (disp=y kbd=y)` ->
  `DEVICE CAP (bounded MMIO+IRQ+DMA); driver granted ONLY its device; out-of-region REFUSED OK`.
- DRV-2 second driver (INPUT, PS/2): `REAL controller status read via cap=y
  status=0x1c; scancodes delivered this boot=0 (headless: no key pressed - read PATH
  genuinely functions, delivery is event-driven); ambient port REFUSED=y` ->
  `SECOND DRIVER GENUINELY FUNCTIONS (real HW reads via its cap); out-of-window REFUSED OK`.
  (The status byte 0x1c is a real i8042 read; port 0x66, outside the cap window, is
  refused.)
- DRV-3 content-addressed device data: `framebuffer frame 824b31a4... content-addressed:
  re-read+re-hash verifies=y; identical frame DEDUP (no 2nd copy)=y; single-pixel
  change DETECTED by hash=y` -> a drawn 16x16 frame is a stored object, re-verifiable
  by re-hashing the framebuffer. The keyboard's live event stream is LABELED
  non-content-addressed (liveness is not a stored object; not forced).
- DRV-4 coexistence + isolation: `both drivers function=y; display->keyboard region
  REFUSED=y; keyboard->display region REFUSED=y; IRQ-line isolation=y; DMA-window
  isolation=y` -> `TWO DEVICE-CAP-BOUNDED DRIVERS COEXIST; MUTUAL ISOLATION (anti-amp) OK`.
- DRV-5 measure (rdtsc, QEMU, this run): `device-cap check=167 cyc; driver op (gated
  pixel write)=178 cyc; frame hash (1KiB)=75651 cyc; bind/mint (anti-amp gate)=1566 cyc`.
  (Numbers vary a little per boot; they are measured, never imported.)

## Disproofs

- Ambient device access: every access goes through `cvsasx_swcap_check`; a driver's
  own over-run AND a foreign device's region are refused (DRV-1, DRV-4).
- Second-driver-faked: the INPUT driver performs REAL i8042 port reads via its cap
  (the status byte is a live hardware value); no fabricated result.
- Framework-not-general: ONE `drv_bind`/`drv_access` model hosts BOTH drivers.
- Forced content-addressing: the framebuffer FRAME is genuinely content-addressed
  (re-verifiable, dedup, tamper-detected); the keyboard event stream is labeled
  non-CA rather than dressed up as content-addressed.
- Imported numbers: all DRV-5 numbers are `rdtsc`-measured this boot.

## Scope (honest)

- Single-CPU.
- DEMONSTRATED: real Limine framebuffer + real PS/2 controller; caps minted through
  the proven anti-amp gate; every access swcap-checked; out-of-/foreign-region and
  over-wide bind refused; a drawn frame re-verifiable by hash.
- DEFERRED: interrupt-driven delivery (the keyboard is polled here, so a headless
  boot with no keypress delivers 0 scancodes - the read PATH still functions); a real
  DMA engine (the DMA window is a bounded RAM buffer modeling a device's DMA region);
  USB/xHCI HID.
- BOUNDARY: the device capability bounds LOCAL driver authority (which addresses the
  driver may touch). It does not model the device's own bus-master reach - a real
  IOMMU would be needed to bound where a DMA-capable device can write.
