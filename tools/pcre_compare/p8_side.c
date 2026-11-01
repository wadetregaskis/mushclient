/* p8_side.c -- PCRE 8.45's half of pcre_compare: it alone sees pcre.h, whose names
   collide with mc_pcre.h's. */

#include <string.h>
#define PCRE_STATIC
#include "pcre.h"
#include "p8_side.h"

void * p8_compile (const char * pattern, int options, const char ** error, int * erroffset)
  {
  return pcre_compile (pattern, options, error, erroffset, NULL);
  }

void * p8_study (void * code, int jit)
  {
  const char * error;
  return pcre_study ((pcre *) code, jit ? PCRE_STUDY_JIT_COMPILE : 0, &error);
  }

int p8_jit_active (void * extra)
  {
  return extra != NULL && (((pcre_extra *) extra)->flags & PCRE_EXTRA_EXECUTABLE_JIT) != 0;
  }

static pcre_jit_stack * jit_stack (void * unused)
  {
  static pcre_jit_stack * stack = NULL;
  (void) unused;
  if (stack == NULL)
    stack = pcre_jit_stack_alloc (32 * 1024, 8 * 1024 * 1024);
  return stack;
  }

int p8_exec (void * code, void * extra, const char * subject, int length, int start, int options,
             int * ovector, int ovecsize)
  {
  if (extra != NULL && (((pcre_extra *) extra)->flags & PCRE_EXTRA_EXECUTABLE_JIT) != 0)
    pcre_assign_jit_stack ((pcre_extra *) extra, jit_stack, NULL);
  return pcre_exec ((pcre *) code, (pcre_extra *) extra, subject, length, start, options, ovector, ovecsize);
  }

int p8_capture_count (void * code)
  {
  int n = -1;
  pcre_fullinfo ((pcre *) code, NULL, PCRE_INFO_CAPTURECOUNT, &n);
  return n;
  }

int p8_name_table (void * code, const unsigned char ** table, int * entry_size)
  {
  int n = 0;
  pcre_fullinfo ((pcre *) code, NULL, PCRE_INFO_NAMECOUNT, &n);
  pcre_fullinfo ((pcre *) code, NULL, PCRE_INFO_NAMETABLE, table);
  pcre_fullinfo ((pcre *) code, NULL, PCRE_INFO_NAMEENTRYSIZE, entry_size);
  return n;
  }

void p8_free (void * code, void * extra)
  {
  if (extra != NULL)
    pcre_free_study ((pcre_extra *) extra);
  pcre_free (code);
  }

const char * p8_version (void)
  {
  return pcre_version ();
  }

typedef struct { int * log; int * used; int size; int result; } p8_log;

static int p8_callout (pcre_callout_block * cb)
  {
  p8_log * l = (p8_log *) cb->callout_data;
  int i;
  if (*l->used + 5 + 2 * cb->capture_top < l->size)
    {
    l->log [(*l->used)++] = cb->callout_number;
    l->log [(*l->used)++] = cb->capture_top;
    l->log [(*l->used)++] = cb->capture_last;
    l->log [(*l->used)++] = cb->start_match;
    l->log [(*l->used)++] = cb->current_position;
    for (i = 2; i < 2 * cb->capture_top; i++)
      l->log [(*l->used)++] = cb->offset_vector [i];
    }
  return l->result;
  }

int p8_exec_logging (void * code, void * extra, const char * subject, int length, int options,
                     int * ovector, int ovecsize, int callout_result, int * log, int * log_used, int log_size)
  {
  pcre_extra e;
  p8_log l;
  int rc;
  l.log = log; l.used = log_used; l.size = log_size; l.result = callout_result;
  *log_used = 0;
  if (extra != NULL)
    e = *(pcre_extra *) extra;
  else
    memset (&e, 0, sizeof e);
  e.flags |= PCRE_EXTRA_CALLOUT_DATA;
  e.callout_data = &l;
  pcre_callout = p8_callout;
  rc = p8_exec (code, &e, subject, length, 0, options, ovector, ovecsize);
  pcre_callout = NULL;
  return rc;
  }
