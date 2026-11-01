/* lrexlib.c - POSIX & PCRE regular expression library */
/* POSIX regexs can use Spencer extensions for matching NULs if available */
/* Reuben Thomas   nov00-25aug04 */
/* Shmuel Zeigerman   may04-25aug04 */

// Implements:

//   rex.flags
//   re = rex.new
//   rex.version

//   re:exec
//   re:match
//   re:gmatch

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LUA_52
    #include "..\..\lua52\src\lua.h"
    #include "..\..\lua52\src\lauxlib.h"
#elif defined(_WIN32)
    #include "..\lua.h"
    #include "..\lauxlib.h"
#else
    #include "lua.h"
    #include "lauxlib.h"
#endif

#define LREXLIB_PCRE      // NJG
#ifdef _MSC_VER
#pragma warning( disable: 4057)  // 'const unsigned char *' differs in indirection to slightly different base types from 'const char *'
#endif

/* Sanity check */
#if !defined(LREXLIB_POSIX) && !defined(LREXLIB_PCRE)
#error Define LREXLIB_POSIX or LREXLIB_PCRE, otherwise this library is useless!
#endif

static void L_lua_error(lua_State *L, const char *message)
{
  lua_pushstring(L, message);
  lua_error(L);
}

static void *Lmalloc(lua_State *L, size_t size)
{
  void *p = malloc(size);
  if(p == NULL)
    L_lua_error(L, "malloc failed");
  return p;
}

static int get_startoffset(lua_State *L, int stackpos, size_t len)
{
  int startoffset = luaL_optint(L, stackpos, 1);
  if(startoffset > 0)
    startoffset--;
  else if(startoffset < 0) {
    startoffset += len;
    if(startoffset < 0)
      startoffset = 0;
  }
  return startoffset;
}

static int regex_tostring (lua_State *L, const char* type_handle, const char* type_name)
{
  char buf[256];
  void *udata = luaL_checkudata(L, 1, type_handle);
  if(udata) {
    sprintf(buf, "%s (%p)", type_name, udata);
    lua_pushstring(L, buf);
  }
  else {
    sprintf(buf, "must be userdata of type '%s'", type_name);
    luaL_argerror(L, 1, buf);
  }
  return 1;
}

typedef struct { const char* key; int val; } flags_pair;

static int regex_get_flags (lua_State *L, const flags_pair *arr)
{
  const flags_pair *p;
  lua_newtable(L);
  for(p=arr; p->key != NULL; p++) {
    lua_pushstring(L, p->key);
    lua_pushnumber(L, p->val);
    lua_rawset(L, -3);
  }
  return 1;
}

/* POSIX regex methods */

#ifdef LREXLIB_POSIX

#include <regex.h>

/* Test if regex.h corresponds to the extended POSIX library,
   i.e. H.Spencer's
*/
#if defined(REG_BASIC) && defined(REG_STARTEND)
#define LREXLIB_POSIX_EXT
#endif

const char posix_handle[] = "posix_regex_handle";
const char posix_typename[] = "posix_regex";

typedef struct {
  regex_t r;
  regmatch_t *match;
} posix2;      /* a better name is needed */

static int posix_comp(lua_State *L) {
  int res;
  posix2 *p2;
  size_t clen;
  const char *pattern = luaL_checklstring(L, 1, &clen);
  int cflags = luaL_optint(L, 2, REG_EXTENDED);

#ifdef LREXLIB_POSIX_EXT
  if((cflags & REG_EXTENDED) && (cflags & REG_NOSPEC)) {
    L_lua_error(L,
      "flags REG_EXTENDED and REG_NOSPEC must not be specified together");
  }
#endif

  p2 = (posix2 *)lua_newuserdata(L, sizeof(posix2));
  p2->match = NULL;

#ifdef LREXLIB_POSIX_EXT
  if(cflags & REG_PEND)
    p2->r.re_endp = pattern + clen;
#endif

  res = regcomp(&p2->r, pattern, cflags);
  if (res) {
    size_t sz = regerror(res, &p2->r, NULL, 0);
    char *errbuf = (char *) Lmalloc(L, sz);
    regerror(res, &p2->r, errbuf, sz);
    lua_pushstring(L, errbuf);
    free(errbuf);
    lua_error(L);
  }

  p2->match =
    (regmatch_t *) Lmalloc(L, (p2->r.re_nsub + 1) * sizeof(regmatch_t));
  luaL_getmetatable(L, posix_handle);
  lua_setmetatable(L, -2);
  return 1;
}

