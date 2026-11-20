// speech_router.cpp -- see speech_router.h.

#include "speech_router.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace a11y {

const char * const kCategoryOutput  = "output";
const char * const kCategoryNote    = "note";
const char * const kCategoryInput   = "input";
const char * const kCategoryPrompt  = "prompt";
const char * const kCategoryTrigger = "trigger";
const char * const kCategoryAlias   = "alias";
const char * const kCategoryTimer   = "timer";
const char * const kCategoryScript  = "script";
const char * const kCategorySystem  = "system";

namespace {

bool IsBlank (const std::string & s)
  {
  return s.find_first_not_of (" \t\r\n") == std::string::npos;
  }

// The length of the UTF-8 character starting here, which is 1 for anything that is not
// one: this is for finding the edges of characters, not for validating them.
size_t CharLength (const std::string & s, size_t i)
  {
  const unsigned char c = (unsigned char) s [i];
  size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  if (i + n > s.size ())
    return 1;
  for (size_t j = 1; j < n; j++)
    if (((unsigned char) s [i + j] & 0xC0) != 0x80)
      return 1;
  return n;
  }

size_t PreviousCharStart (const std::string & s, size_t i)
  {
  if (i == 0)
    return 0;
  size_t j = i - 1;
  while (j > 0 && i - j < 4 && ((unsigned char) s [j] & 0xC0) == 0x80)
    j--;
  return j + CharLength (s, j) == i ? j : i - 1;
  }

bool IsWordChar (const std::string & s, size_t i)
  {
  const unsigned char c = (unsigned char) s [i];
  return c >= 0x80 || isalnum (c) || c == '\'' || c == '_';
  }

void LuaQuote (std::string & out, const std::string & s)
  {
  out += '"';
  for (const char c : s)
    switch (c)
      {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default:   out += c; break;
      }
  out += '"';
  }

// One Lua string starting at the quote at s [i].  Leaves i after the closing quote.
bool LuaUnquote (const std::string & s, size_t & i, std::string & out)
  {
  if (i >= s.size () || s [i] != '"')
    return false;
  out.clear ();
  for (i++; i < s.size (); i++)
    {
    char c = s [i];
    if (c == '"')
      {
      i++;
      return true;
      }
    if (c == '\\' && i + 1 < s.size ())
      {
      c = s [++i];
      switch (c)
        {
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        default: break;
        }
      }
    out += c;
    }
  return false;
  }

const char * PriorityName (pal::SpeechPriority p)
  {
  return p == pal::SpeechPriority::Polite ? "0" : p == pal::SpeechPriority::Assertive ? "2" : "1";
  }

}  // namespace

// ---------------------------------------------------------------- categories

std::string CategoriesToText (const std::map<std::string, CategorySettings> & categories)
  {
  std::string out;
  for (const auto & it : categories)
    {
    out += it.first;
    out += ";on=";
    out += it.second.enabled ? "1" : "0";
    out += ";prio=";
    out += PriorityName (it.second.priority);
    if (!it.second.cue.empty ())
      {
      out += ";cue=";
      out += it.second.cue;
      }
    if (it.second.cueVolume != 0)
      out += ";vol=" + std::to_string ((int) it.second.cueVolume);
    if (it.second.cuePan != 0)
      out += ";pan=" + std::to_string ((int) it.second.cuePan);
    out += "\n";
    }
  return out;
  }

std::map<std::string, CategorySettings> CategoriesFromText (const std::string & text)
  {
  std::map<std::string, CategorySettings> categories;
  size_t start = 0;
  while (start < text.size ())
    {
    size_t end = text.find ('\n', start);
    if (end == std::string::npos)
      end = text.size ();
    const std::string line = text.substr (start, end - start);
    start = end + 1;

    size_t semi = line.find (';');
    const std::string name = line.substr (0, semi);
    if (name.empty ())
      continue;
    CategorySettings settings;
    while (semi != std::string::npos)
      {
      const size_t from = semi + 1;
      semi = line.find (';', from);
      const std::string field = line.substr (from, semi == std::string::npos ? semi : semi - from);
      const size_t equals = field.find ('=');
      if (equals == std::string::npos)
        continue;
      const std::string key = field.substr (0, equals);
      const std::string value = field.substr (equals + 1);
      if (key == "on")
        settings.enabled = value != "0";
      else if (key == "prio")
        settings.priority = value == "0" ? pal::SpeechPriority::Polite :
                            value == "2" ? pal::SpeechPriority::Assertive :
                                           pal::SpeechPriority::Normal;
      else if (key == "cue")
        settings.cue = value;
      else if (key == "vol")
        settings.cueVolume = atof (value.c_str ());
      else if (key == "pan")
        settings.cuePan = atof (value.c_str ());
      }
    categories [name] = settings;
    }
  return categories;
  }

