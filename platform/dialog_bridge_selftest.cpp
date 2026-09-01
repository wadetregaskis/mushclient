// platform/dialog_bridge_selftest.cpp — the MFC→wx dialog bridge under test
// (--extended-selftest=dialog-bridge).
//
// WHY THIS NEEDS THE LOOP-RUNNING MODE
//
// Everything here lives behind a dlg.ShowModal(), and until the extended mode
// existed no in-app selftest could get past one: they all ran inside
// wxApp::OnInit and std::exit()ed before OnRun, so there was no event loop and
// a modal could never complete.  That is the whole reason
// docs/UNREACHABLE-PATHS-2026-08.md findings 16, 17 and 19 -- the property-sheet
// page lifecycle, the bridged-dialog command routing, and pump_dialog_update_ui
// -- had no coverage at all, despite the comments in wx_mfc_impl.cpp recording
// four separately shipped bugs from getting the sequence wrong.
//
// HOW A MODAL IS DRIVEN
//
// The dialog has to be dismissed from OUTSIDE itself or the harness hangs and
// the watchdog kills it, so every scenario schedules its driver from a
// CallAfter posted BEFORE the ShowModal call -- either from the probe's own
// OnInitDialog (hook_run_dialog runs it at wx_mfc_impl.cpp:4110, four lines
// before ShowModal) or from the harness immediately before sheet.DoModal().
// The driver then synthesises the events the user would have generated and
// finishes by ending the modal loop.
//
// WHAT THE ASSERTIONS ARE SHAPED LIKE
//
// The contract is an ORDER, not a set: which of {UpdateData, OnKillActive,
// OnSetActive, OnOK, OnDestroy} runs on WHICH page and WHEN.  So the probes
// append to one shared transcript and the tests compare the whole string.  A
// per-callback "was it called?" assertion would survive most of the mutations
// that matter (moving OnKillActive after OnOK, validating every page instead of
// the active one, running OnDestroy after the window is gone).

#ifndef _WIN32

// ── wxWidgets (first — same include-order rule as wx_mfc_impl.cpp) ─────────
#include <wx/wx.h>
#include <wx/treebook.h>   // the tree sheet's book control (wxBookCtrlBase)
#include <wx/notebook.h>   // the flat sheet's
#include <wx/listctrl.h>   // wxListCtrl / wxListEvent (NM_RDBLCLK synthesis)

// ── MUSHclient headers ────────────────────────────────────────────────────
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "resource.h"
#include "dialogs/world_prefs/genpropertypage.h"
#include "dialogs/world_prefs/TreePropertySheet.h"
#include "platform/wx_dialog_bridge.h"   // MfcDialogResources (the templates the pump walks)
#include "platform/extended_selftest.h"  // ExtendedSelfTestCheck

#include <set>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
bool chk(bool ok, const char* what)
{
    if (!ExtendedSelfTestCheck(ok, what)) { ++g_fail; return false; }
    return true;
}

// ── the shared transcript ─────────────────────────────────────────────────
// Every probe callback appends one token.  Tests compare the joined string, so
// a reordering fails as loudly as an omission.
std::vector<std::string> g_seq;
size_t g_mark = 0;
void rec(const std::string& s) { g_seq.push_back(s); }
std::string Join(size_t from)
{
    std::string out;
    for (size_t i = from; i < g_seq.size(); ++i) {
        if (!out.empty()) out += " ";
        out += g_seq[i];
    }
    return out;
}
std::string Transcript() { return Join(0); }
void ResetTranscript() { g_seq.clear(); g_mark = 0; }

// Compare and, on failure, print both sides — a one-token difference in a
// twelve-token sequence is unreadable from a bare [FAIL] line.
bool chkSeq(const char* expected, const char* what)
{
    const std::string got = Transcript();
    const bool ok = (got == expected);
    chk(ok, what);
    if (!ok) {
        printf("      expected: %s\n", expected);
        printf("      actual  : %s\n", got.c_str());
    }
    return ok;
}

// The same, for one STEP of a multi-step scenario: what was recorded since the
// last call.  A sheet's transcript is thirty-odd tokens long, and a whole-string
// comparison would name the scenario rather than the step that broke.
bool chkDelta(const char* expected, const char* what)
{
    const std::string got = Join(g_mark);
    g_mark = g_seq.size();
    const bool ok = (got == expected);
    chk(ok, what);
    if (!ok) {
        printf("      expected: [%s]\n", expected);
        printf("      actual  : [%s]\n", got.c_str());
    }
    return ok;
}

// ── event synthesis ───────────────────────────────────────────────────────
// A real click on a bridged control arrives as a wxEVT_BUTTON on the dialog
// (the bridge Binds there, wx_mfc_impl.cpp:4017); a WM_COMMAND the dialog posts
// itself arrives as a wxEVT_MENU (:4060).  ProcessEvent, not QueueEvent, so the
// driver can assert on the consequence on the next line.
void SendCommand(wxWindow* w, wxEventType type, int id)
{
    wxCommandEvent e(type, id);
    e.SetEventObject(w);
    w->GetEventHandler()->ProcessEvent(e);
}

// Let the running loop reach idle.  wxYield on its own does not reliably
// deliver wxEVT_IDLE here, so give it real elapsed time to settle into.
void SpinIdle(int ms)
{
    const wxLongLong until = wxGetLocalTimeMillis() + ms;
    while (wxGetLocalTimeMillis() < until) { wxYield(); wxMilliSleep(5); }
}

