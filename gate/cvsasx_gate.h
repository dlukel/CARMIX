/* ===========================================================================
 * CV-SASX  -  gate/cvsasx_gate.h
 * The load-time safety gate (Master Prompt Part 2, Law L5): hosted WebAssembly
 * is validated, AOT-lowered to a CHERI-SFI form, and checked by a linear-time
 * SFI verifier BEFORE a single instruction runs. CHERI is the run-time backstop;
 * the checker is the load-time gate. BOTH must hold; neither alone is trusted.
 *
 * MODULE:  gate/  ·  DEPENDS ON LAWS: L2 (capability isolation), L5 (safety
 *          established before first execution).
 *
 * STRATEGIC FRAMING (from the research): Wasm-as-capability-layer is mainstream
 * (Faasm, MSWasm, Cage) - gate/ is an ENGINEERING module, not the novelty. The
 * novelty is the cross-machine rematerialization above it (cap/). Consequences,
 * realized below:
 *   - CHECKER-CENTRIC trust (RockSalt/VeriWasm/rWasm lineage // REF): the small
 *     SFI CHECKER is the trust anchor; the AOT compiler is UNTRUSTED (// TCB).
 *   - PLUGGABLE BACKEND from day one (MSWasm precedent // REF): validation +
 *     checker are backend-agnostic; only the backend touches the enforcement
 *     mechanism. CHERI is the first concrete backend; MTE/PAC and software-SFI
 *     are future backends admitted by the same // SEAM (NOT built here).
 *
 * SCOPE (honest): this prototype handles a small, DOCUMENTED subset of Wasm 1.0
 * (i32 arithmetic/compare, locals, a single linear memory load/store, structured
 * branches, and call_indirect for the control-flow test). See // not implemented below.
 *
 * // REF: Wasm 1.0 spec (validation rules); WasmCert-Isabelle/Coq (mechanized
 *         semantics - a correctness REFERENCE, NOT claimed proven against here);
 *         RockSalt (Morrisett PLDI'12), VeriWasm (Johnson NDSS'21), vWasm/rWasm;
 *         Cage (Wasm on Arm MTE); MSWasm (swappable memory-safety enforcement).
 * // TCB: the SFI checker (cvsasx_gate_check) - the load-time trust anchor.
 * // TCB: the AOT lowerer (gate/wasm_lower.c) - UNTRUSTED compiler; in the TCB
 *         unless verified/translation-validated. CompCert + translation-
 *         validation are the // REF paths NOT taken in this prototype.
 * // SEAM: cvsasx_gate_backend_t - the enforcement seam (CHERI / MTE / SFI).
 * // not implemented: full Wasm 1.0 (br_table, globals, multi-mem, f32/f64, i64) + proposals
 *         (threads, SIMD, GC) - out of scope; named, not faked.
 * // not implemented: emit standalone CHERI-RISC-V MACHINE CODE (JIT/AOT-to-binary). This
 *         prototype lowers to the CHERI-SFI IR below and executes it via a native
 *         CHERI executor - the enforcement semantics (bounded-cap memory, checked
 *         control flow) are IDENTICAL; only the packaging differs.
 * // not implemented: MTE/PAC and software-SFI backends (commodity retargets) - the SEAM
 *         admits them; they are not built in this phase.
 * // not implemented: full functional verification of the lowerer (vWasm-style) - not done;
 *         trust comes from the checker + the CHERI backstop instead.
 *
 * Freestanding: <stdint.h>/<stddef.h> only.
 * ===========================================================================*/
#ifndef CVSASX_GATE_H
#define CVSASX_GATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * The CHERI-SFI IR: the AOT-lowered, checkable form of a validated Wasm function.
 * A stack machine over i32 values + i32 locals. Linear memory is a single bounded
 * capability owned by the backend. This IR is what the checker verifies and the
 * executor runs.
 * ------------------------------------------------------------------------- */
typedef enum gir_op {
    GIR_END = 0,        /* end of function (returns top-of-stack, or void)        */
    GIR_CONST,          /* imm: push i32 imm                                      */
    GIR_LOCAL_GET,      /* a: push local[a]                                       */
    GIR_LOCAL_SET,      /* a: local[a] = pop                                      */
    GIR_ADD, GIR_SUB, GIR_MUL, GIR_AND, GIR_OR, GIR_XOR, GIR_SHL, GIR_SHRU,
    GIR_EQZ,            /* push (pop==0)                                          */
    GIR_LTU,            /* b=pop,a=pop; push (a<b) unsigned                       */
    GIR_LOAD,           /* a=cap_slot, imm=offset: pop addr; push mem[addr+imm]   */
    GIR_STORE,          /* a=cap_slot, imm=offset: pop val; pop addr; mem[..]=val */
    GIR_DROP,           /* pop, discard                                           */
    GIR_BR,             /* a: branch to instruction index a                       */
    GIR_BR_IF,          /* a: pop cond; if cond!=0 branch to a                    */
    GIR_RETURN,         /* return top-of-stack                                    */
    GIR_CALL_INDIRECT,  /* a=expected type id: pop table-index; dispatch          */
    /* --- ops a buggy/MALICIOUS lowerer might emit; the CHECKER MUST reject --- */
    GIR_CAP_FORGE,      /* (illegal) fabricate/amplify a capability               */
    /* --- OPTIMIZER-ONLY: bounds-check-ELIDED accesses. The checker REJECTS these
     * in untrusted input (its default case -> REJECT_OPCODE); cvsasx_gate_optimize
     * emits them ONLY for accesses it PROVED in-bounds, AFTER cvsasx_gate_check. */
    GIR_LOAD_RAW,       /* like GIR_LOAD, runtime bounds check ELIDED (proven safe) */
    GIR_STORE_RAW,      /* like GIR_STORE, runtime bounds check ELIDED              */
    GIR_OP_MAX
} gir_op_t;