static void posix_getargs
  (lua_State *L, posix2 **p2, const char **text, size_t *text_len)
{
  *p2 = (posix2 *)luaL_checkudata(L, 1, posix_handle);
  luaL_argcheck(L, *p2 != NULL, 1, "compiled regexp expected");
  *text = luaL_checklstring(L, 2, text_len);
}

typedef void (*posix_push_matches)
  (lua_State *L, const char *text, int startoffset,
  regmatch_t *match, size_t ncapt);

static void posix_push_substrings
  (lua_State *L, const char *text, int startoffset,
  regmatch_t *match, size_t ncapt)
{
  size_t i;
  if(startoffset) {
    /* suppress compiler warning */
  }
  lua_newtable(L);
  for (i = 1; i <= ncapt; i++) {
    if (match[i].rm_so >= 0) {
      lua_pushlstring
        (L, text + match[i].rm_so, match[i].rm_eo - match[i].rm_so);
    }
    else
      lua_pushboolean(L, 0);
    lua_rawseti(L, -2, i);
  }
}

static void posix_push_offsets
  (lua_State *L, const char *text, int startoffset,
  regmatch_t *match, size_t ncapt)
{
  size_t i, j;
  if(text) {
    /* suppress compiler warning */
  }
  lua_newtable(L);
  for (i=1, j=1; i <= ncapt; i++) {
    if (match[i].rm_so >= 0) {
      lua_pushnumber(L, startoffset + match[i].rm_so + 1);
      lua_rawseti(L, -2, j++);
      lua_pushnumber(L, startoffset + match[i].rm_eo);
      lua_rawseti(L, -2, j++);
    }
    else {
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
    }
  }
}

static int posix_match_generic(lua_State *L, posix_push_matches push_matches)
{
  size_t elen;
  const char *text;
  posix2 *p2;
  int startoffset;
  int res;

#ifdef LREXLIB_POSIX_EXT
  int eflags = luaL_optint(L, 4, REG_STARTEND);
#else
  int eflags = luaL_optint(L, 4, 0);
#endif

  posix_getargs(L, &p2, &text, &elen);
  startoffset = get_startoffset(L, 3, elen);

#ifdef LREXLIB_POSIX_EXT
  if(eflags & REG_STARTEND) {
    p2->match[0].rm_so = startoffset;
    p2->match[0].rm_eo = elen;
    startoffset = 0;
  }
  else
    text += startoffset;
#else
  text += startoffset;
#endif

  /* execute the search */
  res = regexec(&p2->r, text, p2->r.re_nsub + 1, p2->match, eflags);
  if (res == 0) {
    lua_pushnumber(L, p2->match[0].rm_so + 1 + startoffset);
    lua_pushnumber(L, p2->match[0].rm_eo + startoffset);
    (*push_matches)(L, text, startoffset, p2->match, p2->r.re_nsub);
    return 3;
  }
  return 0;
}

static int posix_match(lua_State *L)
{
  return posix_match_generic(L, posix_push_substrings);
}

static int posix_exec(lua_State *L)
{
  return posix_match_generic(L, posix_push_offsets);
}

