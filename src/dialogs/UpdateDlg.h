// UpdateDlg.h -- the Update dialog, and what the Windows client does about updates.
// The decisions are in update_logic.h and update_service.h; this is the Windows face
// of them.  The port's is platform/wx_update.cpp.

#pragma once

#include "../update_service.h"

class CUpdateDlg : public CDialog
  {
  public:
  CUpdateDlg (CWnd * pParent = NULL);

  protected:
  virtual BOOL OnInitDialog ();
  virtual void OnCancel ();
  afx_msg void OnInstall ();
  afx_msg LRESULT OnDownloadProgress (WPARAM wParam, LPARAM lParam);
  DECLARE_MESSAGE_MAP ()

  private:
  void SetStatus (const std::string & text);
  void DownloadFinished (const bool bOK, const std::string & pathOrError, const std::string & sha256);
  void InstallDownloaded (const std::string & path, const std::string & sha256);
  void Finish (int result);
  void Busy (const bool bBusy);

  update::Release m_latest;
  update::InstallPlan m_plan;
  bool m_bBusy = false;
  bool m_bCancelling = false;
  // Stays true until the dialog is gone: what a worker posts afterwards does nothing.
  std::shared_ptr<bool> m_pAlive;
  };

namespace update_ui {

// CMainFrame: after creation, once a second, and when the worker posts something (the
// message's LPARAM is a new'd std::function<void()> for it to run and delete).
void Start (HWND hFrame);      // from OnCreate, when App doesn't know about it yet
void Tick ();
const UINT WM_UPDATE_POST = WM_APP + 0x70;

// Help > Check for Updates, and a click on the badge.  There is only ever one dialog:
// asked for another while it's up, it comes to the front instead.
void CheckFromMenu ();
void OpenDialog ();
bool IsDialogOpen ();

// What this copy is.
update::ThisCopy DescribeThisCopy ();

// After an update has swapped a portable copy's files: start it again on the way out.
void RunAtExitIfWanted ();

// What that starts: this process's exe, in full.
std::wstring ProgramToRestart ();

// Starts a downloaded installer, if it is still what was signed: it is held open so that
// nothing can change it, hashed, and started while held.  parameters and pProcess (the
// started process, for the caller to close) are for tests, and may be null.
bool LaunchVerified (const std::string & path, const std::string & sha256, const wchar_t * parameters,
                     std::string * error, HANDLE * pProcess = nullptr);

// At startup: the leavings of the last update.
void CleanUpAtStartup ();

} // namespace update_ui
