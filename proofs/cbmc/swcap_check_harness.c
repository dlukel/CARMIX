/* ===========================================================================
 * CV-SASX / CARMIX  -  proofs/cbmc/swcap_check_harness.c
 *
 * PART P3 (the real-C rung): a CBMC bounded model-checking harness for the REAL,
 * committed software anti-amplification enforcement gate cvsasx_swcap_check().
 *
 * WHAT THIS IS: a COMPLETE SAT/SMT decision, WITHIN THE STATED BOUNDS, of the
 * bounds + permission (anti-amplification) property of the real swcap enforcement
 * logic. CBMC compiles the C to a bit-precise logical formula and hands it to a
 * SAT/SMT solver, which decides EXHAUSTIVELY whether the asserted property can
 * ever be violated over the whole nondeterministic input domain.
 *
 * WHAT THIS IS NOT: this is NOT a Coq/Qed machine-checked proof (see
 * proofs/Carmix.v for that lineage), and it is NOT an seL4-scale functional-
 * correctness refinement of the full system. It is a bit-precise, bounded
 * decision procedure over ONE function's real object code path.
 *
 * The REAL swcap.c is #include-d verbatim below (relative path); it is NOT
 * retyped and NOT edited. swcap.c stays byte-identical - this harness only reads
 * it. (Master directive D5 / D10.)
 *
 * TARGET FUNCTION: cvsasx_swcap_check(c, offset, size, need_perms) - the SFI
 * enforcement gate, the software analogue of the CHERI hardware bounds/perm
 * check. It reads c->valid, c->perms, c->length (the check is base-relative, so
 * c->base is intentionally unconstrained here).
 *
 * BOUND JUSTIFICATION (D4 - no vacuous bound):
 *   cvsasx_swcap_check contains NO loops and NO recursion; its control-flow is
 *   straight-line with exactly four guard branches. CBMC therefore needs NO
 *   unwind bound at all - the decision is COMPLETE and EXHAUSTIVE over the entire
 *   input space: every 2^64 offset, every 2^64 size, every 2^32 need_perms,
 *   every 2^64 base, every 2^64 length, every 2^8 valid. Nothing is excluded.
 *   Non-vacuity is additionally forced by asserting grant == spec (an IFF): the
 *   solver must explore BOTH the grant side and the deny side, so a gate that
 *   trivially denied everything would be a counterexample, not a false pass.
 * ===========================================================================*/

#include <assert.h>
#include <stdint.h>

/* The REAL committed enforcement gate, included verbatim (never retyped/edited).
 * swcap.c pulls in carmix/cvsasx_swcap.h -> cap/cvsasx_cap.h -> cap/cvsasx_pir.h
 * (found via -I carmix -I cap on the cbmc command line). */
#include "../../carmix/swcap.c"

/* Nondeterministic inputs: CBMC leaves the return values unconstrained, so the
 * solver considers every possible value. */
uint64_t nondet_u64(void);
uint32_t nondet_u32(void);
uint8_t  nondet_u8(void);

int main(void) {
    /* Nondet capability fields (the whole [base,length,perms,valid] handle). */
    cvsasx_swcap_t c;
    c.base   = nondet_u64();
    c.length = nondet_u64();
    c.perms  = nondet_u32();
    c.valid  = nondet_u8();

    /* Nondet requested access. */
    uint64_t offset    = nondet_u64();
    uint64_t size      = nondet_u64();
    uint32_t need_perms = nondet_u32();

    /* Call the REAL enforcement gate. &c is non-NULL: we test a real capability,
     * not the null-argument path. */
    int grant = cvsasx_swcap_check(&c, offset, size, need_perms);

    /* The exact anti-amplification specification of the gate:
     *   - is_valid:      the capability carries a (software) validity tag;
     *   - perms_subset:  requested permission bits are a SUBSET of the
     *                    capability's - no permission amplification;
     *   - within_bounds: the requested window [offset, offset+size) lies within
     *                    [0, length), i.e. within [base, base+length) in absolute
     *                    terms (the gate is base-relative). Written overflow-safe:
     *                    (offset <= length) short-circuits before the subtraction,
     *                    so length - offset never underflows. */
    int is_valid     = (c.valid != 0);
    int perms_subset = ((c.perms & need_perms) == need_perms);
    int within_bounds = (offset <= c.length) && (size <= c.length - offset);
    int spec = is_valid && perms_subset && within_bounds;

    /* THE PROPERTY (bounds + permission anti-amplification, both directions):
     * the gate grants IFF the access is valid, permission-subset, and in-bounds.
     * Forward direction  (grant  => spec): no granted access ever amplifies
     *   permissions or escapes [base, base+length) - the safety property.
     * Backward direction (spec  => grant): the gate is exact, not vacuously
     *   restrictive - it really does admit the safe set (anti-vacuity, D4). */
    assert(grant == spec);

    return 0;
}