static int posix_gmatch(lua_State *L) {
  int res;
  size_t len;
  size_t nmatch = 0, limit = 0;
  const char *text;
  posix2 *p2;
  size_t maxmatch = (size_t)luaL_optnumber(L, 4, 0);

#ifdef LREXLIB_POSIX_EXT
  int eflags = luaL_optint(L, 5, REG_STARTEND);
#else
  int eflags = luaL_optint(L, 5, 0);
#endif

  posix_getargs(L, &p2, &text, &len);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  if(maxmatch > 0) /* this must be stated in the docs */
    limit = 1;

  while (!limit || nmatch < maxmatch) {

#ifdef LREXLIB_POSIX_EXT
    if(eflags & REG_STARTEND) {
      p2->match[0].rm_so = 0;
      p2->match[0].rm_eo = len;
    }
#endif

    res = regexec(&p2->r, text, p2->r.re_nsub + 1, p2->match, eflags);
    if (res == 0) {
      nmatch++;
      lua_pushvalue(L, 3);
      lua_pushlstring(L, text + p2->match[0].rm_so,
                      p2->match[0].rm_eo - p2->match[0].rm_so);
      posix_push_substrings(L, text, 0, p2->match, p2->r.re_nsub);
      lua_call(L, 2, 1);
      if(lua_toboolean(L, -1))
        break;
      lua_pop(L, 1);
      text += p2->match[0].rm_eo;

#ifdef LREXLIB_POSIX_EXT
      if(eflags & REG_STARTEND)
        len -= p2->match[0].rm_eo;
#endif

    } else
      break;
  }
  lua_pushnumber(L, nmatch);
  return 1;
}

static int posix_gc (lua_State *L) {
  posix2 *p2 = (posix2 *)luaL_checkudata(L, 1, posix_handle);
  if (p2) {
    regfree(&p2->r);
    if(p2->match)
      free(p2->match);
  p2 = NULL;
  }
  return 0;
}

static int posix_tostring (lua_State *L) {
  return regex_tostring(L, posix_handle, posix_typename);
}

static flags_pair posix_flags[] =
{
#ifdef LREXLIB_POSIX_EXT
  { "BASIC",    REG_BASIC },
  { "NOSPEC",   REG_NOSPEC },
  { "PEND",     REG_PEND },
  { "STARTEND", REG_STARTEND },
#endif
  { "EXTENDED", REG_EXTENDED },
  { "ICASE",    REG_ICASE },
  { "NOSUB",    REG_NOSUB },
  { "NEWLINE",  REG_NEWLINE },
  { "NOTBOL",   REG_NOTBOL },
  { "NOTEOL",   REG_NOTEOL },
  { NULL, 0 }
};

static int posix_get_flags (lua_State *L) {
  return regex_get_flags(L, posix_flags);
}

static const luaL_Reg posixmeta[] = {
  {"exec",       posix_exec},
  {"match",      posix_match},
  {"gmatch",     posix_gmatch},
  {"__gc",       posix_gc},
  {"__tostring", posix_tostring},
  {NULL, NULL}
};

#endif /* LREXLIB_POSIX */


/* PCRE methods */

#ifdef LREXLIB_PCRE

#include <locale.h>

#include "mc_pcre.h"

const char pcre_handle[] = "pcre_regex_handle";
const char pcre_typename[] = "pcre_regex";
static const char temp_match_handle[] = "pcre_temp_match_handle";

typedef struct {
  pcre2_code *pr;
  pcre2_match_data *md;
  int running;                /* in pcre2_match with a callout: see Lpcre_run */
  int *match;                 /* the offsets of the last match, in ints */
  int ncapt;
  const unsigned char *tables;
} lrex_pcre;

static const unsigned char *Lpcre_maketables(lua_State *L, int stackpos)
{
  const unsigned char *tables;
  char old_locale[256];
  const char *locale = luaL_checkstring(L, stackpos);

  strcpy(old_locale, setlocale(LC_CTYPE, NULL)); /* store the locale */
  if(NULL == setlocale(LC_CTYPE, locale))        /* set new locale */
    L_lua_error(L, "cannot set locale");
  tables = pcre2_maketables(NULL);         /* make tables with new locale */
  setlocale(LC_CTYPE, old_locale);         /* restore the old locale */
  return tables;
}

