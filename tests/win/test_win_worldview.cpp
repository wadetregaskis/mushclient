// tests/win/test_win_worldview.cpp — every method of the native Windows
// pal::IWorldView adapter, called when there is nothing behind it.
//
// `pal/win_worldview.cpp` is ~1200 lines of port-introduced code that ships to
// every Windows user: about fifty methods forwarding to `CMUSHView`,
// `CSendView`, `CChildFrame` and `CMainFrame`, replacing calls that used to be
// inline in `doc.cpp` / `doc_shared.cpp`. `tests/test_pal_worldview.cpp` drives
// the PORT's adapter and is excluded here, so until now this file had no test
// on the platform it exists for.
//
// ── What this pins, and why it is one test rather than fifty ───────────────
//
// Every one of those methods has to survive being called when the thing it
// forwards to does not exist. That is not a hypothetical: the whole point of
// the adapter is that the engine no longer knows whether a frontend is there,
// and on Windows a document exists from `File ▸ New World` before its frame is
// created, keeps existing while the frame is being destroyed, and is
// constructed with no window at all by every one of this suite's 180 other
// test binaries.
//
// The failure mode is specific and this campaign has already paid for it twice.
// `docs/WINDOWS-VALIDATION.md` §10v: *"A guard that has to dereference the
// thing it is guarding is not a guard"* — `CMainFrame::SetStatusMessage`
// guarded on `App.m_pMainWnd` (a frame OBJECT) to decide whether the status
// BAR had a window, and that one mistake was behind three crashing suites and
// two exclusions. **Six more of the same shape were in this very file**, fixed
// by routing all 27 `Frame.` uses through a `frame()` helper that returns NULL
// unless `m_hWnd` is non-null. Nothing pinned that fix. This does.
//
// ── The observable ─────────────────────────────────────────────────────────
//
// Two things, and the second is the one that makes this worth writing:
//
//   1. the process survives — a null dereference here is an access violation,
//      which `McTestInstallCrashHandler` turns into a symbolised stack naming
//      the method;
//   2. **`McTestAssertionsRaised ()` does not move.** MFC asserts wherever a
//      Win32 handle is required and absent, so a method that "works" by
//      calling `CWnd::SetWindowText` on a null `m_hWnd` still trips
//      `ASSERT(::IsWindow(m_hWnd))`. In a Debug build of the product that is a
//      modal Abort/Retry/Ignore box; in Release it is a silent no-op that the
//      author never intended. Without the counter this test could only catch
//      the crashes, which is the half that already gets noticed.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"
#include "MUSHview.h"       // CMUSHView: two views, one document
#include "pal/iworldview.h"
#include "pal/win_worldview.h"

#include <string>
#include <vector>

long McTestAssertionsRaised();

