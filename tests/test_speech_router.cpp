// tests/test_speech_router.cpp
// Speech: the router against a recording backend, and then the real document, to show what
// reaches a screen reader and, above all, that nobody without one hears or sees anything.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "a11y.h"
#include "speech_router.h"
#include "flags.h"
#include "scripting/errors.h"
#include "fake_transport.h"
#include "fake_worldview.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using a11y::SpeechRouter;
using pal::SpeechPriority;

namespace {

struct FakeSpeech : pal::ISpeech
  {
  unsigned caps = pal::kSpeechScreenReader | pal::kSpeechCanStop;
  bool busy = false;
  std::vector<std::string> said;
  std::vector<SpeechPriority> priorities;
  int stops = 0;
  int flushes = 0;
  std::string voice = "unset";
  int rate = 99;
  std::string label = "Fake";

  unsigned capabilities () const override { return caps; }
  const char * name () const override { return label.c_str (); }
  bool speak (const char * utf8, SpeechPriority priority, bool bFlush) override
    {
    said.push_back (utf8);
    priorities.push_back (priority);
    if (bFlush)
      flushes++;
    return true;
    }
  void stop () override { stops++; }
  bool isBusy () override { return busy; }
  std::vector<std::string> brailled;
  void braille (const char * utf8) override { brailled.push_back (utf8); }
  void setRate (int r) override { rate = r; }
  void setVoice (const char * v) override { voice = v; }
  };

struct FakePlatform : pal::ISpeechPlatform
  {
  FakeSpeech reader;
  FakeSpeech voice;
  bool bReader = false;
  int ownVoiceAsked = 0;

  FakePlatform () { voice.caps = pal::kSpeechKnowsBusy | pal::kSpeechCanStop | pal::kSpeechVoices; voice.label = "Voice"; }
  pal::ISpeech * screenReader () override { return bReader ? &reader : nullptr; }
  pal::ISpeech * ownVoice () override { ownVoiceAsked++; return &voice; }
  };

std::string Joined (const std::vector<std::string> & v)
  {
  std::string out;
  for (const std::string & s : v)
    out += s + "|";
  return out;
  }

}  // namespace

// ---------------------------------------------------------------- the router

TEST (SpeechRouter, LinesArriveTogetherAtTheNextTick)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("one", "output");
  r.line ("two", "output");
  EXPECT_TRUE (s.said.empty ());
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("one\ntwo", s.said [0].c_str ());
}

TEST (SpeechRouter, BrailleGetsTheNewestLineIfTheBackendCanDoIt)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("no display here", "output");
  r.tick (1000);
  EXPECT_TRUE (s.brailled.empty ());

  s.caps |= pal::kSpeechBraille;
  r.line ("one", "output");
  r.line ("two", "output");
  r.tick (9000);
  ASSERT_EQ (1u, s.brailled.size ());
  EXPECT_STREQ ("two", s.brailled [0].c_str ());

  r.setBraille (false);
  r.line ("three", "output");
  r.tick (20000);
  EXPECT_EQ (1u, s.brailled.size ());
}

TEST (SpeechRouter, BlankLinesAreRememberedButNotSaid)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("   ", "output");
  r.tick (1000);
  EXPECT_TRUE (s.said.empty ());
  EXPECT_EQ (1u, r.history ().size ());
}

TEST (SpeechRouter, EnterSilencesEverythingWaiting)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("old", "output");
  r.commandEntered ();
  r.tick (1000);
  EXPECT_TRUE (s.said.empty ());
  EXPECT_EQ (1, s.stops);
}

TEST (SpeechRouter, AFloodKeepsTheNewestAndSaysHowManyWent)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.policy ().maxPending = 10;
  r.policy ().keepLatest = 2;
  for (int i = 1; i <= 50; i++)
    r.line ("line " + std::to_string (i), "output");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("48 lines skipped.\nline 49\nline 50", s.said [0].c_str ());
  EXPECT_EQ ((uint64_t) 48, r.linesSkipped ());
}

TEST (SpeechRouter, NoLimitMeansNothingIsDropped)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.policy ().maxPending = 0;
  r.policy ().maxBatchChars = 100000;
  for (int i = 1; i <= 200; i++)
    r.line ("x", "output");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_EQ ((uint64_t) 0, r.linesSkipped ());
  EXPECT_EQ (399u, s.said [0].size ());
}

TEST (SpeechRouter, WhatMattersSurvivesAFlood)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.policy ().maxPending = 5;
  r.policy ().keepLatest = 1;
  for (int i = 1; i <= 20; i++)
    r.line ("spam", "output");
  r.say ("you are being attacked", "trigger", SpeechPriority::Assertive, false);
  for (int i = 1; i <= 20; i++)
    r.line ("spam", "output");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_TRUE (s.said [0].find ("you are being attacked") != std::string::npos);
  EXPECT_TRUE (s.said [0].find ("39 lines skipped.") != std::string::npos);
  EXPECT_EQ (SpeechPriority::Assertive, s.priorities [0]);
}

TEST (SpeechRouter, ExactBackendWaitsUntilItIsNotBusy)
{
  SpeechRouter r;
  FakeSpeech s;
  s.caps |= pal::kSpeechKnowsBusy;
  r.setBackend (&s);
  r.line ("one", "output");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());

  s.busy = true;
  r.line ("two", "output");
  r.tick (2000);
  EXPECT_EQ (1u, s.said.size ());

  s.busy = false;
  r.tick (3000);
  EXPECT_EQ (2u, s.said.size ());
}

TEST (SpeechRouter, ABackendStuckOnBusyDoesNotSilenceEverythingForEver)
{
  SpeechRouter r;
  FakeSpeech s;
  s.caps |= pal::kSpeechKnowsBusy;
  s.busy = true;
  r.setBackend (&s);
  r.line ("one", "output");
  r.tick (1000);
  EXPECT_EQ (0u, s.said.size ());    // it says it is busy, and it might be
  r.tick (7000);
  EXPECT_EQ (1u, s.said.size ());    // that long is not to be believed
  r.line ("two", "output");
  r.tick (7500);
  EXPECT_EQ (1u, s.said.size ());
  r.tick (20000);
  EXPECT_EQ (2u, s.said.size ());
}

TEST (SpeechRouter, EstimatedBackendIsOnlyAFewSecondsAhead)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.policy ().estimatedCharsPerSecond = 10;
  r.policy ().leadMs = 4000;
  r.policy ().maxBatchChars = 50;

  r.line (std::string (100, 'a'), "output");    // ten seconds of it
  r.tick (1000);
  EXPECT_EQ (1u, s.said.size ());

  r.line ("next", "output");
  r.tick (2000);          // nine seconds still to go: it would be too far behind
  EXPECT_EQ (1u, s.said.size ());
  r.tick (7100);          // under four to go
  EXPECT_EQ (2u, s.said.size ());
}

TEST (SpeechRouter, HoldingKeepsLinesBackAndThenSaysSo)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.setHold (true);
  r.line ("a", "output");
  r.line ("b", "output");
  r.tick (1000);
  EXPECT_TRUE (s.said.empty ());

  r.setHold (false);
  r.tick (2000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("2 new lines.\na\nb", s.said [0].c_str ());
}

TEST (SpeechRouter, UrgentThingsGetThroughAHold)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.setHold (true);
  r.line ("ordinary", "output");
  r.say ("urgent", "trigger", SpeechPriority::Assertive, false);
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("urgent", s.said [0].c_str ());
  EXPECT_EQ (1u, r.pendingCount ());
}

