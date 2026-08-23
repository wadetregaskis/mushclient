// tests/win/test_menu_rules_win.cpp — the claims the port's menu-rule table
// makes ABOUT WINDOWS, checked against Windows.
//
// platform/menu_ui_rules.cpp exists because wxWidgets and MFC disagree by
// default: MFC greys a menu item that nothing in the command-routing chain
// handles, wx leaves it enabled.  So the port restates MFC's answer in a table,
// and tests/test_menu_rules.cpp pins the table.
//
// A table restating another system's behaviour is only as good as the reading
// it was written from, and that reading has never been checked against the
// system.  This file checks it.  Every list here is copied literally from
// tests/test_menu_rules.cpp — deliberately, because two files asserting the
// same claim about two different subjects IS the cross-check.  If MFC and the
// table ever disagree, one of these two suites fails and the other does not,
// and that is the whole point.  (Looping either one out of the other's data
// would make both agree with anything.)
//
// The mechanism is MFC's own.  CCmdUI::DoUpdate(pTarget, TRUE) is exactly what
// CFrameWnd::OnInitMenuPopup calls to decide how to draw a menu item, so
// running it against the same targets asks the question the menu asks.  For the
// "who owns this command" half, OnCmdMsg with a non-NULL AFX_CMDHANDLERINFO
// REPORTS the handler instead of dispatching it, which answers "is there one"
// without side effects.
//
// Nothing here needs a window.  The frame is the host's CMainFrame object
// (mc_test.h MC_TEST_WIN_MAIN_FRAME) and the views are bare DYNCREATE
// instances, which is enough because a message map is a static table and
// OnCmdMsg walks it whether or not the object owns an HWND.
//
// tests/test_menu_rules.cpp stays excluded from this target: two thirds of it
// drives PortMenuRuleUiState / PortMenuActionForCommandId / kPortMenuId*, which
// are the port's own inventions and have no Windows meaning at all.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"
#include "MUSHview.h"
#include "sendvw.h"
#include "OtherTypes.h"

#include <string>
#include <vector>

namespace {

// A CCmdUI that records what the handler did to it instead of drawing it.
// Enable() must set m_bEnableChanged itself: DoUpdate reads that flag to decide
// whether anybody answered, and the base implementation asserts on a CCmdUI
// with neither a menu nor a control behind it.
struct CmdUiProbe : public CCmdUI {
    int     enableCalls;
    bool    enabled;
    int     check;          // -1 = SetCheck never called
    bool    sawText;
    CString text;

    explicit CmdUiProbe(UINT id)
        : enableCalls(0), enabled(false), check(-1), sawText(false)
    { m_nID = id; }

