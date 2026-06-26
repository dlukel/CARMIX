/* ===========================================================================
 * CV-SASX  -  sls/sls.c
 * The content-addressed single-level store: a Merkle object graph over store/.
 * An sls object IS a store/ object whose content is its TAGGED serialization, so
 * identity == BLAKE3(tag‖content) == the store key. Leaf = 0x00‖data; Node =
 * 0x01‖n‖child-hashes. Mutation produces a new object; unchanged children are
 * SHARED by hash (store/ dedup) - structural sharing.
 *
 * BACKEND-INDEPENDENT (// SEAM): references NO CHERI builtin and no direct
 * hashing - store/ does the BLAKE3 on WRITE (// HOT-PATH); the DIFF re-hashes
 * NOTHING (it only compares stored child-hashes and resolves O(changed) objects).
 * Content-addressing makes the graph ACYCLIC by construction (a node's hash
 * depends on its children's hashes, so a hash cannot contain itself).
 * ===========================================================================*/
#include "cvsasx_sls.h"

#define HLEN ((uint32_t)CVSASX_HASH_LEN)

/* Visited-pair memo for the structural diff (audit AH-1): makes the diff
 * O(distinct changed pairs) on SHARED DAGs instead of O(paths) - structural
 * sharing creates fan-in, so a shared changed subtree would otherwise be
 * re-descended once per path (up to MAX_CHILDREN^depth). Static => single-thread
 * (// not implemented: concurrency); cap-bounded => fail-closed (TOO_MANY) on overflow. */
static cvsasx_oid_t g_seen_a[CVSASX_SLS_DIFF_MAX];
static cvsasx_oid_t g_seen_b[CVSASX_SLS_DIFF_MAX];

static void sls_bcopy(unsigned char *d, const unsigned char *s, uint32_t n){
    for (uint32_t i = 0; i < n; ++i) d[i] = s[i];
}

void cvsasx_sls_init(cvsasx_sls_t *s, cvsasx_store_t *store){
    if (s){ s->store = store; s->get_calls = 0; }
}

cvsasx_sls_status_t cvsasx_sls_put_leaf(cvsasx_sls_t *s, const void *data, uint32_t len,
                                        cvsasx_oid_t *out){
    if (s == 0 || out == 0 || (data == 0 && len != 0)) return CVSASX_SLS_NULL_ARG;
    if (len > CVSASX_SLS_MAX_LEAF) return CVSASX_SLS_TOO_MANY;
    unsigned char buf[1u + CVSASX_SLS_MAX_LEAF];
    buf[0] = (unsigned char)CVSASX_SLS_LEAF;                 /* domain separation */
    sls_bcopy(buf + 1, (const unsigned char *)data, len);
    cvsasx_hash_t h;                                         /* // HOT-PATH: hash on write */
    if (cvsasx_store_put(s->store, buf, (size_t)(1u + len), &h) != CVSASX_STORE_OK)
        return CVSASX_SLS_STORE_FULL;
    *out = h;                                                /* oid == content hash */
    return CVSASX_SLS_OK;
}

cvsasx_sls_status_t cvsasx_sls_put_node(cvsasx_sls_t *s, const cvsasx_oid_t *children,
                                        uint32_t n, cvsasx_oid_t *out){
    if (s == 0 || out == 0 || (children == 0 && n != 0)) return CVSASX_SLS_NULL_ARG;
    if (n > CVSASX_SLS_MAX_CHILDREN) return CVSASX_SLS_TOO_MANY;
    unsigned char buf[5u + CVSASX_SLS_MAX_CHILDREN * CVSASX_HASH_LEN];
    buf[0] = (unsigned char)CVSASX_SLS_NODE;
    buf[1] = (unsigned char)(n & 0xffu);
    buf[2] = (unsigned char)((n >> 8) & 0xffu);
    buf[3] = (unsigned char)((n >> 16) & 0xffu);
    buf[4] = (unsigned char)((n >> 24) & 0xffu);
    for (uint32_t c = 0; c < n; ++c)
        sls_bcopy(buf + 5u + c * HLEN, children[c].b, HLEN);
    cvsasx_hash_t h;                                         /* // HOT-PATH: hash on write */
    if (cvsasx_store_put(s->store, buf, (size_t)(5u + n * HLEN), &h) != CVSASX_STORE_OK)
        return CVSASX_SLS_STORE_FULL;
    *out = h;
    return CVSASX_SLS_OK;
}