TEST (SpeechRouter, InterruptDropsWhatWasWaiting)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("old", "output");
  r.say ("now", "script", SpeechPriority::Normal, true);
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("now", s.said [0].c_str ());
  EXPECT_EQ (1, s.stops);
}

// An alert queued a moment before Enter, or before a script's interrupt, is still news.
TEST (SpeechRouter, EnterAndInterruptKeepWhatIsUrgent)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("old", "output");
  r.say ("you are being attacked", "trigger", SpeechPriority::Assertive, false);
  r.commandEntered ();
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("you are being attacked", s.said [0].c_str ());
  EXPECT_EQ (1, s.stops);
  EXPECT_EQ ((uint64_t) 1, r.linesSkipped ());

  r.line ("more old", "output");
  r.say ("low health", "trigger", SpeechPriority::Assertive, false);
  r.say ("now", "script", SpeechPriority::Normal, true);
  r.tick (100000);
  ASSERT_EQ (2u, s.said.size ());
  EXPECT_STREQ ("now\nlow health", s.said [1].c_str ());    // the interrupt's own goes first

  // whereas stopping is stopping
  r.say ("urgent", "trigger", SpeechPriority::Assertive, false);
  r.stop ();
  r.tick (200000);
  EXPECT_EQ (2u, s.said.size ());
}

// A busy category of priority 2 can build a backlog that nothing short of stopping would
// clear.  Enter keeps only the few newest that came in just now, and an interrupt the same.
TEST (SpeechRouter, EnterKeepsOnlyAFewUrgentLinesThatJustArrived)
{
  SpeechRouter r;
  FakeSpeech s;
  s.caps |= pal::kSpeechKnowsBusy;
  s.busy = true;
  r.setBackend (&s);
  r.tick (1000);
  r.say ("an old alert", "trigger", SpeechPriority::Assertive, false);
  r.tick (4000);          // busy: nothing said, and that alert is three seconds old
  for (int i = 1; i <= 5; i++)
    r.say ("alert " + std::to_string (i), "trigger", SpeechPriority::Assertive, false);
  r.line ("output", "output");
  r.commandEntered ();
  EXPECT_EQ (3u, r.pendingCount ());
  s.busy = false;
  r.tick (4500);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("alert 3\nalert 4\nalert 5", s.said [0].c_str ());
  EXPECT_EQ ((uint64_t) 4, r.linesSkipped ());

  // An interrupt likewise, and its own is said ahead of them.
  s.busy = true;
  r.tick (5000);
  for (int i = 6; i <= 10; i++)
    r.say ("alert " + std::to_string (i), "trigger", SpeechPriority::Assertive, false);
  r.say ("now", "script", SpeechPriority::Normal, true);
  s.busy = false;
  r.tick (20000);
  ASSERT_EQ (2u, s.said.size ());
  EXPECT_STREQ ("now\nalert 8\nalert 9\nalert 10", s.said [1].c_str ());
}

// ... and a flood of them, past a generous limit, loses the oldest.
TEST (SpeechRouter, UrgentLinesHaveALimitOfTheirOwn)
{
  SpeechRouter r;
  FakeSpeech s;
  s.caps |= pal::kSpeechKnowsBusy;
  s.busy = true;
  r.setBackend (&s);
  r.policy ().maxUrgent = 20;
  for (int i = 1; i <= 30; i++)
    r.say ("alert " + std::to_string (i), "trigger", SpeechPriority::Assertive, false);
  r.tick (1000);
  EXPECT_EQ (20u, r.pendingCount ());
  s.busy = false;
  r.tick (2000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_EQ (0u, s.said [0].find ("10 lines skipped.\nalert 11\n"));
  EXPECT_TRUE (s.said [0].find ("alert 30") != std::string::npos);
  EXPECT_EQ ((uint64_t) 10, r.linesSkipped ());

  // 0 is no limit, as maxPending's is.
  r.policy ().maxUrgent = 0;
  s.busy = true;
  for (int i = 1; i <= 30; i++)
    r.say ("again " + std::to_string (i), "trigger", SpeechPriority::Assertive, false);
  r.tick (2050);          // too soon after the last to be ready
  EXPECT_EQ (30u, r.pendingCount ());
}

TEST (SpeechRouter, SkipToLatestLeavesTheNewest)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.line ("a", "output");
  r.line ("b", "output");
  r.line ("c", "output");
  r.skipToLatest ();
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("c", s.said [0].c_str ());
}

TEST (SpeechRouter, MutedSaysNothingButRemembers)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.setMuted (true);
  r.line ("quiet", "output");
  r.say ("quieter", "script", SpeechPriority::Normal, false);
  r.tick (1000);
  EXPECT_TRUE (s.said.empty ());
  EXPECT_EQ (2u, r.history ().size ());
}

TEST (SpeechRouter, NoBackendThrowsAwayWhatWouldBeStale)
{
  SpeechRouter r;
  r.line ("nobody is listening", "output");
  r.tick (1000);
  FakeSpeech s;
  r.setBackend (&s);
  r.tick (2000);
  EXPECT_TRUE (s.said.empty ());
}

TEST (SpeechRouter, APromptIsNotSaidTwice)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.prompt ("Enter your name:");
  r.prompt ("Enter your name:");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("Enter your name:", s.said [0].c_str ());

  // and when the rest of the line comes, only the rest
  r.line ("Enter your name: Bob", "output");
  r.tick (9000);
  ASSERT_EQ (2u, s.said.size ());
  EXPECT_STREQ (" Bob", s.said [1].c_str ());
  EXPECT_STREQ ("Enter your name: Bob", r.history ().back ().text.c_str ());
}

TEST (SpeechRouter, CategoriesCanBeOffAndAreOnByDefault)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.category ("spam").enabled = false;
  r.line ("no", "spam");
  r.line ("typed", a11y::kCategoryInput);      // off, as they just typed it
  r.line ("yes", "tells");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("yes", s.said [0].c_str ());
  EXPECT_EQ (3u, r.history ().size ());
}

TEST (SpeechRouter, ACategoryWithACuePlaysIt)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  std::vector<std::string> cues;
  r.onCue = [&] (const std::string & name, const a11y::CategorySettings & settings)
    {
    cues.push_back (name + ":" + settings.cue + ":" + std::to_string ((int) settings.cuePan));
    };
  r.category ("tells").cue = "ding.wav";
  r.category ("tells").cuePan = -50;
  r.line ("hi", "tells");
  r.line ("no cue", "output");
  ASSERT_EQ (1u, cues.size ());
  EXPECT_STREQ ("tells:ding.wav:-50", cues [0].c_str ());
}

TEST (SpeechRouter, CategoriesSurviveAWorldFile)
{
  std::map<std::string, a11y::CategorySettings> before;
  before ["tells"].cue = "a.wav";
  before ["tells"].cueVolume = -10;
  before ["tells"].cuePan = 30;
  before ["tells"].priority = SpeechPriority::Assertive;
  before ["spam"].enabled = false;
  const auto after = a11y::CategoriesFromText (a11y::CategoriesToText (before));
  ASSERT_EQ (2u, after.size ());
  EXPECT_STREQ ("a.wav", after.at ("tells").cue.c_str ());
  EXPECT_EQ (-10, (int) after.at ("tells").cueVolume);
  EXPECT_EQ (30, (int) after.at ("tells").cuePan);
  EXPECT_EQ (SpeechPriority::Assertive, after.at ("tells").priority);
  EXPECT_FALSE (after.at ("spam").enabled);
  EXPECT_TRUE (after.at ("tells").enabled);
}

