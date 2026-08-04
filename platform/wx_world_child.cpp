// platform/wx_world_child.cpp — MDI child frame: output pane + command input.
//
// Include order is critical (same rule as wx_mfc_impl.cpp):
//   1. wx headers first (so wx's min/max, MessageBox etc. are not shadowed)
//   2. MUSHclient / MFC-compat headers after

#ifndef _WIN32

// ── wxWidgets ─────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/config.h>
#include <wx/clipbrd.h>   // wxTheClipboard, wxTextDataObject, wxDF_TEXT
#include <wx/display.h>   // wxDisplay (usable screen area for the default size)
#include <string>
#include <netdb.h>  // getaddrinfo, freeaddrinfo

// ── MUSHclient headers ────────────────────────────────────────────────────
// stdafx.h and MUSHclient.h must precede doc.h: MUSHclient.h pulls in
// OtherTypes.h (MACRO_COUNT, CAliasMap, CTriggerMap, …) that doc.h needs.
#include "stdafx.h"                   // compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"               // CMUSHclientApp + OtherTypes.h + all prerequisites
#include "doc.h"                      // CMUSHclientDoc, SendMsg, etc.
#include "dialogs/GlobalChangeDlg.h"  // CGlobalChangeDlg (Game ▸ Global Change)
#include "dialogs/GeneratedNameDlg.h" // CGeneratedNameDlg (Game ▸ Generate Character Name)
#include "dialogs/HighlightPhraseDlg.h"  // CHighlightPhraseDlg (Display ▸ Highlight Word)
#include "dialogs/MultiLineTriggerDlg.h" // CMultiLineTriggerDlg (Display ▸ Multi-Line Trigger)
#include "dialogs/TextAttributesDlg.h"   // CTextAttributesDlg (Display ▸ Text Attributes)
#include "miniwindow.h"               // CMiniWindow, MiniWindowVector, MINIWINDOW_* flags
#include "scripting/scripting.h"      // CScriptEngine, lua_State* — for Shift+Tab globals
#include "platform/wx_world_child.h"  // our own header (CWorldChildFrame, COutputPane)
#include "platform/wx_print.h"        // PrintStyledDocument (File ▸ Print World)
#include "platform/wx_osx_text.h"    // wxOSXSetWindowTabbing ("Window tabs" pref)
#include "genprint.h"                 // FONT_BOLD / FONT_ITALIC / FONT_UNDERLINE
#include <wx/numdlg.h>                // wxGetNumberFromUser (Display ▸ Go To Line)

// MW_MOUSE_* flag constants (from mushview.h — copied here to avoid pulling in
// all of mushview.h which declares the Windows-only CMUSHView class).
#ifndef MW_MOUSE_SHIFT
#  define MW_MOUSE_SHIFT       0x01
#  define MW_MOUSE_CTRL        0x02
#  define MW_MOUSE_ALT         0x04
#  define MW_MOUSE_LH          0x10
#  define MW_MOUSE_RH          0x20
#  define MW_MOUSE_DBL         0x40
#  define MW_MOUSE_NOT_FIRST   0x80
#  define MW_MOUSE_SCROLL_BACK 0x100
#endif
#include "platform/wx_gdi_compat.h"   // WxDCFromCDC
#include "platform/wx_mfc_hooks.h"    // g_wxhook_set_status

#include <list>
#include <set>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

// Defined in wx_gdi_compat.cpp (same convention as wxgdi_read_bgra in
// wx_surface.cpp): the backing-store scale miniwindow canvases are created at —
// 2 on a Retina panel, 1 on an ordinary one.
double wx_canvas_scale();

// App singleton — defined in platform/mushapp_globals.cpp for the mushapp build.
extern CMUSHclientApp App;

// ── Event IDs ─────────────────────────────────────────────────────────────
static const int ID_INPUT        = wxID_HIGHEST + 200;
static const int ID_WORLD_TIMER  = wxID_HIGHEST + 201;
static const int ID_FIND_INPUT   = wxID_HIGHEST + 202;
static const int ID_FIND_NEXT    = wxID_HIGHEST + 203;
static const int ID_FIND_PREV    = wxID_HIGHEST + 204;

// Fallback command-history depth, used only when the doc is unavailable.  The
// real limit is the world's own "Lines in command history" setting
// (m_nHistoryLines, default 1000, range 20-5000) — see HistoryLimit().
static const int kMaxHistoryFallback = 1000;

// ── wx_save_child_geometry ────────────────────────────────────────────────
// The only writer of /geometry/child_w + /geometry/child_h — the keys this
// frame's constructor reads back (see the "World window geometry" block
// there).  Declared in wx_world_child.h; see the note there for why the quit
// hook has to share it.
void wx_save_child_geometry(wxConfigBase& cfg, const wxSize& size)
{
    // Same floor as the constructor's clamp: a smaller value would be thrown
    // away on the next launch anyway, and a zero-size (never-laid-out or
    // already-torn-down) window must not overwrite a good saved size.
    if (size.x >= 200 && size.y >= 100) {
        cfg.Write(wxT("/geometry/child_w"), (long) size.x);
        cfg.Write(wxT("/geometry/child_h"), (long) size.y);
    }
}

// ── History file path ─────────────────────────────────────────────────────────
// Returns the path to the per-world persistent command-history file, creating
// ~/.mushclient/ if needed.  Uses the world name to distinguish files so that
// different worlds maintain separate histories.  Returns "" on failure.
//
// The world name is sanitised: only [A-Za-z0-9._-] are kept; runs of other
// characters are collapsed to a single underscore.  E.g. "Broken Lands"
// → "history_Broken_Lands.txt".  Falls back to "history.txt" if empty.
static std::string GetHistoryFilePath(const std::string& worldName)
{
    const char* home = getenv("HOME");
    if (!home || !*home) return {};
    std::string dir = std::string(home) + "/.mushclient";
    ::mkdir(dir.c_str(), 0700);   // safe no-op if already exists

    if (worldName.empty()) return dir + "/history.txt";

    // Sanitise: keep alphanumeric + a handful of safe punctuation chars;
    // collapse anything else to a single underscore.
    std::string safe;
    for (unsigned char c : worldName) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            safe += (char)c;
        } else {
            if (!safe.empty() && safe.back() != '_')
                safe += '_';
        }
    }
    // Trim trailing underscores left by a name ending in punctuation.
    while (!safe.empty() && safe.back() == '_') safe.pop_back();
    if (safe.empty()) safe = "default";

    return dir + "/history_" + safe + ".txt";
}

// The MUD should wrap to the VISIBLE text width: the full output width minus any
// area a plugin reserved via world.TextRectangle (e.g. Quow's docked
// miniwindows).  Without this the MUD wraps to the full width and the client
// re-wraps long lines unexpectedly.  m_gui_cols / GetInfo(281) stays the FULL
// width — plugins compute their own margins from it; only the engine wrap column
// and NAWS use this reduced count.
static int EffectiveTextCols(CMUSHclientDoc* doc, int paneW, int charW)
{
    if (charW <= 0) return 1;
    int textW = paneW;
    if (doc)
    {
        const CRect& tr = doc->m_TextRectangle;
        if (!(tr.left == 0 && tr.top == 0 && tr.right == 0 && tr.bottom == 0))
        {
            int rw = (int) (tr.right - tr.left);
            if (rw > 0 && rw < paneW) textW = rw;
        }
        // ...and by the world's pixel offset, which insets the text from the
        // left edge of the window.  This is the SAME formula the renderer uses
        // in COutputPane::RebuildWrapCache, and the two must change together
        // forever: this one sets m_nWrapColumn (the engine's hard wrap, read at
        // doc_shared.cpp:2839), that one sets m_wrapCols (the visual wrap).  If
        // the renderer's were the narrower of the two, every full-width engine
        // line would spill a one- or two-character continuation row.
        // Windows keeps them equal by construction -- it has one formula,
        // mushview.cpp:3220: (GetOutputWindowWidth() - m_iPixelOffset) / charW.
        textW -= (int) doc->m_iPixelOffset;
    }
    return std::max(1, textW / charW);
}

// ── Miniwindow compositing ────────────────────────────────────────────────
// One paint pass of a doc's miniwindows onto destDC — the wx twin of
// CMUSHView::OnDraw's two miniwindow loops (mushview.cpp:996-1026 and
// :1497-1524) plus the Blit_Bitmap they call (mushview.cpp:684-847).
// A free function, not a lambda, so the in-app compositor selftest
// (platform/wx_composite_selftest.cpp) can drive the real body.
//
//   clientW/clientH — the output pane's client size ("client rect")
//   frameW/frameH   — the owning child frame's client size ("owner rect")
//   bUnder          — this pass draws the MINIWINDOW_DRAW_UNDERNEATH set
void wx_composite_miniwindows(CMUSHclientDoc* doc, wxDC& destDC,
                              int clientW, int clientH,
                              int frameW, int frameH, bool bUnder)
{
    if (!doc) return;
    MiniWindowVector& order = doc->m_MiniWindowsOrder;
    if (order.empty()) return;

    // ── Phase 1: compute m_rect for this pass's miniwindows ─────────
    // Engine-owned placement maths shared verbatim with the Windows view
    // (CalculateMiniWindowRectangles, miniwindow_compose.cpp, docs/PAL.md
    // S4). Runs per pass with pass-scoped corner tracking, exactly as
    // CMUSHView::OnDraw always has — including the original's quirky
    // m_rect right/bottom values for corner/wall position modes, which
    // Phase 2 then normalizes exactly as Blit_Bitmap does.
    // "frame" rect = the child frame; "client" rect = the output pane.
    {
    RECT clientrect = { 0, 0, clientW, clientH };
    RECT ownerrect  = { 0, 0, frameW, frameH };
    CalculateMiniWindowRectangles(doc, bUnder, clientrect, ownerrect);
    }

    // ── Phase 2: blit each visible miniwindow onto destDC ───────────
    for (auto& kv : order) {
        CMiniWindow* mw = kv.second;
        if (!mw->GetShow() || mw->m_bTemporarilyHide) continue;

        bool wantUnder = (mw->GetFlags() & MINIWINDOW_DRAW_UNDERNEATH) != 0;
        if (wantUnder != bUnder) continue;

        wxDC* srcDC = WxDCFromCDC(mw->GetDC());
        if (!srcDC) continue;

        long iW = mw->GetWidth(), iH = mw->GetHeight();
        if (iW <= 0 || iH <= 0) continue;

        bool bTransparent = (mw->GetFlags() & MINIWINDOW_TRANSPARENT) != 0;
        short pos = mw->GetPosition();

        // Tile mode: special case (Blit_Bitmap's `case 13`, which tiles the
        // canvas across the client rect and returns before any other blit).
        // It is also the one mode CalculateMiniWindowRectangles skips
        // (`case 13: continue`), because Blit_Bitmap sets the rect itself:
        // `rect = r`, the view's client rect, and returns before the
        // position/size normalization below.  Without this the port left a
        // tiled window's m_rect at (0,0,0,0) and WindowInfo 12/13 answered 0.
        if (pos == 13) {
            mw->m_rect = CRect(0, 0, clientW, clientH);
            int across = clientW / iW + 1;
            int down   = clientH / iH + 1;
            for (int tx = 0; tx < across; ++tx)
                for (int ty = 0; ty < down; ++ty)
                    destDC.Blit(tx * iW, ty * iH, iW, iH, srcDC, 0, 0, wxCOPY);
            continue;
        }

        const int destX = mw->m_rect.left, destY = mw->m_rect.top;

        // NO stretch arm, for any position mode.  OnDraw passes
        // bUseRect = (GetPosition() != 13), so Blit_Bitmap's
        // `if (iMode <= 3 && !bUseRect)` StretchBlt is FALSE for every
        // miniwindow and it falls through to the 1:1 BitBlt below — the
        // stretch is reachable only from DrawImage (the world's background /
        // foreground bitmaps), which the port draws in draw_doc_image.
        // The port used to StretchBlit positions 0-3 to the landed rect,
        // which blew a 200x100 canvas up to the whole pane while hit-testing
        // stayed at 200x100 (miniwindow_compose.cpp reads left/top + size),
        // so hotspots fired nowhere near where they were drawn.
        if (bTransparent) {
            // Build a masked bitmap using the miniwindow's background colour
            // as the transparency key, then draw it with the mask applied.
            COLORREF bg = mw->GetBackgroundColour();
            wxColour transCol(GetRValue(bg), GetGValue(bg), GetBValue(bg));
            // The intermediate MUST carry the canvas's scale.  A plain
            // wxBitmap(iW, iH) is 1x, so copying a HiDPI canvas into it
            // throws away three quarters of the pixels and hands the
            // compositor a blurred image — which is exactly what made
            // plugin text fuzzy while the maps drawn straight through the
            // opaque path stayed sharp.
            wxBitmap srcBmp;
            if (!srcBmp.CreateWithDIPSize(wxSize(iW, iH), wx_canvas_scale(), 32))
                srcBmp = wxBitmap(iW, iH);
            {
                wxMemoryDC tmpDC(srcBmp);
                tmpDC.Blit(0, 0, iW, iH, srcDC, 0, 0, wxCOPY);
            }   // tmpDC destructor deselects srcBmp
            srcBmp.SetMask(new wxMask(srcBmp, transCol));
            destDC.DrawBitmap(srcBmp, destX, destY, /*useMask=*/true);
        } else {
            destDC.Blit(destX, destY, iW, iH, srcDC, 0, 0, wxCOPY);
        }

        // Blit_Bitmap's closing two lines (mushview.cpp:844-846):
        //   // make into rectangle, not position and width/height
        //   rect.right += rect.left;  rect.bottom += rect.top;
        // Its bUseRect arm first stuffs the WIDTH/HEIGHT into right/bottom, so
        // after a paint every miniwindow's m_rect is a NORMALIZED rectangle —
        // which is what WindowInfo 12/13 hand to scripts (miniwindow.cpp:1066).
        // The port used to stop at the width/height half, so an absolute or
        // corner-positioned window reported a right edge to the left of its
        // own left edge.
        mw->m_rect.right  = destX + iW;
        mw->m_rect.bottom = destY + iH;
    }
}   // end of wx_composite_miniwindows

// ── Miniwindow tooltip ────────────────────────────────────────────────────
// Resolve the tooltip for a client-space point that may be over a miniwindow.
// Returns TRUE when the point is over one — the caller must then use `text`
// and NOT fall through to the output text underneath, even when `text` comes
// back empty.  That is CMUSHView::Mouse_Tooltip_MiniWindow
// (mushview.cpp:6582-6640): `if (!mw) return false;` up front, an empty
// buffer + `return true` for a window with no hotspot tooltip, and a caller
// (OnToolTipNeedText, mushview.cpp:4319) that returns immediately on true.
// The port used to discard the CMiniWindow* and fall through, so hovering an
// opaque miniwindow leaked the MXP hint of the text it was covering.
bool wx_miniwindow_tooltip(CMUSHclientDoc* doc, int x, int y, std::string& text)
{
    text.clear();
    if (!doc) return false;

    std::string sHotspotId, sMiniwindowId;
    CHotspot* pHotspot = nullptr;
    CMiniWindow* mw = MouseOverMiniWindow(doc, CPoint(x, y),
                                          sHotspotId, pHotspot, sMiniwindowId);
    if (!mw) return false;          // not over a miniwindow at all

    if (pHotspot && !pHotspot->m_sTooltipText.empty()) {
        // "title\tbody" is a two-part tooltip: Windows puts the part before
        // the tab in the tooltip's bold TITLE (TTM_SETTITLE) and the rest in
        // the body, consuming the tab (mushview.cpp:6601-6620).  wxToolTip
        // has no title concept, so join with a newline -- the tab itself must
        // not survive, which is what the port used to show.
        const std::string& tip = pHotspot->m_sTooltipText;
        const std::string::size_type tab = tip.find('\t');
        if (tab == std::string::npos)
            text = tip;
        else
            text = tip.substr(0, tab) + "\n" + tip.substr(tab + 1);
    }
    return true;    // over a miniwindow: this answer is final
}   // end of wx_miniwindow_tooltip

// ── WxMouseHooks ──────────────────────────────────────────────────────────
// The view services the engine-owned miniwindow mouse routing calls back into
// (MiniWindowMouseHooks, miniwindow.h): capture + keyboard modifiers.
// Tooltips are a no-op here because the pane polls m_fn_get_tooltip on every
// motion, and mouse-leave needs no arming — wx delivers wxEVT_LEAVE_WINDOW
// natively.  File scope (it used to be local to the constructor) so
// CWorldChildFrame::mouseMoveMiniWindow can build one too.
struct WxMouseHooks : public MiniWindowMouseHooks {
    COutputPane* pane;
    WxMouseHooks(COutputPane* p) : pane(p) {}
    virtual bool IsCaptured() { return pane && pane->HasCapture(); }
    virtual void SetMouseCapture() {
        if (pane && !pane->HasCapture()) pane->CaptureMouse();
    }
    virtual void ReleaseMouseCapture() {
        if (pane && pane->HasCapture()) pane->ReleaseMouse();
    }
    virtual long KeyboardModifiers() {
        long flags = 0;
        if (wxGetKeyState(WXK_SHIFT))       flags |= MW_MOUSE_SHIFT;
        if (wxGetKeyState(WXK_RAW_CONTROL)) flags |= MW_MOUSE_CTRL;
        if (wxGetKeyState(WXK_ALT))         flags |= MW_MOUSE_ALT;
        return flags;
    }
};

// ── Event table ───────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(CWorldChildFrame, wxMDIChildFrame)
    EVT_TEXT_ENTER(ID_INPUT,              CWorldChildFrame::OnInputEnter)
    EVT_TIMER(     ID_WORLD_TIMER,        CWorldChildFrame::OnWorldTimer)
    EVT_CLOSE(                            CWorldChildFrame::OnClose)
    EVT_SIZE(                             CWorldChildFrame::OnSize)
    EVT_ACTIVATE(                         CWorldChildFrame::OnActivate)
wxEND_EVENT_TABLE()

