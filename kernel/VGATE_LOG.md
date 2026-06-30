# VGATE_LOG.md - the single-CPU versioned (MVCC) capability gate

This is the honest record of the one build-ready result of the live-capture research
line: a versioned capability slot whose authority transition has a single atomic
commit point, so a capture firing at any instant reads either the committed pre-state
or the committed post-state capability, never a torn or half-written one. The in-flight
authority transient that previously needed a drain collapses to that commit point.

All cycle figures below were measured by rdtsc in CARMIX on this run. No figure is
imported from a research document or hardcoded. The numbers vary run to run under TCG
because they are real.

## Scope (stated plainly, read first)

This build implements and measures the SINGLE-CPU versioned capability gate. It
eliminates the in-flight authority transient (the surrender-mid-flight drain) by giving
the authority transition a single atomic commit point. It is demonstrated on a single
CPU, where there are no remote cores, no cross-thread hardware races, and no stale TLBs.

It does NOT implement or validate the multi-core memory cut, the TLB hazard handling,
or the RC/synchronization-point cut. Those require SMP bring-up (AP startup, per-CPU
state, the IPI path, cross-core invalidation), which does not exist in CARMIX. The DRCC
formalization and the RC-cut-satisfies-DRCC result from the research line remain theory,
UNVALIDATED on CARMIX until SMP exists. This build banks the one piece demonstrable now.

## Proven core untouched

The versioned slot was added BESIDE the proven core, calling the existing gate
unchanged. The nine proven-core modules (carmix/swcap.c, carmix/backend_sw_gate.c,
sls/sls.c, gate/sfi_checker.c, gate/executor.c, gate/optimize.c, store/object_store.c,
cap/cvsasx_cap.h, sls/cvsasx_sls.h) and the rest of the proven core are byte-for-byte
identical after comment-stripping. Only kernel/kernel.c changed (the VG section). The
versioned slot reuses the existing capability representation (cvsasx_swcap_t) and the
existing anti-amplification check (cvsasx_sw_cap_remint), neither replaced nor
duplicated.

## What was built and shown

### G1 / VG1 - versioned capability slot, atomic commit

The slot is a version word plus two cvsasx_swcap_t buffers; the active buffer is
version & 1. A replacement is written to the INACTIVE buffer; the commit is a single
8-byte aligned store to the version word, which x86-64 guarantees atomic.

```
VG1 versioned slot: version word + buf[2], active = version&1. Initial active = buf[0] LOAD len=128 valid=1
VG1 commit point = a single 8-byte aligned version store (x86-64 atomic): version field offset=0 size=8 -> single word-sized write (D1)
```

D1 (fake commit atomicity) is disproven: the version field is a naturally-aligned
uint64_t at struct offset 0 (compile-time asserted), and the bump is one C assignment
that compiles to a single aligned MOV; on x86-64 an aligned 8-byte store is atomic.

### G3 / VG2 - anti-amplification preserved, proven

The committed capability after a gate op is exactly what the existing check produces,
never more. A capture before the commit sees the pre-surrender capability; after, the
checked replacement; neither exceeds the ceiling. A wider request is refused and
nothing is committed.

```
VG2 gate op (LOAD 128 -> LOAD 64): pre-commit capture sees len=128 post-commit capture sees len=64 status=0 post<=ceiling=y
VG2 adversarial wider-bounds status=9 (BAD_BOUNDS) added-perm status=12 (AMPLIFY_PERMS); active capability UNCHANGED by the refusals=y
VG2 replay re-runs the existing check (status=0), no amplification on replay
VG2 -> ANTI-AMPLIFICATION PRESERVED (existing check reused, never exceeded, wider refused) OK
```

D3 (anti-amplification weakened to fit) is disproven: the existing check
(cvsasx_sw_cap_remint) is reused unchanged; a wider-bounds request is refused with
status 9 (CVSASX_ERR_BAD_BOUNDS) and an added-permission request with status 12
(CVSASX_ERR_AMPLIFY_PERMS), and on a refusal the active capability is unchanged (no
amplification). Re-driving from a pre-commit capture re-runs the check.

### G4 / VG3 - capture at random points sees no torn capability

The gate was driven through 64 surrender-and-replace ops; a capture (a read of the
version-selected active buffer) was fired at many points across each op, including
during the inactive-buffer write and at the commit boundary.