TEST (SpeechSubstitutions, WholeLineReplacedOrSkipped)
{
  SpeechRouter r;
  FakeSpeech s;
  r.setBackend (&s);
  r.substitutions ().entries ["You hit it"] = "hit";
  r.substitutions ().entries ["Spam"] = "!skip";
  r.line ("You hit it", "output");
  r.line ("Spam", "output");
  r.line ("You hit it hard", "output");
  r.tick (1000);
  ASSERT_EQ (1u, s.said.size ());
  EXPECT_STREQ ("hit\nYou hit it hard", s.said [0].c_str ());
  EXPECT_STREQ ("You hit it", r.history ().front ().text.c_str ());   // review sees the real thing
}

TEST (SpeechSubstitutions, OffMeansOff)
{
  a11y::Substitutions subs;
  subs.entries ["a"] = "b";
  subs.enabled = false;
  std::string spoken;
  EXPECT_TRUE (subs.apply ("a", spoken));
  EXPECT_STREQ ("a", spoken.c_str ());
}

TEST (SpeechSubstitutions, TheFileRoundTrips)
{
  a11y::Substitutions before;
  before.enabled = false;
  before.entries ["He said \"hi\""] = "hi\\there";
  before.entries ["Two\nlines"] = "!skip";

  a11y::Substitutions after;
  after.fromFile (before.toFile ());
  EXPECT_FALSE (after.enabled);
  ASSERT_EQ (2u, after.entries.size ());
  EXPECT_STREQ ("hi\\there", after.entries ["He said \"hi\""].c_str ());
  EXPECT_STREQ ("!skip", after.entries ["Two\nlines"].c_str ());
}

TEST (SpeechSubstitutions, ReadsWhatMushReaderWrote)
{
  a11y::Substitutions subs;
  subs.fromFile ("return {\n  [\"x\"] = \"y\",\n  status = 1,\n  [ \"p q\" ] = \"!skip\",\n}\n");
  EXPECT_TRUE (subs.enabled);
  EXPECT_STREQ ("y", subs.entries ["x"].c_str ());
  EXPECT_STREQ ("!skip", subs.entries ["p q"].c_str ());
}

// ---------------------------------------------------------------- review

TEST (SpeechReview, MovesByLineAndNewLinesDoNotMoveIt)
{
  SpeechRouter r;
  r.line ("one", "output");
  r.line ("two", "output");
  r.line ("three", "output");
  a11y::ReviewCursor & c = r.review ();
  EXPECT_FALSE (c.reviewing ());

  EXPECT_STREQ ("three", c.previousLine ().c_str ());   // from live: the newest
  EXPECT_TRUE (c.reviewing ());
  EXPECT_STREQ ("two", c.previousLine ().c_str ());

  r.line ("four", "output");
  EXPECT_STREQ ("two", c.currentLine ().c_str ());
  EXPECT_STREQ ("three", c.nextLine ().c_str ());
  EXPECT_STREQ ("four", c.nextLine ().c_str ());
  EXPECT_STREQ ("Bottom. four", c.nextLine ().c_str ());
}

TEST (SpeechReview, EdgesSaySo)
{
  SpeechRouter r;
  EXPECT_STREQ ("No lines.", r.review ().previousLine ().c_str ());
  r.line ("only", "output");
  EXPECT_STREQ ("only", r.review ().previousLine ().c_str ());
  EXPECT_STREQ ("Top. only", r.review ().previousLine ().c_str ());
  EXPECT_STREQ ("only", r.review ().lastLine ().c_str ());
  EXPECT_FALSE (r.review ().reviewing ());
}

TEST (SpeechReview, TrimmingOldLinesDoesNotShiftTheCursor)
{
  SpeechRouter r;
  r.setHistoryLimit (5);
  for (int i = 1; i <= 5; i++)
    r.line ("line " + std::to_string (i), "output");
  a11y::ReviewCursor & c = r.review ();
  c.firstLine ();
  c.nextLine ();
  c.nextLine ();
  EXPECT_STREQ ("line 3", c.currentLine ().c_str ());

  r.line ("line 6", "output");        // line 1 falls off
  r.line ("line 7", "output");        // line 2 falls off
  EXPECT_STREQ ("line 3", c.currentLine ().c_str ());

  r.line ("line 8", "output");        // ... and the line it was on: go to the oldest there is
  EXPECT_STREQ ("line 4", c.currentLine ().c_str ());
}

TEST (SpeechReview, Words)
{
  SpeechRouter r;
  r.line ("The quick, brown fox", "output");
  a11y::ReviewCursor & c = r.review ();
  c.previousLine ();
  EXPECT_STREQ ("The", c.currentWord ().c_str ());
  EXPECT_STREQ ("quick", c.nextWord ().c_str ());
  EXPECT_STREQ ("brown", c.nextWord ().c_str ());
  EXPECT_STREQ ("fox", c.nextWord ().c_str ());
  EXPECT_STREQ ("End of line.", c.nextWord ().c_str ());
  EXPECT_STREQ ("brown", c.previousWord ().c_str ());
  EXPECT_STREQ ("quick", c.previousWord ().c_str ());
  EXPECT_STREQ ("The", c.previousWord ().c_str ());
  EXPECT_STREQ ("Start of line.", c.previousWord ().c_str ());
}

TEST (SpeechReview, CharactersAndSpellingKnowUtf8)
{
  SpeechRouter r;
  r.line ("A \xC3\xA9\xE2\x82\xAC", "output");   // A, space, e-acute, euro
  a11y::ReviewCursor & c = r.review ();
  c.previousLine ();
  // the first character is where it starts
  EXPECT_STREQ ("space", c.nextChar ().c_str ());
  EXPECT_STREQ ("\xC3\xA9", c.nextChar ().c_str ());
  EXPECT_STREQ ("\xE2\x82\xAC", c.nextChar ().c_str ());
  EXPECT_STREQ ("End of line.", c.nextChar ().c_str ());
  EXPECT_STREQ ("\xC3\xA9", c.previousChar ().c_str ());
  EXPECT_STREQ ("space", c.previousChar ().c_str ());
  EXPECT_STREQ ("cap A", c.previousChar ().c_str ());
  EXPECT_STREQ ("Start of line.", c.previousChar ().c_str ());
}

TEST (SpeechReview, Spell)
{
  SpeechRouter r;
  r.line ("Hi", "output");
  r.review ().previousLine ();
  EXPECT_STREQ ("cap H, i", r.review ().spellWord ().c_str ());
}

TEST (SpeechReview, RecentLinesCountBack)
{
  SpeechRouter r;
  for (int i = 1; i <= 12; i++)
    r.line ("line " + std::to_string (i), "output");
  EXPECT_STREQ ("line 12", r.review ().recentLine (1).c_str ());
  EXPECT_STREQ ("line 3", r.review ().recentLine (10).c_str ());
  EXPECT_STREQ ("No such line.", r.review ().recentLine (13).c_str ());
}

TEST (SpeechReview, ACategoryIsItsOwnHistory)
{
  SpeechRouter r;
  r.line ("a", "output");
  r.line ("psst", "tells");
  r.line ("b", "output");
  r.line ("hey", "tells");
  a11y::ReviewCursor & c = r.review ();
  c.setCategory ("tells");
  EXPECT_EQ (2u, c.lineCount ());
  EXPECT_STREQ ("hey", c.previousLine ().c_str ());
  EXPECT_STREQ ("psst", c.previousLine ().c_str ());
  EXPECT_STREQ ("Top. psst", c.previousLine ().c_str ());
  c.setCategory ("");
  EXPECT_EQ (4u, c.lineCount ());
}

// ---------------------------------------------------------------- the document

