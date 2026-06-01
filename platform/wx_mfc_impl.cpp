// platform/wx_mfc_impl.cpp
// wxWidgets-backed implementations of the MFC compatibility hook functions.
//
// This is the ONLY file that includes both <wx/wx.h> and the MUSHclient
// MFC-compatibility headers.  It bridges the two worlds via the function
// pointers declared in wx_mfc_hooks.h.
//
// Include order is critical:
//   1. wx headers first (define wxUSE_*, etc.)
//   2. Then MUSHclient headers (compat.h, MUSHclient.h, …)
// This avoids conflicts between wx's and our own min/max, MessageBox, etc.

#ifndef _WIN32

// ── wxWidgets ─────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/mdi.h>
#include <wx/config.h>     // wxConfig
#include <wx/spinctrl.h>
#include <wx/filename.h>
#include <wx/fontpicker.h>
#include <wx/clrpicker.h>  // wxColourPickerCtrl
#include <wx/dnd.h>        // wxFileDropTarget
#include <wx/dirdlg.h>     // wxDirDialog
#include <wx/notebook.h>   // wxNotebook (CMacrosDlg)
#include <wx/clipbrd.h>    // wxTheClipboard
#include <wx/dataobj.h>    // wxTextDataObject
#include <wx/colordlg.h>   // wxColourDialog
#include <wx/fontdlg.h>    // wxFontDialog, wxFontData
#include <wx/treebook.h>   // wxTreebook (World Properties property sheet)
#include <wx/listctrl.h>   // wxListCtrl (entity-manager object lists)
#if wxUSE_SOUND
#  include <wx/sound.h>    // wxSound (trigger / MSP audio)
#endif
#include <algorithm>
#include <unordered_map>
#include <map>
#include <string>
#include <vector>

// ── MUSHclient headers (MFC compat layer) ────────────────────────────────
// Must come AFTER wx so our stubs don't shadow wx internals inside this TU.
#include "stdafx.h"        // pulls in compat.h → mfc_ui.h → wx_mfc_hooks.h
#include "MUSHclient.h"    // CMUSHclientApp full definition
#include "mainfrm.h"       // CMainFrame full definition
#include "doc.h"           // CMUSHclientDoc (needed for hook_new_world)
#include "TextDocument.h"             // CTextDocument (for hook_create_text_window)
#include "platform/wx_world_child.h"  // CWorldChildFrame
#include "platform/wx_notepad.h"      // CWxNotepadFrame
#include "platform/wx_entity_dlgs.h" // CTriggerListDlg, CAliasListDlg, CTimerListDlg
#include "platform/wx_dialog_bridge.h" // MfcDialogResources, BuildWxDialogFromTemplate
#include "dialogs/world_prefs/TreePropertySheet.h" // port CTreePropertySheet (page list)
#include "scripting/scripting.h"     // CScriptEngine

// Lua C API — included after MUSHclient headers to avoid wx/macro conflicts.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Hook pointer definitions live in platform/wx_mfc_hooks_storage.cpp (compiled
// into both mushcore and mushapp) so that mushcli and tests can link without
// needing wx_mfc_impl.cpp.  wx_mfc_install_hooks() below assigns them all.

// ── Status bar pointer ─────────────────────────────────────────────────────
// Populated by hook_load_mdi_frame; used by hook_set_status.
static wxStatusBar* s_statusBar = nullptr;

// ── Doc → frame registry ───────────────────────────────────────────────────
// Maps each live CMUSHclientDoc* to its CWorldChildFrame so that hooks
// (e.g. hook_game_preferences) can look up the output pane without iterating
// the MDI child hierarchy (which is platform-dependent and unreliable).
static std::unordered_map<CMUSHclientDoc*, CWorldChildFrame*> s_docFrameMap;

// Forward declaration — defined in the World-list persistence section below.
static void world_list_remove(const std::string& path);

// ── Lua Immediate / Eval Dialog ───────────────────────────────────────────────
// Game → Execute Lua… opens this dialog for the currently active world.
// Sends a chunk of Lua code through the world's existing CScriptEngine (same
// lua_State as triggers/aliases/timers) so scripts can inspect and mutate live
// world state.  Results (return values) and errors are shown in the output area.
// The input field retains its text between calls so snippets can be re-run with
// minor edits; a dedicated Clear button wipes both panes.
class CLuaEvalDlg : public wxDialog
{
public:
    CLuaEvalDlg(wxWindow* parent, CMUSHclientDoc* doc)
        : wxDialog(parent, wxID_ANY, wxT("Execute Lua"),
                   wxDefaultPosition, wxSize(600, 420),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_doc(doc)
    {
        wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);

        // ── Code input area ────────────────────────────────────────────────
        top->Add(new wxStaticText(this, wxID_ANY, wxT("Lua code:")),
                 0, wxLEFT | wxTOP, 8);

        m_code = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxDefaultSize,
                                wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_PROCESS_TAB |
                                wxHSCROLL | wxTE_PROCESS_ENTER);
        m_code->SetMinSize(wxSize(-1, 120));
        // Monospace font for code.
        wxFont codeFont = wxFont(wxFontInfo(12).Family(wxFONTFAMILY_TELETYPE));
        m_code->SetFont(codeFont);
        top->Add(m_code, 1, wxEXPAND | wxALL, 8);

        // ── Button row ────────────────────────────────────────────────────
        wxBoxSizer* btnRow = new wxBoxSizer(wxHORIZONTAL);
        wxButton* execBtn  = new wxButton(this, wxID_OK,     wxT("Execute"));
        wxButton* clearBtn = new wxButton(this, wxID_CLEAR,  wxT("Clear"));
        wxButton* closeBtn = new wxButton(this, wxID_CANCEL, wxT("Close"));
        execBtn->SetDefault();
        btnRow->Add(execBtn,  0, wxRIGHT, 8);
        btnRow->Add(clearBtn, 0, wxRIGHT, 8);
        btnRow->AddStretchSpacer();
        btnRow->Add(closeBtn, 0);
        top->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        // ── Output / results area ─────────────────────────────────────────
        top->Add(new wxStaticText(this, wxID_ANY, wxT("Output:")),
                 0, wxLEFT, 8);
        m_output = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP |
                                  wxHSCROLL);
        m_output->SetMinSize(wxSize(-1, 140));
        m_output->SetFont(codeFont);
        m_output->SetBackgroundColour(wxColour(30, 30, 30));
        m_output->SetForegroundColour(wxColour(200, 200, 200));
        top->Add(m_output, 1, wxEXPAND | wxALL, 8);

        SetSizer(top);
        Layout();
        m_code->SetFocus();

        // ── Event handlers ────────────────────────────────────────────────
        execBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Execute(); });
        clearBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_code->Clear();
            m_output->Clear();
            m_code->SetFocus();
        });
        // Ctrl+Enter also executes (convenient alternative to clicking Execute).
        m_code->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& evt) {
            if (evt.GetKeyCode() == WXK_RETURN && evt.RawControlDown()) {
                Execute();
                return;
            }
            evt.Skip();
        });

        // Raise parent (MDI frame) on activate to stay above MDI children.
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& evt) {
            if (evt.GetActive() && GetParent()) {
                GetParent()->Raise();
                Raise();
            }
            evt.Skip();
        });
    }

private:
    void Execute()
    {
        wxString code = m_code->GetValue();
        if (code.IsEmpty()) return;

        CScriptEngine* eng = m_doc->GetScriptEngine();
        if (!eng || !eng->L) {
            m_output->AppendText(wxT("ERROR: No active Lua scripting engine.\n"));
            return;
        }

        lua_State* L = eng->L;
        std::string chunk = std::string(code.ToUTF8().data());

        // Wrap bare expressions in "return …" so simple queries like
        // "world.GetWorldName()" display their result without needing "return".
        // We try the original chunk first; if it fails to parse with a message
        // containing "<eof>" we retry with "return " prepended.
        int topBefore = lua_gettop(L);

        // Try loading as a statement block.
        int loadErr = luaL_loadbuffer(L, chunk.c_str(), chunk.size(), "=(eval)");
        if (loadErr) {
            // Parse error — try "return <expr>" to handle bare expressions.
            const char* errMsg = lua_tostring(L, -1);
            bool isEofErr = (errMsg && strstr(errMsg, "<eof>"));
            lua_pop(L, 1);  // pop the error

            if (isEofErr) {
                std::string exprChunk = "return " + chunk;
                loadErr = luaL_loadbuffer(L, exprChunk.c_str(), exprChunk.size(), "=(eval)");
            }
        }

        if (loadErr) {
            // Still a parse error.
            wxString msg = wxT("PARSE ERROR: ");
            if (const char* e = lua_tostring(L, -1)) msg += wxString::FromUTF8(e);
            msg += wxT("\n");
            lua_pop(L, 1);
            m_output->AppendText(msg);
            return;
        }

        // Execute with one base for error handler (traceback).
        lua_getglobal(L, "debug");
        lua_getfield(L, -1, "traceback");
        lua_remove(L, -2);  // remove "debug" table
        int tracebackIdx = lua_gettop(L) - 1;  // position of traceback fn before chunk fn

        // Stack: [chunk_fn, traceback_fn]  — need traceback *before* chunk fn.
        // Swap: move traceback to topBefore+1.
        lua_insert(L, topBefore + 1);  // traceback now just above original top

        int callErr = lua_pcall(L, 0, LUA_MULTRET, topBefore + 1);

        if (callErr) {
            wxString msg = wxT("ERROR: ");
            if (const char* e = lua_tostring(L, -1)) msg += wxString::FromUTF8(e);
            msg += wxT("\n");
            lua_settop(L, topBefore);  // restore stack
            m_output->AppendText(msg);
            return;
        }

        // Collect return values.
        int nResults = lua_gettop(L) - (topBefore + 1);  // +1 for the traceback fn
        wxString resultLine;
        for (int i = topBefore + 2; i <= lua_gettop(L); ++i) {
            if (!resultLine.IsEmpty()) resultLine += wxT("\t");
            int t = lua_type(L, i);
            if (t == LUA_TSTRING) {
                resultLine += wxString::FromUTF8(lua_tostring(L, i));
            } else if (t == LUA_TNUMBER) {
                resultLine += wxString::FromUTF8(lua_tostring(L, i));
            } else if (t == LUA_TBOOLEAN) {
                resultLine += lua_toboolean(L, i) ? wxT("true") : wxT("false");
            } else if (t == LUA_TNIL) {
                resultLine += wxT("nil");
            } else {
                // table, userdata, function, thread
                resultLine += wxString::FromUTF8(lua_typename(L, t));
                resultLine += wxString::Format(wxT(": %p"),
                    static_cast<const void*>(lua_topointer(L, i)));
            }
        }
        lua_settop(L, topBefore);  // restore stack cleanly

        if (nResults == 0) {
            m_output->AppendText(wxT("OK (no return values)\n"));
        } else {
            m_output->AppendText(wxT("=> ") + resultLine + wxT("\n"));
        }
    }

    CMUSHclientDoc* m_doc;
    wxTextCtrl*     m_code   = nullptr;
    wxTextCtrl*     m_output = nullptr;
};

// ── Macros & Keypad Dialog ────────────────────────────────────────────────────
// Configure → Macros & Keypad… — edit f-key macros and numpad macros for the
// currently active world.  Mirrors the Windows Keypad/Macro tab in world prefs.
//
// F-key macros live in doc->m_macros[MACRO_COUNT] (text) and
// doc->m_macro_type[MACRO_COUNT] (send type: REPLACE_COMMAND / SEND_NOW /
// ADD_TO_COMMAND).  Keypad macros live in doc->m_keypad[eKeypad_Max_Items]
// (text only — no separate type array).
class CMacrosDlg : public wxDialog
{
public:
    explicit CMacrosDlg(wxWindow* parent, CMUSHclientDoc* doc)
        : wxDialog(parent, wxID_ANY, wxT("Macros & Keypad"),
                   wxDefaultPosition, wxSize(560, 600),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_doc(doc)
    {
        wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);

        wxNotebook* nb = new wxNotebook(this, wxID_ANY);

        // ── Function Keys page ────────────────────────────────────────────
        {
            auto* page = new wxScrolledWindow(nb, wxID_ANY);
            page->SetScrollRate(0, 16);
            BuildFKeyPage(page);
            nb->AddPage(page, wxT("Function Keys"));
        }

        // ── Numpad page ───────────────────────────────────────────────────
        {
            auto* page = new wxScrolledWindow(nb, wxID_ANY);
            page->SetScrollRate(0, 16);
            BuildKpadPage(page);
            nb->AddPage(page, wxT("Numpad"));
        }

        top->Add(nb, 1, wxEXPAND | wxALL, 8);
        top->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        SetSizer(top);

        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& evt) {
            if (evt.GetActive() && GetParent()) {
                GetParent()->Raise();
                Raise();
            }
            evt.Skip();
        });

        Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
            if (evt.GetId() == wxID_OK) Apply();
            evt.Skip();  // let dialog close normally
        });
    }

private:
    // ── F-key table ───────────────────────────────────────────────────────
    // (label, macro index — use -1 for combinations that don't exist)
    struct FKeyDef { const char* label; int idx; };

    static const FKeyDef* FKeyTable()
    {
        // F1-F12 plain, Shift+F1-F12, Ctrl+F1-F12 (no Ctrl+F4 in MUSHclient).
        // Indexed so section headers can be inserted at rows 0, 12, 24.
        static const FKeyDef t[] = {
            // ── Plain ──────────────────────────────────────────────────────
            { "F1",          MAC_F1  },
            { "F2",          MAC_F2  },
            { "F3",          MAC_F3  },
            { "F4",          MAC_F4  },
            { "F5",          MAC_F5  },
            { "F6",          MAC_F6  },
            { "F7",          MAC_F7  },
            { "F8",          MAC_F8  },
            { "F9",          MAC_F9  },
            { "F10",         MAC_F10 },
            { "F11",         MAC_F11 },
            { "F12",         MAC_F12 },
            // ── Shift ──────────────────────────────────────────────────────
            { "Shift+F1",    MAC_SHIFT_F1  },
            { "Shift+F2",    MAC_SHIFT_F2  },
            { "Shift+F3",    MAC_SHIFT_F3  },
            { "Shift+F4",    MAC_SHIFT_F4  },
            { "Shift+F5",    MAC_SHIFT_F5  },
            { "Shift+F6",    MAC_SHIFT_F6  },
            { "Shift+F7",    MAC_SHIFT_F7  },
            { "Shift+F8",    MAC_SHIFT_F8  },
            { "Shift+F9",    MAC_SHIFT_F9  },
            { "Shift+F10",   MAC_SHIFT_F10 },
            { "Shift+F11",   MAC_SHIFT_F11 },
            { "Shift+F12",   MAC_SHIFT_F12 },
            // ── Ctrl ───────────────────────────────────────────────────────
            { "Ctrl+F1",     MAC_CTRL_F1   },
            { "Ctrl+F2",     MAC_CTRL_F2   },
            { "Ctrl+F3",     MAC_CTRL_F3   },
            { "Ctrl+F4",     -1            },   // does not exist in MUSHclient
            { "Ctrl+F5",     MAC_CTRL_F5   },
            { "Ctrl+F6",     MAC_CTRL_F6   },
            { "Ctrl+F7",     MAC_CTRL_F7   },
            { "Ctrl+F8",     MAC_CTRL_F8   },
            { "Ctrl+F9",     MAC_CTRL_F9   },
            { "Ctrl+F10",    MAC_CTRL_F10  },
            { "Ctrl+F11",    MAC_CTRL_F11  },
            { "Ctrl+F12",    MAC_CTRL_F12  },
        };
        return t;
    }
    static constexpr int kFKeyCount = 36;  // 3 groups × 12

    // ── Keypad table ──────────────────────────────────────────────────────
    struct KpadDef { const char* label; int idx; };

    static const KpadDef* KpadTable()
    {
        static const KpadDef t[] = {
            // ── Plain keypad ───────────────────────────────────────────────
            { "Num 0",       eKeypad_0    },
            { "Num 1",       eKeypad_1    },
            { "Num 2",       eKeypad_2    },
            { "Num 3",       eKeypad_3    },
            { "Num 4",       eKeypad_4    },
            { "Num 5",       eKeypad_5    },
            { "Num 6",       eKeypad_6    },
            { "Num 7",       eKeypad_7    },
            { "Num 8",       eKeypad_8    },
            { "Num 9",       eKeypad_9    },
            { "Num .",       eKeypad_Dot  },
            { "Num /",       eKeypad_Slash},
            { "Num *",       eKeypad_Star },
            { "Num -",       eKeypad_Dash },
            { "Num +",       eKeypad_Plus },
            // ── Ctrl+keypad ────────────────────────────────────────────────
            { "Ctrl+Num 0",  eCtrl_Keypad_0    },
            { "Ctrl+Num 1",  eCtrl_Keypad_1    },
            { "Ctrl+Num 2",  eCtrl_Keypad_2    },
            { "Ctrl+Num 3",  eCtrl_Keypad_3    },
            { "Ctrl+Num 4",  eCtrl_Keypad_4    },
            { "Ctrl+Num 5",  eCtrl_Keypad_5    },
            { "Ctrl+Num 6",  eCtrl_Keypad_6    },
            { "Ctrl+Num 7",  eCtrl_Keypad_7    },
            { "Ctrl+Num 8",  eCtrl_Keypad_8    },
            { "Ctrl+Num 9",  eCtrl_Keypad_9    },
            { "Ctrl+Num .",  eCtrl_Keypad_Dot  },
            { "Ctrl+Num /",  eCtrl_Keypad_Slash},
            { "Ctrl+Num *",  eCtrl_Keypad_Star },
            { "Ctrl+Num -",  eCtrl_Keypad_Dash },
            { "Ctrl+Num +",  eCtrl_Keypad_Plus },
        };
        return t;
    }
    static constexpr int kKpadCount = 30;  // eKeypad_Max_Items

    // ── Widget storage ────────────────────────────────────────────────────
    struct FKeyRow { wxTextCtrl* text = nullptr; wxChoice* type = nullptr; int idx = -1; };
    struct KpadRow { wxTextCtrl* text = nullptr; int idx = -1; };

    std::vector<FKeyRow> m_fkeyRows;  // kFKeyCount entries
    std::vector<KpadRow> m_kpadRows;  // kKpadCount entries

    // ── Page builders ─────────────────────────────────────────────────────
    void BuildFKeyPage(wxScrolledWindow* page)
    {
        // 3 columns: label (fixed) | text ctrl (expand) | type choice (fixed)
        wxFlexGridSizer* grid = new wxFlexGridSizer(0, 3, 4, 8);
        grid->AddGrowableCol(1, 1);

        static const char* kTypeLabels[] = { "Replace", "Send", "Add To" };

        const FKeyDef* defs = FKeyTable();
        m_fkeyRows.resize(kFKeyCount);

        // Helper: insert a bold separator row.
        auto addSep = [&](const wxString& label) {
            auto* st = new wxStaticText(page, wxID_ANY, label);
            wxFont f = st->GetFont(); f.MakeBold(); st->SetFont(f);
            grid->Add(st, 0, wxALIGN_CENTER_VERTICAL | wxTOP, 6);
            grid->AddSpacer(0);
            grid->AddSpacer(0);
        };

        for (int i = 0; i < kFKeyCount; ++i) {
            // Section headers at start of each group.
            if (i ==  0) addSep(wxT("Plain"));
            if (i == 12) addSep(wxT("Shift"));
            if (i == 24) addSep(wxT("Ctrl"));

            const FKeyDef& d = defs[i];
            bool exists = (d.idx >= 0);

            grid->Add(new wxStaticText(page, wxID_ANY, wxString::FromUTF8(d.label)),
                      0, wxALIGN_CENTER_VERTICAL);

            wxTextCtrl* tc = new wxTextCtrl(page, wxID_ANY,
                exists ? wxString::FromUTF8(static_cast<const char*>(m_doc->m_macros[d.idx]))
                       : wxString());
            if (!exists) tc->Enable(false);
            grid->Add(tc, 1, wxEXPAND);

            wxChoice* ch = new wxChoice(page, wxID_ANY);
            ch->Append(wxT("Replace"));
            ch->Append(wxT("Send"));
            ch->Append(wxT("Add To"));
            if (exists) {
                int sel = (int)m_doc->m_macro_type[d.idx];
                ch->SetSelection(sel >= 0 && sel < 3 ? sel : 1 /*SEND_NOW*/);
            } else {
                ch->SetSelection(1);
                ch->Enable(false);
            }
            grid->Add(ch, 0, wxALIGN_CENTER_VERTICAL);

            m_fkeyRows[i] = { tc, ch, d.idx };
        }

        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);
        vs->Add(grid, 1, wxEXPAND | wxALL, 8);
        page->SetSizer(vs);
        page->FitInside();
    }

    void BuildKpadPage(wxScrolledWindow* page)
    {
        // 2 columns: label (fixed) | text ctrl (expand)
        wxFlexGridSizer* grid = new wxFlexGridSizer(0, 2, 4, 8);
        grid->AddGrowableCol(1, 1);

        const KpadDef* defs = KpadTable();
        m_kpadRows.resize(kKpadCount);

        // Section headers.
        auto addSep = [&](const wxString& label) {
            auto* st = new wxStaticText(page, wxID_ANY, label);
            wxFont f = st->GetFont(); f.MakeBold(); st->SetFont(f);
            grid->Add(st, 0, wxALIGN_CENTER_VERTICAL | wxTOP, 6);
            grid->AddSpacer(0);
        };

        for (int i = 0; i < kKpadCount; ++i) {
            if (i ==  0) addSep(wxT("Plain"));
            if (i == 15) addSep(wxT("Ctrl"));

            const KpadDef& d = defs[i];
            grid->Add(new wxStaticText(page, wxID_ANY, wxString::FromUTF8(d.label)),
                      0, wxALIGN_CENTER_VERTICAL);

            wxTextCtrl* tc = new wxTextCtrl(page, wxID_ANY,
                wxString::FromUTF8(static_cast<const char*>(m_doc->m_keypad[d.idx])));
            grid->Add(tc, 1, wxEXPAND);
            m_kpadRows[i] = { tc, d.idx };
        }

        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);
        vs->Add(grid, 1, wxEXPAND | wxALL, 8);
        page->SetSizer(vs);
        page->FitInside();
    }

    // ── Apply changes to doc ──────────────────────────────────────────────
    void Apply()
    {
        for (const FKeyRow& r : m_fkeyRows) {
            if (r.idx < 0 || !r.text) continue;
            m_doc->m_macros[r.idx]     = CString(r.text->GetValue().ToUTF8().data());
            m_doc->m_macro_type[r.idx] = static_cast<unsigned short>(
                std::max(0, r.type ? r.type->GetSelection() : 1));
        }
        for (const KpadRow& r : m_kpadRows) {
            if (r.idx < 0 || !r.text) continue;
            m_doc->m_keypad[r.idx] = CString(r.text->GetValue().ToUTF8().data());
        }
        m_doc->SetModifiedFlag(TRUE);
    }

    CMUSHclientDoc* m_doc;
};

