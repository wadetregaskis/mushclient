// a11y.cpp -- the document's half of speech, review and the text pane.  What to say and
// when is in speech_router.cpp, and the screen readers themselves are behind pal::ISpeech.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "a11y.h"
#include "text_edges.h"
#include "mushview.h"     // MW_MOUSE_LH
#include "miniwindow.h"
#include "scripting/errors.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

extern char working_dir [_MAX_PATH];

namespace pal {
ISpeechPlatform * g_speechPlatform = nullptr;
}

namespace {

const int kPollIntervalMs = 2000;     // how often to look for a screen reader
const int kPromptQuietMs = 350;       // a partial line this quiet is a prompt
const size_t kSeedLines = 5000;       // as many as review and the text pane keep

// Which screen reader there is applies to the whole application, not to a world.
struct AppSpeech
  {
  long long iLastPoll = -1000000;
  pal::ISpeech * pReader = nullptr;
  bool bAutoReading = false;
  bool bAppliedVoice = false;
  int iAppliedRate = 0;
  int iAppliedVolume = 100;
  std::string strAppliedVoice;
  };

AppSpeech g_app;

long long NowMs ()
  {
  using namespace std::chrono;
  return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
  }

void PollPlatform (const long long now)
  {
  if (now - g_app.iLastPoll < kPollIntervalMs)
    return;
  g_app.iLastPoll = now;

  const int iMode = App.m_iSpeechMode;
  g_app.pReader = iMode != eSpeechOff && pal::g_speechPlatform ?
                  pal::g_speechPlatform->screenReader () : nullptr;
  g_app.bAutoReading = iMode == eSpeechAlways || (iMode == eSpeechAuto && g_app.pReader);
  }

pal::ISpeech * OwnVoice ()
  {
  pal::ISpeech * pVoice = pal::g_speechPlatform ? pal::g_speechPlatform->ownVoice () : nullptr;
  if (pVoice)
    {
    // The preferences can change at any time.
    const std::string strVoice = (const char *) App.m_strSpeechVoice;
    if (!g_app.bAppliedVoice || g_app.iAppliedRate != App.m_iSpeechRate)
      pVoice->setRate (App.m_iSpeechRate);
    if (!g_app.bAppliedVoice || g_app.iAppliedVolume != App.m_iSpeechVolume)
      pVoice->setVolume (App.m_iSpeechVolume);
    if (!g_app.bAppliedVoice || g_app.strAppliedVoice != strVoice)
      pVoice->setVoice (strVoice.c_str ());
    g_app.bAppliedVoice = true;
    g_app.iAppliedRate = App.m_iSpeechRate;
    g_app.iAppliedVolume = App.m_iSpeechVolume;
    g_app.strAppliedVoice = strVoice;
    }
  return pVoice;
  }

pal::ISpeech * BackendFor (const CA11yState & state)
  {
  if (App.m_iSpeechMode == eSpeechOff)
    return nullptr;
  if (g_app.pReader)
    return g_app.pReader;
  if (App.m_iSpeechMode == eSpeechAlways || state.bExplicitVoice)
    return OwnVoice ();
  return nullptr;
  }

// Is this world's output being read out?
bool Reading (const CMUSHclientDoc & doc)
  {
  return g_app.bAutoReading || (doc.m_bSpeechReadAll && App.m_iSpeechMode != eSpeechOff);
  }

// Lines from a plugin which does its own speaking would be spoken twice.  A real
// MushReader or LuaAudio never runs: they are replaced by ID, in a11y_shims.cpp.
bool OtherSpeechPlugin (CMUSHclientDoc & doc)
  {
  for (CPlugin * pPlugin : doc.m_PluginList)
    {
    if (!pPlugin->m_bEnabled)
      continue;
    if (pPlugin->m_strID.CompareNoCase (kA11yMushReaderID) == 0)
      continue;
    if (pPlugin->m_strID.CompareNoCase ("463242566069ebfd1b379ec1") == 0)  // Text_To_Speech.xml
      return true;
    CString strName = pPlugin->m_strName;
    strName.MakeLower ();
    if (strName.Find ("mushreader") >= 0 || strName.Find ("text_to_speech") >= 0 ||
        strName.Find ("screenreader") >= 0 || strName.Find ("screen_reader") >= 0)
      return true;
    }
  return false;
  }

void LoadSubstitutions (CA11yState & state);

CA11yState & State (CMUSHclientDoc & doc)
  {
  if (!doc.m_pA11y)
    {
    doc.m_pA11y = new CA11yState;
    CA11yState & state = *doc.m_pA11y;
    LoadSubstitutions (state);
    state.router.onCue = [&doc, &state] (const std::string &, const a11y::CategorySettings & settings)
      {
      if (state.pSound)
        {
        state.pSound->playCue (settings.cue.c_str (), settings.cueVolume, settings.cuePan);
        return;
        }
      // The highest-numbered buffer nothing is using, as MSP does: scripts choosing
      // from the bottom do not meet us until the others are busy.
      for (int buffer = MAX_SOUND_BUFFERS; buffer >= 1; buffer--)
        if (doc.GetSoundStatus ((short) buffer) <= 0)
          {
          doc.PlaySound ((short) buffer, settings.cue.c_str (), FALSE,
                         settings.cueVolume, settings.cuePan);
          break;
          }
      };
    }
  return *doc.m_pA11y;
  }

// A world's whole line from the rows of it that end in hard_return.  Output already in
// the line list when reading starts is there to be reviewed, and is not news.
void WalkLines (CMUSHclientDoc & doc, const std::function<void (const std::string &, int)> & each,
                const bool bButNotTheLastOne = false)
  {
  // Only the newest kSeedLines: a scrollback can be far longer than anything kept here.
  POSITION start = doc.m_LineList.GetHeadPosition ();
  size_t iEnds = 0;
  for (POSITION pos = doc.m_LineList.GetTailPosition (); pos; )
    {
    const POSITION here = pos;
    const CLine * pLine = doc.m_LineList.GetPrev (pos);
    if (pLine->hard_return && ++iEnds > kSeedLines)
      {
      // the row ending the line before the first of them: they start after it
      start = here;
      doc.m_LineList.GetNext (start);
      break;
      }
    }

  std::string strLine;
  std::string strPrevious;
  int iPreviousFlags = 0;
  bool bHavePrevious = false;
  for (POSITION pos = start; pos; )
    {
    CLine * pLine = doc.m_LineList.GetNext (pos);
    if (pLine == doc.m_pCurrentLine && !pLine->hard_return)
      break;
    strLine.append (pLine->text, pLine->len);
    if (pLine->hard_return)
      {
      if (bHavePrevious)
        each (strPrevious, iPreviousFlags);
      strPrevious = strLine;
      iPreviousFlags = pLine->flags;
      bHavePrevious = true;
      strLine.clear ();
      }
    }
  if (bHavePrevious && !bButNotTheLastOne)
    each (strPrevious, iPreviousFlags);
  }

// The line still arriving, all of it: a long one has wrapped onto several rows, and only the
// last is m_pCurrentLine.  Said as a prompt, it has to be what the whole line starts with, or
// it is said again when the newline comes.
std::string PartialLine (CMUSHclientDoc & doc)
  {
  if (doc.m_LineList.IsEmpty () || doc.m_LineList.GetTail () != doc.m_pCurrentLine)
    return std::string (doc.m_pCurrentLine->text, doc.m_pCurrentLine->len);
  std::vector<const CLine *> rows;
  for (POSITION pos = doc.m_LineList.GetTailPosition (); pos && rows.size () < 1000; )
    {
    const CLine * pLine = doc.m_LineList.GetPrev (pos);
    if (pLine->hard_return)
      break;
    rows.push_back (pLine);
    }
  std::string out;
  for (auto it = rows.rbegin (); it != rows.rend (); ++it)
    out.append ((*it)->text, (*it)->len);
  return out;
  }

const char * CategoryForFlags (const int iFlags)
  {
  if (iFlags & COMMENT)
    return a11y::kCategoryNote;
  if (iFlags & USER_INPUT)
    return a11y::kCategoryInput;
  return a11y::kCategoryOutput;
  }

void SeedRouter (CMUSHclientDoc & doc, CA11yState & state, const bool bButNotTheLastOne = false)
  {
  if (state.bRouterSeeded)
    return;
  state.bRouterSeeded = true;
  WalkLines (doc, [&] (const std::string & line, const int iFlags)
    {
    state.router.line (A11y_ToUtf8 (doc, line.c_str ()), CategoryForFlags (iFlags), false);
    }, bButNotTheLastOne);
  }

void SeedPane (CMUSHclientDoc & doc, CA11yState & state)
  {
  if (state.bPaneSeeded || !doc.m_pWorldView)
    return;
  state.bPaneSeeded = true;
  std::string strAll;
  WalkLines (doc, [&] (const std::string & line, int)
    {
    strAll += A11y_ToUtf8 (doc, line.c_str ());
    strAll += "\n";
    });
  doc.m_pWorldView->accessibleReset (strAll.c_str ());
  }

void SyncSettings (CMUSHclientDoc & doc, CA11yState & state)
  {
  a11y::SpeechRouter & router = state.router;
  router.setMuted (doc.m_bSpeechMute != 0);
  router.setBraille (doc.m_bSpeechBraille != 0);
  router.policy ().maxPending = (size_t) doc.m_iSpeechFloodLines;

  const std::string strCategories = (const char *) doc.m_strSpeechCategories;
  if (strCategories != state.strAppliedCategories)
    {
    state.strAppliedCategories = strCategories;
    for (const auto & it : a11y::CategoriesFromText (strCategories))
      router.category (it.first) = it.second;
    }
  }

void SaveCategories (CMUSHclientDoc & doc, CA11yState & state)
  {
  const std::string text = a11y::CategoriesToText (state.router.categories ());
  state.strAppliedCategories = text;
  if (text == (const char *) doc.m_strSpeechCategories)
    return;
  doc.m_strSpeechCategories = text.c_str ();
  doc.SetModifiedFlag (TRUE);
  }

bool SameSettings (const a11y::CategorySettings & a, const a11y::CategorySettings & b)
  {
  return a.enabled == b.enabled && a.priority == b.priority && a.cue == b.cue &&
         a.cueVolume == b.cueVolume && a.cuePan == b.cuePan;
  }

std::string SubstitutionsPath ()
  {
  return std::string (working_dir) + "substitutions.mush";
  }

void LoadSubstitutions (CA11yState & state)
  {
  std::ifstream f (SubstitutionsPath ().c_str (), std::ios::binary);
  if (!f)
    return;
  std::stringstream text;
  text << f.rdbuf ();
  state.router.substitutions ().fromFile (text.str ());
  }

bool SaveSubstitutions (CA11yState & state)
  {
  std::ofstream f (SubstitutionsPath ().c_str (), std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  f << state.router.substitutions ().toFile ();
  return bool (f);
  }

std::string Trim (const std::string & s)
  {
  const size_t first = s.find_first_not_of (" \t");
  if (first == std::string::npos)
    return "";
  return s.substr (first, s.find_last_not_of (" \t") - first + 1);
  }

std::string Lower (std::string s)
  {
  for (char & c : s)
    c = (char) tolower ((unsigned char) c);
  return s;
  }

void SubstCommand (CMUSHclientDoc & doc, CA11yState & state, const std::string & strArgument)
  {
  a11y::Substitutions & subs = state.router.substitutions ();
  const std::string arg = Trim (strArgument);
  const std::string lower = Lower (arg);

  if (arg.empty () || lower == "help")
    doc.Note ("subst on|off|list|clear|add <text>==<replacement>|remove <text>|save");
  else if (lower == "on" || lower == "off")
    {
    subs.enabled = lower == "on";
    SaveSubstitutions (state);
    doc.Note (subs.enabled ? "Substitutions on." : "Substitutions off.");
    }
  else if (lower == "list")
    {
    if (subs.entries.empty ())
      doc.Note ("No substitutions.");
    for (const auto & it : subs.entries)
      doc.Note ((it.first + " == " + it.second).c_str ());
    }
  else if (lower == "clear")
    {
    subs.entries.clear ();
    SaveSubstitutions (state);
    doc.Note ("Substitutions cleared.");
    }
  else if (lower == "save")
    doc.Note (SaveSubstitutions (state) ? "Substitutions saved." : "Unable to save substitutions.");
  else if (lower.compare (0, 7, "remove ") == 0)
    {
    subs.entries.erase (Trim (arg.substr (7)));
    SaveSubstitutions (state);
    doc.Note ("Substitution removed.");
    }
  else if (lower.compare (0, 4, "add ") == 0)
    {
    const std::string payload = arg.substr (4);
    const size_t sep = payload.find ("==");
    const std::string key = sep == std::string::npos ? "" : Trim (payload.substr (0, sep));
    if (key.empty ())
      doc.Note ("Substitution add requires <text>==<replacement>.");
    else
      {
      subs.entries [key] = Trim (payload.substr (sep + 2));
      SaveSubstitutions (state);
      doc.Note ("Substitution added.");
      }
    }
  else
    doc.Note ("subst on|off|list|clear|add <text>==<replacement>|remove <text>|save");
  }

// Is a shim plugin loaded and enabled?
bool PluginEnabled (CMUSHclientDoc & doc, const char * sID)
  {
  CPlugin * pPlugin = doc.GetPlugin (sID);
  return pPlugin && pPlugin->m_bEnabled;
  }

// The review keys and the commands are only there while output is being read, or for
// someone who installed MushReader: a script that merely speaks, or a text pane wanted
// always, does not take keys or words from the player.
bool KeysLive (CMUSHclientDoc & doc)
  {
  return Reading (doc) || PluginEnabled (doc, kA11yMushReaderID);
  }

// Has the world or a plugin put something on this key with Accelerator?  Then it is theirs.
bool WorldHasAccelerator (const CMUSHclientDoc & doc, const A11yKey key, const int ch,
                          const bool bCtrl, const bool bAlt, const bool bShift)
  {
  if (doc.m_AcceleratorToCommandMap.empty ())
    return false;
  WORD vk = 0;
  switch (key)
    {
    case A11yKey::Up:       vk = VK_UP;     break;
    case A11yKey::Down:     vk = VK_DOWN;   break;
    case A11yKey::Left:     vk = VK_LEFT;   break;
    case A11yKey::Right:    vk = VK_RIGHT;  break;
    case A11yKey::Home:     vk = VK_HOME;   break;
    case A11yKey::End:      vk = VK_END;    break;
    case A11yKey::PageUp:   vk = VK_PRIOR;  break;
    case A11yKey::PageDown: vk = VK_NEXT;   break;
    case A11yKey::Return:   vk = VK_RETURN; break;
    case A11yKey::F6:       vk = VK_F6;     break;
    case A11yKey::F12:      vk = VK_F12;    break;
    case A11yKey::Character:
      vk = ch == ',' ? 0xBC :     // VK_OEM_COMMA
           ch == '.' ? 0xBE :     // VK_OEM_PERIOD
                       (WORD) ch; // letters and digits are their own
      break;
    }
  BYTE fVirt = FVIRTKEY | FNOINVERT;
  if (bCtrl)  fVirt |= FCONTROL;
  if (bAlt)   fVirt |= FALT;
  if (bShift) fVirt |= FSHIFT;
  return doc.m_AcceleratorToCommandMap.count (((long) fVirt) << 16 | vk) != 0;
  }

void ToggleMute (CMUSHclientDoc & doc, CA11yState & state)
  {
  a11y::SpeechRouter & router = state.router;
  router.setBackend (BackendFor (state));
  if (doc.m_bSpeechMute)
    {
    doc.m_bSpeechMute = false;
    router.setMuted (false);
    router.sayNow ("speech on");
    }
  else
    {
    doc.m_bSpeechMute = true;
    router.setMuted (true);
    // setMuted stops the backend: tell them, after that.

    if (router.backend ())
      router.backend ()->speak ("speech off", pal::SpeechPriority::Assertive, false);
    }
  }

void CycleCategory (CMUSHclientDoc & doc, CA11yState & state, const int iDirection)
  {
  a11y::SpeechRouter & router = state.router;
  const std::vector<std::string> names = router.categoryNames ();
  const int iCount = (int) names.size () + 1;    // the first is "all"
  state.iCategory = (state.iCategory + iDirection + iCount) % iCount;
  const std::string name = state.iCategory ? names [state.iCategory - 1] : "";
  router.review ().setCategory (name);
  router.sayNow ((name.empty () ? std::string ("All lines") : name) + ", " +
                 std::to_string (router.review ().lineCount ()) + " lines.");
  }

void SayHotspot (CMUSHclientDoc & doc, CA11yState & state, const int iDirection)
  {
  const std::vector<A11yHotspot> hotspots = A11y_ListHotspots (doc);
  if (hotspots.empty ())
    {
    state.router.sayNow ("No hotspots.");
    return;
    }
  const int iCount = (int) hotspots.size ();
  state.iHotspot = state.iHotspot < 0 && iDirection < 0 ? iCount - 1 :
                   (state.iHotspot + iDirection + iCount) % iCount;
  const A11yHotspot & hotspot = hotspots [state.iHotspot];
  state.router.sayNow (hotspot.label + ", " + std::to_string (state.iHotspot + 1) + " of " +
                       std::to_string (iCount));
  }

// What the review keys do, by name, which is also world.SpeechReview's argument: a plugin
// can put them on other keys.  False if there is no such action.
bool ReviewAction (CMUSHclientDoc & doc, CA11yState & state, const std::string & action)
  {
  a11y::SpeechRouter & router = state.router;
  a11y::ReviewCursor & review = router.review ();

  SeedRouter (doc, state);

  if      (action == "previous_line")  router.sayNow (review.previousLine ());
  else if (action == "next_line")      router.sayNow (review.nextLine ());
  else if (action == "first_line")     router.sayNow (review.firstLine ());
  else if (action == "last_line")      router.sayNow (review.lastLine ());
  else if (action == "current_line")   router.sayNow (review.currentLine ());
  else if (action == "previous_word")  router.sayNow (review.previousWord ());
  else if (action == "next_word")      router.sayNow (review.nextWord ());
  else if (action == "current_word")   router.sayNow (review.currentWord ());
  else if (action == "spell_word")     router.sayNow (review.spellWord ());
  else if (action == "previous_char")  router.sayNow (review.previousChar ());
  else if (action == "next_char")      router.sayNow (review.nextChar ());
  else if (action == "skip_to_latest") router.skipToLatest ();
  else if (action == "previous_category") CycleCategory (doc, state, -1);
  else if (action == "next_category")     CycleCategory (doc, state, 1);
  else if (action == "next_hotspot")      SayHotspot (doc, state, 1);
  else if (action == "previous_hotspot")  SayHotspot (doc, state, -1);
  else if (action == "activate_hotspot")
    {
    const std::vector<A11yHotspot> hotspots = A11y_ListHotspots (doc);
    if (state.iHotspot >= 0 && state.iHotspot < (int) hotspots.size ())
      {
      router.sayNow (hotspots [state.iHotspot].label);
      A11y_ActivateHotspot (doc, hotspots [state.iHotspot]);
      }
    else
      router.sayNow ("No hotspot chosen.");
    }
  else if (action == "toggle_mute")
    ToggleMute (doc, state);
  else if (action == "toggle_pane")
    {
    if (state.bPaneShown && doc.m_pWorldView)
      doc.m_pWorldView->accessibleFocusPane (!doc.m_pWorldView->accessiblePaneFocused ());
    else
      router.sayNow ("No text pane.");
    }
  else if (action.compare (0, 7, "recent_") == 0)
    router.sayNow (review.recentLine (atoi (action.c_str () + 7)));
  else
    return false;

  return true;
  }

}  // namespace

bool A11y_AutoReading ()
  {
  return g_app.bAutoReading;
  }

void A11y_ResetForTest ()
  {
  g_app = AppSpeech ();
  }

// ---------------------------------------------------------------- text

std::string A11y_ToUtf8 (const CMUSHclientDoc & doc, const char * sText)
  {
#ifdef _WIN32
  if (doc.m_bUTF_8)
    {
    // not assuming it is valid: U+FFFD rather than something a backend chokes on
    const int nWide = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, sText, -1, NULL, 0);
    if (nWide > 0)
      return sText;
    }
  CStringW strWide = WorldTextToWide (&doc, sText);
  const int nBytes = WideCharToMultiByte (CP_UTF8, 0, strWide, strWide.GetLength (), NULL, 0, NULL, NULL);
  std::string out (nBytes, '\0');
  if (nBytes)
    WideCharToMultiByte (CP_UTF8, 0, strWide, strWide.GetLength (), &out [0], nBytes, NULL, NULL);
  return out;
#else
  // Not knowing the world's code page here, anything that is not UTF-8 is taken for
  // Latin-1, which at least never comes out as something a backend chokes on.
  std::string out;
  const unsigned char * p = (const unsigned char *) sText;
  bool bValid = true;
  for (const unsigned char * q = p; *q && bValid; )
    {
    const int n = *q < 0x80 ? 1 : (*q >> 5) == 6 ? 2 : (*q >> 4) == 14 ? 3 : (*q >> 3) == 30 ? 4 : 0;
    if (!n)
      bValid = false;
    for (int i = 1; i < n && bValid; i++)
      bValid = (q [i] & 0xC0) == 0x80;
    q += n ? n : 1;
    }
  if (bValid)
    return sText;
  for (; *p; p++)
    if (*p < 0x80)
      out += (char) *p;
    else
      {
      out += (char) (0xC0 | (*p >> 6));
      out += (char) (0x80 | (*p & 0x3F));
      }
  return out;
#endif
  }

