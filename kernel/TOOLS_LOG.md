# TOOLS_LOG.md - CARMIX-native dev + daily tools surface (T-1..T-5)

`run_tools()` in `kernel/kernel.c`, called from `kmain` right after `run_int()` and
before `run_shell()`. It ties the ALREADY-BUILT subsystems into a usable,
capability-mediated daily workflow. Nothing new was invented; no proven-core file
(`carmix/ gate/ sls/ store/ cap/ proofs/`) was touched. It composes:

- the content-addressed FS (`fs_dir_put` / `fs_resolve` / `fs_read` + `u0_store`) - run_fs
- `u1_spawn` + the `u1_parent`/`u1_child` bounded-authority model - run_cu / run_u1u2
- rematerialization debugging (`dbg_state` / `dbg_resolve` / `dbg_store`) - run_dbg
- the loopback endpoint channel (`net_reach` + `net_send_ceiling_ok` + `net_msg_build`) - run_net

## Honest scope

- Single-CPU, headless.
- There is NO in-kernel C compiler. The CARMIX-native dev model is: content-addressed
  source + content-addressed program image + spawn-under-capability + rematerialization
  debugging. An actual compile toolchain is EXTERNAL / future work - the ELF used in T-2
  was built by an external toolchain and embedded (`user_elf.h`). No compiler is faked.
- Every security branch shows a REAL refusal path (T-2 unauthorized run, T-4 unauthorized
  send), not a caveat in prose.

## Seams and REAL serial evidence (this run)

Observed on serial this boot (`SINGLE_BOOT=1 BOOT_SECS=140`):

**T-1 content-addressed text editor** - an edit produces a NEW object (new hash) and
re-roots it into the FS; the OLD version is retained (free versioning).
```
T-1 editor: edit -> OLD text hash=034b1b7b239edc2a (len 16) -> NEW text hash=b8a4ab706fa3176b (len 26); new hash=y
T-1 re-root (NOT in-place): new root=adba27708789ccb2 resolves new text=y; OLD root=3d4c76f854bf92af STILL resolves the OLD text (free versioning)=y
```
The edit is a distinct object (hash changes), the new root resolves it, and the OLD
root still resolves the OLD text. This disproves in-place edit.

**T-2 run a program under capability** - a content-addressed program image is spawned
under a bounded capability set; an unauthorized run (missing the needed capability) is
REFUSED at the anti-amp boundary (`u1_spawn` D1: cannot grant what the parent lacks).
```
T-2 program image ecf8604801a2524d (8776 B) spawned under a bounded cap set {console, image-read}: bounded=y
T-2 unauthorized run (requests a capability the launcher does NOT hold) -> u1_spawn returned 1 = REFUSED (D1 anti-amp, no ambient authority)=y
```
The image is a real content-addressed object (its own hash). The child is spawned
holding exactly {console, image-read}; requesting an un-held slot returns `U0_EFAULT`.
This disproves ambient authority.

**T-3 debug it by rematerialization** - the run's state is captured as a content-addressed
snapshot; a PAST state is re-materialized DIRECTLY by its hash and a re-hash CONFIRMS the
bytes ARE that state (direct addressing, NOT replay).
```
T-3 remat-debug: state snapshots st0=b098b5eec8e7 -> st1=f84face3ce75
T-3 time-travel to st0 by hash: fetched 80 bytes, re-hash == recorded hash=y (NOTHING re-executed -> direct addressing, NOT replay)
T-3 re-materialized PAST r0=1 vs current(st1) r0=42 -> the ACTUAL old bytes, recovered by naming them=y
```
Fetch st0 by hash, re-hash equals the recorded hash, and the PAST register value (r0=1)
is recovered from the re-materialized bytes vs the current state (r0=42). This disproves
replay-masquerading-as-rematerialization.

**T-4 daily flow** - create + edit a file (T-1), navigate to it by name (`fs_resolve`),
list the live process + its capability set (the run_cu model), and send a message over
an endpoint capability (run_net). Authority is enforced at every step; ONE unauthorized
step (a send with no endpoint cap) is REFUSED.
```
T-4 live process P1 (image ecf8604801a2) caps: slot0=CONSOLE slot2=STORE-READ; count=2
T-4 daily flow: create+edit note (old kept, new resolves)=y; navigate-by-name new todo=70e18f383c1c3fbc
T-4 send over endpoint cap (content-addressed msg 9796c9e59b9124bb, cap-gated + anti-amp)=y; UNAUTHORIZED send (no endpoint cap) REFUSED (no ambient reach)=y
```

**T-5 measure (rdtsc, this run)**
```
T-5 rdtsc (this run): edit+re-root=155841 spawn-run=411 rematerialize-debug(fetch+re-hash)=29909 namespace-nav=40695 cyc
```
edit+re-root is the honest mutation-tension number (a write re-threads the tree to a new
root); spawn-run is cheap (cap-set bound check); remat-debug and namespace-nav are
store-get + re-hash costs. Numbers vary run to run (rdtsc, single QEMU vCPU).

```
TOOLS -> DAILY WORKFLOW OK: content-addressed editor (edit re-roots, old kept), program image spawned under a bounded cap (unauthorized run REFUSED), remat-debug time-travels by hash (re-hash-confirmed, NOT replay), daily flow end-to-end with authority enforced + one step REFUSED
```

## Regression

Single boot this run: all prior stages green (128 subsystem-OK summary lines); the only
hard FAIL is the accepted PM0 stall (`PM0 -> *** FAIL (see stall report) ***`, documented
fail-closed re-entry guard). M0/F2 negatives reach their fail-closed lines; every anti-amp
refusal (T-2, T-4, plus all prior stages) prints its real refusal.

## What is disproved (with a real branch each)

- ambient authority: T-2 unauthorized run + T-4 unauthorized send both REFUSED.
- in-place edit: T-1 edit re-roots to a new object/root; the old version still resolves.
- replay-masquerade: T-3 re-hash confirms the re-materialized state is the actual bytes.
- faked compiler: no compiler is claimed or emulated; the toolchain is external/embedded.