    virtual void Enable(BOOL bOn)
    {
        ++enableCalls;
        enabled = (bOn != FALSE);
        m_bEnableChanged = TRUE;
    }
    virtual void SetCheck(int nCheck) { check = nCheck; }
    virtual void SetText(LPCTSTR t)   { sawText = true; text = t; }
};

// Is there a COMMAND handler for this id anywhere in the target's routing
// chain?  A non-NULL AFX_CMDHANDLERINFO makes CCmdTarget::OnCmdMsg report the
// handler rather than call it, so this asks the message maps without running
// anything — and it is the same question CCmdUI::DoUpdate itself asks when no
// update-UI handler spoke up.
bool has_command_handler(CCmdTarget* target, UINT id)
{
    CmdUiProbe   probe(id);
    AFX_CMDHANDLERINFO info;
    info.pTarget = NULL;
    info.pmf     = NULL;
    return target->OnCmdMsg(id, CN_COMMAND, &probe, &info) != FALSE;
}

// The same question for the UPDATE-UI side.  Almost useless in this app, and
// deliberately kept so one test can prove why: see the catch-all note below.
bool has_update_handler(CCmdTarget* target, UINT id)
{
    CmdUiProbe   probe(id);
    AFX_CMDHANDLERINFO info;
    info.pTarget = NULL;
    info.pmf     = NULL;
    return target->OnCmdMsg(id, CN_UPDATE_COMMAND_UI, &probe, &info) != FALSE;
}

// ── the lists, copied literally from tests/test_menu_rules.cpp ─────────────

// Commands the port's table says Windows greys with no world open, because
// their only handler is on the view, the input bar or the document.
const struct { int id; const char* name; } kWorldOnly[] = {
    { 32248, "ID_DISPLAY_FIND" },
    { 32249, "ID_DISPLAY_FINDAGAIN" },
    { 32201, "ID_DISPLAY_FREEZEOUTPUT" },
    { 32963, "ID_DISPLAY_GOTOLINE" },
    { 32250, "ID_FILE_PRINT_WORLD" },
    { 32275, "ID_DISPLAY_GOTOURL" },
    { 32276, "ID_DISPLAY_SENDMAILTO" },
    { 33036, "ID_DISPLAY_HIGHLIGHTPHRASE" },
    { 33044, "ID_DISPLAY_MULTILINETRIGGER" },
    { 32331, "ID_DISPLAY_TEXTATTRIBUTES" },
    { 32318, "ID_DISPLAY_BOOKMARKSELECTION" },
    { 32319, "ID_DISPLAY_GOTOBOOKMARK" },
    { 32320, "ID_INPUT_GLOBALCHANGE" },
    { 32323, "ID_EDIT_SPELLCHECK" },
    { 32326, "ID_EDIT_GENERATECHARACTERNAME" },
    { 32272, "ID_REPEAT_LAST_COMMAND" },
    { 32152, "ID_GAME_COMMANDHISTORY" },
    { 32946, "ID_EDIT_GOTOMATCHINGBRACE" },
    { 32947, "ID_EDIT_SELECTTOMATCHINGBRACE" },
    { 32953, "ID_EDIT_FLIPTONOTEPAD" },
    { 32983, "ID_FILE_PLUGINS" },
    { 32971, "ID_EDIT_DEBUGINCOMINGPACKETS" },
    { 32334, "ID_GAME_TRACE" },
};

// Commands that work with no world open.  The port's table must NOT claim
// these, or a table that greyed everything would pass the list above.
//
// ID_FILE_OPENWORLDSINSTARTUPLIST is NOT here even though the port's list has
// it: CMainFrame::OnUpdateFileOpenworldsinstartuplist greys it when the startup
// list is empty, so it is conditional rather than unconditionally live, and it
// gets a test of its own below.  The port agrees — platform/menubar_selftest.cpp
// drives both arms of the same condition.
const struct { int id; const char* name; } kGlobal[] = {
    { 32327,  "ID_EDIT_RELOADNAMESFILE" },
    { 32203,  "ID_DISPLAY_ACTIVITYLIST" },
    { 32325,  "ID_GAME_SENDTOALLWORLDS" },
    { 32234,  "ID_GAME_MINIMISEPROGRAM" },
    { 0xE140, "ID_APP_ABOUT" },

    // ── moved here from the port's world-only list, 2026-08-21 ─────────────
    // Windows has THREE handlers for Functions List: CSendView (input-bar
    // context), CTextView (notepad context) and CMainFrame — and the frame's
    // (scripting/functionlist.cpp:602) is context-free, `ShowFunctionslist ("",
    // 0, 0, true)`.  So the item is live with nothing open at all, and this is
    // the divergence this suite was written to find: the port had it in the
    // greyed list.  See docs/WINDOWS-VALIDATION.md 10am.
    { 33046,  "ID_GAME_FUNCTIONSLIST" },
};

// A bare view, made the way the framework makes one and deleted the way the
// access specifiers allow (~CView is protected; CObject's destructor is public
// and virtual, so this is the same deletion by a permitted route).
//
// The CRuntimeClass is passed in rather than derived from a template
// parameter: RUNTIME_CLASS(T) token-pastes to `T::classT`, so inside a template
// it names a member called `classT` on every class.  A helper that looks like
// it should be generic here cannot be.
CView* make_view(CRuntimeClass* rc) { return (CView*) rc->CreateObject(); }
void   free_view(CObject* v)        { delete v; }

// A view ATTACHED to a document, for the handlers that read GetDocument().
//
// The document is deleted FIRST and the view is never RemoveView'd, which is
// not tidiness but correctness: CDocument::RemoveView on the last view runs
// OnChangedViewList, which closes and DELETES the document — fatal for one
// that is not on the heap, and a double delete for one that is.  ~CDocument
// calls DisconnectViews instead, which detaches without deleting.  This is the
// same shape tests/test_accelerators.cpp uses, for the same reason.
//
// AddView prints three MFC assertions per fixture and they are expected:
// CDocument::UpdateFrameCounts walks the view list asserting
// ::IsWindow(pView->m_hWnd) before calling IsWindowVisible, and these views
// have no window.  The count it is maintaining comes out 0, which is right.
struct ViewDoc {
    CView*          view;
    CMUSHclientDoc* doc;

