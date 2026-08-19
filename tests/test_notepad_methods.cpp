// tests/test_notepad_methods.cpp
//
// The world.*Notepad* method family (scripting/methods/methods_notepad.cpp).
//
// Until 2026-08-06 this family had ZERO coverage anywhere.  The headless suite
// exercised a twin in platform/doc_gui_method_stubs.cpp that forwarded to a
// g_wxhook_*_notepad table, and --notepad-selftest exercises CTextDocument's
// save/convert/quit paths but never calls a single CMUSHclientDoc::*Notepad*
// method.  The twin had drifted in several observable ways, which is what a
// family with no tests does; the drifts are named on the tests that pin them.
//
// The real TU is compiled into mushcore now, so everything below drives the
// shipping bodies.  tests/notepad_fixture.h supplies a real CTextDocument whose
// twelve m_fn_* callbacks are lambdas over a std::string — the same seam
// CWxNotepadFrame fills in the product.
//
// Two fixture properties are deliberate and load-bearing, per the recorded
// "degenerate fixture hides a mapping" note:
//   * the rect is 100,50,400,250 — right != width and top != 0 — because
//     GetNotepadWindowPosition converts l/t/r/b into l/t/w/h, and a square
//     origin-anchored rect would make that conversion an identity.
//   * the two-world tests give BOTH worlds a notepad with the SAME title, so a
//     lookup that ignores the world still finds something and the test still
//     fails, rather than finding nothing and passing for the wrong reason.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "TextDocument.h"
#include "scripting/errors.h"
#include "notepad_fixture.h"

#include <string>

// Read a BSTR result as a std::string and free it, the way the dispatch layer
// does.  Returning CString would hide an empty-vs-null difference.
static std::string bstr(BSTR b)
{
    CString s;
    s = b;
    std::string out((const char*) s);
    SysFreeString(b);
    return out;
}

// The titles the doc reports through GetNotepadList, as a sorted vector.
static std::vector<std::string> list_titles(VARIANT v)
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

// ── Append / Replace / SendTo ───────────────────────────────────────────────

TEST(NotepadMethods, AppendAddsAtTheEndAndReplaceOverwrites)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    pad.text = "first";

    EXPECT_TRUE(doc.AppendToNotepad("notes", " second"));
    EXPECT_EQ(std::string("first second"), pad.text);

    // Append must not be replace: a second append keeps the first.
    EXPECT_TRUE(doc.AppendToNotepad("notes", " third"));
    EXPECT_EQ(std::string("first second third"), pad.text);

    // …and replace must not be append.
    EXPECT_TRUE(doc.ReplaceNotepad("notes", "wiped"));
    EXPECT_EQ(std::string("wiped"), pad.text);
}

TEST(NotepadMethods, AppendToAnUnknownTitleDoesNotTouchAnExistingNotepad)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    pad.text = "untouched";

    // No notepad by this name: the real body falls through to CreateTextWindow,
    // which returns false with no window hook installed.  What matters for
    // correctness is that it did not write into the notepad that DOES exist.
    doc.AppendToNotepad("other", "should not appear");
    EXPECT_EQ(std::string("untouched"), pad.text);
}

TEST(NotepadMethods, SendToNotepadCreatesRatherThanAppending)
{
    // The drift this pins: the twin forwarded SendToNotepad to the append path
    // under a comment calling it "the portable equivalent".  It is not —
    // methods_notepad.cpp:181 never looks for an existing window, it always
    // creates one.  Two calls make two windows on Windows and made ONE
    // ever-growing notepad headless.
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "log", doc.m_iUniqueDocumentNumber);
    pad.text = "existing";

    doc.SendToNotepad("log", "brand new");

    // It must NOT have appended to the existing notepad of the same name.
    EXPECT_EQ(std::string("existing"), pad.text);
}

// ── FindNotepad's identity match ────────────────────────────────────────────

TEST(NotepadMethods, LookupIsScopedToTheOwningWorld)
{
    // Non-degenerate on purpose: both worlds own a notepad called "shared", so
    // a lookup that ignores m_pRelatedWorld finds the WRONG one rather than
    // finding nothing.  That distinction is the whole point of the test.
    ClearAllNotepads();
    CMUSHclientDoc a, b;
    FakeNotepad padA(&a, "shared", a.m_iUniqueDocumentNumber);
    FakeNotepad padB(&b, "shared", b.m_iUniqueDocumentNumber);
    padA.text = "A";
    padB.text = "B";

    EXPECT_TRUE(a.AppendToNotepad("shared", "!"));
    EXPECT_EQ(std::string("A!"), padA.text);
    EXPECT_EQ(std::string("B"),  padB.text);   // the control

    EXPECT_TRUE(b.AppendToNotepad("shared", "?"));
    EXPECT_EQ(std::string("A!"), padA.text);
    EXPECT_EQ(std::string("B?"), padB.text);
}

