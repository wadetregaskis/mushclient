// doc_scrollback_shared.cpp -- saving a world's output buffer between sessions.
//
// scrollback_store.* is the file; this is the half that knows what a CLine is.
//
// What gets saved: every finished line, in the order it appeared, with its styles
// and everything else that can be seen through GetLineInfo, as it looked after the
// triggers had finished with it.  Not the banner a session starts with, or the line
// marking where the restored text ends, because the next session prints its own.  Not
// a line marked m_bNotSaved: what an alias echoed of a command typed with echo off.
//
// What comes back differently, on purpose:
//
//   * LOG_LINE is off.  The line was logged, if it was going to be, last time.  Opening
//     a log replays the buffer, and skips these (see OpenLog).
//   * Links that SEND something are not links any more: clicking one would send a
//     command to a game that has moved on.  They keep their look, and lose the action.
//     (ACTION_PROMPT too.)  Hyperlinks to web pages and the like still work.
//   * The high-performance time stamp means nothing after a reboot, so it is rebuilt
//     from the line's wall-clock time.  GetLineInfo (n, 13), "seconds since the world
//     started", is therefore negative for a restored line: how long before this
//     session it arrived.  GetLineInfo (n, 12) is correspondingly earlier than this
//     session's lines, and no more precise than a second.
//   * The last line gets a hard return, whatever it had: with no more text coming,
//     a new line must not be taken for its continuation by a trigger.
//
// None of this goes through DisplayMsg or the line-processing path, so no trigger,
// plugin callback or log sees a restored line arrive.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scrollback_store.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

extern char working_dir [_MAX_PATH];

namespace {

// How often a world with new lines hands them over.  A crash loses at most this much
// of the buffer.  Not less, because every hand-over can become a small block.
const int kFlushSeconds = 10;

// How much to encode in one tick, so that saving a whole huge buffer in one go (when
// the option has just been turned on) spreads over several and can't stall play.
const size_t kMaxLinesPerTick = 40000;

// A file's line numbers carry on from where they got to, session after session.  They
// are `long`, which is 32 bits on Windows; a long way short of that, start again.
const int64_t kRenumberAbove = 1000000000;

// What CStyle::iFlags says about clicking on it.
bool ActionSurvivesRestore (const unsigned int iFlags)
  {
  const unsigned int iAction = iFlags & ACTIONTYPE;
  return iAction == ACTION_NONE || iAction == ACTION_HYPERLINK;
  }

} // anonymous namespace

class CScrollbackSession
  {
  public:
  std::unique_ptr<scrollback::Writer> writer;
  long   savedThrough = 0;      // the last line number handed to the writer
  bool   bUtf8 = false;
  time_t lastFlush = 0;
  long   liveFrom = 0;          // what the writer was last told about the buffer's head
  std::string strReportedError;   // the last trouble put in the window
  bool   bRenumbered = false;
  };