cvsasx_sls_status_t cvsasx_sls_get(cvsasx_sls_t *s, const cvsasx_oid_t *oid,
                                   const void **bytes, size_t *len){
    if (s == 0 || oid == 0 || bytes == 0 || len == 0) return CVSASX_SLS_NULL_ARG;
    s->get_calls++;
    if (cvsasx_store_get(s->store, oid, bytes, len) != CVSASX_STORE_OK) return CVSASX_SLS_MISS;
    return CVSASX_SLS_OK;
}

/* ENFORCED external object access (audit AM-5): resolve an oid and mint a backend
 * load-only reference. cvsasx_sls_get returns a const VIEW for TRUSTED internal
 * spine use (the diff); EXTERNAL consumers must use this, which on CHERI yields a
 * load-only capability (writes trap) - the C `const` alone is not enforcement.
 * NOTE: this FORWARDS to the backend; sls.c itself stays CHERI-free (// SEAM). */
cvsasx_sls_status_t cvsasx_sls_obj_grant(cvsasx_sls_t *s, cvsasx_sls_backend_t *be,
                                         const cvsasx_oid_t *oid, void **out_ref, size_t *out_len){
    if (s == 0 || be == 0 || oid == 0 || out_ref == 0 || out_len == 0) return CVSASX_SLS_NULL_ARG;
    const void *b; size_t l;
    cvsasx_sls_status_t st = cvsasx_sls_get(s, oid, &b, &l);
    if (st != CVSASX_SLS_OK) return st;
    *out_ref = be->obj_grant(be, (void *)b, (uint32_t)l);   /* backend enforces read-only */
    *out_len = l;
    return CVSASX_SLS_OK;
}

/* ---- structural diff (the load-bearing novelty) ------------------------- */
typedef struct diff_ctx {
    cvsasx_sls_t *s;
    cvsasx_oid_t *out; uint32_t cap; uint32_t n;
    uint32_t seen_n;
    uint64_t cmps;
    cvsasx_sls_status_t err;
} diff_ctx;

static void rec_change(diff_ctx *c, const cvsasx_oid_t *oid){
    for (uint32_t i = 0; i < c->n; ++i)                      /* dedup -> true SET (audit AH-3) */
        if (cvsasx_hash_eq(&c->out[i], oid)) return;
    if (c->n >= c->cap){ c->err = CVSASX_SLS_TOO_MANY; return; }
    c->out[c->n++] = *oid;
}

/* 1 if (A,B) already processed (skip); 0 if newly recorded. Fail-closed on overflow. */
static int seen_or_add(diff_ctx *c, const cvsasx_oid_t *A, const cvsasx_oid_t *B){
    for (uint32_t i = 0; i < c->seen_n; ++i)
        if (cvsasx_hash_eq(&g_seen_a[i], A) && cvsasx_hash_eq(&g_seen_b[i], B)) return 1;
    if (c->seen_n >= CVSASX_SLS_DIFF_MAX){ c->err = CVSASX_SLS_TOO_MANY; return 1; }
    g_seen_a[c->seen_n] = *A; g_seen_b[c->seen_n] = *B; c->seen_n++;
    return 0;
}

