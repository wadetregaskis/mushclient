// pal/ispeech.h -- the speech seam of the Platform Abstraction Layer.
//
// ISpeech is somewhere text can go to be spoken: the player's own screen reader, or
// the system's voice.  ISpeechPlatform says which of those exist right now.  The
// engine (src/speech_router.*) decides what to say and when; nothing here does.
//
// Implemented by: pal/win_speech.cpp (NVDA, JAWS, UI Automation, SAPI),
// platform/wx_osx_speech.mm (VoiceOver announcements, AVSpeechSynthesizer),
// platform/linux_speech.cpp (ATK notifications, speech-dispatcher), and a recording
// fake in tests/test_speech_router.cpp.  See docs/ACCESSIBILITY.md.
//
// No toolkit types and nothing newer than C++14, like the rest of pal/: MSVC compiles
// this at /std:c++14.  Text is NUL-terminated UTF-8, whatever the world's encoding is.

#pragma once

#include <string>
#include <vector>

namespace pal {

enum class SpeechPriority
  {
  Polite,      // when the reader has finished what it is saying
  Normal,
  Assertive    // the reader may put it ahead of what is queued
  };

enum SpeechCaps
  {
  kSpeechKnowsBusy    = 1 << 0,   // isBusy tells the truth, so delivery can be paced exactly
  kSpeechCanStop      = 1 << 1,   // stop really silences it
  kSpeechBraille      = 1 << 2,   // braille() reaches a display
  kSpeechScreenReader = 1 << 3,   // the player's own reader, with its own voice and settings
  kSpeechVoices       = 1 << 4    // setRate, setVolume and setVoice do something
  };

class ISpeech
  {
  public:
    virtual ~ISpeech () {}

    virtual unsigned    capabilities () const = 0;
    virtual const char * name () const = 0;      // "NVDA", "SAPI", ...

    // flush: silence whatever is being said first, if this backend can.  False if
    // the text did not get there, which is the reader having just gone away.
    virtual bool speak (const char * utf8, SpeechPriority priority, bool flush) = 0;
    virtual void stop () = 0;

    // Only meaningful with kSpeechKnowsBusy.
    virtual bool isBusy () { return false; }

    virtual void braille (const char * /*utf8*/) {}

    // For the system voice.  Rate is -10 .. +10 and 0 is normal, volume is 0 .. 100,
    // a voice is one of voices(), and "" is the system's default.
    virtual void setRate (int /*rate*/) {}
    virtual void setVolume (int /*volume*/) {}
    virtual void setVoice (const char * /*name*/) {}
    virtual std::vector<std::string> voices () { return std::vector<std::string> (); }
  };

class ISpeechPlatform
  {
  public:
    virtual ~ISpeechPlatform () {}

    // The screen reader that is running now, or NULL.  Called every few seconds, so it
    // must be cheap when there is no reader.  Owned by the platform.
    virtual ISpeech * screenReader () = 0;

    // The system's own voice, or NULL if there is none.  Only asked for when the
    // player wants speech and no reader can give it, so nothing is loaded otherwise.
    virtual ISpeech * ownVoice () = 0;
  };

// The one for this platform, or NULL (the headless build, and the tests until they
// install a fake).  Set once at start-up on the main thread.  All of this is main
// thread only.
extern ISpeechPlatform * g_speechPlatform;

// Installs the real one: each platform has its own definition.
void InstallPlatformSpeech ();

}  // namespace pal
