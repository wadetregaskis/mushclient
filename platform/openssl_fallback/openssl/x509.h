// platform/openssl/x509.h — minimal stub
#pragma once
#if !defined(OPENSSL_VERSION_TEXT)
#include "ssl.h"   // gets X509, X509_STORE, X509_STORE_CTX
#ifdef __cplusplus
extern "C" {
#endif
static inline int   X509_STORE_add_cert(X509_STORE* s, X509* c) { (void)s;(void)c; return 0; }
static inline void  X509_free(X509* x) { (void)x; }
static inline X509* d2i_X509(X509** px, const unsigned char** in, long len) {
    (void)px;(void)in;(void)len; return (X509*)0;
}
static inline const char* X509_verify_cert_error_string(long n) { (void)n; return ""; }
#define X509_V_OK 0
#ifdef __cplusplus
}
#endif
#endif
