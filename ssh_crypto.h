/*
 * SSH_CRYPTO.H - the OpenSSL functions used by ssh.c and tls.c.
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
EVP_PKEY *d2i_PUBKEY(EVP_PKEY **a, const unsigned char **pp, long length);
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

/* TLS (tls.c, FTPS) */
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct ssl_session_st SSL_SESSION;
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;
typedef struct x509_st X509;

#define SSL_VERIFY_NONE					0x00
#define SSL_ERROR_WANT_READ				2
#define SSL_ERROR_WANT_WRITE			3
#define SSL_ERROR_ZERO_RETURN			6
#define SSL_CTRL_MODE					33
#define SSL_CTRL_SET_TLSEXT_HOSTNAME	55
#define SSL_CTRL_SET_MIN_PROTO_VERSION	123
#define SSL_MODE_ENABLE_PARTIAL_WRITE	0x00000001UL
#define SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 0x00000002UL
#define TLS1_2_VERSION					0x0303
#define TLSEXT_NAMETYPE_host_name		0
#define X509_V_OK						0

const SSL_METHOD *TLS_client_method(void);
SSL_CTX *SSL_CTX_new(const SSL_METHOD *meth);
void SSL_CTX_free(SSL_CTX *ctx);
long SSL_CTX_ctrl(SSL_CTX *ctx, int cmd, long larg, void *parg);
void SSL_CTX_set_verify(SSL_CTX *ctx, int mode, void *callback);
int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);
SSL *SSL_new(SSL_CTX *ctx);
void SSL_free(SSL *ssl);
void SSL_set_bio(SSL *s, BIO *rbio, BIO *wbio);
void SSL_set_connect_state(SSL *s);
int SSL_connect(SSL *ssl);
int SSL_read(SSL *ssl, void *buf, int num);
int SSL_write(SSL *ssl, const void *buf, int num);
int SSL_get_error(const SSL *s, int ret_code);
int SSL_shutdown(SSL *s);
long SSL_get_verify_result(const SSL *ssl);
typedef struct X509_VERIFY_PARAM_st X509_VERIFY_PARAM;
X509_VERIFY_PARAM *SSL_get0_param(SSL *ssl);
int X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *param, const char *name, size_t namelen);
long SSL_ctrl(SSL *ssl, int cmd, long larg, void *parg);
SSL_SESSION *SSL_get1_session(SSL *ssl);
int SSL_set_session(SSL *to, SSL_SESSION *session);
void SSL_SESSION_free(SSL_SESSION *ses);
X509 *SSL_get1_peer_certificate(const SSL *s);
int SSL_has_pending(const SSL *s);
int X509_digest(const X509 *data, const EVP_MD *type, unsigned char *md, unsigned int *len);
void X509_free(X509 *a);
const char *X509_verify_cert_error_string(long n);
BIO *BIO_new(const BIO_METHOD *type);
const BIO_METHOD *BIO_s_mem(void);
int BIO_read(BIO *b, void *data, int dlen);
int BIO_write(BIO *b, const void *data, int dlen);
unsigned long ERR_get_error(void);
void ERR_error_string_n(unsigned long e, char *buf, size_t len);

/* the SDK pragma file has names longer than 31 characters that collide
   once truncated (Warning 84): none of them is used here */
#pragma msg 84 ignore push
#include <pragmas/amissl_pragmas.h>
#pragma msg 84 pop

#else

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>

#endif

#endif
