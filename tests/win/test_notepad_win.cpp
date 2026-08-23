// tests/win/test_notepad_win.cpp — the world.*Notepad* family, over the REAL
// MFC doc/view notepad.
//
// tests/test_notepad_methods.cpp covers this family on the port, and cannot be
// compiled here: every one of its 18 tests drives CTextDocument's twelve
// m_fn_* std::function members, which TextDocument.h declares `#ifndef _WIN32`.
// That is not an accident of the fixture — it is the shape of the two
// implementations.  Off Windows a notepad is a wx text control behind twelve
// callbacks; on Windows it is a CTextView (a CEditView) inside a frame, reached
// by walking the document's view list.  scripting/methods/methods_notepad.cpp
// is `#ifndef _WIN32 / #else` eleven times over, and it is the `#else` half —
// the half that ships to every Windows user — that has never had a test.
//
// So this is not a port of that file.  The assertions are stronger here,
// because the observables are real:
//
//   port fixture                | here
//   ----------------------------|------------------------------------------
//   applyFontCalls counter      | the HFONT the EDIT control answers
//                               |   WM_GETFONT with, and its LOGFONT
//   applyColourCalls counter    | the frame caption CTextView::OnUpdate
//                               |   rewrites when UpdateAllViews reaches it
//   readOnly bool               | the control's ES_READONLY style bit
//   lastMove[4]                 | the frame's real screen rectangle
//   saveCalls / lastSavePath    | the bytes on disk
//   closeCalls                  | ::IsWindow(frame) going FALSE
//
// A counter can only fail when nothing was called.  Every row on the right
// fails additionally when the wrong thing was called, or when the right thing
// was called with the wrong argument.
//
// ── Why a console test process may do this at all ──────────────────────────
// Nothing here needs the application: a window is a kernel object, not a GUI
// mode, and tests/win/test_gap_coverage.cpp already creates a real WC_LISTVIEW
// the same way.  What the notepad needs beyond that is a CFrameWnd for
// GetParentFrame() to find — CTextView::OnUpdate dereferences it
// unconditionally (TextView.cpp) — and App.m_pNormalDocTemplate, which
// MC_TEST_WIN_DOC_TEMPLATES already builds.
//
// The one thing that does NOT work here is creation: CreateTextWindow goes
// through CMultiDocTemplate::OpenDocumentFile, whose CTextChildFrame is a
// CMDIChildWnd and needs the main frame's MDI client window — and this host
// constructs CMainFrame without ever creating it (mc_test.h
// MC_TEST_WIN_MAIN_FRAME).  So the two tests that would observe a creation
// instead observe the property that matters and that a creation cannot fake:
// that the call did not write into the notepad that already exists.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "TextDocument.h"
#include "TextView.h"
#include "scripting/errors.h"

#include <string>
#include <vector>
#include <algorithm>

// McTestDialogsCancelled: the WH_CBT hook in tests/win/all_tests.cpp counts
// every #32770 the process puts up.  SwitchToNotepad's whole Windows body is a
// modal chooser, so the count is the only evidence it ran.
long McTestDialogsCancelled();

