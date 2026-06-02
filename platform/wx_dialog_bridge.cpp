// platform/wx_dialog_bridge.cpp
//
// The MFC-dialog -> wxWidgets bridge.  Like wx_mfc_impl.cpp this file includes
// BOTH wx and MUSHclient headers: it owns the parsed dialog-resource registry
// and (subsequently) builds a wxDialog from a template, runs the canonical
// CDialog's OnInitDialog/DoDataExchange/handlers, and returns the modal result —
// so dialogs/**/*.cpp render natively with no per-dialog wx code.
//
// Step 9 (this commit): load + parse the recovered resources once, on demand.

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#  include <wx/wx.h>
#endif
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/settings.h>   // wxSystemSettings (native dialog font)
#include <wx/statline.h>   // wxStaticLine (etched separators)
#include <wx/listctrl.h>   // wxListCtrl (SysListView32 — entity managers)
#include <wx/slider.h>     // wxSlider (msctls_trackbar32 — ColourPickerDlg RGB)

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "wx_dialog_resource.h"
#include "wx_dialog_bridge.h"
#include "wx_mfc_hooks.h"   // g_wxhook_make_owner_draw_button (BS_OWNERDRAW)
#include "wx_osx_text.h"    // wxOSXDisableSmartSubstitutions (macOS smart-quote fix)

