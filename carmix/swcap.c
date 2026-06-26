/* ===========================================================================
 * CV-SASX / CARMIX  -  carmix/swcap.c
 * The software anti-amplification gate. Ports cap/cap_custodian.c's re-mint
 * DECISIONS verbatim (they were always pure software) to software handles;
 * derive = construct {base,length,perms,valid}; enforce = cvsasx_swcap_check.
 * NO CHERI. Perms are canonical (CVSASX_PERM_*) on both sides - no CHERI-bit
 * translation, so the canonical anti-amplification logic is exactly as in CHERI.
 * ===========================================================================*/
#include "cvsasx_swcap.h"

/* Canonical bits a hosted, unsealed data capability may NEVER request (= cap_custodian.c). */
#define SW_FORBIDDEN_MASK ( CVSASX_PERM_SEAL | CVSASX_PERM_UNSEAL | CVSASX_PERM_INVOKE | \
                            CVSASX_PERM_ACCESS_SYS_REGS | CVSASX_PERM_SET_CID | CVSASX_PERM_UNMODELLED )

static int hash_eq(const uint8_t *a, const uint8_t *b){
    unsigned d = 0; for (size_t i = 0; i < CVSASX_BLAKE3_LEN; ++i) d |= (unsigned)(a[i]^b[i]); return d == 0;
}

cvsasx_status_t cvsasx_sw_custodian_init(cvsasx_sw_custodian_t *c, cvsasx_swcap_t root){
    if (c == NULL) return CVSASX_ERR_NULL_ARG;
    c->root.valid = 0; c->initialized = 0;
    if (!root.valid) return CVSASX_ERR_BAD_ROOT;     /* `valid` is the software tag */
    c->root = root; c->initialized = 1; return CVSASX_OK;
}

cvsasx_status_t cvsasx_sw_cap_strip(cvsasx_swcap_t cap, const cvsasx_referent_t *ref,
                                    cvsasx_pir_t *out_pir){
    if (out_pir == NULL || ref == NULL) return CVSASX_ERR_NULL_ARG;
    for (size_t i = 0; i < sizeof *out_pir; ++i) ((uint8_t*)out_pir)[i] = 0;
    out_pir->struct_version = CVSASX_PIR_VERSION;
    out_pir->otype_class    = CVSASX_OTYPE_CLASS_OPAQUE;
    if (!cap.valid) return CVSASX_ERR_UNTAGGED;
    if (cap.base < ref->object_base_addr) return CVSASX_ERR_REFERENT_RANGE;
    uint64_t off = cap.base - ref->object_base_addr;
    if (off > ref->object_length)              return CVSASX_ERR_REFERENT_RANGE;
    if (cap.length > ref->object_length - off) return CVSASX_ERR_REFERENT_RANGE;
    for (size_t i = 0; i < CVSASX_BLAKE3_LEN; ++i) out_pir->referent_hash[i] = ref->hash[i];
    out_pir->offset = off; out_pir->length = cap.length; out_pir->perms = cap.perms;  /* perms already canonical */
    out_pir->otype_class = CVSASX_OTYPE_CLASS_UNSEALED; out_pir->sealed = 0;
    out_pir->flags = CVSASX_PIR_FLAG_REFERENT_VALID;
    return CVSASX_OK;
}

static cvsasx_status_t sw_check_region(const cvsasx_sw_custodian_t *cust, const cvsasx_sw_region_t *r){
    if (!r->object_cap.valid) return CVSASX_ERR_BAD_REGION;
    if (r->object_cap.perms & ~cust->root.perms) return CVSASX_ERR_BAD_REGION;   /* perms subset of root */
    if (r->object_cap.base < cust->root.base)    return CVSASX_ERR_BAD_REGION;
    uint64_t off = r->object_cap.base - cust->root.base;                          /* within root (overflow-safe) */
    if (off > cust->root.length)                         return CVSASX_ERR_BAD_REGION;
    if (r->object_cap.length > cust->root.length - off)  return CVSASX_ERR_BAD_REGION;
    if (r->object_cap.base   != r->object_base_addr) return CVSASX_ERR_BAD_REGION; /* exactly bounds the object */
    if (r->object_cap.length != r->object_length)    return CVSASX_ERR_BAD_REGION;
    return CVSASX_OK;
}

