/* ===========================================================================
 * CV-SASX  -  store/store_mem.c
 * Freestanding mem* and strlen used by the vendored BLAKE3 reference + store code.
 * `-nostdlib` provides no libc; these are auditable byte-loops.
 *
 * Each is __attribute__((optnone)) so LLVM's LoopIdiomRecognize cannot rewrite
 * the loops back into calls to memcpy/memset (which would recurse infinitely /
 * blow the stack). This is defense-in-depth ALONGSIDE the build's -fno-builtin -
 * correctness no longer depends on a flag the file can't see (audit M3).
 *
 * Not TCB in the capability sense (no authority), but correctness-relevant:
 * BLAKE3 relies on memcpy/memset being correct. Validated transitively by the
 * BLAKE3 known-answer vectors (test S0).
 * ===========================================================================*/
#include <stddef.h>

#define CVSASX_NORECURSE __attribute__((optnone))

CVSASX_NORECURSE void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

CVSASX_NORECURSE void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)c;
    return dst;
}

CVSASX_NORECURSE void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
    else       { for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1]; }
    return dst;
}

CVSASX_NORECURSE int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

CVSASX_NORECURSE size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
