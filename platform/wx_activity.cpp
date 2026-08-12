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
#include <wx/stopwatch.h>
#include <algorithm>
#include <ctime>
#include <vector>

#include "stdafx.h"                   // compat.h, GetConnectionStatus()
#include "MUSHclient.h"               // CMUSHclientApp + App + doc template
#include "doc.h"                      // CMUSHclientDoc, eConnectConnectedToMud
#include "platform/wx_activity.h"
#include "platform/wx_outputpane.h"      // DecodeTextLenient — legacy-charset names
#include "platform/activity_refresh.h"   // the refresh policy, lifted out to be testable

// activity_refresh.h restates the refresh-type enum so it can stay free of MFC;
// this is the only place both spellings are visible, so check them here.
static_assert ((int) CMUSHclientApp::eRefreshOnActivity   == kActivityRefreshOnActivity &&
               (int) CMUSHclientApp::eRefreshPeriodically == kActivityRefreshPeriodically &&
               (int) CMUSHclientApp::eRefreshBoth         == kActivityRefreshBoth,
               "activity_refresh.h's refresh types have drifted from CMUSHclientApp's");

// Defined in wx_mfc_impl.cpp, where the full CWorldChildFrame type and the correct
// wx/MFC include order live: raise + activate the world's MDI child frame.  (We
// cannot include wx_world_child.h here — it uses MFC types and its wx includes
// would be clobbered by compat.h's macros in this translation unit.)
void wx_activate_world_frame(CMUSHclientDoc* doc);

namespace {

// Column order (a useful subset of CActivityView's columns).
enum { kColWorld, kColNew, kColLines, kColStatus, kColSince, kColDuration };

// Lenient: a legacy-charset world name must show in its row, not blank it
// (bare FromUTF8 is all-or-nothing).
wxString C(const CString& s) { return DecodeTextLenient(static_cast<const char*>(s)); }

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
        // Column click → sort by that column, toggling the direction on a
        // repeat click — CActivityView::OnColumnclick behaviour (2026-08-12;
        // the port bound no handler, so headers were dead).
        m_list->Bind(wxEVT_LIST_COL_CLICK, &CActivityFrame::OnColClick, this);

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
    //
    // The decision itself is ActivityRefreshForTick (platform/activity_refresh.h):
    // nothing can reach a wxEVT_TIMER handler under test, so it lives where it
    // can be called directly.
    static const int kTickMs = 250;

    void OnTimer(wxTimerEvent&)
    {
        // Charge the policy the time that really passed, not the 250 ms we
        // asked for.  macOS coalesces timers (and the machine may have slept),
        // so counting nominal ticks would quietly stretch the user's "every N
        // seconds" into something longer — the wall clock is what Windows
        // compares against too (mainfrm.cpp:774).
        const long long elapsedMs = m_sinceTick.Time();
        m_sinceTick.Start();

        const ActivityRefreshDecision decision = ActivityRefreshForTick (
            App.m_nActivityWindowRefreshType,
            (int) App.m_nActivityWindowRefreshInterval,   // signed on purpose — see the header
            App.m_bUpdateActivity != FALSE,
            elapsedMs,
            m_msSinceRefresh);

        m_msSinceRefresh = decision.accumulatorMs;

        if (!decision.refresh)
            return;

        App.m_bUpdateActivity = FALSE;   // consumed
        Repopulate();
    }

    long long   m_msSinceRefresh = 0;
    wxStopWatch m_sinceTick;             // runs from construction

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

    void OnColClick(wxListEvent& evt)
    {
        const int col = evt.GetColumn();
        if (col < 0) return;
        if (col == m_lastSortCol)
            m_sortReverse = !m_sortReverse;
        else
            m_sortReverse = false;
        m_lastSortCol = col;
        Repopulate();
    }

    void Repopulate()
    {
        if (!m_list || !App.m_pWorldDocTemplate)
            return;

        m_list->Freeze();
        m_list->DeleteAllItems();

        // Snapshot the docs, sorted by the clicked column (insertion order
        // until a header is clicked).  Sorting the snapshot rather than the
        // control matches the rebuild-per-refresh model, and the comparator
        // is the hoisted, headless-tested ActivityCompareKeys.
        std::vector<CMUSHclientDoc*> docs;
        for (POSITION p = App.m_pWorldDocTemplate->GetFirstDocPosition(); p; ) {
            CMUSHclientDoc* d =
                (CMUSHclientDoc*) App.m_pWorldDocTemplate->GetNextDoc(p);
            if (d) docs.push_back(d);
        }
        if (m_lastSortCol >= 0) {
            const time_t now = time(nullptr);
            auto key = [now](CMUSHclientDoc* d) {
                ActivitySortKey k;
                k.name        = (const char*) d->m_mush_name;
                k.newLines    = d->m_new_lines;
                k.totalLines  = d->m_total_lines;
                k.status      = (const char*)
                                GetConnectionStatus(d->m_iConnectPhase);
                k.connectTime = (long long) d->m_tConnectTime.GetTime();
                k.durationSecs= (long long) now - k.connectTime;
                return k;
            };
            const int col = m_lastSortCol;
            const bool rev = m_sortReverse;
            std::stable_sort(docs.begin(), docs.end(),
                [&key, col, rev](CMUSHclientDoc* a, CMUSHclientDoc* b) {
                    return ActivityCompareKeys(key(a), key(b), col, rev) < 0;
                });
        }

        int row = 0;
        for (CMUSHclientDoc* pDoc : docs)
        {

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
    int  m_lastSortCol = -1;      // -1 = insertion order (no click yet)
    bool m_sortReverse = false;
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
