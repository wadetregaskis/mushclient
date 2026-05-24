// platform/wx_entity_dlgs.cpp — Trigger / Alias / Timer editor dialogs
//
// Include order must follow the same rule as wx_world_child.cpp:
//   1. wx headers first (so wx's symbols are visible before Windows-compat shims)
//   2. MUSHclient / MFC-compat headers after
//
// Dialogs operate directly on CMUSHclientDoc's maps/arrays without going
// through the Lua layer.  Uses the same logic as lua_world_api_portable.cpp
// (regcomp, ConvertToRegularExpression, SortTriggers/SortAliases).

#ifndef _WIN32

// ── wxWidgets ─────────────────────────────────────────────────────────────
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/wrapsizer.h>

// ── MUSHclient headers ────────────────────────────────────────────────────
// Same include order as wx_world_child.cpp so the CMUSHclientDoc type matches
// the mushapp-compiled version linked into mushgui.
#include "stdafx.h"           // compat.h → mfc_ui.h; pulls in OtherTypes.h
#include "MUSHclient.h"       // OtherTypes.h → CTrigger, CAlias, CTimer, eSendTo*
#include "doc.h"              // CMUSHclientDoc: GetTriggerMap, SortTriggers, …
#include "flags.h"            // eEnabled, eIgnoreCase, eTriggerRegularExpression, …
#include "regexp.h"           // t_regexp, regcomp
#include "mcpcre/pcre.h"      // PCRE_CASELESS
#include "scripting/errors.h" // eOK

#include "platform/wx_entity_dlgs.h"

#include <vector>
#include <string>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static wxString cs2wx(const CString& s) { return wxString((const char*)s, wxConvUTF8); }
static CString  wx2cs(const wxString& s) { return CString(s.ToUTF8().data()); }