cvsasx_status_t cvsasx_sw_cap_remint(const cvsasx_sw_custodian_t *cust, const cvsasx_pir_t *pir,
                                     const cvsasx_sw_region_t *region, cvsasx_swcap_t *out_cap){
    if (out_cap == NULL) return CVSASX_ERR_NULL_ARG;
    out_cap->valid = 0; out_cap->base = 0; out_cap->length = 0; out_cap->perms = 0;   /* fail-closed */
    if (cust == NULL || pir == NULL || region == NULL) return CVSASX_ERR_NULL_ARG;
    if (!cust->initialized || !cust->root.valid) return CVSASX_ERR_BAD_ROOT;
    if (pir->struct_version != CVSASX_PIR_VERSION) return CVSASX_ERR_VERSION;
    if (pir->sealed || (pir->flags & CVSASX_PIR_FLAG_WAS_SEALED) ||
        pir->otype_class != CVSASX_OTYPE_CLASS_UNSEALED) return CVSASX_ERR_SEALED_UNSUPPORTED;
    if (!(pir->flags & CVSASX_PIR_FLAG_REFERENT_VALID)) return CVSASX_ERR_REFERENT_MISMATCH;
    if (!hash_eq(pir->referent_hash, region->hash))     return CVSASX_ERR_REFERENT_MISMATCH;
    { cvsasx_status_t rs = sw_check_region(cust, region); if (rs != CVSASX_OK) return rs; }

    /* bounds anti-amplification (overflow-safe; zero-length rejected) */
    if (pir->length == 0)                                  return CVSASX_ERR_BAD_BOUNDS;
    if (pir->offset > region->object_length)               return CVSASX_ERR_BAD_BOUNDS;
    if (pir->length > region->object_length - pir->offset) return CVSASX_ERR_BAD_BOUNDS;

    /* permission anti-amplification (canonical policy + subset of region) */
    if (pir->perms & SW_FORBIDDEN_MASK) return CVSASX_ERR_PERM_FORBIDDEN;
    if ((pir->perms & CVSASX_PERM_EXECUTE) && (pir->perms & CVSASX_PERM_WRITE_ANY))
        return CVSASX_ERR_WX_VIOLATION;
    if (pir->perms & ~region->object_cap.perms) return CVSASX_ERR_AMPLIFY_PERMS;

    /* DERIVE (software: construct EXACTLY - no compression, so no rounding hazard) */
    cvsasx_swcap_t c;
    c.base   = region->object_cap.base + (uint64_t)pir->offset;
    c.length = (uint64_t)pir->length;
    c.perms  = (uint32_t)pir->perms;
    c.valid  = 1;

    /* POST-MINT VERIFICATION (defense in depth; the CHERI compression-rounding
     * vector does NOT exist in software, so these are exact - reject on any drift). */
    if (!c.valid)                                            return CVSASX_ERR_DERIVE_UNTAGGED;
    if (c.base   != region->object_cap.base + pir->offset)  return CVSASX_ERR_VERIFY_BASE;
    if (c.length != pir->length)                            return CVSASX_ERR_VERIFY_LENGTH;
    if (c.perms  != pir->perms)                             return CVSASX_ERR_VERIFY_PERMS;

    *out_cap = c;
    return CVSASX_OK;
}

int cvsasx_swcap_check(const cvsasx_swcap_t *c, uint64_t offset, uint64_t size, uint32_t need_perms){
    if (c == NULL || !c->valid) return 0;
    if ((c->perms & need_perms) != need_perms) return 0;          /* permission */
    if (offset > c->length) return 0;
    if (size > c->length - offset) return 0;                      /* bounds (overflow-safe) */
    return 1;
}
