// crypto.h -- crypto primitives used by lib_tls.c (implemented in lib_crypto.c)
//
// SHA-256 comes from your sha256.h (sha256_ctx, sha256_init/update/final, sha256()).
// Everything else is declared here. All functions return 0 on success and
// nonzero on failure unless noted.
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include "sha256.h"

/* HMAC-SHA256. `out` must not alias `msg`. */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/* AES-128-GCM (12-byte nonce, 16-byte tag).
 * seal: writes pt_len + 16 bytes (ciphertext || tag) to `out`.
 * open: `ct` is ciphertext || tag (ct_len includes the tag); writes
 *       ct_len - 16 bytes to `out`; returns nonzero if the tag is wrong.
 * `out` must not overlap the input. */
int aes128gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *pt, size_t pt_len, uint8_t *out);
int aes128gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *ct, size_t ct_len, uint8_t *out);

/* Random bytes, read from $/dev/md64api/urandom. Fails closed: returns
 * nonzero if the node cannot be opened, is unseeded (no RDSEED/RDRAND on the
 * CPU; run QEMU with -cpu max), or returns a short read. There is no
 * fallback source. */
int random_bytes(void *buf, size_t len);

/* Key exchange.
 * x25519: private/public/shared are 32 bytes; shared() fails on an all-zero result.
 * p256:   private 32 bytes, public = 65-byte uncompressed point (04 || X || Y),
 *         shared = 32-byte X coordinate; shared() rejects points not on the curve.
 * (Scalar multiplication is not constant-time.) */
int x25519_keypair(uint8_t priv[32], uint8_t pub[32]);
int x25519_shared(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]);
int p256_keypair(uint8_t priv[32], uint8_t pub[65]);
int p256_shared(const uint8_t priv[32], const uint8_t peer[65], uint8_t out[32]);

/* X.509: just enough DER to get at the leaf's key and DNS names.
 * Pointers refer into the DER buffer, which must outlive the mcert_t. */
enum { MC_KEY_NONE = 0, MC_KEY_RSA = 1, MC_KEY_EC_P256 = 2, MC_KEY_EC_P384 = 3 };

/* Certificate signature algorithms understood by mcert_verify_issued_by().
 * (SHA-1, MD5 and RSASSA-PSS certificate signatures are deliberately rejected.) */
enum { MC_SIG_NONE = 0,
       MC_SIG_RSA_SHA256 = 1, MC_SIG_RSA_SHA384 = 2, MC_SIG_RSA_SHA512 = 3,
       MC_SIG_ECDSA_SHA256 = 4, MC_SIG_ECDSA_SHA384 = 5, MC_SIG_ECDSA_SHA512 = 6 };

/* key_usage bit numbers (RFC 5280 4.2.1.3) */
#define MC_KU_DIGITAL_SIGNATURE 0
#define MC_KU_KEY_CERT_SIGN     5

typedef struct {
    const uint8_t *der;       size_t der_len;
    const uint8_t *subject;   size_t subject_len;   /* contents of the Name SEQUENCE */
    const uint8_t *exts;      size_t exts_len;      /* contents of the Extensions SEQUENCE */
    int            key_type;
    const uint8_t *ec_point;                        /* 65 bytes, uncompressed */
    const uint8_t *rsa_n;     size_t rsa_n_len;     /* big-endian, leading zero stripped */
    const uint8_t *rsa_e;     size_t rsa_e_len;

    /* --- fields used for chain validation --- */
    const uint8_t *tbs;       size_t tbs_len;       /* whole TBSCertificate TLV (what the CA signed) */
    const uint8_t *issuer;    size_t issuer_len;    /* contents of the issuer Name SEQUENCE */
    const uint8_t *tbs_sigalg; size_t tbs_sigalg_len;   /* AlgorithmIdentifier contents inside the TBS */
    const uint8_t *sigalg;    size_t sigalg_len;        /* AlgorithmIdentifier contents outside the TBS */
    const uint8_t *sig;       size_t sig_len;       /* signature value (unused-bits byte stripped) */
    int            sig_alg;                         /* MC_SIG_* */
    int64_t        not_before, not_after;           /* unix seconds */
    int            has_bc, is_ca;                   /* basicConstraints present / cA */
    int            path_len;                        /* pathLenConstraint, -1 if absent */
    int            has_ku;  unsigned key_usage;     /* keyUsage bits, see MC_KU_* */
    int            has_eku, eku_server_auth;        /* extKeyUsage present / contains serverAuth or anyEKU */
} mcert_t;

int mcert_parse(mcert_t *out, const uint8_t *der, size_t der_len);

/* 1 if `host` matches a dNSName in subjectAltName (exact, or "*." wildcard for
 * the leftmost label), or - when `host` is a dotted IPv4 literal - an iPAddress
 * entry; falls back to the subject CN only when there is no SAN
 * extension. Case-insensitive. 0 otherwise. */
int mcert_match_host(const mcert_t *c, const char *host);

/* Same Name (byte-for-byte DER contents)? */
int mcert_name_equal(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);
/* CN (else O) of a Name's contents as printable ASCII, for error messages. */
void mcert_name_label(const uint8_t *name, size_t nlen, char *out, size_t cap);
/* 0 = valid at `now` (unix seconds), 1 = not yet valid, 2 = expired. */
int mcert_check_dates(const mcert_t *c, int64_t now);
/* 0 only if `cert` carries a valid signature made by `issuer`'s key
 * (RSA PKCS#1 v1.5 or ECDSA P-256/P-384, with SHA-256/384/512). */
int mcert_verify_issued_by(const mcert_t *cert, const mcert_t *issuer);

/* SHA-384 / SHA-512 (SHA-256 comes from sha256.h). */
void sha384_hash(const uint8_t *msg, size_t len, uint8_t out[48]);
void sha512_hash(const uint8_t *msg, size_t len, uint8_t out[64]);

/* Verify a ServerKeyExchange signature against the leaf's public key.
 * Supported schemes (these are the ones lib_tls.c advertises):
 *   0x0403 ecdsa_secp256r1_sha256   0x0804 rsa_pss_rsae_sha256
 *   0x0401 rsa_pkcs1_sha256
 * Returns 0 only if the signature is valid. */
int verify_signature(const mcert_t *leaf, uint16_t scheme,
                     const uint8_t *msg, size_t msg_len,
                     const uint8_t *sig, size_t sig_len);

#endif