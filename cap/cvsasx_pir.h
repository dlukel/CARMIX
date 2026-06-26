/* ===========================================================================
 * CV-SASX  -  cap/cvsasx_pir.h
 * Position-Independent Reference (PIR): the machine-INDEPENDENT description of a
 * CHERI capability, used by the Cross-Machine Capability Rematerialization
 * Protocol (master prompt Part 4).
 *
 * MODULE:        cap/ (Capability Rematerialization and Root Custodian)
 * DEPENDS ON LAWS:
 *   L3  Content-addressed, immutable referents (no paths): a capability's
 *       referent is named by (BLAKE3 object hash + offset), never a local vaddr.
 *   L4  Capability provenance is monotonic and rooted: the PIR carries only
 *       narrowable rights (bounds/perms). The re-mint custodian REJECTS any PIR
 *       observably broader than the destination region capability, and checks
 *       that region cap observably within root (perms-subset + bounds-within).
 *       INTENDED - UNVERIFIED that the region/root chain reflects true lineage:
 *       capability provenance is not architecturally queryable (cap_custodian.c).
 *
 * TCB STATUS:    TCB. The PIR is the cross-machine capability description; its
 *                integrity is security-critical. A forged/larger PIR that is
 *                re-minted without rejection is a full isolation bypass, so the
 *                re-mint path (cap_custodian.c) MUST validate every field.
 *                This header itself defines only data layout + constants.
 *
 * KNOWN OPEN POINTS (master prompt 4.5 - stubbed, NOT fabricated):
 *   - otype_class: a PORTABLE class identity for sealed capabilities. The map
 *     from a portable class to a destination-local otype is UNSOLVED. See the
 *     CVSASX_OTYPE_CLASS_* block below. not implemented
 *   - PIR canonical on-wire serialization (endianness/packing) is owned by
 *     migrate/, not cap/. This struct is the in-memory form only. integration point
 *
 * Freestanding: uses only <stdint.h> (a freestanding-mandated C header).
 * No libc, no hosted assumptions.
 * ===========================================================================*/
#ifndef CVSASX_CAP_PIR_H
#define CVSASX_CAP_PIR_H

#include <stdint.h>   /* freestanding header: exact-width integer types only */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Content-address length.
 * REF: BLAKE3 default extended-output / Merkle-root digest length is 32 bytes
 *      (256 bits). The object store (store/) is content-addressed by BLAKE3.
 * VERIFIED (store/ Phase 2): store/cvsasx_store.h CVSASX_HASH_LEN == 32 (BLAKE3-256
 *         default digest), confirmed against the OFFICIAL BLAKE3 vectors on
 *         QEMU-CHERI (store/ test S0) and the cap/<->store/ seam (S5). Same width.
 * ------------------------------------------------------------------------- */
#define CVSASX_BLAKE3_LEN  32u

/* PIR binary layout version. The PIR set is committed to the content-addressed
 * object store (master prompt 4.2), so its layout is part of the migrated
 * state's hash; bump this on ANY field change so old and new layouts hash
 * distinctly and cannot be silently confused. */
#define CVSASX_PIR_VERSION  1u

/* ---------------------------------------------------------------------------
 * CV-SASX CANONICAL, PORTABLE permission encoding.
 *
 * This is CV-SASX's OWN frozen bit assignment - it is deliberately NOT the
 * local CHERI-RISC-V CGetPerm bit layout. Rationale (grounded in the ISA
 * research digest):
 *   - cheriintrin.h CHERI_PERM_* are TARGET-DEFINED symbolic values, not fixed
 *     numbers; their numeric bit positions differ across CHERI-RISC-V-128,
 *     Morello, and CHERIoT. (Digest: "PORTABILITY MANDATE: ... never persist
 *     raw target bits.")
 *   - Therefore the PIR persists a canonical set, and cap_custodian.c
 *     translates canonical <-> local at the strip / re-mint boundary.
 * These constants are an arbitrary-but-stable namespace local to CV-SASX. Do
 * NOT assume CVSASX_PERM_x == any CHERI_PERM_x bit position.
 * ------------------------------------------------------------------------- */
typedef uint32_t cvsasx_perm_t;

/* Data-plane permissions (the in-scope set for unsealed data-capability
 * migration, master prompt 4.5 "Handle unsealed data capabilities first"). */