// ── ANSI Color Palette Dialog ─────────────────────────────────────────────────
// Configure → Colors… — edit the 8 normal and 8 bright ANSI colors for the
// current world.  Changes affect GetStyleRGB color conversion immediately but
// only take visual effect on text received after the dialog is closed (already-
// buffered lines in the output pane retain the colors they were rendered with).
//
// Colors are stored as Windows COLORREFs (0x00BBGGRR) in doc->m_normalcolour[8]
// and doc->m_boldcolour[8] (indices 0-7 = Black, Red, Green, Yellow, Blue,
// Magenta, Cyan, White).  The wxColourPickerCtrl converts RGB↔COLORREF.
class CColorsDlg : public wxDialog
{
public:
    explicit CColorsDlg(wxWindow* parent, CMUSHclientDoc* doc)
        : wxDialog(parent, wxID_ANY, wxT("ANSI Color Palette"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE)
        , m_doc(doc)
    {
        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);

        // ── Header row ────────────────────────────────────────────────────
        {
            wxFlexGridSizer* hdr = new wxFlexGridSizer(1, 3, 0, 8);
            hdr->AddGrowableCol(0, 1);
            hdr->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));  // spacer
            auto* n = new wxStaticText(this, wxID_ANY, wxT("Normal"));
            auto* b = new wxStaticText(this, wxID_ANY, wxT("Bright / Bold"));
            wxFont f = n->GetFont(); f.MakeBold(); n->SetFont(f); b->SetFont(f);
            hdr->Add(n, 0, wxALIGN_CENTER);
            hdr->Add(b, 0, wxALIGN_CENTER);
            vs->Add(hdr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        }

        // ── Color rows (3 columns: label | normal picker | bold picker) ──
        wxFlexGridSizer* grid = new wxFlexGridSizer(8, 3, 6, 10);
        grid->AddGrowableCol(0, 1);

        static const char* kNames[] = {
            "Black", "Red", "Green", "Yellow",
            "Blue",  "Magenta", "Cyan", "White"
        };

        for (int i = 0; i < 8; ++i) {
            grid->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8(kNames[i])),
                      0, wxALIGN_CENTER_VERTICAL);

            // Normal color
            COLORREF nr = doc->m_normalcolour[i];
            wxColour nc(GetRValue(nr), GetGValue(nr), GetBValue(nr));
            m_normal[i] = new wxColourPickerCtrl(this, wxID_ANY, nc,
                              wxDefaultPosition, wxSize(60, -1),
                              wxCLRP_DEFAULT_STYLE | wxCLRP_SHOW_LABEL);
            grid->Add(m_normal[i], 0, wxALIGN_CENTER_VERTICAL);

            // Bold/bright color
            COLORREF br = doc->m_boldcolour[i];
            wxColour bc(GetRValue(br), GetGValue(br), GetBValue(br));
            m_bold[i] = new wxColourPickerCtrl(this, wxID_ANY, bc,
                            wxDefaultPosition, wxSize(60, -1),
                            wxCLRP_DEFAULT_STYLE | wxCLRP_SHOW_LABEL);
            grid->Add(m_bold[i], 0, wxALIGN_CENTER_VERTICAL);
        }
        vs->Add(grid, 0, wxEXPAND | wxALL, 12);

        // ── Hint ─────────────────────────────────────────────────────────
        auto* hint = new wxStaticText(this, wxID_ANY,
            wxT("Color changes apply to newly received text."));
        wxFont hf = hint->GetFont();
        hf.SetPointSize(std::max(8, hf.GetPointSize() - 1));
        hint->SetFont(hf);
        vs->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        // ── Restore defaults button + OK/Cancel ───────────────────────────
        wxBoxSizer* btnRow = new wxBoxSizer(wxHORIZONTAL);
        wxButton* defBtn = new wxButton(this, wxID_ANY, wxT("Restore Defaults"));
        defBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RestoreDefaults(); });
        btnRow->Add(defBtn, 0);
        btnRow->AddStretchSpacer();
        btnRow->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0);
        vs->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        SetSizerAndFit(vs);

        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& evt) {
            if (evt.GetActive() && GetParent()) {
                GetParent()->Raise();
                Raise();
            }
            evt.Skip();
        });

        Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
            if (evt.GetId() == wxID_OK) Apply();
            evt.Skip();
        });
    }

private:
    void RestoreDefaults()
    {
        static const COLORREF kNormal[8] = {
            RGB(  0,   0,   0),   // Black
            RGB(128,   0,   0),   // Red
            RGB(  0, 128,   0),   // Green
            RGB(128, 128,   0),   // Yellow
            RGB(  0,   0, 128),   // Blue
            RGB(128,   0, 128),   // Magenta
            RGB(  0, 128, 128),   // Cyan
            RGB(192, 192, 192),   // White
        };
        static const COLORREF kBold[8] = {
            RGB(128, 128, 128),   // Bold Black  (dark grey)
            RGB(255,   0,   0),   // Bold Red
            RGB(  0, 255,   0),   // Bold Green
            RGB(255, 255,   0),   // Bold Yellow
            RGB(  0,   0, 255),   // Bold Blue
            RGB(255,   0, 255),   // Bold Magenta
            RGB(  0, 255, 255),   // Bold Cyan
            RGB(255, 255, 255),   // Bold White
        };
        for (int i = 0; i < 8; ++i) {
            m_normal[i]->SetColour(wxColour(GetRValue(kNormal[i]),
                                            GetGValue(kNormal[i]),
                                            GetBValue(kNormal[i])));
            m_bold[i]->SetColour(wxColour(GetRValue(kBold[i]),
                                          GetGValue(kBold[i]),
                                          GetBValue(kBold[i])));
        }
    }

    void Apply()
    {
        for (int i = 0; i < 8; ++i) {
            wxColour nc = m_normal[i]->GetColour();
            wxColour bc = m_bold[i]->GetColour();
            m_doc->m_normalcolour[i] = RGB(nc.Red(), nc.Green(), nc.Blue());
            m_doc->m_boldcolour[i]   = RGB(bc.Red(), bc.Green(), bc.Blue());
        }
        m_doc->SetModifiedFlag(TRUE);
    }

    CMUSHclientDoc*      m_doc;
    wxColourPickerCtrl*  m_normal[8];
    wxColourPickerCtrl*  m_bold[8];
};

// ── Recent Worlds list ────────────────────────────────────────────────────────
// Tracks up to kMaxRecentWorlds file paths most recently opened or saved,
// persisted to wxConfig at /recent/0 … /recent/N-1.  Shown in File → Open
// Recent so the user can quickly reopen worlds without browsing.
//
// This is distinct from App.m_strWorldList (auto-reopen on launch): that list
// controls what opens automatically; the Recent menu is a manual affordance.

static const int  kMaxRecentWorlds = 10;
static const int  kRecentMenuBase  = wxID_HIGHEST + 300;

// ── Configure menu IDs ────────────────────────────────────────────────────────
static const int ID_MENU_CFG_TRIGGERS  = wxID_HIGHEST + 400;
static const int ID_MENU_CFG_ALIASES   = wxID_HIGHEST + 401;
static const int ID_MENU_CFG_TIMERS    = wxID_HIGHEST + 402;
static const int ID_MENU_CFG_VARIABLES = wxID_HIGHEST + 403;
static const int ID_MENU_CFG_MACROS    = wxID_HIGHEST + 404;
static const int ID_MENU_CFG_COLORS    = wxID_HIGHEST + 405;

// Import / Export set IDs (one pair per set type)
static const int ID_MENU_IMPORT_TRIGGERS = wxID_HIGHEST + 420;
static const int ID_MENU_EXPORT_TRIGGERS = wxID_HIGHEST + 421;
static const int ID_MENU_IMPORT_ALIASES  = wxID_HIGHEST + 422;
static const int ID_MENU_EXPORT_ALIASES  = wxID_HIGHEST + 423;
static const int ID_MENU_IMPORT_TIMERS   = wxID_HIGHEST + 424;
static const int ID_MENU_EXPORT_TIMERS   = wxID_HIGHEST + 425;
static const int ID_MENU_IMPORT_COLORS   = wxID_HIGHEST + 426;
static const int ID_MENU_EXPORT_COLORS   = wxID_HIGHEST + 427;
static const int ID_MENU_IMPORT_MACROS   = wxID_HIGHEST + 428;
static const int ID_MENU_EXPORT_MACROS   = wxID_HIGHEST + 429;

// ── Connection / logging IDs ──────────────────────────────────────────────────
static const int ID_CONN_LOG_TOGGLE    = wxID_HIGHEST + 410;

// ── Game menu IDs ─────────────────────────────────────────────────────────────
static const int ID_MENU_GAME_LUA_EVAL = wxID_HIGHEST + 411;
static wxMenu*    s_recentMenu     = nullptr;
static std::vector<std::string> s_recentWorlds;

// Load the recent list from config into s_recentWorlds.
static void recent_worlds_load()
{
    s_recentWorlds.clear();
    wxConfig cfg(wxT("mushclient"));
    for (int i = 0; i < kMaxRecentWorlds; ++i) {
        wxString key = wxString::Format(wxT("/recent/%d"), i);
        wxString val;
        if (!cfg.Read(key, &val) || val.IsEmpty()) break;
        s_recentWorlds.push_back(std::string(val.ToUTF8()));
    }
}

// Persist the current s_recentWorlds to config.
static void recent_worlds_save()
{
    wxConfig cfg(wxT("mushclient"));
    for (int i = 0; i < kMaxRecentWorlds; ++i) {
        wxString key = wxString::Format(wxT("/recent/%d"), i);
        if (i < (int)s_recentWorlds.size())
            cfg.Write(key, wxString::FromUTF8(s_recentWorlds[i].c_str()));
        else
            cfg.DeleteEntry(key);
    }
}

// Rebuild s_recentMenu from s_recentWorlds.
static void recent_worlds_update_menu()
{
    if (!s_recentMenu) return;
    while (s_recentMenu->GetMenuItemCount() > 0)
        s_recentMenu->Delete(s_recentMenu->FindItemByPosition(0));
    if (s_recentWorlds.empty()) {
        wxMenuItem* dummy = s_recentMenu->Append(kRecentMenuBase,
                                                  wxT("(no recent worlds)"));
        dummy->Enable(false);
        return;
    }
    for (int i = 0; i < (int)s_recentWorlds.size(); ++i) {
        wxString path  = wxString::FromUTF8(s_recentWorlds[i].c_str());
        wxString label = wxString::Format(wxT("&%d  %s"),
                                          (i + 1) % 10,
                                          wxFileName(path).GetFullName());
        s_recentMenu->Append(kRecentMenuBase + i, label);
    }
}

// Prepend path to s_recentWorlds (deduplicated), persist, and update the menu.
static void recent_worlds_add(const std::string& path)
{
    if (path.empty()) return;
    s_recentWorlds.erase(
        std::remove(s_recentWorlds.begin(), s_recentWorlds.end(), path),
        s_recentWorlds.end());
    s_recentWorlds.insert(s_recentWorlds.begin(), path);
    if ((int)s_recentWorlds.size() > kMaxRecentWorlds)
        s_recentWorlds.resize(kMaxRecentWorlds);
    recent_worlds_save();
    recent_worlds_update_menu();
}

void wx_register_world_frame(CMUSHclientDoc* doc, CWorldChildFrame* frame)
{
    if (doc) s_docFrameMap[doc] = frame;
}

void wx_unregister_world_frame(CMUSHclientDoc* doc)
{
    if (doc) s_docFrameMap.erase(doc);
}

void wx_world_remove_from_list(const char* path)
{
    if (path && *path) world_list_remove(path);
}

// ── Helper: cast void* → wxWindow* ───────────────────────────────────────
static inline wxWindow* toWx(void* h) { return static_cast<wxWindow*>(h); }
static inline const wxWindow* toWxC(const void* h) { return static_cast<const wxWindow*>(h); }

// ── Helper: apply world font settings to an output pane ──────────────────
// ── World-list persistence ────────────────────────────────────────────────
// App.m_strWorldList is a '*'-delimited list of world file paths that are
// opened automatically on startup.  These helpers keep it up-to-date and
// flush changes to the preferences SQLite DB so they survive app restarts.

static void world_list_add(const std::string& path)
{
    if (path.empty()) return;

    // Parse the current list; skip if already present.
    std::string cur = static_cast<const char*>(App.m_strWorldList);
    if (!cur.empty()) {
        // Check for exact match of any '*'-separated entry.
        std::string check = cur;
        while (!check.empty()) {
            size_t sep = check.find('*');
            std::string entry = (sep == std::string::npos) ? check : check.substr(0, sep);
            if (entry == path) return;  // already in list
            if (sep == std::string::npos) break;
            check = check.substr(sep + 1);
        }
        cur += '*';
    }
    cur += path;
    App.m_strWorldList = CString(cur.c_str());
    App.db_write_string("prefs", "WorldList",
                        static_cast<const char*>(App.m_strWorldList));
}

static void world_list_remove(const std::string& path)
{
    if (path.empty()) return;

    std::string cur = static_cast<const char*>(App.m_strWorldList);
    if (cur.empty()) return;

    // Rebuild the '*'-delimited list, skipping the given path.
    std::string rebuilt;
    std::string check = cur;
    while (!check.empty()) {
        size_t sep = check.find('*');
        std::string entry = (sep == std::string::npos) ? check : check.substr(0, sep);
        if (entry != path) {
            if (!rebuilt.empty()) rebuilt += '*';
            rebuilt += entry;
        }
        if (sep == std::string::npos) break;
        check = check.substr(sep + 1);
    }
    if (rebuilt == cur) return;  // nothing changed
    App.m_strWorldList = CString(rebuilt.c_str());
    App.db_write_string("prefs", "WorldList",
                        static_cast<const char*>(App.m_strWorldList));
}

// ── Hook implementations ──────────────────────────────────────────────────

static void hook_invalidate(void* h, bool erase)
{
    if (auto* w = toWx(h)) w->Refresh(erase);
}

static void hook_show_window(void* h, int nCmd)
{
    if (auto* w = toWx(h)) {
        // SW_SHOW / SW_SHOWNORMAL → Show; SW_HIDE → Hide
        w->Show(nCmd != 0 /* SW_HIDE == 0 */);
        // SW_MAXIMIZE
        if (nCmd == 3 /*SW_MAXIMIZE*/)
            if (auto* tl = wxDynamicCast(w, wxTopLevelWindow))
                tl->Maximize();
    }
}

