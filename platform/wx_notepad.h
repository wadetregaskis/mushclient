// platform/wx_notepad.h
// CWxNotepadFrame — wxFrame-based notepad window for the non-Windows port.
//
// One CWxNotepadFrame is created per CTextDocument.  The frame owns the
// document lifetime: when the frame is destroyed it deletes its CTextDocument.
// Callers must not delete CTextDocument directly after handing it to a frame.

#pragma once
#ifndef _WIN32

#include <wx/wx.h>
#include <wx/stc/stc.h>       // wxStyledTextCtrl for Find/replace support
#include <map>
#include <string>

// Forward declaration — avoids pulling stdafx.h into wx headers.
class CTextDocument;

class CWxNotepadFrame : public wxFrame
{
public:
    // Construct a notepad frame, populate it with sText, wire all m_fn_*
    // callbacks on pDoc, and register pDoc with App.m_pNormalDocTemplate.
    // The frame takes ownership of pDoc.
    CWxNotepadFrame(CTextDocument* pDoc,
                    const wxString& title,
                    const wxString& text);
    ~CWxNotepadFrame() override;

    // Find an existing frame by title (case-insensitive).
    // Returns nullptr if not found.
    static CWxNotepadFrame* Find(const std::string& title);

    // Append or replace the text content of this frame.
    void AppendText(const std::string& text, bool replace);

private:
    CTextDocument* m_pDoc;
    wxTextCtrl*    m_ctrl;
    bool           m_forceClose; // set by m_fn_close_window to skip SaveModified

    // Apply font/colour fields from m_pDoc to m_ctrl.
    void ApplyFont();
    void ApplyColours();

    // Event handlers
    void OnClose   (wxCloseEvent&   evt);
    void OnMenuNew (wxCommandEvent& evt);
    void OnMenuSave(wxCommandEvent& evt);

    // Static registry: lower-cased title → frame pointer.
    // Populated in constructor; erased in destructor.
    static std::map<std::string, CWxNotepadFrame*> s_registry;

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