// Depth-first search for the first descendant of `root` of wx class `cls`.
// Used to find the sheet bridges' wxTreebook / wxNotebook, which are locals
// inside the static hook bodies and so cannot be reached any other way.
wxWindow* FindDescendantOfClass(wxWindow* root, const wxChar* cls)
{
    if (!root) return nullptr;
    for (wxWindow* child : root->GetChildren()) {
        if (child->IsKindOf(wxClassInfo::FindClass(cls))) return child;
        if (wxWindow* deep = FindDescendantOfClass(child, cls)) return deep;
    }
    return nullptr;
}

// ══════════════════════════════════════════════════════════════════════════
// Finding 17 — bridged-dialog command routing, and finding 19 — the update-UI
// pump.  One probe covers both because both live in hook_run_dialog and the
// pump's only trigger points are that hook's initial call and idle bind.
// ══════════════════════════════════════════════════════════════════════════

class CCmdRoutingProbe : public CDialog
{
public:
    CCmdRoutingProbe() : CDialog(IDD_SEND_TO_ALL) {}

    // ── what the update-UI pump did ──
    std::set<UINT> m_pumped;        // every id it built a CCmdUI for
    std::set<UINT> m_expected;      // every id it SHOULD have, computed from the template
    int  m_staticsInTemplate = 0;   // IDC_STATIC controls — see the comment at the assertion
    bool m_sawPump = false;
    bool m_firstPumpBeforeModal = false;

    // ── what OnDestroy saw ──
    bool m_destroyHadWindow   = false;
    bool m_destroyHadControls = false;
    bool m_destroyAfterModal  = false;

    BOOL OnInitDialog() override;
    void OnOK()     override { rec("OnOK");     CDialog::OnOK(); }
    void OnCancel() override { rec("OnCancel"); CDialog::OnCancel(); }
    void OnDestroy() override;

    afx_msg void OnMappedOk()       { rec("mapped:IDOK"); }
    afx_msg void OnMappedSelectAll(){ rec("mapped:SELECT_ALL"); }
    afx_msg void OnUpdateAny(CCmdUI* pCmdUI);

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CCmdRoutingProbe, CDialog)
    // The precedence under test: a mapped ON_BN_CLICKED(IDOK) must beat the
    // CDialog::OnOK virtual, as it does for CImmediateDlg and CImportXMLdlg.
    ON_BN_CLICKED(IDOK, OnMappedOk)
    ON_BN_CLICKED(IDC_SELECT_ALL, OnMappedSelectAll)
    // A catch-all so the test can assert WHICH ids the pump walked, not merely
    // that some handler ran.  0xFFFFFFFF is (UINT) IDC_STATIC, so an id the
    // pump should have skipped would show up here.
    ON_UPDATE_COMMAND_UI_RANGE(0, 0xFFFFFFFFu, OnUpdateAny)
END_MESSAGE_MAP()

void CCmdRoutingProbe::OnUpdateAny(CCmdUI* pCmdUI)
{
    m_pumped.insert(pCmdUI->m_nID);
    if (!m_sawPump) {
        m_sawPump = true;
        // hook_run_dialog pumps once at :4111, BEFORE ShowModal — that initial
        // call is what sets a dialog's opening control state, and it is a
        // separate call site from the idle bind at :4105.  IsModal() is the
        // only thing that tells them apart from in here.
        wxDialog* d = static_cast<wxDialog*>(m_hWnd);
        m_firstPumpBeforeModal = d && !d->IsModal();
    }
    // Drive one real control from the handler, so the CCmdUI → wx wiring is
    // covered and not just the dispatch.
    if (pCmdUI->m_nID == (UINT) IDC_ECHO)
        pCmdUI->Enable(FALSE);
}

void CCmdRoutingProbe::OnDestroy()
{
    rec("OnDestroy");
    wxDialog* d = static_cast<wxDialog*>(m_hWnd);
    m_destroyHadWindow   = (d != nullptr);
    // The canonical OnDestroy handlers read the live controls (column widths,
    // window placement), so the window must still be built at this point.
    m_destroyHadControls = d && d->FindWindow(IDC_ECHO) != nullptr;
    m_destroyAfterModal  = d && !d->IsModal();
}

// The driver: everything the "user" does, run from inside the modal loop.
void DriveCmdRouting(CCmdRoutingProbe* probe, wxDialog* dlg)
{
    chk(dlg->IsModal(), "the driver runs inside the dialog's modal loop");

    // 1. Mapped handler beats the OnOK virtual, and does NOT close the dialog.
    SendCommand(dlg, wxEVT_BUTTON, IDOK);
    chk(dlg->IsModal(), "a mapped ON_BN_CLICKED(IDOK) leaves the dialog OPEN "
                        "(CImmediateDlg's Run button)");

    // 2. Mapped handler for an ordinary control id.
    SendCommand(dlg, wxEVT_BUTTON, IDC_SELECT_ALL);

    // 3. An id with no handler that is neither IDOK nor IDCANCEL is Skip()ed:
    //    nothing recorded, nothing closed.
    SendCommand(dlg, wxEVT_BUTTON, IDC_SELECT_NONE);
    chk(dlg->IsModal(), "an unmapped, non-OK/Cancel id neither runs a handler "
                        "nor closes the dialog");

    // 4. A WM_COMMAND the dialog posts itself arrives as wxEVT_MENU and takes
    //    the same route (CTimerDlg/CTriggerDlg/CAliasDlg post IDC_EDIT_SEND
    //    that way from Alt+Return).
    SendCommand(dlg, wxEVT_MENU, IDC_SELECT_ALL);

    // 5. The pump's effect on a live control, and its non-effect on one the
    //    handler left alone.
    wxWindow* echo   = dlg->FindWindow(IDC_ECHO);
    wxWindow* select = dlg->FindWindow(IDC_SELECT_ALL);
    chk(echo && !echo->IsEnabled(),
        "CCmdUI::Enable(FALSE) from an ON_UPDATE_COMMAND_UI handler really "
        "disables the wx control");
    chk(select && select->IsEnabled(),
        "...and a control the handler does not touch stays enabled");

    // 6. The pump's OTHER call site.  Everything above is satisfied by the one
    //    pump before ShowModal (:4111); this separates the idle bind (:4105),
    //    which is what re-derives control state after an interaction -- the
    //    entity pages' Edit/Copy/Delete following the list selection.  Put the
    //    control back by hand and let the loop idle: only the idle pump can
    //    correct it, since nothing else is dispatched in between.
    if (echo) echo->Enable(true);
    SpinIdle(300);
    chk(echo && !echo->IsEnabled(),
        "the idle-bound pump runs too, not just the one before ShowModal");

    // 7. No mapped handler for IDCANCEL, so it falls back to the CDialog
    //    virtual, which EndDialog()s and ends the loop.
    SendCommand(dlg, wxEVT_BUTTON, IDCANCEL);
}

