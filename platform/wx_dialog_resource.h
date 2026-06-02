// platform/wx_dialog_resource.h
//
// Parsed in-memory form of the recovered MFC dialog/menu resources
// (platform/mushclient_dialogs.rc, decompiled from the localisation DLL).
// The wx MFC-dialog shim consumes these templates: CDialog::DoModal() looks up
// its IDD here and builds a wxDialog with controls keyed by their (numeric)
// resource ID.  Symbolic IDs in the .rc are resolved to the same numeric values
// the compiled dialogs/**/*.cpp use, via the resource.h #defines.
//
// Non-Windows only — Windows loads these resources from the localisation DLL.

#pragma once

#ifndef _WIN32

#include <string>
#include <vector>
#include <unordered_map>

// One control within a dialog template (an .rc CONTROL statement).
struct RcControl
{
    std::string  text;        // caption (already unescaped)
    int          id = 0;      // numeric resource id (IDC_STATIC == -1)
    std::string  className;   // "Button","Edit","Static","ListBox","ComboBox",
                              // "ScrollBar", or a custom WNDCLASS string
    unsigned int style = 0;   // full Win32 window style
    unsigned int exStyle = 0;
    int x = 0, y = 0, cx = 0, cy = 0;   // dialog units
    // Preset items for a ComboBox, recovered from the dialog's DLGINIT resource
    // (CB_ADDSTRING blobs).  Empty for non-combo controls or combos populated by
    // code (OnInitDialog AddString).
    std::vector<std::string> comboItems;
};

// A dialog template (an .rc DIALOGEX block).
struct RcDialog
{
    int          id = 0;
    int          x = 0, y = 0, cx = 0, cy = 0;   // dialog units
    unsigned int style = 0;
    unsigned int exStyle = 0;
    std::string  caption;
    std::string  fontFace;
    int          fontPt = 8;
    std::vector<RcControl> controls;
};

// A menu item / popup (an .rc MENUITEM / POPUP). Popups nest via children.
struct RcMenuItem
{
    std::string             text;
    int                     id = 0;        // 0 for popups/separators
    bool                    isPopup = false;
    bool                    isSeparator = false;
    std::vector<RcMenuItem> children;
};
struct RcMenu
{
    int                     id = 0;
    std::vector<RcMenuItem> items;
};

// Registry of all parsed dialog + menu templates, with symbol resolution.
class RcResources
{
public:
    // Build the symbol->id map from resource.h text (#define NAME value lines).
    void LoadSymbols(const std::string& resourceHText);

    // Parse an .rc body (dialogs + menus). Symbols must be loaded first.
    // Returns false only on gross structural failure; unknown symbols resolve
    // to 0 and are counted in UnresolvedSymbols().
    bool ParseRc(const std::string& rcText);

    const RcDialog* FindDialog(int id) const;
    const RcMenu*   FindMenu(int id) const;

    int  ResolveSymbol(const std::string& name, bool* found = nullptr) const;

    size_t DialogCount() const { return m_dialogs.size(); }
    size_t MenuCount()   const { return m_menus.size(); }
    size_t ControlCount() const;
    int    UnresolvedSymbols() const { return m_unresolved; }

private:
    std::unordered_map<std::string, int> m_symbols;
    std::unordered_map<int, RcDialog>    m_dialogs;
    std::unordered_map<int, RcMenu>      m_menus;
    int m_unresolved = 0;
};

#endif // !_WIN32