    explicit ViewDoc(CRuntimeClass* rc)
      : view(make_view(rc)),
        doc(new CMUSHclientDoc)
    { if (view) doc->AddView(view); }

    ~ViewDoc() { delete doc; free_view(view); }   // document first

    CMUSHclientDoc& d() { return *doc; }

private:
    ViewDoc(const ViewDoc&);
    ViewDoc& operator=(const ViewDoc&);
};

}   // namespace

// ── the catch-all, which is why only the OUTCOME can be asserted ───────────

TEST(MenuRulesWin, EveryIdHasAnUpdateUiHandlerAndAlmostNoneHasACommandOne)
{
    // `mainfrm.cpp:238  ON_UPDATE_COMMAND_UI_RANGE (0, 0xFFFF, OnFixMenus)` —
    // CMainFrame claims the update-UI side of EVERY command id there is, and
    // `doc.cpp:205` does the same on the document.  Both handlers call
    // ContinueRouting, so at run time they strip an accelerator from the
    // caption and step aside; but a probe that asks "is there an update-UI
    // handler" is answered by the catch-all before any of that, because
    // AFX_CMDHANDLERINFO short-circuits _AfxDispatchCmdMsg before the handler
    // runs and therefore before ContinueRouting can be seen.
    //
    // So "is this command handled?" cannot be asked of the update-UI side in
    // this app at all — the honest question is what DoUpdate ENABLES, which is
    // what the rest of this file asserts.  This test exists to pin the fact
    // that reasoning rests on, because it is invisible at every call site.
    ASSERT_TRUE(App.m_pMainWnd != NULL);
    const UINT kNobodysId = 31111;   // in no menu, in no message map
    EXPECT_TRUE(has_update_handler(App.m_pMainWnd, kNobodysId));
    EXPECT_FALSE(has_command_handler(App.m_pMainWnd, kNobodysId));

    // ...and it is still greyed, because DoUpdate falls through to the COMMAND
    // side once the catch-all declines to set an enable state.
    CmdUiProbe probe(kNobodysId);
    probe.DoUpdate(App.m_pMainWnd, TRUE);
    EXPECT_TRUE(probe.enableCalls > 0);
    EXPECT_FALSE(probe.enabled);
}

// ── who owns which command ─────────────────────────────────────────────────

TEST(MenuRulesWin, WorldOnlyCommandsAreGreyedWithNoWorldOpen)
{
    // The claim the whole port table rests on.  With no world open the routing
    // chain is CMainFrame then CWinApp — CFrameWnd::OnCmdMsg's first step is the
    // active view, and there is none — so a command whose only handler lives on
    // the world's view, input bar or document reaches nobody, and MFC greys it.
    ASSERT_TRUE(App.m_pMainWnd != NULL);
    for (size_t i = 0; i < sizeof kWorldOnly / sizeof kWorldOnly[0]; ++i) {
        CmdUiProbe probe(kWorldOnly[i].id);
        probe.DoUpdate(App.m_pMainWnd, TRUE);
        if (probe.enabled)
            printf("      (%s (%d) is ENABLED with no world open)\n",
                   kWorldOnly[i].name, kWorldOnly[i].id);
        EXPECT_TRUE(probe.enableCalls > 0);   // somebody decided, either way
        EXPECT_FALSE(probe.enabled);
    }
}

