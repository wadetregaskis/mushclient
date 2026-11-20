// update_service.cpp -- see update_service.h.

#include "stdafx.h"
#include "update_service.h"
#include "pal/https_get.h"
#include "version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace update {

const char * const kReleasesApiUrl = "https://api.github.com/repos/wadetregaskis/mushclient/releases?per_page=100";
const char * const kReleasesPageUrl = "https://github.com/wadetregaskis/mushclient/releases";
const char * const kFeedOverrideVariable = "MUSHCLIENT_UPDATE_FEED";

namespace {

const int kCheckTimeoutSeconds = 20;
const int kFirstCheckAfterSeconds = 30;
const int kCheckEverySeconds = 24 * 60 * 60;
const int kRetryAfterFailureSeconds = 60 * 60;

// When the release list doesn't give a size.  The packages are tens of megabytes.
const uint64_t kMaxUnsizedDownload = 1024ull * 1024 * 1024;

std::string UserAgent ()
  {
  return std::string ("MUSHclient/") + VERSION_STRING;
  }

} // anonymous namespace

Fetcher DefaultFetcher ()
  {
  return [] (const std::string & url, int * status, std::string * body, std::string * error)
    {
    return pal::HttpsGet (url, kCheckTimeoutSeconds, UserAgent (), status, body, error);
    };
  }

Downloader DefaultDownloader ()
  {
  return [] (const std::string & url, const std::string & path, const Progress & progress, std::string * error)
    {
    int status = 0;
    if (!pal::HttpsDownload (url, kCheckTimeoutSeconds, UserAgent (), path, progress, &status, error))
      return false;
    if (status != 200)
      {
      *error = "the server answered with status " + std::to_string (status);
      return false;
      }
    return true;
    };
  }

CheckResult RunCheck (const Fetcher & fetcher, const std::string & installed)
  {
  CheckResult result;
  std::string body;
  int status = 0;

  const char * feed = getenv (kFeedOverrideVariable);
  if (feed && *feed)
    {
    FILE * f = fopen (feed, "rb");
    if (!f)
      {
      result.error = std::string ("could not read ") + feed;
      return result;
      }
    char buffer [65536];
    size_t got;
    while ((got = fread (buffer, 1, sizeof buffer, f)) > 0)
      body.append (buffer, got);
    fclose (f);
    }
  else
    {
    std::string why;
    if (!fetcher (kReleasesApiUrl, &status, &body, &why))
      {
      result.error = why;
      return result;
      }
    // A 403 is usually the rate limit, and the body says so; ParseReleases passes that on.
    if (status != 200 && status != 403)
      {
      result.error = "GitHub answered with status " + std::to_string (status);
      return result;
      }
    }

  std::vector<Release> all;
  if (!ParseReleases (body, &all, &result.error))
    return result;
  result.newer = ReleasesNewerThan (all, installed);
  result.ok = true;
  return result;
  }

InstallPlan PlanInstall (const Release & release, const ThisCopy & copy, const std::string & publicKeyHex)
  {
  InstallPlan plan;
  if (copy.systemManaged)
    {
    plan.why = "This copy was installed by your system's package manager, so that is what should update it.";
    return plan;
    }
  if (copy.foreignFolder)
    {
    plan.why = "This copy isn't in a folder of its own, as its package unpacks to, so MUSHclient won't "
               "replace that folder itself.";
    return plan;
    }
  const std::string name = AssetNameFor (copy, release.version);
  if (name.empty ())
    {
    plan.why = "There is no package for this kind of copy.";
    return plan;
    }
  if (publicKeyHex.empty ())
    {
    plan.why = "This copy of MUSHclient has no key to check a download's signature with, "
               "so it won't install one itself.";
    return plan;
    }
  if (!CryptoAvailable ())
    {
    plan.why = "This build of MUSHclient has no way to check a download's signature, "
               "so it won't install one itself.";
    return plan;
    }
  if (!FindAsset (release, name))
    {
    plan.why = "This release has no package named " + name + ".";
    return plan;
    }
  if (!FindAsset (release, ManifestName (release.version)) || !FindAsset (release, SignatureName (release.version)))
    {
    plan.why = "This release isn't signed, so MUSHclient won't install it itself.";
    return plan;
    }
  plan.canInstall = true;
  return plan;
  }

