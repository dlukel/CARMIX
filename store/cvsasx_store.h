/* ===========================================================================
 * CV-SASX  -  store/cvsasx_store.h
 * The content-addressed object store: the cross-module contract (Master Prompt
 * L3 + Part 3). Present a BLAKE3 hash, get the bytes (or a MISS). Identical
 * content => one hash => one stored copy (dedup). No paths, no keys, no
 * overwrite; "modifying" means producing a NEW object with a NEW hash.
 *
 * MODULE:        store/  ·  DEPENDS ON LAWS:
 *   L3  content-addressed, immutable objects keyed by BLAKE3 hash.
 *
 * TCB STATUS:    The hash function (BLAKE3) is correctness-AND-safety-critical:
 *                a wrong digest breaks dedup AND lets two distinct objects
 *                collide to one address. Established against the OFFICIAL
 *                known-answer vectors (test S0), not asserted.
 *
 * DIGEST LENGTH AGREEMENT (closes the open // VERIFY: in cap/cvsasx_pir.h):
 *   The content address is the **32-byte** BLAKE3-256 digest. cap/'s
 *   CVSASX_BLAKE3_LEN (32) and store/'s CVSASX_HASH_LEN (32) are the SAME width
 *   by this cross-module agreement. // VERIFIED: BLAKE3 default digest = 32 B.
 *
 * Freestanding: <stdint.h>/<stddef.h> only (freestanding-mandated headers).
 * ===========================================================================*/
#ifndef CVSASX_STORE_H
#define CVSASX_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BLAKE3-256 content address. Same 32-byte width as cap/'s CVSASX_BLAKE3_LEN. */
#define CVSASX_HASH_LEN 32u
typedef struct cvsasx_hash { uint8_t b[CVSASX_HASH_LEN]; } cvsasx_hash_t;

/* This module targets LP64 / l64pc128 (size_t == uint64_t); the put() capacity
 * arithmetic and offset/len widths assume it. // VERIFY: holds for the target. */
_Static_assert(sizeof(size_t) == sizeof(uint64_t), "store/ assumes 64-bit size_t (l64pc128)");

typedef enum cvsasx_store_status {
    CVSASX_STORE_OK = 0,
    CVSASX_STORE_MISS,        /* get/exists: hash not present */
    CVSASX_STORE_FULL,        /* put: arena or index capacity exhausted */
    CVSASX_STORE_NULL_ARG
} cvsasx_store_status_t;

/* --- BLAKE3 hashing primitive (the only hashing entry point) ---------------
 * Off the execution hot path (Master Prompt 3.1): callers hash only at
 * commit / epoch boundaries, never inside execution loops. */
void cvsasx_blake3(const void *bytes, size_t len, cvsasx_hash_t *out);

/* Domain-separated hash: BLAKE3(tag || bytes). Used by the epoch tree. */
void cvsasx_blake3_tagged(uint8_t tag, const void *bytes, size_t len, cvsasx_hash_t *out);

/* --- object-store types ----------------------------------------------------
 * In-memory, fixed-arena (no malloc): the caller supplies the storage arena and
 * the index array at init. Each unique object is stored once; the index maps a
 * hash to (offset,len) within the arena. */
typedef struct cvsasx_store_entry {
    uint8_t  hash[CVSASX_HASH_LEN];
    uint64_t offset;   /* byte offset of the object within the arena */
    uint64_t len;      /* object length in bytes */
} cvsasx_store_entry_t;

typedef struct cvsasx_store {
    uint8_t              *arena;        /* object byte storage */
    uint64_t              arena_cap;
    uint64_t              arena_used;
    cvsasx_store_entry_t *index;        /* hash -> (offset,len) */
    uint64_t              index_cap;
    uint64_t              index_count;  /* number of DISTINCT objects stored */
    /* accounting (for dedup / immutability tests; not authoritative state) */
    uint64_t              put_calls;
    uint64_t              dedup_hits;   /* put()s that matched an existing object */
    uint64_t              bytes_stored; /* total distinct bytes written to arena */
} cvsasx_store_t;

/* Initialize a store over caller-provided arena + index storage. */
void cvsasx_store_init(cvsasx_store_t *s,
                       uint8_t *arena, uint64_t arena_cap,
                       cvsasx_store_entry_t *index, uint64_t index_cap);

/* put: idempotent. Hash the bytes; if already present, return its hash WITHOUT
 * storing a second copy (dedup); else append to the arena and index it.
 * *out_hash always receives the content address on OK. */
cvsasx_store_status_t cvsasx_store_put(cvsasx_store_t *s,
                                       const void *bytes, size_t len,
                                       cvsasx_hash_t *out_hash);

/* get: return a (read-only) view of the stored bytes for a hash, or MISS.
 * The returned pointer aliases the arena; objects are never mutated in place. */
cvsasx_store_status_t cvsasx_store_get(cvsasx_store_t *s,
                                       const cvsasx_hash_t *h,
                                       const void **out_bytes, size_t *out_len);

/* exists: 1 if the hash is present, 0 otherwise. */
int cvsasx_store_exists(cvsasx_store_t *s, const cvsasx_hash_t *h);

/* small helpers (no hashing) */
int cvsasx_hash_eq(const cvsasx_hash_t *a, const cvsasx_hash_t *b);

#ifdef __cplusplus
}
#endif

/* ===========================================================================
 * MARKER SUMMARY
 *   // VERIFIED: content address = 32-byte BLAKE3-256 (official default); equals
 *             cap/ CVSASX_BLAKE3_LEN - closes cap/cvsasx_pir.h's open VERIFY.
 *   // VERIFY: vendored BLAKE3 matches upstream (store/blake3/UPSTREAM.txt commit);
 *             correctness proven by official vectors (S0).
 *   // not implemented:   eviction / GC of unreferenced objects - not implemented (named).
 *   // not implemented:   on-disk persistence vs in-memory arena - in-memory only here.
 *   // not implemented:   concurrency / thread-safety - single-threaded assumption.
 *   // not implemented:   cross-machine object replication is migrate/'s concern, not store/.
 *   // TCB:    BLAKE3 (collision/dedup-critical) - validated by S0 vectors.
 * ===========================================================================*/
#endif /* CVSASX_STORE_H */