static void hook_update_window(void* h)
{
    if (auto* w = toWx(h)) { w->Update(); w->Refresh(); }
}

static void hook_get_client_rect(const void* h, RECT* r)
{
    if (!r) return;
    if (auto* w = toWxC(h)) {
        wxSize sz = w->GetClientSize();
        r->left = 0; r->top = 0;
        r->right = sz.x; r->bottom = sz.y;
    } else {
        r->left = r->top = r->right = r->bottom = 0;
    }
}

static void hook_set_window_text(void* h, const char* text)
{
    auto* w = toWx(h);
    if (!w) return;
    wxString s = wxString::FromUTF8(text ? text : "");
    // Static text: set the label then re-wrap to the control's (fixed) width,
    // since MFC statics word-wrap within their rectangle and wxStaticText clips.
    if (wxStaticText* st = wxDynamicCast(w, wxStaticText))
    {
        int width = st->GetSize().GetWidth();
        st->SetLabel(s);
        if (width > 0) st->Wrap(width);
    }
    else
        w->SetLabel(s);
}

static void hook_move_window(void* h, int x, int y, int w, int ht, bool repaint)
{
    if (auto* wnd = toWx(h)) {
        wnd->SetSize(x, y, w, ht);
        if (repaint) wnd->Refresh();
    }
}

static void hook_destroy_window(void* h)
{
    if (auto* w = toWx(h)) w->Destroy();
}

static void hook_set_focus(void* h)
{
    if (auto* w = toWx(h)) w->SetFocus();
}

// Helper: resolve the target window, falling back to the MDI parent frame
// when h is null.  Frame.m_hWnd is always null on non-Windows (the real
// window is stored in App.m_pMainWnd->m_hWnd by hook_load_mdi_frame).
static wxWindow* resolveTarget(void* h)
{
    if (h) return toWx(h);
    return (App.m_pMainWnd && App.m_pMainWnd->m_hWnd)
         ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
         : nullptr;
}

static long hook_send_message(void* h, unsigned msg, uintptr_t wp, intptr_t lp)
{
    // Route WM_COMMAND messages to the wx event system.
    // Other messages are currently ignored (they trigger Windows-only paths).
    if (msg == WM_COMMAND) {
        if (auto* w = resolveTarget(h)) {
            wxCommandEvent evt(wxEVT_MENU, (int)wp);
            w->GetEventHandler()->ProcessEvent(evt);
        }
    }
    (void)lp;
    return 0;
}

static int hook_post_message(void* h, unsigned msg, uintptr_t wp, intptr_t lp)
{
    if (msg == WM_COMMAND) {
        if (auto* w = resolveTarget(h)) {
            wxCommandEvent* evt = new wxCommandEvent(wxEVT_MENU, (int)wp);
            w->GetEventHandler()->QueueEvent(evt);
            (void)lp;
            return TRUE;
        }
    }

    // ── TLS handshake failure fallback prompt ─────────────────────────────────
    // Sent by doc.cpp::ContinueSSLHandshake() when the TLS handshake fails and
    // the user should be asked whether to retry without encryption.
    // wp is the CMUSHclientDoc* that failed; we defer the dialog to the next
    // event-loop iteration so we're not inside a socket callback stack.
    if (msg == WM_USER_SSL_FALLBACK_PROMPT && wp != 0) {
        CMUSHclientDoc* doc = reinterpret_cast<CMUSHclientDoc*>(wp);
        wxWindow* parent = App.m_pMainWnd
                         ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                         : nullptr;
        wxTheApp->CallAfter([doc, parent]() {
            // Verify the doc is still live before touching it.
            if (s_docFrameMap.find(doc) == s_docFrameMap.end())
                return;
            wxString prompt = wxString::Format(
                wxT("TLS connection to \"%s\" failed.\n\n%s\n\n"
                    "Connect without encryption?"),
                wxString::FromUTF8(static_cast<const char*>(doc->m_mush_name)),
                wxString::FromUTF8(static_cast<const char*>(doc->m_strSSLLastError)));
            if (wxMessageBox(prompt, wxT("TLS Connection Failed"),
                             wxYES_NO | wxICON_QUESTION, parent) == wxYES)
            {
                doc->m_bUseSSL = false;
                doc->ConnectSocket();
            }
        });
        (void)lp;
        return TRUE;
    }

    (void)lp;
    return FALSE;
}

static int hook_message_box(const char* text, const char* caption, unsigned flags)
{
    // Map common MB_* flags to wxMessageBox styles.
    long style = wxOK;
    if (flags & 0x0001 /*MB_OKCANCEL*/)   style = wxOK | wxCANCEL;
    if (flags & 0x0002 /*MB_ABORTRETRYIGNORE*/) style = wxOK; // approximation
    if ((flags & 0x0003 /*MB_YESNOCANCEL*/) ||
        (flags & 0x0004 /*MB_YESNO*/))    style = wxYES_NO;
    if (flags & 0x0010 /*MB_ICONHAND*/)   style |= wxICON_ERROR;
    if (flags & 0x0020 /*MB_ICONQUESTION*/) style |= wxICON_QUESTION;
    if (flags & 0x0030 /*MB_ICONEXCLAMATION*/) style |= wxICON_WARNING;
    if (flags & 0x0040 /*MB_ICONINFORMATION*/) style |= wxICON_INFORMATION;

    wxString wxText    = text    ? wxString::FromUTF8(text)    : wxString();
    wxString wxCaption = caption ? wxString::FromUTF8(caption) : wxString("MUSHclient");

    int res = wxMessageBox(wxText, wxCaption, (int)style);

    // Map wx result to MB_* return codes.
    switch (res) {
    case wxOK:     return IDOK;
    case wxCANCEL: return IDCANCEL;
    case wxYES:    return IDYES;
    case wxNO:     return IDNO;
    default:       return IDOK;
    }
}

static void hook_set_status(const char* msg)
{
    if (s_statusBar)
        s_statusBar->SetStatusText(msg ? wxString::FromUTF8(msg) : wxString());
}

// ── MDI frame creation ────────────────────────────────────────────────────
// Called when CMDIFrameWnd::LoadFrame() is invoked on CMainFrame.
// Creates a real wxMDIParentFrame, builds a menu bar, and binds menu events
// via Bind() lambdas (avoids static event-table issues on macOS).

// Forward declarations — defined later in this file / in other TUs.
extern CMUSHclientApp App;
static void* hook_open_world_file(const char* path);
static void  hook_save_active_world(wxMDIParentFrame* frame, bool saveAs);
static void  hook_file_preferences(wxWindow* parent);

// ── File drop target ──────────────────────────────────────────────────────
// Lets the user drag MUSHclient world files (.mcl, .mck, .mcp) directly onto
// the main window to open them — a common macOS/Linux workflow.

namespace {
class WorldFileDropTarget : public wxFileDropTarget
{
public:
    bool OnDropFiles(wxCoord /*x*/, wxCoord /*y*/,
                     const wxArrayString& files) override
    {
        bool opened = false;
        for (const wxString& path : files) {
            wxString ext = wxFileName(path).GetExt().Lower();
            if (ext == "mcl" || ext == "mck" || ext == "mcp") {
                hook_open_world_file(path.ToUTF8().data());
                opened = true;
            }
        }
        return opened;
    }
};
} // anonymous namespace

