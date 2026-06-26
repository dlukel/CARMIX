/* ===========================================================================
 * CV-SASX  -  store/epoch_tree.c
 * Diff-proportional epoch commits via a chunk-granular, domain-separated Merkle
 * tree (Part 3.2/3.3). A commit re-hashes only chunks dirtied since the last
 * commit, then walks up recomputing only internal nodes whose subtree contains
 * a dirty leaf. Clean subtrees keep their hashes => the incremental root equals
 * a from-scratch full recompute, at cost proportional to the diff.
 *
 * Fail-closed: over-capacity init and out-of-range writes are rejected (no OOB).
 * ===========================================================================*/
#include "epoch_tree.h"

static uint32_t next_pow2(uint32_t x) {
    uint32_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

int cvsasx_epoch_init(cvsasx_epoch_t *e, uint8_t *mem, uint64_t mem_len) {
    e->mem = mem;
    e->mem_len = mem_len;
    e->committed = 0;
    e->valid = 0;
    e->last_leaf_hashes = 0;
    e->last_node_hashes = 0;
    uint32_t nc = (uint32_t)((mem_len + CVSASX_EPOCH_CHUNK_SIZE - 1) / CVSASX_EPOCH_CHUNK_SIZE);
    if (nc == 0) nc = 1;
    uint32_t nl = next_pow2(nc);
    if (nl > CVSASX_EPOCH_MAX_CHUNKS) {     /* fail-closed: would overflow node[]/leaf_dirty[] */
        e->n_chunks = 0;
        e->n_leaves = 0;
        return 0;                           /* valid stays 0 */
    }
    e->n_chunks = nc;
    e->n_leaves = nl;
    for (uint32_t i = 0; i < nc; ++i) e->leaf_dirty[i] = 1;   /* only real chunks dirty */
    for (uint32_t i = nc; i < nl; ++i) e->leaf_dirty[i] = 0;  /* padding leaves not dirty */
    for (uint32_t i = 0; i < 2u * nl; ++i)
        for (unsigned j = 0; j < CVSASX_HASH_LEN; ++j) e->node[i].b[j] = 0;
    e->valid = 1;
    return 1;
}

/* // HOT-PATH: no hashing, no commit. Fail-closed on out-of-range / invalid. */
void cvsasx_epoch_write(cvsasx_epoch_t *e, uint64_t offset, const void *data, size_t len) {
    if (!e->valid || len == 0) return;
    if (offset > e->mem_len || (uint64_t)len > e->mem_len - offset) return;  /* bounds */
    uint8_t *dst = e->mem + offset;
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) dst[i] = src[i];        /* mutate linear memory */
    uint64_t first = offset / CVSASX_EPOCH_CHUNK_SIZE;
    uint64_t last  = (offset + len - 1) / CVSASX_EPOCH_CHUNK_SIZE;
    for (uint64_t c = first; c <= last && c < e->n_chunks; ++c)
        e->leaf_dirty[c] = 1;                                /* mark real chunks; NO hash */
}

void cvsasx_epoch_commit(cvsasx_epoch_t *e, cvsasx_hash_t *out_root) {
    if (!e->valid) return;
    const uint64_t L = e->n_leaves;
    uint8_t changed[2u * CVSASX_EPOCH_MAX_CHUNKS];
    for (uint64_t i = 0; i < 2u * L; ++i) changed[i] = 0;
    e->last_leaf_hashes = 0;
    e->last_node_hashes = 0;

    /* 1. Re-hash dirty leaf chunks (domain tag LEAF). */
    for (uint32_t c = 0; c < e->n_chunks; ++c) {
        if (e->leaf_dirty[c]) {
            uint64_t off  = (uint64_t)c * CVSASX_EPOCH_CHUNK_SIZE;
            uint64_t clen = CVSASX_EPOCH_CHUNK_SIZE;
            if (off + clen > e->mem_len) clen = e->mem_len - off;  /* short last chunk */
            cvsasx_blake3_tagged(CVSASX_EPOCH_TAG_LEAF, e->mem + off, (size_t)clen, &e->node[L + c]);
            e->leaf_dirty[c] = 0;
            changed[L + c] = 1;
            e->last_leaf_hashes++;
        }
    }
    /* Padding leaves (structural): hash the empty input once at first commit. */
    if (!e->committed) {
        for (uint32_t c = e->n_chunks; c < L; ++c) {
            cvsasx_blake3_tagged(CVSASX_EPOCH_TAG_LEAF, e->mem, 0, &e->node[L + c]);
            changed[L + c] = 1;
        }
    }

    /* 2. Recompute internal nodes whose children changed, bottom-up (domain tag
     * NODE). Only nodes on the path(s) from dirty leaves to the root are touched. */
    for (uint64_t k = L - 1; k >= 1; --k) {
        if (changed[2 * k] || changed[2 * k + 1]) {
            uint8_t pair[2u * CVSASX_HASH_LEN];
            for (unsigned j = 0; j < CVSASX_HASH_LEN; ++j) {
                pair[j]                   = e->node[2 * k].b[j];
                pair[CVSASX_HASH_LEN + j] = e->node[2 * k + 1].b[j];
            }
            cvsasx_blake3_tagged(CVSASX_EPOCH_TAG_NODE, pair, sizeof pair, &e->node[k]);
            changed[k] = 1;
            e->last_node_hashes++;
        }
        if (k == 1) break;   /* avoid unsigned wrap past 0 (no internal node 0) */
    }

    e->committed = 1;
    /* node[1] is the root for all valid trees (L>=1); for L==1 it equals the sole
     * leaf hash (documented convention; see L7 in the audit). */
    if (out_root) *out_root = e->node[1];
}

/* ===========================================================================
 * MARKER SUMMARY
 *   // HOT-PATH: cvsasx_epoch_write does NO hashing (mutate + mark dirty only),
 *             and is fail-closed (bounds + valid checks).
 *   // TUNABLE: CVSASX_EPOCH_CHUNK_SIZE (4096) - not asserted optimal (3.2/3.5).
 *   // SECURITY: leaf/internal domain separation (TAG_LEAF/TAG_NODE).
 *   // VERIFIED: incremental root == full recompute (audit model: 2160 randomized
 *             commits, 0 divergences; on-target S4 + S4b).
 *   // not implemented:    epoch-node on-store serialization (root + unchanged-subtree refs +
 *             new subtrees) - detailed layout is migrate/'s concern.
 *   // not implemented:    memories needing > MAX_CHUNKS leaves are rejected (fail-closed);
 *             multi-tree / dynamic sizing is future work.
 * ===========================================================================*/
