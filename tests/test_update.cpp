// tests/test_update.cpp -- the update logic and its worker, with the network and the
// release list faked and a key made here: update_logic.*, update_service.*.  Also the
// world backups' policy, world_backups.*.

#include "mc_test.h"
#include "stdafx.h"
#include "update_logic.h"
#include "update_service.h"
#include "world_backups.h"

#if defined(_WIN32) || defined(OPENSSL_FOUND)
#define HAVE_CRYPTO_FOR_TESTS 1
#include <openssl/evp.h>
#endif

#ifndef ZLIB_VERSION
#include <zlib.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace update;
namespace fs = std::filesystem;

namespace {

fs::path TempFolder (const char * name)
  {
  const char * dir = getenv ("TEMP");
  fs::path path = fs::path (dir ? dir : "/tmp") / (std::string ("mc_update_") + name);
  std::error_code ec;
  fs::remove_all (path, ec);
  fs::create_directories (path, ec);
  return path;
  }

void WriteText (const fs::path & path, const std::string & text)
  {
  std::error_code ec;
  fs::create_directories (path.parent_path (), ec);
  std::ofstream f (path, std::ios::binary);
  f << text;
  }

std::string ReadText (const fs::path & path)
  {
  std::ifstream f (path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf ();
  return s.str ();
  }

// A small JSON release list, as GitHub would say it.
const char kReleases [] = R"JSON([
  { "tag_name": "v6.10", "name": "6.10", "draft": false, "prerelease": false,
    "html_url": "https://github.com/x/y/releases/tag/v6.10",
    "body": "## Changes\r\n\r\n* **Fixed** a thing\r\n* Added [docs](https://example.com/docs)\r\n\r\nSee `foo_bar` and caf\u00e9 \ud83d\ude00.",
    "assets": [
      { "name": "MUSHclient-6.10-windows-arm64-setup.exe", "size": 1234567,
        "browser_download_url": "https://github.com/x/y/releases/download/v6.10/MUSHclient-6.10-windows-arm64-setup.exe" },
      { "name": "MUSHclient-6.10-windows-arm64-discworld-setup.exe", "size": 2,
        "browser_download_url": "https://github.com/x/y/d" }
    ] },
  { "tag_name": "v6.20", "draft": true, "prerelease": false, "html_url": "", "body": "a draft", "assets": [] },
  { "tag_name": "v6.30", "draft": false, "prerelease": true, "html_url": "", "body": "a prerelease", "assets": [] },
  { "tag_name": "v6.9", "draft": false, "prerelease": false, "html_url": "", "body": "Six point nine", "assets": [] },
  { "tag_name": "nightly", "draft": false, "prerelease": false, "html_url": "", "body": "not a version", "assets": [] },
  { "tag_name": "v6.00", "draft": false, "prerelease": false, "html_url": "", "body": "This one", "assets": [] },
  { "tag_name": "v5.06", "draft": false, "prerelease": false, "html_url": "", "body": "Old", "assets": null }
])JSON";

} // anonymous namespace

// ---------------- versions ----------------

TEST (UpdateVersions, ComparedAsNumbers)
  {
  const auto newer = [] (const char * a, const char * b)
    {
    Version x, y;
    EXPECT_TRUE (ParseVersion (a, &x));
    EXPECT_TRUE (ParseVersion (b, &y));
    return CompareVersions (x, y) > 0;
    };
  EXPECT_TRUE (newer ("6.10", "6.9"));
  EXPECT_FALSE (newer ("6.9", "6.10"));
  EXPECT_TRUE (newer ("v6.01", "6.00"));
  EXPECT_FALSE (newer ("6.00", "v6.00"));
  EXPECT_FALSE (newer ("6.0", "6.00"));     // numerically the same
  EXPECT_TRUE (newer ("7.0", "6.99"));
  EXPECT_TRUE (newer ("6.10.1", "6.10"));
  EXPECT_FALSE (newer ("6.10", "6.10.0"));
  }

TEST (UpdateVersions, NotVersions)
  {
  Version v;
  EXPECT_FALSE (ParseVersion ("", &v));
  EXPECT_FALSE (ParseVersion ("v", &v));
  EXPECT_FALSE (ParseVersion ("nightly", &v));
  EXPECT_FALSE (ParseVersion ("6.10-beta", &v));
  EXPECT_FALSE (ParseVersion ("6..1", &v));
  EXPECT_FALSE (ParseVersion ("6.", &v));
  EXPECT_FALSE (ParseVersion ("99999999999999.1", &v));
  }

// ---------------- the release list ----------------

TEST (UpdateReleases, ParsesWhatGitHubSays)
  {
  std::vector<Release> all;
  std::string error;
  ASSERT_TRUE (ParseReleases (kReleases, &all, &error));
  EXPECT_EQ (6u, all.size ());     // "nightly" isn't a version
  EXPECT_EQ (std::string ("v6.10"), all [0].tag);
  EXPECT_EQ (std::string ("6.10"), all [0].version);
  EXPECT_FALSE (all [0].draft);
  EXPECT_TRUE (all [1].draft);
  EXPECT_TRUE (all [2].prerelease);
  ASSERT_EQ (2u, all [0].assets.size ());
  EXPECT_EQ ((uint64_t) 1234567, all [0].assets [0].size);
  EXPECT_TRUE (all [0].notes.find ("caf\xC3\xA9") != std::string::npos);        // \u00e9
  EXPECT_TRUE (all [0].notes.find ("\xF0\x9F\x98\x80") != std::string::npos);   // a surrogate pair
  }

TEST (UpdateReleases, SaysWhyWhenRefused)
  {
  std::vector<Release> all;
  std::string error;
  EXPECT_FALSE (ParseReleases ("{\"message\": \"API rate limit exceeded\"}", &all, &error));
  EXPECT_TRUE (error.find ("rate limit") != std::string::npos);
  EXPECT_FALSE (ParseReleases ("<html>not json</html>", &all, &error));
  EXPECT_FALSE (ParseReleases ("[{\"tag_name\": \"v1\"", &all, &error));
  EXPECT_FALSE (ParseReleases ("", &all, &error));
  std::string deep (1000, '[');
  EXPECT_FALSE (ParseReleases (deep, &all, &error));
  EXPECT_TRUE (ParseReleases ("[]", &all, &error));
  EXPECT_EQ (0u, all.size ());
  }