static int hook_load_mdi_frame(void* selfVoid, unsigned /*id*/,
                                unsigned long /*style*/, void* /*parentHwnd*/)
{
    CWnd* self = static_cast<CWnd*>(selfVoid);

    // Create the top-level MDI parent window (two-phase for error detection).
    wxMDIParentFrame* frame = new wxMDIParentFrame();
    bool ok = frame->Create(
        nullptr,
        wxID_ANY,
        wxT("MUSHclient"),
        wxDefaultPosition,
        wxSize(1024, 768),
        wxDEFAULT_FRAME_STYLE);

    if (!ok) {
        frame->Destroy();
        return FALSE;
    }

    // ── Restore main window geometry ──────────────────────────────────────
    // Saved geometry survives app restarts via wxConfig.  We default to
    // 1024×768 centred on first run; subsequent runs restore the last size
    // and (on platforms that honour it) position.  Maximized state is also
    // preserved.  Iconized state is not (would be confusing on relaunch).
    {
        wxConfig cfg(wxT("mushclient"));
        long w = 1024, h = 768;
        long x = -1, y = -1;    // -1 = not saved; keep platform default
        bool maximized = false;
        cfg.Read(wxT("/geometry/main_w"), &w, 1024L);
        cfg.Read(wxT("/geometry/main_h"), &h,  768L);
        cfg.Read(wxT("/geometry/main_x"), &x, -1L);
        cfg.Read(wxT("/geometry/main_y"), &y, -1L);
        cfg.Read(wxT("/geometry/main_maximized"), &maximized, false);
        // Clamp to sanity: don't restore a 1×1 or off-screen window.
        if (w < 400) w = 1024;
        if (h < 300) h =  768;
        if (x >= 0 && y >= 0)
            frame->SetSize((int)x, (int)y, (int)w, (int)h);
        else
            frame->SetSize((int)w, (int)h);
        if (maximized)
            frame->Maximize(true);
    }

    // ── Menu bar ─────────────────────────────────────────────────────────
    wxMenuBar* mb = new wxMenuBar();

    wxMenu* fileMenu = new wxMenu();
    fileMenu->Append(0xE100,      wxT("New World\tCtrl+N"));
    fileMenu->Append(wxID_OPEN,   wxT("Open World File…\tCtrl+O"));
    // ── Open Recent submenu ───────────────────────────────────────────────
    // Populated immediately from config; updated each time a world is opened.
    s_recentMenu = new wxMenu();
    fileMenu->AppendSubMenu(s_recentMenu, wxT("Open Recent"));
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_SAVE,   wxT("Save World\tCtrl+S"));
    fileMenu->Append(wxID_SAVEAS, wxT("Save World As…"));
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_CLOSE,  wxT("Close World\tCtrl+Shift+W"));
    // Note: plain Ctrl+W is reserved for word-delete-backward in the input bar.
    fileMenu->AppendSeparator();
    fileMenu->Append(32246 /*ID_FILE_PREFERENCES*/, wxT("Preferences…"));
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   wxT("Quit MUSHclient\tCtrl+Q"));
    mb->Append(fileMenu, wxT("&File"));

    // ── Edit menu ─────────────────────────────────────────────────────────
    // Standard IDs (wxID_COPY etc.) are routed automatically by wxWidgets to
    // the focused window; no explicit handler is needed in our menu dispatcher.
    wxMenu* editMenu = new wxMenu();
    editMenu->Append(wxID_COPY,      wxT("Copy\tCtrl+C"));
    editMenu->Append(wxID_CUT,       wxT("Cut\tCtrl+X"));
    editMenu->Append(wxID_PASTE,     wxT("Paste\tCtrl+V"));
    editMenu->AppendSeparator();
    editMenu->Append(wxID_SELECTALL, wxT("Select All\tCtrl+A"));
    editMenu->AppendSeparator();
    editMenu->Append(wxID_FIND,      wxT("Find…\tCtrl+F"));
    mb->Append(editMenu, wxT("&Edit"));

    wxMenu* connMenu = new wxMenu();
    // Note: Ctrl+K, Ctrl+D, and Ctrl+R are standard Unix input-bar shortcuts
    // (kill-to-end, delete-forward, and reverse history search respectively) so
    // those keys are not used as menu accelerators here.
    connMenu->Append(32289,                     wxT("Quick Connect…\tCtrl+Shift+K"));
    connMenu->AppendSeparator();
    connMenu->Append(32239 /*ID_CONNECTION_CONNECT*/,    wxT("Connect"));
    connMenu->Append(0xE200,                    wxT("Disconnect"));
    connMenu->Append(0xE201,                    wxT("Reconnect\tCtrl+Shift+R"));
    connMenu->AppendSeparator();
    connMenu->Append(ID_CONN_LOG_TOGGLE,        wxT("Start &Logging…"));
    mb->Append(connMenu, wxT("&Connection"));

    wxMenu* gameMenu = new wxMenu();
    gameMenu->Append(32158 /*ID_GAME_PREFERENCES*/,     wxT("World Properties…\tCtrl+,"));
    gameMenu->AppendSeparator();
    gameMenu->Append(32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/, wxT("Reload Script"));
    gameMenu->Append(ID_MENU_GAME_LUA_EVAL,              wxT("Execute Lua…\tCtrl+Shift+L"));
    mb->Append(gameMenu, wxT("&Game"));

    // ── Configure menu ────────────────────────────────────────────────────
    wxMenu* cfgMenu = new wxMenu();
    cfgMenu->Append(ID_MENU_CFG_TRIGGERS,  wxT("&Triggers…\tCtrl+T"));
    cfgMenu->Append(ID_MENU_CFG_ALIASES,   wxT("&Aliases…\tCtrl+Shift+A"));
    cfgMenu->Append(ID_MENU_CFG_TIMERS,    wxT("T&imers…\tCtrl+Shift+I"));
    cfgMenu->AppendSeparator();
    cfgMenu->Append(ID_MENU_CFG_VARIABLES, wxT("&Variables…\tCtrl+Shift+V"));
    cfgMenu->AppendSeparator();
    cfgMenu->Append(ID_MENU_CFG_MACROS,   wxT("&Macros && Keypad…"));
    cfgMenu->Append(ID_MENU_CFG_COLORS,   wxT("&Colors…"));
    cfgMenu->AppendSeparator();
    // Import / Export submenus
    wxMenu* importMenu = new wxMenu();
    importMenu->Append(ID_MENU_IMPORT_TRIGGERS, wxT("&Triggers…"));
    importMenu->Append(ID_MENU_IMPORT_ALIASES,  wxT("&Aliases…"));
    importMenu->Append(ID_MENU_IMPORT_TIMERS,   wxT("T&imers…"));
    importMenu->Append(ID_MENU_IMPORT_COLORS,   wxT("&Colors…"));
    importMenu->Append(ID_MENU_IMPORT_MACROS,   wxT("&Macros…"));
    cfgMenu->AppendSubMenu(importMenu, wxT("&Import"));

    wxMenu* exportMenu = new wxMenu();
    exportMenu->Append(ID_MENU_EXPORT_TRIGGERS, wxT("&Triggers…"));
    exportMenu->Append(ID_MENU_EXPORT_ALIASES,  wxT("&Aliases…"));
    exportMenu->Append(ID_MENU_EXPORT_TIMERS,   wxT("T&imers…"));
    exportMenu->Append(ID_MENU_EXPORT_COLORS,   wxT("&Colors…"));
    exportMenu->Append(ID_MENU_EXPORT_MACROS,   wxT("&Macros…"));
    cfgMenu->AppendSubMenu(exportMenu, wxT("E&xport"));
    mb->Append(cfgMenu, wxT("&Configure"));

    // ── Window menu ───────────────────────────────────────────────────────
    // wxMDIParentFrame recognises a menu named exactly "&Window" and populates
    // it automatically with the list of open child frames on macOS/GTK.
    wxMenu* windowMenu = new wxMenu();
    windowMenu->Append(wxID_MDI_WINDOW_TILE_HORZ, wxT("Tile Horizontally"));
    windowMenu->Append(wxID_MDI_WINDOW_TILE_VERT, wxT("Tile Vertically"));
    windowMenu->Append(wxID_MDI_WINDOW_CASCADE,    wxT("Cascade"));
    windowMenu->AppendSeparator();
    windowMenu->Append(wxID_MDI_WINDOW_ARRANGE_ICONS, wxT("Arrange Icons"));
    mb->Append(windowMenu, wxT("&Window"));

    wxMenu* helpMenu = new wxMenu();
    helpMenu->Append(wxID_ABOUT, wxT("About MUSHclient"));
    mb->Append(helpMenu, wxT("&Help"));

    frame->SetMenuBar(mb);

    // ── Populate "Open Recent" submenu from persisted config ──────────────
    recent_worlds_load();
    recent_worlds_update_menu();

    // ── Status bar ────────────────────────────────────────────────────────
    s_statusBar = frame->CreateStatusBar(1);
    if (s_statusBar)
        s_statusBar->SetStatusText(wxT("Ready"));

    // ── Drag-and-drop world files ─────────────────────────────────────────
    // Ownership of the drop target transfers to the window.
    frame->SetDropTarget(new WorldFileDropTarget());

    // ── Socket polling timer ──────────────────────────────────────────────
    // CAsyncSocket::Poll() must be called periodically on the GUI thread to
    // deliver OnConnect / OnReceive / OnClose callbacks.  20 ms is a
    // responsive polling interval that keeps the UI smooth.
    wxTimer* pollTimer = new wxTimer(frame);
    frame->Bind(wxEVT_TIMER, [](wxTimerEvent&) {
        CAsyncSocket::Poll(0);
    }, pollTimer->GetId());
    pollTimer->Start(20);

    // ── App-level close confirmation ──────────────────────────────────────
    // EVT_CLOSE fires when the user closes the main window (via ×, Cmd+Q,
    // or our File → Quit handler).  If the global pref is set, ask before
    // destroying.  Also stop the poll timer here so it doesn't fire during
    // the child-frame teardown sequence.
    frame->Bind(wxEVT_CLOSE_WINDOW, [frame, pollTimer](wxCloseEvent& evt) {
        if (evt.CanVeto() && App.m_bConfirmBeforeClosingMushclient) {
            int answer = wxMessageBox(
                wxT("Are you sure you want to quit MUSHclient?"),
                wxT("Confirm Quit"),
                wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, frame);
            if (answer != wxYES) {
                evt.Veto();
                return;
            }
        }
        // Save main window geometry so the next session opens at the same size.
        if (!frame->IsIconized()) {
            wxConfig cfg(wxT("mushclient"));
            bool maximized = frame->IsMaximized();
            cfg.Write(wxT("/geometry/main_maximized"), maximized);
            if (!maximized) {
                wxRect r = frame->GetRect();
                cfg.Write(wxT("/geometry/main_x"), (long)r.x);
                cfg.Write(wxT("/geometry/main_y"), (long)r.y);
                cfg.Write(wxT("/geometry/main_w"), (long)r.width);
                cfg.Write(wxT("/geometry/main_h"), (long)r.height);
            }
        }
        // Stop the socket-poll timer before child frames are destroyed.
        if (pollTimer->IsRunning()) pollTimer->Stop();
        evt.Skip();  // proceed with default close (destroys child frames → the app)
    });

    // ── Menu update-UI: enable/disable items based on connection state ────
    // wxEVT_UPDATE_UI fires just before each menu item is shown, letting us
    // grey items that don't apply to the current state.
    frame->Bind(wxEVT_UPDATE_UI, [frame](wxUpdateUIEvent& evt) {
        // Helper: get the active child's doc, or nullptr.
        auto activeDoc = [frame]() -> CMUSHclientDoc* {
            wxMDIChildFrame* active = frame->GetActiveChild();
            if (!active) return nullptr;
            auto* child = dynamic_cast<CWorldChildFrame*>(active);
            return child ? child->GetDoc() : nullptr;
        };

        int id = evt.GetId();
        CMUSHclientDoc* doc = activeDoc();

        if (id == 32239 /*ID_CONNECTION_CONNECT*/) {
            // Enable only when disconnected and a world is open.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectNotConnected);
        } else if (id == 0xE200 /*Disconnect*/) {
            // Enable only when connected.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectConnectedToMud);
        } else if (id == 0xE201 /*Reconnect*/) {
            // Enable only when not connected.
            evt.Enable(doc && doc->m_iConnectPhase == eConnectNotConnected);
        } else if (id == wxID_SAVE || id == wxID_SAVEAS ||
                   id == wxID_CLOSE ||
                   id == 32158 /*ID_GAME_PREFERENCES*/ ||
                   id == ID_MENU_CFG_TRIGGERS  ||
                   id == ID_MENU_CFG_ALIASES   ||
                   id == ID_MENU_CFG_TIMERS    ||
                   id == ID_MENU_CFG_VARIABLES ||
                   id == ID_MENU_CFG_MACROS    ||
                   id == ID_MENU_CFG_COLORS    ||
                   id == ID_MENU_IMPORT_TRIGGERS ||
                   id == ID_MENU_EXPORT_TRIGGERS ||
                   id == ID_MENU_IMPORT_ALIASES  ||
                   id == ID_MENU_EXPORT_ALIASES  ||
                   id == ID_MENU_IMPORT_TIMERS   ||
                   id == ID_MENU_EXPORT_TIMERS   ||
                   id == ID_MENU_IMPORT_COLORS   ||
                   id == ID_MENU_EXPORT_COLORS   ||
                   id == ID_MENU_IMPORT_MACROS   ||
                   id == ID_MENU_EXPORT_MACROS) {
            // Need an active world.
            evt.Enable(doc != nullptr);
        } else if (id == ID_MENU_GAME_LUA_EVAL ||
                   id == 32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/) {
            // Need an active world with scripting enabled.
            evt.Enable(doc != nullptr && doc->m_bEnableScripts);
        } else if (id == ID_CONN_LOG_TOGGLE) {
            // Need an active world; label flips between Start/Stop.
            evt.Enable(doc != nullptr);
            if (doc) {
                bool logging = (doc->m_logfile != nullptr);
                evt.SetText(logging ? wxT("Stop &Logging") : wxT("Start &Logging…"));
            }
        } else {
            evt.Skip();  // default behaviour for all other items
        }
    });

    // ── Menu event routing via Bind() ─────────────────────────────────────
    frame->Bind(wxEVT_MENU, [frame](wxCommandEvent& evt) {
        int id = evt.GetId();

        // Helper: get the doc for the currently active MDI child, or nullptr.
        auto activeDoc = [frame]() -> CMUSHclientDoc* {
            wxMDIChildFrame* active = frame->GetActiveChild();
            if (!active) return nullptr;
            auto* child = dynamic_cast<CWorldChildFrame*>(active);
            return child ? child->GetDoc() : nullptr;
        };

        // ── Edit menu: Copy / Cut / Paste / Select All ─────────────────────
        // The frame's Bind intercepts menu events before wxMDI can route them
        // to the focused child control, so we dispatch explicitly here.
        // For a wxTextCtrl (input bar), post a standard wxCommandEvent so the
        // built-in edit handling fires.  For the output pane, call directly.
        if (id == wxID_COPY || id == wxID_CUT ||
            id == wxID_PASTE || id == wxID_SELECTALL)
        {
            wxWindow* focused = wxWindow::FindFocus();
            if (focused) {
                // Is it the output pane?
                if (auto* pane = dynamic_cast<COutputPane*>(focused)) {
                    if      (id == wxID_COPY)      pane->CopySelection();
                    else if (id == wxID_SELECTALL) pane->SelectAll();
                    // Cut and Paste are no-ops on a read-only output pane.
                } else {
                    // Forward to focused window (wxTextCtrl, etc.) via its
                    // own event handler so the built-in editing works.
                    wxCommandEvent fwd(wxEVT_MENU, id);
                    fwd.SetEventObject(focused);
                    focused->GetEventHandler()->ProcessEvent(fwd);
                }
            }
            return;   // consumed — do not call evt.Skip()
        }

        if (id == wxID_EXIT || id == 0xE141 /*ID_APP_EXIT*/) {
            frame->Close(false);  // false = veto-able so EVT_CLOSE handler can confirm
        } else if (id == wxID_NEW || id == 0xE100 /*ID_FILE_NEW*/
                   || id == 32289 /*ID_CONNECTION_QUICK_CONNECT*/) {
            App.OnFileNew();
        } else if (id == wxID_OPEN || id == 0xE101 /*ID_FILE_OPEN*/) {
            // Show a file-open dialog filtered to MUSHclient world files.
            // Start in the default world directory if one is configured.
            wxString defaultDir = wxString::FromUTF8(
                static_cast<const char*>(App.m_strDefaultWorldFileDirectory));
            wxFileDialog dlg(frame,
                             wxT("Open World File"),
                             defaultDir, wxEmptyString,
                             wxT("MUSHclient world files (*.mcl;*.mck;*.mcp)|*.mcl;*.mck;*.mcp|All files (*.*)|*.*"),
                             wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK)
                hook_open_world_file(dlg.GetPath().ToUTF8().data());
        } else if (id == wxID_CLOSE) {
            // Close the currently active MDI child (world window).
            if (wxMDIChildFrame* active = frame->GetActiveChild())
                active->Close();
        } else if (id == wxID_SAVE || id == 0xE103 /*ID_FILE_SAVE*/) {
            hook_save_active_world(frame, /*saveAs=*/false);
        } else if (id == wxID_SAVEAS || id == 0xE104 /*ID_FILE_SAVE_AS*/) {
            hook_save_active_world(frame, /*saveAs=*/true);
        } else if (id == wxID_FIND) {
            // Edit → Find…: show find bar in the active child frame.
            if (wxMDIChildFrame* active = frame->GetActiveChild()) {
                if (auto* child = dynamic_cast<CWorldChildFrame*>(active))
                    child->ShowFindBar(true);
            }
        } else if (id == 32239 /*ID_CONNECTION_CONNECT*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_iConnectPhase == eConnectNotConnected) {
                    doc->m_bDisconnectOK = false;
                    doc->ConnectSocket();
                }
            }
        } else if (id == 0xE200 /*Disconnect*/ ||
                   id == 32240 /*ID_CONNECTION_DISCONNECT*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                doc->DisconnectWorld();
                if (s_statusBar)
                    s_statusBar->SetStatusText(wxT("Disconnected."));
            }
        } else if (id == 0xE201 /*Reconnect*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_iConnectPhase == eConnectNotConnected) {
                    doc->m_bDisconnectOK = false;
                    doc->ConnectSocket();
                    if (s_statusBar)
                        s_statusBar->SetStatusText(wxT("Connecting…"));
                }
            }
        } else if (id == 32246 /*ID_FILE_PREFERENCES*/) {
            hook_file_preferences(frame);
        } else if (id == 32158 /*ID_GAME_PREFERENCES*/) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->GamePreferences(-1);
        } else if (id == 32290 /*ID_GAME_RELOAD_SCRIPT_FILE*/) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                if (doc->m_bEnableScripts || !doc->m_strScriptFilename.IsEmpty()) {
                    doc->DisableScripting();
                    doc->CreateScriptEngine();
                    if (s_statusBar)
                        s_statusBar->SetStatusText(wxT("Script reloaded."));
                } else {
                    wxMessageBox(wxT("No script file is configured for this world.\n"
                                     "Set one in Game → World Properties → Scripting."),
                                 wxT("Reload Script"), wxOK | wxICON_INFORMATION, frame);
                }
            }
        } else if (id == ID_MENU_GAME_LUA_EVAL) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CScriptEngine* eng = doc->GetScriptEngine();
                if (!eng || !eng->L) {
                    wxMessageBox(
                        wxT("No active Lua scripting engine for this world.\n"
                            "Enable scripting in Game → World Properties → Scripting."),
                        wxT("Execute Lua"), wxOK | wxICON_INFORMATION, frame);
                } else {
                    CLuaEvalDlg dlg(frame, doc);
                    dlg.ShowModal();
                }
            }
        } else if (id == wxID_ABOUT) {
            wxString ver = wxString::FromUTF8(
                static_cast<const char*>(MUSHCLIENT_VERSION));
            wxMessageBox(
                wxT("MUSHclient version ") + ver + wxT("\n")
                wxT("macOS/Linux port — wxWidgets GUI\n\n")
                wxT("Original Windows version by Nick Gammon\n")
                wxT("http://www.gammon.com.au/mushclient"),
                wxT("About MUSHclient"),
                wxOK | wxICON_INFORMATION, frame);
        } else if (id >= kRecentMenuBase &&
                   id <  kRecentMenuBase + kMaxRecentWorlds) {
            // File → Open Recent → pick a specific entry.
            int idx = id - kRecentMenuBase;
            if (idx < (int)s_recentWorlds.size())
                hook_open_world_file(s_recentWorlds[idx].c_str());
        } else if (id == ID_MENU_CFG_TRIGGERS) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CTriggerListDlg dlg(frame, doc);
                dlg.ShowModal();
            }
        } else if (id == ID_MENU_CFG_ALIASES) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CAliasListDlg dlg(frame, doc);
                dlg.ShowModal();
            }
        } else if (id == ID_MENU_CFG_TIMERS) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CTimerListDlg dlg(frame, doc);
                dlg.ShowModal();
            }
        } else if (id == ID_MENU_CFG_VARIABLES) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CVariableListDlg dlg(frame, doc);
                dlg.ShowModal();
            }
        } else if (id == ID_MENU_CFG_MACROS) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CMacrosDlg dlg(frame, doc);
                dlg.ShowModal();
            }
        } else if (id == ID_MENU_CFG_COLORS) {
            if (CMUSHclientDoc* doc = activeDoc()) {
                CColorsDlg dlg(frame, doc);
                dlg.ShowModal();
            }
        } else if (id == ID_MENU_IMPORT_TRIGGERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(TRIGGER, "", nullptr);
        } else if (id == ID_MENU_EXPORT_TRIGGERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(TRIGGER, nullptr);
        } else if (id == ID_MENU_IMPORT_ALIASES) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(ALIAS, "", nullptr);
        } else if (id == ID_MENU_EXPORT_ALIASES) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(ALIAS, nullptr);
        } else if (id == ID_MENU_IMPORT_TIMERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(TIMER, "", nullptr);
        } else if (id == ID_MENU_EXPORT_TIMERS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(TIMER, nullptr);
        } else if (id == ID_MENU_IMPORT_COLORS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(COLOUR, "", nullptr);
        } else if (id == ID_MENU_EXPORT_COLORS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(COLOUR, nullptr);
        } else if (id == ID_MENU_IMPORT_MACROS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Load_Set(MACRO, "", nullptr);
        } else if (id == ID_MENU_EXPORT_MACROS) {
            if (CMUSHclientDoc* doc = activeDoc())
                doc->Save_Set(MACRO, nullptr);
        } else if (id == ID_CONN_LOG_TOGGLE) {
            CMUSHclientDoc* doc = activeDoc();
            if (!doc) { evt.Skip(); return; }
            if (doc->m_logfile) {
                // Stop logging.
                fclose(doc->m_logfile);
                doc->m_logfile = nullptr;
                if (s_statusBar)
                    s_statusBar->SetStatusText(wxT("Logging stopped."));
            } else {
                // Start logging — ask for a file.
                wxString defaultName = wxString::FromUTF8(
                    static_cast<const char*>(doc->m_strAutoLogFileName));
                wxFileDialog dlg(frame,
                                 wxT("Log to File"),
                                 wxEmptyString, defaultName,
                                 wxT("Log files (*.log;*.txt)|*.log;*.txt|All files (*.*)|*.*"),
                                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
                if (dlg.ShowModal() != wxID_OK) return;
                wxString path = dlg.GetPath();
                doc->m_logfile_name = CString(path.ToUTF8().data());
                doc->m_logfile = fopen(static_cast<const char*>(doc->m_logfile_name), "a+");
                if (!doc->m_logfile) {
                    wxMessageBox(wxT("Could not open log file:\n") + path,
                                 wxT("Logging Error"), wxOK | wxICON_ERROR, frame);
                } else {
                    if (s_statusBar)
                        s_statusBar->SetStatusText(
                            wxT("Logging to: ") + path);
                }
            }
        } else {
            evt.Skip();
        }
    });

    // Store the wxWindow* in m_hWnd (HWND = void* on macOS/Linux)
    self->m_hWnd = static_cast<void*>(frame);

    // CMainFrame::OnCreate is a no-op on non-Windows (returns 0 immediately).
    CREATESTRUCT cs{};
    if (auto* mdiFrame = dynamic_cast<CMDIFrameWnd*>(self)) {
        if (mdiFrame->OnCreate(&cs) == -1) {
            frame->Destroy();
            self->m_hWnd = nullptr;
            return FALSE;
        }
    }

    return TRUE;
}

// ── Open World File ───────────────────────────────────────────────────────
// Creates a CMUSHclientDoc, calls OnOpenDocument(path) to parse the XML world
// file, then creates and shows a CWorldChildFrame for it.
// Returns the new doc on success, nullptr on failure (file not found, invalid
// XML, user pressed cancel on a nested dialog, etc.).
static void* hook_open_world_file(const char* path)
{
    wxMDIParentFrame* mdiParent = nullptr;
    if (App.m_pMainWnd)
        mdiParent = static_cast<wxMDIParentFrame*>(
                        static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd));
    if (!mdiParent || !path || !*path) return nullptr;

    // If this world file is already open, activate its window instead of opening
    // a second copy.  This happens when a world is BOTH in the auto-reopen
    // WorldList (opened by InitInstance) AND passed on the command line / double-
    // clicked / chosen from File > Recent.  Matches Windows MUSHclient, which
    // focuses the existing world rather than loading a duplicate.
    {
        char canon[4096];
        std::string keyNew = ::realpath(path, canon) ? canon : path;
        for (const auto& kv : s_docFrameMap) {
            if (!kv.first || !kv.second) continue;
            CString existing = kv.first->GetPathName();
            if (existing.IsEmpty()) continue;
            char canon2[4096];
            const char* ep = static_cast<const char*>((LPCTSTR) existing);
            std::string keyOld = ::realpath(ep, canon2) ? canon2 : ep;
            if (keyNew == keyOld) {
                kv.second->Activate();
                kv.second->Raise();
                return kv.first;            // already open → return the existing doc
            }
        }
    }

    CMUSHclientDoc* doc = new CMUSHclientDoc();

    // Initialise colour tables (same as hook_new_world; overridden by world file).
    doc->m_normalcolour[0] = RGB(  0,   0,   0);
    doc->m_normalcolour[1] = RGB(128,   0,   0);
    doc->m_normalcolour[2] = RGB(  0, 128,   0);
    doc->m_normalcolour[3] = RGB(128, 128,   0);
    doc->m_normalcolour[4] = RGB(  0,   0, 128);
    doc->m_normalcolour[5] = RGB(128,   0, 128);
    doc->m_normalcolour[6] = RGB(  0, 128, 128);
    doc->m_normalcolour[7] = RGB(192, 192, 192);
    doc->m_boldcolour[0]   = RGB(128, 128, 128);
    doc->m_boldcolour[1]   = RGB(255,   0,   0);
    doc->m_boldcolour[2]   = RGB(  0, 255,   0);
    doc->m_boldcolour[3]   = RGB(255, 255,   0);
    doc->m_boldcolour[4]   = RGB(  0,   0, 255);
    doc->m_boldcolour[5]   = RGB(255,   0, 255);
    doc->m_boldcolour[6]   = RGB(  0, 255, 255);
    doc->m_boldcolour[7]   = RGB(255, 255, 255);
    for (int i = 0; i < MAX_CUSTOM; ++i) {
        doc->m_customtext[i] = doc->m_normalcolour[i % 8];
        doc->m_customback[i] = RGB(0, 0, 0);
    }

    // Set pre-load defaults (may be overridden by the world file).
    doc->m_nWrapColumn = 80;
    doc->m_bUTF_8      = 1;
    doc->m_maxlines    = 5000;

    // Load world settings from XML.  This calls Serialize() → Load_World_XML()
    // which parses ip_address, port, triggers, aliases, timers, etc.
    if (!doc->OnOpenDocument(path)) {
        delete doc;
        // Remove from the startup world list — the file is missing or unreadable.
        world_list_remove(path);
        return nullptr;
    }

    // Record the file path on the document (the base OnOpenDocument doesn't), so
    // it can be identified as already-open above, and so Save writes back here.
    doc->SetPathName(path);

    // Patch custom-text colours to be visible (same as hook_new_world).
    for (int i = 0; i < MAX_CUSTOM; ++i)
        doc->m_customtext[i] = RGB(192, 192, 192);

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();
    child->Show();

    // All output callbacks (m_fn_line_ready, m_fn_note, m_fn_begin_batch,
    // m_fn_flush, m_fn_status) and initial pane state (font, scrollback,
    // background) are applied inside CWorldChildFrame's constructor.

    // Flush any notes produced during OnOpenDocument.
    doc->UpdateAllViews(nullptr);

    // Remember this world file path so it reopens on next launch.
    world_list_add(path);
    // Also add to the File → Open Recent list.
    recent_worlds_add(path);

    // Auto-connect if the world file says to (m_connect_now != 0).
    // Mirrors what Windows does: after opening the file the connection phase is
    // eConnectNotConnected and ConnectSocket() starts the async dial.
    if (doc->m_connect_now && doc->m_iConnectPhase == eConnectNotConnected)
        doc->ConnectSocket();

    return static_cast<void*>(doc);
}

