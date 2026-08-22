// tests/win/test_gap_coverage.cpp — engine behaviour whose only test lives in
// a suite the Windows target excludes.
//
// Every fix in docs/WINDOWS-VALIDATION.md §10h's "leak and use-after-free" list
// is in shared engine code, so all of it runs on Windows — but four of the
// seven are covered by tests in suites that DO compile here (test_regexp,
// test_database, test_mxp) and are therefore already verified, while the rest
// are covered only in `test_world_load` and `test_lua_scripting`, which are
// excluded for reasons that have nothing to do with the fix under test.
//
// So the coverage gap is an artefact of where the assertion happens to live,
// not of what the Windows build can reach.  This file closes it by driving the
// same engine code through an entry point that is available here.
//
// It is Windows-only and under tests/win/ for one reason: on macOS these fixes
// already have tests, and a second copy would be two places to keep in step.
// If one of the excluded suites is ever re-entered, the corresponding test here
// becomes redundant and should go.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "OtherTypes.h"
#include "scripting/errors.h"   // eOK / eCouldNotOpenFile

#include "MUSHview.h"       // CMUSHView: the click-column tests below
#include "posix_compat.h"   // getpid / unlink, for the names-file tests

#include <crtdbg.h>
#include <string>

namespace {

// Live debug-CRT block count, the same instrument tests/test_mxp.cpp uses.
size_t live_blocks()
{
    _CrtMemState st;
    _CrtMemCheckpoint(&st);
    size_t n = 0;
    for (int i = 0; i < _MAX_BLOCKS; ++i)
        n += (size_t) st.lCounts[i];
    return n;
}

}   // namespace

// ── c1a3990: a rejected duplicate label left a FREED pointer in the map ────
//
// The overwrite arm deletes the old object, and the regexp compile a few lines
// later throws for a bad pattern; the loader reports that and carries on, so
// anything still keyed to the deleted object — SortTriggers, or the
// destructor's DELETE_MAP — was working on freed memory.
//
// tests/test_world_load.cpp asserts this through LoadWorldFromFile, which is
// port-only (doc.h declares it #ifndef _WIN32).  world.ImportXML reaches the
// SAME loader: methods_xml.cpp wraps the text in a CArchive and calls
// Load_World_XML with XML_OVERWRITE set, which is exactly the arm that deletes.
TEST(ImportXMLR5, DuplicateLabelWhoseReplacementFailsLeavesNoFreedEntry)
{
    {   // triggers
        const char* xml =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE muclient>\n"
            "<muclient><triggers>\n"
            "  <trigger match=\"a\" name=\"keep\" sequence=\"100\"><send>y</send></trigger>\n"
            "  <trigger match=\"b\" name=\"dup\"  sequence=\"100\"><send>y</send></trigger>\n"
            "  <trigger match=\"(unclosed\" regexp=\"y\" name=\"dup\" sequence=\"100\">"
            "<send>y</send></trigger>\n"
            "</triggers></muclient>";

        CMUSHclientDoc doc;
        doc.ImportXML(xml);          // the count is not the subject; the map is

        // The replacement was refused, and the original went with it.
        EXPECT_EQ(1, (int) doc.GetTriggerMap().GetCount());
        CTrigger* t = NULL;
        EXPECT_FALSE(doc.GetTriggerMap().Lookup("dup", t));
        ASSERT_TRUE(doc.GetTriggerMap().Lookup("keep", t));
        EXPECT_STREQ("a", (const char*) t->trigger);

        doc.SortTriggers();          // must not walk a freed entry
    }

    {   // aliases — identical shape
        const char* xml =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE muclient>\n"
            "<muclient><aliases>\n"
            "  <alias match=\"a\" name=\"keep\" sequence=\"100\"><send>y</send></alias>\n"
            "  <alias match=\"b\" name=\"dup\"  sequence=\"100\"><send>y</send></alias>\n"
            "  <alias match=\"(unclosed\" regexp=\"y\" name=\"dup\" sequence=\"100\">"
            "<send>y</send></alias>\n"
            "</aliases></muclient>";

        CMUSHclientDoc doc;
        doc.ImportXML(xml);

        EXPECT_EQ(1, (int) doc.GetAliasMap().GetCount());
        CAlias* a = NULL;
        EXPECT_FALSE(doc.GetAliasMap().Lookup("dup", a));
        ASSERT_TRUE(doc.GetAliasMap().Lookup("keep", a));
        EXPECT_STREQ("a", (const char*) a->name);

        doc.SortAliases();           // must not walk a freed entry
    }
}

