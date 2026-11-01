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
// True with *pathOut naming a file whose signature and hash have been checked.
bool RunDownload (const Fetcher & fetcher, const Downloader & downloader,
                  const Release & release, const ThisCopy & copy, const std::string & publicKeyHex,
                  const std::string & folder, const Progress & progress,
                  std::string * pathOut, std::string * error);

// What's special about a copy when it comes to updating, as far as the frontends need:
// whether the dialog can install at all, or must send you to the page, and why.
struct InstallPlan
  {
  bool canInstall = false;
  std::string why;              // when it can't, in a sentence
  };

InstallPlan PlanInstall (const Release & release, const ThisCopy & copy, const std::string & publicKeyHex);

class Controller
  {
  public:
  // Runs a function on the UI thread, some time soon.  Called from the worker.
  typedef std::function<void (std::function<void ()>)> Poster;

  static Controller & Get ();

  void Configure (Poster poster, const std::string & installedVersion);

  // For tests.  Null restores the real ones.
  void SetNetwork (Fetcher fetcher, Downloader downloader);

  // From a timer, on the UI thread.  Starts a check if one is due: a short while after
  // starting, then about daily.
  void Tick (time_t now, bool bEnabled);

  // Help > Check for Updates.  Works with the option off.
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

  // Download on a worker; `done` runs on the UI thread with the path or the error.  At
  // most one at a time.  progress runs on the worker, and false from it cancels.
  void StartDownload (const Release & release, const ThisCopy & copy, const std::string & publicKeyHex,
                      const std::string & folder, Progress progress,
                      std::function<void (bool bOK, const std::string & pathOrError)> done);
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
  bool        m_bChecking = false;
  bool        m_bDismissed = false;
  std::vector<Release> m_newer;
  std::function<void (bool, const CheckResult &)> m_onChecked;
  std::atomic<bool> m_bCancel { false };
  std::atomic<bool> m_bDownloading { false };
  };

} // namespace update