namespace {

// Call everything on `v`.  Arguments are deliberately unhelpful — negative
// lines, out-of-range colours, empty and null strings — because a method that
// only survives well-formed input is not guarded, it is lucky.
//
// The return values are read into volatile sinks rather than discarded: a
// getter that returns an uninitialised value is a real defect, and reading it
// is what lets the debug CRT and any future sanitizer see it.
void drive_every_method(pal::IWorldView* v)
{
    volatile bool  b = false;
    volatile int   i = 0;
    volatile long  l = 0;
    (void) b; (void) i; (void) l;

    // ── core → frontend: output ────────────────────────────────────────────
    SOutputLine line;
    v->appendLine(std::move(line));
    v->note("a note");
    v->note("");
    v->setBookmark(0, true);
    v->setBookmark(-1, false);          // a line index that cannot exist
    v->setTitle("title");
    v->setStatus("status");
    v->setBackgroundColour(0x00FF00FFu);
    v->setMaxLines(0);
    v->setMaxLines(-1);
    v->requestRepaint(pal::Rect{ 0, 0, 0, 0 });
    v->requestRepaint(pal::Rect{ -5, -5, 10, 10 });
    v->clearOutput();
    v->setPaused(true);
    v->setPaused(false);
    v->flash();

    // ── frontend → core: readers ───────────────────────────────────────────
    b = v->isPaused();
    { std::string s = v->currentCommand();       (void) s.size(); }
    { pal::TextRange r = v->outputSelection();   i = r.startLine; }
    i = v->scrollPos();
    { pal::Size sz = v->outputAreaChars();       i = sz.w; }

    // ── the command bar ────────────────────────────────────────────────────
    b = v->setCommand("typed");
    b = v->setCommand("");
    v->selectCommand();
    { pal::Range r = v->commandSelection();      i = r.start; }
    v->setCommandSelection(0, 0);
    v->setCommandSelection(-1, 9999);
    v->replaceCommandSelection(0, 0, "x");
    v->replaceCommandSelection(-1, -1, "");
    { std::string s = v->pushCommand();          (void) s.size(); }
    { std::vector<std::string> h = v->commandList(5);   (void) h.size(); }
    { std::vector<std::string> h = v->commandList(0);   (void) h.size(); }
    { std::vector<std::string> h = v->commandList(-1);  (void) h.size(); }
    v->addToCommandHistory("one");
    v->deleteCommandHistory();
    v->setInputFont("Courier New", 10, false, false);
    v->setInputFont("", 0, true, true);          // "keep current" on both
    b = v->setCommandWindowHeight(100);
    b = v->setCommandWindowHeight(0);

    // ── the output pane ────────────────────────────────────────────────────
    v->beginBatch();
    v->endBatch();
    v->deleteLines(1);
    v->deleteLines(0);
    v->deleteHeadLines(1);
    v->deleteHeadLines(0);
    v->setScroll(0);
    v->setScroll(-1);
    v->setOutputSelection(0, 0, 0, 0);           // the documented "clear"
    v->setOutputSelection(1, 2, 1, 5);
    v->setOutputFont("Courier New", 10, false, false);
    v->setOutputFont("", 0, false, false);
    v->textRectChanged();
    { pal::Rect r = v->outputRect();             i = r.w; }
    b = v->setOutputWidthPixels(400);

    // ── the world window and the app ───────────────────────────────────────
    v->setMainTitle("main");
    v->moveWorldWindow(0, 0, 100, 100);
    for (int status = 0; status <= 5; ++status)  // 1..4 are the documented set
        v->setWindowStatus(status);
    v->activateClient();
    b = v->isFullscreen();
    b = v->saveWorld();
    { pal::Rect r = v->mainRect();               i = r.h; }
    { pal::Rect r = v->mainClientRect();         i = r.h; }
    b = v->hasMainWindow();

    // ── miniwindow mouse routing ───────────────────────────────────────────
    v->mouseMoveMiniWindow(0, 0);
    v->mouseMoveMiniWindow(-1, -1);
    b = v->mouseCaptured();

    // ── the info bar ───────────────────────────────────────────────────────
    v->infobarShow(true);
    v->infobarShow(false);
    v->infobarClear();
    v->infobarText("info");
    v->infobarText("");
    v->infobarColour(0x00112233u);
    v->infobarBackground(0x00445566u);
    v->infobarFont("Courier New", 9, 0);
    v->infobarFont("", 0, 3);
    { pal::Size sz = v->infobarSize();           i = sz.w; }
}

}   // namespace

TEST(WinWorldView, EveryMethodSurvivesAFrameWithNoWindow)
{
    // The host constructs CMainFrame and never Create()s it (mc_test.h
    // MC_TEST_WIN_MAIN_FRAME), so `Frame` is a live object whose m_hWnd is
    // NULL — exactly the state that broke three suites before the `frame()`
    // helper existed, and exactly the state every other test binary in this
    // target runs in.
    ASSERT_TRUE(App.m_pMainWnd != NULL);
    ASSERT_TRUE(((CMainFrame*) App.m_pMainWnd)->m_hWnd == NULL);

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.m_pWorldView != NULL);       // the doc builds its own

    const long before = McTestAssertionsRaised();
    drive_every_method(doc.m_pWorldView);
    const long raised = McTestAssertionsRaised() - before;

    if (raised != 0)
        printf("      (%ld MFC assertion(s) from the no-window adapter)\n", raised);
    EXPECT_EQ(0L, raised);
}