// ---------------------------------------------------------------- substitutions

bool Substitutions::apply (const std::string & line, std::string & spoken) const
  {
  spoken = line;
  if (!enabled)
    return true;
  const auto it = entries.find (line);
  if (it == entries.end ())
    return true;
  if (it->second == "!skip")
    return false;
  spoken = it->second;
  return true;
  }

std::string Substitutions::toFile () const
  {
  std::string out = "return {\n  status = ";
  out += enabled ? "1" : "0";
  out += ",\n";
  for (const auto & it : entries)
    {
    out += "  [";
    LuaQuote (out, it.first);
    out += "] = ";
    LuaQuote (out, it.second);
    out += ",\n";
    }
  out += "}\n";
  return out;
  }

void Substitutions::fromFile (const std::string & text)
  {
  entries.clear ();
  enabled = true;

  const size_t status = text.find ("status");
  if (status != std::string::npos)
    {
    const size_t digit = text.find_first_of ("01", status);
    if (digit != std::string::npos)
      enabled = text [digit] != '0';
    }

  // ["from"] = "to"
  size_t i = 0;
  while ((i = text.find ('[', i)) != std::string::npos)
    {
    size_t j = i + 1;
    while (j < text.size () && isspace ((unsigned char) text [j]))
      j++;
    std::string from, to;
    if (!LuaUnquote (text, j, from))
      {
      i++;
      continue;
      }
    while (j < text.size () && text [j] != '=' && text [j] != '\n')
      j++;
    if (j >= text.size () || text [j] != '=')
      {
      i = j;
      continue;
      }
    j++;
    while (j < text.size () && isspace ((unsigned char) text [j]))
      j++;
    if (LuaUnquote (text, j, to))
      entries [from] = to;
    i = j;
    }
  }

// ---------------------------------------------------------------- review

bool ReviewCursor::matches (const HistoryLine & line) const
  {
  return m_category.empty () || line.category == m_category;
  }

const HistoryLine * ReviewCursor::find (uint64_t seq) const
  {
  if (m_history.empty () || seq < m_history.front ().seq || seq > m_history.back ().seq)
    return nullptr;
  return &m_history [(size_t) (seq - m_history.front ().seq)];
  }

const HistoryLine * ReviewCursor::edge (int direction) const
  {
  if (direction < 0)
    {
    for (const HistoryLine & line : m_history)
      if (matches (line))
        return &line;
    }
  else
    for (auto it = m_history.rbegin (); it != m_history.rend (); ++it)
      if (matches (*it))
        return &*it;
  return nullptr;
  }

const HistoryLine * ReviewCursor::current () const
  {
  const HistoryLine * line = m_reviewing ? find (m_seq) : nullptr;
  if (line)
    return line;
  // live, or the line it was on has been trimmed away
  return m_reviewing ? edge (-1) : edge (1);
  }

const HistoryLine * ReviewCursor::step (const HistoryLine * from, int direction) const
  {
  if (!from)
    return nullptr;
  uint64_t seq = from->seq;
  for (;;)
    {
    seq += direction;
    const HistoryLine * line = find (seq);
    if (!line)
      return nullptr;
    if (matches (*line))
      return line;
    }
  }

std::string ReviewCursor::sayLine (const HistoryLine * line)
  {
  if (!line)
    return "No lines.";
  m_reviewing = true;
  m_seq = line->seq;
  m_offset = 0;
  return IsBlank (line->text) ? "blank" : line->text;
  }

std::string ReviewCursor::previousLine ()
  {
  const HistoryLine * from = current ();
  const HistoryLine * line = m_reviewing ? step (from, -1) : from;
  if (!line && from)
    return "Top. " + sayLine (from);
  return sayLine (line);
  }

std::string ReviewCursor::nextLine ()
  {
  const HistoryLine * from = current ();
  if (!m_reviewing)
    return "Bottom. " + sayLine (from);
  const HistoryLine * line = step (from, 1);
  if (!line && from)
    return "Bottom. " + sayLine (from);
  return sayLine (line);
  }

std::string ReviewCursor::firstLine ()
  {
  return sayLine (edge (-1));
  }