TEST (UpdateReleases, OffersOnlyPublishedNewerOnes)
  {
  std::vector<Release> all;
  std::string error;
  ASSERT_TRUE (ParseReleases (kReleases, &all, &error));
  std::vector<Release> newer = ReleasesNewerThan (all, "6.00");
  ASSERT_EQ (2u, newer.size ());
  EXPECT_EQ (std::string ("6.10"), newer [0].version);    // newest first, as a number
  EXPECT_EQ (std::string ("6.9"), newer [1].version);

  EXPECT_EQ (0u, ReleasesNewerThan (all, "6.10").size ());
  EXPECT_EQ (1u, ReleasesNewerThan (all, "6.9").size ());
  EXPECT_EQ (0u, ReleasesNewerThan (all, "not a version").size ());
  }

TEST (UpdateNotes, TidiedAndConcatenated)
  {
  const std::string tidy = TidyNotes (
      "# Title\r\n"
      "\r\n\r\n"
      "Some **bold** and `code` text.\r\n"
      "* a [link](https://example.com)\r\n"
      "- plain ![pic](https://example.com/a.png) item\r\n"
      "---\r\n"
      "```\r\n"
      "**literal**\r\n"
      "```\r\n"
      "2 * 3 and snake_case_name\r\n");
  EXPECT_TRUE (tidy.find ("Title\n\nSome bold and code text.\n") == 0);
  EXPECT_TRUE (tidy.find ("- a link (https://example.com)\n") != std::string::npos);
  EXPECT_TRUE (tidy.find ("- plain pic item\n") != std::string::npos);
  EXPECT_TRUE (tidy.find ("---") == std::string::npos);
  EXPECT_TRUE (tidy.find ("    **literal**") != std::string::npos);
  EXPECT_TRUE (tidy.find ("2 * 3 and snake_case_name") != std::string::npos);
  EXPECT_TRUE (tidy.find ('\r') == std::string::npos);

  std::vector<Release> all;
  std::string error;
  ASSERT_TRUE (ParseReleases (kReleases, &all, &error));
  const std::string notes = ConcatenateNotes (ReleasesNewerThan (all, "6.00"));
  const size_t ten = notes.find ("Version 6.10");
  const size_t nine = notes.find ("Version 6.9");
  ASSERT_TRUE (ten != std::string::npos);
  ASSERT_TRUE (nine != std::string::npos);
  EXPECT_LT (ten, nine);        // newest first
  EXPECT_TRUE (notes.find ("Six point nine") != std::string::npos);
  EXPECT_TRUE (notes.find ("a draft") == std::string::npos);
  EXPECT_TRUE (notes.find ("This one") == std::string::npos);
  }

// ---------------- which file ----------------

TEST (UpdateAssets, TheNamesInPackagingReadme)
  {
  ThisCopy copy;
  copy.platform = Platform::Windows;
  copy.arch = "x64";
  EXPECT_EQ (std::string ("MUSHclient-6.10-windows-x64-setup.exe"), AssetNameFor (copy, "6.10"));
  copy.portable = true;
  EXPECT_EQ (std::string ("MUSHclient-6.10-windows-x64.zip"), AssetNameFor (copy, "6.10"));
  copy.discworld = true;
  EXPECT_EQ (std::string ("MUSHclient-6.10-windows-x64-discworld.zip"), AssetNameFor (copy, "6.10"));
  copy.portable = false;
  copy.arch = "arm64";
  EXPECT_EQ (std::string ("MUSHclient-6.10-windows-arm64-discworld-setup.exe"), AssetNameFor (copy, "6.10"));
  copy.arch.clear ();           // 32-bit Windows has no package
  EXPECT_EQ (std::string (), AssetNameFor (copy, "6.10"));

  ThisCopy mac;
  mac.platform = Platform::MacOS;
  EXPECT_EQ (std::string ("MUSHclient-6.10-macOS-universal.zip"), AssetNameFor (mac, "6.10"));
  mac.discworld = true;
  EXPECT_EQ (std::string ("MUSHclient-6.10-macOS-universal-discworld.zip"), AssetNameFor (mac, "6.10"));

  ThisCopy linux_;
  linux_.platform = Platform::Linux;
  linux_.arch = "x86_64";
  EXPECT_EQ (std::string ("mushclient-6.10-linux-x86_64.tar.xz"), AssetNameFor (linux_, "6.10"));
  linux_.discworld = true;
  EXPECT_EQ (std::string ("mushclient-discworld-6.10-linux-x86_64.tar.xz"), AssetNameFor (linux_, "6.10"));
  linux_.systemManaged = true;  // a .deb
  EXPECT_EQ (std::string (), AssetNameFor (linux_, "6.10"));

  EXPECT_EQ (std::string ("MUSHclient-6.10-SHA256SUMS.txt"), ManifestName ("6.10"));
  EXPECT_EQ (std::string ("MUSHclient-6.10-SHA256SUMS.txt.sig"), SignatureName ("6.10"));
  }

// ---------------- signatures ----------------

#ifdef HAVE_CRYPTO_FOR_TESTS

namespace {

// A throwaway key, made fresh for each run, as packaging/make-update-key makes the
// real one.
struct TestKey
  {
  EVP_PKEY * pkey = nullptr;
  std::string publicHex;

  TestKey ()
    {
    EVP_PKEY_CTX * ctx = EVP_PKEY_CTX_new_id (EVP_PKEY_ED25519, nullptr);
    EVP_PKEY_keygen_init (ctx);
    EVP_PKEY_keygen (ctx, &pkey);
    EVP_PKEY_CTX_free (ctx);
    unsigned char raw [32];
    size_t size = sizeof raw;
    EVP_PKEY_get_raw_public_key (pkey, raw, &size);
    static const char kDigits [] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++)
      {
      publicHex += kDigits [raw [i] >> 4];
      publicHex += kDigits [raw [i] & 15];
      }
    }

  ~TestKey ()
    {
    EVP_PKEY_free (pkey);
    }