BOOL CCmdRoutingProbe::OnInitDialog()
{
    rec("OnInitDialog");
    CDialog::OnInitDialog();

    wxDialog* dlg = static_cast<wxDialog*>(m_hWnd);

    // What the pump SHOULD walk: the template's controls with a positive id
    // that resolve to a live control (pump_dialog_update_ui skips id <= 0 and
    // any id with no control, wx_mfc_impl.cpp:3949-3956).
    if (const RcDialog* t = MfcDialogResources().FindDialog(IDD_SEND_TO_ALL))
        for (const RcControl& c : t->controls) {
            if (c.id <= 0) { ++m_staticsInTemplate; continue; }
            if (dlg && dlg->FindWindow(c.id)) m_expected.insert((UINT) c.id);
        }

    // Scheduled HERE, i.e. before hook_run_dialog's ShowModal at :4114.
    wxTheApp->CallAfter([this, dlg]() { DriveCmdRouting(this, dlg); });
    return TRUE;
}

void TestCommandRouting()
{
    printf("Bridged-dialog command routing + the update-UI pump "
           "(findings 17, 19):\n");
    ResetTranscript();

    CCmdRoutingProbe probe;
    const INT_PTR rc = probe.DoModal();

    chkSeq("OnInitDialog mapped:IDOK mapped:SELECT_ALL mapped:SELECT_ALL "
           "OnCancel OnDestroy",
           "the exact dispatch sequence: mapped handlers ran, OnOK never did, "
           "IDCANCEL fell back to the virtual");
    chk(rc == IDCANCEL, "DoModal returns what EndDialog was given");

    chk(probe.m_sawPump, "the update-UI pump ran at all");
    chk(probe.m_firstPumpBeforeModal,
        "the FIRST pump is the one before ShowModal, not an idle one");
    chk(!probe.m_expected.empty() && probe.m_pumped == probe.m_expected,
        "the pump built a CCmdUI for exactly the template's resolvable "
        "positive-id controls");
    // Honest scope note: deleting `if (c.id <= 0) continue;` from the pump was
    // mutation-tested and SURVIVED, and it always will.  The bridge builds
    // IDC_STATIC controls with wxID_ANY (wx_dialog_bridge.cpp:163), so wx gives
    // them a generated id in the auto range and FindWindow(-1) never matches;
    // the `if (cmdui.m_hWndCtrl)` test then skips them anyway, and no shipped
    // template has an id of 0 either (the resource loader reports 0 unresolved
    // control ids).  The guard is defence in depth, not live behaviour.  The
    // pair below therefore pins the WALK -- that the pump visits exactly the
    // resolvable controls -- and records that statics were present while it did.
    chk(probe.m_staticsInTemplate > 0,
        "the fixture template really does contain IDC_STATIC controls");
    chk(probe.m_pumped.count(0xFFFFFFFFu) == 0,
        "...and no CCmdUI was built for (UINT) IDC_STATIC");

    chk(probe.m_destroyHadWindow && probe.m_destroyHadControls,
        "OnDestroy runs while the wx window and its controls are still alive");
    chk(probe.m_destroyAfterModal,
        "...but after the modal loop has ended");
    chk(probe.m_hWnd == nullptr,
        "m_hWnd is cleared only after OnDestroy has run");
}

// ══════════════════════════════════════════════════════════════════════════
// The caption close button must route through the canonical OnCancel virtual.
//
// On Windows the red X is WM_SYSCOMMAND/SC_CLOSE, which the dialog manager
// turns into an IDCANCEL command, so a dialog's OnCancel override ALWAYS
// runs.  The bridge's wxEVT_CLOSE_WINDOW bind used to call EndModal(IDCANCEL)
// directly, skipping the virtual — and CImmediateDlg / CDebugLuaDlg both use
// OnCancel to copy the live edit text into the member their caller persists
// ("save the expression text for next time"), so closing either with the red
// button silently lost what was typed.  The probe mirrors that shape exactly:
// a CEdit bound by DDX_Control, read back inside OnCancel.
// ══════════════════════════════════════════════════════════════════════════

class CCloseBoxProbe : public CDialog
{
public:
    CCloseBoxProbe() : CDialog(IDD_IMMEDIATE) {}

    CEdit   m_ctlExpression;             // the CImmediateDlg shape, exactly
    CString m_strSaved = "stale";        // pre-poisoned: "did nothing" must FAIL