TEST(NotepadMethods, LookupIsCaseInsensitiveOnTheTitle)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "Notes", doc.m_iUniqueDocumentNumber);
    pad.text = "x";
    // CompareNoCase (methods_notepad.cpp FindNotepad).
    EXPECT_TRUE(doc.AppendToNotepad("NOTES", "y"));
    EXPECT_EQ(std::string("xy"), pad.text);
}

// ── The readers ─────────────────────────────────────────────────────────────

TEST(NotepadMethods, GetTextAndLengthReportTheWindowContents)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    pad.text = "hello";

    EXPECT_EQ(std::string("hello"), bstr(doc.GetNotepadText("notes")));
    EXPECT_EQ(5L, doc.GetNotepadLength("notes"));

    // The not-found arms, which are what the stub could only ever return.
    EXPECT_EQ(std::string(""), bstr(doc.GetNotepadText("nope")));
    EXPECT_EQ(0L, doc.GetNotepadLength("nope"));
}

TEST(NotepadMethods, GetWindowPositionReportsLeftTopWidthHeight)
{
    // The fixture rect is "100 50 400 250" (l t r b).  The documented BSTR
    // contract is left,top,WIDTH,HEIGHT — so 100,50,300,200.  Because right !=
    // width and bottom != height in this fixture, a body that forgot the
    // conversion produces 100,50,400,250 and fails.
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_EQ(std::string("100,50,300,200"),
              bstr(doc.GetNotepadWindowPosition("notes")));
    EXPECT_EQ(std::string(""), bstr(doc.GetNotepadWindowPosition("nope")));
}

TEST(NotepadMethods, LengthAndAppendAreByteOffsetsNotCharacterCounts)
{
    // GetNotepadText returns UTF-8 BYTES (it round-trips through a BSTR/
    // CString), so GetNotepadLength and AppendToTheNotepad's own end-of-text
    // seek (methods_notepad.cpp:117-121, m_fn_get_text_length feeding straight
    // into m_fn_set_sel) must agree with it in bytes, not characters — the
    // product's real seam (platform/wx_notepad.cpp) used to answer in wx
    // CHARACTER positions instead, which is invisible to this fixture (its
    // m_fn_get_text_length is already byte-based) but is exactly the contract
    // this pins: a body that counted characters here would report 4 for
    // "café" and land the append one byte short of the true end.
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    pad.text = "caf\xC3\xA9";   // "café": 4 characters, 5 UTF-8 bytes

    const std::string text = bstr(doc.GetNotepadText("notes"));
    EXPECT_EQ(5u, text.size());
    EXPECT_EQ(strlen(text.c_str()), (size_t) doc.GetNotepadLength("notes"));
    EXPECT_EQ(5L, doc.GetNotepadLength("notes"));

    EXPECT_TRUE(doc.AppendToNotepad("notes", " latte"));
    EXPECT_EQ(std::string("caf\xC3\xA9 latte"), pad.text);
}

TEST(NotepadMethods, GetNotepadListHonoursTheAllFlag)
{
    // The drift this pins: the twin discarded `All` entirely and always
    // answered app-wide.  World-scoping is the parameter's whole purpose
    // (methods_notepad.cpp:512-513).
    ClearAllNotepads();
    CMUSHclientDoc a, b;
    FakeNotepad mine (&a, "mine",  a.m_iUniqueDocumentNumber);
    FakeNotepad theirs(&b, "theirs", b.m_iUniqueDocumentNumber);

    auto scoped = list_titles(a.GetNotepadList(FALSE));
    ASSERT_EQ(1u, (unsigned) scoped.size());
    EXPECT_EQ(std::string("mine"), scoped[0]);

    auto all = list_titles(a.GetNotepadList(TRUE));
    ASSERT_EQ(2u, (unsigned) all.size());
    EXPECT_EQ(std::string("mine"),   all[0]);
    EXPECT_EQ(std::string("theirs"), all[1]);
}

TEST(NotepadMethods, GetNotepadListIsEmptyWithNoNotepads)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    VARIANT v = doc.GetNotepadList(TRUE);
    // "cannot create empty array dimension" — the real body never calls
    // CreateOneDim on a zero count, so Detach hands back VT_EMPTY.
    EXPECT_EQ((int) VT_EMPTY, (int) v.vt);
    VariantClear(&v);
}

// ── The state setters ───────────────────────────────────────────────────────

TEST(NotepadMethods, NotepadFontWritesTheStateAndReappliesIt)
{
    // The drift this pins: the twin forwarded to a hook and wrote NONE of this
    // state, losing the documented keep-current semantics along with it.
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);
    pad.doc->m_strFontName = "Courier";
    pad.doc->m_iFontSize   = 10;

    EXPECT_TRUE(doc.NotepadFont("notes", "Menlo", 14, 0, 0));
    EXPECT_EQ(std::string("Menlo"), std::string((const char*) pad.doc->m_strFontName));
    EXPECT_EQ(14L, (long) pad.doc->m_iFontSize);
    EXPECT_GT(pad.applyFontCalls, 0);   // "wrote the member but never repainted" fails

    // An empty name means KEEP, and size <= 0 means KEEP
    // (methods_notepad.cpp:456-470).  Without these two the test would pass for
    // a body that assigns unconditionally.
    const int before = pad.applyFontCalls;
    EXPECT_TRUE(doc.NotepadFont("notes", "", 0, 0, 0));
    EXPECT_EQ(std::string("Menlo"), std::string((const char*) pad.doc->m_strFontName));
    EXPECT_EQ(14L, (long) pad.doc->m_iFontSize);
    EXPECT_GT(pad.applyFontCalls, before);
}