// ── Constructor ───────────────────────────────────────────────────────────
CWorldChildFrame::CWorldChildFrame(wxMDIParentFrame* parent,
                                   CMUSHclientDoc*   doc,
                                   const wxString&   title)
    : wxMDIChildFrame(parent, wxID_ANY, title,
                      wxDefaultPosition, wxSize(800, 600))
    , m_doc(doc)
{
    // Vertical sizer: output pane takes all space; input bar at bottom.
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    m_output = new COutputPane(this);
    sizer->Add(m_output, 1 /*stretch*/, wxEXPAND);

    // ── Find bar (hidden by default, shown by Ctrl+F) ─────────────────────
    m_findBar = new wxPanel(this, wxID_ANY);
    {
        wxBoxSizer* fs = new wxBoxSizer(wxHORIZONTAL);
        fs->Add(new wxStaticText(m_findBar, wxID_ANY, wxT("Find:")),
                0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
        m_findInput = new wxTextCtrl(m_findBar, ID_FIND_INPUT, wxEmptyString,
                                     wxDefaultPosition, wxSize(220, -1),
                                     wxTE_PROCESS_ENTER);
        fs->Add(m_findInput, 0, wxALIGN_CENTER_VERTICAL);
        wxButton* btnNext = new wxButton(m_findBar, ID_FIND_NEXT, wxT("▼"),
                                         wxDefaultPosition, wxSize(28, -1));
        wxButton* btnPrev = new wxButton(m_findBar, ID_FIND_PREV, wxT("▲"),
                                         wxDefaultPosition, wxSize(28, -1));
        fs->Add(btnNext, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        fs->Add(btnPrev, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
        // Case-sensitive toggle ("Aa").  Restore the last user preference.
        m_findCaseBox = new wxCheckBox(m_findBar, wxID_ANY, wxT("Aa"));
        m_findCaseBox->SetToolTip(wxT("Case-sensitive search"));
        {
            wxConfig cfg(wxT("mushclient"));
            bool savedCase = false;
            cfg.Read(wxT("/find/case_sensitive"), &savedCase, false);
            m_findCaseBox->SetValue(savedCase);
        }
        fs->Add(m_findCaseBox, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        // Match-count label ("3 of 47" or "Not found").
        m_findLabel = new wxStaticText(m_findBar, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxSize(90, -1));
        fs->Add(m_findLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        int idFindClose = wxID_HIGHEST + 205;
        wxButton* btnClose = new wxButton(m_findBar, idFindClose, wxT("✕"),
                                          wxDefaultPosition, wxSize(28, -1));
        fs->Add(btnClose, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
        m_findBar->SetSizer(fs);

        // Enter / Shift+Enter in the find box: find next / previous.
        m_findInput->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            DoFind(true);
        });
        m_findInput->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            // Live search: highlight as the user types; clear stale count.
            bool cs = m_findCaseBox && m_findCaseBox->IsChecked();
            m_output->HighlightSearch(m_findInput->GetValue(), cs);
            if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
        });
        // Case toggle: re-highlight immediately, reset stale count, and persist.
        m_findCaseBox->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            bool cs = m_findCaseBox->IsChecked();
            m_output->HighlightSearch(m_findInput ? m_findInput->GetValue() : wxString(), cs);
            if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
            wxConfig cfg(wxT("mushclient"));
            cfg.Write(wxT("/find/case_sensitive"), cs);
        });
        m_findInput->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_ESCAPE) {
                ShowFindBar(false);
            } else if (e.GetKeyCode() == WXK_RETURN && e.ShiftDown()) {
                DoFind(false);
            } else {
                e.Skip();
            }
        });
        btnNext->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoFind(true);  });
        btnPrev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoFind(false); });
        btnClose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowFindBar(false); });
    }
    sizer->Add(m_findBar, 0, wxEXPAND);
    m_findBar->Hide();

    // Return keyboard focus to the input bar after a mouse selection so the
    // user can keep typing without clicking back in the input bar.
    m_output->m_fn_return_focus = [this]() {
        wxTextCtrl* inp = ActiveInput();
        if (!inp) return;
        // Preserve the caret/selection across the focus change.  Focusing a text
        // field on macOS SELECTS ALL of its text (standard NSTextField
        // behaviour), so a bare SetFocus() here silently armed a select-all on
        // the command bar every time the user clicked in the output pane: the
        // next character typed replaced the whole half-typed command, and the
        // double-click "paste word" splice overwrote it instead of inserting.
        long from = 0, to = 0;
        inp->GetSelection(&from, &to);
        inp->SetFocus();
        inp->SetSelection(from, to);
    };

    // world.Pause(bPause) now drives IWorldView::setPaused (this frame) via
    // CMUSHclientDoc::EmitPaused; see the pal::IWorldView section below.
    if (m_doc) {
        // world.Repaint() now drives IWorldView::requestRepaint (this frame) via
        // CMUSHclientDoc::EmitRepaint; body in the pal::IWorldView section below.

        // Async DNS now runs inside the PAL transport (pal::ITransport, T2d):
        // its getaddrinfo thread posts the completion to a main-thread mailbox
        // drained by pal::PumpTransportCompletions() (wx_mfc_impl poll timer),
        // gate-guarded against doc/transport teardown mid-lookup.

        // world.ClearOutput() now drives IWorldView::clearOutput (this frame) via
        // CMUSHclientDoc::EmitClearOutput; the engine resets the emission counter.

        // world.SetStatus(msg) and world.SetTitle(s) now drive
        // IWorldView::setStatus / ::setTitle (this frame) via
        // CMUSHclientDoc::EmitStatus / ::EmitTitle; the bodies live in the
        // pal::IWorldView section below.

        // world.SetBackgroundColour(rgb) and SetOption("max_output_lines", n) now
        // drive IWorldView::setBackgroundColour / ::setMaxLines (this frame) via
        // CMUSHclientDoc::EmitBackgroundColour / ::EmitMaxLines; bodies below.

        // world.GetSelectionStartLine / …Column now drive IWorldView::outputSelection
        // (this frame) via CMUSHclientDoc::GetOutputSelection; body in the section below.

        // world.GetCommand() now drives IWorldView::currentCommand (this frame) via
        // CMUSHclientDoc::GetCommandText; body in the section below.

        // GetInfo(296) scroll offset now drives IWorldView::scrollPos (this frame)
        // via CMUSHclientDoc::GetScrollPos; body in the section below.

        // ── Output display seam ───────────────────────────────────────────
        // This frame IS the world's pal::IWorldView (see the header): the engine
        // drives finished output lines through it via EmitOutputLine → appendLine.
        // Set here so it is never accidentally overwritten after construction.
        m_doc->m_pWorldView = this;
        // ── Apply initial pane state from the doc ─────────────────────────
        // For quick-connect worlds the doc fields are at defaults (no face,
        // black background, 5000-line scrollback).  For world-file worlds the
        // doc has already been loaded via OnOpenDocument before the constructor
        // runs, so these reflect the saved world settings.
        if (m_output) {
            // Font: m_font_name is face name; m_font_height encodes size as either:
            //   negative → LOGFONT pixel height at 96 DPI (from world XML saved on
            //              Windows, e.g. -12 = 9pt); convert to points.
            //   positive → already a point size (scripting options default = 12;
            //              App.m_iDefaultOutputFontHeight default = 9).
            // Skip altogether if neither face nor height is configured.
            const char* face = static_cast<const char*>(m_doc->m_font_name);
            int         lfH  = (int)m_doc->m_font_height;
            if ((face && *face) || lfH != 0) {
                int points;
                if (lfH < 0)
                    points = (-lfH * 72 + 48) / 96;  // LOGFONT pixels → points
                else if (lfH > 0)
                    points = lfH;                      // already a point size
                else
                    points = COutputPane::kDefaultFontSize;
                points = std::max(COutputPane::kMinFontSize,
                                  std::min(COutputPane::kMaxFontSize, points));
                wxFontWeight weight = (m_doc->m_font_weight >= 700)
                                      ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL;
                // An empty or Windows-only face name (e.g. "FixedSys") still
                // produces a valid font via wxFONTFAMILY_TELETYPE fallback.
                wxString faceName = (face && *face)
                                    ? wxString::FromUTF8(face) : wxString();
                wxFont font(points, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                            weight, false, faceName);
                if (font.IsOk()) m_output->SetOutputFont(font);
            }
            // Scrollback limit.
            if (m_doc->m_maxlines > 0) m_output->SetMaxLines(m_doc->m_maxlines);
            // Auto-freeze: if the world has it disabled, the pane should keep
            // auto-scrolling even when the user has scrolled up.
            m_output->SetAllowAutoFreeze(m_doc->m_bAutoFreeze != 0);
            // Start-paused: if the world's "auto_pause" option is set, begin
            // with auto-scroll disengaged so output doesn't scroll on arrival.
            if (m_doc->m_bStartPaused)
                m_output->SetPaused(true);
            // Background colour (NO_COLOUR = 0xFFFFFFFF → black).
            ApplyPaneBackground(m_doc->m_iBackgroundColour);
            // Word delimiters for double-click selection (global pref).
            m_output->SetWordDelimitersDblClick(
                static_cast<const char*>(App.m_strWordDelimitersDblClick));
            // Auto-copy selection to clipboard (world pref).
            m_output->SetCopySelectionToClipboard(
                m_doc->m_bCopySelectionToClipboard != 0);
            // Pixel offset of the text from the side of the window (world pref).
            m_output->SetTextPixelOffset((int) m_doc->m_iPixelOffset);
        }
    }

    // Double-clicking a word in the output: "Double-click Sends Word" /
    // "…Pastes Word".  The decision is shared (doc_dblclick_shared.cpp); what is
    // left here is this frontend's own history and command bar, which is exactly
    // the split the hyperlink hoist uses.
    m_output->m_fn_word_double_clicked = [this](const wxString& word, bool bShift) {
        if (!m_doc) return;

        // DIVERGENCE, deliberate: Windows treats Shift+double-click as "the
        // whole PARAGRAPH" (mushview.cpp:4780/4812).  Reconstructing a paragraph
        // across this pane's wrapped visual rows is a different job, and sending
        // a single WORD where Windows sends a LINE is precisely the case a user
        // would notice, so decline rather than approximate.
        if (bShift) return;

        const CString cword(word.utf8_str());
        switch (m_doc->OutputWordDoubleClicked(cword)) {
        case eDblClickAbort:
            break;                       // not connected; already prompted

        case eDblClickSend:
            // The port's history gate is STRICTER than Windows' here, on
            // purpose.  Windows uses !(m_bNoEcho && !m_bAlwaysRecordCommandHistory)
            // (sendvw.cpp:2237), which it can afford because its history is
            // RAM-only; this port persists m_history to a PLAINTEXT file at
            // close, so honouring always_record_command_history would write a
            // password-prompt reply to disk.  Never record while echo is off --
            // the same rule the Return path uses.
            if (!m_doc->m_bNoEcho)
                addToCommandHistory(word.utf8_str());
            // sendvw.cpp:2253-2254: history browsing starts at the bottom again.
            m_histIdx = -1;
            m_histParked = eHistNotParked;
            ApplyHistRSearch(false);
            m_doc->SendDoubleClickedWord(cword);
            break;

        case eDblClickInsert: {
            // Gate on WHICH WIDGET the splice would land in, not on m_bNoEcho.
            // The two disagree for up to one 100 ms tick: m_bNoEcho flips in the
            // telnet parser, while the masked bar is shown/hidden later by the
            // world timer, so testing the flag can still splice output text into
            // a live password field.  ActiveInput() is the predicate that
            // actually chooses the target, so ask it.
            if (ActiveInput() != m_input) break;
            // Windows uses ReplaceSel: insert AT THE CARET, keeping what is
            // already typed ("kill " + double-click "orc" -> "kill orc").  The
            // PAL command-selection pair is the equivalent; m_fn_put_in_input is
            // NOT reusable here because it replaces the whole bar.
            long selStart = 0, selEnd = 0;
            m_doc->GetCommandSelection(selStart, selEnd);
            m_doc->ReplaceCommandSelectionRange(selStart, selEnd, word.utf8_str());
            break;
        }

        case eDblClickNothing:
            break;
        }
    };

    // "Put in Input Bar" context-menu action: paste selected output text into
    // the command input for review/editing before the user presses Enter.
    m_output->m_fn_put_in_input = [this](const wxString& text) {
        wxTextCtrl* inp = ActiveInput();
        if (inp) {
            inp->SetValue(text);
            inp->SetInsertionPointEnd();
            inp->SetFocus();
        }
    };

    // "Send to MUD" context-menu action: evaluate the selected text as a command
    // through the normal alias/trigger pipeline, exactly as if typed and Enter pressed.
    m_output->m_fn_send_to_mud = [this](const wxString& text) {
        if (!m_doc || text.IsEmpty()) return;
        CString cmd(text.ToUTF8().data());
        bool bOmit = false;
        m_doc->EvaluateCommand(cmd, /*countThem=*/true, bOmit);
        m_doc->UpdateAllViews(nullptr);
    };

    // ── Session logging callbacks ──────────────────────────────────────────
    // Each line appended to the output pane is forwarded to LogLine() so that
    // the log file always reflects exactly what the user sees on screen.
    m_output->m_fn_on_line_appended = [this](const SOutputLine& line) {
        LogLine(line);
    };

    // Tells the context menu whether a log is currently open.
    m_output->m_fn_is_logging = [this]() -> bool {
        return IsLogging();
    };

    // Toggle logging on/off: shows a save dialog when starting.
    m_output->m_fn_toggle_log = [this]() {
        if (IsLogging()) {
            StopLogging();
            if (m_output)
                m_output->AppendNote("** Logging stopped **", RGB(200, 200, 0));
        } else {
            // Default filename: WORLDNAME_YYYYMMDD_HHMMSS.log
            std::string worldName = m_doc
                ? static_cast<const char*>(m_doc->m_mush_name) : "world";
            time_t now = time(nullptr);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));

            // Sanitise worldName for use in filename (same rules as history).
            std::string safe;
            for (unsigned char c : worldName) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.') safe += (char)c;
                else if (!safe.empty() && safe.back() != '_') safe += '_';
            }
            while (!safe.empty() && safe.back() == '_') safe.pop_back();
            if (safe.empty()) safe = "world";

            std::string defName = safe + "_" + ts + ".log";
            const char* home = getenv("HOME");
            std::string defDir = home ? std::string(home) + "/Documents" : ".";

            wxFileDialog dlg(this,
                             wxT("Save Session Log"),
                             wxString::FromUTF8(defDir.c_str()),
                             wxString::FromUTF8(defName.c_str()),
                             wxT("Log files (*.log;*.txt)|*.log;*.txt|All files (*.*)|*.*"),
                             wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() != wxID_OK) return;

            std::string path = dlg.GetPath().ToUTF8().data();
            if (StartLogging(path)) {
                if (m_output)
                    m_output->AppendNote(
                        "** Logging to " + path + " **", RGB(200, 200, 0));
            } else {
                wxMessageBox(
                    wxString::FromUTF8(("Could not open log file:\n" + path).c_str()),
                    wxT("Logging Error"), wxOK | wxICON_ERROR, this);
            }
        }
    };

    // F3 / Shift+F3 from the output pane fires the find bar.
    m_output->m_fn_find_next_key = [this](bool fwd) {
        ShowFindBar(true);
        DoFind(fwd);
    };

    // Forward printable characters typed in the output pane to the input bar
    // so the user can type without clicking back into it after scrolling.
    m_output->m_fn_key_to_input = [this](wxChar ch) {
        wxTextCtrl* inp = ActiveInput();
        if (!inp) return;
        inp->AppendText(wxString(ch));
        inp->SetFocus();
        inp->SetInsertionPointEnd();
    };

    // Forward accelerator key events from the output pane to the world frame's
    // FireAcceleratorIfMatch so Lua Accelerator() bindings fire even when the
    // user has scrolled the output pane and focus is there rather than the input.
    m_output->m_fn_accelerator_check = [this](wxKeyEvent& e) {
        return FireAcceleratorIfMatch(e);
    };

    // When the auto-scroll state changes, update the status bar.
    // "↑ Scrolled back" reminds the user that new output is not visible yet.
    // Restoring auto-scroll shows the connection status again.
    m_output->m_fn_autoscroll_changed = [this](bool autoNow) {
        if (m_doc) m_doc->m_gui_paused = !autoNow;
        if (IsActiveWorld())
            UpdateStatusBar();
    };

    // When the font zoom changes, the character grid changes without a resize
    // event.  Use CallAfter so the new metrics are already committed before we
    // read CharWidth()/LineHeight() and send NAWS.
    // We also persist the new font size so it survives across restarts.
    m_output->m_fn_grid_changed = [this]() {
        // Persist font size immediately (synchronous — size is already committed).
        {
            wxConfig cfg(wxT("mushclient"));
            cfg.Write(wxT("/display/font_size"), (long)m_output->FontSize());
        }
        // Keep the input bars in sync with the output pane font so Ctrl+scroll
        // zoom also updates the input bar typeface.
        {
            const wxFont& outFont = m_output->GetCurrentFont();
            // InvalidateBestSize for the same reason as setOutputFont: wxOSX
            // does not drop the cached best size on SetFont, so without it the
            // bar keeps its old height and clips the zoomed text.
            if (m_input) {
                m_input->SetFont(outFont);
                m_input->InvalidateBestSize();
                m_input->Refresh();
            }
            if (m_inputPassword) {
                m_inputPassword->SetFont(outFont);
                m_inputPassword->InvalidateBestSize();
                m_inputPassword->Refresh();
            }
            // Re-measure the bar against the zoomed line height (see setInputFont).
            m_inputBarLines = 0;
            UpdateInputBarHeight();   // calls Layout()
        }
        CallAfter([this]() {
            if (!m_doc || !m_output) return;
            int charW = m_output->CharWidth();
            int lineH = m_output->LineHeight();
            if (charW <= 0 || lineH <= 0) return;
            int paneW, paneH;
            m_output->GetClientSize(&paneW, &paneH);
            if (paneW <= 0 || paneH <= 0) return;
            int cols = paneW / charW;
            int rows = paneH / lineH;
            m_doc->m_gui_cols       = cols;
            m_doc->m_gui_rows       = rows;
            m_doc->m_gui_font_height = lineH;
            m_doc->m_gui_font_width  = charW;
            // Also update the Windows-API font metric fields so GetInfo(212/213)
            // returns real values in the mushapp (mushgui) build.
            m_doc->m_FontHeight      = lineH;
            m_doc->m_FontWidth       = charW;
            m_doc->m_InputFontHeight = lineH;
            m_doc->m_InputFontWidth  = charW;
            // A font change alters how many columns fit, so the wrap column has
            // to be re-derived (when the world follows the window) and the MUD
            // re-told.  Shared with the resize path.
            ApplyWrapColumnAndNAWS();
            // Persist face name + LOGFONT height so font zoom survives save/reload.
            const wxFont& zf = m_output->GetCurrentFont();
            if (zf.IsOk()) {
                std::string faceStr = zf.GetFaceName().ToUTF8().data();
                m_doc->m_font_name   = CString(faceStr.c_str());
                m_doc->m_font_height = -(zf.GetPointSize() * 96 / 72);
                m_doc->m_font_weight = (zf.GetWeight() >= wxFONTWEIGHT_BOLD) ? 700 : 400;
            }
        });
    };

    // Keep the doc's GUI geometry (GetInfo 280/281) current on every pane resize
    // — including layout-driven shrinks that don't resize the frame (the input
    // bar claiming its space at startup), which the frame's OnSize misses.  This
    // is why a plugin's bottom margin (computed from GetInfo(280)) used to land
    // too low: GetInfo(280) kept the taller startup height.
    m_output->m_fn_resized = [this]() {
        if (!m_doc || !m_output) return;
        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        if (charW <= 0 || lineH <= 0) return;
        int paneW, paneH;
        m_output->GetClientSize(&paneW, &paneH);
        if (paneW <= 0 || paneH <= 0) return;
        m_doc->m_gui_cols        = paneW / charW;
        m_doc->m_gui_rows        = paneH / lineH;
        m_doc->m_gui_font_height = lineH;
        m_doc->m_gui_font_width  = charW;
        // The frame's OnSize fires OnPluginWorldOutputResized, but at startup the
        // pane shrink (the input bar claiming its space) settles the geometry
        // through HERE only.  Without re-notifying, Quow's plugins keep the
        // miniwindow positions they computed from the stale start-up geometry and
        // stay stacked at the top-left until the first manual resize.  Defer it
        // (CallAfter is auto-cancelled if the frame closes) so the plugins don't
        // re-enter wx in the middle of the resize.
        CallAfter([this]() {
            if (m_doc) m_doc->SendToAllPluginCallbacks(ON_PLUGIN_WORLD_OUTPUT_RESIZED);
        });
    };

    // ── Miniwindow compositing callback ───────────────────────────────────
    // Called from COutputPane::OnPaint() twice per frame: once before text
    // (bUnder=true, MINIWINDOW_DRAW_UNDERNEATH) and once after (bUnder=false).
    // The DC is already positioned at client origin (scroll removed by caller).
    // We implement our own rectangle calculation here rather than calling the
    // Windows-only CMUSHView::Calculate_MiniWindow_Rectangles().
    // Text rectangle (world.TextRectangle): expose the world's reserved text
    // area so the output pane shifts/narrows its text to avoid docked
    // miniwindows.  Mirrors CMUSHView::HaveTextRectangle() — active when the
    // rectangle is not all-zero.
    m_output->m_fn_text_rect = [this](int& l, int& t, int& r, int& b) -> bool {
        if (!m_doc) return false;
        const CRect& tr = m_doc->m_TextRectangle;
        if (tr.left == 0 && tr.top == 0 && tr.right == 0 && tr.bottom == 0)
            return false;
        l = tr.left; t = tr.top; r = tr.right; b = tr.bottom;
        return true;
    };

    // ── SetBackgroundImage / SetForegroundImage (Windows OnDraw parity) ────
    // The exact Blit_Bitmap !bUseRect placement: modes 0/2 stretch to the
    // pane/frame, 1/3 aspect-stretch, 4-12 positioned 1:1 (corners / wall
    // centres / centre), 13 tile. Formerly the images loaded on the port but
    // the view never painted them.
    auto draw_doc_image = [this](wxDC& dc, int clientW, int clientH,
                                 CBitmap& bmp, int mode)
    {
        // Draw straight from the wxBitmap the shim already holds — a wxMemoryDC
        // source, no shim CDC (whose CreateCompatibleDC/SelectObject depend on
        // the FULL_BUILD guard that isn't set for this TU).
        wxBitmap* wxbmp = WxBitmapFromCBitmap(&bmp);
        if (!wxbmp || !wxbmp->IsOk())
            return;   // no image loaded
        const int w = wxbmp->GetWidth(), h = wxbmp->GetHeight();
        if (w <= 0 || h <= 0)
            return;

        int fw = 0, fh = 0;
        GetClientSize(&fw, &fh);              // frame rect, for modes 2/3

        wxMemoryDC src(*wxbmp);
        switch (mode) {                       // == Blit_Bitmap's !bUseRect placement
        case 0:  dc.StretchBlit(0, 0, clientW, clientH, &src, 0, 0, w, h, wxCOPY); break;
        case 1:  dc.StretchBlit(0, 0, (int)(clientH * (double)w / h), clientH,
                                &src, 0, 0, w, h, wxCOPY); break;
        case 2:  dc.StretchBlit(0, 0, fw, fh, &src, 0, 0, w, h, wxCOPY); break;
        case 3:  dc.StretchBlit(0, 0, (int)(fh * (double)w / h), fh,
                                &src, 0, 0, w, h, wxCOPY); break;
        case 4:  dc.Blit(0, 0, w, h, &src, 0, 0, wxCOPY); break;
        case 5:  dc.Blit((clientW - w) / 2, 0, w, h, &src, 0, 0, wxCOPY); break;
        case 6:  dc.Blit(clientW - w, 0, w, h, &src, 0, 0, wxCOPY); break;
        case 7:  dc.Blit(clientW - w, (clientH - h) / 2, w, h, &src, 0, 0, wxCOPY); break;
        case 8:  dc.Blit(clientW - w, clientH - h, w, h, &src, 0, 0, wxCOPY); break;
        case 9:  dc.Blit((clientW - w) / 2, clientH - h, w, h, &src, 0, 0, wxCOPY); break;
        case 10: dc.Blit(0, clientH - h, w, h, &src, 0, 0, wxCOPY); break;
        case 11: dc.Blit(0, (clientH - h) / 2, w, h, &src, 0, 0, wxCOPY); break;
        case 12: dc.Blit((clientW - w) / 2, (clientH - h) / 2, w, h, &src, 0, 0, wxCOPY); break;
        case 13: {                            // tile
            const int across = clientW / w + 1, down = clientH / h + 1;
            for (int tx = 0; tx < across; ++tx)
                for (int ty = 0; ty < down; ++ty)
                    dc.Blit(tx * w, ty * h, w, h, &src, 0, 0, wxCOPY);
            break;
        }
        }
    };

    m_output->m_fn_draw_background_image =
        [this, draw_doc_image](wxDC& dc, int cw, int ch) {
        if (m_doc)
            draw_doc_image(dc, cw, ch, m_doc->m_BackgroundBitmap,
                           m_doc->m_iBackgroundMode);
    };
    m_output->m_fn_draw_foreground_image =
        [this, draw_doc_image](wxDC& dc, int cw, int ch) {
        if (m_doc)
            draw_doc_image(dc, cw, ch, m_doc->m_ForegroundBitmap,
                           m_doc->m_iForegroundMode);
    };

    // ── ON_PLUGIN_DRAW_OUTPUT_WINDOW (docs/PAL.md S4c) ─────────────────────
    // Windows passes a 1-based DOC line index plus the pixel scroll offset.
    // The pane speaks in its own line list, which tail-aligns with the doc's
    // (both append one entry per completed output line and trim to the same
    // maximum), so the doc line = doc count - pane count + pane index.
    m_output->m_fn_draw_output_window = [this](int firstPaneLine, int pixelOffset) {
        if (!m_doc) return;
        long docLine = (long)m_doc->GetLastLine()
                     - (long)m_output->LineCount() + firstPaneLine + 1;   // 1-based
        if (docLine < 1) docLine = 1;
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_DRAW_OUTPUT_WINDOW,
                                        docLine, pixelOffset, "", false, false);
    };

    m_output->m_fn_composite_miniwindows =
        [this](wxDC& destDC, int clientW, int clientH, bool bUnder)
    {
        if (!m_doc) return;
        int frameW = 0, frameH = 0;
        GetClientSize(&frameW, &frameH);
        wx_composite_miniwindows(m_doc, destDC, clientW, clientH,
                                 frameW, frameH, bUnder);
    };

    // ── Miniwindow mouse-event callbacks ──────────────────────────────────
    // Engine-owned routing (miniwindow_compose.cpp, docs/PAL.md S4): the old
    // CMUSHView Mouse_* family, shared verbatim with the Windows view. The
    // pane forwards raw client-space coordinates; WxMouseHooks (file scope,
    // just above — the IWorldView::mouseMoveMiniWindow adapter needs it too)
    // supplies the view services.

    m_output->m_fn_miniwindow_mouse_down = [this](int x, int y, long flags) -> bool {
        if (!m_doc) return false;
        WxMouseHooks hooks(m_output);
        return MouseDownMiniWindow(m_doc, CPoint(x, y), flags, hooks);
    };

    m_output->m_fn_miniwindow_mouse_up = [this](int x, int y, long flags) -> bool {
        if (!m_doc) return false;
        WxMouseHooks hooks(m_output);
        return MouseUpMiniWindow(m_doc, CPoint(x, y), flags, hooks);
    };

    m_output->m_fn_miniwindow_mouse_move = [this](int x, int y) -> int {
        if (!m_doc) return -999;
        WxMouseHooks hooks(m_output);
        MouseMoveMiniWindow(m_doc, CPoint(x, y), hooks);
        // Cursor: a fresh hit-test AFTER the move processing — the callbacks
        // it dispatched may have deleted the hotspot or the window (this is
        // what Windows' OnSetCursor does with its own MouseOverMiniWindow
        // pass). Returns the hotspot cursor (-1..12), 0 over a bare
        // miniwindow, -999 when not over one.
        std::string cid, cwid;
        CHotspot* ch = nullptr;
        CMiniWindow* cmw = MouseOverMiniWindow(m_doc, CPoint(x, y), cid, ch, cwid);
        return cmw ? (ch ? (int)ch->m_Cursor : 0) : -999;
    };

    // Scroll wheel: pack the flags exactly as CMUSHView::OnMouseWheel does —
    // direction bit + |delta| in the high-order word (the pane passes the raw
    // wxEVT_MOUSEWHEEL rotation, same +/-120-per-notch units as Win32 zDelta).
    m_output->m_fn_miniwindow_mouse_wheel = [this](int x, int y, int delta) -> bool {
        if (!m_doc) return false;
        WxMouseHooks hooks(m_output);
        long flags = (delta < 0 ? MW_MOUSE_SCROLL_BACK : 0)
                   | ((long)(delta < 0 ? -delta : delta) << 16);
        return MouseWheelMiniWindow(m_doc, CPoint(x, y), flags, hooks);
    };

    // ── Tooltip callback ───────────────────────────────────────────────────
    // Returns the tooltip text for the client-space point (x, y).
    // Priority: miniwindow (hotspot tooltip, else NOTHING) > MXP run hint.
    m_output->m_fn_get_tooltip =
        [this](int x, int y) -> std::string
    {
        if (!m_doc) return {};

        // 1. Over a miniwindow?  Then its answer is final — an opaque window
        // hides the text it covers, so Windows never shows that text's hint
        // (Mouse_Tooltip_MiniWindow returns TRUE with an empty buffer, and
        // OnToolTipNeedText returns before the MXP code).
        std::string mwTip;
        if (wx_miniwindow_tooltip(m_doc, x, y, mwTip))
            return mwTip;

        // 2. Check MXP hint text on the output line under the cursor.
        // Use PointToChar for wrap-aware hit-testing: it returns the absolute
        // character column within the logical line, which we then walk to find
        // which run the cursor is over (same approach as link detection).
        SelPoint pt = m_output->PointToChar(x, y);
        const SOutputLine* line = m_output->GetLine(pt.line);
        if (!line) return {};

        // RunCharLen, not run.text.size(): pt.col is a rendered CHARACTER
        // column, and run.text is UTF-8 BYTES.  Using the byte count shifted
        // every run after the first non-ASCII one, so the hint returned was the
        // wrong run's (or none).  The hyperlink walk in wx_outputpane.cpp
        // already measures with RunCharLen; this was the odd one out.
        int c = 0;
        for (const auto& run : *line) {
            int runLen = RunCharLen(run);
            if (pt.col >= c && pt.col < c + runLen)
                return run.hint;   // may be empty — that's fine
            c += runLen;
        }
        return {};
    };

    // ── MXP hyperlink / send-link / plain-URL callback ───────────────────────
    // action is either:
    //   • An http:// or https:// URL auto-detected from plain text
    //     → open in the system default browser via wxLaunchDefaultBrowser().
    //   • A pipe-delimited MXP command string from an <A href="..."> element
    //     → each alternative goes to CMUSHclientDoc::HyperlinkClicked
    //       (doc_hyperlink_shared.cpp), the same body Windows runs.
    //
    // This used to call EvaluateCommand(), which was wrong in four ways: it
    // alias-expands (Windows deliberately does not for a link), it hardcodes
    // m_display_my_input as the echo flag so the MXP page's "echo hyperlink"
    // option did nothing, it never touched the command history, and it had no
    // !!pluginID:sub(arg) decode — so a plugin's own callback links went to the
    // MUD as literal text.  actionType/bCommentLine are the two facts the
    // decision needs and they now travel with the action from the run.
    m_output->m_fn_link_clicked = [this](const std::string& action,
                                         int actionType, bool bCommentLine) {
        if (action.empty()) return;

        // Plain URL from auto-detection.
        if ((action.size() >= 7 && action.compare(0, 7, "http://")  == 0) ||
            (action.size() >= 8 && action.compare(0, 8, "https://") == 0))
        {
            wxLaunchDefaultBrowser(wxString::FromUTF8(action.c_str()));
            return;
        }

        if (!m_doc) return;

        // Split the pipe-delimited alternatives, skipping empty segments.
        std::vector<std::string> choices;
        {
            size_t start = 0;
            for (;;) {
                size_t sep = action.find('|', start);
                std::string part = action.substr(start,
                    (sep == std::string::npos) ? std::string::npos : sep - start);
                if (!part.empty()) choices.push_back(std::move(part));
                if (sep == std::string::npos) break;
                start = sep + 1;
            }
        }
        if (choices.empty()) return;

        // One place that runs a chosen alternative through the shared handler
        // and then does the parts that are this frontend's own business —
        // exactly what mushview.cpp does with the same return value.
        auto fire = [this, actionType, bCommentLine](const std::string& cmd) {
            switch (m_doc->HyperlinkClicked(CString(cmd.c_str()),
                                            actionType, bCommentLine))
            {
                case eHyperlinkAbort:
                    return;                    // not connected; leave the focus

                case eHyperlinkFillCommand:    // ACTION_PROMPT
                    // CSendView::SetCommand: replace the bar, caret at the end.
                    // Windows also runs CSendView::CheckTyping first (confirm
                    // before replacing something already typed) — that prompt is
                    // its own backlog item on the port and is not faked here.
                    if (m_input) {
                        m_input->SetValue(wxString::FromUTF8(cmd.c_str(), cmd.size()));
                        m_input->SetInsertionPointEnd();
                    }
                    break;

                case eHyperlinkAddHistory:
                    addToCommandHistory(cmd.c_str());
                    break;

                case eHyperlinkNothing:
                    break;
            }
            // put the focus back in the send window (OnKeysActivatecommandview)
            if (m_input) m_input->SetFocus();
            m_doc->UpdateAllViews(nullptr);
        };

        // Single action: run it immediately without a menu.
        if (choices.size() == 1) {
            fire(choices[0]);
            return;
        }

        // Multiple alternatives — present a popup menu so the user can choose.
        // We bind the handlers directly on the wxMenu object (wxMenu extends
        // wxEvtHandler); when the menu goes out of scope the bindings are freed.
        // Menu events are dispatched to the menu's own handler chain first, so
        // the Bind() calls below will be reached before the window's handlers.
        wxMenu linkMenu;
        for (const std::string& ch : choices) {
            wxMenuItem* item = linkMenu.Append(
                wxID_ANY, wxString::FromUTF8(ch.c_str(), ch.size()));
            linkMenu.Bind(wxEVT_MENU, [fire, cmd = ch](wxCommandEvent&) {
                fire(cmd);
            }, item->GetId());
        }
        // PopupMenu() runs its own event loop and returns after the user
        // picks an item or dismisses the menu.  The selected item's handler
        // fires synchronously inside PopupMenu() before it returns.
        m_output->PopupMenu(&linkMenu);
        // linkMenu destructs here, automatically cleaning up all Bind() handlers.
    };

    // The command bar is MULTI-LINE, matching Windows: CSendView::PreCreateWindow
    // (sendvw.cpp:310) creates the command window with ES_MULTILINE, so a paste
    // containing newlines simply sits in the box until the user presses Return.
    // A single-line control here had to do something with those newlines, and
    // what it did was send them — see the OnInputKeyDown Return comment.
    //
    // wxTE_DONTWRAP keeps the old single-line feel for long commands: they scroll
    // horizontally rather than wrapping onto a second visual row (which would
    // also make GetNumberOfLines() count display rows, breaking the height
    // calculation in UpdateInputBarHeight).
    m_input = new wxTextCtrl(this, ID_INPUT, wxEmptyString,
                             wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_PROCESS_ENTER);
    sizer->Add(m_input, 0, wxEXPAND);

    // Fire OnPluginCommandChanged per-keystroke — mirrors CSendView::OnChange()
    // in sendvw.cpp (line 1175).  Re-entrancy guard prevents recursion if a
    // plugin callback itself modifies the command bar.
    m_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        // Grow/shrink the bar to fit however many lines are now in it.  Runs
        // even without a document so a paste is visible before connecting.
        UpdateInputBarHeight();
        if (!m_doc) return;
        static thread_local bool s_firing = false;
        if (!s_firing) {
            s_firing = true;
            m_doc->SendToAllPluginCallbacks(ON_PLUGIN_COMMAND_CHANGED);
            s_firing = false;
        }
    });

    // Password-mode input bar: shown in place of m_input when the MUD sends
    // IAC WILL ECHO (telnet echo suppression) to signal a password prompt.
    // Uses wxTE_PASSWORD so characters are masked.  Hidden by default.
    m_inputPassword = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    sizer->Add(m_inputPassword, 0, wxEXPAND);
    m_inputPassword->Hide();

    // Apply the output pane's monospace font to both input bars so that the
    // input line uses the same typeface as the output — consistent look and
    // feel, and correct column-count alignment for users who type long lines.
    {
        const wxFont& outFont = m_output->GetCurrentFont();
        m_input->SetFont(outFont);
        m_inputPassword->SetFont(outFont);
    }

    // …but if the world configures its own input font (World Properties > Input
    // > Commands), that wins.  Previously the output font was applied here
    // unconditionally and nothing ever read m_input_font_*, so a configured
    // input font was visible only until the next world open, and a new world
    // seeded from App.m_strDefaultInputFont never showed it at all.  Same
    // point-size normalisation as the output font: negative = LOGFONT pixel
    // height at 96 DPI, positive = a point size.
    if (m_doc) {
        const char* face = static_cast<const char*>(m_doc->m_input_font_name);
        const int   lfH  = (int) m_doc->m_input_font_height;
        if ((face && *face) || lfH != 0) {
            int points;
            if (lfH < 0)       points = (-lfH * 72 + 48) / 96;
            else if (lfH > 0)  points = lfH;
            else               points = COutputPane::kDefaultFontSize;
            setInputFont(face ? face : "", points,
                         m_doc->m_input_font_weight >= 600,
                         m_doc->m_input_font_italic != 0);
        }
    }

    // Pin the bar to one line now.  A multi-line wxTextCtrl's default best size
    // is several lines tall, so without this the command bar starts out as a
    // large empty box and only snaps to one line at the first text change.
    UpdateInputBarHeight();
    // Wire the same send/key handlers directly to m_inputPassword.
    m_inputPassword->Bind(wxEVT_TEXT_ENTER,
        [this](wxCommandEvent& e) { OnInputEnter(e); });
    m_inputPassword->Bind(wxEVT_KEY_DOWN,
        &CWorldChildFrame::OnInputKeyDown, this);

    // ── Info bar (world.Info / InfoClear / InfoColour / InfoBackground /
    // InfoFont / ShowInfoBar) ─────────────────────────────────────────────────
    // A short read-only styled strip at the very bottom, hidden until a script
    // calls ShowInfoBar(true).  On Windows this is a CRichEditCtrl docked on the
    // main frame; per-world here.  world.Info appends text in the current style;
    // InfoColour/InfoFont update that style; InfoBackground recolours the strip.
    m_infobar = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxSize(-1, 24),
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 |
                               wxTE_NO_VSCROLL | wxBORDER_SIMPLE);
    m_infobar->SetFont(m_output->GetCurrentFont());
    sizer->Add(m_infobar, 0, wxEXPAND);
    m_infobar->Hide();
    m_infobarStyle = wxTextAttr(*wxBLACK);
    m_infobarStyle.SetFont(m_infobar->GetFont());

    // ── Per-world status bar (bottom of the child frame) ──────────────────
    // On macOS the wxMDIParentFrame's own status bar is covered by the
    // maximised child, so each world frame carries its own.  Five fields
    // mirroring the Windows status line; UpdateStatusBar() fills them.
    m_statusBar = new wxStatusBar(this, wxID_ANY,
                                  wxSTB_DEFAULT_STYLE & ~wxSTB_SIZEGRIP);
    m_statusBar->SetFieldsCount(5);
    {
        const int widths[5] = { -1, 160, 90, 130, 70 };  // -1 = stretch (message)
        m_statusBar->SetStatusWidths(5, widths);
    }
    sizer->Add(m_statusBar, 0, wxEXPAND);

    // Standard / World / Activity bars.  Built last so their Insert() calls put
    // them above everything already added, and before SetSizer so the frame
    // lays out once with them present.
    m_toolbars.reset(new CWorldToolBars(this, sizer));

    SetSizer(sizer);

    // Seed the App-level output settings through the same seam File >
    // Preferences uses, so opening a world and changing a preference share one
    // code path rather than drifting.
    ApplyGlobalPrefs();

    // View > Status Bar is app-wide (see s_statusBarVisible in wx_mfc_impl.cpp)
    // and persisted, so a world opened now must match it rather than always
    // showing its strip.  After SetSizer: ShowStatusBarStrip ends in Layout(),
    // which does nothing before the sizer is attached.
    if (!wx_status_bar_visible())
        ShowStatusBarStrip(false);

    // Same reasoning for View > Always On Top.  Windows pins the MDI PARENT and
    // the worlds ride along inside it; here each world is its own top-level
    // window, so one opened while the setting is on has to pin itself.
    if (App.m_bAlwaysOnTop)
        SetWindowStyleFlag(GetWindowStyleFlag() | wxSTAY_ON_TOP);

    // ── World window geometry ─────────────────────────────────────────────
    // We persist a single "last used" size (not per-world, since all worlds
    // share the same MDI client area on Windows).
    //
    // The first-run default is derived from the display: the full usable HEIGHT,
    // and kMaxDefaultWidthDIP wide unless the screen is narrower than that.  It
    // has to be computed here, on the child, because on wxOSX the MDI *parent*
    // is never shown — this frame IS the window the user sees.
    // hook_load_mdi_frame (wx_mfc_impl.cpp) applies the same rule to the parent,
    // which on macOS therefore sizes a window nobody looks at; the visible one
    // was left on a hard-coded 800×600.
    //
    // wxDisplay::GetClientArea() is the *usable* area, so it already excludes
    // the Dock and the menu bar when the menu bar is actually on screen.  With
    // the menu bar set to auto-hide, macOS reports the full display height and
    // the window correctly fills it — which is the behaviour we want in both
    // cases, without having to ask about the auto-hide setting ourselves.
    {
        wxConfig cfg(wxT("mushclient"));

        int di = wxDisplay::GetFromWindow(this);
        if (di < 0) di = 0;               // not mapped yet → primary display
        const wxRect scr = wxDisplay(di).GetClientArea();

        // FromDIP because the width is specified in *points*, not device pixels:
        // 1000 here means 2000 physical pixels on a 2x Retina display.  It is a
        // no-op on macOS and GTK (both already work in logical units) and does
        // the scaling on MSW — either way it documents which unit is meant.
        const long defW = std::max<long>(200, std::min<long>(
                              scr.width, FromDIP((int) kMaxDefaultWidthDIP)));
        const long defH = std::max<long>(100, scr.height);

        long w = defW, h = defH;
        // Two-arg Read leaves the value alone when the key is absent, and tells
        // us whether it was there — which is how we know this is a first run.
        const bool hadW = cfg.Read(wxT("/geometry/child_w"), &w);
        const bool hadH = cfg.Read(wxT("/geometry/child_h"), &h);

        // A restored size is clamped to the usable screen too: a window saved on
        // a larger display must not come back bigger than this one.
        if (w < 200 || w > scr.width)  w = defW;
        if (h < 100 || h > scr.height) h = defH;

        SetSize((int)w, (int)h);

#ifdef __WXOSX__
        // First run only, and only where the child is a real top-level window:
        // place it, because a full-height frame left at the OS's cascade offset
        // hangs off the bottom of the screen.  Horizontally centred, top of the
        // usable area (i.e. immediately below the menu bar).
        if (!hadW && !hadH)
            SetPosition(wxPoint(scr.x + (scr.width - (int) w) / 2, scr.y));
#else
        (void) hadW; (void) hadH;
#endif
    }

    m_input->SetFocus();

    // ── Load persistent command history ───────────────────────────────────
    // Each world keeps its own history file keyed by world name, so different
    // MUDs don't pollute each other's command history.  The path is fixed once
    // at construction time (m_historyFilePath) so a mid-session name change in
    // World Properties doesn't cause a load/save mismatch.
    {
        std::string worldName = m_doc
            ? static_cast<const char*>(m_doc->m_mush_name) : "";
        m_historyFilePath = GetHistoryFilePath(worldName);

        std::string histPath = m_historyFilePath;
        if (!histPath.empty()) {
            if (FILE* f = fopen(histPath.c_str(), "r")) {
                char buf[8192];
                while (fgets(buf, (int)sizeof(buf), f)) {
                    std::string line = buf;
                    // Strip trailing newline / carriage-return.
                    while (!line.empty() &&
                           (line.back() == '\n' || line.back() == '\r'))
                        line.pop_back();
                    if (!line.empty())
                        m_history.push_back(wxString::FromUTF8(line.c_str()));
                }
                fclose(f);
                // Trim to the maximum if the file was longer.
                while ((int)m_history.size() > HistoryLimit())
                    m_history.erase(m_history.begin());
            }
        }
    }

    // ── Input bar: key handler for history navigation ─────────────────────
    // Bind directly to m_input so we only see input-bar key events.
    m_input->Bind(wxEVT_KEY_DOWN, &CWorldChildFrame::OnInputKeyDown, this);

    // Paste is deliberately NOT intercepted.  wxTextCtrl's own paste inserts the
    // clipboard text — newlines and all — which is exactly what Windows does.
    // Sending a multi-line clipboard is a separate, explicit command:
    // Edit ▸ Paste To World (ID_EDIT_PASTETOMUSH, Shift+Ctrl+V), which honours
    // the world's Paste preferences (preamble/postamble, per-line wrappers,
    // delay, echo and Confirm Before Pasting).

    // ── Output pane: redirect printable keys to the input bar ─────────────
    // When the user clicks in the output pane and then types, we want the
    // text to end up in the input bar rather than being silently discarded.
    m_output->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& evt) {
        int code = evt.GetKeyCode();
        // Ctrl+F → show the find bar.
        if (code == 'F' && evt.RawControlDown()) {
            ShowFindBar(true);
            return;
        }
        // Let Ctrl+C, Ctrl+A, Ctrl+±/0 (zoom), and arrow keys be handled by
        // the output pane's own handler.
        bool paneOwns = evt.RawControlDown() ||
                        code == WXK_UP || code == WXK_DOWN ||
                        code == WXK_LEFT || code == WXK_RIGHT ||
                        code == WXK_PAGEUP || code == WXK_PAGEDOWN ||
                        code == WXK_HOME || code == WXK_END;
        if (paneOwns) { evt.Skip(); return; }
        // Anything else (printable characters, Escape, backspace…) →
        // redirect focus to the input bar.
        if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
        evt.Skip();   // let wxTextCtrl receive the event after getting focus
    });

    // ── Restore persistent font size ──────────────────────────────────────────
    // Apply the user's last font-size preference (written by Ctrl+scroll zoom),
    // but ONLY for worlds that do not carry a font of their own.
    //
    // This is a single *global* value.  Applying it unconditionally overrode the
    // per-world font that was just applied from the doc above, so a font chosen
    // in World Properties > Appearance > Output was silently replaced by whatever
    // size the user had last zoomed to in any other world — one of the reasons
    // the font setting appeared to do nothing.  An explicit per-world font now
    // wins; zooming such a world persists the new size into the world itself
    // (see the m_fn_grid_changed writeback above), so zoom still survives a
    // restart for those worlds via the world file rather than this global.
    // Fontless worlds (e.g. quick-connect) keep the old global behaviour.
    {
        const char* worldFace =
            m_doc ? static_cast<const char*>(m_doc->m_font_name) : NULL;
        const bool worldHasOwnFont =
            m_doc && ((worldFace && *worldFace) || (int)m_doc->m_font_height != 0);

        wxConfig cfg(wxT("mushclient"));
        long savedSize = 0;
        cfg.Read(wxT("/display/font_size"), &savedSize, 0L);
        if (!worldHasOwnFont &&
            savedSize >= COutputPane::kMinFontSize &&
            savedSize <= COutputPane::kMaxFontSize &&
            savedSize != m_output->FontSize())
        {
            m_output->AdjustFontSize((int)savedSize - m_output->FontSize());
        }
    }

    // ── Per-world timer ────────────────────────────────────────────────────
    // Fires every 100 ms to drive CheckTimers() (world/plugin timers) and
    // SendToAllPluginCallbacks(ON_PLUGIN_TICK) (every ~40 ms via accumulator).
    // Also flushes any output the timer callbacks may have produced.
    // Style the input bars now that they exist.  ApplyPaneBackground() above ran
    // before m_input/m_inputPassword were created, so without this the command
    // bar kept wx's default white-on-black-text look and the world's configured
    // input colours were not visible until something else happened to call
    // UpdateInputBarColors (e.g. leaving Ctrl+R search).
    UpdateInputBarColors();

    m_worldTimer.SetOwner(this, ID_WORLD_TIMER);
    m_worldTimer.Start(100);

    // Register this world document with the application's world doc-template
    // so that GetWorldList() / GetWorld() / GetWorldById() can enumerate all
    // open worlds.  Matched by RemoveDocument() in OnClose.
    if (m_doc && App.m_pWorldDocTemplate)
        App.m_pWorldDocTemplate->AddDocument(m_doc);
}

