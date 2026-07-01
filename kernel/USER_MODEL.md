# USER_MODEL.md - Using CARMIX for real work today

Honest documentation of the user-visible model that the committed stages actually
demonstrate. This is not marketing and not a daily-driver claim. Every capability
below is grounded in a named `run_*` stage in `kernel/kernel.c` and its committed
log under `kernel/`. Where something is designed but not built, it is named as such.
For the full standing of the machine-checked core versus the tested code, read
`kernel/ACADEMIC_SYNTHESIS.md`. For the planned path forward, read
`kernel/ROADMAP_NEXT_ERA.md`.

The whole system runs single-CPU under QEMU. There is no physical-hardware boot and
no physical monitor observed. Numbers cited here are `rdtsc` measurements from the
committed logs and vary boot to boot.

## The one thesis, stated once

CARMIX is built from three primitives and nothing else: content-addressed objects
(an object's name is the BLAKE3 hash of its bytes), anti-amplification capabilities
(authority is a bounded grant that can only be attenuated, never widened, checked by
the gate in `kernel/cap/`), and rematerialization (turning a hashed state back into a
running one by fetching it by hash and re-verifying the hash on read). Every layer
below is one of those three primitives aimed at a new target. There is no POSIX
compatibility layer, no ambient authority, and no global mutable namespace.

## What you can do today, grounded in a committed stage

### Store and name work by content

Every object you create is put into the content store (`cvsasx_store_put`) and named
by its hash. Reading it back (`cvsasx_store_get`) re-hashes the bytes and confirms the
name (`cvsasx_hash_eq`), so integrity is re-checked on every read, not assumed. This is
the same store engine used by every stage. Identical bytes collapse to one name, so
deduplication is intrinsic and not a side table.

### Use the content-addressed filesystem (run_fs, run_cfs)

The filesystem is a Merkle namespace mapping names to hashes (`fs_dir_put`,
`fs_resolve`, `fs_read`). Opening a file is capability-gated. A write does not mutate
in place. It re-roots the tree, threading three new objects from the changed leaf up
to a new root hash (`kernel/FS_LOG.md`, FS4). The old root still resolves the entire
old tree, so the previous version is retained by construction. That means versioning
and snapshots are free: a snapshot is just a retained root hash, and a diff is a
structural hash comparison.

The cost is measured and not hidden. From `kernel/FS_LOG.md` FS8 (this-run rdtsc,
depth-2 tree): open plus resolve 47344 cyc, read plus re-verify 24869 cyc, write plus
re-root 71178 cyc. The re-root cost is the honest mutation tension of
immutability-by-hash and is surfaced, not buried behind a POSIX illusion.

### Run programs under bounded capabilities (run_cu)

Userland is capability-mediated with no ambient authority (`kernel/CU_LOG.md`,
CU1-CU6). A program is spawned from a content-addressed image (`u1_spawn`) and receives
only the capabilities explicitly delegated to it, each a bounded grant minted through
the anti-amplification gate. Programs talk over capability IPC (`u1_cap_send`,
`u1_cap_recv`) rather than a shared global namespace. A stage that reaches past its
grant is refused at the gate. Requesting more authority than was delegated is
impossible by construction, bounded by the machine-checked anti-amplification lemma
described in `kernel/ACADEMIC_SYNTHESIS.md`.

### Debug by rematerialization (run_dbg)

Execution state is a small Merkle tree in a content store (`kernel/DBG_LOG.md`,
DBG1-DBG6). Reaching a past state is direct addressing, not replay: you `store_put` a
state snapshot, and later `store_get` plus re-hash confirms you have the exact bytes
back. Undo is a store-get of a prior root. "What changed" is a structural diff
proportional to the change, not to the state size, with exact last-common-node and
first-differing-node queries. The honest edge, stated in the stage, is that this
recovers internal deterministic state only. It cannot un-send a packet or reproduce a
race, and it halts honestly at the first non-deterministic external edge.

### Send messages over the loopback channel (run_net)

There is a two-endpoint, in-kernel, loopback message channel (`kernel/NET_LOG.md`,
NET-1 onward). Endpoint identity and reachability are capabilities, and messages are
content-addressed objects. What is CARMIX-native (endpoint identity, reachability,
message integrity) is demonstrated. What content-addressing cannot name (total order
and liveness) is labeled as the explicit non-content-addressed edge and never faked.
A real NIC is out of scope and stated (NET-5).

### Draw to the graphics and output surface (run_gfx, run_shell)

The graphics surface is invented from the same three primitives, not ported from a
compositor (`kernel/GFX_LOG.md`). A surface's pixels are hash-named in the store, a
composited frame is a content-addressed object whose identity is its content hash, and
identical frames dedup by hash. A surface is reachable only through a surface
capability minted at the gate, so there is no ambient draw into a shared screen.
Input events are hash-named objects in an ordered log. `run_shell` drives a live
two-window desktop under real PS/2 input. The honest limit is that this is headless:
the framebuffer exists in QEMU and no physical monitor has been observed. GPU, KMS,
USB-HID, and real fonts are out of scope and hardware-blocked.

### See the whole system cohere (run_int)

`run_int` (`kernel/INTEGRATION_LOG.md`) does not add a mechanism. It proves the
architecture coheres as one running system and gathers the cross-subsystem hot-path
costs in one place, all measured this run across three boots, which is itself the
evidence the numbers are measured and not imported. It also states the real-hardware
gap plainly.

## What you do differently from a conventional OS

- You name work by hash, not by a path treated as identity. A path in CARMIX resolves
  to a hash. The hash is the identity. Two byte-identical objects are the same object.
- You hold explicit bounded capabilities, not ambient authority. There is no uid 0 to
  bypass a check, because the authority order has no top element to occupy. Every
  grant moves strictly down.
- Files are immutable by hash, so a write is a new version, not an in-place mutation.
  The old version does not disappear. It is simply no longer pointed at by the new
  root.
- There is no ambient global namespace. Cooperation between programs is explicit
  capability passing, not a shared writable directory tree.
- Integrity is re-verified on read, not trusted once at write. A tampered object fails
  its hash check and is refused, rather than being silently served.

## The genuine advantages

- Integrity by construction. The name is the hash, so corruption or tampering is a
  hash mismatch caught on read.
- Free versioning and snapshots. A write re-roots, so the prior root is a retained
  snapshot at no extra bookkeeping, and diffs are structural hash comparisons.
- Least authority and anti-amplification. A program cannot exceed the authority it was
  handed, enforced by the gate and bounded by a machine-checked monotonicity lemma.
- Whole-system time-travel by hash, for the in-system deterministic state that is
  content-addressed. Reaching a past state is direct addressing, not replay.
- Deduplication. Identical bytes are one object across the whole store.
- Reproducibility of the deterministic, data-race-free spine, re-verified by re-hash.

## The honest frictions and limits

- Re-root cost on every write. A depth-2 write re-root is 71178 cyc this run
  (`kernel/FS_LOG.md` FS8). This is a permanent structural cost of immutability-by-hash,
  shared with copy-on-write filesystems, amortizable but never eliminable. The mutable
  data plane deliberately stays off per-write re-rooting.
- Explicit capability passing. Cooperation that ambient authority gives for free
  requires handing over a bounded capability. This is real UX friction and is where
  capability systems historically ask more of the user.
- No in-kernel compiler yet. Content-addressed distribution is reproducible, but the
  source-to-binary build step is not built here.
- Headless graphics. The framebuffer runs under QEMU and no physical monitor has been
  observed.
- Physical-metal boot deferred. There is no hardware. Every demonstration is
  single-CPU under QEMU.

Beyond these, `kernel/ACADEMIC_SYNTHESIS.md` states five irreducible boundaries that
no amount of engineering removes, because they live on the far side of the
content-addressing membrane:

1. Real-time and liveness. A hash names bytes that already exist, so it cannot name a
   deadline or predict termination, and activate-by-hash is a latency floor. Hard
   real-time is excluded.
2. Mutable shared state. Content-addressing names immutable values, so two independent
   writers diverge into two hashes with no canonical merge. Raw multi-writer coherence
   is out of the model.
3. External non-CARMIX party. The anti-amplification ceiling binds only the local side
   that runs the gate. A foreign party is downgraded to a data source whose bytes are
   re-hashed, and its presented authority is treated as a request the local gate
   re-mints from local grants.
4. Emulated root of trust. The attestation and quoting key is the software AUTH
   keypair, not a hardware root. This demonstrates the protocol, not the security, of
   every distrusting-migration and attestation claim until a hardware anchor exists.
5. Observational-equivalence undecidability. The strongest sameness the system can
   decide is structural equivalence. Two structurally-different states that behave
   identically cannot be recognized, by Rice's theorem.

## The model stated coherently

One thesis runs through every layer. Objects are named by content, authority is a
bounded grant that only attenuates, and a hashed state is turned back into a running
one by rematerialization with the hash re-verified on read. The filesystem, userland,
debugger, message channel, and graphics surface are each that one thesis aimed at a new
target, not five separate subsystems. The advantages (integrity, free versioning,
least authority, time-travel, dedup, reproducibility) are consequences of putting
content and authority into one name. The limits (re-root cost, explicit passing, and
the five irreducible boundaries) are the honest price of that same choice.
