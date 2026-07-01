# GFX_LOG.md - CARMIX-NATIVE REMATERIALIZATION GRAPHICS

Stage: `run_gfx()` in `kernel/kernel.c` (called in `kmain` after `run_drv`, before
`run_loader`; the real Limine framebuffer and the Cycle 6 DISPLAY device cap
`drv_reg[0]` are up by then). Reuses ONLY proven/existing primitives: the content
store (`cvsasx_store_init/put/get`, `cvsasx_blake3`, `cvsasx_hash_eq`), the software
capability gate via Cycle 6's `drv_mint` / `drv_access` (which wrap
`cvsasx_sw_custodian_init` + `cvsasx_sw_cap_remint` + `cvsasx_swcap_check`), and the
DISPLAY device cap + `drv_fb_px`. No proven-core file is touched.

## Thesis (why this is NOT a ported compositor)

A conventional windowing system hands processes ambient draws into a shared screen
buffer and mediates with a privileged compositor process. CARMIX has no ambient
authority, so the graphics system is invented from the three native primitives:

- **Content-addressing**: a surface's pixel bytes are hash-named in the store; a
  composited FRAME is a content-addressed object (the hash of its composition);
  input events are hash-named objects in an ordered log. Identical frames dedup by
  hash - there is no "frame buffer generation counter", the frame's identity IS its
  content address.
- **Anti-amplification capabilities**: a surface is reachable ONLY through a SURFACE
  CAPABILITY - a `cvsasx_swcap_t` minted over the surface bytes through the proven
  anti-amp gate. Drawing to / reading a surface you do not hold the cap for is
  refused at the gate. There is no ambient screen and no ambient input.
- **Rematerialization**: a past frame is recovered DIRECTLY by its hash
  (`cvsasx_store_get` + re-hash confirms it is the actual frame) and re-presented -
  the display time-travels to an earlier UI state. Nothing is re-composited: this is
  the Cycle 4 debugging model (`run_dbg`) applied to pixels, not replay.

A frame is literally the hash of `composite(authorized surfaces)`; present is a
capability-gated blit to the framebuffer. That is the whole model - no window
manager, no z-stack server, no ambient blit path.

## Model

- SURFACE = a pixel buffer that is (a) `cvsasx_store_put`-named by content and (b)
  bounded by a surface cap minted via `drv_mint(base=&buf, len=bytes, grant=bytes)`.
  `gfx_touch`/`gfx_peek` go through `drv_access` (the proven `cvsasx_swcap_check`),
  so any byte outside the surface's own buffer is refused.
- FRAME = `gfx_composite(A,B,frame)` reads the two authorized 16x16 surfaces (each
  read gated by that surface's cap) into one 32x16 canvas, then `cvsasx_store_put`
  names it by content.
- PRESENT = `gfx_present` writes the frame to the REAL framebuffer THROUGH the DRV
  DISPLAY device cap (`drv_fb_px`), so a process without the display cap cannot reach
  the screen (a surface cap does not reach the framebuffer either).
- INPUT EVENT = a 10-byte `gfx_evt_t` stored by content and appended (its hash) to an
  ordered log `gfx_evlog`; a repeated event dedups in the store while the log keeps
  order.

## Seams (OBSERVED serial, single-boot run this cycle; hashes are content-derived so
stable across boots, cycle counts are `rdtsc`-measured per boot and vary)

- GFX-1 `surfA=a7c84d2cdfe9 surfB=b476ac3c5c41 -> composited FRAME=b24544ea505084c1`;
  `frame re-composite+re-hash == frame hash=y; identical frame DEDUP (no 2nd copy)=y;
  input events hash-named in ordered log (len=3), repeat event dedups by hash=y` ->
  `SURFACES/FRAMES/EVENTS ARE CONTENT-ADDRESSED OBJECTS OK`.
- GFX-2 `compositor: authorized surfaces {A,B} composited + presented to the REAL
  framebuffer @64,520 via the DISPLAY device cap; framebuffer readback == composited
  frame=y` -> `CAPABILITY-MEDIATED COMPOSITE PRESENTED OK`. (The readback compares
  every one of the 512 presented pixels against the frame - a real present, not a
  claim.)
- GFX-3 `anti-amp: in-authority draw (own surface) OK=y; out-of-authority surface
  WRITE REFUSED=y; out-of-authority surface READ REFUSED=y; surface cap can NOT reach
  the framebuffer (no ambient screen)=y` -> `NO AMBIENT SCREEN/INPUT AUTHORITY OK`.
  (Holding A's cap, reaching B's bytes is out of A's cap region -> refused by the
  swcap gate; a surface cap cannot reach `FB->address` either.)
- GFX-4 `past frame b24544ea505084c1 vs current 29f6a64560e8a57a (surface mutated ->
  UI state moved=y)`; `re-materialize past frame BY HASH: fetched 2048 bytes, re-hash
  == recorded hash=y (the ACTUAL past frame recovered by NAMING it - NOTHING
  re-composited -> NOT replay); recovered==past not current=y`; `re-presented the
  re-materialized past frame -> the display time-travels to the earlier UI state
  (framebuffer==past frame=y)` -> `VISUAL STATE RE-MATERIALIZED BY HASH OK`.
- GFX-5 rdtsc (QEMU, this run): `surface-store=80511 composite-frame(+hash)=267165
  present-to-fb(32x16)=99591 input-event-append=46245 re-materialize-by-hash=170071
  cyc`. Optimization (content-addressed present skip, the frame hash is already known
  from compositing): `present-always=99591 cyc -> present-if-changed(identical
  frame)=465 cyc` - a re-present of an unchanged frame collapses to ONE 32-byte hash
  compare instead of a 512-pixel blit; correctness comes from content-addressing (the
  identical frame is provably already on screen). Numbers vary per boot; measured,
  never imported.

## Disproofs

- Ambient screen/input: a surface without its cap is refused, and a surface cap
  cannot reach the framebuffer (GFX-3).
- Ported compositor: there is no window manager - a frame IS the hash of its
  composition, present is capability-gated (GFX-1/GFX-2).
- Frames-not-content-addressed: a frame is a stored object, re-derivable to the same
  hash and dedup'd by hash (GFX-1).
- Replay-masquerading-as-rematerialization: the past frame is FETCHED by hash and
  re-hash-confirmed; nothing is re-composited (GFX-4).

## Scope (honest)

- Single-CPU.
- DEMONSTRATED: the content-addressed surface/frame/event model; the
  capability-mediated compositor; anti-amp refusal of out-of-authority surface access
  and of a surface cap reaching the screen; by-hash re-materialization of past visual
  state - all on the REAL Limine framebuffer via the proven anti-amp gate + the Cycle
  6 display cap, with a full-frame framebuffer readback proving the present.
- DEFERRED/BOUNDARY: a real on-hardware display - headless QEMU's framebuffer is a
  linear buffer we write and read back, so no physical monitor is observed here (the
  readback is the honest substitute); live input is event-driven, so the events here
  are synthesized and hash-named honestly rather than captured from a keypress; alpha
  / z-order blending beyond side-by-side composition is not implemented. As Cycle 6
  labeled it, live input liveness itself is not a stored object - a captured event IS.
