# U3_LOG.md - concurrency as rematerialization across real cores (single-CPU-per-core)

Honest record of U-3: multiple content-addressed executions rematerialized across the two
brought-up SMP cores, coordinating only through capability IPC, with anti-amplification
holding across cores, and a first on-CARMIX exercise of DRCC.

All figures rdtsc-measured on this run (QEMU -smp 2 -accel tcg,thread=multi, so the two
vCPUs run on separate host threads, genuinely in parallel).

## Scope (read first)

U-3 is genuine multi-process concurrency on the committed SMP cores. It reuses the SMP
bring-up (the second core, per-CPU state, the IPI path), the U-1 capability IPC, and the
anti-amplification gate. The DRCC part is a FIRST EXERCISE, one concrete instance, NOT a
full validation of the theory. This is not production concurrency. The AP is put in a
cross-core dispatch worker loop for U-3 and parks again afterward, so the single-CPU stages
that follow run unchanged.

## Proven core untouched

The nine proven modules and the rest of the proven core are byte-for-byte identical. The
only committed-code change is in kernel/kernel.c: the SMP AP, after its self-test, now runs
a dispatch worker loop instead of parking immediately (it parks after U-3). The SMP S1-S4
self-test runs before that and is unchanged.

## What was built and shown (observed)

### U3-1 genuine multi-process concurrency on two cores

```
U3-1 two processes on two cores: BSP progress=100000 AP progress=100000 (both advanced=y); locked shared=100000/100000 (lock holds across cores=y)
U3-1 unlocked shared=67265/100000 -> lost updates=32735 (a RACE: genuine cross-core parallelism)
U3-1 -> GENUINE MULTI-PROCESS CONCURRENCY ON TWO CORES OK
```

Two content-addressed executions were rematerialized onto the two cores (one dispatched to
the AP, one on the BSP) and ran concurrently. Both advanced their own per-CPU counter. A
shared counter incremented under the cross-core spinlock totalled exactly 100000 (the lock
holds across cores). The same counter incremented WITHOUT the lock lost 32735 updates, which
only happens when two cores execute the read-modify-write truly in parallel. A single core
could not lose updates, so this is genuine parallelism.

### U3-2 capability-mediated cross-core coordination

```
U3-2 cross-core IPC: data crossed=y; capability arrived usable=y; un-held cap send refused (anti-amp across cores)=y
```

The two processes coordinate only through the U-1 capability IPC. A data message and a
capability crossed and arrived usable. A send attempting to transfer a capability the sender
does not hold was refused. Anti-amplification holds at the cross-core crossing.

### U3-3 DRF processes reach reproducible content-addressed state (first DRCC exercise)

```
U3-3 DRCC FIRST EXERCISE (one instance, NOT a full validation): DRF pair content hash d05014af13f5df61 reproducible across 3 runs=y
U3-3 racy pair: content hash differs across runs (DRCC undefined for racy state)=y (the boundary the theory drew)
U3-3 -> DRF REACHES REPRODUCIBLE CONTENT-ADDRESSED STATE (first DRCC exercise on CARMIX) OK
```

The DRCC result (banked as theory) says a data-race-free program reaches a reproducible
content hash at a synchronization-point cut. This is the first chance to exercise it on real
concurrent CARMIX cores. Two data-race-free processes each wrote only their own slot (no
shared racy state), a synchronization-point cut read the combined state in a canonical order,
and the content hash was identical across three runs. As the honest boundary, a racy pair
(both cores writing the same slot without a lock) produced a different hash across runs, which
is exactly where the theory says DRCC is undefined. This is ONE instance, a first exercise,
NOT a full validation of DRCC. It is labeled that way here and in the boot output.

### U3-4 anti-amplification holds across cores

```
U3-4 cross-core amplification attempt (request 128 over a 64 ceiling reached via cross-core coordination): refused by the gate=y (status 9)
```

An attempt to gain authority via a cross-core interaction (requesting a capability wider than
the ceiling) was refused by the same anti-amplification gate with BAD_BOUNDS (status 9). The
ceiling holds transitively across cores, not just within one.

### U3-5 measurement

```
U3-5 rdtsc: cross-core dispatch+join=195393 cyc (per rematerialize-on-AP + sync)
```

Dispatching a process onto the AP and joining it measured about 195k cyc per cycle, dominated
by the cross-core wakeup and synchronization under multi-threaded TCG.

## Regression

The full regression re-runs green except the same pre-existing PM0 stall. SMP, GC, DS, VG,
persistence, TS/SM, U-0/U-1/U-2, and the migration suite all still pass, and the AP parks
after U-3 so the single-CPU stages are unchanged.

## Forbidden dodges, each disproven

- D1 faked concurrency: the cross-core race (32735 lost updates) proves real parallelism.
- D2 cross-core amplification: refused by the gate (status 9).
- D3 DRCC overclaim: labeled a FIRST EXERCISE, one instance, with the racy-state contrast shown.
- D7 proven-core drift: the nine proven modules are byte-identical; only the AP park became a
  worker loop in kernel.c.
- D8 separability: the U-3 COMPLETE marker precedes U-4.

This banks concurrency as the rematerialization of content-addressed executions across real
cores, with anti-amplification across them and the first DRCC exercise on CARMIX.
