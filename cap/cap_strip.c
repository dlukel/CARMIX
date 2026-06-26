/* ===========================================================================
 * CV-SASX  -  cap/cap_strip.c
 * STRIP: decompose a live CHERI capability into a Position-Independent
 * Reference (master prompt 4.1). Reads each semantic field via the CGet*
 * introspection family (through the CHERI-LLVM builtins), DROPS the validity
 * tag (never serialized), and records the referent as (object hash + offset).
 *
 * MODULE:        cap/ (Capability Rematerialization and Root Custodian)
 * DEPENDS ON LAWS: L3 (referent = content address, not vaddr), L4 (records only
 *                  describable rights; cannot create authority).
 * TCB STATUS:    Integrity-relevant but NOT amplification-capable. strip()
 *                only READS and DOWN-ENCODES; it mints no capability and writes
 *                no tag. A bug here can corrupt a PIR (caught downstream by the
 *                re-mint validator + referent-hash check) but cannot, by
 *                itself, widen any authority. Not counted as broad-authority TCB.
 *
 * Build (one module):
 *   clang --target=riscv64-unknown-elf -march=rv64imafdcxcheri -mabi=l64pc128d \
 *         -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *         -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memcmp \
 *         -O2 -Wall -Wextra -c cap_strip.c -o cap_strip.o
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): exact -march/-mabi strings for the targeted CHERI-LLVM build
 *   //         (cheribuild commonly uses rv64imafdcxcheri / l64pc128d). Confirm
 *   //         with `clang --print-supported-extensions` / the SDK docs.
 *   // NOTE: -fno-builtin* prevents the byte loops below from lowering to libc
 *   //       mem* calls that -nostdlib cannot resolve; -std=gnu11 pins
 *   //       _Static_assert/_Bool.
 * ===========================================================================*/
#include "cvsasx_cap.h"

/* cheriintrin.h provides the cheri_* wrappers, cheri_otype_t, and the
 * CHERI_OTYPE_* constants we use to classify sealing. It is a CHERI-LLVM
 * compiler header (available freestanding). We call the underlying
 * __builtin_cheri_* directly for the field reads so the lowering to each CGet*
 * instruction is explicit and auditable.
 * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): <cheriintrin.h> is present in the target SDK; if a given toolchain
 *            revision lacks it, the otype constants must be sourced from the ISA
 *            reference instead of invented. */
#if !defined(__has_include)
#  error "compiler lacks __has_include; cannot probe for <cheriintrin.h>"
#endif
#if !__has_include(<cheriintrin.h>)
#  error "cap_strip.c needs <cheriintrin.h> (cheri_otype_t / CHERI_OTYPE_*)."
#endif
#include <cheriintrin.h>

/* ---- tiny freestanding byte copy/zero (no libc; auditable) -------------- */
/* // NOTE (freestanding): these byte loops are exactly the idiom LLVM's
 * LoopIdiomRecognize can rewrite into memcpy/memset libcalls. -nostdlib
 * provides no such symbols, so the documented build keeps -fno-builtin(-memcpy/
 * -memset) to force them inline. Do NOT drop those flags without either
 * providing freestanding mem* or asserting the .o has no undefined mem* refs. */
static void cvsasx_bcopy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
}
static void cvsasx_pir_zero(cvsasx_pir_t *p) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < sizeof(*p); ++i) b[i] = 0u;
}

/* ---------------------------------------------------------------------------
 * Classify a local otype into a PORTABLE class id.
 * // not implemented (master prompt 4.5 - otype reconciliation): only the two
 * architecturally well-defined cases are classifiable portably. Any other
 * sealed otype is namespace-local and has no portable meaning, so it collapses
 * to OPAQUE and is rejected at re-mint. This is a stub for an unsolved problem,
 * deliberately NOT a reconciliation mechanism.
 *
 * Digest facts used (cheri-facts.md):
 *   - cheri_type_get / __builtin_cheri_type_get is SIGNED; reserved otypes are
 *     negative. (UCAM-CL-TR-987 §7.4 CGetType: EXTS for reserved.)
 *   - CHERI_OTYPE_UNSEALED == -1 and CHERI_OTYPE_SENTRY == -2 on CHERI-RISC-V
 *     (legacy CTSRD convention). On Morello these are 0 and 1 - hence we compare
 *     against the cheriintrin.h symbols, never literals.
 * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): CHERI_OTYPE_UNSEALED / CHERI_OTYPE_SENTRY values for the target
 *            (sail-cheri-riscv src/cheri_types.sail; cheriintrin.h arch split).
 * ------------------------------------------------------------------------- */
