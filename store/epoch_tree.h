/* ===========================================================================
 * CV-SASX  -  store/epoch_tree.h
 * The epoch tree (Master Prompt 3.2 / 3.3): a chunk-granular Merkle tree over a
 * task's linear memory that makes commits DIFF-PROPORTIONAL - a commit re-hashes
 * only dirty chunks plus the path to the root; unchanged subtrees keep their
 * prior hashes.
 *
 * MODULE:  store/  ·  DEPENDS ON LAWS: L3 (content-addressed), Part 3 (epochs).
 *
 * HOT PATH: cvsasx_epoch_write() mutates linear memory + marks chunks dirty -
 *           NO hashing (Master Prompt 3.1). All hashing is in commit().
 *
 * Chunk granularity is a TUNABLE (3.2/3.5): finer = less data hashed per commit
 * but more dirty-tracking + tree nodes. NOT asserted optimal - benchmark.
 *
 * SECURITY: leaf and internal nodes are DOMAIN-SEPARATED (distinct 1-byte
 * prefixes) so a chunk's bytes cannot impersonate an internal node - the epoch
 * root is second-preimage resistant, not just collision-resistant on content.
 * ===========================================================================*/
#ifndef CVSASX_EPOCH_TREE_H
#define CVSASX_EPOCH_TREE_H

#include "cvsasx_store.h"

#ifndef CVSASX_EPOCH_CHUNK_SIZE
#define CVSASX_EPOCH_CHUNK_SIZE 4096u      /* TUNABLE - not asserted optimal */
#endif
#ifndef CVSASX_EPOCH_MAX_CHUNKS
#define CVSASX_EPOCH_MAX_CHUNKS 256u       /* power of 2; sizes the static tree */
#endif

/* Domain-separation tags (RFC 6962 style). */
#define CVSASX_EPOCH_TAG_LEAF 0x00u
#define CVSASX_EPOCH_TAG_NODE 0x01u

typedef struct cvsasx_epoch {
    uint8_t  *mem;
    uint64_t  mem_len;
    uint32_t  n_chunks;      /* ceil(mem_len / CHUNK_SIZE) */
    uint32_t  n_leaves;      /* n_chunks padded up to a power of two */
    /* binary Merkle tree, heap layout (1-indexed): node[1]=root, children of k
     * are 2k,2k+1, leaves at [n_leaves .. 2*n_leaves-1]. */
    cvsasx_hash_t node[2u * CVSASX_EPOCH_MAX_CHUNKS];
    uint8_t   leaf_dirty[CVSASX_EPOCH_MAX_CHUNKS];
    uint8_t   committed;
    uint8_t   valid;         /* 0 if init rejected (over capacity); write/commit no-op */
    uint64_t  last_leaf_hashes;
    uint64_t  last_node_hashes;
} cvsasx_epoch_t;

/* Initialize over linear memory. Returns 1 on success, 0 (and e->valid=0) if the
 * memory needs more than CVSASX_EPOCH_MAX_CHUNKS leaves (fail-closed; no OOB). */
int cvsasx_epoch_init(cvsasx_epoch_t *e, uint8_t *mem, uint64_t mem_len);

/* // HOT-PATH: no hashing. PRECONDITION offset+len <= mem_len; an out-of-range
 * write, a zero-length write, or a write to an invalid epoch is a fail-closed
 * no-op (no OOB, no dirty-set change). Dirty-tracking covers only real chunks. */
void cvsasx_epoch_write(cvsasx_epoch_t *e, uint64_t offset, const void *data, size_t len);

/* Commit: re-hash ONLY dirty chunks + the internal nodes on their paths to the
 * root; clean subtrees keep their hashes. No-op on an invalid epoch. The
 * incremental root equals the root a full recompute of all current chunks would
 * produce. last_leaf_hashes / last_node_hashes record the work done. */
void cvsasx_epoch_commit(cvsasx_epoch_t *e, cvsasx_hash_t *out_root);

#endif /* CVSASX_EPOCH_TREE_H */
