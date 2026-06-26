/* Minimal freestanding header for the vendored tweetnacl ref Ed25519.
 * Source: github.com/dominictarr/tweetnacl @ master (NaCl "tweet" by D.J. Bernstein
 * et al.). The kernel uses ONLY crypto_sign (A) and crypto_sign_open (B); keypair
 * generation is host-only (commented out in kernel/tweetnacl.c). No libc, no SIMD,
 * no randombytes needed for sign/verify. See TWEETNACL_PROVENANCE.txt. */
#ifndef CARMIX_TWEETNACL_H
#define CARMIX_TWEETNACL_H

#define crypto_sign_BYTES            64u   /* detached signature length */
#define crypto_sign_PUBLICKEYBYTES   32u
#define crypto_sign_SECRETKEYBYTES   64u

/* sm = sig(64) || m ; *smlen = n + 64. Returns 0. */
int crypto_sign(unsigned char *sm, unsigned long long *smlen,
                const unsigned char *m, unsigned long long n, const unsigned char *sk);
/* Verifies sm under pk; on success writes the recovered message to m and *mlen.
 * Returns 0 if the signature is valid, -1 otherwise. */
int crypto_sign_open(unsigned char *m, unsigned long long *mlen,
                     const unsigned char *sm, unsigned long long n, const unsigned char *pk);

#endif