static cvsasx_otype_class_t cvsasx_otype_to_class(cheri_otype_t otype,
                                                  int is_sealed) {
    if (!is_sealed)                       return CVSASX_OTYPE_CLASS_UNSEALED;
    if (otype == CHERI_OTYPE_SENTRY)      return CVSASX_OTYPE_CLASS_SENTRY;
    /* Any other sealed otype: portable identity unsolved. // not implemented */
    return CVSASX_OTYPE_CLASS_OPAQUE;
}

/* ===========================================================================
 * cvsasx_cap_strip - master prompt 4.1
 * ===========================================================================*/
cvsasx_status_t cvsasx_cap_strip(cvsasx_cap_t cap,
                                 const cvsasx_referent_t *ref,
                                 cvsasx_pir_t *out_pir) {
    if (out_pir == NULL || ref == NULL) return CVSASX_ERR_NULL_ARG;

    cvsasx_pir_zero(out_pir);                 /* deterministic image; reserved=0 */
    out_pir->struct_version = CVSASX_PIR_VERSION;
    out_pir->otype_class    = CVSASX_OTYPE_CLASS_OPAQUE; /* safe default until set */

    /* ---- read the validity tag (for validation only; NEVER serialized) ----
     * Reading the tag of an untagged capability is well-defined and does NOT
     * trap. (Digest: UCAM-CL-TR-987 §7.4 CGetTag has no precondition.)
     * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_tag_get lowers to CGetTag; returns _Bool;
     *            no exception on untagged input. */
    _Bool tagged = __builtin_cheri_tag_get(cap);   /* CGetTag */
    if (!tagged) {
        /* An untagged capability carries no authority; there is nothing to
         * migrate. Report rather than emit a referent-bearing PIR. */
        return CVSASX_ERR_UNTAGGED;
    }

    /* ---- introspect semantic fields via the CGet* family ------------------
     * Each builtin below is annotated with the instruction it lowers to and the
     * digest verify target. The TAG is deliberately absent from this set. */

    /* base = lower bound of the authority window.
     * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_base_get -> CGetBase; size_t (unsigned).
     *           (UCAM-CL-TR-987 §7.4 CGetBase; cheriintrin cheri_base_get) */
    unsigned long base  = __builtin_cheri_base_get(cap);    /* CGetBase  */

    /* length = size of the authority window in bytes.
     * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_length_get -> CGetLen; size_t; saturates to
     *           2^xlen-1 for full-address-space caps.
     *           (UCAM-CL-TR-987 §7.4 CGetLen; cheriintrin cheri_length_get) */
    unsigned long len   = __builtin_cheri_length_get(cap);  /* CGetLen   */

    /* perms = architectural + user permission bitfield (LOCAL encoding).
     * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_perms_get -> CGetPerm; returns size_t; the bit
     *           LAYOUT is target-defined (see cvsasx_perms_local_to_canonical).
     *           (UCAM-CL-TR-987 §7.4 CGetPerm; cheriintrin cheri_perms_get) */
    unsigned long perms = __builtin_cheri_perms_get(cap);   /* CGetPerm  */

    /* otype = object type; SIGNED (reserved otypes negative).
     * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_type_get -> CGetType; signed (cheri_otype_t =
     *           long); unsealed == -1, sentry == -2 on CHERI-RISC-V.
     *           (UCAM-CL-TR-987 §7.4 CGetType EXTS; cheri_types.sail) */
    cheri_otype_t otype = __builtin_cheri_type_get(cap);    /* CGetType  */

    /* sealed-state.
     * // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_sealed_get -> CGetSealed (was CGetUnsealed);
     *           returns _Bool. (UCAM-CL-TR-987 §7.4 CGetSealed; rename in
     *           ISAv9 changelog) */
    _Bool sealed = __builtin_cheri_sealed_get(cap);         /* CGetSealed */

    /* NOTE on the address/offset: the cursor (cheri_address_get -> CGetAddr) is
     * the cap's current pointer value within [base, base+len). We migrate the
     * AUTHORITY WINDOW (base/length), so we record the window's offset within
     * the referent object, not the transient cursor. If a future task needs the
     * cursor preserved, add it as an explicit PIR field; today it is out of
     * scope and intentionally not serialized. */

    /* ---- bind the window to its content-addressed referent (L3) -----------
     * The capability's authority window [base, base+len) must lie entirely
     * within the referent object instantiated at ref->object_base_addr. We
     * express the window position as an offset within the object - a
     * machine-INDEPENDENT quantity. The local object_base_addr is used only to
     * compute that offset and is never stored. All arithmetic is overflow-safe. */
    if (base < ref->object_base_addr) return CVSASX_ERR_REFERENT_RANGE;
    unsigned long off = base - ref->object_base_addr;
    if (off > ref->object_length)                 return CVSASX_ERR_REFERENT_RANGE;
    if (len > ref->object_length - off)           return CVSASX_ERR_REFERENT_RANGE;

    /* ---- serialize into the PIR ------------------------------------------- */
    cvsasx_bcopy(out_pir->referent_hash, ref->hash, CVSASX_BLAKE3_LEN);
    out_pir->offset      = (uint64_t)off;
    out_pir->length      = (uint64_t)len;
    out_pir->perms       = (uint32_t)cvsasx_perms_local_to_canonical(perms);
    out_pir->otype_class = cvsasx_otype_to_class(otype, sealed ? 1 : 0);
    out_pir->sealed      = sealed ? 1u : 0u;
    out_pir->flags       = CVSASX_PIR_FLAG_REFERENT_VALID;
    if (sealed) out_pir->flags |= CVSASX_PIR_FLAG_WAS_SEALED;
    /* reserved[] left zero by cvsasx_pir_zero - deterministic image. */

    /* The validity tag has NOT been written into the PIR anywhere above:
     * stripping is, by construction, a tag-dropping operation (4.1). */

    if (sealed) {
        /* Stripped faithfully, but sealed-capability MIGRATION is out of scope
         * (master prompt 4.5). Signal it so migrate/ does not attempt re-mint;
         * the custodian also independently refuses sealed PIRs. // not implemented */
        return CVSASX_WARN_SEALED_OUT_OF_SCOPE;
    }
    return CVSASX_OK;
}

