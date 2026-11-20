// msp.cpp -- the MUD Sound Protocol, in the client.
//
// A server says IAC WILL 90 and, once told DO, starts lines with !!SOUND(...) or
// !!MUSIC(...).  This parses them before anything else sees the line (so a trigger
// cannot be set off by, or a log filled with, a sound), keeps to the spec's rules for
// priority and repeats, and fetches missing files if and only if the player has said so.
//
// What a server can do is deliberately small.  It names a file relative to a sound
// directory, and that name has to get past proto::IsSafeMspName, whether it is being
// read or written.  Everything about downloads is off by default, and on, a host is
// asked about once, addresses are checked after DNS (pal::SafeGet), and the only place
// a file lands is inside this world's own cache folder, without replacing anything.
//
// One player at a time: a plugin that claims option 90 gets MSP to itself, and if the old
// msp.xml is loaded its trigger keeps doing what it does, so nothing plays twice.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "protocols.h"
#include "pal/https_get.h"
#include "user_data.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

const char * const kMspPluginID = "7da63d78d9f91bebb5285127";    // plugins/msp.xml

const size_t kMaxInFlight = 4;
const size_t kMaxWaiting = 32;
const size_t kMaxDownloadBytes = 16 * 1024 * 1024;
const int    kDownloadTimeoutSeconds = 60;

// what we could play instead of a kind we cannot
const char * const kAlternateExtensions [] = { ".mp3", ".wav", ".flac" };

bool IsUnplayableName (const std::string & name)
  {
  const std::string lower = proto::Lower (name);
  return proto::IsMidiName (name) ||
         (lower.size () > 4 && lower.compare (lower.size () - 4, 4, ".ogg") == 0);
  }

std::string SoundDirectoryOfUser ()
  {
#ifdef _WIN32
  return std::string ((const char *) UserData_Directory ()) + "sounds\\";
#else
  std::error_code ec;
  return (fs::current_path (ec) / "sounds").string () + "/";
#endif
  }

std::string SoundDirectoryOfProgram ()
  {
  return std::string ((const char *) ExtractDirectory (App.m_strMUSHclientFileName)) + "sounds/";
  }

// Every world's downloads are under this, each in a folder of its own.
std::string CacheRoot ()
  {
  std::string base;
#ifdef _WIN32
  base = (const char *) UserData_Directory ();
#else
  std::error_code ec;
  base = fs::current_path (ec).string () + "/";
#endif
  return base + "msp_cache/";
  }

// The directories to look in, first to last.
std::vector <std::string> SoundRoots (CMUSHclientDoc & doc)
  {
  std::vector <std::string> roots;
  if (!doc.m_strMSPSoundDirectory.IsEmpty ())
    roots.push_back ((const char *) doc.m_strMSPSoundDirectory);
  roots.push_back (Proto_WorldCacheDirectory (doc));
  roots.push_back (SoundDirectoryOfUser ());
  roots.push_back (SoundDirectoryOfProgram ());
  return roots;
  }

// Files in a directory, under root, that match the last part of a name.
void Matches (const std::string & root, const std::string & directory, const std::string & pattern,
              std::vector <std::string> * found)
  {
  const std::string base = Proto_ContainedPath (root, directory.empty () ? "." : directory);
  if (base.empty ())
    return;

  if (pattern.find_first_of ("*?") == std::string::npos)
    {
    const std::string path = Proto_ContainedPath (root, directory.empty () ? pattern : directory + "/" + pattern);
    std::error_code ec;
    if (!path.empty () && fs::is_regular_file (path, ec))
      found->push_back (path);
    return;
    }

  std::error_code ec;
  for (fs::directory_iterator it (base, ec), end; !ec && it != end; it.increment (ec))
    {
    // A name the code page cannot hold makes path::string () throw on Windows.  Such a
    // name cannot match, since what a server asks for is ASCII (IsSafeMspName).
    std::string leaf;
    try { leaf = it->path ().filename ().string (); }
    catch (const std::exception &) { continue; }
    if (!proto::MspWildcardMatch (pattern, leaf))
      continue;
    std::error_code ec2;
    if (!it->is_regular_file (ec2))
      continue;
    const std::string path = Proto_ContainedPath (root, directory.empty () ? leaf : directory + "/" + leaf);
    if (!path.empty ())
      found->push_back (path);
    }
  }