// ── fbc4161: ImportXML leaked the CArchiveException on a bad import ────────
//
// The catch arm is three lines above the return in methods_xml.cpp, and MFC
// exceptions are thrown by pointer: without the e->Delete() the object stays
// allocated for the life of the process, once per malformed import.  A script
// that retries on failure leaks once per retry.
//
// tests/test_lua_scripting.cpp asserts this through Lua; here it is the C++
// method directly, which is the same code either way.
TEST(ImportXMLR5, BadImportReportsMinusOneAndLeaksNothing)
{
    const char* const bad = "<?xml version=\"1.0\"?><muclient><triggers><trigger";

    {   // Not XML at all: IsArchiveXML says no, so this returns -1 without
        // throwing.  The control for the arm below.
        CMUSHclientDoc doc;
        EXPECT_EQ(-1L, doc.ImportXML("this is not xml"));
        EXPECT_EQ(-1L, doc.ImportXML(bad));      // and the throwing arm reports it too
    }

    // The document is constructed INSIDE the loop and destroyed each time, and
    // that is the whole design of this test.  A failed import is not silent:
    // LoadError writes "[WARNING] <file>" and the detail into the world's
    // scrollback (ColourNote), so a doc that survives the loop accumulates two
    // CLine objects, their CStyles and the list plexes per call -- measured at
    // 12.3 blocks each, steady, which looks exactly like a per-call leak and is
    // not one.  Destroying the document takes its output with it, so anything
    // still allocated afterwards is allocated OUTSIDE the document: the
    // CErrorException the parser throws, or the CArchiveException ImportXML
    // catches, which is what fbc4161 was about.
    {   // warm: first use allocates the parser's tables and the note styles
        CMUSHclientDoc warm;
        warm.ImportXML(bad);
    }

    const size_t before = live_blocks();
    for (int i = 0; i < 20; ++i)
    {
        CMUSHclientDoc doc;
        EXPECT_EQ(-1L, doc.ImportXML(bad));
    }
    const size_t after = live_blocks();

    // One escaped exception per call would be 20 blocks and then some, since
    // CErrorException carries a CString.  Compared with slack rather than
    // exactly: a document's construction and destruction is not required to be
    // perfectly block-neutral, and this test is about the exception, not that.
    EXPECT_TRUE(after < before + 20);
}

// ── 3c58886: the document constructor leaked a BSTR, once per document ─────
//
// doc_construct.cpp's `m_strWorldID = GetUniqueID()` resolved to the
// AUTOMATION member rather than the file-scope helper — the three sibling
// callers all write `::GetUniqueID()` — and the automation one returns a BSTR
// the caller owns.  Present since the 2010 import (docs/UPSTREAM-BUGS.md 19).
//
// This one has no test on either platform, which is why it is here rather than
// in the list above: it is new coverage, not a Windows copy of existing
// coverage, and it would be worth moving to a shared suite.
TEST(DocCtorR5, ConstructingAndDestroyingDocumentsDoesNotGrowTheHeap)
{
    // Warm up: the first document pulls in a lot that is then reused.
    { CMUSHclientDoc warm; (void) warm.m_strWorldID.GetLength(); }

    const size_t before = live_blocks();
    for (int i = 0; i < 25; ++i)
    {
        CMUSHclientDoc doc;
        EXPECT_TRUE(doc.m_strWorldID.GetLength() > 0);   // it really was set
    }
    const size_t after = live_blocks();

    // A BSTR per document would be 25 blocks.  Allow a little slack for
    // anything the engine legitimately caches on first use, but far less than
    // one-per-iteration, which is the shape of the defect.
    EXPECT_TRUE(after < before + 25);
}

MC_TEST_MAIN()

// ═══ The names file (9139076 / docs/UPSTREAM-BUGS.md 42) ═══════════════════
//
// ReadNames checked that each of [start], [middle], [end] and [stop] was
// PRESENT but not that anything was collected between them, so a file whose
// markers are adjacent set bNamesRead = true with an empty CStringArray.
// GenerateName then evaluated `rand () % FirstNames.GetSize ()` -- a division
// by zero -- and subscripted the empty array.  world.ReadNamesFile answered
// eOK for the file that caused it, which is what made the crash arrive later
// and somewhere else.
//
// This is here rather than in tests/test_utilities.cpp because it is one of
// the few things the Windows target can test that the headless one cannot at
// all: NameGeneration.cpp is a mushgui_adapters TU, so the macOS test
// executables link platform/windows_stubs.cpp's ReadNames instead -- and that
// one always throws, so a test written against it would pass with and without
// the fix.  9139076's own commit message asked for exactly this run.