namespace {

std::string SlurpFile(const wxString& path)
{
    std::ifstream f(path.fn_str(), std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Locate a bundled resource file.  In a .app this is Contents/Resources/;
// for a plain (non-bundle) binary, fall back to the executable's directory.
wxString FindResource(const wxString& name)
{
    wxString candidates[] = {
        wxStandardPaths::Get().GetResourcesDir() + wxFILE_SEP_PATH + name,
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath()
            + wxFILE_SEP_PATH + name,
    };
    for (const wxString& c : candidates)
        if (wxFileName::FileExists(c))
            return c;
    return candidates[0];   // best guess; caller logs the failure
}

} // namespace

// Process-wide registry of the recovered dialog/menu templates, loaded once.
RcResources& MfcDialogResources()
{
    static RcResources res;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        wxString reshdr = FindResource(wxT("resource.h"));
        wxString rcfile = FindResource(wxT("mushclient_dialogs.rc"));
        std::string h  = SlurpFile(reshdr);
        std::string rc = SlurpFile(rcfile);
        if (!h.empty())  res.LoadSymbols(h);
        if (!rc.empty()) res.ParseRc(rc);
        std::fprintf(stderr,
            "[dialog-bridge] resources: %zu dialogs, %zu menus, %zu controls "
            "(%d unresolved control ids) from %s\n",
            res.DialogCount(), res.MenuCount(), res.ControlCount(),
            res.UnresolvedSymbols(),
            rc.empty() ? "<not found>" : (const char*)rcfile.utf8_str());
    }
    return res;
}

// Pre-load the resources at startup (wx-only callers use this void trampoline;
// it avoids first-dialog latency and surfaces any bundling problem immediately).
void LoadMfcDialogResources()
{
    MfcDialogResources();
}

// ── Win32 control-style bits we key off when mapping to wx widgets ───────────
namespace {
enum {
    WS_DISABLED   = 0x08000000,
    BS_TYPEMASK   = 0x0000000F,
    BS_DEFPUSHBUTTON = 1, BS_CHECKBOX = 2, BS_AUTOCHECKBOX = 3,
    BS_RADIOBUTTON = 4, BS_3STATE = 5, BS_AUTO3STATE = 6, BS_GROUPBOX = 7,
    BS_AUTORADIOBUTTON = 9,
    ES_CENTER = 1, ES_RIGHT = 2, ES_MULTILINE = 4, ES_PASSWORD = 0x20,
    ES_AUTOHSCROLL = 0x80, ES_READONLY = 0x800,
    SS_TYPEMASK = 0x1F, SS_CENTER = 1, SS_RIGHT = 2, SS_ICON = 3, SS_BITMAP = 14,
    CBS_SIMPLE = 1, CBS_DROPDOWN = 2, CBS_DROPDOWNLIST = 3,
    LBS_EXTENDEDSEL = 0x800, LBS_MULTIPLESEL = 8,
    SBS_VERT = 1,
};

inline wxString U(const std::string& s) { return wxString::FromUTF8(s.c_str()); }

// Create the wx widget for one RcControl, already positioned in pixels.
// Returns the created window (added as a child of `parent`), or null if skipped.
wxWindow* CreateControl(wxWindow* parent, const RcControl& c,
                        const wxPoint& pos, const wxSize& size)
{
    const std::string& cls = c.className;
    int id = (c.id == -1) ? wxID_ANY : c.id;     // IDC_STATIC -> no id
    wxWindow* w = nullptr;

    if (cls == "Button")
    {
        switch (c.style & BS_TYPEMASK)
        {
            case BS_GROUPBOX:
                w = new wxStaticBox(parent, id, U(c.text), pos, size);
                break;
            case BS_CHECKBOX: case BS_AUTOCHECKBOX:
            case BS_3STATE:   case BS_AUTO3STATE:
                w = new wxCheckBox(parent, id, U(c.text), pos, size);
                break;
            case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON:
                w = new wxRadioButton(parent, id, U(c.text), pos, size,
                                      (c.style & 0x20000 /*WS_GROUP*/) ? wxRB_GROUP : 0);
                break;
            case 0x0B: /*BS_OWNERDRAW*/
                // Self-painting control: its paint handler runs the canonical
                // CButton::DrawItem (e.g. CColourButton's colour swatch).
                if (g_wxhook_make_owner_draw_button)
                    w = (wxWindow*) g_wxhook_make_owner_draw_button(
                            parent, id, c.text.c_str(), pos.x, pos.y, size.x, size.y);
                else
                    w = new wxButton(parent, id, U(c.text), pos, size);
                break;
            default:   // BS_PUSHBUTTON / BS_DEFPUSHBUTTON
                w = new wxButton(parent, id, U(c.text), pos, size);
                break;
        }
    }
    else if (cls == "Static")
    {
        switch (c.style & SS_TYPEMASK)
        {
            case 0x10: // SS_ETCHEDHORZ — a horizontal separator line
                w = new wxStaticLine(parent, id, pos, wxSize(size.x, 2), wxLI_HORIZONTAL);
                break;
            case 0x11: // SS_ETCHEDVERT — a vertical separator line
                w = new wxStaticLine(parent, id, pos, wxSize(2, size.y), wxLI_VERTICAL);
                break;
            case 7: case 8: case 9: case 0x12: // SS_*FRAME — a rectangular frame
                w = new wxStaticBox(parent, id, wxEmptyString, pos, size);
                break;
            case SS_ICON: case SS_BITMAP: // image statics: reserve space (no source yet)
                w = new wxStaticText(parent, id, wxEmptyString, pos, size);
                break;
            default:
            {
                long st = ((c.style & SS_TYPEMASK) == SS_CENTER) ? wxALIGN_CENTRE_HORIZONTAL
                        : ((c.style & SS_TYPEMASK) == SS_RIGHT)  ? wxALIGN_RIGHT
                        :                                          wxALIGN_LEFT;
                // Keep the control at its template size (MFC statics have fixed
                // rects) so text set later via DDX wraps to the right width.
                w = new wxStaticText(parent, id, U(c.text), pos, size, st | wxST_NO_AUTORESIZE);
                break;
            }
        }
    }
    else if (cls == "Edit")
    {
        long st = 0;
        if (c.style & ES_MULTILINE) st |= wxTE_MULTILINE;
        if (c.style & ES_PASSWORD)  st |= wxTE_PASSWORD;
        if (c.style & ES_READONLY)  st |= wxTE_READONLY;
        if (c.style & ES_RIGHT)     st |= wxTE_RIGHT;
        else if (c.style & ES_CENTER) st |= wxTE_CENTRE;
        w = new wxTextCtrl(parent, id, U(c.text), pos, size, st);
        if (st & wxTE_MULTILINE)   // keep typed/pasted Lua from being mangled
            wxOSXDisableSmartSubstitutions(static_cast<wxTextCtrl*>(w));
    }
    else if (cls == "ComboBox")
    {
        long st = ((c.style & 3) == CBS_DROPDOWNLIST) ? wxCB_READONLY : 0;
        // A combo's template height (cy) is the DROPPED-DOWN list height, not the
        // closed control height — using it verbatim makes the combo absurdly tall
        // and overlap its siblings.  Let wx pick the natural (closed) height.
        wxComboBox* cb = new wxComboBox(parent, id, wxEmptyString, pos,
                                        wxSize(size.GetWidth(), -1), 0, nullptr, st);
        // Preset items recovered from the dialog's DLGINIT resource (e.g. the
        // scripting-language list, proxy type, alias/timer "send to").  Combos
        // that instead fill themselves in OnInitDialog have no DLGINIT items.
        for (const std::string& item : c.comboItems)
            cb->Append(wxString::FromUTF8(item.c_str()));
        w = cb;
    }
    else if (cls == "ListBox")
    {
        long st = (c.style & (LBS_EXTENDEDSEL | LBS_MULTIPLESEL)) ? wxLB_EXTENDED : wxLB_SINGLE;
        w = new wxListBox(parent, id, pos, size, 0, nullptr, st | wxLB_NEEDED_SB);
    }
    else if (cls == "ScrollBar")
    {
        w = new wxScrollBar(parent, id, pos, size,
                            (c.style & SBS_VERT) ? wxSB_VERTICAL : wxSB_HORIZONTAL);
    }
    else if (cls == "msctls_trackbar32")
    {
        // Slider (CSliderCtrl), e.g. the RGB sliders in ColourPickerDlg.  TBS_VERT
        // == 0x2; default horizontal.  Range/pos/page set later via the bridged
        // CSliderCtrl methods.
        long st = (c.style & 0x2 /*TBS_VERT*/) ? wxSL_VERTICAL : wxSL_HORIZONTAL;
        w = new wxSlider(parent, id, 0, 0, 100, pos, size, st);
    }
    else if (cls == "SysListView32")
    {
        // The entity managers' object list (triggers/aliases/timers/variables).
        // The canonical pages drive it in report mode (LVS_REPORT == 0x1); they
        // add columns + items via the bridged CListCtrl methods.  Single-select
        // unless LVS_SINGLESEL (0x4) is absent → MFC default is multi-select.
        long st = wxLC_REPORT;
        if (c.style & 0x4 /*LVS_SINGLESEL*/) st |= wxLC_SINGLE_SEL;
        if (c.style & 0x8000 /*LVS_NOSORTHEADER*/) st |= wxLC_NO_SORT_HEADER;
        w = new wxListCtrl(parent, id, pos, size, st);
    }
    else
    {
        // Unknown / custom window class (SysTreeView32, SysTabControl32,
        // RICHEDIT, …): a placeholder panel preserves layout and z-order
        // without crashing.  Real mappings come later.
        w = new wxPanel(parent, id, pos, size);
    }

    if (w)
    {
        w->SetFont(parent->GetFont());          // match the layout-conversion font
        // MFC text statics word-wrap within their rectangle; wxStaticText clips
        // by default, so wrap long label text to the control width.
        if (wxStaticText* st = wxDynamicCast(w, wxStaticText))
            st->Wrap(size.GetWidth());
        if (c.style & WS_DISABLED) w->Enable(false);
    }
    return w;
}

} // namespace

// Populate `target` (a wxDialog, or a wxTreebook page wxPanel, …) with the
// template's controls, absolutely positioned via wx's dialog-unit conversion.
// Controls are created in template order (groupboxes first, so siblings paint
// on top).  The target's min-client-size is set to the template's size so a
// container (e.g. wxTreebook) can size itself to the largest page.
void PopulateWindowFromTemplate(wxWindow* target, const RcDialog& t)
{
    // Use the native macOS dialog font for BOTH the dialog-unit -> pixel
    // conversion (ConvertDialogToPixels, below) AND the controls (each control
    // gets this same font in CreateControl).  The recovered template's font
    // ("MS Sans Serif" 8pt) has no macOS equivalent, and mixing an 8pt
    // conversion with default ~13pt control text is exactly what made the
    // earlier layout wrong: rects sized for one font, text drawn in another.
    target->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));

    wxSize client = target->ConvertDialogToPixels(wxSize(t.cx, t.cy));

    for (const RcControl& c : t.controls)
    {
        wxPoint pos = target->ConvertDialogToPixels(wxPoint(c.x, c.y));
        wxSize  sz  = target->ConvertDialogToPixels(wxSize(c.cx, c.cy));
        CreateControl(target, c, pos, sz);
    }
    target->SetMinClientSize(client);
}

// Build a standalone wxDialog from a recovered template.  `parent` may be null.
wxDialog* BuildWxDialogFromTemplate(wxWindow* parent, const RcDialog& t)
{
    wxDialog* dlg = new wxDialog(parent, wxID_ANY, U(t.caption),
                                 wxDefaultPosition, wxDefaultSize,
                                 wxDEFAULT_DIALOG_STYLE);
    PopulateWindowFromTemplate(dlg, t);
    dlg->SetClientSize(dlg->GetMinClientSize());
    return dlg;
}
