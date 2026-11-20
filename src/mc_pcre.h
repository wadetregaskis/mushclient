/* mc_pcre.h -- how MUSHclient compiles and runs a regular expression.

   Shared by regexp.cpp (triggers, aliases and the like) and lrexlib.c (Lua's
   "rex"), both of which go through mc_pcre.c, over PCRE2 (pcre2/).

   The rest of the client, and every script, was written against PCRE 8, and a
   plugin can hold any of its numbers: rex.flags() returns them, rex.new (p, 1)
   uses one, and GetTriggerInfo's execution error is another.  So PCRE 8's
   numbers go on being what the client deals in: the PCRE_* names below keep
   their PCRE 8 values, and mc_pcre.c turns them into PCRE2's at the edge, in
   one place.  Likewise a match's offsets are ints, -1 for "unset".

   Patterns: a pattern PCRE 8 took has to go on meaning what it meant.  See
   mc_pcre_compile in mc_pcre.c.

   Running: since 10.30 the interpreter keeps its frames on the heap, so a deep
   match no longer overflows the machine stack.  The JIT is on, on every
   platform where it can get executable memory (a pattern it cannot compile, or
   a platform where it fails, is interpreted).  It backtracks on a stack of its
   own, up to 8 MB per thread; a match that outgrows that is rerun in the
   interpreter, unless the pattern has a script's callout, which a rerun would
   call twice. */

#ifndef MUSHCLIENT_MC_PCRE_H
#define MUSHCLIENT_MC_PCRE_H

#include <stddef.h>

#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#ifndef PCRE2_STATIC
#define PCRE2_STATIC
#endif
#include "pcre2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Options: PCRE 8's bits */
#define PCRE_CASELESS           0x00000001  /* compile */
#define PCRE_MULTILINE          0x00000002  /* compile */
#define PCRE_DOTALL             0x00000004  /* compile */
#define PCRE_EXTENDED           0x00000008  /* compile */
#define PCRE_ANCHORED           0x00000010  /* compile, match */
#define PCRE_DOLLAR_ENDONLY     0x00000020  /* compile */
#define PCRE_EXTRA              0x00000040  /* compile: an unknown escape is an error */
#define PCRE_NOTBOL             0x00000080  /* match */
#define PCRE_NOTEOL             0x00000100  /* match */
#define PCRE_UNGREEDY           0x00000200  /* compile */
#define PCRE_NOTEMPTY           0x00000400  /* match */
#define PCRE_UTF8               0x00000800  /* compile */
#define PCRE_NO_AUTO_CAPTURE    0x00001000  /* compile */
#define PCRE_NO_UTF8_CHECK      0x00002000  /* compile, match */
#define PCRE_AUTO_CALLOUT       0x00004000  /* compile */
#define PCRE_PARTIAL_SOFT       0x00008000  /* match */
#define PCRE_PARTIAL            0x00008000  /* match */
#define PCRE_NEVER_UTF          0x00010000  /* compile */
#define PCRE_NO_AUTO_POSSESS    0x00020000  /* compile */
#define PCRE_FIRSTLINE          0x00040000  /* compile */
#define PCRE_DUPNAMES           0x00080000  /* compile */
#define PCRE_NEWLINE_CR         0x00100000  /* compile */
#define PCRE_NEWLINE_LF         0x00200000  /* compile */
#define PCRE_NEWLINE_CRLF       0x00300000  /* compile */
#define PCRE_NEWLINE_ANY        0x00400000  /* compile */
#define PCRE_NEWLINE_ANYCRLF    0x00500000  /* compile */
#define PCRE_BSR_ANYCRLF        0x00800000  /* compile */
#define PCRE_BSR_UNICODE        0x01000000  /* compile */
#define PCRE_JAVASCRIPT_COMPAT  0x02000000  /* compile */
#define PCRE_NO_START_OPTIMIZE  0x04000000  /* compile, match */
#define PCRE_PARTIAL_HARD       0x08000000  /* match */
#define PCRE_NOTEMPTY_ATSTART   0x10000000  /* match */
#define PCRE_UCP                0x20000000  /* compile */

/* Run-time errors: PCRE 8's numbers.  They reach scripts, as
   GetTriggerInfo (t, 28) and execution_error. */
