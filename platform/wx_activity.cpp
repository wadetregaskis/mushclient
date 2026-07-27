// wx_activity.cpp — Activity window implementation (see wx_activity.h).
//
// Mirrors CActivityView::OnUpdate's data/columns, but renders into a plain wx
// frame (the MFC CListView/MDI doc-view framework is not used on the port).

// wx headers MUST precede the MFC-compat headers: stdafx.h (compat.h) defines
// Windows-style macros (DrawText, GetObject, …) that otherwise clobber wx's own
// headers (wx/graphics.h etc.).  Same ordering as wx_mfc_impl.cpp / wx_world_child.cpp.
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/timer.h>

#include "stdafx.h"                   // compat.h, GetConnectionStatus()
#include "MUSHclient.h"               // CMUSHclientApp + App + doc template
#include "doc.h"                      // CMUSHclientDoc, eConnectConnectedToMud
#include "platform/wx_activity.h"

// Defined in wx_mfc_impl.cpp, where the full CWorldChildFrame type and the correct
// wx/MFC include order live: raise + activate the world's MDI child frame.  (We
// cannot include wx_world_child.h here — it uses MFC types and its wx includes
// would be clobbered by compat.h's macros in this translation unit.)
void wx_activate_world_frame(CMUSHclientDoc* doc);

namespace {

// Column order (a useful subset of CActivityView's columns).
enum { kColWorld, kColNew, kColLines, kColStatus, kColSince, kColDuration };

wxString C(const CString& s) { return wxString::FromUTF8(static_cast<const char*>(s)); }

class CActivityFrame : public wxFrame
{
public:
    static CActivityFrame* s_instance;

    explicit CActivityFrame(wxWindow* parent)
        : wxFrame(parent, wxID_ANY, wxT("Activity"),
                  wxDefaultPosition, wxSize(600, 300))
    {
        s_instance = this;

        // This list is the port's own widget, so it never reaches the canonical
        // RestoreColumnConfiguration that applies LVS_EX_GRIDLINES to the
        // bridged ones -- honour the same global preference here, as
        // ActivityView.cpp does on Windows.
        long listStyle = wxLC_REPORT | wxLC_SINGLE_SEL;
        if (App.m_bShowGridLinesInListViews)
            listStyle |= wxLC_HRULES | wxLC_VRULES;
        m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                listStyle);
        m_list->InsertColumn(kColWorld,    wxT("World"),     wxLIST_FORMAT_LEFT,  150);
        m_list->InsertColumn(kColNew,      wxT("New"),       wxLIST_FORMAT_RIGHT,  60);
        m_list->InsertColumn(kColLines,    wxT("Lines"),     wxLIST_FORMAT_RIGHT,  80);
        m_list->InsertColumn(kColStatus,   wxT("Status"),    wxLIST_FORMAT_LEFT,  110);
        m_list->InsertColumn(kColSince,    wxT("Connected"), wxLIST_FORMAT_LEFT,  150);
        m_list->InsertColumn(kColDuration, wxT("Duration"),  wxLIST_FORMAT_LEFT,  120);

        m_timer.SetOwner(this);
        Bind(wxEVT_TIMER,        &CActivityFrame::OnTimer, this);
        Bind(wxEVT_CLOSE_WINDOW, &CActivityFrame::OnClose, this);
        // Double-click (or Enter) on a row → bring that world's window to front.
        m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &CActivityFrame::OnActivateRow, this);

        Repopulate();
        m_timer.Start(kTickMs);
    }

