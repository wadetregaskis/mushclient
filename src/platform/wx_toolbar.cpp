// wx_toolbar.cpp — the three canonical MFC toolbars over wxToolBar.
// See wx_toolbar.h for why they live on the world window rather than the frame.

// wx headers MUST precede the MFC-compat headers: stdafx.h (compat.h) defines
// Windows-style macros (DrawText, GetObject, …) that otherwise clobber wx's own
// headers.  Same ordering as wx_activity.cpp / wx_world_child.cpp.
#include <wx/wx.h>
#include <wx/toolbar.h>
#include <wx/aui/framemanager.h>
#include <wx/aui/dockart.h>     // wxAuiDockArt metrics: a pane's width
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/config.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "resource.h"
#include "platform/app_config.h"   // OpenAppConfig -- never the raw wxConfig
#include "platform/menu_ui_rules.h" // kPortMenuIdDisconnect -- one owner of the id
#include <algorithm>
#include <climits>
#include <memory>
#include "platform/wx_toolbar.h"

// Defined in wx_mfc_impl.cpp (see the same forward declaration in
// wx_activity.cpp): raise + activate a world's frame.  Declared rather than
// included because wx_world_child.h's wx includes would be clobbered by
// compat.h's macros in this translation unit.
void wx_activate_world_frame(CMUSHclientDoc* doc);
CMUSHclientDoc* wx_doc_from_frame(wxFrame* frame);

// Also wx_mfc_impl.cpp, and declared here for the same reason: the one answer
// to "may the user choose this command right now", layered canonical table ->
// menu_ui_rules -> the document's own ON_UPDATE_COMMAND_UI.  The menu bar and
// the output pane's context menu both ask it; the toolbar used to ask the
// DOCUMENT alone, which is a strictly smaller question (see UpdateState).
bool wx_canonical_menu_ui_state(int id, CMUSHclientDoc* doc,
                                bool& enabled, int& checked);

// The activity bar's six interchangeable strips, by style (a global
// preference): mainfrm.cpp's table, the one Windows loads them by.
extern int ActivityToolBarResourceNames [6];

// MFC/AFX standard command ids.  These live in afxres.h, not resource.h, and the
// port already routes them by number elsewhere (wx_mfc_impl.cpp) — same values.
#define MC_ID_FILE_NEW      0xE100
#define MC_ID_FILE_OPEN     0xE101
#define MC_ID_FILE_SAVE     0xE103