// Defined in NameGeneration.cpp and declared in no header; the doc's
// GenerateName reads it to decide whether the arrays are safe to index, which
// makes it the flag the whole defect turns on.
extern bool bNamesRead;

namespace {

// Write a names file and hand back its path.  Sections are passed verbatim, so
// a caller makes one empty simply by passing "".
std::string write_names_file(const char* tag, const char* starts,
                             const char* middles, const char* ends)
{
    static int s_seq = 0;
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_names_%d_%d_%s.txt",
             (int)getpid(), ++s_seq, tag);
    FILE* f = fopen(path, "wb");
    if (!f) return "";
    fprintf(f, "/* a names file */\n[start]\n%s[middle]\n%s[end]\n%s[stop]\n",
            starts, middles, ends);
    fclose(f);
    return path;
}

}   // namespace

// The control, and it comes first: a well-formed file really is accepted and
// really does generate a name, so the refusals below cannot be passing because
// ReadNames rejects everything handed to it.
TEST(NamesFileR5, WellFormedFileIsAcceptedAndGeneratesAName)
{
    CMUSHclientDoc doc;
    std::string p = write_names_file("ok", "Ael\nBor\n", "an\nir\n", "dun\nwyn\n");
    ASSERT_TRUE(!p.empty());

    EXPECT_EQ((long)eOK, doc.ReadNamesFile(p.c_str()));
    EXPECT_TRUE(bNamesRead);

    // Every generated name is one entry from each of the three sections, in
    // order -- so assert the SHAPE rather than a literal, which would be a
    // one-in-eight coin toss.  Twenty draws also means an off-by-one in the
    // rand() % size indexing would have to be lucky twenty times running.
    const char* firsts[] = { "Ael", "Bor" };
    const char* mids[]   = { "an",  "ir"  };
    const char* lasts[]  = { "dun", "wyn" };
    for (int i = 0; i < 20; ++i) {
        VARIANT v = doc.GenerateName();
        ASSERT_EQ((int)VT_BSTR, (int)v.vt);
        CString name = v.bstrVal;
        EXPECT_TRUE(name.GetLength() > 0);
        bool ok = false;
        for (int a = 0; a < 2 && !ok; a++)
          for (int b = 0; b < 2 && !ok; b++)
            for (int c = 0; c < 2 && !ok; c++)
              ok = (name == CString(firsts[a]) + mids[b] + lasts[c]);
        EXPECT_TRUE(ok);
    }
    unlink(p.c_str());
}

// The defect itself, once per section, because the fix added one check per
// section and a single case would leave two of them unexercised.
//
// Each iteration asserts both halves: the file is refused (eCouldNotOpenFile,
// which is what world.ReadNamesFile turns the exception into) AND bNamesRead
// is left false.  The second is the crash's actual precondition -- everything
// downstream only touches the arrays when that flag says they are good.
TEST(NamesFileR5, AnEmptySectionIsRefusedAndLeavesNamesUnread)
{
    CMUSHclientDoc doc;

    struct { const char* tag; const char* s; const char* m; const char* e; }
    cases[] = {
        { "nostart",  "",      "an\n",  "dun\n" },
        { "nomiddle", "Ael\n", "",      "dun\n" },
        { "noend",    "Ael\n", "an\n",  ""      },
    };

    for (int i = 0; i < 3; ++i) {
        // Start each case from a known-good read, so "bNamesRead is false"
        // below is something THIS file did, not something inherited from an
        // earlier test in the process.
        std::string good = write_names_file("seed", "Ael\n", "an\n", "dun\n");
        ASSERT_EQ((long)eOK, doc.ReadNamesFile(good.c_str()));
        ASSERT_TRUE(bNamesRead);
        unlink(good.c_str());

        std::string p = write_names_file(cases[i].tag, cases[i].s,
                                         cases[i].m, cases[i].e);
        ASSERT_TRUE(!p.empty());

        EXPECT_EQ((long)eCouldNotOpenFile, doc.ReadNamesFile(p.c_str()));
        EXPECT_FALSE(bNamesRead);
        unlink(p.c_str());
    }
}