  std::string Sign (const std::string & message) const
    {
    EVP_MD_CTX * ctx = EVP_MD_CTX_new ();
    EVP_DigestSignInit (ctx, nullptr, nullptr, nullptr, pkey);
    unsigned char sig [64];
    size_t size = sizeof sig;
    EVP_DigestSign (ctx, sig, &size, (const unsigned char *) message.data (), message.size ());
    EVP_MD_CTX_free (ctx);
    return std::string ((const char *) sig, size);
    }
  };

std::string Sha256Of (const fs::path & path)
  {
  std::string hex, error;
  EXPECT_TRUE (HashFile (path.u8string (), &hex, &error));
  return hex;
  }

} // anonymous namespace

TEST (UpdateSignature, AGoodOneIsAcceptedAndAnythingElseIsNot)
  {
  TestKey key, otherKey;
  const std::string message = "MUSHclient 6.10\nabc  file.zip\n";
  const std::string signature = key.Sign (message);
  std::string error;

  EXPECT_TRUE (VerifySignature (message, signature, key.publicHex, &error));

  // A tampered message.
  EXPECT_FALSE (VerifySignature (message + "x", signature, key.publicHex, &error));
  // A tampered signature.
  std::string bad = signature;
  bad [10] ^= 1;
  EXPECT_FALSE (VerifySignature (message, bad, key.publicHex, &error));
  // Someone else's key.
  EXPECT_FALSE (VerifySignature (message, signature, otherKey.publicHex, &error));
  // No key at all.
  EXPECT_FALSE (VerifySignature (message, signature, "", &error));
  EXPECT_TRUE (error.find ("no key") != std::string::npos);
  // Nonsense.
  EXPECT_FALSE (VerifySignature (message, signature, "not hex", &error));
  EXPECT_FALSE (VerifySignature (message, signature, std::string (64, 'z'), &error));
  EXPECT_FALSE (VerifySignature (message, "short", key.publicHex, &error));
  EXPECT_FALSE (VerifySignature (message, "", key.publicHex, &error));
  }

// Made by packaging/make-update-key and packaging/sign-update-manifest with the openssl
// command line, on four small files.  What the owner's scripts write is what the client
// has to accept.  This key signs nothing else.
TEST (UpdateSignature, AcceptsWhatThePackagingScriptsMake)
  {
  const std::string publicKey = "acd7913aa873649eca42b3df21c5a8dcef9bdd36f1cabdf926aa3493af5cc944";
  const std::string manifest =
      "MUSHclient 6.10\n"
      "9095bdb859308b62acf04036ffd4adfe366d7f737d276eb6c46ae434f3816c9b *MUSHclient-6.10-windows-arm64-discworld-setup.exe\n"
      "5b7a2754b389130e138d57ba83f1eae7f05a41c66becacfa2dd2060143ae66db *MUSHclient-6.10-windows-x64.zip\n"
      "9cfa1468c93fc18652e34a000f0c6614b0fa18f6f4887477ad9b0d36ca6a7eaa *mushclient_6.10_amd64.deb\n"
      "90aebae315675cbf04612de4f7d5874850f48e0b8dd82becbeaa47ca93f5ebfb *mushclient-discworld-6.10-linux-x86_64.tar.xz\n";
  const char signatureHex [] =
      "f3915ab4ab008896dd549066b3f88b13d73d89375225a44bc5f2ef790b66acfb"
      "d55e816c2e77987ba7537b506b5f2b4fc1473fad66387f23d66c7abc84903800";
  std::string signature;
  for (size_t i = 0; i < 128; i += 2)
    signature += (char) strtol (std::string (signatureHex + i, 2).c_str (), nullptr, 16);

  std::string error;
  EXPECT_TRUE (VerifySignature (manifest, signature, publicKey, &error));

  const fs::path folder = TempFolder ("scriptvector");
  const fs::path file = folder / "MUSHclient-6.10-windows-x64.zip";
  WriteText (file, "zip bytes");
  EXPECT_TRUE (VerifyDownload (file.u8string (), "MUSHclient-6.10-windows-x64.zip", "6.10",
                               manifest, signature, publicKey, &error));
  }

TEST (UpdateSignature, HashFileAgainstAKnownValue)
  {
  const fs::path folder = TempFolder ("hash");
  WriteText (folder / "abc", "abc");
  std::string hex, error;
  ASSERT_TRUE (HashFile ((folder / "abc").u8string (), &hex, &error));
  EXPECT_EQ (std::string ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), hex);
  EXPECT_FALSE (HashFile ((folder / "missing").u8string (), &hex, &error));
  }

TEST (UpdateSignature, TheManifestHasToNameTheVersionAndTheFile)
  {
  const std::string manifest =
      "MUSHclient 6.10\n"
      "aaa  MUSHclient-6.10-windows-x64.zip\n"
      "BBB *MUSHclient-6.10-windows-arm64.zip\n";
  std::string hex, error;
  ASSERT_TRUE (ManifestHashFor (manifest, "6.10", "MUSHclient-6.10-windows-x64.zip", &hex, &error));
  EXPECT_EQ (std::string ("aaa"), hex);
  ASSERT_TRUE (ManifestHashFor (manifest, "6.10", "MUSHclient-6.10-windows-arm64.zip", &hex, &error));
  EXPECT_EQ (std::string ("bbb"), hex);

  // A genuine old release's list can't be offered as this one.
  EXPECT_FALSE (ManifestHashFor (manifest, "6.20", "MUSHclient-6.10-windows-x64.zip", &hex, &error));
  // The wrong platform's file isn't on the list.
  EXPECT_FALSE (ManifestHashFor (manifest, "6.10", "mushclient-6.10-linux-x86_64.tar.xz", &hex, &error));
  EXPECT_FALSE (ManifestHashFor ("", "6.10", "x", &hex, &error));
  }

