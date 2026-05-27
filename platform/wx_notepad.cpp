// platform/wx_notepad.cpp
// CWxNotepadFrame — wx implementation of MUSHclient's internal notepad windows.
//
// Each notepad is an independent top-level wxFrame containing a wxTextCtrl.
// The frame wires CTextDocument::m_fn_* callbacks so that the portable
// methods_notepad.cpp code can drive it without knowing about wx.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/filename.h>
#include <wx/filedlg.h>

// Pull in the full MUSHclient type universe the same way other wx files do.
#include "../stdafx.h"
#include "../MUSHclient.h"
#include "../TextDocument.h"
#include "../doc.h"

#include "wx_notepad.h"
#include "compat.h"           // GetRValue / GetGValue / GetBValue

// ── Menu IDs ──────────────────────────────────────────────────────────────
enum {
    ID_NP_FILE_SAVE    = wxID_HIGHEST + 2000,
    ID_NP_FILE_NEW,
};

// ── Event table ───────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(CWxNotepadFrame, wxFrame)
    EVT_CLOSE                (CWxNotepadFrame::OnClose)
    EVT_MENU(ID_NP_FILE_SAVE,  CWxNotepadFrame::OnMenuSave)
    EVT_MENU(ID_NP_FILE_NEW,   CWxNotepadFrame::OnMenuNew)
wxEND_EVENT_TABLE()

// ── Static registry ────────────────────────────────────────────────────────
std::map<std::string, CWxNotepadFrame*> CWxNotepadFrame::s_registry;

// ── Helpers ────────────────────────────────────────────────────────────────

// Build a wxFont from the fields stored in pDoc.
static wxFont MakeFont(const CTextDocument* pDoc)
{
    const wxString face = wxString::FromUTF8(
        static_cast<const char*>(pDoc->m_strFontName));
    int ptSize = static_cast<int>(pDoc->m_iFontSize);
    if (ptSize <= 0) ptSize = 10;
    wxFontWeight weight = (pDoc->m_iFontWeight >= FW_BOLD)
                          ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL;
    wxFontStyle style = pDoc->m_bFontItalic
                        ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    return wxFont(ptSize, wxFONTFAMILY_TELETYPE, style, weight,
                  pDoc->m_bFontUnderline, face);
}

// ── Constructor ────────────────────────────────────────────────────────────

