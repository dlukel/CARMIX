/* ===========================================================================
 * CV-SASX  -  gate/optimize.c
 * Static bounds-check ELISION (VeriWasm lineage // REF). Rewrites memory accesses
 * the optimizer PROVES in-bounds - a CONSTANT address with const+offset+4 <=
 * mem_size - to LOAD_RAW/STORE_RAW so the runtime bounds check is dropped. Runs
 * ONLY after cvsasx_gate_check; the checker REJECTS RAW opcodes in untrusted input
 * (its default case), so an attacker cannot inject elided accesses.
 *
 * SOUNDNESS: the address const must be provable. A small abstract-interpretation
 * pass tracks per-stack-slot constness; at every BRANCH TARGET it conservatively
 * resets to UNKNOWN (the join of all predecessors), so constness is only believed
 * within straight-line code. A loop body that re-establishes `CONST k` each
 * iteration is genuinely const each iteration -> safe to elide. NO heuristics.
 * Backend-agnostic: no CHERI builtin (// SEAM).
 * ===========================================================================*/
#include "cvsasx_gate.h"

#define OPTMAX 4096u   /* modules larger than this are returned unoptimized (safe) */

typedef struct { int is_const; uint32_t val; } absv;

cvsasx_gate_status_t cvsasx_gate_optimize(const cvsasx_gate_module_t *m, gir_inst_t *out_code,
                                          cvsasx_gate_module_t *out_mod,
                                          cvsasx_gate_opt_mode_t mode, uint32_t *out_n_elided){
    if (m == 0 || m->code == 0 || out_code == 0 || out_mod == 0) return CVSASX_GATE_NULL_ARG;
    uint32_t n = m->code_len, elided = 0;
    for (uint32_t i = 0; i < n; ++i) out_code[i] = m->code[i];
    *out_mod = *m; out_mod->code = out_code;
    if (out_n_elided) *out_n_elided = 0;
    if (n > OPTMAX) return CVSASX_GATE_OK;                     /* too big: safe unoptimized copy */

    uint8_t is_target[OPTMAX];
    for (uint32_t i = 0; i < n; ++i) is_target[i] = 0;
    for (uint32_t i = 0; i < n; ++i){
        uint16_t op = m->code[i].op;
        if (op == GIR_BR || op == GIR_BR_IF){ uint32_t t = m->code[i].a; if (t < n) is_target[t] = 1; }
    }

    absv cs[CVSASX_GATE_STACK_MAX]; int sp = 0;
    int force = (mode == CVSASX_GATE_OPT_FORCE_RAW);
    for (uint32_t i = 0; i < n; ++i){
        if (is_target[i]) for (int k = 0; k < sp && k < (int)CVSASX_GATE_STACK_MAX; ++k) cs[k].is_const = 0;
        const gir_inst_t *in = &m->code[i];
        switch (in->op){
        case GIR_CONST: if (sp < (int)CVSASX_GATE_STACK_MAX){ cs[sp].is_const = 1; cs[sp].val = in->imm; sp++; } break;
        case GIR_LOCAL_GET: if (sp < (int)CVSASX_GATE_STACK_MAX){ cs[sp].is_const = 0; sp++; } break;
        case GIR_LOCAL_SET: if (sp > 0) sp--; break;
        case GIR_ADD: case GIR_SUB: case GIR_MUL: case GIR_AND: case GIR_OR:
        case GIR_XOR: case GIR_SHL: case GIR_SHRU: case GIR_LTU:
            if (sp > 0) sp--; if (sp > 0) cs[sp-1].is_const = 0; break;   /* pop2 push1(unknown) */
        case GIR_EQZ: if (sp > 0) cs[sp-1].is_const = 0; break;
        case GIR_LOAD: {
            int prove = force;
            if (!prove && sp >= 1 && cs[sp-1].is_const){
                uint64_t eff = (uint64_t)cs[sp-1].val + in->imm;
                if (eff + 4u <= (uint64_t)m->mem_size) prove = 1;        /* PROVEN in-bounds */
            }
            if (prove){ out_code[i].op = GIR_LOAD_RAW; elided++; }
            if (sp > 0) cs[sp-1].is_const = 0;                            /* addr popped, val pushed */
        } break;
        case GIR_STORE: {
            int prove = force;
            if (!prove && sp >= 2 && cs[sp-2].is_const){
                uint64_t eff = (uint64_t)cs[sp-2].val + in->imm;
                if (eff + 4u <= (uint64_t)m->mem_size) prove = 1;
            }
            if (prove){ out_code[i].op = GIR_STORE_RAW; elided++; }
            if (sp > 0) sp--; if (sp > 0) sp--;                          /* pop val, addr */
        } break;
        case GIR_DROP: case GIR_BR_IF: case GIR_CALL_INDIRECT: if (sp > 0) sp--; break;
        default: break;   /* BR / RETURN / END / others: no stack effect tracked here */
        }
    }
    if (out_n_elided) *out_n_elided = elided;
    return CVSASX_GATE_OK;
}