TEST(WinWorldView, EveryMethodSurvivesAnAdapterWhoseDocumentHasNoView)
{
    // The same sweep over a SEPARATE adapter built by hand, so the pass above
    // cannot be resting on some state the document's own adapter happens to
    // hold.  This one is destroyed here rather than by the document.
    CMUSHclientDoc doc;
    pal::IWorldView* v = pal::CreateWinWorldView(&doc);
    ASSERT_TRUE(v != NULL);

    const long before = McTestAssertionsRaised();
    drive_every_method(v);
    const long raised = McTestAssertionsRaised() - before;

    if (raised != 0)
        printf("      (%ld MFC assertion(s) from a hand-built adapter)\n", raised);
    EXPECT_EQ(0L, raised);

    delete v;
}

TEST(WinWorldView, TheReadersAnswerTheDocumentedEmptyValuesRatherThanRubbish)
{
    // Surviving is not enough: a getter with no window must return the value
    // the engine's callers are written for, not whatever was on the stack.
    // These are the ones whose "nothing there" answer is documented in
    // pal/iworldview.h, and each has a caller that branches on it.
    CMUSHclientDoc doc;
    pal::IWorldView* v = doc.m_pWorldView;
    ASSERT_TRUE(v != NULL);

    EXPECT_FALSE(v->isPaused());                 // not frozen when not shown
    EXPECT_TRUE(v->currentCommand().empty());    // no bar, no text
    EXPECT_TRUE(v->commandList(10).empty());     // no history to report
    EXPECT_FALSE(v->hasMainWindow());
    EXPECT_FALSE(v->mouseCaptured());
    EXPECT_FALSE(v->isFullscreen());

    // "all zeros" is how outputSelection says there is no selection — the
    // documented sentinel, and 1-based elsewhere so zero cannot be a real one.
    const pal::TextRange sel = v->outputSelection();
    EXPECT_EQ(0, sel.startLine); EXPECT_EQ(0, sel.startCol);
    EXPECT_EQ(0, sel.endLine);   EXPECT_EQ(0, sel.endCol);

    const pal::Range cmdSel = v->commandSelection();
    EXPECT_EQ(0, cmdSel.start);  EXPECT_EQ(0, cmdSel.end);

    // The infobar is documented to report (0,0) when it is not showing, and
    // there is certainly no infobar here.
    const pal::Size ib = v->infobarSize();
    EXPECT_EQ(0, ib.w);          EXPECT_EQ(0, ib.h);

    // setCommandWindowHeight and setOutputWidthPixels are the two that report
    // refusal; with no panes to resize they must refuse rather than claim
    // success (the engine turns false into eBadParameter).
    EXPECT_FALSE(v->setCommandWindowHeight(100));
    EXPECT_FALSE(v->setOutputWidthPixels(400));
}

TEST(WinWorldView, SaveWorldAnswersTrueForASaveThatHappened)
{
    // The contract (pal/iworldview.h) is "false = no path", so true means the
    // file was written.  This adapter used to answer `!DoSave (...)`, copied
    // from CMUSHclientDoc::Save whose own comment says "returns zero if no
    // error" -- an ERROR CODE pasted into a BOOLEAN seam, giving false for a
    // save that worked.  The wx adapter and the test fake both say true.
    //
    // Latent rather than live: nothing calls SaveWorldViaView yet.  Which is
    // exactly why it needs pinning -- the cost lands on whoever wires it up.
    CMUSHclientDoc doc;
    pal::IWorldView* v = doc.m_pWorldView;
    ASSERT_TRUE(v != NULL);

    // No path: refuse, and do NOT go looking for one.  Prompting is what the
    // old body did, and in this host CWinApp::DoPromptFileName's
    // ENSURE (m_pDocManager != NULL) throws out of the adapter.
    ASSERT_TRUE(doc.GetPathName().IsEmpty());
    const long before = McTestAssertionsRaised();
    EXPECT_FALSE(v->saveWorld());
    EXPECT_EQ(0L, McTestAssertionsRaised() - before);

    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_wv_%d.mcl", (int) ::GetCurrentProcessId());
    ::DeleteFileA(path);
    doc.SetPathName(path, FALSE);

    EXPECT_TRUE(v->saveWorld());
    EXPECT_NE(0xFFFFFFFFu, (unsigned) ::GetFileAttributesA(path));   // it exists

    ::DeleteFileA(path);
}

