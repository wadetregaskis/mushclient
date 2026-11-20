// speech_router.h -- what to say, and when.  No document, no toolkit, no clock of its
// own: lines go in, ticks come in with the time, and calls come out on pal::ISpeech, so
// a test can drive all of it with a fake.  src/a11y.cpp is the glue to the document.
//
// docs/ACCESSIBILITY.md describes what a player or plugin author sees.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pal/ispeech.h"

namespace a11y {

// ---------------------------------------------------------------- categories

struct CategorySettings
  {
  bool enabled = true;
  pal::SpeechPriority priority = pal::SpeechPriority::Normal;
  std::string cue;           // a sound file, or nothing
  double cueVolume = 0;      // as world.PlaySound: 0 is full, -100 is silent
  double cuePan = 0;         // -100 left, +100 right
  };

// The names the client itself files things under.  Scripts can use any others.
extern const char * const kCategoryOutput;     // lines from the MUD
extern const char * const kCategoryNote;       // world.Note and friends
extern const char * const kCategoryInput;      // the player's own commands echoed
extern const char * const kCategoryPrompt;     // a line with no newline, gone quiet
extern const char * const kCategoryTrigger;    // Send To: Speech, from a trigger
extern const char * const kCategoryAlias;      // ... an alias
extern const char * const kCategoryTimer;      // ... a timer
extern const char * const kCategoryScript;     // world.Speak with no category
extern const char * const kCategorySystem;     // what the client says about itself

// "tells;on=1;prio=2;cue=a.wav;vol=-10;pan=30", a category a line, for a world option.
std::string CategoriesToText (const std::map<std::string, CategorySettings> & categories);
std::map<std::string, CategorySettings> CategoriesFromText (const std::string & text);

// ---------------------------------------------------------------- substitutions

// MushReader's: a whole line that matches is said differently, or if replaced with
// "!skip" not at all.  Kept in its substitutions.mush, which is Lua source for a table.
class Substitutions
  {
  public:
    bool enabled = true;
    std::map<std::string, std::string> entries;

    // What to say for this line.  False means say nothing.
    bool apply (const std::string & line, std::string & spoken) const;

    std::string toFile () const;
    void fromFile (const std::string & text);
  };

// ---------------------------------------------------------------- flood policy

struct FloodPolicy
  {
  size_t maxPending = 40;          // lines waiting before the middle is dropped; 0 is never
  size_t keepLatest = 5;           // ... how many of the newest survive that
  size_t maxBatchChars = 500;      // lines given to the backend together, about this long
  int    estimatedCharsPerSecond = 25;   // for a backend that cannot say if it is busy
  int    leadMs = 4000;            // ... how far ahead of itself it may be given work
  int    minGapMs = 120;           // after speaking, before believing "not busy"
  int    busyGraceMs = 5000;       // past the estimate, before no longer believing "busy"
  };

// ---------------------------------------------------------------- history and review

struct HistoryLine
  {
  uint64_t seq;
  std::string text;
  std::string category;
  };

// The review cursor: a line, and a place in it, that new lines do not move.  It holds a
// sequence number, so trimming old lines cannot make it point at something else.
class ReviewCursor
  {
  public:
    explicit ReviewCursor (const std::deque<HistoryLine> & history) : m_history (history) {}

    // All of these say what to speak.
    std::string previousLine ();
    std::string nextLine ();
    std::string firstLine ();
    std::string lastLine ();      // ... and is live again, following new lines
    std::string currentLine ();
    std::string recentLine (int n);   // 1 is the newest

    std::string previousWord ();
    std::string nextWord ();
    std::string currentWord () const;
    std::string previousChar ();
    std::string nextChar ();
    std::string spellWord () const;

    // True while it is sitting on a particular line rather than following new ones.
    bool reviewing () const { return m_reviewing; }
    void goLive ();

    // "" is every category.
    void setCategory (const std::string & category);
    const std::string & category () const { return m_category; }
    size_t lineCount () const;

  private:
    const HistoryLine * find (uint64_t seq) const;
    const HistoryLine * current () const;
    const HistoryLine * step (const HistoryLine * from, int direction) const;
    const HistoryLine * edge (int direction) const;
    bool matches (const HistoryLine & line) const;
    std::string sayLine (const HistoryLine * line);

