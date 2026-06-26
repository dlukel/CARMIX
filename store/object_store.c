/* ===========================================================================
 * CV-SASX  -  store/object_store.c
 * Content-addressed, immutable, deduplicating object store (Master Prompt L3).
 * In-memory fixed arena (no malloc): the caller supplies arena + index storage.
 *
 * Invariants:
 *   - Immutable: a stored object is never moved or overwritten. "Modifying"
 *     means put()ing new bytes, which get a new hash and a new arena slot.
 *   - Dedup: identical bytes => identical BLAKE3 hash => stored once. A repeat
 *     put() returns the existing hash and increments dedup_hits, storing nothing.
 *   - Content-addressed: the only key is the BLAKE3 hash; no paths, no overwrite.
 * ===========================================================================*/
#include "cvsasx_store.h"

int cvsasx_hash_eq(const cvsasx_hash_t *a, const cvsasx_hash_t *b) {
    unsigned diff = 0;
    for (unsigned i = 0; i < CVSASX_HASH_LEN; ++i)
        diff |= (unsigned)(a->b[i] ^ b->b[i]);
    return diff == 0;
}

void cvsasx_store_init(cvsasx_store_t *s,
                       uint8_t *arena, uint64_t arena_cap,
                       cvsasx_store_entry_t *index, uint64_t index_cap) {
    s->arena = arena; s->arena_cap = arena_cap; s->arena_used = 0;
    s->index = index; s->index_cap = index_cap; s->index_count = 0;
    s->put_calls = 0; s->dedup_hits = 0; s->bytes_stored = 0;
}

/* Locate an object by hash. Linear scan.
 * // not implemented (perf, not correctness): O(n) lookup; a hash-keyed open-addressing
 * index would make put/get O(1). Dedup correctness does not depend on it. */
static cvsasx_store_entry_t *find_entry(cvsasx_store_t *s, const uint8_t *hash) {
    for (uint64_t i = 0; i < s->index_count; ++i) {
        unsigned diff = 0;
        for (unsigned j = 0; j < CVSASX_HASH_LEN; ++j)
            diff |= (unsigned)(s->index[i].hash[j] ^ hash[j]);
        if (diff == 0) return &s->index[i];
    }
    return (cvsasx_store_entry_t *)0;
}

cvsasx_store_status_t cvsasx_store_put(cvsasx_store_t *s,
                                       const void *bytes, size_t len,
                                       cvsasx_hash_t *out_hash) {
    if (s == 0 || (bytes == 0 && len != 0)) return CVSASX_STORE_NULL_ARG;
    s->put_calls++;

    cvsasx_hash_t h;
    cvsasx_blake3(bytes, len, &h);          /* hashing: commit path, NOT hot path */

    if (find_entry(s, h.b)) {               /* already stored -> dedup, no 2nd copy.
                                             * // VERIFY: dedup correctness is delegated to
                                             * BLAKE3 collision-resistance (proven by S0);
                                             * no byte re-compare on a hit. */
        s->dedup_hits++;
        if (out_hash) *out_hash = h;        /* out_hash set only on OK (not on FULL) */
        return CVSASX_STORE_OK;
    }
    if (s->index_count >= s->index_cap)            return CVSASX_STORE_FULL;
    if (len > s->arena_cap - s->arena_used)        return CVSASX_STORE_FULL;

    uint8_t *dst = s->arena + s->arena_used;       /* append; never overwrite */
    const uint8_t *src = (const uint8_t *)bytes;
    for (size_t i = 0; i < len; ++i) dst[i] = src[i];

    cvsasx_store_entry_t *e = &s->index[s->index_count];
    for (unsigned j = 0; j < CVSASX_HASH_LEN; ++j) e->hash[j] = h.b[j];
    e->offset = s->arena_used;
    e->len = (uint64_t)len;
    s->index_count++;
    s->arena_used += (uint64_t)len;
    s->bytes_stored += (uint64_t)len;
    if (out_hash) *out_hash = h;            /* out_hash set only when object is resident */
    return CVSASX_STORE_OK;
}

cvsasx_store_status_t cvsasx_store_get(cvsasx_store_t *s,
                                       const cvsasx_hash_t *h,
                                       const void **out_bytes, size_t *out_len) {
    if (s == 0 || h == 0) return CVSASX_STORE_NULL_ARG;
    cvsasx_store_entry_t *e = find_entry(s, h->b);
    if (e == 0) return CVSASX_STORE_MISS;
    if (out_bytes) *out_bytes = s->arena + e->offset;   /* read-only alias */
    if (out_len)   *out_len   = (size_t)e->len;
    return CVSASX_STORE_OK;
}

int cvsasx_store_exists(cvsasx_store_t *s, const cvsasx_hash_t *h) {
    if (s == 0 || h == 0) return 0;
    return find_entry(s, h->b) != (cvsasx_store_entry_t *)0;
}

/* ===========================================================================
 * MARKER SUMMARY
 *   // not implemented: O(n) linear index lookup - hash-keyed index is a perf optimization.
 *   // not implemented: eviction / GC of unreferenced objects - not implemented.
 *   // not implemented: on-disk persistence - in-memory arena only.
 *   // not implemented: concurrency - single-threaded assumption (no locks).
 *   // HOT-PATH note: store put/get are COMMIT-path ops; the execution hot path
 *           (linear-memory mutation) lives in epoch_tree.c and does NO hashing.
 * ===========================================================================*/
