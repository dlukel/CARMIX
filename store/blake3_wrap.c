/* ===========================================================================
 * CV-SASX  -  store/blake3_wrap.c
 * Thin wrapper over the vendored BLAKE3 reference: the single hashing entry
 * point for store/. Called only at commit / epoch boundaries - never on the
 * execution hot path (Master Prompt 3.1).
 * ===========================================================================*/
#include "cvsasx_store.h"
#include "blake3.h"   /* vendored reference, store/blake3/UPSTREAM.txt */

void cvsasx_blake3(const void *bytes, size_t len, cvsasx_hash_t *out) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes, len);
    blake3_hasher_finalize(&h, out->b, CVSASX_HASH_LEN);
}

/* Domain-separated hash: BLAKE3(tag-byte || bytes). The epoch tree uses distinct
 * tags for leaf chunks vs internal nodes so a chunk cannot impersonate a node
 * (second-preimage resistance for the epoch root). */
void cvsasx_blake3_tagged(uint8_t tag, const void *bytes, size_t len, cvsasx_hash_t *out) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, &tag, 1);
    blake3_hasher_update(&h, bytes, len);
    blake3_hasher_finalize(&h, out->b, CVSASX_HASH_LEN);
}