namespace {

struct SpeechWorld
  {
  CMUSHclientDoc doc;
  FakePlatform platform;
  long long now = 100000;

  explicit SpeechWorld (bool bReader = true)
    {
    A11y_ResetForTest ();
    App.m_iSpeechMode = eSpeechAuto;
    App.m_iA11yTextPane = eTextPaneNever;
    pal::g_speechPlatform = &platform;
    platform.bReader = bReader;
    doc.m_bSpeechBackground = true;    // there are no views here to say it is the active world
    tick ();
    }

  ~SpeechWorld ()
    {
    pal::g_speechPlatform = nullptr;
    A11y_ResetForTest ();
    App.m_iSpeechMode = eSpeechAuto;
    App.m_iA11yTextPane = eTextPaneWithReader;
    }

  void tick () { now += 5000; A11y_TickAt (doc, now); }
  void feed (const char * s) { doc.DisplayMsg (s, (int) strlen (s), 0); }
  std::string said () { return Joined (platform.reader.said); }
  };

}  // namespace

TEST (SpeechDoc, NobodyWithoutAReaderHearsOrAllocatesAnything)
{
  SpeechWorld w (false);
  w.feed ("Hello there\n");
  w.feed ("Enter your name: ");
  w.tick ();
  w.tick ();
  EXPECT_TRUE (w.doc.m_pA11y == nullptr);
  EXPECT_EQ (0, w.platform.ownVoiceAsked);
  EXPECT_TRUE (w.platform.voice.said.empty ());
  EXPECT_FALSE (A11y_AutoReading ());
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::F12, 0, true, false, true));
  EXPECT_FALSE (A11y_Command (w.doc, "tts"));
  EXPECT_FALSE (A11y_Command (w.doc, "subst list"));
  EXPECT_TRUE (w.doc.m_pA11y == nullptr);
}

TEST (SpeechDoc, WithAReaderItJustWorks)
{
  SpeechWorld w;
  EXPECT_TRUE (A11y_AutoReading ());
  w.feed ("Hello there\n");
  w.tick ();
  EXPECT_STREQ ("Hello there|", w.said ().c_str ());
  EXPECT_EQ (0, w.platform.ownVoiceAsked);
}

TEST (SpeechDoc, SpeechModeOffMeansOff)
{
  SpeechWorld w;
  App.m_iSpeechMode = eSpeechOff;
  w.tick ();
  w.feed ("Hello\n");
  w.doc.Speak ("explicit", "", -1, FALSE);
  w.tick ();
  EXPECT_TRUE (w.platform.reader.said.empty ());
  EXPECT_TRUE (w.platform.voice.said.empty ());
}

TEST (SpeechDoc, AlwaysMeansTheSystemVoice)
{
  SpeechWorld w (false);
  App.m_iSpeechMode = eSpeechAlways;
  w.tick ();
  w.feed ("Hello\n");
  w.tick ();
  EXPECT_STREQ ("Hello|", Joined (w.platform.voice.said).c_str ());
  EXPECT_EQ (0, w.platform.voice.rate);     // the preference was applied
}

TEST (SpeechDoc, AReaderTakesOverFromTheVoice)
{
  SpeechWorld w (false);
  App.m_iSpeechMode = eSpeechAlways;
  w.tick ();
  w.platform.bReader = true;
  w.tick ();
  w.feed ("Hello\n");
  w.tick ();
  EXPECT_STREQ ("Hello|", w.said ().c_str ());
  EXPECT_TRUE (w.platform.voice.said.empty ());
}

TEST (SpeechDoc, NotesAreSpokenAndTheirOwnCategory)
{
  SpeechWorld w;
  w.doc.Note ("a note");
  w.tick ();
  EXPECT_STREQ ("a note|", w.said ().c_str ());
  EXPECT_STREQ (a11y::kCategoryNote, w.doc.m_pA11y->router.history ().back ().category.c_str ());
}

TEST (SpeechDoc, OmittedFromOutputIsOmittedFromSpeech)
{
  SpeechWorld w;
  w.doc.AddTrigger ("t1", "secret*", "", eEnabled | eOmitFromOutput, -1, 0, "", "");
  w.feed ("secret stuff\n");
  w.feed ("public stuff\n");
  w.tick ();
  EXPECT_STREQ ("public stuff|", w.said ().c_str ());
}

TEST (SpeechDoc, TheBackgroundWorldIsQuietUnlessAsked)
{
  SpeechWorld w;
  w.doc.m_bSpeechBackground = false;
  w.feed ("background\n");
  w.tick ();
  EXPECT_TRUE (w.platform.reader.said.empty ());
  EXPECT_EQ (1u, w.doc.m_pA11y->router.history ().size ());

  // but what someone asked to have said, still is
  w.doc.Speak ("from a script", "", -1, FALSE);
  w.tick ();
  EXPECT_STREQ ("from a script|", w.said ().c_str ());
}

TEST (SpeechDoc, ScrollbackThatWasAlreadyThereIsReviewedAndNotSpoken)
{
  // Lines in the line list that did not arrive now, which is what restored scrollback is
  SpeechWorld w (false);
  w.feed ("old one\n");
  w.feed ("old two\n");

  w.platform.bReader = true;
  w.tick ();
  w.feed ("new\n");
  w.tick ();
  EXPECT_STREQ ("new|", w.said ().c_str ());

  a11y::ReviewCursor & c = w.doc.m_pA11y->router.review ();
  EXPECT_STREQ ("new", c.previousLine ().c_str ());
  EXPECT_STREQ ("old two", c.previousLine ().c_str ());
  EXPECT_STREQ ("old one", c.previousLine ().c_str ());
}

TEST (SpeechDoc, EnterSilences)
{
  SpeechWorld w;
  w.doc.m_pA11y->router.line ("x", "output");    // make sure the state exists
  w.feed ("a long room description\n");
  A11y_CommandEntered (w.doc);
  w.tick ();
  EXPECT_TRUE (w.platform.reader.said.empty ());
  EXPECT_EQ (1, w.platform.reader.stops);

  w.doc.m_bSpeechInterruptOnEnter = false;
  w.feed ("this one stays\n");
  A11y_CommandEntered (w.doc);
  w.tick ();
  EXPECT_STREQ ("this one stays|", w.said ().c_str ());
}

TEST (SpeechDoc, APromptWithNoNewlineIsSpokenOnceItGoesQuiet)
{
  SpeechWorld w;
  w.feed ("Enter your name: ");
  A11y_TickAt (w.doc, w.now + 10);
  A11y_TickAt (w.doc, w.now + 20);
  EXPECT_TRUE (w.platform.reader.said.empty ());
  A11y_TickAt (w.doc, w.now + 500);
  EXPECT_STREQ ("Enter your name: |", w.said ().c_str ());
}

TEST (SpeechDoc, WorldSpeak)
{
  SpeechWorld w;
  EXPECT_EQ ((long) eOK, w.doc.Speak ("hello", "tells", 2, FALSE));
  EXPECT_EQ ((long) eBadParameter, w.doc.Speak ("hello", "tells", 3, FALSE));
  w.tick ();
  EXPECT_STREQ ("hello|", w.said ().c_str ());
  EXPECT_EQ (SpeechPriority::Assertive, w.platform.reader.priorities [0]);
  EXPECT_EQ ((long) eOK, w.doc.StopSpeech ());
  EXPECT_EQ (1, w.platform.reader.stops);
}

TEST (SpeechDoc, AScriptAskingWithNoReaderGetsTheSystemVoice)
{
  SpeechWorld w (false);
  w.doc.Speak ("hello", "", -1, FALSE);
  w.now += 5000;
  A11y_TickAt (w.doc, w.now);
  EXPECT_STREQ ("hello|", Joined (w.platform.voice.said).c_str ());
}