namespace {

// ── Button tables ─────────────────────────────────────────────────────────
// Layout and order come verbatim from the TOOLBAR resources in MUSHclient.rc
// (nickgammon/mushclient_resources): IDR_MAINFRAME, IDR_GAME_TOOLBAR and
// IDR_ACTIVITY_TOOLBAR.  Order matters twice over — it is both the on-screen
// order and the index into the bitmap strip, exactly as MFC treats it.
//
// `id == 0` is a SEPARATOR: it takes a slot on screen but NOT an image index,
// which is why the tile counter only advances for real buttons.
//
// A canonical command id is not always what this port dispatches — the wx build
// routes clipboard/About through wx's standard ids, opens each world-prefs page
// by enum rather than by its own command id, and has one Connect/Disconnect pair
// where Windows has a single toggle.  `act`/`arg` say what a click should
// actually do, so the table keeps the canonical layout without pretending the
// canonical ids are all live here.

enum Act {
    eFwd,          // arg = command id to hand to the parent frame's handler
    ePrefsPage,    // arg = world-prefs page enum (doc.h) to open
    eConnectTgl,   // Windows' single toggle; the port has separate commands
    eWorldSwitch,  // arg = world slot 1..10
    eUnsupported,  // no equivalent on this platform — button is greyed
};

struct TbButton { int id; Act act; int arg; bool check; const char* tip; };

const TbButton kStandard[] = {
    { MC_ID_FILE_NEW,       eFwd, MC_ID_FILE_NEW,  false, "New world" },
    { 0,                    eFwd, 0,               false, nullptr },
    { MC_ID_FILE_OPEN,      eFwd, MC_ID_FILE_OPEN, false, "Open world" },
    { MC_ID_FILE_SAVE,      eFwd, MC_ID_FILE_SAVE, false, "Save world" },
    { ID_FILE_PRINT_WORLD,  eFwd, ID_FILE_PRINT_WORLD, false, "Print" },
    { 0,                    eFwd, 0,               false, nullptr },
    // Canonically ID_EDIT_NOTESWORKAREA; the port's standalone-notepad command.
    { ID_EDIT_NOTESWORKAREA, eFwd, 40001,          false, "Notepad" },
    { 0,                    eFwd, 0,               false, nullptr },
    { 0xE401 /*ID_EDIT_CUT*/,   eFwd, wxID_CUT,    false, "Cut" },
    // Copy is deliberately NOT wxID_COPY — see the Edit menu in wx_mfc_impl.cpp.
    { 0xE402 /*ID_EDIT_COPY*/,  eFwd, ID_EDIT_COPY, false, "Copy" },
    { 0xE403 /*ID_EDIT_PASTE*/, eFwd, wxID_PASTE,  false, "Paste" },
    { 0,                    eFwd, 0,               false, nullptr },
    { 0xE140 /*ID_APP_ABOUT*/, eFwd, wxID_ABOUT,   false, "About" },
    // ID_CONTEXT_HELP is Windows' "What's This?" cursor — no macOS equivalent,
    // and nothing in the port implements it.  Kept so the strip stays aligned,
    // greyed so it does not look like a command that silently fails.
    { 0xE146 /*ID_CONTEXT_HELP*/, eUnsupported, 0, false, "Context help (Windows only)" },
};

const TbButton kWorld[] = {
    { ID_GAME_WRAPLINES,     eFwd, ID_GAME_WRAPLINES,  true,  "Wrap output" },
    // A CHECK item: CMUSHclientDoc::OnUpdateFileLogsession (doc.cpp:1008) is
    // SetCheck(m_logfile != NULL), and MFC's CToolCmdUI::SetCheck presses any
    // toolbar button, so Windows needs no declaration.  wx does.
    { ID_FILE_LOGSESSION,    eFwd, ID_FILE_LOGSESSION, true,  "Log session" },
    { 0,                     eFwd, 0,                  false, nullptr },
    { ID_CONNECT_DISCONNECT, eConnectTgl, 0,           false, "Connect / disconnect" },
    { 0,                     eFwd, 0,                  false, nullptr },
    { ID_GAME_PREFERENCES,   eFwd, ID_GAME_PREFERENCES, false, "World preferences" },
    { ID_GAME_CONFIGURE_TRIGGERS,  ePrefsPage, ePageTriggers,  false, "Triggers" },
    { ID_GAME_CONFIGURE_ALIASES,   ePrefsPage, ePageAliases,   false, "Aliases" },
    { ID_GAME_CONFIGURE_TIMERS,    ePrefsPage, ePageTimers,    false, "Timers" },
    { ID_GAME_CONFIGURE_OUTPUT,    ePrefsPage, ePageOutput,    false, "Output" },
    { ID_GAME_CONFIGURE_COMMANDS,  ePrefsPage, ePageCommands,  false, "Commands" },
    { ID_GAME_CONFIGURE_SCRIPTING, ePrefsPage, ePageScripts,   false, "Scripting" },
    { ID_GAME_CONFIGURE_NOTES,     ePrefsPage, ePageNotes,     false, "Notes" },
    { ID_GAME_CONFIGURE_VARIABLES, ePrefsPage, ePageVariables, false, "Variables" },
    { 0,                     eFwd, 0,                  false, nullptr },
    { ID_GAME_RESETALLTIMERS,     eFwd, ID_GAME_RESETALLTIMERS,     false, "Reset all timers" },
    { ID_GAME_RELOAD_SCRIPT_FILE, eFwd, ID_GAME_RELOAD_SCRIPT_FILE, false, "Reload script file" },
    { 0,                     eFwd, 0,                  false, nullptr },
    { ID_GAME_AUTOSAY,       eFwd, ID_GAME_AUTOSAY,    true,  "Auto say" },
    { ID_DISPLAY_FREEZEOUTPUT, eFwd, ID_DISPLAY_FREEZEOUTPUT, true, "Freeze output" },
    { 0,                     eFwd, 0,                  false, nullptr },
    { ID_DISPLAY_FIND,       eFwd, ID_DISPLAY_FIND,    false, "Find" },
    // The port has a single "Find Again" (forwards); searching backwards has no
    // command of its own here yet.
    { ID_FIND_AGAIN_FORWARDS,  eFwd, ID_DISPLAY_FINDAGAIN, false, "Find again" },
    { ID_FIND_AGAIN_BACKWARDS, eUnsupported, 0,        false, "Find again backwards (not wired yet)" },
};

// Ten world-switch buttons.  The canonical order runs 1..9 then 0 — the button
// labelled 0 is world *10*, which is why OnUpdateBtnWorldsWorld0 passes 10.
// Keep this order: it indexes the bitmap strip.
const TbButton kActivity[] = {
    { ID_BTN_WORLDS_WORLD1, eWorldSwitch,  1, true, "World 1" },
    { ID_BTN_WORLDS_WORLD2, eWorldSwitch,  2, true, "World 2" },
    { ID_BTN_WORLDS_WORLD3, eWorldSwitch,  3, true, "World 3" },
    { ID_BTN_WORLDS_WORLD4, eWorldSwitch,  4, true, "World 4" },
    { ID_BTN_WORLDS_WORLD5, eWorldSwitch,  5, true, "World 5" },
    { ID_BTN_WORLDS_WORLD6, eWorldSwitch,  6, true, "World 6" },
    { ID_BTN_WORLDS_WORLD7, eWorldSwitch,  7, true, "World 7" },
    { ID_BTN_WORLDS_WORLD8, eWorldSwitch,  8, true, "World 8" },
    { ID_BTN_WORLDS_WORLD9, eWorldSwitch,  9, true, "World 9" },
    { ID_BTN_WORLDS_WORLD0, eWorldSwitch, 10, true, "World 10" },
};

struct BarSpec { const TbButton* buttons; size_t count; int art; };   // art: a BITMAP id

const BarSpec& Spec (int which)
{
    static const BarSpec specs[] = {
        { kStandard, WXSIZEOF(kStandard), IDR_MAINFRAME    },
        { kWorld,    WXSIZEOF(kWorld),    IDR_GAME_TOOLBAR },
        { kActivity, WXSIZEOF(kActivity), 0 /*chosen by style: ActivityStrip*/ },
    };
    return specs[which];
}

const TbButton* FindButton (int id)
{
    for (int w = 0; w < CWorldToolBars::eBarCount; ++w) {
        const BarSpec& s = Spec(w);
        for (size_t i = 0; i < s.count; ++i)
            if (s.buttons[i].id == id && id != 0)
                return &s.buttons[i];
    }
    return nullptr;
}

} // namespace

// The world occupying activity slot `slot` (1..10), or null.  Numbers any
// unnumbered world on the way, as CMainFrame::OnUpdateBtnWorlds does.
// Not file-static: the Window-menu / startup ID_WORLDS_WORLD* commands in
// wx_mfc_impl.cpp pick their world the same way, and a second copy of this
// walk is how the two would disagree about which world is "world 1".
CMUSHclientDoc* wx_world_in_slot (int slot)
{
    int nItem = 1;
    for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
        CMUSHclientDoc* pDoc =
            (CMUSHclientDoc*) App.m_pWorldDocTemplate->GetNextDoc(pos);
        if (pDoc->m_view_number == 0)
            pDoc->m_view_number = nItem;
        if (pDoc->m_view_number == slot)
            return pDoc;
        nItem++;
    }
    return nullptr;
}

