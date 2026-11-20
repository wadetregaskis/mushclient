/* mc_pcre.c -- PCRE 8's contract, over PCRE2.  See mc_pcre.h. */

#include <stdlib.h>
#include <string.h>
#include "mc_pcre.h"

#if defined(_MSC_VER)
#define MC_THREAD __declspec(thread)
#else
#define MC_THREAD _Thread_local
#endif

/* ----------------------------------------------------------- options ---- */

/* Everything pcre_compile accepted.  Anything else was "unknown option bit(s)
   set", and rex.new (p, rex.flags ().NOTBOL) still has to fail. */
#define MC_COMPILE_OPTIONS \
  (PCRE_CASELESS | PCRE_MULTILINE | PCRE_DOTALL | PCRE_EXTENDED | PCRE_ANCHORED | \
   PCRE_DOLLAR_ENDONLY | PCRE_EXTRA | PCRE_UNGREEDY | PCRE_UTF8 | \
   PCRE_NO_AUTO_CAPTURE | PCRE_NO_UTF8_CHECK | PCRE_AUTO_CALLOUT | PCRE_NEVER_UTF | \
   PCRE_NO_AUTO_POSSESS | PCRE_FIRSTLINE | PCRE_DUPNAMES | PCRE_NEWLINE_CR | \
   PCRE_NEWLINE_LF | PCRE_NEWLINE_ANY | PCRE_BSR_ANYCRLF | PCRE_BSR_UNICODE | \
   PCRE_JAVASCRIPT_COMPAT | PCRE_NO_START_OPTIMIZE | PCRE_UCP)

/* And pcre_exec.  Its newline and \R bits have no equivalent at match time,
   and are ignored (nothing bundled or of Quow's uses them). */
#define MC_MATCH_OPTIONS \
  (PCRE_ANCHORED | PCRE_NOTBOL | PCRE_NOTEOL | PCRE_NOTEMPTY | PCRE_NOTEMPTY_ATSTART | \
   PCRE_NO_UTF8_CHECK | PCRE_NO_START_OPTIMIZE | PCRE_PARTIAL_SOFT | PCRE_PARTIAL_HARD | \
   PCRE_NEWLINE_CR | PCRE_NEWLINE_LF | PCRE_NEWLINE_ANY | PCRE_BSR_ANYCRLF | PCRE_BSR_UNICODE)

static uint32_t compile_options (int o)
  {
  uint32_t r = 0;
  if (o & PCRE_CASELESS)          r |= PCRE2_CASELESS;
  if (o & PCRE_MULTILINE)         r |= PCRE2_MULTILINE;
  if (o & PCRE_DOTALL)            r |= PCRE2_DOTALL;
  if (o & PCRE_EXTENDED)          r |= PCRE2_EXTENDED;
  if (o & PCRE_ANCHORED)          r |= PCRE2_ANCHORED;
  if (o & PCRE_DOLLAR_ENDONLY)    r |= PCRE2_DOLLAR_ENDONLY;
  if (o & PCRE_UNGREEDY)          r |= PCRE2_UNGREEDY;
  if (o & PCRE_UTF8)              r |= PCRE2_UTF;
  if (o & PCRE_NO_AUTO_CAPTURE)   r |= PCRE2_NO_AUTO_CAPTURE;
  if (o & PCRE_NO_UTF8_CHECK)     r |= PCRE2_NO_UTF_CHECK;
  if (o & PCRE_AUTO_CALLOUT)      r |= PCRE2_AUTO_CALLOUT;
  if (o & PCRE_NEVER_UTF)         r |= PCRE2_NEVER_UTF;
  if (o & PCRE_NO_AUTO_POSSESS)   r |= PCRE2_NO_AUTO_POSSESS;
  if (o & PCRE_FIRSTLINE)         r |= PCRE2_FIRSTLINE;
  if (o & PCRE_DUPNAMES)          r |= PCRE2_DUPNAMES;
  if (o & PCRE_NO_START_OPTIMIZE) r |= PCRE2_NO_START_OPTIMIZE;
  if (o & PCRE_UCP)               r |= PCRE2_UCP;
  if (o & PCRE_JAVASCRIPT_COMPAT) r |= PCRE2_ALT_BSUX | PCRE2_MATCH_UNSET_BACKREF | PCRE2_ALLOW_EMPTY_CLASS;
  return r;
  }

static uint32_t match_options (int o)
  {
  uint32_t r = 0;
  if (o == 0)
    return 0;
  if (o & PCRE_ANCHORED)          r |= PCRE2_ANCHORED;
  if (o & PCRE_NOTBOL)            r |= PCRE2_NOTBOL;
  if (o & PCRE_NOTEOL)            r |= PCRE2_NOTEOL;
  if (o & PCRE_NOTEMPTY)          r |= PCRE2_NOTEMPTY;
  if (o & PCRE_NOTEMPTY_ATSTART)  r |= PCRE2_NOTEMPTY_ATSTART;
  if (o & PCRE_NO_UTF8_CHECK)     r |= PCRE2_NO_UTF_CHECK;
  if (o & PCRE_NO_START_OPTIMIZE) r |= PCRE2_NO_START_OPTIMIZE;
  if (o & PCRE_PARTIAL_SOFT)      r |= PCRE2_PARTIAL_SOFT;
  if (o & PCRE_PARTIAL_HARD)      r |= PCRE2_PARTIAL_HARD;
  return r;
  }