TEST (SpeechDoc, SendToSpeech)
{
  SpeechWorld w;
  CString strExtra;
  w.doc.SendTo (eSendToSpeech, "from a trigger", false, false, "d", "", strExtra);
  w.tick ();
  EXPECT_STREQ ("from a trigger|", w.said ().c_str ());
  EXPECT_TRUE (strExtra.IsEmpty ());
}

TEST (SpeechDoc, TriggerSendToSpeechSurvivesAWorldFileRow)
{
  EXPECT_EQ (15, (int) eSendToSpeech);
  EXPECT_EQ (16, (int) eSendToLast);
  EXPECT_STREQ ("speech", (LPCTSTR) GetSendToString (eSendToSpeech));
}

TEST (SpeechDoc, SpeechInfo)
{
  SpeechWorld w;
  w.doc.Speak ("hello", "", -1, FALSE);
  w.tick ();
  VARIANT v = w.doc.SpeechInfo (1);
  EXPECT_EQ (VT_BSTR, (int) v.vt);
  VariantClear (&v);
  v = w.doc.SpeechInfo (9);
  EXPECT_EQ (1, (int) v.lVal);
  v = w.doc.SpeechInfo (3);
  EXPECT_TRUE (v.boolVal != 0);
  v = w.doc.SpeechInfo (999);
  EXPECT_EQ (VT_NULL, (int) v.vt);
}

TEST (SpeechDoc, CategoriesPersistInTheWorldOption)
{
  SpeechWorld w;
  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("tells", "cue", "ding.wav"));
  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("tells", "priority", "2"));
  EXPECT_EQ ((long) eOptionOutOfRange, w.doc.SpeechCategory ("tells", "priority", "9"));
  EXPECT_EQ ((long) eUnknownOption, w.doc.SpeechCategory ("tells", "nonsense", "1"));
  EXPECT_EQ ((long) eBadParameter, w.doc.SpeechCategory ("te;lls", "cue", "x"));
  EXPECT_TRUE (w.doc.m_strSpeechCategories.Find ("tells;on=1;prio=2;cue=ding.wav") >= 0);

  // and a world loaded with them has them
  SpeechWorld other;
  other.doc.m_strSpeechCategories = "spam;on=0;prio=1\n";
  other.tick ();
  other.feed ("output\n");
  other.tick ();
  EXPECT_FALSE (other.doc.m_pA11y->router.category ("spam").enabled);
}

TEST (SpeechDoc, MuteOptionAndKey)
{
  SpeechWorld w;
  w.doc.SetOption ("speech_mute", 1);
  w.tick ();
  w.feed ("quiet\n");
  w.tick ();
  EXPECT_TRUE (w.platform.reader.said.empty ());

  // Ctrl+Shift+F12 turns it back on, and says so
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::F12, 0, true, false, true));
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
  EXPECT_STREQ ("speech on|", w.said ().c_str ());

  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Character, 'M', false, true, true));
  EXPECT_EQ (1, (int) w.doc.m_bSpeechMute);
}

TEST (SpeechDoc, ReviewKeys)
{
  SpeechWorld w;
  w.feed ("one\n");
  w.feed ("two words\n");
  w.tick ();
  w.platform.reader.said.clear ();

  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Down, 0, false, true, true));
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Right, 0, false, true, true));
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Character, 'S', false, true, true));
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Character, '2', false, true, true));
  EXPECT_STREQ ("two words|one|two words|words|w, o, r, d, s|one|", w.said ().c_str ());
  EXPECT_EQ (6, w.platform.reader.flushes);

  // Reviewing holds new lines back, and End lets them go
  w.tick ();
  w.feed ("while reading\n");
  w.tick ();
  EXPECT_EQ (6u, w.platform.reader.said.size ());
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::End, 0, false, true, true));
  w.tick ();
  EXPECT_EQ (8u, w.platform.reader.said.size ());
  EXPECT_STREQ ("1 new line.\nwhile reading", w.platform.reader.said.back ().c_str ());

  // Without Alt+Shift they are the command box's as they always were
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Up, 0, false, false, false));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, false));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Character, 'A', false, false, false));
}

TEST (SpeechDoc, Commands)
{
  SpeechWorld w;
  EXPECT_TRUE (A11y_Command (w.doc, "tts_note hello"));
  w.tick ();
  EXPECT_STREQ ("hello|", w.said ().c_str ());
  EXPECT_FALSE (A11y_Command (w.doc, "tts please"));    // not ours: for the MUD
  EXPECT_FALSE (A11y_Command (w.doc, "look"));
  EXPECT_TRUE (A11y_Command (w.doc, "tts"));
  EXPECT_EQ (1, (int) w.doc.m_bSpeechMute);
  EXPECT_TRUE (A11y_Command (w.doc, "TTS"));
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
}

// ---------------------------------------------------------------- shims

TEST (SpeechShim, ShadowsByTheIdOfThePluginItself)
{
  std::string out;
  EXPECT_TRUE (A11y_ShadowPlugin ("<muclient><plugin name=\"MushReader\" id=\"925cdd0331023d9f0b8f05a7\"></plugin></muclient>", out));
  EXPECT_TRUE (out.find ("function interrupt") != std::string::npos);

  out.clear ();
  EXPECT_TRUE (A11y_ShadowPlugin ("<plugin\n  name='LuaAudio'\n  id = 'aedf0cb0be5bf045860d54b7'\n>", out));
  EXPECT_TRUE (out.find ("function playlooped") != std::string::npos);

  // Another plugin which merely talks to it stays what it is
  out.clear ();
  EXPECT_FALSE (A11y_ShadowPlugin ("<plugin name=\"Mine\" id=\"123456781234567812345678\">"
                                   "<script>CallPlugin (\"925cdd0331023d9f0b8f05a7\", \"say\", \"x\")</script>", out));
  EXPECT_FALSE (A11y_ShadowPlugin ("<world name=\"925cdd0331023d9f0b8f05a7\"/>", out));
  EXPECT_FALSE (A11y_ShadowPlugin ("<plugin name=\"925cdd0331023d9f0b8f05a7\" id=\"123456781234567812345678\">", out));
  EXPECT_FALSE (A11y_ShadowPlugin ("<plugin name=\"Mine\" valid=\"925cdd0331023d9f0b8f05a7\" id=\"123456781234567812345678\">", out));
}

TEST (SpeechShim, SayThroughARealPluginLoad)
{
  namespace fs = std::filesystem;
  SpeechWorld w;
  const fs::path file = fs::temp_directory_path () / "mc_test_MushReader.xml";
  {
  // Only the ID matters: this is what the 2014 plugin's tag has in it, and the DLL it wants is not there
  std::ofstream f (file.string ());
  f << "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
       "<plugin name=\"MushReader\" id=\"925cdd0331023d9f0b8f05a7\" language=\"Lua\" requires=\"4.0\">\n"
       "</plugin>\n<script>error (\"the original must not run\")</script>\n</muclient>\n";
  }
  const long result = w.doc.LoadPlugin (file.string ().c_str ());
  EXPECT_EQ ((long) eOK, result);
  EXPECT_TRUE (w.doc.IsPluginInstalled (kA11yMushReaderID) != FALSE);
  EXPECT_TRUE (w.doc.PluginSupports (kA11yMushReaderID, "say") == eOK);
  EXPECT_TRUE (w.doc.PluginSupports (kA11yMushReaderID, "interrupt") == eOK);

  EXPECT_EQ ((long) eOK, w.doc.CallPlugin (kA11yMushReaderID, "say", "from a soundpack"));
  w.tick ();
  EXPECT_TRUE (w.said ().find ("from a soundpack") != std::string::npos);

  w.doc.UnloadPlugin (kA11yMushReaderID);
  std::error_code ec;
  fs::remove (file, ec);
}

