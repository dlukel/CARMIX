/* ===========================================================================
 * CV-SASX  -  cap/cvsasx_cap.h
 * Public API of the cap/ module: capability type, status codes, the cross-module
 * referent/region contracts, the root-capability custodian, and the strip /
 * re-mint / permission-translation prototypes (master prompt Part 4).
 *
 * MODULE:        cap/ (Capability Rematerialization and Root Custodian)
 * DEPENDS ON LAWS:
 *   L1  Single address space - re-minted caps subdivide one static mapping.
 *   L2  Isolation by capability: hosted caps are RESTRICTED derivations, never
 *       root/almighty.
 *   L4  Monotonic, rooted provenance: every re-minted cap is derived (narrow-
 *       only) from the destination-local REGION capability, which is itself
 *       checked observably within the custodian root (perms-subset +
 *       bounds-within). INTENDED - UNVERIFIED that the region cap is genuinely
 *       root-derived: capability provenance is not architecturally queryable.
 *   L5  Safety established before execution: the custodian REJECTS (never
 *       clamps) any PIR that would amplify (4.4).
 *
 * TCB STATUS:    TCB-heavy. cvsasx_root_custodian holds the destination root
 *                capability; cvsasx_cap_remint() is the anti-amplification gate.
 *                strip() is integrity-relevant but CANNOT amplify (it only reads
 *                and down-encodes; it mints nothing).
 *
 * CROSS-MODULE SEAMS (other modules contact cap/ only through this header):
 *   - cvsasx_referent / cvsasx_region are produced by migrate/ + store/. cap/
 *     does NOT resolve local-address -> (object hash, offset); that mapping is
 *     migrate/'s responsibility. integration point
 *
 * Freestanding: requires a CHERI-LLVM pure-capability CHERI-RISC-V target.
 * ===========================================================================*/
#ifndef CVSASX_CAP_H
#define CVSASX_CAP_H

#include <stdint.h>
#include <stddef.h>
#include "cvsasx_pir.h"

/* This module is meaningless off a CHERI pure-capability target: every
 * primitive below is a capability operation. Fail honestly rather than compile
 * a stub that silently does nothing (master prompt Part 0 rule 4 / 6.6).
 * // VERIFIED (clang17@9e82d29 / clean compile; log §4): __CHERI_PURE_CAPABILITY__ is the CHERI-LLVM predefine for purecap
 *            mode; __CHERI__ is set in both hybrid and purecap. Confirm both
 *            against `clang -dM -E` on the target toolchain. */
/* The CHERI .c files (cap_strip.c, cap_custodian.c) require a CHERI purecap target.
 * The TYPES below (cvsasx_status_t, cvsasx_pir_t, cvsasx_referent_t, ...) are
 * PORTABLE - a software backend (CARMIX) reuses them with CVSASX_CAP_PORTABLE so
 * there is one source of truth for the status codes. The CHERI build is unchanged. */
#if !defined(__CHERI__) && !defined(CVSASX_CAP_PORTABLE)
#  error "cap/ requires a CHERI target (CHERI-LLVM), or define CVSASX_CAP_PORTABLE for portable types only."
#endif
#if !defined(__CHERI_PURE_CAPABILITY__) && !defined(CVSASX_CAP_PORTABLE)
#  error "cap/ requires the PURE-CAPABILITY ABI (-mabi=l64pc128*). See README."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Capability handle.
 * In the pure-capability ABI every pointer IS a 128-bit CHERI capability with
 * an out-of-band validity tag; `void *` is the natural opaque carrier and the
 * CHERI introspection/derivation builtins accept it directly.
 * // VERIFIED (clang17@9e82d29 / clean compile; log §4): in purecap, sizeof(void*)==16 and it is a tagged capability; the
 *            __builtin_cheri_* family accepts `void *` operands. (cheriintrin.h)
 * ------------------------------------------------------------------------- */
typedef void *cvsasx_cap_t;

/* ---------------------------------------------------------------------------
 * Status codes. The custodian REJECTS rather than clamps (master prompt 4.4),
 * so every refusal has a distinct, greppable cause.
 * ------------------------------------------------------------------------- */