TEST (UpdateSignature, VerifyDownload)
  {
  TestKey key;
  const fs::path folder = TempFolder ("verify");
  const fs::path file = folder / "MUSHclient-6.10-windows-x64.zip";
  WriteText (file, "the package");
  const std::string manifest = "MUSHclient 6.10\n" + Sha256Of (file) + "  MUSHclient-6.10-windows-x64.zip\n";
  const std::string signature = key.Sign (manifest);
  const std::string name = "MUSHclient-6.10-windows-x64.zip";
  std::string error;

  EXPECT_TRUE (VerifyDownload (file.u8string (), name, "6.10", manifest, signature, key.publicHex, &error));

  // A file altered after signing.
  WriteText (file, "the packagf");
  EXPECT_FALSE (VerifyDownload (file.u8string (), name, "6.10", manifest, signature, key.publicHex, &error));
  EXPECT_TRUE (error.find ("not the file that was signed") != std::string::npos);
  WriteText (file, "the package");

  // A list altered after signing, to bless some other file.
  std::string forged = manifest;
  forged [20] = forged [20] == 'a' ? 'b' : 'a';
  EXPECT_FALSE (VerifyDownload (file.u8string (), name, "6.10", forged, signature, key.publicHex, &error));

  // The wrong platform: signed, and genuine, but not this copy's.
  EXPECT_FALSE (VerifyDownload (file.u8string (), "MUSHclient-6.10-windows-arm64.zip", "6.10", manifest, signature,
                                key.publicHex, &error));

  // The wrong version.
  EXPECT_FALSE (VerifyDownload (file.u8string (), name, "6.11", manifest, signature, key.publicHex, &error));

  // No key.
  EXPECT_FALSE (VerifyDownload (file.u8string (), name, "6.10", manifest, signature, "", &error));
  }

// The whole of a click, with the network faked.
struct FakeNetwork
  {
  std::map<std::string, std::string> pages;
  std::map<std::string, std::string> files;
  int downloads = 0;

  Fetcher MakeFetcher ()
    {
    return [this] (const std::string & url, int * status, std::string * body, std::string *)
      {
      const auto it = pages.find (url);
      *status = it == pages.end () ? 404 : 200;
      *body = it == pages.end () ? std::string () : it->second;
      return true;
      };
    }

  Downloader MakeDownloader ()
    {
    return [this] (const std::string & url, const std::string & path, const Progress & progress, std::string * error)
      {
      downloads++;
      const auto it = files.find (url);
      if (it == files.end ())
        {
        *error = "not found";
        return false;
        }
      if (progress && !progress (it->second.size (), it->second.size ()))
        {
        *error = "cancelled";
        return false;
        }
      WriteText (fs::u8path (path), it->second);
      return true;
      };
    }
  };

namespace {

Release ReleaseWithFiles (FakeNetwork & net, const TestKey & key, const std::string & package, const std::string & bytes)
  {
  const fs::path folder = TempFolder ("makehash");
  WriteText (folder / "x", bytes);
  const std::string manifest = "MUSHclient 6.10\n" + Sha256Of (folder / "x") + "  " + package + "\n";

  Release r;
  r.version = "6.10";
  r.assets.push_back (Asset { package, "https://f/package", bytes.size () });
  r.assets.push_back (Asset { ManifestName ("6.10"), "https://f/manifest", manifest.size () });
  r.assets.push_back (Asset { SignatureName ("6.10"), "https://f/sig", 64 });
  net.files ["https://f/package"] = bytes;
  net.pages ["https://f/manifest"] = manifest;
  net.pages ["https://f/sig"] = key.Sign (manifest);
  return r;
  }

ThisCopy WindowsPortable ()
  {
  ThisCopy copy;
  copy.platform = Platform::Windows;
  copy.arch = "x64";
  copy.portable = true;
  return copy;
  }

} // anonymous namespace

TEST (UpdateDownload, GoodFileEndsUpInTheFolder)
  {
  TestKey key;
  FakeNetwork net;
  const std::string name = "MUSHclient-6.10-windows-x64.zip";
  const Release r = ReleaseWithFiles (net, key, name, "the package");
  const fs::path folder = TempFolder ("dl_good");
  std::string path, error;
  uint64_t lastDone = 0;
  const bool bOK = RunDownload (net.MakeFetcher (), net.MakeDownloader (), r, WindowsPortable (), key.publicHex,
                                folder.u8string (),
                                [&] (uint64_t done, uint64_t) { lastDone = done; return true; }, &path, &error);
  ASSERT_TRUE (bOK);
  EXPECT_EQ ((uint64_t) 11, lastDone);
  EXPECT_EQ (std::string ("the package"), ReadText (fs::u8path (path)));
  EXPECT_TRUE (fs::u8path (path).filename ().u8string () == name);
  EXPECT_FALSE (fs::exists (fs::u8path (path + ".part")));
  }

TEST (UpdateDownload, TamperedInTransitIsThrownAway)
  {
  TestKey key;
  FakeNetwork net;
  const std::string name = "MUSHclient-6.10-windows-x64.zip";
  const Release r = ReleaseWithFiles (net, key, name, "the package");
  net.files ["https://f/package"] = "the evil package";
  const fs::path folder = TempFolder ("dl_tampered");
  std::string path, error;
  EXPECT_FALSE (RunDownload (net.MakeFetcher (), net.MakeDownloader (), r, WindowsPortable (), key.publicHex,
                             folder.u8string (), nullptr, &path, &error));
  EXPECT_TRUE (error.find ("not the file that was signed") != std::string::npos);
  EXPECT_TRUE (fs::is_empty (folder));
  }

TEST (UpdateDownload, ABadSignatureMeansNothingIsFetched)
  {
  TestKey key, otherKey;
  FakeNetwork net;
  const std::string name = "MUSHclient-6.10-windows-x64.zip";
  const Release r = ReleaseWithFiles (net, otherKey, name, "the package");
  std::string path, error;
  EXPECT_FALSE (RunDownload (net.MakeFetcher (), net.MakeDownloader (), r, WindowsPortable (), key.publicHex,
                             TempFolder ("dl_badsig").u8string (), nullptr, &path, &error));
  EXPECT_EQ (0, net.downloads);
  }

TEST (UpdateDownload, CancelLeavesNothingBehind)
  {
  TestKey key;
  FakeNetwork net;
  const Release r = ReleaseWithFiles (net, key, "MUSHclient-6.10-windows-x64.zip", "the package");
  const fs::path folder = TempFolder ("dl_cancel");
  std::string path, error;
  EXPECT_FALSE (RunDownload (net.MakeFetcher (), net.MakeDownloader (), r, WindowsPortable (), key.publicHex,
                             folder.u8string (), [] (uint64_t, uint64_t) { return false; }, &path, &error));
  EXPECT_TRUE (fs::is_empty (folder));
  }

