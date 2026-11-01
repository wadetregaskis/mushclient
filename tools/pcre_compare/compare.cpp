// compare.cpp -- runs every pattern through PCRE 8.45 and through the client's own
// mc_pcre layer over PCRE2, and reports each place they differ: whether the
// pattern compiles, its capture count and names, and for every subject, the return
// code and the offsets of every capture, with the JIT on and off.  See README.md.
//
//   pcre_compare patterns.txt subjects.txt [interp-stride] [count of patterns from real files]
//
// Both files have one hex-encoded byte string per line; each line of patterns.txt
// is "<options, decimal> <hex>".  Every pattern is run twice, as written (subjects
// as raw bytes) and with PCRE_UTF8 (so a subject that is not valid UTF-8 is an
// error both must report).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "mc_pcre.h"
#include "p8_side.h"

using std::string;
using std::vector;

static vector<string> read_hex_lines (const char * path, vector<int> * options)
  {
  vector<string> out;
  FILE * f = fopen (path, "rb");
  if (!f) { fprintf (stderr, "cannot open %s\n", path); exit (2); }
  static char line [1 << 20];
  while (fgets (line, sizeof line, f))
    {
    char * p = line;
    if (options)
      {
      options->push_back (atoi (p));
      p = strchr (p, ' ');
      if (!p) { options->pop_back (); continue; }
      p++;
      }
    string s;
    for (; p [0] && p [1] && p [0] != '\n' && p [0] != '\r'; p += 2)
      {
      char b [3] = { p [0], p [1], 0 };
      s += (char) strtol (b, NULL, 16);
      }
    out.push_back (s);
    }
  fclose (f);
  return out;
  }

static string show (const string & s, size_t max = 100)
  {
  string r;
  for (size_t i = 0; i < s.size () && i < max; i++)
    {
    unsigned char c = (unsigned char) s [i];
    if (c >= 0x20 && c < 0x7f) r += (char) c;
    else { char b [8]; sprintf (b, "\\x%02x", c); r += b; }
    }
  if (s.size () > max) r += "...";
  return r;
  }

static long total_runs = 0, total_matches = 0, total_nomatch = 0, total_errors = 0;
static long diff_count = 0, callout_runs = 0;
static std::map <string, long> diff_kinds;

static int examples_per_pattern = 2;
static size_t pattern_index = 0, real_patterns = 0;
static long real_diffs = 0;

static void report (const char * kind, const string & pattern, int options, const string & subject, const string & detail)
  {
  static std::map <string, long> per_pattern;
  diff_count++;
  diff_kinds [kind]++;
  if (pattern_index < real_patterns)
    real_diffs++;
  char key [32];
  sprintf (key, "%s %x ", kind, (unsigned) options);
  long & seen = per_pattern [key + pattern];
  if (++seen <= examples_per_pattern)
    printf ("DIFF %s %-16s opts=%#x /%s/  subject \"%s\"  %s\n", pattern_index < real_patterns ? "REAL" : "edge", kind, options,
            show (pattern).c_str (), show (subject).c_str (), detail.c_str ());
  }

typedef struct { int * log; int * used; int size; int result; } new_log;

// As p8_exec_logging, and as lrexlib's callout_function shows them to a script.
static int new_callout (pcre2_callout_block * cb, void * data)
  {
  new_log * l = (new_log *) data;
  if (*l->used + 5 + 2 * (int) cb->capture_top < l->size)
    {
    l->log [(*l->used)++] = (int) cb->callout_number;
    l->log [(*l->used)++] = (int) cb->capture_top;
    l->log [(*l->used)++] = cb->capture_last == 0 ? -1 : (int) cb->capture_last;
    l->log [(*l->used)++] = (int) cb->start_match;
    l->log [(*l->used)++] = (int) cb->current_position;
    for (int i = 2; i < 2 * (int) cb->capture_top; i++)
      l->log [(*l->used)++] = cb->offset_vector [i] == PCRE2_UNSET ? -1 : (int) cb->offset_vector [i];
    }
  return l->result;
  }

// One pair of runs: `a` is 8.45's result, `b` ours.
static bool same (int rca, const int * a, int rcb, const int * b, int pairs, string * why)
  {
  if (rca != rcb)
    {
    char buf [80];
    sprintf (buf, "rc 8.45=%d new=%d", rca, rcb);
    *why = buf;
    return false;
    }
  if (rca < 0)
    return true;
  for (int i = 0; i < pairs * 2; i++)
    if (a [i] != b [i])
      {
      string s = "offsets 8.45:";
      char buf [40];
      for (int j = 0; j < pairs * 2; j++) { sprintf (buf, " %d", a [j]); s += buf; }
      s += "  new:";
      for (int j = 0; j < pairs * 2; j++) { sprintf (buf, " %d", b [j]); s += buf; }
      *why = s;
      return false;
      }
  return true;
  }