#define CVSASX_PERM_GLOBAL           ((cvsasx_perm_t)1u << 0)
#define CVSASX_PERM_EXECUTE          ((cvsasx_perm_t)1u << 1)
#define CVSASX_PERM_LOAD             ((cvsasx_perm_t)1u << 2)
#define CVSASX_PERM_STORE            ((cvsasx_perm_t)1u << 3)
#define CVSASX_PERM_LOAD_CAP         ((cvsasx_perm_t)1u << 4)
#define CVSASX_PERM_STORE_CAP        ((cvsasx_perm_t)1u << 5)
#define CVSASX_PERM_STORE_LOCAL_CAP  ((cvsasx_perm_t)1u << 6)

/* Privileged / sealing permissions. A hosted task's re-minted data capability
 * MUST NOT carry these (master prompt L2: hosted tasks run under RESTRICTED
 * capabilities; L4: only the boot custodian holds broad authority). They are
 * defined so that strip() can FAITHFULLY record what a source capability held;
 * the custodian's anti-amplification policy (4.4) REJECTS any PIR that carries
 * them for a hosted re-mint rather than silently clearing them. */
#define CVSASX_PERM_SEAL             ((cvsasx_perm_t)1u << 7)
#define CVSASX_PERM_UNSEAL           ((cvsasx_perm_t)1u << 8)
#define CVSASX_PERM_INVOKE           ((cvsasx_perm_t)1u << 9)   /* a.k.a. CInvoke/CCall */
#define CVSASX_PERM_ACCESS_SYS_REGS  ((cvsasx_perm_t)1u << 10)  /* ASR */
#define CVSASX_PERM_SET_CID          ((cvsasx_perm_t)1u << 11)

/* Sentinel: the source capability held one or more architectural permission
 * bits that CV-SASX does not model in the map above. strip() sets this so the
 * fact is never silently dropped; the custodian treats it as fail-closed
 * (reject). */
#define CVSASX_PERM_UNMODELLED       ((cvsasx_perm_t)1u << 31)

/* Mask of every NON-privileged permission a hosted capability may request at
 * re-mint: the data-plane perms PLUS EXECUTE (hosted execution graphs need a
 * bounded executable code capability). Anything outside this (the privileged /
 * sealing perms below, or CVSASX_PERM_UNMODELLED) is rejected by the custodian.
 *
 * EXECUTE is admitted here ONLY under a W^X invariant enforced at re-mint, NOT
 * granted freely:
 * // INV (W^X): cvsasx_perms_canonical_to_local REJECTS any request that
 *    combines EXECUTE with STORE or STORE_CAP (-> CVSASX_ERR_WX_VIOLATION), so a
 *    single re-minted capability is never simultaneously writable and
 *    executable. (master prompt L2: hosted tasks run under RESTRICTED caps.)
 *    This is a software invariant - CHERI does not architecturally forbid W&X
 *    on one capability, so the named check is load-bearing. */
#define CVSASX_PERM_HOSTED_MASK \
    ( CVSASX_PERM_GLOBAL | CVSASX_PERM_EXECUTE | CVSASX_PERM_LOAD |  \
      CVSASX_PERM_STORE  | CVSASX_PERM_LOAD_CAP | CVSASX_PERM_STORE_CAP | \
      CVSASX_PERM_STORE_LOCAL_CAP )

/* Any store authority - the set EXECUTE must not be combined with (W^X). */
#define CVSASX_PERM_WRITE_ANY  ( CVSASX_PERM_STORE | CVSASX_PERM_STORE_CAP )

/* ---------------------------------------------------------------------------
 * Portable otype class identity.
 *
 * // not implemented (master prompt 4.5 - "otype reconciliation across machines"):
 * Local otype values are namespace-local (digest: "Local otype values are
 * namespace-local and have NO portable cross-machine meaning"). The mapping
 * from a portable class id to a destination-local otype, and the trust model
 * for that mapping, is an UNSOLVED research design point. The constants below
 * are placeholders so the data path is wired and honest, NOT a solution:
 *   - UNSEALED / SENTRY are the two architecturally well-defined cases we can
 *     classify portably (a sentry is recognizable; an unsealed cap has no
 *     otype). Everything else collapses to OPAQUE and is currently out of
 *     scope for re-mint.
 * ------------------------------------------------------------------------- */
typedef uint32_t cvsasx_otype_class_t;
#define CVSASX_OTYPE_CLASS_UNSEALED  ((cvsasx_otype_class_t)0u)
#define CVSASX_OTYPE_CLASS_SENTRY    ((cvsasx_otype_class_t)1u)
/* OPAQUE: a sealed, non-sentry capability whose portable class identity is
 * UNRESOLVED. Recording it preserves honesty; re-mint of such a PIR is not
 * supported (sealed-capability migration is out of scope, 4.5). // not implemented */
