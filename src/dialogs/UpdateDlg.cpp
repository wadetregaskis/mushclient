// UpdateDlg.cpp -- see UpdateDlg.h.

#include "stdafx.h"
#include "../MUSHclient.h"
#include "../mainfrm.h"
#include "../user_data.h"
#include "../update_key.h"
#include "../version.h"
#include "UpdateDlg.h"

#include <fcntl.h>
#include <io.h>

#include <filesystem>
#include <functional>
#include <memory>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

namespace fs = std::filesystem;

namespace {

const UINT WM_UPDATE_DOWNLOAD_PROGRESS = WM_APP + 0x71;

// Set once a portable copy's files have been replaced: the exe to start on the way out.
std::wstring g_runAtExit;

// The one that's up, if one is.
CUpdateDlg * g_pOpenDialog = nullptr;

// UTF-8 (GitHub's) into a control, as text_edges.cpp does a world's text.
void SetUtf8Text (HWND hWnd, const std::string & utf8)
  {
  const int n = MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, NULL, 0);
  if (n <= 0)
    return;
  std::wstring wide ((size_t) n, L'\0');
  MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, &wide [0], n);
  if (GetACP () == CP_UTF8 && !IsWindowUnicode (hWnd))
    SetWindowTextA (hWnd, utf8.c_str ());
  else
    SetWindowTextW (hWnd, wide.c_str ());
  }

std::string WithCrLf (const std::string & text)
  {
  std::string out;
  out.reserve (text.size () + text.size () / 16);
  for (const char c : text)
    {
    if (c == '\n')
      out += '\r';
    out += c;
    }
  return out;
  }

std::string UpdatesFolder ()
  {
  return std::string ((const char *) UserData_Directory ()) + "updates";
  }

void OpenInBrowser (const std::string & url)
  {
  ShellExecuteA (NULL, "open", url.c_str (), NULL, NULL, SW_SHOWNORMAL);
  }

void SaveTheOption ()
  {
  App.SaveGlobalsToDatabase ();
  }

} // anonymous namespace

BEGIN_MESSAGE_MAP (CUpdateDlg, CDialog)
  ON_BN_CLICKED (IDC_UPDATE_INSTALL, OnInstall)
  ON_MESSAGE (WM_UPDATE_DOWNLOAD_PROGRESS, OnDownloadProgress)
END_MESSAGE_MAP ()

CUpdateDlg::CUpdateDlg (CWnd * pParent) : CDialog (IDD_UPDATE, pParent)
  {
  m_pAlive = std::make_shared<bool> (true);
  }

BOOL CUpdateDlg::OnInitDialog ()
  {
  CDialog::OnInitDialog ();

  update::Controller & controller = update::Controller::Get ();
  const update::Offer offer = update::ChooseOffer (controller.Newer (), update_ui::DescribeThisCopy (),
                                                   update::kUpdatePublicKey);
  m_latest = offer.release;
  m_plan = offer.plan;

  SetUtf8Text (GetDlgItem (IDC_UPDATE_HEADING)->m_hWnd,
               "You have version " + controller.InstalledVersion () + ".  The latest is " +
               controller.LatestVersion () + ".");
  SetUtf8Text (GetDlgItem (IDC_UPDATE_NOTES)->m_hWnd, WithCrLf (update::ConcatenateNotes (controller.Newer ())));

  ((CProgressCtrl *) GetDlgItem (IDC_UPDATE_PROGRESS))->SetRange (0, 100);
  GetDlgItem (IDC_UPDATE_PROGRESS)->ShowWindow (SW_HIDE);

  CheckDlgButton (IDC_UPDATE_DONT_ASK, App.m_bCheckForUpdates ? BST_UNCHECKED : BST_CHECKED);

  if (!m_plan.canInstall)
    {
    SetDlgItemText (IDC_UPDATE_INSTALL, "Open &Release Page");
    SetStatus (m_plan.why + "  The release page has it.");
    }
  else if (!offer.note.empty ())
    SetStatus (offer.note);

  // Give the focus to the button rather than the notes, whose text would be selected.
  GetDlgItem (IDC_UPDATE_INSTALL)->SetFocus ();
  return FALSE;
  }

void CUpdateDlg::SetStatus (const std::string & text)
  {
  SetUtf8Text (GetDlgItem (IDC_UPDATE_STATUS)->m_hWnd, text);
  }

void CUpdateDlg::Busy (const bool bBusy)
  {
  m_bBusy = bBusy;
  GetDlgItem (IDC_UPDATE_INSTALL)->EnableWindow (!bBusy);
  GetDlgItem (IDC_UPDATE_DONT_ASK)->EnableWindow (!bBusy);
  GetDlgItem (IDC_UPDATE_PROGRESS)->ShowWindow (bBusy ? SW_SHOW : SW_HIDE);
  SetDlgItemText (IDCANCEL, bBusy ? "Cancel" : "Remind Me &Later");
  }