// The file for a name.  The spec: the name's own subdirectory first, then the root.  T=
// is a subdirectory to use if the name does not have one.  A wildcard that matches
// several gets one at random, and repeats pick again.
std::string FindSoundFile (CMUSHclientDoc & doc, const std::string & name, const std::string & type)
  {
  const size_t slash = name.rfind ('/');
  std::string directory = slash == std::string::npos ? "" : name.substr (0, slash);
  const std::string leaf = slash == std::string::npos ? name : name.substr (slash + 1);
  if (directory.empty () && !type.empty ())
    directory = type;

  for (const std::string & root : SoundRoots (doc))
    {
    std::vector <std::string> found;
    if (!directory.empty ())
      Matches (root, directory, leaf, &found);
    if (found.empty ())
      Matches (root, "", leaf, &found);
    if (found.empty ())
      continue;
    if (found.size () == 1)
      return found [0];

    static std::mt19937 generator { std::random_device { } () };
    return found [std::uniform_int_distribution <size_t> (0, found.size () - 1) (generator)];
    }
  return "";
  }

// Status > 0 is playing, on both platforms.
bool BufferBusy (CMUSHclientDoc & doc, const int buffer)
  {
  if (doc.m_pProto->bTestSound)
    return doc.m_pProto->testBusy [buffer];
  return doc.GetSoundStatus ((short) buffer) > 0;
  }

void StopBuffer (CMUSHclientDoc & doc, const int buffer)
  {
  if (doc.m_pProto->bTestSound)
    doc.m_pProto->testBusy [buffer] = false;
  else
    doc.StopSound ((short) buffer);
  }

bool VoicePlaying (CMUSHclientDoc & doc, const CMspVoice & voice)
  {
  return voice.bActive && voice.buffer >= 1 && BufferBusy (doc, voice.buffer);
  }

// The highest-numbered buffer nothing is using, so scripts choosing from the bottom
// (PlaySound's buffer 0 does) do not meet us until all the others are busy.
int FreeBuffer (CMUSHclientDoc & doc)
  {
  for (int buffer = MAX_SOUND_BUFFERS; buffer >= 1; buffer--)
    if (!BufferBusy (doc, buffer))
      return buffer;
  return 0;
  }

bool StartVoice (CMUSHclientDoc & doc, CMspVoice & voice, const std::string & file, const bool bLoop)
  {
  int buffer = voice.bActive && voice.buffer >= 1 ? voice.buffer : FreeBuffer (doc);
  if (!buffer)
    return false;   // every buffer is busy, and a sound effect is not worth stopping a script's

  long result = 0;
  if (doc.m_pProto->bTestSound)
    {
    doc.m_pProto->testBusy [buffer] = true;
    doc.m_pProto->testPlayed.push_back (file + (bLoop ? " loop" : ""));
    }
  else
    result = doc.PlaySound ((short) buffer, file.c_str (), bLoop,
                            proto::MspVolumeToDb (voice.volume), 0.0);
  if (result != 0)
    {
    voice.bActive = false;
    return false;
    }

  voice.bActive = true;
  voice.buffer = buffer;
  voice.file = file;
  return true;
  }

void StopVoice (CMUSHclientDoc & doc, CMspVoice & voice)
  {
  if (voice.bActive && voice.buffer >= 1)
    StopBuffer (doc, voice.buffer);
  voice = CMspVoice ();
  }

// ---------------------------------------------------------------- downloads

