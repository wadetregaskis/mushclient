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
#include <zlib.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>

// g_debug is defined in mushapp_globals.cpp; doc_stub.h declares it extern
// but can't be included here (conflicts with the full CMUSHclientDoc class).
extern bool g_debug;

// ── g_before_output / g_after_output ─────────────────────────────────────────
// Declared extern in platform/doc_stub.h; provided here for the full build.
// In the CLI build these are set by cli_main.cpp to manage readline output.
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
// Raises an editor window to the front (Windows-only feature).
bool SendToFront(const char* /*name*/)
{
    return false;
}

// ── Set_Up_Set_Strings ────────────────────────────────────────────────────────
// Populates file-dialog strings for a settings-file type (Windows-only).
// Returns TRUE (= error / not applicable) on non-Windows.
BOOL Set_Up_Set_Strings(const int   /*set_type*/,
                         CString   & /*suggested_name*/,
                         CString   & /*filter*/,
                         CString   & /*title*/,
                         CString   & /*suggested_extension*/)
{
    return TRUE;
}

// ── CMUSHclientDoc::Note (CString overload) ───────────────────────────────────
void CMUSHclientDoc::Note(const CString& s)
{
    Note((LPCTSTR) s);
}

// ── CMUSHclientDoc::UpdateAllViews ────────────────────────────────────────────
// Emits completed lines to the wxWidgets output pane via m_fn_line_ready.
// Walk m_LineList from m_gui_lines_emitted, stop before m_pCurrentLine
// (the line still being built).  Each CStyle run → SOutputRun.
//
// Uses m_fn_begin_batch / m_fn_flush (BeginBatch / EndBatch on the pane) to
// accumulate all new lines and trigger a single scrollbar update + repaint
// rather than one per line.
void CMUSHclientDoc::UpdateAllViews(void* /*hint*/)
{
    if (!m_fn_line_ready) return;

    // Skip lines already forwarded to the GUI.
    long toSkip = m_gui_lines_emitted;
    POSITION pos = m_LineList.GetHeadPosition();
    while (pos && toSkip > 0) {
        m_LineList.GetNext(pos);
        --toSkip;
    }

    // Fast-exit: nothing pending.
    if (!pos) return;

    bool batchOpen = false;

    // Emit each completed (non-current) line.
    while (pos) {
        CLine* pLine = m_LineList.GetNext(pos);
        if (pLine == m_pCurrentLine) break;  // in-progress — stop here

        // Open a paint batch on the first line we actually emit.
        if (!batchOpen) {
            if (m_fn_begin_batch) m_fn_begin_batch();
            batchOpen = true;
        }

        // Horizontal rule: emit a single sentinel run rather than text.
        if (pLine->flags & HORIZ_RULE) {
            SOutputLine outline;
            SOutputRun hrRun;
            hrRun.horiz_rule = true;
            outline.push_back(std::move(hrRun));
            m_fn_line_ready(std::move(outline));
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
                run.bold      = (pStyle->iFlags & HILITE)    != 0;
                run.underline = (pStyle->iFlags & UNDERLINE) != 0;
                run.text = std::string(pLine->text + charOff, pStyle->iLength);
                // MXP hyperlink / send-link: copy action and hint strings.
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                }
                outline.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }

        m_fn_line_ready(std::move(outline));
        ++m_gui_lines_emitted;
    }

    // Close the batch (triggers a single scrollbar update + repaint).
    if (batchOpen && m_fn_flush) m_fn_flush();

    // Always trigger a repaint so miniwindow operations (WindowCreate,
    // WindowShow, WindowPosition, …) that call UpdateAllViews without emitting
    // any new text lines still refresh the canvas.
    if (m_fn_repaint) m_fn_repaint();
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
        !m_pSocket || !m_bNAWS_wanted)
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

// ── CMUSHclientDoc::Load_Set ──────────────────────────────────────────────────
// Loads a settings file via a Windows file dialog.  Not implemented on non-Windows.
BOOL CMUSHclientDoc::Load_Set(const int /*set_type*/,
                               CString   /*strFileName*/,
                               CWnd*     /*pParentWnd*/)
{
    return TRUE;   // TRUE = error / not applicable
}

// ── CMUSHclientDoc::Save_Set ──────────────────────────────────────────────────
// Saves a settings file via a Windows file dialog.  Not implemented on non-Windows.
BOOL CMUSHclientDoc::Save_Set(const int /*set_type*/,
                               CWnd*     /*pParentWnd*/)
{
    return TRUE;   // TRUE = error / not applicable
}

// ── CMUSHclientDoc::GamePreferences ──────────────────────────────────────────
// Shows the world-preferences dialog.
// On non-Windows, calls g_wxhook_game_preferences (set by wx_mfc_impl.cpp) which
// shows a real wxWidgets dialog.  Falls back to true (accepted) if hook is null.
bool CMUSHclientDoc::GamePreferences(const int iPage)
{
    if (g_wxhook_game_preferences)
        return g_wxhook_game_preferences(this, iPage);
    return true;
}

// ── CMUSHclientDoc::OnGamePreferences ────────────────────────────────────────
// MFC message-map handler for ID_GAME_PREFERENCES.  Calls GamePreferences(-1)
// to open on the last-used page.  On non-Windows the dialog is a no-op stub.
void CMUSHclientDoc::OnGamePreferences()
{
    GamePreferences(-1);
}

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

#endif // !_WIN32
