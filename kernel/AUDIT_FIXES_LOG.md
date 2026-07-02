# AUDIT_FIXES_LOG.md - four real defects from the code-quality audit, fixed at root

Honest record of the surgical fixes applied to kernel/kernel.c after the read-only code-quality
audit (recorded in kernel/MEGAPLAN.md) found four real defects. Root-cause fixes only, no proven-core
change, full regression re-verified green except the one labeled PM0 stall. Every audit negative is
demonstrated firing on real serial.

## The four fixes

- F-SMP1 (the one finding that affects plausible hardware). percpu[] was keyed by the ACPI
  processor_id, which is not guaranteed dense; an unknown LAPIC id was silently misattributed to
  CPU0. Fixed at root: the BSP assigns a DENSE slot 0..g_ncpu-1 and carries it to ap_entry through
  the Limine MP extra_argument; every percpu write is guarded by idx>=0 && idx<SMP_MAXCPU;
  lapic_index returns -1 on a miss (was 0); isr_ipi rejects a spurious IPI from an unknown LAPIC id
  instead of charging CPU0; the BSP-lapic-absent case STOPs. CV, DRCC, and U-3 cross-core dispatch
  all trust this mapping and all stay green. Demonstrated: S2 prints the dense-slot map and asserts
  lapic_index(lapic_id)==slot<g_ncpu for every core (S2 -> DENSE per-CPU slot map OK).
- F-GC1. The run_gc GC3 root-set insert used a raw gc_nrc++ that could write gc_rc[256]. Fixed:
  insert via the committed bounds-checked gc_rc_apply path. Demonstrated: with gc_nrc scratch-set to
  the 256 cap, the insert refuses instead of writing one past the end.
- F-ELF1. elf_parse range checks used the additive form (phoff + phnum*phentsize > len and
  off + filesz > len) which can wrap in 64-bit. Fixed: subtraction form (phoff > len ||
  phnum*phentsize > len-phoff; off > len || filesz > len-off), the same overflow-safe style the gate
  uses. Demonstrated: a malformed image whose p_offset is near 2^64 is REJECTED by elf_parse
  (L0 F-ELF1 malformed image -> elf_parse returned a negative).
- F-U1. materialize/U1 restore copied to rsp without checking the destination lies inside the task
  stack. Fixed: validate [rsp, rsp+used) within [t->stack, t->stack+U_STACK_SZ] (subtraction form),
  fail-closed return 0 otherwise. Demonstrated: an out-of-range rsp restore is REFUSED with no wild
  write (U1 F-U1 refused=y).

## PM0

PM0 is left as the one labeled *** FAIL, per the LA2 diagnosis (kernel/INTEGRATION_LOG.md addendum):
a test-accounting artifact where the shared global pf_anon_zeroed delta underflows in full boots.
The real accounting fix (snapshot-isolate the count or gate the middle guard on >=3) was not applied
here because it was not clearly safe under the byte-identical-proven-core rule for this pass; it
stays honestly labeled rather than masked.

## Verification (observed on real serial, this build)

- Compile clean. Full boot green with exactly ONE *** FAIL, the labeled PM0 stall (line 861 of the
  boot). M0 and F2 negatives reach their fail-closed lines.
- The behavior-changing F-SMP1 rework did NOT regress the cross-core paths: CV8 DRF convergence
  still fires and U-3 completes; the boot runs through every stage to run_sr (SR7) and the E4
  desktop.
- All four audit negatives print their refusal on serial (S2 dense map, F-GC1 bound, L0 F-ELF1
  reject, U1 F-U1 refuse).

## Guards

The nine proven modules and proofs/Carmix.v are byte-for-byte identical. Only kernel/kernel.c
changed (66 insertions, 15 deletions) plus this log. No numbers are imported.