    const std::deque<HistoryLine> & m_history;
    std::string m_category;
    bool m_reviewing = false;
    uint64_t m_seq = 0;
    size_t m_offset = 0;     // byte offset of the current word or character
    };

// How a single character reads: "space" rather than silence, and capitals marked.
std::string SayCharacter (const std::string & utf8Character);

// ---------------------------------------------------------------- the router

class SpeechRouter
  {
  public:
    SpeechRouter ();

    // The backend can change at any time: a reader starts or stops.  NULL is silence.
    void setBackend (pal::ISpeech * backend);
    pal::ISpeech * backend () const { return m_backend; }

    // A line of output has arrived.  bSpeak false puts it in the history only.
    void line (const std::string & text, const std::string & category, bool bSpeak = true);

    // A partial line with no newline, gone quiet, such as "Enter your name:".  When the
    // whole line arrives what has already been said is not said again.
    void prompt (const std::string & text);

    // Something a script or trigger asked to have said.  Never dropped by the flood
    // policy if its priority is Assertive.
    void say (const std::string & text, const std::string & category,
              pal::SpeechPriority priority, bool bInterrupt);

    // Now, over anything else: review, and the client's own messages.
    void sayNow (const std::string & text);

    // Delivers what it can.  Call often, with a clock that does not go backwards.
    void tick (int64_t nowMs);

    // The player pressed Enter: what is waiting is no longer news.
    void commandEntered ();
    // Silence, and nothing waiting.
    void stop ();
    // Silence, and only the newest line.
    void skipToLatest ();

    void setMuted (bool bMuted);
    bool muted () const { return m_muted; }

    // What is said also goes to a braille display, if the backend has a way there.
    void setBraille (bool bBraille) { m_braille = bBraille; }

    // While held, what is not urgent waits: the player is reading something.
    void setHold (bool bHold);
    bool held () const { return m_hold; }

    // Called with a category's settings as a line for it arrives, for sound cues.
    std::function<void (const std::string &, const CategorySettings &)> onCue;

    CategorySettings & category (const std::string & name);
    const std::map<std::string, CategorySettings> & categories () const { return m_categories; }
    std::vector<std::string> categoryNames () const;     // those with lines, sorted

    Substitutions & substitutions () { return m_substitutions; }
    FloodPolicy & policy () { return m_policy; }

    const std::deque<HistoryLine> & history () const { return m_history; }
    ReviewCursor & review () { return m_review; }
    void setHistoryLimit (size_t n) { m_historyLimit = n; }

    size_t pendingCount () const { return m_pending.size (); }
    uint64_t linesSkipped () const { return m_totalSkipped; }
    uint64_t linesSpoken () const { return m_totalSpoken; }
    const std::string & lastSpoken () const { return m_lastSpoken; }

  private:
    struct Pending
      {
      std::string text;
      pal::SpeechPriority priority;
      };

    void remember (const std::string & text, const std::string & category);
    void enqueue (const std::string & text, pal::SpeechPriority priority);
    void applyFloodPolicy ();
    bool backendReady (int64_t nowMs);
    void deliver (const std::string & text, pal::SpeechPriority priority, bool bFlush, int64_t nowMs);

    pal::ISpeech * m_backend = nullptr;
    std::deque<Pending> m_pending;
    std::deque<HistoryLine> m_history;
    std::map<std::string, CategorySettings> m_categories;
    Substitutions m_substitutions;
    FloodPolicy m_policy;
    ReviewCursor m_review;
    size_t m_historyLimit = 5000;
    uint64_t m_nextSeq = 1;

    bool m_muted = false;
    bool m_braille = true;
    bool m_hold = false;
    size_t m_heldNew = 0;      // lines that have waited for a hold
    size_t m_skipped = 0;      // dropped, and not yet said so
    bool m_stopRequested = false;
    std::string m_promptSpoken;

    int64_t m_now = 0;
    int64_t m_estimatedFreeAt = 0;
    int64_t m_lastSpeakAt = -1000000;

    uint64_t m_totalSkipped = 0;
    uint64_t m_totalSpoken = 0;
    std::string m_lastSpoken;
  };

}  // namespace a11y
