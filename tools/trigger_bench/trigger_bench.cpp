// trigger_bench -- the trigger path's regular expressions, headless.  See README.md.
//
//   trigger_bench verify <patterns> <subjects>
//     Every pattern against every subject: wherever the required-literal prefilter
//     (mc_pcre_required_literal / mc_pcre_has_literal) would skip a pattern, the
//     pattern must not match, with PCRE_NOTEMPTY and without.  Any line saying
//     MISSED is a trigger that the prefilter would have made miss a line.  The
//     literal search is also checked against a plain one (SEARCH WRONG).
//
//   trigger_bench time <patterns> <subjects> [passes]
//     The cost per line of running every enabled pattern over each subject the way
//     regexec does (strlen, two clock reads, the JIT), with and without the
//     prefilter and the clock reads, and which patterns cost most.
//
//   trigger_bench which <patterns> <subjects>
//     Which enabled patterns match each subject.
//
// patterns: one per line, "<options> <hex pattern> [enabled [name]]", options being
// PCRE 8's bits (1 caseless, 2 multi-line, 0x800 UTF-8).  subjects: one per line,
// in hex if the file name ends in .hex, otherwise as text.

#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "mc_pcre.h"

struct Pattern
  {
  std::string text, name;
  int options = 0;
  bool enabled = true;
  pcre2_code * code = nullptr;
  pcre2_match_data * md = nullptr;
  int jit_only = 0;
  char literal [64];
  int literal_length = 0;
  int anchor = 0;
  double cost = 0;
  long matches = 0, skipped = 0;
  };

static std::string unhex (const std::string & h)
  {
  std::string r;
  for (size_t i = 0; i + 1 < h.size (); i += 2)
    r += (char) strtol (h.substr (i, 2).c_str (), nullptr, 16);
  return r;
  }

static std::vector<Pattern> load_patterns (const char * file)
  {
  std::vector<Pattern> v;
  FILE * f = fopen (file, "rb");
  if (!f) { perror (file); exit (2); }
  char line [65536];
  while (fgets (line, sizeof line, f))
    {
    char hex [65536] = "", name [1024] = "";
    int options = 0, enabled = 1;
    int n = sscanf (line, "%i %65535s %d %1023s", &options, hex, &enabled, name);
    if (n < 2)
      continue;
    Pattern p;
    p.options = options;
    p.text = unhex (hex);
    p.enabled = enabled != 0;
    p.name = name;
    v.push_back (p);
    }
  fclose (f);
  return v;
  }

static std::vector<std::string> load_subjects (const char * file)
  {
  std::vector<std::string> v;
  FILE * f = fopen (file, "rb");
  if (!f) { perror (file); exit (2); }
  const bool hex = strlen (file) > 4 && _stricmp (file + strlen (file) - 4, ".hex") == 0;
  std::string cur;
  int c;
  std::string all;
  while ((c = fgetc (f)) != EOF)
    all += (char) c;
  fclose (f);
  size_t i = 0;
  while (i <= all.size ())
    {
    size_t j = all.find ('\n', i);
    if (j == std::string::npos)
      j = all.size ();
    std::string l = all.substr (i, j - i);
    if (!l.empty () && l.back () == '\r')
      l.pop_back ();
    if (j < all.size () || !l.empty ())
      v.push_back (hex ? unhex (l) : l);
    i = j + 1;
    }
  return v;
  }

static void compile_all (std::vector<Pattern> & pats)
  {
  int bad = 0;
  for (auto & p : pats)
    {
    const char * error;
    int offset;
    p.code = mc_pcre_compile (p.text.c_str (), p.options, NULL, 1, &error, &offset);
    if (!p.code)
      {
      bad++;
      continue;
      }
    p.md = pcre2_match_data_create_from_pattern (p.code, NULL);
    p.jit_only = mc_pcre_is_jit_only (p.code);
    p.literal_length = mc_pcre_required_literal (p.text.c_str (), p.options, p.literal, sizeof p.literal);
    if (p.literal_length)
      p.anchor = mc_pcre_literal_anchor (p.literal, p.literal_length, (p.options & PCRE_CASELESS) != 0);
    }
  int with = 0;
  for (auto & p : pats)
    if (p.code && p.literal_length)
      with++;
  printf ("patterns: %zu, %d did not compile, %d have a required literal\n", pats.size (), bad, with);
  }