// ── UpdateTitle ───────────────────────────────────────────────────────────
void CWorldChildFrame::UpdateTitle()
{
    if (!m_doc) return;
    wxString title = wxString::FromUTF8(
        static_cast<const char*>(m_doc->m_mush_name));
    if (!title.empty())
        SetTitle(title);
}

// ── TriggerNAWS ───────────────────────────────────────────────────────────
// Re-sends a NAWS packet with the current character-grid dimensions.
// Called after a font change so the MUD sees the updated col/row count.
// Uses CallAfter to ensure the output pane has finished re-measuring its font
// metrics before we read CharWidth() and LineHeight().
void CWorldChildFrame::TriggerNAWS()
{
    if (!m_doc || !m_output) return;
    CallAfter([this]() { ApplyWrapColumnAndNAWS(); });
}

// ── ApplyWrapColumnAndNAWS ────────────────────────────────────────────────
// The one place the visible text width turns into an engine wrap column and a
// NAWS packet.  There were four near-copies of this, and all four differed from
// Windows in the same two ways; see the header for the contract.
bool CWorldChildFrame::ApplyWrapColumnAndNAWS(bool skipIfUnchanged)
{
    if (!m_doc || !m_output) return false;
    const int charW = m_output->CharWidth();
    const int lineH = m_output->LineHeight();
    if (charW <= 0 || lineH <= 0) return false;
    int paneW = 0, paneH = 0;
    m_output->GetClientSize(&paneW, &paneH);
    if (paneW <= 0 || paneH <= 0) return false;

    // Windows only lets the window width drive the wrap column when the world
    // asks for it: CMUSHView::OnSize calls AutoWrapWindowWidth, whose whole body
    // is gated on m_bAutoWrapWindowWidth (world option auto_wrap_window_width,
    // default off).  The port used to overwrite m_nWrapColumn unconditionally,
    // which made "wrap output at column N" a setting with no effect — and worse,
    // wrote the window-derived number back into the user's world file on the next
    // save, so their chosen value was gone for good.  AutoWrapColumnFor owns
    // that decision now (doc_shared.cpp) and returns 0 for "leave it alone".
    const int wanted =
        m_doc->AutoWrapColumnFor(EffectiveTextCols(m_doc, paneW, charW));

    if (wanted == 0) {
        if (skipIfUnchanged)
            return true;   // wrap column does not follow the window: nothing moved
    }
    else {
        if (skipIfUnchanged && wanted == (int) m_doc->m_nWrapColumn) return true;
        m_doc->m_nWrapColumn = (unsigned short) wanted;
    }

    // Report the WRAP COLUMN, not the raw pane width — CMUSHView::OnSize ends
    // with SendWindowSizes (pDoc->m_nWrapColumn), so with auto-wrap off the MUD
    // is told the column the client will actually wrap at.
    m_doc->SendNAWS(m_doc->m_nWrapColumn, paneH / lineH);
    return true;
}

// ── UpdateInputBarColors ──────────────────────────────────────────────────
// Derives input bar fg/bg from the output pane's current background colour
// and applies them to both the normal and password input bars.
// ITU-R BT.601 luma: white text on dark backgrounds, dark text on light.
// ── ApplyGlobalPrefs ──────────────────────────────────────────────────────
// Re-push the App-level settings the output pane caches at construction, so a
// change made in File > Preferences takes effect on already-open worlds instead
// of waiting for the next world open.
void CWorldChildFrame::ApplyGlobalPrefs()
{
    if (!m_output) return;
    m_output->SetWordDelimitersDblClick(
        static_cast<const char*>(App.m_strWordDelimitersDblClick));
    // File > Preferences > Output.  The only readers of these two were in
    // CMUSHView::OnDraw / ScrollToPosition -- a view the port never constructs
    // -- so both checkboxes were inert.
    m_output->SetBleedBackground(App.m_bBleedBackground != 0);
    m_output->SetSmootherScrolling(App.m_bSmootherScrolling != 0);
    m_output->SetSmoothScrolling(App.m_bSmoothScrolling != 0);

    // File > Preferences > "Activity button bar style" (Style 1..6).  Windows
    // reloads the strip here too (GlobalPrefs.cpp, right after the prefs sheet
    // closes) rather than waiting for a restart.
    if (m_toolbars)
        m_toolbars->ReloadActivityStyle();

    // Global Preferences ▸ "Window tabs".  Windows builds a CMDITabs strip from
    // CMainFrame::OnCreate -- which returns early on the port -- into the MDI
    // client area, which macOS wx does not have: each world frame here is a real
    // top-level NSWindow.  Native window tabbing is the equivalent, and giving
    // every world the same tabbing identifier is what lets them group.
    //
    // Top and Bottom BOTH map to "prefer tabs": AppKit only ever draws them
    // along the top.  Better that Bottom does the useful thing than nothing at
    // all, but it is not parity -- see wxOSXSetWindowTabbing.
    wxOSXSetWindowTabbing(this,
                          App.m_iWindowTabsStyle != WINDOW_TABS_NONE,
                          "au.com.gammon.MUSHclient.world");
}

// Command-history depth: the world's own "Lines in command history" setting.
// Read at trim time (not cached) so a prefs change takes effect immediately.
int CWorldChildFrame::HistoryLimit() const
{
    if (m_doc && m_doc->m_nHistoryLines > 0)
        return (int) m_doc->m_nHistoryLines;
    return kMaxHistoryFallback;
}

void CWorldChildFrame::UpdateInputBarColors()
{
    unsigned long col = m_output ? m_output->PaneBackground() : 0UL;
    int r = static_cast<int>(GetRValue(col));
    int g = static_cast<int>(GetGValue(col));
    int b = static_cast<int>(GetBValue(col));
    int luma = (r * 299 + g * 587 + b * 114) / 1000;

    // Default: derive a readable pair from the output-pane background.
    wxColour inputFg = (luma < 128) ? wxColour(220, 220, 220) : wxColour(20, 20, 20);
    wxColour inputBg = wxColour(r, g, b);

    // But an explicitly configured input colour (World Properties > Input >
    // Commands) wins.  Those two settings previously did nothing at all on the
    // port because the pair was always derived from the pane background.  The
    // option defaults are black-on-white (scriptingoptions_numeric.inc:86,91);
    // treat those as "not set" so untouched worlds keep the readable
    // auto-derived pair rather than snapping to literal black-on-white.
    if (m_doc) {
        const COLORREF fg = m_doc->m_input_text_colour;
        const COLORREF bg = m_doc->m_input_background_colour;
        const bool fgSet = (fg != RGB(0, 0, 0));
        const bool bgSet = (bg != RGB(255, 255, 255));
        if (fgSet || bgSet) {
            if (fgSet)
                inputFg = wxColour(GetRValue(fg), GetGValue(fg), GetBValue(fg));
            if (bgSet)
                inputBg = wxColour(GetRValue(bg), GetGValue(bg), GetBValue(bg));
        }
    }
    for (wxTextCtrl* inp : {m_input, m_inputPassword}) {
        if (!inp) continue;
        inp->SetForegroundColour(inputFg);
        inp->SetBackgroundColour(inputBg);
        inp->Refresh();
    }
}

// ── ApplyPaneBackground ───────────────────────────────────────────────────
// Applies col as the output pane background and styles the input bar to match.
// When col == 0xFFFFFFFF (NO_COLOUR) the default black background is kept.
// See the header.  Snapshots on the first call and reports "unchanged", so a
// freshly opened world never triggers a pointless rebuild of the lines it has
// only just emitted.
bool CWorldChildFrame::PaletteChanged()
{
    if (!m_doc) return false;

    bool changed = !m_paletteSnapshotValid;
    for (int i = 0; i < 8; ++i) {
        if (m_doc->m_normalcolour[i] != m_lastNormalColour[i]) changed = true;
        if (m_doc->m_boldcolour[i]   != m_lastBoldColour[i])   changed = true;
        m_lastNormalColour[i] = m_doc->m_normalcolour[i];
        m_lastBoldColour[i]   = m_doc->m_boldcolour[i];
    }
    // The pane background is derived from these too, so a change to any of them
    // has to repaint even when the eight ANSI pairs are untouched.
    if (m_doc->m_customback[15]          != m_lastCustomBack15)     changed = true;
    if (m_doc->m_bCustom16isDefaultColour != m_lastCustom16Default) changed = true;
    if (m_doc->m_iBackgroundColour        != m_lastBackgroundColour) changed = true;
    m_lastCustomBack15      = m_doc->m_customback[15];
    m_lastCustom16Default   = m_doc->m_bCustom16isDefaultColour;
    m_lastBackgroundColour  = m_doc->m_iBackgroundColour;

    const bool first = !m_paletteSnapshotValid;
    m_paletteSnapshotValid = true;
    return changed && !first;
}

void CWorldChildFrame::ApplyPaneBackground(unsigned long col)
{
    if (!m_output) return;

    // 0xFFFFFFFF is NO_COLOUR — world hasn't specified a custom background, so
    // it comes from the PALETTE.  Windows derives it on every paint
    // (mushview.cpp:909-915); this used to hard-code black, so changing ANSI
    // "Black", or ticking "use custom colour 16 as the default", left the pane
    // background untouched when on Windows it repaints the whole window.
    if (col == 0xFFFFFFFFUL) {
        col = 0;   // fallback if there is no doc yet (construction order)
        if (m_doc)
            col = m_doc->m_bCustom16isDefaultColour
                    ? m_doc->TranslateColour(m_doc->m_customback[15], 1.0)
                    : m_doc->TranslateColour(m_doc->m_normalcolour[BLACK], 1.0);
    }

    m_output->SetPaneBackground(col);
    UpdateInputBarColors();
}

// ── ApplyHistRSearch ──────────────────────────────────────────────────────
// Sets m_histRSearch and tints the input bar to signal Ctrl+R mode.
// Tints the background deep blue when entering; restores world colors on exit.
// The no-op guard (m_histRSearch == on) prevents redundant repaints on every
// key press in the common case where we're not in Ctrl+R mode.
void CWorldChildFrame::ApplyHistRSearch(bool on)
{
    if (m_histRSearch == on) return;
    m_histRSearch = on;
    if (!m_input) return;

    if (on) {
        // Deep-blue tint: clearly distinguishable from the normal input bar but
        // not so aggressive that it's distracting.  Text is pale blue so it's
        // legible against the tinted background regardless of the world theme.
        m_input->SetBackgroundColour(wxColour(20, 45, 95));
        m_input->SetForegroundColour(wxColour(160, 210, 255));
    } else {
        // Restore to match the output pane's background.
        UpdateInputBarColors();
    }
    m_input->Refresh();
}

// ── ShowFindBar ───────────────────────────────────────────────────────────
void CWorldChildFrame::ShowFindBar(bool show)
{
    if (!m_findBar) return;
    if (show) {
        m_findBar->Show();
        if (m_findInput) {
            m_findInput->SetFocus();
            m_findInput->SelectAll();
        }
    } else {
        m_findBar->Hide();
        if (m_output) m_output->HighlightSearch(wxEmptyString, false);
        if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
        if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
    }
    Layout();
}

// ── DoFind ─────────────────────────────────────────────────────────────────
void CWorldChildFrame::DoFind(bool forward)
{
    if (!m_findInput || !m_output) return;
    wxString term = m_findInput->GetValue();

    if (term.IsEmpty()) {
        if (m_findLabel) m_findLabel->SetLabel(wxEmptyString);
        return;
    }

    bool caseSensitive = m_findCaseBox && m_findCaseBox->IsChecked();
    int matchLine = -1, ordinal = 0, totalMatches = 0;
    bool found = m_output->FindNext(term, forward,
                                    &matchLine, &ordinal, &totalMatches,
                                    caseSensitive);

    if (found) {
        // Reset background in case a previous search had left it red.
        m_findInput->SetBackgroundColour(wxNullColour);
        m_findInput->Refresh();

        if (m_findLabel) {
            wxString countStr;
            if (totalMatches >= 9999)
                countStr = wxString::Format(wxT("%d of 9999+"), ordinal);
            else
                countStr = wxString::Format(wxT("%d of %d"), ordinal, totalMatches);
            m_findLabel->SetLabel(countStr);
        }
    } else {
        // Flash the find input red briefly to signal "not found".
        m_findInput->SetBackgroundColour(wxColour(255, 100, 100));
        m_findInput->Refresh();
        if (m_findLabel) m_findLabel->SetLabel(wxT("Not found"));
        CallAfter([this]() {
            if (m_findInput) {
                m_findInput->SetBackgroundColour(wxNullColour);
                m_findInput->Refresh();
            }
        });
    }
}

// ── ActiveInput ───────────────────────────────────────────────────────────
// Returns the currently visible input control: the password bar when telnet
// echo suppression is active, the normal bar otherwise.
wxTextCtrl* CWorldChildFrame::ActiveInput() const
{
    if (m_inputPassword && m_inputPassword->IsShown())
        return m_inputPassword;
    return m_input;
}

// ── Global Change (input find/replace) ────────────────────────────────────
// CSendView::OnInputGlobalchange, split the way that function splits: the
// dialog and the remembered from/to are frontend work and stay here; everything
// after DoModal is the canonical body in doc_commandbar_shared.cpp.
//
// The port used to reimplement the second half and got two things wrong: it
// ignored the SELECTION (Windows scopes the change to the selected text --
// sendvw.cpp:2077 -- while the port always rewrote the whole bar), and it wrote
// back with SetValue rather than a range replace, so the edit could not be
// undone.  It also silently rewrote the bar with identical text instead of
// reporting "No replacements made".
void CWorldChildFrame::GlobalChangeInput()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp || !m_doc)
        return;

    // The selection BEFORE the dialog runs, exactly as sendvw.cpp:2060 reads
    // it.  In BYTE offsets, which is the space GlobalChangeCommand indexes
    // GetCommandText() with (commandSelection() already reports bytes).
    const pal::Range sel = commandSelection();

    // Remember the last from/to across invocations (CSendView kept these as
    // view members; a static is the port equivalent — shared, which is fine).
    static CString s_lastFrom, s_lastTo;

    CGlobalChangeDlg dlg;
    dlg.m_strChangeFrom = s_lastFrom;
    dlg.m_strChangeTo   = s_lastTo;
    if (dlg.DoModal() != IDOK)
        return;
    s_lastFrom = dlg.m_strChangeFrom;
    s_lastTo   = dlg.m_strChangeTo;

    if (!m_doc->GlobalChangeCommand(dlg.m_strChangeFrom, dlg.m_strChangeTo,
                                    sel.start, sel.end))
        ::UMessageBox(TFormat("No replacements made for \"%s\".",
                              (LPCTSTR) s_lastFrom),
                      MB_ICONINFORMATION);
}

// ── Generate Character Name ────────────────────────────────────────────────
// Mirrors CSendView::OnEditGeneratecharactername (stubbed on the port).
void CWorldChildFrame::GenerateCharacterName()
{
    if (!m_doc)
        return;

    // Resolve the names file.  Its default is a bare "names.txt", which
    // GenerateName() would look for in the current working directory (not found
    // → silent failure).  If the path is empty or has no directory component,
    // resolve it among the bundled resources, where the build ships it.
    if (App.m_strDefaultNameGenerationFile.IsEmpty() ||
        App.m_strDefaultNameGenerationFile.Find('/') == -1)
    {
        CString dir = ExtractDirectory(App.m_strMUSHclientFileName);
#if defined (__APPLE__)
        // .app: resources live in Contents/Resources/, not Contents/MacOS/.
        dir += "../Resources/";
#endif
        App.m_strDefaultNameGenerationFile = dir + "names.txt";
    }

    CString strName = GenerateName();
    if (strName.IsEmpty())
        return;   // no usable names file

    // Canonical dialog (bridged): shows the name, Try Again / Copy / Send to World.
    CGeneratedNameDlg dlg;
    dlg.m_strName     = strName;
    dlg.m_pDoc        = m_doc;
    dlg.m_strFileName = App.m_strDefaultNameGenerationFile;
    dlg.DoModal();
}

// ── Recall Last Word ───────────────────────────────────────────────────────
// Edit ▸ Recall Last Word / Ctrl+Backspace (ID_REPEAT_LAST_WORD).  The whole
// body is canonical (doc_commandbar_shared.cpp, was CSendView::OnRepeatLastWord);
// this is just the menu entry point.
void CWorldChildFrame::RecallLastWord()
{
    if (!m_doc)
        return;
    m_doc->RepeatLastWord();
    m_histIdx = -1; ApplyHistRSearch(false);
    if (wxTextCtrl* inp = ActiveInput())
        inp->SetFocus();
}

// ── Spell-check the input ──────────────────────────────────────────────────
// Mirrors CSendView::OnEditSpellcheck (stubbed): runs the canonical spell-check
// Lua over the input text (it pops the suggestion dialog per misspelling) and
// writes the corrected text back.
//
// Returns false when the user CANCELLED the dialog.  Windows distinguishes the
// two (App.SpellCheck returns false, MUSHclient.cpp:1444) and its caller uses
// that to abort a send; the port could not, because SpellCheckString folded
// "cancelled" into "here is your text back unchanged".
bool CWorldChildFrame::SpellCheckInput()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp)
        return true;

    long from = 0, to = 0;
    inp->GetSelection(&from, &to);
    bool bAll = (from == to);   // no selection → check the whole line
    wxString wtext = bAll ? inp->GetValue() : inp->GetStringSelection();

    bool bCancelled = false;
    CString corrected = App.SpellCheckString(CString(wtext.ToUTF8().data()), bAll,
                                             &bCancelled);
    if (bCancelled)
        return false;   // leave the text exactly as it was, like ReplaceSel-not-called

    wxString wcorrected = wxString::FromUTF8(static_cast<const char*>(corrected));

    if (bAll)
        inp->SetValue(wcorrected);
    else
        inp->Replace(from, to, wcorrected);
    inp->SetInsertionPointEnd();
    return true;
}