static void diff_rec(diff_ctx *c, const cvsasx_oid_t *A, const cvsasx_oid_t *B, uint32_t depth){
    if (c->err != CVSASX_SLS_OK) return;
    c->cmps++;
    if (cvsasx_hash_eq(A, B)) return;                        /* PRUNE: identical subtree */
    if (depth >= CVSASX_SLS_MAX_DEPTH){ c->err = CVSASX_SLS_TOO_DEEP; return; }
    /* MEMO (audit AH-1): process each DISTINCT (A,B) pair ONCE, so a shared changed
     * subtree reached via many parents costs O(1), not O(paths) -> diff is O(changed). */
    if (seen_or_add(c, A, B)) return;

    const void *sa, *sb; size_t la, lb;
    if (cvsasx_sls_get(c->s, A, &sa, &la) != CVSASX_SLS_OK){ c->err = CVSASX_SLS_MISS; return; }
    if (cvsasx_sls_get(c->s, B, &sb, &lb) != CVSASX_SLS_OK){ c->err = CVSASX_SLS_MISS; return; }
    const unsigned char *pa = (const unsigned char *)sa, *pb = (const unsigned char *)sb;
    if (la < 1 || lb < 1){ c->err = CVSASX_SLS_MALFORMED; return; }

    if (pa[0] == CVSASX_SLS_NODE && pb[0] == CVSASX_SLS_NODE){
        if (la < 5 || lb < 5){ c->err = CVSASX_SLS_MALFORMED; return; }
        uint32_t na = (uint32_t)pa[1] | ((uint32_t)pa[2]<<8) | ((uint32_t)pa[3]<<16) | ((uint32_t)pa[4]<<24);
        uint32_t nb = (uint32_t)pb[1] | ((uint32_t)pb[2]<<8) | ((uint32_t)pb[3]<<16) | ((uint32_t)pb[4]<<24);
        /* fail-closed on a malformed/oversized node so child reads cannot run OOB */
        if (na > CVSASX_SLS_MAX_CHILDREN || nb > CVSASX_SLS_MAX_CHILDREN){ c->err = CVSASX_SLS_MALFORMED; return; }
        if (la != (size_t)(5u + na*HLEN) || lb != (size_t)(5u + nb*HLEN)){ c->err = CVSASX_SLS_MALFORMED; return; }
        if (na != nb){ rec_change(c, B); return; }           /* shape change: whole subtree changed */
        for (uint32_t i = 0; i < na; ++i){
            cvsasx_oid_t ca, cb;
            sls_bcopy(ca.b, pa + 5u + i*HLEN, HLEN);
            sls_bcopy(cb.b, pb + 5u + i*HLEN, HLEN);
            diff_rec(c, &ca, &cb, depth + 1u);
            if (c->err != CVSASX_SLS_OK) return;
        }
    } else {
        rec_change(c, B);                                    /* leaf (or kind) change */
    }
}

cvsasx_sls_status_t cvsasx_sls_diff(cvsasx_sls_t *s,
                                    const cvsasx_oid_t *rootA, const cvsasx_oid_t *rootB,
                                    cvsasx_oid_t *out_changed, uint32_t cap, uint32_t *out_n,
                                    uint64_t *out_gets, uint64_t *out_cmps){
    if (s == 0 || rootA == 0 || rootB == 0 || out_changed == 0 || out_n == 0)
        return CVSASX_SLS_NULL_ARG;
    uint64_t g0 = s->get_calls;
    diff_ctx c;
    c.s = s; c.out = out_changed; c.cap = cap; c.n = 0; c.seen_n = 0; c.cmps = 0; c.err = CVSASX_SLS_OK;
    diff_rec(&c, rootA, rootB, 0);
    *out_n = c.n;
    if (out_gets) *out_gets = s->get_calls - g0;             /* objects RESOLVED - O(changed) */
    if (out_cmps) *out_cmps = c.cmps;                        /* hash comparisons (prune checks) */
    return c.err;
}

/* ===========================================================================
 * MARKER SUMMARY
 *   // SEAM: no CHERI builtin here - backend-agnostic spine (the CARMIX port enabler).
 *   // HOT-PATH: hashing happens on WRITE only (store_put inside put_leaf/put_node);
 *               the DIFF re-hashes nothing (pure hash-compare + O(changed) resolves).
 *   // INV: content-addressing => acyclic graph (a hash cannot contain itself).
 *   // not implemented: NVM persistence; GC/eviction of unreachable objects; concurrent writers;
 *           full hash==capability==live-reference namespace unification.
 * ===========================================================================*/