// ---------------------------------------------------------------- hooks

void A11y_Screendraw (CMUSHclientDoc & doc, const long iType, const char * sText)
  {
  const bool bReading = Reading (doc);
  if (!bReading && !doc.m_pA11y)
    return;

  // With nothing being read there can still be a pane (Always), or a history that review
  // has already seeded and which should not get a gap.
  CA11yState & state = State (doc);
  if (bReading)
    SeedRouter (doc, state, true);     // this line is in the list by now, and is about to be said
  if (!bReading && !state.bRouterSeeded && !state.bPaneShown)
    return;

  const std::string text = A11y_ToUtf8 (doc, sText);
  if (state.bRouterSeeded)
    {
    const bool bSpeak = bReading && (doc.IsWorldViewActive () || doc.m_bSpeechBackground) &&
                        !OtherSpeechPlugin (doc);
    state.router.line (text, CategoryForFlags (iType), bSpeak);
    }
  state.strPartial.clear ();

  if (state.bPaneShown && doc.m_pWorldView)
    doc.m_pWorldView->accessibleLine (text.c_str ());
  }

void A11y_Tick (CMUSHclientDoc & doc)
  {
  A11y_TickAt (doc, NowMs ());
  }

void A11y_TickAt (CMUSHclientDoc & doc, const long long now)
  {
  PollPlatform (now);

  // Anyone not using any of this stops here.
  const bool bReading = Reading (doc);
  if (!doc.m_pA11y && !bReading && App.m_iA11yTextPane != eTextPaneAlways)
    return;

  CA11yState & state = State (doc);
  a11y::SpeechRouter & router = state.router;

  if (bReading && !g_app.bAutoReading)
    state.bExplicitVoice = true;
  if (bReading)
    SeedRouter (doc, state);
  router.setBackend (BackendFor (state));
  SyncSettings (doc, state);

  // The text pane
  const bool bWantPane = App.m_iA11yTextPane == eTextPaneAlways ||
                        (App.m_iA11yTextPane == eTextPaneWithReader && bReading);
  if (bWantPane != state.bPaneShown && doc.m_pWorldView)
    {
    state.bPaneShown = bWantPane;
    doc.m_pWorldView->accessiblePaneShow (bWantPane);
    state.bPaneSeeded = false;
    if (bWantPane)
      SeedPane (doc, state);
    }

  if (state.bPaneShown && doc.m_pWorldView)
    doc.m_pWorldView->accessibleFlush ();

  // Hold what is arriving while they are reading something else.
  bool bHold = false;
  if (doc.m_bSpeechHoldWhileReviewing)
    bHold = router.review ().reviewing () ||
            (state.bPaneShown && doc.m_pWorldView && doc.m_pWorldView->accessiblePaneFocused ());
  router.setHold (bHold);

  // A prompt has no newline, and the login screen is mostly prompts.
  if (bReading && doc.m_pCurrentLine && doc.m_pCurrentLine->len > 0 &&
      !doc.m_pCurrentLine->hard_return &&
      (doc.IsWorldViewActive () || doc.m_bSpeechBackground))
    {
    const std::string strPartial = PartialLine (doc);
    if (strPartial != state.strPartial)
      {
      state.strPartial = strPartial;
      state.iPartialSince = now;
      }
    else if (now - state.iPartialSince >= kPromptQuietMs)
      router.prompt (A11y_ToUtf8 (doc, strPartial.c_str ()));
    }

  router.tick (now);
  }