// Fetch one file.  On a thread of its own: the UI must not wait for a slow server.
// Touches nothing of the document; the result goes in a queue the timer empties, which
// survives the world being closed underneath it.
void DownloadWorker (std::shared_ptr <CMspDownloads> downloads, const std::string name,
                     const std::string url, const std::string path, const bool bAllowHttp,
                     const std::string cacheRoot, const std::string worldDirectory)
  {
  CMspDownloads::Result result;
  result.name = name;

  pal::SafeGetOptions options;
  options.bAllowHttp = bAllowHttp;
  options.maxBytes = kMaxDownloadBytes;
  options.timeoutSeconds = kDownloadTimeoutSeconds;
  options.userAgent = std::string ("MUSHclient/") + (const char *) MUSHCLIENT_VERSION;

  int status = 0;
  std::string body;
  if (!pal::SafeGet (url, options, &status, &body, &result.error))
    ;  // the error says why
  else if (status != 200)
    result.error = "the server said " + std::to_string (status);
  else if (!proto::LooksLikeAudio (name, body))
    result.error = "it is not the kind of file its name says";
  else
    {
    // Into a temporary name, created fresh, and then moved without replacing anything.
    std::error_code ec;
    fs::create_directories (fs::path (path).parent_path (), ec);

    // A name of this attempt's own: one left by a crash, which nothing will remove, would
    // otherwise stop this sound being saved for good.
    static std::atomic<unsigned> attempts { 0 };
    std::random_device random;
    const std::string temporary = path + "." + std::to_string (random ()) + "-" +
                                  std::to_string (++attempts) + ".part";
#ifdef _WIN32
    const int fd = _open (temporary.c_str (), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    const int fd = open (temporary.c_str (), O_CREAT | O_EXCL | O_WRONLY, 0644);
#endif
    if (fd < 0)
      result.error = "could not create the file";
    else
      {
#ifdef _WIN32
      const bool bWritten = _write (fd, body.data (), (unsigned int) body.size ()) == (int) body.size ();
      _close (fd);
      const bool bMoved = bWritten && MoveFileExA (temporary.c_str (), path.c_str (), 0);
      if (!bMoved)
        DeleteFileA (temporary.c_str ());
#else
      const bool bWritten = write (fd, body.data (), body.size ()) == (ssize_t) body.size ();
      close (fd);
      // link fails if the name exists, where rename would replace it
      const bool bMoved = bWritten && link (temporary.c_str (), path.c_str ()) == 0;
      unlink (temporary.c_str ());
#endif
      if (!bMoved)
        result.error = "could not save the file";
      else
        Proto_MspTrimCache (cacheRoot, worldDirectory, path,
                            proto::kMspCacheWorldBytes, proto::kMspCacheTotalBytes);
      }
    }

  std::lock_guard <std::mutex> guard (downloads->lock);
  downloads->done.push_back (result);
  }

void StartDownload (CMUSHclientDoc & doc, const proto::MspTrigger & trigger, const std::string & name)
  {
  CProtocolState & state = *doc.m_pProto;

  // Not for a line that may be anyone's (see MspTrigger::bMayDownload).  Silently: a
  // note would be a way for another player to write in this player's window.
  if (!trigger.bMayDownload)
    return;

  const std::string base = !trigger.url.empty () ? trigger.url : state.mspDefaultUrl;
  if (base.empty ())
    return;

  if (!doc.m_bMSPDownloads)
    {
    if (!state.bMspDownloadsOffNoted)
      {
      state.bMspDownloadsOffNoted = true;
      Proto_Note (doc, "This server offers sounds to download, which is off "
                       "(world.SetOption (\"msp_downloads\", 1) would turn it on).");
      }
    return;
    }

  // A proxy is there to hide this machine's address, and a download would give it away.
  if (doc.m_iSocksProcessing != eProxyServerNone)
    return;

  if (!proto::IsSafeMspName (name, false) || !proto::IsDownloadableAudioName (name))
    return;

  // T= is the server's too, and checked by the same rule as the name, here as well as
  // in Proto_MspPlay: this is where it becomes part of a path that is written.
  const std::string type = proto::IsSafeMspName (trigger.type, false) ? trigger.type : "";
  const std::string relative = (name.find ('/') == std::string::npos && !type.empty ())
                                  ? type + "/" + name : name;
  if (!proto::IsSafeMspName (relative, false))
    return;
  if (state.mspInFlight.count (relative) || state.mspFailed.count (relative))
    return;

  const std::string path = Proto_ContainedPath (Proto_WorldCacheDirectory (doc), relative);
  if (path.empty ())
    return;

  const std::string url = proto::MspDownloadUrl (base, name);
  const std::string host = proto::UrlHost (url);
  if (url.empty () || !proto::IsPlausibleHostName (host))
    return;

  if (proto::Lower (url.substr (0, 7)) == "http://" && !doc.m_bMSPAllowHttp)
    {
    if (state.mspHostsAsked.insert ("http:" + host).second)
      Proto_Note (doc, "This server offers sounds from " + host + " over plain http, which "
                       "is off (the option msp_allow_http would allow it).");
    return;
    }

  if (!proto::HostListHas ((const char *) doc.m_strMSPAllowedHosts, host))
    {
    if (state.mspWaiting.size () < kMaxWaiting)
      state.mspWaiting.push_back (std::make_pair (host, trigger));
    if (state.mspHostsAsked.insert (host).second)
      {
      CProtocolNote note;
      note.text = "This server wants to download sounds from " + host +
                  ", which would tell that host your address.";
      note.action = Proto_ActionLink ("msp_allow", host);
      note.linkText = "Allow downloads from " + host;
      Proto_Note (doc, note);
      }
    return;
    }

  if (state.mspInFlight.size () >= kMaxInFlight)
    return;       // not now.  It will be asked for again

  state.mspInFlight.insert (relative);
  if (trigger.bMusic)
    state.mspPendingMusic.reset (new proto::MspTrigger (trigger));

  const std::string worldDirectory = Proto_WorldCacheDirectory (doc);
  std::thread (DownloadWorker, state.downloads, relative, url, path, doc.m_bMSPAllowHttp != 0,
               CacheRoot (), worldDirectory).detach ();
  }

} // anonymous namespace

