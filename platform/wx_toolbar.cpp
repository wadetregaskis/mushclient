// wx_toolbar.cpp — the three canonical MFC toolbars over wxToolBar.
// See wx_toolbar.h for why they live on the world window rather than the frame.

// wx headers MUST precede the MFC-compat headers: stdafx.h (compat.h) defines
// Windows-style macros (DrawText, GetObject, …) that otherwise clobber wx's own
// headers.  Same ordering as wx_activity.cpp / wx_world_child.cpp.
#include <wx/wx.h>
#include <wx/toolbar.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/config.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "resource.h"
#include "platform/wx_toolbar.h"

// Defined in wx_mfc_impl.cpp (see the same forward declaration in
// wx_activity.cpp): raise + activate a world's frame.  Declared rather than
// included because wx_world_child.h's wx includes would be clobbered by
// compat.h's macros in this translation unit.
void wx_activate_world_frame(CMUSHclientDoc* doc);

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
    { 0xE402 /*ID_EDIT_COPY*/,  eFwd, wxID_COPY,   false, "Copy" },
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
    { ID_FILE_LOGSESSION,    eFwd, ID_FILE_LOGSESSION, false, "Log session" },
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

// The world occupying activity slot `slot` (1..10), or null.  Numbers any
// unnumbered world on the way, as CMainFrame::OnUpdateBtnWorlds does.
CMUSHclientDoc* WorldInSlot (int slot)
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

// ── Art ───────────────────────────────────────────────────────────────────

// Same two-candidate search the dialog bridge uses (wx_dialog_bridge.cpp):
// the bundle's Resources dir, then alongside the executable.
wxString FindArt (const wxString& file)
{
    const wxString rel = wxString(wxT("toolbar_art")) + wxFILE_SEP_PATH + file;
    wxString candidates[] = {
        wxStandardPaths::Get().GetResourcesDir() + wxFILE_SEP_PATH + rel,
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath()
            + wxFILE_SEP_PATH + rel,
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
// there is no equivalent here, so keep it simple: one flag per bar.
wxString VisKey (int which)
{
    return wxString::Format(wxT("/toolbars/visible%d"), which);
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
        wxConfig cfg(wxT("mushclient"));
        for (int i = 0; i < eBarCount; ++i)
            cfg.Read(VisKey(i), &m_visible[i], true);
    }

    // Insert in canonical order at the very top of the frame's vertical sizer,
    // above the output pane.  `slot` counts only the bars that were actually
    // built: if one has no art, the next must still land directly beneath its
    // predecessor rather than below the output pane.
    int slot = 0;
    for (int i = 0; i < eBarCount; ++i) {
        m_bars[i] = Build(static_cast<Which>(i));
        if (!m_bars[i]) continue;
        m_sizer->Insert(slot++, m_bars[i], 0, wxEXPAND);
        if (!m_visible[i])
            m_bars[i]->Hide();
    }

    for (int i = 0; i < eBarCount; ++i) {
        const BarSpec& spec = Spec(i);
        for (size_t b = 0; b < spec.count; ++b)
            if (spec.buttons[b].id != 0)
                m_ourIds.insert(spec.buttons[b].id);
    }

    m_frame->Bind(wxEVT_TOOL, [this](wxCommandEvent& evt) {
        if (m_ourIds.find(evt.GetId()) == m_ourIds.end()) { evt.Skip(); return; }

        // Re-entrancy guard.  wxMDIParentFrameBase::TryBefore offers the parent
        // frame's events to the ACTIVE CHILD first — which is this frame — so an
        // event handed upwards comes straight back here and, without this flag,
        // recurses until the stack dies.  On the second pass, skip: the event
        // then carries on to the parent's own handler, where the canonical
        // command dispatch lives.
        if (m_forwarding) { evt.Skip(); return; }

        DispatchId(evt.GetId());
    });
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
        if (doc)
            Forward(doc->m_iConnectPhase == eConnectNotConnected
                        ? 32239   /*ID_CONNECTION_CONNECT*/
                        : 0xE200  /*Disconnect*/);
        return;

    case eWorldSwitch:
        if (CMUSHclientDoc* target = WorldInSlot(b.arg))
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

    wxToolBar* bar = new wxToolBar(m_frame, wxID_ANY,
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
    if (m_bars[which]) {
        m_bars[which]->Show(show);
        m_frame->Layout();
    }
    wxConfig cfg(wxT("mushclient"));
    cfg.Write(VisKey(which), show);
    cfg.Flush();
}

void CWorldToolBars::ResetLocations ()
{
    for (int i = 0; i < eBarCount; ++i)
        SetBarVisible(static_cast<Which>(i), true);
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

            bool enabled = true;
            int  checked = -1;
            if (activeDoc) {
                CCmdUI cmdui;
                cmdui.m_nID            = (UINT) b.id;
                cmdui.m_pCaptureEnable = &enabled;
                cmdui.m_pCaptureCheck  = &checked;
                AfxDispatchUpdateUI(activeDoc, &cmdui);
            }
            bar->EnableTool(b.id, enabled);
            if (b.check && checked >= 0)
                bar->ToggleTool(b.id, checked != 0);
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

    for (int slot = 1; slot <= 10; ++slot) {
        const int btnIndex = slot - 1;
        const int id       = kActivity[btnIndex].id;

        CMUSHclientDoc* found = WorldInSlot(slot);
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