#define PCRE_ERROR_NOMATCH          (-1)
#define PCRE_ERROR_NULL             (-2)
#define PCRE_ERROR_BADOPTION        (-3)
#define PCRE_ERROR_BADMAGIC         (-4)
#define PCRE_ERROR_UNKNOWN_OPCODE   (-5)
#define PCRE_ERROR_NOMEMORY         (-6)
#define PCRE_ERROR_NOSUBSTRING      (-7)
#define PCRE_ERROR_MATCHLIMIT       (-8)
#define PCRE_ERROR_CALLOUT          (-9)
#define PCRE_ERROR_BADUTF8         (-10)
#define PCRE_ERROR_BADUTF8_OFFSET  (-11)
#define PCRE_ERROR_PARTIAL         (-12)
#define PCRE_ERROR_BADPARTIAL      (-13)
#define PCRE_ERROR_INTERNAL        (-14)
#define PCRE_ERROR_BADCOUNT        (-15)
#define PCRE_ERROR_DFA_UITEM       (-16)
#define PCRE_ERROR_DFA_UCOND       (-17)
#define PCRE_ERROR_DFA_UMLIMIT     (-18)
#define PCRE_ERROR_DFA_WSSIZE      (-19)
#define PCRE_ERROR_DFA_RECURSE     (-20)
#define PCRE_ERROR_RECURSIONLIMIT  (-21)
#define PCRE_ERROR_NULLWSLIMIT     (-22)
#define PCRE_ERROR_BADNEWLINE      (-23)
#define PCRE_ERROR_BADOFFSET       (-24)
#define PCRE_ERROR_SHORTUTF8       (-25)
#define PCRE_ERROR_RECURSELOOP     (-26)
#define PCRE_ERROR_JIT_STACKLIMIT  (-27)
#define PCRE_ERROR_BADMODE         (-28)
#define PCRE_ERROR_JIT_BADOPTION   (-31)

/* Compile `pattern`, with PCRE 8's options, and the JIT too if `jit` is non-zero
   (a failure there just leaves the pattern interpreted).  NULL on failure, with
   the reason in *error (valid until the thread's next call here) and the byte
   offset in the pattern in *erroffset.  tables: from pcre2_maketables, or NULL. */
pcre2_code * mc_pcre_compile (const char * pattern, int options,
                              const unsigned char * tables, int jit,
                              const char ** error, int * erroffset);

/* The pattern PCRE2 has to be given for it to act as PCRE 8 did on `pattern`:
   malloc'd, or NULL if `pattern` will do as it stands.  Exposed for the tests. */
char * mc_pcre_rewrite (const char * pattern, int options);

/* Run a pattern.  options: PCRE 8's.  Returns what pcre_exec did: the highest
   numbered capture set, plus one, or one of PCRE_ERROR_*.  callout, if any,
   gets a pattern's (?C) points.  *md may be replaced: it is shrunk first if a
   previous deep match left it holding a lot of memory. */
int mc_pcre_match (const pcre2_code * code, pcre2_match_data ** md,
                   const char * subject, int length, int start_offset, int options,
                   int (* callout) (pcre2_callout_block *, void *), void * callout_data);

/* The trigger path, which runs once per enabled trigger per line, and nearly
   always fails to match, is worth a shorter way.  mc_pcre_match_jit is mc_pcre_match
   with no callout, going straight into the JIT's code, for a pattern for which
   mc_pcre_is_jit_only says that is right: JIT-compiled, and not UTF-8, because the
   JIT does not check its subject.  */
int mc_pcre_is_jit_only (const pcre2_code * code);
int mc_pcre_match_jit (const pcre2_code * code, pcre2_match_data ** md,
                       const char * subject, int length, int start_offset, int options);

/* A literal that every match of `pattern`, compiled with `options` (PCRE 8's),
   must contain: up to `size` bytes of it to out, and its length, or 0 if none
   could be found for certain.  For a pattern that is caseless the literal is in
   lower case, and is to be compared ignoring ASCII case.  If the subject lacks
   it, the pattern cannot match there, and running it can be skipped: the
   trigger path's prefilter (regexec).  Conservative: see mc_pcre.c, and
   tools/trigger_bench, which checks it against every pattern we know of. */
int mc_pcre_required_literal (const char * pattern, int options, char * out, int size);

/* The same, allowing for a set of literals of which a match must contain one at
   least (from an alternation, each of whose branches has its own): how many
   there are (0 if none could be found for certain, or they don't fit), their
   texts one after another in out, and their lengths in lengths [0 .. max). */
int mc_pcre_required_literals (const char * pattern, int options,
                               char * out, int size, int * lengths, int max);

/* Which byte of a literal to look for first: the one least likely to be in a
   line of text.  Worked out once, with the literal. */
int mc_pcre_literal_anchor (const char * literal, size_t n, int caseless);

/* Is literal[0..n) in subject[0..length)?  caseless: literal is in lower case,
   and ASCII letters in the subject are compared folded.  anchor: from
   mc_pcre_literal_anchor (any index into the literal is correct, only slower). */
int mc_pcre_has_literal (const char * subject, size_t length,
                         const char * literal, size_t n, int caseless, int anchor);

/* A match's offsets, as ints with -1 for unset: pairs of them to out[]. */
void mc_pcre_get_offsets (pcre2_match_data * md, int * out, int pairs);

/* The calling thread's match context, which holds its JIT stack.  Kept for the
   thread's life.  Exposed for the tests. */
pcre2_match_context * mc_pcre_match_context (void);

#ifdef __cplusplus
}
#endif

#endif /* MUSHCLIENT_MC_PCRE_H */