namespace {

// ── Art ───────────────────────────────────────────────────────────────────

// A strip is a BITMAP resource of resources/en.rc, which the build converts
// into art/bitmap/<id>.bmp (tools/port_resources.py).  Same two-candidate
// search the dialog bridge uses (wx_dialog_bridge.cpp): the bundle's
// Resources dir, then alongside the executable.
wxString FindArt (int bitmapId)
{
    const wxString rel = wxString::Format(wxT("art%cbitmap%c%d.bmp"),
                                          wxFILE_SEP_PATH, wxFILE_SEP_PATH, bitmapId);
    const wxString exeDir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    wxString candidates[] = {
        wxStandardPaths::Get().GetResourcesDir() + wxFILE_SEP_PATH + rel,
        exeDir + wxFILE_SEP_PATH + rel,
    };
    for (const wxString& c : candidates)
        if (wxFileName::FileExists(c))
            return c;
    return wxString();
}

// The strips are 4-bit Windows bitmaps on the standard 16-colour VGA palette;
// MFC treats palette index 7 (#C0C0C0, light grey) as the background and
// repaints it in the button-face colour.  A mask is the closer equivalent here:
// it composites correctly whatever the platform draws behind the bar, and it
// survives the dark-mode toolbar background.
const int kTileW = 16, kTileH = 15;
const int kKeyR  = 192, kKeyG = 192, kKeyB = 192;

wxImage LoadStrip (int bitmapId)
{
    const wxString path = FindArt(bitmapId);
    if (path.empty()) return wxImage();
    wxImage img;
    wxLogNull noLog;    // a bad/missing strip degrades to "no bar", not a dialog
    if (!img.LoadFile(path)) return wxImage();
    return img;
}

// The activity strip for a style; styles are clamped to 0..5 by the callers,
// as mainfrm.cpp OnCreate clamps them.
int ActivityStrip (unsigned style)
{
    return ActivityToolBarResourceNames[style < 6 ? style : 0];
}

// Cut tile `index` out of a strip and return it as a toolbar-ready bitmap.
//
// These are 16x15 assets drawn for a 96 dpi 1x display.  Handing wx the raw
// 16x15 image makes the platform scale it up with interpolation on a Retina
// screen, which turns 1-pixel details into mush.  Point-doubling here and
// declaring the result as a 2x bitmap keeps the original pixels crisp instead.
// It still looks like 1990s toolbar art — that is a question about the source
// artwork, recorded in packaging/README.md, not something scaling can fix.
wxBitmap TileBitmap (const wxImage& strip, int index)
{
    if (!strip.IsOk()) return wxBitmap();
    const int x = index * kTileW;
    if (x + kTileW > strip.GetWidth()) return wxBitmap();

    wxImage tile = strip.GetSubImage(wxRect(x, 0, kTileW, kTileH));

#if wxCHECK_VERSION(3, 1, 0)
    tile = tile.Scale(kTileW * 2, kTileH * 2, wxIMAGE_QUALITY_NEAREST);
    tile.SetMaskColour(kKeyR, kKeyG, kKeyB);
    return wxBitmap(tile, -1 /*depth*/, 2.0 /*scale*/);
#else
    tile.SetMaskColour(kKeyR, kKeyG, kKeyB);
    return wxBitmap(tile);
#endif
}

// Windows persists toolbar state in the registry via CFrameWnd::SaveBarState;
// there is no equivalent here, so keep it simple: one flag per bar, and one
// row/order pair per bar (see wx_toolbar.h for why not a perspective string).
wxString VisKey   (int which) { return wxString::Format(wxT("/toolbars/visible%d"), which); }
wxString RowKey   (int which) { return wxString::Format(wxT("/toolbars/row%d"),     which); }
wxString OrderKey (int which) { return wxString::Format(wxT("/toolbars/order%d"),   which); }

// wxAuiManager matches panes by name when it restores them, so these are part
// of the persisted contract even though the perspective string is not used.
const char* const kPaneName [CWorldToolBars::eBarCount] =
    { "standard", "world", "activity" };

// The bar's own name, shown in the drag hint while a bar is in flight.  Windows
// takes these from the control bars' window text (mainfrm.cpp:320-350).
const char* const kBarLabel [CWorldToolBars::eBarCount] =
    { "Standard", "World", "Activity" };

// ── The shared arrangement ────────────────────────────────────────────────
// One record for the whole app, not one per world window: see wx_toolbar.h.
// A world opened later starts where the ones already open are.
CWorldToolBars::Placement s_place [CWorldToolBars::eBarCount];
bool s_placeLoaded = false;
// True while one strip is pushing its arrangement to the others, so their own
// change detection does not turn round and push it back.
bool s_publishing  = false;

// Function-local so there is no static-initialisation order to get wrong: a
// CWorldToolBars can be constructed from a selftest before main() would have
// run a namespace-scope constructor here.
std::set<CWorldToolBars*>& Instances ()
{
    static std::set<CWorldToolBars*> s;
    return s;
}

// Bumped when the meaning of the stored arrangement changes.  2: the default
// became all three on one row, wrapped to the window (see wx_toolbar.h).
const wxChar* const kLayoutKey     = wxT("/toolbars/layout");
const int           kLayoutVersion = 2;

// The arrangement nobody has rearranged: all three on one row, in declaration
// order, which the wrap turns into as many side by side as the window takes.
void DefaultPlacements (CWorldToolBars::Placement place[CWorldToolBars::eBarCount])
{
    for (int i = 0; i < CWorldToolBars::eBarCount; ++i) {
        place[i].row   = 0;
        place[i].order = i;
    }
}

// Rows compacted from 0 and orders restarted within each row, ties broken by
// bar index -- one spelling for each picture, so that comparing two
// arrangements compares what they look like.
void Canonicalise (CWorldToolBars::Placement place[CWorldToolBars::eBarCount])
{
    const int n = CWorldToolBars::eBarCount;
    int idx[CWorldToolBars::eBarCount];
    for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx, idx + n, [place](int a, int b) {
        if (place[a].row   != place[b].row)   return place[a].row   < place[b].row;
        if (place[a].order != place[b].order) return place[a].order < place[b].order;
        return a < b;
    });
    int outRow = -1, outOrder = 0, prevRow = INT_MIN;
    CWorldToolBars::Placement out[CWorldToolBars::eBarCount];
    for (int k = 0; k < n; ++k) {
        const int i = idx[k];
        if (place[i].row != prevRow) { prevRow = place[i].row; ++outRow; outOrder = 0; }
        out[i].row   = outRow;
        out[i].order = outOrder++;
    }
    for (int i = 0; i < n; ++i) place[i] = out[i];
}