TEST (UpdatePlan, WhenItWontInstall)
  {
  TestKey key;
  FakeNetwork net;
  const std::string name = "MUSHclient-6.10-windows-x64.zip";
  Release r = ReleaseWithFiles (net, key, name, "the package");
  ThisCopy copy = WindowsPortable ();

  EXPECT_TRUE (PlanInstall (r, copy, key.publicHex).canInstall);

  // No key in the source, which is how it starts out.
  InstallPlan plan = PlanInstall (r, copy, "");
  EXPECT_FALSE (plan.canInstall);
  EXPECT_TRUE (plan.why.find ("no key") != std::string::npos);

  // A wrong-platform release: no file for this copy.
  ThisCopy other = copy;
  other.arch = "arm64";
  EXPECT_FALSE (PlanInstall (r, other, key.publicHex).canInstall);

  // The system's package manager.
  ThisCopy deb;
  deb.platform = Platform::Linux;
  deb.arch = "x86_64";
  deb.systemManaged = true;
  plan = PlanInstall (r, deb, key.publicHex);
  EXPECT_FALSE (plan.canInstall);
  EXPECT_TRUE (plan.why.find ("package manager") != std::string::npos);

  // A release from before there were signatures.
  r.assets.pop_back ();
  plan = PlanInstall (r, copy, key.publicHex);
  EXPECT_FALSE (plan.canInstall);
  EXPECT_TRUE (plan.why.find ("isn't signed") != std::string::npos);
  }

#endif // HAVE_CRYPTO_FOR_TESTS

// ---------------- the check, end to end, on its worker ----------------

namespace {

// The worker posts here, and the test runs what it posted, as a UI thread would.
struct FakeUiThread
  {
  std::mutex mutex;
  std::vector<std::function<void ()>> queue;

  Controller::Poster Poster ()
    {
    return [this] (std::function<void ()> f)
      {
      std::lock_guard<std::mutex> lock (mutex);
      queue.push_back (std::move (f));
      };
    }

  // Runs what arrives until `until` says stop, or a few seconds pass.
  template <typename Until> bool Run (Until until)
    {
    for (int i = 0; i < 1000; i++)
      {
      std::vector<std::function<void ()>> ready;
        {
        std::lock_guard<std::mutex> lock (mutex);
        ready.swap (queue);
        }
      for (auto & f : ready)
        f ();
      if (until ())
        return true;
      std::this_thread::sleep_for (std::chrono::milliseconds (5));
      }
    return false;
    }
  };

} // anonymous namespace

TEST (UpdateController, BadgeAppearsAndCanBeDismissed)
  {
  FakeUiThread ui;
  Controller & c = Controller::Get ();
  c.Configure (ui.Poster (), "6.00");
  int fetches = 0;
  c.SetNetwork ([&] (const std::string & url, int * status, std::string * body, std::string *)
    {
    fetches++;
    EXPECT_EQ (std::string (kReleasesApiUrl), url);
    *status = 200;
    *body = kReleases;
    return true;
    }, nullptr);
  bool bToldUserAsked = true;
  int told = 0;
  c.SetOnChecked ([&] (bool bUserAsked, const CheckResult & result)
    {
    told++;
    bToldUserAsked = bUserAsked;
    EXPECT_TRUE (result.ok);
    });

  // Not at once, and not if it's turned off.
  const time_t start = 1000000;
  c.Tick (start, true);
  EXPECT_FALSE (c.IsChecking ());
  c.Tick (start + 40, false);
  EXPECT_FALSE (c.IsChecking ());
  EXPECT_EQ (0, fetches);

  c.Tick (start + 41, true);
  EXPECT_TRUE (c.IsChecking ());
  ASSERT_TRUE (ui.Run ([&] { return !c.IsChecking (); }));
  EXPECT_EQ (1, fetches);
  EXPECT_EQ (1, told);
  EXPECT_FALSE (bToldUserAsked);
  EXPECT_TRUE (c.HasUpdate ());
  EXPECT_EQ (std::string ("6.10"), c.LatestVersion ());
  EXPECT_EQ (2u, c.Newer ().size ());
  EXPECT_EQ (std::string ("Update available: 6.10"), c.BadgeText ());

  c.DismissBadge ();
  EXPECT_EQ (std::string (), c.BadgeText ());

  // About daily, not before.
  c.Tick (start + 41 + 3600, true);
  EXPECT_FALSE (c.IsChecking ());
  c.Tick (start + 41 + 24 * 3600, true);
  ASSERT_TRUE (ui.Run ([&] { return !c.IsChecking (); }));
  EXPECT_EQ (2, fetches);
  EXPECT_EQ (std::string ("Update available: 6.10"), c.BadgeText ());   // later has come

  c.SetOnChecked (nullptr);
  c.SetNetwork (nullptr, nullptr);
  }

