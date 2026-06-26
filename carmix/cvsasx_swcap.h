/* ===========================================================================
 * CV-SASX / CARMIX  -  carmix/cvsasx_swcap.h
 * The SOFTWARE (Wasm-SFI) capability backend: enforces the capability model on
 * COMMODITY hardware with NO CHERI. A software capability is the MSWasm HANDLE
 * model - {base, length, perms, valid} - unforgeable because it lives in TRUSTED
 * runtime state and the guest reaches an object only through the bounds-checked
 * backend (the SFI check), not via a raw handle. Drop-in for backend_cheri.c
 * behind the existing cap/ + gate/ + sls/ seams.
 *
 * KILL CRITERION: if anti-amplification (dest authority <= source) holds here,
 * enforced in software, the thesis survives the commodity port. The decision
 * logic is IDENTICAL to cap/cap_custodian.c (which was already pure software -
 * CHERI only provided derive+enforce); only introspect->struct-read and
 * derive->struct-construct change.
 *
 * Reuses cvsasx_cap.h (status enum, canonical perms, PIR, referent - all
 * CHERI-free). // not implemented: temporal safety (use-after-free) is sandbox-grade here,
 * weaker than CHERI silicon-grade (see VERIFICATION_LOG guarantee delta).
 * ===========================================================================*/
#ifndef CVSASX_SWCAP_H
#define CVSASX_SWCAP_H
#include <stdint.h>
#include <stddef.h>
#define CVSASX_CAP_PORTABLE 1   /* reuse cap/'s PORTABLE types on a non-CHERI target */
#include "cvsasx_cap.h"   /* cvsasx_status_t, CVSASX_PERM_*, cvsasx_pir_t, cvsasx_referent_t */

typedef struct cvsasx_swcap {
    uint64_t base; uint64_t length; uint32_t perms; uint8_t valid;
} cvsasx_swcap_t;

typedef struct cvsasx_sw_custodian { cvsasx_swcap_t root; uint8_t initialized; } cvsasx_sw_custodian_t;

typedef struct cvsasx_sw_region {
    cvsasx_swcap_t object_cap;
    uint64_t object_base_addr; uint64_t object_length;
    uint8_t  hash[CVSASX_BLAKE3_LEN];
} cvsasx_sw_region_t;

cvsasx_status_t cvsasx_sw_custodian_init(cvsasx_sw_custodian_t *c, cvsasx_swcap_t root);

/* STRIP a software handle -> PIR (same portable PIR format; tag/valid not carried). */
cvsasx_status_t cvsasx_sw_cap_strip(cvsasx_swcap_t cap, const cvsasx_referent_t *ref,
                                    cvsasx_pir_t *out_pir);

/* RE-MINT (THE KILL CRITERION): mint a fresh software handle whose authority is
 * provably <= the source's, enforced WITHOUT CHERI. Fail-closed: *out is invalid
 * on every reject (wider bounds, added perms, W^X, forbidden, malformed, sealed). */
cvsasx_status_t cvsasx_sw_cap_remint(const cvsasx_sw_custodian_t *cust, const cvsasx_pir_t *pir,
                                     const cvsasx_sw_region_t *region, cvsasx_swcap_t *out_cap);

/* The SFI enforcement: 1 = access in [offset,offset+size) with need_perms allowed;
 * 0 = denied. Software analogue of the CHERI hardware bounds/perm check. */
int cvsasx_swcap_check(const cvsasx_swcap_t *c, uint64_t offset, uint64_t size, uint32_t need_perms);

#endif
