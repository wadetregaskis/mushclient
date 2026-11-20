// a11y.h -- speech, review and the text pane, as far as the document is concerned.
//
// CMUSHclientDoc::m_pA11y owns a CA11yState, which is why it is here and not in doc.h,
// and which stays NULL for as long as nobody has asked for any of this: a player with no
// screen reader has a NULL pointer and one test of a bool, and hears and sees nothing new.
//
// docs/ACCESSIBILITY.md describes what a player or plugin author sees.

#pragma once

#include <string>
#include <vector>

#include "pal/isound.h"
#include "speech_router.h"

class CMUSHclientDoc;

// Global preference: SpeechMode.
enum
  {
  eSpeechOff = 0,      // nothing automatic, and nothing a script asks for
  eSpeechAuto = 1,     // through a screen reader if one is running, and otherwise silent
  eSpeechAlways = 2    // and with the system's voice if there is no reader
  };

// Global preference: A11yTextPane.
enum
  {
  eTextPaneNever = 0,
  eTextPaneWithReader = 1,   // whenever output is being read automatically
  eTextPaneAlways = 2
  };

class CA11yState
  {
  public:
    a11y::SpeechRouter router;
    pal::ISound * pSound = nullptr;  // for sound cues; NULL plays through the document

    bool bExplicitVoice = false;     // a script or trigger has asked to be heard
    bool bPaneShown = false;
    bool bPaneSeeded = false;
    bool bRouterSeeded = false;
    std::string strAppliedCategories;

    // A partial line going quiet is a prompt.
    std::string strPartial;
    long long iPartialSince = 0;

    // Hotspot list, in the order it was last read out.
    int iHotspot = -1;

    int iCategory = 0;   // which of the categories review is looking at; 0 is all
  };

// Toolkit-neutral keys, for review.
enum class A11yKey
  {
  Up, Down, Left, Right, Home, End, PageUp, PageDown, Return, F6, F12,
  Character     // ch says which: upper case letter, digit, or ',' and '.'
  };

// ---------------------------------------------------------------- hooks into the engine

// Every line that reaches the screen, from the same place plugins hear of it.  Restored
// scrollback does not come through here, so it is never spoken.
void A11y_Screendraw (CMUSHclientDoc & doc, long iType, const char * sText);

// Twenty-odd times a second, per world.
void A11y_Tick (CMUSHclientDoc & doc);
void A11y_TickAt (CMUSHclientDoc & doc, long long nowMs);    // for tests

// The player pressed Enter in the command box.
void A11y_CommandEntered (CMUSHclientDoc & doc);

// A command about to be evaluated: tts, subst and the rest.  True if it was one.
bool A11y_Command (CMUSHclientDoc & doc, const char * sCommand);

// A key pressed in the command box or the text pane.  True if it was ours.
bool A11y_Key (CMUSHclientDoc & doc, A11yKey key, int ch, bool bCtrl, bool bAlt, bool bShift);

// Send To: Speech.
void A11y_SendToSpeech (CMUSHclientDoc & doc, const char * sText);

// Is automatic reading on at all (a reader is running, or the player asked for it)?
bool A11y_AutoReading ();

// Text in a world's bytes, as UTF-8.
std::string A11y_ToUtf8 (const CMUSHclientDoc & doc, const char * sText);

// ---------------------------------------------------------------- hotspots

struct A11yHotspot
  {
  std::string window;
  std::string id;
  std::string label;
  };

std::vector<A11yHotspot> A11y_ListHotspots (CMUSHclientDoc & doc);
bool A11y_ActivateHotspot (CMUSHclientDoc & doc, const A11yHotspot & hotspot);

// ---------------------------------------------------------------- plugins

// If this XML is a plugin that is shadowed by ID, what to parse instead, so that what runs is ours.  True if it
// did.  See a11y_shims.cpp.
bool A11y_ShadowPlugin (const char * sXml, std::string & strReplacement);

extern const char * const kA11yMushReaderID;
extern const char * const kA11yLuaAudioID;

// ---------------------------------------------------------------- tests

void A11y_ResetForTest ();