TEST (SpeechShim, LuaAudioLoadsAndAnswers)
{
  namespace fs = std::filesystem;
  SpeechWorld w;
  const fs::path file = fs::temp_directory_path () / "mc_test_LuaAudio.xml";
  {
  std::ofstream f (file.string ());
  f << "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
       "<plugin name=\"LuaAudio\" id=\"aedf0cb0be5bf045860d54b7\" language=\"Lua\" requires=\"4.0\">\n"
       "</plugin>\n</muclient>\n";
  }
  EXPECT_EQ ((long) eOK, w.doc.LoadPlugin (file.string ().c_str ()));
  EXPECT_TRUE (w.doc.PluginSupports (kA11yLuaAudioID, "play") == eOK);
  EXPECT_TRUE (w.doc.PluginSupports (kA11yLuaAudioID, "stop") == eOK);

  EXPECT_TRUE (w.doc.PluginSupports (kA11yLuaAudioID, "setvol") == eOK);
  EXPECT_EQ ((long) eOK, w.doc.CallPlugin (kA11yLuaAudioID, "stop", "0"));
  w.doc.UnloadPlugin (kA11yLuaAudioID);
  std::error_code ec;
  fs::remove (file, ec);
}

// ---------------------------------------------------------------- cues and hotspots

namespace {
struct FakeSound : pal::ISound
  {
  std::vector<std::string> played;
  bool playCue (const char * file, double volume, double pan) override
    {
    played.push_back (std::string (file) + ":" + std::to_string ((int) volume) + ":" + std::to_string ((int) pan));
    return true;
    }
  };
}  // namespace

TEST (SpeechDoc, ACueGoesThroughTheSoundSeamAtItsVolumeAndPan)
{
  SpeechWorld w;
  FakeSound sound;
  w.doc.m_pA11y->pSound = &sound;
  w.doc.SpeechCategory ("tells", "cue", "ding.wav");
  w.doc.SpeechCategory ("tells", "cue_volume", "-20");
  w.doc.SpeechCategory ("tells", "cue_pan", "50");
  w.doc.Speak ("hi", "tells", -1, FALSE);
  w.doc.Speak ("no cue", "other", -1, FALSE);
  ASSERT_EQ (1u, sound.played.size ());
  EXPECT_STREQ ("ding.wav:-20:50", sound.played [0].c_str ());
}

TEST (SpeechDoc, SpeechReviewNamesTheSameActionsAsTheKeys)
{
  SpeechWorld w;
  w.feed ("a line\n");
  w.tick ();
  w.platform.reader.said.clear ();
  EXPECT_EQ ((long) eOK, w.doc.SpeechReview ("previous_line"));
  EXPECT_EQ ((long) eOK, w.doc.SpeechReview ("recent_1"));
  EXPECT_EQ ((long) eBadParameter, w.doc.SpeechReview ("nonsense"));
  EXPECT_STREQ ("a line|a line|", w.said ().c_str ());
}

TEST (SpeechDoc, ThereAreNoHotspotsUntilThereAre)
{
  SpeechWorld w;
  EXPECT_TRUE (A11y_ListHotspots (w.doc).empty ());
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Character, 'H', false, true, true));
  EXPECT_STREQ ("No hotspots.|", w.said ().c_str ());
  EXPECT_EQ ((long) eNoSuchWindow, w.doc.ActivateHotspot ("nope", "nope"));
  EXPECT_EQ ((long) eNoSuchWindow, w.doc.WindowHotspotLabel ("nope", "nope", "x"));
}

// ---------------------------------------------------------------- who the keys and words belong to

namespace {

// The engine's side of a text pane, recorded.
struct PaneView : FakeWorldView
  {
  bool shown = false;
  bool focused = false;
  std::vector<std::string> paneLines;
  std::string reset;
  void accessiblePaneShow (bool s) override { shown = s; }
  void accessibleLine (const char * utf8) override { paneLines.push_back (utf8); }
  void accessibleReset (const char * utf8) override { reset = utf8; paneLines.clear (); }
  bool accessiblePaneFocused () const override { return focused; }
  void accessibleFocusPane (bool pane) override { focused = pane; }
  };

// Puts a PaneView in the document's place for as long as it lives.
struct WithPaneView
  {
  CMUSHclientDoc & doc;
  pal::IWorldView * saved;
  PaneView view;
  explicit WithPaneView (CMUSHclientDoc & d) : doc (d), saved (d.m_pWorldView) { doc.m_pWorldView = &view; }
  ~WithPaneView () { doc.m_pWorldView = saved; }
  };

}  // namespace

// A text pane asked for always gets the output with no reader at all.  It used to be shown
// and then never given a line.
TEST (SpeechDoc, ThePaneSaidAlwaysGetsLinesWithNoReader)
{
  SpeechWorld w (false);
  WithPaneView pane (w.doc);
  App.m_iA11yTextPane = eTextPaneAlways;
  w.tick ();
  EXPECT_TRUE (pane.view.shown);
  w.feed ("one\n");
  w.feed ("two\n");
  w.tick ();
  EXPECT_STREQ ("one|two|", Joined (pane.view.paneLines).c_str ());
  EXPECT_TRUE (w.platform.voice.said.empty ());
  EXPECT_EQ (0, w.platform.ownVoiceAsked);

  // F6 goes there, as the docs say; the review keys and the words do not wake up for it
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::F6, 0, false, false, false));
  EXPECT_TRUE (pane.view.focused);
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::F12, 0, true, false, true));
  EXPECT_FALSE (A11y_Command (w.doc, "tts"));
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
}

// A script that speaks makes the state exist, and that is not the player using speech: Alt+Shift
// and Ctrl+Shift+F12 and the word tts are still theirs.
TEST (SpeechDoc, AScriptSpeakingDoesNotTakeKeysOrWords)
{
  SpeechWorld w (false);
  w.doc.Speak ("hello", "", -1, FALSE);
  w.doc.SpeechCategory ("tells", "cue", "ding.wav");
  ASSERT_TRUE (w.doc.m_pA11y != nullptr);
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Character, 'L', false, true, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::F12, 0, true, false, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::F6, 0, false, false, false));
  EXPECT_FALSE (A11y_Command (w.doc, "tts"));
  EXPECT_FALSE (A11y_Command (w.doc, "subst list"));
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);

  // Reading everything, with the system voice, is using speech
  App.m_iSpeechMode = eSpeechAlways;
  w.tick ();
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::F12, 0, true, false, true));
  EXPECT_EQ (1, (int) w.doc.m_bSpeechMute);
  EXPECT_TRUE (A11y_Command (w.doc, "tts"));
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
}

