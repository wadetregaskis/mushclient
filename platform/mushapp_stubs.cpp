// platform/mushapp_stubs.cpp
// Non-Windows implementations/stubs required by the mushapp (full) build.
//
// This file provides three categories of symbols:
//
//  1. Functions that doc_portable.cpp previously provided for mushcore, but
//     doc_portable.cpp is excluded from the mushapp build (its globals and
//     many implementations conflict with the real ones in doc.cpp etc.).
//
//  2. Stubs for functions that exist only inside #ifdef _WIN32 guards in
//     other source files (evaluate.cpp, timers.cpp, scripting/scripting.cpp,
//     Utilities.cpp) but are called from code that compiles on all platforms.
//
//  3. Forwarding overloads for CMUSHclientDoc methods that doc_stub.h
//     declares with a slightly different signature than doc.h.
//
// Included in CMake target: mushapp (not mushcore).

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"   // full CMUSHclientApp / CMUSHclientDoc definition
#include "doc.h"
#include "OtherTypes.h"   // CLine, CStyle, HILITE, UNDERLINE
#include "scripting/errors.h"  // eOK
#include "platform/wx_osx_text.h"  // wxOSXActivateApp (utils.sendtofront)
#include <zlib.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>

// g_debug is defined in mushapp_globals.cpp; doc_stub.h declares it extern
// but can't be included here (conflicts with the full CMUSHclientDoc class).
extern bool g_debug;

// ── g_before_output / g_after_output ─────────────────────────────────────────
// Declared extern in platform/doc_stub.h; provided here for the full build.
// These were set by the former CLI to manage readline output (CLI now removed).
// In the GUI build they stay nullptr (the GUI handles its own output ordering).
void (*g_before_output)() = nullptr;
void (*g_after_output)()  = nullptr;

// ── dbg ──────────────────────────────────────────────────────────────────────
// Writes a dim debug line to stderr when g_debug is true.
void dbg(const char* fmt, ...)
{
    if (!g_debug) return;
    if (g_before_output) g_before_output();
    fprintf(stderr, "\033[2m[dbg] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\033[0m\n");
    fflush(stderr);
    if (g_after_output) g_after_output();
}

// ── telopt_name ───────────────────────────────────────────────────────────────
// Returns a human-readable name for a telnet option byte.
const char* telopt_name(unsigned char opt)
{
    switch (opt) {
    case   0: return "BINARY";
    case   1: return "ECHO";
    case   3: return "SGA";
    case  24: return "TERMINAL-TYPE";
    case  25: return "EOR";
    case  31: return "NAWS";
    case  32: return "TERMINAL-SPEED";
    case  34: return "LINEMODE";
    case  36: return "ENVIRON";
    case  39: return "NEW-ENVIRON";
    case  42: return "CHARSET";
    case  85: return "COMPRESS(MCCP1)";
    case  86: return "COMPRESS2(MCCP2)";
    case  90: return "MSP";
    case  91: return "MXP";
    case  93: return "ZMP";
    case 102: return "MUD-SPECIFIC(ATCP)";
    case 200: return "ATCP";
    case 201: return "GMCP";
    default: {
        static char buf[4][16];
        static int  idx = 0;
        snprintf(buf[idx & 3], sizeof buf[0], "<%d>", (int)opt);
        return buf[idx++ & 3];
    }
    }
}

// ── CheckObjectName ───────────────────────────────────────────────────────────
// Validates an object (trigger/alias/timer) name.  Stub always accepts.
long CheckObjectName(const CString& /*strName*/, const bool /*bConform*/)
{
    return eOK;
}

// ── SendToFront ───────────────────────────────────────────────────────────────
// utils.sendtofront(name): raise a matching application to the front.  On macOS
// this activates a running app whose name/bundle-id matches (wxOSXActivateApp);
// elsewhere it is a no-op returning false.
bool SendToFront(const char* name)
{
    return wxOSXActivateApp(name);
}


// ── CMUSHclientDoc::Note (CString overload) ───────────────────────────────────
void CMUSHclientDoc::Note(const CString& s)
{
    Note((LPCTSTR) s);
}