bool SamePlacements (const CWorldToolBars::Placement a[CWorldToolBars::eBarCount],
                     const CWorldToolBars::Placement b[CWorldToolBars::eBarCount])
{
    for (int i = 0; i < CWorldToolBars::eBarCount; ++i)
        if (a[i].row != b[i].row || a[i].order != b[i].order)
            return false;
    return true;
}

void LoadPlacements ()
{
    if (s_placeLoaded) return;
    s_placeLoaded = true;

    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    CWorldToolBars::Placement def[CWorldToolBars::eBarCount];
    DefaultPlacements(def);
    bool oldDefault = true;     // exactly what layout 1 started with
    for (int i = 0; i < CWorldToolBars::eBarCount; ++i) {
        cfg.Read(RowKey(i),   &s_place[i].row,   def[i].row);
        cfg.Read(OrderKey(i), &s_place[i].order, def[i].order);
        // A config file can be hand-edited or written by a future version with
        // more bars; a row number outside the strip would put a bar somewhere
        // no drag could reach it from.
        if (s_place[i].row   < 0 || s_place[i].row   >= CWorldToolBars::eBarCount)
            s_place[i].row = def[i].row;
        if (s_place[i].order < 0 || s_place[i].order >= CWorldToolBars::eBarCount)
            s_place[i].order = def[i].order;
        if (s_place[i].row != i || s_place[i].order != 0)
            oldDefault = false;
    }
    // Layout 1's default was one bar per row, and View > Reset Toolbar Locations
    // saved it, so a file holding exactly that is far more likely to be the old
    // default than a choice; it becomes the new one.  Once only: a layout-2
    // file holding it is somebody's choice and is kept.
    if (cfg.ReadLong(kLayoutKey, 1) < kLayoutVersion && oldDefault)
        DefaultPlacements(s_place);
    Canonicalise(s_place);
}

void SavePlacements ()
{
    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    for (int i = 0; i < CWorldToolBars::eBarCount; ++i) {
        cfg.Write(RowKey(i),   s_place[i].row);
        cfg.Write(OrderKey(i), s_place[i].order);
    }
    cfg.Write(kLayoutKey, kLayoutVersion);
    cfg.Flush();
}

} // namespace

// ── CWorldToolBars ────────────────────────────────────────────────────────

CWorldToolBars::CWorldToolBars (wxFrame* frame, wxBoxSizer* sizer)
    : m_frame(frame), m_sizer(sizer)
{
    for (int i = 0; i < eBarCount; ++i) {
        m_bars[i]    = nullptr;
        m_visible[i] = true;
    }
    for (int i = 0; i < 10; ++i)
        m_activityImage[i] = -1;

    {
        const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
        for (int i = 0; i < eBarCount; ++i)
            cfg.Read(VisKey(i), &m_visible[i], true);
    }

    LoadPlacements();

    // The strip, at the very top of the frame's vertical sizer and above the
    // output pane.  It is added with proportion 0 and NO min size of its own:
    // wxAuiManager gives the window it manages a real wxSizer and calls
    // SetSizer on every Update, which invalidates the best-size cache, so the
    // strip's GetBestSize() IS the current AUI layout's height and the frame's
    // sizer picks it up for free.  Measured on this wx (3.3.2): 82 px with the
    // three bars on separate rows, 55 px with two of them sharing one.
    m_host = new wxPanel(m_frame, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                         wxBORDER_NONE);
    m_sizer->Insert(0, m_host, 0, wxEXPAND);

    // No wxAUI_MGR_ALLOW_FLOATING — floating is out, and leaving the flag on
    // would let a drag that misses every dock tear a bar off into a window of
    // its own.  Floatable(false) on each pane says the same thing; both, so
    // that neither one alone is load-bearing.
    m_mgr = new wxAuiManager(m_host,
                             wxAUI_MGR_TRANSPARENT_HINT | wxAUI_MGR_HINT_FADE);

    for (int i = 0; i < eBarCount; ++i) {
        m_bars[i] = Build(static_cast<Which>(i));
        if (!m_bars[i]) continue;          // no art -> no bar, as before
        m_mgr->AddPane(m_bars[i],
            wxAuiPaneInfo()
                .Name    (wxString::FromUTF8(kPaneName[i]))
                .Caption (wxString::FromUTF8(kBarLabel[i]))
                .ToolbarPane().Top()
                .Row(s_place[i].row).Position(s_place[i].order)
                .LeftDockable(false).RightDockable(false).BottomDockable(false)
                .Floatable(false)
                .Show(m_visible[i]));
    }
    // Wrapped to whatever width there is yet: a world window has not laid its
    // strip out at this point, so this is the frame's width, and the size event
    // below re-wraps once the strip has a width of its own.
    //
    // The frame has no sizer yet when a world window builds its bars (it calls
    // SetSizer after us), so the Layout inside is a no-op there and the frame's
    // own first layout does the job.  In a selftest, where the sizer is already
    // attached, it is what gives the strip its height before anything is asked
    // about it.
    ApplyPlacements(s_place, true);

    for (int i = 0; i < eBarCount; ++i) {
        const BarSpec& spec = Spec(i);
        for (size_t b = 0; b < spec.count; ++b)
            if (spec.buttons[b].id != 0)
                m_ourIds.insert(spec.buttons[b].id);
    }

    // wxAuiManager fires this from its own paint, so it is the first moment
    // after a drop at which the new arrangement is readable.  The work is
    // DEFERRED because re-laying the frame out from inside a paint would
    // re-enter it, and a deferred call needs the alive token: nothing here can
    // promise this object outlives a call already sitting in the queue.
    m_alive = std::make_shared<bool>(true);
    m_host->Bind(wxEVT_AUI_RENDER, [this](wxAuiManagerEvent& evt) {
        evt.Skip();
        if (m_pollPending) return;
        m_pollPending = true;
        std::shared_ptr<bool> alive = m_alive;
        m_host->CallAfter([this, alive] {
            if (!*alive) return;
            m_pollPending = false;
            PollLayout();
        });
    });
    // A wider or narrower window re-wraps the bars.  On the FRAME, because
    // wxAuiManager handles the strip's own size event without passing it on;
    // and deferred, because the frame's handler -- which sizes the strip -- runs
    // after this one.  Bound on the frame, so the alive token as well.
    m_frame->Bind(wxEVT_SIZE, [this, alive = m_alive](wxSizeEvent& evt) {
        evt.Skip();
        if (!*alive || m_reflowPending) return;
        m_reflowPending = true;
        m_frame->CallAfter([this, alive] {
            if (!*alive) return;
            m_reflowPending = false;
            Reflow();
        });
    });

    // The alive token again, for a different reason: this handler is bound on
    // the FRAME, which outlives this object, and ~CWorldToolBars cannot unbind a
    // lambda it kept no handle to.  Without the guard the only safe thing to do
    // with a CWorldToolBars was to leak it — and it can no longer be leaked,
    // because its wxAuiManager has to be UnInit'd before the strip it manages is
    // destroyed.
    {
        std::shared_ptr<bool> alive = m_alive;
        m_frame->Bind(wxEVT_TOOL, [this, alive](wxCommandEvent& evt) {
            if (!*alive) { evt.Skip(); return; }
            if (m_ourIds.find(evt.GetId()) == m_ourIds.end()) { evt.Skip(); return; }

            // Re-entrancy guard.  wxMDIParentFrameBase::TryBefore offers the
            // parent frame's events to the ACTIVE CHILD first — which is this
            // frame — so an event handed upwards comes straight back here and,
            // without this flag, recurses until the stack dies.  On the second
            // pass, skip: the event then carries on to the parent's own
            // handler, where the canonical command dispatch lives.
            if (m_forwarding) { evt.Skip(); return; }

            DispatchId(evt.GetId());
        });
    }

    // Last, once everything above is in place: another strip may publish an
    // arrangement to this one the moment it is on the list.
    Instances().insert(this);
}