TEST(NotepadMethods, NotepadColourWritesBothColoursAndRefusesBadNames)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_TRUE(doc.NotepadColour("notes", "red", "white"));
    EXPECT_EQ((long) RGB(255, 0, 0),       (long) pad.doc->m_textColour);
    EXPECT_EQ((long) RGB(255, 255, 255),   (long) pad.doc->m_backColour);
    EXPECT_GT(pad.applyColourCalls, 0);

    // A name that is not a colour must be refused, and must not half-apply.
    const int calls = pad.applyColourCalls;
    EXPECT_FALSE(doc.NotepadColour("notes", "notacolour", "white"));
    EXPECT_EQ((long) RGB(255, 0, 0), (long) pad.doc->m_textColour);
    EXPECT_EQ(calls, pad.applyColourCalls);
}

TEST(NotepadMethods, NotepadReadOnlyWritesTheStateBothWays)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_TRUE(doc.NotepadReadOnly("notes", TRUE));
    EXPECT_TRUE(pad.readOnly);
    EXPECT_TRUE(pad.doc->m_bReadOnly);

    // Both directions — a setter that only ever sets true passes a one-way test.
    EXPECT_TRUE(doc.NotepadReadOnly("notes", FALSE));
    EXPECT_FALSE(pad.readOnly);
    EXPECT_FALSE(pad.doc->m_bReadOnly);
}

TEST(NotepadMethods, MoveWindowPassesTheRectangleThrough)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_TRUE(doc.MoveNotepadWindow("notes", 11, 22, 33, 44));
    EXPECT_EQ(1, pad.moveCalls);
    EXPECT_EQ(11, pad.lastMove[0]);
    EXPECT_EQ(22, pad.lastMove[1]);
    EXPECT_EQ(33, pad.lastMove[2]);
    EXPECT_EQ(44, pad.lastMove[3]);

    EXPECT_FALSE(doc.MoveNotepadWindow("nope", 1, 2, 3, 4));
    EXPECT_EQ(1, pad.moveCalls);   // control: the miss must not move anything
}

TEST(NotepadMethods, ActivateFindsTheRightWindowAndMissesCleanly)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_TRUE(doc.ActivateNotepad("notes"));
    EXPECT_EQ(1, pad.activateCalls);

    EXPECT_FALSE(doc.ActivateNotepad("nope"));
    EXPECT_EQ(1, pad.activateCalls);
}

TEST(NotepadMethods, SaveReportsTheBackendResult)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_TRUE(doc.SaveNotepad("notes", "/tmp/x.txt", TRUE));
    EXPECT_EQ(1, pad.saveCalls);
    EXPECT_EQ(std::string("/tmp/x.txt"), pad.lastSavePath);

    // A failing save must be reported as failure, not swallowed.
    pad.saveSucceeds = false;
    EXPECT_FALSE(doc.SaveNotepad("notes", "/tmp/x.txt", TRUE));

    pad.saveSucceeds = true;
    EXPECT_FALSE(doc.SaveNotepad("nope", "/tmp/x.txt", TRUE));
}

TEST(NotepadMethods, CloseClosesTheWindowAndMissesCleanly)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_FALSE(doc.CloseNotepad("nope", FALSE));
    EXPECT_EQ(0, pad.closeCalls);

    EXPECT_TRUE(doc.CloseNotepad("notes", FALSE));
    EXPECT_EQ(1, pad.closeCalls);
}

TEST(NotepadMethods, SaveMethodSelectsThePolicy)
{
    ClearAllNotepads();
    CMUSHclientDoc doc;
    FakeNotepad pad(&doc, "notes", doc.m_iUniqueDocumentNumber);

    EXPECT_TRUE(doc.NotepadSaveMethod("notes", eNotepadSaveAlways));
    EXPECT_EQ((int) eNotepadSaveAlways, pad.doc->m_iSaveOnChange);

    EXPECT_TRUE(doc.NotepadSaveMethod("notes", eNotepadSaveNever));
    EXPECT_EQ((int) eNotepadSaveNever, pad.doc->m_iSaveOnChange);

    // The stub returned FALSE unconditionally, so the miss arm is the ONLY
    // behaviour it ever had — keep it pinned.
    EXPECT_FALSE(doc.NotepadSaveMethod("nope", eNotepadSaveAlways));
}

MC_TEST_MAIN()