TEST (UpdateController, CheckNowReportsUpToDateAndErrors)
  {
  FakeUiThread ui;
  Controller & c = Controller::Get ();
  c.Configure (ui.Poster (), "6.10");
  bool bUserAsked = false;
  CheckResult got;
  c.SetOnChecked ([&] (bool b, const CheckResult & result) { bUserAsked = b; got = result; });

  c.SetNetwork ([] (const std::string &, int * status, std::string * body, std::string *)
    {
    *status = 200;
    *body = kReleases;
    return true;
    }, nullptr);
  c.CheckNow (true);
  ASSERT_TRUE (ui.Run ([&] { return !c.IsChecking (); }));
  EXPECT_TRUE (bUserAsked);
  EXPECT_TRUE (got.ok);
  EXPECT_EQ (0u, got.newer.size ());
  EXPECT_FALSE (c.HasUpdate ());
  EXPECT_EQ (std::string (), c.BadgeText ());

  c.SetNetwork ([] (const std::string &, int *, std::string *, std::string * error)
    {
    *error = "no route to host";
    return false;
    }, nullptr);
  c.CheckNow (true);
  ASSERT_TRUE (ui.Run ([&] { return !c.IsChecking (); }));
  EXPECT_FALSE (got.ok);
  EXPECT_EQ (std::string ("no route to host"), got.error);

  c.SetNetwork ([] (const std::string &, int * status, std::string * body, std::string *)
    {
    *status = 403;
    *body = "{\"message\": \"API rate limit exceeded\"}";
    return true;
    }, nullptr);
  c.CheckNow (true);
  ASSERT_TRUE (ui.Run ([&] { return !c.IsChecking (); }));
  EXPECT_FALSE (got.ok);
  EXPECT_TRUE (got.error.find ("rate limit") != std::string::npos);

  c.SetNetwork ([] (const std::string &, int * status, std::string * body, std::string *)
    {
    *status = 500;
    *body = "oops";
    return true;
    }, nullptr);
  c.CheckNow (true);
  ASSERT_TRUE (ui.Run ([&] { return !c.IsChecking (); }));
  EXPECT_FALSE (got.ok);
  EXPECT_TRUE (got.error.find ("500") != std::string::npos);

  c.SetOnChecked (nullptr);
  c.SetNetwork (nullptr, nullptr);
  }

// ---------------- zip files ----------------

