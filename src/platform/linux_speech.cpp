// linux_speech.cpp -- pal::ISpeech on Linux.  UNCOMPILED where it was written: there is no
// Linux on the machine this came from.  Kept small for that reason.
//
//   Orca     An ATK notification on the top-level window: "notification" (ATK 2.50), which
//            carries politeness and maps to AT-SPI Announcement, or "announcement" (2.46).
//            Older than that, nothing can be done through ATK, and speech-dispatcher is used.
//   System   speech-dispatcher, with dlopen so that it is not a build dependency, and with
//            nothing but its entry points: the callback for when speech ends needs the
//            layout of its struct, which cannot be checked from here.  So pacing is the
//            router's estimate, not exact.  Say so if the docs are wrong about that.
//
// ATK and GTK are looked up with dlsym: wxGTK has already loaded them, and this keeps
// their headers out of the build.

#include "pal/ispeech.h"
#include "wx_a11y.h"

#include <wx/app.h>
#include <wx/window.h>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

typedef void * (*GtkGetAccessible) (void *);
typedef unsigned (*AtkVersion) (void);
typedef void (*SignalEmitByName) (void *, const char *, ...);
typedef void (*AtkSetName) (void *, const char *);

// Is this process, if any, the player's own Orca?
bool IsOrca (const std::string & pid)
  {
  std::ifstream comm ("/proc/" + pid + "/comm");
  std::string name;
  return std::getline (comm, name) && name == "orca";
  }

// Asked every two seconds.  Orca found is remembered by its pid, which is one small file to
// look at; Orca not found is looked for again only every ten seconds, and only among this
// user's own processes, so that someone else's Orca on the machine does not count.
bool OrcaRunning ()
  {
  static std::string s_pid;
  static std::chrono::steady_clock::time_point s_lastScan;
  static bool s_bScanned = false;

  if (!s_pid.empty ())
    {
    if (IsOrca (s_pid))
      return true;
    s_pid.clear ();
    }

  const auto now = std::chrono::steady_clock::now ();
  if (s_bScanned && now - s_lastScan < std::chrono::seconds (10))
    return false;
  s_bScanned = true;
  s_lastScan = now;

  DIR * dir = opendir ("/proc");
  if (!dir)
    return false;
  const uid_t uid = getuid ();
  while (dirent * entry = readdir (dir))
    {
    if (entry->d_name [0] < '0' || entry->d_name [0] > '9')
      continue;
    struct stat st;
    const std::string path = std::string ("/proc/") + entry->d_name;
    if (stat (path.c_str (), &st) != 0 || st.st_uid != uid)
      continue;
    if (IsOrca (entry->d_name))
      {
      s_pid = entry->d_name;
      break;
      }
    }
  closedir (dir);
  return !s_pid.empty ();
  }

void * TopLevelAccessible ()
  {
  wxWindow * top = wxTheApp ? wxTheApp->GetTopWindow () : nullptr;
  GtkGetAccessible get = (GtkGetAccessible) dlsym (RTLD_DEFAULT, "gtk_widget_get_accessible");
  if (!top || !get)
    return nullptr;
  return get ((void *) top->GetHandle ());
  }

class AtkSpeech : public pal::ISpeech
  {
  public:
    // Which signal this ATK has: 0 for none.
    int Generation ()
      {
      AtkVersion major = (AtkVersion) dlsym (RTLD_DEFAULT, "atk_get_major_version");
      AtkVersion minor = (AtkVersion) dlsym (RTLD_DEFAULT, "atk_get_minor_version");
      if (!major || !minor || major () != 2)
        return major && major () > 2 ? 2 : 0;
      return minor () >= 50 ? 2 : minor () >= 46 ? 1 : 0;
      }

    unsigned capabilities () const override { return pal::kSpeechScreenReader; }
    const char * name () const override { return "Orca"; }

    bool speak (const char * utf8, pal::SpeechPriority priority, bool) override
      {
      SignalEmitByName emit = (SignalEmitByName) dlsym (RTLD_DEFAULT, "g_signal_emit_by_name");
      void * accessible = TopLevelAccessible ();
      if (!emit || !accessible)
        return false;
      if (Generation () == 2)
        // ATK_LIVE_POLITE 1, ATK_LIVE_ASSERTIVE 2
        emit (accessible, "notification", utf8, priority == pal::SpeechPriority::Assertive ? 2 : 1);
      else
        emit (accessible, "announcement", utf8);
      return true;
      }

    void stop () override {}    // a notification cannot be called back
  };