static int Lpcre_comp(lua_State *L)
{
  char buf[512];
  const char *error;
  int erroffset;
  uint32_t ncapt = 0;
  lrex_pcre *ud;
  size_t clen;  /* clen isn't used in PCRE */
  const char *pattern = luaL_checklstring(L, 1, &clen);
  int cflags = luaL_optint(L, 2, 0);
  const unsigned char *tables = NULL;

  if(lua_gettop(L) > 2 && !lua_isnil(L, 3))
    tables = Lpcre_maketables(L, 3);

  ud = (lrex_pcre*)lua_newuserdata(L, sizeof(lrex_pcre));
  luaL_getmetatable(L, pcre_handle);
  lua_setmetatable(L, -2);
  ud->match = NULL;
  ud->md = NULL;
  ud->running = 0;
  ud->tables = tables; /* keep this for eventual freeing */
  ud->pr = mc_pcre_compile(pattern, cflags, tables, 1, &error, &erroffset);
  if(!ud->pr)
    {
    sprintf(buf, "%s (pattern offset: %d)", error, erroffset+1);
                     /* show offset 1-based as it's common in Lua */
    L_lua_error(L, buf);
    }

  pcre2_pattern_info(ud->pr, PCRE2_INFO_CAPTURECOUNT, &ncapt);
  ud->ncapt = (int) ncapt;
  ud->md = pcre2_match_data_create_from_pattern(ud->pr, NULL);
  if(!ud->md)
    L_lua_error(L, "malloc failed");
  /* 2 ints per capture, plus 2 for the whole match */
  ud->match = (int *) Lmalloc(L, (ud->ncapt + 1) * 2 * sizeof(int));

  return 1;
}

static void Lpcre_getargs(lua_State *L, lrex_pcre **pud, const char **text,
                          size_t *text_len)
{
  *pud = (lrex_pcre *)luaL_checkudata(L, 1, pcre_handle);
  luaL_argcheck(L, *pud != NULL, 1, "compiled regexp expected");
  *text = luaL_checklstring(L, 2, text_len);
}

typedef void (*Lpcre_push_matches)
  (lua_State *L, const char *text, lrex_pcre * ud);

/* The length of a capture.  \K in a lookahead can leave the end before the start. */
static size_t capture_length (const int * match, int j)
{
  return match[j + 1] > match[j] ? (size_t) (match[j + 1] - match[j]) : 0;
}

static void Lpcre_push_substrings
  (lua_State *L, const char *text, lrex_pcre * ud)
{
  int i;
  uint32_t namecount = 0;
  PCRE2_SPTR name_table = NULL;
  uint32_t name_entry_size = 0;
  PCRE2_SPTR tabptr;
  const int *match = ud->match;

  lua_newtable(L);
  for (i = 1; i <= ud->ncapt; i++) {
    int j = i * 2;
    if (match[j] >= 0)
      lua_pushlstring(L, text + match[j], capture_length(match, j));
    else
      lua_pushboolean(L, 0);
    lua_rawseti(L, -2, i);
  }
  /* now do named subpatterns - NJG */
  pcre2_pattern_info(ud->pr, PCRE2_INFO_NAMECOUNT, &namecount);
  if (namecount == 0)
    return;
  pcre2_pattern_info(ud->pr, PCRE2_INFO_NAMETABLE, &name_table);
  pcre2_pattern_info(ud->pr, PCRE2_INFO_NAMEENTRYSIZE, &name_entry_size);

  // to handle duplicates - first add every name as a non-match

  tabptr = name_table;
  for (i = 0; i < (int) namecount; i++)
    {
    int n = (((int)tabptr[0]) << 8) | tabptr[1];
    const unsigned char * name = tabptr + 2;
    if (n >= 0 && n <= ud->ncapt)    // if in range
      {
      lua_pushstring (L, (const char *) name);  // name
      lua_pushboolean (L, 0);    // false
      lua_settable (L, -3);      // set it
      }
    tabptr += name_entry_size;
  }

  // now add the actual matches

  tabptr = name_table;
  for (i = 0; i < (int) namecount; i++)
    {
    int n = (((int)tabptr[0]) << 8) | tabptr[1];
    const unsigned char * name = tabptr + 2;
    if (n >= 0 && n <= ud->ncapt)
      {
      int j = n * 2;
      if (match[j] >= 0)
        {
        lua_pushstring (L, (const char *) name);  // name
        lua_pushlstring (L, text + match[j], capture_length(match, j)); // contents
        lua_settable (L, -3);      // set it
        }   // this one matched
      }  // n in range of captures
    tabptr += name_entry_size;
  }
}