#define GIR_CAPSLOT_LINMEM 0u    /* the ONLY legal cap slot for memory ops */

/* Operand-stack ceiling, SHARED between the checker (which now rejects modules
 * whose modelled depth exceeds it - audit H2) and the executor (whose stack array
 * is this size). Sharing the constant prevents the modelled and enforced bounds
 * from drifting apart. */
#define CVSASX_GATE_STACK_MAX 512u

typedef struct gir_inst {
    uint16_t op;        /* gir_op_t */
    uint16_t a;         /* cap_slot / branch target / local idx / type id */
    uint32_t imm;       /* const value / static memory offset */
} gir_inst_t;

/* A gate module = one function's lowered IR + its declared shape. */
typedef struct cvsasx_gate_module {
    const gir_inst_t *code;
    uint32_t code_len;
    uint32_t n_params;          /* i32 params (a prefix of the locals)        */
    uint32_t n_locals;          /* total i32 locals (>= n_params)             */
    uint32_t mem_size;          /* linear-memory size in bytes (the cap bound) */
    /* call_indirect dispatch table (validated targets only). */
    const uint32_t *table;      /* table[i] = code index of target entry      */
    const uint16_t *table_types;/* table_types[i] = type id of that target    */
    uint32_t table_len;
} cvsasx_gate_module_t;

/* ---------------------------------------------------------------------------
 * // SEAM: the enforcement backend. Validation + the checker NEVER touch this
 * (or any CHERI builtin); only a concrete backend does. The executor routes ALL
 * linear-memory access through it. CHERI is the first backend (backend_cheri.c);
 * MTE/PAC and software-SFI are future backends (not implemented).
 * ------------------------------------------------------------------------- */
typedef struct cvsasx_gate_backend {
    const char *name;
    /* Bind linear memory [mem, mem+mem_size). CHERI: derive a bounded data cap. */
    void    (*mem_bind)(struct cvsasx_gate_backend *self, void *mem, uint32_t mem_size);
    /* Bounded i32 load/store. The enforcement (CHERI bounds / MTE tag / SFI mask)
     * lives HERE. On CHERI an out-of-bounds access TRAPS - the run-time backstop. */
    int32_t (*load32)(struct cvsasx_gate_backend *self, uint32_t addr);
    void    (*store32)(struct cvsasx_gate_backend *self, uint32_t addr, int32_t val);
    /* RAW i32 load/store: the runtime bounds check is ELIDED (used only for accesses
     * the checker PROVED in-bounds). On CHERI these are the same cap access (hardware
     * still enforces - defense in depth). On the software backend they skip the SFI
     * check - that is the static-elision speedup. */
    int32_t (*load32_raw)(struct cvsasx_gate_backend *self, uint32_t addr);
    void    (*store32_raw)(struct cvsasx_gate_backend *self, uint32_t addr, int32_t val);
} cvsasx_gate_backend_t;

/* ---------------------------------------------------------------------------
 * Status codes. REJECT_* = caught by the checker (load time); TRAP = caught by
 * CHERI (run time).
 * ------------------------------------------------------------------------- */
typedef enum cvsasx_gate_status {
    CVSASX_GATE_OK = 0,
    CVSASX_GATE_REJECT_VALIDATION,  /* Wasm-level validation failed (frontend) */
    CVSASX_GATE_REJECT_MEMSLOT,     /* I1: memory op used a non-LINMEM cap slot */
    CVSASX_GATE_REJECT_FORGE,       /* I2: forge/amplify op present */
    CVSASX_GATE_REJECT_CFLOW,       /* I3: branch/call target out of range / unvalidated */
    CVSASX_GATE_REJECT_OTYPE,       /* I4: otype/sealing op present */
    CVSASX_GATE_REJECT_STACK,       /* I5: operand-stack discipline / local idx */
    CVSASX_GATE_REJECT_OPCODE,      /* unknown opcode */
    CVSASX_GATE_TRAP,               /* run time: CHERI trapped (backstop caught it) */
    CVSASX_GATE_NULL_ARG
} cvsasx_gate_status_t;

