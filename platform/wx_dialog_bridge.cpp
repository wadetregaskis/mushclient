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
#include <wx/statbmp.h>    // wxStaticBitmap (SS_ICON / SS_BITMAP image statics)
#include <wx/image.h>      // wxImage / wxInitAllImageHandlers (BMP / ICO art)
#include <wx/listctrl.h>   // wxListCtrl (SysListView32 — entity managers)
#include <wx/slider.h>     // wxSlider (msctls_trackbar32 — ColourPickerDlg RGB)
#include <wx/valtext.h>     // wxTextValidator (ES_NUMBER digit filtering)
#include <algorithm>       // std::min

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

// Map a Win32 image-resource id to its bundled dialog-art filename.  These files
// come from nickgammon/mushclient_resources (res/ + MUSHclient.rc) and are
// bundled into Contents/Resources/dialog_art/.  Returns "" for ids with no art.
static wxString DialogArtFile(int resId)
{
    switch (resId) {
        case 103: return wxT("litebulb.bmp");   // IDB_LIGHTBULB  (Tip-of-the-Day bulb)
        case 128: return wxT("MUSHclient.png");  // IDR_MAINFRAME  (app icon / About logo)
        case 129: return wxT("doc.ico");         // IDR_MUSHCLTYPE (tray default)
        case 288: return wxT("Cdrom01.ico");     // IDI_ICON1
        case 289: return wxT("Wrench.ico");      // IDI_ICON2
        case 290: return wxT("Clock05.ico");     // IDI_ICON3
        case 291: return wxT("Earth.ico");       // IDI_ICON4
        case 292: return wxT("Graph08.ico");     // IDI_ICON5
        case 293: return wxT("Handshak.ico");    // IDI_ICON6
        case 294: return wxT("Mail16b.ico");     // IDI_ICON7
        case 295: return wxT("Net01.ico");       // IDI_ICON8
        case 296: return wxT("Point11.ico");     // IDI_ICON9
        case 297: return wxT("Clip07.ico");      // IDI_ICON10
        default:  return wxString();
    }
}

// Load a bundled dialog-art image (BMP / ICO) by Win32 resource id.  Returns an
// invalid wxBitmap if the id is unknown or the file is missing.  Used to render
// SS_ICON / SS_BITMAP statics and to drive the macOS Dock icon (ma — see
// wx_mfc_impl.cpp).
wxBitmap LoadDialogArt(int resId)
{
    wxString file = DialogArtFile(resId);
    if (file.empty()) return wxBitmap();
    // Image handlers are registered once in the app's OnInit (wx_app.cpp) — doing
    // it here lazily was fragile (see that comment).  Suppress wx's error log
    // around the load so a missing/odd art file degrades to "no image" instead of
    // popping an error dialog ("Unknown image data format") at the user.
    wxString path = FindResource(wxT("dialog_art") + wxString(wxFILE_SEP_PATH) + file);
    if (!wxFileName::FileExists(path)) return wxBitmap();
    wxImage img;
    wxLogNull noLog;
    if (!img.LoadFile(path)) return wxBitmap();
    return wxBitmap(img);
}

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
    WS_VISIBLE    = 0x10000000,
    WS_DISABLED   = 0x08000000,
    BS_TYPEMASK   = 0x0000000F,
    BS_DEFPUSHBUTTON = 1, BS_CHECKBOX = 2, BS_AUTOCHECKBOX = 3,
    BS_RADIOBUTTON = 4, BS_3STATE = 5, BS_AUTO3STATE = 6, BS_GROUPBOX = 7,
    BS_AUTORADIOBUTTON = 9,
    ES_CENTER = 1, ES_RIGHT = 2, ES_MULTILINE = 4, ES_UPPERCASE = 8,
    ES_PASSWORD = 0x20, ES_AUTOHSCROLL = 0x80, ES_READONLY = 0x800,
    ES_NUMBER = 0x2000,
    SS_TYPEMASK = 0x1F, SS_CENTER = 1, SS_RIGHT = 2, SS_ICON = 3, SS_BITMAP = 14,
    CBS_SIMPLE = 1, CBS_DROPDOWN = 2, CBS_DROPDOWNLIST = 3,
    CBS_SORT = 0x100,
    CBS_OWNERDRAWFIXED = 0x10, CBS_OWNERDRAWVARIABLE = 0x20,
    CBS_OWNERDRAWMASK  = CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE,
    LBS_SORT = 0x2, LBS_EXTENDEDSEL = 0x800, LBS_MULTIPLESEL = 8,
    SBS_VERT = 1,
};