// From libspeechd.h: SPD_MODE_THREADED is 1; SPDPriority is SPD_IMPORTANT 1, SPD_MESSAGE 2,
// SPD_TEXT 3, SPD_NOTIFICATION 4 and SPD_PROGRESS 5.  A notification is dropped whenever anything
// else is speaking, so it is never what an alert goes as: that is a message, which goes ahead of
// text and is not cut off by it.
const int kSpdMessage = 2;
const int kSpdText = 3;
typedef void * (*SpdOpen) (const char *, const char *, const char *, int);
typedef int (*SpdSay) (void *, int, const char *);
typedef int (*SpdCancel) (void *);
typedef int (*SpdSetInt) (void *, signed int);
typedef int (*SpdSetName) (void *, const char *);

class SpeechDispatcher : public pal::ISpeech
  {
  public:
    bool Load ()
      {
      if (m_bTried)
        return m_conn != nullptr;
      m_bTried = true;
      void * lib = dlopen ("libspeechd.so.2", RTLD_NOW | RTLD_LOCAL);
      if (!lib)
        return false;
      SpdOpen open = (SpdOpen) dlsym (lib, "spd_open");
      m_say = (SpdSay) dlsym (lib, "spd_say");
      m_cancel = (SpdCancel) dlsym (lib, "spd_cancel");
      m_rate = (SpdSetInt) dlsym (lib, "spd_set_voice_rate");
      m_volume = (SpdSetInt) dlsym (lib, "spd_set_volume");
      m_voice = (SpdSetName) dlsym (lib, "spd_set_synthesis_voice");
      if (open && m_say && m_cancel)
        m_conn = open ("MUSHclient", "main", nullptr, 1);
      return m_conn != nullptr;
      }

    unsigned capabilities () const override { return pal::kSpeechCanStop | pal::kSpeechVoices; }
    const char * name () const override { return "speech-dispatcher"; }

    bool speak (const char * utf8, pal::SpeechPriority priority, bool bFlush) override
      {
      if (bFlush)
        m_cancel (m_conn);
      return m_say (m_conn, priority == pal::SpeechPriority::Assertive ? kSpdMessage : kSpdText, utf8) >= 0;
      }

    void stop () override { m_cancel (m_conn); }
    void setRate (int rate) override { if (m_rate) m_rate (m_conn, rate * 10); }
    void setVolume (int volume) override { if (m_volume) m_volume (m_conn, volume * 2 - 100); }
    void setVoice (const char * sName) override { if (m_voice && sName [0]) m_voice (m_conn, sName); }

  private:
    bool m_bTried = false;
    void * m_conn = nullptr;
    SpdSay m_say = nullptr;
    SpdCancel m_cancel = nullptr;
    SpdSetInt m_rate = nullptr;
    SpdSetInt m_volume = nullptr;
    SpdSetName m_voice = nullptr;
  };

class LinuxSpeechPlatform : public pal::ISpeechPlatform
  {
  public:
    pal::ISpeech * screenReader () override
      {
      return m_atk.Generation () > 0 && OrcaRunning () ? &m_atk : nullptr;
      }

    pal::ISpeech * ownVoice () override
      {
      return m_spd.Load () ? &m_spd : nullptr;
      }

    // speech-dispatcher's voices are not listed here (voices () is the default), so the
    // preferences need not open a connection to learn that.
    std::vector<std::string> ownVoiceNames () override { return std::vector<std::string> (); }

  private:
    AtkSpeech m_atk;
    SpeechDispatcher m_spd;
  };

}  // namespace

void pal::InstallPlatformSpeech ()
  {
  static LinuxSpeechPlatform platform;
  pal::g_speechPlatform = &platform;
  }

void McSetAccessibleName (wxWindow * window, const char * utf8Name)
  {
  GtkGetAccessible get = (GtkGetAccessible) dlsym (RTLD_DEFAULT, "gtk_widget_get_accessible");
  AtkSetName setName = (AtkSetName) dlsym (RTLD_DEFAULT, "atk_object_set_name");
  if (!window || !get || !setName)
    return;
  void * accessible = get ((void *) window->GetHandle ());
  if (accessible)
    setName (accessible, utf8Name);
  }