// ── CMUSHclientDoc::UpdateAllViews ────────────────────────────────────────────
// Emits completed lines to the wxWidgets output pane via the IWorldView seam.
// Walk m_LineList from m_gui_lines_emitted, stop before m_pCurrentLine
// (the line still being built).  Each CStyle run → SOutputRun.
//
// Uses BeginOutputBatch / EndOutputBatch (BeginBatch / EndBatch on the pane) to
// accumulate all new lines and trigger a single scrollbar update + repaint
// rather than one per line.
void CMUSHclientDoc::UpdateAllViews(void* /*hint*/)
{
    if (!HaveOutputSink()) return;

    // Skip lines already forwarded to the GUI.
    long toSkip = m_gui_lines_emitted;
    POSITION pos = m_LineList.GetHeadPosition();
    while (pos && toSkip > 0) {
        m_LineList.GetNext(pos);
        --toSkip;
    }

    // Fast-exit: nothing pending — but still repaint so that operations that
    // change the visual state without emitting a new line (e.g. RemoveChunk
    // after a buffer-size change, or a miniwindow deletion) still refresh the
    // canvas.
    if (!pos) {
        EmitRepaint();
        return;
    }

    bool batchOpen = false;

    // Emit each completed (non-current) line.
    while (pos) {
        CLine* pLine = m_LineList.GetNext(pos);
        if (pLine == m_pCurrentLine) break;  // in-progress — stop here

        // Open a paint batch on the first line we actually emit.
        if (!batchOpen) {
            BeginOutputBatch();
            batchOpen = true;
        }

        // Horizontal rule: emit a single sentinel run rather than text.
        if (pLine->flags & HORIZ_RULE) {
            SOutputLine outline;
            SOutputRun hrRun;
            hrRun.horiz_rule = true;
            outline.push_back(std::move(hrRun));
            EmitOutputLine(std::move(outline));
            ++m_gui_lines_emitted;
            continue;
        }

        SOutputLine outline;
        POSITION spos = pLine->styleList.GetHeadPosition();
        int charOff = 0;
        while (spos) {
            CStyle* pStyle = pLine->styleList.GetNext(spos);
            if (pStyle->iLength > 0) {
                SOutputRun run;
                GetStyleRGB(pStyle, run.fg, run.bg);
                run.bold          = (pStyle->iFlags & HILITE)    != 0;
                run.underline     = (pStyle->iFlags & UNDERLINE) != 0;
                run.italic        = (pStyle->iFlags & BLINK)     != 0;  // MUSHclient maps BLINK → italic
                run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
                run.text = std::string(pLine->text + charOff, pStyle->iLength);
                run.utf8 = m_bUTF_8;                      // non-UTF-8 worlds: tag the
                if (!m_bUTF_8)                            // bytes' charset so the pane
                    run.charset = (int) m_font_charset;   // decodes them (else blank)
                // MXP hyperlink / send-link: copy action and hint strings.
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                }
                outline.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }

        EmitOutputLine(std::move(outline));
        ++m_gui_lines_emitted;
    }

    // Close the batch (triggers a single scrollbar update + repaint).
    if (batchOpen) EndOutputBatch();

    // ── Emit the in-progress line as a pending update ──────────────────────
    // m_pCurrentLine is the line still being assembled (no trailing '\n' yet).
    // MUDs often send a prompt without a newline (e.g. "> " or "Press enter
    // to continue") — we emit it as a pending SOutputLine so the output pane
    // can display it as a dim trailing row even before the '\n' arrives.
    // Re-emitting on every UpdateAllViews call is cheap: AppendLine just
    // replaces m_pendingLine and schedules a single Refresh().
    if (m_pCurrentLine && m_pCurrentLine->len > 0) {
        SOutputLine pending;
        POSITION spos = m_pCurrentLine->styleList.GetHeadPosition();
        int charOff = 0;
        while (spos) {
            CStyle* pStyle = m_pCurrentLine->styleList.GetNext(spos);
            if (pStyle->iLength > 0) {
                SOutputRun run;
                GetStyleRGB(pStyle, run.fg, run.bg);
                run.bold          = (pStyle->iFlags & HILITE)    != 0;
                run.underline     = (pStyle->iFlags & UNDERLINE) != 0;
                run.italic        = (pStyle->iFlags & BLINK)     != 0;
                run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
                run.text = std::string(m_pCurrentLine->text + charOff, pStyle->iLength);
                run.utf8 = m_bUTF_8;
                if (!m_bUTF_8) run.charset = (int) m_font_charset;
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                }
                pending.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }
        if (!pending.empty()) {
            pending.pending = true;
            EmitOutputLine(std::move(pending));
        }
    }

    // Always trigger a repaint so miniwindow operations (WindowCreate,
    // WindowShow, WindowPosition, …) that call UpdateAllViews without emitting
    // any new text lines still refresh the canvas.
    EmitRepaint();
}

