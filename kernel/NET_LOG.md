# NET_LOG.md - Cycle 3, networking (loopback), respecting NETWORK_MODEL.md

`run_net()` in `kernel/kernel.c`, called from `kmain` after `run_cfs()`. A LOOPBACK,
in-kernel, two-endpoint message channel. This is the demonstrable half of
`kernel/NETWORK_MODEL.md`: endpoint identity and reachability are CARMIX-native, and
ordering plus liveness are the explicit non-content-addressed edge, labeled as such
and never faked. Real NIC is out of scope and stated (NET-5).

All numbers below are rdtsc-measured this run (single boot, SINGLE_BOOT=1). Re-running
regenerates them; they are not imported or hardcoded.

## What is reused, not reimplemented

- Content store: `cvsasx_store_init/put/get`, `cvsasx_blake3`, `cvsasx_hash_eq`, and
  the store's own `index_count`/`dedup_hits` accounting (a dedicated `net_store` arena
  so networking does not pressure the shared `u0_store`).
- The proven swcap anti-amplification gate: `cvsasx_sw_cap_remint` with
  `cvsasx_sw_custodian_init` + `conc_make_pir`, identical to the U-0 pool acquire and
  the U3-4 cross-core ceiling. No gate source was edited.
- The U-4 delegation-attenuation pattern for the delegated endpoint cap.
- `sputs/sdec/hx/rdtsc` for output and measurement.

No generic socket layer is imported. There is no Berkeley-socket API, no ambient
network authority, no TCP/IP.

## The model (four parts, per the NETWORK_MODEL verdict)

1. Channel identity is an immutable content-addressed endpoint descriptor (a 16-byte
   tuple: peer id + channel id), stored so its BLAKE3 hash is its stable name.
2. Frozen history is a sequence of content-addressed message objects, each
   `seq(8) | prev_hash(32) | payload`, named by the hash of its own bytes; integrity
   by re-hash on receive, dedup by identical hash.
3. Reachability is a bounded capability. `net_cap_t = {held, desc, reach}` names
   exactly one channel descriptor and carries a reach ceiling. `net_reach()` refuses
   any process holding no cap (no ambient reachability) or a cap for a different
   channel. The reach ceiling is gate-enforced via `cvsasx_sw_cap_remint`.
4. The live part (ordering across messages, and liveness) is authority-gated but NOT
   content-addressed. Ordering is an explicit hash chain + sequence counter. Liveness
   is explicit mutable session state (`alive` flag, `high_water` seq), labeled
   non-content-addressed. The stream is never claimed content-addressed.

## Seams (observed serial output this run)

- NET-1 endpoint as a bounded capability. Authorized reach y; NO-cap reach REFUSED
  (no ambient) y; wrong-channel cap REFUSED y. Delegated cap ceiling 32 < parent 64
  (strict attenuation) y; over-delegation REFUSED y; the attenuated ceiling is
  GATE-enforced: a send of ceiling+1 is refused by the swcap remint y.
  `NET-1 -> ENDPOINT IS A BOUNDED CAP; UNAUTHORIZED REACH REFUSED; DELEGATION ATTENUATES OK`.
- NET-2 messages as content-addressed objects. Message stored under the hash of its
  own bytes y; receive re-hash verifies intact y; a byte tampered in flight is
  rejected by hash mismatch y; an identical message dedups (no second copy:
  index_count unchanged, dedup_hits +1) y.
  `NET-2 -> MESSAGES ARE CONTENT-ADDRESSED; INTEGRITY BY RE-HASH; IDENTICAL DEDUP OK`.
- NET-3 ordering/liveness edge, explicit and honest. Each message references the prior
  message's hash; the chain verifies order y; delivering msg2 after msg0 (prev != last)
  is DETECTED as out-of-order/missing y. Liveness is explicit mutable session state
  (alive=1, high-water seq=2), LABELED non-content-addressed; the stream is not claimed
  content-addressed.
  `NET-3 -> ORDER VERIFIABLE VIA HASH CHAIN; GAPS DETECTED; LIVENESS LABELED NON-CA OK`.
- NET-4 end-to-end. Over loopback, 3/3 messages delivered, each cap-gated (reach +
  ceiling checked per send), integrity-verified (re-hash), and in-order (chain);
  an unauthorized sender (no cap) is REFUSED end-to-end.
  `NET-4 -> WORKING CAP-GATED, ORDERED, INTEGRITY-VERIFIED CHANNEL (AUTHORITY ENFORCED) OK`.
- NET-5 real-NIC readiness (honest gap). Every result is LOOPBACK, in-kernel. Missing
  for a real NIC: a virtio-net/e1000 driver + TX/RX rings; device interrupts + async
  I/O (this build is single-CPU cooperative); a real clock for timeouts/backpressure/
  liveness (here a mutable counter/flag proxy); an external non-CARMIX peer (the
  anti-amp reach ceiling binds only the LOCAL endpoint, so across a foreign peer only
  re-hash integrity is unilaterally enforceable). The CARMIX-native halves reuse the
  proven store + gate unchanged.
- NET-6 measurement (rdtsc, loopback), this run:
  - endpoint-check (net_reach): 438 cyc
  - message send + re-verify (build + store_put + re-hash + compare): 70062 cyc
  - ordering-chain verify (two hash-chain link compares): 1011 cyc
  - round-trip (reach + build + send + re-verify): 60435 cyc

## Disproven claims (each a real refusal/detection branch)

- Ambient reachability: DISPROVEN. A process holding no cap, or a cap for another
  channel, is refused by `net_reach` (NET-1). Reaching a peer requires the cap.
- Stream-claimed-content-addressed: DISPROVEN by construction. Ordering and liveness
  are explicitly labeled non-content-addressed (NET-3, NET scope line). Only the
  descriptor and each completed message are content-addressed.
- Unverified message: DISPROVEN. A tampered message fails re-hash and is rejected
  (NET-2); every end-to-end delivery re-hashes before accepting (NET-4).
- Imported generic sockets: none. No socket API, no TCP/IP; reach is a capability, not
  a file descriptor.

## Scope (honest)

Single-CPU LOOPBACK, in-kernel. Demonstrated: content-addressed endpoint descriptor,
bounded reach with gate-enforced anti-amplification and U-4 delegation attenuation,
content-addressed messages with re-hash integrity and dedup, explicit hash-chain
ordering with gap detection, and an end-to-end cap-gated ordered integrity-verified
channel. Deferred (hardware-blocked): real NIC/ring/IRQ/TCP-IP, real clock for
liveness/timeouts/backpressure, and an external non-CARMIX peer. Real boundary
(irreducible, from NETWORK_MODEL.md N4): a live connection - the temporal liveness
predicate plus the not-yet-existing future bytes plus mutable session state - cannot
be a content-addressed capability; it is expressible only in the authority dimension,
and across a foreign peer only re-hash integrity is unilaterally enforceable.
