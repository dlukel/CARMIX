/* ===========================================================================
 * CV-SASX  -  cap/cap_custodian.c
 * The root-capability custodian and the RE-MINT driver (master prompt 4.3/4.4):
 * validate a PIR against destination policy, derive a fresh locally-valid
 * capability monotonically from the destination root (via the region cap), and
 * VERIFY the result against the request - REJECTING (never clamping) any
 * amplification. Also holds the canonical<->local permission translation.
 *
 * MODULE:        cap/ (Capability Rematerialization and Root Custodian)
 * DEPENDS ON LAWS:
 *   L2  Hosted caps are restricted derivations (privileged perms rejected).
 *   L4  Monotonic, rooted provenance: derive narrow-only from root/region.
 *   L5  Reject before execution; the runtime CHERI checks are the backstop,
 *       this validator is the load-time gate. Both independent.
 *
 * TCB STATUS:    TCB-HEAVY.
 *   // TCB: cvsasx_root_custodian holds broad destination authority.
 *   // TCB: cvsasx_cap_remint is THE anti-amplification gate - a bug is a full
 *          isolation bypass (4.4). Kept small, loud, and fail-closed.
 *   // TCB: cvsasx_perms_canonical_to_local decides which permissions a hosted
 *          capability may receive.
 *
 * Build (one module):
 *   clang --target=riscv64-unknown-elf -march=rv64imafdcxcheri -mabi=l64pc128d \
 *         -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *         -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memcmp \
 *         -O2 -Wall -Wextra -c cap_custodian.c -o cap_custodian.o
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): -march/-mabi strings for the target CHERI-LLVM (see cap_strip.c).
 *   // NOTE: -fno-builtin* keeps the freestanding byte loops (cvsasx_hash_eq)
 *   //       from being lowered (LLVM LoopIdiom) to libc mem* calls that
 *   //       -nostdlib cannot resolve. -std=gnu11 pins _Static_assert/_Bool.
 *
 * DERIVATION PATH SELECTION:
 *   Default: C-builtin derivation (below) - this is what CHERI-LLVM compiles to
 *   the CSetAddr/CSetBoundsExact/CAndPerm instructions, and it builds today.
 *   Optional: define CVSASX_USE_ASM_DERIVE to route through the instruction-level
 *   skeleton in cap_remint.S (which is itself fail-closed until verified). The
 *   POST-MINT verification below makes BOTH paths safe regardless of bounds
 *   rounding behavior.
 * ===========================================================================*/
#include "cvsasx_cap.h"

#if !defined(__has_include) || !__has_include(<cheriintrin.h>)
#  error "cap_custodian.c needs <cheriintrin.h> (CHERI_PERM_* symbols)."
#endif
#include <cheriintrin.h>

/* ===========================================================================
 * Permission translation: CV-SASX canonical  <->  target-local CHERI bits.
 *
 * The map uses cheriintrin.h CHERI_PERM_* SYMBOLS, never numeric literals,
 * because the numeric bit positions are target-defined (digest: PORTABILITY
 * MANDATE). Only the portable, definitely-present data-plane permissions are
 * mapped. Privileged/sealing permissions are intentionally NOT mapped to local
 * bits - they are rejected wholesale for hosted re-mint (L2).
 *
 * // VERIFIED (per row; predefines GLOBAL=1..ASR=1024 @9e82d29; §4): CHERI_PERM_<X> exists in the target cheriintrin.h and
 *    names the intended architectural permission. Bit positions for legacy
 *    cc128 CHERI-RISC-V per digest are GLOBAL=0, EXECUTE=1, LOAD=2, STORE=3,
 *    LOAD_CAP=4, STORE_CAP=5, STORE_LOCAL_CAP=6 - but we rely on the SYMBOLS,
 *    not these numbers, so the code stays correct on differing encodings.
 * // NOTE (portability, NOT this toolchain): the RISC-V CHERI standard's AP/SDP permission MODEL (if
 *    that is the target toolchain rather than legacy CHERI-LLVM) still exposes
 *    these via the same CHERI_PERM_* symbols; the digest flags the standard
 *    extension reworks permissions entirely.
 * ========================================================================= */
