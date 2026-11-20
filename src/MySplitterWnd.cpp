// MySplitterWnd.cpp : implementation file
//

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include "MySplitterWnd.h"
#include "appearance.h"
#include "childfrm.h"
#include "mushview.h"
#include "sendvw.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CMySplitterWnd

CMySplitterWnd::CMySplitterWnd()
{
  m_pDoc = NULL;
#ifdef _WIN32
  m_iBarsDpi = 0;
#endif
}

CMySplitterWnd::~CMySplitterWnd()
{
}


BEGIN_MESSAGE_MAP(CMySplitterWnd, CSplitterWnd)
	//{{AFX_MSG_MAP(CMySplitterWnd)
	ON_WM_LBUTTONUP()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()


#ifdef _WIN32
void CMySplitterWnd::ScaleBars (const int iDpi)
{
  // from MFC's own sizes each time, so that moving between monitors doesn't drift
  if (!m_iBarsDpi)
    {
    m_iBaseBars [0] = m_cxSplitter;
    m_iBaseBars [1] = m_cySplitter;
    m_iBaseBars [2] = m_cxSplitterGap;
    m_iBaseBars [3] = m_cySplitterGap;
    m_iBarsDpi = 96;
    }
  if (iDpi == m_iBarsDpi || iDpi <= 0)
    return;
  m_cxSplitter = ::MulDiv (m_iBaseBars [0], iDpi, 96);
  m_cySplitter = ::MulDiv (m_iBaseBars [1], iDpi, 96);
  m_cxSplitterGap = ::MulDiv (m_iBaseBars [2], iDpi, 96);
  m_cySplitterGap = ::MulDiv (m_iBaseBars [3], iDpi, 96);
  m_iBarsDpi = iDpi;
}

// The base class's bars and borders come from the light palette MFC cached at startup.
void CMySplitterWnd::OnDrawSplitter(CDC* pDC, ESplitType nType, const CRect& rect)
{
  if (!Appearance_IsDark () || pDC == NULL)
    {
    CSplitterWnd::OnDrawSplitter (pDC, nType, rect);
    return;
    }

  if (nType == splitBorder)
    {
    pDC->Draw3dRect (rect, Appearance_EdgeColour (), Appearance_EdgeColour ());
    CRect rectInner (rect);
    rectInner.DeflateRect (1, 1);
    pDC->Draw3dRect (rectInner, Appearance_FaceColour (), Appearance_FaceColour ());
    }
  else
    pDC->FillSolidRect (rect, Appearance_FaceColour ());
}
#endif

///////////////////////////////////////////////////////////////////////////
// CMySplitterWnd message handlers

void CMySplitterWnd::OnLButtonUp(UINT nFlags, CPoint point) 
{
  CSplitterWnd::OnLButtonUp(nFlags, point);

  // this hideous code is so that we know when the user has resized the
  // input pane, so we don't snap the size back to what it was
  if (m_pDoc)
    {
    int cyCur,
        cyMin;
    GetRowInfo (COMMAND_PANE, cyCur, cyMin);

    // snap to boundary stuff (version 4.16)
    if (((GetKeyState (VK_LCONTROL) & 0x8000) != 0 ||
        (GetKeyState (VK_RCONTROL) & 0x8000) != 0) && m_pDoc->m_input_font)
      {
      CDC dc;

      dc.CreateCompatibleDC (NULL);
      dc.SelectObject (m_pDoc->m_input_font);

      TEXTMETRIC tm;
      dc.GetTextMetrics(&tm);

      // need to know height of entire font (eg. 12 point FixedSys is 15)
      int m_iLineHeight = tm.tmHeight; 

      const int iFudge = 4;   // trial and error stuff, if anyone has a better idea let me know

      double fLines = (cyCur - iFudge) / m_iLineHeight;
      int iLines = floor (fLines + 0.5);   // round to nearest number of lines
      if (iLines)   
        cyCur =  m_iLineHeight * iLines + iFudge;  // calculate desired height for those lines
      else
        cyCur = 0;  // zero lines is still zero
      } // end of ctrl key held down

    if (!(m_pDoc->m_mush_name.IsEmpty ()))
      App.db_write_int ("worlds", (LPCTSTR) CFormat ("%s:%s", (LPCTSTR) m_pDoc->m_mush_name, "Bottom Height"), m_pDoc->ToLogical (cyCur));

//    TRACE1 ("Bottom height (saved) = %i\n", cyCur);

    if (m_pDoc->m_pActiveCommandView)
      m_pDoc->m_pActiveCommandView->m_owner_frame->FixUpSplitterBar ();
    else if (m_pDoc->m_pActiveOutputView)
      m_pDoc->m_pActiveOutputView->m_owner_frame->FixUpSplitterBar ();

	  }

}

