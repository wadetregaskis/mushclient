#if !defined(AFX_TEXTDOCUMENT_H__D2527D3F_0F59_11D4_BB0A_0080AD7972EF__INCLUDED_)
#define AFX_TEXTDOCUMENT_H__D2527D3F_0F59_11D4_BB0A_0080AD7972EF__INCLUDED_

/*
Copyright (C) 2000 Nick Gammon.

*/

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
// TextDocument.h : header file
//

// enums for  m_iSaveOnChange
enum 
  {
  eNotepadSaveDefault,     // 0
  eNotepadSaveAlways,      // 1
  eNotepadSaveNever,       // 2
  };

/////////////////////////////////////////////////////////////////////////////
// CTextDocument document

class CTextDocument : public CDocument
{
protected:
	CTextDocument();           // protected constructor used by dynamic creation
	DECLARE_DYNCREATE(CTextDocument)

// Attributes
public:

	HANDLE	m_pThread;			// Notification thread
	CEvent  m_eventFileChanged;		// file changed thread event
  bool m_bInFileChanged;
  CTime m_timeFileMod;

  CString m_strTitle;
  CMUSHclientDoc * m_pRelatedWorld; // which world it belongs to, if any

   __int64 m_iUniqueDocumentNumber;    // to confirm we have the right doc
   CString m_strFontName;        // window font
   LONG     m_iFontSize;
   LONG     m_iFontWeight;
   DWORD    m_iFontCharset;
   bool     m_bFontItalic;
   bool     m_bFontUnderline;
   bool     m_bFontStrikeout;
   COLORREF m_textColour;        // window colour
   COLORREF m_backColour;
   CString  m_strSearchString;   // if recall, what did they recall?
   CString  m_strRecallLinePreamble;   // what line preamble did they want?
   bool m_bMatchCase;
   bool m_bRegexp;          // and other params
   bool m_bCommands;
   bool m_bOutput;
   bool m_bNotes;
   int  m_iSaveOnChange;    // whether to save if contents changed, see enum above
   int  m_iLines;
   int  m_iNotepadType;    // "type" of notepad window - see enum in doc.h

   BOOL m_bReadOnly;

#ifndef _WIN32
   // ── wx notepad callbacks ─────────────────────────────────────────────────
   // Wired by CWxNotepadFrame (platform/wx_notepad.cpp) at window creation.
   // Each callback is cleared to nullptr when the window is destroyed.

   // Read the full text content of the notepad.
   std::function<std::string()>              m_fn_get_text;
   // Return the number of characters in the notepad.
   std::function<int()>                      m_fn_get_text_length;
   // Set the edit-control selection.  Negative end → select to end.
   std::function<void(int, int)>             m_fn_set_sel;
   // Replace the current selection with the given UTF-8 string.
   std::function<void(const std::string&)>   m_fn_replace_sel;
   // Bring this notepad window to the foreground and activate it.
   std::function<void()>                     m_fn_activate;
   // Move/resize the notepad window (screen coordinates).
   std::function<void(int,int,int,int)>      m_fn_move_window;
   // Get the window rect as "left top right bottom" string.
   std::function<std::string()>              m_fn_get_rect;
   // Save to a file; if path is empty use the current path.
   // Returns true on success, false on cancel / error.
   std::function<bool(const std::string&, bool)> m_fn_save_to_file;
   // Apply font changes (called after m_strFontName / m_iFontSize change).
   std::function<void()>                     m_fn_apply_font;
   // Apply colour changes (called after m_textColour / m_backColour change).
   std::function<void()>                     m_fn_apply_colours;
   // Set read-only state on the wx text control.
   std::function<void(bool)>                 m_fn_set_read_only;
   // Close the window (called from CloseNotepad / OnCloseDocument).
   std::function<void()>                     m_fn_close_window;
   // Pointer back to the wx frame (for IsKindOf checks via GetParentFrame).
   void*                                     m_pWxFrame = nullptr;
#endif // !_WIN32

// Operations
public:

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CTextDocument)
	public:
	virtual void Serialize(CArchive& ar);   // overridden for document i/o
	virtual void OnCloseDocument();
	virtual BOOL SaveModified();
	protected:
	virtual BOOL OnNewDocument();
	//}}AFX_VIRTUAL

// Implementation
public:
	virtual ~CTextDocument();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif


  CMUSHclientDoc * FindWorld (void);
  void OnFileChanged(void);
  virtual BOOL DoSave (LPCTSTR lpszPathName, BOOL bReplace = TRUE);
  void CreateMonitoringThread(const char * sName);
  void SetTheFont (void);
  void SetReadOnly (BOOL bReadOnly);

#ifndef _WIN32
  // Non-Windows factory: bypasses the protected constructor so that the wx
  // notepad hook (hook_create_text_window in wx_mfc_impl.cpp) can allocate
  // documents directly without going through the MFC document template.
  static CTextDocument* CreateInstance() { return new CTextDocument(); }
#endif

	// Generated message map functions
protected:
	//{{AFX_MSG(CTextDocument)
	afx_msg void OnFileOpen();
	afx_msg void OnUpdateStatuslineMushname(CCmdUI* pCmdUI);
	afx_msg void OnFileNew();
	afx_msg void OnEditConvertclipboardforumcodes();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_TEXTDOCUMENT_H__D2527D3F_0F59_11D4_BB0A_0080AD7972EF__INCLUDED_)