namespace {

// Read a BSTR result as a std::string and free it, the way the dispatch layer
// does.  A CString would hide an empty-vs-null difference.
std::string bstr(BSTR b)
{
    CString s;
    s = b;
    std::string out((const char*) s);
    SysFreeString(b);
    return out;
}

// The titles the doc reports through GetNotepadList, sorted.
std::vector<std::string> list_titles(VARIANT v)
{
    std::vector<std::string> out;
    if (v.vt != VT_EMPTY && v.parray) {
        COleSafeArray sa;
        sa.Attach(v);
        const long n = sa.GetOneDimSize();
        for (long i = 0; i < n; i++) {
            VARIANT e; VariantInit(&e);
            sa.GetElement(&i, &e);
            CString s; s = e.bstrVal;
            out.push_back(std::string((const char*) s));
            VariantClear(&e);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ── The fixture: a real notepad ────────────────────────────────────────────
//
// Built in the order the application builds one, because the order is what
// makes it a faithful model rather than a lookalike:
//
//   1. the document, through DECLARE_DYNCREATE (the constructor is protected,
//      and CreateInstance — the port's way past it — is #ifndef _WIN32)
//   2. a real frame window for GetParentFrame() to return
//   3. the view, created as an EDIT child of that frame.  AddView comes AFTER
//      Create, not before: CView::OnCreate asserts m_pDocument == NULL
//   4. AddView, then AddDocument — with the identity fields FindNotepad
//      matches on (world AND document number) set first, because a fixture
//      that skips either is findable when the real thing would not be, and
//      every "not found" arm silently stops being tested
//
// Teardown is CTextDocument::OnCloseDocument, which is the same call
// world.CloseNotepad makes: it destroys the frame, which destroys the view,
// whose ~CView removes it from the document, which — being the last view —
// closes and deletes the document.  Nothing here deletes anything by hand;
// getting that wrong is how a fixture ends up testing its own bookkeeping.
struct WinNotepad {
    CTextDocument* doc;
    CTextView*     view;
    CFrameWnd*     frame;
    HWND           hFrame;    // outlives the objects, so closed() can ask

    WinNotepad(CMUSHclientDoc* world, const char* title, __int64 docnum)
        : doc(NULL), view(NULL), frame(NULL), hFrame(NULL)
    {
        doc = (CTextDocument*) RUNTIME_CLASS(CTextDocument)->CreateObject();
        if (!doc)
            return;

        doc->m_pRelatedWorld         = world;
        doc->m_iUniqueDocumentNumber = docnum;
        doc->m_strTitle              = title;
        doc->SetTitle(title);

        // Deliberately not square and not origin-anchored: the window-position
        // reader reformats left/top/right/bottom into left/top/WIDTH/HEIGHT,
        // and a square window at the origin makes that conversion an identity.
        frame = new CFrameWnd;
        if (!frame->Create(NULL, title, WS_OVERLAPPEDWINDOW,
                           CRect(120, 90, 520, 390), NULL, NULL, 0, NULL)) {
            delete frame;
            frame = NULL;
            return;
        }
        hFrame = frame->GetSafeHwnd();

        view = (CTextView*) RUNTIME_CLASS(CTextView)->CreateObject();
        if (!view)
            return;
        if (!view->Create(NULL, NULL, WS_CHILD | WS_VISIBLE,
                          CRect(0, 0, 380, 260), frame, AFX_IDW_PANE_FIRST, NULL)) {
            // ~CView is protected (the framework deletes a view from
            // PostNcDestroy, which never runs when there is no window).
            // CObject's destructor is public and virtual, so this is the same
            // deletion by a route the access specifier allows.
            delete (CObject*) view;
            view = NULL;
            return;
        }
        doc->AddView(view);
        frame->SetActiveView(view);

        if (App.m_pNormalDocTemplate)
            App.m_pNormalDocTemplate->AddDocument(doc);
    }

    ~WinNotepad()
    {
        if (doc && !closed())
            doc->OnCloseDocument();       // deletes the frame, view and document
        doc = NULL; view = NULL; frame = NULL;
    }

    bool ok() const { return doc && view && frame && ::IsWindow(hFrame) != FALSE; }

    // TRUE once the notepad has really gone — the assertion CloseNotepad earns.
    bool closed() const { return hFrame == NULL || ::IsWindow(hFrame) == FALSE; }

    std::string text() const
    {
        CString s;
        if (view) view->GetWindowText(s);
        return std::string((const char*) s);
    }

    void setText(const char* s)
    {
        if (view) view->GetEditCtrl().SetWindowText(s);
    }

    std::string caption() const
    {
        CString s;
        if (frame && ::IsWindow(hFrame)) frame->GetWindowText(s);
        return std::string((const char*) s);
    }

    // The HFONT the EDIT control is actually using.  NULL until something has
    // applied one, and a DIFFERENT handle each time SetTheFont runs — which is
    // what makes "it re-applied" observable without a counter.
    HFONT controlFont() const
    {
        if (!view || !::IsWindow(view->GetSafeHwnd())) return NULL;
        return (HFONT) view->SendMessage(WM_GETFONT, 0, 0);
    }

private:
    WinNotepad(const WinNotepad&);
    WinNotepad& operator=(const WinNotepad&);
};

// Every notepad the process still knows about, closed the way the app closes
// them.  A test that failed part-way can leave one behind, and GetNotepadList's
// answer is the whole document template.
void CloseAllNotepads()
{
    if (!App.m_pNormalDocTemplate)
        return;
    for (int guard = 0; guard < 64; ++guard) {
        POSITION pos = App.m_pNormalDocTemplate->GetFirstDocPosition();
        if (!pos)
            return;
        CTextDocument* d = (CTextDocument*) App.m_pNormalDocTemplate->GetNextDoc(pos);
        d->OnCloseDocument();
    }
}

int notepad_count()
{
    int n = 0;
    if (!App.m_pNormalDocTemplate)
        return 0;
    for (POSITION pos = App.m_pNormalDocTemplate->GetFirstDocPosition(); pos != NULL; ) {
        App.m_pNormalDocTemplate->GetNextDoc(pos);
        ++n;
    }
    return n;
}

// The point size SetTheFont turns into a LOGFONT height, computed the way
// CTextView::SetTheFont computes it rather than assumed to be 96 dpi — even
// though this binary pins EnableDpiAwareness=false exactly so that it is.
int logfont_height_for(int points)
{
    CDC dc;
    dc.CreateCompatibleDC(NULL);
    return -MulDiv(points, dc.GetDeviceCaps(LOGPIXELSY), 72);
}

std::string read_file(const char* path)
{
    std::string out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

}   // namespace

// ── Append / Replace / SendTo ───────────────────────────────────────────────

TEST(NotepadWin, AppendAddsAtTheEndAndReplaceOverwrites)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("first");

    EXPECT_TRUE(doc.AppendToNotepad("notes", " second"));
    EXPECT_EQ(std::string("first second"), pad.text());

    // Append must not be replace: a second append keeps the first.
    EXPECT_TRUE(doc.AppendToNotepad("notes", " third"));
    EXPECT_EQ(std::string("first second third"), pad.text());

    // ...and replace must not be append.
    EXPECT_TRUE(doc.ReplaceNotepad("notes", "wiped"));
    EXPECT_EQ(std::string("wiped"), pad.text());
}

TEST(NotepadWin, AppendToAnUnknownTitleDoesNotTouchAnExistingNotepad)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("untouched");

    // No notepad by this name, so the body falls through to CreateTextWindow.
    // Here that fails at the MDI child frame; what is asserted is the part that
    // is true on any Windows machine — it did not write into the notepad that
    // DOES exist.
    doc.AppendToNotepad("other", "should not appear");
    EXPECT_EQ(std::string("untouched"), pad.text());
}

TEST(NotepadWin, SendToNotepadCreatesRatherThanAppending)
{
    // methods_notepad.cpp SendToNotepad calls CreateTextWindow unconditionally
    // — it never looks for an existing window.  Two calls make two notepads; an
    // implementation that "helpfully" appended would make one growing one.
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "log", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("existing");

    doc.SendToNotepad("log", "brand new");

    EXPECT_EQ(std::string("existing"), pad.text());
}

// ── FindNotepad's identity match ────────────────────────────────────────────

TEST(NotepadWin, LookupIsScopedToTheOwningWorld)
{
    // Non-degenerate on purpose: both worlds own a notepad called "shared", so
    // a lookup that ignored m_pRelatedWorld would find the WRONG one rather
    // than find nothing.  That distinction is the whole point of the test.
    CloseAllNotepads();
    CMUSHclientDoc a, b;
    WinNotepad padA(&a, "shared", a.m_iUniqueDocumentNumber);
    WinNotepad padB(&b, "shared", b.m_iUniqueDocumentNumber);
    ASSERT_TRUE(padA.ok());
    ASSERT_TRUE(padB.ok());
    padA.setText("A");
    padB.setText("B");

    EXPECT_TRUE(a.AppendToNotepad("shared", "!"));
    EXPECT_EQ(std::string("A!"), padA.text());
    EXPECT_EQ(std::string("B"),  padB.text());   // the control

    EXPECT_TRUE(b.AppendToNotepad("shared", "?"));
    EXPECT_EQ(std::string("A!"), padA.text());
    EXPECT_EQ(std::string("B?"), padB.text());
}

TEST(NotepadWin, LookupIsCaseInsensitiveOnTheTitle)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "Notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("x");

    EXPECT_TRUE(doc.AppendToNotepad("NOTES", "y"));    // CompareNoCase
    EXPECT_EQ(std::string("xy"), pad.text());
}

// ── The readers ─────────────────────────────────────────────────────────────

TEST(NotepadWin, GetTextAndLengthReportTheWindowContents)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("hello");

