/* ===========================================================================
 * CV-SASX  -  sls/cvsasx_sls.h
 * The content-addressed SINGLE-LEVEL STORE: one object space where an object's
 * IDENTITY IS THE BLAKE3 HASH OF ITS CONTENT. Mutation never edits in place - it
 * produces a NEW object/epoch with STRUCTURAL SHARING (unchanged sub-objects keep
 * their hashes). The object graph is a Merkle structure, so the DIFF between two
 * epochs is exactly the set of changed objects, computed in O(changed) by pruning
 * equal-hash subtrees - a ZERO-COMPUTE structural property, NOT a dedup pass.
 *
 * THE SPINE: scheduler, fault-tolerance, and the eventual GUI sit on this module.
 *
 * NOVELTY (the exact sentence this module must defend): "diff-proportional
 * migration and monotonic capability re-minting emerge as ZERO-COMPUTE STRUCTURAL
 * CONSEQUENCES of executing live state within a content-addressed immutable memory
 * model." The structural diff (cvsasx_sls_diff) IS that mechanism: it re-hashes
 * NOTHING and scans only O(changed) nodes - contrast the dedup-PASS prior art
 * (Sapuntzakis OSDI'02, Shrinker Euro-Par'11, Medes EuroSys'22, CRIU, KSM), which
 * scan/re-hash at migration time.
 *
 * DIFFERENTIATION (// REF, recorded in VERIFICATION_LOG):
 *   - Twizzler (ATC'20): single-level OBJECT space but OPAQUE/RANDOM IDs + MUTABLE
 *     objects. We differ: content-addressed identity (hash==identity) + immutability.
 *   - Unison: content-addresses LIVE state by syntax-tree hash, but it is a USERSPACE
 *     LANGUAGE RUNTIME. We force the OS MEMORY MODEL itself (objects + via the
 *     backend, capability state) into the content-addressed immutable structure.
 *   - Nix/IPFS: content-address STATIC artifacts; no live state, no capabilities.
 *
 * BACKEND-INDEPENDENCE (the commodity / CARMIX path): the store/Merkle/diff logic
 * here references NO CHERI builtin. Object-capability enforcement is delegated to a
 * PLUGGABLE backend (cvsasx_sls_backend) - CHERI now (sls/backend_cheri.c, the only
 * CHERI-touching file, // SEAM), software/Wasm on commodity x86-64/ARM later
 * (// not implemented: the CARMIX port). Same module, two backends - built once, not twice.
 *
 * Reuses store/ (content-addressed chunk store + BLAKE3) as the backing - hashing
 * is store/'s, NOT re-hand-rolled here. // not implemented: NVM persistence; GC/eviction of
 * unreachable objects; concurrent writers; full hash==cap==live-ref namespace.
 * Freestanding.
 * ===========================================================================*/
#ifndef CVSASX_SLS_H
#define CVSASX_SLS_H

#include <stdint.h>
#include <stddef.h>
#include "cvsasx_store.h"     /* store/: content-addressed store + BLAKE3 + hash type */