// ...and the consequence.  Before the fix the bad file above answered eOK and
// set bNamesRead, so the very next world.GenerateName ran
// `rand () % FirstNames.GetSize ()` over an empty array.  The doc method's own
// `if (!bNamesRead) return VT_NULL` guard is not the fix and never was: with
// the flag wrongly true it does not fire.
//
// The ASSERT below stops the test before GenerateName when the flag is wrong,
// deliberately.  Measured by reverting the three GetSize checks: this test and
// the one above both go red on the PRECONDITION -- eOK where
// eCouldNotOpenFile belongs, and bNamesRead true -- while the control above
// stays green.  That is the whole defect, and it is a better place to stop
// than one instruction further on, where the answer is a fault rather than a
// failure.
TEST(NamesFileR5, GenerateNameAfterABadFileAnswersNullInsteadOfDividingByZero)
{
    CMUSHclientDoc doc;
    std::string p = write_names_file("bad", "", "an\n", "dun\n");
    ASSERT_TRUE(!p.empty());

    EXPECT_EQ((long)eCouldNotOpenFile, doc.ReadNamesFile(p.c_str()));
    ASSERT_FALSE(bNamesRead);

    VARIANT v = doc.GenerateName();
    EXPECT_EQ((int)VT_NULL, (int)v.vt);
    EXPECT_FALSE(bNamesRead);

    unlink(p.c_str());
}

// ═══ LoadWorldFromFile, the host's world-load entry ════════════════════════
//
// The Windows framework opens a world through CDocument::OnOpenDocument, which
// wants a window this host does not have, so doc.h gives the test host its own
// LoadWorldFromFile under _WIN32 && MUSHCLIENT_TESTS (defined in
// tests/win/all_tests.cpp).  It is a driver, not a loader: the body it drives
// is CMUSHclientDoc::Serialize's loading branch in serialize.cpp, which IS the
// Windows world loader.
//
// This test exists because that entry point is what unblocks
// tests/test_world_load.cpp (105 uses) and tests/test_lua_scripting.cpp, and
// those two are not enabled yet -- see their reasons in
// tests/win/gen_test_vcxproj.ps1 and docs/WINDOWS-VALIDATION.md 10y.  Without
// it the host entry would be code with no caller, which is exactly the shape
// that rots.
//
// It also earns its keep alone: it is the only thing here that drives
// Serialize -> IsArchiveXML -> Serialize_World_XML -> Load_World_XML end to
// end against real MFC and a real CArchive.
TEST(WorldLoadHost, RealSerializeLoadsAWorldFileEndToEnd)
{
    const char* xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "<world site=\"example.org\" port=\"4321\" name=\"HostLoadWorld\" />\n"
        "<triggers>\n"
        "  <trigger match=\"you have died\" name=\"death\" enabled=\"y\""
        " send_to=\"0\" sequence=\"100\">\n"
        "    <send>say I died!</send>\n"
        "  </trigger>\n"
        "</triggers>\n"
        "</muclient>\n";

    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_hostworld_%d.mcl", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fwrite(xml, 1, strlen(xml), f);
    fclose(f);

    CMUSHclientDoc doc;
    ASSERT_TRUE(doc.LoadWorldFromFile(path));

    // The world header reached the document...
    EXPECT_STREQ("HostLoadWorld", (const char*) doc.m_mush_name);
    EXPECT_STREQ("example.org",   (const char*) doc.m_server);
    EXPECT_EQ(4321, (int) doc.m_port);

    // ...and so did a child collection, which is the half that proves
    // Load_World_XML ran rather than just the header being parsed.
    CTrigger* t = nullptr;
    ASSERT_TRUE(doc.GetTriggerMap().Lookup("death", t));
    ASSERT_NE(nullptr, t);
    EXPECT_STREQ("you have died", (const char*) t->trigger);

    // SetPathName is Serialize's, not the driver's: assert it, because a
    // driver that skipped it would still pass everything above and would then
    // break %WORLDFILEDIR% and Save for every test built on this entry point.
    EXPECT_TRUE(CString(doc.GetPathName()).Find("mc_test_hostworld") != -1);

    unlink(path);
}

