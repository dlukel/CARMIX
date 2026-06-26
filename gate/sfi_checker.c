/* ===========================================================================
 * CV-SASX  -  gate/sfi_checker.c
 * The linear-time SFI checker (// TCB) - the load-time trust anchor (Master
 * Prompt Part 2.3; RockSalt/VeriWasm lineage // REF). One pass over the AOT-
 * lowered IR; verifies the FIXED, DOCUMENTED invariant set I1..I5 (see
 * cvsasx_gate.h). Backend-agnostic: references NO CHERI builtin (// SEAM).
 *
 * MODEL-INCOMPLETENESS (// REF VeriWasm CVE-2021-32629): the guarantee holds
 * only relative to this modeled set. That is why the CHERI run-time backstop is
 * mandatory and INDEPENDENT - the checker catches STATIC structural violations;
 * CHERI catches DYNAMIC out-of-bounds the checker cannot know statically.
 * ===========================================================================*/
#include "cvsasx_gate.h"

cvsasx_gate_status_t cvsasx_gate_check(const cvsasx_gate_module_t *m) {
    if (m == 0 || m->code == 0) return CVSASX_GATE_NULL_ARG;

    /* Pre-pass (O(table_len)): every call_indirect target must be a validated,
     * in-range code entry. Keeps the main pass linear in code size (I3). */
    for (uint32_t t = 0; t < m->table_len; ++t) {
        if (m->table == 0) return CVSASX_GATE_REJECT_CFLOW;
        if (m->table[t] >= m->code_len) return CVSASX_GATE_REJECT_CFLOW; /* unvalidated target */
    }

    /* Main pass (O(code_len)): linear, no fixpoint. */
    long depth = 0;   /* operand-stack depth (basic I5; full join-typing is the
                       * frontend Wasm validator's job - defense in depth). */
    for (uint32_t i = 0; i < m->code_len; ++i) {
        const gir_inst_t *in = &m->code[i];
        switch (in->op) {
        case GIR_CONST:     depth += 1; break;
        case GIR_LOCAL_GET: if (in->a >= m->n_locals) return CVSASX_GATE_REJECT_STACK;
                            depth += 1; break;
        case GIR_LOCAL_SET: if (in->a >= m->n_locals) return CVSASX_GATE_REJECT_STACK;
                            if (depth < 1) return CVSASX_GATE_REJECT_STACK;
                            depth -= 1; break;
        case GIR_ADD: case GIR_SUB: case GIR_MUL: case GIR_AND: case GIR_OR:
        case GIR_XOR: case GIR_SHL: case GIR_SHRU: case GIR_LTU:
                            if (depth < 2) return CVSASX_GATE_REJECT_STACK;
                            depth -= 1; break;              /* pop 2, push 1 */
        case GIR_EQZ:       if (depth < 1) return CVSASX_GATE_REJECT_STACK;
                            break;                          /* pop 1, push 1 */
        case GIR_LOAD:      /* I1: linear-memory access MUST use the linmem cap slot */
                            if (in->a != GIR_CAPSLOT_LINMEM) return CVSASX_GATE_REJECT_MEMSLOT;
                            if (depth < 1) return CVSASX_GATE_REJECT_STACK;
                            break;                          /* pop addr, push val (net 0) */
        case GIR_STORE:     if (in->a != GIR_CAPSLOT_LINMEM) return CVSASX_GATE_REJECT_MEMSLOT;
                            if (depth < 2) return CVSASX_GATE_REJECT_STACK;
                            depth -= 2; break;              /* pop val, addr */
        case GIR_DROP:      if (depth < 1) return CVSASX_GATE_REJECT_STACK;
                            depth -= 1; break;
        case GIR_BR:        /* I3: target in range, on an instruction boundary */
                            if (in->a >= m->code_len) return CVSASX_GATE_REJECT_CFLOW;
                            break;
        case GIR_BR_IF:     if (in->a >= m->code_len) return CVSASX_GATE_REJECT_CFLOW;
                            if (depth < 1) return CVSASX_GATE_REJECT_STACK;
                            depth -= 1; break;              /* pop cond */
        case GIR_RETURN:    break;
        case GIR_END:       break;
        case GIR_CALL_INDIRECT:
                            /* I3: a dispatch table must exist (targets pre-validated
                             * above); the runtime range-checks the index + type id. */
                            if (m->table == 0 || m->table_len == 0) return CVSASX_GATE_REJECT_CFLOW;
                            if (depth < 1) return CVSASX_GATE_REJECT_STACK;
                            depth -= 1; break;              /* pop table index */
        case GIR_CAP_FORGE: /* I2: capability forge/amplify is never permitted */
                            return CVSASX_GATE_REJECT_FORGE;
        default:            /* I2/I4: unknown / forbidden opcode (incl. any otype op,
                             * of which the permitted IR set contains none). */
                            return CVSASX_GATE_REJECT_OPCODE;
        }
        /* I5 (UPPER bound): the modelled operand-stack depth must never exceed the
         * executor's stack ceiling (audit H2). Over-deep modules are rejected at
         * LOAD time, not merely caught fail-closed by the executor at run time -
         * closing the checker-completeness gap for the stack-overflow class. */
        if (depth > (long)CVSASX_GATE_STACK_MAX) return CVSASX_GATE_REJECT_STACK;
    }
    return CVSASX_GATE_OK;
}

/* ===========================================================================
 * MARKER SUMMARY
 *   // TCB:  this checker is the load-time trust anchor.
 *   // SEAM: references no CHERI builtin - backend-agnostic.
 *   // REF:  RockSalt (PLDI'12), VeriWasm (NDSS'21); CVE-2021-32629 (model gap).
 *   // not implemented: I5 here is a basic depth/underflow + local-range check; FULL
 *           structured-control-flow stack-height typing at join points is done by
 *           the Wasm validator (wasm_validate.c). The SFI-critical invariants
 *           (I1 memory slot, I2 no-forge, I3 CF targets, I4 no-otype) are complete.
 * ===========================================================================*/