// ── Which view's selection does a script see? ──────────────────────────────
//
// `Window ▸ New Window` (ID_WINDOW_NEW, in the menu with no handler of the
// app's own, so MFC's `CMDIFrameWnd::OnWindowNew` serves it) gives one world
// TWO `CMUSHView`s. Each carries its own `m_selstart_line`/`m_selend_col`.
//
// `outputSelection` — behind `GetSelectionStart/EndLine/Column` — walks the
// document's view list and answers the FIRST `CMUSHView` it finds, while
// `CMUSHView::OnEditCopy` copies `this` view's selection via `GetSelection`.
// So with two windows open, selecting in the second and asking a script what
// is selected reports the first window's answer, which is usually "nothing".
//
// This is upstream's own loop, transcribed (`git show e362b37`); the port did
// not introduce it and the adapter only added an `m_LineList` emptiness guard
// upstream lacks. It is pinned rather than changed because "which view is
// authoritative" is a genuine design question for a document-level API, and
// answering it differently would alter a documented return value on both
// platforms. `docs/WINDOWS-VALIDATION.md` §10av records the reasoning.
//
// AddView raises MFC assertions here — CDocument::UpdateFrameCounts walks the
// view list asserting ::IsWindow on views that have none, and it re-walks the
// whole list on each add.  Measured: 18 for this test, which is the suite's
// only contribution to the assertion baseline from this file.  Same expected
// noise tests/win/test_menu_rules_win.cpp documents for the same reason.
TEST(WinWorldViewR6, OutputSelectionAnswersTheFirstViewNotTheSelectedOne)
{
    CMUSHclientDoc* doc = new CMUSHclientDoc;
    doc->DisplayMsg("hello world", 11, COMMENT);
    ASSERT_TRUE(doc->m_LineList.GetCount() > 0);

    CMUSHView* first  = (CMUSHView*) RUNTIME_CLASS(CMUSHView)->CreateObject();
    CMUSHView* second = (CMUSHView*) RUNTIME_CLASS(CMUSHView)->CreateObject();
    ASSERT_TRUE(first != NULL && second != NULL);
    doc->AddView(first);
    doc->AddView(second);

    // Nothing selected in the first window...
    first->m_selstart_line = first->m_selend_line = 0;
    first->m_selstart_col  = first->m_selend_col  = 0;

    // ...and the user has selected five characters in the second.
    second->m_selstart_line = 0;  second->m_selstart_col = 0;
    second->m_selend_line   = 0;  second->m_selend_col   = 5;

    const pal::TextRange r = doc->m_pWorldView->outputSelection();

    // The first view wins, so the script is told there is no selection at all.
    EXPECT_EQ(0, r.startLine);
    EXPECT_EQ(0, r.endCol);

    // Non-vacuous: put the same selection on the FIRST view and it is seen,
    // which proves the zeros above are the view choice and not a dead reader.
    first->m_selend_col = 5;
    const pal::TextRange r2 = doc->m_pWorldView->outputSelection();
    EXPECT_EQ(1, r2.startLine);      // 1-based
    EXPECT_EQ(6, r2.endCol);         // endcol + 1

    // Teardown order matters.  RemoveView on the last view runs
    // OnChangedViewList -> OnCloseDocument, which deletes the document; so the
    // views are never RemoveView'd and the DOCUMENT goes first, whose
    // ~CDocument calls DisconnectViews to detach without deleting.  The views
    // are then ours to free, and ~CView is protected, hence the CObject cast.
    delete doc;
    delete (CObject*) first;
    delete (CObject*) second;
}
