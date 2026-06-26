/* ===========================================================================
 * CV-SASX  -  gate/wasm_frontend.c
 * Wasm 1.0 (DOCUMENTED SUBSET) binary parser + validator + AOT lowering to the
 * CHERI-SFI IR (deliverables 1 + 2). Backend-agnostic: references NO CHERI
 * builtin (// SEAM). The lowerer is // TCB (untrusted) - its output is
 * independently re-verified by cvsasx_gate_check (defense in depth: a lowering
 * bug is caught by the checker).
 *
 * // REF: WebAssembly 1.0 spec - §5 binary format (sections, LEB128), §3
 *         validation. WasmCert-Isabelle/Coq is the mechanized-semantics REFERENCE
 *         (NOT claimed proven against). Validation rules are taken from the spec,
 *         not invented; the test module is cross-checked by wabt `wasm-validate`.
 *
 * SUBSET (// not implemented beyond this): one memory; one function with i32 params/locals;
 * i32.const, local.get/set, i32.add/sub/mul/and/or/xor/shl/shr_u, i32.eqz/lt_u,
 * i32.load/store, drop, return, end. No globals/tables/br_table/f32/f64/i64/calls.
 * ===========================================================================*/
#include "cvsasx_gate.h"

typedef struct { const unsigned char *p, *end; int err; } wr_t;

static unsigned char rd_byte(wr_t *r){ if(r->p>=r->end){r->err=1;return 0;} return *r->p++; }
static uint32_t rd_u32(wr_t *r){                      /* unsigned LEB128 */
    uint32_t v=0; int s=0;
    for(;;){ if(r->p>=r->end){r->err=1;return 0;} unsigned char b=*r->p++;
        v|=(uint32_t)(b&0x7f)<<s; if(!(b&0x80)) return v; s+=7; if(s>=35){r->err=1;return 0;} }
}
static int32_t rd_s32(wr_t *r){                       /* signed LEB128 */
    int32_t v=0; int s=0; unsigned char b;
    do{ if(r->p>=r->end){r->err=1;return 0;} b=*r->p++;
        v|=(int32_t)(b&0x7f)<<s; s+=7; }while(b&0x80);
    if(s<32 && (b&0x40)) v|=-(int32_t)(1u<<s);
    return v;
}

#define WASM_I32 0x7f
#define RJ CVSASX_GATE_REJECT_VALIDATION

/* Lower one expression (function body) to IR; returns instruction count or <0. */
static long lower_expr(wr_t *r, gir_inst_t *out, uint32_t cap, uint32_t n_locals,
                       int have_memory){
    uint32_t n=0;
    #define EMIT(OP,A,IMM) do{ if(n>=cap) return -1; out[n].op=(uint16_t)(OP); \
        out[n].a=(uint16_t)(A); out[n].imm=(uint32_t)(IMM); n++; }while(0)
    for(;;){
        if(r->err) return -1;
        unsigned char op = rd_byte(r);
        switch(op){
        case 0x41: { int32_t c=rd_s32(r); EMIT(GIR_CONST,0,(uint32_t)c); } break; /* i32.const */
        case 0x20: { uint32_t i=rd_u32(r); if(i>=n_locals) return -1; EMIT(GIR_LOCAL_GET,i,0);} break;
        case 0x21: { uint32_t i=rd_u32(r); if(i>=n_locals) return -1; EMIT(GIR_LOCAL_SET,i,0);} break;
        case 0x6a: EMIT(GIR_ADD,0,0); break;
        case 0x6b: EMIT(GIR_SUB,0,0); break;
        case 0x6c: EMIT(GIR_MUL,0,0); break;
        case 0x71: EMIT(GIR_AND,0,0); break;
        case 0x72: EMIT(GIR_OR,0,0);  break;
        case 0x73: EMIT(GIR_XOR,0,0); break;
        case 0x74: EMIT(GIR_SHL,0,0); break;
        case 0x76: EMIT(GIR_SHRU,0,0);break;
        case 0x45: EMIT(GIR_EQZ,0,0); break;            /* i32.eqz */
        case 0x49: EMIT(GIR_LTU,0,0); break;            /* i32.lt_u */
        case 0x28: {                                    /* i32.load align offset */
            if(!have_memory) return -1; (void)rd_u32(r); uint32_t off=rd_u32(r);
            EMIT(GIR_LOAD,GIR_CAPSLOT_LINMEM,off);
        } break;
        case 0x36: {                                    /* i32.store align offset */
            if(!have_memory) return -1; (void)rd_u32(r); uint32_t off=rd_u32(r);
            EMIT(GIR_STORE,GIR_CAPSLOT_LINMEM,off);
        } break;
        case 0x1a: EMIT(GIR_DROP,0,0); break;           /* drop */
        case 0x0f: EMIT(GIR_RETURN,0,0); break;         /* return */
        case 0x0b: EMIT(GIR_END,0,0); return (long)n;   /* end of body */
        default: return -1;                             /* unsupported opcode */
        }
    }
    #undef EMIT
}