// The checkbox speaks for the option, whichever way the dialog is closed.
void CUpdateDlg::Finish (const int result)
  {
  const bool bOff = IsDlgButtonChecked (IDC_UPDATE_DONT_ASK) == BST_CHECKED;
  if ((App.m_bCheckForUpdates == 0) != bOff)
    {
    App.m_bCheckForUpdates = bOff ? 0 : 1;
    SaveTheOption ();
    }
  update::Controller::Get ().DismissBadge ();
  *m_pAlive = false;
  EndDialog (result);
  }

void CUpdateDlg::OnCancel ()
  {
  if (m_bBusy)
    {
    m_bCancelling = true;
    update::Controller::Get ().CancelDownload ();
    SetStatus ("Cancelling...");
    GetDlgItem (IDCANCEL)->EnableWindow (FALSE);
    return;   // DownloadFinished ends it
    }
  Finish (IDCANCEL);
  }

void CUpdateDlg::OnInstall ()
  {
  if (!m_plan.canInstall)
    {
    OpenInBrowser (update::ReleasePageFor (m_latest));
    Finish (IDOK);
    return;
    }

  Busy (true);
  SetStatus ("Downloading...");

  const HWND hDialog = m_hWnd;
  update::Controller::Get ().StartDownload (
      m_latest, update_ui::DescribeThisCopy (), update::kUpdatePublicKey, UpdatesFolder (),
      [hDialog] (uint64_t done, uint64_t total)
        {
        if (total)
          ::PostMessage (hDialog, WM_UPDATE_DOWNLOAD_PROGRESS, (WPARAM) (done * 100 / total), 0);
        return true;
        },
      [this, alive = m_pAlive] (bool bOK, const std::string & pathOrError, const std::string & sha256)
        {
        if (*alive)
          DownloadFinished (bOK, pathOrError, sha256);
        });
  }

LRESULT CUpdateDlg::OnDownloadProgress (WPARAM wParam, LPARAM)
  {
  ((CProgressCtrl *) GetDlgItem (IDC_UPDATE_PROGRESS))->SetPos ((int) wParam);
  return 0;
  }

void CUpdateDlg::DownloadFinished (const bool bOK, const std::string & pathOrError, const std::string & sha256)
  {
  Busy (false);
  GetDlgItem (IDCANCEL)->EnableWindow (TRUE);

  if (m_bCancelling)
    {
    Finish (IDCANCEL);
    return;
    }
  if (!bOK)
    {
    SetStatus ("Could not download it: " + pathOrError);
    return;
    }
  InstallDownloaded (pathOrError, sha256);
  }

void CUpdateDlg::InstallDownloaded (const std::string & path, const std::string & sha256)
  {
  if (!UserData_IsPortable ())
    {
    // The operating system asks about elevation, now that they have clicked.  Then the
    // client closes the ordinary way, questions included, so that its files can be
    // replaced.  (If they cancel that, the installer says the program is running.)
    std::string error;
    if (!update_ui::LaunchVerified (path, sha256, nullptr, &error))
      {
      SetStatus ("Could not start the installer: " + error);
      return;
      }
    Finish (IDOK);
    Frame.PostMessage (WM_CLOSE);
    return;
    }

  // A portable copy: unpack beside the exe, then swap the files.  Nothing the running
  // client needs is disturbed, because its files are only renamed; it restarts on the
  // way out.  Their own files in this folder aren't touched (update::IsUserDataPath).
  Busy (true);
  GetDlgItem (IDCANCEL)->EnableWindow (FALSE);
  SetStatus ("Unpacking...");
  UpdateWindow ();

  const std::string programFolder = (const char *) UserData_ProgramDirectory ();
  // The exe itself, to start on the way out: App.m_strMUSHclientFileName is, despite its
  // name, the folder it's in.  Asked before the swap renames this one.
  const std::wstring exe = update_ui::ProgramToRestart ();
  const std::string stage = programFolder + "update-staging";
  std::string error;
  std::error_code ec;
  fs::remove_all (fs::u8path (stage), ec);
  // What's unpacked is the copy in memory whose hash was checked as it was read.
  const bool bDone = update::ExtractZip (path, stage, &error, sha256) &&
                     update::SwapInFiles (stage, programFolder, &error);
  fs::remove_all (fs::u8path (stage), ec);
  fs::remove (fs::u8path (path), ec);

  Busy (false);
  GetDlgItem (IDCANCEL)->EnableWindow (TRUE);

  if (!bDone)
    {
    SetStatus ("Could not install it: " + error);
    return;
    }

  g_runAtExit = exe;
  ::UMessageBox (TFormat ("MUSHclient %s is ready, and will start when this one has closed.",
                          m_latest.version.c_str ()),
                 MB_OK | MB_ICONINFORMATION);
  Finish (IDOK);
  Frame.PostMessage (WM_CLOSE);
  }

// ---------------- what the main frame calls ----------------