// ── CMUSHclientDoc::InitZlib ──────────────────────────────────────────────────
// Initialises a zlib inflate stream for MCCP decompression.
bool CMUSHclientDoc::InitZlib(z_stream& zs)
{
    zs.zalloc   = Z_NULL;
    zs.zfree    = Z_NULL;
    zs.opaque   = Z_NULL;
    zs.avail_in = 0;
    zs.next_in  = Z_NULL;
    return (inflateInit(&zs) == Z_OK);
}

// ── CMUSHclientDoc::SendNAWS ──────────────────────────────────────────────────
// Sends a NAWS (Negotiate About Window Size) subnegotiation packet directly,
// bypassing the Windows view-iteration loop in SendWindowSizes().
// Called from CWorldChildFrame::OnSize whenever the output pane is resized.
// Clamps cols/rows to [1, 65535] to produce valid two-byte big-endian values.
void CMUSHclientDoc::SendNAWS(int cols, int rows)
{
    cols = std::max(1, std::min(cols, 65535));
    rows = std::max(1, std::min(rows, 65535));

    // Always update m_gui_rows so that the next SendWindowSizes call
    // (triggered by e.g. a wrap-column change) uses the correct row count.
    m_gui_rows = rows;

    if (m_iConnectPhase != eConnectConnectedToMud ||
        !ConnectionOpen() || !m_bNAWS_wanted)
        return;

    unsigned char p[] = {
        (unsigned char)IAC, (unsigned char)SB, (unsigned char)TELOPT_NAWS,
        (unsigned char)HIBYTE(cols), (unsigned char)LOBYTE(cols),
        (unsigned char)HIBYTE(rows), (unsigned char)LOBYTE(rows),
        (unsigned char)IAC, (unsigned char)SE
    };
    SendPacket(p, sizeof p);
}

// ── CMUSHclientDoc::CheckTickTimers ───────────────────────────────────────────
// Windows-only timer callback (WM_TIMER).  No-op on non-Windows.
void CMUSHclientDoc::CheckTickTimers()
{
}

// ── CMUSHclientDoc::CreateMonitoringThread ────────────────────────────────────
// Creates a Windows monitoring thread.  No-op on non-Windows.
void CMUSHclientDoc::CreateMonitoringThread()
{
}

// ── Set_Up_Set_Strings: non-Windows version ───────────────────────────────────
// Returns the file extension, dialog title, and wxWidgets wildcard filter string
// for each set type.  Returns TRUE on bad set_type (mirrors Windows version).
BOOL Set_Up_Set_Strings(int set_type,
                                CString& suggested_name,
                                CString& filter,
                                CString& title,
                                CString& ext)
{
    switch (set_type) {
    case TRIGGER:
        ext    = "mct";
        filter = "MUSHclient trigger files (*.mct)|*.mct|All files (*.*)|*.*";
        title  = "Trigger file";
        break;
    case ALIAS:
        ext    = "mca";
        filter = "MUSHclient alias files (*.mca)|*.mca|All files (*.*)|*.*";
        title  = "Alias file";
        break;
    case COLOUR:
        ext    = "mcc";
        filter = "MUSHclient colour files (*.mcc)|*.mcc|All files (*.*)|*.*";
        title  = "Colour file";
        break;
    case MACRO:
        ext    = "mcm";
        filter = "MUSHclient macro files (*.mcm)|*.mcm|All files (*.*)|*.*";
        title  = "Macro file";
        break;
    case TIMER:
        ext    = "mci";
        filter = "MUSHclient timer files (*.mci)|*.mci|All files (*.*)|*.*";
        title  = "Timer file";
        break;
    default:
        return TRUE;   // bad set_type
    }
    return FALSE;
}