TEST(MenuRulesWin, EveryWorldOnlyCommandIsClaimedSomewhereInTheWorldChain)
{
    // The other half, and the one that makes the list above mean something: an
    // id nobody handles ANYWHERE would pass the test above while being a typo.
    // Asked on the COMMAND side, because the update-UI side is swallowed by the
    // catch-all above — CMUSHclientDoc carries one of its own.
    CView*         view = make_view(RUNTIME_CLASS(CMUSHView));
    CView*         send = make_view(RUNTIME_CLASS(CSendView));
    CMUSHclientDoc doc;
    ASSERT_TRUE(view != NULL);
    ASSERT_TRUE(send != NULL);

    for (size_t i = 0; i < sizeof kWorldOnly / sizeof kWorldOnly[0]; ++i) {
        const bool onView = has_command_handler(view, kWorldOnly[i].id);
        const bool onSend = has_command_handler(send, kWorldOnly[i].id);
        const bool onDoc  = has_command_handler(&doc, kWorldOnly[i].id);
        if (!onView && !onSend && !onDoc)
            printf("      (%s (%d) is handled by NOBODY)\n",
                   kWorldOnly[i].name, kWorldOnly[i].id);
        EXPECT_TRUE(onView || onSend || onDoc);
    }

    free_view(send);
    free_view(view);
}

TEST(MenuRulesWin, GlobalCommandsAreLiveWithNoWorldOpen)
{
    // The control on the list above: a table that greyed everything would pass
    // it.  These six are handled by CMainFrame or by the app, so they survive
    // with no world open — which for most of them means an ON_COMMAND with no
    // update-UI handler of its own, and DoUpdate's fall-through enabling it.
    ASSERT_TRUE(App.m_pMainWnd != NULL);
    for (size_t i = 0; i < sizeof kGlobal / sizeof kGlobal[0]; ++i) {
        CmdUiProbe probe(kGlobal[i].id);
        probe.DoUpdate(App.m_pMainWnd, TRUE);
        if (!probe.enabled)
            printf("      (%s (%d) is GREYED with no world open)\n",
                   kGlobal[i].name, kGlobal[i].id);
        EXPECT_TRUE(probe.enabled);
    }
}

TEST(MenuRulesWin, OpenWorldsInStartupListFollowsTheStartupList)
{
    // Not "global" and not world-only: a third shape, live or dead by a piece
    // of GLOBAL state.  Worth pinning here because a suite that only knew the
    // two shapes would have to put it in one of them and be wrong either way —
    // which is exactly what the first draft of this file did.
    ASSERT_TRUE(App.m_pMainWnd != NULL);
    const CString saved = App.m_strWorldList;

    App.m_strWorldList = "";
    CmdUiProbe empty(32954 /* ID_FILE_OPENWORLDSINSTARTUPLIST */);
    EXPECT_TRUE(empty.DoUpdate(App.m_pMainWnd, TRUE) != FALSE);   // a real handler
    EXPECT_FALSE(empty.enabled);

    App.m_strWorldList = "example.mcl";
    CmdUiProbe listed(32954);
    listed.DoUpdate(App.m_pMainWnd, TRUE);
    EXPECT_TRUE(listed.enabled);

    App.m_strWorldList = saved;
}

// ── the behaviour of the handlers the table dispatches to ──────────────────
//
// Connect and Disconnect are NOT restated in the port's table: it dispatches
// them through the real message map, so these tests are about the canonical
// handlers themselves (doc.cpp OnUpdateConnectionConnect / ...Disconnect).

TEST(MenuRulesWin, ConnectNeedsADisconnectedWorldWithAnAddress)
{
    CMUSHclientDoc doc;
    doc.m_server = "mud.example.com";

    doc.m_iConnectPhase = eConnectNotConnected;
    CmdUiProbe ready(32239 /* ID_CONNECTION_CONNECT */);
    EXPECT_TRUE(ready.DoUpdate(&doc, TRUE) != FALSE);
    EXPECT_TRUE(ready.enabled);

    // A world still on the placeholder address has nowhere to connect to.
    doc.m_server = "0.0.0.0";
    CmdUiProbe placeholder(32239);
    placeholder.DoUpdate(&doc, TRUE);
    EXPECT_FALSE(placeholder.enabled);
    doc.m_server = "mud.example.com";

    // Every phase from the first name lookup on means a connect is already
    // under way or finished.
    for (int phase = eConnectMudNameLookup; phase <= eConnectDisconnecting; phase++) {
        doc.m_iConnectPhase = phase;
        CmdUiProbe p(32239);
        p.DoUpdate(&doc, TRUE);
        if (p.enabled)
            printf("      (Connect is live in phase %d)\n", phase);
        EXPECT_FALSE(p.enabled);
    }
}

