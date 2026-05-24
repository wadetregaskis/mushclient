#pragma once
// platform/wx_entity_dlgs.h — Trigger / Alias / Timer editor dialogs (wxWidgets)
//
// Three list dialogs, each opening a per-item edit subdialog:
//   CTriggerListDlg   — Edit > Triggers…
//   CAliasListDlg     — Edit > Aliases…
//   CTimerListDlg     — Edit > Timers…
//
// All dialogs take a raw CMUSHclientDoc* and manipulate the maps directly.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>

class CMUSHclientDoc;

// ─────────────────────────────────────────────────────────────────────────────
// CTriggerListDlg
// ─────────────────────────────────────────────────────────────────────────────
class CTriggerListDlg : public wxDialog
{
public:
    CTriggerListDlg(wxWindow* parent, CMUSHclientDoc* pDoc);

private:
    CMUSHclientDoc* m_pDoc;
    wxListCtrl*     m_list;

    void Populate();
    void OnAdd(wxCommandEvent&);
    void OnEdit(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);

    wxDECLARE_EVENT_TABLE();
};

// ─────────────────────────────────────────────────────────────────────────────
// CAliasListDlg
// ─────────────────────────────────────────────────────────────────────────────
class CAliasListDlg : public wxDialog
{
public:
    CAliasListDlg(wxWindow* parent, CMUSHclientDoc* pDoc);

private:
    CMUSHclientDoc* m_pDoc;
    wxListCtrl*     m_list;

    void Populate();
    void OnAdd(wxCommandEvent&);
    void OnEdit(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);

    wxDECLARE_EVENT_TABLE();
};

// ─────────────────────────────────────────────────────────────────────────────
// CTimerListDlg
// ─────────────────────────────────────────────────────────────────────────────
class CTimerListDlg : public wxDialog
{
public:
    CTimerListDlg(wxWindow* parent, CMUSHclientDoc* pDoc);

private:
    CMUSHclientDoc* m_pDoc;
    wxListCtrl*     m_list;

    void Populate();
    void OnAdd(wxCommandEvent&);
    void OnEdit(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);

    wxDECLARE_EVENT_TABLE();
};

#endif // !_WIN32