// What regexec does, minus the bookkeeping.
static int run (Pattern & p, const char * s, int len, int options)
  {
  if (p.jit_only)
    return mc_pcre_match_jit (p.code, &p.md, s, len, 0, options);
  return mc_pcre_match (p.code, &p.md, s, len, 0, options, NULL, NULL);
  }

static bool all_ascii (const std::string & s)
  {
  for (unsigned char c : s)
    if (c >= 0x80)
      return false;
  return true;
  }

// The obvious search, to check mc_pcre_has_literal by.
static bool naive_has (const std::string & s, const char * lit, int n, bool caseless)
  {
  for (size_t i = 0; i + n <= s.size (); i++)
    {
    int j = 0;
    for (; j < n; j++)
      {
      unsigned char c = (unsigned char) s [i + j];
      if (caseless && c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
      if (c != (unsigned char) lit [j])
        break;
      }
    if (j == n)
      return true;
    }
  return false;
  }

static int verify (const char * pfile, const char * sfile)
  {
  auto pats = load_patterns (pfile);
  auto subs = load_subjects (sfile);
  compile_all (pats);
  printf ("subjects: %zu\n", subs.size ());
  long missed = 0, errors = 0, skips = 0, matches = 0, checks = 0, search_wrong = 0;
  for (auto & p : pats)
    {
    if (!p.code)
      continue;
    for (auto & s : subs)
      {
      // regexec's rule: the literal is missing, and in UTF-8 mode the subject is ASCII
      const bool caseless = (p.options & PCRE_CASELESS) != 0;
      const bool has = !p.literal_length ||
          mc_pcre_has_literal (s.data (), s.size (), p.literal, p.literal_length, caseless, p.anchor);
      if (p.literal_length && has != naive_has (s, p.literal, p.literal_length, caseless))
        {
        search_wrong++;
        if (search_wrong < 20)
          printf ("SEARCH WRONG: literal [%.*s] anchor %d caseless %d subject [%s]\n",
                  p.literal_length, p.literal, p.anchor, caseless, s.c_str ());
        }
      const bool skip = !has && (!(p.options & PCRE_UTF8) || all_ascii (s));
      for (int o = 0; o < 2; o++)
        {
        const int rc = run (p, s.c_str (), (int) s.size (), o ? PCRE_NOTEMPTY : 0);
        checks++;
        if (rc >= 0)
          matches++;
        if (!skip)
          continue;
        skips++;
        if (rc >= 0)
          {
          missed++;
          if (missed < 50)
            printf ("MISSED: pattern [%s] options %d literal [%.*s] subject [%s]\n", p.text.c_str (),
                    p.options, p.literal_length, p.literal, s.c_str ());
          }
        else if (rc != PCRE_ERROR_NOMATCH)
          {
          errors++;
          if (errors < 20)
            printf ("error %d becomes no match: pattern [%s] subject [%s]\n", rc, p.text.c_str (), s.c_str ());
          }
        }
      }
    }
  printf ("checks: %ld, matches: %ld, skipped by the prefilter: %ld, of which matched: %ld, "
          "errors that become a plain no-match: %ld, literal searches that disagree with a "
          "plain search: %ld\n", checks, matches, skips, missed, errors, search_wrong);
  printf (missed || search_wrong ? "FAIL\n" : "OK\n");
  return missed || search_wrong ? 1 : 0;
  }

static double now ()
  {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart)
    QueryPerformanceFrequency (&f);
  LARGE_INTEGER t;
  QueryPerformanceCounter (&t);
  return (double) t.QuadPart / (double) f.QuadPart;
  }