namespace update_ui {

namespace {

void Checked (const bool bUserAsked, const update::CheckResult & result)
  {
  Frame.ShowUpdateBadge ();

  if (!bUserAsked)
    return;    // the badge says it, if there is anything to say

  if (g_pOpenDialog)
    {
    // It's up already, from the badge, and says what there is to say.
    g_pOpenDialog->SetForegroundWindow ();
    return;
    }

  if (!result.ok)
    ::UMessageBox (TFormat ("Could not check for a new version of MUSHclient: %s", result.error.c_str ()),
                   MB_OK | MB_ICONEXCLAMATION);
  else if (result.newer.empty ())
    ::UMessageBox (TFormat ("You have version %s of MUSHclient, which is the latest.", VERSION_STRING),
                   MB_OK | MB_ICONINFORMATION);
  else
    OpenDialog ();
  }

} // anonymous namespace

void Start (const HWND hFrame)
  {
  update::Controller & controller = update::Controller::Get ();
  controller.Configure ([hFrame] (std::function<void ()> work)
    {
    // The handler owns it from there.  If the window has gone, so is the point of it.
    std::function<void ()> * pWork = new std::function<void ()> (std::move (work));
    if (::PostMessage (hFrame, WM_UPDATE_POST, 0, (LPARAM) pWork))
      return true;
    delete pWork;
    return false;
    }, VERSION_STRING);
  controller.SetOnChecked (Checked);
  }

void Tick ()
  {
  update::Controller::Get ().Tick (time (NULL), App.m_bCheckForUpdates != 0);
  }

void CheckFromMenu ()
  {
  update::Controller::Get ().CheckNow (true);
  }

void OpenDialog ()
  {
  // Only ever one.  The badge can't be clicked while it's up, being modal, but a check
  // from the menu that finishes meanwhile would open another over it.
  if (g_pOpenDialog)
    {
    g_pOpenDialog->SetForegroundWindow ();
    return;
    }
  CUpdateDlg dlg (&Frame);
  g_pOpenDialog = &dlg;
  dlg.DoModal ();
  g_pOpenDialog = nullptr;
  Frame.ShowUpdateBadge ();
  }

bool IsDialogOpen ()
  {
  return g_pOpenDialog != nullptr;
  }

std::wstring ProgramToRestart ()
  {
  std::wstring path (MAX_PATH, L'\0');
  while (path.size () <= 32768)
    {
    const DWORD n = GetModuleFileNameW (NULL, &path [0], (DWORD) path.size ());
    if (n == 0)
      return std::wstring ();
    if (n < path.size ())
      {
      path.resize (n);
      return path;
      }
    path.resize (path.size () * 2);     // it was cut short
    }
  return std::wstring ();
  }

bool LaunchVerified (const std::string & path, const std::string & sha256, const wchar_t * parameters,
                     std::string * error, HANDLE * pProcess)
  {
  // Shared for reading only: nothing can write to it, or replace it, until it's closed,
  // which is after it has started.  (Starting a program, elevated or not, only reads it.)
  const std::wstring wide = fs::u8path (path).wstring ();
  const HANDLE hFile = CreateFileW (wide.c_str (), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    {
    *error = "it can't be opened";
    return false;
    }
  const int fd = _open_osfhandle ((intptr_t) hFile, _O_RDONLY | _O_BINARY);
  if (fd < 0)
    {
    CloseHandle (hFile);
    *error = "it can't be read";
    return false;
    }
  FILE * file = _fdopen (fd, "rb");
  if (!file)
    {
    _close (fd);
    *error = "it can't be read";
    return false;
    }

  std::string actual;
  bool bOK = update::HashOpenFile (file, &actual, error);
  if (bOK && (sha256.empty () || actual != sha256))
    {
    *error = "it changed after it was checked";
    bOK = false;
    }
  if (bOK)
    {
    SHELLEXECUTEINFOW info;
    memset (&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = (pProcess ? SEE_MASK_NOCLOSEPROCESS : 0) | SEE_MASK_NO_CONSOLE;
    info.lpVerb = L"open";
    info.lpFile = wide.c_str ();
    info.lpParameters = parameters;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW (&info))
      {
      *error = "Windows wouldn't start it";
      bOK = false;
      }
    else if (pProcess)
      *pProcess = info.hProcess;
    }
  fclose (file);     // and the handle
  return bOK;
  }

update::ThisCopy DescribeThisCopy ()
  {
  update::ThisCopy copy;
  copy.platform = update::Platform::Windows;
#if defined(_M_ARM64)
  copy.arch = "arm64";
#elif defined(_M_X64)
  copy.arch = "x64";
#endif                 // nothing is published for 32 bits, so nothing is named
  copy.portable = UserData_IsPortable ();
  std::error_code ec;
  copy.discworld = fs::exists (fs::u8path ((const char *) UserData_ProgramDirectory ()) / update::kEditionMarker, ec);
  return copy;
  }

void RunAtExitIfWanted ()
  {
  if (g_runAtExit.empty ())
    return;
  const std::wstring folder = fs::path (g_runAtExit).parent_path ().wstring ();
  ShellExecuteW (NULL, L"open", g_runAtExit.c_str (), NULL, folder.c_str (), SW_SHOWNORMAL);
  }

void CleanUpAtStartup ()
  {
  update::CleanUpAfterSwap ((const char *) UserData_ProgramDirectory ());
  std::error_code ec;
  fs::remove_all (fs::u8path (UpdatesFolder ()), ec);
  }

} // namespace update_ui