void A11y_CommandEntered (CMUSHclientDoc & doc)
  {
  if (doc.m_pA11y && doc.m_bSpeechInterruptOnEnter)
    doc.m_pA11y->router.commandEntered ();
  }

// ---------------------------------------------------------------- send to speech

void A11y_SendToSpeech (CMUSHclientDoc & doc, const char * sText)
  {
  const char * sCategory = a11y::kCategoryScript;
  switch (doc.m_iCurrentActionSource)
    {
    case eTriggerFired: sCategory = a11y::kCategoryTrigger; break;
    case eTimerFired:   sCategory = a11y::kCategoryTimer;   break;
    case eUserTyping:   sCategory = a11y::kCategoryAlias;   break;
    default: break;
    }

  CA11yState & state = State (doc);
  state.bExplicitVoice = true;
  a11y::SpeechRouter & router = state.router;
  router.setBackend (BackendFor (state));
  router.say (A11y_ToUtf8 (doc, sText), sCategory, router.category (sCategory).priority, false);
  }

// ---------------------------------------------------------------- the commands

bool A11y_Command (CMUSHclientDoc & doc, const char * sCommand)
  {
  // Only for someone using speech: to anyone else these are words to send to the MUD.  An
  // alias for one of them has already had its turn (EvaluateCommand), and won.
  if (!KeysLive (doc))
    return false;

  const std::string command = Trim (sCommand);
  const std::string lower = Lower (command);
  const std::string verb = lower.substr (0, lower.find (' '));
  if (verb != "tts" && verb != "tts_note" && verb != "tts_interrupt" && verb != "tts_stop" &&
      verb != "subst" && verb != "mushreader")
    return false;
  if (verb == "tts" && command.size () != 3)
    return false;

  CA11yState & state = State (doc);
  a11y::SpeechRouter & router = state.router;
  state.bExplicitVoice = true;
  router.setBackend (BackendFor (state));

  const std::string rest = Trim (command.substr (verb.size ()));

  if (verb == "tts")
    ToggleMute (doc, state);
  else if (verb == "tts_stop")
    router.stop ();
  else if (verb == "tts_note")
    router.say (A11y_ToUtf8 (doc, rest.c_str ()), a11y::kCategoryScript, pal::SpeechPriority::Normal, false);
  else if (verb == "tts_interrupt")
    router.say (A11y_ToUtf8 (doc, rest.c_str ()), a11y::kCategoryScript, pal::SpeechPriority::Normal, true);
  else if (verb == "subst")
    {
    SubstCommand (doc, state, rest);
    }
  else if (lower == "mushreader help" || lower == "mushreader:help" || lower == "mushreader")
    {
    doc.Note ("MUSHclient reads aloud by itself: see docs/ACCESSIBILITY.md.");
    doc.Note ("Commands: tts, tts_note <text>, tts_interrupt <text>, tts_stop.");
    doc.Note ("Substitutions: subst on|off|list|clear|add <text>==<replacement>|remove <text>|save.");
    }
  else
    return false;

  return true;
  }