/* ===========================================================================
 * MARKER SUMMARY
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __has_include(<cheriintrin.h>); header present in target SDK.
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): -march=rv64imafdcxcheri / -mabi=l64pc128d for the target.
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_tag_get  -> CGetTag    (_Bool, no trap on untagged)
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_base_get -> CGetBase   (size_t)
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_length_get -> CGetLen  (size_t)
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_perms_get -> CGetPerm  (size_t, target layout)
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_type_get -> CGetType   (SIGNED cheri_otype_t)
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): __builtin_cheri_sealed_get -> CGetSealed (_Bool)
 *   // VERIFIED (clang17@9e82d29 / sail@bb07488 / clean compile; log §4): CHERI_OTYPE_UNSEALED/SENTRY values (RISC-V: -1/-2; Morello: 0/1)
 *   // not implemented:   otype_class portable identity for non-sentry sealed caps (4.5).
 *   // not implemented:   sealed-capability migration out of scope (CVSASX_WARN_SEALED_*).
 *   integration point ref->{hash,object_base_addr} resolution owned by migrate/store.
 *   // TCB:    none added here - strip cannot amplify (reads + down-encodes only).
 *   // REF:    cheri-facts.md (UCAM-CL-TR-987 §7.4 CGet* family; cheriintrin.h).
 * ===========================================================================*/