TEST (SpeechDoc, AnAcceleratorOnTheKeyIsTheWorlds)
{
  SpeechWorld w;
  w.feed ("a line\n");
  w.tick ();
  // As AcceleratorTo files them, which on Windows needs a frame to put the table in
  const auto bind = [&] (const char * sKey)
    {
    BYTE fVirt;
    WORD key;
    StringToKeyCode (sKey, fVirt, key);
    w.doc.m_AcceleratorToCommandMap [((long) (fVirt | FNOINVERT)) << 16 | key] = 12000;
    };
  bind ("Alt+Shift+Up");
  bind ("Alt+Shift+L");
  bind ("Ctrl+Shift+F12");
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::Character, 'L', false, true, true));
  EXPECT_FALSE (A11y_Key (w.doc, A11yKey::F12, 0, true, false, true));
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);

  // and the ones it left alone are still review's
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Down, 0, false, true, true));
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Character, 'W', false, true, true));

  // Given back, it is review's again
  BYTE fVirt;
  WORD key;
  StringToKeyCode ("Alt+Shift+Up", fVirt, key);
  w.doc.m_AcceleratorToCommandMap.erase (((long) (fVirt | FNOINVERT)) << 16 | key);
  EXPECT_TRUE (A11y_Key (w.doc, A11yKey::Up, 0, false, true, true));
}

TEST (SpeechDoc, AnAliasForTheWordIsTheWorlds)
{
  SpeechWorld w;
  w.doc.m_enable_aliases = TRUE;
  w.doc.m_display_my_input = FALSE;
  w.doc.m_iConnectPhase = eConnectConnectedToMud;
  w.doc.m_pTransport = new RecordingTransport ();    // the document's to delete
  RecordingTransport * pTransport = static_cast<RecordingTransport *> (w.doc.m_pTransport);
  bool bOmit = false;

  // Nobody else's: the client's
  w.doc.EvaluateCommand ("tts", true, bOmit, false);
  EXPECT_EQ (1, (int) w.doc.m_bSpeechMute);
  EXPECT_TRUE (pTransport->sent.empty ());
  w.doc.EvaluateCommand ("tts", true, bOmit, false);
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);

  // The world's own alias for it runs instead
  EXPECT_EQ ((long) eOK, w.doc.AddAlias ("mine", "tts", "my own tts", eEnabled, ""));
  w.doc.EvaluateCommand ("tts", true, bOmit, false);
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
  EXPECT_STREQ ("my own tts\r\n", pTransport->sent.c_str ());

  // and a word no alias has is still the client's
  w.doc.EvaluateCommand ("tts_stop", true, bOmit, false);
  EXPECT_STREQ ("my own tts\r\n", pTransport->sent.c_str ());
}

// An alias that takes every command (to log it, or to rewrite it) is not for tts_note in
// particular: the client's own commands still work, a soundpack's Execute of them included.
TEST (SpeechDoc, ACatchAllAliasDoesNotTakeTheCommands)
{
  SpeechWorld w;
  w.doc.m_enable_aliases = TRUE;
  w.doc.m_display_my_input = FALSE;
  w.doc.m_iConnectPhase = eConnectConnectedToMud;
  w.doc.m_pTransport = new RecordingTransport ();    // the document's to delete
  RecordingTransport * pTransport = static_cast<RecordingTransport *> (w.doc.m_pTransport);
  bool bOmit = false;

  EXPECT_EQ ((long) eOK, w.doc.AddAlias ("everything", "*", "%1", eEnabled, ""));
  EXPECT_EQ ((long) eOK, w.doc.AddAlias ("regexp", "^(.*)$", "%1", eEnabled | eAliasRegularExpression, ""));
  w.doc.EvaluateCommand ("tts_note hello there", true, bOmit, false);
  w.tick ();
  EXPECT_TRUE (w.said ().find ("hello there") != std::string::npos);
  w.doc.EvaluateCommand ("tts", true, bOmit, false);
  EXPECT_EQ (1, (int) w.doc.m_bSpeechMute);
  w.doc.EvaluateCommand ("TTS", true, bOmit, false);
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
  EXPECT_TRUE (pTransport->sent.empty ());

  // Anything else still goes through them.
  w.doc.EvaluateCommand ("north", true, bOmit, false);
  EXPECT_STREQ ("north\r\n", pTransport->sent.c_str ());

  // An alias for the word, a regular expression with its ^ and in another case too, wins.
  // (Over the client's command, that is: among aliases, the usual order, so without these.)
  EXPECT_EQ ((long) eOK, w.doc.DeleteAlias ("everything"));
  EXPECT_EQ ((long) eOK, w.doc.DeleteAlias ("regexp"));
  EXPECT_EQ ((long) eOK, w.doc.AddAlias ("mine", "^TTS_NOTE (.*)$", "said %1", eEnabled | eAliasRegularExpression | eIgnoreAliasCase, ""));
  w.doc.EvaluateCommand ("tts_note hi", true, bOmit, false);
  EXPECT_STREQ ("north\r\nsaid hi\r\n", pTransport->sent.c_str ());
  // ... but not for tts, which it only starts like
  w.doc.EvaluateCommand ("tts", true, bOmit, false);
  EXPECT_EQ (1, (int) w.doc.m_bSpeechMute);
  EXPECT_STREQ ("north\r\nsaid hi\r\n", pTransport->sent.c_str ());
  w.doc.EvaluateCommand ("tts", true, bOmit, false);
  EXPECT_EQ (0, (int) w.doc.m_bSpeechMute);
  // and a disabled one is nobody's
  EXPECT_EQ ((long) eOK, w.doc.EnableAlias ("mine", FALSE));
  w.doc.EvaluateCommand ("tts_note bye", true, bOmit, false);
  w.tick ();
  EXPECT_TRUE (w.said ().find ("bye") != std::string::npos);
  EXPECT_STREQ ("north\r\nsaid hi\r\n", pTransport->sent.c_str ());
}

// ---------------------------------------------------------------- smaller things

// A soundpack sets its categories each time it loads: saying the same again is not a change.
TEST (SpeechDoc, ACategorySetAsItWasLeavesTheWorldUnchanged)
{
  SpeechWorld w;
  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("tells", "cue", "ding.wav"));
  EXPECT_TRUE (w.doc.IsModified () != FALSE);
  w.doc.SetModifiedFlag (FALSE);

  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("tells", "cue", "ding.wav"));
  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("tells", "enabled", "1"));
  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("input", "enabled", "0"));
  EXPECT_FALSE (w.doc.IsModified () != FALSE);

  EXPECT_EQ ((long) eOK, w.doc.SpeechCategory ("tells", "cue", "dong.wav"));
  EXPECT_TRUE (w.doc.IsModified () != FALSE);
  EXPECT_TRUE (w.doc.m_strSpeechCategories.Find ("cue=dong.wav") >= 0);
}

// Review and the pane keep 5000 lines, so that is all a reader starting has to take in.
TEST (SpeechDoc, AReaderStartingTakesInOnlyTheNewestLines)
{
  SpeechWorld w (false);
  w.doc.m_maxlines = 0;      // unlimited scrollback
  for (int i = 0; i < 6000; i++)
    w.feed (("line " + std::to_string (i) + "\n").c_str ());

  WithPaneView pane (w.doc);
  App.m_iA11yTextPane = eTextPaneWithReader;
  w.platform.bReader = true;
  w.tick ();
  const auto & history = w.doc.m_pA11y->router.history ();
  ASSERT_EQ (5000u, history.size ());
  EXPECT_STREQ ("line 1000", history.front ().text.c_str ());
  EXPECT_STREQ ("line 5999", history.back ().text.c_str ());
  EXPECT_TRUE (pane.view.reset.compare (0, 10, "line 1000\n") == 0);
  EXPECT_TRUE (pane.view.reset.find ("line 999\n") == std::string::npos);
  EXPECT_TRUE (w.platform.reader.said.empty ());     // none of it is news
}