// ---------------------------------------------------------------- paths

std::string Proto_WorldCacheDirectory (CMUSHclientDoc & doc)
  {
  const std::string id = doc.m_strWorldID.IsEmpty () ? "unsaved" : (const char *) doc.m_strWorldID;
  return CacheRoot () + id + "/";
  }

namespace {

struct CachedFile
  {
  fs::path path;
  uint64_t size;
  fs::file_time_type written;
  };

// The files a download could have made, under a directory.  A .part a crash left goes
// as it is found, once it is old enough not to be a download still being written.
std::vector <CachedFile> CachedFiles (const fs::path & directory)
  {
  std::vector <CachedFile> files;
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now ();
  for (fs::recursive_directory_iterator it (directory, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment (ec))
    {
    std::error_code ec2;
    if (it->is_symlink (ec2) || !it->is_regular_file (ec2))
      continue;
    // Only the extension, and in ASCII.  (path::string () throws, on Windows, for a name
    // the code page cannot hold, and this is a thread of its own: that would end the
    // program.  A name a download made is ASCII, so anything else is not ours.)
    const std::wstring wide = it->path ().filename ().wstring ();
    std::string name;
    for (const wchar_t w : wide)
      name += (w > 0 && w < 0x80) ? (char) w : '?';
    const auto written = it->last_write_time (ec2);
    if (ec2)
      continue;
    if (name.size () > 5 && proto::Lower (name.substr (name.size () - 5)) == ".part")
      {
      if (now - written > std::chrono::hours (1))
        fs::remove (it->path (), ec2);
      continue;
      }
    if (!proto::IsDownloadableAudioName (name))
      continue;       // a player's own, or an image: not ours to remove
    const uint64_t size = it->file_size (ec2);
    if (!ec2)
      files.push_back (CachedFile { it->path (), size, written });
    }
  return files;
  }

// Remove the oldest until what is left is at most `cap`.  Not `keep`.
void TrimTo (std::vector <CachedFile> & files, const uint64_t cap, const fs::path & keep)
  {
  uint64_t total = 0;
  for (const CachedFile & file : files)
    total += file.size;
  if (total <= cap)
    return;

  std::sort (files.begin (), files.end (),
             [] (const CachedFile & a, const CachedFile & b) { return a.written < b.written; });
  std::error_code ec;
  for (auto it = files.begin (); it != files.end () && total > cap; )
    {
    if (fs::equivalent (it->path, keep, ec))
      {
      ++it;
      continue;
      }
    // A file being played may not go (Windows says so): it stays, and counts.
    if (fs::remove (it->path, ec) && !ec)
      {
      total -= it->size;
      it = files.erase (it);
      }
    else
      ++it;
    }
  }

} // anonymous namespace

void Proto_MspTrimCache (const std::string & cacheRoot, const std::string & worldDirectory,
                         const std::string & keep, const uint64_t worldCap, const uint64_t totalCap)
  {
  // Two downloads finishing together may both trim; removing what the other removed
  // fails quietly, and the second pass finds less to do.
  static std::mutex lock;
  std::lock_guard <std::mutex> guard (lock);

  std::vector <CachedFile> world = CachedFiles (fs::path (worldDirectory));
  TrimTo (world, worldCap, fs::path (keep));

  std::vector <CachedFile> all = CachedFiles (fs::path (cacheRoot));
  TrimTo (all, totalCap, fs::path (keep));
  }

std::string Proto_ContainedPath (const std::string & base, const std::string & relative)
  {
  std::error_code ec;

  const fs::path root = fs::weakly_canonical (fs::path (base), ec);
  if (ec || root.empty ())
    return "";

  const fs::path full = fs::weakly_canonical (root / fs::path (relative), ec);
  if (ec)
    return "";

  // element by element: "C:\sounds" is not inside "C:\sound", and a string compare says it is
  auto rootPart = root.begin ();
  auto fullPart = full.begin ();
  for (; rootPart != root.end (); ++rootPart, ++fullPart)
    {
    // the empty element a trailing separator leaves on the root is no part of it
    if (rootPart->empty ())
      break;
    if (fullPart == full.end ())
      return "";
#ifdef _WIN32
    if (proto::Lower (rootPart->string ()) != proto::Lower (fullPart->string ()))
#else
    if (*rootPart != *fullPart)
#endif
      return "";
    }

  return full.string ();
  }

// ---------------------------------------------------------------- who plays

bool Proto_MspPluginLoaded (CMUSHclientDoc & doc)
  {
  for (PluginListIterator pit = doc.m_PluginList.begin (); pit != doc.m_PluginList.end (); ++pit)
    if ((*pit)->m_bEnabled && (*pit)->m_strID == kMspPluginID)
      return true;
  return false;
  }

bool Proto_MspWillAccept (CMUSHclientDoc & doc)
  {
  // With msp.xml loaded the client can handle MSP, so that is what the server hears.
  // It is its trigger that will play the line.
  if (Proto_MspPluginLoaded (doc))
    return true;
  if (!doc.m_bNativeMSP)
    return false;
  doc.m_pProto->bMspNegotiated = true;
  return true;
  }

bool Proto_MspParsing (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  if (!doc.m_bNativeMSP || state.bMspPluginClaimed)
    return false;
  if (!state.bMspNegotiated && !doc.m_bMSPWithoutNegotiation)
    return false;
  return !Proto_MspPluginLoaded (doc);
  }

// ---------------------------------------------------------------- the trigger

// It was not a trigger.  What we held back is ordinary text, and the display loop gets
// it as if it had arrived normally.  (But not this time as a candidate for MSP.)
//
// The byte that showed it was not a trigger goes with it, and the caller then takes it
// as used up.  What was held back can leave the display loop in the middle of something
// -- an MXP tag, a UTF-8 character, an ESC -- which the next byte is part of, and only
// the same pass over the bytes keeps them together: the outer loop would take that byte
// as plain text while the phase said otherwise.
static void GiveBack (CMUSHclientDoc & doc, const int c = -1)
  {
  CProtocolState & state = *doc.m_pProto;
  std::string text = state.mspBuffer;
  state.mspBuffer.clear ();
  doc.m_phase = NONE;
  if (c >= 0)
    text += (char) c;
  if (text.empty ())
    return;
  state.bMspReplaying = true;
  doc.DisplayMsg (text.c_str (), (int) text.size (), 0, true);
  state.bMspReplaying = false;
  }

bool Proto_MspByte (CMUSHclientDoc & doc, const unsigned char c)
  {
  CProtocolState & state = *doc.m_pProto;

  if (doc.m_phase == HAVE_MSP_AFTER)
    {
    // More on the line after the ).  The spec says there should not be, but it is the
    // server's text, so it shows.
    doc.m_phase = NONE;
    return false;
    }

  const std::string candidate = state.mspBuffer + (char) c;

  static const std::string kSound = "!!SOUND(";
  static const std::string kMusic = "!!MUSIC(";

  if (candidate.size () <= kSound.size ())
    {
    if (kSound.compare (0, candidate.size (), candidate) != 0 &&
        kMusic.compare (0, candidate.size (), candidate) != 0)
      {
      GiveBack (doc, c);
      return true;
      }
    state.mspBuffer = candidate;
    return true;
    }

  if (candidate.size () > proto::kMspMaxTrigger)
    {
    GiveBack (doc, c);
    return true;
    }

  state.mspBuffer = candidate;

  if (c != ')')
    return true;

  // a ) inside quotes is part of a URL
  if (std::count (candidate.begin (), candidate.end (), '"') % 2 ||
      std::count (candidate.begin (), candidate.end (), '\'') % 2)
    return true;

  const bool bMusic = candidate.compare (0, kMusic.size (), kMusic) == 0;
  const std::string inner = candidate.substr (kSound.size (), candidate.size () - kSound.size () - 1);

  proto::MspTrigger trigger;
  if (!proto::ParseMspTrigger (inner, bMusic, &trigger))
    {
    GiveBack (doc);        // !!SOUND() is not a trigger (and the ) is in the buffer)
    return true;
    }

  // Without negotiation, a line starting !!SOUND( is as likely to be a player's.
  trigger.bMayDownload = state.bMspNegotiated;

  state.mspBuffer.clear ();
  doc.m_phase = HAVE_MSP_AFTER;
  Proto_MspPlay (doc, trigger);
  return true;
  }

bool Proto_MspInterrupted (CMUSHclientDoc & doc, const unsigned char c)
  {
  if (doc.m_phase == HAVE_MSP_TRIGGER)
    {
    GiveBack (doc, c);     // and the line ending, ESC or IAC with it, as above
    return true;
    }

  // HAVE_MSP_AFTER: the line ending goes with the trigger, which was the whole line.
  if (c == '\r')
    return true;

  doc.m_phase = NONE;

  if (c != '\n')
    return false;

  if (doc.m_bMXP && !doc.m_bPuebloActive)
    doc.MXP_mode_change (-1);
  return true;
  }

// ---------------------------------------------------------------- playing

void Proto_MspPlay (CMUSHclientDoc & doc, const proto::MspTrigger & triggerIn)
  {
  CProtocolState & state = *doc.m_pProto;

  // From here on T= is either safe or nothing, for finding, for repeats and for what a
  // download, now or once a host is allowed, writes to.
  proto::MspTrigger trigger = triggerIn;
  if (!proto::IsSafeMspName (trigger.type, false))
    trigger.type.clear ();

  CMspVoice & voice = trigger.bMusic ? state.music : state.sound;

  if (trigger.bOff)
    {
    // spec: Off with U= says where things are, and Off alone stops
    if (!trigger.url.empty ())
      {
      if (trigger.bMayDownload)
        state.mspDefaultUrl = trigger.url;
      }
    else
      StopVoice (doc, voice);
    return;
    }

  if (!proto::IsSafeMspName (trigger.name, true))
    {
    if (!state.bMspBadNameNoted)
      {
      state.bMspBadNameNoted = true;
      Proto_Note (doc, "This server asked for a sound by a name that is not safe to use, "
                       "and it was ignored.");
      }
    return;
    }

  const std::string type = trigger.type;
  const std::string name = proto::MspWithDefaultExtension (trigger.name, trigger.bMusic);
  const bool bPlaying = VoicePlaying (doc, voice);

  // Spec: a sound only displaces one of lower priority.  On a tie, what is playing plays.
  if (!trigger.bMusic && bPlaying && trigger.priority <= voice.priority)
    return;

  // Spec: the same music with C=1 carries on, with the new repeats and volume.
  if (trigger.bMusic && bPlaying && trigger.cont && voice.pattern == name)
    {
    voice.volume = trigger.volume;
    voice.repeatsLeft = trigger.repeats < 0 ? -1 : trigger.repeats - 1;
    if (!state.bTestSound)
      doc.PlaySound ((short) voice.buffer, "", trigger.repeats < 0,
                     proto::MspVolumeToDb (voice.volume), 0.0);
    return;
    }

  // Find it.  For a kind of file we cannot play, a sibling we can is what the player
  // would want to hear; failing that it is one quiet note and no sound.
  std::string file;
  if (IsUnplayableName (name))
    {
    const size_t dot = name.rfind ('.');
    for (const char * extension : kAlternateExtensions)
      if (file.empty ())
        file = FindSoundFile (doc, name.substr (0, dot) + extension, type);
    if (file.empty () && !FindSoundFile (doc, name, type).empty () && !state.bMidiNoted)
      {
      state.bMidiNoted = true;
      Proto_Note (doc, "This server played " + name + ": MIDI and Ogg files are not supported "
                       "(WAV, MP3 and FLAC are). There will be no more notes about this.");
      }
    }
  else
    file = FindSoundFile (doc, name, type);

  if (file.empty ())
    {
    StartDownload (doc, trigger, name);
    return;
    }

  if (bPlaying)
    StopBuffer (doc, voice.buffer);

  voice.pattern = name;
  voice.type = type;
  voice.priority = trigger.priority;
  voice.volume = trigger.volume;
  voice.bActive = bPlaying;      // keep its buffer

  const bool bWildcard = name.find_first_of ("*?") != std::string::npos;

  // A DirectSound loop is seamless, so use it for "for ever", unless each pass is to
  // choose a new file.
  const bool bLoop = trigger.repeats < 0 && !bWildcard;
  voice.repeatsLeft = trigger.repeats < 0 ? -1 : trigger.repeats - 1;

  StartVoice (doc, voice, file, bLoop);
  voice.bLooping = bLoop;
  }

void Proto_MspTick (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  // repeats
  for (CMspVoice * voice : { &state.sound, &state.music })
    {
    if (!voice->bActive || voice->bLooping)
      continue;
    if (BufferBusy (doc, voice->buffer))
      continue;

    if (voice->repeatsLeft == 0)
      {
      voice->bActive = false;
      continue;
      }

    const std::string file = FindSoundFile (doc, voice->pattern, voice->type);
    if (file.empty ())
      {
      voice->bActive = false;
      continue;
      }
    if (voice->repeatsLeft > 0)
      voice->repeatsLeft--;
    StartVoice (doc, *voice, file, false);
    }

  // finished downloads
  std::deque <CMspDownloads::Result> done;
    {
    std::lock_guard <std::mutex> guard (state.downloads->lock);
    done.swap (state.downloads->done);
    }

  for (const CMspDownloads::Result & result : done)
    {
    state.mspInFlight.erase (result.name);

    if (!result.error.empty ())
      {
      state.mspFailed.insert (result.name);
      Proto_Note (doc, "Could not download the sound " + result.name + ": " + result.error);
      continue;
      }

    // Music can start late and still be right.  A sword's clang cannot, so a sound is
    // left there for next time.
    if (state.mspPendingMusic && !state.music.bActive)
      {
      const proto::MspTrigger trigger = *state.mspPendingMusic;
      state.mspPendingMusic.reset ();
      Proto_MspPlay (doc, trigger);
      }
    }
  }

void Proto_MspStopAll (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  StopVoice (doc, state.sound);
  StopVoice (doc, state.music);
  state.mspPendingMusic.reset ();
  }

void Proto_MspAllowHost (CMUSHclientDoc & doc, const std::string & host)
  {
  CProtocolState & state = *doc.m_pProto;

  // only ever what was offered, in that note
  if (!state.mspHostsAsked.count (host))
    return;

  doc.m_strMSPAllowedHosts = proto::HostListAdd ((const char *) doc.m_strMSPAllowedHosts, host).c_str ();
  doc.SetModifiedFlag (TRUE);

  Proto_Note (doc, "Sounds can now be downloaded from " + host + " for this world, once saved "
                   "(the option msp_allowed_hosts).");
  Proto_FlushNotes (doc);

  std::vector <std::pair <std::string, proto::MspTrigger> > waiting;
  waiting.swap (state.mspWaiting);

  for (const auto & item : waiting)
    if (item.first == host)
      {
      const std::string name = proto::MspWithDefaultExtension (item.second.name, item.second.bMusic);
      StartDownload (doc, item.second, name);
      }
    else
      state.mspWaiting.push_back (item);
  }
