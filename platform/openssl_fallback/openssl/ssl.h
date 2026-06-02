// platform/openssl/ssl.h — minimal OpenSSL stub for non-Windows build environments
// that lack openssl-dev. On macOS with homebrew openssl, the real headers take
// precedence via the build system's -I flags.
#pragma once

// Sentinel: doc_stub.h checks this to avoid redefining SSL functions we provide.
#define PLATFORM_OPENSSL_SSL_H_INCLUDED

// If the real OpenSSL headers are already included (e.g., via -I/opt/homebrew/...),
// skip this stub entirely.
#if !defined(OPENSSL_VERSION_TEXT)

#include <stddef.h>  // size_t

#ifdef __cplusplus
extern "C" {
#endif

// Opaque types
typedef struct ssl_st       SSL;
typedef struct ssl_ctx_st   SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;

// X.509 types (also in x509.h; safe to define once here)
#ifndef OPENSSL_X509_DEFINED
#define OPENSSL_X509_DEFINED
typedef struct x509_st      X509;
typedef struct X509_store   X509_STORE;
typedef struct X509_store_ctx_st X509_STORE_CTX;
#endif

// Constants
#define SSL_VERIFY_NONE              0
#define SSL_VERIFY_PEER              1
#define SSL_ERROR_WANT_READ          2
#define SSL_ERROR_WANT_WRITE         3
#define SSL_ERROR_ZERO_RETURN        6
#define X509_V_OK                    0
#define TLS1_2_VERSION            0x0303

// Stub functions — bodies never called in platform-stub builds
static inline SSL_METHOD* TLS_client_method(void) { return (SSL_METHOD*)0; }
static inline SSL_CTX*  SSL_CTX_new(SSL_METHOD* m) { (void)m; return (SSL_CTX*)0; }
static inline void      SSL_CTX_free(SSL_CTX* c) { (void)c; }
static inline int       SSL_CTX_set_min_proto_version(SSL_CTX* c, int v) { (void)c;(void)v; return 0; }
// SSL_CTX_set_verify defined below with proper SSL_verify_cb type
static inline X509_STORE* SSL_CTX_get_cert_store(SSL_CTX* c) { (void)c; return (X509_STORE*)0; }

static inline SSL*  SSL_new(SSL_CTX* c) { (void)c; return (SSL*)0; }
static inline void  SSL_free(SSL* s) { (void)s; }
static inline int   SSL_set_fd(SSL* s, int fd) { (void)s;(void)fd; return 0; }
static inline void  SSL_set_connect_state(SSL* s) { (void)s; }
static inline int   SSL_do_handshake(SSL* s) { (void)s; return -1; }
static inline int   SSL_read(SSL* s, void* b, int n) { (void)s;(void)b;(void)n; return -1; }
static inline int   SSL_write(SSL* s, const void* b, int n) { (void)s;(void)b;(void)n; return -1; }
static inline int   SSL_get_error(SSL* s, int r) { (void)s;(void)r; return 0; }
static inline long  SSL_get_verify_result(SSL* s) { (void)s; return X509_V_OK; }
static inline const char* SSL_get_version(SSL* s) { (void)s; return ""; }
static inline const char* SSL_get_cipher_name(SSL* s) { (void)s; return ""; }
static inline int   SSL_shutdown(SSL* s) { (void)s; return 1; }
static inline int   SSL_pending(SSL* s)  { (void)s; return 0; }

// Macro for SNI (server name indication)
#define SSL_set_tlsext_host_name(ssl, name) ((void)0)

// Hostname verification (no-op stub; real OpenSSL checks the cert CN/SAN)
static inline int SSL_set1_host(SSL* s, const char* host) { (void)s;(void)host; return 1; }

typedef int (*SSL_verify_cb)(int, X509_STORE_CTX*);

static inline void SSL_CTX_set_verify(SSL_CTX* c, int m, SSL_verify_cb cb) {
    (void)c; (void)m; (void)cb;
}
static inline int SSL_CTX_set_default_verify_paths(SSL_CTX* c) { (void)c; return 1; }

#ifdef __cplusplus
}
#endif
#endif // !OPENSSL_VERSION_TEXT