CWxNotepadFrame::CWxNotepadFrame(CTextDocument* pDoc,
                                 const wxString& title,
                                 const wxString& text)
    : wxFrame(nullptr, wxID_ANY, title,
              wxDefaultPosition, wxSize(640, 480),
              wxDEFAULT_FRAME_STYLE)
    , m_pDoc(pDoc)
    , m_ctrl(nullptr)
    , m_forceClose(false)
{
    // ── Menu bar ────────────────────────────────────────────────────────
    wxMenuBar* mb  = new wxMenuBar();
    wxMenu*    mnu = new wxMenu();
    mnu->Append(ID_NP_FILE_NEW,  wxT("&New notepad\tCtrl+N"));
    mnu->Append(ID_NP_FILE_SAVE, wxT("&Save\tCtrl+S"));
    mb->Append(mnu, wxT("&File"));
    SetMenuBar(mb);

    // ── Text control ────────────────────────────────────────────────────
    long ctrlStyle = wxTE_MULTILINE | wxTE_RICH2 | wxHSCROLL | wxTE_NOHIDESEL;
    if (pDoc->m_bReadOnly)
        ctrlStyle |= wxTE_READONLY;

    m_ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                            wxDefaultPosition, wxDefaultSize, ctrlStyle);

    // ── Initial content ─────────────────────────────────────────────────
    if (!text.IsEmpty())
        m_ctrl->SetValue(text);

    // ── Font & colours ──────────────────────────────────────────────────
    ApplyFont();
    ApplyColours();

    // ── Wire m_fn_* callbacks on the document ────────────────────────────
    // The lambdas capture 'this' and 'm_ctrl' by value-of-pointer; they are
    // always called on the main thread, so no locking is needed.

    pDoc->m_fn_get_text = [this]() -> std::string {
        if (!m_ctrl) return {};
        return std::string(m_ctrl->GetValue().ToUTF8());
    };

    pDoc->m_fn_get_text_length = [this]() -> int {
        if (!m_ctrl) return 0;
        return static_cast<int>(m_ctrl->GetLastPosition());
    };

    pDoc->m_fn_set_sel = [this](int from, int to) {
        if (!m_ctrl) return;
        long last = m_ctrl->GetLastPosition();
        long f = (from < 0) ? last : static_cast<long>(from);
        long t = (to   < 0) ? last : static_cast<long>(to);
        m_ctrl->SetSelection(f, t);
    };

    pDoc->m_fn_replace_sel = [this](const std::string& s) {
        if (!m_ctrl) return;
        m_ctrl->WriteText(wxString::FromUTF8(s.c_str()));
    };

    pDoc->m_fn_activate = [this]() {
        if (!IsShown()) Show(true);
        Raise();
        m_ctrl->SetFocus();
    };

    pDoc->m_fn_move_window = [this](int x, int y, int w, int h) {
        SetSize(x, y, w, h);
    };

    pDoc->m_fn_get_rect = [this]() -> std::string {
        wxRect r = GetRect();
        char buf[80];
        snprintf(buf, sizeof(buf), "%d %d %d %d",
                 r.GetLeft(), r.GetTop(), r.GetRight(), r.GetBottom());
        return std::string(buf);
    };

    pDoc->m_fn_save_to_file = [this](const std::string& path, bool /*bReplace*/) -> bool {
        if (!m_ctrl) return false;
        wxString wxPath = wxString::FromUTF8(path.c_str());

        if (wxPath.IsEmpty()) {
            // Show save dialog.
            wxFileDialog dlg(this,
                             wxT("Save notepad"),
                             wxEmptyString,
                             wxString::FromUTF8(
                                 static_cast<const char*>(m_pDoc->m_strTitle)),
                             wxT("Text files (*.txt)|*.txt|All files (*.*)|*.*"),
                             wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() != wxID_OK)
                return false;
            wxPath = dlg.GetPath();
        }

        bool ok = m_ctrl->SaveFile(wxPath);
        if (ok) {
            m_pDoc->SetPathName(static_cast<const char*>(
                wxPath.ToUTF8().data()));
            m_pDoc->SetModifiedFlag(FALSE);
            SetTitle(wxFileName(wxPath).GetFullName());
        }
        return ok;
    };

    pDoc->m_fn_apply_font = [this]() { ApplyFont(); };

    pDoc->m_fn_apply_colours = [this]() { ApplyColours(); };

    pDoc->m_fn_set_read_only = [this](bool ro) {
        if (!m_ctrl) return;
        m_ctrl->SetEditable(!ro);
    };

    pDoc->m_fn_close_window = [this]() {
        m_forceClose = true;
        Close(true);   // force — skips SaveModified check in OnClose
    };

    pDoc->m_pWxFrame = static_cast<void*>(this);

    // ── Register with the document template ──────────────────────────────
    if (App.m_pNormalDocTemplate)
        App.m_pNormalDocTemplate->RegisterDoc(pDoc);

    // ── Register in the static title registry ─────────────────────────────
    {
        std::string key = std::string(title.ToUTF8());
        for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        s_registry[key] = this;
    }

    Show(true);
}

// ── Find ───────────────────────────────────────────────────────────────────

