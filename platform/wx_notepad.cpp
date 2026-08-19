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
#include "wx_mfc_hooks.h"     // g_wxhook_open_text_file (File > Open)
#include <wx/numdlg.h>       // wxGetNumberFromUser (Go To Line)
#include <algorithm>         // std::max/min (Go To Line clamps)
#include "compat.h"           // GetRValue / GetGValue / GetBValue
#include "wx_osx_text.h"       // wxOSXDisableSmartSubstitutions (macOS smart-quote fix)

// ── Menu IDs ──────────────────────────────────────────────────────────────
enum {
    ID_NP_FILE_SAVE    = wxID_HIGHEST + 2000,
    ID_NP_FILE_NEW,
    ID_NP_FILE_OPEN,
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
    // Search / navigation / misc (2026-08-13; canonical CTextView commands
    // the port's bar lacked — the whole Search popup among them).
    ID_NP_FILE_SAVEAS,
    ID_NP_SEARCH_FIND,
    ID_NP_SEARCH_FINDNEXT,
    ID_NP_SEARCH_REPLACE,
    ID_NP_EDIT_GOTO,
    ID_NP_EDIT_INSERTDATETIME,
    ID_NP_EDIT_WORDCOUNT,
    ID_NP_EDIT_SENDTOWORLD,
};

// ── Event table ───────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(CWxNotepadFrame, wxFrame)
    EVT_CLOSE                (CWxNotepadFrame::OnClose)
    EVT_MENU(ID_NP_FILE_SAVE,  CWxNotepadFrame::OnMenuSave)
    EVT_MENU(ID_NP_FILE_NEW,   CWxNotepadFrame::OnMenuNew)
    EVT_MENU(ID_NP_FILE_OPEN,  CWxNotepadFrame::OnMenuOpen)
    EVT_MENU_RANGE(ID_NP_CONVERT_FIRST, ID_NP_CONVERT_LAST, CWxNotepadFrame::OnConvert)
    EVT_MENU(ID_NP_EDIT_ASCIIART, CWxNotepadFrame::OnEditAsciiArt)
    EVT_MENU(ID_NP_FILE_SAVEAS,        CWxNotepadFrame::OnMenuSaveAs)
    EVT_MENU(ID_NP_SEARCH_FIND,        CWxNotepadFrame::OnMenuFind)
    EVT_MENU(ID_NP_SEARCH_FINDNEXT,    CWxNotepadFrame::OnMenuFindNext)
    EVT_MENU(ID_NP_SEARCH_REPLACE,     CWxNotepadFrame::OnMenuReplace)
    EVT_MENU(ID_NP_EDIT_GOTO,          CWxNotepadFrame::OnMenuGoto)
    EVT_MENU(ID_NP_EDIT_INSERTDATETIME,CWxNotepadFrame::OnMenuInsertDateTime)
    EVT_MENU(ID_NP_EDIT_WORDCOUNT,     CWxNotepadFrame::OnMenuWordCount)
    EVT_MENU(ID_NP_EDIT_SENDTOWORLD,   CWxNotepadFrame::OnMenuSendToWorld)
    EVT_UPDATE_UI(ID_NP_EDIT_SENDTOWORLD, CWxNotepadFrame::OnUpdateSendToWorld)
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

// ── The one thing the control may be fed ────────────────────────────────────
// LF, and nothing else.  wx does not COLLAPSE a CRLF to LF, it EXPANDS it to
// two line breaks (measured on wx 3.3.2 — see stdafx.h), so text that reaches
// the control with a CR in it is silently double-spaced, and the next Save
// writes that damage into the user's file.  Every write site below goes
// through here.
//
// A thin wrapper over the shared NotepadBytesToControlText rather than a
// second implementation: two copies of "\r\n → \n" is exactly how the port
// grows twin-drift bugs.  The fast path costs one scan and skips the
// conversion entirely, which is what almost every call takes.

// For text that arrives as raw bytes (the m_fn_* callbacks, AppendText):
// normalise first, then decode.
static wxString NotepadBytesForControl(const char* data, size_t len)
{
    const std::string lf = NotepadBytesToControlText(data, len);
    return DecodeUTF8Lenient(lf.data(), lf.size());
}