TEST(MenuRulesWin, DisconnectIsLiveThroughoutAHangingConnect)
{
    // The rule that has shipped broken before: Disconnect must be live on EVERY
    // non-zero phase, so a connect to a host that never answers can still be
    // cancelled.  Phases 1-8 are the two name lookups, the two connects, the
    // three SOCKS waits and the TLS handshake.
    CMUSHclientDoc doc;

    doc.m_iConnectPhase = eConnectNotConnected;
    CmdUiProbe idle(32240 /* ID_CONNECTION_DISCONNECT */);
    EXPECT_TRUE(idle.DoUpdate(&doc, TRUE) != FALSE);
    EXPECT_FALSE(idle.enabled);

    for (int phase = eConnectMudNameLookup; phase <= eConnectDisconnecting; phase++) {
        doc.m_iConnectPhase = phase;
        CmdUiProbe p(32240);
        p.DoUpdate(&doc, TRUE);
        if (!p.enabled)
            printf("      (Disconnect is greyed in phase %d)\n", phase);
        EXPECT_TRUE(p.enabled);
    }
}

TEST(MenuRulesWin, TraceAndDebugChecksFollowTheDocument)
{
    // Both are per-DOCUMENT flags, and both are check items.  The port used to
    // tick them once at click time, so the tick described whichever world was
    // active then; here the canonical handlers are asked per document.
    CMUSHclientDoc a, b;
    a.m_bTrace = true;
    a.m_bDebugIncomingPackets = false;
    b.m_bTrace = false;
    b.m_bDebugIncomingPackets = true;

    CmdUiProbe traceA(32334);  traceA.DoUpdate(&a, TRUE);
    CmdUiProbe traceB(32334);  traceB.DoUpdate(&b, TRUE);
    EXPECT_EQ(1, traceA.check);
    EXPECT_EQ(0, traceB.check);

    CmdUiProbe dbgA(32971);    dbgA.DoUpdate(&a, TRUE);
    CmdUiProbe dbgB(32971);    dbgB.DoUpdate(&b, TRUE);
    EXPECT_EQ(0, dbgA.check);
    EXPECT_EQ(1, dbgB.check);

    // Toggling the flag flips the tick on the next menu draw.
    a.m_bTrace = false;
    CmdUiProbe again(32334);   again.DoUpdate(&a, TRUE);
    EXPECT_EQ(0, again.check);
}

TEST(MenuRulesWin, PlainCommandsAreNotCheckItems)
{
    // A command whose handler calls SetCheck would draw a permanent empty tick
    // box beside itself.  -1 here means SetCheck was never called.
    ViewDoc fixture(RUNTIME_CLASS(CMUSHView));
    ASSERT_TRUE(fixture.view != NULL);

    static const int kPlain[] = {
        32248,  // ID_DISPLAY_FIND
        32320,  // ID_INPUT_GLOBALCHANGE
        32250,  // ID_FILE_PRINT_WORLD
    };
    for (size_t i = 0; i < sizeof kPlain / sizeof kPlain[0]; ++i) {
        CmdUiProbe p(kPlain[i]);
        p.DoUpdate(fixture.view, TRUE);
        EXPECT_EQ(-1, p.check);
    }
}

TEST(MenuRulesWin, QuitFromThisWorldNeedsAConnectedSession)
{
    // ID_ACTIONS_QUIT's update-UI is CSendView::OnUpdateSessionOpen — the input
    // bar, not the output view — and it reads the DOCUMENT's connect phase
    // through GetDocument().  So the view has to be attached to one.
    ViewDoc fixture(RUNTIME_CLASS(CSendView));
    ASSERT_TRUE(fixture.view != NULL);

    fixture.d().m_iConnectPhase = eConnectNotConnected;
    CmdUiProbe closed(32217 /* ID_ACTIONS_QUIT */);
    EXPECT_TRUE(closed.DoUpdate(fixture.view, TRUE) != FALSE);
    EXPECT_FALSE(closed.enabled);

    fixture.d().m_iConnectPhase = eConnectConnectedToMud;
    CmdUiProbe open(32217);
    open.DoUpdate(fixture.view, TRUE);
    EXPECT_TRUE(open.enabled);

    // Mid-connect is not connected.
    fixture.d().m_iConnectPhase = eConnectAwaitingSSLHandshake;
    CmdUiProbe handshaking(32217);
    handshaking.DoUpdate(fixture.view, TRUE);
    EXPECT_FALSE(handshaking.enabled);
}
