# SMP_LOG.md - multi-core bring-up (S1-S4)

This is the honest record of SMP bring-up: a second CPU running, per-CPU state, a
working inter-processor interrupt (IPI) path, and one cross-core sanity test. It is the
foundation that makes CARMIX genuinely multi-core. It is engineering, not research: the
steps are known, executed, and proven on observable evidence.

All cycle figures are rdtsc-measured in CARMIX on this run. The run is under QEMU
`-smp 2 -accel tcg,thread=multi` (multi-threaded TCG), so the two vCPUs execute on
separate host threads, i.e. genuinely in parallel. That is why the unlocked race below
manifests; single-threaded TCG interleaves the vCPUs and would not show lost updates.

## Scope (read first, plainly)

This is SMP bring-up ONLY: a second CPU up, per-CPU state, a working IPI path, one
sanity test. It does NOT implement or validate the multi-core memory cut, the COW-sweep
TLB shootdown for capture, the RC/synchronization-point cut, or DRCC. Those are SEPARATE
later cycles this foundation UNLOCKS but does not contain. After the test the AP parks
(cli;hlt) and the single-CPU path (all prior stages) runs exactly as before. This build
banks the multi-core foundation, not the multi-core capture.

How the AP is started: Limine performs the standard INIT-SIPI-SIPI and hands each
Application Processor to the kernel in long mode at its goto_address (the known sequence,
via the existing boot path). This build does not hand-write a real-mode trampoline; it
takes the parked AP over at goto_address. That is the standard way to bring up APs on a
Limine-booted kernel and is the reuse the directive asked for.

## Proven core untouched

The nine proven modules (carmix/swcap.c, carmix/backend_sw_gate.c, sls/sls.c,
gate/sfi_checker.c, gate/executor.c, gate/optimize.c, store/object_store.c,
cap/cvsasx_cap.h, sls/cvsasx_sls.h) and the rest of the proven core are byte-for-byte
identical after comment-stripping. SMP bring-up is new infrastructure built beside them:
the only tracked files changed are kernel/kernel.c (the SMP section, the Limine MP
request, an xAPIC LAPIC driver, the IPI path, per-CPU state) and kernel/build.sh (the
`-smp 2 -accel tcg,thread=multi` QEMU flags). No proven module was edited.

## What was built and shown (observed)

### S1 - AP startup

```
S1 Limine MP: cpu_count=2 bsp_lapic_id=0 (Limine did the INIT-SIPI-SIPI; APs parked in long mode)
S1 AP reached kernel C: lapic_id=1 proc_id=1 (a second core is executing)
S1 -> AP STARTED: lapic_id=1 up; startup latency=599023 cyc (rdtsc this run, excludes the AP serial print) OK
```

A second core reached kernel C code and announced itself on serial with its LAPIC id
(read from its own LAPIC), which is observable proof a second core is executing. The
startup latency (599,023 cyc, rdtsc this run) is measured from writing the AP's
goto_address to the AP signalling it is up, excluding the AP's serial print.

### S2 - per-CPU state

```
S2 per-CPU isolation: BSP(idx0) counter=200000  AP(idx1) counter=200000 -> each core incremented ONLY its own per-CPU counter (=200000 each): isolated y
```

Each CPU has its own per-CPU record (indexed by Limine processor_id): its LAPIC id, its
own counter, its up/ipi flags. Each core read its own id and incremented only its own
per-CPU counter; neither stepped on the other's. The boot CPU also has its own kernel
stack and GDT/TSS from the existing boot; each AP runs on the stack Limine gave it.

### S3 - LAPIC and IPI path

```
S3 IPI CPU0->CPU1 delivered+handled=y  IPI CPU1->CPU0 delivered+handled=y
S3 -> IPI PATH WORKS BOTH WAYS (real LAPIC IPI, handler fired, EOI) OK
```

The Local APIC is initialized in xAPIC mode on each core (IA32_APIC_BASE global enable,
x2APIC cleared; the LAPIC MMIO mapped uncached at PCD). A real IPI was sent CPU0 to CPU1
and CPU1 to CPU0, via the LAPIC ICR (vector 0xF0); each target took the interrupt in its
handler, set a flag the sender then observed, and acknowledged with a LAPIC EOI. This is
a genuine hardware IPI, not a software callback. This primitive is what every future
cross-core operation (including the TLB shootdown for capture) will be built on.

### S4 - cross-core sanity test (honest)

```
S4 cross-core (each core 100000 unlocked + 100000 locked increments, concurrent):
S4   LOCKED shared counter = 200000 (expected 200000 -> the spinlock works across cores: y)
S4   UNLOCKED shared counter = 112245 of 200000 attempted -> lost updates = 87755 (a RACE: proves the two cores ran TRULY concurrently)
S4 -> CROSS-CORE SANITY OK (lock exact across cores, per-CPU isolated, real concurrency)
```

Both cores ran the same work concurrently. Under a spinlock (an xchg-based test-and-set),
the shared counter totalled exactly 200000 (2 x 100000), so the lock works across real
cores. WITHOUT the lock, the shared counter reached only 112245 of 200000 attempted:
87,755 lost updates. Lost updates on a read-modify-write can only happen when two cores
execute it truly concurrently; a single faked core could not lose updates. So the race is
positive evidence of genuine parallel execution. (Under single-threaded TCG the vCPUs
interleave rather than run in parallel and the race does not appear; this run uses
multi-threaded TCG, where it does.)

## rdtsc numbers measured this run

- AP startup latency: 599,023 cyc (goto_address write to AP up, excluding the serial line).
- No IPI round-trip latency is reported because it was not measured in isolation this run;
  the IPI functional result (delivered+handled both ways) is what is reported.

## Regression (single-CPU path intact)

The full single-CPU regression (boot, rematerialization, TS, SM, persistence, the
versioned gate, the proven core, the desktop) re-runs after the AP parks. It stays green
except the same byte-identical pre-existing PM0 stall; the M0 (pool-exhaustion,
fail-closed) and F2 (corrupt-store, fail-closed then OK) negatives are present. Bringing
up the second core did not break the single-CPU path (the AP parks with cli;hlt and the
prior stages run on the BSP exactly as before).

## What this does NOT do (named)

This is the foundation. It does not implement the multi-core memory cut, the COW-sweep
TLB shootdown for capture, the versioned gate under real cross-core concurrency, the
RC/synchronization-point cut, or DRCC validation. The DRCC formalization and the result
that a synchronization-point cut establishes it for data-race-free programs remain theory,
now UNLOCKED for future validation on this SMP foundation but not validated here. Those
are future cycles. No claim in this build validates multi-core capture or DRCC.