/* ------------------------------------------------------------ errors ---- */

static int pcre8_error (int e)
  {
  if (e <= PCRE2_ERROR_UTF8_ERR1 && e >= PCRE2_ERROR_UTF8_ERR21)
    return PCRE_ERROR_BADUTF8;       /* PCRE 8 had one code, and an offset */
  switch (e)
    {
    case PCRE2_ERROR_NOMATCH:           return PCRE_ERROR_NOMATCH;
    case PCRE2_ERROR_PARTIAL:           return PCRE_ERROR_PARTIAL;
    case PCRE2_ERROR_NULL:              return PCRE_ERROR_NULL;
    case PCRE2_ERROR_BADDATA:           return PCRE_ERROR_BADOPTION;
    case PCRE2_ERROR_BADOPTION:         return PCRE_ERROR_BADOPTION;
    case PCRE2_ERROR_BADMAGIC:          return PCRE_ERROR_BADMAGIC;
    case PCRE2_ERROR_BADMODE:           return PCRE_ERROR_BADMODE;
    case PCRE2_ERROR_BADOFFSET:         return PCRE_ERROR_BADOFFSET;
    case PCRE2_ERROR_BADUTFOFFSET:      return PCRE_ERROR_BADUTF8_OFFSET;
    case PCRE2_ERROR_CALLOUT:           return PCRE_ERROR_CALLOUT;
    case PCRE2_ERROR_JIT_BADOPTION:     return PCRE_ERROR_JIT_BADOPTION;
    case PCRE2_ERROR_JIT_STACKLIMIT:    return PCRE_ERROR_JIT_STACKLIMIT;
    case PCRE2_ERROR_MATCHLIMIT:        return PCRE_ERROR_MATCHLIMIT;
    case PCRE2_ERROR_NOMEMORY:          return PCRE_ERROR_NOMEMORY;
    case PCRE2_ERROR_NOSUBSTRING:       return PCRE_ERROR_NOSUBSTRING;
    case PCRE2_ERROR_NOUNIQUESUBSTRING: return PCRE_ERROR_NOSUBSTRING;
    case PCRE2_ERROR_RECURSELOOP:       return PCRE_ERROR_RECURSELOOP;
    case PCRE2_ERROR_DEPTHLIMIT:        return PCRE_ERROR_RECURSIONLIMIT;
    /* PCRE 8's nearest: it was the machine stack that ran out there */
    case PCRE2_ERROR_HEAPLIMIT:         return PCRE_ERROR_RECURSIONLIMIT;
    default:                            return PCRE_ERROR_INTERNAL;
    }
  }

/* ---------------------------------------------------------- patterns ---- */

/* What PCRE 8 took and PCRE2 does not, or takes differently.  The differences,
   found by tools/pcre_compare, which runs both:

   Refused by PCRE2:
     \j \y and the other letters with no meaning: PCRE 8 took them as the letter,
       and in a class \A \B \C \G \K \R \X \Z \z too.
       (PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL would do that, and also make \u0041 and
       \p{Foo} compile, which PCRE 8 refused.)
     [\w-_] [\d-a] [[:alpha:]-z]: a hyphen after a set is a literal
     \8 \9 with no such group yet: the digit
     \x with no digits: a zero byte
     (?X): PCRE 8's PCRE_EXTRA, for the rest of the group.  Dropped.
     \K in a lookaround: PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK, always set

   Taken by PCRE2, to mean something else:
     x{,3} x{ 2 } x{2, 3}: literal text to PCRE 8, quantifiers to PCRE2

   Newly accepted, which harms nothing: variable length lookbehind, group names
   of 33 or more characters, and so on.

   Left alone, because they are too obscure to be worth a rewrite, and no plugin
   the comparison has seen does them.  tools/pcre_compare lists them as expected:
     (?i)\p{Lu}: PCRE2 follows Perl, and it then matches lower case too
     \p{Greek} and the like: Unicode has moved on, and PCRE2 now goes by script
       extensions, so a few more characters match, such as U+0342
     (?xx)[a b]: PCRE2 has a new meaning for xx
     \8(a)(b)(c)(d)(e)(f)(g)(h): to PCRE2 a forward reference
     (?(R1)a|b) with no group 1: refused by PCRE2 alone

   So: only a pattern that is at risk is rewritten at all.  One with no brace in
   the suspect shape goes straight to PCRE2; only if that refuses it is it
   rewritten and tried again.  A pattern PCRE2 takes unchanged, which is nearly
   all of them, is never touched. */