// ConvertToRegularExpression is declared in stdafx.h (via full build).
// Compile regexp for a trigger/alias pattern (bRegexp=false → convert first).
// Returns nullptr on failure and sets errMsg.
static t_regexp* compile_pattern(const CString& match, bool bRegexp,
                                  bool bIgnoreCase, wxString& errMsg)
{
    CString strRegexp = bRegexp ? match : ConvertToRegularExpression(match);
    t_regexp* re = nullptr;
    try {
        re = regcomp(strRegexp, bIgnoreCase ? PCRE_CASELESS : 0);
    } catch (...) {
        errMsg = wxString::Format("Bad regular expression: %s",
                                  (const char*)strRegexp);
        return nullptr;
    }
    if (!re) { errMsg = "Failed to compile pattern."; }
    return re;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Trigger edit dialog ───────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

class CTriggerEditDlg : public wxDialog
{
public:
    // Pass nullptr for pTrigger to create a new one; non-null to edit existing.
    CTriggerEditDlg(wxWindow* parent, CMUSHclientDoc* pDoc,
                    CTrigger* pTrigger = nullptr)
        : wxDialog(parent, wxID_ANY,
                   pTrigger ? "Edit Trigger" : "Add Trigger",
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_pDoc(pDoc), m_pTrigger(pTrigger)
    {
        auto* top = new wxBoxSizer(wxVERTICAL);

        // ── Identity ─────────────────────────────────────────────────────────
        auto* grid = new wxFlexGridSizer(2, wxSize(6, 4));
        grid->AddGrowableCol(1, 1);

        auto addRow = [&](const wxString& lbl, wxWindow* w) {
            grid->Add(new wxStaticText(this, wxID_ANY, lbl),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(w, 1, wxEXPAND);
        };

        m_label    = new wxTextCtrl(this, wxID_ANY);
        m_match    = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                    wxSize(360, -1));
        m_send     = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                    wxSize(360, 70),
                                    wxTE_MULTILINE | wxTE_PROCESS_ENTER);
        m_script   = new wxTextCtrl(this, wxID_ANY);
        m_sequence = new wxSpinCtrl(this, wxID_ANY, "100",
                                    wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0, 10000, 100);

        addRow("Label:",    m_label);
        addRow("Match:",    m_match);
        addRow("Send:",     m_send);
        addRow("Script:",   m_script);
        addRow("Sequence:", m_sequence);

        top->Add(grid, 0, wxEXPAND | wxALL, 8);

        // ── Flags ─────────────────────────────────────────────────────────────
        auto* flagBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
        auto* flags1  = new wxWrapSizer(wxHORIZONTAL);

        auto addCB = [&](const wxString& lbl) {
            auto* cb = new wxCheckBox(this, wxID_ANY, lbl);
            flags1->Add(cb, 0, wxRIGHT | wxBOTTOM, 8);
            return cb;
        };

        m_cbEnabled   = addCB("Enabled");
        m_cbRegexp    = addCB("Regular expression");
        m_cbIgnCase   = addCB("Ignore case");
        m_cbKeepEval  = addCB("Keep evaluating");
        m_cbOmitOut   = addCB("Omit from output");
        m_cbOmitLog   = addCB("Omit from log");
        m_cbExpVars   = addCB("Expand variables");
        m_cbRepeat    = addCB("Repeat");
        m_cbOneShot   = addCB("One shot");

        flagBox->Add(flags1, 1, wxEXPAND | wxALL, 4);
        top->Add(flagBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        top->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
                 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        SetSizerAndFit(top);
        SetMinSize(wxSize(420, -1));

        if (m_pTrigger) LoadFromTrigger();
        else m_cbEnabled->SetValue(true);

        Bind(wxEVT_BUTTON, &CTriggerEditDlg::OnOK, this, wxID_OK);
    }

    wxString m_resultName;

private:
    CMUSHclientDoc* m_pDoc;
    CTrigger*       m_pTrigger;

    wxTextCtrl*  m_label;
    wxTextCtrl*  m_match;
    wxTextCtrl*  m_send;
    wxTextCtrl*  m_script;
    wxSpinCtrl*  m_sequence;
    wxCheckBox*  m_cbEnabled;
    wxCheckBox*  m_cbRegexp;
    wxCheckBox*  m_cbIgnCase;
    wxCheckBox*  m_cbKeepEval;
    wxCheckBox*  m_cbOmitOut;
    wxCheckBox*  m_cbOmitLog;
    wxCheckBox*  m_cbExpVars;
    wxCheckBox*  m_cbRepeat;
    wxCheckBox*  m_cbOneShot;

    void LoadFromTrigger()
    {
        CTrigger* t = m_pTrigger;
        m_label   ->SetValue(cs2wx(t->strLabel));
        m_match   ->SetValue(cs2wx(t->trigger));
        m_send    ->SetValue(cs2wx(t->contents));
        m_script  ->SetValue(cs2wx(t->strProcedure));
        m_sequence->SetValue(t->iSequence);
        m_cbEnabled ->SetValue(t->bEnabled        != 0);
        m_cbRegexp  ->SetValue(t->bRegexp         != 0);
        m_cbIgnCase ->SetValue(t->ignore_case     != 0);
        m_cbKeepEval->SetValue(t->bKeepEvaluating != 0);
        m_cbOmitOut ->SetValue(t->bOmitFromOutput != 0);
        m_cbOmitLog ->SetValue(t->omit_from_log   != 0);
        m_cbExpVars ->SetValue(t->bExpandVariables != 0);
        m_cbRepeat  ->SetValue(t->bRepeat         != 0);
        m_cbOneShot ->SetValue(t->bOneShot        != 0);
    }

    void OnOK(wxCommandEvent&)
    {
        CString label = wx2cs(m_label->GetValue().Trim());
        CString match = wx2cs(m_match->GetValue());

        if (label.IsEmpty()) {
            wxMessageBox("Label cannot be empty.", "Error", wxICON_ERROR, this); return;
        }
        if (match.IsEmpty()) {
            wxMessageBox("Match pattern cannot be empty.", "Error", wxICON_ERROR, this); return;
        }
        if (CheckObjectName(label) != eOK) {
            wxMessageBox("Label contains invalid characters.", "Error", wxICON_ERROR, this); return;
        }

        bool bRegexp  = m_cbRegexp ->GetValue();
        bool bIgnCase = m_cbIgnCase->GetValue();

        wxString errMsg;
        t_regexp* re = compile_pattern(match, bRegexp, bIgnCase, errMsg);
        if (!re) { wxMessageBox(errMsg, "Regexp Error", wxICON_ERROR, this); return; }

        bool labelChanged = !m_pTrigger ||
                            (label.CompareNoCase(m_pTrigger->strLabel) != 0);
        CTrigger* existing = nullptr;
        if (labelChanged && m_pDoc->GetTriggerMap().Lookup(label, existing)) {
            delete re;
            wxMessageBox("A trigger with that label already exists.", "Error",
                         wxICON_ERROR, this);
            return;
        }

        // All validation passed — now mutate the data.
        CTrigger* t = m_pTrigger;
        if (t) {
            if (labelChanged) {
                m_pDoc->GetTriggerMap().RemoveKey(m_pTrigger->strLabel);
                CTriggerArray& arr = m_pDoc->GetTriggerArray();
                for (int i = 0; i < arr.GetSize(); ++i) {
                    if (arr[i] == t) { arr.RemoveAt(i); break; }
                }
            }
            delete t->regexp;
        } else {
            t = new CTrigger();
            m_pTrigger = t;
        }

        t->strLabel         = label;
        t->strInternalName  = label;
        t->trigger          = match;
        t->contents         = wx2cs(m_send->GetValue());
        t->strProcedure     = wx2cs(m_script->GetValue().Trim());
        t->iSequence        = m_sequence->GetValue();
        t->bEnabled         = m_cbEnabled ->GetValue() ? TRUE : FALSE;
        t->bRegexp          = bRegexp     ? TRUE : FALSE;
        t->ignore_case      = bIgnCase    ? TRUE : FALSE;
        t->bKeepEvaluating  = m_cbKeepEval->GetValue() ? TRUE : FALSE;
        t->bOmitFromOutput  = m_cbOmitOut ->GetValue() ? TRUE : FALSE;
        t->omit_from_log    = m_cbOmitLog ->GetValue() ? TRUE : FALSE;
        t->bExpandVariables = m_cbExpVars ->GetValue() ? TRUE : FALSE;
        t->bRepeat          = m_cbRepeat  ->GetValue() ? TRUE : FALSE;
        t->bOneShot         = m_cbOneShot ->GetValue() ? TRUE : FALSE;
        t->regexp           = re;
        t->dispid           = DISPID_UNKNOWN;

        m_pDoc->GetTriggerMap().SetAt(label, t);
        if (labelChanged) m_pDoc->GetTriggerArray().Add(t);
        m_pDoc->SortTriggers();

        m_resultName = cs2wx(label);
        EndModal(wxID_OK);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ── Alias edit dialog ─────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

class CAliasEditDlg : public wxDialog
{
public:
    CAliasEditDlg(wxWindow* parent, CMUSHclientDoc* pDoc,
                  CAlias* pAlias = nullptr)
        : wxDialog(parent, wxID_ANY,
                   pAlias ? "Edit Alias" : "Add Alias",
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_pDoc(pDoc), m_pAlias(pAlias)
    {
        auto* top = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, wxSize(6, 4));
        grid->AddGrowableCol(1, 1);

        auto addRow = [&](const wxString& lbl, wxWindow* w) {
            grid->Add(new wxStaticText(this, wxID_ANY, lbl),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(w, 1, wxEXPAND);
        };

        m_label  = new wxTextCtrl(this, wxID_ANY);
        m_match  = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(360,-1));
        m_send   = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                   wxSize(360, 70),
                                   wxTE_MULTILINE | wxTE_PROCESS_ENTER);
        m_script = new wxTextCtrl(this, wxID_ANY);

        addRow("Label:",  m_label);
        addRow("Match:",  m_match);
        addRow("Send:",   m_send);
        addRow("Script:", m_script);

        top->Add(grid, 0, wxEXPAND | wxALL, 8);

        auto* flagBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
        auto* flags1  = new wxWrapSizer(wxHORIZONTAL);

        auto addCB = [&](const wxString& lbl) {
            auto* cb = new wxCheckBox(this, wxID_ANY, lbl);
            flags1->Add(cb, 0, wxRIGHT | wxBOTTOM, 8);
            return cb;
        };

        m_cbEnabled  = addCB("Enabled");
        m_cbRegexp   = addCB("Regular expression");
        m_cbIgnCase  = addCB("Ignore case");
        m_cbKeepEval = addCB("Keep evaluating");
        m_cbEcho     = addCB("Echo alias");
        m_cbOmitOut  = addCB("Omit from output");
        m_cbOmitLog  = addCB("Omit from log");
        m_cbOneShot  = addCB("One shot");

        flagBox->Add(flags1, 1, wxEXPAND | wxALL, 4);
        top->Add(flagBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        top->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
                 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        SetSizerAndFit(top);
        SetMinSize(wxSize(420, -1));

        if (m_pAlias) LoadFromAlias();
        else m_cbEnabled->SetValue(true);

        Bind(wxEVT_BUTTON, &CAliasEditDlg::OnOK, this, wxID_OK);
    }

    wxString m_resultName;

private:
    CMUSHclientDoc* m_pDoc;
    CAlias*         m_pAlias;

    wxTextCtrl* m_label;
    wxTextCtrl* m_match;
    wxTextCtrl* m_send;
    wxTextCtrl* m_script;
    wxCheckBox* m_cbEnabled;
    wxCheckBox* m_cbRegexp;
    wxCheckBox* m_cbIgnCase;
    wxCheckBox* m_cbKeepEval;
    wxCheckBox* m_cbEcho;
    wxCheckBox* m_cbOmitOut;
    wxCheckBox* m_cbOmitLog;
    wxCheckBox* m_cbOneShot;

    void LoadFromAlias()
    {
        CAlias* a = m_pAlias;
        m_label ->SetValue(cs2wx(a->strLabel));
        m_match ->SetValue(cs2wx(a->name));
        m_send  ->SetValue(cs2wx(a->contents));
        m_script->SetValue(cs2wx(a->strProcedure));
        m_cbEnabled ->SetValue(a->bEnabled        != 0);
        m_cbRegexp  ->SetValue(a->bRegexp         != 0);
        m_cbIgnCase ->SetValue(a->bIgnoreCase     != 0);
        m_cbKeepEval->SetValue(a->bKeepEvaluating != 0);
        m_cbEcho    ->SetValue(a->bEchoAlias      != 0);
        m_cbOmitOut ->SetValue(a->bOmitFromOutput != 0);
        m_cbOmitLog ->SetValue(a->bOmitFromLog    != 0);
        m_cbOneShot ->SetValue(a->bOneShot        != 0);
    }

    void OnOK(wxCommandEvent&)
    {
        CString label = wx2cs(m_label->GetValue().Trim());
        CString match = wx2cs(m_match->GetValue());

        if (label.IsEmpty()) {
            wxMessageBox("Label cannot be empty.", "Error", wxICON_ERROR, this); return;
        }
        if (match.IsEmpty()) {
            wxMessageBox("Match pattern cannot be empty.", "Error", wxICON_ERROR, this); return;
        }
        if (CheckObjectName(label) != eOK) {
            wxMessageBox("Label contains invalid characters.", "Error", wxICON_ERROR, this); return;
        }

        bool bRegexp  = m_cbRegexp ->GetValue();
        bool bIgnCase = m_cbIgnCase->GetValue();

        wxString errMsg;
        t_regexp* re = compile_pattern(match, bRegexp, bIgnCase, errMsg);
        if (!re) { wxMessageBox(errMsg, "Regexp Error", wxICON_ERROR, this); return; }

        bool labelChanged = !m_pAlias ||
                            (label.CompareNoCase(m_pAlias->strLabel) != 0);
        CAlias* existing = nullptr;
        if (labelChanged && m_pDoc->GetAliasMap().Lookup(label, existing)) {
            delete re;
            wxMessageBox("An alias with that label already exists.", "Error",
                         wxICON_ERROR, this);
            return;
        }

        CAlias* a = m_pAlias;
        if (a) {
            if (labelChanged) {
                m_pDoc->GetAliasMap().RemoveKey(m_pAlias->strLabel);
                CAliasArray& arr = m_pDoc->GetAliasArray();
                for (int i = 0; i < arr.GetSize(); ++i) {
                    if (arr[i] == a) { arr.RemoveAt(i); break; }
                }
            }
            delete a->regexp;
        } else {
            a = new CAlias();
            m_pAlias = a;
        }

        a->strLabel         = label;
        a->strInternalName  = label;
        a->name             = match;
        a->contents         = wx2cs(m_send->GetValue());
        a->strProcedure     = wx2cs(m_script->GetValue().Trim());
        a->bEnabled         = m_cbEnabled ->GetValue() ? TRUE : FALSE;
        a->bRegexp          = bRegexp     ? TRUE : FALSE;
        a->bIgnoreCase      = bIgnCase    ? TRUE : FALSE;
        a->bKeepEvaluating  = m_cbKeepEval->GetValue() ? TRUE : FALSE;
        a->bEchoAlias       = m_cbEcho   ->GetValue()  ? TRUE : FALSE;
        a->bOmitFromOutput  = m_cbOmitOut->GetValue()  ? TRUE : FALSE;
        a->bOmitFromLog     = m_cbOmitLog->GetValue()  ? TRUE : FALSE;
        a->bOneShot         = m_cbOneShot->GetValue()  ? TRUE : FALSE;
        a->regexp           = re;
        a->dispid           = DISPID_UNKNOWN;

        m_pDoc->GetAliasMap().SetAt(label, a);
        if (labelChanged) m_pDoc->GetAliasArray().Add(a);
        m_pDoc->SortAliases();

        m_resultName = cs2wx(label);
        EndModal(wxID_OK);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ── Timer edit dialog ─────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

class CTimerEditDlg : public wxDialog
{
public:
    CTimerEditDlg(wxWindow* parent, CMUSHclientDoc* pDoc,
                  CTimer* pTimer = nullptr)
        : wxDialog(parent, wxID_ANY,
                   pTimer ? "Edit Timer" : "Add Timer",
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_pDoc(pDoc), m_pTimer(pTimer)
    {
        auto* top = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, wxSize(6, 4));
        grid->AddGrowableCol(1, 1);

        auto addRowW = [&](const wxString& lbl, wxWindow* w) {
            grid->Add(new wxStaticText(this, wxID_ANY, lbl),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(w, 1, wxEXPAND);
        };
        auto addRowS = [&](const wxString& lbl, wxSizer* s) {
            grid->Add(new wxStaticText(this, wxID_ANY, lbl),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(s, 1, wxEXPAND);
        };

        m_label  = new wxTextCtrl(this, wxID_ANY);
        m_send   = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                   wxSize(360, 70),
                                   wxTE_MULTILINE | wxTE_PROCESS_ENTER);
        m_script = new wxTextCtrl(this, wxID_ANY);

        auto* timeRow = new wxBoxSizer(wxHORIZONTAL);
        m_hour   = new wxSpinCtrl(this, wxID_ANY, "0", wxDefaultPosition,
                                   wxSize(60,-1), wxSP_ARROW_KEYS, 0, 23, 0);
        m_minute = new wxSpinCtrl(this, wxID_ANY, "0", wxDefaultPosition,
                                   wxSize(60,-1), wxSP_ARROW_KEYS, 0, 59, 0);
        m_second = new wxTextCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(70,-1));
        timeRow->Add(new wxStaticText(this, wxID_ANY, "h:"), 0, wxALIGN_CENTER_VERTICAL);
        timeRow->Add(m_hour,   0, wxRIGHT, 4);
        timeRow->Add(new wxStaticText(this, wxID_ANY, "m:"), 0, wxALIGN_CENTER_VERTICAL);
        timeRow->Add(m_minute, 0, wxRIGHT, 4);
        timeRow->Add(new wxStaticText(this, wxID_ANY, "s:"), 0, wxALIGN_CENTER_VERTICAL);
        timeRow->Add(m_second, 0);

        addRowW("Label:",    m_label);
        addRowS("Interval:", timeRow);
        addRowW("Send:",     m_send);
        addRowW("Script:",   m_script);

        top->Add(grid, 0, wxEXPAND | wxALL, 8);

        auto* flagBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
        auto* flags1  = new wxWrapSizer(wxHORIZONTAL);

        auto addCB = [&](const wxString& lbl) {
            auto* cb = new wxCheckBox(this, wxID_ANY, lbl);
            flags1->Add(cb, 0, wxRIGHT | wxBOTTOM, 8);
            return cb;
        };

        m_cbEnabled      = addCB("Enabled");
        m_cbAtTime       = addCB("Fire at time (not interval)");
        m_cbOneShot      = addCB("One shot");
        m_cbActiveClosed = addCB("Active when closed");

        flagBox->Add(flags1, 1, wxEXPAND | wxALL, 4);
        top->Add(flagBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        top->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
                 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        SetSizerAndFit(top);
        SetMinSize(wxSize(420, -1));

        if (m_pTimer) LoadFromTimer();
        else m_cbEnabled->SetValue(true);

        Bind(wxEVT_BUTTON, &CTimerEditDlg::OnOK, this, wxID_OK);
    }

    wxString m_resultName;

private:
    CMUSHclientDoc* m_pDoc;
    CTimer*         m_pTimer;

    wxTextCtrl* m_label;
    wxTextCtrl* m_send;
    wxTextCtrl* m_script;
    wxSpinCtrl* m_hour;
    wxSpinCtrl* m_minute;
    wxTextCtrl* m_second;
    wxCheckBox* m_cbEnabled;
    wxCheckBox* m_cbAtTime;
    wxCheckBox* m_cbOneShot;
    wxCheckBox* m_cbActiveClosed;

    void LoadFromTimer()
    {
        CTimer* t = m_pTimer;
        bool bAt = (t->iType == CTimer::eAtTime);
        m_label ->SetValue(cs2wx(t->strLabel));
        m_send  ->SetValue(cs2wx(t->strContents));
        m_script->SetValue(cs2wx(t->strProcedure));
        m_hour  ->SetValue(bAt ? t->iAtHour    : t->iEveryHour);
        m_minute->SetValue(bAt ? t->iAtMinute  : t->iEveryMinute);
        m_second->SetValue(wxString::Format("%.2f",
                            bAt ? t->fAtSecond : t->fEverySecond));
        m_cbEnabled     ->SetValue(t->bEnabled          != 0);
        m_cbAtTime      ->SetValue(bAt);
        m_cbOneShot     ->SetValue(t->bOneShot          != 0);
        m_cbActiveClosed->SetValue(t->bActiveWhenClosed != 0);
    }

    void OnOK(wxCommandEvent&)
    {
        CString label = wx2cs(m_label->GetValue().Trim());
        if (label.IsEmpty()) {
            wxMessageBox("Label cannot be empty.", "Error", wxICON_ERROR, this); return;
        }
        if (CheckObjectName(label) != eOK) {
            wxMessageBox("Label contains invalid characters.", "Error", wxICON_ERROR, this); return;
        }

        double sec = 0.0;
        if (!m_second->GetValue().ToDouble(&sec) || sec < 0.0 || sec >= 60.0) {
            wxMessageBox("Seconds must be between 0 and 59.99.", "Error", wxICON_ERROR, this); return;
        }
        int h = m_hour->GetValue();
        int m = m_minute->GetValue();
        bool bAt = m_cbAtTime->GetValue();
        if (!bAt && h == 0 && m == 0 && sec == 0.0) {
            wxMessageBox("Interval cannot be zero.", "Error", wxICON_ERROR, this); return;
        }

        bool labelChanged = !m_pTimer ||
                            (label.CompareNoCase(m_pTimer->strLabel) != 0);
        CTimer* existing = nullptr;
        if (labelChanged && m_pDoc->GetTimerMap().Lookup(label, existing)) {
            wxMessageBox("A timer with that label already exists.", "Error",
                         wxICON_ERROR, this);
            return;
        }

        CTimer* t = m_pTimer;
        if (!t) t = new CTimer();
        if (labelChanged && m_pTimer)
            m_pDoc->GetTimerMap().RemoveKey(m_pTimer->strLabel);

        t->strLabel          = label;
        t->strContents       = wx2cs(m_send->GetValue());
        t->strProcedure      = wx2cs(m_script->GetValue().Trim());
        t->bEnabled          = m_cbEnabled     ->GetValue() ? TRUE : FALSE;
        t->bOneShot          = m_cbOneShot     ->GetValue() ? TRUE : FALSE;
        t->bActiveWhenClosed = m_cbActiveClosed->GetValue() ? TRUE : FALSE;
        t->iType             = bAt ? CTimer::eAtTime : CTimer::eInterval;
        t->dispid            = DISPID_UNKNOWN;

        if (bAt) {
            t->iAtHour = h; t->iAtMinute = m; t->fAtSecond   = sec;
            t->iEveryHour = 0; t->iEveryMinute = 0; t->fEverySecond = 0.0;
        } else {
            t->iEveryHour = h; t->iEveryMinute = m; t->fEverySecond = sec;
            t->iAtHour = 0; t->iAtMinute = 0; t->fAtSecond = 0.0;
        }

        m_pDoc->GetTimerMap().SetAt(label, t);
        m_pDoc->ResetOneTimer(t);

        m_pTimer     = t;
        m_resultName = cs2wx(label);
        EndModal(wxID_OK);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ── List dialog shared IDs ────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

enum {
    // Base at +500 — avoids clash with kRecentMenuBase (+300) in wx_mfc_impl.cpp
    // and ID_MENU_CFG_* (+400..403).
    ID_TRIG_ADD  = wxID_HIGHEST + 500,
    ID_TRIG_EDIT,
    ID_TRIG_DEL,
    ID_ALIAS_ADD,
    ID_ALIAS_EDIT,
    ID_ALIAS_DEL,
    ID_TIMER_ADD,
    ID_TIMER_EDIT,
    ID_TIMER_DEL,
    ID_VAR_ADD,
    ID_VAR_EDIT,
    ID_VAR_DEL,
};

// ─────────────────────────────────────────────────────────────────────────────
// ── CTriggerListDlg ───────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

wxBEGIN_EVENT_TABLE(CTriggerListDlg, wxDialog)
    EVT_BUTTON(ID_TRIG_ADD,           CTriggerListDlg::OnAdd)
    EVT_BUTTON(ID_TRIG_EDIT,          CTriggerListDlg::OnEdit)
    EVT_BUTTON(ID_TRIG_DEL,           CTriggerListDlg::OnDelete)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, CTriggerListDlg::OnItemActivated)
wxEND_EVENT_TABLE()

CTriggerListDlg::CTriggerListDlg(wxWindow* parent, CMUSHclientDoc* pDoc)
    : wxDialog(parent, wxID_ANY, "Triggers",
               wxDefaultPosition, wxSize(700, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_pDoc(pDoc)
{
    auto* top = new wxBoxSizer(wxVERTICAL);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->AppendColumn("Label",  wxLIST_FORMAT_LEFT,  160);
    m_list->AppendColumn("Match",  wxLIST_FORMAT_LEFT,  220);
    m_list->AppendColumn("Script", wxLIST_FORMAT_LEFT,  120);
    m_list->AppendColumn("Seq",    wxLIST_FORMAT_RIGHT,  50);
    m_list->AppendColumn("On",     wxLIST_FORMAT_CENTER, 40);

    top->Add(m_list, 1, wxEXPAND | wxALL, 8);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->Add(new wxButton(this, ID_TRIG_ADD,  "Add"),    0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_TRIG_EDIT, "Edit"),   0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_TRIG_DEL,  "Delete"), 0, wxRIGHT, 4);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    top->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(top);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&){ Close(); }, wxID_CLOSE);
    Populate();
}

void CTriggerListDlg::Populate()
{
    m_list->DeleteAllItems();

    struct Row { CString label; CTrigger* t; };
    std::vector<Row> rows;
    CTriggerMap& map = m_pDoc->GetTriggerMap();
    CString key; CTrigger* t;
    for (POSITION pos = map.GetStartPosition(); pos; )
        map.GetNextAssoc(pos, key, t), rows.push_back({key, t});

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.t->iSequence != b.t->iSequence) return a.t->iSequence < b.t->iSequence;
        return a.label.CompareNoCase(b.label) < 0;
    });

    long i = 0;
    for (auto& r : rows) {
        m_list->InsertItem(i, cs2wx(r.t->strLabel));
        m_list->SetItem(i, 1, cs2wx(r.t->trigger));
        m_list->SetItem(i, 2, cs2wx(r.t->strProcedure));
        m_list->SetItem(i, 3, wxString::Format("%d", r.t->iSequence));
        m_list->SetItem(i, 4, r.t->bEnabled ? "✓" : "");
        m_list->SetItemData(i, (wxUIntPtr)r.t);
        ++i;
    }
}

void CTriggerListDlg::OnAdd(wxCommandEvent&)
{
    CTriggerEditDlg dlg(this, m_pDoc);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CTriggerListDlg::OnEdit(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select a trigger first.", "Edit"); return; }
    CTrigger* t = (CTrigger*)m_list->GetItemData(sel);
    CTriggerEditDlg dlg(this, m_pDoc, t);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CTriggerListDlg::OnDelete(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select a trigger first.", "Delete"); return; }
    CTrigger* t = (CTrigger*)m_list->GetItemData(sel);
    if (wxMessageBox(wxString::Format("Delete trigger '%s'?", cs2wx(t->strLabel)),
                     "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;
    m_pDoc->GetTriggerMap().RemoveKey(t->strLabel);
    CTriggerArray& arr = m_pDoc->GetTriggerArray();
    for (int i = 0; i < arr.GetSize(); ++i)
        if (arr[i] == t) { arr.RemoveAt(i); break; }
    delete t;
    Populate();
}

void CTriggerListDlg::OnItemActivated(wxListEvent& evt)
{
    CTrigger* t = (CTrigger*)evt.GetData();
    CTriggerEditDlg dlg(this, m_pDoc, t);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

// ─────────────────────────────────────────────────────────────────────────────
// ── CAliasListDlg ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

wxBEGIN_EVENT_TABLE(CAliasListDlg, wxDialog)
    EVT_BUTTON(ID_ALIAS_ADD,          CAliasListDlg::OnAdd)
    EVT_BUTTON(ID_ALIAS_EDIT,         CAliasListDlg::OnEdit)
    EVT_BUTTON(ID_ALIAS_DEL,          CAliasListDlg::OnDelete)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, CAliasListDlg::OnItemActivated)
wxEND_EVENT_TABLE()

CAliasListDlg::CAliasListDlg(wxWindow* parent, CMUSHclientDoc* pDoc)
    : wxDialog(parent, wxID_ANY, "Aliases",
               wxDefaultPosition, wxSize(660, 400),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_pDoc(pDoc)
{
    auto* top = new wxBoxSizer(wxVERTICAL);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->AppendColumn("Label",  wxLIST_FORMAT_LEFT,  160);
    m_list->AppendColumn("Match",  wxLIST_FORMAT_LEFT,  220);
    m_list->AppendColumn("Script", wxLIST_FORMAT_LEFT,  140);
    m_list->AppendColumn("On",     wxLIST_FORMAT_CENTER, 40);

    top->Add(m_list, 1, wxEXPAND | wxALL, 8);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->Add(new wxButton(this, ID_ALIAS_ADD,  "Add"),    0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_ALIAS_EDIT, "Edit"),   0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_ALIAS_DEL,  "Delete"), 0, wxRIGHT, 4);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    top->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(top);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&){ Close(); }, wxID_CLOSE);
    Populate();
}

void CAliasListDlg::Populate()
{
    m_list->DeleteAllItems();
    struct Row { CString label; CAlias* a; };
    std::vector<Row> rows;
    CAliasMap& map = m_pDoc->GetAliasMap();
    CString key; CAlias* a;
    for (POSITION pos = map.GetStartPosition(); pos; )
        map.GetNextAssoc(pos, key, a), rows.push_back({key, a});

    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        return x.label.CompareNoCase(y.label) < 0;
    });

    long i = 0;
    for (auto& r : rows) {
        m_list->InsertItem(i, cs2wx(r.a->strLabel));
        m_list->SetItem(i, 1, cs2wx(r.a->name));
        m_list->SetItem(i, 2, cs2wx(r.a->strProcedure));
        m_list->SetItem(i, 3, r.a->bEnabled ? "✓" : "");
        m_list->SetItemData(i, (wxUIntPtr)r.a);
        ++i;
    }
}

void CAliasListDlg::OnAdd(wxCommandEvent&)
{
    CAliasEditDlg dlg(this, m_pDoc);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CAliasListDlg::OnEdit(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select an alias first.", "Edit"); return; }
    CAlias* a = (CAlias*)m_list->GetItemData(sel);
    CAliasEditDlg dlg(this, m_pDoc, a);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CAliasListDlg::OnDelete(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select an alias first.", "Delete"); return; }
    CAlias* a = (CAlias*)m_list->GetItemData(sel);
    if (wxMessageBox(wxString::Format("Delete alias '%s'?", cs2wx(a->strLabel)),
                     "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;
    m_pDoc->GetAliasMap().RemoveKey(a->strLabel);
    CAliasArray& arr = m_pDoc->GetAliasArray();
    for (int i = 0; i < arr.GetSize(); ++i)
        if (arr[i] == a) { arr.RemoveAt(i); break; }
    delete a;
    Populate();
}

void CAliasListDlg::OnItemActivated(wxListEvent& evt)
{
    CAlias* a = (CAlias*)evt.GetData();
    CAliasEditDlg dlg(this, m_pDoc, a);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

// ─────────────────────────────────────────────────────────────────────────────
// ── CTimerListDlg ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

wxBEGIN_EVENT_TABLE(CTimerListDlg, wxDialog)
    EVT_BUTTON(ID_TIMER_ADD,          CTimerListDlg::OnAdd)
    EVT_BUTTON(ID_TIMER_EDIT,         CTimerListDlg::OnEdit)
    EVT_BUTTON(ID_TIMER_DEL,          CTimerListDlg::OnDelete)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, CTimerListDlg::OnItemActivated)
wxEND_EVENT_TABLE()

CTimerListDlg::CTimerListDlg(wxWindow* parent, CMUSHclientDoc* pDoc)
    : wxDialog(parent, wxID_ANY, "Timers",
               wxDefaultPosition, wxSize(600, 360),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_pDoc(pDoc)
{
    auto* top = new wxBoxSizer(wxVERTICAL);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->AppendColumn("Label",    wxLIST_FORMAT_LEFT,  150);
    m_list->AppendColumn("Interval", wxLIST_FORMAT_LEFT,  100);
    m_list->AppendColumn("Type",     wxLIST_FORMAT_LEFT,   70);
    m_list->AppendColumn("Script",   wxLIST_FORMAT_LEFT,  140);
    m_list->AppendColumn("On",       wxLIST_FORMAT_CENTER, 40);

    top->Add(m_list, 1, wxEXPAND | wxALL, 8);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->Add(new wxButton(this, ID_TIMER_ADD,  "Add"),    0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_TIMER_EDIT, "Edit"),   0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_TIMER_DEL,  "Delete"), 0, wxRIGHT, 4);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    top->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(top);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&){ Close(); }, wxID_CLOSE);
    Populate();
}

void CTimerListDlg::Populate()
{
    m_list->DeleteAllItems();
    struct Row { CString label; CTimer* t; };
    std::vector<Row> rows;
    CTimerMap& map = m_pDoc->GetTimerMap();
    CString key; CTimer* t;
    for (POSITION pos = map.GetStartPosition(); pos; )
        map.GetNextAssoc(pos, key, t), rows.push_back({key, t});

    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        return x.label.CompareNoCase(y.label) < 0;
    });

    long i = 0;
    for (auto& r : rows) {
        CTimer* t = r.t;
        bool bAt = (t->iType == CTimer::eAtTime);
        wxString interval = wxString::Format("%02d:%02d:%05.2f",
            bAt ? t->iAtHour    : t->iEveryHour,
            bAt ? t->iAtMinute  : t->iEveryMinute,
            bAt ? t->fAtSecond  : t->fEverySecond);
        m_list->InsertItem(i, cs2wx(t->strLabel));
        m_list->SetItem(i, 1, interval);
        m_list->SetItem(i, 2, bAt ? "At" : "Every");
        m_list->SetItem(i, 3, cs2wx(t->strProcedure));
        m_list->SetItem(i, 4, t->bEnabled ? "✓" : "");
        m_list->SetItemData(i, (wxUIntPtr)t);
        ++i;
    }
}

void CTimerListDlg::OnAdd(wxCommandEvent&)
{
    CTimerEditDlg dlg(this, m_pDoc);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CTimerListDlg::OnEdit(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select a timer first.", "Edit"); return; }
    CTimer* t = (CTimer*)m_list->GetItemData(sel);
    CTimerEditDlg dlg(this, m_pDoc, t);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CTimerListDlg::OnDelete(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select a timer first.", "Delete"); return; }
    CTimer* t = (CTimer*)m_list->GetItemData(sel);
    if (wxMessageBox(wxString::Format("Delete timer '%s'?", cs2wx(t->strLabel)),
                     "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;
    m_pDoc->GetTimerMap().RemoveKey(t->strLabel);
    delete t;
    Populate();
}

void CTimerListDlg::OnItemActivated(wxListEvent& evt)
{
    CTimer* t = (CTimer*)evt.GetData();
    CTimerEditDlg dlg(this, m_pDoc, t);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Variable edit dialog ──────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

class CVariableEditDlg : public wxDialog
{
public:
    // Pass nullptr for pVar to create a new variable; non-null to edit.
    CVariableEditDlg(wxWindow* parent, CMUSHclientDoc* pDoc,
                     CVariable* pVar = nullptr)
        : wxDialog(parent, wxID_ANY,
                   pVar ? "Edit Variable" : "Add Variable",
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_pDoc(pDoc), m_pVar(pVar)
    {
        auto* top  = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, wxSize(6, 4));
        grid->AddGrowableCol(1, 1);

        auto addRow = [&](const wxString& lbl, wxWindow* w) {
            grid->Add(new wxStaticText(this, wxID_ANY, lbl),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(w, 1, wxEXPAND);
        };

        m_name  = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(340, -1));
        m_value = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                  wxSize(340, 80), wxTE_MULTILINE);

        addRow("Name:",  m_name);
        addRow("Value:", m_value);

        top->Add(grid, 1, wxEXPAND | wxALL, 8);
        top->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
                 wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        SetSizerAndFit(top);
        SetMinSize(wxSize(380, -1));

        if (m_pVar) {
            m_name ->SetValue(cs2wx(m_pVar->strLabel));
            m_value->SetValue(cs2wx(m_pVar->strContents));
        }

        Bind(wxEVT_BUTTON, &CVariableEditDlg::OnOK, this, wxID_OK);
    }

    wxString m_resultName;

private:
    CMUSHclientDoc* m_pDoc;
    CVariable*      m_pVar;
    wxTextCtrl*     m_name;
    wxTextCtrl*     m_value;

    void OnOK(wxCommandEvent&)
    {
        CString name = wx2cs(m_name->GetValue().Trim());
        if (name.IsEmpty()) {
            wxMessageBox("Name cannot be empty.", "Error", wxICON_ERROR, this);
            return;
        }
        if (CheckObjectName(name) != eOK) {
            wxMessageBox("Name contains invalid characters.", "Error",
                         wxICON_ERROR, this);
            return;
        }

        bool nameChanged = !m_pVar ||
                           (name.CompareNoCase(m_pVar->strLabel) != 0);
        CVariable* existing = nullptr;
        if (nameChanged && m_pDoc->GetVariableMap().Lookup(name, existing)) {
            wxMessageBox("A variable with that name already exists.", "Error",
                         wxICON_ERROR, this);
            return;
        }

        CVariable* v = m_pVar;
        if (v) {
            if (nameChanged)
                m_pDoc->GetVariableMap().RemoveKey(m_pVar->strLabel);
        } else {
            v = new CVariable();
            m_pVar = v;
        }

        v->strLabel    = name;
        v->strContents = wx2cs(m_value->GetValue());

        m_pDoc->GetVariableMap().SetAt(name, v);

        m_resultName = cs2wx(name);
        EndModal(wxID_OK);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ── CVariableListDlg ──────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

wxBEGIN_EVENT_TABLE(CVariableListDlg, wxDialog)
    EVT_BUTTON(ID_VAR_ADD,             CVariableListDlg::OnAdd)
    EVT_BUTTON(ID_VAR_EDIT,            CVariableListDlg::OnEdit)
    EVT_BUTTON(ID_VAR_DEL,             CVariableListDlg::OnDelete)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY,  CVariableListDlg::OnItemActivated)
wxEND_EVENT_TABLE()

CVariableListDlg::CVariableListDlg(wxWindow* parent, CMUSHclientDoc* pDoc)
    : wxDialog(parent, wxID_ANY, "Variables",
               wxDefaultPosition, wxSize(620, 400),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_pDoc(pDoc)
{
    auto* top = new wxBoxSizer(wxVERTICAL);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->AppendColumn("Name",  wxLIST_FORMAT_LEFT, 200);
    m_list->AppendColumn("Value", wxLIST_FORMAT_LEFT, 360);

    top->Add(m_list, 1, wxEXPAND | wxALL, 8);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->Add(new wxButton(this, ID_VAR_ADD,  "Add"),    0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_VAR_EDIT, "Edit"),   0, wxRIGHT, 4);
    btnRow->Add(new wxButton(this, ID_VAR_DEL,  "Delete"), 0, wxRIGHT, 4);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    top->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(top);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&){ Close(); }, wxID_CLOSE);
    Populate();
}

void CVariableListDlg::Populate()
{
    m_list->DeleteAllItems();

    struct Row { CString name; CVariable* v; };
    std::vector<Row> rows;
    CVariableMap& map = m_pDoc->GetVariableMap();
    CString key; CVariable* v;
    for (POSITION pos = map.GetStartPosition(); pos; )
        map.GetNextAssoc(pos, key, v), rows.push_back({key, v});

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.name.CompareNoCase(b.name) < 0;
    });

    long i = 0;
    for (auto& r : rows) {
        m_list->InsertItem(i, cs2wx(r.v->strLabel));
        // Truncate long values in the list display (full value shown in edit dlg).
        wxString val = cs2wx(r.v->strContents);
        if (val.length() > 120) val = val.Left(117) + "...";
        // Replace newlines so multi-line values show on one row.
        val.Replace("\n", "↵");
        val.Replace("\r", "");
        m_list->SetItem(i, 1, val);
        m_list->SetItemData(i, (wxUIntPtr)r.v);
        ++i;
    }
}

void CVariableListDlg::OnAdd(wxCommandEvent&)
{
    CVariableEditDlg dlg(this, m_pDoc);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CVariableListDlg::OnEdit(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select a variable first.", "Edit"); return; }
    CVariable* v = (CVariable*)m_list->GetItemData(sel);
    CVariableEditDlg dlg(this, m_pDoc, v);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

void CVariableListDlg::OnDelete(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) { wxMessageBox("Select a variable first.", "Delete"); return; }
    CVariable* v = (CVariable*)m_list->GetItemData(sel);
    if (wxMessageBox(wxString::Format("Delete variable '%s'?", cs2wx(v->strLabel)),
                     "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;
    m_pDoc->GetVariableMap().RemoveKey(v->strLabel);
    delete v;
    Populate();
}

void CVariableListDlg::OnItemActivated(wxListEvent& evt)
{
    CVariable* v = (CVariable*)evt.GetData();
    CVariableEditDlg dlg(this, m_pDoc, v);
    if (dlg.ShowModal() == wxID_OK) Populate();
}

#endif // !_WIN32
