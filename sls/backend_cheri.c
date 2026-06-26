/* ===========================================================================
 * CV-SASX  -  sls/backend_cheri.c
 * The CHERI object-capability backend: the first concrete implementation of the
 * cvsasx_sls_backend // SEAM, and the ONLY sls/ file that references a CHERI
 * builtin. The store/Merkle/diff spine (sls.c) is backend-independent, so a
 * software/Wasm backend can replace this on commodity x86-64/ARM without touching
 * the spine (// not implemented: the CARMIX port).
 *
 * Objects are IMMUTABLE, so an object grant is a LOAD-ONLY bounded capability:
 * CSetBounds to the object extent, CAndPerm to load|global (no store, no execute).
 * ===========================================================================*/
#include "cvsasx_sls.h"
#include <cheriintrin.h>

static void *cheri_obj_grant(cvsasx_sls_backend_t *self, void *bytes, uint32_t len){
    (void)self;
    void *c = bytes;
    c = cheri_bounds_set(c, len);                            /* exact object extent     */
    c = cheri_perms_and(c, (unsigned long)(CHERI_PERM_LOAD | CHERI_PERM_GLOBAL)); /* read-only */
    return c;                                                /* OOB / write traps (capmode) */
}

static cvsasx_sls_backend_t g_cheri = { "cheri", cheri_obj_grant };

cvsasx_sls_backend_t *cvsasx_sls_backend_cheri(void){ return &g_cheri; }

/* ===========================================================================
 *   // SEAM: only sls/ file touching CHERI builtins (the enforcement).
 *   // not implemented: software/Wasm backend for commodity hardware = the CARMIX port (not built).
 * ===========================================================================*/
