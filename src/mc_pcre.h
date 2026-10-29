/* mc_pcre.h -- how MUSHclient studies and runs a PCRE pattern.

   Shared by regexp.cpp (triggers and aliases) and lrexlib.c (Lua's "rex").

   Studying: with the JIT where it is built in.  pcre/config.h turns it on for
   Windows only; elsewhere pcre_study ignores the JIT flag (and
   pcre_jit_compile.c supplies stubs).  An extra that may hold JIT code must be
   freed with pcre_free_study.

   Running: PCRE's interpreter recurses on the machine stack, roughly once per
   repeat of a group, and had no useful limit (MATCH_LIMIT_RECURSION is ten
   million), so one long enough line crashed the client: "^(?:a|b)*c$"
   against a thousand-character line overflows the 1 MB stack of a Debug
   build, and against twenty thousand characters the Release build.
   mc_pcre_exec caps the recursion at what the calling thread's remaining stack
   holds -- PCRE reports its own frame size -- keeping MC_PCRE_STACK_RESERVE
   spare, so such a line fails with PCRE_ERROR_RECURSIONLIMIT, which callers
   answer like any runtime error.  The JIT ignores that cap: it backtracks on
   a stack of its own, up to 8 MB per thread, and fails with
   PCRE_ERROR_JIT_STACKLIMIT beyond it.  It is not rerun in the interpreter,
   which is shallower still.  Where the stack cannot be located, the cap is
   PCRE's own, as before. */

#ifndef MUSHCLIENT_MC_PCRE_H
#define MUSHCLIENT_MC_PCRE_H

#include <stddef.h>
#include <string.h>
#include "pcre.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(_MSC_VER)
#define MC_PCRE_THREAD __declspec(thread)
#elif defined(__cplusplus)
#define MC_PCRE_THREAD thread_local
#else
#define MC_PCRE_THREAD _Thread_local
#endif

/* Stack an interpreted match leaves unused: for pcre_exec's own frame, a
   callout's script, and the system's handling of the guard page. */
#define MC_PCRE_STACK_RESERVE (128 * 1024)

/* The calling thread's JIT stack, made on first use and kept for the thread's
   life.  NULL (allocation failed) makes PCRE use 32 KB of the machine stack. */
static __inline pcre_jit_stack * mc_pcre_jit_stack (void * unused)
  {
  static MC_PCRE_THREAD pcre_jit_stack * stack = NULL;
  (void) unused;
  if (stack == NULL)
    stack = pcre_jit_stack_alloc (32 * 1024, 8 * 1024 * 1024);
  return stack;
  }

/* pcre_study, compiling with the JIT where it is built in */
static __inline pcre_extra * mc_pcre_study (const pcre * code, const char ** error)
  {
  pcre_extra * extra = pcre_study (code, PCRE_STUDY_JIT_COMPILE, error);
  if (extra != NULL && (extra->flags & PCRE_EXTRA_EXECUTABLE_JIT) != 0)
    pcre_assign_jit_stack (extra, mc_pcre_jit_stack, NULL);
  return extra;
  }

/* The lowest address of the calling thread's stack, found once per thread;
   NULL where the platform will not say. */
static __inline const char * mc_pcre_stack_floor (void)
  {
  static MC_PCRE_THREAD const char * floor = NULL;
  static MC_PCRE_THREAD int looked = 0;
  if (!looked)
    {
    looked = 1;
#if defined(_WIN32)
    {
    /* the stack's whole reservation, of which this variable is a part */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery (&mbi, &mbi, sizeof mbi) == sizeof mbi)
      floor = (const char *) mbi.AllocationBase;
    }
#elif defined(__APPLE__)
    {
    pthread_t self = pthread_self ();
    floor = (const char *) pthread_get_stackaddr_np (self)
            - pthread_get_stacksize_np (self);
    }
#elif defined(__linux__) && defined(_GNU_SOURCE)
    {
    pthread_attr_t attr;
    void * addr;
    size_t size;
    if (pthread_getattr_np (pthread_self (), &attr) == 0)
      {
      if (pthread_attr_getstack (&attr, &addr, &size) == 0)
        floor = (const char *) addr;
      pthread_attr_destroy (&attr);
      }
    }
#endif
    }
  return floor;
  }

/* pcre_exec, with the interpreter's recursion capped by the stack left */
static __inline int mc_pcre_exec (const pcre * code, const pcre_extra * extra,
                                  const char * subject, int length,
                                  int start_offset, int options,
                                  int * ovector, int ovecsize)
  {
  static int frame = 0;   /* bytes of stack per recursion; the same on every thread */
  const char * floor;
  /* The JIT runs, and needs no cap, for compiled code without custom tables
     and these options; anything else (partial matching, say) falls back to
     the interpreter inside pcre_exec, so is capped. */
  if (extra != NULL &&
      (extra->flags & (PCRE_EXTRA_EXECUTABLE_JIT | PCRE_EXTRA_TABLES)) == PCRE_EXTRA_EXECUTABLE_JIT &&
      extra->executable_jit != NULL &&
      (options & ~(PCRE_NO_UTF8_CHECK | PCRE_NOTBOL | PCRE_NOTEOL |
                   PCRE_NOTEMPTY | PCRE_NOTEMPTY_ATSTART)) == 0)
    return pcre_exec (code, extra, subject, length, start_offset, options,
                      ovector, ovecsize);
  floor = mc_pcre_stack_floor ();
  if (floor != NULL)
    {
    char here;
    ptrdiff_t left = &here - floor - MC_PCRE_STACK_RESERVE;
    unsigned long depth;
    pcre_extra capped;
    if (frame <= 0)
      {
      frame = -pcre_exec (NULL, NULL, NULL, -999, -999, 0, NULL, 0);
      if (frame <= 0)
        frame = 4096;   /* not reported: assume a large one */
      }
    depth = left > 0 ? (unsigned long) (left / frame) : 0;
    if (extra != NULL)
      capped = *extra;
    else
      memset (&capped, 0, sizeof capped);
    if ((capped.flags & PCRE_EXTRA_MATCH_LIMIT_RECURSION) == 0 ||
        capped.match_limit_recursion > depth)
      capped.match_limit_recursion = depth;
    capped.flags |= PCRE_EXTRA_MATCH_LIMIT_RECURSION;
    return pcre_exec (code, &capped, subject, length, start_offset, options,
                      ovector, ovecsize);
    }
  return pcre_exec (code, extra, subject, length, start_offset, options,
                    ovector, ovecsize);
  }

#endif /* MUSHCLIENT_MC_PCRE_H */