// ── Print the world output buffer ──────────────────────────────────────────
void CWorldChildFrame::PrintWorld()
{
    if (!m_doc) return;

    // Walk the DOC, not the output pane.  Two reasons, both of which rule the
    // pane out entirely: SOutputRun's bold/italic/underline are already filtered
    // through the Appearance ▸ Output "Show bold/italic/underline" checkboxes
    // (doc_display_shared.cpp), which Windows printing deliberately ignores —
    // mushview.cpp:3956 tests HILITE raw — and SOutputRun carries no ANSI colour
    // index at all, which is exactly what the print styles are keyed on.
    std::vector<SPrintLine> lines;
    for (POSITION pos = m_doc->m_LineList.GetHeadPosition(); pos; )
    {
        CLine* pLine = m_doc->m_LineList.GetNext(pos);
        if (!pLine) continue;

        SPrintLine out;
        int charOff = 0;
        for (POSITION spos = pLine->styleList.GetHeadPosition(); spos; )
        {
            CStyle* pStyle = pLine->styleList.GetNext(spos);
            if (!pStyle || pStyle->iLength <= 0) continue;

            // World Properties ▸ Appearance ▸ Printing: eight "normal" and eight
            // "bold" style words, one per ANSI colour, each a FONT_* bitmask.
            // Mirrors mushview.cpp:3952-3967 — HILITE picks the bold table, and
            // anything that is not an ANSI-coloured style prints as WHITE's.
            const short style = pStyle->iFlags & STYLE_BITS;
            const int   idx   = pStyle->iForeColour;
            // ...but only when the index is really one of the eight.  Windows
            // subscripts m_nBoldPrintStyle[8] with iForeColour directly, and an
            // xterm-256 style carries 0..255 there, so on Windows this reads off
            // the end of the array (see docs/UPSTREAM-BUGS.md).  Fall back to
            // WHITE's entry, which is what the non-ANSI arm already does.
            const bool  ansi  = ((style & COLOURTYPE) == COLOUR_ANSI) &&
                                idx >= 0 && idx < 8;
            const int   slot  = ansi ? idx : WHITE;
            const int   f     = (style & HILITE) ? m_doc->m_nBoldPrintStyle[slot]
                                                 : m_doc->m_nNormalPrintStyle[slot];

            SPrintRun run;
            run.bold      = (f & FONT_BOLD)      != 0;
            run.italic    = (f & FONT_ITALIC)    != 0;
            run.underline = (f & FONT_UNDERLINE) != 0;
            run.text      = DecodeOutputBytes(pLine->text + charOff,
                                              (size_t) pStyle->iLength,
                                              m_doc->m_bUTF_8 != 0,
                                              (int) m_doc->m_font_charset);
            charOff += pStyle->iLength;
            out.runs.push_back(std::move(run));
        }
        // The page header carries the arrival time of the page's first line —
        // same format string as mushview.cpp, including the Windows-only "%#"
        // no-leading-zero flag, which the CTime shim maps to POSIX "%-".
        out.stamp = wxString::FromUTF8((const char*)
            pLine->m_theTime.Format(TranslateTime("%A, %B %d, %Y, %#I:%M %p")));
        lines.push_back(std::move(out));
    }

    // The printer face/size and margins are global preferences; wx_print.cpp
    // keeps itself free of the MFC-compat headers, so read them here.  The job
    // title is the world's name, as on Windows (mushview.cpp), not the frame's.
    PrintStyledDocument(this,
                        wxString::FromUTF8((const char*) m_doc->m_mush_name),
                        lines,
                        wxString::FromUTF8((const char*) App.m_strPrinterFont),
                        (int) App.m_nPrinterFontSize,
                        (int) App.m_nPrinterLeftMargin,
                        (int) App.m_nPrinterTopMargin,
                        (int) App.m_nPrinterLinesPerPage);
}

// ── View-level output/input commands ──────────────────────────────────────────
// Ports of CMUSHView/CSendView handlers (those views are not instantiated on the
// port).  The output selection comes from the wx pane; ShellExecute is the
// macOS `open` shim, the same path the canonical handlers use.

void CWorldChildFrame::GotoSelectedURL()
{
    if (!m_output) return;
    CString sel((const char*) m_output->GetSelectionText().utf8_str());
    sel.TrimLeft();  sel.TrimRight();
    if (sel.IsEmpty())
        ::TMessageBox("No URL selected", MB_ICONEXCLAMATION);
    else if (sel.GetLength() > 512)
        ::TMessageBox("URL too long", MB_ICONEXCLAMATION);
    else
    {
        CString upper = sel;  upper.MakeUpper();
        if (upper.Left(15).Find("://") == -1)
            sel = "http://" + sel;
        if ((long) ShellExecute((HWND)nullptr, "open", (LPCTSTR) sel, NULL, NULL, SW_SHOWNORMAL) <= 32)
            ::UMessageBox(TFormat("Unable to open the URL \"%s\"", (const char*) sel),
                          MB_ICONEXCLAMATION);
    }
    if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
}

void CWorldChildFrame::SendMailToSelection()
{
    if (!m_output) return;
    CString sel((const char*) m_output->GetSelectionText().utf8_str());
    sel.TrimLeft();  sel.TrimRight();
    if (sel.IsEmpty())
        ::TMessageBox("No email address selected", MB_ICONEXCLAMATION);
    else if (sel.GetLength() > 128)
        ::TMessageBox("Email address too long", MB_ICONEXCLAMATION);
    else if (sel.Find('@') == -1)
        ::UMessageBox(TFormat("Email address \"%s\" invalid - does not contain a \"@\"",
                              (const char*) sel), MB_ICONEXCLAMATION);
    else
    {
        CString upper = sel;  upper.MakeUpper();
        if (upper.Left(7) != "MAILTO:")
            sel = "mailto:" + sel;
        if ((long) ShellExecute((HWND)nullptr, "open", (LPCTSTR) sel, NULL, NULL, SW_SHOWNORMAL) <= 32)
            ::UMessageBox(TFormat("Unable to send mail to \"%s\"", (const char*) sel),
                          MB_ICONEXCLAMATION);
    }
    if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus();
}

void CWorldChildFrame::CopySelectionHTML()
{
    if (m_output)
        m_output->CopySelectionAsHTML(m_output->GetCurrentFont().GetFaceName());
}

// Input ▸ Repeat Last Command.  Windows routes it through the ORDINARY send
// path -- CSendView::OnRepeatLastCommand (sendvw.cpp:1808) ends in
// `SendCommand (strText, TRUE)` -- so it gets everything a typed line gets.
// The port called EvaluateCommand directly, which is the same bypass the Enter
// handler was fixed out of: it skipped the script prefix, command stacking and
// its ";;" escape, the MAX_EXECUTION_DEPTH guard, the per-sub-command
// ON_PLUGIN_COMMAND callback, auto-say, backslash translation, and the history
// re-add that moves the repeated command back to the bottom of the list.
void CWorldChildFrame::RepeatLastCommand()
{
    if (!m_doc || m_history.empty()) return;

    CString cmd((const char*) m_history.back().utf8_str());
    if (cmd.IsEmpty()) return;

    m_doc->m_bOmitFromCommandHistory = false;    // sendvw.cpp:625

    // sendvw.cpp:736 records the command before auto-say strips its prefix.
    const CString strForHistory = cmd;

    if (m_doc->ApplyAutoSay(cmd)) {
        if (!m_doc->m_bOmitFromCommandHistory)
            addToCommandHistory(strForHistory);
        m_doc->UpdateAllViews(nullptr);
        return;
    }

    if (m_doc->m_bTranslateBackslashSequences)
        cmd = ::FixupEscapeSequences(cmd);

    m_doc->m_iExecutionDepth = 0;                // hand-driven, depth zero
    if (m_doc->Execute(cmd) != 0 /*eOK*/) {
        m_doc->UpdateAllViews(nullptr);
        return;
    }

    if (!m_doc->m_bOmitFromCommandHistory)
        addToCommandHistory(strForHistory);

    m_doc->UpdateAllViews(nullptr);
}

void CWorldChildFrame::ShowCommandHistory()
{
    if (m_history.empty()) {
        wxMessageBox(wxT("No command history yet."), wxT("Command History"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxArrayString choices;   // most-recent first
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
        choices.Add(*it);
    wxSingleChoiceDialog dlg(this, wxT("Pick a command to place in the input bar:"),
                             wxT("Command History"), choices);
    if (dlg.ShowModal() == wxID_OK) {
        if (wxTextCtrl* inp = ActiveInput()) {
            inp->SetValue(dlg.GetStringSelection());
            inp->SetInsertionPointEnd();
            inp->SetFocus();
        }
    }
}

void CWorldChildFrame::HighlightSelectedWord()
{
    if (!m_doc || !m_output) return;

    CHighlightPhraseDlg dlg;
    dlg.m_iOtherForeground = m_doc->m_normalcolour[WHITE];
    dlg.m_iOtherBackground = m_doc->m_normalcolour[BLACK];

    // Pre-fill from the selection (first line, <=80 chars) + its style colour.
    long sLine, sCol, eLine, eCol;
    if (m_output->GetSelectionInfo(sLine, sCol, eLine, eCol) &&
        (eLine > sLine || (eLine == sLine && eCol > sCol)))
    {
        CString strSelection((const char*) m_output->GetSelectionText().utf8_str());
        int nl = strSelection.Find("\n");
        if (nl != -1) strSelection = strSelection.Left(nl);
        if (strSelection.GetLength() > 80) strSelection = strSelection.Left(80);
        dlg.m_strText = strSelection;

        CLine* pLine = m_doc->m_LineList.GetAt(m_doc->GetLinePosition(sLine));
        if (pLine) {
            CStyle* pStyle; int iCol; POSITION fp;
            if (m_doc->FindStyle(pLine, sCol, iCol, pStyle, fp))
                m_doc->GetStyleRGB(pStyle, dlg.m_iOtherForeground, dlg.m_iOtherBackground);
        }
    }

    memcpy(dlg.m_customtext, m_doc->m_customtext, sizeof(dlg.m_customtext));
    memcpy(dlg.m_customback, m_doc->m_customback, sizeof(dlg.m_customback));
    for (int i = 0; i < MAX_CUSTOM; i++)
        dlg.m_strCustomName[i] = m_doc->m_strCustomColourName[i];
    dlg.m_iColour = OTHER_CUSTOM + 1;
    dlg.m_bWord   = true;

    if (dlg.DoModal() != IDOK) { if (wxTextCtrl* inp = ActiveInput()) inp->SetFocus(); return; }

    CString strText;
    if (dlg.m_bWord) strText += "\\b";
    strText += ConvertToRegularExpression(dlg.m_strText, false, false);
    if (dlg.m_bWord) strText += "\\b";

    t_regexp* regexp = NULL;
    try {
        regexp = regcomp(strText, (dlg.m_bMatchCase ? 0 : PCRE_CASELESS) |
                                  (m_doc->m_bUTF_8 ? PCRE_UTF8 : 0));
    } catch (CException* e) {
        e->Delete();
        ::TMessageBox("Cannot compile regular expression");
        return;
    }

    CString strTriggerName;
    strTriggerName.Format("*trigger%s", (LPCTSTR) App.GetUniqueString());
    CTrigger* trigger_item;
    m_doc->GetTriggerMap().SetAt(strTriggerName, trigger_item = new CTrigger);
    trigger_item->nUpdateNumber    = App.GetUniqueNumber();
    trigger_item->strInternalName  = strTriggerName;
    trigger_item->trigger          = strText;
    trigger_item->colour           = dlg.m_iColour - 1;
    trigger_item->bKeepEvaluating  = true;
    trigger_item->ignore_case      = !dlg.m_bMatchCase;
    trigger_item->bRegexp          = true;
    trigger_item->bRepeat          = true;
    trigger_item->regexp           = regexp;
    trigger_item->iSendTo          = eSendToWorld;
    trigger_item->iSequence        = 90;
    trigger_item->strGroup         = "Highlighted Words";
    trigger_item->iOtherForeground = dlg.m_iOtherForeground;
    trigger_item->iOtherBackground = dlg.m_iOtherBackground;
    m_doc->SortTriggers();
    m_doc->SetModifiedFlag(TRUE);
}

void CWorldChildFrame::MakeMultiLineTrigger()
{
    if (!m_doc || !m_output) return;

    CMultiLineTriggerDlg dlg;
    CString strSelection((const char*) m_output->GetSelectionText().utf8_str());
    strSelection = strSelection.Left(10000);
    strSelection.Replace("\r", "");

    dlg.m_strTriggerText = ConvertToRegularExpression(strSelection, false, false);
    dlg.m_strTriggerText.Replace("\\n", "\r\n");   // show linebreaks
    dlg.m_bMatchCase = true;

    if (dlg.DoModal() != IDOK) return;

    CString strText = dlg.m_strTriggerText;
    int iCount = 1;
    for (const char* p = strText; *p; p++)
        if (*p == '\n') iCount++;

    strText.Replace(ENDLINE, "\\n");
    strText += "\\Z";

    t_regexp* regexp = NULL;
    try {
        regexp = regcomp(strText, (dlg.m_bMatchCase ? 0 : PCRE_CASELESS) |
                                  (m_doc->m_bUTF_8 ? PCRE_UTF8 : 0));
    } catch (CException* e) {
        e->Delete();
        ::TMessageBox("Cannot compile regular expression");
        return;
    }

    CString strTriggerName;
    strTriggerName.Format("*trigger%s", (LPCTSTR) App.GetUniqueString());
    CTrigger* trigger_item;
    m_doc->GetTriggerMap().SetAt(strTriggerName, trigger_item = new CTrigger);
    trigger_item->nUpdateNumber   = App.GetUniqueNumber();
    trigger_item->strInternalName = strTriggerName;
    trigger_item->trigger         = strText;
    trigger_item->colour          = SAMECOLOUR;
    trigger_item->ignore_case     = !dlg.m_bMatchCase;
    trigger_item->contents        = "%0";
    trigger_item->bRegexp         = true;
    trigger_item->bMultiLine      = true;
    trigger_item->bKeepEvaluating = true;
    trigger_item->regexp          = regexp;
    trigger_item->iSendTo         = eSendToOutput;
    trigger_item->strGroup        = "Multi Line";
    trigger_item->iLinesToMatch   = iCount;
    m_doc->SortTriggers();
    m_doc->SetModifiedFlag(TRUE);
}

// Edit ▸ Go To / Select To Matching Brace — operates on the command input bar.
//
// The scan is the canonical one (ScanMatchingBrace, utilities_shared.cpp), so
// all seven File ▸ Preferences ▸ Notepad brace options apply here as on
// Windows.  The reimplementation this replaced counted only same-delimiter
// nesting and had no quote or escape handling at all, so every one of those
// checkboxes was inert — including the three that are ON by default
// (nest braces, backslash escapes, percent escapes).
//
// What stays here is the only thing that genuinely differs: the canonical
// scanner works in BYTE offsets while wxTextCtrl indexes CHARACTERS, so the
// caret and the result are converted around the call.  (That is also why
// wrapping the wxTextCtrl in a CEdit and calling FindMatchingBrace would be
// wrong: mfc_ui.h's edit-selection hook passes byte offsets straight to
// wxTextCtrl::SetSelection, which would mis-select any non-ASCII line.)
void CWorldChildFrame::GotoMatchingBrace(bool select)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;

    const wxString      text = inp->GetValue();
    const wxScopedCharBuffer utf8 = text.utf8_str();
    const int len = (int) utf8.length();
    if (len == 0) return;

    // caret: characters → bytes
    int caret = (int) text.Mid(0, inp->GetInsertionPoint()).utf8_str().length();

    int startByte = caret, endByte = caret;
    if (!ScanMatchingBrace(utf8.data(), len, startByte, endByte,
                           select, App.m_nParenMatchFlags))
        return;                       // no match (Windows beeps here)

    // result: bytes → characters
    const long cs = (long) wxString::FromUTF8(utf8.data(), startByte).length();
    const long ce = (long) wxString::FromUTF8(utf8.data(), endByte).length();

    if (select)
        inp->SetSelection(cs, ce);
    else
        inp->SetInsertionPoint(ce);
    inp->ShowPosition(ce);
    inp->SetFocus();
}

// Game ▸ Functions List — the canonical scripting-function reference window
// (IDD_FUNCTION_LIST), seeded with the input-bar text + selection.
void CWorldChildFrame::ShowFunctionsList()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp || !m_doc) return;
    CString text((const char*) inp->GetValue().utf8_str());
    long from = 0, to = 0;
    inp->GetSelection(&from, &to);
    bool bLua = m_doc->GetScriptEngine() && m_doc->GetScriptEngine()->L;
    ::ShowFunctionslist(text, (int) from, (int) to, bLua);
}

// Display ▸ Text Attributes — show the style (colour/flags) of the first
// selected character via the canonical CTextAttributesDlg (IDD_TEXT_ATTRIBUTES).
void CWorldChildFrame::ShowTextAttributes()
{
    if (!m_doc || !m_output) return;

    long sLine, sCol, eLine, eCol;
    if (!m_output->GetSelectionInfo(sLine, sCol, eLine, eCol) ||
        !(eLine > sLine || (eLine == sLine && eCol > sCol)))
        return;   // need a non-empty selection

    CLine* pLine = m_doc->m_LineList.GetAt(m_doc->GetLinePosition(sLine));
    if (!pLine) return;
    long startcol = MIN(sCol, pLine->len);

    CStyle* pStyle; int iCol; POSITION fp;
    if (!m_doc->FindStyle(pLine, startcol + 1, iCol, pStyle, fp)) {
        ::TMessageBox("Cannot find style of this character");
        return;
    }

    static const char* sColours[8] =
        { "Black","Red","Green","Yellow","Blue","Magenta","Cyan","White" };
    unsigned int iStyle = pStyle->iFlags;

    CTextAttributesDlg dlg;
    dlg.m_pDoc   = m_doc;
    dlg.m_iLine  = sLine + 1;       // 1-relative
    dlg.m_pLine  = pLine;
    dlg.m_strLetter = (startcol < pLine->len) ? CString(pLine->text[startcol]) : CString(" ");

    if ((iStyle & COLOURTYPE) == COLOUR_CUSTOM) {
        dlg.m_strTextColour   = "Custom";
        dlg.m_strBackColour   = "Custom";
        dlg.m_strCustomColour = m_doc->m_strCustomColourName[pStyle->iForeColour];
    } else if ((iStyle & COLOURTYPE) == COLOUR_RGB) {
        dlg.m_strTextColour = CFormat("R=%i, G=%i, B=%i",
            GetRValue(pStyle->iForeColour), GetGValue(pStyle->iForeColour), GetBValue(pStyle->iForeColour));
        dlg.m_strBackColour = CFormat("R=%i, G=%i, B=%i",
            GetRValue(pStyle->iBackColour), GetGValue(pStyle->iBackColour), GetBValue(pStyle->iBackColour));
        dlg.m_strCustomColour = "RGB";
    } else {   // COLOUR_ANSI
        dlg.m_strTextColour = (pStyle->iForeColour >= 8)
            ? CFormat("R=%i, G=%i, B=%i", GetRValue(xterm_256_colours[pStyle->iForeColour]),
                      GetGValue(xterm_256_colours[pStyle->iForeColour]), GetBValue(xterm_256_colours[pStyle->iForeColour]))
            : CString(sColours[pStyle->iForeColour & 7]);
        dlg.m_strBackColour = (pStyle->iBackColour >= 8)
            ? CFormat("R=%i, G=%i, B=%i", GetRValue(xterm_256_colours[pStyle->iBackColour]),
                      GetGValue(xterm_256_colours[pStyle->iBackColour]), GetBValue(xterm_256_colours[pStyle->iBackColour]))
            : CString(sColours[pStyle->iBackColour & 7]);
        dlg.m_strCustomColour = "n/a";
    }
    dlg.DoModal();
}

// Display ▸ Bookmark Selection — CMUSHView::OnDisplayBookmarkselection
// (mushview.cpp): toggle the BOOKMARK flag of the selection's start line, or
// of the last line when there is no selection.  The toggle goes through
// CMUSHclientDoc::Bookmark so the engine's CLine flag is what changes: that is
// the store world.Bookmark writes and GetLineInfo(n, 7) / Text Attributes read,
// and it comes back here as setBookmark() to repaint the margin marker.
void CWorldChildFrame::BookmarkSelection()
{
    if (!m_output || !m_doc) return;

    const int idx = m_output->BookmarkTargetLine();     // 0-based
    if (idx < 0 || idx >= (int) m_doc->m_LineList.GetCount()) return;

    CLine* pLine = m_doc->m_LineList.GetAt(m_doc->GetLinePosition(idx));
    if (!pLine) return;

    // Bookmark() takes a 1-based line number.
    m_doc->Bookmark(idx + 1, (pLine->flags & BOOKMARK) == 0);
}

void CWorldChildFrame::GotoBookmark()
{
    if (m_output) m_output->GotoNextBookmark();
}

// ── Freeze Output: toggle the output pane's autoscroll pause ────────────────
void CWorldChildFrame::FreezeOutput()
{
    if (m_output)
        m_output->SetPaused(!m_output->IsPaused());
}

// ── Go To Line: prompt for a line number and scroll the output to it ────────
void CWorldChildFrame::GotoLine()
{
    if (!m_output)
        return;
    int total = m_output->LineCount();
    if (total <= 0)
        return;
    long line = wxGetNumberFromUser(
        wxString::Format(wxT("Go to which line? (1 - %d)"), total),
        wxT("Line:"), wxT("Go To Line"), total, 1, total, this);
    if (line >= 1)
        m_output->ScrollToLine((int) line - 1);
}

// ── ApplyNoEcho ───────────────────────────────────────────────────────────
// Switches the input bar between normal and password (masked) mode.
// Called from OnWorldTimer whenever m_doc->m_bNoEcho changes.
void CWorldChildFrame::ApplyNoEcho(bool noEcho)
{
    if (noEcho) {
        // Switch to the password bar.  Clear any partial command first so it
        // isn't accidentally sent as a password.
        if (m_input) m_input->Hide();
        if (m_inputPassword) {
            m_inputPassword->Clear();
            m_inputPassword->Show();
            m_inputPassword->SetFocus();
        }
    } else {
        // Switch back to the normal bar.  Wipe the password field for security.
        if (m_inputPassword) {
            m_inputPassword->Clear();
            m_inputPassword->Hide();
        }
        if (m_input) {
            m_input->Show();
            m_input->SetFocus();
        }
    }
    Layout();
    m_lastNoEcho = noEcho;
}

// ── IsActiveWorld ─────────────────────────────────────────────────────────
// Returns true when this frame is the currently active MDI child.
// Used to guard status-bar updates so background worlds don't overwrite the
// status text for the world the user is currently looking at.
bool CWorldChildFrame::IsActiveWorld() const
{
    wxMDIParentFrame* parent =
        dynamic_cast<wxMDIParentFrame*>(GetParent());
    return parent && (parent->GetActiveChild() == this);
}

// ── Session logging ───────────────────────────────────────────────────────
//
// Logging captures the plain-text content of each incoming output line to a
// UTF-8 text file.  Horizontal rules are written as a dash line.  Style
// attributes (colour, bold, underline) are not preserved — the log is plain
// text, suitable for reading or grepping later.

bool CWorldChildFrame::StartLogging(const std::string& path)
{
    if (m_logFile) return false;   // already logging

    m_logFile = fopen(path.c_str(), "a");
    if (!m_logFile) return false;

    m_logFilePath = path;

    // Write a timestamped header.
    time_t now = time(nullptr);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    std::string worldName = m_doc
        ? static_cast<const char*>(m_doc->m_mush_name) : "Unknown";
    fprintf(m_logFile,
            "===== Log started %s - World: %s =====\n",
            ts, worldName.c_str());
    fflush(m_logFile);
    return true;
}

void CWorldChildFrame::StopLogging()
{
    if (!m_logFile) return;

    time_t now = time(nullptr);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(m_logFile, "===== Log stopped %s =====\n", ts);
    fclose(m_logFile);
    m_logFile = nullptr;
    m_logFilePath.clear();
}

void CWorldChildFrame::LogLine(const SOutputLine& line)
{
    if (!m_logFile) return;

    // Horizontal rules become a dashed separator.
    if (line.size() == 1 && line[0].horiz_rule) {
        fputs("----------------------------------------\n", m_logFile);
        fflush(m_logFile);
        return;
    }

    // Concatenate run texts; skip empty and horiz_rule runs.
    std::string text;
    for (const auto& run : line) {
        if (!run.text.empty() && !run.horiz_rule)
            text += run.text;
    }
    text += '\n';
    fputs(text.c_str(), m_logFile);
    fflush(m_logFile);
}

// ── UpdateStatusBar ───────────────────────────────────────────────────────
// Writes the current connection state to the MDI parent's status bar.
// Only meaningful when this frame is the active MDI child; callers should
// check IsActiveWorld() before calling (or let it be a no-op when inactive).
void CWorldChildFrame::UpdateStatusBar()
{
    if (!m_doc || !m_statusBar) return;

    auto pane = [this](int field, const std::string& s) {
        m_statusBar->SetStatusText(wxString::FromUTF8(s), field);
    };

    int phase = m_doc->m_iConnectPhase;
    const bool connected = (phase == eConnectConnectedToMud);

    // ── Pane 1: world name (its own field now, no longer a message prefix) ──
    pane(1, static_cast<const char*>(m_doc->m_mush_name));

    // ── Pane 0 (stretch): main status / connection state ───────────────────
    // Script status takes priority, then scrollback, reconnect countdown, the
    // connected server, otherwise the in-progress connection phase.
    // The doc is constructed with m_strStatusMessage = "Ready" and nothing in
    // the tree ever empties it, so treating it as a live script message pinned
    // pane 0 to "Ready" forever and made every branch below dead code.  Treat
    // it as the idle sentinel it is.
    static const CString kIdleStatus = Translate("Ready");
    std::string msg;
    if (!m_doc->m_strStatusMessage.IsEmpty() &&
        m_doc->m_strStatusMessage != kIdleStatus) {
        msg = static_cast<const char*>(m_doc->m_strStatusMessage);
    } else if (m_output && !m_output->IsAutoScrolling()) {
        msg = "\xe2\x86\x91 Scrolled back \xe2\x80\x94 press Escape or scroll to bottom to resume";
    } else if (m_reconnectCountdown > 0) {
        msg = "Reconnecting in " + std::to_string((m_reconnectCountdown + 9) / 10) + "s\xe2\x80\xa6";
    } else if (connected) {
        msg = "Connected to ";
        msg += static_cast<const char*>(m_doc->m_server);
        msg += ":" + std::to_string(m_doc->m_port);
    } else {
        switch (phase) {
        case eConnectMudNameLookup:
        case eConnectProxyNameLookup:        msg = "Resolving\xe2\x80\xa6";     break;
        case eConnectConnectingToMud:
        case eConnectConnectingToProxy:      msg = "Connecting\xe2\x80\xa6";    break;
        case eConnectAwaitingProxyResponse1:
        case eConnectAwaitingProxyResponse2:
        case eConnectAwaitingProxyResponse3: msg = "Connecting via proxy\xe2\x80\xa6"; break;
        case eConnectAwaitingSSLHandshake:   msg = "TLS handshake\xe2\x80\xa6"; break;
        case eConnectDisconnecting:          msg = "Disconnecting\xe2\x80\xa6"; break;
        default:                             msg = "Not connected.";             break;
        }
    }
    pane(0, msg);

    // ── Pane 2: connection duration (H:MM:SS) while connected ──────────────
    if (connected) {
        time_t start = m_doc->m_tConnectTime.GetTime();
        time_t now   = time(nullptr);
        long secs = (start > 0 && now >= start) ? (long)(now - start) : 0;
        char buf[32];
        snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
        pane(2, buf);
    } else {
        pane(2, "");
    }

    // ── Pane 3: lines received ↓ / sent ↑ this connection ──────────────────
    {
        char buf[64];
        snprintf(buf, sizeof buf, "\xe2\x86\x93%ld \xe2\x86\x91%ld",
                 (long) m_doc->m_nTotalLinesReceived, (long) m_doc->m_nTotalLinesSent);
        pane(3, buf);
    }

    // ── Pane 4: log indicator ──────────────────────────────────────────────
    pane(4, IsLogging() ? "\xf0\x9f\x94\xb4 Log" : "");
}

bool CWorldChildFrame::SetTransientStatus(const wxString& text)
{
    if (!m_statusBar || !m_statusBar->IsShown()) return false;
    // Pane 0 only -- the world name, duration, counts and log indicator stay
    // put.  The next UpdateStatusBar() (world timer, activation, connection
    // change) redraws pane 0 from the doc, which is the same transience the
    // MDI parent bar had.
    m_statusBar->SetStatusText(text, 0);
    return true;
}

void CWorldChildFrame::ShowStatusBarStrip(bool show)
{
    if (m_statusBar && m_statusBar->IsShown() != show) {
        m_statusBar->Show(show);
        Layout();   // reclaim/yield the strip's height
    }
}

bool CWorldChildFrame::IsStatusBarStripShown() const
{
    return m_statusBar && m_statusBar->IsShown();
}

// ── OnActivate ────────────────────────────────────────────────────────────
// Called when this MDI child is activated (gets focus) or deactivated.
// On activation, refresh the status bar so switching worlds gives immediate
// visual feedback.  Also fires on_world_get_focus / on_world_lose_focus Lua
// callbacks and OnPluginGetFocus / OnPluginLoseFocus plugin callbacks,
// mirroring CMUSHView::OnSetFocus / OnKillFocus in mushview.cpp.
void CWorldChildFrame::OnActivate(wxActivateEvent& evt)
{
    if (evt.GetActive()) {
        UpdateStatusBar();
        if (m_doc) {
            // The engine's "is this world's window the active one?" state, and
            // with it the unread-line counter the activity toolbar and the
            // Activity window's New column report.  Windows does this in
            // CMUSHView/CSendView::OnActivateView; those views do not exist
            // here, so nothing ever marked a world read — every world stayed
            // "has new messages" forever, from its first line onwards.
            m_doc->SetWorldViewActive(true);
            m_doc->FireWorldGetFocus();
            if (!m_doc->m_bWorldClosing)
                m_doc->SendToAllPluginCallbacks(ON_PLUGIN_GETFOCUS);
        }
    } else {
        if (m_doc) {
            m_doc->SetWorldViewActive(false);
            m_doc->FireWorldLoseFocus();
            if (!m_doc->m_bWorldClosing)
                m_doc->SendToAllPluginCallbacks(ON_PLUGIN_LOSEFOCUS);
        }
    }
    evt.Skip();
}