static void Lpcre_push_offsets
  (lua_State *L, const char *text, lrex_pcre * ud)
{
  int i, j, k;
  if(text) {
    /* suppress compiler warning */
  }
  lua_newtable(L);
  for (i=1, j=1; i <= ud->ncapt; i++) {
    k = i * 2;
    if (ud->match[k] >= 0) {
      lua_pushnumber(L, ud->match[k] + 1);
      lua_rawseti(L, -2, j++);
      lua_pushnumber(L, ud->match[k+1]);
      lua_rawseti(L, -2, j++);
    }
    else {
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
    }
  }
}

// what a callout needs to know: the function is argument 5 or 6
typedef struct {
  lua_State *L;
  int f_loc;
  int failed;   /* it raised an error, which is on the Lua stack */
} callout_info;

// handle match callout
static int callout_function (pcre2_callout_block * cb, void * data)
  {
  callout_info * info = (callout_info *) data;

  // for substrings
  int i, j, k;

  int result;
  lua_State *L = info->L;
  if (!L)
    return 0;   // bad data

  lua_pushvalue(L, info->f_loc);    // get function
  lua_pushnumber (L, cb->callout_number); // arg 1: which callout

  // arg 2: offset vectors
  lua_newtable(L);
  for (i = 1, j = 1; i < (int) cb->capture_top; i++)
    {
    k = i * 2;
    if (cb->offset_vector[k] != PCRE2_UNSET)
      {
      lua_pushnumber(L, (lua_Number) cb->offset_vector[k] + 1);
      lua_rawseti(L, -2, j++);
      lua_pushnumber(L, (lua_Number) cb->offset_vector[k+1]);
      lua_rawseti(L, -2, j++);
      }
    else
      {
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      }
    } // end of doing each capture

  lua_pushlstring(L, (const char *) cb->subject, cb->subject_length);  // arg 3: subject
  lua_pushnumber (L, (lua_Number) cb->start_match + 1); // arg 4: start match position (1 relative)
  lua_pushnumber (L, (lua_Number) cb->current_position + 1); // arg 5: current position (1 relative)
  lua_pushnumber (L, (lua_Number) cb->capture_top - 1); // arg 6: highest captured substring
  // arg 7: last captured substring: PCRE 8 said -1 for none, PCRE2 says 0
  lua_pushnumber (L, cb->capture_last == 0 ? -1 : (lua_Number) cb->capture_last);


  // args: 1 - callback number
  //       2 - match vectors
  //       3 - subject string
  //       4 - start match position
  //       5 - current position
  //       6 - highest captured substring
  //       7 - last captured substring

  // Not lua_call: an error would longjmp out through the frames of JIT-compiled
  // code, which have no unwind information, and that takes the client down on
  // Windows.  The match is stopped first, and the error raised from Lpcre_run.
  if (lua_pcall(L, 7, 1, 0) != 0)
    {
    info->failed = 1;
    return PCRE2_ERROR_CALLOUT;
    }
  result = (int) lua_tonumber (L, -1);
  lua_pop(L, 1);          // result not wanted now

  if (result < 0)
    result = PCRE2_ERROR_NOMATCH;
  return result;
  }

static int temp_match_gc (lua_State *L)
{
  pcre2_match_data **box = (pcre2_match_data **) lua_touserdata(L, 1);
  if (box && *box)
    pcre2_match_data_free(*box);
  return 0;
}

