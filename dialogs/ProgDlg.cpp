//  ProgDlg.cpp : implementation file
// CG: This file was added by the Progress Dialog component

#include "stdafx.h"
#include "../resource.h"
#include "ProgDlg.h"

#ifdef _DEBUG
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CProgressDlg dialog

CProgressDlg::CProgressDlg(UINT nCaptionID)
{
	m_nCaptionID = CG_IDS_PROGRESS_CAPTION;
	if (nCaptionID != 0)
		m_nCaptionID = nCaptionID;

    m_bCancel=FALSE;
    m_nLower=0;
    m_nUpper=100;
    m_nStep=10;
    //{{AFX_DATA_INIT(CProgressDlg)
    // NOTE: the ClassWizard will add member initialization here
    //}}AFX_DATA_INIT
    m_bParentDisabled = FALSE;
    m_bHideCancel = FALSE;
#ifndef _WIN32
    m_pPortProgress = NULL;
    m_iPortPos      = 0;
#endif
}

CProgressDlg::~CProgressDlg()
{
#ifndef _WIN32
    // m_hWnd is always null on the port (see Create), so the Windows test below
    // would leak the wxProgressDialog and leave it on screen forever.
    if (m_pPortProgress)
      DestroyWindow();
#endif
    if(m_hWnd!=NULL)
      DestroyWindow();
}

BOOL CProgressDlg::DestroyWindow()
{
#ifndef _WIN32
    if (m_pPortProgress) {
        if (g_wxhook_progress_close) g_wxhook_progress_close(m_pPortProgress);
        m_pPortProgress = NULL;
    }
    return TRUE;
#else
    ReEnableParent();
    return CDialog::DestroyWindow();
#endif
}

void CProgressDlg::ReEnableParent()
{
    if(m_bParentDisabled && (m_pParentWnd!=NULL))
      m_pParentWnd->EnableWindow(TRUE);
    m_bParentDisabled=FALSE;
}

BOOL CProgressDlg::Create(CWnd *pParent)
{
#ifndef _WIN32
    // PORT ARM.  This dialog is MODELESS, and the wx dialog bridge implements
    // only the modal path (CDialog::Create is a `return FALSE` stub), so every
    // method below self-disabled on the null m_hWnd: no progress, no status, and
    // Cancel could never fire.  Worse, the pump this class relies on
    // (PeekMessage/DispatchMessage) is a permanent FALSE here, so a large paste
    // froze the whole app with no way to abort.
    //
    // Rather than build a general modeless bridge, the port backs this one class
    // with wxProgressDialog, which provides the same four things natively --
    // a bar, a status line, a Cancel button, and a pump that keeps the UI live.
    // The canonical Windows bodies below are untouched.
    //
    // DELIBERATE DEVIATION from "no native-wx dialog reimplementations": this is
    // a progress indicator, not one of the app's own resource-defined dialogs,
    // and the canonical template's progress bar ("msctls_progress32") is not a
    // control the bridge can build anyway.  A general modeless hook remains the
    // better long-term answer -- it would also revive the four other call sites
    // that use CDialog::Create directly -- see docs/UPSTREAM-DIVERGENCES.md.
    m_bCancel = FALSE;
    if (g_wxhook_progress_open)
        m_pPortProgress = g_wxhook_progress_open(
            (LPCTSTR) Translate ("Progress"), "", m_nUpper - m_nLower);
    return m_pPortProgress != NULL;
#else
    // Get the true parent of the dialog
    m_pParentWnd = CWnd::GetSafeOwner(pParent);

    // m_bParentDisabled is used to re-enable the parent window
    // when the dialog is destroyed. So we don't want to set
    // it to TRUE unless the parent was already enabled.

    if((m_pParentWnd!=NULL) && m_pParentWnd->IsWindowEnabled())
    {
      m_pParentWnd->EnableWindow(FALSE);
      m_bParentDisabled = TRUE;
    }

    if(!CDialog::Create(CProgressDlg::IDD,pParent))
    {
      ReEnableParent();
      return FALSE;
    }

    return TRUE;
#endif
}

void CProgressDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    //{{AFX_DATA_MAP(CProgressDlg)
    DDX_Control(pDX, CG_IDC_PROGDLG_PROGRESS, m_Progress);
    //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CProgressDlg, CDialog)
    //{{AFX_MSG_MAP(CProgressDlg)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

void CProgressDlg::SetStatus(LPCTSTR lpszMessage)
{
#ifndef _WIN32
    // Remembered rather than shown immediately: wxProgressDialog carries the
    // message on its Update() call, so it lands with the next position change.
    m_strPortStatus = lpszMessage ? lpszMessage : "";
    return;
#endif
    if (m_hWnd == NULL)   // no window (headless) — nothing to show
        return;
    CWnd *pWndStatus = GetDlgItem(CG_IDC_PROGDLG_STATUS);

    // Verify that the static text control exists
    ASSERT(pWndStatus!=NULL);
    pWndStatus->SetWindowText(lpszMessage);
}

void CProgressDlg::OnCancel()
{
    m_bCancel=TRUE;
}