// ---------------------------------------------------------------- the keys

// All Alt+Shift, which nothing else in the client uses, and only while reading, so that
// nobody else loses anything.  Ctrl+Shift+F12 is what MushReader's shim uses to mute.  A
// key the world or a plugin has put an accelerator on is theirs.
bool A11y_Key (CMUSHclientDoc & doc, const A11yKey key, const int ch,
               const bool bCtrl, const bool bAlt, const bool bShift)
  {
  const bool bLive = KeysLive (doc);
  const bool bPane = doc.m_pA11y && doc.m_pA11y->bPaneShown && doc.m_pWorldView;
  if (!bLive && !bPane)
    return false;
  if (WorldHasAccelerator (doc, key, ch, bCtrl, bAlt, bShift))
    return false;

  // Between the command box and the text pane, wherever there is one.  F6 is a macro key,
  // so that it is only this if the world has not given it a job.
  if (key == A11yKey::F6 && !bCtrl && !bAlt && !bShift && bPane && doc.m_macros [MAC_F6].IsEmpty ())
    {
    doc.m_pWorldView->accessibleFocusPane (!doc.m_pWorldView->accessiblePaneFocused ());
    return true;
    }

  if (!bLive)
    return false;

  CA11yState & state = State (doc);
  a11y::SpeechRouter & router = state.router;
  router.setBackend (BackendFor (state));

  if (bCtrl && bShift && !bAlt && key == A11yKey::F12)
    {
    ToggleMute (doc, state);
    return true;
    }

  if (!bAlt || !bShift || bCtrl)
    return false;

  const char * sAction = nullptr;
  char sRecent [12];
  switch (key)
    {
    case A11yKey::Up:       sAction = "previous_line"; break;
    case A11yKey::Down:     sAction = "next_line"; break;
    case A11yKey::Left:     sAction = "previous_word"; break;
    case A11yKey::Right:    sAction = "next_word"; break;
    case A11yKey::Home:     sAction = "first_line"; break;
    case A11yKey::End:      sAction = "last_line"; break;
    case A11yKey::PageUp:   sAction = "previous_category"; break;
    case A11yKey::PageDown: sAction = "next_category"; break;
    case A11yKey::Return:   sAction = "activate_hotspot"; break;
    case A11yKey::Character:
      switch (ch)
        {
        case ',': sAction = "previous_char"; break;
        case '.': sAction = "next_char"; break;
        case 'L': sAction = "current_line"; break;
        case 'W': sAction = "current_word"; break;
        case 'S': sAction = "spell_word"; break;
        case 'K': sAction = "skip_to_latest"; break;
        case 'H': sAction = "next_hotspot"; break;
        case 'J': sAction = "previous_hotspot"; break;
        case 'M': sAction = "toggle_mute"; break;
        case 'O': sAction = "toggle_pane"; break;
        default:
          if (ch >= '0' && ch <= '9')
            {
            snprintf (sRecent, sizeof sRecent, "recent_%d", ch == '0' ? 10 : ch - '0');
            sAction = sRecent;
            }
          break;
        }
      break;
    default:
      break;
    }

  return sAction && ReviewAction (doc, state, sAction);
  }