    EXPECT_EQ(std::string("hello"), bstr(doc.GetNotepadText("notes")));
    EXPECT_EQ(5L, doc.GetNotepadLength("notes"));

    EXPECT_EQ(std::string(""), bstr(doc.GetNotepadText("nope")));
    EXPECT_EQ(0L, doc.GetNotepadLength("nope"));
}

TEST(NotepadWin, GetWindowPositionReportsLeftTopWidthHeight)
{
    // The BSTR contract is left,top,WIDTH,HEIGHT (windowPositionHelper), built
    // from GetWindowPlacement's rcNormalPosition, which is left,top,RIGHT,BOTTOM.
    // The expectation is computed from that same rectangle rather than written
    // as a literal, because the window manager — not this test — decides where
    // a frame ends up; what is under test is the conversion, and the fixture
    // frame is non-square so a missing conversion cannot pass.
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());

    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    ASSERT_TRUE(pad.frame->GetWindowPlacement(&wp) != FALSE);
    const RECT& r = wp.rcNormalPosition;
    ASSERT_NE(r.right - r.left, r.bottom - r.top);   // the fixture is not square

    CString expected;
    expected.Format("%ld,%ld,%ld,%ld", (long) r.left, (long) r.top,
                    (long) (r.right - r.left), (long) (r.bottom - r.top));
    EXPECT_EQ(std::string((const char*) expected),
              bstr(doc.GetNotepadWindowPosition("notes")));

    // ...and it must be reporting THIS window, not a constant: move it and the
    // answer has to follow.
    const std::string before = bstr(doc.GetNotepadWindowPosition("notes"));
    EXPECT_TRUE(doc.MoveNotepadWindow("notes", 200, 160, 460, 340));
    EXPECT_NE(before, bstr(doc.GetNotepadWindowPosition("notes")));

    EXPECT_EQ(std::string(""), bstr(doc.GetNotepadWindowPosition("nope")));
}

