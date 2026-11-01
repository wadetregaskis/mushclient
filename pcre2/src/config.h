/* pcre2/src/config.h -- PCRE2's build configuration, for MUSHclient.
   From config.h.generic, which lists everything else that can be set.  Compiled
   with HAVE_CONFIG_H, on every platform. */

#define PCRE2_CODE_UNIT_WIDTH 8
#define SUPPORT_PCRE2_8 1
#define PCRE2_STATIC 1
#define PCRE2_EXPORT

/* Unicode properties and UTF-8 */
#define SUPPORT_UNICODE 1

/* The JIT, on every platform.  sljit handles MSVC on x86, x64 and ARM64, and
   uses pthread_jit_write_protect_np on Apple silicon, where the hardened runtime
   grants allow-jit (packaging/entitlements.plist).  If the executable memory
   can't be had, pcre2_jit_compile fails and the pattern is interpreted. */
#define SUPPORT_JIT 1

/* What PCRE 8 used here, so the same patterns hit the same limits. */
#define NEWLINE_DEFAULT 2       /* LF */
#define LINK_SIZE 2
#define MATCH_LIMIT 10000000
#define MATCH_LIMIT_DEPTH MATCH_LIMIT
#define PARENS_NEST_LIMIT 250
#define MAX_NAME_COUNT 10000

/* PCRE2's defaults */
#define MAX_NAME_SIZE 128
#define MAX_VARLOOKBEHIND 255

/* The interpreter's frames, in kibibytes.  Its default is 20 GB, which a
   backtracking pattern on a long line would take a client to the swap file for
   before reporting anything. */
#if defined(_WIN64) || defined(__LP64__)
#define HEAP_LIMIT 1048576
#else
#define HEAP_LIMIT 262144
#endif

#if !defined(_WIN32)
#define HAVE_UNISTD_H 1
#endif
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