// A line long enough to wrap, said as a prompt while it paused, is not said again when the
// rest of it and its newline come.
TEST (SpeechDoc, APausedLongLineIsNotSaidTwice)
{
  SpeechWorld w;
  w.doc.m_wrap = TRUE;
  w.doc.m_nWrapColumn = 20;
  // Not the world's first line, which the engine assembles without its first row (see
  // ProcessPreviousLine's backward scan when it reaches the head of the list)
  w.feed ("Hello.\n");
  w.tick ();
  w.platform.reader.said.clear ();

  w.feed ("The quick brown fox jumps over the lazy dog and keeps running ");
  A11y_TickAt (w.doc, w.now + 10);
  A11y_TickAt (w.doc, w.now + 500);
  ASSERT_EQ (1u, w.platform.reader.said.size ());
  EXPECT_STREQ ("The quick brown fox jumps over the lazy dog and keeps running ",
                w.platform.reader.said [0].c_str ());

  w.feed ("away.\n");
  w.now += 5000;
  A11y_TickAt (w.doc, w.now);
  ASSERT_EQ (2u, w.platform.reader.said.size ());
  EXPECT_STREQ ("away.", w.platform.reader.said [1].c_str ());
}

// ---------------------------------------------------------------- LuaAudio's shim, on its own

namespace {

// The shim's script in a bare Lua, with the world's sound functions faked: ten buffers that
// anyone can use.
struct LuaAudioScript
  {
  lua_State * L = luaL_newstate ();

  LuaAudioScript ()
    {
    luaL_openlibs (L);
    std::string xml;
    A11y_ShadowPlugin ("<plugin id=\"aedf0cb0be5bf045860d54b7\">", xml);
    const size_t start = xml.find ("<script><![CDATA[") + strlen ("<script><![CDATA[");
    const size_t end = xml.find ("]]></script>", start);
    ok = Run (
      "buffers = {}\n"
      "function PlaySound (b, file, loop, vol, pan)\n"
      "  if file == '' then local s = buffers [b]; if s then s.vol = vol; s.pan = pan end; return 0 end\n"
      "  buffers [b] = { file = file, status = loop and 2 or 1, vol = vol, pan = pan }\n"
      "  return 0\n"
      "end\n"
      "function GetSoundStatus (b) local s = buffers [b]; return s and s.status or -1 end\n"
      "function StopSound (b) if b == 0 then buffers = {} else buffers [b] = nil end; return 0 end\n"
      "function DoAfterSpecial () end\n"
      "function Note () end\n"
      "function GetInfo () return '' end\n"
      "sendto = { script = 12 }\n") &&
      Run (xml.substr (start, end - start));
    }
  ~LuaAudioScript () { lua_close (L); }

  bool Run (const std::string & code)
    {
    if (luaL_loadstring (L, code.c_str ()) != 0 || lua_pcall (L, 0, 0, 0) != 0)
      {
      fprintf (stderr, "  Lua: %s\n", lua_tostring (L, -1));
      lua_pop (L, 1);
      return false;
      }
    return true;
    }

  bool ok = false;
  };

}  // namespace

// LuaAudio's volume is BASS's, linear, and PlaySound's is dB: half is -6, not -50.
TEST (SpeechShim, LuaAudioVolumeIsLinearAndBecomesDecibels)
{
  LuaAudioScript s;
  ASSERT_TRUE (s.ok);
  EXPECT_TRUE (s.Run ("assert (_luaaudio_db (100) == 0)"));
  EXPECT_TRUE (s.Run ("assert (_luaaudio_db (0) == -100)"));
  EXPECT_TRUE (s.Run ("assert (math.abs (_luaaudio_db (50) + 6.0206) < 0.001)"));
  EXPECT_TRUE (s.Run ("assert (math.abs (_luaaudio_db (10) + 20) < 0.001)"));
  EXPECT_TRUE (s.Run ("assert (_luaaudio_db (150) == 0 and _luaaudio_db (-5) == -100)"));
  // and through play, with the master volume as well
  EXPECT_TRUE (s.Run ("local h = play ('a.wav', false, 0, 50)\n"
                      "assert (h ~= 0)\n"
                      "assert (math.abs (buffers [10].vol + 6.0206) < 0.001, buffers [10].vol)\n"
                      "setvol (50, 0)\n"
                      "assert (math.abs (buffers [10].vol + 12.041) < 0.001, buffers [10].vol)\n"
                      "setvol (0, 0)\n"
                      "assert (buffers [10].vol == -100)"));
}

// Fading everything out stops it, and what plays next is as loud as before the fade.
TEST (SpeechShim, LuaAudioFadingEverythingOutLeavesTheVolumeAsItWas)
{
  LuaAudioScript s;
  ASSERT_TRUE (s.ok);
  EXPECT_TRUE (s.Run ("pending = {}\n"
                      "function DoAfterSpecial (t, code) pending [#pending + 1] = code end\n"
                      "function run () while #pending > 0 do (loadstring or load) (table.remove (pending, 1)) () end end\n"
                      "setvol (50, 0)\n"
                      "play ('loop.wav', true, 0, 100)\n"
                      "fadeout (0, 500)\n"
                      "run ()\n"
                      "assert (next (buffers) == nil, 'still playing')\n"
                      "assert (getvolume (0) == 50, getvolume (0))\n"
                      "play ('b.wav', false, 0, 100)\n"
                      "assert (math.abs (buffers [10].vol + 6.0206) < 0.001, buffers [10].vol)\n"
                      // One sound's fade leaves the master alone, as it always did.
                      "local h = play ('c.wav', true, 0, 100)\n"
                      "fadeout (h, 500)\n"
                      "run ()\n"
                      "assert (getvolume (0) == 50)"));
}

// A handle is a sound of ours: it does not steal a buffer someone else is using, and stopping
// it after it has ended does not stop someone else's sound in the same buffer.
TEST (SpeechShim, LuaAudioHandlesAreItsOwnSounds)
{
  LuaAudioScript s;
  ASSERT_TRUE (s.ok);
  EXPECT_TRUE (s.Run (
    // every buffer but one is someone else's
    "for b = 1, 9 do buffers [b] = { file = 'msp' .. b, status = 1 } end\n"
    "local a = play ('a.wav')\n"
    "assert (a ~= 0 and buffers [10].file == 'a.wav')\n"
    // all ten busy, ours a one-shot: ours gives way, theirs do not
    "local b = play ('b.wav')\n"
    "assert (b ~= 0 and b ~= a and buffers [10].file == 'b.wav')\n"
    "for i = 1, 9 do assert (buffers [i].file == 'msp' .. i) end\n"
    "assert (not isplaying (a) and isplaying (b))\n"
    // a loop of ours does not give way either, so there is no room
    "stop (b)\n"
    "local c = play ('c.wav', true)\n"
    "assert (c ~= 0)\n"
    "assert (play ('d.wav') == 0)\n"
    "for i = 1, 9 do assert (buffers [i].file == 'msp' .. i) end\n"
    "assert (buffers [10].file == 'c.wav')\n"
    // ours ends, and someone else takes the buffer: the old handle touches nothing
    "stop (c)\n"
    "buffers [10] = { file = 'x10', status = 1 }\n"
    "buffers [5] = nil\n"
    "local e = play ('e.wav')\n"
    "assert (buffers [5].file == 'e.wav')\n"
    "buffers [5].status = 0\n"
    "assert (not isplaying (e))\n"
    "buffers [5] = { file = 'theirs', status = 1 }\n"
    "stop (e)\n"
    "assert (buffers [5].file == 'theirs')\n"
    // and stopping everything stops only ours
    "buffers [10] = nil\n"
    "local f = play ('f.wav')\n"
    "stop (0)\n"
    "for i = 1, 9 do assert (buffers [i] and buffers [i].file ~= 'f.wav') end\n"
    "assert (buffers [10] == nil)\n"));
}