// ── Save Active World ─────────────────────────────────────────────────────
// Saves the doc of the currently active MDI child frame.
// If saveAs==true OR the doc has no path yet, shows a save-file dialog first.
static void hook_save_active_world(wxMDIParentFrame* frame, bool saveAs)
{
    if (!frame) return;

    // Identify the active MDI child frame.
    wxMDIChildFrame* active = frame->GetActiveChild();
    if (!active) return;
    CWorldChildFrame* child = dynamic_cast<CWorldChildFrame*>(active);
    if (!child) return;

    // Access the doc via the public accessor pattern: child has m_doc.
    // We use the OutputPane's parent (child frame) to reach the doc.
    // Since we can't access m_doc directly (it's private), we work through
    // a dedicated accessor.  For now, add a GetDoc() to CWorldChildFrame.
    CMUSHclientDoc* doc = child->GetDoc();
    if (!doc) return;

    // Determine the save path.
    CString  pathCStr = doc->GetPathName();
    wxString path     = wxString::FromUTF8(static_cast<const char*>(pathCStr));

    if (saveAs || path.IsEmpty()) {
        // Use the directory of the existing path, or fall back to the default
        // world file directory configured in preferences.
        wxString saveDir = path.IsEmpty()
            ? wxString::FromUTF8(static_cast<const char*>(App.m_strDefaultWorldFileDirectory))
            : wxFileName(path).GetPath();
        wxFileDialog dlg(frame,
                         wxT("Save World File"),
                         saveDir,
                         path.IsEmpty() ? wxT("world.mcl") : wxFileName(path).GetFullName(),
                         wxT("MUSHclient world files (*.mcl)|*.mcl|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        path = dlg.GetPath();
    }

    if (path.IsEmpty()) return;

    std::string savePath = path.ToUTF8().data();
    if (!doc->OnSaveDocument(savePath.c_str())) {
        wxMessageBox(wxT("Failed to save world file."), wxT("Save Error"), wxICON_ERROR | wxOK);
        return;
    }

    // Remove the old path from the world list if save-as changed it.
    std::string oldPath = static_cast<const char*>(doc->GetPathName());
    if (!oldPath.empty() && oldPath != savePath)
        world_list_remove(oldPath);

    // Update the document's path so subsequent Ctrl+S uses the same file.
    doc->SetPathName(savePath.c_str());
    doc->SetModifiedFlag(FALSE);

    // Add to the startup world list so this world reopens on next launch.
    world_list_add(savePath);
    // Also add to the File → Open Recent list.
    recent_worlds_add(savePath);
}

// ── Quick Connect dialog ──────────────────────────────────────────────────
// Shows a small wx dialog with World Name / Server / Port fields.
// Fills *outName and *outAddr with heap-allocated strings (caller frees).
// Returns IDOK on success, IDCANCEL if the user cancels.

namespace {
class QuickConnectDlg : public wxDialog
{
public:
    wxTextCtrl* m_name;
    wxTextCtrl* m_addr;
    wxSpinCtrl* m_port;

    QuickConnectDlg(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, wxT("Quick Connect"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer* grid = new wxFlexGridSizer(3, 2, 6, 8);
        grid->AddGrowableCol(1, 1);

        auto addRow = [&](const wxString& label, wxWindow* ctrl) {
            grid->Add(new wxStaticText(this, wxID_ANY, label),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(ctrl, 1, wxEXPAND);
        };

        m_name = new wxTextCtrl(this, wxID_ANY, wxT("My World"),
                                wxDefaultPosition, wxSize(280, -1));
        m_addr = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(280, -1));
        m_port = new wxSpinCtrl(this, wxID_ANY, wxT("4000"),
                                wxDefaultPosition, wxDefaultSize,
                                wxSP_ARROW_KEYS, 1, 65535, 4000);

        addRow(wxT("World name:"), m_name);
        addRow(wxT("Server:"),     m_addr);
        addRow(wxT("Port:"),       m_port);

        vs->Add(grid, 1, wxEXPAND | wxALL, 12);
        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        SetSizerAndFit(vs);
        m_addr->SetFocus();

        // Allow OK only when server is non-empty.
        m_addr->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (wxButton* ok = dynamic_cast<wxButton*>(FindWindowById(wxID_OK)))
                ok->Enable(!m_addr->GetValue().IsEmpty());
        });
        if (wxButton* ok = dynamic_cast<wxButton*>(FindWindowById(wxID_OK)))
            ok->Enable(false);
    }
};
} // anonymous namespace

// ── GlobalPrefsDlg ────────────────────────────────────────────────────────
// Application-wide preferences dialog (tabbed).  Maps directly to
// CMUSHclientApp members stored in the preferences DB.

namespace {

// Helper: make a directory-picker row (label + text + "…" browse button).
// The text control is returned; parent owns all widgets.
static wxTextCtrl* MakeDirRow(wxWindow* parent, wxSizer* box,
                               const wxString& label, const wxString& value,
                               const wxString& dlgTitle)
{
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(parent, wxID_ANY, label),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    wxTextCtrl* ctrl = new wxTextCtrl(parent, wxID_ANY, value,
                                       wxDefaultPosition, wxSize(260, -1));
    row->Add(ctrl, 1, wxEXPAND);
    wxButton* btn = new wxButton(parent, wxID_ANY, wxT("…"),
                                  wxDefaultPosition, wxSize(28, -1));
    btn->Bind(wxEVT_BUTTON, [ctrl, dlgTitle](wxCommandEvent&) {
        wxDirDialog dlg(nullptr, dlgTitle, ctrl->GetValue(),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            ctrl->SetValue(dlg.GetPath());
    });
    row->Add(btn, 0, wxLEFT, 4);
    box->Add(row, 0, wxEXPAND | wxALL, 6);
    return ctrl;
}

class GlobalPrefsDlg : public wxDialog
{
public:
    // ── General tab ───────────────────────────────────────────────────────
    wxCheckBox* m_reconnectOnFail;
    wxCheckBox* m_notifyOnDisconnect;
    wxCheckBox* m_notifyCannotConnect;
    wxCheckBox* m_autoConnect;
    wxCheckBox* m_errorsToOutput;
    wxCheckBox* m_regexpMatchEmpty;
    wxCheckBox* m_openMaximised;
    wxCheckBox* m_confirmCloseWorld;
    wxCheckBox* m_confirmCloseApp;
    wxCheckBox* m_confirmSaveVars;
    wxCheckBox* m_appendToLog;
    wxCheckBox* m_autoLog;
    wxCheckBox* m_allTypingToCommand;
    wxTextCtrl* m_wordDelimitersDblClick;
    // ── Font tab ──────────────────────────────────────────────────────────
    wxTextCtrl*   m_fontName;      // output font face name
    wxSpinCtrl*   m_fontSize;      // output font point size
    // ── Directories tab ───────────────────────────────────────────────────
    wxTextCtrl* m_worldDir;
    wxTextCtrl* m_pluginsDir;
    wxTextCtrl* m_logDir;

    explicit GlobalPrefsDlg(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, wxT("Preferences"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxBoxSizer* vs = new wxBoxSizer(wxVERTICAL);
        wxNotebook* nb = new wxNotebook(this, wxID_ANY);

        // ════════════════════════════════════════════════════════════════
        // Tab 1: General
        // ════════════════════════════════════════════════════════════════
        {
            wxScrolledWindow* pg = new wxScrolledWindow(nb);
            pg->SetScrollRate(0, 10);
            wxBoxSizer* pvs = new wxBoxSizer(wxVERTICAL);

            // Connection
            {
                wxStaticBoxSizer* box =
                    new wxStaticBoxSizer(wxVERTICAL, pg, wxT("Connection"));
                m_reconnectOnFail    = new wxCheckBox(pg, wxID_ANY,
                    wxT("Reconnect automatically on link failure"));
                m_notifyOnDisconnect = new wxCheckBox(pg, wxID_ANY,
                    wxT("Notify when world disconnects"));
                m_notifyCannotConnect = new wxCheckBox(pg, wxID_ANY,
                    wxT("Notify if connection attempt fails"));
                m_autoConnect         = new wxCheckBox(pg, wxID_ANY,
                    wxT("Auto-connect worlds when opened"));
                m_reconnectOnFail->SetValue(App.m_bReconnectOnLinkFailure     != 0);
                m_notifyOnDisconnect->SetValue(App.m_bNotifyOnDisconnect       != 0);
                m_notifyCannotConnect->SetValue(App.m_bNotifyIfCannotConnect   != 0);
                m_autoConnect->SetValue(App.m_bAutoConnectWorlds               != 0);
                box->Add(m_reconnectOnFail,     0, wxALL, 6);
                box->Add(m_notifyOnDisconnect,  0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                box->Add(m_notifyCannotConnect, 0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                box->Add(m_autoConnect,         0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                pvs->Add(box, 0, wxEXPAND | wxALL, 10);
            }
            // Display / Scripting
            {
                wxStaticBoxSizer* box =
                    new wxStaticBoxSizer(wxVERTICAL, pg, wxT("Display / Scripting"));
                m_errorsToOutput  = new wxCheckBox(pg, wxID_ANY,
                    wxT("Show script errors in the output window"));
                m_regexpMatchEmpty = new wxCheckBox(pg, wxID_ANY,
                    wxT("Regular expressions can match empty strings"));
                m_openMaximised   = new wxCheckBox(pg, wxID_ANY,
                    wxT("Open worlds maximised"));
                m_errorsToOutput->SetValue(App.m_bErrorNotificationToOutputWindow != 0);
                m_regexpMatchEmpty->SetValue(App.m_bRegexpMatchEmpty               != 0);
                m_openMaximised->SetValue(App.m_bOpenWorldsMaximised               != 0);
                box->Add(m_errorsToOutput,   0, wxALL, 6);
                box->Add(m_regexpMatchEmpty, 0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                box->Add(m_openMaximised,    0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                pvs->Add(box, 0, wxEXPAND | wxLEFT|wxRIGHT|wxBOTTOM, 10);
            }
            // Behaviour / Logging
            {
                wxStaticBoxSizer* box =
                    new wxStaticBoxSizer(wxVERTICAL, pg, wxT("Behaviour / Logging"));
                m_confirmCloseWorld = new wxCheckBox(pg, wxID_ANY,
                    wxT("Confirm before closing a world"));
                m_confirmCloseApp   = new wxCheckBox(pg, wxID_ANY,
                    wxT("Confirm before quitting MUSHclient"));
                m_confirmSaveVars   = new wxCheckBox(pg, wxID_ANY,
                    wxT("Confirm before saving variables to file"));
                m_autoLog           = new wxCheckBox(pg, wxID_ANY,
                    wxT("Auto-log all worlds"));
                m_appendToLog       = new wxCheckBox(pg, wxID_ANY,
                    wxT("Append to existing log files (don't overwrite)"));
                m_confirmCloseWorld->SetValue(App.m_bConfirmBeforeClosingWorld     != 0);
                m_confirmCloseApp->SetValue(App.m_bConfirmBeforeClosingMushclient  != 0);
                m_confirmSaveVars->SetValue(App.m_bConfirmBeforeSavingVariables    != 0);
                m_autoLog->SetValue(App.m_bAutoLogWorld                            != 0);
                m_appendToLog->SetValue(App.m_bAppendToLogFiles                    != 0);
                box->Add(m_confirmCloseWorld, 0, wxALL, 6);
                box->Add(m_confirmCloseApp,   0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                box->Add(m_confirmSaveVars,   0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                box->Add(m_autoLog,           0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                box->Add(m_appendToLog,       0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
                pvs->Add(box, 0, wxEXPAND | wxLEFT|wxRIGHT|wxBOTTOM, 10);
            }
            // Input / editing
            {
                wxStaticBoxSizer* box =
                    new wxStaticBoxSizer(wxVERTICAL, pg, wxT("Input / editing"));
                m_allTypingToCommand = new wxCheckBox(pg, wxID_ANY,
                    wxT("Send all keyboard input to the command bar"));
                m_allTypingToCommand->SetValue(App.m_bAllTypingToCommandWindow != 0);
                box->Add(m_allTypingToCommand, 0, wxALL, 6);

                wxBoxSizer* delimRow = new wxBoxSizer(wxHORIZONTAL);
                delimRow->Add(new wxStaticText(pg, wxID_ANY,
                    wxT("Double-click word delimiters:")),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_wordDelimitersDblClick = new wxTextCtrl(pg, wxID_ANY,
                    wxString::FromUTF8(
                        static_cast<const char*>(App.m_strWordDelimitersDblClick)),
                    wxDefaultPosition, wxSize(160, -1));
                delimRow->Add(m_wordDelimitersDblClick, 1, wxEXPAND);
                box->Add(delimRow, 0, wxEXPAND | wxLEFT|wxRIGHT|wxBOTTOM, 6);
                pvs->Add(box, 0, wxEXPAND | wxLEFT|wxRIGHT|wxBOTTOM, 10);
            }
            pg->SetSizer(pvs);
            nb->AddPage(pg, wxT("General"));
        }

        // ════════════════════════════════════════════════════════════════
        // Tab 2: Font
        // ════════════════════════════════════════════════════════════════
        {
            wxPanel* pg = new wxPanel(nb);
            wxBoxSizer* pvs = new wxBoxSizer(wxVERTICAL);

            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, pg, wxT("Default output / input font"));
            box->Add(new wxStaticText(pg, wxID_ANY,
                wxT("Applied to new worlds.  Leave blank to use the system\n"
                    "monospace font.  Existing worlds keep their saved font.")),
                0, wxALL, 6);

            // Face name row
            {
                wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(pg, wxID_ANY, wxT("Font:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_fontName = new wxTextCtrl(pg, wxID_ANY,
                    wxString::FromUTF8(static_cast<const char*>(App.m_strDefaultOutputFont)),
                    wxDefaultPosition, wxSize(200, -1));
                row->Add(m_fontName, 1, wxEXPAND);

                // "Choose…" button opens a wxFontDialog
                wxButton* chooseBtn = new wxButton(pg, wxID_ANY, wxT("Choose…"));
                chooseBtn->Bind(wxEVT_BUTTON, [this, pg](wxCommandEvent&) {
                    wxString face = m_fontName->GetValue();
                    int      pts  = m_fontSize->GetValue();
                    wxFont   init(pts > 0 ? pts : 12,
                                  wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                                  wxFONTWEIGHT_NORMAL, false, face);
                    wxFontData fd;
                    fd.SetInitialFont(init);
                    fd.SetAllowSymbols(false);
                    wxFontDialog dlg(pg, fd);
                    if (dlg.ShowModal() == wxID_OK) {
                        wxFont chosen = dlg.GetFontData().GetChosenFont();
                        m_fontName->SetValue(chosen.GetFaceName());
                        m_fontSize->SetValue(chosen.GetPointSize());
                    }
                });
                row->Add(chooseBtn, 0, wxLEFT, 6);
                box->Add(row, 0, wxEXPAND | wxLEFT|wxRIGHT|wxBOTTOM, 6);
            }
            // Size row
            {
                wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(pg, wxID_ANY, wxT("Size (pt):")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_fontSize = new wxSpinCtrl(pg, wxID_ANY,
                    wxEmptyString, wxDefaultPosition, wxSize(70, -1),
                    wxSP_ARROW_KEYS, 4, 72,
                    App.m_iDefaultOutputFontHeight > 0
                        ? App.m_iDefaultOutputFontHeight : 12);
                row->Add(m_fontSize, 0);
                box->Add(row, 0, wxLEFT|wxRIGHT|wxBOTTOM, 6);
            }
            pvs->Add(box, 0, wxEXPAND | wxALL, 10);
            pg->SetSizer(pvs);
            nb->AddPage(pg, wxT("Font"));
        }

        // ════════════════════════════════════════════════════════════════
        // Tab 3: Directories
        // ════════════════════════════════════════════════════════════════
        {
            wxPanel* pg = new wxPanel(nb);
            wxBoxSizer* pvs = new wxBoxSizer(wxVERTICAL);
            wxStaticBoxSizer* box =
                new wxStaticBoxSizer(wxVERTICAL, pg, wxT("Default directories"));

            m_worldDir   = MakeDirRow(pg, box, wxT("World files:"),
                wxString::FromUTF8(static_cast<const char*>(App.m_strDefaultWorldFileDirectory)),
                wxT("Choose default world directory"));
            m_pluginsDir = MakeDirRow(pg, box, wxT("Plugins:"),
                wxString::FromUTF8(static_cast<const char*>(App.m_strPluginsDirectory)),
                wxT("Choose plugins directory"));
            m_logDir     = MakeDirRow(pg, box, wxT("Log files:"),
                wxString::FromUTF8(static_cast<const char*>(App.m_strDefaultLogFileDirectory)),
                wxT("Choose default log directory"));

            pvs->Add(box, 0, wxEXPAND | wxALL, 10);
            pg->SetSizer(pvs);
            nb->AddPage(pg, wxT("Directories"));
        }

        vs->Add(nb, 1, wxEXPAND | wxALL, 8);
        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                wxEXPAND | wxLEFT|wxRIGHT|wxBOTTOM, 10);
        SetSizerAndFit(vs);
        SetSize(wxSize(520, -1));
    }
};
} // anonymous namespace

static void hook_file_preferences(wxWindow* parent)
{
    GlobalPrefsDlg dlg(parent);
    if (dlg.ShowModal() != wxID_OK) return;

    // ── General tab ───────────────────────────────────────────────────────
    App.m_bReconnectOnLinkFailure          = dlg.m_reconnectOnFail->GetValue()     ? 1 : 0;
    App.m_bNotifyOnDisconnect              = dlg.m_notifyOnDisconnect->GetValue()   ? 1 : 0;
    App.m_bNotifyIfCannotConnect           = dlg.m_notifyCannotConnect->GetValue()  ? 1 : 0;
    App.m_bAutoConnectWorlds               = dlg.m_autoConnect->GetValue()          ? 1 : 0;
    App.m_bErrorNotificationToOutputWindow = dlg.m_errorsToOutput->GetValue()       ? 1 : 0;
    App.m_bRegexpMatchEmpty                = dlg.m_regexpMatchEmpty->GetValue()      ? 1 : 0;
    App.m_bOpenWorldsMaximised             = dlg.m_openMaximised->GetValue()         ? 1 : 0;
    App.m_bConfirmBeforeClosingWorld       = dlg.m_confirmCloseWorld->GetValue()    ? 1 : 0;
    App.m_bConfirmBeforeClosingMushclient  = dlg.m_confirmCloseApp->GetValue()      ? 1 : 0;
    App.m_bConfirmBeforeSavingVariables    = dlg.m_confirmSaveVars->GetValue()      ? 1 : 0;
    App.m_bAutoLogWorld                    = dlg.m_autoLog->GetValue()              ? 1 : 0;
    App.m_bAppendToLogFiles                = dlg.m_appendToLog->GetValue()          ? 1 : 0;
    App.m_bAllTypingToCommandWindow        = dlg.m_allTypingToCommand->GetValue()   ? 1 : 0;
    App.m_strWordDelimitersDblClick =
        CString(dlg.m_wordDelimitersDblClick->GetValue().ToUTF8().data());

    // ── Font tab ──────────────────────────────────────────────────────────
    App.m_strDefaultOutputFont       = CString(dlg.m_fontName->GetValue().ToUTF8().data());
    App.m_strDefaultInputFont        = App.m_strDefaultOutputFont;  // keep in sync
    App.m_strFixedPitchFont          = App.m_strDefaultOutputFont;
    App.m_iDefaultOutputFontHeight   = dlg.m_fontSize->GetValue();
    App.m_iDefaultInputFontHeight    = App.m_iDefaultOutputFontHeight;
    App.m_iFixedPitchFontSize        = App.m_iDefaultOutputFontHeight;

    // ── Directories tab ───────────────────────────────────────────────────
    App.m_strDefaultWorldFileDirectory =
        CString(dlg.m_worldDir->GetValue().ToUTF8().data());
    App.m_strPluginsDirectory =
        CString(dlg.m_pluginsDir->GetValue().ToUTF8().data());
    App.m_strDefaultLogFileDirectory =
        CString(dlg.m_logDir->GetValue().ToUTF8().data());

    // ── Persist all changed values to the preferences DB ──────────────────
    // Boolean settings
    App.db_write_int("prefs", "ReconnectOnLinkFailure",         App.m_bReconnectOnLinkFailure);
    App.db_write_int("prefs", "NotifyOnDisconnect",             App.m_bNotifyOnDisconnect);
    App.db_write_int("prefs", "NotifyIfCannotConnect",          App.m_bNotifyIfCannotConnect);
    App.db_write_int("prefs", "AutoConnectWorlds",              App.m_bAutoConnectWorlds);
    App.db_write_int("prefs", "ErrorNotificationToOutputWindow",App.m_bErrorNotificationToOutputWindow);
    App.db_write_int("prefs", "RegexpMatchEmpty",               App.m_bRegexpMatchEmpty);
    App.db_write_int("prefs", "OpenWorldsMaximised",            App.m_bOpenWorldsMaximised);
    App.db_write_int("prefs", "ConfirmBeforeClosingWorld",      App.m_bConfirmBeforeClosingWorld);
    App.db_write_int("prefs", "ConfirmBeforeClosingMushclient", App.m_bConfirmBeforeClosingMushclient);
    App.db_write_int("prefs", "ConfirmBeforeSavingVariables",   App.m_bConfirmBeforeSavingVariables);
    App.db_write_int("prefs", "AutoLogWorld",                   App.m_bAutoLogWorld);
    App.db_write_int("prefs", "AppendToLogFiles",               App.m_bAppendToLogFiles);
    App.db_write_int("prefs", "AllTypingToCommandWindow",       App.m_bAllTypingToCommandWindow);
    App.db_write_string("prefs", "WordDelimitersDblClick",
                        static_cast<const char*>(App.m_strWordDelimitersDblClick));
    // Font settings
    App.db_write_string("prefs", "DefaultOutputFont",
                        static_cast<const char*>(App.m_strDefaultOutputFont));
    App.db_write_string("prefs", "DefaultInputFont",
                        static_cast<const char*>(App.m_strDefaultInputFont));
    App.db_write_string("prefs", "FixedPitchFont",
                        static_cast<const char*>(App.m_strFixedPitchFont));
    App.db_write_int("prefs", "DefaultOutputFontHeight",  App.m_iDefaultOutputFontHeight);
    App.db_write_int("prefs", "DefaultInputFontHeight",   App.m_iDefaultInputFontHeight);
    App.db_write_int("prefs", "FixedPitchFontSize",       App.m_iFixedPitchFontSize);
    // Directory settings
    App.db_write_string("prefs", "DefaultWorldFileDirectory",
                        static_cast<const char*>(App.m_strDefaultWorldFileDirectory));
    App.db_write_string("prefs", "PluginsDirectory",
                        static_cast<const char*>(App.m_strPluginsDirectory));
    App.db_write_string("prefs", "DefaultLogFileDirectory",
                        static_cast<const char*>(App.m_strDefaultLogFileDirectory));

    // ── Live-update all open worlds with new global pref values ──────────
    // Word delimiters and copy-to-clipboard are per-pane state; push the
    // new values to every currently-open CWorldChildFrame output pane.
    std::string newDelims =
        static_cast<const char*>(App.m_strWordDelimitersDblClick);
    for (auto& kv : s_docFrameMap) {
        CWorldChildFrame* cf = kv.second;
        if (cf && cf->OutputPane())
            cf->OutputPane()->SetWordDelimitersDblClick(newDelims);
    }
}

// ── WorldPreferencesDlg ───────────────────────────────────────────────────
// Dialog for editing world properties: connection, display, scripting, options.
// Covers the settings needed to connect and run Lua scripts without the full
// Windows property-sheet UI.


// ── hook_get_clipboard_text / hook_set_clipboard_text ─────────────────────────
// System clipboard access for non-Windows.  Used by GetClipboardContents() and
// putontoclipboard() in doc.cpp (which power the world.GetClipboard() /
// world.SetClipboard() Lua API, the colour-from-clipboard picker, and
// IsClipboardXML()).
static char* hook_get_clipboard_text()
{
    if (!wxTheClipboard->Open()) return nullptr;
    char* result = nullptr;
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
        wxTheClipboard->IsSupported(wxDF_TEXT))
    {
        wxTextDataObject data;
        wxTheClipboard->GetData(data);
        wxScopedCharBuffer buf = data.GetText().ToUTF8();
        if (buf.length() > 0)
            result = strdup(buf.data());
    }
    wxTheClipboard->Close();
    return result;   // caller must free()
}

static void hook_set_clipboard_text(const char* text)
{
    if (!text) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
        wxTheClipboard->Close();
    }
}

// ── hook_file_dialog ──────────────────────────────────────────────────────────
// Generic file-open / file-save dialog used by CFileDialog::DoModal() on
// non-Windows.  Converts the MFC-style pipe-separated filter string to a
// wxFileDialog wildcard string, shows the dialog, and returns the selected path
// as a heap-allocated (malloc) UTF-8 string.  The caller must free() it.
// Returns nullptr if the user cancels or if something goes wrong.
static char* hook_file_dialog(bool bOpen,
                               const char* title,
                               const char* defaultDir,
                               const char* defaultFile,
                               const char* filter,
                               bool overwrite)
{
    // Convert MFC filter "Desc1|*.ext1|Desc2|*.ext2||" → wx "Desc1|*.ext1|Desc2|*.ext2"
    // MFC uses | between tokens and || (or trailing |) to end the list.
    // wx uses the same | separator but without a trailing || terminator.
    wxString wxFilter;
    if (filter && *filter) {
        wxFilter = wxString::FromUTF8(filter);
        // Strip trailing |s (MFC uses "||" as end-of-list sentinel).
        while (!wxFilter.IsEmpty() && wxFilter.Last() == '|')
            wxFilter.RemoveLast();
    }
    if (wxFilter.IsEmpty())
        wxFilter = wxT("All files (*.*)|*.*");

    wxString wxTitle = (title && *title)
                       ? wxString::FromUTF8(title)
                       : wxString(bOpen ? wxT("Open") : wxT("Save As"));
    wxString wxDir  = (defaultDir  && *defaultDir)
                      ? wxString::FromUTF8(defaultDir)  : wxString(wxEmptyString);
    wxString wxFile = (defaultFile && *defaultFile)
                      ? wxString::FromUTF8(defaultFile) : wxString(wxEmptyString);

    long style = bOpen
        ? (wxFD_OPEN | wxFD_FILE_MUST_EXIST)
        : (wxFD_SAVE | (overwrite ? wxFD_OVERWRITE_PROMPT : 0));

    // Use the MDI parent as the dialog owner when available.
    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;

    wxFileDialog dlg(parent, wxTitle, wxDir, wxFile, wxFilter, style);
    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    return strdup(dlg.GetPath().ToUTF8().data());
}

// ── hook_save_world ───────────────────────────────────────────────────────────
// Saves the world file for the given doc.  If saveAs is true or the doc has no
// path, opens a save-file dialog.  parent is a wxWindow* used as dialog parent.
// Returns true if the file was saved, false if cancelled or on error.
static bool hook_save_world(void* vdoc, bool saveAs, void* vparent)
{
    auto* doc    = static_cast<CMUSHclientDoc*>(vdoc);
    auto* parent = static_cast<wxWindow*>(vparent);

    CString  pathCStr = doc->GetPathName();
    wxString path     = wxString::FromUTF8(static_cast<const char*>(pathCStr));

    if (saveAs || path.IsEmpty()) {
        wxString saveDir = path.IsEmpty()
            ? wxString::FromUTF8(
                  static_cast<const char*>(App.m_strDefaultWorldFileDirectory))
            : wxFileName(path).GetPath();
        wxFileDialog dlg(parent,
                         wxT("Save World File"),
                         saveDir,
                         path.IsEmpty() ? wxT("world.mcl")
                                        : wxFileName(path).GetFullName(),
                         wxT("MUSHclient world files (*.mcl)|*.mcl"
                             "|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return false;
        path = dlg.GetPath();
    }

    if (path.IsEmpty()) return false;

    std::string savePath = path.ToUTF8().data();
    if (!doc->OnSaveDocument(savePath.c_str())) {
        wxMessageBox(wxT("Failed to save world file."),
                     wxT("Save Error"), wxICON_ERROR | wxOK, parent);
        return false;
    }

    // Update path, clear modified flag, maintain world lists.
    std::string oldPath = static_cast<const char*>(doc->GetPathName());
    if (!oldPath.empty() && oldPath != savePath)
        world_list_remove(oldPath);
    doc->SetPathName(savePath.c_str());
    doc->SetModifiedFlag(FALSE);
    world_list_add(savePath);
    recent_worlds_add(savePath);
    return true;
}

static int hook_quick_connect_dlg(char** outName, char** outAddr, int* outPort)
{
    // Use the MDI parent frame as dialog parent.
    // Frame.m_hWnd holds the wxMDIParentFrame* set by hook_load_mdi_frame.
    // Same as hook_new_world: use App.m_pMainWnd, not the stub Frame global.
    wxWindow* parent = App.m_pMainWnd
                     ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                     : nullptr;

    QuickConnectDlg dlg(parent);

    // Pre-fill with the last-used values so repeat connections need fewer clicks.
    {
        wxConfig cfg(wxT("mushclient"));
        wxString lastAddr;
        long     lastPort = 0;
        wxString lastName;
        if (cfg.Read(wxT("/quickconnect/addr"), &lastAddr) && !lastAddr.IsEmpty()) {
            dlg.m_addr->SetValue(lastAddr);
            if (wxButton* ok = dynamic_cast<wxButton*>(dlg.FindWindowById(wxID_OK)))
                ok->Enable(true);
        }
        if (cfg.Read(wxT("/quickconnect/port"), &lastPort) && lastPort > 0)
            dlg.m_port->SetValue((int)lastPort);
        if (cfg.Read(wxT("/quickconnect/name"), &lastName) && !lastName.IsEmpty())
            dlg.m_name->SetValue(lastName);
    }

    if (dlg.ShowModal() != wxID_OK)
        return IDCANCEL;

    // Persist for next time.
    {
        wxConfig cfg(wxT("mushclient"));
        cfg.Write(wxT("/quickconnect/addr"), dlg.m_addr->GetValue());
        cfg.Write(wxT("/quickconnect/port"), (long)dlg.m_port->GetValue());
        cfg.Write(wxT("/quickconnect/name"), dlg.m_name->GetValue());
    }

    *outName = strdup(dlg.m_name->GetValue().ToUTF8().data());
    *outAddr = strdup(dlg.m_addr->GetValue().ToUTF8().data());
    *outPort = dlg.m_port->GetValue();
    return IDOK;
}

// ── New-world hook ────────────────────────────────────────────────────────
// "Open a new world" flow:
//   1. Create CMUSHclientDoc, call OnNewDocument (→ Quick Connect dialog
//      → SetUpOutputWindow → OpenSession).  Lines accumulate in m_LineList.
//   2. On cancel/error: delete the doc and return nullptr.
//   3. On success: create CWorldChildFrame, show it, wire m_fn_line_ready,
//      then flush all buffered lines (welcome banner + any MUD output so far).

static void* hook_new_world()
{
    // Retrieve the MDI parent frame.
    // InitInstance creates `new CMainFrame` and stores it in App.m_pMainWnd;
    // that object's m_hWnd is the wxMDIParentFrame* set by hook_load_mdi_frame.
    // The global `Frame` stub is a separate object whose m_hWnd is always null.
    wxMDIParentFrame* mdiParent = nullptr;
    if (App.m_pMainWnd)
        mdiParent = static_cast<wxMDIParentFrame*>(
                        static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd));
    if (!mdiParent) return nullptr;

    CMUSHclientDoc* doc = new CMUSHclientDoc();

    // ── Default ANSI colour tables ────────────────────────────────────────
    // On non-Windows these arrays are not populated by the MFC/registry
    // initialisation that runs on Windows, and they stay zero (all black)
    // unless a world file is loaded from XML.  For Quick Connect (no world
    // file) we initialise them to the standard VGA / xterm palette here,
    // matching what doc_portable.cpp does for the mushcore CLI build.
    doc->m_normalcolour[0/*BLACK*/]   = RGB(  0,   0,   0);
    doc->m_normalcolour[1/*RED*/]     = RGB(128,   0,   0);
    doc->m_normalcolour[2/*GREEN*/]   = RGB(  0, 128,   0);
    doc->m_normalcolour[3/*YELLOW*/]  = RGB(128, 128,   0);
    doc->m_normalcolour[4/*BLUE*/]    = RGB(  0,   0, 128);
    doc->m_normalcolour[5/*MAGENTA*/] = RGB(128,   0, 128);
    doc->m_normalcolour[6/*CYAN*/]    = RGB(  0, 128, 128);
    doc->m_normalcolour[7/*WHITE*/]   = RGB(192, 192, 192);

    doc->m_boldcolour[0/*BLACK*/]   = RGB(128, 128, 128);
    doc->m_boldcolour[1/*RED*/]     = RGB(255,   0,   0);
    doc->m_boldcolour[2/*GREEN*/]   = RGB(  0, 255,   0);
    doc->m_boldcolour[3/*YELLOW*/]  = RGB(255, 255,   0);
    doc->m_boldcolour[4/*BLUE*/]    = RGB(  0,   0, 255);
    doc->m_boldcolour[5/*MAGENTA*/] = RGB(255,   0, 255);
    doc->m_boldcolour[6/*CYAN*/]    = RGB(  0, 255, 255);
    doc->m_boldcolour[7/*WHITE*/]   = RGB(255, 255, 255);

    // Custom colours cycle through the normal palette (matching doc_portable.cpp).
    for (int i = 0; i < MAX_CUSTOM; ++i) {
        doc->m_customtext[i] = doc->m_normalcolour[i % 8];
        doc->m_customback[i] = RGB(0, 0, 0);
    }

    // eQuickConnect makes OnNewDocument show the Quick Connect dialog.
    App.m_TypeOfNewDocument = CMUSHclientApp::eQuickConnect;

    // Initialise display-critical fields that SetUpOutputWindow uses to size
    // the first CLine text buffer.  On Windows these come from the "default
    // world" settings that MFC loads before OnNewDocument; on non-Windows we
    // provide sane defaults here.  They will be overwritten later if the user
    // opens an actual world file or the window is resized.
    doc->m_nWrapColumn = 80;     // CLine allocates m_nWrapColumn bytes for text
    doc->m_bUTF_8      = 1;     // use UTF-8 throughout; 4× multiplier in CLine
    doc->m_maxlines    = 5000;  // SetUpOutputWindow sizes m_pLinePositions from this

    // OnNewDocument: shows dialog, calls SetUpOutputWindow, OpenSession.
    // Lines produced by Note() accumulate in doc->m_LineList but are not
    // forwarded to the pane yet (m_fn_line_ready is wired after this by the
    // CWorldChildFrame constructor; UpdateAllViews below drains the buffer).
    if (!doc->OnNewDocument()) {
        delete doc;   // user cancelled — destructor cleans up
        return nullptr;
    }

    // ── Fix note colours after OnNewDocument ─────────────────────────────
    // OnNewDocument → SetDefaults() sets m_iNoteTextColour = 4 (custom palette
    // index).  Our m_customtext[4] is initialised to m_normalcolour[4] = dark
    // blue — not readable on black.  GetStyleRGB is called lazily at
    // UpdateAllViews time (not at Note() time), so patching m_customtext[] here
    // still affects the lines that were buffered during OnNewDocument.
    // Set every custom-text slot to light grey so all custom colour indices
    // produce readable output when no world file has overridden them.
    for (int i = 0; i < MAX_CUSTOM; ++i)
        doc->m_customtext[i] = RGB(192, 192, 192);  // light grey

    // Create and show the child frame.
    CWorldChildFrame* child = new CWorldChildFrame(mdiParent, doc);
    wx_register_world_frame(doc, child);  // keep the registry up-to-date
    child->UpdateTitle();   // set title from m_mush_name
    child->Show();

    // All output callbacks (m_fn_line_ready, m_fn_note, m_fn_begin_batch,
    // m_fn_flush, m_fn_status) and initial pane state (font, scrollback,
    // background) are applied inside CWorldChildFrame's constructor.

    // Flush welcome banner + any output buffered during OnNewDocument.
    doc->UpdateAllViews(nullptr);

    return static_cast<void*>(doc);
}

// ── App InitInstance bootstrap ────────────────────────────────────────────
// Called by wx_app.cpp::OnInit() via the hook pointer to avoid including
// MUSHclient.h in wx_app.cpp (which would conflict with wx headers).

static int hook_init_instance()
{
    // Set basic CWinApp members that MFC normally sets before InitInstance.
    App.m_lpCmdLine = "";
    App.m_nCmdShow  = SW_SHOWNORMAL;

    return App.InitInstance() != FALSE ? 1 : 0;
}

// ── hook_create_text_window ───────────────────────────────────────────────
// Allocates a CTextDocument, fills its fields from the parameters, creates a
// CWxNotepadFrame that wires all m_fn_* callbacks and registers the doc with
// App.m_pNormalDocTemplate, then returns the doc pointer (the frame owns it).
static void* hook_create_text_window(
    const char* sText,
    const char* sTitle,
    CMUSHclientDoc* pDoc,
    long long iUniqueDocNum,
    const char* strFontName,
    long  iFontSize,
    long  iFontWeight,
    unsigned int iFontCharset,
    unsigned long textColour,
    unsigned long backColour,
    const char* strSearchString,
    const char* strRecallLinePreamble,
    bool bMatchCase,
    bool bRegexp,
    bool bCommands,
    bool bOutput,
    bool bNotes,
    int  iLines,
    int  iNotepadType)
{
    if (!App.m_pNormalDocTemplate)
        return nullptr;

    // Allocate the document object via the static factory (constructor is
    // protected in MFC document classes).
    CTextDocument* td = CTextDocument::CreateInstance();
    td->m_pRelatedWorld         = pDoc;
    td->m_iUniqueDocumentNumber = iUniqueDocNum;
    td->m_strFontName           = strFontName  ? strFontName  : "";
    td->m_iFontSize             = iFontSize;
    td->m_iFontWeight           = iFontWeight;
    td->m_iFontCharset          = iFontCharset;
    td->m_textColour            = static_cast<COLORREF>(textColour);
    td->m_backColour            = static_cast<COLORREF>(backColour);
    td->m_strSearchString       = strSearchString       ? strSearchString       : "";
    td->m_strRecallLinePreamble = strRecallLinePreamble ? strRecallLinePreamble : "";
    td->m_bMatchCase            = bMatchCase;
    td->m_bRegexp               = bRegexp;
    td->m_bCommands             = bCommands;
    td->m_bOutput               = bOutput;
    td->m_bNotes                = bNotes;
    td->m_iLines                = iLines;
    td->m_iNotepadType          = iNotepadType;

    // Apply notepad-type colour overrides (matches Utilities.cpp logic).
    if (iNotepadType == eNotepadWorldLoadError) {
        td->m_textColour = RGB(128,   0,   0);   // maroon
        td->m_backColour = RGB(255, 228, 181);   // moccasin
    } else if (iNotepadType == eNotepadXMLcomments) {
        td->m_textColour = RGB(  0, 100,   0);   // darkgreen
        td->m_backColour = RGB(176, 224, 230);   // powderblue
    } else if (App.m_cNotepadTextColour != App.m_cNotepadBackColour) {
        td->m_textColour = App.m_cNotepadTextColour;
        td->m_backColour = App.m_cNotepadBackColour;
    }

    // Sanitise title for display (matches Utilities.cpp).
    CString strTitle = sTitle ? sTitle : "";
    td->m_strTitle = strTitle;
    strTitle.Replace(" ",  "");
    strTitle.Replace("#",  "");
    strTitle.Replace("%",  "");
    strTitle.Replace(";",  "");
    strTitle.Replace("/",  "-");
    strTitle.Replace("\\", "-");
    strTitle.Replace(":",  "-");
    td->SetTitle(strTitle);

    wxString wxTitle = wxString::FromUTF8(
        static_cast<const char*>(td->m_strTitle));
    wxString wxText  = wxString::FromUTF8(sText ? sText : "");

    // CWxNotepadFrame constructor registers td with the template and shows.
    // The frame takes ownership of td.
    new CWxNotepadFrame(td, wxTitle, wxText);

    return static_cast<void*>(td);
}

// ── Install all hooks ─────────────────────────────────────────────────────
// ── Sound hooks ───────────────────────────────────────────────────────────
// CMainFrame::PlaySoundFile and CancelSound delegate here on non-Windows so
// that trigger sounds and MSP audio actually play via wxWidgets.
//
// wxSound supports .wav on all platforms.  For macOS the underlying backend
// (NSSound) also handles .aiff, .mp3, .m4a, etc.  On Linux only .wav via
// OSS/ALSA is guaranteed.  Formats that wxSound cannot play are silently
// ignored (returns false), leaving the MUD session uninterrupted.
#if wxUSE_SOUND
namespace {
// Keep the current wxSound alive while it plays asynchronously.
// Replaced (and therefore stopped) when a new sound is requested.
wxSound* s_currentSound = nullptr;
} // anonymous namespace

static bool hook_play_sound(const char* path)
{
    if (!path || !*path) return false;
    // Plugins reference sound files by Windows-style backslash paths; POSIX
    // needs '/'.
    std::string fixed = PortableFilePath(path);
    wxSound* snd = new wxSound(wxString::FromUTF8(fixed.c_str()));
    if (!snd->IsOk()) {
        delete snd;
        return false;
    }
    // Stop whatever was playing before we replace the pointer.
    wxSound::Stop();
    delete s_currentSound;
    s_currentSound = snd;
    return s_currentSound->Play(wxSOUND_ASYNC);
}

static void hook_stop_sound()
{
    wxSound::Stop();
    delete s_currentSound;
    s_currentSound = nullptr;
}
#endif // wxUSE_SOUND


// ── Notepad append ────────────────────────────────────────────────────────

static bool hook_append_to_notepad(const char* title, const char* text, bool replace)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    frame->AppendText(text ? text : "", replace);
    return true;
}


static char* hook_get_notepad_text(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return nullptr;
    std::string text = frame->GetText();
    char* result = static_cast<char*>(malloc(text.size() + 1));
    if (result) memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

static char** hook_get_notepad_list()
{
    std::vector<std::string> titles = CWxNotepadFrame::ListTitles();
    char** arr = static_cast<char**>(malloc((titles.size() + 1) * sizeof(char*)));
    if (!arr) return nullptr;
    for (size_t i = 0; i < titles.size(); ++i) {
        arr[i] = static_cast<char*>(malloc(titles[i].size() + 1));
        if (arr[i]) memcpy(arr[i], titles[i].c_str(), titles[i].size() + 1);
    }
    arr[titles.size()] = nullptr;
    return arr;
}

// ── Notepad activate / close / move / position ───────────────────────────

static bool hook_activate_notepad(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    if (!frame->IsShown()) frame->Show(true);
    frame->Raise();
    return true;
}

static bool hook_close_notepad(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    frame->Close(false);   // allow SaveModified check
    return true;
}

static bool hook_move_notepad(const char* title, int x, int y, int w, int h)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    frame->SetSize(x, y, w, h);
    return true;
}

static char* hook_get_notepad_position(const char* title)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return nullptr;
    wxRect r = frame->GetRect();
    char buf[80];
    snprintf(buf, sizeof(buf), "%d %d %d %d",
             r.GetLeft(), r.GetTop(), r.GetWidth(), r.GetHeight());
    char* result = static_cast<char*>(malloc(strlen(buf) + 1));
    if (result) strcpy(result, buf);
    return result;
}

// ── Notepad save ─────────────────────────────────────────────────────────

static bool hook_notepad_colour(const char* title, unsigned long fg, unsigned long bg)
{
    CWxNotepadFrame* f = CWxNotepadFrame::Find(title ? title : "");
    if (!f) return false;
    f->SetColours(fg, bg);
    return true;
}

static bool hook_notepad_font(const char* title, const char* fontName,
                               int size, int style, int charset)
{
    CWxNotepadFrame* f = CWxNotepadFrame::Find(title ? title : "");
    if (!f) return false;
    f->SetFont(fontName ? fontName : "", size, style, charset);
    return true;
}

static bool hook_notepad_readonly(const char* title, bool readOnly)
{
    CWxNotepadFrame* f = CWxNotepadFrame::Find(title ? title : "");
    if (!f) return false;
    f->SetReadOnly(readOnly);
    return true;
}

static bool hook_save_notepad(const char* title, const char* filename, bool /*replaceExisting*/)
{
    CWxNotepadFrame* frame = CWxNotepadFrame::Find(title ? title : "");
    if (!frame) return false;
    // Delegate to the frame's File→Save handler (same as menu Ctrl+S).
    // Pass the filename via a synthetic event if non-empty, otherwise show dialog.
    wxCommandEvent evt(wxEVT_MENU, wxID_SAVE);
    // We can't easily pass a filename through the event; instead call SaveFile
    // directly on the ctrl via the doc's m_fn_save_to_file if wired.
    // GetText() retrieves the current text; save it using wxTextCtrl::SaveFile.
    if (filename && *filename)
    {
        wxString path = wxString::FromUTF8(filename);
        // SaveFile is accessible from the wxTextCtrl inside the frame.
        // Since we can't access m_ctrl directly, use m_pDoc callback if set.
        // Fall back: AppendText trick: use the m_fn_save_to_file lambda.
        // The lambda is on the CTextDocument, which we can reach via FindNotepad.
        // For simplicity, use the hook_get_notepad_text + write ourselves.
        std::string text = frame->GetText();
        FILE* fp = fopen(filename, "w");
        if (!fp) return false;
        fwrite(text.c_str(), 1, text.size(), fp);
        fclose(fp);
        return true;
    }
    else
    {
        // Show save dialog — trigger via the frame's menu save handler.
        wxCommandEvent saveEvt(wxEVT_MENU, wxID_SAVE);
        frame->GetEventHandler()->ProcessEvent(saveEvt);
        return true;
    }
}

// ── Colour picker ─────────────────────────────────────────────────────────

static long hook_pick_colour(long suggested)
{
    wxColourData data;
    if (suggested != -1)
    {
        // COLORREF is 0x00BBGGRR — extract R, G, B.
        unsigned char r = (suggested >>  0) & 0xFF;
        unsigned char g = (suggested >>  8) & 0xFF;
        unsigned char b = (suggested >> 16) & 0xFF;
        data.SetColour(wxColour(r, g, b));
    }
    wxColourDialog dlg(wxGetActiveWindow(), &data);
    if (dlg.ShowModal() != wxID_OK)
        return -1;
    wxColour col = dlg.GetColourData().GetColour();
    // Pack back into COLORREF (0x00BBGGRR).
    return (long)(((unsigned long)col.Blue()  << 16) |
                  ((unsigned long)col.Green() <<  8) |
                   (unsigned long)col.Red());
}

// ── Menu / choice dialog ──────────────────────────────────────────────────

static char* hook_menu_dialog(const char* const* items, int count,
                               const char* defaultItem)
{
    if (count <= 0)
        return nullptr;

    wxArrayString choices;
    int selection = 0;
    for (int i = 0; i < count; ++i)
    {
        choices.Add(wxString::FromUTF8(items[i]));
        if (defaultItem && defaultItem[0] && strcmp(items[i], defaultItem) == 0)
            selection = i;
    }

    wxSingleChoiceDialog dlg(wxGetActiveWindow(),
                             wxT("Select an item:"),
                             wxT("Menu"),
                             choices);
    dlg.SetSelection(selection);
    if (dlg.ShowModal() != wxID_OK)
        return nullptr;

    wxString chosen = dlg.GetStringSelection();
    std::string utf8 = chosen.ToUTF8().data();
    char* result = static_cast<char*>(malloc(utf8.size() + 1));
    if (result)
        memcpy(result, utf8.c_str(), utf8.size() + 1);
    return result;
}

// ── Flash icon / request user attention ───────────────────────────────────
// Requests user attention on the main application frame.  On macOS this
// bounces the dock icon; on Linux it flashes the taskbar button.

static void hook_flash_icon()
{
    wxWindow* topWin = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxTopLevelWindow* top = wxDynamicCast(topWin, wxTopLevelWindow);
    if (top) top->RequestUserAttention(wxUSER_ATTENTION_INFO);
}

// ── Move main window ──────────────────────────────────────────────────────
// Repositions and/or resizes the MDI parent frame.

static void hook_move_main_window(long left, long top, long width, long height)
{
    wxWindow* win = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    if (win) win->SetSize((int)left, (int)top, (int)width, (int)height);
}


// ── utils.inputbox / utils.editbox ────────────────────────────────────────
// Shows a simple text entry dialog.  Returns heap-allocated string (caller
// frees) or nullptr on cancel.

static char* hook_inputbox(const char* title,
                            const char* prompt,
                            const char* defValue,
                            bool        multiline)
{
    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxString result;
    if (multiline) {
        wxDialog dlg(parent, wxID_ANY, wxString::FromUTF8(title ? title : "Input"),
                     wxDefaultPosition, wxSize(400, 300),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        wxBoxSizer* sz = new wxBoxSizer(wxVERTICAL);
        if (prompt && *prompt)
            sz->Add(new wxStaticText(&dlg, wxID_ANY, wxString::FromUTF8(prompt)),
                    0, wxALL, 8);
        wxTextCtrl* txt = new wxTextCtrl(&dlg, wxID_ANY,
            wxString::FromUTF8(defValue ? defValue : ""),
            wxDefaultPosition, wxDefaultSize,
            wxTE_MULTILINE | wxTE_DONTWRAP);
        sz->Add(txt, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
        wxStdDialogButtonSizer* btns = new wxStdDialogButtonSizer();
        btns->AddButton(new wxButton(&dlg, wxID_OK));
        btns->AddButton(new wxButton(&dlg, wxID_CANCEL));
        btns->Realize();
        sz->Add(btns, 0, wxALL, 8);
        dlg.SetSizer(sz);
        dlg.Centre();
        if (dlg.ShowModal() != wxID_OK) return nullptr;
        result = txt->GetValue();
    } else {
        wxTextEntryDialog dlg(parent,
            wxString::FromUTF8(prompt ? prompt : ""),
            wxString::FromUTF8(title ? title : "Input"),
            wxString::FromUTF8(defValue ? defValue : ""));
        if (dlg.ShowModal() != wxID_OK) return nullptr;
        result = dlg.GetValue();
    }
    return strdup(result.ToUTF8().data());
}

// ── utils.directorypicker ─────────────────────────────────────────────────

static char* hook_directorypicker(const char* title, const char* defPath)
{
    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxDirDialog dlg(parent,
        wxString::FromUTF8(title ? title : "Select directory"),
        wxString::FromUTF8(defPath ? defPath : ""),
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return nullptr;
    return strdup(dlg.GetPath().ToUTF8().data());
}

// ── WindowMenu popup ────────────────────────────────────────────────────────
// CMiniWindow::Menu() builds the plugin's context menu into the portable CMenu
// tree (platform/mfc_ui.h); render it as a wxWidgets popup and return the
// selected command id (TPM_RETURNCMD semantics), or 0 if cancelled.
static wxMenu* wx_build_popup_menu(CMenu* m)
{
    wxMenu* wxm = new wxMenu();
    for (const CMenu::Item& it : m->m_items)
    {
        if (it.flags & MF_SEPARATOR)
            wxm->AppendSeparator();
        else if (it.flags & MF_POPUP)
        {
            CMenu*  child   = CMenu::FromMenuHandle((HMENU)(intptr_t) it.arg);
            wxMenu* wxchild = child ? wx_build_popup_menu(child) : new wxMenu();
            wxm->AppendSubMenu(wxchild, wxString::FromUTF8(it.text.c_str()));
        }
        else
        {
            // Regular command item.  Grayed placeholder items arrive with id 0.
            int id = (it.arg != 0) ? (int) it.arg : wxID_ANY;
            wxMenuItem* mi = (it.flags & MF_CHECKED)
                ? wxm->AppendCheckItem(id, wxString::FromUTF8(it.text.c_str()))
                : wxm->Append         (id, wxString::FromUTF8(it.text.c_str()));
            if (mi)
            {
                if (it.flags & MF_CHECKED) mi->Check(true);
                if (it.flags & MF_GRAYED)  mi->Enable(false);
            }
        }
    }
    return wxm;
}

static int hook_popup_menu(CMenu* menu, int /*x*/, int /*y*/)
{
    if (!menu) return 0;
    wxWindow* win = wxGetActiveWindow();
    if (!win) win = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    if (!win) return 0;

    wxMenu* wxm = wx_build_popup_menu(menu);
    // A context menu is invoked in response to a click, so anchor it at the
    // mouse cursor.  (The Left/Top the caller passes are in client coordinates
    // that the port's no-op ClientToScreen leaves unconverted, so the live
    // cursor position is the reliable anchor.)  GetPopupMenuSelectionFromUser
    // returns the chosen id without firing a menu event — exactly the
    // TPM_RETURNCMD behaviour CMiniWindow::Menu expects.
    wxPoint pt = win->ScreenToClient(::wxGetMousePosition());
    int sel = win->GetPopupMenuSelectionFromUser(*wxm, pt);
    delete wxm;
    return (sel > 0) ? sel : 0;   // wxID_NONE / negative ⇒ cancelled
}

// ── password_dialog ───────────────────────────────────────────────────────
// Called from CPasswordDialog::DoModal() on non-Windows when connecting to a
// world that has a character name but no stored password.

static int hook_password_dialog(const char* charName, char** outPassword)
{
    wxWindow* parent = App.m_pMainWnd
                      ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                      : nullptr;

    wxString prompt;
    if (charName && *charName)
        prompt.Printf(wxT("Enter password for %s:"),
                      wxString::FromUTF8(charName));
    else
        prompt = wxT("Enter password:");

    wxPasswordEntryDialog dlg(parent, prompt, wxT("Password Required"));
    if (dlg.ShowModal() != wxID_OK) return IDCANCEL;

    wxString pw = dlg.GetValue();
    if (pw.IsEmpty()) return IDCANCEL;

    *outPassword = strdup(pw.ToUTF8().data());
    return IDOK;
}

// ── MFC dialog bridge: DoModal / EndDialog ────────────────────────────────
// Maps a live CDialog* to the wxDialog backing its modal loop, so EndDialog
// (which the canonical OnOK/OnCancel call) can end the right ShowModal.
static std::map<void*, wxDialog*> s_mfcModalDialogs;

static void hook_end_dialog(void* self, int result)
{
    auto it = s_mfcModalDialogs.find(self);
    if (it != s_mfcModalDialogs.end() && it->second && it->second->IsModal())
        it->second->EndModal(result);
}

static intptr_t hook_run_dialog(void* self, unsigned idd)
{
    CDialog* cdlg = static_cast<CDialog*>(self);
    const RcDialog* t = MfcDialogResources().FindDialog((int)idd);
    if (!t)
    {
        std::fprintf(stderr, "[dialog-bridge] DoModal: template %u not found\n", idd);
        return IDCANCEL;
    }

    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxDialog* dlg = BuildWxDialogFromTemplate(parent, *t);
    if (!dlg) return IDCANCEL;

    cdlg->m_hWnd = (HWND) dlg;
    s_mfcModalDialogs[self] = dlg;

    // Route control commands to the canonical handlers.  OK/Cancel use the MFC
    // virtuals (which call EndDialog); other buttons will dispatch through the
    // reactivated message map (task #12) — for now they are inert.
    // OK/Cancel use the CDialog virtuals; every other control command is routed
    // through the reactivated message map to its canonical ON_BN_CLICKED/etc.
    // handler.  Buttons, checkboxes and radios all send BN_CLICKED.
    dlg->Bind(wxEVT_BUTTON, [cdlg](wxCommandEvent& e) {
        int id = e.GetId();
        if (id == IDOK)          cdlg->OnOK();
        else if (id == IDCANCEL) cdlg->OnCancel();
        else if (!AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) id)) e.Skip();
    });
    dlg->Bind(wxEVT_CHECKBOX, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) e.GetId())) e.Skip();
    });
    dlg->Bind(wxEVT_RADIOBUTTON, [cdlg](wxCommandEvent& e) {
        if (!AfxDispatchCommand(cdlg, BN_CLICKED, (UINT) e.GetId())) e.Skip();
    });
    // Closing the window (X / Esc) behaves like Cancel.
    dlg->Bind(wxEVT_CLOSE_WINDOW, [dlg](wxCloseEvent&) {
        if (dlg->IsModal()) dlg->EndModal(IDCANCEL);
    });

    cdlg->OnInitDialog();

    int result = dlg->ShowModal();

    cdlg->m_hWnd = nullptr;
    s_mfcModalDialogs.erase(self);
    dlg->Destroy();
    return result;
}

// ── MFC property-sheet bridge: CTreePropertySheet::DoModal -> wxTreebook ──────
// World Properties (24 pages).  The canonical CPropertyPage objects are reused
// verbatim; we render them as a native wxTreebook (branch -> top-level page,
// node/endbranch -> child of the last top-level page), populate each page panel
// from its recovered template, run its OnInitDialog (canonical setup +
// UpdateData(FALSE) DDX load), and route in-page control commands through the
// reactivated message map.  On OK every page does UpdateData(TRUE) so the
// canonical GamePreferences ChangedPrefsP*/SavePrefsP* see the edited values.
static intptr_t hook_run_property_sheet(void* self)
{
    CTreePropertySheet* sheet = static_cast<CTreePropertySheet*>(self);
    if (!sheet) return IDCANCEL;

    wxWindow* parent = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;
    wxString title = sheet->m_caption.IsEmpty()
                       ? wxString(wxT("World Properties"))
                       : wxString::FromUTF8((const char*) sheet->m_caption);

    wxDialog dlg(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    wxTreebook* tb = new wxTreebook(&dlg, wxID_ANY);

    // Pages, index-aligned with the treebook (insertion order == add order ==
    // the canonical nPage that m_iWantedPage is compared against).
    std::vector<CPropertyPage*> pages;
    pages.reserve(sheet->m_portPages.size());

    for (const auto& pp : sheet->m_portPages)
    {
        CPropertyPage* page  = pp.pPage;
        wxPanel*       panel = new wxPanel(tb, wxID_ANY);

        const RcDialog* t = page
            ? MfcDialogResources().FindDialog((int) page->m_nIDTemplate)
            : nullptr;
        if (t)
            PopulateWindowFromTemplate(panel, *t);
        else
            panel->SetMinClientSize(wxSize(420, 240));  // keep index aligned

        wxString label = wxString::FromUTF8((const char*) pp.caption);
        if ((pp.nType & tps_item_type_mask) == tps_item_branch)
            tb->AddPage(panel, label);
        else
            tb->AddSubPage(panel, label);

        pages.push_back(page);

        if (page)
        {
            page->m_hWnd = (HWND) panel;   // GetDlgItem/DDX search within this panel

            // Route this page's in-panel control commands to its canonical
            // ON_BN_CLICKED/etc. handlers (e.g. checkboxes that enable siblings).
            panel->Bind(wxEVT_BUTTON, [page](wxCommandEvent& e) {
                if (!AfxDispatchCommand(page, BN_CLICKED, (UINT) e.GetId())) e.Skip();
            });
            panel->Bind(wxEVT_CHECKBOX, [page](wxCommandEvent& e) {
                if (!AfxDispatchCommand(page, BN_CLICKED, (UINT) e.GetId())) e.Skip();
            });
            panel->Bind(wxEVT_RADIOBUTTON, [page](wxCommandEvent& e) {
                if (!AfxDispatchCommand(page, BN_CLICKED, (UINT) e.GetId())) e.Skip();
            });

            page->OnInitDialog();   // canonical setup + DDX load (UpdateData(FALSE))
        }
    }

    // Open at the wanted page (m_iWantedPage == add-order index; see
    // TreePropertySheet.cpp OnInitDialog `nPage == m_iWantedPage`).
    int wanted = sheet->m_iWantedPage;
    if (wanted >= 0 && wanted < (int) tb->GetPageCount())
        tb->SetSelection((size_t) wanted);

    // Validate-on-leave, matching MFC's OnKillActive: when the user navigates
    // away from a page, capture its controls into members (UpdateData(TRUE)) and
    // run its DDV; if validation fails the page already showed its message, so
    // veto the page change and stay put.  Bound AFTER the initial SetSelection so
    // it doesn't fire during setup.  Pages never visited keep their LoadPrefs
    // values (valid) — exactly as on Windows, where unvisited pages aren't
    // validated and GamePreferences' SavePrefsP* then writes back unchanged.
    tb->Bind(wxEVT_TREEBOOK_PAGE_CHANGING, [&pages](wxBookCtrlEvent& e) {
        int leaving = e.GetOldSelection();
        if (leaving >= 0 && leaving < (int) pages.size()
            && pages[leaving] && !pages[leaving]->UpdateData(TRUE))
            e.Veto();   // validation failed (message shown); stay on this page
    });

    // Standard OK / Cancel.
    wxStdDialogButtonSizer* btns = new wxStdDialogButtonSizer();
    wxButton* ok = new wxButton(&dlg, wxID_OK);
    btns->AddButton(ok);
    btns->AddButton(new wxButton(&dlg, wxID_CANCEL));
    btns->Realize();
    ok->SetDefault();

    wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
    top->Add(tb,   1, wxEXPAND | wxALL, 6);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    dlg.SetSizer(top);
    dlg.Fit();
    wxSize sz = dlg.GetSize();
    dlg.SetSize(wxSize(std::max(sz.x, 680), std::max(sz.y, 520)));
    dlg.CentreOnParent();

    // OK: validate the ACTIVE page only (MFC validates the active page's
    // OnKillActive on OK; other pages were already captured/validated when the
    // user navigated away from them, and unvisited pages keep their loaded
    // values).  If the active page fails validation it has shown its message, so
    // keep the sheet open.  Accept (EndModal) only when the active page passes.
    // (Cancel keeps wxDialog's default: ends the modal loop with wxID_CANCEL.)
    dlg.Bind(wxEVT_BUTTON, [&dlg, &pages, tb](wxCommandEvent& e) {
        if (e.GetId() != wxID_OK) { e.Skip(); return; }
        int active = tb->GetSelection();
        if (active >= 0 && active < (int) pages.size()
            && pages[active] && !pages[active]->UpdateData(TRUE))
            return;   // validation failed (message shown); stay open
        dlg.EndModal(wxID_OK);
    }, wxID_OK);

    int rc = dlg.ShowModal();

    for (CPropertyPage* page : pages)            // detach before panels die
        if (page) page->m_hWnd = nullptr;

    return (rc == wxID_OK) ? IDOK : IDCANCEL;
}

// ── Dialog control accessors (GetDlgItem / DDX) ───────────────────────────
static void* hook_get_dlg_item(void* parent, int id)
{
    return parent ? ((wxWindow*) parent)->FindWindow(id) : nullptr;
}

static char* hook_get_window_text(void* hwnd)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (!w) return nullptr;
    wxString s;
    if (wxTextCtrl* t = wxDynamicCast(w, wxTextCtrl))      s = t->GetValue();
    else if (wxComboBox* c = wxDynamicCast(w, wxComboBox)) s = c->GetValue();
    else                                                   s = w->GetLabel();
    return strdup(s.utf8_str());
}

static void hook_button_set_check(void* hwnd, int state)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (wxCheckBox* cb = wxDynamicCast(w, wxCheckBox))         cb->SetValue(state != 0);
    else if (wxRadioButton* rb = wxDynamicCast(w, wxRadioButton)) rb->SetValue(state != 0);
}

static int hook_button_get_check(void* hwnd)
{
    wxWindow* w = (wxWindow*) hwnd;
    if (wxCheckBox* cb = wxDynamicCast(w, wxCheckBox))            return cb->GetValue() ? 1 : 0;
    if (wxRadioButton* rb = wxDynamicCast(w, wxRadioButton))      return rb->GetValue() ? 1 : 0;
    return 0;
}

static void hook_enable_window(void* hwnd, int enable)
{
    if (hwnd) ((wxWindow*) hwnd)->Enable(enable != 0);
}

// ── CListCtrl → wxListCtrl (entity-manager object lists) ──────────────────
// `hwnd` is the wxListCtrl* (bound onto a CListCtrl member by DDX_Control).
// The canonical CGenPropertyPage list pages drive these in report mode and stash
// a CString* (the object name) as each row's item data.
namespace {
inline wxListCtrl* LC(void* hwnd) { return wxDynamicCast((wxWindow*) hwnd, wxListCtrl); }
}

static void hook_listctrl_insert_column(void* hwnd, int col, const char* heading,
                                        int fmt, int width)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return;
    // LVCFMT_RIGHT=1, LVCFMT_CENTER=2 → wxLIST_FORMAT_*; default left.
    int wxfmt = (fmt == 1) ? wxLIST_FORMAT_RIGHT
              : (fmt == 2) ? wxLIST_FORMAT_CENTRE
              :              wxLIST_FORMAT_LEFT;
    lc->InsertColumn(col, wxString::FromUTF8(heading ? heading : ""), wxfmt,
                     width > 0 ? width : wxLIST_AUTOSIZE);
}

static int hook_listctrl_insert_item(void* hwnd, int item, const char* text)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return -1;
    return lc->InsertItem(item, wxString::FromUTF8(text ? text : ""));
}

static void hook_listctrl_set_item_text(void* hwnd, int item, int col, const char* text)
{
    if (wxListCtrl* lc = LC(hwnd))
        lc->SetItem(item, col, wxString::FromUTF8(text ? text : ""));
}

static int hook_listctrl_get_item_count(void* hwnd)
{
    wxListCtrl* lc = LC(hwnd);
    return lc ? lc->GetItemCount() : 0;
}

static void hook_listctrl_set_item_data(void* hwnd, int item, void* data)
{
    if (wxListCtrl* lc = LC(hwnd))
        lc->SetItemPtrData(item, (wxUIntPtr) data);
}

static void* hook_listctrl_get_item_data(void* hwnd, int item)
{
    wxListCtrl* lc = LC(hwnd);
    return lc ? (void*) lc->GetItemData(item) : nullptr;
}

// MFC: GetNextItem(start, flags) returns the next item after `start` matching
// `flags` (LVNI_ALL=0 → any; LVNI_SELECTED=2 → selected), or -1.  start==-1
// means "from the beginning".
static int hook_listctrl_get_next_item(void* hwnd, int start, int flags)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return -1;
    int geom = (flags & 0x2 /*LVNI_SELECTED*/)
                 ? wxLIST_NEXT_ALL : wxLIST_NEXT_ALL;
    int state = (flags & 0x2 /*LVNI_SELECTED*/)
                 ? wxLIST_STATE_SELECTED : wxLIST_STATE_DONTCARE;
    return lc->GetNextItem(start, geom, state);
}

static void hook_listctrl_set_item_state(void* hwnd, int item, unsigned state, unsigned mask)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return;
    long wxstate = 0, wxmask = 0;
    if (mask & 0x2 /*LVIS_SELECTED*/) { wxmask |= wxLIST_STATE_SELECTED;
        if (state & 0x2) wxstate |= wxLIST_STATE_SELECTED; }
    if (mask & 0x1 /*LVIS_FOCUSED*/)  { wxmask |= wxLIST_STATE_FOCUSED;
        if (state & 0x1) wxstate |= wxLIST_STATE_FOCUSED; }
    lc->SetItemState(item, wxstate, wxmask);
}

static unsigned hook_listctrl_get_item_state(void* hwnd, int item, unsigned mask)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc) return 0;
    unsigned out = 0;
    if ((mask & 0x2) && lc->GetItemState(item, wxLIST_STATE_SELECTED)) out |= 0x2;
    if ((mask & 0x1) && lc->GetItemState(item, wxLIST_STATE_FOCUSED))  out |= 0x1;
    return out;
}

