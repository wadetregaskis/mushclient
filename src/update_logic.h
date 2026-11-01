// update_logic.h -- deciding about and checking a new version of the client.
//
// Everything here is plain work on strings and files, with no window, no thread and
// no network: what GitHub's list of releases says, which of them matter, which file
// is this copy's, whether that file is what the release's signature says it is, and
// how a portable copy's files get replaced.  update_service.h runs it on a worker, and
// the frontends show it.
//
// What protects an update:
//
//   A release carries MUSHclient-<version>-SHA256SUMS.txt, naming every package with
//   its SHA-256, and MUSHclient-<version>-SHA256SUMS.txt.sig, an Ed25519 signature of
//   that text.  The private key is not on GitHub: packaging/sign-update-manifest makes
//   them on the owner's machine (packaging/README.md).  The public key is one constant,
//   kUpdatePublicKey in update_key.h.
//
//   The first line of the signed text names the version, so an old signed release
//   can't be passed off as a new one.  A file is installed only after its hash has
//   been checked against signed text naming it, and the signature against that key.
//   There is no way to turn that off.  With no key in the source the client doesn't
//   download anything: it sends you to the release page.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace update {

// ---------------- versions ----------------

// The numbers in "6.10", "v6.10" or "6.10.2", compared numerically, so 6.10 is later
// than 6.9.  (6.00 and 6.0 are the same.)  Anything else ("6.10-beta") isn't a version.
struct Version
  {
  std::vector<int> parts;
  };

bool ParseVersion (const std::string & text, Version * out);
int  CompareVersions (const Version & a, const Version & b);     // <0, 0, >0

// ---------------- releases ----------------

struct Asset
  {
  std::string name;
  std::string url;
  uint64_t    size = 0;
  };

struct Release
  {
  std::string tag;         // v6.10
  std::string version;     // 6.10
  Version     parsed;
  std::string notes;       // Markdown
  std::string pageUrl;
  bool        draft = false;
  bool        prerelease = false;
  std::vector<Asset> assets;
  };

// GitHub's JSON list.  A release whose tag isn't a version is left out.  False, and
// why, if the text isn't a list of the right sort of thing.
bool ParseReleases (const std::string & json, std::vector<Release> * out, std::string * error);

// The ones to offer, newest first: later than `installed`, published, and not
// pre-releases.
std::vector<Release> ReleasesNewerThan (const std::vector<Release> & all, const std::string & installed);

// Markdown made presentable as plain text: headings and emphasis lose their marks,
// lists keep theirs, links show where they go.  Line ends are \n.
std::string TidyNotes (const std::string & markdown);

// Every release's notes under its version, in the order given.
std::string ConcatenateNotes (const std::vector<Release> & releases);

// ---------------- which file is this copy's ----------------

enum class Platform { Windows, MacOS, Linux };

struct ThisCopy
  {
  Platform    platform = Platform::Windows;
  std::string arch;                  // as release file names have it: x64, arm64; x86_64 on Linux
  bool        portable = false;      // Windows: the zip rather than the installer
  bool        discworld = false;     // the Discworld edition (a file packaging leaves: kEditionMarker)
  bool        systemManaged = false; // a .deb, or anywhere it can't write: not ours to replace
  };

// Beside the executable (in Resources on macOS) in the Discworld edition's packages.
extern const char * const kEditionMarker;

// The release file that updates this copy, or "" if it can't update itself.  The names
// are in packaging/README.md.
std::string AssetNameFor (const ThisCopy & copy, const std::string & version);

const Asset * FindAsset (const Release & release, const std::string & name);

std::string ManifestName (const std::string & version);
std::string SignatureName (const std::string & version);

// ---------------- signatures and hashes ----------------

// False if this build has no crypto (the port without OpenSSL).
bool CryptoAvailable ();

// signature: the 64 raw bytes.  publicKeyHex: 64 hex digits.
bool VerifySignature (const std::string & message, const std::string & signature,
                      const std::string & publicKeyHex, std::string * error);

bool HashFile (const std::string & path, std::string * hexOut, std::string * error);

// The hash the signed text gives for this file.  Fails unless its first line is
// "MUSHclient <version>" for this version, and it names the file exactly.
bool ManifestHashFor (const std::string & manifest, const std::string & version,
                      const std::string & fileName, std::string * hexOut, std::string * error);

// All of the above: true only if this file, called this, is what was signed.
bool VerifyDownload (const std::string & path, const std::string & fileName, const std::string & version,
                     const std::string & manifest, const std::string & signature,
                     const std::string & publicKeyHex, std::string * error);

// ---------------- replacing a portable copy ----------------

// Every file of a .zip into destDir, which it makes.  Stored or deflated entries only,
// with their checksums tested.  If everything is under one top-level folder, that is
// left off.  An entry that would land outside destDir is refused, and so is the zip.
bool ExtractZip (const std::string & zipPath, const std::string & destDir, std::string * error);

// In a portable copy the user's own files live among the program's.  True for a path
// (relative, with either slash) that belongs to the user, which an update adds if it
// isn't there and otherwise leaves alone.
bool IsUserDataPath (const std::string & relativePath);

// Moves the files under stageDir into installDir.  A file in use can't be replaced but
// can be renamed, even the running exe, so each existing file is renamed to
// <name>.mcold first, and the new one moved to where it was.  If any step fails every
// step so far is undone, and installDir is as it was.  What's left for
// CleanUpAfterSwap is noted in a file in installDir.
bool SwapInFiles (const std::string & stageDir, const std::string & installDir, std::string * error);

// At startup: removes what the last swap left, if it left anything.  (A file still in
// use stays until next time.)
void CleanUpAfterSwap (const std::string & installDir);

} // namespace update
