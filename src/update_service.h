// update_service.h -- checking for a new version, and fetching it, off the UI thread.
//
// update_logic.h decides things; this does them on a worker and tells the UI thread.
// There is one Controller, which both frontends drive the same way: Configure it once,
// Tick it now and then, and show what it says.  Nothing is sent to GitHub but the
// request itself and a User-Agent naming the client and its version.
//
// The network is reached through two function objects, so tests run all of this
// against fakes.

#pragma once

#include "update_logic.h"

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace update {

extern const char * const kReleasesApiUrl;
extern const char * const kReleasesPageUrl;

// Set this in the environment to a file holding a JSON release list, and that is what
// a check reads instead of GitHub.  It decides only what is offered, and not what gets
// installed, which is still signed or nothing.
extern const char * const kFeedOverrideVariable;

// How the worker reaches the network.  Fetch: true if a response came back, whatever
// its status.  Download: true if it was all saved; false, with why, otherwise,
// including when `progress` returns false.
typedef std::function<bool (const std::string & url, int * status, std::string * body, std::string * error)> Fetcher;
typedef std::function<bool (uint64_t done, uint64_t total)> Progress;
typedef std::function<bool (const std::string & url, const std::string & path,
                            const Progress & progress, std::string * error)> Downloader;

// The real ones: pal::HttpsGet and pal::HttpsDownload.
Fetcher    DefaultFetcher ();
Downloader DefaultDownloader ();

struct CheckResult
  {
  bool ok = false;
  std::string error;
  std::vector<Release> newer;     // newest first; empty means up to date
  };

CheckResult RunCheck (const Fetcher & fetcher, const std::string & installed);

// Everything between a click and a file ready to install, on the calling thread.
// True with *pathOut naming a file whose signature and hash have been checked, and
// *sha256Out (if wanted) that hash, so that whatever installs it can check it again as
// it reads it.  Nothing much bigger than the release list says is downloaded.
bool RunDownload (const Fetcher & fetcher, const Downloader & downloader,
                  const Release & release, const ThisCopy & copy, const std::string & publicKeyHex,
                  const std::string & folder, const Progress & progress,
                  std::string * pathOut, std::string * error, std::string * sha256Out = nullptr);

// The most a download of this asset may be: its size in the release list, and a little.
uint64_t DownloadLimit (const Asset & asset);

// What's special about a copy when it comes to updating, as far as the frontends need:
// whether the dialog can install at all, or must send you to the page, and why.
struct InstallPlan
  {
  bool canInstall = false;
  std::string why;              // when it can't, in a sentence
  };

InstallPlan PlanInstall (const Release & release, const ThisCopy & copy, const std::string & publicKeyHex);

// What the update dialog offers.  The newest release, unless it has nothing this copy
// can install and an older one (still newer than this) has: then that one, and a note
// saying why.  `newer` is newest first, as CheckResult has it, and not empty.
struct Offer
  {
  Release     release;
  InstallPlan plan;
  std::string note;            // "" when it is the newest
  };

Offer ChooseOffer (const std::vector<Release> & newer, const ThisCopy & copy, const std::string & publicKeyHex);

// The page to send someone to for a release: its own, if that is on GitHub, or the list
// of them.  (The release list is GitHub's, but this goes to the shell, so it is checked.)
std::string ReleasePageFor (const Release & release);

class Controller
  {
  public:
  // Runs a function on the UI thread, some time soon.  Called from the worker.  False
  // if it couldn't be sent (the window has gone, or its queue is full), and then it
  // won't run.
  typedef std::function<bool (std::function<void ()>)> Poster;

  static Controller & Get ();

  void Configure (Poster poster, const std::string & installedVersion);

  // For tests.  Null restores the real ones.
  void SetNetwork (Fetcher fetcher, Downloader downloader);

  // From a timer, on the UI thread.  Starts a check if one is due: a short while after
  // starting, then about daily, or an hour after one that failed.
  void Tick (time_t now, bool bEnabled);

  // Help > Check for Updates.  Works with the option off.  If a check is already under
  // way, its result is the answer, and is reported as asked for.
  void CheckNow (bool bUserAsked);

  // Told on the UI thread when a check finishes.
  void SetOnChecked (std::function<void (bool bUserAsked, const CheckResult & result)> handler);

  bool IsChecking () const;

  // What the last good check found.
  bool HasUpdate () const { return !m_newer.empty (); }
  const std::vector<Release> & Newer () const { return m_newer; }
  std::string LatestVersion () const { return m_newer.empty () ? std::string () : m_newer [0].version; }
  std::string InstalledVersion () const { return m_installed; }

  // "Update available: 6.10" when the badge should show, otherwise "".
  std::string BadgeText () const;
  // Remind Me Later: no badge until the next check says so again.
  void DismissBadge () { m_bDismissed = true; }

  // Download on a worker; `done` runs on the UI thread with the path and its SHA-256, or
  // the error.  At most one at a time: asked for another meanwhile, `done` is told at
  // once that one is under way.  progress runs on the worker, and false from it cancels.
  typedef std::function<void (bool bOK, const std::string & pathOrError, const std::string & sha256)> DownloadDone;
  void StartDownload (const Release & release, const ThisCopy & copy, const std::string & publicKeyHex,
                      const std::string & folder, Progress progress, DownloadDone done);
  void CancelDownload () { m_bCancel = true; }
  bool IsDownloading () const { return m_bDownloading; }

  private:
  Controller () {}
  void Spawn (std::function<void ()> work);

  Poster      m_poster;
  std::string m_installed;
  Fetcher     m_fetcher;
  Downloader  m_downloader;
  time_t      m_nextCheck = 0;
  time_t      m_lastTick = 0;
  bool        m_bDismissed = false;
  std::vector<Release> m_newer;
  std::function<void (bool, const CheckResult &)> m_onChecked;
  // Atomic because the worker clears them when it can't post its result.
  std::atomic<bool> m_bChecking { false };
  std::atomic<bool> m_bUserWaiting { false };     // someone asked while a check was running
  std::atomic<bool> m_bCancel { false };
  std::atomic<bool> m_bDownloading { false };
  };

} // namespace update
