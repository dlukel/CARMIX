/* ===========================================================================
 * CV-SASX / CARMIX  -  carmix/backend_sw_gate.c
 * Software gate backend (no CHERI). Drop-in cvsasx_gate_backend_t.
 *   load32/store32     : checked - BOUNDS-ONLY (Part C: the linmem cap's valid +
 *                        LOAD|STORE perms are a MODULE CONSTANT, hoisted to bind, so
 *                        the per-access hot path is just the bounds compare).
 *   load32_raw/_raw    : ELIDED - used only for accesses the optimizer PROVED safe.
 * On OOB, load32/store32 set a fault flag (the runtime would abort the guest) - the
 * software analogue of the CHERI trap. gate/ is UNCHANGED.
 * ===========================================================================*/
#include "cvsasx_gate.h"
#include "cvsasx_swcap.h"

static struct { cvsasx_gate_backend_t base; uint8_t *mem; uint32_t size; int fault; } g;

static void sw_bind(cvsasx_gate_backend_t *self, void *mem, uint32_t size){
    (void)self; g.mem = (uint8_t*)mem; g.size = size; g.fault = 0;
    /* PERM HOIST (Part C): linmem is the one r/w region; valid + LOAD|STORE are a
     * module invariant verified here, so the per-access path below is bounds-only.
     * (Richer models - read-only / multi-region - would hoist per-region; not implemented.) */
}
static inline int sw_inb(uint32_t addr){ return !(addr > g.size || g.size - addr < 4u); }

static int32_t sw_load32(cvsasx_gate_backend_t *self, uint32_t addr){
    (void)self; if (!sw_inb(addr)){ g.fault = 1; return 0; }        /* bounds-only check */
    uint8_t *p = g.mem + addr;
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}
static void sw_store32(cvsasx_gate_backend_t *self, uint32_t addr, int32_t val){
    (void)self; if (!sw_inb(addr)){ g.fault = 1; return; }
    uint8_t *p = g.mem + addr; uint32_t v = (uint32_t)val;
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}
static int32_t sw_load32_raw(cvsasx_gate_backend_t *self, uint32_t addr){   /* check ELIDED (proven) */
    (void)self; uint8_t *p = g.mem + addr;
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}
static void sw_store32_raw(cvsasx_gate_backend_t *self, uint32_t addr, int32_t val){
    (void)self; uint8_t *p = g.mem + addr; uint32_t v = (uint32_t)val;
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}

cvsasx_gate_backend_t *cvsasx_gate_backend_sw(void){
    g.base.name = "sw-sfi";
    g.base.mem_bind = sw_bind;
    g.base.load32 = sw_load32;       g.base.store32 = sw_store32;
    g.base.load32_raw = sw_load32_raw; g.base.store32_raw = sw_store32_raw;
    return &g.base;
}
int  cvsasx_gate_backend_sw_fault(void){ return g.fault; }
void cvsasx_gate_backend_sw_reset_fault(void){ g.fault = 0; }
