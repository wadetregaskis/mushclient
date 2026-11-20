// wx_osx_speech.mm -- pal::ISpeech on macOS.  UNCOMPILED where it was written: there is no
// Mac on the machine this came from.  Kept small for that reason.
//
//   VoiceOver   NSAccessibilityAnnouncementRequestedNotification, posted to the key window
//               (posted to NSApp it is dropped).  There is no callback for when it has
//               finished, and under load VoiceOver can skip to the latest, so pacing is
//               the router's estimate.
//   System      AVSpeechSynthesizer, which knows when it is speaking, so pacing is exact.

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>

#include "pal/ispeech.h"
#include "wx_a11y.h"

#include <wx/window.h>

namespace {

NSString * ToNS (const char * utf8)
  {
  NSString * s = [NSString stringWithUTF8String:utf8];
  return s ? s : @"";
  }

class VoiceOverSpeech : public pal::ISpeech
  {
  public:
    unsigned capabilities () const override { return pal::kSpeechScreenReader; }
    const char * name () const override { return "VoiceOver"; }

    bool speak (const char * utf8, pal::SpeechPriority priority, bool) override
      {
      NSWindow * window = [NSApp keyWindow] ?: [NSApp mainWindow];
      if (!window)
        return false;
      const NSInteger level = priority == pal::SpeechPriority::Assertive ? NSAccessibilityPriorityHigh :
                              priority == pal::SpeechPriority::Polite    ? NSAccessibilityPriorityLow :
                                                                           NSAccessibilityPriorityMedium;
      NSAccessibilityPostNotificationWithUserInfo (window,
          NSAccessibilityAnnouncementRequestedNotification,
          @{ NSAccessibilityAnnouncementKey : ToNS (utf8),
             NSAccessibilityPriorityKey : @(level) });
      return true;
      }

    void stop () override {}    // an announcement cannot be called back
  };

class AvSpeech : public pal::ISpeech
  {
  public:
    AvSpeech () : m_synth ([[AVSpeechSynthesizer alloc] init]) {}

    unsigned capabilities () const override
      {
      return pal::kSpeechKnowsBusy | pal::kSpeechCanStop | pal::kSpeechVoices;
      }

    const char * name () const override { return "AVSpeechSynthesizer"; }

    bool speak (const char * utf8, pal::SpeechPriority, bool bFlush) override
      {
      if (bFlush)
        [m_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
      AVSpeechUtterance * u = [AVSpeechUtterance speechUtteranceWithString:ToNS (utf8)];
      if (m_voice)
        u.voice = m_voice;
      // -10 .. 10 about the default
      const float span = m_rate >= 0 ? AVSpeechUtteranceMaximumSpeechRate - AVSpeechUtteranceDefaultSpeechRate
                                     : AVSpeechUtteranceDefaultSpeechRate - AVSpeechUtteranceMinimumSpeechRate;
      u.rate = AVSpeechUtteranceDefaultSpeechRate + span * m_rate / 10.0f;
      u.volume = m_volume / 100.0f;
      if ([u respondsToSelector:@selector(setPrefersAssistiveTechnologySettings:)])
        u.prefersAssistiveTechnologySettings = YES;
      [m_synth speakUtterance:u];
      return true;
      }

    void stop () override { [m_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate]; }
    bool isBusy () override { return m_synth.speaking; }
    void setRate (int rate) override { m_rate = rate; }
    void setVolume (int volume) override { m_volume = volume; }

    void setVoice (const char * sName) override
      {
      m_voice = nil;
      NSString * wanted = ToNS (sName);
      for (AVSpeechSynthesisVoice * voice in [AVSpeechSynthesisVoice speechVoices])
        if ([voice.name isEqualToString:wanted])
          m_voice = voice;
      }

    std::vector<std::string> voices () override
      {
      std::vector<std::string> names;
      for (AVSpeechSynthesisVoice * voice in [AVSpeechSynthesisVoice speechVoices])
        names.push_back ([voice.name UTF8String]);
      return names;
      }

  private:
    AVSpeechSynthesizer * m_synth;
    AVSpeechSynthesisVoice * m_voice = nil;
    int m_rate = 0;
    int m_volume = 100;
  };

class MacSpeechPlatform : public pal::ISpeechPlatform
  {
  public:
    pal::ISpeech * screenReader () override
      {
      return [[NSWorkspace sharedWorkspace] isVoiceOverEnabled] ? &m_voiceOver : nullptr;
      }

    pal::ISpeech * ownVoice () override
      {
      if (!m_av)
        m_av = new AvSpeech;
      return m_av;
      }

  private:
    VoiceOverSpeech m_voiceOver;
    AvSpeech * m_av = nullptr;
  };

}  // namespace

void pal::InstallPlatformSpeech ()
  {
  static MacSpeechPlatform platform;
  pal::g_speechPlatform = &platform;
  }

// A native control's own name for VoiceOver.  wx's labels and fields are unrelated siblings.
void McSetAccessibleName (wxWindow * window, const char * utf8Name)
  {
  if (!window)
    return;
  id peer = (id) window->GetHandle ();
  if ([peer respondsToSelector:@selector(setAccessibilityLabel:)])
    [peer setAccessibilityLabel:ToNS (utf8Name)];
  }