static int hook_listctrl_get_selected_count(void* hwnd)
{
    wxListCtrl* lc = LC(hwnd);
    return lc ? lc->GetSelectedItemCount() : 0;
}

static void hook_listctrl_delete_item(void* hwnd, int item)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->DeleteItem(item);
}

static void hook_listctrl_delete_all_items(void* hwnd)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->DeleteAllItems();
}

static void hook_listctrl_ensure_visible(void* hwnd, int item)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->EnsureVisible(item);
}

static void hook_listctrl_set_column_width(void* hwnd, int col, int width)
{
    if (wxListCtrl* lc = LC(hwnd)) lc->SetColumnWidth(col, width);
}

// CListCtrl::SortItems(cmp, lparam): cmp compares two rows by their item data
// (the CString* the page stored).  wxListCtrl::SortItems does exactly this —
// it reorders rows (text + data move together) by a comparator over item data.
namespace {
struct ListSortCtx { int (*cmp)(intptr_t, intptr_t, intptr_t); intptr_t lparam; };
int wxCALLBACK ListSortTrampoline(wxIntPtr a, wxIntPtr b, wxIntPtr ctxp)
{
    ListSortCtx* ctx = (ListSortCtx*) ctxp;
    return ctx->cmp((intptr_t) a, (intptr_t) b, ctx->lparam);
}
}
static void hook_listctrl_sort_items(void* hwnd,
        int (*cmp)(intptr_t, intptr_t, intptr_t), intptr_t lparam)
{
    wxListCtrl* lc = LC(hwnd);
    if (!lc || !cmp) return;
    ListSortCtx ctx{cmp, lparam};
    lc->SortItems(ListSortTrampoline, (wxIntPtr) &ctx);
}