// ── FireAcceleratorIfMatch / FireAcceleratorCommand ────────────────────────
// Non-Windows implementation of the Lua Accelerator()/AcceleratorTo() API.
//
// On Windows, MUSHclient registers a HACCEL table with the OS; the OS detects
// matching key presses and dispatches WM_COMMAND to CSendView::OnAcceleratorCommand.
// On non-Windows we replicate that dispatch manually: convert the wx key event to
// the Windows ACCEL encoding (FVIRTKEY | modifier flags, plus the VK code), look
// it up in m_AcceleratorToCommandMap, and call FireAcceleratorCommand if found.
//
// The wx→Windows VK mapping only covers keys that appear in the VirtualKeys[]
// table in accelerators.cpp (the set a user can actually name in a Lua script).

// Not static: also reused by the Key Name dialog's hotkey control (wx_mfc_impl.cpp).
WORD WxKeyToWinVK(int wxCode)
{
    // Upper-case alpha A-Z: wx delivers the upper-case code point, same as Windows VK
    if (wxCode >= 'A' && wxCode <= 'Z') return (WORD)wxCode;
    // Digit 0-9: same code point on both platforms
    if (wxCode >= '0' && wxCode <= '9') return (WORD)wxCode;

    switch (wxCode) {
        // Function keys
        case WXK_F1:  return VK_F1;
        case WXK_F2:  return VK_F1 + 1;
        case WXK_F3:  return VK_F1 + 2;
        case WXK_F4:  return VK_F1 + 3;
        case WXK_F5:  return VK_F1 + 4;
        case WXK_F6:  return VK_F1 + 5;
        case WXK_F7:  return VK_F1 + 6;
        case WXK_F8:  return VK_F1 + 7;
        case WXK_F9:  return VK_F1 + 8;
        case WXK_F10: return VK_F10;
        case WXK_F11: return VK_F11;
        case WXK_F12: return VK_F12;
#ifdef WXK_F13
        case WXK_F13: return VK_F13; case WXK_F14: return VK_F14;
        case WXK_F15: return VK_F15; case WXK_F16: return VK_F16;
        case WXK_F17: return VK_F17; case WXK_F18: return VK_F18;
        case WXK_F19: return VK_F19; case WXK_F20: return VK_F20;
        case WXK_F21: return VK_F21; case WXK_F22: return VK_F22;
        case WXK_F23: return VK_F23; case WXK_F24: return VK_F24;
#endif
        // Navigation
        case WXK_LEFT:     return VK_LEFT;
        case WXK_RIGHT:    return VK_RIGHT;
        case WXK_UP:       return VK_UP;
        case WXK_DOWN:     return VK_DOWN;
        case WXK_HOME:     return VK_HOME;
        case WXK_END:      return VK_END;
        case WXK_PAGEUP:   return VK_PRIOR;
        case WXK_PAGEDOWN: return VK_NEXT;
        case WXK_INSERT:   return VK_INSERT;
        case WXK_DELETE:   return VK_DELETE;
        // Common editing / whitespace
        case WXK_BACK:   return VK_BACK;
        case WXK_TAB:    return VK_TAB;
        case WXK_RETURN: return VK_RETURN;
        case WXK_ESCAPE: return VK_ESCAPE;
        case WXK_SPACE:  return VK_SPACE;
        // Numpad
        case WXK_NUMPAD0:    return VK_NUMPAD0;
        case WXK_NUMPAD1:    return VK_NUMPAD0 + 1;
        case WXK_NUMPAD2:    return VK_NUMPAD0 + 2;
        case WXK_NUMPAD3:    return VK_NUMPAD0 + 3;
        case WXK_NUMPAD4:    return VK_NUMPAD0 + 4;
        case WXK_NUMPAD5:    return VK_NUMPAD0 + 5;
        case WXK_NUMPAD6:    return VK_NUMPAD0 + 6;
        case WXK_NUMPAD7:    return VK_NUMPAD0 + 7;
        case WXK_NUMPAD8:    return VK_NUMPAD0 + 8;
        case WXK_NUMPAD9:    return VK_NUMPAD0 + 9;
        // The numpad operator keys come in TWO wx flavours and only the second
        // is what a real numeric keypad produces: WXK_MULTIPLY et al are the
        // symbolic "main keyboard" codes, WXK_NUMPAD_MULTIPLY et al the keypad
        // ones.  Mapping only the first set meant every Lua Accelerator() bound
        // to keypad * / + / - / . / Enter fell through to `return 0` and never
        // fired.  (The keypad *command* path below already gets this right,
        // which is what made the asymmetry easy to miss.)
        case WXK_MULTIPLY:
        case WXK_NUMPAD_MULTIPLY: return VK_MULTIPLY;
        case WXK_ADD:
        case WXK_NUMPAD_ADD:      return VK_ADD;
        case WXK_SUBTRACT:
        case WXK_NUMPAD_SUBTRACT: return VK_SUBTRACT;
        case WXK_DECIMAL:
        case WXK_NUMPAD_DECIMAL:  return VK_DECIMAL;
        case WXK_DIVIDE:
        case WXK_NUMPAD_DIVIDE:   return VK_DIVIDE;
        // Windows reports keypad Enter as VK_RETURN (it differs only in the
        // extended-key bit, which accelerator tables do not carry).
        case WXK_NUMPAD_ENTER:    return VK_RETURN;
        // The keypad navigation cluster likewise: Windows sends the same VKs as
        // the main cluster when NumLock is off.
        case WXK_NUMPAD_LEFT:     return VK_LEFT;
        case WXK_NUMPAD_RIGHT:    return VK_RIGHT;
        case WXK_NUMPAD_UP:       return VK_UP;
        case WXK_NUMPAD_DOWN:     return VK_DOWN;
        case WXK_NUMPAD_HOME:     return VK_HOME;
        case WXK_NUMPAD_END:      return VK_END;
        case WXK_NUMPAD_PAGEUP:   return VK_PRIOR;
        case WXK_NUMPAD_PAGEDOWN: return VK_NEXT;
        case WXK_NUMPAD_INSERT:   return VK_INSERT;
        case WXK_NUMPAD_DELETE:   return VK_DELETE;
        case WXK_NUMPAD_TAB:      return VK_TAB;
        default: return 0;
    }
}

bool CWorldChildFrame::FireAcceleratorIfMatch(wxKeyEvent& evt)
{
    if (!m_doc || m_doc->m_AcceleratorToCommandMap.empty())
        return false;

    WORD vk = WxKeyToWinVK(evt.GetKeyCode());
    if (vk == 0) return false;

    BYTE fVirt = FVIRTKEY | FNOINVERT;
    if (evt.ShiftDown())        fVirt |= FSHIFT;
    if (evt.RawControlDown())   fVirt |= FCONTROL;
    if (evt.AltDown())          fVirt |= FALT;

    long lookup = ((long)fVirt << 16) | vk;
    auto it = m_doc->m_AcceleratorToCommandMap.find(lookup);
    if (it == m_doc->m_AcceleratorToCommandMap.end())
        return false;

    FireAcceleratorCommand(it->second);
    return true;
}

void CWorldChildFrame::FireAcceleratorCommand(WORD nID)
{
    if (!m_doc) return;

    string sCommand = m_doc->m_CommandToMacroMap[nID];
    if (sCommand.empty()) return;

    BOOL bSavedAutoSay = m_doc->m_bEnableAutoSay;
    m_doc->m_bEnableAutoSay = FALSE;
    m_doc->m_iCurrentActionSource = eUserAccelerator;

    if (m_doc->m_CommandToSendToMap[nID] == eSendToExecute) {
        // "Send to: Execute" means run it through the full command pipeline, so
        // use the canonical Execute() rather than reaching past it straight to
        // EvaluateCommand.  This is what the comment here always claimed ("same
        // path as CSendView::SendCommand") but was not actually doing, so an
        // accelerator whose text used the script prefix or contained the command
        // stack character behaved differently from typing the same text.
        m_doc->m_iExecutionDepth = 0;
        m_doc->Execute(sCommand.c_str());
    } else {
        // Resolve the key name for the trace message (linear search — rare path).
        CString key = "unknown key";
        for (auto& kv : m_doc->m_AcceleratorToCommandMap) {
            if (kv.second == nID) {
                key = KeyCodeToString((BYTE)(kv.first >> 16), (WORD)kv.first);
                break;
            }
        }

        CString strExtraOutput;
        CPlugin* pSavedPlugin = m_doc->m_CurrentPlugin;
        m_doc->m_CurrentPlugin = m_doc->GetPlugin(m_doc->m_CommandToPluginMap[nID].c_str());

        if (m_doc->m_CurrentPlugin || m_doc->m_CommandToPluginMap[nID].empty()) {
            m_doc->SendTo(m_doc->m_CommandToSendToMap[nID],
                          sCommand.c_str(),
                          TRUE,   // omit from output
                          TRUE,   // omit from log
                          TFormat("Accelerator: %s", (LPCTSTR)key),
                          "",     // variable name (unused for most destinations)
                          strExtraOutput);
        }

        m_doc->m_CurrentPlugin = pSavedPlugin;

        if (!strExtraOutput.IsEmpty())
            m_doc->DisplayMsg(strExtraOutput, strExtraOutput.GetLength(), COMMENT);
    }

    m_doc->m_iCurrentActionSource = eUnknownActionSource;
    m_doc->m_bEnableAutoSay = bSavedAutoSay;
}

// ── AltArrowRecall ─────────────────────────────────────────────────────────
// The body of CSendView::OnAltUparrow / OnAltDownarrow (sendvw.cpp:1789-1945),
// which the port never had: Alt/Option+Arrow recalls the history entries that
// START WITH what you have typed.  Returns false when Windows would decline and
// run the ordinary walk instead, so the caller just falls through.
//
// Three things make this NOT the ordinary walk:
//   1. It REQUIRES a non-empty partial (sendvw.cpp:1822-1828, :1901-1907).
//      With an empty command bar the ordinary handler runs instead — and that
//      one DOES honour arrow_keys_wrap, so getting this wrong would silently
//      disable the park-then-wrap landed in 86e3f6f on every press.
//   2. arrow_keys_wrap is never consulted on this path.
//   3. Running off an end DISCARDS the partial so the next press does an
//      ordinary walk: past the oldest clears the bar, past the newest puts your
//      typed text back.
//
// Up starts at the newest and walks older; Down starts at the OLDEST and walks
// newer, and works from a cold start with no prior browsing — which is why Down
// does something here where the ordinary Down key does nothing.
//
// The PLAIN arrows are diverted in here too whenever arrow_recalls_partial is
// on and there is a partial (sendvw.cpp:393-402, :473-482); that is why the
// caller, not this function, decides which options admit which key.  Both
// builds share the behaviour — see docs/UPSTREAM-DIVERGENCES.md for the two
// places it now departs from upstream MUSHclient (exact matches are recallable,
// and Down restores your typing rather than blanking it).
bool CWorldChildFrame::AltArrowRecall(bool bUp)
{
    if (!m_doc || !m_input) return false;
    // The OPTION test lives in the caller, because it differs by key: Alt+Arrow
    // enters on either option, a plain arrow only on arrow_recalls_partial
    // (sendvw.cpp:393-402 / :473-482 vs :1822-1828 / :1901-1907).
    //
    // Windows bails before touching anything when there is no history at all
    // (sendvw.cpp:1797-1798), leaving the typed text alone.
    if (m_history.empty()) return false;

    // The partial is what the user typed.  Windows refreshes it only when the
    // user PERSONALLY typed since the last recall (m_bChanged, sendvw.cpp:1809
    // and :1888); the port's analogue is "this is a fresh browse", because any
    // non-navigation keystroke and every scripted setCommand reset m_histIdx.
    const bool bFresh = (m_histIdx < 0);
    const wxString partial = bFresh ? m_input->GetValue() : m_histSaved;
    if (partial.empty()) return false;      // → ordinary walk, as on Windows

    const wxString lpfx = partial.Lower();
    auto matches = [&lpfx](const wxString& entry) {
        return entry.Lower().StartsWith(lpfx);   // an exact match IS a match
    };

    int found = -1;
    if (bUp) {
        for (int i = bFresh ? (int) m_history.size() - 1 : m_histIdx - 1; i >= 0; --i)
            if (matches(m_history[i])) { found = i; break; }
    } else {
        for (int i = bFresh ? 0 : m_histIdx + 1; i < (int) m_history.size(); ++i)
            if (matches(m_history[i])) { found = i; break; }
    }

    if (bFresh)
        m_histSaved = partial;              // so a continuing walk keeps matching

    if (found < 0) {
        // Ran off the end: never wrap, and discard the partial so the next
        // press does an ordinary walk (point 4 above).  Going forward past the
        // newest match puts your typed text back; going back past the oldest
        // clears, as upstream does at both ends.
        m_input->SetValue(bUp ? wxString() : partial);
        m_input->SetInsertionPointEnd();
        m_histIdx    = -1;
        m_histParked = eHistNotParked;
        m_histSaved.Clear();
        return true;
    }

    m_histIdx    = found;
    m_histParked = eHistNotParked;
    m_input->SetValue(m_history[m_histIdx]);
    m_input->SetInsertionPointEnd();
    return true;
}

// ── FireMacro ──────────────────────────────────────────────────────────────
// Dispatch macro slot `idx`, the port's stand-in for CSendView::SendMacro
// (sendvw.cpp:743-793).  Returns false when the slot is unusable, which lets a
// key-driven caller fall through to typing the key normally.
//
// This lives here rather than inside OnInputKeyDown because it has TWO kinds of
// caller.  The key-bound slots (F-keys, Alt+letter, numpad) come from the key
// handler; the 17 NAMED slots -- up/down/north/…/logout/quit -- have no key
// binding on Windows either and are reached only by COMMAND ID, from
// world.DoCommand("North"), the Input menu and the world toolbar.  Every one of
// those ids is handled solely by CSendView's message map, and CSendView is
// never instantiated on the port, so before this they were silently unfirable:
// DoCommand still returned eOK while nothing happened.  The command-id route
// into here is in platform/wx_mfc_impl.cpp's frame menu handler.
//
// NOT reproduced from SendMacro, deliberately, both pending item 23:
//   * REPLACE_COMMAND does not call CheckTyping (the
//     confirm_before_replacing_typing prompt).  That option defaults TRUE and
//     the port has no m_bChanged to tell user-typed text from script-placed
//     text, so wiring it naively would pop a modal on nearly every macro.
//   * SEND_NOW does not add to the command history.  Windows gates that on
//     !m_bDoNotAddMacrosToCommandHistory; the port's history has different
//     password rules and persists to a plaintext file, so that belongs with the
//     rest of the history work, not here.
bool CWorldChildFrame::FireMacro(int idx)
{
    if (!m_doc) return false;
    if (idx < 0 || idx >= MACRO_COUNT) return false;
    // By value: Execute() can run a script that rewrites m_macros[idx].
    const CString mac = m_doc->m_macros[idx];
    if (mac.IsEmpty()) return false;

    wxTextCtrl* inp = ActiveInput();

    switch (m_doc->m_macro_type[idx]) {
    case REPLACE_COMMAND:
        if (inp) {
            inp->SetValue(wxString::FromUTF8(static_cast<const char*>(mac)));
            inp->SetInsertionPointEnd();
        }
        break;
    case SEND_NOW: {
        // Mirrors sendvw.cpp:775-777 (set eUserMacro, send, restore) plus
        // SendCommand's non-auto-say arm at :702-711.  Both halves were
        // missing: GetInfo(239) read 0 instead of eUserMacro, and going
        // straight to EvaluateCommand skipped command stacking, the
        // script prefix, the depth reset and the per-sub-command
        // ON_PLUGIN_COMMAND callback that Execute() owns.
        // (Windows also runs macros through auto-say; the port applies
        // auto-say only to typed lines, which is a separate gap.)
        CString cmd = mac;
        if (m_doc->m_bTranslateBackslashSequences)
            cmd = ::FixupEscapeSequences(cmd);
        m_doc->m_iCurrentActionSource = eUserMacro;
        m_doc->m_iExecutionDepth = 0;
        m_doc->Execute(cmd);
        m_doc->m_iCurrentActionSource = eUnknownActionSource;
        m_doc->UpdateAllViews(nullptr);
        break;
    }
    case ADD_TO_COMMAND:
        if (inp) inp->WriteText(wxString::FromUTF8(static_cast<const char*>(mac)));
        break;
    default:
        break;
    }
    return true;
}