TEST(NotepadWin, LengthAndAppendAreByteOffsetsNotCharacterCounts)
{
    // GetNotepadText round-trips through a BSTR, and GetNotepadLength answers
    // GetWindowTextLength, which AppendToTheNotepad then feeds straight into
    // SetSel as the place to append.  All three have to agree in the same unit
    // or the append lands short of the true end.  On the port this contract was
    // broken once, in wx CHARACTER positions.
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("caf\xC3\xA9");        // "cafe-acute": 4 characters, 5 UTF-8 bytes

    const std::string text = bstr(doc.GetNotepadText("notes"));
    EXPECT_EQ(5u, (unsigned) text.size());
    EXPECT_EQ(5L, doc.GetNotepadLength("notes"));

    EXPECT_TRUE(doc.AppendToNotepad("notes", " latte"));
    EXPECT_EQ(std::string("caf\xC3\xA9 latte"), pad.text());
}

TEST(NotepadWin, GetNotepadListHonoursTheAllFlag)
{
    CloseAllNotepads();
    CMUSHclientDoc a, b;
    WinNotepad mine  (&a, "mine",   a.m_iUniqueDocumentNumber);
    WinNotepad theirs(&b, "theirs", b.m_iUniqueDocumentNumber);
    ASSERT_TRUE(mine.ok());
    ASSERT_TRUE(theirs.ok());

    std::vector<std::string> scoped = list_titles(a.GetNotepadList(FALSE));
    ASSERT_EQ(1u, (unsigned) scoped.size());
    EXPECT_EQ(std::string("mine"), scoped[0]);

    std::vector<std::string> all = list_titles(a.GetNotepadList(TRUE));
    ASSERT_EQ(2u, (unsigned) all.size());
    EXPECT_EQ(std::string("mine"),   all[0]);
    EXPECT_EQ(std::string("theirs"), all[1]);
}

TEST(NotepadWin, GetNotepadListIsEmptyWithNoNotepads)
{
    CloseAllNotepads();
    ASSERT_EQ(0, notepad_count());
    CMUSHclientDoc doc;
    VARIANT v = doc.GetNotepadList(TRUE);
    // "cannot create empty array dimension" — the body never calls CreateOneDim
    // on a zero count, so Detach hands back VT_EMPTY.
    EXPECT_EQ((int) VT_EMPTY, (int) v.vt);
    VariantClear(&v);
}

// ── The state setters ───────────────────────────────────────────────────────