    void DoDataExchange(CDataExchange* pDX) override
    {
        CDialog::DoDataExchange(pDX);
        DDX_Control(pDX, IDC_EXPRESSION, m_ctlExpression);
    }

    BOOL OnInitDialog() override
    {
        rec("OnInitDialog");
        CDialog::OnInitDialog();
        wxDialog* dlg = static_cast<wxDialog*>(m_hWnd);
        // The "user": type into the edit, then click the caption close button.
        wxTheApp->CallAfter([dlg]() {
            chk(dlg->IsModal(), "the close-box driver runs inside the modal loop");
            wxTextCtrl* ed = wxDynamicCast(dlg->FindWindow(IDC_EXPRESSION), wxTextCtrl);
            if (chk(ed != nullptr, "IDD_IMMEDIATE's expression edit was built"))
                ed->ChangeValue(wxT("say hello"));
            // wxWindow::Close() generates the same wxEVT_CLOSE_WINDOW the
            // titlebar close button does, and processes it synchronously.
            dlg->Close();
        });
        return TRUE;
    }

    void OnCancel() override
    {
        rec("OnCancel");
        // The canonical CImmediateDlg::OnCancel body: save the expression
        // text for next time, from the STILL-LIVE control.
        m_ctlExpression.GetWindowText(m_strSaved);
        CDialog::OnCancel();
    }
    void OnDestroy() override { rec("OnDestroy"); }
};

void TestCloseBoxRunsOnCancel()
{
    printf("Caption close routes through the canonical OnCancel:\n");
    ResetTranscript();

    CCloseBoxProbe probe;
    const INT_PTR rc = probe.DoModal();

    chkSeq("OnInitDialog OnCancel OnDestroy",
           "the close box ran the OnCancel virtual (not a bare EndModal)");
    chk(rc == IDCANCEL, "and DoModal still returns IDCANCEL");
    chk(probe.m_strSaved == "say hello",
        "OnCancel read the live edit text (CImmediateDlg's save-for-next-time)");
}

// ══════════════════════════════════════════════════════════════════════════
// Esc in a bridged dialog runs the canonical Cancel arm.
//
// wx's own Esc→Cancel looks the Cancel button up by wxID_CANCEL (5101), and
// template controls keep their Win32 ids (IDCANCEL == 2), so no bridged
// dialog has one -- Esc had NO route to Cancel at all.  The bridge's
// CHAR_HOOK now translates it, offers it to PreTranslateMessage first (MFC's
// order -- CDebugLuaDlg maps Esc to "continue execution"), and dispatches the
// IDCANCEL command when the override declines.
// ══════════════════════════════════════════════════════════════════════════

class CEscProbe : public CDialog
{
public:
    CEscProbe() : CDialog(IDD_SEND_TO_ALL) {}

    bool m_eatEscape    = false;   // the CDebugLuaDlg shape when true
    int  m_preSawEscape = 0;

    BOOL OnInitDialog() override
    {
        rec("OnInitDialog");
        CDialog::OnInitDialog();
        wxDialog* dlg = static_cast<wxDialog*>(m_hWnd);
        wxTheApp->CallAfter([this, dlg]() {
            // The event AppKit hands wx for a real Escape key-down.
            wxKeyEvent esc(wxEVT_CHAR_HOOK);
            esc.m_keyCode = WXK_ESCAPE;
            esc.SetEventObject(dlg);
            dlg->GetEventHandler()->ProcessEvent(esc);
            if (m_eatEscape)
            {
                chk(dlg->IsModal(),
                    "an Esc consumed by PreTranslateMessage leaves the dialog open");
                SendCommand(dlg, wxEVT_BUTTON, IDCANCEL);   // close for real
            }
        });
        return TRUE;
    }

    BOOL PreTranslateMessage(MSG* pMsg) override
    {
        if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_ESCAPE)
        {
            ++m_preSawEscape;
            if (m_eatEscape) return TRUE;
        }
        return CDialog::PreTranslateMessage(pMsg);
    }

    void OnCancel()  override { rec("OnCancel"); CDialog::OnCancel(); }
    void OnDestroy() override { rec("OnDestroy"); }
};

