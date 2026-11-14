/*
 * SSH_CRYPTO.H - the OpenSSL functions used by ssh.c.
 *
 * On the Amiga they come from AmiSSL (amissl.library). The OpenSSL 3 headers
 * shipped with the AmiSSL SDK cannot be read by SAS/C 6 (the safestack macros
 * overflow its line buffer, and it has no 64 bit integers), so only the
 * functions used here are declared, with opaque types, and they are called
 * through the SDK pragmas (pragmas/amissl_pragmas.h: call offsets only).
 *
 * Anywhere else (the Linux test build of ssh.c/sftp.c) the real OpenSSL
 * headers are used.
 */
#ifndef MFTP_SSH_CRYPTO_H
#define MFTP_SSH_CRYPTO_H

#if defined(__SASC) || defined(AMIGA)

#include <exec/types.h>

typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;
typedef struct engine_st ENGINE;
typedef struct rsa_st RSA;
typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;

#define NID_sha1		64
#define NID_sha256		672
#define NID_sha512		674
#define EVP_PKEY_X25519	1034
#define EVP_PKEY_ED25519	1087

extern struct Library *AmiSSLBase;

int RAND_bytes(unsigned char *buf, int num);
const EVP_MD *EVP_sha1(void);
const EVP_MD *EVP_sha256(void);
const EVP_MD *EVP_sha512(void);
int EVP_Digest(const void *data, size_t count, unsigned char *md, unsigned int *size, const EVP_MD *type, ENGINE *impl);
unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len, const unsigned char *data, size_t data_len, unsigned char *md, unsigned int *md_len);
EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *c);
int EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *impl, const unsigned char *key, const unsigned char *iv);
int EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl);
const EVP_CIPHER *EVP_aes_128_ctr(void);
const EVP_CIPHER *EVP_aes_256_ctr(void);
EVP_PKEY_CTX *EVP_PKEY_CTX_new_id(int id, ENGINE *e);
EVP_PKEY_CTX *EVP_PKEY_CTX_new(EVP_PKEY *pkey, ENGINE *e);
void EVP_PKEY_CTX_free(EVP_PKEY_CTX *ctx);
int EVP_PKEY_keygen_init(EVP_PKEY_CTX *ctx);
int EVP_PKEY_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY **ppkey);
int EVP_PKEY_get_raw_public_key(const EVP_PKEY *pkey, unsigned char *pub, size_t *len);
EVP_PKEY *EVP_PKEY_new_raw_public_key(int type, ENGINE *e, const unsigned char *pub, size_t len);
int EVP_PKEY_derive_init(EVP_PKEY_CTX *ctx);
int EVP_PKEY_derive_set_peer(EVP_PKEY_CTX *ctx, EVP_PKEY *peer);
int EVP_PKEY_derive(EVP_PKEY_CTX *ctx, unsigned char *key, size_t *keylen);
void EVP_PKEY_free(EVP_PKEY *pkey);
EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *ctx);
int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pctx, const EVP_MD *type, ENGINE *e, EVP_PKEY *pkey);
int EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *sigret, size_t siglen, const unsigned char *tbs, size_t tbslen);
RSA *RSA_new(void);
void RSA_free(RSA *r);
int RSA_set0_key(RSA *r, BIGNUM *n, BIGNUM *e, BIGNUM *d);
int RSA_verify(int type, const unsigned char *m, unsigned int m_length, const unsigned char *sigbuf, unsigned int siglen, RSA *rsa);
BIGNUM *BN_new(void);
void BN_free(BIGNUM *a);
BIGNUM *BN_bin2bn(const unsigned char *s, int len, BIGNUM *ret);
int BN_bn2bin(const BIGNUM *a, unsigned char *to);
int BN_num_bits(const BIGNUM *a);
int BN_mod_exp(BIGNUM *r, const BIGNUM *a, const BIGNUM *p, const BIGNUM *m, BN_CTX *ctx);
BN_CTX *BN_CTX_new(void);
void BN_CTX_free(BN_CTX *c);
int BN_hex2bn(BIGNUM **a, const char *str);
int BN_set_word(BIGNUM *a, unsigned long w);
int BN_cmp(const BIGNUM *a, const BIGNUM *b);
int BN_is_zero(const BIGNUM *a);
int BN_is_one(const BIGNUM *a);
const BIGNUM *BN_value_one(void);
int BN_sub(BIGNUM *r, const BIGNUM *a, const BIGNUM *b);

/* the SDK pragma file has names longer than 31 characters that collide
   once truncated (Warning 84): none of them is used here */
#pragma msg 84 ignore push
#include <pragmas/amissl_pragmas.h>
#pragma msg 84 pop

#else

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>

#endif

#endif