struct cvsasx_perm_row { cvsasx_perm_t canon; unsigned long local; };

static const struct cvsasx_perm_row CVSASX_PERM_MAP[] = {
    { CVSASX_PERM_GLOBAL,          (unsigned long)CHERI_PERM_GLOBAL },          /* VERIFIED §4 */
    { CVSASX_PERM_EXECUTE,         (unsigned long)CHERI_PERM_EXECUTE },         /* VERIFIED §4 */
    { CVSASX_PERM_LOAD,            (unsigned long)CHERI_PERM_LOAD },            /* VERIFIED §4 */
    { CVSASX_PERM_STORE,           (unsigned long)CHERI_PERM_STORE },           /* VERIFIED §4 */
    { CVSASX_PERM_LOAD_CAP,        (unsigned long)CHERI_PERM_LOAD_CAP },        /* VERIFIED §4 */
    { CVSASX_PERM_STORE_CAP,       (unsigned long)CHERI_PERM_STORE_CAP },       /* VERIFIED §4 */
    { CVSASX_PERM_STORE_LOCAL_CAP, (unsigned long)CHERI_PERM_STORE_LOCAL_CAP }, /* VERIFIED §4 */
};
#define CVSASX_PERM_MAP_N \
    (sizeof(CVSASX_PERM_MAP) / sizeof(CVSASX_PERM_MAP[0]))

/* Canonical bits a hosted, unsealed data capability may NEVER request. */
#define CVSASX_PERM_FORBIDDEN_MASK                                       \
    ( CVSASX_PERM_SEAL | CVSASX_PERM_UNSEAL | CVSASX_PERM_INVOKE |       \
      CVSASX_PERM_ACCESS_SYS_REGS | CVSASX_PERM_SET_CID |               \
      CVSASX_PERM_UNMODELLED )

cvsasx_perm_t cvsasx_perms_local_to_canonical(unsigned long local_perms) {
    cvsasx_perm_t out = 0;
    unsigned long covered = 0;
    for (size_t i = 0; i < CVSASX_PERM_MAP_N; ++i) {
        if (local_perms & CVSASX_PERM_MAP[i].local) {
            out     |= CVSASX_PERM_MAP[i].canon;
            covered |= CVSASX_PERM_MAP[i].local;
        }
    }
    /* Any LOCAL permission bit we did not model (e.g. SEAL/UNSEAL/ASR/SET_CID or
     * a target-specific bit) is recorded as UNMODELLED so it is never silently
     * dropped; the custodian then fails closed on it. This keeps strip() honest
     * without it needing to enumerate every target's privileged bits. */
    if (local_perms & ~covered) out |= CVSASX_PERM_UNMODELLED;
    return out;
}

/* // TCB: permission translator - decides the local CHERI permission mask a
 * hosted capability may receive. The forbidden-mask and W^X rejections here are
 * the permission half of anti-amplification (master prompt 4.4 / L2). */