typedef enum cvsasx_status {
    CVSASX_OK = 0,

    /* generic */
    CVSASX_ERR_NULL_ARG,
    CVSASX_ERR_VERSION,             /* PIR struct_version mismatch */

    /* strip-side */
    CVSASX_ERR_UNTAGGED,            /* source capability has no valid tag */
    CVSASX_ERR_REFERENT_RANGE,      /* cap [base,base+len) not inside referent object */
    CVSASX_WARN_SEALED_OUT_OF_SCOPE,/* stripped OK but source sealed - see 4.5 */

    /* custodian / re-mint anti-amplification (all are isolation-critical) */
    CVSASX_ERR_BAD_ROOT,            /* custodian root untagged/uninitialized */
    CVSASX_ERR_BAD_REGION,          /* region cap invalid or exceeds root */
    CVSASX_ERR_REFERENT_MISMATCH,   /* PIR hash != instantiated object hash */
    CVSASX_ERR_BAD_BOUNDS,          /* offset/length zero/overflow/outside object */
    CVSASX_ERR_PERM_FORBIDDEN,      /* PIR requests privileged/unmodelled perms */
    CVSASX_ERR_WX_VIOLATION,        /* PIR requests EXECUTE together with STORE/STORE_CAP (W^X) */
    CVSASX_ERR_AMPLIFY_PERMS,       /* requested perms exceed the destination region capability */
    CVSASX_ERR_SEALED_UNSUPPORTED,  /* re-mint of sealed cap not supported (4.5) */

    /* POST-MINT verification (defense-in-depth against rounding amplification) */
    CVSASX_ERR_DERIVE_UNTAGGED,     /* derivation cleared the tag (e.g. inexact bounds) */
    CVSASX_ERR_VERIFY_BASE,         /* minted base != requested */
    CVSASX_ERR_VERIFY_LENGTH,       /* minted length != requested (rounding!) */
    CVSASX_ERR_VERIFY_PERMS,        /* minted perms != requested set */
    CVSASX_ERR_VERIFY_SEALED        /* minted cap unexpectedly sealed */
} cvsasx_status_t;

/* ---------------------------------------------------------------------------
 * cvsasx_referent - strip-side input (produced by migrate/store).
 * Describes WHICH content-addressed object the capability points into and where
 * that object is CURRENTLY instantiated in this machine's single address space
 * (L1). `object_base_addr` is a LOCAL address used only transiently to compute
 * the machine-independent offset; it is never stored in the PIR.
 * integration point resolving a local cap base -> (hash, object_base_addr) is the
 *              job of migrate/ + store/, not cap/.
 * ------------------------------------------------------------------------- */
typedef struct cvsasx_referent {
    uint8_t  hash[CVSASX_BLAKE3_LEN]; /* content address of the referent object */
    uint64_t object_base_addr;        /* LOCAL vaddr where the object currently sits */
    uint64_t object_length;           /* object size in bytes */
} cvsasx_referent_t;

/* ---------------------------------------------------------------------------
 * cvsasx_region - re-mint-side input (produced by migrate/store).
 * The destination-local instantiation of a referent object: a bounding
 * capability `object_cap` (INTENDED to be a monotone descendant of the
 * custodian root, obtained from store/ when the object was faulted in - see the
 * provenance caveat on L4), the local base address, the length, and the hash
 * this region is claimed to hold. The custodian re-derives the task capability
 * FROM object_cap. Narrowing RELATIVE TO object_cap is architecturally
 * structural (CHERI monotonicity of CSetAddr/CSetBoundsExact/CAndPerm); but
 * binding object_cap to root, and the canonical->local permission translation,
 * are POLICY checks enforced by software (cvsasx_check_region,
 * cvsasx_perms_canonical_to_local), not by the architecture.
 * ------------------------------------------------------------------------- */
typedef struct cvsasx_region {
    cvsasx_cap_t object_cap;          /* bounded cap over the instantiated object */
    uint64_t     object_base_addr;    /* LOCAL base vaddr of the object */
    uint64_t     object_length;       /* object size in bytes */
    uint8_t      hash[CVSASX_BLAKE3_LEN]; /* content address claimed for this region */
} cvsasx_region_t;

