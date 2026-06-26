/* ===========================================================================
 * CV-SASX  -  gate/backend_cheri.c
 * The CHERI enforcement backend: the first concrete implementation of the
 * cvsasx_gate_backend_t // SEAM. This is the ONLY gate/ file that references a
 * CHERI builtin - validation, the checker, and the executor are backend-agnostic
 * (so an MTE/PAC or software-SFI backend can replace this without touching them).
 *
 * Linear memory = a single CHERI capability, CSetBounds to the EXACT linear-
 * memory extent, permissions load/store only (no execute) - Master Prompt 2.2.
 * On an out-of-bounds access the capability TRAPS: the run-time backstop. There
 * is deliberately NO explicit per-access software bounds check here - CHERI
 * subsumes it (the design-intent parallels Cage on Arm MTE). A software-SFI
 * backend would instead mask the address; that difference lives behind this seam.
 * ===========================================================================*/
#include "cvsasx_gate.h"
#include <cheriintrin.h>

static struct cheri_state {
    cvsasx_gate_backend_t base;
    volatile unsigned char *linmem;   /* cap bounded to [mem, mem+mem_size) */
    uint32_t mem_size;
} g_cheri;

static void cheri_mem_bind(cvsasx_gate_backend_t *self, void *mem, uint32_t mem_size) {
    (void)self;
    void *c = mem;
    c = cheri_bounds_set(c, mem_size);                       /* exact linear-memory extent */
    c = cheri_perms_and(c, (unsigned long)(CHERI_PERM_LOAD |
                                           CHERI_PERM_STORE |
                                           CHERI_PERM_GLOBAL)); /* load/store, NO execute */
    g_cheri.linmem = (volatile unsigned char *)c;
    g_cheri.mem_size = mem_size;
}

/* Bounded i32 load/store. CHERI checks every access against the capability's
 * bounds; an out-of-range addr (or addr+4 past the end) TRAPS. No software
 * bounds check - CHERI is the enforcement (and the independent run-time layer). */
static int32_t cheri_load32(cvsasx_gate_backend_t *self, uint32_t addr) {
    (void)self;
    volatile int32_t *p = (volatile int32_t *)(g_cheri.linmem + addr);
    return *p;
}
static void cheri_store32(cvsasx_gate_backend_t *self, uint32_t addr, int32_t val) {
    (void)self;
    volatile int32_t *p = (volatile int32_t *)(g_cheri.linmem + addr);
    *p = val;
}

cvsasx_gate_backend_t *cvsasx_gate_backend_cheri(void) {
    g_cheri.base.name = "cheri";
    g_cheri.base.mem_bind = cheri_mem_bind;
    g_cheri.base.load32 = cheri_load32;
    g_cheri.base.store32 = cheri_store32;
    /* On CHERI, "raw" is the same bounded-cap access - the hardware enforces bounds
     * regardless, so an elided access is still safe (defense in depth) and free. */
    g_cheri.base.load32_raw = cheri_load32;
    g_cheri.base.store32_raw = cheri_store32;
    return &g_cheri.base;
}

/* Expose the minted linear-memory capability so cap/ can strip it to a PIR
 * (gate test G5: the gate produces capabilities cap/ can rematerialize). */
void *cvsasx_gate_backend_cheri_linmem(void) { return (void *)g_cheri.linmem; }

/* ===========================================================================
 * MARKER SUMMARY
 *   // SEAM: the only gate/ file referencing CHERI builtins (the enforcement).
 *   // VERIFIED (cap/ §): cheri_bounds_set/perms_and lower to CSetBounds/CAndPerm.
 *   // TCB: the backend mints the bounded linmem cap; a wrong bound here is an
 *          isolation hole (proven correct by gate test G1 - OOB traps).
 *   // not implemented: MTE/PAC + software-SFI backends implement the same seam (not built).
 * ===========================================================================*/