void CProgressDlg::SetRange(int nLower,int nUpper)
{
    m_nLower = nLower;
    m_nUpper = nUpper;
#ifndef _WIN32
    // Callers do Create() then SetRange(), so the real maximum is only known
    // now -- re-open at the correct size rather than showing a bar that fills
    // long before the work is done.
    if (m_pPortProgress && g_wxhook_progress_set_range)
        g_wxhook_progress_set_range(m_pPortProgress, nUpper - nLower);
    return;
#endif
    m_Progress.SetRange32(nLower,nUpper);
}
  
int CProgressDlg::SetPos(int nPos)
{
#ifndef _WIN32
    // wxProgressDialog::Update both advances the bar AND pumps the event loop,
    // so this is the port's pump as well as its position setter -- it is what
    // keeps the window painting and the Cancel button clickable during a long
    // send.  It reports false once the user has hit Cancel; latch that for
    // CheckCancelButton, which the send loops poll.
    m_iPortPos = nPos;
    if (m_pPortProgress && g_wxhook_progress_update) {
        if (!g_wxhook_progress_update(m_pPortProgress,
                                      nPos - m_nLower,
                                      (LPCTSTR) m_strPortStatus))
            m_bCancel = TRUE;
    }
    return nPos;
#else
    PumpMessages();
    int iResult = m_Progress.SetPos(nPos);
    UpdatePercent(nPos);
    return iResult;
#endif
}

int CProgressDlg::SetStep(int nStep)
{
    m_nStep = nStep; // Store for later use in calculating percentage
    return m_Progress.SetStep(nStep);
}

int CProgressDlg::OffsetPos(int nPos)
{
#ifndef _WIN32
    // Both of the relative movers were dead on the port: they drove
    // m_Progress (a CProgressCtrl stub) and PumpMessages (a no-op on a null
    // m_hWnd), so nothing reached the wxProgressDialog.  The visible effect was
    // a plugin-loading bar that sat at zero for the whole load — xml_load_world
    // advances it with SetStep(1) + StepIt(), and Lua's progress library offers
    // both to scripts.  Express them in terms of SetPos, which IS wired.
    return SetPos(m_iPortPos + nPos);
#else
    PumpMessages();
    int iResult = m_Progress.OffsetPos(nPos);
    UpdatePercent(iResult+nPos);
    return iResult;
#endif
}

int CProgressDlg::StepIt()
{
#ifndef _WIN32
    return SetPos(m_iPortPos + m_nStep);   // see OffsetPos
#else
    PumpMessages();
    int iResult = m_Progress.StepIt();
    UpdatePercent(iResult+m_nStep);
    return iResult;
#endif
}

#ifndef _WIN32
// Port only.  CWnd::SetWindowText has no window to talk to here, so the title
// every caller sets right after Create() was silently dropped.
void CProgressDlg::SetWindowText(LPCTSTR lpszString)
{
    if (m_pPortProgress && g_wxhook_progress_set_title)
        g_wxhook_progress_set_title(m_pPortProgress, lpszString ? lpszString : "");
}
#endif

void CProgressDlg::PumpMessages()
{
    // Must call Create() before using the dialog; with no window (the wx port
    // has no real progress dialog) this is a no-op rather than an assert/crash.
    if (m_hWnd == NULL)
        return;

    MSG msg;
    // Handle dialog messages
    while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
      if(!IsDialogMessage(&msg))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);  
      }
    }
}

BOOL CProgressDlg::CheckCancelButton()
{
    // Process all pending messages
    PumpMessages();

    // Reset m_bCancel to FALSE so that
    // CheckCancelButton returns FALSE until the user
    // clicks Cancel again. This will allow you to call
    // CheckCancelButton and still continue the operation.
    // If m_bCancel stayed TRUE, then the next call to
    // CheckCancelButton would always return TRUE

    BOOL bResult = m_bCancel;
    m_bCancel = FALSE;

    return bResult;
}

void CProgressDlg::UpdatePercent(int nNewPos)
{
    if (m_hWnd == NULL)   // no window — nothing to update
        return;
    CWnd *pWndPercent = GetDlgItem(CG_IDC_PROGDLG_PERCENT);
    int nPercent;
    
    int nDivisor = m_nUpper - m_nLower;
    ASSERT(nDivisor>0);  // m_nLower should be smaller than m_nUpper

    int nDividend = (nNewPos - m_nLower);
    ASSERT(nDividend>=0);   // Current position should be greater than m_nLower

    nPercent = nDividend * 100 / nDivisor;

    // Since the Progress Control wraps, we will wrap the percentage
    // along with it. However, don't reset 100% back to 0%
    if(nPercent!=100)
      nPercent %= 100;

    // Display the percentage
    CString strBuf;
    strBuf.Format(_T("%d%c"),nPercent,_T('%'));

	CString strCur; // get current percentage
    pWndPercent->GetWindowText(strCur);

	if (strCur != strBuf)
		pWndPercent->SetWindowText(strBuf);
}
    
/////////////////////////////////////////////////////////////////////////////
// CProgressDlg message handlers

BOOL CProgressDlg::OnInitDialog() 
{
    CDialog::OnInitDialog();
    m_Progress.SetRange(m_nLower,m_nUpper);
    m_Progress.SetStep(m_nStep);
    m_Progress.SetPos(m_nLower);

    if (m_bHideCancel)
      GetDlgItem (IDCANCEL)->ShowWindow (SW_HIDE); 

    return TRUE;  
}
