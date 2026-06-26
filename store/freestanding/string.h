/* Freestanding <string.h> shim for the store/ module.
 * `-nostdlib` provides no libc <string.h>. The vendored BLAKE3 reference uses
 * only memcpy/memset (and strlen, on the derive-key path cvsasx_blake3 never
 * calls); store/ code also uses memcmp; memmove is provided for general use.
 * These are DEFINED in store_mem.c (optnone + -fno-builtin so they don't recurse). Keeping
 * the BLAKE3 sources unmodified (they `#include <string.h>`) preserves the
 * "matches the official reference" property - this shim just satisfies the
 * include under -nostdlib.
 * // VERIFY: that CHERI-LLVM does not already provide a freestanding <string.h>
 *           that would shadow this (it does not in -nostdlib; confirmed by build).
 */
#ifndef CVSASX_FREESTANDING_STRING_H
#define CVSASX_FREESTANDING_STRING_H
#include <stddef.h>
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);   /* used by BLAKE3's derive-key path */
#endif