CWorldToolBars::~CWorldToolBars ()
{
    if (m_alive) *m_alive = false;
    Instances().erase(this);
    if (m_mgr) {
        // Before the host goes: UnInit pops the manager off the host's event
        // handler chain, and the host is destroyed by the frame afterwards
        // (members die before the wxWindow base destroys its children).
        m_mgr->UnInit();
        delete m_mgr;
        m_mgr = nullptr;
    }
}

void CWorldToolBars::DispatchId (int id)
{
    const TbButton* pb = FindButton(id);
    if (!pb) return;
    const TbButton& b = *pb;
    CMUSHclientDoc* doc = ActiveDoc();

    switch (b.act) {
    case eUnsupported:
        return;

    case ePrefsPage:
        if (doc) doc->GamePreferences(b.arg);
        return;

    case eConnectTgl:
        // Windows has one toggle button (ID_CONNECT_DISCONNECT); the port has
        // separate Connect / Disconnect commands, so pick by current state.
        // The Disconnect id is menu_ui_rules.h's constant, not a copy of its
        // value: this pair has come adrift before (see menu_ui_rules.cpp), and
        // a second literal is exactly how it does so.
        if (doc)
            Forward(doc->m_iConnectPhase == eConnectNotConnected
                        ? 32239   /*ID_CONNECTION_CONNECT*/
                        : kPortMenuIdDisconnect);
        return;

    case eWorldSwitch:
        if (CMUSHclientDoc* target = wx_world_in_slot(b.arg))
            wx_activate_world_frame(target);
        return;

    case eFwd:
    default:
        Forward(b.arg);
        return;
    }
}

void CWorldToolBars::Forward (int cmdId)
{
    if (cmdId == 0) return;
    wxWindow* top = wxTheApp->GetTopWindow();
    if (!top || top == m_frame) return;

    m_forwarding = true;
    wxCommandEvent fwd(wxEVT_MENU, cmdId);
    fwd.SetEventObject(top);
    top->GetEventHandler()->ProcessEvent(fwd);
    m_forwarding = false;
}

CMUSHclientDoc* CWorldToolBars::ActiveDoc () const
{
    if (m_doc) return m_doc;
    return wx_doc_from_frame(m_frame);
}

wxToolBar* CWorldToolBars::Build (Which which)
{
    const BarSpec& spec = Spec(which);

    int art = spec.art;
    if (which == eActivity) {
        // Six interchangeable strips; the choice is a global preference.
        unsigned style = App.m_iActivityButtonBarStyle;
        if (style > 5) style = 0;          // same clamp as mainfrm.cpp OnCreate
        art = ActivityStrip(style);
    }

    const wxImage strip = LoadStrip(art);
    if (!strip.IsOk())
        return nullptr;                    // no art → no bar, rather than blanks

    // Parented to the strip, not the frame: wxAuiManager positions the panes
    // it manages inside the window it manages.
    wxToolBar* bar = new wxToolBar(m_host, wxID_ANY,
                                   wxDefaultPosition, wxDefaultSize,
                                   wxTB_HORIZONTAL | wxTB_FLAT | wxTB_NODIVIDER);
    bar->SetToolBitmapSize(wxSize(kTileW, kTileH));

    int tile = 0;
    for (size_t i = 0; i < spec.count; ++i) {
        const TbButton& b = spec.buttons[i];
        if (b.id == 0) { bar->AddSeparator(); continue; }

        const wxBitmap bmp = TileBitmap(strip, tile++);
        if (!bmp.IsOk()) continue;
        bar->AddTool(b.id, wxString::FromUTF8(b.tip), bmp,
                     wxString::FromUTF8(b.tip),
                     b.check ? wxITEM_CHECK : wxITEM_NORMAL);
    }

    bar->Realize();
    return bar;
}

bool CWorldToolBars::IsBarVisible (Which which) const
{
    return m_visible[which];
}

void CWorldToolBars::SetBarVisible (Which which, bool show)
{
    m_visible[which] = show;
    if (m_bars[which] && m_mgr) {
        wxAuiPaneInfo& pane = m_mgr->GetPane(m_bars[which]);
        if (pane.IsOk()) {
            pane.Show(show);
            // Re-wrapped: a bar going away can let the next one up into its
            // row, and one coming back can push others down.
            ApplyPlacements(s_place, true);
        }
    }
    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    cfg.Write(VisKey(which), show);
    cfg.Flush();
}