static int is_digit (char c) { return c >= '0' && c <= '9'; }
static int is_space (char c) { return c == ' ' || c == '\t'; }
static int is_alpha (char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* Does PCRE 8 read this brace as a quantifier: {n} {n,} {n,m} */
static int pcre8_quantifier (const char * p)
  {
  if (*p++ != '{' || !is_digit (*p))
    return 0;
  while (is_digit (*p))
    p++;
  if (*p == '}')
    return 1;
  if (*p++ != ',')
    return 0;
  while (is_digit (*p))
    p++;
  return *p == '}';
  }

/* Does PCRE2 read this brace as a quantifier?  The same, plus {,m} and spaces
   and tabs inside. */
static int pcre2_quantifier (const char * p)
  {
  int digits = 0;
  if (*p++ != '{')
    return 0;
  while (is_space (*p)) p++;
  while (is_digit (*p)) { p++; digits++; }
  while (is_space (*p)) p++;
  if (*p == '}')
    return digits > 0;
  if (*p++ != ',')
    return 0;
  while (is_space (*p)) p++;
  while (is_digit (*p)) { p++; digits++; }
  while (is_space (*p)) p++;
  return *p == '}' && digits > 0;
  }

/* The cheap test: is there a brace PCRE2 and PCRE 8 would read differently?
   It may also find one inside a class or an escape, where it does not matter:
   mc_pcre_rewrite then simply changes nothing. */
static int suspect_brace (const char * pattern)
  {
  const char * p;
  for (p = pattern; (p = strchr (p, '{')) != NULL; p++)
    if (pcre2_quantifier (p) && !pcre8_quantifier (p))
      return 1;
  return 0;
  }

typedef struct { char * s; size_t n; } out_t;

static void put (out_t * o, char c) { o->s [o->n++] = c; }

static int is_hex (char c)
  {
  return is_digit (c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }

/* An escape followed by a braced argument, like \x{41}.  Copies it whole. */
static const char * copy_braced (out_t * o, const char * p)
  {
  const char letter = p [1];
  put (o, *p++);   /* the backslash */
  put (o, *p++);   /* the letter */
  if (letter == 'x' && *p != '{' && !is_hex (*p))
    {
    put (o, '0');  /* \x alone was a zero byte */
    put (o, '0');
    }
  else if (*p == '{')
    {
    const char * close = strchr (p, '}');
    if (close != NULL)
      while (p <= close)
        put (o, *p++);
    }
  return p;
  }

/* Letters with no meaning after a backslash: PCRE 8 read them as themselves. */
static int meaningless_letter (char c)
  {
  return c != '\0' && strchr ("FIJMOTYijmqy", c) != NULL;
  }

/* Those that mean nothing in a class, either. */
static int meaningless_in_class (char c)
  {
  return meaningless_letter (c) || (c != '\0' && strchr ("ABCGKRXZz", c) != NULL);
  }

/* A set in a class: a shorthand, or a POSIX name.  Returns where it ends, or
   NULL if p is not at one. */
static const char * class_set_end (const char * p)
  {
  if (p [0] == '\\' && p [1] != '\0' && strchr ("dDwWsShHvV", p [1]) != NULL)
    return p + 2;
  if (p [0] == '\\' && (p [1] == 'p' || p [1] == 'P'))
    {
    if (p [2] == '{')
      {
      const char * close = strchr (p, '}');
      return close != NULL ? close + 1 : NULL;
      }
    return p [2] != '\0' ? p + 3 : NULL;
    }
  if (p [0] == '[' && p [1] == ':')
    {
    const char * close = strstr (p + 2, ":]");
    return close != NULL ? close + 2 : NULL;
    }
  return NULL;
  }

/* Copy a class, from its '[' up to and including its ']'. */
static const char * rewrite_class (out_t * o, const char * p, int strict)
  {
  int after_set = 0;       /* the last thing was a set, not one character */
  put (o, *p++);           /* the [ */
  if (*p == '^')
    put (o, *p++);
  if (*p == ']')           /* a literal, in first place */
    put (o, *p++);
  while (*p != '\0' && *p != ']')
    {
    const char * set;
    if (p [0] == '\\' && p [1] == 'Q')
      {
      const char * end = strstr (p + 2, "\\E");
      const char * stop = end != NULL ? end + 2 : p + strlen (p);
      while (p < stop)
        put (o, *p++);
      after_set = 0;
      continue;
      }
    set = class_set_end (p);
    if (set != NULL)
      {
      while (p < set)
        put (o, *p++);
      after_set = 1;
      continue;
      }
    if (*p == '[' && (p [1] == '.' || p [1] == '='))
      {
      /* POSIX collating elements: both refuse them, and PCRE2 will say so */
      put (o, *p++);
      after_set = 0;
      continue;
      }
    if (*p == '\\')
      {
      if (p [1] == 'x' || p [1] == 'o' || p [1] == 'N')
        p = copy_braced (o, p);
      else if (p [1] == 'c' && p [2] != '\0')
        {
        put (o, *p++); put (o, *p++); put (o, *p++);
        }
      else if (!strict && meaningless_in_class (p [1]))
        {
        p++;               /* drop the backslash */
        put (o, *p++);
        }
      else
        {
        put (o, *p++);
        if (*p != '\0')
          put (o, *p++);
        }
      after_set = 0;
      continue;
      }
    if (*p == '-' && p [1] != ']' && p [1] != '\0')
      {
      /* A hyphen after a set is the character. */
      if (after_set)
        {
        put (o, '\\');
        put (o, *p++);
        after_set = 0;
        continue;
        }
      /* A range.  Its end goes round the loop, unless it is a plain character:
         that may be another hyphen, [+--], which must not be taken for one. */
      put (o, *p++);
      if (*p != '\\' && !(p [0] == '[' && p [1] == ':'))
        put (o, *p++);
      after_set = 0;
      continue;
      }
    put (o, *p++);
    after_set = 0;
    }
  if (*p == ']')
    put (o, *p++);
  return p;
  }

char * mc_pcre_rewrite (const char * pattern, int options)
  {
  /* Each byte can gain one backslash. */
  const size_t length = strlen (pattern);
  const int strict = (options & PCRE_EXTRA) != 0;
  const int no_capture = (options & PCRE_NO_AUTO_CAPTURE) != 0;
  /* (?x) lasts to the end of its group: one bit per open group, to some depth. */
  unsigned long long extended_stack = 0;
  int depth = 0;
  int extended = (options & PCRE_EXTENDED) != 0;
  int groups = 0;          /* capture groups opened so far */
  const char * p = pattern;
  out_t o;

  o.n = 0;
  o.s = (char *) malloc (length * 2 + 1);
  if (o.s == NULL)
    return NULL;

  while (*p != '\0')
    {
    char c = *p;

    if (c == '\\')
      {
      char e = p [1];
      if (e == 'Q')
        {
        const char * end = strstr (p + 2, "\\E");
        const char * stop = end != NULL ? end + 2 : p + strlen (p);
        while (p < stop)
          put (&o, *p++);
        }
      else if (e != '\0' && strchr ("xopPgkN", e) != NULL)
        p = copy_braced (&o, p);
      else if (e == 'c' && p [2] != '\0')
        {
        put (&o, *p++); put (&o, *p++); put (&o, *p++);
        }
      else if (!strict && meaningless_letter (e))
        {
        p++;               /* drop the backslash */
        put (&o, *p++);
        }
      else if (e == '8' || e == '9')
        {
        /* A back reference only if there are that many groups so far */
        int number = 0;
        const char * q = p + 1;
        while (is_digit (*q) && number < 100000)
          number = number * 10 + (*q++ - '0');
        if (number > groups)
          p++;             /* drop the backslash: the digits are literal */
        else
          while (p < q)
            put (&o, *p++);
        }
      else
        {
        put (&o, *p++);
        if (*p != '\0')
          put (&o, *p++);
        }
      continue;
      }

    if (c == '[')
      {
      p = rewrite_class (&o, p, strict);
      continue;
      }

    if (c == '{')
      {
      if (pcre2_quantifier (p) && !pcre8_quantifier (p))
        put (&o, '\\');
      put (&o, *p++);
      continue;
      }

    if (c == '#' && extended)
      {
      while (*p != '\0' && *p != '\n')
        put (&o, *p++);
      continue;
      }

    if (c == '(')
      {
      if (depth < 64)
        {
        if (extended)
          extended_stack |= 1ULL << depth;
        else
          extended_stack &= ~(1ULL << depth);
        }
      depth++;
      if (p [1] == '?' && p [2] == '#')
        {
        /* a comment, to the next parenthesis */
        depth--;
        while (*p != '\0' && *p != ')')
          put (&o, *p++);
        if (*p == ')')
          put (&o, *p++);
        continue;
        }
      if (p [1] == '*')
        {
        /* a verb or the like: no group of ours */
        }
      else if (p [1] != '?')
        {
        if (!no_capture)
          groups++;
        }
      else if ((p [2] == '<' && p [3] != '=' && p [3] != '!') ||
               p [2] == '\'' ||
               (p [2] == 'P' && p [3] == '<'))
        groups++;          /* named */
      else
        {
        /* (?x) (?x-i) (?x: and so on */
        const char * q = p + 2;
        int on = 1, set = extended;
        if (*q == '^')
          {
          set = 0;
          q++;
          }
        while (is_alpha (*q) || *q == '-')
          {
          if (*q == '-')
            on = 0;
          else if (*q == 'x')
            set = on;
          q++;
          }
        if (*q == ')' || *q == ':')
          {
          extended = set;
          put (&o, *p++);  /* the ( */
          put (&o, *p++);  /* the ? */
          while (p < q)
            {
            if (*p != 'X' || strict)
              put (&o, *p);
            p++;
            }
          put (&o, *p++);  /* the ) or : */
          if (*q == ')')
            depth--;       /* (?x) itself opens nothing */
          continue;
          }
        }
      put (&o, *p++);
      continue;
      }

    if (c == ')')
      {
      if (depth > 0)
        {
        depth--;
        if (depth < 64)
          extended = (extended_stack >> depth) & 1;
        }
      put (&o, *p++);
      continue;
      }

    put (&o, *p++);
    }

  o.s [o.n] = '\0';
  if (strcmp (o.s, pattern) == 0)
    {
    free (o.s);
    return NULL;
    }
  return o.s;
  }

/* One compile, with the options that don't go in the option word. */
static pcre2_code * compile_one (const char * pattern, int options,
                                 const unsigned char * tables,
                                 int * errorcode, PCRE2_SIZE * erroroffset)
  {
  pcre2_code * code;
  pcre2_compile_context * cc = pcre2_compile_context_create (NULL);
  if (cc != NULL)
    {
    pcre2_set_compile_extra_options (cc, PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK);
    if (tables != NULL)
      pcre2_set_character_tables (cc, tables);
    switch (options & (PCRE_NEWLINE_CR | PCRE_NEWLINE_LF | PCRE_NEWLINE_ANY))
      {
      case PCRE_NEWLINE_CR:      pcre2_set_newline (cc, PCRE2_NEWLINE_CR);      break;
      case PCRE_NEWLINE_LF:      pcre2_set_newline (cc, PCRE2_NEWLINE_LF);      break;
      case PCRE_NEWLINE_CRLF:    pcre2_set_newline (cc, PCRE2_NEWLINE_CRLF);    break;
      case PCRE_NEWLINE_ANY:     pcre2_set_newline (cc, PCRE2_NEWLINE_ANY);     break;
      case PCRE_NEWLINE_ANYCRLF: pcre2_set_newline (cc, PCRE2_NEWLINE_ANYCRLF); break;
      default: break;
      }
    if (options & PCRE_BSR_ANYCRLF)
      pcre2_set_bsr (cc, PCRE2_BSR_ANYCRLF);
    else if (options & PCRE_BSR_UNICODE)
      pcre2_set_bsr (cc, PCRE2_BSR_UNICODE);
    }
  code = pcre2_compile ((PCRE2_SPTR) pattern, PCRE2_ZERO_TERMINATED,
                        compile_options (options), errorcode, erroroffset, cc);
  pcre2_compile_context_free (cc);
  return code;
  }

pcre2_code * mc_pcre_compile (const char * pattern, int options,
                              const unsigned char * tables, int jit,
                              const char ** error, int * erroffset)
  {
  static MC_THREAD char message [256];
  pcre2_code * code = NULL;
  int errorcode = 0;
  PCRE2_SIZE offset = 0;

  *error = NULL;
  *erroffset = 0;

  if (options & ~MC_COMPILE_OPTIONS)
    {
    *error = "unknown option bit(s) set";
    return NULL;
    }

  /* Only a pattern with a suspect brace is rewritten first.  Any other goes to
     PCRE2 as it stands, and is rewritten only if PCRE2 refuses that. */
  if (!suspect_brace (pattern))
    code = compile_one (pattern, options, tables, &errorcode, &offset);

  if (code == NULL)
    {
    char * fixed = mc_pcre_rewrite (pattern, options);
    if (fixed != NULL)
      {
      code = compile_one (fixed, options, tables, &errorcode, &offset);
      /* The complaint is then about what is left, which is the real problem:
         PCRE2's first would have been about something we have just dealt with.
         The offset is the rewritten pattern's, so bring it back by the difference. */
      if (code == NULL)
        {
        const size_t extra = strlen (fixed) - strlen (pattern);   /* unsigned, and it wraps right */
        offset -= extra;
        if (offset > strlen (pattern))
          offset = strlen (pattern);
        }
      free (fixed);
      }
    else if (suspect_brace (pattern))
      code = compile_one (pattern, options, tables, &errorcode, &offset);
    }

  if (code == NULL)
    {
    if (pcre2_get_error_message (errorcode, (PCRE2_UCHAR *) message, sizeof message) < 0)
      strcpy (message, "unknown error");
    *error = message;
    *erroffset = (int) offset;
    return NULL;
    }

  if (jit)
    pcre2_jit_compile (code, PCRE2_JIT_COMPLETE);   /* on failure, it is interpreted */

  return code;
  }

/* -------------------------------------------------- a required literal ---- */

/* See mc_pcre.h.  The rule is that a mistake may only ever lose a literal, never
   invent one: a literal that a match need not contain would make a trigger miss
   a line it matches, silently.  So the pattern is read as PCRE2 reads it, and
   anything not understood completely gives up on the whole pattern:

   - A run of plain characters at the top level of the pattern, or inside a
     group that is itself required (not quantified, not a lookaround, and with no
     alternation of its own), must be in every match.  A quantifier takes the
     character before it out of the run; anything else that isn't a plain
     character (a class, a dot, an escape like \d, an anchor, a group) ends it.
   - A sequence with a | at its own level has no required literal.
   - Option settings like (?i), verbs like (*ACCEPT), conditions, recursion,
     callouts, \Q..\E, extended mode, and a brace that isn't a plain {n}, {n,}
     or {n,m}: no literal at all.
   - Caseless: compared ignoring ASCII case, and no character outside ASCII is
     taken.  In UTF-8 mode caseless K and S are not taken either, since there they
     also match the Kelvin sign and the long s.  In UTF-8 mode no character
     outside ASCII is taken at all, so a quantifier on one is always seen. */

#define MC_LITERAL_RUN 32

typedef struct
  {
  char text [MC_LITERAL_RUN];
  int length;
  } lit_run_t;

typedef struct
  {
  const char * p;
  int caseless, utf;
  int bad;               /* something not understood: no literal at all */
  } lit_scan_t;

static void lit_keep (lit_run_t * best, lit_run_t * run)
  {
  if (run->length > best->length)
    *best = *run;
  run->length = 0;
  }

/* A quantifier at s->p: consumes it and says so.  A brace that isn't a plain
   {n}, {n,} or {n,m} might be one to PCRE2 ({,m}, spaces) or a literal: give up. */
static int lit_quantifier (lit_scan_t * s)
  {
  const char * p = s->p;
  if (*p == '?' || *p == '*' || *p == '+')
    p++;
  else if (*p == '{')
    {
    if (!pcre8_quantifier (p) || pcre2_quantifier (p) != 1)
      {
      s->bad = 1;
      return 0;
      }
    p = strchr (p, '}') + 1;
    }
  else
    return 0;
  if (*p == '?' || *p == '+')   /* lazy or possessive */
    p++;
  s->p = p;
  return 1;
  }

/* Skip to and past `close`, through characters that may be in a name. */
static void lit_skip_name (lit_scan_t * s, char close)
  {
  const char * p = s->p;
  while (is_alpha (*p) || is_digit (*p) || *p == '_')
    p++;
  if (*p != close)
    s->bad = 1;
  else
    s->p = p + 1;
  }

/* Skip a braced argument {...} at s->p. */
static void lit_skip_braced (lit_scan_t * s)
  {
  const char * close = strchr (s->p, '}');
  if (close == NULL)
    s->bad = 1;
  else
    s->p = close + 1;
  }

/* An escape, at its backslash.  Returns the character if it stands for one plain
   character that may be taken into a literal, else -1 (having skipped all of it). */
static int lit_escape (lit_scan_t * s)
  {
  const unsigned char e = (unsigned char) s->p [1];
  if (e == '\0' || e >= 0x80)
    {
    s->bad = 1;
    return -1;
    }
  s->p += 2;
  if (!is_alpha ((char) e) && !is_digit ((char) e))
    return e;                     /* \. \\ \( \  and so on: the character */
  if (is_digit ((char) e))
    {
    while (is_digit (*s->p))      /* a back reference, or octal */
      s->p++;
    return -1;
    }
  switch (e)
    {
    case 'x':
      if (*s->p == '{')
        lit_skip_braced (s);
      else
        while (is_hex (*s->p))     /* more than two is just losing a literal */
          s->p++;
      return -1;
    case 'o':
      if (*s->p != '{')
        s->bad = 1;
      else
        lit_skip_braced (s);
      return -1;
    case 'N':
      if (*s->p == '{')
        lit_skip_braced (s);
      return -1;
    case 'c':
      if (*s->p == '\0' || (unsigned char) *s->p >= 0x80)
        s->bad = 1;
      else
        s->p++;
      return -1;
    case 'p': case 'P':
      if (*s->p == '{')
        lit_skip_braced (s);
      else if (*s->p == '\0')
        s->bad = 1;
      else
        s->p++;
      return -1;
    case 'g':
      if (*s->p == '{')
        lit_skip_braced (s);
      else if (*s->p == '<' || *s->p == '\'')
        {
        const char close = *s->p == '<' ? '>' : '\'';
        s->p++;
        if (*s->p == '-' || *s->p == '+')
          s->p++;
        lit_skip_name (s, close);
        }
      else
        {
        if (*s->p == '-' || *s->p == '+')
          s->p++;
        while (is_digit (*s->p))
          s->p++;
        }
      return -1;
    case 'k':
      if (*s->p == '{')
        lit_skip_braced (s);
      else if (*s->p == '<' || *s->p == '\'')
        {
        const char close = *s->p == '<' ? '>' : '\'';
        s->p++;
        lit_skip_name (s, close);
        }
      else
        s->bad = 1;
      return -1;
    default:
      if (strchr ("dDwWsShHvVRXCbBAzZGKntrefa", e) == NULL)
        s->bad = 1;               /* \Q \E, and letters with no meaning */
      return -1;
    }
  }

/* A class, at its '['. */
static void lit_class (lit_scan_t * s)
  {
  const char * p = s->p + 1;
  if (*p == '^')
    p++;
  if (*p == ']')
    p++;
  while (*p != ']')
    {
    if (*p == '\0')
      {
      s->bad = 1;
      return;
      }
    if (*p == '\\')
      {
      if (p [1] == '\0' || p [1] == 'Q' || p [1] == 'E')
        {
        s->bad = 1;
        return;
        }
      p += 2;
      }
    else if (p [0] == '[' && (p [1] == ':' || p [1] == '.' || p [1] == '='))
      {
      const char end [3] = { p [1], ']', '\0' };
      const char * close = strstr (p + 2, end);
      if (close == NULL)
        {
        s->bad = 1;
        return;
        }
      p = close + 2;
      }
    else
      p++;
    }
  s->p = p + 1;
  }

static void lit_sequence (lit_scan_t * s, lit_run_t * best, int depth);

/* A group, at its '('.  Its literal counts only if the group must be matched,
   once, as it stands. */
static void lit_group (lit_scan_t * s, lit_run_t * best, int depth)
  {
  const char * p = s->p + 1;
  int required = 1;
  lit_run_t inner;

  if (depth > 50 || *p == '*')    /* (*ACCEPT) and every other verb */
    {
    s->bad = 1;
    return;
    }
  if (*p == '?')
    {
    p++;
    if (*p == ':' || *p == '>')
      p++;
    else if (*p == '=' || *p == '!')
      {
      p++;
      required = 0;
      }
    else if (*p == '<' && (p [1] == '=' || p [1] == '!'))
      {
      p += 2;
      required = 0;
      }
    else if (*p == '<' || *p == '\'' || (*p == 'P' && p [1] == '<'))
      {
      const char close = *p == '\'' ? '\'' : '>';
      s->p = p + (*p == 'P' ? 2 : 1);
      lit_skip_name (s, close);
      if (s->bad)
        return;
      p = s->p;
      }
    else
      {
      /* (?i) (?#...) (?(...)) (?R) (?1) (?&name) (?P=name) (?C) (?|...) ... */
      s->bad = 1;
      return;
      }
    }
  s->p = p;
  lit_sequence (s, &inner, depth + 1);
  if (s->bad)
    return;
  if (*s->p != ')')
    {
    s->bad = 1;
    return;
    }
  s->p++;
  if (lit_quantifier (s))
    required = 0;
  if (required)
    lit_keep (best, &inner);
  }

/* A sequence of items, to an unmatched ')' or the end. */
static void lit_sequence (lit_scan_t * s, lit_run_t * best, int depth)
  {
  lit_run_t run, mine;
  int alternation = 0;
  run.length = 0;
  mine.length = 0;

  while (!s->bad && *s->p != '\0' && *s->p != ')')
    {
    const unsigned char c = (unsigned char) *s->p;
    int literal = -1;

    switch (c)
      {
      case '|':
        alternation = 1;
        run.length = 0;
        s->p++;
        continue;
      case '(':
        lit_keep (&mine, &run);
        lit_group (s, &mine, depth);
        continue;
      case '\\':
        literal = lit_escape (s);
        break;
      case '[':
        lit_class (s);
        break;
      case '?': case '*': case '+': case '{':
        s->bad = 1;              /* a quantifier with nothing to quantify, or a brace */
        continue;
      case '.': case '^': case '$': case ']': case '}':
        s->p++;
        break;
      default:
        if (c >= 0x80 && s->utf)
          {
          s->p++;                /* the whole character, so a quantifier is seen */
          while (((unsigned char) *s->p & 0xC0) == 0x80)
            s->p++;
          }
        else
          {
          s->p++;
          literal = c;
          }
        break;
      }
    if (s->bad)
      break;

    if (literal >= 0 && s->caseless)
      {
      if (literal >= 0x80 ||
          (s->utf && (literal == 'k' || literal == 'K' || literal == 's' || literal == 'S')))
        literal = -1;
      else if (literal >= 'A' && literal <= 'Z')
        literal += 'a' - 'A';
      }

    if (lit_quantifier (s) || literal < 0)
      lit_keep (&mine, &run);    /* that was the last of this run */
    else
      {
      if (run.length == MC_LITERAL_RUN)
        lit_keep (&mine, &run);
      run.text [run.length++] = (char) literal;
      }
    }

  lit_keep (&mine, &run);
  if (alternation)
    mine.length = 0;
  *best = mine;
  }

int mc_pcre_required_literal (const char * pattern, int options, char * out, int size)
  {
  lit_scan_t s;
  lit_run_t best;
  const int understood = PCRE_CASELESS | PCRE_MULTILINE | PCRE_DOTALL | PCRE_UTF8 |
                         PCRE_ANCHORED | PCRE_DOLLAR_ENDONLY | PCRE_UNGREEDY |
                         PCRE_NO_AUTO_CAPTURE | PCRE_DUPNAMES;

  if (pattern == NULL || (options & ~understood) != 0 || size < 1)
    return 0;
  s.p = pattern;
  s.caseless = (options & PCRE_CASELESS) != 0;
  s.utf = (options & PCRE_UTF8) != 0;
  s.bad = 0;
  lit_sequence (&s, &best, 0);
  if (s.bad || *s.p != '\0' || best.length == 0)
    return 0;                    /* a stray ')' is an error anyway */
  if (best.length > size)
    best.length = size;
  memcpy (out, best.text, best.length);
  return best.length;
  }

/* Fold ASCII upper case to lower. */
static unsigned char lit_fold (unsigned char c)
  {
  return c >= 'A' && c <= 'Z' ? (unsigned char) (c + ('a' - 'A')) : c;
  }

/* Roughly how common a byte is in a MUD's English output, so that the search
   looks first for the literal's rarest byte: a space or an 'e' turns up on
   every line, and a 'k' or a digit seldom. */
static int lit_commonness (unsigned char c)
  {
  static const unsigned char lower [26] =
    { /* a */ 65, 12, 22, 35, 100, 18, 17, 50, 58, 1, 6, 33, 20,
      /* n */ 57, 62, 15, 1, 50, 52, 75, 23, 8, 19, 1, 16, 1 };
  if (c == ' ')
    return 180;
  if (c >= 'a' && c <= 'z')
    return lower [c - 'a'];
  if (c >= 'A' && c <= 'Z')
    return 4;
  if (c == '.' || c == ',')
    return 8;
  if (c >= '0' && c <= '9')
    return 4;
  return 2;
  }

int mc_pcre_literal_anchor (const char * literal, size_t n, int caseless)
  {
  size_t i, best = 0;
  int least = 1000;
  for (i = 0; i < n; i++)
    {
    const unsigned char c = (unsigned char) literal [i];
    /* caseless, a letter is looked for in both cases: less selective */
    int w = lit_commonness (c);
    if (caseless && c >= 'a' && c <= 'z')
      w += 4;
    if (w < least)
      {
      least = w;
      best = i;
      }
    }
  return (int) best;
  }

int mc_pcre_has_literal (const char * subject, size_t length,
                         const char * literal, size_t n, int caseless, int anchor)
  {
  const unsigned char * s = (const unsigned char *) subject;
  const unsigned char * l = (const unsigned char *) literal;
  unsigned char a;
  /* where the anchor byte can be: the literal must fit around it */
  const unsigned char * p, * end;
  size_t j;

  if (n == 0)
    return 1;
  if (length < n)
    return 0;
  if (anchor < 0 || (size_t) anchor >= n)
    anchor = 0;
  a = l [anchor];
  p = s + anchor;
  end = s + length - (n - 1 - anchor);
  if (!caseless || a < 'a' || a > 'z')
    {
    while (p < end && (p = (const unsigned char *) memchr (p, a, end - p)) != NULL)
      {
      const unsigned char * start = p - anchor;
      if (!caseless)
        {
        if (memcmp (start, l, n) == 0)
          return 1;
        }
      else
        {
        for (j = 0; j < n && lit_fold (start [j]) == l [j]; j++)
          ;
        if (j == n)
          return 1;
        }
      p++;
      }
    return 0;
    }
  for (; p < end; p++)
    if ((*p | 0x20) == a)   /* a is a lower-case letter: this is it in either case */
      {
      const unsigned char * start = p - anchor;
      for (j = 0; j < n && lit_fold (start [j]) == l [j]; j++)
        ;
      if (j == n)
        return 1;
      }
  return 0;
  }

/* ----------------------------------------------------------- running ---- */

int mc_pcre_match (const pcre2_code * code, pcre2_match_data ** md,
                   const char * subject, int length, int start_offset, int options,
                   int (* callout) (pcre2_callout_block *, void *), void * callout_data);

pcre2_match_context * mc_pcre_match_context (void)
  {
  static MC_THREAD pcre2_match_context * context = NULL;
  if (context == NULL)
    {
    context = pcre2_match_context_create (NULL);
    if (context != NULL)
      {
      /* If this fails, the JIT uses 32 KB of the machine stack, and a match
         that needs more is rerun in the interpreter. */
      pcre2_jit_stack * stack = pcre2_jit_stack_create (32 * 1024, 8 * 1024 * 1024, NULL);
      if (stack != NULL)
        pcre2_jit_stack_assign (context, NULL, stack);
      }
    }
  return context;
  }

/* A match takes its copy of the callout as it starts, so this does not disturb
   any match that is under way, in whose callout this match may be running. */
static void set_callout (pcre2_match_context * context,
                         int (* callout) (pcre2_callout_block *, void *), void * callout_data)
  {
  static MC_THREAD int have_callout = 0;
  if (context != NULL && (callout != NULL || have_callout))
    {
    pcre2_set_callout (context, callout, callout_data);
    have_callout = callout != NULL;
    }
  }

int mc_pcre_is_jit_only (const pcre2_code * code)
  {
  size_t jit = 0;
  uint32_t all_options = 0;
  pcre2_pattern_info (code, PCRE2_INFO_JITSIZE, &jit);
  pcre2_pattern_info (code, PCRE2_INFO_ALLOPTIONS, &all_options);
  return jit > 0 && (all_options & PCRE2_UTF) == 0;
  }

int mc_pcre_match_jit (const pcre2_code * code, pcre2_match_data ** md,
                       const char * subject, int length, int start_offset, int options)
  {
  pcre2_match_context * context = mc_pcre_match_context ();
  int rc;

  /* pcre2_jit_match checks nothing, and doesn't know the options it can't do */
  if (*md != NULL && start_offset >= 0 && start_offset <= length &&
      (options & ~(PCRE_NOTBOL | PCRE_NOTEOL | PCRE_NOTEMPTY | PCRE_NOTEMPTY_ATSTART)) == 0)
    {
    set_callout (context, NULL, NULL);
    rc = pcre2_jit_match (code, (PCRE2_SPTR) subject, (PCRE2_SIZE) length, (PCRE2_SIZE) start_offset,
                          match_options (options), *md, context);
    if (rc >= 0)
      return rc;
    if (rc == PCRE2_ERROR_NOMATCH)
      return PCRE_ERROR_NOMATCH;
    if (rc != PCRE2_ERROR_JIT_STACKLIMIT)
      return pcre8_error (rc);
    }
  return mc_pcre_match (code, md, subject, length, start_offset, options, NULL, NULL);
  }

int mc_pcre_match (const pcre2_code * code, pcre2_match_data ** md,
                   const char * subject, int length, int start_offset, int options,
                   int (* callout) (pcre2_callout_block *, void *), void * callout_data)
  {
  pcre2_match_context * context = mc_pcre_match_context ();
  uint32_t o;
  int rc;

  if (code == NULL)
    return PCRE_ERROR_NULL;
  if (*md == NULL)
    return PCRE_ERROR_NOMEMORY;
  if (options & ~MC_MATCH_OPTIONS)
    return PCRE_ERROR_BADOPTION;

  /* An interpreted match's frames stay with the match data.  Don't let one long
     line pin megabytes per pattern, for good. */
  if (pcre2_get_match_data_heapframes_size (*md) > 1024 * 1024)
    {
    pcre2_match_data * fresh = pcre2_match_data_create_from_pattern (code, NULL);
    if (fresh != NULL)
      {
      pcre2_match_data_free (*md);
      *md = fresh;
      }
    }

  o = match_options (options);
  set_callout (context, callout, callout_data);

  rc = pcre2_match (code, (PCRE2_SPTR) subject, (PCRE2_SIZE) length, (PCRE2_SIZE) start_offset,
                    o, *md, context);

  /* The JIT's stack is 8 MB; the interpreter's limit is the heap. */
  if (rc == PCRE2_ERROR_JIT_STACKLIMIT && callout == NULL)
    rc = pcre2_match (code, (PCRE2_SPTR) subject, (PCRE2_SIZE) length, (PCRE2_SIZE) start_offset,
                      o | PCRE2_NO_JIT, *md, context);

  return rc >= 0 ? rc : pcre8_error (rc);
  }

void mc_pcre_get_offsets (pcre2_match_data * md, int * out, int pairs)
  {
  const PCRE2_SIZE * ovector = pcre2_get_ovector_pointer (md);
  const int have = (int) pcre2_get_ovector_count (md);
  int i;
  for (i = 0; i < pairs; i++)
    {
    if (i < have && ovector [i * 2] != PCRE2_UNSET)
      {
      out [i * 2]     = (int) ovector [i * 2];
      out [i * 2 + 1] = (int) ovector [i * 2 + 1];
      }
    else
      {
      out [i * 2]     = -1;
      out [i * 2 + 1] = -1;
      }
    }
  }