// Same process, same patterns, same lines, turn about: so that anything else the
// machine is doing falls on both alike.  Each is run as regexec runs it.
static void bench (const vector<string> & patterns, const vector<int> & options, const vector<string> & subjects, size_t real)
  {
  struct Side { void * c8, * s8, * s8j; pcre2_code * c2, * c2j; pcre2_match_data * md, * mdj; vector<int> ov; int jit_only; };
  vector<Side> sides;
  for (size_t i = 0; i < real && i < patterns.size (); i++)
    {
    Side s;
    const char * e; int o;
    s.c8 = p8_compile (patterns [i].c_str (), options [i], &e, &o);
    s.c2 = mc_pcre_compile (patterns [i].c_str (), options [i], NULL, 0, &e, &o);
    s.c2j = mc_pcre_compile (patterns [i].c_str (), options [i], NULL, 1, &e, &o);
    if (!s.c8 || !s.c2 || !s.c2j) continue;
    s.s8 = p8_study (s.c8, 0);
    s.s8j = p8_study (s.c8, 1);
    s.md = pcre2_match_data_create_from_pattern (s.c2, NULL);
    s.mdj = pcre2_match_data_create_from_pattern (s.c2j, NULL);
    s.ov.resize ((p8_capture_count (s.c8) + 1) * 3);
    s.jit_only = mc_pcre_is_jit_only (s.c2j);
    sides.push_back (s);
    }
  vector<const string *> lines;
  for (size_t i = 0; i < subjects.size (); i += 16)
    if (subjects [i].size () < 200)
      lines.push_back (&subjects [i]);
  printf ("\nspeed: %d patterns x %d lines, %s\n", (int) sides.size (), (int) lines.size (), "ms for each pass, turn about");
  typedef std::chrono::steady_clock clk;
  double t [4][7];
  long sink = 0;
  for (int round = 0; round < 7; round++)
    {
    for (int which = 0; which < 4; which++)
      {
      clk::time_point a = clk::now ();
      for (size_t l = 0; l < lines.size (); l++)
        {
        const char * sub = lines [l]->data ();
        const int len = (int) lines [l]->size ();
        for (size_t p = 0; p < sides.size (); p++)
          {
          Side & s = sides [p];
          int rc = 0;
          switch (which)
            {
            case 0: rc = p8_exec (s.c8, s.s8j, sub, len, 0, PCRE_NOTEMPTY, &s.ov [0], (int) s.ov.size ()); break;
            case 1: rc = s.jit_only ? mc_pcre_match_jit (s.c2j, &s.mdj, sub, len, 0, PCRE_NOTEMPTY)
                                    : mc_pcre_match (s.c2j, &s.mdj, sub, len, 0, PCRE_NOTEMPTY, NULL, NULL); break;
            case 2: rc = p8_exec (s.c8, s.s8, sub, len, 0, PCRE_NOTEMPTY, &s.ov [0], (int) s.ov.size ()); break;
            case 3: rc = mc_pcre_match (s.c2, &s.md, sub, len, 0, PCRE_NOTEMPTY, NULL, NULL); break;
            }
          sink += rc;
          }
        }
      t [which][round] = std::chrono::duration<double, std::milli> (clk::now () - a).count ();
      }
    }
  const char * names [4] = { "8.45 JIT", "10.49 JIT", "8.45 interpreted", "10.49 interpreted" };
  double best [4];
  for (int which = 0; which < 4; which++)
    {
    best [which] = t [which][0];
    printf ("  %-18s", names [which]);
    for (int round = 0; round < 7; round++)
      {
      printf (" %8.1f", t [which][round]);
      if (t [which][round] < best [which]) best [which] = t [which][round];
      }
    printf ("   best %8.1f\n", best [which]);
    }
  printf ("  10.49 against 8.45, best passes: JIT %.3f, interpreted %.3f  (%ld)\n", best [1] / best [0], best [3] / best [2], sink);
  }