unsigned long cvsasx_perms_canonical_to_local(cvsasx_perm_t canon,
                                              cvsasx_status_t *err) {
    if (err) *err = CVSASX_OK;
    /* Reject privileged/sealing/unmodelled perms outright (L2). */
    if (canon & CVSASX_PERM_FORBIDDEN_MASK) {
        if (err) *err = CVSASX_ERR_PERM_FORBIDDEN;
        return 0;
    }
    /* // INV (W^X): a hosted capability is never both writable and executable.
     * CHERI does NOT architecturally forbid W&X on one capability, so this
     * software check is load-bearing. Reject rather than clamp. */
    if ((canon & CVSASX_PERM_EXECUTE) && (canon & CVSASX_PERM_WRITE_ANY)) {
        if (err) *err = CVSASX_ERR_WX_VIOLATION;
        return 0;
    }
    unsigned long local = 0;
    cvsasx_perm_t accounted = 0;
    for (size_t i = 0; i < CVSASX_PERM_MAP_N; ++i) {
        if (canon & CVSASX_PERM_MAP[i].canon) {
            local     |= CVSASX_PERM_MAP[i].local;
            accounted |= CVSASX_PERM_MAP[i].canon;
        }
    }
    /* Belt and braces: a canonical bit set but not accounted for (should be
     * impossible after the forbidden-mask check) is fail-closed. */
    if (canon & ~accounted) {
        if (err) *err = CVSASX_ERR_PERM_FORBIDDEN;
        return 0;
    }
    return local;
}