// ── FireNamedMacroCommand ──────────────────────────────────────────────────
// The command-id entry point for the 17 named slots.  ID_ACTIONS_QUIT and
// ID_ACTIONS_LOGOUT confirm first and QUIT suppresses auto-reconnect, exactly
// as CSendView::OnActionsQuit / OnActionsLogout do (sendvw.cpp:999-1029).
// Returns false if the id is not one of the 17, so the caller can keep looking.
bool CWorldChildFrame::FireNamedMacroCommand(int id)
{
    if (!m_doc) return false;

    struct Row { int id; int mac; };
    static const Row kRows[] = {
        { 32126 /*ID_GAME_NORTH*/,       MAC_NORTH   },
        { 32128 /*ID_GAME_SOUTH*/,       MAC_SOUTH   },
        { 32130 /*ID_GAME_EAST*/,        MAC_EAST    },
        { 32132 /*ID_GAME_WEST*/,        MAC_WEST    },
        { 32134 /*ID_GAME_UP*/,          MAC_UP      },
        { 32136 /*ID_GAME_DOWN*/,        MAC_DOWN    },
        { 32138 /*ID_GAME_EXAMINE*/,     MAC_EXAMINE },
        { 32140 /*ID_GAME_LOOK*/,        MAC_LOOK    },
        { 32143 /*ID_GAME_SAY*/,         MAC_SAY     },
        { 32145 /*ID_GAME_WHISPER*/,     MAC_WHISPER },
        { 32147 /*ID_GAME_PAGESOMEONE*/, MAC_PAGE    },
        { 32154 /*ID_GAME_WHO*/,         MAC_WHO     },
        { 32156 /*ID_GAME_DOING*/,       MAC_DOING   },
        { 32160 /*ID_GAME_TAKE*/,        MAC_TAKE    },
        { 32162 /*ID_GAME_DROP*/,        MAC_DROP    },
    };

    for (const Row& r : kRows)
        if (r.id == id) { FireMacro(r.mac); return true; }

    if (id == 32218 /*ID_ACTIONS_LOGOUT*/) {
        wxString q = wxString::Format(
            wxT("Logout from this character on %s?"),
            wxString::FromUTF8((const char*) m_doc->m_mush_name));
        if (wxMessageBox(q, wxT("Logout"),
                         wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES)
            FireMacro(MAC_LOGOUT);
        return true;
    }

    if (id == 32217 /*ID_ACTIONS_QUIT*/) {
        wxString q = wxString::Format(
            wxT("Quit from %s?"),
            wxString::FromUTF8((const char*) m_doc->m_mush_name));
        if (wxMessageBox(q, wxT("Quit"),
                         wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES) {
            m_doc->m_bDisconnectOK = true;   // don't reconnect after a deliberate quit
            FireMacro(MAC_QUIT);
        }
        return true;
    }

    return false;
}

// ── OnInputKeyDown ─────────────────────────────────────────────────────────
// Up/Down arrows navigate command history; all other keys pass through normally.
void CWorldChildFrame::OnInputKeyDown(wxKeyEvent& evt)
{
    int code = evt.GetKeyCode();

    // Lua Accelerator() API — check before built-in key bindings so scripts
    // can override any key (including Escape, PgUp/Dn, etc.).
    if (FireAcceleratorIfMatch(evt))
        return;

    // Escape: snap the output pane back to the bottom (re-engage auto-scroll).
    // If escape_deletes_input is set, also clear the command bar.
    if (code == WXK_ESCAPE) {
        if (m_output) m_output->ScrollToBottom();
        if (m_doc && m_doc->m_bEscapeDeletesInput) {
            wxTextCtrl* inp = ActiveInput();
            if (inp) {
                // "Save deleted command in history" (m_bSaveDeletedCommand):
                // keep what Escape throws away so it can still be recalled.
                // Never for a masked password prompt.
                wxString discarded = inp->GetValue();
                if (m_doc->m_bSaveDeletedCommand && !discarded.empty() &&
                    !m_doc->m_bNoEcho) {
                    if (m_history.empty() || m_history.back() != discarded)
                        m_history.push_back(discarded);
                    if ((int) m_history.size() > HistoryLimit())
                        m_history.erase(m_history.begin());
                }
                inp->Clear();
            }
        }
        return;   // consume — don't close dialogs or deselect in the input bar
    }

    // Page Up / Page Down: scroll the output pane one page without leaving the
    // input bar.  This lets the user page through output while still typing.
    if (code == WXK_PAGEUP) {
        if (m_output) m_output->ScrollPage(-1);
        return;
    }
    if (code == WXK_PAGEDOWN) {
        if (m_output) m_output->ScrollPage(+1);
        return;
    }

    // Ctrl+Home / Ctrl+End: jump to the very top or bottom of the output buffer
    // without leaving the input bar.  Mirrors the Ctrl+Home/End shortcuts that
    // work when the output pane itself has focus (COutputPane::OnKeyDown).
    if (code == WXK_HOME && evt.RawControlDown()) {
        if (m_output) m_output->ScrollToTop();
        return;
    }
    if (code == WXK_END && evt.RawControlDown()) {
        if (m_output) m_output->ScrollToBottom();
        return;
    }

    wxTextCtrl* inp = ActiveInput();

    // Ctrl+U: kill the entire input line (standard Unix terminal shortcut).
    // Saves to the kill ring so Ctrl+Y can restore it.
    if (code == 'U' && evt.RawControlDown()) {
        if (inp) {
            m_killBuf = inp->GetValue();
            inp->Clear();
            m_histIdx = -1; ApplyHistRSearch(false);
        }
        return;
    }

    // Ctrl+Backspace: canonical ID_REPEAT_LAST_WORD (Edit ▸ Recall Last Word).
    //
    // This used to be hardwired to "delete the word before the caret" — which
    // is only HALF of what Windows binds to the key, and not the half you get
    // by default.  m_bCtrlBackspaceDeletesLastWord (world option
    // "ctrl_backspace_deletes_last_word") chooses: OFF (the shipped default,
    // doc_construct.cpp) recalls the last word of the previously-sent command
    // at the caret; ON deletes the word before the caret.  Its only reader was
    // CSendView::OnRepeatLastWord, which the port never instantiates, so both
    // arms were unreachable and the option did nothing whichever way it was
    // set.  The body now lives in doc_commandbar_shared.cpp.
    if (code == WXK_BACK && evt.RawControlDown()) {
        if (m_doc) {
            m_doc->RepeatLastWord();
            m_histIdx = -1; ApplyHistRSearch(false);
        }
        return;
    }

    // Ctrl+W: kill the word immediately before the cursor.  The Unix terminal
    // convention; Windows binds nothing to it, so it keeps the port's own
    // delete-word body (which, unlike Recall Last Word's delete arm, leaves the
    // preceding space alone and fills the kill ring for Ctrl+Y).
    if (code == 'W' && evt.RawControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            if (pos > 0) {
                wxString text = inp->GetValue();
                long newPos = pos;
                // Skip over any trailing whitespace.
                while (newPos > 0 && text[newPos - 1] == ' ')
                    --newPos;
                // Skip over the word itself (stop at any whitespace).
                while (newPos > 0 && text[newPos - 1] != ' ')
                    --newPos;
                m_killBuf = text.Mid(newPos, pos - newPos);
                inp->Remove(newPos, pos);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+D: delete character forward (emacs/Unix convention).
    if (code == 'D' && evt.RawControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            long len = (long)inp->GetValue().size();
            if (pos < len) {
                inp->Remove(pos, pos + 1);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+Delete: delete the word immediately after the cursor.
    // Skips leading spaces then deletes forward to the next space boundary.
    if (code == WXK_DELETE && evt.RawControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            wxString text = inp->GetValue();
            long len = (long)text.size();
            if (pos < len) {
                long newPos = pos;
                // Skip over leading whitespace.
                while (newPos < len && text[newPos] == ' ')
                    ++newPos;
                // Skip over the word.
                while (newPos < len && text[newPos] != ' ')
                    ++newPos;
                inp->Remove(pos, newPos);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+K: kill from the cursor to the end of the input line.
    // Saves the killed text to the kill ring so Ctrl+Y can restore it.
    if (code == 'K' && evt.RawControlDown()) {
        if (inp) {
            long pos = inp->GetInsertionPoint();
            wxString text = inp->GetValue();
            long len = (long)text.size();
            if (pos < len) {
                m_killBuf = text.Mid(pos);
                inp->Remove(pos, len);
                m_histIdx = -1; ApplyHistRSearch(false);
            }
        }
        return;
    }

    // Ctrl+Y: yank (paste) the most recently killed text at the cursor.
    // Complements Ctrl+K / Ctrl+W / Ctrl+U.  No-op if the kill ring is empty.
    if (code == 'Y' && evt.RawControlDown()) {
        if (inp && !m_killBuf.IsEmpty()) {
            long pos = inp->GetInsertionPoint();
            inp->SetInsertionPoint(pos);
            inp->WriteText(m_killBuf);
            m_histIdx = -1; ApplyHistRSearch(false);
        }
        return;
    }

    // Ctrl+A: move cursor to the beginning of the input line (Unix terminal).
    // Note: Ctrl+A is already bound to SelectAll on macOS for text controls,
    // so we only intercept it explicitly here; the result is the same on all
    // platforms — cursor moves to position 0.
    if (code == 'A' && evt.RawControlDown()) {
        if (inp) inp->SetInsertionPoint(0);
        return;
    }

    // Ctrl+E: move cursor to the end of the input line (Unix terminal).
    if (code == 'E' && evt.RawControlDown()) {
        if (inp) inp->SetInsertionPointEnd();
        return;
    }

    // Ctrl+Left / Ctrl+Right (Windows / Linux) or Alt+Left / Alt+Right (macOS):
    // move the insertion point one word backward or forward.
    // Word boundary: alphanumeric characters and underscores.  The cursor skips
    // any leading non-word characters and then jumps over the next word token.
    //
    // When Shift is also held, let the native wxTextCtrl handler run instead
    // (it already produces the correct word-selection behaviour on all platforms)
    // so we don't have to reimplement extend-selection logic.
    if ((code == WXK_LEFT || code == WXK_RIGHT) &&
        (evt.RawControlDown() || evt.AltDown()))
    {
        if (evt.ShiftDown()) { evt.Skip(); return; }   // native handles Shift+word-nav
        if (inp) {
            long     pos  = inp->GetInsertionPoint();
            wxString text = inp->GetValue();
            long     len  = (long)text.size();
            if (code == WXK_LEFT) {
                // Skip non-word chars backward, then skip the preceding word.
                while (pos > 0 && !wxIsalnum(text[pos-1]) && text[pos-1] != '_') --pos;
                while (pos > 0 &&  (wxIsalnum(text[pos-1]) || text[pos-1] == '_')) --pos;
            } else {
                // Skip non-word chars forward, then skip the next word.
                while (pos < len && !wxIsalnum(text[pos]) && text[pos] != '_') ++pos;
                while (pos < len &&  (wxIsalnum(text[pos]) || text[pos] == '_')) ++pos;
            }
            inp->SetInsertionPoint(pos);
        }
        return;
    }

    // Ctrl+P / Ctrl+N: emacs-style aliases for history Up/Down.
    // Re-map the key code so the existing Up/Down handlers below take over
    // without duplicating any logic.  These must be checked before the
    // Tab and Up/Down blocks so the re-mapped value is seen by them.
    // Both are user-settable (World Properties > Input > Commands); they were
    // previously hardwired on, so a user who wanted Ctrl+P/Ctrl+N free for
    // something else could not turn them off.
    if (evt.RawControlDown() && code == 'P' &&
        m_doc && m_doc->m_bCtrlPGoesToPreviousCommand)
        code = WXK_UP;
    else if (evt.RawControlDown() && code == 'N' &&
             m_doc && m_doc->m_bCtrlNGoesToNextCommand)
        code = WXK_DOWN;

    // Ctrl+Z: jump the output pane to the end of the buffer (opt-in setting).
    // Without this it fell through to the text control's native undo.
    if (evt.RawControlDown() && code == 'Z' &&
        m_doc && m_doc->m_bCtrlZGoesToEndOfBuffer && m_output) {
        m_output->ScrollToBottom();
        return;
    }

    // Shift+Tab: show a popup list of script-defined completion items and/or
    // Lua global function names (mirroring Windows FunctionMenu behaviour).
    // Scripts populate the list with world.ShiftTabCompleteItem(); the list
    // is filtered to items that start with whatever is currently in the input.
    if (code == WXK_TAB && inp == m_input && evt.ShiftDown() && m_doc) {
        std::vector<std::string> candidates;

        // ── Lua global functions ──────────────────────────────────────────
        if (m_doc->m_bTabCompleteFunctions) {
            CScriptEngine* eng = m_doc->GetScriptEngine();
            if (eng && eng->L) {
                lua_State* LS = eng->L;
                lua_pushvalue(LS, LUA_GLOBALSINDEX);
                lua_pushnil(LS);
                while (lua_next(LS, -2)) {
                    if (lua_type(LS, -1) == LUA_TFUNCTION &&
                        lua_type(LS, -2) == LUA_TSTRING) {
                        const char* fname = lua_tostring(LS, -2);
                        if (fname && fname[0] != '_')
                            candidates.push_back(fname);
                    }
                    lua_pop(LS, 1);
                }
                lua_pop(LS, 1);
            }
        }

        // ── Script-registered completion items ───────────────────────────
        for (const auto& item : m_doc->m_ExtraShiftTabCompleteItems)
            candidates.push_back(item);

        if (candidates.empty()) return;  // nothing to show — consume Shift+Tab

        // Sort and deduplicate (case-insensitive).
        std::sort(candidates.begin(), candidates.end(),
            [](const std::string& a, const std::string& b) {
                return strcasecmp(a.c_str(), b.c_str()) < 0;
            });
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());

        // Filter to items that start with the current (partial) input.
        wxString partial = inp->GetValue().Lower();
        if (!partial.IsEmpty()) {
            std::vector<std::string> filtered;
            for (const auto& c : candidates) {
                wxString lc = wxString::FromUTF8(c.c_str()).Lower();
                if (lc.StartsWith(partial))
                    filtered.push_back(c);
            }
            if (!filtered.empty())
                candidates = std::move(filtered);
        }

        // Build wxArrayString for the dialog.
        wxArrayString wxCands;
        wxCands.Alloc(candidates.size());
        for (const auto& c : candidates)
            wxCands.Add(wxString::FromUTF8(c.c_str()));

        wxSingleChoiceDialog dlg(this,
            "Choose a function or command:",
            "Shift+Tab Completion",
            wxCands);

        // Pre-select the first entry.
        dlg.SetSelection(0);

        if (dlg.ShowModal() == wxID_OK) {
            inp->SetValue(wxCands[dlg.GetSelection()]);
            inp->SetInsertionPointEnd();
        }
        return;
    }

    // Tab: complete the word under the cursor.
    //
    // The port used to reimplement this from scratch here, and got materially
    // different behaviour: it completed against the COMMAND HISTORY rather
    // than the output buffer, replaced the whole input line rather than the
    // word at the cursor, tokenised on a hardcoded [A-Za-z0-9_-] instead of
    // App.m_strWordDelimiters, cycled through candidates (Windows takes the
    // first match), and never consulted m_strTabCompletionDefaults,
    // m_bLowerCaseTabCompletion or m_bTabCompletionSpace -- which is exactly
    // why those three world options read as dead settings.  The canonical body
    // now lives in doc_tabcomplete_shared.cpp and drives the input bar through
    // pal::IWorldView (replaceCommandSelection), so both frontends share it.
    if (code == WXK_TAB && inp == m_input) {
        if (m_doc) m_doc->TabCompleteCommand();
        return;
    }

    // "Arrow keys traverse command history" (m_bArrowsChangeHistory).  When the
    // user turns this off they want Up/Down to move the caret instead, so fall
    // through to the text control rather than browsing history.
    if (code == WXK_UP && inp == m_input && m_doc && !m_doc->m_bArrowsChangeHistory) {
        evt.Skip();
        return;
    }
    if (code == WXK_DOWN && inp == m_input && m_doc && !m_doc->m_bArrowsChangeHistory) {
        evt.Skip();
        return;
    }

    // "Only traverse commands if we are at the *start* or *end* of the command"
    // — sendvw.cpp:1716/1742, the gate that guards BOTH arrow arms of
    // CSendView::PreTranslateMessage.  The port had no equivalent, so a plain
    // arrow browsed the history from any caret position: composing a two-line
    // command with Shift+Return (which this frame supports, and Windows does
    // too — the bar is ES_MULTILINE there and wxTE_MULTILINE here) and then
    // pressing Up to reach the first line destroyed the whole composition and
    // replaced it with a history entry.
    //
    // Restricted to a PLAIN arrow, matching where Windows applies it:
    //   * Ctrl+P / Ctrl+N (remapped to WXK_UP/WXK_DOWN just above) reach the
    //     history through OnFileCtrlP/OnFileCtrlN, and this block explicitly
    //     does not run when Ctrl is down (`GetKeyState (VK_CONTROL) >= 0`);
    //   * Alt+Arrow, when the gate declines, still reaches the history via the
    //     Alt+Up / Alt+Down accelerators on ID_KEYS_PREVCOMMAND /
    //     ID_KEYS_NEXTCOMMAND (mushclient_dialogs.rc:3005-3006), so gating it
    //     here would take away a working key rather than restore one.
    if ((code == WXK_UP || code == WXK_DOWN) && inp == m_input && m_doc &&
        !evt.AltDown() && !evt.RawControlDown() && !m_doc->CommandCaretAtEdge()) {
        evt.Skip();   // let the text control move the caret
        return;
    }

    // Two world options govern the two arrow blocks below, and BOTH were
    // hardcoded here — their only readers are in CSendView, which the port never
    // constructs (World Properties > Input > Commands).
    //
    // "Arrow key recalls partial command" (m_bArrowRecallsPartial, default OFF).
    // The port applied a case-insensitive prefix filter unconditionally, i.e. it
    // behaved as though the box were permanently ticked.  On Windows with it OFF
    // — the shipped default — plain Up/Down walk the WHOLE history whatever you
    // have typed; partial matching is what the ALT arrows are for.  With it ON,
    // Windows diverts plain Up/Down into the Alt handlers (sendvw.cpp:393/475)
    // — which is what the bPartialArrow divert just below now does, so the
    // ordinary blocks that follow it never filter.
    //
    // "Arrow keys wrap around" (m_bArrowKeysWrap, default OFF).  Windows, on
    // running past either end: clears the command bar and marks the history
    // cursor as parked at that end; the NEXT press then either wraps to the far
    // end (option on) or clears again (option off).  The port instead silently
    // stayed on the oldest entry, so neither state was reachable.  Note the
    // partial body deliberately does NOT wrap, exactly as upstream.
    const bool bWrap = m_doc && m_doc->m_bArrowKeysWrap;

    // Partial-command recall.  Windows has TWO history bodies and picks between
    // them here: the ordinary walk (the two blocks just below) and the
    // partial-matching walk (AltArrowRecall).  Which options admit which key
    // differs, so the choice is made here rather than inside the body:
    //   * Alt/Option+Arrow enters on EITHER option (sendvw.cpp:1683-1686 picks
    //     OnAltUparrow, whose gate at :1901-1907 is the OR).
    //   * a PLAIN arrow enters only on arrow_recalls_partial, and only when
    //     there is something to match (sendvw.cpp:393-402, :473-482).
    // If the partial body declines — no history, or nothing typed — Windows
    // runs the ordinary handler, which is exactly what falling through does.
    // `inp == m_input` keeps all of this off the masked password bar.
    const bool bPartialArrow = m_doc &&
        (evt.AltDown() ? (m_doc->m_bAltArrowRecallsPartial || m_doc->m_bArrowRecallsPartial)
                       : m_doc->m_bArrowRecallsPartial);
    if ((code == WXK_UP || code == WXK_DOWN) && inp == m_input && bPartialArrow) {
        if (AltArrowRecall(code == WXK_UP))
            return;
    }

    if (code == WXK_UP && inp == m_input) {
        // Pressing Up exits Ctrl+R mode.
        ApplyHistRSearch(false);  // ↑ always uses prefix (startsWith) match
        if (m_history.empty()) return;
        // THE ORDINARY WALK — no prefix filtering.  All partial matching now
        // happens in AltArrowRecall above, exactly as Windows splits the two
        // bodies; this block is only reached when there is nothing to match on,
        // and Windows' ordinary handler does not filter either.  (It used to
        // filter here as well, which left arrow_keys_wrap wrapping to the
        // absolute newest/oldest entry rather than a matching one.)
        if (m_histIdx < 0) {
            // Start browsing: save what the user had typed so ↓ can restore it.
            // Starting fresh also clears any parked end left by a previous
            // browse, so the park-then-wrap pair below always needs two presses.
            m_histParked = eHistNotParked;
            m_histSaved  = m_input->GetValue();
            m_histIdx    = (int) m_history.size() - 1;   // newest
        } else {
            const int found = m_histIdx - 1;             // next older
            if (found < 0) {
                // Ran off the oldest end.  Windows clears the bar and parks the
                // cursor there (sendvw.cpp:527-530); a further press wraps to the
                // newest entry if the option is on, or clears again if it is not.
                if (m_histParked == eHistParkedOld && bWrap) {
                    m_histIdx = (int) m_history.size() - 1;   // wrap to newest
                    m_histParked = eHistNotParked;
                } else {
                    m_histParked = eHistParkedOld;
                    m_input->SetValue(wxString());
                    m_input->SetInsertionPointEnd();
                    return;
                }
            } else {
                m_histIdx = found;
                m_histParked = eHistNotParked;
            }
        }
        m_input->SetValue(m_history[m_histIdx]);
        m_input->SetInsertionPointEnd();
        return;  // consume — don't scroll the output pane
    }

    if (code == WXK_DOWN && inp == m_input) {
        // Pressing Down exits Ctrl+R mode.
        ApplyHistRSearch(false);  // ↓ always uses prefix (startsWith) match
        if (m_histIdx < 0) { evt.Skip(); return; }
        // THE ORDINARY WALK — see the Up block; no prefix filtering here.
        const int found = (m_histIdx + 1 < (int) m_history.size())
                        ? m_histIdx + 1 : -1;            // next newer
        if (found < 0) {
            // Past the newest entry: put back what the user had been typing.
            // Upstream blanks the bar here (sendvw.cpp OnKeysNextcommand); the
            // restore is the same deliberate change now made to the partial
            // path on both builds — see docs/UPSTREAM-DIVERGENCES.md.  Wrapping
            // takes over on the press AFTER that, mirroring the Up side: park
            // first, then wrap (or restore again).
            if (m_histParked == eHistParkedNew && bWrap) {
                m_histIdx = 0;                        // wrap to oldest
                m_histParked = eHistNotParked;
                m_input->SetValue(m_history[m_histIdx]);
            } else {
                m_histParked = eHistParkedNew;
                m_histIdx = -1;
                m_input->SetValue(m_histSaved);
            }
        } else {
            m_histIdx = found;
            m_histParked = eHistNotParked;
            m_input->SetValue(m_history[m_histIdx]);
        }
        m_input->SetInsertionPointEnd();
        return;
    }

    // Ctrl+R: reverse incremental history search (substring match, newest first).
    // Mirrors Ctrl+R in bash/zsh: each press cycles to the next older entry
    // whose text contains the current input as a substring (case-insensitive).
    // Not available in password mode.
    if (code == 'R' && evt.RawControlDown() && inp == m_input) {
        ApplyHistRSearch(true);

        // Use the current input as the query on first press; on subsequent
        // presses keep m_histSaved (so editing history entries mid-cycle
        // doesn't confuse the search).
        wxString query = (m_histIdx < 0) ? m_input->GetValue() : m_histSaved;
        wxString lq    = query.Lower();

        int startFrom;
        if (m_histIdx < 0) {
            // Enter Ctrl+R mode: save the current text as the query.
            m_histSaved = m_input->GetValue();
            startFrom   = (int)m_history.size() - 1;
        } else {
            // Continue backward from one before the current position.
            startFrom   = m_histIdx - 1;
        }

        int found = -1;
        for (int i = startFrom; i >= 0; --i) {
            bool match = lq.IsEmpty()
                ? true
                : m_history[i].Lower().Contains(lq);
            if (match) { found = i; break; }
        }

        if (found >= 0) {
            m_histIdx = found;
            m_input->SetValue(m_history[m_histIdx]);
            m_input->SetInsertionPointEnd();
        } else if (m_histIdx < 0) {
            // No match at all on first press — stay put (don't change input).
        }
        // If already cycling and there are no more older matches, stay put.
        return;
    }

    // Ctrl+F → open the find bar.
    if (code == 'F' && evt.RawControlDown()) {
        ShowFindBar(true);
        return;   // consume — don't forward to the doc
    }

    // F3 / Shift+F3 → find next / previous (opens the bar if not already shown).
    if (code == WXK_F3) {
        ShowFindBar(true);
        DoFind(!evt.ShiftDown());
        return;
    }

    // Ctrl+L → clear the output pane (same as the menu item and Ctrl+L in the
    // output pane itself; terminal convention: clear screen without disconnect).
    if (code == 'L' && evt.RawControlDown()) {
        if (m_output) m_output->ClearOutput();
        return;
    }

    // ── F-key and numpad macros ──────────────────────────────────────────────
    // Map function keys (F1-F12, Shift+F1-F12, Ctrl+F1-F12) to m_macros[] and
    // numpad keys to m_keypad[] when m_keypad_enable is set.  User-configured
    // macros take priority over built-in bindings (e.g. F3 for Find).
    // All macros are processed on the input bar, not the output pane, because
    // we want them available while typing.
    if (m_doc) {
        // Helper: dispatch a macro by index.
        // REPLACE_COMMAND → set input bar; SEND_NOW → send via EvaluateCommand;
        // ADD_TO_COMMAND → insert at cursor.
        auto fireMacro = [this](int idx) -> bool { return FireMacro(idx); };

        // Helper: send a keypad key by index.
        auto fireKeypad = [this](int idx) -> bool {
            if (!m_doc->m_keypad_enable) return false;
            if (idx < 0 || idx >= eKeypad_Max_Items) return false;
            // By value, not by reference: Execute() can run a script that
            // rewrites m_keypad[idx] out from under us.
            CString kp = m_doc->m_keypad[idx];
            // DIVERGENCE FROM WINDOWS, deliberate — keep this guard.  Windows
            // sends even a blank keypad slot (sendvw.cpp:1093) because its
            // else-arm types the digit instead (:1099).  Here the early-out IS
            // that else-arm: it lets an unbound numpad key fall through to
            // evt.Skip() and type its digit.  Removing it would send a blank
            // line to the MUD for every unconfigured numpad key.
            if (kp.IsEmpty()) return false;
            // Windows turns auto-say off for keypad commands — you don't want
            // to *say* "west" (sendvw.cpp:1039-1041).  Inert on the port today
            // (auto-say is applied only in OnInputEnter), included so this path
            // stays correct if that changes.
            BOOL bSavedAutoSay = m_doc->m_bEnableAutoSay;
            m_doc->m_bEnableAutoSay = FALSE;
            if (m_doc->m_bTranslateBackslashSequences)   // sendvw.cpp:707-709
                kp = ::FixupEscapeSequences(kp);
            m_doc->m_iCurrentActionSource = eUserKeypad;  // GetInfo(239) == 3
            m_doc->m_iExecutionDepth = 0;                 // sendvw.cpp:702
            m_doc->Execute(kp);                           // not EvaluateCommand
            m_doc->m_iCurrentActionSource = eUnknownActionSource;
            m_doc->m_bEnableAutoSay = bSavedAutoSay;
            m_doc->UpdateAllViews(nullptr);
            return true;
        };

        bool ctrl  = evt.RawControlDown();
        bool shift = evt.ShiftDown();
        bool alt   = evt.AltDown();

        // "Use F1 as a macro key" (File > Preferences > Macros).  Windows
        // gates ONLY F1 -- plain, Shift and Ctrl -- on this flag
        // (sendvw.cpp:1721, mushview.cpp:4252); the other function keys are
        // always macros.  The port had no gate at all, so the checkbox did
        // nothing and MAC_F1 fired even at its shipped default of OFF.
        // Read it at key-press time, never cache it: configuration.cpp
        // clears the flag around the World Properties sheet (so F1 works as
        // Help inside the dialog) and restores it afterwards.
        const bool bF1Macro = App.m_bF1macro != 0;
        int macIdx  = -1;
        int kpIdx   = -1;

        if (alt && !ctrl && !shift) {
            // Alt+letter macros (MAC_ALT_A..MAC_ALT_Z, 17 slots).  This whole
            // block used to be skipped outright when Alt was held, so none of
            // them ever fired on the port.  wx delivers Option+K as KEY_DOWN
            // code 'K' with AltDown() set before any macOS dead-key
            // composition — the accelerator path (FireAcceleratorIfMatch) has
            // relied on exactly that for FALT since it was written.
            //
            // Only slots the user has actually filled in are consumed: on macOS
            // Option+letter is how you type ø, π, ∂ and friends, so fireMacro
            // returns false for an empty macro and we fall through to
            // evt.Skip() below, leaving normal text entry alone.
            switch (code) {
            case 'A': macIdx = MAC_ALT_A; break;
            case 'B': macIdx = MAC_ALT_B; break;
            case 'J': macIdx = MAC_ALT_J; break;
            case 'K': macIdx = MAC_ALT_K; break;
            case 'L': macIdx = MAC_ALT_L; break;
            case 'M': macIdx = MAC_ALT_M; break;
            case 'N': macIdx = MAC_ALT_N; break;
            case 'O': macIdx = MAC_ALT_O; break;
            case 'P': macIdx = MAC_ALT_P; break;
            case 'Q': macIdx = MAC_ALT_Q; break;
            case 'R': macIdx = MAC_ALT_R; break;
            case 'S': macIdx = MAC_ALT_S; break;
            case 'T': macIdx = MAC_ALT_T; break;
            case 'U': macIdx = MAC_ALT_U; break;
            case 'X': macIdx = MAC_ALT_X; break;
            case 'Y': macIdx = MAC_ALT_Y; break;
            case 'Z': macIdx = MAC_ALT_Z; break;
            default: break;
            }
        } else if (!alt && !ctrl && !shift) {
            switch (code) {
            case WXK_F1:  if (bF1Macro) macIdx = MAC_F1;  break;
            case WXK_F2:  macIdx = MAC_F2;  break;
            case WXK_F3:  macIdx = MAC_F3;  break;
            case WXK_F4:  macIdx = MAC_F4;  break;
            case WXK_F5:  macIdx = MAC_F5;  break;
            case WXK_F6:  macIdx = MAC_F6;  break;
            case WXK_F7:  macIdx = MAC_F7;  break;
            case WXK_F8:  macIdx = MAC_F8;  break;
            case WXK_F9:  macIdx = MAC_F9;  break;
            case WXK_F10: macIdx = MAC_F10; break;
            case WXK_F11: macIdx = MAC_F11; break;
            case WXK_F12: macIdx = MAC_F12; break;
            // Numpad digits
            case WXK_NUMPAD0:        kpIdx = eKeypad_0;    break;
            case WXK_NUMPAD1:        kpIdx = eKeypad_1;    break;
            case WXK_NUMPAD2:        kpIdx = eKeypad_2;    break;
            case WXK_NUMPAD3:        kpIdx = eKeypad_3;    break;
            case WXK_NUMPAD4:        kpIdx = eKeypad_4;    break;
            case WXK_NUMPAD5:        kpIdx = eKeypad_5;    break;
            case WXK_NUMPAD6:        kpIdx = eKeypad_6;    break;
            case WXK_NUMPAD7:        kpIdx = eKeypad_7;    break;
            case WXK_NUMPAD8:        kpIdx = eKeypad_8;    break;
            case WXK_NUMPAD9:        kpIdx = eKeypad_9;    break;
            case WXK_NUMPAD_DECIMAL: kpIdx = eKeypad_Dot;  break;
            case WXK_NUMPAD_DIVIDE:  kpIdx = eKeypad_Slash; break;
            case WXK_NUMPAD_MULTIPLY:kpIdx = eKeypad_Star; break;
            case WXK_NUMPAD_SUBTRACT:kpIdx = eKeypad_Dash; break;
            case WXK_NUMPAD_ADD:     kpIdx = eKeypad_Plus; break;
            default: break;
            }
        } else if (!alt && shift && !ctrl) {
            switch (code) {
            case WXK_F1:  if (bF1Macro) macIdx = MAC_SHIFT_F1;  break;
            case WXK_F2:  macIdx = MAC_SHIFT_F2;  break;
            case WXK_F3:  macIdx = MAC_SHIFT_F3;  break;
            case WXK_F4:  macIdx = MAC_SHIFT_F4;  break;
            case WXK_F5:  macIdx = MAC_SHIFT_F5;  break;
            case WXK_F6:  macIdx = MAC_SHIFT_F6;  break;
            case WXK_F7:  macIdx = MAC_SHIFT_F7;  break;
            case WXK_F8:  macIdx = MAC_SHIFT_F8;  break;
            case WXK_F9:  macIdx = MAC_SHIFT_F9;  break;
            case WXK_F10: macIdx = MAC_SHIFT_F10; break;
            case WXK_F11: macIdx = MAC_SHIFT_F11; break;
            case WXK_F12: macIdx = MAC_SHIFT_F12; break;
            default: break;
            }
        } else if (!alt && ctrl && !shift) {
            switch (code) {
            case WXK_F1:  if (bF1Macro) macIdx = MAC_CTRL_F1;  break;
            case WXK_F2:  macIdx = MAC_CTRL_F2;  break;
            case WXK_F3:  macIdx = MAC_CTRL_F3;  break;
            case WXK_F5:  macIdx = MAC_CTRL_F5;  break;
            case WXK_F6:  macIdx = MAC_CTRL_F6;  break;
            case WXK_F7:  macIdx = MAC_CTRL_F7;  break;
            case WXK_F8:  macIdx = MAC_CTRL_F8;  break;
            case WXK_F9:  macIdx = MAC_CTRL_F9;  break;
            case WXK_F10: macIdx = MAC_CTRL_F10; break;
            case WXK_F11: macIdx = MAC_CTRL_F11; break;
            case WXK_F12: macIdx = MAC_CTRL_F12; break;
            // Ctrl+numpad
            case WXK_NUMPAD0:        kpIdx = eCtrl_Keypad_0;    break;
            case WXK_NUMPAD1:        kpIdx = eCtrl_Keypad_1;    break;
            case WXK_NUMPAD2:        kpIdx = eCtrl_Keypad_2;    break;
            case WXK_NUMPAD3:        kpIdx = eCtrl_Keypad_3;    break;
            case WXK_NUMPAD4:        kpIdx = eCtrl_Keypad_4;    break;
            case WXK_NUMPAD5:        kpIdx = eCtrl_Keypad_5;    break;
            case WXK_NUMPAD6:        kpIdx = eCtrl_Keypad_6;    break;
            case WXK_NUMPAD7:        kpIdx = eCtrl_Keypad_7;    break;
            case WXK_NUMPAD8:        kpIdx = eCtrl_Keypad_8;    break;
            case WXK_NUMPAD9:        kpIdx = eCtrl_Keypad_9;    break;
            case WXK_NUMPAD_DECIMAL: kpIdx = eCtrl_Keypad_Dot;  break;
            case WXK_NUMPAD_DIVIDE:  kpIdx = eCtrl_Keypad_Slash; break;
            case WXK_NUMPAD_MULTIPLY:kpIdx = eCtrl_Keypad_Star; break;
            case WXK_NUMPAD_SUBTRACT:kpIdx = eCtrl_Keypad_Dash; break;
            case WXK_NUMPAD_ADD:     kpIdx = eCtrl_Keypad_Plus; break;
            default: break;
            }
        }

        if (macIdx >= 0 && fireMacro(macIdx)) return;
        if (kpIdx  >= 0 && fireKeypad(kpIdx))  return;
    }

    // Any non-navigation key cancels history browsing, Ctrl+R mode, and tab
    // cycling.  A bare MODIFIER press is not such a key: pressing Option to
    // reach Alt+Arrow, or Control to reach Ctrl+R, delivers its own key-down
    // event first, and counting that as "the user typed something" cancelled
    // the browse before the combination even arrived.  Windows has no
    // equivalent hazard — it refreshes its partial only on a real text change
    // (m_bChanged, sendvw.cpp:1178) — so excluding the modifiers here is what
    // makes a multi-press Alt+Arrow walk continue instead of restarting.
    const bool bBareModifier = (code == WXK_ALT     || code == WXK_SHIFT ||
                                code == WXK_CONTROL || code == WXK_RAW_CONTROL);
    if (code != WXK_UP && code != WXK_DOWN && !bBareModifier) {
        m_histIdx    = -1;
        ApplyHistRSearch(false);
    }

    // Shift+Return and Option+Return insert a line break instead of sending, so
    // a multi-line command can be composed by hand rather than only pasted in.
    // Applied to Windows too, where it is Alt+Return and lives in
    // CSendView::PreTranslateMessage — see docs/UPSTREAM-DIVERGENCES.md.
    // Command bar only: the password prompt stays a single-line control.
    if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) &&
        inp == m_input && !evt.RawControlDown() && !evt.MetaDown() &&
        (evt.ShiftDown() || evt.AltDown())) {
        // Inserts at the caret, replacing any selection.  wxEVT_TEXT follows,
        // so UpdateInputBarHeight grows the bar to show the new line.
        m_input->WriteText(wxT("\n"));
        return;   // consume — must not reach the send branch below
    }

    // Plain Return ALWAYS sends and never inserts a newline.  That is precisely
    // CSendView::OnChar (sendvw.cpp:315): it tests `nChar == VK_RETURN` and
    // sends, with no path that lets an unmodified Return reach the multi-line
    // edit control.
    //
    // The command bar is multi-line here as well, so this must be handled at
    // key-down and consumed: letting it through to the control would insert a
    // line break instead of sending.  (It also covers the empty-field case,
    // where wxOSX does not fire wxEVT_TEXT_ENTER at all, so a blank Return —
    // which some MUDs use as a keep-alive — would otherwise be unsendable.)
    if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) &&
        !evt.RawControlDown() && !evt.AltDown() && !evt.MetaDown()) {
        if (inp == m_input || (inp && inp->GetValue().IsEmpty())) {
            wxCommandEvent dummy(wxEVT_TEXT_ENTER);
            OnInputEnter(dummy);
            return;  // consume — don't insert a newline
        }
    }

    evt.Skip();
}

// ── UpdateInputBarHeight ──────────────────────────────────────────────────
// The command bar holds multiple lines (Windows: ES_MULTILINE, sendvw.cpp:310)
// but should still LOOK like a one-line bar until it actually has more than one
// line in it.  Windows solves this by making the command window a splitter pane
// the user drags; there is no such splitter here, so instead the bar grows to
// fit its content and shrinks straight back when the content is sent.
//
// Only a paste can put a newline in here — Return always sends (OnInputKeyDown),
// exactly as CSendView::OnChar does — so in normal typing this never fires.
void CWorldChildFrame::UpdateInputBarHeight()
{
    if (!m_input) return;

    // wxTE_DONTWRAP means GetNumberOfLines() counts real lines, not display rows.
    const int wanted = std::max(1, std::min(m_input->GetNumberOfLines(),
                                            (int) kMaxInputBarLines));
    if (wanted == m_inputBarLines) return;   // nothing to do — avoids relayout churn
    m_inputBarLines = wanted;

    // Derive the chrome (border + internal margins) from the control itself
    // rather than hard-coding it, so it stays right across themes and DPI.
    const int lineH  = std::max(1, m_input->GetCharHeight());
    const int chrome = std::max(0, m_input->GetSize().y -
                                   m_input->GetClientSize().y) + 4;

    m_input->SetMinSize(wxSize(-1, wanted * lineH + chrome));
    Layout();
}

// ── OnInputEnter ──────────────────────────────────────────────────────────
void CWorldChildFrame::OnInputEnter(wxCommandEvent& /*evt*/)
{
    if (!m_doc) return;

    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;

    // Windows sets the action source across the whole Return handler
    // (sendvw.cpp:323) and clears it once the send is done (:371), so
    // GetInfo(239) reads eUserTyping (1) inside any alias or plugin callback the
    // line fires.  Nothing on the port set it at all, so a typed command
    // reported 0 / eUnknownActionSource.  This handler has four exits, hence the
    // scope guard rather than four assignments.
    struct ActionSourceGuard {
        CMUSHclientDoc* doc;
        ~ActionSourceGuard() { doc->m_iCurrentActionSource = eUnknownActionSource; }
    } asGuard { m_doc };
    m_doc->m_iCurrentActionSource = eUserTyping;

    // "Spell check on send" (m_bSpellCheckOnSend): check the line before it goes
    // anywhere, then re-read the possibly-corrected text.  Mirrors sendvw.cpp:329.
    // Collapse any selection first so SpellCheckInput checks the whole line, and
    // never run it on a masked password prompt.
    // Two things this used to get wrong, both from sendvw.cpp:329-346:
    //   * a SCRIPT command was checked like prose -- with a script prefix
    //     configured, typing `/tell_all("hi")` popped the suggestion dialog on
    //     the Lua identifiers, and accepting a suggestion rewrote the code
    //     before it ran.  Windows skips the check entirely for those lines.
    //   * CANCELLING the suggestion dialog still sent the line.  On Windows
    //     `if (!bOK) return;` aborts the send and leaves the text in the bar.
    if (m_doc->m_bSpellCheckOnSend && !m_doc->m_bNoEcho &&
        !m_doc->IsScriptPrefixCommand(CString(inp->GetValue().ToUTF8().data()))) {
        inp->SetSelection(0, 0);
        if (!SpellCheckInput())
            return;   // spell check was cancelled — don't send, don't clear
    }

    wxString text = inp->GetValue();
    inp->Clear();
    m_histIdx = -1; ApplyHistRSearch(false);   // exit history browsing on send
    m_histParked = eHistNotParked;             // ...and forget any parked end

    // Don't record passwords in history, or write them to the log.
    //
    // Ask WHICH CONTROL the text came from, not just what the echo flag says
    // right now.  m_bNoEcho is set the instant the telnet parser sees IAC
    // WILL/WONT ECHO, but the visible swap to and from the masked bar happens in
    // OnWorldTimer, up to 100 ms later.  In the window after the server restores
    // echo, the flag is already false while the masked control is still on screen
    // and still focused — so text the user typed as a password would have been
    // classified as an ordinary command.  Testing the control closes that window;
    // the flag stays in the test as well, so a masked prompt is still recognised
    // in the moment before the swap.
    bool isPassword = (inp == m_inputPassword) || m_doc->m_bNoEcho;

    // History is recorded at the END of this function, not here.  Windows
    // orders it that way (sendvw.cpp:736, AFTER Execute) for three reasons the
    // port used to lose by pushing first:
    //   * an alias with "omit from command history" sets
    //     m_bOmitFromCommandHistory while it is being evaluated, so the flag
    //     does not exist yet at this point -- nothing in platform/ read it at
    //     all, making that alias option dead;
    //   * a plugin's OnPluginCommandEntered may DISCARD the command ("\t"), and
    //     Windows never records a discarded one (CSendView::OnChar returns
    //     before SendCommand);
    //   * a plugin may REWRITE the command, and Windows records the rewritten
    //     text, not what was typed.
    m_doc->m_bOmitFromCommandHistory = false;    // sendvw.cpp:625

    // Unpause output on send (unpause_on_send world option, default on).
    if (m_doc->m_bUnpauseOnSend && m_output && m_output->IsPaused())
        m_output->SetPaused(false);

    // EvaluateCommand processes aliases, speed-walks, and scripting callbacks
    // before sending the text to the MUD — the same path Windows uses for
    // keyboard input from CMUSHclientView::OnChar / ProcessCommand.
    // EvaluateCommand drops empty strings immediately (evaluate.cpp), so bypass
    // it for blank Enter presses and send the bare newline directly.
    CString cmd(text.ToUTF8().data());
    bool bOmitFromLog = isPassword;   // don't log passwords either

    // Fire OnPluginCommandEntered — mirrors sendvw.cpp line 352.
    // Plugins may modify cmd, or return "\t" (discard) / "\r" (ignore silently).
    if (!cmd.IsEmpty()) {
        m_doc->SendToAllPluginCallbacksRtn(ON_PLUGIN_COMMAND_ENTERED, cmd);
        if (cmd == "\t") {
            // Plugin discarded the command; no-op (input already cleared).
            m_doc->UpdateAllViews(nullptr);
            return;
        }
        if (cmd == "\r") {
            // Plugin said ignore silently.
            m_doc->UpdateAllViews(nullptr);
            return;
        }
    }

    // "Auto repeat last command" (m_bAutoRepeat): a blank Enter re-sends the
    // previous command instead of sending a bare newline.  Mirrors sendvw.cpp:725.
    if (cmd.IsEmpty() && m_doc->m_bAutoRepeat && !isPassword && !m_history.empty())
        cmd = CString(m_history.back().ToUTF8().data());

    // Auto-say (World Properties > Input > Auto Say).  Handles the override
    // prefix, the three exclusions and the prepend+send itself — including its
    // own backslash translation on the re-evaluate arm — so it runs BEFORE the
    // plain-send translation below, mirroring sendvw.cpp's ordering.  Returns
    // true when it consumed the command; it may also strip the override prefix
    // and return false, in which case we send the stripped text normally.
    // What Windows puts in the history: the command as the plugin left it, but
    // BEFORE auto-say strips its prefix and before backslash translation --
    // sendvw.cpp:736 records strOriginalCommand, "we might have stripped
    // auto-say prefix".
    const CString strForHistory = cmd;

    if (!cmd.IsEmpty() && m_doc->ApplyAutoSay(cmd)) {
        // Auto-say still records: on Windows the history add sits after BOTH
        // arms of the auto-say if/else, not inside the plain-send one.
        if (!isPassword && !m_doc->m_bOmitFromCommandHistory)
            addToCommandHistory(strForHistory);
        m_doc->UpdateAllViews(nullptr);
        return;
    }

    // "Translate backslash sequences" (m_bTranslateBackslashSequences): turn \n,
    // \t, \\ etc. in the typed line into the real characters, exactly as
    // sendvw.cpp:678/707 does at its Execute() call.
    if (!cmd.IsEmpty() && m_doc->m_bTranslateBackslashSequences)
        cmd = ::FixupEscapeSequences(cmd);

    if (isPassword) {
        // A masked password is deliberately NOT routed through Execute().
        // Execute() would apply command stacking -- splitting a password that
        // contains the stack character (';' by default) into two commands and
        // sending half of it as a separate line -- and would run a password that
        // happens to start with the script prefix through the script engine.
        // It also has no way to carry bOmitFromLog, so the password would be
        // written to the session log.  Keep the direct path for this case.
        if (cmd.IsEmpty())
            m_doc->DoSendMsg(cmd, m_doc->m_display_my_input, !bOmitFromLog);
        else
            m_doc->EvaluateCommand(cmd, true /*countThem*/, bOmitFromLog);
    } else {
        // Everything else goes through the canonical engine entry point, which
        // the port previously bypassed entirely by calling EvaluateCommand
        // directly.  Execute() (scripting/methods/methods_commands.cpp:159) is a
        // strict superset and restores four things that simply did not work
        // here: the SCRIPT PREFIX (:180-213), COMMAND STACKING including the
        // ";;" escape and the leading-";" disable (:220-239), the
        // MAX_EXECUTION_DEPTH recursion guard (:167-171), and a per-sub-command
        // ON_PLUGIN_COMMAND callback (:264-274).  It also owns the empty-line
        // arm (CheckConnected + SendMsg(ENDLINE), :278-292), which is what the
        // port used to hand-roll just above.
        //
        // Depth is reset first because this is a hand-typed line, matching
        // sendvw.cpp:702 (Windows does the same before its Execute call).
        m_doc->m_iExecutionDepth = 0;
        if (m_doc->Execute(cmd) != 0 /*eOK*/) {
            // sendvw.cpp:729-731 returns here, so a command that failed to
            // execute is not recorded.
            m_doc->UpdateAllViews(nullptr);
            return;
        }
    }

    // sendvw.cpp:735-737.  m_bOmitFromCommandHistory is set by evaluate.cpp:896
    // while an alias with "omit from command history" is being evaluated, which
    // is why this has to come after Execute rather than before it.
    if (!isPassword && !m_doc->m_bOmitFromCommandHistory)
        addToCommandHistory(strForHistory);

    // Flush any output produced by alias scripts / triggers to the pane.
    m_doc->UpdateAllViews(nullptr);
}

// ── NotifyPluginLayoutSettle ───────────────────────────────────────────────
// Refresh the doc's GUI geometry (GetInfo(280/281) = rows*lineH / cols*charW)
// from the current pane size, then fire ON_PLUGIN_WORLD_OUTPUT_RESIZED so
// plugins re-lay-out their docked miniwindows.  Used to settle the startup
// layout for plugins (e.g. Quow's Cow Bar) that defer their layout until after
// the world connects; see OnWorldTimer.
void CWorldChildFrame::NotifyPluginLayoutSettle()
{
    if (!m_doc) return;
    if (m_output) {
        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        int paneW = 0, paneH = 0;
        m_output->GetClientSize(&paneW, &paneH);
        if (charW > 0 && lineH > 0 && paneW > 0 && paneH > 0) {
            m_doc->m_gui_cols        = paneW / charW;
            m_doc->m_gui_rows        = paneH / lineH;
            m_doc->m_gui_font_width  = charW;
            m_doc->m_gui_font_height = lineH;
        }
    }
    m_doc->SendToAllPluginCallbacks(ON_PLUGIN_WORLD_OUTPUT_RESIZED);
}

// ── OnWorldTimer ──────────────────────────────────────────────────────────
// Called every 100 ms.  Drives MUSHclient world/plugin timers, flushes
// output, updates UI on phase changes, and runs the auto-reconnect countdown.
void CWorldChildFrame::OnWorldTimer(wxTimerEvent& /*evt*/)
{
    // Toolbar enable/check state and the activity bar's per-world colours.
    // Windows drives these from MFC's idle-time ON_UPDATE_COMMAND_UI pump plus
    // the frame's ACTIVITY_TIMER; the port has neither, so ride the world timer
    // (UpdateState only touches wx state that actually changed).
    if (m_toolbars)
        m_toolbars->UpdateState(m_doc);

    // Pick up a change to the world's input colours (World Properties > Input >
    // Commands).  Those live on the doc with no notification seam, so poll them
    // — but only repaint when they actually differ from what we last applied,
    // so this costs two comparisons per tick in the normal case.
    if (m_doc) {
        const unsigned long fg = (unsigned long) m_doc->m_input_text_colour;
        const unsigned long bg = (unsigned long) m_doc->m_input_background_colour;
        if (fg != m_lastInputFg || bg != m_lastInputBg) {
            m_lastInputFg = fg;
            m_lastInputBg = bg;
            if (!m_histRSearch)   // don't fight the Ctrl+R tint
                UpdateInputBarColors();
        }
    }

    // Same treatment for the output-pane options that the constructor applies
    // once and nothing ever re-applied, so changing them in World Properties >
    // Appearance > Output only took effect after closing and reopening the
    // world: scrollback limit, freeze-on-scrollback, auto-copy selection, and
    // whether that auto-copy is HTML.  Cheap scalar compares; only touch the
    // pane when something actually changed.
    if (m_doc && m_output) {
        const long maxl   = (long) m_doc->m_maxlines;
        const bool freeze = m_doc->m_bAutoFreeze != 0;
        const bool copySel= m_doc->m_bCopySelectionToClipboard != 0;
        const bool asHtml = m_doc->m_bAutoCopyInHTML != 0;
        const int  poff   = (int) m_doc->m_iPixelOffset;
        if (maxl != m_lastMaxLines || freeze != m_lastAutoFreeze ||
            copySel != m_lastCopySel || asHtml != m_lastCopyHtml) {
            m_lastMaxLines  = maxl;
            m_lastAutoFreeze= freeze;
            m_lastCopySel   = copySel;
            m_lastCopyHtml  = asHtml;
            if (maxl > 0) m_output->SetMaxLines((int) maxl);
            m_output->SetAllowAutoFreeze(freeze);
            m_output->SetCopySelectionToClipboard(copySel);
            m_output->SetAutoCopyInHTML(asHtml);
        }
        // The pixel offset changes the usable width, so unlike its neighbours it
        // also has to resync the engine's wrap column and NAWS -- otherwise the
        // MUD keeps wrapping to the old width until the next window resize.
        // Polled here because the option is reachable from World Properties AND
        // from world.SetOption("pixel_offset"), neither of which has a seam.
        if (poff != m_lastPixelOffset) {
            m_lastPixelOffset = poff;
            m_output->SetTextPixelOffset(poff);
            textRectChanged();
        }
    }

    if (!m_doc) return;

    // ── Speed-walk / command queue draining ──────────────────────────────────
    // On Windows, CTimerWnd fires a WM_TIMER at m_iSpeedWalkDelay ms intervals
    // and drains one queued command each time.  On non-Windows, m_pTimerWnd is
    // null, so we replicate that rate here via an accumulator.
    if (!m_doc->m_QueuedCommandsList.IsEmpty()) {
        int delay = m_doc->m_iSpeedWalkDelay;   // 0 = flush all immediately
        if (delay <= 0) {
            // No delay — drain the entire queue now.
            while (!m_doc->m_QueuedCommandsList.IsEmpty()) {
                CString strCommand = m_doc->m_QueuedCommandsList.RemoveHead();
                char cMessageType = strCommand[0];
                m_doc->DoSendMsg(strCommand.Mid(1),
                                 toupper(cMessageType) == QUEUE_WITH_ECHO ||
                                 toupper(cMessageType) == IMMEDIATE_WITH_ECHO,
                                 cMessageType >= 'A');
            }
            m_speedWalkAccum = 0;
        } else {
            // Drain one QUEUE_* command each time the accumulator reaches the delay.
            m_speedWalkAccum += 100;    // 100 ms per timer tick
            while (m_speedWalkAccum >= delay && !m_doc->m_QueuedCommandsList.IsEmpty()) {
                m_speedWalkAccum -= delay;
                // Drain through to (and including) the next QUEUE_* command;
                // interleaved IMMEDIATE_* commands fall through in the same slot,
                // matching CTimerWnd::OnTimer (which only breaks on QUEUE_*).
                for (;;) {
                    CString strCommand = m_doc->m_QueuedCommandsList.RemoveHead();
                    char cMessageType = strCommand[0];
                    m_doc->DoSendMsg(strCommand.Mid(1),
                                     toupper(cMessageType) == QUEUE_WITH_ECHO ||
                                     toupper(cMessageType) == IMMEDIATE_WITH_ECHO,
                                     cMessageType >= 'A');
                    if (toupper(cMessageType) == QUEUE_WITH_ECHO ||
                        toupper(cMessageType) == QUEUE_WITHOUT_ECHO)
                        break;   // QUEUE_* command consumes this delay slot
                    if (m_doc->m_QueuedCommandsList.IsEmpty())
                        break;
                }
            }
        }
        m_doc->ShowQueuedCommands();
    } else {
        m_speedWalkAccum = 0;   // reset when queue is empty
    }

    // ── World timers (triggers based on elapsed time, speed-walk queues, etc.)
    // File > Preferences > Timers > "Check timers every N seconds"
    // (App.m_nTimerInterval, default 0).  Windows treats 0 as "a tenth of a
    // second" and any other value as that many seconds, in
    // CMainFrame::CheckTimerFallback (mainfrm.cpp:2432-2440):
    //     double fInterval = 0.10;
    //     if (App.m_nTimerInterval) fInterval = App.m_nTimerInterval;
    //     if (fElapsedTime > fInterval) ProcessTimers ();
    // That function is unreachable here (no MFC timer), so this tick drained
    // the world's timers every 100 ms regardless and the setting did nothing.
    // 100 ms IS the default cadence, so leaving 0 alone keeps today's
    // behaviour; a non-zero value now spaces the checks out as asked.
    m_timerCheckAccum += 100;
    if (m_timerCheckAccum >= (App.m_nTimerInterval > 0
                              ? (int) App.m_nTimerInterval * 1000 : 100)) {
        m_timerCheckAccum = 0;
        m_doc->CheckTimers();
    }

    // ── Script file monitoring: poll mtime every 2 s ─────────────────────
    // Equivalent to the Windows FindFirstChangeNotification thread, but runs
    // on the main thread so UMessageBox / scripting calls are always safe.
    m_scriptCheckAccum += 100;
    if (m_scriptCheckAccum >= 2000) {
        m_scriptCheckAccum = 0;
        m_doc->OnScriptFileChanged(false);
    }

    // ── ON_PLUGIN_TICK: Windows fires this every ~40 ms.  We drive it via an
    //    accumulator so that the average rate matches even though our timer
    //    only fires every 100 ms.  Each 100 ms tick we fire it 2–3 times,
    //    keeping the long-run rate at 1 call per 40 ms.
    m_pluginTickAccum += 100;
    while (m_pluginTickAccum >= 40) {
        m_pluginTickAccum -= 40;
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_TICK);
    }

    // ── Plugin-layout settle re-notify ───────────────────────────────────────
    // Some plugins (e.g. Quow's Cow Bar) only lay out their docked miniwindows
    // on an ON_PLUGIN_WORLD_OUTPUT_RESIZED that arrives *after* they finish
    // initialising — which for them is after the world connects, or a ~4 s
    // fallback timer (they guard layout on a "bPreInstalled" flag set then).
    // Every resize notification we send fires during the initial open (0–1 s),
    // i.e. BEFORE that, so without this the windows stay stacked at the top-left
    // until the first manual resize.  Re-send the notification at ~2 s and ~5 s
    // after open (after both the connect and the fallback) so the layout settles
    // by itself — exactly what a manual resize would trigger.
    // The connect-phase handler below fires this immediately on connect (the
    // earliest the plugins will honour it); these ticks are the fallback for
    // worlds that never connect (Quow flips its flag on a ~4 s timer even then).
    if (m_settleResizeState < 2) {
        m_settleResizeAccum += 100;
        if (m_settleResizeAccum >= (m_settleResizeState == 0 ? 2000 : 5000)) {
            m_settleResizeState++;
            NotifyPluginLayoutSettle();
        }
    }

    // Flush any output produced by the timer callbacks.
    m_doc->UpdateAllViews(nullptr);

    // ── Connection-phase change: update title and output pane notes ───────
    int phase     = m_doc->m_iConnectPhase;
    int prevPhase = m_lastConnectPhase;

    if (phase != prevPhase) {
        m_lastConnectPhase = phase;

        // The world name plus the phase suffix — unless a script has set its
        // own title with world.SetTitle, which wins outright (as it does on
        // Windows, CMUSHView::FixupTitle).  This block used to rebuild the
        // title from the world name unconditionally, so a plugin's title was
        // destroyed by the next connect/disconnect/close and never restored.
        SetTitle(wxString::FromUTF8(
            static_cast<const char*>(m_doc->WorldWindowTitle())));

        // Reaching "connected" is when Quow-style plugins flip the flag that
        // gates their real miniwindow layout (they defer it during load as a
        // crash guard).  Fire the layout re-notify now so docked windows snap
        // into place at connect-time — the earliest possible — instead of
        // waiting for the 2 s/5 s fallback ticks above.
        if (phase == eConnectConnectedToMud && m_settleResizeState < 2) {
            NotifyPluginLayoutSettle();
            m_settleResizeState = 2;   // fallback ticks no longer needed
        }

        // Common strings used in output-pane notes.
        std::string server   = static_cast<const char*>(m_doc->m_server);
        std::string port     = std::to_string(m_doc->m_port);
        std::string hostport = server + ":" + port;

        // ── Output pane annotations ───────────────────────────────────────
        // Suppress the startup pass (prevPhase == -1, phase == eConnectNotConnected)
        // so we don't show a spurious "not connected" note when a world first opens.
        if (prevPhase >= 0 && m_output) {

            if (phase == eConnectConnectedToMud) {
                // Green — successful connection.
                m_output->AppendNote("** Connected to " + hostport + " **",
                                     RGB(68, 204, 68));
                m_reconnectCountdown = 0;
                TriggerNAWS();

            } else if (phase == eConnectNotConnected) {
                if (prevPhase == eConnectConnectedToMud ||
                    prevPhase == eConnectDisconnecting) {
                    // Amber — link dropped or user-initiated disconnect.
                    m_output->AppendNote("** Disconnected from " + hostport + " **",
                                         RGB(255, 136, 0));
                    // Auto-reconnect on unexpected link failure.
                    if (!m_doc->m_bDisconnectOK && App.m_bReconnectOnLinkFailure) {
                        m_reconnectCountdown = 50;  // 50 × 100 ms = 5 s
                    }
                } else if (prevPhase != eConnectNotConnected) {
                    // Red — connection attempt failed.
                    m_output->AppendNote("** Connection to " + hostport + " failed **",
                                         RGB(255, 68, 68));
                }

            } else if (phase == eConnectMudNameLookup &&
                       prevPhase == eConnectNotConnected) {
                // Cyan — new connection attempt starting.
                m_output->AppendNote("** Connecting to " + hostport + "\xe2\x80\xa6 **",
                                     RGB(0, 204, 204));
            }
        }

        // Startup case: already connected when the frame opens (rare but safe).
        if (prevPhase < 0 && phase == eConnectConnectedToMud)
            TriggerNAWS();

        // Refresh the status bar for the active world.
        if (IsActiveWorld())
            UpdateStatusBar();
    }

    // ── Telnet echo suppression (password mode) ───────────────────────────
    // The MUD sends IAC WILL ECHO to suppress echo (password prompts) and
    // IAC WONT ECHO to restore it.  We swap the input bar for a masked one.
    if (m_doc->m_bNoEcho != m_lastNoEcho)
        ApplyNoEcho(m_doc->m_bNoEcho);

    // ── ANSI palette changed → recolour what is already on screen ─────────
    // Windows resolves each style through the palette on every paint, so its
    // scrollback recolours itself; this port bakes the RGB once per line at
    // emit time, so without this only NEW lines would pick up the change.
    //
    // Polled rather than hooked to the prefs dialog on purpose: the palette has
    // several entry points -- World Properties ▸ ANSI Colour, Game ▸ Configure ▸
    // ANSI Colours, the "use default colours" tick, and world.SetNormalColour /
    // SetBoldColour from a script -- and one check here covers all of them.  It
    // also sidesteps an ordering bug in the canonical dialog path, where
    // GamePreferences refreshes the views BEFORE SavePrefsP5 writes the new
    // colours (configuration.cpp:2364 vs :2383).  Comparing 16 COLORREFs every
    // 100 ms is far cheaper than the repaint it guards.  Same shape as the
    // no-echo poll immediately above.
    if (PaletteChanged()) {
        // Keep the reader where they were.  A rebuild re-emits every line, and
        // the pane lands at the bottom when it does; that is right if they were
        // already following the output, and wrong if they had scrolled up to
        // read something -- which is exactly when a colour tweak is likely.
        const bool wasFollowing = m_output && !m_output->IsPaused();
        int viewX = 0, viewY = 0;
        if (m_output)
            m_output->GetViewStart(&viewX, &viewY);
        // Bookmarks need no save/restore around the rebuild: they live on the
        // engine's CLines, and the re-emit carries the flag back out with each
        // line (doc_display_shared.cpp).

        // The pane background follows the palette too when the world has no
        // explicit background of its own (NO_COLOUR) -- see ApplyPaneBackground.
        ApplyPaneBackground(m_doc->m_iBackgroundColour);

        // Detach the log hook across the rebuild.  RebuildOutputView clears the
        // pane and RE-EMITS the whole scrollback, and every line the pane
        // accepts is forwarded to LogLine -- so with a session log open, one
        // colour tweak appended a verbatim second copy of the entire buffer to
        // the log file, a third copy on the next tweak, and so on.  The lines
        // are not new; only the presentation changed.  (OnClose uses the same
        // detach-and-restore trick for m_fn_close_window.)
        auto savedLogHook = m_output ? m_output->m_fn_on_line_appended
                                     : std::function<void(const SOutputLine&)>();
        if (m_output) m_output->m_fn_on_line_appended = nullptr;

        m_doc->RebuildOutputView();

        if (m_output) m_output->m_fn_on_line_appended = savedLogHook;

        if (m_output && !wasFollowing)
            m_output->Scroll(viewX, viewY);
    }

    // ── Auto-reconnect countdown ──────────────────────────────────────────
    if (m_reconnectCountdown > 0) {
        if (phase != eConnectNotConnected) {
            // Reconnect already started (phase advanced) — clear the counter.
            m_reconnectCountdown = 0;
        } else {
            --m_reconnectCountdown;
            if (m_reconnectCountdown == 0) {
                // Time to fire the reconnect.
                if (m_output)
                    m_output->AppendNote("** Reconnecting\xe2\x80\xa6 **",
                                         RGB(255, 204, 0));
                m_doc->m_bDisconnectOK = false;
                m_doc->ConnectSocket();
            }
            // Refresh status bar with countdown (only if active).
            if (IsActiveWorld())
                UpdateStatusBar();
        }
    }
}

// ── pal::IWorldView ─────────────────────────────────────────────────────────
// This frame is the wx adapter for one world's output view.  Each method
// forwards to the output pane / input bar.  Only appendLine() is engine-driven
// today (CMUSHclientDoc::EmitOutputLine); the others are the adapter's ready
// surface, dormant until their engine paths migrate off the m_fn_* callbacks.

void CWorldChildFrame::appendLine(SOutputLine&& line)
{
    if (m_output) m_output->AppendLine(std::move(line));
}

void CWorldChildFrame::note(const char* text)
{
    if (m_output) m_output->AppendNote(std::string(text));
}

// world.Bookmark / Display ▸ Bookmark Selection: the engine's CLine flag
// changed — update the pane's rendering mirror.  lineIndex is 0-based, the
// same space the pane indexes m_lines by (both count from the oldest line the
// buffer still holds).
void CWorldChildFrame::setBookmark(long lineIndex, bool set)
{
    if (m_output) m_output->SetBookmark((int) lineIndex, set);
}

void CWorldChildFrame::setTitle(const char* /*title*/)
{
    // world.SetTitle(s): update the MDI child-frame title bar.  The doc has
    // already stored the new title in m_strWindowTitle, so ask it for the whole
    // title — that way clearing the custom title (SetTitle "") puts the world
    // name AND the connection-phase suffix back, instead of the bare name, and
    // this path and the phase-change tick can never disagree.
    if (m_doc)
        SetTitle(wxString::FromUTF8(
            static_cast<const char*>(m_doc->WorldWindowTitle())));
}

void CWorldChildFrame::setStatus(const char* /*text*/)
{
    // world.SetStatus(msg) and every Frame.SetStatusMessage caller land here.
    // The text is already in m_doc->m_strStatusMessage (ShowStatusLine sets it
    // before emitting), and UpdateStatusBar reads it from there along with the
    // world name, duration and line counts, so just repaint.
    //
    // This used to forward to g_wxhook_set_status, i.e. to the MDI PARENT's
    // status bar -- which wxOSX hides behind the child -- so nothing was ever
    // visible.  Each world owns its own bar, so there is also no reason to gate
    // on IsActiveWorld() any more: a background world updates its own strip.
    UpdateStatusBar();
}

void CWorldChildFrame::setBackgroundColour(pal::Colour c)
{
    ApplyPaneBackground(c);
}

void CWorldChildFrame::setMaxLines(int n)
{
    if (m_output) m_output->SetMaxLines(n);
}

void CWorldChildFrame::requestRepaint(pal::Rect /*area*/)
{
    // The engine only ever needs a full-pane refresh here; the sub-rect is
    // advisory and the pane repaints lazily, so ignore it. Refresh(false) = no
    // background erase (same semantics as the old repaint callback).
    if (m_output) m_output->Refresh(false);
}

void CWorldChildFrame::clearOutput()
{
    if (m_output) m_output->ClearOutput();
}

void CWorldChildFrame::setPaused(bool paused)
{
    if (m_output) m_output->SetPaused(paused);
}

// The read side of setPaused — what Display ▸ Freeze Output draws its tick
// from (Windows reads CMUSHView::m_freeze for the same purpose).
bool CWorldChildFrame::isPaused() const
{
    return m_output && m_output->IsPaused();
}

void CWorldChildFrame::flash()
{
    RequestUserAttention();
}

std::string CWorldChildFrame::currentCommand() const
{
    wxTextCtrl* in = ActiveInput();
    return in ? std::string(in->GetValue().utf8_str()) : std::string();
}

pal::TextRange CWorldChildFrame::outputSelection() const
{
    // Line/col endpoints (1-based; 0 = no selection), from the pane's selection
    // state — same semantics as the old selection callback.
    pal::TextRange r;
    if (m_output) {
        long sl = 0, sc = 0, el = 0, ec = 0;
        if (m_output->GetSelectionInfo(sl, sc, el, ec))
            r = { (int) sl, (int) sc, (int) el, (int) ec };
    }
    return r;
}

int CWorldChildFrame::scrollPos() const
{
    return m_output ? (int) m_output->GetScrollPositionY() : 0;
}

pal::Size CWorldChildFrame::outputAreaChars() const
{
    return m_doc ? pal::Size{ m_doc->m_gui_cols, m_doc->m_gui_rows }
                 : pal::Size{ 80, 24 };
}

// ── the command input bar (P2 command family) ────────────────────────────────

// world.SetCommand(text): set the command bar contents.  Per the Windows API
// contract, returns false (eCommandNotEmpty) and does NOT overwrite if the bar
// is already non-empty.  Returns true on success.
bool CWorldChildFrame::setCommand(const char* text)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return false;
    if (!inp->GetValue().IsEmpty()) return false;  // eCommandNotEmpty
    inp->SetValue(wxString::FromUTF8(text));
    inp->SetInsertionPointEnd();
    m_histIdx = -1; ApplyHistRSearch(false);
    return true;
}

// world.SelectCommand(): select all text in the command input bar.
void CWorldChildFrame::selectCommand()
{
    wxTextCtrl* inp = ActiveInput();
    if (inp) inp->SetSelection(-1, -1);  // wxWidgets: -1,-1 = select all
}

// GetInfo(236/237): the command input selection range.
//
// The seam speaks UTF-8 BYTE offsets into GetCommandText(), because that is what
// every shared body scanning the command text indexes with, and what Windows'
// CEdit::GetSel returns over its ANSI buffer.  wxTextCtrl counts CHARACTERS, so
// the two disagree the moment the bar holds anything non-ASCII -- which used to
// mangle tab completion (doc_tabcomplete_shared.cpp reads strCurrent[nStartChar]
// and splices with the same numbers).
pal::Range CWorldChildFrame::commandSelection() const
{
    pal::Range r;
    wxTextCtrl* inp = ActiveInput();
    if (inp) {
        long from = 0, to = 0;
        inp->GetSelection(&from, &to);
        const wxScopedCharBuffer utf8 = inp->GetValue().utf8_str();
        r.start = (int) UTF8CharToByteOffset(utf8.data(), utf8.length(), (size_t) from);
        r.end   = (int) UTF8CharToByteOffset(utf8.data(), utf8.length(), (size_t) to);
    }
    return r;
}

// world.SetCommandSelection(first, last): select a range in the command bar.
// Indices are 1-based BYTE positions (0 = end of bar) -- the same space
// commandSelection() reports in, so GetInfo(236/237) can be fed straight back,
// and the same space Windows' CEdit::SetSel works in.
void CWorldChildFrame::setCommandSelection(long first, long last)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;
    const wxScopedCharBuffer utf8 = inp->GetValue().utf8_str();
    const size_t nbytes = utf8.length();
    long f = (first <= 0) ? (long) nbytes : first - 1;
    // End is exclusive (one past the last char), matching Windows'
    // CEdit::SetSel(First - 1, Last): only the start is decremented.
    long l = (last  <= 0) ? (long) nbytes : last;
    inp->SetSelection(
        (long) UTF8ByteToCharOffset(utf8.data(), nbytes, (size_t) (f < 0 ? 0 : f)),
        (long) UTF8ByteToCharOffset(utf8.data(), nbytes, (size_t) (l < 0 ? 0 : l)));
}