Offer ChooseOffer (const std::vector<Release> & newer, const ThisCopy & copy, const std::string & publicKeyHex)
  {
  Offer offer;
  if (newer.empty ())
    return offer;
  for (size_t i = 0; i < newer.size (); i++)
    {
    const InstallPlan plan = PlanInstall (newer [i], copy, publicKeyHex);
    if (!plan.canInstall)
      continue;
    offer.release = newer [i];
    offer.plan = plan;
    if (i > 0)
      offer.note = newer [0].version + " has nothing this copy can install, so this is " + newer [i].version +
                   ", the latest that has.";
    return offer;
    }
  // None of them: the newest, and why not.
  offer.release = newer [0];
  offer.plan = PlanInstall (newer [0], copy, publicKeyHex);
  return offer;
  }

std::string ReleasePageFor (const Release & release)
  {
  static const std::string kGitHub = "https://github.com/";
  const std::string & url = release.pageUrl;
  bool bGood = url.size () > kGitHub.size () && url.compare (0, kGitHub.size (), kGitHub) == 0;
  for (const unsigned char c : url)
    if (c <= ' ' || c >= 0x7F || strchr ("\"<>\\^`{|}", c))
      bGood = false;
  return bGood ? url : std::string (kReleasesPageUrl);
  }

uint64_t DownloadLimit (const Asset & asset)
  {
  if (!asset.size)
    return kMaxUnsizedDownload;
  return asset.size + asset.size / 100 + 64 * 1024;
  }

bool RunDownload (const Fetcher & fetcher, const Downloader & downloader,
                  const Release & release, const ThisCopy & copy, const std::string & publicKeyHex,
                  const std::string & folder, const Progress & progress,
                  std::string * pathOut, std::string * error, std::string * sha256Out)
  {
  const InstallPlan plan = PlanInstall (release, copy, publicKeyHex);
  if (!plan.canInstall)
    {
    *error = plan.why;
    return false;
    }

  const std::string name = AssetNameFor (copy, release.version);
  const Asset * pPackage = FindAsset (release, name);
  const Asset * pManifest = FindAsset (release, ManifestName (release.version));
  const Asset * pSignature = FindAsset (release, SignatureName (release.version));

  auto fetchSmall = [&] (const Asset & asset, std::string & out)
    {
    int status = 0;
    if (!fetcher (asset.url, &status, &out, error))
      return false;
    if (status != 200)
      {
      *error = "could not fetch " + asset.name + ": status " + std::to_string (status);
      return false;
      }
    return true;
    };

  // The signature first: there is no point in fetching something it won't cover.
  std::string manifest, signature, expected;
  if (!fetchSmall (*pManifest, manifest) || !fetchSmall (*pSignature, signature))
    return false;
  if (!VerifySignature (manifest, signature, publicKeyHex, error) ||
      !ManifestHashFor (manifest, release.version, name, &expected, error))
    return false;

  std::error_code ec;
  fs::create_directories (fs::u8path (folder), ec);
  const fs::path final_ = fs::u8path (folder) / fs::u8path (name);
  fs::path part = final_;
  part += ".part";
  const std::string partPath = part.u8string ();

  // No more than the release list says, give or take: a server that sends more is
  // filling the disk, not sending the package.
  const uint64_t limit = DownloadLimit (*pPackage);
  bool bTooBig = false;
  const Progress limited = [&] (uint64_t got, uint64_t total)
    {
    if (got > limit || total > limit)
      {
      bTooBig = true;
      return false;
      }
    return !progress || progress (got, total);
    };

  if (!downloader (pPackage->url, partPath, limited, error))
    {
    if (bTooBig)
      *error = name + " is larger than the release says it is";
    fs::remove (part, ec);
    return false;
    }

  if (!VerifyDownload (partPath, name, release.version, manifest, signature, publicKeyHex, error))
    {
    fs::remove (part, ec);
    return false;
    }

  fs::remove (final_, ec);
  fs::rename (part, final_, ec);
  if (ec)
    {
    *error = "could not save " + name;
    fs::remove (part, ec);
    return false;
    }
  *pathOut = final_.u8string ();
  if (sha256Out)
    *sha256Out = expected;
  return true;
  }

// ---------------- the controller ----------------

Controller & Controller::Get ()
  {
  static Controller controller;
  return controller;
  }