/*static*/ CWxNotepadFrame* CWxNotepadFrame::Find(const std::string& title)
{
    std::string key = title;
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto it = s_registry.find(key);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// ── AppendText ─────────────────────────────────────────────────────────────

void CWxNotepadFrame::AppendText(const std::string& text, bool replace)
{
    if (!m_ctrl) return;
    if (replace) {
        m_ctrl->SetValue(wxString::FromUTF8(text.c_str()));
    } else {
        m_ctrl->AppendText(wxString::FromUTF8(text.c_str()));
    }
    Raise();
}

// ── GetText ────────────────────────────────────────────────────────────────

std::string CWxNotepadFrame::GetText() const
{
    if (!m_ctrl) return {};
    return std::string(m_ctrl->GetValue().ToUTF8());
}

// ── ListTitles ─────────────────────────────────────────────────────────────

/*static*/ std::vector<std::string> CWxNotepadFrame::ListTitles()
{
    std::vector<std::string> titles;
    titles.reserve(s_registry.size());
    for (const auto& kv : s_registry)
        titles.push_back(kv.second->GetTitle().ToUTF8().data());
    return titles;
}

// ── Destructor ─────────────────────────────────────────────────────────────

CWxNotepadFrame::~CWxNotepadFrame()
{
    // Document was already cleaned up (or never existed).
}

// ── ApplyFont ──────────────────────────────────────────────────────────────

void CWxNotepadFrame::ApplyFont()
{
    if (!m_ctrl || !m_pDoc) return;
    wxFont f = MakeFont(m_pDoc);
    if (f.IsOk()) {
        m_ctrl->SetFont(f);
        m_ctrl->Refresh();
    }
}

// ── ApplyColours ───────────────────────────────────────────────────────────

void CWxNotepadFrame::ApplyColours()
{
    if (!m_ctrl || !m_pDoc) return;
    COLORREF fg = m_pDoc->m_textColour;
    COLORREF bg = m_pDoc->m_backColour;
    m_ctrl->SetForegroundColour(
        wxColour(GetRValue(fg), GetGValue(fg), GetBValue(fg)));
    m_ctrl->SetBackgroundColour(
        wxColour(GetRValue(bg), GetGValue(bg), GetBValue(bg)));
    m_ctrl->Refresh();
}

// ── OnClose ────────────────────────────────────────────────────────────────

void CWxNotepadFrame::OnClose(wxCloseEvent& evt)
{
    if (!m_forceClose && evt.CanVeto() && m_pDoc) {
        // User-initiated close: let SaveModified decide.
        // Clear m_fn_close_window first to prevent recursion if SaveModified
        // triggers another close.
        m_pDoc->m_fn_close_window = nullptr;
        if (!m_pDoc->SaveModified()) {
            // User cancelled — restore the callback and veto.
            m_pDoc->m_fn_close_window = [this]() {
                m_forceClose = true;
                Close(true);
            };
            evt.Veto();
            return;
        }
    }

    // ── Cleanup ─────────────────────────────────────────────────────────
    if (m_pDoc) {
        // Unregister from the doc template and static registry.
        if (App.m_pNormalDocTemplate)
            App.m_pNormalDocTemplate->UnregisterDoc(m_pDoc);
        {
            std::string key = std::string(GetTitle().ToUTF8());
            for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            s_registry.erase(key);
        }

        // Clear all callbacks to prevent use-after-free if anything tries to
        // call them during the delete.
        m_pDoc->m_fn_get_text         = nullptr;
        m_pDoc->m_fn_get_text_length  = nullptr;
        m_pDoc->m_fn_set_sel          = nullptr;
        m_pDoc->m_fn_replace_sel      = nullptr;
        m_pDoc->m_fn_activate         = nullptr;
        m_pDoc->m_fn_move_window      = nullptr;
        m_pDoc->m_fn_get_rect         = nullptr;
        m_pDoc->m_fn_save_to_file     = nullptr;
        m_pDoc->m_fn_apply_font       = nullptr;
        m_pDoc->m_fn_apply_colours    = nullptr;
        m_pDoc->m_fn_set_read_only    = nullptr;
        m_pDoc->m_fn_close_window     = nullptr;
        m_pDoc->m_pWxFrame            = nullptr;

        delete m_pDoc;
        m_pDoc = nullptr;
    }

    m_ctrl = nullptr;
    Destroy();
}

// ── Menu handlers ──────────────────────────────────────────────────────────

void CWxNotepadFrame::OnMenuSave(wxCommandEvent& /*evt*/)
{
    if (!m_pDoc) return;
    m_pDoc->DoSave(
        m_pDoc->GetPathName().IsEmpty()
            ? nullptr
            : static_cast<LPCTSTR>(m_pDoc->GetPathName()));
}

void CWxNotepadFrame::OnMenuNew(wxCommandEvent& /*evt*/)
{
    // Delegate back to the document's "File → New" logic.
    if (!m_pDoc) return;
    // Re-use the doc's OnFileNew which calls CreateTextWindow for a fresh pad.
    // On non-Windows this isn't hooked to MFC command routing, so call directly.
    CMUSHclientDoc* pWorld = m_pDoc->FindWorld();
    CString title = m_pDoc->m_strTitle;
    CreateTextWindow("",
                     static_cast<LPCTSTR>(title),
                     pWorld,
                     m_pDoc->m_iUniqueDocumentNumber,
                     m_pDoc->m_strFontName,
                     m_pDoc->m_iFontSize,
                     m_pDoc->m_iFontWeight,
                     m_pDoc->m_iFontCharset,
                     m_pDoc->m_textColour,
                     m_pDoc->m_backColour,
                     "",
                     "",
                     false, false, false, false, false, 0,
                     eNotepadNormal);
}

#endif // !_WIN32
