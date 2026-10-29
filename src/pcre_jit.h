/* pcre_jit.h -- studying a pattern with PCRE's JIT.

   Shared by regexp.cpp (triggers and aliases) and lrexlib.c (Lua's "rex").
   pcre/config.h turns the JIT on for Windows only; elsewhere this is plain
   pcre_study (it ignores the JIT flag, and pcre_jit_compile.c supplies stubs).

   What the JIT changes, and what it does not:
   - pcre_exec runs the compiled code only when it can: options the JIT does
     not handle, and lrexlib's custom character tables, quietly use the
     interpreter as before.
   - The interpreter recurses on the machine stack, roughly once per repeat of
     a group, so a long enough line crashes it: "^(?:a|b)*c$" against a
     thousand-character line overflows the 1 MB main-thread stack of a Debug
     build.  The JIT keeps its backtracking on a stack of its own -- here up to
     8 MB per thread, reserved, committed only as it is used -- and a match
     that outgrows even that fails with PCRE_ERROR_JIT_STACKLIMIT, which
     regexec reports and answers like any runtime error.  It is deliberately
     NOT rerun in the interpreter: whatever is too deep for the JIT is far too
     deep for that.
   - An extra that may hold JIT code must be freed with pcre_free_study. */

#ifndef MUSHCLIENT_PCRE_JIT_H
#define MUSHCLIENT_PCRE_JIT_H

#include "pcre.h"

#if defined(_MSC_VER)
#define MC_PCRE_THREAD __declspec(thread)
#elif defined(__cplusplus)
#define MC_PCRE_THREAD thread_local
#else
#define MC_PCRE_THREAD _Thread_local
#endif

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

#endif /* MUSHCLIENT_PCRE_JIT_H */
