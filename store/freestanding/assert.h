/* Freestanding <assert.h> shim for the store/ module.
 * The store build uses -DNDEBUG so the vendored BLAKE3's assert()s are no-ops;
 * this header exists only so `#include <assert.h>` resolves under -nostdlib
 * (there is no libc to provide it, and no hosted abort path).
 * BLAKE3 correctness is established independently by the official known-answer
 * vectors (test S0), not by these asserts.
 */
#ifndef CVSASX_FREESTANDING_ASSERT_H
#define CVSASX_FREESTANDING_ASSERT_H
#undef assert
#define assert(e) ((void)0)
#endif
