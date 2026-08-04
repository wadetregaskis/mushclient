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
#include "wx_outputpane.h"   // DecodeUTF8Lenient
#include "compat.h"           // GetRValue / GetGValue / GetBValue
#include "wx_osx_text.h"       // wxOSXDisableSmartSubstitutions (macOS smart-quote fix)

// ── Menu IDs ──────────────────────────────────────────────────────────────
enum {
    ID_NP_FILE_SAVE    = wxID_HIGHEST + 2000,
    ID_NP_FILE_NEW,
    // Convert-menu transforms (parity with canonical CTextView ID_CONVERT_*).
    // Kept contiguous so the event table can bind them as one range.
    ID_NP_CONVERT_FIRST,
    ID_NP_CONVERT_UPPER = ID_NP_CONVERT_FIRST,
    ID_NP_CONVERT_LOWER,
    ID_NP_CONVERT_UNIX2DOS,
    ID_NP_CONVERT_DOS2UNIX,
    ID_NP_CONVERT_MAC2DOS,
    ID_NP_CONVERT_DOS2MAC,
    ID_NP_CONVERT_REMOVEEOL,
    ID_NP_CONVERT_QUOTELINES,
    ID_NP_CONVERT_REMOVEBLANKS,
    ID_NP_CONVERT_QUOTEFORUM,
    ID_NP_CONVERT_BASE64ENC,
    ID_NP_CONVERT_BASE64DEC,
    ID_NP_CONVERT_HTMLSPECIAL,
    ID_NP_CONVERT_LAST = ID_NP_CONVERT_HTMLSPECIAL,
    // Edit ▸ ASCII Art (canonical ID_EDIT_ASCIIART).  Outside the Convert
    // range: it is an Edit-menu command, and it inserts rather than transforms.
    ID_NP_EDIT_ASCIIART,
};

// ── Event table ───────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(CWxNotepadFrame, wxFrame)
    EVT_CLOSE                (CWxNotepadFrame::OnClose)
    EVT_MENU(ID_NP_FILE_SAVE,  CWxNotepadFrame::OnMenuSave)
    EVT_MENU(ID_NP_FILE_NEW,   CWxNotepadFrame::OnMenuNew)
    EVT_MENU_RANGE(ID_NP_CONVERT_FIRST, ID_NP_CONVERT_LAST, CWxNotepadFrame::OnConvert)
    EVT_MENU(ID_NP_EDIT_ASCIIART, CWxNotepadFrame::OnEditAsciiArt)
wxEND_EVENT_TABLE()

// ── Static registry ────────────────────────────────────────────────────────
std::map<std::string, CWxNotepadFrame*> CWxNotepadFrame::s_registry;

// ── Helpers ────────────────────────────────────────────────────────────────

// Build a wxFont from the fields stored in pDoc.
static wxFont MakeFont(const CTextDocument* pDoc)
{
    // An empty face name means "use the global fixed-pitch font", not "use
    // whatever wx defaults to" — see the canonical CTextView::SetTheFont.
    // Without this, notepads created with no explicit font silently ignore the
    // Fixed pitch font preference that governs them on Windows.
    CString strFont = pDoc->m_strFontName;
    long    lSize   = pDoc->m_iFontSize;
    if (strFont.IsEmpty ())
      {
      strFont = App.m_strFixedPitchFont;
      lSize   = App.m_iFixedPitchFontSize;
      }

    const wxString face = wxString::FromUTF8(
        static_cast<const char*>(strFont));
    int ptSize = static_cast<int>(lSize);
    if (ptSize <= 0) ptSize = 10;
    wxFontWeight weight = (pDoc->m_iFontWeight >= FW_BOLD)
                          ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL;
    wxFontStyle style = pDoc->m_bFontItalic
                        ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    return wxFont(ptSize, wxFONTFAMILY_TELETYPE, style, weight,
                  pDoc->m_bFontUnderline, face);
}