// Replace [start, end) in the command bar as one undoable edit.  Offsets are
// RAW (0-based) UTF-8 BYTE offsets, matching commandSelection() — not the
// 1-based scripting space of setCommandSelection above.  Tab completion is the
// caller, and it derived these by indexing GetCommandText() itself.
void CWorldChildFrame::replaceCommandSelection(long start, long end,
                                               const char* text)
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return;

    const wxScopedCharBuffer utf8 = inp->GetValue().utf8_str();
    const long cs = (long) UTF8ByteToCharOffset(utf8.data(), utf8.length(),
                                                (size_t) (start < 0 ? 0 : start));
    const long ce = (long) UTF8ByteToCharOffset(utf8.data(), utf8.length(),
                                                (size_t) (end   < 0 ? 0 : end));

    const wxString replacement = wxString::FromUTF8(text ? text : "");
    // wxTextCtrl::Replace is a single undo record, like CEdit::ReplaceSel with
    // bCanUndo=TRUE.  It leaves the caret after the inserted text.
    inp->Replace(cs, ce, replacement);
    inp->SetInsertionPoint(cs + (long) replacement.length());
}

// world.PushCommand(): save current bar text to history, clear bar.
std::string CWorldChildFrame::pushCommand()
{
    wxTextCtrl* inp = ActiveInput();
    if (!inp) return {};
    std::string text(inp->GetValue().ToUTF8());
    if (!text.empty()) {
        wxString wxText = wxString::FromUTF8(text.c_str());
        if (m_history.empty() || m_history.back() != wxText)
            m_history.push_back(wxText);
        if ((int)m_history.size() > HistoryLimit())
            m_history.erase(m_history.begin());
    }
    inp->Clear();
    m_histIdx = -1;
    ApplyHistRSearch(false);
    return text;
}

