// doc_autosave_shared.cpp -- saving a world a few seconds after it changes.
//
// There is no timer that saves on a schedule.  Anything that changes a world marks it
// modified, and SetModifiedFlag remembers when; CheckTimers looks once a tick, and when
// a world has been quiet for kQuietSeconds, or has been changing without a break for
// kLongestWaitSeconds, it is written.  The delay is a feature: a change that makes the
// client crash has a moment to do so before it is on disk.  And if it gets that far
// regardless, the copy from before it is in the backups (world_backups.h).
//
// A world that has never been saved has no name to save under, and still asks.  With
// this on nothing else does: closing a world saves it if need be, without a question,
// and asks only if that fails.
//
// World variables don't mark a world modified, because scripts change them constantly.
// They are saved too, but on a slower count that nothing restarts.
//
// Saving goes through the same script and plugin callbacks as File > Save, so that
// anything keeping state in world variables gets to bring them up to date.  It doesn't
// call OnPluginSaveState: plugin state is saved when a plugin asks (world.SaveState) or
// closes, as ever, since that runs arbitrary script and a plugin may not expect to be
// asked in the middle of things.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "world_backups.h"

#include <ctime>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

extern char working_dir [_MAX_PATH];

namespace {

const int kQuietSeconds = 7;
const int kLongestWaitSeconds = 60;
const int kVariablesWaitSeconds = 60;
const int kRetryAfterFailureSeconds = 300;

// world_backups takes UTF-8.  Off the 64-bit Windows builds' UTF-8 manifest, narrow
// paths are in the process's code page.
std::string PathToUtf8 (const CString & strPath)
  {
#ifdef _WIN32
  if (GetACP () != CP_UTF8)
    {
    const int nWide = MultiByteToWideChar (CP_ACP, 0, strPath, -1, NULL, 0);
    if (nWide <= 0)
      return std::string ();
    std::wstring wide ((size_t) nWide, L'\0');
    MultiByteToWideChar (CP_ACP, 0, strPath, -1, &wide [0], nWide);
    const int nNarrow = WideCharToMultiByte (CP_UTF8, 0, wide.c_str (), -1, NULL, 0, NULL, NULL);
    if (nNarrow <= 0)
      return std::string ();
    std::string out ((size_t) nNarrow, '\0');
    WideCharToMultiByte (CP_UTF8, 0, wide.c_str (), -1, &out [0], nNarrow, NULL, NULL);
    out.resize ((size_t) nNarrow - 1);
    return out;
    }
#endif
  return std::string ((const char *) strPath);
  }

} // anonymous namespace


void CMUSHclientDoc::SetModifiedFlag (BOOL bModified)
  {
  CDocument::SetModifiedFlag (bModified);

  if (!bModified)
    {
    m_tAutosaveFirstChange = 0;
    m_tAutosaveLastChange = 0;
    }
  else if (!m_bAutosaving)      // what a save's own callbacks do doesn't count
    {
    m_tAutosaveLastChange = time (NULL);
    if (!m_tAutosaveFirstChange)
      m_tAutosaveFirstChange = m_tAutosaveLastChange;
    }
  } // end of CMUSHclientDoc::SetModifiedFlag


CString CMUSHclientDoc::WorldBackupsRoot (void) const
  {
  CString strRoot (working_dir);
#ifdef _WIN32
  strRoot += "backups\\";
#else
  strRoot += "backups/";
#endif
  return strRoot;
  } // end of CMUSHclientDoc::WorldBackupsRoot


CString CMUSHclientDoc::WorldBackupsFolder (void) const
  {
  const std::string strFolder = world_backups::FolderFor (PathToUtf8 (WorldBackupsRoot ()),
                                                          PathToUtf8 (GetPathName ()));
  return CString (strFolder.c_str ());
  } // end of CMUSHclientDoc::WorldBackupsFolder


// Before the world file is overwritten, by anything.
void CMUSHclientDoc::BackUpWorldFileIfDue (const CString & strPath)
  {
  if (!App.m_bAutosaveWorlds || strPath.IsEmpty ())
    return;
  world_backups::BackUpIfDue (PathToUtf8 (WorldBackupsRoot ()), PathToUtf8 (strPath),
                              !m_bBackedUpThisSession, time (NULL));
  m_bBackedUpThisSession = true;
  } // end of CMUSHclientDoc::BackUpWorldFileIfDue


void CMUSHclientDoc::AutosaveTick (void)
  {
  if (!App.m_bAutosaveWorlds)
    return;

  const bool bModified = IsModified () != 0;
  if (!bModified && !m_bVariablesChanged)
    {
    m_tAutosaveFirstChange = 0;
    m_tAutosaveLastChange = 0;
    return;
    }

  const CString strPath = GetPathName ();
  if (strPath.IsEmpty ())
    return;       // it still has to be given a name, and only they can do that

  const time_t tNow = time (NULL);
  if (tNow < m_tAutosaveRetry)
    return;

  // Marked modified some way that didn't come through us, or just variables.
  if (!m_tAutosaveFirstChange)
    {
    m_tAutosaveFirstChange = tNow;
    m_tAutosaveLastChange = tNow;
    }

  const bool bDue = bModified
      ? (tNow - m_tAutosaveLastChange >= kQuietSeconds || tNow - m_tAutosaveFirstChange >= kLongestWaitSeconds)
      : tNow - m_tAutosaveFirstChange >= kVariablesWaitSeconds;
  if (!bDue)
    return;

  // Not while the World Properties dialog or the like is up, or a plugin is in the
  // middle of something: there's no hurry.
  if (m_CurrentPlugin)
    return;
#if defined(_WIN32) && !defined(MUSHCLIENT_TESTS)
  if (App.m_pMainWnd && !App.m_pMainWnd->IsWindowEnabled ())
    return;
#endif

  m_bAutosaving = true;
  m_bAutosaveFailed = false;
  FireWorldSaveCallbacks ();
  BackUpWorldFileIfDue (strPath);
  // OnSaveDocument and not CDocument::DoSave, which would also move this world to the
  // top of the recent files list, every time.
  const BOOL bSaved = OnSaveDocument (strPath);
  m_bAutosaving = false;

  if (bSaved && !m_bAutosaveFailed)
    {
    m_bVariablesChanged = false;
    m_tAutosaveFirstChange = 0;
    m_tAutosaveLastChange = 0;
    m_bAutosaveReportedFailure = false;
    return;
    }

  // Most likely a read-only file.  There's no dialog from a timer, and no use in
  // trying every second.  The world stays modified, so closing it still asks.
  m_tAutosaveRetry = tNow + kRetryAfterFailureSeconds;
  if (!m_bAutosaveReportedFailure)
    {
    m_bAutosaveReportedFailure = true;
    Note (TFormat ("Could not save the world automatically to %s.", (LPCTSTR) strPath));
    }
  } // end of CMUSHclientDoc::AutosaveTick


// What closing a world saves without asking.  Runs from the close prompts, on both
// platforms.
void CMUSHclientDoc::SaveWorldBeforeClosing (void)
  {
  const bool bAutomatically = App.m_bAutosaveWorlds && !GetPathName ().IsEmpty ();
  if (bAutomatically ? (IsModified () || m_bVariablesChanged)
                     : (m_bSaveWorldAutomatically && IsModified ()))
    {
    // A quiet attempt: if it fails the world is still modified, and the question that
    // follows protects it.
    m_bAutosaving = bAutomatically;
    m_bAutosaveFailed = false;
    DoSave (GetPathName (), TRUE);
    m_bAutosaving = false;
    }
  } // end of CMUSHclientDoc::SaveWorldBeforeClosing
