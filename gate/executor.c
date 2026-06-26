/* ===========================================================================
 * CV-SASX  -  gate/executor.c
 * The IR executor: a small stack machine that runs a CHECKED gate module. All
 * linear-memory access is routed through the backend (// SEAM) - the CHERI
 * backend enforces bounds at run time. Backend-agnostic: references NO CHERI
 * builtin. (// not implemented: a machine-code JIT would replace this native interpreter;
 * the enforcement semantics are identical.)
 *
 * PRECONDITION: only call on a module that passed cvsasx_gate_check.
 * ===========================================================================*/
#include "cvsasx_gate.h"

#define GSTACK_MAX  CVSASX_GATE_STACK_MAX   /* shared with the checker (audit H2) */
#define GLOCALS_MAX 64

cvsasx_gate_status_t cvsasx_gate_exec(const cvsasx_gate_module_t *m,
                                      cvsasx_gate_backend_t *be,
                                      const int32_t *args, uint32_t n_args,
                                      int32_t *out) {
    if (m == 0 || be == 0) return CVSASX_GATE_NULL_ARG;
    if (m->n_locals > GLOCALS_MAX) return CVSASX_GATE_NULL_ARG;

    int32_t stack[GSTACK_MAX];
    int sp = 0;
    int32_t locals[GLOCALS_MAX];
    for (uint32_t i = 0; i < m->n_locals; ++i)
        locals[i] = (i < n_args && args) ? args[i] : 0;

    uint32_t pc = 0;
    while (pc < m->code_len) {
        /* Self-guard BEFORE the op (audit H2): a push never writes at sp>=GSTACK_MAX
         * even if this module was not run through the checker - the executor does
         * not rely solely on its caller having checked. (The checker now also
         * rejects over-deep modules at load time; this is the run-time belt.) */
        if (sp < 0 || sp >= GSTACK_MAX) return CVSASX_GATE_REJECT_STACK;
        const gir_inst_t *in = &m->code[pc];
        switch (in->op) {
        case GIR_CONST:     stack[sp++] = (int32_t)in->imm; pc++; break;
        case GIR_LOCAL_GET: stack[sp++] = locals[in->a]; pc++; break;
        case GIR_LOCAL_SET: locals[in->a] = stack[--sp]; pc++; break;
        case GIR_ADD: stack[sp-2] += stack[sp-1]; sp--; pc++; break;
        case GIR_SUB: stack[sp-2] -= stack[sp-1]; sp--; pc++; break;
        case GIR_MUL: stack[sp-2] *= stack[sp-1]; sp--; pc++; break;
        case GIR_AND: stack[sp-2] &= stack[sp-1]; sp--; pc++; break;
        case GIR_OR:  stack[sp-2] |= stack[sp-1]; sp--; pc++; break;
        case GIR_XOR: stack[sp-2] ^= stack[sp-1]; sp--; pc++; break;
        case GIR_SHL: stack[sp-2] = stack[sp-2] << (stack[sp-1] & 31); sp--; pc++; break;
        case GIR_SHRU:stack[sp-2] = (int32_t)((uint32_t)stack[sp-2] >> (stack[sp-1] & 31)); sp--; pc++; break;
        case GIR_LTU: stack[sp-2] = ((uint32_t)stack[sp-2] < (uint32_t)stack[sp-1]) ? 1 : 0; sp--; pc++; break;
        case GIR_EQZ: stack[sp-1] = (stack[sp-1] == 0) ? 1 : 0; pc++; break;
        case GIR_LOAD: {                                 /* pop addr; push mem[addr+off] */
            uint32_t a = (uint32_t)stack[sp-1] + in->imm;
            stack[sp-1] = be->load32(be, a);             /* CHERI bounds-enforced */
            pc++;
        } break;
        case GIR_STORE: {                                /* pop val, addr; mem[addr+off]=val */
            int32_t v = stack[--sp];
            uint32_t a = (uint32_t)stack[--sp] + in->imm;
            be->store32(be, a, v);                       /* CHERI bounds-enforced */
            pc++;
        } break;
        case GIR_LOAD_RAW: {                             /* proven in-bounds: bounds check ELIDED */
            uint32_t a = (uint32_t)stack[sp-1] + in->imm;
            stack[sp-1] = be->load32_raw(be, a);
            pc++;
        } break;
        case GIR_STORE_RAW: {
            int32_t v = stack[--sp];
            uint32_t a = (uint32_t)stack[--sp] + in->imm;
            be->store32_raw(be, a, v);
            pc++;
        } break;
        case GIR_DROP: sp--; pc++; break;
        case GIR_BR: pc = in->a; break;
        case GIR_BR_IF: { int32_t c = stack[--sp]; pc = c ? in->a : pc + 1; } break;
        case GIR_RETURN: if (out) *out = (sp > 0) ? stack[sp-1] : 0; return CVSASX_GATE_OK;
        case GIR_END:    if (out) *out = (sp > 0) ? stack[sp-1] : 0; return CVSASX_GATE_OK;
        case GIR_CALL_INDIRECT: {                         /* runtime CF range + type check */
            uint32_t idx = (uint32_t)stack[--sp];
            if (m->table == 0 || idx >= m->table_len) return CVSASX_GATE_TRAP;
            if (m->table_types && m->table_types[idx] != in->a) return CVSASX_GATE_TRAP;
            pc = m->table[idx];
        } break;
        default: return CVSASX_GATE_REJECT_OPCODE;
        }
        if (sp < 0 || sp >= GSTACK_MAX) return CVSASX_GATE_REJECT_STACK;
    }
    if (out) *out = (sp > 0) ? stack[sp-1] : 0;
    return CVSASX_GATE_OK;
}