#ifdef __cplusplus
extern "C" {
#endif

#define CVSASX_SLS_LEAF        0x00u   /* domain tag: leaf (opaque data)            */
#define CVSASX_SLS_NODE        0x01u   /* domain tag: internal node (child hashes)  */
#define CVSASX_SLS_MAX_CHILDREN 16u    /* max node arity (record-size bound)        */
#define CVSASX_SLS_MAX_DEPTH    32u    /* recursion bound for diff (fail-closed)    */
#define CVSASX_SLS_MAX_LEAF     1024u  /* max leaf payload (serialization buffer)   */
#define CVSASX_SLS_DIFF_MAX     512u   /* max distinct changed (A,B) pairs per diff (memo bound) */

/* An object id IS its content hash. Identity == hash. No opaque IDs. */
typedef cvsasx_hash_t cvsasx_oid_t;

typedef enum cvsasx_sls_status {
    CVSASX_SLS_OK = 0,
    CVSASX_SLS_NULL_ARG,
    CVSASX_SLS_STORE_FULL,
    CVSASX_SLS_MISS,          /* oid not resident */
    CVSASX_SLS_MALFORMED,     /* a stored object's serialization is invalid (fail-closed) */
    CVSASX_SLS_TOO_MANY,      /* more children / changes than capacity */
    CVSASX_SLS_TOO_DEEP       /* diff recursion bound exceeded */
} cvsasx_sls_status_t;

/* ---------------------------------------------------------------------------
 * // SEAM: object-capability enforcement backend. The store/Merkle/diff logic
 * NEVER touches this (or any CHERI builtin); only a concrete backend does.
 * ------------------------------------------------------------------------- */
typedef struct cvsasx_sls_backend {
    const char *name;
    /* Mint an ENFORCED reference over an object instance [bytes, bytes+len).
     * CHERI: a bounded, LOAD-ONLY capability (objects are immutable). A software
     * backend: a bounds-checked accessor. */
    void *(*obj_grant)(struct cvsasx_sls_backend *self, void *bytes, uint32_t len);
} cvsasx_sls_backend_t;

typedef struct cvsasx_sls {
    cvsasx_store_t *store;     /* backing content-addressed store (store/) */
    uint64_t       get_calls;  /* cumulative resolve count (SL1 diff instrumentation) */
} cvsasx_sls_t;

void cvsasx_sls_init(cvsasx_sls_t *s, cvsasx_store_t *store);

/* Create a LEAF from opaque data. Identity == BLAKE3(0x00 ‖ data). // HOT-PATH (hash). */
cvsasx_sls_status_t cvsasx_sls_put_leaf(cvsasx_sls_t *s, const void *data, uint32_t len,
                                        cvsasx_oid_t *out);
/* Create a NODE from ordered child oids. Identity == BLAKE3(0x01 ‖ n ‖ children).
 * Unchanged children are SHARED by hash (store/ dedup) - structural sharing. // HOT-PATH. */
cvsasx_sls_status_t cvsasx_sls_put_node(cvsasx_sls_t *s, const cvsasx_oid_t *children,
                                        uint32_t n, cvsasx_oid_t *out);
/* Resolve an oid -> a const VIEW of the serialized bytes (aliases the store). For
 * TRUSTED internal/spine use (the diff). The `const` is a CONTRACT, not enforcement
 * - EXTERNAL consumers must use cvsasx_sls_obj_grant for an enforced (load-only on
 * CHERI) reference (audit AM-5). */
cvsasx_sls_status_t cvsasx_sls_get(cvsasx_sls_t *s, const cvsasx_oid_t *oid,
                                   const void **bytes, size_t *len);

/* ENFORCED external object access: resolve `oid` and mint a backend load-only
 * reference (*out_ref) over its bytes (*out_len). On CHERI a write through it TRAPS;
 * sls.c stays CHERI-free (it only FORWARDS to the backend). */
cvsasx_sls_status_t cvsasx_sls_obj_grant(cvsasx_sls_t *s, cvsasx_sls_backend_t *be,
                                         const cvsasx_oid_t *oid, void **out_ref, size_t *out_len);

/* THE STRUCTURAL DIFF (the load-bearing novelty): the changed-OBJECT oids between
 * epoch rootA and rootB, in time O(changed) - walks the Merkle structure, PRUNES
 * subtrees whose hash is unchanged (no descend, no re-hash), and MEMOISES visited
 * (A,B) pairs so a SHARED changed subtree (structural sharing => DAG fan-in) costs
 * O(1), not O(paths). The diff calls NO hashing and resolves only O(changed) objects.
 *   out_changed[0..cap) receives the DEDUPED set of changed OBJECT oids - usually a
 *     changed LEAF, but a SUBTREE-ROOT (node) oid on a shape/kind change (resolve +
 *     descend each). Sound for migration: transfer each changed object; the store
 *     prunes shared sub-objects by hash.
 *   *out_n the count; *out_gets objects RESOLVED (O(changed)); *out_cmps hash compares.
 * Returns TOO_MANY past `cap`/the memo bound; TOO_DEEP past MAX_DEPTH (fail-closed). */
cvsasx_sls_status_t cvsasx_sls_diff(cvsasx_sls_t *s,
                                    const cvsasx_oid_t *rootA, const cvsasx_oid_t *rootB,
                                    cvsasx_oid_t *out_changed, uint32_t cap, uint32_t *out_n,
                                    uint64_t *out_gets, uint64_t *out_cmps);

/* The CHERI object-capability backend (sls/backend_cheri.c). */
cvsasx_sls_backend_t *cvsasx_sls_backend_cheri(void);

#ifdef __cplusplus
}
#endif
#endif /* CVSASX_SLS_H */