// ---------------------------------------------------------------- hotspots

std::vector<A11yHotspot> A11y_ListHotspots (CMUSHclientDoc & doc)
  {
  std::vector<A11yHotspot> hotspots;
  for (const auto & window : doc.m_MiniWindowsOrder)
    {
    CMiniWindow * mw = window.second;
    if (!mw || !mw->GetShow () || mw->m_bTemporarilyHide)
      continue;
    for (const auto & it : mw->m_Hotspots)
      {
      const CHotspot * pHotspot = it.second;
      if (pHotspot->m_sMouseDown.empty () && pHotspot->m_sMouseUp.empty ())
        continue;   // nothing happens if you click on it

      A11yHotspot hotspot;
      hotspot.window = window.first;
      hotspot.id = it.first;

      // Tooltips are "title<tab>body".
      hotspot.label = pHotspot->m_sTooltipText;
      const size_t tab = hotspot.label.find ('\t');
      if (tab != std::string::npos)
        hotspot.label.replace (tab, 1, ": ");
      if (hotspot.label.empty ())
        hotspot.label = pHotspot->m_sLabel;
      if (hotspot.label.empty ())
        hotspot.label = mw->TextInRect (pHotspot->m_rect);
      if (hotspot.label.empty ())
        hotspot.label = hotspot.id;
      hotspot.label = A11y_ToUtf8 (doc, hotspot.label.c_str ());
      hotspots.push_back (hotspot);
      }
    }
  return hotspots;
  }