/* ---- freestanding 32-byte hash compare (constant-shape; no libc) -------- */
static int cvsasx_hash_eq(const uint8_t *a, const uint8_t *b) {
    unsigned diff = 0;
    for (size_t i = 0; i < CVSASX_BLAKE3_LEN; ++i) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

/* ===========================================================================
 * cvsasx_custodian_init - master prompt L4
 * // TCB: establishes the single broad-authority holder.
 * ========================================================================= */
cvsasx_status_t cvsasx_custodian_init(cvsasx_root_custodian_t *cust,
                                      cvsasx_cap_t root) {
    if (cust == NULL) return CVSASX_ERR_NULL_ARG;
    cust->root = NULL;
    cust->initialized = 0u;
    /* // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_tag_get -> CGetTag. A valid root must be tagged.
     * Its BREADTH (full task region, correct perms) is boot/'s responsibility;
     * we cannot architecturally query "is this almighty", so we trust boot/ to
     * pass the intended root and only assert it is a live capability here. */
    if (!__builtin_cheri_tag_get(root)) return CVSASX_ERR_BAD_ROOT;
    cust->root = root;
    cust->initialized = 1u;
    return CVSASX_OK;
}

/* ===========================================================================
 * Validate that a destination region capability is a legitimate, in-policy
 * descendant of the custodian root and actually covers the claimed object.
 * Architecturally we cannot query provenance, so we check the observable
 * invariants: tagged, perms subset of root, bounds within root, and matching
 * the declared [object_base_addr, +object_length).
 * // TCB: ties the proximate derivation source (region cap) back to root policy.
 * ========================================================================= */
static cvsasx_status_t cvsasx_check_region(const cvsasx_root_custodian_t *cust,
                                           const cvsasx_region_t *region) {
    /* // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_{tag,base,length,perms}_get lower to
     *            CGet{Tag,Base,Len,Perm}; size_t returns. (cheri-facts.md §7.4)
     * // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_length_get (CGetLen) SATURATES to 2^xlen-1 for
     *            a full-address-space capability; the within-root test below
     *            assumes root is NOT full-address-space (boot/ derives root to
     *            the task region), otherwise it loses one byte at the extreme. */
    if (!__builtin_cheri_tag_get(region->object_cap))             /* CGetTag  */
        return CVSASX_ERR_BAD_REGION;

    unsigned long rbase  = __builtin_cheri_base_get(region->object_cap);   /* CGetBase */
    unsigned long rlen   = __builtin_cheri_length_get(region->object_cap); /* CGetLen  */
    unsigned long rperms = __builtin_cheri_perms_get(region->object_cap);  /* CGetPerm */

    unsigned long root_base  = __builtin_cheri_base_get(cust->root);   /* CGetBase */
    unsigned long root_len   = __builtin_cheri_length_get(cust->root); /* CGetLen  */
    unsigned long root_perms = __builtin_cheri_perms_get(cust->root);  /* CGetPerm */

    /* region perms must be observably a subset of root perms. NECESSARY, not
     * sufficient: true lineage is not architecturally queryable, so a
     * forged-but-coincidentally-in-bounds tagged cap would also pass (see the
     * provenance // not implemented in cvsasx_cap_remint). We refuse any region whose
     * observable authority exceeds root. */
    if (rperms & ~root_perms) return CVSASX_ERR_BAD_REGION;

    /* region [rbase, rbase+rlen) must lie within root [root_base, +root_len).
     * Overflow-safe comparisons. */
    if (rbase < root_base) return CVSASX_ERR_BAD_REGION;
    {
        unsigned long root_off = rbase - root_base;
        if (root_off > root_len)            return CVSASX_ERR_BAD_REGION;
        if (rlen > root_len - root_off)     return CVSASX_ERR_BAD_REGION;
    }

    /* region cap must EXACTLY bound the object it claims to instantiate. We
     * require rlen == object_length (not >=) so that object_length - otherwise
     * an unauthenticated scalar from migrate/store, NOT covered by the BLAKE3
     * content hash (which binds object bytes, not the declared length) - is
     * pinned to a real capability quantity (CGetLen of object_cap). This closes
     * a region-internal over-provisioning gap: an inflated object_length can no
     * longer widen the bounds window inside a larger-than-claimed region cap. */
    if (rbase != region->object_base_addr) return CVSASX_ERR_BAD_REGION;
    if (rlen  != region->object_length)    return CVSASX_ERR_BAD_REGION;

    return CVSASX_OK;
}

/* ===========================================================================
 * cvsasx_cap_remint - master prompt 4.3 / 4.4
 *
 * Strategy: derive the task capability FROM the region capability (a monotone
 * descendant of root), so monotonicity relative to the object is structural.
 * Then INTROSPECT the minted capability and compare it byte-for-byte against the
 * request, REJECTING any divergence - this closes the silent-amplification gap
 * that bounds compression / rounding can otherwise open (digest: "Re-minting
 * then re-introspecting ... is necessary to prevent silent privilege
 * amplification"). Fail-closed: *out_cap is null on every error path.
 * ========================================================================= */
cvsasx_status_t cvsasx_cap_remint(const cvsasx_root_custodian_t *cust,
                                  const cvsasx_pir_t *pir,
                                  const cvsasx_region_t *region,
                                  cvsasx_cap_t *out_cap) {
    if (out_cap == NULL) return CVSASX_ERR_NULL_ARG;
    *out_cap = NULL;                       /* fail-closed: null (untagged) by default */

    if (cust == NULL || pir == NULL || region == NULL) return CVSASX_ERR_NULL_ARG;
    if (!cust->initialized || !__builtin_cheri_tag_get(cust->root))
        return CVSASX_ERR_BAD_ROOT;
    if (pir->struct_version != CVSASX_PIR_VERSION) return CVSASX_ERR_VERSION;

    /* Sealed-capability re-mint is out of scope (master prompt 4.5). // not implemented */
    if (pir->sealed || (pir->flags & CVSASX_PIR_FLAG_WAS_SEALED) ||
        pir->otype_class != CVSASX_OTYPE_CLASS_UNSEALED)
        return CVSASX_ERR_SEALED_UNSUPPORTED;

    if (!(pir->flags & CVSASX_PIR_FLAG_REFERENT_VALID))
        return CVSASX_ERR_REFERENT_MISMATCH;

    /* The PIR must refer to the very object this region instantiates. The PIR
     * set is content-addressed, so a hash match is tamper-evident (4.2). NOTE
     * (master prompt 4.5 PIR integrity): this binds the PIR to an object, NOT to
     * an authorized task - task<->PIR-set authorization and malicious-source
     * defense remain an OPEN trust-model question owned above cap/. // not implemented */
    if (!cvsasx_hash_eq(pir->referent_hash, region->hash))
        return CVSASX_ERR_REFERENT_MISMATCH;

    /* Validate the region cap against root policy. */
    {
        cvsasx_status_t rs = cvsasx_check_region(cust, region);
        if (rs != CVSASX_OK) return rs;
    }

    /* Bounds anti-amplification: the requested window must lie wholly within the
     * instantiated object. Overflow-safe; zero-length rejected. */
    if (pir->length == 0)                                return CVSASX_ERR_BAD_BOUNDS;
    if (pir->offset > region->object_length)             return CVSASX_ERR_BAD_BOUNDS;
    if (pir->length > region->object_length - pir->offset) return CVSASX_ERR_BAD_BOUNDS;

    /* Permission anti-amplification (canonical policy + subset of region). */
    cvsasx_status_t perr = CVSASX_OK;
    unsigned long local_mask = cvsasx_perms_canonical_to_local(
                                   (cvsasx_perm_t)pir->perms, &perr);
    if (perr != CVSASX_OK) return perr;
    {
        unsigned long region_perms = __builtin_cheri_perms_get(region->object_cap); /* CGetPerm */
        if (local_mask & ~region_perms) return CVSASX_ERR_AMPLIFY_PERMS;
    }

    /* Requested local base = region base + object-relative offset. region base
     * was validated to equal object_base_addr above, and offset is in-range, so
     * this cannot exceed the region. */
    unsigned long req_base = __builtin_cheri_base_get(region->object_cap) /* CGetBase */
                             + (unsigned long)pir->offset;
    unsigned long req_len  = (unsigned long)pir->length;

    /* ---- DERIVE (narrow-only) -------------------------------------------- */
    cvsasx_cap_t c;
#if defined(CVSASX_USE_ASM_DERIVE)
    /* Instruction-level skeleton path (cap_remint.S). Fail-closed until the asm
     * is verified (it traps otherwise). Post-mint checks below still apply. */
    c = cvsasx_cap_derive_from_root_asm(region->object_cap, req_base, req_len,
                                        local_mask);
#else
    /* C-builtin path: the canonical, compiling derivation.
     * // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_address_set -> CSetAddr (clears tag if the new
     *           address is unrepresentable or cs1 sealed). */
    c = __builtin_cheri_address_set(region->object_cap, req_base);       /* CSetAddr        */
    /* // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_bounds_set_exact -> CSetBoundsExact. Per the
     *           Sail-derived semantics it CLEARS THE TAG (does not trap) if the
     *           requested bounds are not exactly representable; the ISAv9 design
     *           prose says "throws an exception" - these conflict (digest). We
     *           use the EXACT variant and treat an untagged result as a hard
     *           error below, so EITHER documented behavior is handled safely. */
    c = __builtin_cheri_bounds_set_exact(c, req_len);                    /* CSetBoundsExact */
    /* // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_perms_and -> CAndPerm (AND-only; monotonic;
     *           clears tag only if cs1 was sealed). */
    c = __builtin_cheri_perms_and(c, local_mask);                        /* CAndPerm        */
#endif

    /* ---- POST-MINT VERIFICATION (anti-amplification, defense-in-depth) ----
     * // TCB: REJECT, never clamp. This is what makes rounding/representability
     * surprises safe: if the derived capability is broader than requested (or
     * the tag was cleared, or it came back sealed), we refuse outright.
     * // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_{tag,base,length,perms,sealed}_get lower to
     *            CGet{Tag,Base,Len,Perm,Sealed}. (cheri-facts.md §7.4)
     * // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): CGetLen saturates to 2^xlen-1; req_len is a real object
     *            sub-extent (<< 2^xlen), so the exact length compare is sound. */
    if (!__builtin_cheri_tag_get(c))                       /* CGetTag    */
        return CVSASX_ERR_DERIVE_UNTAGGED;                 /* e.g. inexact bounds cleared tag */
    if (__builtin_cheri_base_get(c)   != req_base)         /* CGetBase   */
        return CVSASX_ERR_VERIFY_BASE;
    if (__builtin_cheri_length_get(c) != req_len)          /* CGetLen    */
        return CVSASX_ERR_VERIFY_LENGTH;                   /* rounding amplification */
    {
        unsigned long got = __builtin_cheri_perms_get(c);  /* CGetPerm   */
        /* Exact set equality: no extra perms (amplify) and none missing.
         * // INV: equality is over the MODELLED hosted lattice
         *    (CVSASX_PERM_HOSTED_MASK); privileged/unmodelled bits were already
         *    fail-closed in cvsasx_perms_canonical_to_local - this is
         *    anti-amplification WITHIN the modelled set, not a claim about
         *    arbitrary permission bits.
         * // NOTE (portability): equality is VALID here - this toolchain is confirmed
         *    one-bit-per-permission layout. On the RISC-V CHERI STANDARD AP/SDP
         *    model (or Morello/CHERIoT) CGetPerm may return a DECODED form whose
         *    bits differ from the AND-mask; for that target replace this with a
         *    symbolic "no permission outside the requested set" test
         *    (cheri-facts.md §3.1.2 / permission-layout digest section). */
        if (got & ~local_mask) return CVSASX_ERR_VERIFY_PERMS;
        if (local_mask & ~got) return CVSASX_ERR_VERIFY_PERMS;
    }
    if (__builtin_cheri_sealed_get(c))                     /* CGetSealed */
        return CVSASX_ERR_VERIFY_SEALED;

    /* // not implemented (master prompt 4.5 - revocation / cross-epoch coherence): the
     * capability minted here is NOT tracked for later sweeping revocation across
     * epochs or machines (Cornucopia / Cornucopia Reloaded lineage, // REF
     * below). A stale PIR re-minted after its source object was freed or
     * superseded is NOT detected here. This is an unsolved design point, stubbed
     * not faked: there is no revocation hook on this path yet. */
    *out_cap = c;
    return CVSASX_OK;
}

/* ===========================================================================
 * MARKER SUMMARY
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): CHERI_PERM_GLOBAL/EXECUTE/LOAD/STORE/LOAD_CAP/STORE_CAP/
 *             STORE_LOCAL_CAP exist & name intended perms in target cheriintrin.h.
 *   // NOTE (portability): standard-extension AP/SDP permission model would need
 *             CHERI_PERM_* if the target is the RISC-V CHERI standard, not legacy.
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_tag_get/base_get/length_get/perms_get/sealed_get
 *             -> CGetTag/CGetBase/CGetLen/CGetPerm/CGetSealed.
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_address_set -> CSetAddr (tag-clear semantics).
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_bounds_set_exact -> CSetBoundsExact (tag-clear
 *             vs exception discrepancy - both handled by post-mint check).
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488; §4): __builtin_cheri_perms_and -> CAndPerm (monotonic AND).
 *   // INV:    W^X - EXECUTE never co-granted with STORE/STORE_CAP
 *             (cvsasx_perms_canonical_to_local -> CVSASX_ERR_WX_VIOLATION).
 *   // not implemented:   sealed/opaque-cap re-mint unsupported (4.5).
 *   // not implemented:   revocation / cross-epoch coherence NOT implemented (4.5) - no
 *             revocation hook on the successful-derive path.
 *   // not implemented:   task<->PIR-set authorization & malicious-source defense are an
 *             unresolved trust-model question above cap/ (4.5 PIR integrity).
 *   // not implemented:   provenance cannot be queried architecturally; region-vs-root
 *             ancestry is checked via observable invariants, not true lineage.
 *   // TCB:    cvsasx_root_custodian (broad authority); cvsasx_cap_remint
 *             (anti-amplification gate); cvsasx_perms_canonical_to_local;
 *             cvsasx_check_region; cvsasx_custodian_init.
 *   // REF:    cheri-facts.md (UCAM-CL-TR-987 / sail-cheri-riscv / cheriintrin.h);
 *             Cornucopia/Cornucopia Reloaded for revocation (NOT implemented).
 * ===========================================================================*/