/* ---------------------------------------------------------------------------
 * THE LINEAR-TIME SFI CHECKER (// TCB) - the load-time trust anchor.
 *
 * Modeled invariant set (FIXED + DOCUMENTED; one linear pass over the IR):
 *   I1 (memory): every GIR_LOAD/GIR_STORE has a == GIR_CAPSLOT_LINMEM - all
 *      linear-memory access goes through the single bounded linmem capability.
 *   I2 (no forge): no GIR_CAP_FORGE; opcode in the permitted set only - the IR
 *      cannot derive/amplify a capability.
 *   I3 (control flow): every GIR_BR/GIR_BR_IF target is in [0,code_len) on an
 *      instruction boundary; GIR_CALL_INDIRECT type ids are declared and the
 *      runtime range-checks the table index - CF stays in bounds, validated targets.
 *   I4 (otypes): no sealing/otype op exists in the permitted opcode set.
 *   I5 (stack discipline): a linear abstract pass tracks operand-stack depth;
 *      operands present, no underflow, local indices < n_locals.
 *
 * // REF: VeriWasm CVE-2021-32629 - the checker's guarantee holds ONLY relative
 *         to this modeled set; an incomplete model is a real, historically-
 *         realized failure mode. That is exactly why the CHERI run-time backstop
 *         is MANDATORY and INDEPENDENT (Master Prompt 2.4). The two layers catch
 *         different classes: the checker catches STATIC structural violations;
 *         CHERI catches DYNAMIC out-of-bounds the checker cannot know statically.
 * ------------------------------------------------------------------------- */
cvsasx_gate_status_t cvsasx_gate_check(const cvsasx_gate_module_t *m);

/* Execute a module that PASSED the checker, via a backend. args/n_args seed the
 * parameter locals; the i32 result is written to *out. All linear-memory access
 * goes through the backend (CHERI enforcement). */
cvsasx_gate_status_t cvsasx_gate_exec(const cvsasx_gate_module_t *m,
                                      cvsasx_gate_backend_t *be,
                                      const int32_t *args, uint32_t n_args,
                                      int32_t *out);

/* STATIC BOUNDS-CHECK ELISION (optimize.c). Produces an optimized copy of `m`
 * (code written into out_code, length m->code_len) where memory accesses the
 * optimizer PROVES in-bounds - a CONSTANT address with const+offset+4 <= mem_size -
 * are rewritten LOAD/STORE -> LOAD_RAW/STORE_RAW (runtime bounds check dropped).
 * MUST be run ONLY on a module that PASSED cvsasx_gate_check. NOT-proven accesses
 * keep the checked op. *out_n_elided = count rewritten.
 * // VERIFY: each elided access is GENUINELY proven (constant addr within bound) -
 *           a heuristic would be a CRITICAL safety hole. Soundness is re-confirmed by
 *           the adversarial elision tests (carmix perf P1).
 * mode FORCE_RAW (UNSAFE - baseline measurement ONLY) makes ALL accesses raw. */
typedef enum { CVSASX_GATE_OPT_ELIDE_PROVEN = 0, CVSASX_GATE_OPT_FORCE_RAW = 1 } cvsasx_gate_opt_mode_t;
cvsasx_gate_status_t cvsasx_gate_optimize(const cvsasx_gate_module_t *m, gir_inst_t *out_code,
                                          cvsasx_gate_module_t *out_mod,
                                          cvsasx_gate_opt_mode_t mode, uint32_t *out_n_elided);

/* Parse + validate + AOT-lower a Wasm 1.0 (subset) binary module to a gate
 * module (wasm_frontend.c). The IR is written into out_code[0..out_code_cap);
 * on success *out_mod points into it. Returns REJECT_VALIDATION on malformed or
 * unsupported input. // TCB: the lowerer is untrusted - cvsasx_gate_check
 * independently re-verifies its output (defense in depth). */
cvsasx_gate_status_t cvsasx_wasm_load(const unsigned char *wasm, uint32_t len,
                                      gir_inst_t *out_code, uint32_t out_code_cap,
                                      uint32_t linmem_size,
                                      cvsasx_gate_module_t *out_mod);

/* The CHERI backend instance (backend_cheri.c). */
cvsasx_gate_backend_t *cvsasx_gate_backend_cheri(void);

/* The minted linear-memory capability (for the cap/ strip seam - gate test G5). */
void *cvsasx_gate_backend_cheri_linmem(void);

#ifdef __cplusplus
}
#endif

/* ===========================================================================
 * MARKER SUMMARY
 *   // TCB:  cvsasx_gate_check (load-time trust anchor); the AOT lowerer (untrusted).
 *   // SEAM: cvsasx_gate_backend_t - validation/checker are backend-agnostic.
 *   // REF:  Wasm 1.0 spec; WasmCert; RockSalt/VeriWasm/rWasm; Cage; MSWasm;
 *           VeriWasm CVE-2021-32629 (model-incompleteness -> CHERI backstop).
 *   // not implemented: full Wasm + proposals; machine-code JIT; MTE/PAC + software-SFI
 *           backends; full lowerer verification - all named, not faked.
 * ===========================================================================*/
#endif /* CVSASX_GATE_H */