TEST(NotepadWin, NotepadFontReachesTheEditControl)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.doc->m_strFontName = "Courier New";
    pad.doc->m_iFontSize   = 10;

    EXPECT_TRUE(doc.NotepadFont("notes", "Lucida Console", 14, 1 /* bold */, 0));
    EXPECT_EQ(std::string("Lucida Console"),
              std::string((const char*) pad.doc->m_strFontName));
    EXPECT_EQ(14L, (long) pad.doc->m_iFontSize);
    EXPECT_EQ((long) FW_BOLD, (long) pad.doc->m_iFontWeight);

    // The state is only half of it: the control has to be wearing the font.
    ASSERT_TRUE(pad.view->m_font != NULL);
    HFONT applied = pad.controlFont();
    EXPECT_TRUE(applied != NULL);
    EXPECT_EQ((void*) pad.view->m_font->m_hObject, (void*) applied);

    LOGFONT lf;
    memset(&lf, 0, sizeof lf);
    ASSERT_TRUE(pad.view->m_font->GetLogFont(&lf) != 0);
    EXPECT_STREQ("Lucida Console", lf.lfFaceName);
    EXPECT_EQ((long) FW_BOLD, (long) lf.lfWeight);
    EXPECT_EQ((long) logfont_height_for(14), (long) lf.lfHeight);

    // An empty name means KEEP, and a size <= 0 means KEEP
    // (methods_notepad.cpp).  Without these the test would pass for a body that
    // assigned unconditionally.  It must still RE-APPLY, which is observable
    // because SetTheFont builds a new CFont every time.
    EXPECT_TRUE(doc.NotepadFont("notes", "", 0, 1, 0));
    EXPECT_EQ(std::string("Lucida Console"),
              std::string((const char*) pad.doc->m_strFontName));
    EXPECT_EQ(14L, (long) pad.doc->m_iFontSize);
    EXPECT_NE((void*) applied, (void*) pad.controlFont());
    EXPECT_EQ((void*) pad.view->m_font->m_hObject, (void*) pad.controlFont());
}

TEST(NotepadWin, NotepadColourWritesBothColoursAndRefusesBadNames)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());

    // NotepadColour's only visible effect beyond the two members is
    // UpdateAllViews, and CTextView::OnUpdate rewrites the frame caption from
    // the document title.  Poisoning the caption first makes "it repainted"
    // observable without a counter.
    pad.frame->SetWindowText("stale");
    EXPECT_TRUE(doc.NotepadColour("notes", "red", "white"));
    EXPECT_EQ((long) RGB(255, 0, 0),     (long) pad.doc->m_textColour);
    EXPECT_EQ((long) RGB(255, 255, 255), (long) pad.doc->m_backColour);
    EXPECT_EQ(std::string("notes"), pad.caption());

    // A name that is not a colour must be refused, and must not half-apply.
    pad.frame->SetWindowText("stale");
    EXPECT_FALSE(doc.NotepadColour("notes", "notacolour", "white"));
    EXPECT_EQ((long) RGB(255, 0, 0), (long) pad.doc->m_textColour);
    EXPECT_EQ(std::string("stale"), pad.caption());   // nothing repainted
}

TEST(NotepadWin, NotepadReadOnlyReachesTheControlBothWays)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    ASSERT_EQ(0UL, (unsigned long) (pad.view->GetStyle() & ES_READONLY));

    EXPECT_TRUE(doc.NotepadReadOnly("notes", TRUE));
    EXPECT_TRUE(pad.doc->m_bReadOnly != FALSE);
    EXPECT_NE(0UL, (unsigned long) (pad.view->GetStyle() & ES_READONLY));

    // Both directions — a setter that only ever sets true passes a one-way test.
    EXPECT_TRUE(doc.NotepadReadOnly("notes", FALSE));
    EXPECT_TRUE(pad.doc->m_bReadOnly == FALSE);
    EXPECT_EQ(0UL, (unsigned long) (pad.view->GetStyle() & ES_READONLY));
}

TEST(NotepadWin, MoveWindowMovesTheRealFrame)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());

    // Comfortably above any minimum tracking size, so the window manager does
    // not clamp the answer and turn a real result into a flaky one.
    EXPECT_TRUE(doc.MoveNotepadWindow("notes", 210, 170, 480, 360));
    CRect r;
    pad.frame->GetWindowRect(&r);
    EXPECT_EQ(210L, (long) r.left);
    EXPECT_EQ(170L, (long) r.top);
    EXPECT_EQ(480L, (long) r.Width());
    EXPECT_EQ(360L, (long) r.Height());

    EXPECT_FALSE(doc.MoveNotepadWindow("nope", 10, 20, 300, 200));
    CRect after;
    pad.frame->GetWindowRect(&after);
    EXPECT_EQ(210L, (long) after.left);   // the miss must not move anything
    EXPECT_EQ(170L, (long) after.top);
}