namespace {
struct NoMouseHooks : MiniWindowMouseHooks {};
}

bool A11y_ActivateHotspot (CMUSHclientDoc & doc, const A11yHotspot & hotspot)
  {
  NoMouseHooks hooks;

  // Any of these callbacks can delete the window, or the hotspot.  So each goes back
  // to the document to see if there is still something there.
  const auto fire = [&] (string CHotspot::* pRoutine, DISPID CHotspot::* pDispid, const long iFlags)
    {
    const auto mwit = doc.m_MiniWindows.find (hotspot.window);
    if (mwit == doc.m_MiniWindows.end ())
      return false;
    CMiniWindow * mw = mwit->second;
    const auto hit = mw->m_Hotspots.find (hotspot.id);
    if (hit == mw->m_Hotspots.end ())
      return false;
    CHotspot * pHotspot = hit->second;
    const string sPlugin = mw->m_sCallbackPlugin;
    const string sRoutine = pHotspot->*pRoutine;
    mw->m_bExecutingScript = true;
    SendMouseEventToPlugin (&doc, hooks, pHotspot->*pDispid, sPlugin, sRoutine, hotspot.id, iFlags, true);
    const auto again = doc.m_MiniWindows.find (hotspot.window);
    if (again != doc.m_MiniWindows.end ())
      again->second->m_bExecutingScript = false;
    return true;
    };

  // What a mouse would do on its way through.
  fire (&CHotspot::m_sMouseOver, &CHotspot::m_dispid_MouseOver, 0);
  fire (&CHotspot::m_sMouseDown, &CHotspot::m_dispid_MouseDown, MW_MOUSE_LH);
  fire (&CHotspot::m_sMouseUp, &CHotspot::m_dispid_MouseUp, MW_MOUSE_LH);
  return fire (&CHotspot::m_sCancelMouseOver, &CHotspot::m_dispid_CancelMouseOver, 0);
  }