void TestEscapeRunsCancel()
{
    printf("Esc in a bridged dialog runs the canonical Cancel arm:\n");

    {
        ResetTranscript();
        CEscProbe probe;
        const INT_PTR rc = probe.DoModal();
        chkSeq("OnInitDialog OnCancel OnDestroy",
               "Esc dispatched the IDCANCEL command (no wxID_CANCEL button "
               "exists for wx's own machinery to find)");
        chk(rc == IDCANCEL, "and DoModal returns IDCANCEL");
        chk(probe.m_preSawEscape == 1,
            "PreTranslateMessage saw the key FIRST (MFC's order)");
    }

    // The CDebugLuaDlg shape: an override that consumes Esc must stop the
    // Cancel arm -- exactly one OnCancel below, and it is the driver's own
    // close click, not the swallowed Esc.
    {
        ResetTranscript();
        CEscProbe probe;
        probe.m_eatEscape = true;
        probe.DoModal();
        chk(probe.m_preSawEscape == 1, "the swallowing override saw the key");
        chkSeq("OnInitDialog OnCancel OnDestroy",
               "a PreTranslateMessage that consumes Esc stops the Cancel arm");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// A right-click on a bridged list dispatches NM_RDBLCLK.
//
// bind_list_notifications synthesised LVN_COLUMNCLICK, NM_DBLCLK and
// LVN_ITEMCHANGED but never NM_RDBLCLK, so CPluginsDlg's right-double-click
// state-file editor (its ON_NOTIFY(NM_RDBLCLK, IDC_PLUGINS_LIST, ...) is the
// only consumer in the app) was dead code on the port -- and there is no
// other UI path to a plugin's state file from the Plugins dialog.  wx has no
// right-DOUBLE-click list event, so a single right-click is the stand-in.
// ══════════════════════════════════════════════════════════════════════════

class CRClickProbe : public CDialog
{
public:
    CRClickProbe() : CDialog(IDD_PLUGINS) {}

    int  m_rclickItem = -2;   // pre-poisoned: "nothing dispatched" must FAIL
    UINT m_rclickCode = 0;

    BOOL OnInitDialog() override
    {
        CDialog::OnInitDialog();
        wxDialog* dlg = static_cast<wxDialog*>(m_hWnd);
        wxTheApp->CallAfter([dlg]() {
            wxListCtrl* lv = wxDynamicCast(dlg->FindWindow(IDC_PLUGINS_LIST),
                                           wxListCtrl);
            chk(lv != nullptr, "IDD_PLUGINS' list was built as a wxListCtrl");
            if (lv)
            {
                lv->InsertColumn(0, wxT("Name"));
                lv->InsertItem(0, wxT("row0"));
                lv->InsertItem(1, wxT("row1"));
                // The same synthesis shape the existing selftests use for list
                // events: the event a real right-click on row 1 produces.
                wxListEvent e(wxEVT_LIST_ITEM_RIGHT_CLICK, IDC_PLUGINS_LIST);
                e.SetEventObject(lv);
                e.m_itemIndex = 1;
                lv->GetEventHandler()->ProcessEvent(e);
            }
            dlg->EndModal(IDCANCEL);
        });
        return TRUE;
    }

    afx_msg void OnRClick(NMHDR* pNMHDR, LRESULT* pResult)
    {
        m_rclickCode = pNMHDR->code;
        m_rclickItem = (int) ((NMITEMACTIVATE*) pNMHDR)->iItem;
        *pResult = 0;
    }
    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CRClickProbe, CDialog)
    // The exact map entry CPluginsDlg::OnRdblclkPluginsList uses.
    ON_NOTIFY(NM_RDBLCLK, IDC_PLUGINS_LIST, OnRClick)
END_MESSAGE_MAP()

void TestListRightClickDispatchesRdblclk()
{
    printf("Right-click on a bridged list dispatches NM_RDBLCLK:\n");

    CRClickProbe probe;
    probe.DoModal();

    chk(probe.m_rclickCode == (UINT) NM_RDBLCLK,
        "the ON_NOTIFY(NM_RDBLCLK) handler ran (CPluginsDlg's state-file editor)");
    chk(probe.m_rclickItem == 1,
        "...and was handed the clicked row's index");
}

// ══════════════════════════════════════════════════════════════════════════
// Finding 19 — the deferred list-reload transition.
//
// pump_dialog_update_ui is the port's ONLY consumer of m_bReloadList: there is
// no MFC idle loop here, so canonical CGenPropertyPage::OnKickIdle never runs.
// Skip the branch and the entity managers (triggers / aliases / timers /
// variables) never refresh after Add, Change, Delete or Paste.
//
// The probe is a CGenPropertyPage over the real Aliases template, driven as a
// plain bridged dialog (CPropertyPage IS a CDialog, so DoModal reaches
// hook_run_dialog).  It supplies its own empty object map rather than a world
// document, so the canonical LoadList runs for real against live controls.
// ══════════════════════════════════════════════════════════════════════════

int ProbeCompareObjects(const int, const CObject*, const CObject*) { return 0; }

class CReloadProbe : public CGenPropertyPage
{
public:
    CReloadProbe() : CGenPropertyPage(IDD_PREFS_P7) {}

    CListCtrl  m_probeList;
    CStatic    m_probeSummary;
    CObjectMap m_probeMap;
    CFindInfo  m_probeFind;

    bool    m_sawPump = false;
    bool    m_firstPumpSawReloadPending = false;
    CString m_summaryBefore, m_summaryAfter;

    BOOL OnInitDialog() override;
    // NOT the canonical CGenPropertyPage::OnDestroy: that one persists the
    // list's column configuration, which needs the column arrays a real page
    // sets up.  This probe exists for the reload branch; finding 17's probe
    // above covers the OnDestroy call site itself.
    void OnDestroy() override { rec("reload:destroy"); }

    afx_msg void OnUpdateAny(CCmdUI*);
    DECLARE_MESSAGE_MAP()

    // The abstract half of CGenPropertyPage.  None of it is reached with an
    // empty object map — LoadList's per-item loop never runs — so these exist
    // only to make the class concrete.
    void InitDialog(CDialog*) override {}
    void LoadDialog(CDialog*, CObject*) override {}
    void UnloadDialog(CDialog*, CObject*) override {}
    bool CheckIfChanged(CDialog*, CObject*) const override { return false; }
    CString GetObjectName(CDialog*) const override { return ""; }
    CObject* MakeNewObject() override { return nullptr; }
    void SetModificationNumber(CObject*, __int64) override {}
    void SetDispatchID(CObject*, const DISPID) override {}
    void SetInternalName(CObject*, const CString) override {}
    void SetDefaultSequence() override {}
    __int64 GetModificationNumber(CObject*) const override { return 0; }
    CString GetScriptName(CObject*) const override { return ""; }
    CString GetLabel(CObject*) const override { return ""; }
    CString GetGroup(CObject*) const override { return ""; }
    CString GetDescription(CObject*) const override { return ""; }
    int GetSequence(CObject*) const override { return 0; }
    CString GetFindText(CObject*) const override { return ""; }
    int AddItem(CObject*, const int, const BOOL) override { return 0; }
    bool CheckIfIncluded(CObject*) override { return true; }
    bool CheckIfExecuting(CObject*) override { return false; }
    bool GetFilterFlag() override { return false; }
    CString GetFilterScript() override { return ""; }
    void GetFilterInfo(CObject*, lua_State*) override {}
    bool CheckIfTemporary(CObject*) override { return false; }
};

BEGIN_MESSAGE_MAP(CReloadProbe, CGenPropertyPage)
    ON_UPDATE_COMMAND_UI_RANGE(0, 0xFFFFFFFFu, OnUpdateAny)
END_MESSAGE_MAP()

void CReloadProbe::OnUpdateAny(CCmdUI*)
{
    if (m_sawPump) return;
    m_sawPump = true;
    // The port walks the controls FIRST and reloads afterwards; canonical
    // CGenPropertyPage::OnKickIdle (genpropertypage.cpp:1210-1229) reloads
    // first and then calls UpdateDialogControls.  Reading the flag from inside
    // a control handler is the direct way to say which order this is.
    m_firstPumpSawReloadPending = m_bReloadList;
}

BOOL CReloadProbe::OnInitDialog()
{
    rec("reload:init");
    // The canonical CGenPropertyPage::OnInitDialog builds the alternative tree
    // control, which is Windows-only (and needs a real page's column set), so
    // go straight to the CPropertyPage base and do this probe's own setup.
    CPropertyPage::OnInitDialog();

    wxDialog* dlg = static_cast<wxDialog*>(m_hWnd);
    m_probeList.m_hWnd    = (HWND) (dlg ? dlg->FindWindow(IDC_ALIASES_LIST) : nullptr);
    m_probeSummary.m_hWnd = (HWND) (dlg ? dlg->FindWindow(IDC_COUNT) : nullptr);
    SetUpPage("widget", &m_probeMap, &m_probeList, &m_probeSummary,
              ProbeCompareObjects, &m_probeFind, 0, false /*no tree control*/);

    // The template's design-time caption for the summary static is
    // "0 aliases."; LoadList overwrites it with "0 items.".  That difference is
    // how the test tells "LoadList ran" from "the page was merely built".
    m_probeSummary.GetWindowText(m_summaryBefore);

    m_bReloadList = true;   // what Add / Change / Delete / Paste set

    wxTheApp->CallAfter([this, dlg]() {
        m_probeSummary.GetWindowText(m_summaryAfter);
        dlg->EndModal(IDCANCEL);   // no OK button on a prefs page template
    });
    return TRUE;
}

void TestDeferredListReload()
{
    printf("Deferred entity-list reload (finding 19):\n");
    ResetTranscript();

    CReloadProbe probe;
    probe.DoModal();

    chkSeq("reload:init reload:destroy",
           "a CPropertyPage driven as a plain bridged dialog gets the same "
           "OnInitDialog/OnDestroy pair");
    chk(probe.m_sawPump, "the pump reached the page's update-UI handlers");
    chk(probe.m_summaryBefore.Find("alias") >= 0,
        "fixture: the summary static starts with the template's caption");
    chk(probe.m_summaryAfter.Find("item") >= 0 &&
        probe.m_summaryAfter.Find("alias") < 0,
        "the pump ran LoadList, which rewrote the 'N items.' summary");
    chk(!probe.m_bReloadList,
        "...and cleared m_bReloadList, so it reloads once and not every idle");
    chk(probe.m_firstPumpSawReloadPending,
        "PINNED: the port updates the controls BEFORE reloading - canonical "
        "OnKickIdle reloads first (see the note in the harness)");
}

// ══════════════════════════════════════════════════════════════════════════
// Finding 16 — the property-sheet page lifecycle.
//
// Both sheet bridges reimplement MFC's page-activation contract by hand, and
// the comments at wx_mfc_impl.cpp:4313-4421 record FOUR separately shipped bugs
// from getting the sequence wrong (colour swatches silently discarded because
// OnKillActive was not called; m_last_prefs_page never written because
// OnSetActive was not; list column widths never persisted because OnDestroy was
// not; global-prefs pages never asked to commit because OnOK was not).  All
// four are silent in use, so the transcript IS the test.
// ══════════════════════════════════════════════════════════════════════════

class CLifecyclePage : public CPropertyPage
{
public:
    CLifecyclePage(UINT idd, const char* tag) : CPropertyPage(idd), m_tag(tag) {}

    std::string m_tag;
    bool m_failValidation  = false;   // make DDV reject, as a DDV_MinMax would
    bool m_refuseKillActive = false;  // return FALSE from OnKillActive
    HWND m_hWndAtDestroy = nullptr;

    BOOL OnInitDialog() override
    {
        rec(m_tag + ":init");
        return CPropertyPage::OnInitDialog();   // → UpdateData(FALSE)
    }
    void DoDataExchange(CDataExchange* pDX) override
    {
        rec(m_tag + (pDX->m_bSaveAndValidate ? ":ddx-save" : ":ddx-load"));
        if (pDX->m_bSaveAndValidate && m_failValidation) {
            rec(m_tag + ":ddv-fail");
            pDX->Fail();          // what a failed DDV_MinMax* does
        }
    }
    // The pump that reaches a page inside a SHEET is a different pair of call
    // sites from the one that reaches a plain bridged dialog
    // (build_canonical_sheet_page vs hook_run_dialog), and it is the sheet pair
    // the shipping entity managers -- Triggers, Aliases, Timers, Variables --
    // actually run through.  Counting here is what tells the two apart.
    int m_pumps = 0;
    void OnUpdateAnything(CCmdUI* pCmdUI) { ++m_pumps; if (pCmdUI) pCmdUI->Enable(TRUE); }

    BOOL OnSetActive()  override { rec(m_tag + ":setactive");  return TRUE; }
    BOOL OnKillActive() override
    {
        rec(m_tag + ":killactive");
        return m_refuseKillActive ? FALSE : TRUE;
    }
    void OnOK()      override { rec(m_tag + ":ok"); }
    void OnDestroy() override { rec(m_tag + ":destroy"); m_hWndAtDestroy = m_hWnd; }
    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CLifecyclePage, CPropertyPage)
    ON_UPDATE_COMMAND_UI_RANGE(1, 0xFFFF, CLifecyclePage::OnUpdateAnything)
END_MESSAGE_MAP()

// Shared by both sheet drivers: the sheet's own wxDialog and its book control
// are locals inside the static hook bodies, so the only way in is from a page's
// panel outwards.
bool FindSheetWindows(CPropertyPage& firstPage, const wxChar* bookClass,
                      wxDialog** dlgOut, wxBookCtrlBase** bookOut)
{
    *dlgOut = nullptr; *bookOut = nullptr;
    wxWindow* panel = static_cast<wxWindow*>(firstPage.m_hWnd);
    if (!chk(panel != nullptr, "the sheet bound each page's m_hWnd to its panel"))
        return false;
    *dlgOut = wxDynamicCast(wxGetTopLevelParent(panel), wxDialog);
    if (!chk(*dlgOut != nullptr, "the sheet's pages live in a wxDialog"))
        return false;
    *bookOut = wxDynamicCast(FindDescendantOfClass(*dlgOut, bookClass),
                             wxBookCtrlBase);
    return chk(*bookOut != nullptr, "the sheet built its book control");
}

void DriveTreeSheet(CLifecyclePage& p0, CLifecyclePage& p1, CLifecyclePage& p2)
{
    wxDialog*       dlg  = nullptr;
    wxBookCtrlBase* book = nullptr;
    if (!FindSheetWindows(p0, wxT("wxTreebook"), &dlg, &book)) {
        if (dlg && dlg->IsModal()) dlg->EndModal(wxID_CANCEL);
        return;
    }

    // Everything the sheet did before anyone touched it: every page built and
    // DDX-loaded, then OnSetActive on the page it OPENS on — the one that
    // writes m_last_prefs_page, so that just opening the sheet and pressing OK
    // records the page you were on.
    chkDelta("p0:init p0:ddx-load p1:init p1:ddx-load p2:init p2:ddx-load "
             "p0:setactive",
             "build: every page initialises and DDX-loads, then the OPENING "
             "page gets OnSetActive");

    // Leaving a page: capture (UpdateData(TRUE)) then OnKillActive — the
    // second half is what CPrefsP3/CPrefsP5 harvest their colour swatches in,
    // and dropping it silently discarded every colour picked.
    book->SetSelection(1);
    chkDelta("p0:ddx-save p0:killactive p1:setactive",
             "leave: UpdateData then OnKillActive on the OLD page, then "
             "OnSetActive on the new one");

    // Failed validation vetoes the page change: the page has already shown its
    // message, so the sheet must stay put and must NOT run OnKillActive.
    p1.m_failValidation = true;
    book->SetSelection(2);
    chkDelta("p1:ddx-save p1:ddv-fail",
             "a failed DDV vetoes the change: no OnKillActive, no OnSetActive");
    chk(book->GetSelection() == 1, "...and the sheet stays on the failing page");
    p1.m_failValidation = false;

    // PINNED, and a divergence from MFC: the bridge consults UpdateData's
    // result but IGNORES OnKillActive's, where MFC treats FALSE as a veto.
    // Inert today -- the only two canonical overrides (CPrefsP3, CPrefsP5)
    // return CPropertyPage::OnKillActive(), i.e. always TRUE -- but a page that
    // ever refuses would be overruled without a sound.  Asserted so the day
    // someone writes one, this says so.
    p1.m_refuseKillActive = true;
    book->SetSelection(2);
    chkDelta("p1:ddx-save p1:killactive p2:setactive",
             "PINNED: OnKillActive returning FALSE does NOT veto here (MFC "
             "would); only UpdateData's result is consulted");
    p1.m_refuseKillActive = false;

    // OK validates the ACTIVE page only, and keeps the sheet open on failure.
    p2.m_failValidation = true;
    SendCommand(dlg, wxEVT_BUTTON, wxID_OK);
    chkDelta("p2:ddx-save p2:ddv-fail",
             "OK with the active page failing validation: no OnOK anywhere");
    chk(dlg->IsModal(), "...and the sheet stays open");
    p2.m_failValidation = false;

    // The accept path: active page captured + OnKillActive, then OnOK on EVERY
    // page (CPrefsP3/P5/P9/P17/P22/P23 and CGlobalPrefsP1/P10/P12 commit there),
    // then EndModal.  Note the pages the user never visited are NOT validated —
    // as on Windows, where they keep their LoadPrefs values.
    SendCommand(dlg, wxEVT_BUTTON, wxID_OK);
    chkDelta("p2:ddx-save p2:killactive p0:ok p1:ok p2:ok",
             "OK: the active page only is captured, then OnOK runs on EVERY "
             "page in add order");

    if (dlg->IsModal()) {   // belt and braces: never leave the loop wedged
        chk(false, "the OK press ended the modal loop");
        dlg->EndModal(wxID_CANCEL);
    }
}

void TestTreeSheetLifecycle()
{
    printf("Property-sheet page lifecycle - tree bridge (finding 16):\n");
    ResetTranscript();

    CLifecyclePage p0(IDD_PREFS_P13, "p0");
    CLifecyclePage p1(IDD_PREFS_P13, "p1");
    CLifecyclePage p2(IDD_PREFS_P13, "p2");

    CTreePropertySheet sheet(0, "probe sheet", nullptr, 0);
    sheet.AddPage(tps_item_branch, &p0, "Zero");
    sheet.AddPage(tps_item_node,   &p1, "One");
    sheet.AddPage(tps_item_node,   &p2, "Two");

    // Posted before DoModal, so it runs once the sheet's modal loop is up.
    wxTheApp->CallAfter([&]() { DriveTreeSheet(p0, p1, p2); });
    const INT_PTR rc = sheet.DoModal();

    chk(rc == IDOK, "the sheet reports IDOK");
    chkDelta("p0:destroy p1:destroy p2:destroy",
             "after the modal: OnDestroy on EVERY page, in add order");
    chk(p0.m_hWndAtDestroy && p1.m_hWndAtDestroy && p2.m_hWndAtDestroy,
        "...while each page's panel is still bound (its handlers read the "
        "live controls)");
    chk(!p0.m_hWnd && !p1.m_hWnd && !p2.m_hWnd,
        "...and m_hWnd is detached only afterwards");

    // ── the pump that reaches a page inside a SHEET ──────────────────────
    // pump_dialog_update_ui has FOUR call sites: two in hook_run_dialog, which
    // a plain bridged dialog runs through, and two in build_canonical_sheet_page.
    // The shipping entity managers -- Triggers, Aliases, Timers, Variables --
    // are pages in a sheet, so it is the SECOND pair that drives their
    // ON_UPDATE_COMMAND_UI handlers and their deferred list reload.  Without
    // this, neutering both sheet sites left the whole suite green: the
    // deferred-reload coverage was proving the mechanism on a path the app
    // never takes.
    chk(p0.m_pumps > 0,
        "the sheet's own update-UI pump reached the opening page");
    chk(p1.m_pumps > 0,
        "...and the page that was switched to");
}

// The flat (tabbed) bridge is a SECOND hand-written copy of the same machine
// (hook_run_property_sheet_flat), used by File ▸ Preferences.  It is worth its
// own scenario precisely because it is a copy: the two have already drifted.
void DriveFlatSheet(CLifecyclePage& f0, CLifecyclePage& f1)
{
    wxDialog*       dlg  = nullptr;
    wxBookCtrlBase* book = nullptr;
    if (!FindSheetWindows(f0, wxT("wxNotebook"), &dlg, &book)) {
        if (dlg && dlg->IsModal()) dlg->EndModal(wxID_CANCEL);
        return;
    }

    // PINNED, and the drift: the flat bridge has no wxEVT_NOTEBOOK_PAGE_CHANGED
    // bind and no opening-page OnSetActive, so the 13 CGlobalPrefsP*::OnSetActive
    // overrides never run at all.  They are pass-throughs today, so it is inert
    // — but it is an asymmetry with the tree bridge above that nothing else
    // would ever notice.
    chkDelta("f0:init f0:ddx-load f1:init f1:ddx-load",
             "PINNED: the flat bridge runs no OnSetActive at all, not even on "
             "the opening page (the tree bridge does)");

    f0.m_failValidation = true;
    book->SetSelection(1);
    chkDelta("f0:ddx-save f0:ddv-fail",
             "flat: a failed DDV vetoes the tab change too");
    chk(book->GetSelection() == 0, "...and the sheet stays on the failing tab");
    f0.m_failValidation = false;

    book->SetSelection(1);
    chkDelta("f0:ddx-save f0:killactive",
             "flat: leave runs UpdateData then OnKillActive - and still no "
             "OnSetActive on arrival");

    // Cancel: wxDialog's own handler ends the loop with wxID_CANCEL.
    SendCommand(dlg, wxEVT_BUTTON, wxID_CANCEL);
    if (dlg->IsModal()) {
        chk(false, "the Cancel press ended the modal loop");
        dlg->EndModal(wxID_CANCEL);
    }
}

void TestFlatSheetLifecycle()
{
    printf("Property-sheet page lifecycle - flat bridge (finding 16):\n");
    ResetTranscript();

    CLifecyclePage f0(IDD_PREFS_P13, "f0");
    CLifecyclePage f1(IDD_PREFS_P13, "f1");

    CPropertySheet sheet("flat probe sheet");
    sheet.AddPage(&f0);
    sheet.AddPage(&f1);

    wxTheApp->CallAfter([&]() { DriveFlatSheet(f0, f1); });
    const INT_PTR rc = sheet.DoModal();

    chk(rc == IDCANCEL, "Cancel reports IDCANCEL");
    chkDelta("f0:destroy f1:destroy",
             "OnDestroy runs on EVERY page on CANCEL too - that is where a page "
             "persists its column widths, which Cancel must not lose");
    chk(Transcript().find(":ok") == std::string::npos,
        "...and Cancel asked no page to commit");
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════

int RunDialogBridgeSelfTest()
{
    printf("EXTENDED-SELFTEST(dialog-bridge): the MFC->wx dialog and "
           "property-sheet bridges.\n");

    TestCommandRouting();
    TestCloseBoxRunsOnCancel();
    TestEscapeRunsCancel();
    TestListRightClickDispatchesRdblclk();
    TestDeferredListReload();
    TestTreeSheetLifecycle();
    TestFlatSheetLifecycle();

    printf("EXTENDED-SELFTEST(dialog-bridge): %d failed\n", g_fail);
    return g_fail;
}

#endif // !_WIN32