// ── Text-conversion transforms ──────────────────────────────────────────────
// The transforms themselves live in notepad_convert_shared.cpp (mushcore) —
// faithful copies of the canonical CTextView::OnConvert* bodies, wrapped in the
// LF↔CRLF boundary conversion this control needs.  They used to be a private
// copy here, fed the control's text raw; because wxOSX rewrites every CR to LF,
// the CR-keyed transforms (Remove end-of-lines, DOS↔Mac) then did nothing or
// deleted every line break.  Keeping them in mushcore also puts them under the
// headless test suite (tests/test_notepad_convert.cpp).

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

    // Edit menu — wxTextCtrl handles these standard ids when focused.
    wxMenu* edit = new wxMenu();
    edit->Append(wxID_UNDO,      wxT("&Undo\tCtrl+Z"));
    edit->Append(wxID_REDO,      wxT("&Redo\tCtrl+Shift+Z"));
    edit->AppendSeparator();
    edit->Append(wxID_CUT,       wxT("Cu&t\tCtrl+X"));
    edit->Append(wxID_COPY,      wxT("&Copy\tCtrl+C"));
    edit->Append(wxID_PASTE,     wxT("&Paste\tCtrl+V"));
    edit->AppendSeparator();
    edit->Append(wxID_SELECTALL, wxT("Select &All\tCtrl+A"));
    edit->AppendSeparator();
    // Canonical label + accelerator, from the IDR_NORMALTYPE Edit popup in the
    // recovered resources (mushclient_dialogs.rc: "ASCII Art...\tShift+Ctrl+A",
    // ID_EDIT_ASCIIART).
    edit->Append(ID_NP_EDIT_ASCIIART, wxT("ASCII &Art...\tShift+Ctrl+A"));
    mb->Append(edit, wxT("&Edit"));

    // Convert menu — text transforms (canonical CTextView ID_CONVERT_* parity).
    // Each acts on the selection, or the whole document when nothing is selected.
    wxMenu* conv = new wxMenu();
    conv->Append(ID_NP_CONVERT_UPPER,        wxT("&UPPER CASE"));
    conv->Append(ID_NP_CONVERT_LOWER,        wxT("&lower case"));
    conv->AppendSeparator();
    conv->Append(ID_NP_CONVERT_UNIX2DOS,     wxT("Unix → &DOS (LF → CRLF)"));
    conv->Append(ID_NP_CONVERT_DOS2UNIX,     wxT("DOS → &Unix (CRLF → LF)"));
    conv->Append(ID_NP_CONVERT_MAC2DOS,      wxT("&Mac → DOS (CR → CRLF)"));
    conv->Append(ID_NP_CONVERT_DOS2MAC,      wxT("DOS → M&ac (CRLF → CR)"));
    conv->AppendSeparator();
    conv->Append(ID_NP_CONVERT_REMOVEEOL,    wxT("&Remove end-of-lines (join)"));
    conv->Append(ID_NP_CONVERT_REMOVEBLANKS, wxT("Remove e&xtra blanks"));
    conv->Append(ID_NP_CONVERT_QUOTELINES,   wxT("&Quote lines"));
    conv->AppendSeparator();
    conv->Append(ID_NP_CONVERT_QUOTEFORUM,   wxT("Quote &forum codes"));
    conv->Append(ID_NP_CONVERT_BASE64ENC,    wxT("Base64 &encode"));
    conv->Append(ID_NP_CONVERT_BASE64DEC,    wxT("Base64 &decode"));
    conv->Append(ID_NP_CONVERT_HTMLSPECIAL,  wxT("Convert &HTML special characters"));
    mb->Append(conv, wxT("&Convert"));

    SetMenuBar(mb);

    // ── Text control ────────────────────────────────────────────────────
    // "Word Wrap" (File > Preferences > Notepad).  wxTE_DONTWRAP IS wxHSCROLL
    // (wx/textctrl.h), so passing it unconditionally -- as this did -- made
    // every notepad permanently no-wrap, against a default of wrap ON.
    // Windows reads the same preference at creation time
    // (TextView.cpp PreCreateWindow), so honouring it here and not on the fly
    // matches: already-open notepads keep their mode, and wxOSX cannot change
    // the wrap mode of a live wxTextCtrl anyway.
    long ctrlStyle = wxTE_MULTILINE | wxTE_RICH2 | wxTE_NOHIDESEL;
    if (!App.m_bNotepadWordWrap)
        ctrlStyle |= wxHSCROLL;   // == wxTE_DONTWRAP, plus the scrollbar you
                                  // then need -- nothing else here provides it
    if (pDoc->m_bReadOnly)
        ctrlStyle |= wxTE_READONLY;

    m_ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                            wxDefaultPosition, wxDefaultSize, ctrlStyle);
    wxOSXDisableSmartSubstitutions(m_ctrl);   // no curly quotes in script files

    // ── Initial content ─────────────────────────────────────────────────
    if (!text.IsEmpty())
        m_ctrl->SetValue(text);

    // ── Track edits ─────────────────────────────────────────────────────
    // CEditView reflects the edit control's EN_CHANGE to
    // CEditView::OnEditChange, which sets the document's modified flag; that
    // flag is what CDocument::SaveModified consults before prompting.  Nothing
    // here bound the equivalent event, so the flag never became TRUE and
    // SaveModified had to prompt unconditionally to avoid losing edits.
    //
    // Bound AFTER the initial SetValue on purpose: wx fires wxEVT_TEXT for a
    // programmatic SetValue too, and the initial content is not an edit.  The
    // canonical decides that case explicitly in CreateTextWindow
    // (Utilities.cpp: SetModifiedFlag(TRUE) when it has text to install), and
    // hook_create_text_window mirrors it.  Everything after this point —
    // typing, paste, the Convert menu, ASCII art, world.AppendToNotepad's
    // WriteText — is the moral equivalent of the EM_REPLACESEL that raises
    // EN_CHANGE on Windows.
    m_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        if (m_pDoc)
            m_pDoc->SetModifiedFlag(TRUE);
        e.Skip();
    });

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
        App.m_pNormalDocTemplate->AddDocument(pDoc);

    // ── Register in the static title registry ─────────────────────────────
    {
        std::string key = std::string(title.ToUTF8());
        for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        m_registryKey = key;   // remember the insert key — Save renames the frame
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
    // Lenient, not FromUTF8: this text is whatever the world or a script
    // handed us.  FromUTF8 is all-or-nothing, so a single stray byte replaced
    // the notepad with an empty one -- silent data loss on the replace arm.
    const wxString decoded = DecodeUTF8Lenient(text.data(), text.size());
    if (replace) {
        m_ctrl->SetValue(decoded);
    } else {
        m_ctrl->AppendText(decoded);
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

// ── SetColours ─────────────────────────────────────────────────────────────

void CWxNotepadFrame::SetColours(unsigned long fg, unsigned long bg)
{
    if (!m_pDoc) return;
    m_pDoc->m_textColour = static_cast<COLORREF>(fg);
    m_pDoc->m_backColour = static_cast<COLORREF>(bg);
    ApplyColours();
}

// ── SetFont ────────────────────────────────────────────────────────────────

void CWxNotepadFrame::SetFont(const std::string& face, int size, int style, int /*charset*/)
{
    if (!m_pDoc) return;
    if (!face.empty())
        m_pDoc->m_strFontName = face.c_str();
    if (size > 0)
        m_pDoc->m_iFontSize = size;
    m_pDoc->m_iFontWeight  = (style & 1) ? FW_BOLD : FW_NORMAL;
    m_pDoc->m_bFontItalic    = (style & 2) != 0;
    m_pDoc->m_bFontUnderline = (style & 4) != 0;
    // m_bFontStrikeout is not in CTextDocument on non-Windows; skip strikeout.
    ApplyFont();
}

// ── SetReadOnly ────────────────────────────────────────────────────────────

void CWxNotepadFrame::SetReadOnly(bool ro)
{
    if (!m_ctrl) return;
    m_ctrl->SetEditable(!ro);
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
            App.m_pNormalDocTemplate->RemoveDocument(m_pDoc);
        // Erase by the key we inserted with, NOT GetTitle(): the Save path
        // renames the frame (SetTitle to the saved file's basename), so the
        // current title no longer matches the registry key — using it would
        // leave a stale entry pointing at this freed frame (use-after-free).
        s_registry.erase(m_registryKey);

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

// ── Convert menu: text transforms (CTextView::OnConvert* parity) ─────────────
// Edit ▸ ASCII Art.  The canonical command (CTextView::OnEditAsciiart, bound by
// TextView.cpp's ON_COMMAND(ID_EDIT_ASCIIART)) lives in a view the port never
// constructs, and this menu item did not exist here — so the feature, and the
// two ASCII-art preferences whose only consumer it is, were absent on macOS.
//
// art.cpp's RunAsciiArtDialog does everything except the insertion: dialog,
// preference write-backs, FIGlet generation.  Deliberately NOT routed through
// CTextView, whose GetEditCtrl() is a process-wide static CEdit with a null
// m_hWnd — that would run the generator, save the settings, and insert nothing.
void CWxNotepadFrame::OnEditAsciiArt(wxCommandEvent&)
{
    if (!m_ctrl) return;

    CStringArray straLines;
    if (!RunAsciiArtDialog(straLines))
        return;   // cancelled, or generation failed (it reported why)

    // Inserted at the caret, replacing any selection — the same place
    // ReplaceSel puts it on Windows.
    for (int i = 0; i < straLines.GetSize(); i++)
        m_ctrl->WriteText(wxString::FromUTF8(
            static_cast<const char*>(straLines[i])) + wxT("\n"));
}

void CWxNotepadFrame::OnConvert(wxCommandEvent& evt)
{
    if (!m_ctrl) return;

    // Operate on the selection, or the whole document when nothing is selected
    // (mirrors CTextView::GetSelection / ReplaceAndReselect).
    long from = 0, to = 0;
    m_ctrl->GetSelection(&from, &to);
    bool bAll = (from == to);
    wxString wxsrc = bAll ? m_ctrl->GetValue() : m_ctrl->GetStringSelection();
    CString src(static_cast<const char*>(wxsrc.ToUTF8()));
    CString dst;

    int op;
    switch (evt.GetId()) {
        case ID_NP_CONVERT_UPPER:        op = eNotepadConvertUpperCase;     break;
        case ID_NP_CONVERT_LOWER:        op = eNotepadConvertLowerCase;     break;
        case ID_NP_CONVERT_UNIX2DOS:     op = eNotepadConvertUnixToDos;     break;
        case ID_NP_CONVERT_DOS2UNIX:     op = eNotepadConvertDosToUnix;     break;
        case ID_NP_CONVERT_MAC2DOS:      op = eNotepadConvertMacToDos;      break;
        case ID_NP_CONVERT_DOS2MAC:      op = eNotepadConvertDosToMac;      break;
        case ID_NP_CONVERT_REMOVEEOL:    op = eNotepadConvertRemoveEol;     break;
        case ID_NP_CONVERT_REMOVEBLANKS: op = eNotepadConvertRemoveBlanks;  break;
        case ID_NP_CONVERT_QUOTELINES:   op = eNotepadConvertQuoteLines;    break;
        case ID_NP_CONVERT_QUOTEFORUM:   op = eNotepadConvertQuoteForum;    break;
        case ID_NP_CONVERT_BASE64ENC:    op = eNotepadConvertBase64Encode;  break;
        case ID_NP_CONVERT_BASE64DEC:    op = eNotepadConvertBase64Decode;  break;
        case ID_NP_CONVERT_HTMLSPECIAL:  op = eNotepadConvertHtmlSpecial;   break;
        default: return;
    }

    // A failure here is base64 decode on invalid input: leave the notepad alone
    // and beep, as the canonical does when decodeBase64 throws.
    if (!NotepadConvertText(op, src, dst)) {
        wxBell();
        return;
    }

    // Base64 DECODE produces arbitrary binary, and several of the other
    // conversions can too.  FromUTF8 returned "" for those and the SetValue
    // below then ERASED the whole notepad.
    wxString wxdst = DecodeUTF8Lenient(static_cast<const char*>(dst), dst.GetLength());
    if (bAll) {
        m_ctrl->SetValue(wxdst);
        m_ctrl->SetSelection(0, m_ctrl->GetLastPosition());
    } else {
        m_ctrl->Replace(from, to, wxdst);
        m_ctrl->SetSelection(from, from + static_cast<long>(wxdst.length()));
    }
}

#endif // !_WIN32
