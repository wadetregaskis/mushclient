// ButtonColour.h : header file
//

#pragma once

/////////////////////////////////////////////////////////////////////////////
// CColourButton window

class CColourButton : public CButton
{
// Construction
public:
	CColourButton();

// Attributes
public:
	COLORREF	m_colour;
  COLORREF  m_clipboardColour;

// Operations
public:
#ifndef _WIN32
  // PORT ONLY (docs/UPSTREAM-DIVERGENCES.md 23).  Windows dispatches
  // WM_RBUTTONDOWN to OnRButtonDown through ON_WM_RBUTTONDOWN; the port's
  // ON_WM_* macros are empty and an owner-draw swatch is a wx control with no
  // message map behind it, so the bridge calls this instead.  `point` is in
  // CLIENT coordinates, which is what WM_RBUTTONDOWN carries; (-1, -1) means a
  // keyboard invocation, exactly as Win32 uses it.
  void ShowColourPopup (CPoint point) { OnRButtonDown (0, point); }
#endif

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CColourButton)
	public:
	virtual void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct);
	//}}AFX_VIRTUAL

// Implementation
public:
	virtual ~CColourButton();

	// Generated message map functions
protected:
	//{{AFX_MSG(CColourButton)
	afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnPopupCopycolour();
	afx_msg void OnPopupPastecolour();
	afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
	//}}AFX_MSG

	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