// Run the pattern, leaving the offsets in ud->match.  A callout's script may run
// this same pattern, and the match data holds the outer match's frames, so an
// inner match gets match data of its own.
static int Lpcre_run (lua_State *L, lrex_pcre *ud, const char *text, size_t len,
                      int startoffset, int eflags, callout_info * info)
{
  int res;
  if (info)
    info->failed = 0;
  if (ud->running)
    {
    pcre2_match_data **box = (pcre2_match_data **) lua_newuserdata(L, sizeof *box);
    *box = NULL;
    luaL_getmetatable(L, temp_match_handle);
    lua_setmetatable(L, -2);
    *box = pcre2_match_data_create_from_pattern(ud->pr, NULL);
    if (!*box)
      L_lua_error(L, "malloc failed");
    res = mc_pcre_match(ud->pr, box, text, (int)len, startoffset, eflags,
                        info ? callout_function : NULL, (void *) info);
    if (info && info->failed)
      lua_error(L);   /* the message is above the box */
    if (res >= 0)
      mc_pcre_get_offsets(*box, ud->match, ud->ncapt + 1);
    lua_pop(L, 1);  /* the box, for the collector */
    return res;
    }

  ud->running = info != NULL;
  res = mc_pcre_match(ud->pr, &ud->md, text, (int)len, startoffset, eflags,
                      info ? callout_function : NULL, (void *) info);
  ud->running = 0;
  if (info && info->failed)
    lua_error(L);
  if (res >= 0)
    mc_pcre_get_offsets(ud->md, ud->match, ud->ncapt + 1);
  return res;
}

static int Lpcre_match_generic(lua_State *L, Lpcre_push_matches push_matches)
{
  int res;
  const char *text;
  lrex_pcre *ud;
  size_t elen;
  int startoffset;
  int eflags = luaL_optint(L, 4, 0);
  callout_info info;

  Lpcre_getargs(L, &ud, &text, &elen);
  startoffset = get_startoffset(L, 3, elen);

  // if callout function wanted, set up for it
  info.L = L;
  info.f_loc = 5;

  res = Lpcre_run(L, ud, text, elen, startoffset, eflags,
                  lua_isfunction (L, 5) ? &info : NULL);
  if (res >= 0) {
    lua_pushnumber(L, ud->match[0] + 1);
    lua_pushnumber(L, ud->match[1]);
    (*push_matches)(L, text, ud);
    return 3;
  }
  return 0;
}

static int Lpcre_match(lua_State *L)
{
  return Lpcre_match_generic(L, Lpcre_push_substrings);
}

static int Lpcre_exec(lua_State *L)
{
  return Lpcre_match_generic(L, Lpcre_push_offsets);
}

static int Lpcre_gmatch(lua_State *L)
{
  int res;
  size_t len;
  int nmatch = 0, limit = 0;
  const char *text;
  lrex_pcre *ud;
  int maxmatch = luaL_optint(L, 4, 0);
  int eflags = luaL_optint(L, 5, 0);
  int startoffset = 0;
  callout_info info;
  Lpcre_getargs(L, &ud, &text, &len);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  if(maxmatch > 0) /* this must be stated in the docs */
    limit = 1;

  info.L = L;
  info.f_loc = 6;

  while (!limit || nmatch < maxmatch)
    {

    res = Lpcre_run(L, ud, text, len, startoffset, eflags,
                    lua_isfunction (L, 6) ? &info : NULL);
    if (res >= 0)
      {
      nmatch++;
      lua_pushvalue(L, 3);
      lua_pushlstring(L, text + ud->match[0], capture_length(ud->match, 0));
      Lpcre_push_substrings(L, text, ud);
      startoffset = ud->match[1];
      lua_call(L, 2, 1);
      if(lua_toboolean(L, -1))
        break;
      lua_pop(L, 1);
    } else
      break;
  }
  lua_pushnumber(L, nmatch);
  return 1;
}

static int Lpcre_gc (lua_State *L)
{
  lrex_pcre *ud = (lrex_pcre *)luaL_checkudata(L, 1, pcre_handle);
  if (ud) {
    if(ud->pr)      pcre2_code_free(ud->pr);
    if(ud->md)      pcre2_match_data_free(ud->md);
    if(ud->tables)  pcre2_maketables_free(NULL, ud->tables);
    if(ud->match)   free(ud->match);
  }
  return 0;
}