/* ---------------------------------------------------------------------------
 * The root-capability custodian (master prompt L4 / 4.3).
 * // TCB: the ONLY component that holds broad (destination root) authority and
 *         mints restricted capabilities for tasks. Keep instances minimal and
 *         visible; reviewers count these.
 * `root` must be a tagged capability spanning the address region from which all
 * task object regions are sub-derived (typically the boot data root, see boot/).
 * ------------------------------------------------------------------------- */
typedef struct cvsasx_root_custodian {
    cvsasx_cap_t root;        /* destination root (broad) capability - TCB */
    uint8_t      initialized; /* 0 until cvsasx_custodian_init succeeds */
} cvsasx_root_custodian_t;

/* ===========================================================================
 * API
 * ===========================================================================*/

/* Initialize the custodian with the destination root capability.
 * // TCB: establishes the single broad-authority holder. `root` is validated to
 *         be tagged; its breadth is the caller's (boot/) responsibility. */
cvsasx_status_t cvsasx_custodian_init(cvsasx_root_custodian_t *cust,
                                      cvsasx_cap_t root);

/* STRIP (master prompt 4.1): decompose a live capability into a PIR. Reads
 * fields via the CGet* introspection family, DROPS the tag, records the
 * referent as (hash, offset). Never mints anything. */
cvsasx_status_t cvsasx_cap_strip(cvsasx_cap_t cap,
                                 const cvsasx_referent_t *ref,
                                 cvsasx_pir_t *out_pir);

/* RE-MINT (master prompt 4.3/4.4): from a validated PIR and a destination-local
 * region, derive a fresh, locally-valid capability monotonically from the
 * region cap, then VERIFY the result against the request. REJECTS on any
 * amplification. On success *out_cap is tagged and exactly bounded; on any
 * error *out_cap is set to a null (untagged) capability (fail-closed).
 * // TCB: anti-amplification gate. */
cvsasx_status_t cvsasx_cap_remint(const cvsasx_root_custodian_t *cust,
                                  const cvsasx_pir_t *pir,
                                  const cvsasx_region_t *region,
                                  cvsasx_cap_t *out_cap);

/* Permission translation between the CV-SASX canonical encoding and the
 * target-local CHERI permission bits. Defined in cap_custodian.c.
 * canonical_to_local sets *err = CVSASX_ERR_PERM_FORBIDDEN and returns 0 if any
 * privileged/unmodelled canonical bit is present. */
cvsasx_perm_t cvsasx_perms_local_to_canonical(unsigned long local_perms);
unsigned long cvsasx_perms_canonical_to_local(cvsasx_perm_t canon,
                                              cvsasx_status_t *err);

/* Optional instruction-level derivation reference (cap_remint.S). Selected only
 * when CVSASX_USE_ASM_DERIVE is defined AND the asm has been verified
 * (CVSASX_ASM_DERIVE_VERIFIED); otherwise the trapping stub fails closed. The
 * default build uses the C-builtin path in cap_custodian.c. See cap_remint.S.
 * // VERIFIED (clang17@9e82d29 / clean compile; log §4): signature/ABI must match the asm definition before enabling. */
cvsasx_cap_t cvsasx_cap_derive_from_root_asm(cvsasx_cap_t source,
                                             unsigned long base,
                                             unsigned long length,
                                             unsigned long perm_mask);

#ifdef __cplusplus
}
#endif

/* ===========================================================================
 * MARKER SUMMARY
 *   // VERIFIED (clang17@9e82d29 / clean compile; log §4): __CHERI__ / __CHERI_PURE_CAPABILITY__ predefines; purecap void*
 *             is a tagged 16-byte capability accepted by __builtin_cheri_*.
 *   integration point local-address -> (object hash, offset) resolution lives in
 *             migrate/ + store/, not cap/.
 *   // not implemented:   sealed-capability re-mint unsupported (CVSASX_ERR_SEALED_*) (4.5).
 *   // TCB:    cvsasx_root_custodian_t (broad authority); cvsasx_cap_remint
 *             (anti-amplification gate); cvsasx_custodian_init.
 *   // REF:    cheri-facts.md digest (UCAM-CL-TR-987 / sail-cheri-riscv /
 *             cheriintrin.h).
 * ===========================================================================*/
#endif /* CVSASX_CAP_H */
