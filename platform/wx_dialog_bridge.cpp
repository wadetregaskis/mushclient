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

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "wx_dialog_resource.h"

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
            default:   // BS_PUSHBUTTON / BS_DEFPUSHBUTTON / owner-draw
                w = new wxButton(parent, id, U(c.text), pos, size);
                break;
        }
    }
    else if (cls == "Static")
    {
        long st = 0;
        switch (c.style & SS_TYPEMASK)
        {
            case SS_CENTER: st = wxALIGN_CENTRE_HORIZONTAL; break;
            case SS_RIGHT:  st = wxALIGN_RIGHT; break;
            default:        st = wxALIGN_LEFT; break;
        }
        // icon/bitmap statics: a blank placeholder of the right size for now
        w = new wxStaticText(parent, id, U(c.text), pos, size, st);
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
    }
    else if (cls == "ComboBox")
    {
        long st = ((c.style & 3) == CBS_DROPDOWNLIST) ? wxCB_READONLY : 0;
        w = new wxComboBox(parent, id, wxEmptyString, pos, size, 0, nullptr, st);
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
    else
    {
        // Unknown / custom window class (SysListView32, SysTreeView32,
        // SysTabControl32, RICHEDIT, …): a placeholder panel preserves layout
        // and z-order without crashing.  Real mappings come later.
        w = new wxPanel(parent, id, pos, size);
    }

    if (w && (c.style & WS_DISABLED)) w->Enable(false);
    return w;
}

} // namespace

// Build a wxDialog from a recovered template.  `parent` may be null.
// Controls are created in template order (groupboxes first, so siblings paint
// on top) and positioned absolutely via wx's dialog-unit conversion.
wxDialog* BuildWxDialogFromTemplate(wxWindow* parent, const RcDialog& t)
{
    wxDialog* dlg = new wxDialog(parent, wxID_ANY, U(t.caption),
                                 wxDefaultPosition, wxDefaultSize,
                                 wxDEFAULT_DIALOG_STYLE);

    // The dialog font drives dialog-unit -> pixel conversion (MapDialogRect).
    if (!t.fontFace.empty())
        dlg->SetFont(wxFont(t.fontPt > 0 ? t.fontPt : 8,
                            wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                            wxFONTWEIGHT_NORMAL, false, U(t.fontFace)));

    dlg->SetClientSize(dlg->ConvertDialogToPixels(wxSize(t.cx, t.cy)));

    for (const RcControl& c : t.controls)
    {
        wxPoint pos = dlg->ConvertDialogToPixels(wxPoint(c.x, c.y));
        wxSize  sz  = dlg->ConvertDialogToPixels(wxSize(c.cx, c.cy));
        CreateControl(dlg, c, pos, sz);
    }
    return dlg;
}