static int Lpcre_tostring (lua_State *L) {
  return regex_tostring(L, pcre_handle, pcre_typename);
}

static int Lpcre_vers (lua_State *L)
{
  char version[64];
  pcre2_config(PCRE2_CONFIG_VERSION, version);
  lua_pushstring(L, version);
  return 1;
}

/* PCRE 8's numbers: mc_pcre.h.  A plugin may well hold one: rex.new (p, 1). */
static flags_pair pcre_flags[] =
{
  { "CASELESS",        PCRE_CASELESS },
  { "MULTILINE",       PCRE_MULTILINE },
  { "DOTALL",          PCRE_DOTALL },
  { "EXTENDED",        PCRE_EXTENDED },
  { "ANCHORED",        PCRE_ANCHORED },
  { "DOLLAR_ENDONLY",  PCRE_DOLLAR_ENDONLY },
  { "EXTRA",           PCRE_EXTRA },
  { "NOTBOL",          PCRE_NOTBOL },
  { "NOTEOL",          PCRE_NOTEOL },
  { "UNGREEDY",        PCRE_UNGREEDY },
  { "NOTEMPTY",        PCRE_NOTEMPTY },
  { "UTF8",            PCRE_UTF8 },

  { "AUTO_CALLOUT",    PCRE_AUTO_CALLOUT     },

  { "NO_AUTO_CAPTURE", PCRE_NO_AUTO_CAPTURE },
// NJG  { "NO_UTF8_CHECK",   PCRE_NO_UTF8_CHECK },

  { "PARTIAL",         PCRE_PARTIAL          },
  { "FIRSTLINE",       PCRE_FIRSTLINE        },
  { "DUPNAMES",        PCRE_DUPNAMES         },
  { "NEWLINE_CR",      PCRE_NEWLINE_CR       },
  { "NEWLINE_LF",      PCRE_NEWLINE_LF       },
  { "NEWLINE_CRLF",    PCRE_NEWLINE_CRLF     },
  { "NEWLINE_ANY",     PCRE_NEWLINE_ANY      },
  { "NEWLINE_ANYCRLF", PCRE_NEWLINE_ANYCRLF  },

  { NULL, 0 }
};

static int Lpcre_get_flags (lua_State *L) {
  return regex_get_flags(L, pcre_flags);
}

static const luaL_Reg pcremeta[] = {
  {"exec",       Lpcre_exec},
  {"match",      Lpcre_match},
  {"gmatch",     Lpcre_gmatch},
  {"__gc",       Lpcre_gc},
  {"__tostring", Lpcre_tostring},
  {NULL, NULL}
};

#endif /* defined(LREXLIB_PCRE) */


/* Open the library */

static const luaL_Reg rexlib[] = {
#ifdef LREXLIB_POSIX
  {"newPOSIX",    posix_comp},
  {"flagsPOSIX",  posix_get_flags},
#endif
#ifdef LREXLIB_PCRE
  {"new",     Lpcre_comp},
  {"flags",   Lpcre_get_flags},
  {"version", Lpcre_vers},
#endif
  {NULL, NULL}
};

static void createmeta(lua_State *L, const char *name)
{
  luaL_newmetatable(L, name);   /* create new metatable */
  lua_pushliteral(L, "__index");
  lua_pushvalue(L, -2);         /* push metatable */
  lua_rawset(L, -3);            /* metatable.__index = metatable */
}

LUALIB_API int luaopen_rex(lua_State *L)
{
#ifdef LREXLIB_POSIX
  createmeta(L, posix_handle);
  luaL_register (L, NULL, posixmeta);
  lua_pop(L, 1);
#endif
#ifdef LREXLIB_PCRE
  createmeta(L, pcre_handle);
  luaL_register (L, NULL, pcremeta);
  lua_pop(L, 1);
  luaL_newmetatable(L, temp_match_handle);
  lua_pushcfunction(L, temp_match_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
#endif
  luaL_register (L, "rex", rexlib);
  return 1;
}