namespace {

void Put16 (std::string & s, uint32_t v)
  {
  s += (char) v;
  s += (char) (v >> 8);
  }

void Put32 (std::string & s, uint32_t v)
  {
  Put16 (s, v & 0xFFFF);
  Put16 (s, v >> 16);
  }

// Enough of a zip writer to make what 7-Zip makes.
std::string MakeZip (const std::vector<std::pair<std::string, std::string>> & entries, const bool bDeflate = true)
  {
  std::string local, central;
  for (const auto & entry : entries)
    {
    const uint32_t crc = (uint32_t) crc32 (crc32 (0L, Z_NULL, 0), (const Bytef *) entry.second.data (), (uInt) entry.second.size ());
    std::string data = entry.second;
    uint32_t method = 0;
    if (bDeflate && !entry.second.empty ())
      {
      std::vector<unsigned char> out (compressBound ((uLong) entry.second.size ()) + 64);
      z_stream z;
      memset (&z, 0, sizeof z);
      deflateInit2 (&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
      z.next_in = (Bytef *) entry.second.data ();
      z.avail_in = (uInt) entry.second.size ();
      z.next_out = out.data ();
      z.avail_out = (uInt) out.size ();
      deflate (&z, Z_FINISH);
      data.assign ((const char *) out.data (), z.total_out);
      deflateEnd (&z);
      method = 8;
      }

    const uint32_t offset = (uint32_t) local.size ();
    Put32 (local, 0x04034B50);
    Put16 (local, 20); Put16 (local, 0); Put16 (local, method); Put16 (local, 0); Put16 (local, 0);
    Put32 (local, crc); Put32 (local, (uint32_t) data.size ()); Put32 (local, (uint32_t) entry.second.size ());
    Put16 (local, (uint32_t) entry.first.size ()); Put16 (local, 0);
    local += entry.first;
    local += data;

    Put32 (central, 0x02014B50);
    Put16 (central, 20); Put16 (central, 20); Put16 (central, 0); Put16 (central, method); Put16 (central, 0); Put16 (central, 0);
    Put32 (central, crc); Put32 (central, (uint32_t) data.size ()); Put32 (central, (uint32_t) entry.second.size ());
    Put16 (central, (uint32_t) entry.first.size ()); Put16 (central, 0); Put16 (central, 0); Put16 (central, 0); Put16 (central, 0);
    Put32 (central, 0); Put32 (central, offset);
    central += entry.first;
    }

  std::string zip = local + central;
  Put32 (zip, 0x06054B50);
  Put16 (zip, 0); Put16 (zip, 0);
  Put16 (zip, (uint32_t) entries.size ()); Put16 (zip, (uint32_t) entries.size ());
  Put32 (zip, (uint32_t) central.size ()); Put32 (zip, (uint32_t) local.size ());
  Put16 (zip, 0);
  return zip;
  }

} // anonymous namespace

TEST (UpdateZip, ExtractsWithAndWithoutACommonFolder)
  {
  const fs::path folder = TempFolder ("zip");
  const std::string big (100000, 'x');
  WriteText (folder / "a.zip", MakeZip ({ { "top/", "" }, { "top/MUSHclient.exe", "exe bytes" }, { "top/lua/x.lua", big }, { "top/empty.txt", "" } }));
  std::string error;
  ASSERT_TRUE (ExtractZip ((folder / "a.zip").u8string (), (folder / "a").u8string (), &error));
  EXPECT_EQ (std::string ("exe bytes"), ReadText (folder / "a" / "MUSHclient.exe"));
  EXPECT_TRUE (big == ReadText (folder / "a" / "lua" / "x.lua"));
  EXPECT_TRUE (fs::exists (folder / "a" / "empty.txt"));
  EXPECT_FALSE (fs::exists (folder / "a" / "top"));

  WriteText (folder / "b.zip", MakeZip ({ { "one.txt", "1" }, { "two/three.txt", "3" } }, false));
  ASSERT_TRUE (ExtractZip ((folder / "b.zip").u8string (), (folder / "b").u8string (), &error));
  EXPECT_EQ (std::string ("1"), ReadText (folder / "b" / "one.txt"));
  EXPECT_EQ (std::string ("3"), ReadText (folder / "b" / "two" / "three.txt"));
  }

TEST (UpdateZip, RefusesToLeaveItsFolder)
  {
  const fs::path folder = TempFolder ("zipslip");
  std::string error;
  for (const char * evil : { "../outside.txt", "a/../../outside.txt", "..\\outside.txt", "/etc/passwd", "C:\\evil.txt", "x:stream" })
    {
    WriteText (folder / "evil.zip", MakeZip ({ { "fine.txt", "1" }, { evil, "gotcha" } }));
    EXPECT_FALSE (ExtractZip ((folder / "evil.zip").u8string (), (folder / "out").u8string (), &error));
    EXPECT_TRUE (error.find ("outside") != std::string::npos);
    }
  EXPECT_FALSE (fs::exists (folder / "outside.txt"));
  }

TEST (UpdateZip, DamageIsNoticed)
  {
  const fs::path folder = TempFolder ("zipdamage");
  std::string error;
  EXPECT_FALSE (ExtractZip ((folder / "missing.zip").u8string (), (folder / "out").u8string (), &error));
  WriteText (folder / "not.zip", "this isn't a zip file at all, and is long enough that it could be");
  EXPECT_FALSE (ExtractZip ((folder / "not.zip").u8string (), (folder / "out").u8string (), &error));

  // A flipped byte in stored data fails its CRC.
  std::string zip = MakeZip ({ { "file.txt", "some text to damage" } }, false);
  zip [42] ^= 1;
  WriteText (folder / "bad.zip", zip);
  EXPECT_FALSE (ExtractZip ((folder / "bad.zip").u8string (), (folder / "out").u8string (), &error));
  EXPECT_TRUE (error.find ("damaged") != std::string::npos);
  }

// ---------------- the portable swap ----------------

TEST (UpdateSwap, ReplacesProgramFilesAndLeavesTheUsersAlone)
  {
  const fs::path folder = TempFolder ("swap");
  const fs::path install = folder / "install";
  const fs::path stage = folder / "stage";

  WriteText (install / "MUSHclient.exe", "old exe");
  WriteText (install / "lua" / "old.lua", "old lua");
  WriteText (install / "only_old.dll", "stays, as nothing replaces it");
  // Theirs, all of it.
  WriteText (install / "worlds" / "my.mcl", "my world");
  WriteText (install / "worlds" / "plugins" / "bundled.xml", "my edited plugin");
  WriteText (install / "MUSHclient.ini", "my settings");
  WriteText (install / "mushclient_prefs.sqlite", "my prefs");
  WriteText (install / "scrollback" / "abc.mcsb", "my history");
  WriteText (install / "logs" / "log.txt", "my log");
  WriteText (install / "quow_plugins" / "maps" / "user.db", "my map");

  WriteText (stage / "MUSHclient.exe", "new exe");
  WriteText (stage / "lua" / "old.lua", "new lua");
  WriteText (stage / "lua" / "new.lua", "brand new");
  WriteText (stage / "fresh" / "folder" / "file.txt", "in a new folder");
  WriteText (stage / "worlds" / "my.mcl", "their world would be gone");
  WriteText (stage / "worlds" / "plugins" / "bundled.xml", "shipped plugin");
  WriteText (stage / "worlds" / "plugins" / "new_plugin.xml", "a plugin they don't have");
  WriteText (stage / "MUSHclient.ini", "shipped settings");

  std::string error;
  ASSERT_TRUE (SwapInFiles (stage.u8string (), install.u8string (), &error));

  EXPECT_EQ (std::string ("new exe"), ReadText (install / "MUSHclient.exe"));
  EXPECT_EQ (std::string ("new lua"), ReadText (install / "lua" / "old.lua"));
  EXPECT_EQ (std::string ("brand new"), ReadText (install / "lua" / "new.lua"));
  EXPECT_EQ (std::string ("in a new folder"), ReadText (install / "fresh" / "folder" / "file.txt"));
  EXPECT_EQ (std::string ("stays, as nothing replaces it"), ReadText (install / "only_old.dll"));

  EXPECT_EQ (std::string ("my world"), ReadText (install / "worlds" / "my.mcl"));
  EXPECT_EQ (std::string ("my edited plugin"), ReadText (install / "worlds" / "plugins" / "bundled.xml"));
  EXPECT_EQ (std::string ("a plugin they don't have"), ReadText (install / "worlds" / "plugins" / "new_plugin.xml"));
  EXPECT_EQ (std::string ("my settings"), ReadText (install / "MUSHclient.ini"));
  EXPECT_EQ (std::string ("my prefs"), ReadText (install / "mushclient_prefs.sqlite"));
  EXPECT_EQ (std::string ("my history"), ReadText (install / "scrollback" / "abc.mcsb"));
  EXPECT_EQ (std::string ("my log"), ReadText (install / "logs" / "log.txt"));
  EXPECT_EQ (std::string ("my map"), ReadText (install / "quow_plugins" / "maps" / "user.db"));

  // The old ones wait for the next start.
  EXPECT_EQ (std::string ("old exe"), ReadText (install / "MUSHclient.exe.mcold"));
  CleanUpAfterSwap (install.u8string ());
  EXPECT_FALSE (fs::exists (install / "MUSHclient.exe.mcold"));
  EXPECT_FALSE (fs::exists (install / "lua" / "old.lua.mcold"));
  EXPECT_FALSE (fs::exists (install / "update-cleanup.txt"));
  EXPECT_EQ (std::string ("new exe"), ReadText (install / "MUSHclient.exe"));

  // With nothing to clean up, it does nothing, and doesn't go looking.
  CleanUpAfterSwap (install.u8string ());
  EXPECT_EQ (std::string ("new exe"), ReadText (install / "MUSHclient.exe"));
  }

#ifdef _WIN32
// A file that won't move stops it, and everything goes back.  A running exe can be
// renamed, so this needs one held without FILE_SHARE_DELETE.
TEST (UpdateSwap, AFailureUndoesEverything)
  {
  const fs::path folder = TempFolder ("swapfail");
  const fs::path install = folder / "install";
  const fs::path stage = folder / "stage";
  WriteText (install / "a.dll", "old a");
  WriteText (install / "b.dll", "old b");
  WriteText (install / "z.dll", "old z");
  WriteText (stage / "a.dll", "new a");
  WriteText (stage / "added.txt", "new file");
  WriteText (stage / "b.dll", "new b");
  WriteText (stage / "newfolder" / "c.txt", "new c");
  WriteText (stage / "z.dll", "new z");

  const HANDLE locked = CreateFileW ((install / "z.dll").c_str (), GENERIC_READ, FILE_SHARE_READ, NULL,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  ASSERT_TRUE (locked != INVALID_HANDLE_VALUE);

  std::string error;
  EXPECT_FALSE (SwapInFiles (stage.u8string (), install.u8string (), &error));
  CloseHandle (locked);
  EXPECT_TRUE (error.find ("z.dll") != std::string::npos);

  EXPECT_EQ (std::string ("old a"), ReadText (install / "a.dll"));
  EXPECT_EQ (std::string ("old b"), ReadText (install / "b.dll"));
  EXPECT_EQ (std::string ("old z"), ReadText (install / "z.dll"));
  EXPECT_FALSE (fs::exists (install / "added.txt"));
  EXPECT_FALSE (fs::exists (install / "newfolder"));
  EXPECT_FALSE (fs::exists (install / "a.dll.mcold"));
  // And the stage can be tried again.
  EXPECT_EQ (std::string ("new a"), ReadText (stage / "a.dll"));
  EXPECT_EQ (std::string ("new z"), ReadText (stage / "z.dll"));
  }
#endif

// ---------------- world backups ----------------

using world_backups::Entry;

TEST (WorldBackups, RetentionPolicy)
  {
  const time_t now = 1800000000;
  std::vector<Entry> entries;
  auto add = [&] (const std::string & name, time_t ago) { entries.push_back (Entry { name, now - ago }); };

  // Every minute for the last 20 minutes: only the newest 5, plus what the hourly and
  // daily rules pick up, which is the newest of them again.
  for (int i = 0; i < 20; i++)
    add ("min" + std::to_string (i), i * 60);
  std::vector<std::string> doomed = world_backups::ChooseToDelete (entries, now);
  EXPECT_EQ (15u, doomed.size ());
  for (int i = 0; i < 5; i++)
    EXPECT_TRUE (std::find (doomed.begin (), doomed.end (), "min" + std::to_string (i)) == doomed.end ());

  // A crash-loop: the copy from before the bad change is 2 minutes older than the ones
  // after it, and they don't push it out.
  entries.clear ();
  add ("after3", 0);
  add ("after2", 30);
  add ("after1", 60);
  add ("before", 120);
  EXPECT_EQ (0u, world_backups::ChooseToDelete (entries, now).size ());

  // Hourly: ten at least an hour apart, among more.
  entries.clear ();
  for (int i = 0; i < 30; i++)
    add ("hour" + std::to_string (i), i * 3600);
  doomed = world_backups::ChooseToDelete (entries, now);
  // Kept: hour0..hour9 by the hourly rule; hour24 starts yesterday's day, whichever
  // day boundary it is, and at most two more days fit into 30 hours.
  EXPECT_LE (doomed.size (), 20u);
  EXPECT_GE (doomed.size (), 17u);
  EXPECT_TRUE (std::find (doomed.begin (), doomed.end (), "hour9") == doomed.end ());
  EXPECT_TRUE (std::find (doomed.begin (), doomed.end (), "hour15") != doomed.end () ||
               std::find (doomed.begin (), doomed.end (), "hour16") != doomed.end ());

  // Daily: one a day for a week, and older than that goes once ten newer ones exist.
  entries.clear ();
  for (int i = 0; i < 20; i++)
    add ("day" + std::to_string (i), i * 86400);
  doomed = world_backups::ChooseToDelete (entries, now);
  EXPECT_EQ (10u, doomed.size ());     // the newest 10 are hourly-spaced too
  EXPECT_TRUE (std::find (doomed.begin (), doomed.end (), "day9") == doomed.end ());
  EXPECT_TRUE (std::find (doomed.begin (), doomed.end (), "day10") != doomed.end ());

  // Few enough: nothing goes, however old.
  entries.clear ();
  add ("ancient", 365 * 86400);
  EXPECT_EQ (0u, world_backups::ChooseToDelete (entries, now).size ());
  }

TEST (WorldBackups, ACopyIsTakenWhenDueAndOnlyThen)
  {
  const fs::path folder = TempFolder ("backups");
  const std::string root = (folder / "backups").u8string () + "/";
  const fs::path world = folder / "My World.mcl";
  const std::string worldPath = world.u8string ();
  const fs::path where = fs::u8path (world_backups::FolderFor (root, worldPath));

  const auto count = [&] ()
    {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it (where, ec), end; !ec && it != end; it.increment (ec))
      n++;
    return n;
    };

  time_t now = 1800000000;

  // No world file yet: a new world's first save has nothing to lose.
  EXPECT_FALSE (world_backups::BackUpIfDue (root, worldPath, true, now));
  EXPECT_EQ (0u, count ());

  WriteText (world, "version one");
  EXPECT_TRUE (world_backups::BackUpIfDue (root, worldPath, true, now));
  EXPECT_EQ (1u, count ());

  // Same contents: no point, even at the start of another session.
  EXPECT_FALSE (world_backups::BackUpIfDue (root, worldPath, true, now + 10));
  EXPECT_EQ (1u, count ());

  // Later in the same session and within the hour: no.
  WriteText (world, "version two");
  EXPECT_FALSE (world_backups::BackUpIfDue (root, worldPath, false, now + 60));
  EXPECT_EQ (1u, count ());

  // First save of a new session, changed: yes, however soon.
  EXPECT_TRUE (world_backups::BackUpIfDue (root, worldPath, true, now + 120));
  EXPECT_EQ (2u, count ());

  // An hour on: yes.
  WriteText (world, "version three");
  EXPECT_TRUE (world_backups::BackUpIfDue (root, worldPath, false, now + 120 + 3600));
  EXPECT_EQ (3u, count ());

  // The copies are the world file as it was.
  bool bFoundOne = false;
  for (const auto & entry : fs::directory_iterator (where))
    if (ReadText (entry.path ()) == "version one")
      {
      bFoundOne = true;
      EXPECT_EQ (std::string (".mcl"), entry.path ().extension ().u8string ());
      }
  EXPECT_TRUE (bFoundOne);
  }

TEST (WorldBackups, DifferentFoldersForDifferentWorlds)
  {
  EXPECT_TRUE (world_backups::FolderFor ("b/", "C:\\one\\x.mcl") != world_backups::FolderFor ("b/", "C:\\two\\x.mcl"));
  EXPECT_TRUE (world_backups::FolderFor ("b/", "C:\\one\\x.mcl") == world_backups::FolderFor ("b/", "c:/ONE/x.mcl"));
  EXPECT_TRUE (world_backups::FolderFor ("b/", "C:\\one\\my world.mcl").find ("my world-") != std::string::npos);
  }