void CWorldToolBars::ResetLocations ()
{
    // Windows' CMainFrame::OnViewResetToolbars (mainfrm.cpp:1336) re-docks all
    // three at the top and does NOT touch visibility.  This item has always
    // also shown all three here — while there was nothing to rearrange, that
    // was the whole of what "reset" could mean — and that half is kept: taking
    // it away would leave someone who had hidden every bar with no single item
    // that brings them back, which is not a change this has any reason to make.
    for (int i = 0; i < eBarCount; ++i)
        SetBarVisible(static_cast<Which>(i), true);

    // Side by side, as Windows' CMainFrame::DockToolBarsSideBySide docks them;
    // see wx_toolbar.h.
    Placement def[eBarCount];
    DefaultPlacements(def);
    SetArrangement(def);
}

// ── Arrangement ───────────────────────────────────────────────────────────

void CWorldToolBars::ReadPlacements (Placement out[eBarCount]) const
{
    // Seed from the shared record so a bar this window could not build, or one
    // that is hidden and therefore not in any dock, keeps the placement the
    // other windows know about instead of being renumbered out of existence.
    for (int i = 0; i < eBarCount; ++i)
        out[i] = s_place[i];
    if (!m_mgr) return;

    int idx[eBarCount], row[eBarCount], pos[eBarCount], n = 0;
    for (int i = 0; i < eBarCount; ++i) {
        if (!m_bars[i]) continue;
        const wxAuiPaneInfo& p = m_mgr->GetPane(m_bars[i]);
        if (!p.IsOk() || !p.IsShown()) continue;
        idx[n] = i; row[n] = p.dock_row; pos[n] = p.dock_pos; ++n;
    }

    // Sort by (row, pos), ties broken by bar index so the answer never depends
    // on the order wxAuiManager happens to be holding its panes in.
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0; --b) {
            const bool before =
                row[b] < row[b-1] ||
                (row[b] == row[b-1] &&
                 (pos[b] < pos[b-1] ||
                  (pos[b] == pos[b-1] && idx[b] < idx[b-1])));
            if (!before) break;
            std::swap(idx[b], idx[b-1]);
            std::swap(row[b], row[b-1]);
            std::swap(pos[b], pos[b-1]);
        }

    // Canonicalise the VISIBLE bars: rows compacted to 0..k-1 and order
    // restarted within each.  The stored form HAS to be canonical — dock_pos is
    // a pixel offset, so two windows of different widths would otherwise publish
    // different numbers for the same picture and push them at each other without
    // ever settling.
    bool placed[eBarCount] = { false };
    int outRow = -1, outOrder = 0, prevRow = INT_MIN;
    for (int k = 0; k < n; ++k) {
        if (row[k] != prevRow) { prevRow = row[k]; ++outRow; outOrder = 0; }
        out[idx[k]].row   = outRow;
        out[idx[k]].order = outOrder++;
        placed[idx[k]]    = true;
    }

    // A bar NOT in the visible set (hidden, or not built in this window) kept the
    // absolute row it had in s_place above — which now COLLIDES with a renumbered
    // visible bar: hide Standard (row 0) and World renumbers from 1 to 0, so both
    // read (row 0, order 0), and re-showing Standard docks it on top of World.
    // Give each such bar its own fresh row ABOVE the visible ones instead, in
    // stored-placement order, so a re-show can never land on an occupied slot.
    int hid[eBarCount], hn = 0;
    for (int i = 0; i < eBarCount; ++i)
        if (!placed[i]) hid[hn++] = i;
    for (int a = 1; a < hn; ++a)
        for (int b = a; b > 0; --b) {
            const Placement& x = s_place[hid[b]];
            const Placement& y = s_place[hid[b-1]];
            const bool before = x.row < y.row ||
                (x.row == y.row && (x.order < y.order ||
                 (x.order == y.order && hid[b] < hid[b-1])));
            if (!before) break;
            std::swap(hid[b], hid[b-1]);
        }
    int nextRow = outRow + 1;
    for (int k = 0; k < hn; ++k) {
        out[hid[k]].row   = nextRow++;
        out[hid[k]].order = 0;
    }
}

void CWorldToolBars::ApplyPlacements (const Placement place[eBarCount], bool force)
{
    if (!m_mgr) return;

    Placement wrapped[eBarCount];
    bool      shown  [eBarCount];
    WrapToWidth(place, wrapped, shown);

    // The size event fires on every step of a live resize, and most steps
    // change no wrap; an Update for each would be a repaint for nothing.
    if (!force && m_laidOut) {
        bool same = true;
        for (int i = 0; i < eBarCount; ++i)
            if (shown[i] != m_wrappedShown[i] ||
                (shown[i] && (wrapped[i].row   != m_wrapped[i].row ||
                              wrapped[i].order != m_wrapped[i].order)))
                same = false;
        if (same) return;
    }

    for (int i = 0; i < eBarCount; ++i) {
        m_wrapped[i]      = wrapped[i];
        m_wrappedShown[i] = shown[i];
        if (!m_bars[i]) continue;
        wxAuiPaneInfo& p = m_mgr->GetPane(m_bars[i]);
        if (!p.IsOk()) continue;
        p.Row(wrapped[i].row).Position(wrapped[i].order);
    }
    m_mgr->Update();
    // What wxAuiManager made of it, rather than the wrap itself, is what a later
    // drag is measured against: then nothing about how it numbers its rows can
    // look like the user's doing.
    ReadShown(m_shown, m_shownShown);
    m_laidOut = true;
    SyncStripHeight();
}

void CWorldToolBars::ReadShown (Placement out[eBarCount], bool shown[eBarCount]) const
{
    // ReadPlacements numbers the shown bars from row 0 among themselves, so
    // their entries are comparable from one call to the next whatever the
    // hidden ones are doing.
    ReadPlacements(out);
    for (int i = 0; i < eBarCount; ++i) {
        shown[i] = false;
        if (!m_bars[i] || !m_mgr) continue;
        const wxAuiPaneInfo& p = m_mgr->GetPane(m_bars[i]);
        shown[i] = p.IsOk() && p.IsShown();
    }
}