// The other half of the contract: a file that is not a MUSHclient world is
// REFUSED rather than half-loaded.  Serialize throws a badSchema CArchive
// exception for a missing XML signature, and the driver turns that into FALSE.
// Without this the ASSERT_TRUE above would be satisfied by a driver that
// swallowed every error and always answered true.
TEST(WorldLoadHost, ANonWorldFileIsRefused)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_notaworld_%d.txt", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("this is not a world file\n", f);
    fclose(f);

    CMUSHclientDoc doc;
    EXPECT_FALSE(doc.LoadWorldFromFile(path));
    EXPECT_STREQ("", (const char*) doc.m_mush_name);

    unlink(path);
}



// ═══ The two UTF-8 click fix-ups — REFUTED (docs/UPSTREAM-BUGS.md 86) ══════
//
// `CMUSHView::calculate_line_and_column` corrects a click that lands
// mid-character twice: the fix-up after `col--` walks FORWARD to the next
// character's lead byte, and the half-way arm below it walks BACKWARD.  That
// asymmetry was read as a defect and recorded as one, "not fixed; needs a
// Windows run", because CMUSHView is never constructed on the port.
//
// It is not a defect.  The two walks compose, and this is the run that shows it.
//
// The loop measures the boundary AT `col` and then advances `col` past the
// character there, so on exit `col` is the boundary after the character the
// click landed in and `col--` is "that boundary, minus one BYTE".  For a
// single-byte character that is already the next character's lead byte; for a
// multi-byte one it is a continuation byte, and the forward walk is what
// normalises it to the same thing.  The half-way arm then steps back from
// "next character" to "this character", and its backward walk normalises THAT.
// Each walk fixes the byte the step before it produced, in the direction that
// step went.
//
// Measured, "AeB" with the middle character U+00E9 (bytes A=0, lead=1,
// continuation=2, B=3), against the single-byte controls either side:
//
//   bHalfWay = false   click in A -> 1     click in e -> 3     click in B -> 4
//   bHalfWay = true    left half of e -> 1 (the character itself)
//                      right half of e -> 3 (the one after it)
//
// Both rows are the same rule for one-byte and two-byte characters, which is
// the only thing the asymmetry could have broken.  The half-way row is also
// exactly what a drag selection wants: anchor before the character when the
// click is in its left half, after it when the right.
//
// Changing the forward walk to `col--` -- the "single token" the bug entry
// proposed -- was tried here and measured: it moves the left-half answer from 1
// to 0, so a selection started in the left half of an accented character would
// begin one character too far left.  The entry has been rewritten as refuted
// and these tests are what keep it that way.
//
// Three things make testing this possible without a GUI, and each one unlocks
// the other "needs a view" claims in this campaign:
//
//   * CMUSHView's constructor is twenty member assignments (mushview.cpp:275)
//     and is `protected: // create from serialization only`, like every MFC
//     doc/view class.  RUNTIME_CLASS(...)->CreateObject() is the framework's
//     own way past that, and it is public.
//   * CClientDC given a NULL CWnd* is the SCREEN DC.  No window required.
//   * ~CDocument calls DisconnectViews, which nulls each view's back-pointer
//     and empties the list without deleting anything.  RemoveView would be the
//     tidy call and is the WRONG one: removing the LAST view runs
//     OnCloseDocument, which for an auto-delete document is `delete this`.
//
// So the document is destroyed first and the view second, which is why both are
// pointers rather than members with an implicit destruction order.
namespace {

struct ClickFixture {
    CMUSHView*      view;
    CMUSHclientDoc* doc;

    ClickFixture()
      : view((CMUSHView*) RUNTIME_CLASS(CMUSHView)->CreateObject()),
        doc(new CMUSHclientDoc)
    {}
    ~ClickFixture() { delete doc; delete view; }   // doc FIRST — see above