std::string ReviewCursor::lastLine ()
  {
  std::string s = sayLine (edge (1));
  goLive ();
  return s;
  }

std::string ReviewCursor::currentLine ()
  {
  const bool bWasLive = !m_reviewing;
  std::string s = sayLine (current ());
  if (bWasLive)
    m_reviewing = false;    // looking at the newest line is not holding speech back
  return s;
  }

std::string ReviewCursor::recentLine (int n)
  {
  const HistoryLine * line = edge (1);
  for (int i = 1; line && i < n; i++)
    line = step (line, -1);
  if (!line)
    return "No such line.";
  return sayLine (line);
  }

void ReviewCursor::goLive ()
  {
  m_reviewing = false;
  m_seq = 0;
  m_offset = 0;
  }

void ReviewCursor::setCategory (const std::string & category)
  {
  m_category = category;
  goLive ();
  }

size_t ReviewCursor::lineCount () const
  {
  if (m_category.empty ())
    return m_history.size ();
  return (size_t) std::count_if (m_history.begin (), m_history.end (),
                                 [this] (const HistoryLine & line) { return matches (line); });
  }

std::string ReviewCursor::currentWord () const
  {
  const HistoryLine * line = current ();
  if (!line)
    return "No lines.";
  const std::string & s = line->text;
  size_t start = std::min (m_offset, s.size ());
  while (start < s.size () && !IsWordChar (s, start))
    start += CharLength (s, start);
  size_t end = start;
  while (end < s.size () && IsWordChar (s, end))
    end += CharLength (s, end);
  if (start == end)
    return "blank";
  return s.substr (start, end - start);
  }

std::string ReviewCursor::nextWord ()
  {
  const HistoryLine * line = current ();
  if (!line)
    return "No lines.";
  if (!m_reviewing)
    {
    m_reviewing = true;
    m_seq = line->seq;
    m_offset = 0;
    }
  const std::string & s = line->text;
  size_t i = std::min (m_offset, s.size ());
  while (i < s.size () && IsWordChar (s, i))   // off this word
    i += CharLength (s, i);
  while (i < s.size () && !IsWordChar (s, i))  // to the next
    i += CharLength (s, i);
  if (i >= s.size ())
    return "End of line.";
  m_offset = i;
  return currentWord ();
  }

std::string ReviewCursor::previousWord ()
  {
  const HistoryLine * line = current ();
  if (!line)
    return "No lines.";
  if (!m_reviewing)
    {
    m_reviewing = true;
    m_seq = line->seq;
    m_offset = line->text.size ();
    }
  const std::string & s = line->text;
  size_t i = std::min (m_offset, s.size ());
  // back over what is between words, then to the start of the word before
  while (i > 0)
    {
    const size_t j = PreviousCharStart (s, i);
    if (IsWordChar (s, j))
      break;
    i = j;
    }
  if (i == 0)
    return "Start of line.";
  while (i > 0)
    {
    const size_t j = PreviousCharStart (s, i);
    if (!IsWordChar (s, j))
      break;
    i = j;
    }
  m_offset = i;
  return currentWord ();
  }

std::string SayCharacter (const std::string & c)
  {
  if (c == " ")
    return "space";
  if (c == "\t")
    return "tab";
  if (c.size () == 1 && isupper ((unsigned char) c [0]))
    return "cap " + c;
  return c;
  }

std::string ReviewCursor::nextChar ()
  {
  const HistoryLine * line = current ();
  if (!line)
    return "No lines.";
  const std::string & s = line->text;
  if (!m_reviewing)
    {
    m_reviewing = true;
    m_seq = line->seq;
    m_offset = 0;
    }
  else
    {
    size_t i = std::min (m_offset, s.size ());
    if (i < s.size ())
      i += CharLength (s, i);
    if (i >= s.size ())
      return "End of line.";
    m_offset = i;
    }
  if (m_offset >= s.size ())
    return "End of line.";
  return SayCharacter (s.substr (m_offset, CharLength (s, m_offset)));
  }

std::string ReviewCursor::previousChar ()
  {
  const HistoryLine * line = current ();
  if (!line)
    return "No lines.";
  const std::string & s = line->text;
  if (!m_reviewing)
    {
    m_reviewing = true;
    m_seq = line->seq;
    m_offset = s.size ();
    }
  size_t i = std::min (m_offset, s.size ());
  if (i == 0)
    return "Start of line.";
  i = PreviousCharStart (s, i);
  m_offset = i;
  return SayCharacter (s.substr (i, CharLength (s, i)));
  }