```
VG3 drove 64 gate ops with captures; total captures=320 of which mid-inactive-write=256 (these would tear a naive single-buffer slot)
VG3 torn/half-written captures seen=0 -> every capture read a COMPLETE well-formed active capability (pre before commit, post after)
VG3 D2 contrast: a NAIVE single-buffer slot, captured mid-write, read a TORN capability (matches neither pre nor post)=y
VG3 -> CAPTURE-AT-RANDOM-POINTS NEVER TORN (versioned), naive slot WOULD tear (contrast) OK
```

320 captures, of which 256 landed DURING the inactive-buffer write; every one read a
complete, well-formed active capability (the pre-state before the commit, the post-state
after), zero torn.

D2 (torn capture hidden by timing) is disproven two ways: the 256 mid-write captures are
counted and genuinely land while the inactive buffer is being written; and the contrast
shows a NAIVE single-buffer slot, captured mid-write (a replacement differing in base
and length, written in place), reads a TORN capability matching neither the pre nor the
post state. That torn read is exactly what the versioned slot prevents. On single CPU
the capture is driven at instrumented points across the op (the analogue of a concurrent
capture); a seqlock-style version re-check on the reader is what SMP would add, and is
out of scope here.

### G5 / VG4 - cost measured in rdtsc

```
VG4 gate op UNVERSIONED (existing remint into one slot) = 868 cyc; VERSIONED (remint+inactive-write+commit) = 780 cyc; versioning ADDS = 0 cyc (MEASURED delta, rdtsc this run)
VG4 commit-point (version bump alone) = 14 cyc (near the rdtsc-resolution floor)
VG4 per-slot memory: versioned=56 B vs single-buffer baseline=24 B -> delta=32 B (the second buffer + version word)
```

Measured this run (20,000-iteration averages, rdtsc):

- Unversioned gate op (existing remint into a single slot): 868 cyc.
- Versioned gate op (remint + inactive-buffer write + atomic commit): 780 cyc.
- Added cost of versioning: within rdtsc measurement noise of the remint cost. On this
  run the versioned path measured slightly lower than the unversioned (so the reported
  delta is 0); on an earlier run it measured about +168 cyc. The honest finding is that
  the extra work (a four-field buffer copy plus a single word bump) is small relative to
  the remint and at or below the measurement noise on this hardware.
- Commit-point (the version bump alone): 14 cyc, near the rdtsc resolution floor.
- Per-slot memory: 56 B versioned vs 24 B single-buffer, a 32 B delta (the second
  buffer plus the version word).
- The drain that the in-flight transient previously required is eliminated: it collapses
  to the commit bump. The snapshot/capture path pays a bounded version-selected read
  instead of the process path paying a drain, which is the direction the research
  claimed; reported here as measured, not asserted.

D4 (hardcoded or imported numbers) is disproven: every figure above was produced by
rdtsc in this CARMIX run; none is taken from a document.

## Regression

Full regression re-ran green except the same pre-existing PM0 stall:

```
PM0 -> *** FAIL (see stall report) ***          (the one accepted, pre-existing FAIL)
M0 exhaustion: drained 250 pool frames, next frame_reserve returned 0x0 -> FAILED LOUDLY (fail-closed)
F2 -> FAIL-CLOSED ON VERIFY FAILURE OK
```

The recurring negative tests each still reach their fail-closed result: M0 pool
exhaustion fails loudly, and F2 (corrupt-store verify) is followed by its OK line. The
software desktop comes up after the stages, so the kernel is intact past the VG group.
TS, SM, and the persistence stages all still run.

## Forbidden dodges, each actively disproven

- D1 fake commit atomicity: shown a single 8-byte aligned store (offset 0, size 8).
- D2 torn capture hidden by timing: 256 captures landed mid-write reading the intact
  active buffer; the naive single-buffer contrast tears.
- D3 anti-amplification weakened: existing check reused unchanged; wider refused 9/12.
- D4 hardcoded numbers: all figures rdtsc-measured this run.
- D5 multi-core overclaim: stated plainly as single-CPU only; the multi-core cut and the
  DRCC/RC-cut theory are named UNVALIDATED until SMP.
- D6 proven-core drift: the nine proven modules and the rest are byte-identical; only
  kernel/kernel.c changed.

This banks the single-CPU versioned gate. The multi-core memory cut, the TLB hazard,
and the RC/synchronization-point cut remain unvalidated until SMP bring-up exists.