cvsasx_gate_status_t cvsasx_wasm_load(const unsigned char *wasm, uint32_t len,
                                      gir_inst_t *out_code, uint32_t out_code_cap,
                                      uint32_t linmem_size,
                                      cvsasx_gate_module_t *out_mod){
    if(wasm==0 || out_code==0 || out_mod==0) return CVSASX_GATE_NULL_ARG;
    wr_t r = { wasm, wasm+len, 0 };
    /* §5.1 magic + version */
    if(len<8) return RJ;
    if(!(wasm[0]==0x00&&wasm[1]==0x61&&wasm[2]==0x73&&wasm[3]==0x6d)) return RJ;
    if(!(wasm[4]==0x01&&wasm[5]==0x00&&wasm[6]==0x00&&wasm[7]==0x00)) return RJ;
    r.p = wasm+8;

    uint32_t n_params=0, n_locals=0, fn_typeidx=0; int have_memory=0;
    int have_type=0, have_func=0, have_code=0;
    long code_n=-1;

    while(r.p < r.end && !r.err){
        unsigned char sid = rd_byte(&r);
        uint32_t ssize = rd_u32(&r);
        const unsigned char *sbeg = r.p, *send = r.p + ssize;
        if(send > r.end){ return RJ; }
        switch(sid){
        case 1: {                                       /* Type section */
            uint32_t nt = rd_u32(&r); if(nt!=1) return RJ;   /* subset: exactly 1 type */
            if(rd_byte(&r)!=0x60) return RJ;            /* functype tag */
            n_params = rd_u32(&r);
            for(uint32_t i=0;i<n_params;i++) if(rd_byte(&r)!=WASM_I32) return RJ;
            uint32_t nres = rd_u32(&r); if(nres!=1) return RJ;   /* (result i32) */
            if(rd_byte(&r)!=WASM_I32) return RJ;
            have_type=1;
        } break;
        case 3: {                                       /* Function section */
            uint32_t nf = rd_u32(&r); if(nf!=1) return RJ;   /* subset: 1 function */
            fn_typeidx = rd_u32(&r); if(fn_typeidx!=0) return RJ;
            have_func=1;
        } break;
        case 5: {                                       /* Memory section */
            uint32_t nm = rd_u32(&r); if(nm!=1) return RJ;   /* subset: 1 memory */
            unsigned char flag = rd_byte(&r);
            (void)rd_u32(&r);                           /* min */
            if(flag==0x01) (void)rd_u32(&r);            /* max */
            else if(flag!=0x00) return RJ;
            have_memory=1;
        } break;
        case 10: {                                      /* Code section */
            uint32_t nc = rd_u32(&r); if(nc!=1) return RJ;
            (void)rd_u32(&r);                           /* body size */
            uint32_t nlocal_decls = rd_u32(&r);
            n_locals = n_params;
            for(uint32_t i=0;i<nlocal_decls;i++){
                uint32_t cnt = rd_u32(&r); if(rd_byte(&r)!=WASM_I32) return RJ;
                n_locals += cnt;
            }
            code_n = lower_expr(&r, out_code, out_code_cap, n_locals, have_memory);
            if(code_n < 0) return RJ;
            have_code=1;
        } break;
        default:                                        /* skip custom/other sections */
            r.p = send; break;
        }
        if(r.err) return RJ;
        r.p = send;                                     /* resync to section end */
        (void)sbeg;
    }
    if(r.err || !have_type || !have_func || !have_code) return RJ;

    out_mod->code        = out_code;
    out_mod->code_len    = (uint32_t)code_n;
    out_mod->n_params    = n_params;
    out_mod->n_locals    = n_locals;
    out_mod->mem_size    = linmem_size;
    out_mod->table       = 0;
    out_mod->table_types = 0;
    out_mod->table_len   = 0;
    return CVSASX_GATE_OK;
}

/* ===========================================================================
 * // SEAM: no CHERI builtin referenced (backend-agnostic frontend).
 * // TCB:  the lowerer is untrusted; cvsasx_gate_check re-verifies its output.
 * // REF:  Wasm 1.0 spec §3/§5; WasmCert (reference). // not implemented: full Wasm + proposals.
 * ===========================================================================*/