std::string ReviewCursor::spellWord () const
  {
  const std::string word = currentWord ();
  if (word == "No lines." || word == "blank")
    return word;
  std::string out;
  for (size_t i = 0; i < word.size (); )
    {
    const size_t n = CharLength (word, i);
    if (!out.empty ())
      out += ", ";
    out += SayCharacter (word.substr (i, n));
    i += n;
    }
  return out;
  }

// ---------------------------------------------------------------- the router

SpeechRouter::SpeechRouter () : m_review (m_history)
  {
  // A command the player typed they have just heard themselves type.
  m_categories [kCategoryInput].enabled = false;

  // What someone asked to have said is not to be lost in a flood.
  m_categories [kCategoryTrigger].priority = pal::SpeechPriority::Assertive;
  m_categories [kCategoryAlias].priority   = pal::SpeechPriority::Assertive;
  m_categories [kCategoryTimer].priority   = pal::SpeechPriority::Assertive;
  m_categories [kCategorySystem].priority  = pal::SpeechPriority::Assertive;
  }

void SpeechRouter::setBackend (pal::ISpeech * backend)
  {
  if (backend == m_backend)
    return;
  m_backend = backend;
  m_estimatedFreeAt = m_now;
  }

CategorySettings & SpeechRouter::category (const std::string & name)
  {
  return m_categories [name];
  }

std::vector<std::string> SpeechRouter::categoryNames () const
  {
  std::vector<std::string> names;
  for (const HistoryLine & line : m_history)
    if (std::find (names.begin (), names.end (), line.category) == names.end ())
      names.push_back (line.category);
  std::sort (names.begin (), names.end ());
  return names;
  }

void SpeechRouter::remember (const std::string & text, const std::string & category)
  {
  HistoryLine entry;
  entry.seq = m_nextSeq++;
  entry.text = text;
  entry.category = category;
  m_history.push_back (std::move (entry));
  while (m_history.size () > m_historyLimit)
    m_history.pop_front ();
  }

void SpeechRouter::enqueue (const std::string & text, pal::SpeechPriority priority, bool bFirst)
  {
  Pending p;
  p.text = text;
  p.priority = priority;
  p.queuedAt = m_now;
  p.bFirst = bFirst;
  if (bFirst)
    for (Pending & other : m_pending)
      other.bFirst = false;      // the newest interrupt is the one that goes first
  m_pending.push_back (std::move (p));
  if (m_hold)
    m_heldNew++;
  }

void SpeechRouter::line (const std::string & text, const std::string & category, bool bSpeak)
  {
  // What the prompt already said, once, is not news when its newline turns up.
  std::string news = text;
  if (!m_promptSpoken.empty ())
    {
    if (news.compare (0, m_promptSpoken.size (), m_promptSpoken) == 0)
      news.erase (0, m_promptSpoken.size ());
    m_promptSpoken.clear ();
    }

  remember (text, category);
  if (!bSpeak || m_muted)
    return;

  const auto it = m_categories.find (category);
  const CategorySettings settings = it == m_categories.end () ? CategorySettings () : it->second;
  if (!settings.enabled)
    return;

  if (onCue && !settings.cue.empty ())
    onCue (category, settings);

  if (IsBlank (news))
    return;

  std::string spoken;
  if (m_substitutions.apply (news, spoken) && !IsBlank (spoken))
    enqueue (spoken, settings.priority);
  }

void SpeechRouter::prompt (const std::string & text)
  {
  if (m_muted || IsBlank (text) || text == m_promptSpoken)
    return;
  const auto it = m_categories.find (kCategoryPrompt);
  if (it != m_categories.end () && !it->second.enabled)
    return;

  // A prompt that grew says only what is new.
  std::string news = text;
  if (news.compare (0, m_promptSpoken.size (), m_promptSpoken) == 0)
    news.erase (0, m_promptSpoken.size ());
  m_promptSpoken = text;
  if (IsBlank (news))
    return;

  std::string spoken;
  if (m_substitutions.apply (news, spoken) && !IsBlank (spoken))
    enqueue (spoken, it == m_categories.end () ? pal::SpeechPriority::Normal : it->second.priority);
  }

