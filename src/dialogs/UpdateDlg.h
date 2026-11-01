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
  void DownloadFinished (const bool bOK, const std::string & pathOrError);
  void InstallDownloaded (const std::string & path);
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
void Start ();
void Tick ();
const UINT WM_UPDATE_POST = WM_APP + 0x70;

// Help > Check for Updates, and a click on the badge.
void CheckFromMenu ();
void OpenDialog ();

// What this copy is.
update::ThisCopy DescribeThisCopy ();

// After an update has swapped a portable copy's files: start it again on the way out.
void RunAtExitIfWanted ();

// At startup: the leavings of the last update.
void CleanUpAtStartup ();

} // namespace update_ui
