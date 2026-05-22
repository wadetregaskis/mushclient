// MDIClientWnd.h : sub-classed MDI frame
//
/////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef CMDIClientWnd_defined
#define CMDIClientWnd_defined
class CMDIClientWnd : public CWnd
{
	DECLARE_DYNAMIC(CMDIClientWnd)

public:
	CMDIClientWnd();
	virtual ~CMDIClientWnd();

	afx_msg BOOL OnEraseBkgnd(CDC* pDC);

protected:
	DECLARE_MESSAGE_MAP()
};
#endif // CMDIClientWnd_defined