void SpeechRouter::say (const std::string & text, const std::string & category,
                        pal::SpeechPriority priority, bool bInterrupt)
  {
  remember (text, category);
  if (m_muted)
    return;

  const auto it = m_categories.find (category);
  if (it != m_categories.end ())
    {
    if (!it->second.enabled)
      return;
    if (onCue && !it->second.cue.empty ())
      onCue (category, it->second);
    }

  if (bInterrupt)
    {
    dropWaitingButRecentUrgent ();
    m_stopRequested = true;
    }
  if (!IsBlank (text))
    enqueue (text, priority, bInterrupt);
  }

void SpeechRouter::sayNow (const std::string & text)
  {
  // What is waiting stays waiting: it may be what the player is holding back while
  // reading something else, and they were told it would not be lost.
  m_stopRequested = false;
  if (m_backend)
    deliver (text, pal::SpeechPriority::Assertive, true, m_now);
  }

// What is waiting goes, except what is urgent and new: an alert that came in just before the
// player pressed Enter, or a script interrupted, is still news.  One from a while ago is not,
// and nor is the backlog a busy urgent category can build: only a few of the newest stay.
void SpeechRouter::dropWaitingButRecentUrgent ()
  {
  std::deque<Pending> kept;
  for (auto it = m_pending.rbegin (); it != m_pending.rend () && kept.size () < m_policy.urgentKept; ++it)
    if (it->priority == pal::SpeechPriority::Assertive && m_now - it->queuedAt <= m_policy.urgentRecentMs)
      kept.push_front (std::move (*it));
  m_totalSkipped += m_pending.size () - kept.size ();
  m_pending.swap (kept);
  m_skipped = 0;
  }

void SpeechRouter::commandEntered ()
  {
  dropWaitingButRecentUrgent ();
  m_heldNew = 0;
  m_promptSpoken.clear ();
  m_review.goLive ();
  m_estimatedFreeAt = m_now;
  if (m_backend)
    m_backend->stop ();
  }

void SpeechRouter::stop ()
  {
  m_totalSkipped += m_pending.size ();
  m_pending.clear ();
  m_skipped = 0;
  m_heldNew = 0;
  m_estimatedFreeAt = m_now;
  if (m_backend)
    m_backend->stop ();
  }

void SpeechRouter::skipToLatest ()
  {
  const bool bAny = !m_pending.empty ();
  std::string latest;
  if (bAny)
    latest = m_pending.back ().text;
  stop ();
  m_totalSkipped -= bAny ? 1 : 0;
  if (bAny)
    enqueue (latest, pal::SpeechPriority::Normal);
  }

void SpeechRouter::setMuted (bool bMuted)
  {
  if (bMuted == m_muted)
    return;
  m_muted = bMuted;
  if (bMuted)
    stop ();
  }

void SpeechRouter::setHold (bool bHold)
  {
  if (bHold == m_hold)
    return;
  m_hold = bHold;
  if (bHold)
    m_heldNew = 0;
  else if (m_heldNew && !m_muted)
    {
    // Say why there is suddenly a lot to hear, ahead of it.
    const std::string news = std::to_string (m_heldNew) +
                             (m_heldNew == 1 ? " new line." : " new lines.");
    m_heldNew = 0;
    Pending p;
    p.text = news;
    p.priority = pal::SpeechPriority::Normal;
    m_pending.push_front (std::move (p));
    }
  }

// The newest lines and anything urgent survive; what is in the middle does not.  Urgent
// lines have a limit of their own, a generous one, past which the oldest of them go too.
void SpeechRouter::applyFloodPolicy ()
  {
  // The oldest `drop` of those that are urgent (or not), never an interrupt's own.
  auto dropOldest = [this] (const bool bUrgent, size_t drop)
    {
    std::deque<Pending> kept;
    for (Pending & p : m_pending)
      {
      if (drop && !p.bFirst && (p.priority == pal::SpeechPriority::Assertive) == bUrgent)
        {
        drop--;
        m_skipped++;
        m_totalSkipped++;
        continue;
        }
      kept.push_back (std::move (p));
      }
    m_pending.swap (kept);
    };

  size_t urgent = 0;
  for (const Pending & p : m_pending)
    if (p.priority == pal::SpeechPriority::Assertive && !p.bFirst)
      urgent++;
  if (m_policy.maxUrgent && urgent > m_policy.maxUrgent)
    dropOldest (true, urgent - m_policy.maxUrgent);

  if (m_policy.maxPending == 0)
    return;

  size_t droppable = 0;
  for (const Pending & p : m_pending)
    if (p.priority != pal::SpeechPriority::Assertive && !p.bFirst)
      droppable++;
  if (droppable <= m_policy.maxPending)
    return;
  dropOldest (false, droppable - std::min (m_policy.keepLatest, droppable));
  }