void CWorldToolBars::WrapToWidth (const Placement place[eBarCount],
                                  Placement out[eBarCount],
                                  bool shown[eBarCount]) const
{
    const int avail = AvailableWidth();

    int idx[eBarCount];
    for (int i = 0; i < eBarCount; ++i) {
        idx[i]   = i;
        shown[i] = m_bars[i] != nullptr && m_visible[i];
    }
    std::sort(idx, idx + eBarCount, [place](int a, int b) {
        if (place[a].row   != place[b].row)   return place[a].row   < place[b].row;
        if (place[a].order != place[b].order) return place[a].order < place[b].order;
        return a < b;
    });

    // Each row asked for starts a row; within one, a bar that would run past
    // the edge starts another -- unless it is first on its row, where a bar
    // wider than the window is better clipped than moved down to be clipped
    // anyway.
    int row = -1, order = 0, x = 0, prevRow = INT_MIN;
    for (int k = 0; k < eBarCount; ++k) {
        const int i = idx[k];
        if (!shown[i]) continue;
        const int w = PaneWidth(i);
        if (place[i].row != prevRow || (x > 0 && x + w > avail)) {
            prevRow = place[i].row;
            ++row;
            order = 0;
            x     = 0;
        }
        out[i].row   = row;
        out[i].order = order++;
        x += w;
    }
    // Hidden bars below the shown ones, each on a row of its own: they take no
    // room, and showing one re-wraps anyway.
    for (int k = 0; k < eBarCount; ++k) {
        const int i = idx[k];
        if (shown[i]) continue;
        out[i].row   = ++row;
        out[i].order = 0;
    }
}

int CWorldToolBars::PaneWidth (int which) const
{
    // What wxAuiManager gives a toolbar pane along a top dock
    // (GetPanePositionsAndSizes): its best width, with the gripper beside it
    // and a border either side.
    const wxAuiPaneInfo& p = m_mgr->GetPane(m_bars[which]);
    int w = p.IsOk() ? p.best_size.x : -1;
    if (w <= 0)
        w = m_bars[which]->GetBestSize().x;
    if (wxAuiDockArt* art = m_mgr->GetArtProvider())
        w += art->GetMetric(wxAUI_DOCKART_GRIPPER_SIZE)
           + 2 * art->GetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE);
    return w;
}

int CWorldToolBars::AvailableWidth () const
{
    // The frame's width, not the strip's.  The strip is an wxEXPAND row of the
    // frame's sizer and so normally the same, but its minimum is the AUI
    // layout's widest row, and a sizer may hold it at that in a narrower frame
    // -- a width that depends on the wrap it is meant to decide, which can
    // settle on a wrap that is clipped.  No width yet: one bar per row, which is
    // never clipped, and the first size event re-wraps.
    return std::max(m_frame->GetClientSize().x, 0);
}

void CWorldToolBars::Adopt (const Placement place[eBarCount])
{
    Placement want[eBarCount];
    for (int i = 0; i < eBarCount; ++i)
        want[i] = place[i];
    Canonicalise(want);

    const bool changed = !SamePlacements(want, s_place);
    if (changed) {
        for (int i = 0; i < eBarCount; ++i)
            s_place[i] = want[i];
        SavePlacements();
    }

    // Forced here even when nothing changed: a bar dropped onto a row with no
    // room for it leaves the arrangement as it was, and this is what moves it
    // back to where the wrap puts it.
    ApplyPlacements(s_place, true);

    if (!changed) return;
    // Windows has one set of bars on one main frame, so rearranging them is
    // global.  Push it to the other world windows, exactly as the View ▸
    // Toolbars handler pushes a visibility toggle (wx_mfc_impl.cpp:1832);
    // each wraps it to its own width.
    s_publishing = true;
    for (CWorldToolBars* other : Instances())
        if (other != this)
            other->ApplyPlacements(s_place);
    s_publishing = false;
}

void CWorldToolBars::SyncStripHeight ()
{
    if (!m_host) return;
    // What the AUI layout now needs.  Keyed on a CHANGE in that, not on
    // "wanted != actual": PollLayout runs from a paint, so a height the frame's
    // sizer cannot grant -- a window shorter than its own contents -- would
    // otherwise relayout, repaint and ask again forever.
    const int want = m_host->GetBestSize().y;
    if (want == m_stripHeight) return;
    m_stripHeight = want;
    m_frame->Layout();                   // the strip is a row of the frame's sizer
}

int CWorldToolBars::StripHeight () const
{
    // The height the strip ACTUALLY has, not the height it wants.  The two only
    // agree once the frame has been laid out, which is the point: the wanted
    // height changes as soon as wxAuiManager re-docks a pane, so asserting on
    // it would pass even if nothing ever told the frame about it.
    return m_host ? m_host->GetSize().y : 0;
}

CWorldToolBars::Placement CWorldToolBars::GetPlacement (Which which) const
{
    return s_place[which];
}

CWorldToolBars::Placement CWorldToolBars::GetShownPlacement (Which which) const
{
    Placement all[eBarCount];
    bool      shown[eBarCount];
    ReadShown(all, shown);
    return all[which];
}

void CWorldToolBars::SetArrangement (const Placement place[eBarCount])
{
    Placement want[eBarCount];
    for (int i = 0; i < eBarCount; ++i) {
        // Clamp rather than reject: the caller is describing a picture, and a
        // row outside the strip would put a bar where no drag could reach it.
        want[i].row   = std::max(0, std::min(place[i].row,   eBarCount - 1));
        want[i].order = std::max(0, std::min(place[i].order, eBarCount - 1));
    }
    Adopt(want);
}

void CWorldToolBars::Reflow ()
{
    ApplyPlacements(s_place);
}

void CWorldToolBars::PollLayout ()
{
    if (s_publishing) return;
    SyncStripHeight();
    if (!m_laidOut) return;

    // Anything other than what this window laid out is a drop.
    Placement now[eBarCount];
    bool      shown[eBarCount];
    ReadShown(now, shown);
    bool dropped = false;
    for (int i = 0; i < eBarCount; ++i)
        if (shown[i] != m_shownShown[i] ||
            (shown[i] && (now[i].row   != m_shown[i].row ||
                          now[i].order != m_shown[i].order)))
            dropped = true;
    if (!dropped) return;

    // The picture dropped becomes the arrangement, rows the wrap made
    // included: once the user has moved a bar, the rows they see are the rows
    // they chose.
    Placement place[eBarCount];
    ReadPlacements(place);
    Adopt(place);
}