// ---------------------------------------------------------------- world methods

// world.Speak (text [, category [, priority [, interrupt]]])
// priority: 0 polite, 1 normal, 2 assertive, -1 whatever the category says.
long CMUSHclientDoc::Speak (LPCTSTR Text, LPCTSTR Category, short Priority, BOOL Interrupt)
  {
  if (Priority < -1 || Priority > 2)
    return eBadParameter;

  CA11yState & state = State (*this);
  state.bExplicitVoice = true;
  a11y::SpeechRouter & router = state.router;
  router.setBackend (BackendFor (state));

  const std::string category = Category [0] ? Category : a11y::kCategoryScript;
  const pal::SpeechPriority priority =
    Priority == -1 ? router.category (category).priority :
    Priority == 0  ? pal::SpeechPriority::Polite :
    Priority == 1  ? pal::SpeechPriority::Normal : pal::SpeechPriority::Assertive;
  router.say (A11y_ToUtf8 (*this, Text), category, priority, Interrupt != FALSE);
  return eOK;
  }

// world.SpeechReview (action): what the review keys do.  docs/ACCESSIBILITY.md lists them.
long CMUSHclientDoc::SpeechReview (LPCTSTR Action)
  {
  CA11yState & state = State (*this);
  state.bExplicitVoice = true;
  state.router.setBackend (BackendFor (state));
  return ReviewAction (*this, state, Action) ? eOK : eBadParameter;
  }

long CMUSHclientDoc::StopSpeech ()
  {
  if (m_pA11y)
    m_pA11y->router.stop ();
  return eOK;
  }