bool SpeechRouter::backendReady (int64_t nowMs)
  {
  if (m_backend->capabilities () & pal::kSpeechKnowsBusy)
    {
    if (nowMs - m_lastSpeakAt < m_policy.minGapMs)
      return false;
    // It can say so for ever, with a voice that never finishes: NVDA's silent synth does.
    // Long past when it should have finished, stop believing it.
    return !m_backend->isBusy () || nowMs - m_estimatedFreeAt > m_policy.busyGraceMs;
    }

  // It cannot say, but it does have a queue of its own, so it can be a little ahead of
  // itself.  Not so far ahead that what is queued there can no longer be dropped.
  return m_estimatedFreeAt - nowMs <= m_policy.leadMs;
  }

void SpeechRouter::deliver (const std::string & text, pal::SpeechPriority priority, bool bFlush, int64_t nowMs)
  {
  m_backend->speak (text.c_str (), priority, bFlush);
  if (m_braille && (m_backend->capabilities () & pal::kSpeechBraille))
    {
    // A display holds a line or so: the newest of what went together.
    const size_t newline = text.find_last_of ('\n');
    m_backend->braille ((newline == std::string::npos ? text : text.substr (newline + 1)).c_str ());
    }
  m_totalSpoken++;
  m_lastSpoken = text;
  m_lastSpeakAt = nowMs;

  const int cps = std::max (1, m_policy.estimatedCharsPerSecond);
  const int64_t ms = (int64_t) text.size () * 1000 / cps;
  m_estimatedFreeAt = (bFlush ? nowMs : std::max (m_estimatedFreeAt, nowMs)) + ms;
  }

void SpeechRouter::tick (int64_t nowMs)
  {
  m_now = nowMs;

  if (!m_backend)
    {
    // Nobody to tell, and when there is, this will be old.
    m_pending.clear ();
    m_skipped = 0;
    m_stopRequested = false;
    return;
    }

  if (m_stopRequested)
    {
    m_stopRequested = false;
    m_backend->stop ();
    m_estimatedFreeAt = nowMs;
    }

  if (m_pending.empty ())
    return;

  applyFloodPolicy ();

  const bool bHeld = m_hold;
  if (!bHeld && !backendReady (nowMs))
    return;

  // Together, so that a screenful is one thing for the backend to be getting on with.
  std::string batch;
  pal::SpeechPriority priority = pal::SpeechPriority::Polite;
  if (m_skipped && !bHeld)
    {
    batch = std::to_string (m_skipped) + (m_skipped == 1 ? " line skipped." : " lines skipped.");
    m_skipped = 0;
    priority = pal::SpeechPriority::Normal;
    }

  // What a script interrupted with goes before anything, even what urgent was kept
  // through the interrupt.  While held it is as urgent as its priority says.
  for (auto it = m_pending.begin (); it != m_pending.end (); ++it)
    if (it->bFirst)
      {
      if (!bHeld || it->priority == pal::SpeechPriority::Assertive)
        {
        batch = batch.empty () ? it->text : it->text + "\n" + batch;
        if (it->priority == pal::SpeechPriority::Assertive ||
            (it->priority == pal::SpeechPriority::Normal && priority == pal::SpeechPriority::Polite))
          priority = it->priority;
        m_pending.erase (it);
        }
      break;
      }

  // Urgent next.  While held nothing else goes at all.
  for (auto it = m_pending.begin (); it != m_pending.end (); )
    {
    if (it->priority == pal::SpeechPriority::Assertive)
      {
      if (!batch.empty ())
        batch += "\n";
      batch += it->text;
      priority = pal::SpeechPriority::Assertive;
      it = m_pending.erase (it);
      }
    else
      ++it;
    }

  if (!bHeld)
    while (!m_pending.empty () &&
           (batch.empty () || batch.size () + m_pending.front ().text.size () < m_policy.maxBatchChars))
      {
      if (!batch.empty ())
        batch += "\n";
      batch += m_pending.front ().text;
      if (m_pending.front ().priority == pal::SpeechPriority::Normal &&
          priority == pal::SpeechPriority::Polite)
        priority = pal::SpeechPriority::Normal;
      m_pending.pop_front ();
      }

  if (!batch.empty ())
    deliver (batch, priority, false, nowMs);
  }

}  // namespace a11y
