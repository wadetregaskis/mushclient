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