TEST(NotepadWin, ActivateShowsTheFrameAndMissesCleanly)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    ASSERT_TRUE(pad.frame->IsWindowVisible() == FALSE);

    // The miss first, while the window is still hidden: it must not activate
    // some other notepad, and there is only one to get wrong.
    EXPECT_FALSE(doc.ActivateNotepad("nope"));
    EXPECT_TRUE(pad.frame->IsWindowVisible() == FALSE);

    EXPECT_TRUE(doc.ActivateNotepad("notes"));
    EXPECT_TRUE(pad.frame->IsWindowVisible() != FALSE);
    EXPECT_EQ((void*) pad.view, (void*) pad.frame->GetActiveView());
}

TEST(NotepadWin, SwitchToNotepadOffersTheChooserOnlyWhenThereIsOne)
{
    // The entire Windows body is a modal CChooseNotepadDlg, so the WH_CBT
    // canceller in tests/win/all_tests.cpp is what makes it testable at all:
    // the dialog appears, is closed, DoModal answers something other than IDOK,
    // and the body returns true meaning "they gave up".  With no notepad for
    // this world the dialog is never reached and the answer is false.
    CloseAllNotepads();
    CMUSHclientDoc doc;

    const long before = McTestDialogsCancelled();
    EXPECT_FALSE(doc.SwitchToNotepad());
    EXPECT_EQ(before, McTestDialogsCancelled());   // no notepad, no dialog

    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());

    EXPECT_TRUE(doc.SwitchToNotepad());
    EXPECT_GT(McTestDialogsCancelled(), before);
}

TEST(NotepadWin, SaveWritesTheBytesAndReportsFailure)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    pad.setText("saved contents");

    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_notepad_%d.txt", (int) ::GetCurrentProcessId());
    ::DeleteFileA(path);
    ASSERT_TRUE(pad.doc->GetPathName().IsEmpty());

    EXPECT_TRUE(doc.SaveNotepad("notes", path, TRUE));
    EXPECT_EQ(std::string("saved contents"), read_file(path));

    // bReplace TRUE means "this is now the document's file" — but NOT under the
    // name that was passed.  CDocument::SetPathName runs AfxFullPath, so the
    // stored name is the canonical absolute one ("C:\tmp\..." for a "/tmp/..."
    // argument).  Asserting the string that went in would be asserting that it
    // does not, so this asserts the property that matters: the document now
    // points at a file, and that file is the one just written.
    ASSERT_FALSE(pad.doc->GetPathName().IsEmpty());
    EXPECT_EQ(std::string("saved contents"),
              read_file((const char*) pad.doc->GetPathName()));

    // A save that cannot happen must be reported as failure, not swallowed.
    // (CDocument::OnSaveDocument reports it with a message box, which the
    // host's AfxMessageBox hook routes to stderr.)
    EXPECT_FALSE(doc.SaveNotepad("notes", "/tmp/mc_no_such_dir_here/x.txt", FALSE));

    EXPECT_FALSE(doc.SaveNotepad("nope", path, FALSE));

    ::DeleteFileA(path);
}

TEST(NotepadWin, CloseDestroysTheFrameAndTheDocument)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());
    ASSERT_EQ(1, notepad_count());

    EXPECT_FALSE(doc.CloseNotepad("nope", FALSE));
    EXPECT_TRUE(pad.closed() == false);
    EXPECT_EQ(1, notepad_count());

    EXPECT_TRUE(doc.CloseNotepad("notes", FALSE));
    EXPECT_TRUE(pad.closed());          // the window really went
    EXPECT_EQ(0, notepad_count());      // ...and so did the document
}

TEST(NotepadWin, SaveMethodSelectsThePolicy)
{
    CloseAllNotepads();
    CMUSHclientDoc doc;
    WinNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    ASSERT_TRUE(pad.ok());

    EXPECT_TRUE(doc.NotepadSaveMethod("notes", eNotepadSaveAlways));
    EXPECT_EQ((int) eNotepadSaveAlways, pad.doc->m_iSaveOnChange);

    EXPECT_TRUE(doc.NotepadSaveMethod("notes", eNotepadSaveNever));
    EXPECT_EQ((int) eNotepadSaveNever, pad.doc->m_iSaveOnChange);

    EXPECT_FALSE(doc.NotepadSaveMethod("notes", 99));    // bad selector
    EXPECT_EQ((int) eNotepadSaveNever, pad.doc->m_iSaveOnChange);

    EXPECT_FALSE(doc.NotepadSaveMethod("nope", eNotepadSaveAlways));
}