private:
    // The refresh cadence is a preference, not a constant.  Global Preferences ▸
    // Activity carries "Update activity window: On New Activity / Periodically,
    // every N seconds / Both" (m_nActivityWindowRefreshType) and the interval
    // (m_nActivityWindowRefreshInterval, default 15).  Both are read only by
    // CMainFrame::OnTimer (mainfrm.cpp) and CMUSHclientApp::OnIdle
    // (MUSHclient.cpp) — an MFC timer and an MFC idle pump, neither of which
    // runs on the port — so this window used to refresh once a second no matter
    // what was set, and all three settings were dead.
    //
    // One short tick drives both halves, exactly as Windows uses one timer plus
    // the idle loop:
    //   * periodic  (type != eRefreshOnActivity): repopulate when the interval
    //     has elapsed.  Windows compares m_timeLastActivityUpdate + interval
    //     against now (mainfrm.cpp); this counts the same seconds.
    //   * on activity (type != eRefreshPeriodically): repopulate when the engine
    //     has set App.m_bUpdateActivity, which the shared display code raises on
    //     every new line (doc_shared.cpp "new activity!").
    // The flag is cleared once consumed, so a burst coalesces into one refresh.
    static const int kTickMs = 250;

    void OnTimer(wxTimerEvent&)
    {
        const unsigned int type = App.m_nActivityWindowRefreshType;
        bool bRefresh = false;

        if (type != CMUSHclientApp::eRefreshPeriodically && App.m_bUpdateActivity)
            bRefresh = true;                       // new activity arrived

        if (type != CMUSHclientApp::eRefreshOnActivity) {
            // Interval of 0 would busy-refresh; the dialog's DDV bounds it to
            // 1..300, but the prefs DB is not validated, so guard it here.
            const int secs = (App.m_nActivityWindowRefreshInterval > 0)
                           ? (int) App.m_nActivityWindowRefreshInterval : 15;
            m_msSinceRefresh += kTickMs;
            if (m_msSinceRefresh >= secs * 1000) {
                bRefresh = true;
                m_msSinceRefresh = 0;
            }
        }

        if (!bRefresh)
            return;

        App.m_bUpdateActivity = FALSE;   // consumed
        m_msSinceRefresh = 0;
        Repopulate();
    }

    int m_msSinceRefresh = 0;

    void OnClose(wxCloseEvent&)
    {
        m_timer.Stop();
        s_instance = nullptr;
        Destroy();
    }

    // Double-click / Enter on a row → activate that world's MDI child window.
    void OnActivateRow(wxListEvent& evt)
    {
        CMUSHclientDoc* pDoc = reinterpret_cast<CMUSHclientDoc*>(
            static_cast<wxUIntPtr>(m_list->GetItemData(evt.GetIndex())));
        if (pDoc)
            wx_activate_world_frame(pDoc);   // no-ops if the world has since closed
    }

    void Repopulate()
    {
        if (!m_list || !App.m_pWorldDocTemplate)
            return;

        m_list->Freeze();
        m_list->DeleteAllItems();

        POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition();
        int row = 0;
        while (pos)
        {
            CMUSHclientDoc* pDoc =
                (CMUSHclientDoc*) App.m_pWorldDocTemplate->GetNextDoc(pos);
            if (!pDoc)
                continue;

            CString strStatus = GetConnectionStatus(pDoc->m_iConnectPhase);

            CString strSince;
            if (pDoc->m_iConnectPhase == eConnectConnectedToMud)
                strSince = pDoc->FormatTime(pDoc->m_tConnectTime, "%#I:%M %p, %d %b");

            // Total connected time = prior sessions + the current one (if live).
            CTimeSpan ts = pDoc->m_tsConnectDuration;
            if (pDoc->m_iConnectPhase == eConnectConnectedToMud)
                ts += CTime::GetCurrentTime() - pDoc->m_tConnectTime;

            CString strDuration;
            if (ts.GetDays() > 0)
                strDuration = ts.Format("%Dd %Hh %Mm %Ss");
            else if (ts.GetHours() > 0)
                strDuration = ts.Format("%Hh %Mm %Ss");
            else if (ts.GetMinutes() > 0)
                strDuration = ts.Format("%Mm %Ss");
            else
                strDuration = ts.Format("%Ss");

            long idx = m_list->InsertItem(row, C(pDoc->m_mush_name));
            m_list->SetItem(idx, kColNew,      wxString::Format(wxT("%ld"), pDoc->m_new_lines));
            m_list->SetItem(idx, kColLines,    wxString::Format(wxT("%ld"), pDoc->m_total_lines));
            m_list->SetItem(idx, kColStatus,   C(strStatus));
            m_list->SetItem(idx, kColSince,    C(strSince));
            m_list->SetItem(idx, kColDuration, C(strDuration));
            // Remember which world this row is, so double-click can switch to it.
            m_list->SetItemPtrData(idx, reinterpret_cast<wxUIntPtr>(pDoc));
            ++row;
        }

        m_list->Thaw();
    }

    wxListCtrl* m_list = nullptr;
    wxTimer     m_timer;
};

CActivityFrame* CActivityFrame::s_instance = nullptr;

} // namespace

void ShowActivityWindow()
{
    if (CActivityFrame::s_instance)
    {
        CActivityFrame::s_instance->Raise();
        return;
    }

    wxWindow* parent = (App.m_pMainWnd && App.m_pMainWnd->m_hWnd)
                     ? static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)
                     : nullptr;
    (new CActivityFrame(parent))->Show();
}