int main (int argc, char ** argv)
  {
  if (argc < 3)
    {
    fprintf (stderr, "usage: pcre_compare patterns.txt subjects.txt [interp-stride]\n");
    return 2;
    }
  vector<int> pattern_options;
  vector<string> patterns = read_hex_lines (argv [1], &pattern_options);
  vector<string> subjects = read_hex_lines (argv [2], NULL);
  const int interp_stride = argc > 3 ? atoi (argv [3]) : 1;
  real_patterns = argc > 4 ? (size_t) atoi (argv [4]) : 0;

  char v2 [64];
  pcre2_config (PCRE2_CONFIG_VERSION, v2);
  printf ("old: PCRE %s   new: PCRE2 %s via mc_pcre\n", p8_version (), v2);
  printf ("patterns: %d   subjects: %d   interpreted runs on every %d subject\n",
          (int) patterns.size (), (int) subjects.size (), interp_stride);

  long compiled_both = 0, only_old = 0, only_new = 0, neither = 0, rewritten = 0, jit_old = 0, jit_new = 0;
  long variants = 0;

  for (size_t pi = 0; pi < patterns.size (); pi++)
    for (int utf = 0; utf < 2; utf++)
      {
      pattern_index = pi;
      const string & pat = patterns [pi];
      const int options = pattern_options [pi] | (utf ? PCRE_UTF8 : 0);
      if (pat.find ('\0') != string::npos)
        continue;
      if (utf && (pattern_options [pi] & PCRE_UTF8))
        continue;
      variants++;

      const char * error8 = NULL; int off8 = 0;
      void * code8 = p8_compile (pat.c_str (), options, &error8, &off8);
      const char * error2 = NULL; int off2 = 0;
      pcre2_code * code2 = mc_pcre_compile (pat.c_str (), options, NULL, 0, &error2, &off2);
      pcre2_code * code2j = mc_pcre_compile (pat.c_str (), options, NULL, 1, &error2, &off2);

      if (!code8 && !code2) { neither++; continue; }
      if (code8 && !code2)
        {
        only_old++;
        report ("only-8.45", pat, options, "", string ("PCRE2: ") + (error2 ? error2 : "?"));
        p8_free (code8, NULL);
        continue;
        }
      if (!code8 && code2)
        {
        only_new++;
        report ("only-PCRE2", pat, options, "", string ("8.45: ") + (error8 ? error8 : "?"));
        pcre2_code_free (code2);
        pcre2_code_free (code2j);
        continue;
        }
      compiled_both++;
      char * fixed = mc_pcre_rewrite (pat.c_str (), options);
      if (fixed) { rewritten++; free (fixed); }

      // capture count, names
      uint32_t cc2 = 0;
      pcre2_pattern_info (code2, PCRE2_INFO_CAPTURECOUNT, &cc2);
      int cc8 = p8_capture_count (code8);
      if ((int) cc2 != cc8)
        {
        char b [64]; sprintf (b, "8.45=%d new=%u", cc8, cc2);
        report ("capture-count", pat, options, "", b);
        }
      const unsigned char * t8 = NULL; int es8 = 0;
      int n8 = p8_name_table (code8, &t8, &es8);
      uint32_t n2 = 0, es2 = 0; PCRE2_SPTR t2 = NULL;
      pcre2_pattern_info (code2, PCRE2_INFO_NAMECOUNT, &n2);
      pcre2_pattern_info (code2, PCRE2_INFO_NAMETABLE, &t2);
      pcre2_pattern_info (code2, PCRE2_INFO_NAMEENTRYSIZE, &es2);
      if ((int) n2 != n8)
        report ("name-count", pat, options, "", "");
      else
        for (int i = 0; i < n8; i++)
          {
          const unsigned char * a = t8 + i * es8, * b = t2 + i * es2;
          if (a [0] != b [0] || a [1] != b [1] || strcmp ((const char *) a + 2, (const char *) b + 2) != 0)
            report ("name-table", pat, options, "", (const char *) a + 2);
          }

      void * study8 = p8_study (code8, 0);
      void * study8j = p8_study (code8, 1);
      size_t jitsize = 0;
      pcre2_pattern_info (code2j, PCRE2_INFO_JITSIZE, &jitsize);
      if (p8_jit_active (study8j)) jit_old++;
      if (jitsize > 0) jit_new++;

      const int pairs = (int) cc2 + 1;
      vector<int> ov8 (pairs * 3), ov8i (pairs * 3), ov2 (pairs * 2), ov2i (pairs * 2);
      pcre2_match_data * md = pcre2_match_data_create_from_pattern (code2, NULL);
      pcre2_match_data * mdj = pcre2_match_data_create_from_pattern (code2j, NULL);
      const int jit_only = mc_pcre_is_jit_only (code2j);   // as regexec does

      // Callouts, for the patterns that have them.  What a script sees through rex,
      // with the JIT on, and off.  Studied, as the client's patterns always were:
      // start optimisations decide whether a callout runs at all.
      if (pat.find ("(?C") != string::npos || (options & PCRE_AUTO_CALLOUT))
        for (int jit = 0; jit < 2; jit++)
        {
        static int log8 [200000], log2 [200000];
        for (size_t si = 0; si < subjects.size () && si < 3000; si++)
          for (int result = 0; result < 2; result++)
            {
            const string & sub = subjects [si];
            const int len = (int) sub.size ();
            int used8 = 0, used2 = 0;
            int rc8 = p8_exec_logging (code8, jit ? study8j : study8, sub.data (), len, 0, &ov8 [0], pairs * 3, result, log8, &used8, 200000);
            new_log l = { log2, &used2, 200000, result };
            int rc2 = jit ? mc_pcre_match (code2j, &mdj, sub.data (), len, 0, 0, new_callout, &l)
                          : mc_pcre_match (code2, &md, sub.data (), len, 0, 0, new_callout, &l);
            total_runs++;
            callout_runs++;
            if (rc8 != rc2)
              {
              char b [64]; sprintf (b, "rc 8.45=%d new=%d, callout returns %d", rc8, rc2, result);
              report ("callout-rc", pat, options, sub, b);
              }
            else if (used8 != used2 || memcmp (log8, log2, used8 * sizeof (int)) != 0)
              {
              string d = "log 8.45:";
              char b [32];
              for (int i = 0; i < used8 && i < 24; i++) { sprintf (b, " %d", log8 [i]); d += b; }
              d += "  new:";
              for (int i = 0; i < used2 && i < 24; i++) { sprintf (b, " %d", log2 [i]); d += b; }
              report ("callout-log", pat, options, sub, d);
              }
            }
        }

      for (size_t si = 0; si < subjects.size (); si++)
        {
        const string & sub = subjects [si];
        const int len = (int) sub.size ();
        // PCRE_NOTEMPTY is what regexec uses, unless the preference says otherwise;
        // and each subject is searched on from the end of a match, as a Lua gmatch
        // and a Find do
        for (int flags = 0; flags < 2; flags++)
          {
          const int exec_options = flags ? PCRE_NOTEMPTY : 0;
          int start = 0;
          for (int round = 0; round < 3 && start <= len; round++)
            {
            string why;
            // JIT against JIT, and 8.45's JIT is what the client ran
            int rc8 = p8_exec (code8, study8j, sub.data (), len, start, exec_options, &ov8 [0], pairs * 3);
            int rc2 = jit_only
                      ? mc_pcre_match_jit (code2j, &mdj, sub.data (), len, start, exec_options)
                      : mc_pcre_match (code2j, &mdj, sub.data (), len, start, exec_options, NULL, NULL);
            if (rc2 >= 0) mc_pcre_get_offsets (mdj, &ov2 [0], pairs);
            total_runs++;
            if (rc8 >= 0) total_matches++;
            else if (rc8 == PCRE_ERROR_NOMATCH) total_nomatch++;
            else total_errors++;
            if (!same (rc8, &ov8 [0], rc2, &ov2 [0], pairs, &why))
              report (utf ? "match-jit-utf" : "match-jit-raw", pat, options, sub, why);
            // Interpreters
            if (interp_stride > 0 && si % interp_stride == 0)
              {
              int rc8i = p8_exec (code8, study8, sub.data (), len, start, exec_options, &ov8i [0], pairs * 3);
              int rc2i = mc_pcre_match (code2, &md, sub.data (), len, start, exec_options, NULL, NULL);
              if (rc2i >= 0) mc_pcre_get_offsets (md, &ov2i [0], pairs);
              total_runs++;
              if (!same (rc8i, &ov8i [0], rc2i, &ov2i [0], pairs, &why))
                report (utf ? "match-int-utf" : "match-int-raw", pat, options, sub, why);
              if (!same (rc2, &ov2 [0], rc2i, &ov2i [0], pairs, &why))
                report ("new-jit-vs-int", pat, options, sub, why);
              }
            if (rc8 < 0 || rc2 < 0)
              break;
            start = ov8 [1] > start ? ov8 [1] : start + 1;
            }
          }
        }

      pcre2_match_data_free (md);
      pcre2_match_data_free (mdj);
      pcre2_code_free (code2);
      pcre2_code_free (code2j);
      p8_free (code8, study8);
      if (study8j) { /* freed with the code by the OS at exit: pcre_free_study needs its own code */ }
      fflush (stdout);
      }

  bench (patterns, pattern_options, subjects, real_patterns);

  printf ("\nvariants (pattern x raw/UTF): %ld\n", variants);
  printf ("compile: both %ld, neither %ld, only 8.45 %ld, only PCRE2 %ld\n", compiled_both, neither, only_old, only_new);
  printf ("rewritten before compiling, among those both accept: %ld\n", rewritten);
  printf ("JIT compiled: 8.45 %ld, PCRE2 %ld\n", jit_old, jit_new);
  printf ("match runs: %ld  (8.45 JIT runs: %ld matched, %ld no match, %ld runtime errors)\n",
          total_runs, total_matches, total_nomatch, total_errors);
  printf ("callout runs, within those: %ld\n", callout_runs);
  printf ("disagreements: %ld (%ld of them on patterns from real files)\n", diff_count, real_diffs);
  for (std::map <string, long>::const_iterator it = diff_kinds.begin (); it != diff_kinds.end (); ++it)
    printf ("  %-18s %ld\n", it->first.c_str (), it->second);
  return diff_count ? 1 : 0;
  }