void CWorldToolBars::ReloadActivityStyle ()
{
    if (!m_bars[eActivity]) return;

    unsigned style = App.m_iActivityButtonBarStyle;
    if (style > 5) style = 0;
    const wxImage strip =
        LoadStrip(ActivityStrip(style));
    if (!strip.IsOk()) return;

    // Force every button to be re-imaged on the next UpdateState.
    for (int i = 0; i < 10; ++i)
        m_activityImage[i] = -1;

    for (int i = 0; i < 10; ++i) {
        const wxBitmap bmp = TileBitmap(strip, i);
        if (bmp.IsOk())
            m_bars[eActivity]->SetToolNormalBitmap(kActivity[i].id, bmp);
    }
    m_bars[eActivity]->Refresh();
}

void CWorldToolBars::UpdateState (CMUSHclientDoc* activeDoc)
{
    m_doc = activeDoc;   // Dispatch() needs it; clicks arrive between ticks

    // Pick up a drag the render hook missed.  That hook rides wxAuiManager's
    // paint, so it is prompt but not guaranteed; this tick is guaranteed but
    // 100 ms late.  Both are change-detected, so running both is free.
    PollLayout();

    // Keep slot numbers DENSE (2026-08-12).  Windows renumbers on every idle
    // tick (CMUSHclientApp::UpdateWorldCounts from OnIdle) and on activity
    // updates; the port ran neither, so closing world B of three left its
    // slot greyed forever and wx_world_in_slot's lazy fallback then assigned a
    // NEW world a number an existing one already held — a world the bar
    // could never raise or light again.  This tick (every 100 ms) is the
    // port's idle pump; the walk is the canonical one.
    App.UpdateWorldCounts();

    // ── Standard + World bars ─────────────────────────────────────────────
    // Run the canonical ON_UPDATE_COMMAND_UI handler for each button against
    // the active world, as the menu path does (wx_mfc_impl.cpp).  A command
    // with no handler stays enabled; an unsupported one is always greyed.
    for (int w = eStandard; w <= eWorld; ++w) {
        wxToolBar* bar = m_bars[w];
        if (!bar || !m_visible[w]) continue;
        const BarSpec& spec = Spec(w);
        for (size_t i = 0; i < spec.count; ++i) {
            const TbButton& b = spec.buttons[i];
            if (b.id == 0) continue;

            if (b.act == eUnsupported) { bar->EnableTool(b.id, false); continue; }

            // Ask the LAYERED source, not the document alone.  Windows' MFC
            // routes a toolbar update through view -> document -> frame -> app,
            // so every button whose canonical ON_UPDATE_COMMAND_UI lives on
            // CMUSHView / CSendView / CMainFrame got no answer here at all —
            // Freeze Output's handler is CMUSHView::OnUpdateDisplayFreezeoutput
            // (mushview.cpp:3240), and the document's map ends in
            // OnFixMenus, which returns before capturing anything.
            bool enabled = true;
            int  checked = -1;
            wx_canonical_menu_ui_state(b.id, activeDoc, enabled, checked);
            bar->EnableTool(b.id, enabled);
            // Unconditionally for a check item, NOT "only if something
            // answered".  wxToolBarTool::Action presses a wxITEM_CHECK tool
            // BEFORE dispatching the click, so a silent source leaves whatever
            // that auto-toggle wrote: freeze the pane from the Display menu and
            // the button stayed up; then press the button and it went down
            // while FreezeOutput() un-froze — the exact inverse, permanently.
            if (b.check)
                bar->ToggleTool(b.id, checked > 0);
        }
    }

    // ── Activity bar ──────────────────────────────────────────────────────
    // One button per world slot, imaged by that world's state.  This is
    // CMainFrame::OnUpdateBtnWorlds, which the port cannot call (it reaches
    // into m_wndActivityToolBar's MFC button info), so the same rules run here
    // against wxToolBar:
    //   tiles  0- 9  black  open, nothing new
    //   tiles 10-19  red    new messages
    //   tiles 20-29  green  closed / not connected
    wxToolBar* act = m_bars[eActivity];
    if (!act || !m_visible[eActivity]) return;

    unsigned style = App.m_iActivityButtonBarStyle;
    if (style > 5) style = 0;
    static wxImage cachedStrip;
    static unsigned cachedStyle = (unsigned) -1;
    if (cachedStyle != style) {
        cachedStrip = LoadStrip(ActivityStrip(style));
        cachedStyle = style;
    }

    // Build the slot->world map in ONE walk.  UpdateWorldCounts() above already
    // numbered every world 1..N in m_view_number, so calling wx_world_in_slot
    // (which restarts the whole doc-list walk) ten times did 10 O(N) traversals
    // per tick on top of that one -- 11 total, every 100 ms, per open world.
    // (wx_world_in_slot's lazy number-assign branch is never reached from here,
    // since UpdateWorldCounts just assigned them all.)
    CMUSHclientDoc* slotWorld[11] = {};   // indices 1..10
    if (App.m_pWorldDocTemplate) {
        for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
            CMUSHclientDoc* pDoc =
                (CMUSHclientDoc*) App.m_pWorldDocTemplate->GetNextDoc(pos);
            const int n = pDoc->m_view_number;
            if (n >= 1 && n <= 10)
                slotWorld[n] = pDoc;
        }
    }

    for (int slot = 1; slot <= 10; ++slot) {
        const int btnIndex = slot - 1;
        const int id       = kActivity[btnIndex].id;

        CMUSHclientDoc* found = slotWorld[slot];
        if (!found) {
            act->EnableTool(id, false);
            act->ToggleTool(id, false);
            continue;
        }

        act->EnableTool(id, true);
        act->ToggleTool(id, found == activeDoc);

        int image = found->m_new_lines ? btnIndex + 10 : btnIndex;
        if (found->m_iConnectPhase != eConnectConnectedToMud ||
            !found->ConnectionOpen())
            image = btnIndex + 20;

        if (image != m_activityImage[btnIndex]) {
            const wxBitmap bmp = TileBitmap(cachedStrip, image);
            if (bmp.IsOk()) {
                act->SetToolNormalBitmap(id, bmp);
                m_activityImage[btnIndex] = image;
            }
        }
    }
}