#define CVSASX_OTYPE_CLASS_OPAQUE    ((cvsasx_otype_class_t)0xFFFFFFFFu)

/* PIR.flags bits. */
#define CVSASX_PIR_FLAG_REFERENT_VALID  ((uint8_t)1u << 0) /* referent_hash/offset/length filled */
#define CVSASX_PIR_FLAG_WAS_SEALED       ((uint8_t)1u << 1) /* source was sealed (see otype_class) */

/* ---------------------------------------------------------------------------
 * The Position-Independent Reference.
 *
 * Field order is chosen for natural alignment with NO internal padding on an
 * LP64/purecap target (total 64 bytes): hash[32]@0, offset@32, length@40,
 * struct_version@48, perms@52, otype_class@56, sealed@60, flags@61,
 * reserved@62. `reserved` MUST be zero so the in-memory image is deterministic.
 *
 * // TCB: this struct is the cross-machine capability description.
 * NOTE: NO validity tag is present - the tag is intentionally never serialized
 * (master prompt 4.1). A PIR can therefore never, by construction, carry a
 * forgeable tag across machines. This does NOT make the PIR "safe by
 * construction": the bounds/perms/offset/referent fields ARE attacker-
 * influenceable, and their integrity against a malicious source is an OPEN
 * trust-model question (see cvsasx_cap_remint not implemented). The re-mint validator,
 * not this struct, is the enforcement point.
 *
 * // VERIFIED (clean compile, clang17@9e82d29; log §4): on the target purecap ABI sizeof(cvsasx_pir_t)==64 and
 *            no field straddles its natural alignment (static_assert below).
 * integration point canonical byte serialization (fixed little-endian, zeroed
 *            reserved) for content-addressing the PIR is migrate/'s job; this
 *            in-memory layout is NOT guaranteed identical across compilers.
 * ------------------------------------------------------------------------- */
typedef struct cvsasx_pir {
    uint8_t  referent_hash[CVSASX_BLAKE3_LEN]; /* content address of referent object (L3) */
    uint64_t offset;        /* offset of capability base within the referent object */
    uint64_t length;        /* capability bounds length in bytes (L4: narrow-only) */
    uint32_t struct_version;/* == CVSASX_PIR_VERSION */
    uint32_t perms;         /* cvsasx_perm_t: CANONICAL perms, never raw local CHERI bits */
    uint32_t otype_class;   /* cvsasx_otype_class_t - PORTABLE class id (4.5 OPEN) */
    uint8_t  sealed;        /* 0/1: source sealed-state (mirror of WAS_SEALED flag) */
    uint8_t  flags;         /* CVSASX_PIR_FLAG_* */
    uint8_t  reserved[2];   /* MUST be zero (deterministic image) */
} cvsasx_pir_t;

/* Compile-time layout guard. _Static_assert is C11 freestanding-safe. */
_Static_assert(sizeof(cvsasx_pir_t) == 64,
    "cvsasx_pir_t must be exactly 64 bytes; check target ABI padding"); /* VERIFIED: passes @9e82d29 */

#ifdef __cplusplus
}
#endif

/* ===========================================================================
 * MARKER SUMMARY (grep targets for the integrator)
 *   // VERIFIED (store/ Phase 2): 32B == store/ CVSASX_HASH_LEN (S0 vectors + S5 seam).
 *   // VERIFIED (predefines GLOBAL=1..ASR=1024 @9e82d29; §4): CHERI_PERM_* numeric values are target-defined - canonical
 *             encoding here is intentionally independent; translation lives in
 *             cap_custodian.c.
 *   // VERIFIED (§4): sizeof(cvsasx_pir_t)==64 on the target purecap ABI.
 *   // INV:    W^X - EXECUTE is never co-granted with STORE/STORE_CAP; enforced
 *             at re-mint in cvsasx_perms_canonical_to_local.
 *   // not implemented:   otype_class -> destination-local otype reconciliation (4.5).
 *   // not implemented:   re-mint of sealed/opaque capabilities is out of scope (4.5).
 *   integration point canonical PIR wire serialization owned by migrate/.
 *   // TCB:    the PIR layout and constants - integrity-critical input to
 *             re-mint; tag is NEVER a field (never serialized).
 *   // REF:    BLAKE3 (32-byte digest); CHERI ISA digest in scratchpad
 *             cheri-facts.md (UCAM-CL-TR-987 / sail-cheri-riscv / cheriintrin.h).
 * ===========================================================================*/
#endif /* CVSASX_CAP_PIR_H */