// world.GetCommandList(count): up to count most-recent history entries,
// newest first.  count <= 0 means all.
std::vector<std::string> CWorldChildFrame::commandList(long count) const
{
    std::vector<std::string> result;
    if (m_history.empty()) return result;
    long total = (long)m_history.size();
    long n = (count <= 0) ? total : std::min(count, total);
    result.reserve((size_t)n);
    for (long i = total - 1; i >= total - n; --i)
        result.push_back(std::string(m_history[(size_t)i].ToUTF8()));
    return result;
}

// world.AddToCommandHistory(text): inject a string into history.
// Mirrors the duplicate-suppression logic in methods_sending.cpp.
void CWorldChildFrame::addToCommandHistory(const char* text)
{
    if (!*text) return;
    wxString entry = wxString::FromUTF8(text);
    if (!m_history.empty() && m_history.back() == entry)
        return;   // same as last entry — don't add duplicate
    m_history.push_back(entry);
    if ((int)m_history.size() > HistoryLimit())
        m_history.erase(m_history.begin());
    m_histIdx = -1;  // reset browsing position
}

// world.DeleteCommandHistory(): clear the input history.
void CWorldChildFrame::deleteCommandHistory()
{
    m_history.clear();
    m_histIdx = -1;
    m_histSaved.Clear();
    // Rewrite (empty) history file so the cleared history persists.
    if (!m_historyFilePath.empty()) {
        FILE* f = fopen(m_historyFilePath.c_str(), "w");
        if (f) fclose(f);
    }
}

// world.SetInputFont(face, pts, bold, italic): change the command-bar font.
// face "" / pts 0 mean "keep current".
void CWorldChildFrame::setInputFont(const char* face, int pointSize,
                                    bool bold, bool italic)
{
    wxFont cur = m_input ? m_input->GetFont() : wxNullFont;
    wxString faceName = !*face
        ? (cur.IsOk() ? cur.GetFaceName() : wxString())
        : wxString::FromUTF8(face);
    int size = (pointSize > 0) ? pointSize : (cur.IsOk() ? cur.GetPointSize() : 10);
    wxFontStyle  style  = italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    wxFontWeight weight = bold   ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL;
    wxFont f(size, wxFONTFAMILY_TELETYPE, style, weight,
             /*underline=*/false, faceName);
    if (!f.IsOk()) return;
    // InvalidateBestSize() is not optional.  These bars sit in the frame's
    // vertical sizer at proportion 0, so their height comes from the control's
    // cached best size -- and wxOSX does not invalidate that cache on SetFont.
    // Without this the font changes but the bar keeps its old height, clipping
    // the taller text to a sliver, and the next relayout (a window resize, say)
    // restores the stale height even if this Layout() happened to look right.
    if (m_input) {
        m_input->SetFont(f);
        m_input->InvalidateBestSize();
        m_input->Refresh();
    }
    if (m_inputPassword) {
        m_inputPassword->SetFont(f);
        m_inputPassword->InvalidateBestSize();
        m_inputPassword->Refresh();
    }
    // The bar's height is a pinned min size (UpdateInputBarHeight), which wins
    // over best size — so it has to be recomputed against the new line height,
    // not just invalidated.  Clearing the cached line count forces that.
    m_inputBarLines = 0;
    UpdateInputBarHeight();   // calls Layout()
}

// ── output-pane management (P2 output family) ────────────────────────────────

// Bracket a run of appendLine calls: one scrollbar update + repaint per batch.
void CWorldChildFrame::beginBatch()
{
    if (m_output) m_output->BeginBatch();
}

void CWorldChildFrame::endBatch()
{
    if (m_output) m_output->EndBatch();
}

// world.DeleteLines(n): remove the last n lines from the visual buffer.
void CWorldChildFrame::deleteLines(long n)
{
    if (m_output) m_output->DeleteTailLines(n);
}

// world.SetScroll(line): -1 = bottom (resume auto-scroll, matching Windows'
// "-1 goes to the end"), 0 = top, > 0 = 1-based line number.
// (-2 "no change" is filtered engine-side.)
// world.SetScroll(Position, Visible).  Position is a device-pixel Y, NOT a line
// number: Windows does `pt.y = Position; pmyView->ScrollToPosition(pt, false)`
// (methods_output.cpp:766-786) and pal/win_worldview.cpp repeats it verbatim, so
// pixels is the seam's contract.  The port treated it as a 1-based line, which
// also contradicted its own GetInfo(296) — that returns scrollPos() in PIXELS,
// so the obvious round-trip (read the position, scroll back to it) landed
// roughly line-height times too far up.
void CWorldChildFrame::setScroll(long pos)
{
    if (!m_output) return;
    if (pos == -1) {
        m_output->ScrollToBottom();
    } else if (pos <= 0) {
        m_output->ScrollToTop();
    } else {
        const int lineH = m_output->LineHeight();
        m_output->ScrollToLine(lineH > 0 ? (int)(pos / lineH) : (int) pos);
    }
}

// world.SetSelection(sl, el, sc, ec): set text selection in the output pane.
// Lines and columns are 1-based; 0 means start/end of line.
// All-zeros clears the selection.
void CWorldChildFrame::setOutputSelection(int sl, int el, int sc, int ec)
{
    if (m_output) m_output->SetSelectionRange(sl, el, sc, ec);
}

// world.SetOutputFont(face, pts, bold, italic): change the output pane font.
// pts == 0 means "keep current size".
void CWorldChildFrame::setOutputFont(const char* face, int pointSize,
                                     bool bold, bool italic)
{
    if (!m_output) return;
    wxFont cur = m_output->GetCurrentFont();
    wxString faceName = !*face
        ? cur.GetFaceName()
        : wxString::FromUTF8(face);
    int size = (pointSize > 0) ? pointSize : cur.GetPointSize();
    wxFontStyle  style  = italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    wxFontWeight weight = bold   ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL;
    wxFont f(size, wxFONTFAMILY_TELETYPE, style, weight,
             /*underline=*/false, faceName);
    if (!f.IsOk()) f = cur;  // fall back to current font on failure
    m_output->SetOutputFont(f);
    // Update doc font fields so Ctrl+S saves the new font to the world file.
    {
        std::string faceStr = f.GetFaceName().ToUTF8().data();
        m_doc->m_font_name   = CString(faceStr.c_str());
        m_doc->m_font_height = -(f.GetPointSize() * 96 / 72);
        m_doc->m_font_weight = (f.GetWeight() >= wxFONTWEIGHT_BOLD) ? 700 : 400;
    }
    // Persist face name and size so they survive restart even without saving.
    wxConfig cfg(wxT("mushclient"));
    cfg.Write(wxT("/display/font_name"), faceName);
    cfg.Write(wxT("/display/font_size"), (long)f.GetPointSize());
    // NOTE: the input bars are deliberately NOT synced here.  The input font is
    // a separate setting (World Properties > Input → ChangeInputFont →
    // setInputFont), and on Windows changing the output font never touches it.
    // Syncing here would clobber the user's configured input font every time the
    // output font changed — configuration.cpp applies the input font first and
    // the output font second, so the output font would always win.  The input
    // bars are still seeded from the output font at construction, and Ctrl+scroll
    // zoom still updates both on purpose (see m_fn_grid_changed).
    Layout();
}

// world.TextRectangle(): a plugin changed the reserved text area — recompute
// the engine wrap column + re-send NAWS for the new visible width, so the MUD
// wraps to the same width the client shows (no surprise client-side
// re-wrapping).  Plugins set the rectangle DURING OnPluginWorldOutputResized,
// i.e. AFTER the resize handler already sent NAWS for the full width, so this
// re-sends it; gated on an actual change to avoid NAWS spam.
void CWorldChildFrame::textRectChanged()
{
    ApplyWrapColumnAndNAWS(/*skipIfUnchanged=*/true);
}

// world.GetWorldWindowPosition(): the rect of THIS WORLD'S WINDOW — the same
// window world.MoveWorldWindow moves (moveWorldWindow below), in the same
// coordinate space, so the standard save/restore idiom
//     local p = GetWorldWindowPosition(); ... MoveWorldWindow(p.left, p.top,
//                                                             p.width, p.height)
// is a no-op.  Windows reports the frame too: GetWorldWindowPositionX asks the
// view's parent frame for its GetWindowPlacement, and MoveWorldWindowX calls
// MoveWindow on that same frame (methods_output.cpp).
//
// This used to return the OUTPUT PANE's client rect, which is a different
// window: restoring it moved the frame's top-left to where the pane's client
// origin was (down by the title bar) and shrank the frame to the pane's size
// (losing the title bar and input bar), so every save/restore round trip walked
// the window down the screen and shrank it.  Scripts that want the pane's own
// dimensions have GetInfo(280)/(281).
pal::Rect CWorldChildFrame::outputRect() const
{
    // GetRect() is exactly the inverse of the SetSize(x, y, w, h) below (both
    // are position-in-parent + outer size; for the MDI child, which is a
    // top-level window on this platform, that is screen space).
    const wxRect r = GetRect();
    return { r.x, r.y, r.width, r.height };
}

// ── world/main window management (P2 window family) ──────────────────────────

// world.SetMainTitle(s): update the main application window title.
void CWorldChildFrame::setMainTitle(const char* title)
{
    if (wxFrame* top = dynamic_cast<wxFrame*>(wxTheApp->GetTopWindow()))
        top->SetTitle(wxString::FromUTF8(title));
}

// world.MoveWorldWindow(l,t,w,h): reposition/resize this MDI child frame.
// Coordinates are in pixels relative to the MDI parent client area.
void CWorldChildFrame::moveWorldWindow(long l, long t, long w, long h)
{
    SetSize((int)l, (int)t, (int)w, (int)h);
}

// world.SetWorldWindowStatus(n): change the MDI child frame window state.
// 1=maximize, 2=iconize/minimize, 3=restore, 4=show normal.
void CWorldChildFrame::setWindowStatus(int status)
{
    switch (status)
      {
      case 1: Maximize(true);                    break;
      case 2: Iconize(true);                     break;
      case 3: Restore();                         break;
      case 4: Maximize(false); Iconize(false);   break;
      }
}

// world.ActivateClient(): restore the main frame if iconized and raise it.
void CWorldChildFrame::activateClient()
{
    wxMDIParentFrame* parent = wxDynamicCast(GetParent(), wxMDIParentFrame);
    if (parent) {
        if (parent->IsIconized()) parent->Restore();
        parent->Raise();
    }
    // Also bring this child frame to the front within the MDI.
    Activate();
}

// world.GetInfo(125): is this world's frame full-screen?
//
// Deliberately the CHILD, not the MDI parent: on macOS the child IS the
// on-screen window, and View > Full Screen now acts on it (wx_visible_frame
// in wx_mfc_impl.cpp).  While the menu full-screened the hidden parent this
// was hard-wired false, which is what made GetInfo(125) a lie.  Do not
// "fix" it back to Frame.IsFullScreen(); that is the Windows topology, where
// there is one frame and full screen is global to it.  Documented divergence:
// here full screen is per-world-window.
bool CWorldChildFrame::isFullscreen() const
{
    return IsFullScreen();
}

// world.Save(): save the world file to its current path.  Returns false
// if the world has no path yet (the caller should handle that case).
bool CWorldChildFrame::saveWorld()
{
    CString pathCStr = m_doc->GetPathName();
    std::string savePath = static_cast<const char*>(pathCStr);
    if (savePath.empty()) return false;
    // DoSave (not OnSaveDocument) so world.Save() from a script fires the
    // on_world_save handler and the OnPluginWorldSave callback, as on Windows.
    return m_doc->DoSave(savePath.c_str(), TRUE) != FALSE;
}

// world.GetMainWindowPosition(): screen rect of the MDI parent frame.
pal::Rect CWorldChildFrame::mainRect() const
{
    wxMDIParentFrame* parent = wxDynamicCast(GetParent(), wxMDIParentFrame);
    if (parent) {
        wxRect r = parent->GetScreenRect();
        return { r.x, r.y, r.width, r.height };
    }
    return { 0, 0, 1024, 768 };
}

// Re-run the miniwindow mouse-move hit test at a point the pane is not
// reporting right now — WindowAddHotspot's "the pointer is already over the
// hotspot I just created" case.  The engine site used to walk the doc's view
// list for a CMUSHView, which the port never constructs, so the immediate
// MouseOver simply never fired here; it now comes through this seam, and the
// Windows adapter runs that same view walk (pal/win_worldview.cpp).
void CWorldChildFrame::mouseMoveMiniWindow(long x, long y)
{
    if (!m_doc || !m_output) return;
    WxMouseHooks hooks(m_output);
    MouseMoveMiniWindow(m_doc, CPoint((int) x, (int) y), hooks);
}

// ── the info bar (P2 infobar family) ─────────────────────────────────────────
// On Windows these drive a CRichEditCtrl on the main frame; here the world
// child frame hosts a styled wxTextCtrl.  world.Info appends text in the
// current style; InfoColour/InfoFont update that style; InfoBackground
// recolours the strip.

void CWorldChildFrame::infobarShow(bool visible)
{
    if (m_infobar->IsShown() != visible) { m_infobar->Show(visible); Layout(); }
}

void CWorldChildFrame::infobarClear()
{
    m_infobar->Clear();
    m_infobarStyle = wxTextAttr(*wxBLACK);
    m_infobarStyle.SetFont(m_infobar->GetFont());
    m_infobar->SetBackgroundColour(wxColour(255, 255, 255));
}

void CWorldChildFrame::infobarText(const char* text)
{
    long start = m_infobar->GetLastPosition();
    m_infobar->AppendText(wxString::FromUTF8(text));
    m_infobar->SetStyle(start, m_infobar->GetLastPosition(), m_infobarStyle);
}

void CWorldChildFrame::infobarColour(pal::Colour rgb)
{
    m_infobarStyle.SetTextColour(
        wxColour(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
}

void CWorldChildFrame::infobarBackground(pal::Colour rgb)
{
    m_infobar->SetBackgroundColour(
        wxColour(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
    m_infobar->Refresh();
}

void CWorldChildFrame::infobarFont(const char* face, int pointSize, int style)
{
    wxFont f = m_infobarStyle.HasFont() ? m_infobarStyle.GetFont()
                                        : m_infobar->GetFont();
    if (!!*face) f.SetFaceName(wxString::FromUTF8(face));
    if (pointSize > 0) f.SetPointSize(pointSize);
    f.SetWeight   ((style & 1) ? wxFONTWEIGHT_BOLD  : wxFONTWEIGHT_NORMAL);
    f.SetStyle    ((style & 2) ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
    f.SetUnderlined((style & 4) != 0);
    m_infobarStyle.SetFont(f);
    m_infobarStyle.SetFontUnderlined  ((style & 4) != 0);
    m_infobarStyle.SetFontStrikethrough((style & 8) != 0);
}

// world.GetInfo(257/258): live InfoBar size (0,0 while hidden).
pal::Size CWorldChildFrame::infobarSize() const
{
    if (m_infobar && m_infobar->IsShown()) {
        wxSize s = m_infobar->GetSize();
        return { s.x, s.y };
    }
    return { 0, 0 };
}

// ── OnClose ───────────────────────────────────────────────────────────────
void CWorldChildFrame::OnClose(wxCloseEvent& evt)
{
    // ── Unsaved-changes prompt ────────────────────────────────────────────
    // If the document has been modified (triggers, aliases, properties changed)
    // since it was last saved, offer to save before closing.
    // Only when the close event can be vetoed (not a forced app shutdown).
    // "Save world automatically on close" (world prefs ▸ General).  Mirrors
    // CMUSHclientDoc::SaveModified (doc.cpp:2121-2122): save first, silently,
    // which then leaves the document unmodified so the prompt below does not
    // fire.  The port had no consumer for the option at all, so ticking it did
    // nothing and the user was still asked on every close.
    if (m_doc && m_doc->m_bSaveWorldAutomatically && m_doc->IsModified() &&
        g_wxhook_save_world)
    {
        g_wxhook_save_world(m_doc, /*saveAs=*/false, this);
        // Deliberately NOT an early return: Windows falls through to the
        // variables check below, and if the save failed the document is still
        // modified so the normal prompt still protects the user's work.
    }

    // "Offer to save world if only Variables have changed" (Global Prefs ▸
    // Closing, default ON).  This ADDS a prompt rather than suppressing one:
    // changing a script variable does not mark the document modified, so
    // without this a variables-only change closes silently.  Windows asks when
    // the option is on (doc.cpp:2125-2127); the port ignored it entirely.
    if (evt.CanVeto() && m_doc && !m_doc->IsModified() &&
        m_doc->m_bVariablesChanged && App.m_bConfirmBeforeSavingVariables)
    {
        wxString vname = wxString::FromUTF8(
            static_cast<const char*>(m_doc->m_mush_name));
        if (vname.IsEmpty()) vname = wxT("this world");
        int vans = wxMessageBox(
            wxT("Variables have changed in ") + vname +
                wxT(".\n\nSave the world file before closing?"),
            wxT("Variables Changed"),
            wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_QUESTION, this);
        if (vans == wxCANCEL) { evt.Veto(); return; }
        if (vans == wxYES && g_wxhook_save_world) {
            if (!g_wxhook_save_world(m_doc, /*saveAs=*/false, this)) {
                evt.Veto();
                return;
            }
        }
    }

    if (evt.CanVeto() && m_doc && m_doc->IsModified()) {
        wxString name = wxString::FromUTF8(
            static_cast<const char*>(m_doc->m_mush_name));
        if (name.IsEmpty()) name = wxT("this world");

        int answer = wxMessageBox(
            wxT("Save changes to ") + name + wxT(" before closing?"),
            wxT("Unsaved Changes"),
            wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_QUESTION, this);

        if (answer == wxCANCEL) {
            evt.Veto();
            return;
        }
        if (answer == wxYES) {
            // Attempt to save.  If the user cancels the save-as dialog (no path
            // yet) or the save fails, veto the close so no work is lost.
            if (g_wxhook_save_world) {
                bool saved = g_wxhook_save_world(m_doc, /*saveAs=*/false, this);
                if (!saved) {
                    evt.Veto();
                    return;
                }
            }
        }
        // answer == wxNO → discard changes and fall through to close.
    }

    // ── Close confirmation ────────────────────────────────────────────────
    // General "are you sure?" prompt (separate from the save prompt above).
    // Skipped when we already asked about saving — no need to ask twice.
    else if (evt.CanVeto() && App.m_bConfirmBeforeClosingWorld && m_doc &&
             !m_doc->IsModified()) {
        wxString name = wxString::FromUTF8(
            static_cast<const char*>(m_doc->m_mush_name));
        if (name.IsEmpty()) name = wxT("this world");
        int answer = wxMessageBox(
            wxT("Close ") + name + wxT("?"),
            wxT("Confirm Close"),
            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
        if (answer != wxYES) {
            evt.Veto();
            return;
        }
    }

    // ── Save child frame size for next session ────────────────────────────
    // Persist the current window dimensions so the next world that opens uses
    // the same size instead of reverting to the screen-derived default.  The
    // quit hook writes the same keys through the same helper, because a Cmd-Q
    // never reaches this handler at all.
    {
        wxConfig cfg(wxT("mushclient"));
        wx_save_child_geometry(cfg, GetSize());
    }

    // ── Save persistent command history ───────────────────────────────────
    // Write each command on its own line to the same per-world file determined
    // at construction time, so the path is consistent regardless of later
    // World Properties renames.
    if (!m_history.empty()) {
        std::string histPath = m_historyFilePath;
        if (!histPath.empty()) {
            if (FILE* f = fopen(histPath.c_str(), "w")) {
                for (const wxString& cmd : m_history)
                    fprintf(f, "%s\n", cmd.ToUTF8().data());
                fclose(f);
            }
        }
    }

    // Close any open log file before the doc is destroyed.
    if (IsLogging()) StopLogging();

    // Stop the world timer before we touch the doc — prevents a timer callback
    // from firing between m_pWorldView=nullptr and delete doc.  (The value
    // member also stops itself when the frame is destroyed, covering the Cmd+Q
    // teardown path where OnClose never runs.)
    m_worldTimer.Stop();

    if (m_doc) {
        // Capture the world file path before clearing the doc, so we can
        // remove it from the startup-reopen list below.
        std::string worldPath = static_cast<const char*>(m_doc->GetPathName());

        // Run the world-close script teardown while the pane is STILL attached,
        // so anything these handlers print can reach the screen.  Closing a
        // single world previously skipped all of this — only Cmd+Q did it (see
        // the identical block in platform/wx_mfc_impl.cpp hook_query_quit_save)
        // — so Cmd+W and Cmd+Q behaved differently and, because Quow's plugins
        // persist their miniwindow layout from OnPluginClose, closing one world
        // silently threw that layout away.
        //
        // Deliberately NOT routed through SaveModified(): doc.cpp:2098-2144
        // would re-ask the "this will end your session" and variables-changed
        // prompts that OnClose already asked above, double-prompting the user.
        m_doc->FireWorldClose();
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_CLOSE);
        for (PluginListIterator pit = m_doc->m_PluginList.begin();
             pit != m_doc->m_PluginList.end(); ++pit)
            if (*pit) (*pit)->SaveState();

        // Detach the view BEFORE deleting the doc so that any final output
        // flushed by the destructor doesn't call AppendLine on the dying pane
        // (m_pWorldView is this frame; every Emit*/pull helper no-ops on null).
        m_doc->m_pWorldView = nullptr;

        // Deregister from the world doc-template so GetWorldList() no longer
        // returns this world after it is destroyed.
        if (App.m_pWorldDocTemplate)
            App.m_pWorldDocTemplate->RemoveDocument(m_doc);

        // Remove from the doc→frame registry before deleting so that no hook
        // can look up a dangling frame pointer during the destructor.
        wx_unregister_world_frame(m_doc);

        // Closing a world deliberately does NOT remove it from
        // App.m_strWorldList.  That list is the user's curated "open these at
        // startup" set (its only writer on Windows is the Global Preferences
        // page); treating a close as a request to edit it meant the set was
        // whatever happened to be open when you last quit.

        // Disconnect and release the doc.  Deletes socket, triggers, timers, etc.
        CMUSHclientDoc* doc = m_doc;
        m_doc = nullptr;    // prevent re-entrancy
        delete doc;
    }
    evt.Skip();  // let wxWidgets destroy the frame and its child widgets
}

// ── OnSize ────────────────────────────────────────────────────────────────────
// Called whenever the frame (and therefore the output pane) is resized.
// Propagates the new character-grid dimensions to the MUD via a NAWS packet.
//
// We must defer the NAWS send via CallAfter: evt.Skip() marks the event for
// further processing but the base-class OnSize (which calls Layout() to resize
// child windows) runs AFTER our handler returns.  Reading the pane's client
// size before returning gives stale dimensions.  CallAfter executes on the
// next event-loop iteration, after Layout() has already updated child sizes.
void CWorldChildFrame::OnSize(wxSizeEvent& evt)
{
    evt.Skip();  // let wxMDIChildFrame / sizer resize children

    CallAfter([this]() {
        if (!m_doc || !m_output) return;

        int charW = m_output->CharWidth();
        int lineH = m_output->LineHeight();
        if (charW <= 0 || lineH <= 0) return;

        int paneW, paneH;
        m_output->GetClientSize(&paneW, &paneH);
        if (paneW <= 0 || paneH <= 0) return;

        int cols = paneW / charW;
        int rows = paneH / lineH;
        // Keep the GUI geometry fields current — GetInfo(280/281) report the
        // output pane's pixel height/width from these (rows*lineH, cols*charW), and
        // plugins (e.g. Quow's screen margins) compute their bottom/right margins
        // from them.  The font-change path sets these; without doing it here as
        // well they'd keep the *startup* height after the pane later shrinks (when
        // the input bar takes its space), so GetInfo(280) over-reported and bottom
        // margins landed too low.
        m_doc->m_gui_cols        = cols;
        m_doc->m_gui_rows        = rows;
        m_doc->m_gui_font_height = lineH;
        m_doc->m_gui_font_width  = charW;
        m_doc->m_FontHeight      = lineH;
        m_doc->m_FontWidth       = charW;
        m_doc->m_InputFontHeight = lineH;
        m_doc->m_InputFontWidth  = charW;
        ApplyWrapColumnAndNAWS();
        // Fire OnPluginWorldOutputResized — mirrors mushview.cpp CMUSHView::OnSize().
        m_doc->SendToAllPluginCallbacks(ON_PLUGIN_WORLD_OUTPUT_RESIZED);
    });
}

#endif // !_WIN32