void wx_mfc_install_hooks()
{
    g_run_mfc_dialog          = hook_run_dialog;
    g_end_mfc_dialog          = hook_end_dialog;
    g_run_mfc_property_sheet  = hook_run_property_sheet;
    g_wxhook_get_dlg_item     = hook_get_dlg_item;
    g_wxhook_get_window_text  = hook_get_window_text;
    g_wxhook_button_set_check = hook_button_set_check;
    g_wxhook_button_get_check = hook_button_get_check;
    g_wxhook_enable_window    = hook_enable_window;
    g_wxhook_listctrl_insert_column   = hook_listctrl_insert_column;
    g_wxhook_listctrl_insert_item     = hook_listctrl_insert_item;
    g_wxhook_listctrl_set_item_text   = hook_listctrl_set_item_text;
    g_wxhook_listctrl_get_item_count  = hook_listctrl_get_item_count;
    g_wxhook_listctrl_set_item_data   = hook_listctrl_set_item_data;
    g_wxhook_listctrl_get_item_data   = hook_listctrl_get_item_data;
    g_wxhook_listctrl_get_next_item   = hook_listctrl_get_next_item;
    g_wxhook_listctrl_set_item_state  = hook_listctrl_set_item_state;
    g_wxhook_listctrl_get_item_state  = hook_listctrl_get_item_state;
    g_wxhook_listctrl_get_selected_count = hook_listctrl_get_selected_count;
    g_wxhook_listctrl_delete_item     = hook_listctrl_delete_item;
    g_wxhook_listctrl_delete_all_items = hook_listctrl_delete_all_items;
    g_wxhook_listctrl_ensure_visible  = hook_listctrl_ensure_visible;
    g_wxhook_listctrl_set_column_width = hook_listctrl_set_column_width;
    g_wxhook_listctrl_sort_items      = hook_listctrl_sort_items;
    g_wxhook_invalidate       = hook_invalidate;
    g_wxhook_show_window      = hook_show_window;
    g_wxhook_update_window    = hook_update_window;
    g_wxhook_get_client_rect  = hook_get_client_rect;
    g_wxhook_set_window_text  = hook_set_window_text;
    g_wxhook_move_window      = hook_move_window;
    g_wxhook_destroy_window   = hook_destroy_window;
    g_wxhook_set_focus        = hook_set_focus;
    g_wxhook_send_message     = hook_send_message;
    g_wxhook_post_message     = hook_post_message;
    g_wxhook_message_box      = hook_message_box;
    g_wxhook_load_mdi_frame   = hook_load_mdi_frame;
    g_wxhook_init_instance    = hook_init_instance;
    g_wxhook_new_world          = hook_new_world;
    g_wxhook_open_world_file    = hook_open_world_file;
    g_wxhook_quick_connect_dlg  = hook_quick_connect_dlg;
    g_wxhook_set_status         = hook_set_status;
    g_wxhook_save_world         = hook_save_world;
    g_wxhook_file_dialog        = hook_file_dialog;
    g_wxhook_get_clipboard_text   = hook_get_clipboard_text;
    g_wxhook_set_clipboard_text   = hook_set_clipboard_text;
    g_wxhook_create_text_window   = hook_create_text_window;
    g_wxhook_pick_colour          = hook_pick_colour;
    g_wxhook_menu_dialog          = hook_menu_dialog;
    g_wxhook_flash_icon           = hook_flash_icon;
    g_wxhook_move_main_window     = hook_move_main_window;
    g_wxhook_inputbox             = hook_inputbox;
    g_wxhook_directorypicker      = hook_directorypicker;
    g_wxhook_password_dialog      = hook_password_dialog;
    g_wxhook_popup_menu           = hook_popup_menu;
#if wxUSE_SOUND
    g_wxhook_play_sound           = hook_play_sound;
    g_wxhook_stop_sound           = hook_stop_sound;
#endif
    // Notepad hooks do not depend on wxUSE_SOUND — always install them.
    g_wxhook_append_to_notepad    = hook_append_to_notepad;
    g_wxhook_get_notepad_text     = hook_get_notepad_text;
    g_wxhook_get_notepad_list     = hook_get_notepad_list;
    g_wxhook_activate_notepad     = hook_activate_notepad;
    g_wxhook_close_notepad        = hook_close_notepad;
    g_wxhook_move_notepad         = hook_move_notepad;
    g_wxhook_get_notepad_position = hook_get_notepad_position;
    g_wxhook_save_notepad         = hook_save_notepad;
    g_wxhook_notepad_colour       = hook_notepad_colour;
    g_wxhook_notepad_font         = hook_notepad_font;
    g_wxhook_notepad_readonly     = hook_notepad_readonly;

    // Wire the system-beep hook so doc.cpp's MessageBeep() calls produce
    // an audible alert on macOS/Linux via wxWidgets.
    g_fn_system_beep = []() { wxBell(); };
}

#endif // !_WIN32

// ── utils.inputbox / utils.editbox ────────────────────────────────────────
// Shows a simple text entry dialog.  Returns heap-allocated string (caller
// frees) or nullptr on cancel.
// NOTE: appended after the end of the file by the port script; the
// wx_mfc_install_hooks() call below must remain LAST so these static
// functions are visible to it.