static int timing (const char * pfile, const char * sfile, int passes)
  {
  auto all = load_patterns (pfile);
  std::vector<Pattern> pats;
  for (auto & p : all)
    if (p.enabled)
      pats.push_back (p);
  auto subs = load_subjects (sfile);
  compile_all (pats);
  pats.erase (std::remove_if (pats.begin (), pats.end (), [] (const Pattern & p) { return !p.code; }), pats.end ());
  printf ("enabled patterns: %zu, subjects: %zu, passes: %d\n", pats.size (), subs.size (), passes);

  // the cost of a clock read
  {
  const int n = 1000000;
  LARGE_INTEGER t;
  double a = now ();
  for (int i = 0; i < n; i++)
    QueryPerformanceCounter (&t);
  printf ("QueryPerformanceCounter: %.1f ns a call\n", (now () - a) / n * 1e9);
  }

  long hits = 0;
  for (int variant = 0; variant < 4; variant++)
    {
    const bool prefilter = variant & 1, clock = !(variant & 2);
    double best = 1e30;
    for (int rep = 0; rep < 5; rep++)
      {
      hits = 0;
      double a = now ();
      for (int pass = 0; pass < passes; pass++)
        for (auto & s : subs)
          for (auto & p : pats)
            {
            const char * str = s.c_str ();
            int len = (int) strlen (str);
            LARGE_INTEGER t0, t1;
            if (clock)
              QueryPerformanceCounter (&t0);
            int rc;
            if (prefilter && p.literal_length &&
                !mc_pcre_has_literal (str, len, p.literal, p.literal_length, (p.options & PCRE_CASELESS) != 0, p.anchor))
              rc = PCRE_ERROR_NOMATCH;
            else
              rc = run (p, str, len, PCRE_NOTEMPTY);
            if (clock)
              QueryPerformanceCounter (&t1);
            if (rc >= 0)
              hits++;
            }
      double e = now () - a;
      best = std::min (best, e);
      }
    printf ("%-30s %8.2f us a line  (%ld matches a pass)\n",
            variant == 0 ? "as now" : variant == 1 ? "prefilter" : variant == 2 ? "no clock reads" : "prefilter, no clock reads",
            best / passes / subs.size () * 1e6, hits / passes);
    }

  // per-pattern cost, without the prefilter, and what the prefilter skips
  for (auto & p : pats)
    {
    double a = now ();
    for (int pass = 0; pass < passes; pass++)
      for (auto & s : subs)
        if (run (p, s.c_str (), (int) s.size (), PCRE_NOTEMPTY) >= 0)
          p.matches++;
    p.cost = (now () - a) / passes / subs.size () * 1e9;
    for (auto & s : subs)
      if (p.literal_length && !mc_pcre_has_literal (s.data (), s.size (), p.literal, p.literal_length,
                                                    (p.options & PCRE_CASELESS) != 0, p.anchor))
        p.skipped++;
    }
  std::vector<Pattern *> order;
  double total = 0;
  for (auto & p : pats)
    {
    order.push_back (&p);
    total += p.cost;
    }
  std::sort (order.begin (), order.end (), [] (Pattern * a, Pattern * b) { return a->cost > b->cost; });
  printf ("sum of the patterns' own costs: %.2f us a line; the dearest:\n", total / 1000);
  for (size_t i = 0; i < order.size () && i < 25; i++)
    printf ("%8.0f ns  %3ld%% skipped  lit [%.*s]  %.100s\n", order [i]->cost,
            order [i]->skipped * 100 / (long) subs.size (), order [i]->literal_length, order [i]->literal,
            order [i]->text.c_str ());
  long nolit = 0;
  double nolit_cost = 0;
  for (auto & p : pats)
    if (!p.literal_length)
      {
      nolit++;
      nolit_cost += p.cost;
      }
  printf ("without a literal: %ld patterns, %.2f us a line; e.g.\n", nolit, nolit_cost / 1000);
  int shown = 0;
  for (auto * p : order)
    if (!p->literal_length && shown++ < 15)
      printf ("%8.0f ns  %.120s\n", p->cost, p->text.c_str ());
  return 0;
  }

// Which enabled patterns match each subject.
static int which (const char * pfile, const char * sfile)
  {
  auto pats = load_patterns (pfile);
  auto subs = load_subjects (sfile);
  compile_all (pats);
  for (auto & s : subs)
    {
    std::string hit;
    for (auto & p : pats)
      if (p.code && p.enabled && run (p, s.c_str (), (int) s.size (), PCRE_NOTEMPTY) >= 0)
        hit += " " + p.name;
    printf ("%s\t%s\n", hit.empty () ? "-" : hit.c_str () + 1, s.c_str ());
    }
  return 0;
  }

int main (int argc, char ** argv)
  {
  if (argc >= 4 && !strcmp (argv [1], "which"))
    return which (argv [2], argv [3]);
  if (argc >= 4 && !strcmp (argv [1], "verify"))
    return verify (argv [2], argv [3]);
  if (argc >= 4 && !strcmp (argv [1], "time"))
    return timing (argv [2], argv [3], argc > 4 ? atoi (argv [4]) : 20);
  fprintf (stderr, "trigger_bench verify|time <patterns> <subjects> [passes]\n");
  return 2;
  }