namespace {

// An ID is hexadecimal when we make it, but a world file can say anything.
CString SafeForAFileName (const CString & strID)
  {
  CString strSafe;
  for (int i = 0; i < strID.GetLength (); i++)
    {
    const char c = strID [i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')
      strSafe += c;
    }
  return strSafe;
  }

// FNV-1a, as world_backups.cpp has it: "a/x.mcl" and "A\x.mcl" are one world file.
unsigned long PathHash (const CString & strPath)
  {
  unsigned long hash = 2166136261u;
  for (int i = 0; i < strPath.GetLength (); i++)
    {
    const char c = strPath [i];
    hash ^= (unsigned char) (c == '\\' ? '/' : tolower ((unsigned char) c));
    hash = (hash * 16777619u) & 0xFFFFFFFFu;
    }
  return hash;
  }

#ifdef _WIN32

std::wstring Wide (const CString & strPath)
  {
  const int n = MultiByteToWideChar (CP_ACP, 0, strPath, -1, NULL, 0);
  if (n <= 0)
    return std::wstring ();
  std::wstring wide ((size_t) n, L'\0');
  MultiByteToWideChar (CP_ACP, 0, strPath, -1, &wide [0], n);
  wide.resize ((size_t) n - 1);
  return wide;
  }

CString Narrow (const std::wstring & wide)
  {
  const int n = WideCharToMultiByte (CP_ACP, 0, wide.c_str (), -1, NULL, 0, NULL, NULL);
  if (n <= 0)
    return CString ();
  std::string narrow ((size_t) n, '\0');
  WideCharToMultiByte (CP_ACP, 0, wide.c_str (), -1, &narrow [0], n, NULL, NULL);
  return CString (narrow.c_str ());
  }

// Whole, with long names in place of 8.3 ones as far as they exist, and backslashes.
CString NormalPath (const CString & strPath)
  {
  std::wstring wide = Wide (strPath);
  if (wide.empty ())
    return strPath;
  DWORD n = GetFullPathNameW (wide.c_str (), 0, NULL, NULL);
  if (n > 0)
    {
    std::wstring full ((size_t) n, L'\0');
    n = GetFullPathNameW (wide.c_str (), n, &full [0], NULL);
    if (n > 0 && n < full.size ())
      {
      full.resize (n);
      wide = full;
      }
    }
  DWORD m = GetLongPathNameW (wide.c_str (), NULL, 0);
  if (m > 0)
    {
    std::wstring longer ((size_t) m, L'\0');
    m = GetLongPathNameW (wide.c_str (), &longer [0], m);
    if (m > 0 && m < longer.size ())
      {
      longer.resize (m);
      wide = longer;
      }
    }
  return Narrow (wide);
  }

// The volume and the file on it, which is the same for a file however it's reached: a
// mapped drive or its UNC name, subst, a junction, a hard link.  False if it can't say.
bool FileIdentity (const CString & strPath, BY_HANDLE_FILE_INFORMATION & info)
  {
  const HANDLE h = CreateFileW (Wide (strPath).c_str (), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  const bool bOK = GetFileInformationByHandle (h, &info) != 0;
  CloseHandle (h);
  return bOK && (info.nFileIndexHigh || info.nFileIndexLow);
  }

bool SamePath (const CString & a, const CString & b)
  {
  BY_HANDLE_FILE_INFORMATION infoA, infoB;
  if (FileIdentity (a, infoA) && FileIdentity (b, infoB))
    return infoA.dwVolumeSerialNumber == infoB.dwVolumeSerialNumber &&
           infoA.nFileIndexHigh == infoB.nFileIndexHigh && infoA.nFileIndexLow == infoB.nFileIndexLow;
  return NormalPath (a).CompareNoCase (NormalPath (b)) == 0;
  }

enum class Presence { There, Gone, CantTell };

// Whether a world file is still where it was.  "Can't tell" (a share or a drive that isn't
// there just now, no permission to look) is not "gone": a copy of that world mustn't take
// its files because a network was down when it opened.
Presence WorldFilePresence (const CString & strPath)
  {
  const std::wstring wide = Wide (NormalPath (strPath));
  if (GetFileAttributesW (wide.c_str ()) != INVALID_FILE_ATTRIBUTES)
    return Presence::There;
  switch (GetLastError ())
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
      return Presence::Gone;
    case ERROR_PATH_NOT_FOUND:
      {
      // A folder that has gone, or a drive or share that is not there.  Only the drive or
      // share being there says which.
      std::wstring root;
      if (wide.compare (0, 2, L"\\\\") == 0)
        {
        // \\server\share\ (or \\?\... , which NormalPath doesn't make)
        size_t at = wide.find (L'\\', 2);
        if (at != std::wstring::npos)
          at = wide.find (L'\\', at + 1);
        root = at == std::wstring::npos ? wide + L"\\" : wide.substr (0, at + 1);
        }
      else if (wide.size () >= 3 && wide [1] == L':')
        root = wide.substr (0, 3);
      if (!root.empty () && GetFileAttributesW (root.c_str ()) != INVALID_FILE_ATTRIBUTES)
        return Presence::Gone;
      return Presence::CantTell;
      }
    default:      // ERROR_BAD_NETPATH, ERROR_BAD_NET_NAME, ERROR_NOT_READY, ERROR_NETWORK_UNREACHABLE,
      return Presence::CantTell;    // ERROR_ACCESS_DENIED and the rest
    }
  }

#else   // POSIX: uncompiled here

CString NormalPath (const CString & strPath)
  {
  char resolved [PATH_MAX];
  if (realpath (strPath, resolved))
    return CString (resolved);
  return strPath;
  }

bool SamePath (const CString & a, const CString & b)
  {
  struct stat sa, sb;
  if (stat (a, &sa) == 0 && stat (b, &sb) == 0)
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
  return NormalPath (a) == NormalPath (b);
  }

enum class Presence { There, Gone, CantTell };

Presence WorldFilePresence (const CString & strPath)
  {
  struct stat st;
  if (stat (strPath, &st) == 0)
    return Presence::There;
  return errno == ENOENT || errno == ENOTDIR ? Presence::Gone : Presence::CantTell;
  }

#endif

bool FileExists (const CString & strPath)
  {
  if (FILE * f = fopen (strPath, "rb"))
    {
    fclose (f);
    return true;
    }
  return false;
  }

// The world file a world's files were kept for: one line, the path.
CString ReadOwner (const CString & strPath)
  {
  CString strOwner;
  if (FILE * f = fopen (strPath, "rb"))
    {
    char buffer [4096];
    const size_t n = fread (buffer, 1, sizeof buffer - 1, f);
    fclose (f);
    buffer [n] = 0;
    strOwner = buffer;
    strOwner.TrimRight ("\r\n");
    }
  return strOwner;
  }

CString ScrollbackFolder ()
  {
  CString strFolder (working_dir);
#ifdef _WIN32
  strFolder += "scrollback\\";
#else
  strFolder += "scrollback/";
#endif
  return strFolder;
  }

CString ByPathBase (const CString & strFolder, const CString & strWorld)
  {
  CString strByPath;
  strByPath.Format ("%sw-%08lx", (LPCTSTR) strFolder, PathHash (strWorld));
  return strByPath;
  }

bool HasSavedFiles (const CString & strBase)
  {
  return FileExists (strBase + ".mcsb") || FileExists (strBase + ".history");
  }

} // anonymous namespace


// Which files are this world's.  They are named by its ID, which a world file says,
// and a copy of a world file says the same: an alt's world made by Save As or by
// copying the file would otherwise be given the original's output and command history,
// and the two would overwrite each other's.  So <id>.owner says which world file the
// files under <id> are for (the same file, however it's reached: see SamePath).  A world
// file that isn't that one gets w-<hash of its path> instead, unless the owner has gone
// (it was renamed or moved, so this is it), and this one has no files of its own by its
// path, in which case it takes the ID's over.  A copy that has files of its own keeps
// them: the original being deleted doesn't make the copy the original.  An owner that
// can't be looked for (on a share that isn't there just now) is not gone.  With no owner
// at all, the first world to come along with the ID takes it, which is how files from
// before there was an owner are found again, unless, again, it has its own.
//
// Taking the ID's files means writing the owner, which only happens with the option on.
// With it off the answer isn't kept, and the files aren't this world's to delete
// (SavedFilesAreMine): a copy with saving off would otherwise delete its original's.
//
// A world file with no ID gets a new one each time it's opened until it is saved: its
// files are named by its path meanwhile, and taken over by the ID when it has one.  One
// whose ID had to be changed, because another open world had it, goes by the ID in the
// file, which makes it a copy of that other world, as it no doubt is.
CString CMUSHclientDoc::ScrollbackFileBase (void) const
  {
  if (!m_strScrollbackBase.IsEmpty () && m_strScrollbackBaseForID == m_strWorldID)
    return m_strScrollbackBase;

  const CString strFolder = ScrollbackFolder ();
  const CString strWorld = GetPathName ();
  const CString strID = SafeForAFileName (m_bWorldIDIsNew ? m_strWorldIDInFile : m_strWorldID);
  CString strBase;
  bool bSettled = true;
  if (strWorld.IsEmpty ())
    {
    // Never saved: nothing can find these again, and closing deletes them, unless it
    // is saved by then, in which case the next session takes them up.
    const CString strNewID = SafeForAFileName (m_strWorldID);
    if (strNewID.IsEmpty ())
      return CString ();
    strBase = strFolder + strNewID;
    }
  else
    {
    const CString strByPath = ByPathBase (strFolder, strWorld);
    if (strID.IsEmpty ())
      strBase = strByPath;
    else
      {
      const CString strIDBase = strFolder + strID;
      const CString strOwnerFile = strIDBase + ".owner";
      const CString strOwner = ReadOwner (strOwnerFile);
      const bool bMine = !strOwner.IsEmpty () && SamePath (strOwner, strWorld);
      bool bTake = bMine;
      if (!bMine)
        {
        const bool bOwnerGone = strOwner.IsEmpty () || WorldFilePresence (strOwner) == Presence::Gone;
        // Files of its own count only if the ID has some to lose by taking it over.
        bTake = bOwnerGone && !(HasSavedFiles (strByPath) && HasSavedFiles (strIDBase));
        }
      if (!bTake)
        strBase = strByPath;      // a copy of that world
      else
        {
        strBase = strIDBase;
        if (!bMine)
          bSettled = m_bSaveScrollback &&
                     scrollback::WriteFileAtomically ((const char *) strOwnerFile, (LPCTSTR) strWorld,
                                                      (size_t) strWorld.GetLength ());
        // What was kept for it by path, before its ID stuck.
        if (bSettled && !HasSavedFiles (strIDBase))
          {
          rename (strByPath + ".mcsb", strIDBase + ".mcsb");
          rename (strByPath + ".history", strIDBase + ".history");
          }
        }
      }
    }

  if (bSettled)
    {
    m_strScrollbackBase = strBase;
    m_strScrollbackBaseForID = m_strWorldID;
    m_strScrollbackBaseForPath = strWorld;
    }
  return strBase;
  } // end of CMUSHclientDoc::ScrollbackFileBase


bool CMUSHclientDoc::SavedFilesAreMine (void) const
  {
  const CString strBase = ScrollbackFileBase ();
  if (strBase.IsEmpty () || m_strScrollbackBase != strBase || m_strScrollbackBaseForID != m_strWorldID)
    return false;     // not decided for good, so not taken
  const CString strWorld = m_strScrollbackBaseForPath;
  if (strWorld.IsEmpty ())
    return true;      // a fresh ID, which nothing else has
  if (strBase == ByPathBase (ScrollbackFolder (), strWorld))
    return true;
  const CString strOwner = ReadOwner (strBase + ".owner");
  return !strOwner.IsEmpty () && SamePath (strOwner, strWorld);
  } // end of CMUSHclientDoc::SavedFilesAreMine


CString CMUSHclientDoc::ScrollbackFilePath (void) const
  {
  const CString strBase = ScrollbackFileBase ();
  return strBase.IsEmpty () ? strBase : strBase + ".mcsb";
  }


CString CMUSHclientDoc::CommandHistoryFilePath (void) const
  {
  const CString strBase = ScrollbackFileBase ();
  return strBase.IsEmpty () ? strBase : strBase + ".history";
  } // end of CMUSHclientDoc::CommandHistoryFilePath


bool CMUSHclientDoc::RestoreSavedScrollback (void)
  {
  if (!m_bSaveScrollback)
    {
    m_bScrollbackWanted = true;    // ScrollbackTick looks after it going on later
    return false;
    }
  if (m_pScrollback)
    return false;

  const CString strPath = ScrollbackFilePath ();
  if (strPath.IsEmpty ())
    return false;

  // The cap leaves room for the session's first lines, because evicting a chunk
  // before the views are up would be a pointless way to start.
  long iPixelRows = (long) (INT_MAX / (m_FontHeight > 0 ? m_FontHeight : 1)) - 2 * JUMP_SIZE;
  long iMaxLines = iPixelRows;
  if (m_maxlines > 0 && m_maxlines - JUMP_SIZE < iMaxLines)
    iMaxLines = m_maxlines - JUMP_SIZE;
  if (iMaxLines < 1)
    iMaxLines = 1;

  scrollback::LoadResult load = scrollback::Load ((const char *) strPath, m_bUTF_8 != 0, (size_t) iMaxLines);

  if (load.status == scrollback::LoadStatus::NewerVersion ||
      load.status == scrollback::LoadStatus::CouldNotRead)
    return false;     // not ours to touch, and not to be appended to

  // Build the lines.  The blocks are independent, and building a line touches nothing
  // shared but the heap, so this goes across as many threads as Load's decoding did.
  // Newest first, each block from its newest line back, so that if memory runs out it is
  // the oldest lines that are done without (see below).
  const size_t iBlocks = load.blocks.size ();
  std::vector<std::vector<CLine *>> built (iBlocks);   // in order; null until built
  std::vector<size_t> firstBuilt (iBlocks, 0);           // built [b] is built from here on
  struct PendingAction
    {
    size_t slot;               // in built [b]
    CStyle * pStyle;
    const scrollback::Action * pAction;
    };
  std::vector<std::vector<PendingAction>> actions (iBlocks);

  const time_t tStarted = m_whenWorldStarted.GetTime ();
  const LONGLONG iStartedHP = m_whenWorldStartedHighPrecision.QuadPart;
  const LONGLONG iFrequency = App.m_iCounterFrequency;

  // The newest line number in the file, whether or not it comes back: this session's
  // go on from there, or they'd be taken for replacements of the saved ones.
  long iFileLast = 0;
  if (!load.liveBlocks.empty ())
    {
    const scrollback::BlockInfo & newest = load.liveBlocks.back ();
    iFileLast = (long) (newest.cutFrom ? newest.cutFrom - 1 : newest.last);
    }
  if (!load.blocks.empty () && (long) load.blocks.back ()->lines.back ().number > iFileLast)
    iFileLast = (long) load.blocks.back ()->lines.back ().number;

  bool bRenumber = iFileLast > kRenumberAbove;
  long iNextNumber = 1;
  std::vector<long> firstNumbers (iBlocks);
  for (size_t b = 0; b < iBlocks; b++)
    {
    firstNumbers [b] = iNextNumber;
    iNextNumber += (long) load.blocks [b]->lines.size () - (b == 0 ? (long) load.firstLineInFirstBlock : 0);
    }

  // Builds what is left of block b, newest line first.  False if memory ran out, which
  // (a huge buffer in a 32-bit process) must not leave a thread as an exception: that
  // would end the client, and would every time the world was opened.  What it built
  // before then stays built.
  auto buildBlock = [&] (const size_t b) -> bool
    {
    const scrollback::DecodedBlock & block = *load.blocks [b];
    const size_t iFirst = b == 0 ? load.firstLineInFirstBlock : 0;
    std::vector<CLine *> & lines = built [b];
    if (lines.empty ())
      {
      try
        {
        lines.assign (block.lines.size () - iFirst, nullptr);
        }
      catch (...)
        {
        return false;
        }
      firstBuilt [b] = lines.size ();
      }
    while (firstBuilt [b] > 0)
      {
      const size_t slot = firstBuilt [b] - 1;
      const scrollback::Line & line = block.lines [iFirst + slot];
      CLine * pLine = nullptr;
      try
        {
        pLine = new CLine (bRenumber ? firstNumbers [b] + (long) slot : (long) line.number,
                           CTime ((time_t) line.time),
                           iStartedHP + (LONGLONG) (line.time - tStarted) * iFrequency,
                           (unsigned char) (line.flags & ~LOG_LINE),
                           line.hardReturn,
                           line.charCount,
                           (short) line.preamble,
                           block.text + line.textOffset,
                           (int) line.length);
        for (uint32_t s = 0; s < line.styleCount; s++)
          {
          const scrollback::Style & saved = block.styles [line.firstStyle + s];
          CStyle * pStyle = new CStyle;
          pStyle->iLength = (unsigned short) saved.length;
          pStyle->iFlags = (unsigned short) saved.flags;
          pStyle->iForeColour = saved.fore;
          pStyle->iBackColour = saved.back;
          if (!ActionSurvivesRestore (saved.flags))
            pStyle->iFlags &= ~ACTIONTYPE;
          try
            {
            pLine->styleList.AddTail (pStyle);
            }
          catch (...)
            {
            delete pStyle;
            throw;
            }
          if (ActionSurvivesRestore (saved.flags) && saved.action >= 0)
            actions [b].push_back (PendingAction { slot, pStyle, &block.actions [(size_t) saved.action] });
          }
        }
      catch (...)
        {
        // Its styles go with it, so their actions mustn't be left for a later try at it.
        while (!actions [b].empty () && actions [b].back ().slot == slot)
          actions [b].pop_back ();
        delete pLine;
        return false;
        }
      lines [slot] = pLine;
      firstBuilt [b] = slot;
      }
    return true;
    };

  // Lines of b and every block before it go: there's a gap after them.
  auto letGoUpTo = [&] (const size_t b)
    {
    for (size_t gone = 0; gone < b; gone++)
      {
      for (size_t slot = firstBuilt [gone]; slot < built [gone].size (); slot++)
        delete built [gone] [slot];
      built [gone].clear ();
      firstBuilt [gone] = 0;
      actions [gone].clear ();
      }
    };

  std::atomic<size_t> next (0);
  std::atomic<size_t> failedAbove (0);     // 1 + the newest block that ran out, or 0
  auto buildBlocks = [&] ()
    {
    for (;;)
      {
      const size_t n = next.fetch_add (1);
      if (n >= iBlocks)
        return;
      const size_t b = iBlocks - 1 - n;
      if (b + 1 < failedAbove.load ())
        continue;     // older than one that ran out: no use
      if (!buildBlock (b))
        {
        size_t seen = failedAbove.load ();
        while (seen < b + 1 && !failedAbove.compare_exchange_weak (seen, b + 1))
          {
          }
        }
      }
    };

  unsigned iThreads = scrollback::ThreadsToUse ();
  if (iBlocks < iThreads)
    iThreads = iBlocks > 0 ? (unsigned) iBlocks : 1;
  std::vector<std::thread> pool;
  try
    {
    for (unsigned t = 1; t < iThreads; t++)
      pool.emplace_back (buildBlocks);
    }
  catch (...)
    {
    }     // fewer threads, then
  buildBlocks ();
  for (std::thread & t : pool)
    t.join ();

  // What comes back is the newest lines, without a gap, as many as fit; the file keeps
  // them all.  Other threads were building older blocks when one ran out, which is no use
  // past the gap and may be what it ran out for: they go, and that block and the ones
  // before it carry on, one at a time, until there's no more room.
  if (failedAbove.load () > 0)
    {
    const size_t newest = failedAbove.load () - 1;
    letGoUpTo (newest);
    for (size_t b = newest + 1; b-- > 0; )
      if (!buildBlock (b))
        {
        letGoUpTo (b);
        break;
        }
    }

  // Actions are the document's, and shared between styles.
  for (size_t b = 0; b < iBlocks; b++)
    for (const PendingAction & a : actions [b])
      if (a.slot >= firstBuilt [b])
        a.pStyle->pAction = GetAction (a.pAction->action.c_str (), a.pAction->hint.c_str (), a.pAction->variable.c_str ());

  // Into the buffer.
  m_LinePositions.BeginRebuild ();
  long iCount = 0;
  for (size_t b = 0; b < iBlocks; b++)
    for (size_t slot = firstBuilt [b]; slot < built [b].size (); slot++)
      {
      const POSITION pos = m_LineList.AddTail (built [b] [slot]);
      if (iCount % JUMP_SIZE == 0)
        m_LinePositions.Append (pos);
      iCount++;
      }

  if (iCount > 0)
    {
    CLine * pLast = m_LineList.GetTail ();
    pLast->hard_return = true;
    m_total_lines = pLast->m_nLineNumber;
    m_nLastRestoredLine = pLast->m_nLineNumber;
    }
  else
    m_LinePositions.Reset (m_LineList.GetHeadPosition ());
  if (!bRenumber && iFileLast > m_total_lines)
    m_total_lines = iFileLast;

  // And carry on writing.  A second copy of the client with this world open means no
  // writer: what it restored is still worth seeing, but it can't save.  Nor if the files
  // couldn't be claimed (the owner file couldn't be written).
  std::unique_ptr<scrollback::Writer> writer;
  if (SavedFilesAreMine ())
    writer.reset (new scrollback::Writer ((const char *) strPath, m_bUTF_8 != 0, load));
  if (writer && writer->IsOpen ())
    {
    m_pScrollback = new CScrollbackSession;
    m_pScrollback->writer = std::move (writer);
    m_pScrollback->bUtf8 = m_bUTF_8 != 0;
    m_pScrollback->lastFlush = time (NULL);
    m_pScrollback->bRenumbered = bRenumber;
    if (bRenumber)
      m_pScrollback->writer->Clear (m_bUTF_8 != 0);  // they'll all go again under their new numbers
    else
      m_pScrollback->savedThrough = m_total_lines;
    }

  return iCount > 0;
  } // end of CMUSHclientDoc::RestoreSavedScrollback


void CMUSHclientDoc::ScrollbackSessionStarted (void)
  {
  // m_pCurrentLine is the empty line the banner left for what comes next: everything
  // before it is done with.  If the restored lines were renumbered they all still have
  // to go out again, and one session's banner goes with them, which is no great harm.
  if (m_pScrollback && !m_pScrollback->bRenumbered && m_pCurrentLine)
    m_pScrollback->savedThrough = m_pCurrentLine->m_nLineNumber - 1;
  } // end of CMUSHclientDoc::ScrollbackSessionStarted


void CMUSHclientDoc::ScrollbackTick (void)
  {
  // The option has been turned on (World Properties, or SetOption).
  if (!m_pScrollback)
    {
    if (m_bSaveScrollback && m_LinePositions.IsAllocated () && m_bScrollbackWanted)
      {
      m_bScrollbackWanted = false;
      const CString strPath = ScrollbackFilePath ();
      if (!strPath.IsEmpty () && SavedFilesAreMine ())
        {
        scrollback::LoadResult none;    // anything already there is replaced
        std::unique_ptr<scrollback::Writer> writer (new scrollback::Writer ((const char *) strPath, m_bUTF_8 != 0, none));
        if (writer->IsOpen ())
          {
          m_pScrollback = new CScrollbackSession;
          m_pScrollback->writer = std::move (writer);
          m_pScrollback->bUtf8 = m_bUTF_8 != 0;
          }
        }
      }
    return;
    }

  // Turned off: stop, and don't leave what was saved lying around.
  if (!m_bSaveScrollback)
    {
    m_pScrollback->writer->Close (SavedFilesAreMine ());    // a file that is still ours, that is
    delete m_pScrollback;
    m_pScrollback = NULL;
    m_bScrollbackWanted = true;
    return;
    }

  // The UTF-8 option changed under us.  The lines in the buffer are in the old encoding
  // and the file's header says so; start again from here.
  if ((m_bUTF_8 != 0) != m_pScrollback->bUtf8)
    {
    m_pScrollback->bUtf8 = m_bUTF_8 != 0;
    m_pScrollback->writer->Clear (m_pScrollback->bUtf8);
    m_pScrollback->savedThrough = m_pCurrentLine ? m_pCurrentLine->m_nLineNumber - 1 : m_total_lines;
    return;
    }

  // Nothing new, which on an idle world is every time.
  const long iPending = m_total_lines - m_pScrollback->savedThrough;
  if (iPending <= 1)     // the line in progress doesn't count
    return;

  const time_t tNow = time (NULL);
  if (iPending < (long) scrollback::kBlockLines && tNow - m_pScrollback->lastFlush < kFlushSeconds)
    return;

  FlushSavedScrollback (false);
  m_pScrollback->lastFlush = tNow;
  } // end of CMUSHclientDoc::ScrollbackTick


void CMUSHclientDoc::FlushSavedScrollback (const bool bClosing)
  {
  if (!m_pScrollback)
    return;

  CScrollbackSession & session = *m_pScrollback;

  // What is handed over below could set off a rewrite of the whole file to reclaim space,
  // and the close waits for the worker.  That can as well happen next session.
  if (bClosing)
    session.writer->StopReclaiming ();

  // Find what hasn't been saved, walking back from the end: the first line of it, and
  // the last line that is finished with.  A wrapped line is not finished until its
  // last row is, because a trigger can still omit the lot.  Closing, everything counts.
  POSITION posFirst = NULL;
  POSITION posLast = NULL;
  for (POSITION pos = m_LineList.GetTailPosition (); pos; )
    {
    const POSITION here = pos;
    const CLine * pLine = m_LineList.GetPrev (pos);
    if (pLine->m_nLineNumber <= session.savedThrough)
      break;
    if (!posLast &&
        (bClosing ? (pLine != m_pCurrentLine || pLine->len > 0)
                  : (pLine != m_pCurrentLine && pLine->hard_return)))
      posLast = here;
    posFirst = here;
    }

  if (posLast)
    {
    scrollback::BlockBuilder builder;
    const CAction * pLastAction = NULL;
    int32_t iLastAction = -1;
    size_t iTotal = 0;
    long iThrough = session.savedThrough;    // the last line dealt with, saved or not

    auto handOver = [&] ()
      {
      const int64_t iFirstNumber = builder.FirstNumber ();
      const int64_t iLastNumber = builder.LastNumber ();
      const size_t iLines = builder.LineCount ();
      session.writer->AppendLines (builder.Finish (), iLines, iFirstNumber, iLastNumber);
      session.savedThrough = (long) iLastNumber;
      pLastAction = NULL;     // the action table went with the block
      };

    for (POSITION pos = posFirst; pos; )
      {
      const POSITION here = pos;
      const CLine * pLine = m_LineList.GetNext (pos);

      // Drawn, but typed with echo off.  Its number is simply missing from the file.
      if (pLine->m_bNotSaved)
        {
        iThrough = pLine->m_nLineNumber;
        iTotal++;
        if (here == posLast || (!bClosing && iTotal >= kMaxLinesPerTick))
          break;
        continue;
        }

      builder.AddLine (pLine->m_nLineNumber, pLine->m_theTime.GetTime (), pLine->flags, pLine->hard_return,
                       pLine->m_iCharCount, pLine->m_iPreambleOffset, pLine->text, (size_t) pLine->len);

      for (POSITION stylepos = pLine->styleList.GetHeadPosition (); stylepos; )
        {
        const CStyle * pStyle = pLine->styleList.GetNext (stylepos);
        int32_t iAction = -1;
        // Actions that won't be restored needn't be kept, and at least one of them
        // (a send link) is liable to be a command someone wouldn't want lying around.
        if (pStyle->pAction && ActionSurvivesRestore (pStyle->iFlags))
          {
          if (pStyle->pAction != pLastAction)
            {
            pLastAction = pStyle->pAction;
            iLastAction = builder.AddAction ((const char *) pLastAction->m_strAction,
                                             (const char *) pLastAction->m_strHint,
                                             (const char *) pLastAction->m_strVariable);
            }
          iAction = iLastAction;
          }
        builder.AddStyle (pStyle->iLength, pStyle->iFlags, pStyle->iForeColour, pStyle->iBackColour, iAction);
        }

      iTotal++;
      iThrough = pLine->m_nLineNumber;
      if (builder.LineCount () >= scrollback::kBlockLines)
        handOver ();
      if (here == posLast || (!bClosing && iTotal >= kMaxLinesPerTick))
        break;
      }

    if (builder.LineCount () > 0)
      handOver ();
    if (iThrough > session.savedThrough)
      session.savedThrough = iThrough;
    }

  // What the buffer no longer holds needn't stay in the file.
  if (!m_LineList.IsEmpty ())
    {
    const long iHead = m_LineList.GetHead ()->m_nLineNumber;
    if (iHead != session.liveFrom)
      {
      session.liveFrom = iHead;
      session.writer->SetLiveFrom (iHead);
      }
    }

  if (bClosing)
    {
    session.writer->Flush ();
    // A world that has never been saved has no way to find this again, unless it is
    // saved after all (at which point this hasn't happened yet: this is the close).
    const bool bForget = m_strPathName.IsEmpty () || !m_bSaveScrollback;
    session.writer->Close (bForget && SavedFilesAreMine ());
    delete m_pScrollback;
    m_pScrollback = NULL;
    return;
    }

  // No dialog from here, ever: this is a timer.  In the window, once for each new
  // trouble, and once when it's over (the worker tries again by itself).
  const std::string strError = session.writer->LastError ();
  if (strError != session.strReportedError)
    {
    if (!strError.empty ())
      Note (TFormat ("Saved output buffer: %s.", strError.c_str ()));
    else
      Note (Translate ("Saved output buffer: saving again."));
    session.strReportedError = strError;
    }
  } // end of CMUSHclientDoc::FlushSavedScrollback


void CMUSHclientDoc::ScrollbackTailDeleted (void)
  {
  // Numbers get reused after this, so it can't wait for the next flush to notice.
  const long iLast = m_LineList.IsEmpty () ? 0 : m_LineList.GetTail ()->m_nLineNumber;
  if (m_nLastRestoredLine > iLast)
    m_nLastRestoredLine = iLast;
  if (m_pScrollback && m_pScrollback->savedThrough > iLast)
    {
    m_pScrollback->writer->TruncateFrom ((int64_t) iLast + 1);
    m_pScrollback->savedThrough = iLast;
    }
  } // end of CMUSHclientDoc::ScrollbackTailDeleted


void CMUSHclientDoc::ScrollbackHeadDeleted (void)
  {
  if (m_pScrollback && !m_LineList.IsEmpty ())
    {
    const long iHead = m_LineList.GetHead ()->m_nLineNumber;
    m_pScrollback->liveFrom = iHead;
    m_pScrollback->writer->DropBefore (iHead);
    }
  } // end of CMUSHclientDoc::ScrollbackHeadDeleted


void CMUSHclientDoc::ScrollbackCleared (void)
  {
  m_nLastRestoredLine = 0;
  if (m_pScrollback)
    {
    m_pScrollback->writer->Clear (m_pScrollback->bUtf8);
    m_pScrollback->savedThrough = 0;
    m_pScrollback->liveFrom = 0;
    }
  } // end of CMUSHclientDoc::ScrollbackCleared


void CMUSHclientDoc::ScrollbackLineFlagsChanged (const CLine * pLine)
  {
  if (m_pScrollback && pLine->m_nLineNumber <= m_pScrollback->savedThrough)
    m_pScrollback->writer->SetFlags (pLine->m_nLineNumber, pLine->flags);
  } // end of CMUSHclientDoc::ScrollbackLineFlagsChanged


void CMUSHclientDoc::ClearSavedScrollback (void)
  {
  if (m_pScrollback)
    {
    m_pScrollback->writer->Clear (m_pScrollback->bUtf8);
    m_pScrollback->writer->Flush ();
    // Only what arrives from now on: what is on screen was just asked to be forgotten.
    m_pScrollback->savedThrough = m_pCurrentLine ? m_pCurrentLine->m_nLineNumber - 1 : m_total_lines;
    m_pScrollback->liveFrom = 0;
    return;
    }
  const CString strPath = ScrollbackFilePath ();
  if (!strPath.IsEmpty () && SavedFilesAreMine ())
    {
    scrollback::LoadResult none;
    scrollback::Writer writer ((const char *) strPath, m_bUTF_8 != 0, none);
    writer.Close (true);
    }
  } // end of CMUSHclientDoc::ClearSavedScrollback
