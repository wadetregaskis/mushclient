// TimerWnd.cpp : implementation file
//

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CTimerWnd

CTimerWnd::CTimerWnd(CMUSHclientDoc * pDoc)
{
  m_pDoc = pDoc;
  m_iTimer = 0;
}

CTimerWnd::~CTimerWnd()
{
}


BEGIN_MESSAGE_MAP(CTimerWnd, CWnd)
	//{{AFX_MSG_MAP(CTimerWnd)
	ON_WM_TIMER()
	ON_WM_DESTROY()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()


/////////////////////////////////////////////////////////////////////////////
// CTimerWnd message handlers

void CTimerWnd::OnTimer(UINT nIDEvent) 
{
  // no queued commands - don't update status line
  if (m_pDoc->m_QueuedCommandsList.IsEmpty ())
    return;

  m_pDoc->DrainOneQueuedSlot ();   // shared body — doc_shared.cpp
  m_pDoc->ShowQueuedCommands ();    // update status line
}

void CTimerWnd::OnDestroy() 
{
  if (m_iTimer)
      KillTimer (m_iTimer);

  CWnd::OnDestroy();
	
}

void CTimerWnd::ChangeTimerRate (const int iRate)
  {
  int iNewRate = iRate;

  // make sure valid
  if (iNewRate < 0)
    iNewRate = 0;

  if (iNewRate > 30000)
    iNewRate = 30000;

  // get rid of old timer
  if (m_iTimer)
      KillTimer (m_iTimer);

  // if zero, no timer wanted
  if (iNewRate)
    m_iTimer = SetTimer(COMMAND_QUEUE_TIMER_ID, iNewRate, NULL); 
  else
    {

    // if no delay any more, send all outstanding lines.  This used to compare
    // the echo flag case-SENSITIVELY while OnTimer above used toupper, so the
    // same no-log command echoed from one drain and not the other; the shared
    // body settles it (docs/UPSTREAM-BUGS.md #13).
    m_pDoc->DrainAllQueuedCommands ();

    }

  }  // end of ChangeTimerRate