void Controller::Configure (Poster poster, const std::string & installedVersion)
  {
  m_poster = std::move (poster);
  m_installed = installedVersion;
  m_fetcher = DefaultFetcher ();
  m_downloader = DefaultDownloader ();
  m_nextCheck = 0;
  m_lastTick = 0;
  m_newer.clear ();
  m_bDismissed = false;
  m_bUserWaiting = false;
  }

void Controller::SetNetwork (Fetcher fetcher, Downloader downloader)
  {
  m_fetcher = fetcher ? std::move (fetcher) : DefaultFetcher ();
  m_downloader = downloader ? std::move (downloader) : DefaultDownloader ();
  }

void Controller::SetOnChecked (std::function<void (bool, const CheckResult &)> handler)
  {
  m_onChecked = std::move (handler);
  }

bool Controller::IsChecking () const
  {
  return m_bChecking;
  }

// Detached: a slow network shouldn't hold up quitting, and nothing here outlives the
// process.  What it finds comes back through the poster, which is a no-op once the
// window has gone.
void Controller::Spawn (std::function<void ()> work)
  {
  std::thread (std::move (work)).detach ();
  }

void Controller::Tick (const time_t now, const bool bEnabled)
  {
  if (!m_poster)
    return;
  m_lastTick = now;
  if (m_nextCheck == 0)
    m_nextCheck = now + kFirstCheckAfterSeconds;
  if (!bEnabled || now < m_nextCheck || m_bChecking)
    return;
  m_nextCheck = now + kCheckEverySeconds;
  CheckNow (false);
  }

void Controller::CheckNow (const bool bUserAsked)
  {
  if (!m_poster)
    return;
  if (m_bChecking)
    {
    // The one under way answers them.
    if (bUserAsked)
      m_bUserWaiting = true;
    return;
    }
  m_bChecking = true;
  m_bUserWaiting = bUserAsked;

  const Fetcher fetcher = m_fetcher;
  const std::string installed = m_installed;
  const Poster poster = m_poster;
  Spawn ([this, fetcher, installed, poster] ()
    {
    std::shared_ptr<CheckResult> result = std::make_shared<CheckResult> (RunCheck (fetcher, installed));
    const bool bPosted = poster ([this, result] ()
      {
      const bool bUserAsked = m_bUserWaiting.exchange (false);
      m_bChecking = false;
      if (result->ok)
        {
        // A later version than the one dismissed brings the badge back; so does
        // the daily check, which is what "later" means.
        m_newer = result->newer;
        m_bDismissed = false;
        }
      else if (m_lastTick && m_nextCheck > m_lastTick + kRetryAfterFailureSeconds)
        m_nextCheck = m_lastTick + kRetryAfterFailureSeconds;     // not a day without knowing
      if (m_onChecked)
        m_onChecked (bUserAsked, *result);
      });
    if (!bPosted)
      {
      // Nothing will run on the UI thread to say it's over, so this does.
      m_bUserWaiting = false;
      m_bChecking = false;
      }
    });
  }

std::string Controller::BadgeText () const
  {
  if (m_newer.empty () || m_bDismissed)
    return std::string ();
  return "Update available: " + m_newer [0].version;
  }

void Controller::StartDownload (const Release & release, const ThisCopy & copy, const std::string & publicKeyHex,
                                const std::string & folder, Progress progress, DownloadDone done)
  {
  // Whoever asked is always told something, or they wait for ever.
  if (!m_poster)
    {
    if (done)
      done (false, "updates haven't been set up", std::string ());
    return;
    }
  if (m_bDownloading.exchange (true))
    {
    if (done)
      done (false, "another download of it is already under way", std::string ());
    return;
    }
  m_bCancel = false;

  const Fetcher fetcher = m_fetcher;
  const Downloader downloader = m_downloader;
  const Poster poster = m_poster;
  Spawn ([=] ()
    {
    const Progress checked = [this, progress] (uint64_t got, uint64_t total)
      {
      if (m_bCancel)
        return false;
      return !progress || progress (got, total);
      };
    std::string path, error, sha256;
    const bool bOK = RunDownload (fetcher, downloader, release, copy, publicKeyHex, folder, checked,
                                  &path, &error, &sha256);
    const bool bPosted = poster ([this, done, bOK, path, error, sha256] ()
      {
      m_bDownloading = false;
      if (done)
        done (bOK, bOK ? path : error, bOK ? sha256 : std::string ());
      });
    if (!bPosted)
      m_bDownloading = false;
    });
  }

} // namespace update