// world.SpeechCategory (name, option, value): enabled, priority, cue, cue_volume, cue_pan.
long CMUSHclientDoc::SpeechCategory (LPCTSTR Name, LPCTSTR Option, LPCTSTR Value)
  {
  if (!Name [0] || strpbrk (Name, ";\r\n") || strpbrk (Value, ";\r\n"))
    return eBadParameter;

  CA11yState & state = State (*this);
  const auto existing = state.router.categories ().find (Name);
  const a11y::CategorySettings before = existing == state.router.categories ().end () ?
                                        a11y::CategorySettings () : existing->second;
  a11y::CategorySettings & settings = state.router.category (Name);
  const std::string option = Lower (Option);
  if (option == "enabled")
    settings.enabled = atoi (Value) != 0;
  else if (option == "priority")
    {
    const int i = atoi (Value);
    if (i < 0 || i > 2)
      return eOptionOutOfRange;
    settings.priority = i == 0 ? pal::SpeechPriority::Polite :
                        i == 1 ? pal::SpeechPriority::Normal : pal::SpeechPriority::Assertive;
    }
  else if (option == "cue")
    settings.cue = Value;
  else if (option == "cue_volume")
    settings.cueVolume = atof (Value);
  else if (option == "cue_pan")
    settings.cuePan = atof (Value);
  else
    return eUnknownOption;

  // A soundpack sets its categories every time it loads: the world has not changed.
  if (existing != state.router.categories ().end () && SameSettings (before, settings))
    return eOK;
  SaveCategories (*this, state);
  return eOK;
  }

VARIANT CMUSHclientDoc::SpeechInfo (short InfoType)
  {
  VARIANT vaResult;
  VariantInit (&vaResult);

  a11y::SpeechRouter * pRouter = m_pA11y ? &m_pA11y->router : nullptr;
  pal::ISpeech * pBackend = pRouter ? pRouter->backend () : g_app.pReader;

  switch (InfoType)
    {
    case  1: SetUpVariantString (vaResult, g_app.pReader ? g_app.pReader->name () : ""); break;
    case  2: SetUpVariantString (vaResult, pBackend ? pBackend->name () : ""); break;
    case  3: SetUpVariantBool   (vaResult, g_app.bAutoReading); break;
    case  4: SetUpVariantLong   (vaResult, pRouter ? (long) pRouter->pendingCount () : 0); break;
    case  5: SetUpVariantBool   (vaResult, pBackend && pBackend->isBusy ()); break;
    case  6: SetUpVariantBool   (vaResult, m_bSpeechMute != 0); break;
    case  7: SetUpVariantLong   (vaResult, pBackend ? (long) pBackend->capabilities () : 0); break;
    case  8: SetUpVariantLong   (vaResult, App.m_iSpeechMode); break;
    case  9: SetUpVariantLong   (vaResult, pRouter ? (long) pRouter->linesSpoken () : 0); break;
    case 10: SetUpVariantString (vaResult, pRouter ? pRouter->lastSpoken ().c_str () : ""); break;
    case 11: SetUpVariantLong   (vaResult, pRouter ? (long) pRouter->linesSkipped () : 0); break;
    case 12:
      {
      std::string voices;
      if (pal::g_speechPlatform)
        for (const std::string & voice : pal::g_speechPlatform->ownVoiceNames ())
          voices += (voices.empty () ? "" : "|") + voice;
      SetUpVariantString (vaResult, voices.c_str ());
      }
      break;
    case 13: SetUpVariantBool   (vaResult, m_pA11y && m_pA11y->bPaneShown); break;
    case 14: SetUpVariantBool   (vaResult, pRouter && pRouter->review ().reviewing ()); break;
    default:
      vaResult.vt = VT_NULL;
      break;
    }
  return vaResult;
  }

// world.WindowHotspotLabel (window, hotspot, label): what the hotspot list says for it.
long CMUSHclientDoc::WindowHotspotLabel (LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR Label)
  {
  const auto mwit = m_MiniWindows.find (Name);
  if (mwit == m_MiniWindows.end ())
    return eNoSuchWindow;
  const auto hit = mwit->second->m_Hotspots.find (HotspotId);
  if (hit == mwit->second->m_Hotspots.end ())
    return eHotspotNotInstalled;
  hit->second->m_sLabel = Label;
  return eOK;
  }

// world.AccessibleHotspots (): every hotspot the keyboard can reach, a line each, as
// window<tab>id<tab>label.
BSTR CMUSHclientDoc::AccessibleHotspots ()
  {
  std::string out;
  for (const A11yHotspot & hotspot : A11y_ListHotspots (*this))
    out += hotspot.window + "\t" + hotspot.id + "\t" + hotspot.label + "\n";
  return CString (out.c_str ()).AllocSysString ();
  }

// world.ActivateHotspot (window, hotspot): as if clicked.
long CMUSHclientDoc::ActivateHotspot (LPCTSTR Name, LPCTSTR HotspotId)
  {
  const auto mwit = m_MiniWindows.find (Name);
  if (mwit == m_MiniWindows.end ())
    return eNoSuchWindow;
  if (mwit->second->m_Hotspots.find (HotspotId) == mwit->second->m_Hotspots.end ())
    return eHotspotNotInstalled;
  A11yHotspot hotspot;
  hotspot.window = Name;
  hotspot.id = HotspotId;
  A11y_ActivateHotspot (*this, hotspot);
  return eOK;
  }