inline wxString U(const std::string& s) { return wxString::FromUTF8(s.c_str()); }

// Create the wx widget for one RcControl, already positioned in pixels.
// Returns the created window (added as a child of `parent`), or null if skipped.
wxWindow* CreateControl(wxWindow* parent, const RcControl& c,
                        const wxPoint& pos, const wxSize& size,
                        int imageResId = 0)
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
                w = new wxCheckBox(parent, id, U(c.text), pos, size);
                break;
            case BS_3STATE:   case BS_AUTO3STATE:
                // Three states, not two.  The trigger dialog's Bold / Italic /
                // Inverse style matchers are the users of this: unchecked means
                // "must not be bold", checked "must be bold", and the third
                // state "don't care", which is the DEFAULT.  Built as a plain
                // 2-state box, "don't care" was unreachable and read back as
                // "must not be", so every trigger saved from the dialog silently
                // demanded plain text.
                w = new wxCheckBox(parent, id, U(c.text), pos, size,
                                   wxCHK_3STATE | wxCHK_ALLOW_3RD_STATE_FOR_USER);
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
            case SS_ICON: case SS_BITMAP: // image statics (Tip bulb, About logo, tray icons)
            {
                wxBitmap bmp = imageResId ? LoadDialogArt(imageResId) : wxBitmap();
                if (bmp.IsOk())
                {
                    // Scale DOWN to fit the control rect (preserving aspect) so
                    // icons sit in their small boxes; never upscale, so a small
                    // bitmap (e.g. the bulb) shows at natural size top-left —
                    // matching the Windows SS_ICON/SS_BITMAP statics.
                    wxImage im = bmp.ConvertToImage();
                    if (size.x > 0 && size.y > 0 &&
                        (im.GetWidth() > size.x || im.GetHeight() > size.y))
                    {
                        double s = std::min((double) size.x / im.GetWidth(),
                                            (double) size.y / im.GetHeight());
                        im = im.Scale(std::max(1, (int)(im.GetWidth()  * s)),
                                      std::max(1, (int)(im.GetHeight() * s)),
                                      wxIMAGE_QUALITY_HIGH);
                    }
                    w = new wxStaticBitmap(parent, id, wxBitmap(im), pos);
                }
                else
                    w = new wxStaticText(parent, id, wxEmptyString, pos, size);
                break;
            }
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
        // ES_NUMBER: the Win32 control rejects every non-digit KEYSTROKE
        // (including the minus sign), so e.g. Recall's line count -- which has
        // no DDV to catch it at OK time -- can never even hold "-100" on
        // Windows.  wxFILTER_DIGITS is the same rule at the same layer.  Like
        // ES_NUMBER, it filters typing, not programmatic SetValue/DDX loads.
        if (c.style & ES_NUMBER)
            w->SetValidator(wxTextValidator(wxFILTER_DIGITS));
        // ES_UPPERCASE folds text to upper case (the Global Prefs locale
        // field is the one user).  Win32's edit folds EVERYTHING going in --
        // typing, paste and WM_SETTEXT alike -- where wxTextEntry::ForceUpper
        // on wxOSX is a native field formatter that only sees USER edits
        // (measured: a programmatic SetValue("en") stayed lower case), so a
        // DDX load would show the stored value unfolded.  Fold on the text
        // event instead: SetValue raises it synchronously and typing raises
        // it per edit, so both routes match Windows.  ChangeValue re-raises
        // nothing, so the handler cannot recurse.
        if (c.style & ES_UPPERCASE)
        {
            wxTextCtrl* tc = static_cast<wxTextCtrl*>(w);
            tc->Bind(wxEVT_TEXT, [tc](wxCommandEvent& e) {
                const wxString s  = tc->GetValue();
                const wxString up = s.Upper();
                if (up != s)
                {
                    const long ip = tc->GetInsertionPoint();
                    tc->ChangeValue(up);
                    tc->SetInsertionPoint(ip);
                }
                e.Skip();   // EN_CHANGE dispatch still reaches the dialog
            });
        }
    }
    else if (cls == "ComboBox")
    {
        // A combo's template height (cy) is the DROPPED-DOWN list height, not the
        // closed control height — using it verbatim makes the combo absurdly tall
        // and overlap its siblings.  Let wx pick the natural (closed) height.
        // Preset items recovered from the dialog's DLGINIT resource (e.g. the
        // scripting-language list, proxy type, alias/timer "send to").  Combos
        // that instead fill themselves in OnInitDialog have no DLGINIT items.
        const bool readOnly  = (c.style & 3) == CBS_DROPDOWNLIST;
        const bool ownerDraw = (c.style & CBS_OWNERDRAWMASK) != 0;

        if (ownerDraw && g_wxhook_make_owner_draw_combo)
        {
            // Owner-draw combo (CBS_OWNERDRAWFIXED / CBS_OWNERDRAWVARIABLE):
            // the OWNER paints each row, not the control — which is how the five
            // colour drop-downs in the recovered resources show a swatch of the
            // custom colour rather than the word "Custom3".  Built plain, the
            // canonical CColourComboBox::DrawItem never ran and every row was
            // just its name.
            //
            // Neither wxChoice (an NSPopUpButton here) nor wxComboBox can hand
            // item painting back to us, so this is the one place the bridge
            // picks a control class by STYLE rather than by class name: a
            // wxOwnerDrawnComboBox, whose OnDrawItem callback runs the canonical
            // DrawItem.  It is still a wxItemContainer, so the CComboBox hooks —
            // and therefore DDX — drive it through exactly the same item API as
            // the plain builds; see CB() in wx_mfc_impl.cpp.
            w = (wxWindow*) g_wxhook_make_owner_draw_combo(
                    parent, id, pos.x, pos.y, size.x, readOnly);
        }
        else if (readOnly)
        {
            // Read-only drop-down → a native pop-up menu (NSPopUpButton on
            // macOS): clicking anywhere opens the list, and the text is not
            // editable/selectable.  A wxCB_READONLY wxComboBox on macOS is still
            // an editable-style combo (click-on-text doesn't open it), which is
            // exactly the reported bug.
            w = new wxChoice(parent, id, pos,
                             wxSize(size.GetWidth(), -1), 0, nullptr,
                             // CBS_SORT, same deal as LBS_SORT below: wxChoice
                             // honours wxCB_SORT directly (wxChoiceBase::
                             // IsSorted), so Append lands each item at its
                             // alphabetical place -- utils.choose drop-downs.
                             (c.style & CBS_SORT) ? wxCB_SORT : 0);
        }
        else
        {
            // Editable combo (CBS_DROPDOWN / CBS_SIMPLE).
            w = new wxComboBox(parent, id, wxEmptyString, pos,
                               wxSize(size.GetWidth(), -1), 0, nullptr,
                               (c.style & CBS_SORT) ? wxCB_SORT : 0);
        }

        // DLGINIT preset items, appended through wxItemContainer so all three
        // control classes above are filled by the same code.
        if (wxItemContainer* items = dynamic_cast<wxItemContainer*>(w))
            for (const std::string& item : c.comboItems)
                items->Append(wxString::FromUTF8(item.c_str()));
    }
    else if (cls == "ListBox")
    {
        // The two Win32 multi-select styles are NOT the same model, and
        // collapsing both onto wxLB_EXTENDED broke the one dialog that uses the
        // simple one.  LBS_MULTIPLESEL: every plain click toggles that item and
        // leaves the rest alone (wxLB_MULTIPLE).  LBS_EXTENDEDSEL: the
        // Shift/Ctrl model where a plain click replaces the selection
        // (wxLB_EXTENDED).  Game ▸ Send to All Worlds (IDD_SEND_TO_ALL, style
        // 0x…010B) is LBS_MULTIPLESEL, so as wxLB_EXTENDED each world the user
        // clicked deselected the previous one and the message went to one world
        // instead of the several they thought they had picked.
        long st = (c.style & LBS_EXTENDEDSEL) ? wxLB_EXTENDED
                : (c.style & LBS_MULTIPLESEL) ? wxLB_MULTIPLE
                :                               wxLB_SINGLE;
        // LBS_SORT: Windows keeps the list alphabetised as AddString inserts
        // (and AddString returns the SORTED index, which wx's Append matches).
        // Consumers map rows via that returned index + SetItemData, so the
        // flag changes display order only -- but Choose Notepad, Complete Word
        // and Send to All Worlds all declare it, and without it their lists
        // showed in insertion order where Windows users see them alphabetised.
        if (c.style & LBS_SORT) st |= wxLB_SORT;
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
        //
        // This ONE mapping decides the control class for all 15 SysListView32
        // controls in the recovered resources — the five entity pages, the macro
        // list, the Plugins manager, four Plugin Wizard pages, the chat list,
        // the colour picker, the function list and both Lua choose-lists.
        //
        // It is deliberately wxListCtrl and NOT wxDataViewListCtrl, which is the
        // only way to get saved column ORDER working on macOS (wxListCtrl here
        // IS wxGenericListCtrl, whose Set/GetColumnsOrder are unimplemented
        // stubs in the shipped library).  That trade-off, what it would cost and
        // when to revisit it, is written up in docs/LIST-CONTROL-DECISION.md —
        // read it before changing this line.
        // ...but NOT all of them are in report mode, and assuming so made one
        // dialog useless.  The view is the low TWO bits (LVS_ICON 0,
        // LVS_REPORT 1, LVS_SMALLICON 2, LVS_LIST 3), and IDC_FUNCTIONS_LIST
        // (Game ▸ Functions List) is LVS_LIST — the only one of the fifteen
        // that is.  An LVS_LIST control has no columns and its owner never
        // calls InsertColumn, so forcing wxLC_REPORT gave it ZERO columns:
        // every InsertItem succeeded and returned a row index, and not one of
        // ~400 function names was visible, because there was no column to draw
        // them in.  The dialog opened empty, filtering did nothing and OK
        // stayed greyed (nothing selectable).
        const long view = c.style & 0x3;
        long st = (view == 0x3 /*LVS_LIST*/) ? wxLC_LIST : wxLC_REPORT;
        if (c.style & 0x4 /*LVS_SINGLESEL*/) st |= wxLC_SINGLE_SEL;
        if (c.style & 0x8000 /*LVS_NOSORTHEADER*/) st |= wxLC_NO_SORT_HEADER;
        w = new wxListCtrl(parent, id, pos, size, st);
    }
    else if (cls == "msctls_updown32")
    {
        // Spin button (CSpinButtonCtrl), e.g. margins / lines-per-page / timer
        // hour & minute fields.  Its "buddy" edit is wired in
        // PopulateWindowFromTemplate (UDS_AUTOBUDDY = the preceding control).
        if (g_wxhook_make_spin)
            w = (wxWindow*) g_wxhook_make_spin(parent, id, pos.x, pos.y, size.x, size.y);
        else
            w = new wxPanel(parent, id, pos, size);
    }
    else if (cls == "msctls_hotkey32")
    {
        // Key-capture control (CHotKeyCtrl), e.g. the "Show Key Name" dialog.
        if (g_wxhook_make_hotkey)
            w = (wxWindow*) g_wxhook_make_hotkey(parent, id, pos.x, pos.y, size.x, size.y);
        else
            w = new wxTextCtrl(parent, id, wxEmptyString, pos, size, wxTE_READONLY);
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
        // A template can omit WS_VISIBLE to ship a control the user never sees.
        // Gammon uses that to RETIRE a feature without deleting its plumbing:
        // the control, its DDX and its handler all survive, but Windows never
        // draws it.  The bridge used to ignore the bit, so the port rendered
        // four such controls -- users could tick "Auto Log when opening world"
        // and watch it persist and do nothing, because it does nothing on
        // Windows either.  Honour it, and they disappear on both platforms.
        //
        // Only 5 of the 1633 templated controls lack the bit, so this is a
        // narrow change, but see the commit for the one it costs us.
        if (!(c.style & WS_VISIBLE)) w->Hide();
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

    // Image statics carry no source in the decompiled template (the icon ordinal
    // was lost), so map them here: named ones by control id, and the Tray/Taskbar
    // page's anonymous (IDC_STATIC) preview icons by their order — they line up
    // with the radio choices MUSHclient / CD / spanner / clock / world / graph /
    // hands / mail / network / banana (IDR_MUSHCLTYPE then IDI_ICON1..9).
    static const int kTrayArt[10] = { 129, 288, 289, 290, 291, 292, 293, 294, 295, 296 };
    int trayIconSeen = 0;
    wxWindow* prevCtrl = nullptr;   // previous control, for spin → buddy (UDS_AUTOBUDDY)

    for (const RcControl& c : t.controls)
    {
        wxPoint pos = target->ConvertDialogToPixels(wxPoint(c.x, c.y));
        wxSize  sz  = target->ConvertDialogToPixels(wxSize(c.cx, c.cy));

        int imageResId = 0;
        if      (c.id == 2436) imageResId = 128;   // IDC_LOGO → IDR_MAINFRAME (About logo)
        else if (c.id == 2005) imageResId = 103;   // IDC_BULB → IDB_LIGHTBULB (Tip bulb)
        else if (c.className == "Static" && (c.style & SS_TYPEMASK) == SS_ICON &&
                 t.id == 317 /*IDD_GLOBAL_PREFSP11 (Tray/Taskbar)*/ && trayIconSeen < 10)
            imageResId = kTrayArt[trayIconSeen++];

        wxWindow* w = CreateControl(target, c, pos, sz, imageResId);
        // A spin button steps its buddy edit (the control created just before it,
        // per UDS_AUTOBUDDY); wire them now that both wx widgets exist.
        if (w && c.className == "msctls_updown32" && g_wxhook_bind_spin)
            g_wxhook_bind_spin(w, prevCtrl);
        if (w) prevCtrl = w;
    }
    target->SetMinClientSize(client);
}

// Build a standalone wxDialog from a recovered template.  `parent` may be null.
wxDialog* BuildWxDialogFromTemplate(wxWindow* parent, const RcDialog& t)
{
    // Honour WS_THICKFRAME (0x00040000) in the template: such dialogs are meant
    // to be resizeable on Windows (and have an OnSize handler that re-lays-out
    // their controls).  Add wxRESIZE_BORDER so the user can resize them here too.
    long style = wxDEFAULT_DIALOG_STYLE;
    if (t.style & 0x00040000 /*WS_THICKFRAME*/)
        style |= wxRESIZE_BORDER;

    wxDialog* dlg = new wxDialog(parent, wxID_ANY, U(t.caption),
                                 wxDefaultPosition, wxDefaultSize,
                                 style);
    PopulateWindowFromTemplate(dlg, t);
    dlg->SetClientSize(dlg->GetMinClientSize());
    // The template size is the smallest sensible layout; let the user grow the
    // dialog but not shrink controls into each other.
    if (style & wxRESIZE_BORDER)
        dlg->SetMinSize(dlg->GetSize());
    return dlg;
}