// For text that is already a wxString (the constructor, the Convert menu).
static wxString NotepadTextForControl(const wxString& s)
{
    if (s.Find(wxT('\r')) == wxNOT_FOUND)
        return s;                       // the overwhelmingly common case
    // Named, not inlined: utf8_str() on a temporary returns a NON-owning view
    // whose bytes die at the semicolon.
    const wxScopedCharBuffer utf8 = s.utf8_str();
    return NotepadBytesForControl(utf8.data(), utf8.length());
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
    // The canonical CTextView File menu's Open (CTextDocument::OnFileOpen),
    // absent here entirely until 2026-08-18: load an arbitrary text/xml/lua
    // file into a fresh notepad window.
    mnu->Append(ID_NP_FILE_OPEN, wxT("&Open…\tCtrl+O"));
    mnu->Append(ID_NP_FILE_SAVE, wxT("&Save\tCtrl+S"));
    // Save As re-targets an already-pathed document — the plugin-editor case,
    // where plain Save can only overwrite the plugin's own .lua in place.
    mnu->Append(ID_NP_FILE_SAVEAS, wxT("Save &As…"));
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
    edit->AppendSeparator();
    // Canonical CTextView Edit-menu commands (2026-08-13).  Accelerators are
    // macOS-appropriate where Windows' clash with Mac conventions (Go To was
    // Ctrl+G, which is Find Next on a Mac; F3/F4 are system keys here).
    edit->Append(ID_NP_EDIT_GOTO,           wxT("&Go To Line…\tCtrl+L"));
    edit->Append(ID_NP_EDIT_INSERTDATETIME, wxT("Insert &Date/Time\tCtrl+D"));
    edit->Append(ID_NP_EDIT_WORDCOUNT,      wxT("&Word Count"));
    edit->Append(ID_NP_EDIT_SENDTOWORLD,    wxT("Send To W&orld\tShift+Ctrl+S"));
    mb->Append(edit, wxT("&Edit"));

    // Search menu — the canonical IDR_NORMALTYPE popup the port lacked.
    wxMenu* search = new wxMenu();
    search->Append(ID_NP_SEARCH_FIND,     wxT("&Find…\tCtrl+F"));
    search->Append(ID_NP_SEARCH_FINDNEXT, wxT("Find &Next\tCtrl+G"));
    search->Append(ID_NP_SEARCH_REPLACE,  wxT("&Replace…\tAlt+Ctrl+F"));
    mb->Append(search, wxT("&Search"));

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
        m_ctrl->SetValue(NotepadTextForControl(text));

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
        // UTF-8 BYTE length, not the control's character count: GetNotepadText
        // returns the notepad's contents as UTF-8 bytes (methods_notepad.cpp),
        // and AppendToTheNotepad computes an append position from this length
        // and feeds it straight into m_fn_set_sel below — the two callbacks
        // must agree on units, or an append after any non-ASCII text lands at
        // the wrong offset.
        return static_cast<int>(m_ctrl->GetValue().utf8_str().length());
    };

    pDoc->m_fn_set_sel = [this](int from, int to) {
        if (!m_ctrl) return;
        // from/to are UTF-8 BYTE offsets (see m_fn_get_text_length above);
        // wx text-control positions are character offsets, so translate
        // through the same byte->char helper the output pane uses for the
        // same split.
        //
        // The wxString must outlive the buffer: utf8_str() on a TEMPORARY
        // returns a view whose bytes die at the semicolon (the dangling-
        // buffer class that corrupted the heap and aborted the notepad
        // selftest at exit, intermittently, ~1 run in 2).
        const wxString value = m_ctrl->GetValue();
        const wxScopedCharBuffer utf8 = value.utf8_str();
        const size_t nbytes = utf8.length();
        long last = static_cast<long>(m_ctrl->GetLastPosition());
        long f = (from < 0) ? last
                 : static_cast<long>(UTF8ByteToCharOffset(
                       utf8.data(), nbytes, static_cast<size_t>(from)));
        long t = (to   < 0) ? last
                 : static_cast<long>(UTF8ByteToCharOffset(
                       utf8.data(), nbytes, static_cast<size_t>(to)));
        m_ctrl->SetSelection(f, t);
    };

    pDoc->m_fn_replace_sel = [this](const std::string& s) {
        if (!m_ctrl) return;
        // Lenient, matching AppendText below: FromUTF8 is all-or-nothing, so
        // world.AppendToNotepad of legacy-charset bytes wrote NOTHING (while
        // returning true), and ReplaceNotepad — which selects all first —
        // replaced the content with that nothing.  data()+size() also stops
        // c_str() truncating at an embedded NUL.
        //
        // Normalised on the way in: world.AppendToNotepad / ReplaceNotepad hand
        // over whatever the script or the world produced, and a CRLF in that
        // text used to reach the control and double-space it.
        m_ctrl->WriteText(NotepadBytesForControl(s.data(), s.size()));
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
        // Emit EXCLUSIVE right/bottom edges (left+width, top+height), not
        // wxRect's own INCLUSIVE GetRight()/GetBottom(): both consumers of
        // this string (the BSTR-formatted GetNotepadWindowPosition and Lua's
        // L_GetNotepadWindowPosition, scripting/methods/methods_notepad.cpp
        // and scripting/lua_methods.cpp) subtract left/top from right/bottom
        // as if this were a Win32 RECT, whose right/bottom are exclusive — so
        // an inclusive edge here read the window's width/height one pixel short.
        snprintf(buf, sizeof(buf), "%d %d %d %d",
                 r.GetLeft(), r.GetTop(),
                 r.GetLeft() + r.GetWidth(), r.GetTop() + r.GetHeight());
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

        // NOT m_ctrl->SaveFile: that writes the control's LF-only buffer
        // verbatim, so saving a file that came in as CRLF (every bundled
        // plugin) rewrote it as LF -- and, before the load side was
        // normalised, as a DOUBLE-SPACED LF file.  Write the bytes the
        // document's own line-ending style calls for instead.
        const std::string text(m_ctrl->GetValue().ToUTF8());
        // Named: wxPath outlives the buffer, so the bytes are still there when
        // NotepadWriteFile reads them.
        const wxScopedCharBuffer pathUtf8 = wxPath.ToUTF8();
        bool ok = NotepadWriteFile(pathUtf8.data(), text,
                                   m_pDoc->m_iLineEndings);
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
    // Line endings normalised in the same pass -- see NotepadBytesForControl.
    const wxString decoded = NotepadBytesForControl(text.data(), text.size());
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

// ── Search / navigation / misc (2026-08-13) ─────────────────────────────────
// The canonical CTextView commands; core ops separate from their dialogs so
// the notepad selftest can drive them.

// File > Open (canonical CTextDocument::OnFileOpen).  Load an arbitrary text/
// xml/lua file into a fresh notepad window — the port had no menu item for
// this at all, so the command was permanently unreachable from the UI.
//
// Deliberately NOT App.m_pNormalDocTemplate->OpenDocumentFile (what the
// canonical body calls): on this port that override
// (platform/mfc_ui.h CMultiDocTemplate::OpenDocumentFile) always treats a
// non-empty path as a WORLD file, so an Open here would try to load the
// chosen .txt/.xml/.lua as a world.  g_wxhook_open_text_file is the actual
// non-world-file open path — the same one the "edit script with notepad"
// call sites (world_debug.cpp) and the Plugins dialog's Edit button use — and
// it does exactly what OnFileOpen wants: read the file, open a notepad on its
// contents, and set the path so Save writes back rather than prompting.
void CWxNotepadFrame::OnMenuOpen(wxCommandEvent& /*evt*/)
{
    wxFileDialog dlg(this, wxT("Open"), wxEmptyString, wxEmptyString,
                     wxT("Text files (*.txt)|*.txt|"
                         "Plugins (*.xml)|*.xml|"
                         "Lua scripts (*.lua)|*.lua|"
                         "All files (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (g_wxhook_open_text_file)
        g_wxhook_open_text_file((const char*) dlg.GetPath().ToUTF8());
}

void CWxNotepadFrame::OnMenuSaveAs(wxCommandEvent& /*evt*/)
{
    // DoSave(nullptr) takes the save lambda's dialog arm, which re-targets
    // the document (SetPathName) and renames the frame on success.
    if (m_pDoc) m_pDoc->DoSave(nullptr);
}

bool CWxNotepadFrame::FindNextInText(const wxString& term, bool matchCase)
{
    if (!m_ctrl || term.empty()) return false;
    const wxString hay0 = m_ctrl->GetValue();
    const wxString hay  = matchCase ? hay0 : hay0.Lower();
    const wxString ndl  = matchCase ? term : term.Lower();

    long selFrom = 0, selTo = 0;
    m_ctrl->GetSelection(&selFrom, &selTo);
    size_t hit = hay.find(ndl, (size_t) selTo);
    if (hit == wxString::npos && selTo > 0)
        hit = hay.find(ndl);                       // wrap to the top
    if (hit == wxString::npos) return false;

    m_ctrl->SetSelection((long) hit, (long) (hit + ndl.length()));
    m_ctrl->ShowPosition((long) hit);
    return true;
}

int CWxNotepadFrame::ReplaceAllInText(const wxString& term,
                                      const wxString& repl, bool matchCase)
{
    if (!m_ctrl || term.empty()) return 0;
    const wxString text = m_ctrl->GetValue();
    const wxString hay  = matchCase ? text : text.Lower();
    const wxString ndl  = matchCase ? term : term.Lower();

    wxString out;
    out.reserve(text.length());
    int count = 0;
    size_t from = 0;
    for (;;) {
        const size_t hit = hay.find(ndl, from);
        if (hit == wxString::npos) { out += text.Mid(from); break; }
        out += text.Mid(from, hit - from);
        out += repl;
        from = hit + ndl.length();
        ++count;
    }
    // Normalised: the replacement string comes from a dialog field, and a
    // pasted CRLF in it would otherwise reach the control.
    if (count) m_ctrl->SetValue(NotepadTextForControl(out));   // fires wxEVT_TEXT → modified
    return count;
}

void CWxNotepadFrame::GoToLine(long line1)
{
    if (!m_ctrl) return;
    const long lines = std::max(1, m_ctrl->GetNumberOfLines());
    const long line0 = std::max(0L, std::min(lines - 1, line1 - 1));
    const long pos = m_ctrl->XYToPosition(0, line0);
    if (pos < 0) return;
    m_ctrl->SetInsertionPoint(pos);
    m_ctrl->ShowPosition(pos);
    m_ctrl->SetFocus();
}

void CWxNotepadFrame::OnMenuGoto(wxCommandEvent& /*evt*/)
{
    if (!m_ctrl) return;
    long curX = 0, curY = 0;
    m_ctrl->PositionToXY(m_ctrl->GetInsertionPoint(), &curX, &curY);
    const long line = wxGetNumberFromUser(
        wxT("Go to which line?"), wxT("Line:"), wxT("Go To Line"),
        curY + 1, 1, std::max(1, m_ctrl->GetNumberOfLines()), this);
    if (line > 0) GoToLine(line);
}

void CWxNotepadFrame::InsertDateTime()
{
    if (!m_ctrl) return;
    // The canonical format (CTextView::OnEditInsertdatetime), incl. the %#
    // no-leading-zero flag the CTime shim maps to POSIX %-.
    CTime theTime = CTime::GetCurrentTime();
    CString strTime = theTime.Format(TranslateTime("%A, %#d %B %Y, %#I:%M %p"));
    m_ctrl->WriteText(DecodeUTF8Lenient((const char*) strTime,
                                        (size_t) strTime.GetLength()));
}

void CWxNotepadFrame::OnMenuInsertDateTime(wxCommandEvent& /*evt*/)
{
    InsertDateTime();
}

wxString CWxNotepadFrame::WordCountSummary() const
{
    if (!m_ctrl) return wxString();
    // Selection, or the whole document when nothing is selected — the
    // CTextView::GetSelection convention, and its exact counting rules.
    long from = 0, to = 0;
    m_ctrl->GetSelection(&from, &to);
    const bool bAll = (from == to);
    const wxString wtext = bAll ? m_ctrl->GetValue()
                                : m_ctrl->GetStringSelection();
    const wxScopedCharBuffer utf8 = wtext.utf8_str();
    const char* p = utf8.data();

    int iNewLines = 0, iWords = 0;
    for (; *p; p++) {
        if (*p == '\n') iNewLines++;
        if (isspace((unsigned char) p[0]) &&
            !isspace((unsigned char) p[1]) && p[1])
            iWords++;
    }
    const long chars = (long) wtext.length();
    if (chars > 0) {
        iNewLines++;
        if (!isspace((unsigned char) (char) wtext[0])) iWords++;
    }
    return wxString::Format(
        wxT("The %s contains %d line%s, %d word%s, %ld character%s"),
        bAll ? wxT("document") : wxT("selection"),
        iNewLines, iNewLines == 1 ? wxT("") : wxT("s"),
        iWords,    iWords    == 1 ? wxT("") : wxT("s"),
        chars,     chars     == 1 ? wxT("") : wxT("s"));
}

void CWxNotepadFrame::OnMenuWordCount(wxCommandEvent& /*evt*/)
{
    wxMessageBox(WordCountSummary(), wxT("Word Count"),
                 wxOK | wxICON_INFORMATION, this);
}

wxString CWxNotepadFrame::SelectionOrAllText(bool* pAll) const
{
    long from = 0, to = 0;
    if (m_ctrl) m_ctrl->GetSelection(&from, &to);
    const bool bAll = (from == to);
    if (pAll) *pAll = bAll;
    if (!m_ctrl) return wxString();
    return bAll ? m_ctrl->GetValue() : m_ctrl->GetStringSelection();
}

bool CWxNotepadFrame::SendSelectionToWorld()
{
    // CTextView::OnEditSendtoworld: the selection (or the whole document)
    // goes through the world's paste machinery, preambles and all; sending
    // everything counts as saved.
    if (!m_ctrl || !m_pDoc) return false;
    CMUSHclientDoc* pWorld = m_pDoc->FindWorld();
    if (!pWorld) return false;

    bool bAll = false;
    const wxString wtext = SelectionOrAllText(&bAll);
    if (wtext.empty()) return false;

    const wxScopedCharBuffer utf8 = wtext.utf8_str();
    CMemFile f((unsigned char*) utf8.data(), (unsigned) utf8.length());
    const bool bOK = pWorld->SendToMushHelper(&f,
                        pWorld->m_paste_preamble,
                        pWorld->m_pasteline_preamble,
                        pWorld->m_pasteline_postamble,
                        pWorld->m_paste_postamble,
                        pWorld->m_bPasteCommentedSoftcode,
                        pWorld->m_nPasteDelay,
                        pWorld->m_nPasteDelayPerLines,
                        pWorld->m_bConfirmOnPaste,
                        pWorld->m_bPasteEcho) != FALSE;
    if (bOK && bAll)
        m_pDoc->SetModifiedFlag(FALSE);
    return bOK;
}

void CWxNotepadFrame::OnMenuSendToWorld(wxCommandEvent& /*evt*/)
{
    SendSelectionToWorld();
}

void CWxNotepadFrame::OnUpdateSendToWorld(wxUpdateUIEvent& evt)
{
    // CTextView::OnUpdateEditSendtoworld: with no related world the command
    // is disabled and the caption reverts to the generic form; the port left
    // this item unconditionally enabled, so a standalone notepad's Send To
    // World looked live but silently did nothing when clicked.
    CMUSHclientDoc* pWorld = m_pDoc ? m_pDoc->FindWorld() : nullptr;
    if (!pWorld)
    {
        evt.Enable(false);
        evt.SetText(wxT("Send To W&orld\tShift+Ctrl+S"));
        return;
    }

    evt.Enable(!SelectionOrAllText().empty());
    evt.SetText(wxString::Format(wxT("Send To %s\tShift+Ctrl+S"),
                DecodeTextLenient((const char*) pWorld->m_mush_name)));
}

void CWxNotepadFrame::OnMenuFind(wxCommandEvent& /*evt*/)     { OpenFindDialog(false); }
void CWxNotepadFrame::OnMenuReplace(wxCommandEvent& /*evt*/)  { OpenFindDialog(true); }

void CWxNotepadFrame::OnMenuFindNext(wxCommandEvent& /*evt*/)
{
    if (m_findTerm.empty()) { OpenFindDialog(false); return; }
    if (!FindNextInText(m_findTerm, m_findCase))
        wxMessageBox(wxT("Cannot find \"") + m_findTerm + wxT("\""),
                     wxT("Find"), wxOK | wxICON_INFORMATION, this);
}

void CWxNotepadFrame::OpenFindDialog(bool replace)
{
    if (m_frDlg) { m_frDlg->Destroy(); m_frDlg = nullptr; }
    m_frData.SetFlags(wxFR_DOWN | (m_findCase ? wxFR_MATCHCASE : 0));
    m_frData.SetFindString(m_findTerm);
    m_frDlg = new wxFindReplaceDialog(this, &m_frData,
                                      replace ? wxT("Replace") : wxT("Find"),
                                      replace ? wxFR_REPLACEDIALOG : 0);
    m_frDlg->Bind(wxEVT_FIND,             &CWxNotepadFrame::OnFindDialog, this);
    m_frDlg->Bind(wxEVT_FIND_NEXT,        &CWxNotepadFrame::OnFindDialog, this);
    m_frDlg->Bind(wxEVT_FIND_REPLACE,     &CWxNotepadFrame::OnFindDialog, this);
    m_frDlg->Bind(wxEVT_FIND_REPLACE_ALL, &CWxNotepadFrame::OnFindDialog, this);
    m_frDlg->Bind(wxEVT_FIND_CLOSE,       &CWxNotepadFrame::OnFindDialog, this);
    m_frDlg->Show();
}

void CWxNotepadFrame::OnFindDialog(wxFindDialogEvent& evt)
{
    const wxEventType t = evt.GetEventType();
    m_findTerm = evt.GetFindString();
    m_findCase = (evt.GetFlags() & wxFR_MATCHCASE) != 0;
    if (t == wxEVT_FIND || t == wxEVT_FIND_NEXT) {
        if (!FindNextInText(m_findTerm, m_findCase))
            wxMessageBox(wxT("Cannot find \"") + m_findTerm + wxT("\""),
                         wxT("Find"), wxOK | wxICON_INFORMATION, this);
    } else if (t == wxEVT_FIND_REPLACE) {
        if (m_ctrl) {
            const wxString sel = m_ctrl->GetStringSelection();
            const bool match = m_findCase ? (sel == m_findTerm)
                                          : sel.IsSameAs(m_findTerm, false);
            if (match) {
                long f = 0, tn = 0;
                m_ctrl->GetSelection(&f, &tn);
                m_ctrl->Replace(f, tn, evt.GetReplaceString());
            }
        }
        FindNextInText(m_findTerm, m_findCase);
    } else if (t == wxEVT_FIND_REPLACE_ALL) {
        const int n = ReplaceAllInText(m_findTerm, evt.GetReplaceString(),
                                       m_findCase);
        wxMessageBox(wxString::Format(wxT("%d replacement%s made"),
                                      n, n == 1 ? wxT("") : wxT("s")),
                     wxT("Replace All"), wxOK | wxICON_INFORMATION, this);
    } else if (t == wxEVT_FIND_CLOSE) {
        if (m_frDlg) { m_frDlg->Destroy(); m_frDlg = nullptr; }
    }
}

void CWxNotepadFrame::OnMenuNew(wxCommandEvent& /*evt*/)
{
    // Delegate back to the document's "File → New" logic.
    if (!m_pDoc) return;
    // Re-use the doc's OnFileNew which calls CreateTextWindow for a fresh pad.
    // On non-Windows this isn't hooked to MFC command routing (OnFileNew is
    // protected), so reproduce its body directly.
    CMUSHclientDoc* pWorld = m_pDoc->FindWorld();
    CString title = m_pDoc->m_strTitle;

    // Don't make another Recall/search-results window (CTextDocument::
    // OnFileNew's guard, TextDocument.cpp:460): a fresh notepad opened from
    // within a Recall/search-results one must reset to the generic title,
    // not inherit "Recall: <query>".
    if (!m_pDoc->m_strSearchString.IsEmpty())
        title = pWorld ? TFormat("Notepad: %s", (LPCTSTR) pWorld->m_mush_name)
                       : Translate("Untitled");

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
        m_ctrl->WriteText(NotepadTextForControl(wxString::FromUTF8(
            static_cast<const char*>(straLines[i]))) + wxT("\n"));
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

    // A failure here is base64 decode on invalid input: leave the notepad
    // alone.  NotepadConvertText already reported the error (ReportError, the
    // same box CTextView::OnConvertBase64decode puts up) — no beep on top of
    // that; the canonical never beeps here.
    if (!NotepadConvertText(op, src, dst))
        return;

    // Base64 DECODE produces arbitrary binary, and several of the other
    // conversions can too.  FromUTF8 returned "" for those and the SetValue
    // below then ERASED the whole notepad.
    // ...and Base64 decode can therefore also produce a CR, which the control
    // would double-space.  NotepadBytesForControl normalises in the same pass.
    wxString wxdst = NotepadBytesForControl(static_cast<const char*>(dst),
                                            (size_t) dst.GetLength());
    if (bAll) {
        m_ctrl->SetValue(wxdst);
        m_ctrl->SetSelection(0, m_ctrl->GetLastPosition());
    } else {
        m_ctrl->Replace(from, to, wxdst);
        m_ctrl->SetSelection(from, from + static_cast<long>(wxdst.length()));
    }
}

#endif // !_WIN32
