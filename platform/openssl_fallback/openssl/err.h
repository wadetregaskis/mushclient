// platform/openssl/err.h — minimal stub
#pragma once
#if !defined(OPENSSL_VERSION_TEXT)
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long ERR_get_error_t;
static inline unsigned long ERR_get_error(void) { return 0; }
static inline void ERR_error_string_n(unsigned long e, char* buf, size_t len) {
    (void)e; if (buf && len) buf[0] = '\0';
}
#ifdef __cplusplus
}
#endif
#endif