// ── CMUSHclientDoc::Load_Set ──────────────────────────────────────────────────
// Loads a trigger/alias/colour/macro/timer set from an XML .mc? file.
// When strFileName is empty a file-open dialog is shown; otherwise the file is
// loaded directly (used by doc_construct.cpp for default-set files).
BOOL CMUSHclientDoc::Load_Set(const int set_type,
                               CString   strFileName,
                               CWnd*     pParentWnd)
{
    BOOL replace = TRUE;

    if (strFileName.IsEmpty()) {
        CString suggested_name = m_mush_name;
        CString filter, title, ext;
        if (Set_Up_Set_Strings(set_type, suggested_name, filter, title, ext))
            return TRUE;    // bad set_type

        CFileDialog filedlg(TRUE,   // open
                            ext,
                            "",
                            OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                            filter,
                            pParentWnd);
        filedlg.m_ofn.lpstrTitle = title;

        ChangeToFileBrowsingDirectory();
        int nResult = filedlg.DoModal();
        ChangeToStartupDirectory();

        if (nResult != IDOK)
            return TRUE;   // cancelled

        if (set_type == TRIGGER && !m_TriggerMap.IsEmpty()) {
            if (::TMessageBox("Replace existing triggers?\n"
                              "If you reply \"No\", then triggers from the file"
                              " will be added to existing triggers",
                              MB_YESNO | MB_ICONQUESTION) == IDNO)
                replace = FALSE;
        } else if (set_type == ALIAS && !m_AliasMap.IsEmpty()) {
            if (::TMessageBox("Replace existing aliases?\n"
                              "If you reply \"No\", then aliases from the file"
                              " will be added to existing aliases",
                              MB_YESNO | MB_ICONQUESTION) == IDNO)
                replace = FALSE;
        } else if (set_type == TIMER && !m_TimerMap.IsEmpty()) {
            if (::TMessageBox("Replace existing timers?\n"
                              "If you reply \"No\", then timers from the file"
                              " will be added to existing timers",
                              MB_YESNO | MB_ICONQUESTION) == IDNO)
                replace = FALSE;
        }
        strFileName = filedlg.GetPathName();
    }

    CFile*    f  = nullptr;
    CArchive* ar = nullptr;
    try {
        f  = new CFile(strFileName, CFile::modeRead | CFile::shareDenyWrite);
        ar = new CArchive(f, CArchive::load);

        if (IsArchiveXML(*ar)) {
            switch (set_type) {
            case TRIGGER:
                if (replace) DELETE_MAP(m_TriggerMap, CTrigger);
                Load_World_XML(*ar, XML_TRIGGERS | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case ALIAS:
                if (replace) DELETE_MAP(m_AliasMap, CAlias);
                Load_World_XML(*ar, XML_ALIASES  | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case COLOUR:
                Load_World_XML(*ar, XML_COLOURS  | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case MACRO:
                Load_World_XML(*ar, XML_MACROS   | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case TIMER:
                if (replace) DELETE_MAP(m_TimerMap, CTimer);
                Load_World_XML(*ar, XML_TIMERS   | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            }
        } else {
            ::TMessageBox("File does not have a valid MUSHclient XML signature.",
                          MB_ICONSTOP);
            AfxThrowArchiveException(CArchiveException::badSchema);
        }
    }
    catch (CFileException* e)    { ::UMessageBox(TFormat("Unable to open or read %s", (LPCTSTR)strFileName), MB_ICONSTOP); e->Delete(); }
    catch (CMemoryException* e)  { ::TMessageBox("Insufficient memory to do this operation", MB_ICONEXCLAMATION); e->Delete(); }
    catch (CArchiveException* e) { ::TMessageBox("There was a problem in the data format", MB_ICONEXCLAMATION); e->Delete(); }
    catch (...)                  { ::TMessageBox("Unknown error loading file", MB_ICONEXCLAMATION); }

    delete ar;
    delete f;
    SetModifiedFlag(TRUE);
    return FALSE;   // FALSE = OK (matches Windows convention)
}

// ── CMUSHclientDoc::Save_Set ──────────────────────────────────────────────────
// Saves a trigger/alias/colour/macro/timer set to an XML .mc? file.
BOOL CMUSHclientDoc::Save_Set(const int set_type,
                               CWnd*     pParentWnd)
{
    CString suggested_name = m_mush_name;
    CString filter, title, ext;
    if (Set_Up_Set_Strings(set_type, suggested_name, filter, title, ext))
        return TRUE;    // bad set_type

    // Strip characters invalid in filenames
    int i;
    while ((i = suggested_name.FindOneOf("<>\"|?:#%;/\\")) != -1)
        suggested_name = suggested_name.Left(i) + suggested_name.Mid(i + 1);

    CFileDialog filedlg(FALSE,  // save
                        ext,
                        suggested_name,
                        OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                        filter,
                        pParentWnd);
    filedlg.m_ofn.lpstrTitle = title;

    ChangeToFileBrowsingDirectory();
    int nResult = filedlg.DoModal();
    ChangeToStartupDirectory();

    if (nResult != IDOK)
        return TRUE;   // cancelled

    CPlugin* pSavedPlugin = m_CurrentPlugin;
    m_CurrentPlugin = nullptr;   // save main triggers, not plugin ones

    BOOL error = TRUE;
    CFile*    f  = nullptr;
    CArchive* ar = nullptr;
    try {
        f  = new CFile(filedlg.GetPathName(), CFile::modeCreate | CFile::modeReadWrite);
        ar = new CArchive(f, CArchive::store);

        switch (set_type) {
        case TRIGGER: Save_World_XML(*ar, XML_TRIGGERS); break;
        case ALIAS:   Save_World_XML(*ar, XML_ALIASES);  break;
        case COLOUR:  Save_World_XML(*ar, XML_COLOURS);  break;
        case MACRO:   Save_World_XML(*ar, XML_MACROS);   break;
        case TIMER:   Save_World_XML(*ar, XML_TIMERS);   break;
        }
        error = FALSE;
    }
    catch (CFileException* e)    { ::TMessageBox("Unable to create the requested file", MB_ICONEXCLAMATION); e->Delete(); }
    catch (CMemoryException* e)  { ::TMessageBox("Insufficient memory to do this operation", MB_ICONEXCLAMATION); e->Delete(); }
    catch (CArchiveException* e) { ::TMessageBox("There was a problem in the data format", MB_ICONEXCLAMATION); e->Delete(); }

    m_CurrentPlugin = pSavedPlugin;
    delete ar;
    delete f;
    return error;
}

// GamePreferences / OnGamePreferences are no longer stubbed here: the canonical
// implementations in dialogs/world_prefs/configuration.cpp now compile on the
// port and build the real CTreePropertySheet (rendered as a native wxTreebook
// via hook_run_property_sheet).

// ── CMUSHclientDoc::SortTimers ────────────────────────────────────────────────
// On Windows this rebuilds a sorted timer display list.  On non-Windows the
// timer map is iterated directly, so this is a no-op (matching doc_stub.h).
void CMUSHclientDoc::SortTimers()
{
}

// ── BrowseCallbackProc ────────────────────────────────────────────────────────
// Windows SHBrowseForFolder callback — never called on POSIX because
// SHBrowseForFolder itself is inside #ifdef _WIN32 everywhere it's invoked
// (plugins.cpp, GlobalPrefs.cpp).  Provide an empty stub so the link succeeds.
// Signature must match the forward declarations in plugins.cpp / lua_utils.cpp /
// prefspropertypages.cpp (HWND=void*, UINT=unsigned int, LPARAM=long on POSIX).
int __stdcall BrowseCallbackProc(HWND /*hwnd*/, UINT /*uMsg*/,
                                 LPARAM /*lParam*/, LPARAM /*lpData*/)
{
    return 0;
}

// ── RemoveTrailingBackslash ───────────────────────────────────────────────────
// Defined inside #ifdef _WIN32 in GlobalPrefs.cpp; called from plugins.cpp
// (SaveState) on all platforms.  Trim whitespace then strip any trailing
// path separator (either \ or /).
void RemoveTrailingBackslash(CString& str)
{
    str.TrimLeft();
    str.TrimRight();
    if (!str.IsEmpty()) {
        char last = str[str.GetLength() - 1];
        if (last == '\\' || last == '/')
            str = str.Left(str.GetLength() - 1);
    }
}

// ── CMDIFrameWnd::LoadFrame ───────────────────────────────────────────────────
// Defined here (not inline in mfc_ui.h) so it is always recompiled when this
// .cpp changes, avoiding stale-inline issues with incremental builds.
// Calls g_wxhook_load_mdi_frame which wx_mfc_impl.cpp sets before InitInstance.
BOOL CMDIFrameWnd::LoadFrame(UINT id, DWORD style, CWnd* pParent, void* /*pContext*/)
{
    if (g_wxhook_load_mdi_frame)
        return g_wxhook_load_mdi_frame(
            this, id, (unsigned long)style,
            pParent ? pParent->m_hWnd : nullptr) != 0;
    return FALSE;
}

// FireWorld{GetFocus,LoseFocus,Close,Save} are single-sourced in
// doc_scripting_shared.cpp (shared with mushcore; was a duplicate here).

// ── UMessageBox / TMessageBox ─────────────────────────────────────────────
// Route through g_wxhook_message_box when it is set (mushgui at runtime);
// fall back to stderr for headless builds.
int UMessageBox(const char* sText, UINT nType, const char* caption) {
    if (g_wxhook_message_box)
        return g_wxhook_message_box(sText ? sText : "", caption ? caption : "MUSHclient", nType);
    fprintf(stderr, "MUSHclient [%s]: %s\n", caption ? caption : "MUSHclient", sText ? sText : "");
    return (nType & MB_YESNO) ? IDNO : IDOK;
}
int TMessageBox(const char* sText, UINT nType) {
    return UMessageBox(sText, nType, "MUSHclient");
}

#endif // !_WIN32