    bool build(const char* utf8)
    {
        if (!view || !doc)
            return false;
        doc->m_bUTF_8 = TRUE;
        doc->m_iConnectPhase = eConnectConnectedToMud;   // or the bytes go nowhere
        // Fixed pitch, so a cell boundary is arithmetic rather than a guess.
        doc->ChangeFont(10, "Courier New", FW_NORMAL, ANSI_CHARSET,
                        FALSE, FALSE, FALSE, 0);
        if (doc->m_font[0] == NULL || doc->m_FontHeight <= 0)
            return false;

        char buf[64];
        strncpy(buf, utf8, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        const int n = (int) strlen(buf);
        doc->OnBytesReceived(buf, n);
        doc->AddView(view);
        // The line has to have arrived AND carry style runs, because
        // calculate_width measures per run and answers 0 for a line with none.
        if (doc->m_LineList.GetCount() == 0)
            return false;
        return line0()->len == n && line0()->styleList.GetCount() > 0;
    }

    CLine* line0() { return doc->m_LineList.GetAt(doc->GetLinePosition(0)); }

    // The x a click must carry to land `w` pixels into the text, in the
    // coordinates calculate_line_and_column subtracts m_TextRectangle from.
    long clickX(long w)
    {
        return doc->m_TextRectangle.left + doc->m_iPixelOffset
             + line0()->m_iPreambleOffset + w;
    }
    long clickY() { return doc->m_TextRectangle.top; }

    int resolve(long w, bool halfway)
    {
        CClientDC dc(NULL);          // the screen DC -- no window required
        int line = -1, col = -1;
        view->calculate_line_and_column(CPoint(clickX(w), clickY()), dc,
                                        line, col, halfway);
        return col;
    }
};

}   // namespace

// bHalfWay = false is the arm menus and tooltips use.  Its answer is the
// boundary AFTER the character the click landed in, and the point of the test is
// that a two-byte character gets the same treatment as a one-byte one -- which
// is precisely what the forward walk is for.
TEST(ClickColumnR6, TheNoHalfWayArmTreatsAMultiByteCharacterLikeAnyOther)
{
    ClickFixture f;
    ASSERT_TRUE(f.build("A\xC3\xA9" "B"));

    CClientDC dc(NULL);
    const long wA   = f.view->calculate_width(0, 1, f.doc, dc);   // "A"
    const long wAe  = f.view->calculate_width(0, 3, f.doc, dc);   // "A" + the char
    const long wAeB = f.view->calculate_width(0, 4, f.doc, dc);   // the whole line
    ASSERT_TRUE(wA > 0);
    ASSERT_TRUE(wAe > wA);
    ASSERT_TRUE(wAeB > wAe);

    // The controls come FIRST, because they are what establish the rule the
    // multi-byte case has to follow.  Neither is a lucky number: 'A' is byte 0
    // and answers 1, 'B' is byte 3 and answers 4.
    EXPECT_EQ(1, f.resolve(wA / 2, false));                  // in 'A'
    EXPECT_EQ(4, f.resolve((wAe + wAeB) / 2, false));         // in 'B'

    // ...and the two-byte character in between answers 3 -- the byte after it,
    // not the continuation byte 2 that `col--` alone leaves behind.
    const int col = f.resolve((wA + wAe) / 2, false);
    if (col != 3)
        printf("  click inside the two-byte character resolved to byte %d"
               " (its lead byte is 1, its continuation is 2, the next"
               " character starts at 3)\n", col);
    EXPECT_NE(2, col);          // never a continuation byte
    EXPECT_EQ(3, col);
}

// bHalfWay = true is the selection arm, and its contract is the comment above
// it: "if we are 50% through this character, take the next one, otherwise take
// the previous one".  Left half of the accented character anchors at the
// character itself, right half at the one after -- the same rule the
// single-byte controls follow, and never a byte inside a character.
TEST(ClickColumnR6, TheHalfWayArmAnchorsOnCharacterBoundaries)
{
    ClickFixture f;
    ASSERT_TRUE(f.build("A\xC3\xA9" "B"));

    CClientDC dc(NULL);
    const long wA  = f.view->calculate_width(0, 1, f.doc, dc);
    const long wAe = f.view->calculate_width(0, 3, f.doc, dc);
    ASSERT_TRUE(wAe > wA);

    // Controls: the single-byte character before it behaves the same way.
    EXPECT_EQ(0, f.resolve(wA / 4, true));            // left half of 'A'
    EXPECT_EQ(1, f.resolve(wA - wA / 4, true));       // right half of 'A'

    const long quarter = (wAe - wA) / 4;
    const int colLeft  = f.resolve(wA + quarter, true);
    const int colRight = f.resolve(wAe - quarter, true);

    printf("  half-way arm: left half -> %d, right half -> %d\n",
           colLeft, colRight);

    EXPECT_NE(2, colLeft);      // never a continuation byte
    EXPECT_NE(2, colRight);
    EXPECT_EQ(1, colLeft);      // the character itself
    EXPECT_EQ(3, colRight);     // the one after it
}
