// wx_toolbar.cpp — the three canonical MFC toolbars over wxToolBar.
// See wx_toolbar.h for why they live on the world window rather than the frame.

// wx headers MUST precede the MFC-compat headers: stdafx.h (compat.h) defines
// Windows-style macros (DrawText, GetObject, …) that otherwise clobber wx's own
// headers.  Same ordering as wx_activity.cpp / wx_world_child.cpp.
#include <wx/wx.h>
#include <wx/toolbar.h>
#include <wx/aui/framemanager.h>
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

// Also wx_mfc_impl.cpp, and declared here for the same reason: the one answer
// to "may the user choose this command right now", layered canonical table ->
// menu_ui_rules -> the document's own ON_UPDATE_COMMAND_UI.  The menu bar and
// the output pane's context menu both ask it; the toolbar used to ask the
// DOCUMENT alone, which is a strictly smaller question (see UpdateState).
bool wx_canonical_menu_ui_state(int id, CMUSHclientDoc* doc,
                                bool& enabled, int& checked);

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

struct BarSpec { const TbButton* buttons; size_t count; const char* art; };

const BarSpec& Spec (int which)
{
    static const BarSpec specs[] = {
        { kStandard, WXSIZEOF(kStandard), "standard.bmp" },
        { kWorld,    WXSIZEOF(kWorld),    "world.bmp"    },
        { kActivity, WXSIZEOF(kActivity), nullptr /*chosen by style*/ },
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

// Same two-candidate search the dialog bridge uses (wx_dialog_bridge.cpp):
// the bundle's Resources dir, then alongside the executable.
wxString FindArt (const wxString& file)
{
    const wxString rel = wxString(wxT("toolbar_art")) + wxFILE_SEP_PATH + file;
    const wxString relPlat = wxString(wxT("platform")) + wxFILE_SEP_PATH + rel;
    const wxString exeDir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    wxString candidates[] = {
        wxStandardPaths::Get().GetResourcesDir() + wxFILE_SEP_PATH + rel,
        exeDir + wxFILE_SEP_PATH + rel,
        exeDir + wxFILE_SEP_PATH + relPlat,
        exeDir + wxFILE_SEP_PATH + wxT("..") + wxFILE_SEP_PATH + relPlat,
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

wxImage LoadStrip (const wxString& file)
{
    const wxString path = FindArt(file);
    if (path.empty()) return wxImage();
    wxImage img;
    wxLogNull noLog;    // a bad/missing strip degrades to "no bar", not a dialog
    if (!img.LoadFile(path)) return wxImage();
    return img;
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

void LoadPlacements ()
{
    if (s_placeLoaded) return;
    s_placeLoaded = true;

    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    for (int i = 0; i < CWorldToolBars::eBarCount; ++i) {
        // The canonical arrangement is one bar per row, in declaration order —
        // which is also what Windows' OnViewResetToolbars produces.
        cfg.Read(RowKey(i),   &s_place[i].row,   i);
        cfg.Read(OrderKey(i), &s_place[i].order, 0);
        // A config file can be hand-edited or written by a future version with
        // more bars; a row number outside the strip would put a bar somewhere
        // no drag could reach it from.
        if (s_place[i].row   < 0 || s_place[i].row   >= CWorldToolBars::eBarCount)
            s_place[i].row = i;
        if (s_place[i].order < 0 || s_place[i].order >= CWorldToolBars::eBarCount)
            s_place[i].order = 0;
    }
}

void SavePlacements ()
{
    const std::unique_ptr<wxConfigBase> cfgOwner = OpenAppConfig();
    wxConfigBase& cfg = *cfgOwner;
    for (int i = 0; i < CWorldToolBars::eBarCount; ++i) {
        cfg.Write(RowKey(i),   s_place[i].row);
        cfg.Write(OrderKey(i), s_place[i].order);
    }
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
    m_mgr->Update();
    // The frame has no sizer yet when a world window builds its bars (it calls
    // SetSizer after us), so this Layout is a no-op there and the frame's own
    // first layout does the job.  In a selftest, where the sizer is already
    // attached, it is what gives the strip its height before anything is asked
    // about it.
    SyncStripHeight();

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
    return m_doc;
}

wxToolBar* CWorldToolBars::Build (Which which)
{
    const BarSpec& spec = Spec(which);

    wxString artFile;
    if (which == eActivity) {
        // Six interchangeable strips; the choice is a global preference.
        unsigned style = App.m_iActivityButtonBarStyle;
        if (style > 5) style = 0;          // same clamp as mainfrm.cpp OnCreate
        artFile = wxString::Format(wxT("activity_style%u.bmp"), style);
    } else {
        artFile = wxString::FromUTF8(spec.art);
    }

    const wxImage strip = LoadStrip(artFile);
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
            m_mgr->Update();      // drops the now-empty row, or puts it back
        }
        SyncStripHeight();
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

    Placement canonical[eBarCount];
    for (int i = 0; i < eBarCount; ++i) {
        canonical[i].row   = i;
        canonical[i].order = 0;
    }
    SetArrangement(canonical);
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

void CWorldToolBars::ApplyPlacements (const Placement place[eBarCount])
{
    if (!m_mgr) return;
    for (int i = 0; i < eBarCount; ++i) {
        if (!m_bars[i]) continue;
        wxAuiPaneInfo& p = m_mgr->GetPane(m_bars[i]);
        if (!p.IsOk()) continue;
        p.Row(place[i].row).Position(place[i].order);
    }
    m_mgr->Update();
    SyncStripHeight();
}

void CWorldToolBars::PublishPlacements ()
{
    if (s_publishing) return;

    Placement now[eBarCount];
    ReadPlacements(now);

    bool changed = false;
    for (int i = 0; i < eBarCount; ++i)
        if (now[i].row != s_place[i].row || now[i].order != s_place[i].order)
            changed = true;
    if (!changed) return;

    for (int i = 0; i < eBarCount; ++i)
        s_place[i] = now[i];
    SavePlacements();

    // Windows has one set of bars on one main frame, so rearranging them is
    // global.  Push it to the other world windows, exactly as the View ▸
    // Toolbars handler pushes a visibility toggle (wx_mfc_impl.cpp:1832).
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
    Placement all[eBarCount];
    ReadPlacements(all);
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
    ApplyPlacements(want);
    PublishPlacements();
}

void CWorldToolBars::PollLayout ()
{
    if (s_publishing) return;
    SyncStripHeight();
    PublishPlacements();
}

void CWorldToolBars::ReloadActivityStyle ()
{
    if (!m_bars[eActivity]) return;

    unsigned style = App.m_iActivityButtonBarStyle;
    if (style > 5) style = 0;
    const wxImage strip =
        LoadStrip(wxString::Format(wxT("activity_style%u.bmp"), style));
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
        cachedStrip = LoadStrip(wxString::Format(wxT("activity_style%u.bmp"), style));
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
