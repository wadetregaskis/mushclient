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
#include "dialogs/plugins/PluginsDlg.h"  // CPluginsDlg -- the stale-CPlugin* row
#include "dialogs/world_prefs/prefspropertypages.h"  // FittedLastColumnWidth,
                                                 // OutputBufferBytesNeeded, CPrefsP7/8
#include "posix_compat.h"   // getpid / unlink, for the names-file tests
#include "fake_worldview.h"  // the swappable pal::IWorldView — GetInfo 249/250
#include "chatsock.h"        // CChatSocket — the file-block test at the end
#include "flags.h"           // eEnabled / eReplace — the replace-while-running test

#include <commctrl.h>   // a real WC_LISTVIEW for the Plugins dialog test

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

// ═══ aca2478: the Plugins dialog's stale CPlugin* ══════════════════════════
//
// The last row in docs/WINDOWS-VALIDATION.md §10h with no test on EITHER
// platform.  CPluginsDlg::OnUpdateNeedDescription pulled the selected row's
// CPlugin* straight off the list control's item data with no
// find()-in-m_PluginList liveness check, unlike all five sibling handlers.  A
// plugin removed from the list while the dialog stays open without LoadList()
// rebuilding the rows -- world.UnloadPlugin or ReloadPlugin from a timer firing
// during the dialog's modal loop -- leaves that row's item data pointing at a
// CPlugin the idle-driven update-UI pump then reads.
//
// The port covers it in platform/gui_bridge_selftest.cpp by pointing the real
// dialog at a wxListCtrl.  The same shape works here with a REAL Win32 list
// view, and that is the whole reason this is a unit test rather than the GUI
// walk §10h asked for: the hazard needs a plugin to vanish from m_PluginList
// between LoadList() and the next idle, which no sequence of clicks arranges --
// the dialog's own Remove button calls LoadList() afterwards.
//
// A console process may create windows; nothing about this needs the app.  The
// list is a plain WC_LISTVIEW child of a hidden STATIC, Attach()ed to the
// dialog's CListCtrl member and Detach()ed before it goes away, so MFC's handle
// map never outlives the window.
namespace {

// A hidden parent + a report-mode list view, torn down in the right order.
struct ListCtrlFixture {
    HWND parent;
    HWND list;

    ListCtrlFixture() : parent(NULL), list(NULL)
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof icc;
        icc.dwICC  = ICC_LISTVIEW_CLASSES;
        ::InitCommonControlsEx(&icc);

        parent = ::CreateWindowExA(0, "STATIC", "mc_test_parent",
                                   WS_POPUP, 0, 0, 500, 400,
                                   NULL, NULL, ::GetModuleHandle(NULL), NULL);
        if (!parent) return;
        list = ::CreateWindowExA(0, WC_LISTVIEWA, "",
                                 WS_CHILD | LVS_REPORT, 0, 0, 480, 360,
                                 parent, NULL, ::GetModuleHandle(NULL), NULL);
    }
    ~ListCtrlFixture()
    {
        if (list)   ::DestroyWindow(list);
        if (parent) ::DestroyWindow(parent);
    }
    bool ok() const { return parent != NULL && list != NULL; }
};

// The port's version of this test uses AfxDispatchUpdateUI and CCmdUI's
// m_pCaptureEnable.  Neither exists in real MFC -- both are platform/mfc_ui.h's
// additions -- so capture the answer the way the framework actually offers:
// CCmdUI::Enable is virtual, and CCmdUI::DoUpdate is the public entry the idle
// pump itself goes through.  Recording whether Enable was called AT ALL is also
// strictly more than a captured bool can say.
struct CaptureCmdUI : public CCmdUI {
    bool enabled;
    bool sawEnable;
    CaptureCmdUI() : enabled(false), sawEnable(false) {}
    virtual void Enable(BOOL bOn)
    {
        enabled   = (bOn != FALSE);
        sawEnable = true;
        m_bEnableChanged = TRUE;
    }
};

// A plugin object with just the fields LoadList and the handler read.
CPlugin* make_plugin(CMUSHclientDoc& doc, const char* name, const char* id,
                     const char* description)
{
    CPlugin* p = new CPlugin(&doc);
    p->m_strName        = name;
    p->m_strID          = id;
    p->m_strDescription = description;
    p->m_strAuthor      = "test";
    p->m_strPurpose     = "fixture";
    p->m_strLanguage    = "lua";
    p->m_dVersion       = 1.0;
    p->m_bEnabled       = TRUE;
    return p;
}

}   // namespace

TEST(PluginsDlgR6, ShowInfoIsDisabledOnceThePluginHasLeftTheList)
{
    ListCtrlFixture ctl;
    ASSERT_TRUE(ctl.ok());

    CMUSHclientDoc doc;
    CPlugin* live   = make_plugin(doc, "AAALive",   "aaaaaaaaaaaaaaaaaaaaaaaa",
                                  "The live plugin's long description.");
    CPlugin* erased = make_plugin(doc, "ZZZErased", "bbbbbbbbbbbbbbbbbbbbbbbb",
                                  "The erased plugin's long description.");
    doc.m_PluginList.push_back(live);
    doc.m_PluginList.push_back(erased);

    CPluginsDlg dlg;
    dlg.m_pDoc = &doc;
    dlg.m_ctlPluginList.Attach(ctl.list);

    // LoadList only fills rows; the columns a report-mode list needs to hold
    // them come from OnInitDialog, which this test skips (it also reads and
    // writes the user's real column-width preferences).
    dlg.m_ctlPluginList.InsertColumn(eColumnName,     "Name",     LVCFMT_LEFT,  130);
    dlg.m_ctlPluginList.InsertColumn(eColumnPurpose,  "Purpose",  LVCFMT_LEFT,  130);
    dlg.m_ctlPluginList.InsertColumn(eColumnAuthor,   "Author",   LVCFMT_LEFT,   55);
    dlg.m_ctlPluginList.InsertColumn(eColumnLanguage, "Language", LVCFMT_LEFT,   55);
    dlg.m_ctlPluginList.InsertColumn(eColumnFile,     "File",     LVCFMT_LEFT,   80);
    dlg.m_ctlPluginList.InsertColumn(eColumnEnabled,  "Enabled",  LVCFMT_LEFT,   30);
    dlg.m_ctlPluginList.InsertColumn(eColumnVersion,  "Ver",      LVCFMT_RIGHT,  20);

    dlg.LoadList();     // sorts by name: row 0 = AAALive, row 1 = ZZZErased
    ASSERT_EQ(2, dlg.m_ctlPluginList.GetItemCount());

    // The hazard: the plugin leaves m_PluginList without LoadList() rebuilding
    // the rows, so row 1's item data still points at it.  It is kept ALIVE, so
    // the test itself never touches freed memory -- only the liveness check
    // under test would.
    PluginListIterator pit = find(doc.m_PluginList.begin(),
                                  doc.m_PluginList.end(), erased);
    ASSERT_TRUE(pit != doc.m_PluginList.end());
    doc.m_PluginList.erase(pit);

    // The live row first: without this control a handler that simply disabled
    // Show Info always would pass the assertion that matters.
    dlg.m_ctlPluginList.SetItemState(-1, 0, LVIS_SELECTED);
    dlg.m_ctlPluginList.SetItemState(0, LVIS_SELECTED, LVIS_SELECTED);
    ASSERT_EQ(1u, (unsigned) dlg.m_ctlPluginList.GetSelectedCount());

    CaptureCmdUI live_ui;
    live_ui.m_nID = IDC_SHOW_DESCRIPTION;
    EXPECT_TRUE(live_ui.DoUpdate(&dlg, FALSE) != FALSE);
    EXPECT_TRUE(live_ui.sawEnable);
    EXPECT_TRUE(live_ui.enabled);

    // ...and the erased row, whose item data is stale.
    dlg.m_ctlPluginList.SetItemState(-1, 0, LVIS_SELECTED);
    dlg.m_ctlPluginList.SetItemState(1, LVIS_SELECTED, LVIS_SELECTED);
    EXPECT_EQ((DWORD_PTR) erased, dlg.m_ctlPluginList.GetItemData(1));
    ASSERT_EQ(1u, (unsigned) dlg.m_ctlPluginList.GetSelectedCount());

    CaptureCmdUI stale_ui;
    stale_ui.m_nID   = IDC_SHOW_DESCRIPTION;
    stale_ui.enabled = true;              // the opposite of the expected answer
    EXPECT_TRUE(stale_ui.DoUpdate(&dlg, FALSE) != FALSE);
    EXPECT_TRUE(stale_ui.sawEnable);      // the handler ran; it did not just skip
    EXPECT_FALSE(stale_ui.enabled);       // was: read the stale item data unchecked

    dlg.m_ctlPluginList.Detach();   // never let MFC's map outlive the window
    delete erased;                  // m_PluginList no longer owns it
}

// ═══ 44f0737's two hoisted helpers, which Windows could not reach ══════════
//
// FittedLastColumnWidth and OutputBufferBytesNeeded are pure arithmetic in
// dialogs/world_prefs/prefspropertypages.h, and the comment above them says why
// they are there: "in the header so the headless suite can drive them without
// linking a property page ... Both were bugs precisely because they were buried
// in a dialog nothing tests."
//
// They are tested — in tests/test_mfc_ui.cpp, which this target EXCLUDES, for a
// reason that has nothing to do with them ("tests platform/mfc_ui.h, the 108
// fake MFC classes").  So two functions in a shipping header, hoisted expressly
// to be testable, had no coverage on the platform they ship for.  That is the
// same artefact §10h describes and this file exists to close, and it is the
// cheapest instance of it yet: no fixture, no window, no document.
//
// If test_mfc_ui.cpp is ever split so its shim half and its engine half can be
// excluded separately, these two become redundant and should go.

TEST(PrefsHelpersR6, TheMemoryWarningSurvivesTheRaisedLineCeiling)
{
    // Windows' own ceiling: comfortably inside 32 bits either way.
    EXPECT_EQ(16ULL * 1024 * 1024 + 500000ULL * 60,
              OutputBufferBytesNeeded(500000L));

    // The port's ceiling is ~6.02 Gb.  The wrapped DWORD figure was ~1.72 Gb,
    // so a 4 Gb machine was told the biggest possible buffer would fit.  The
    // ceiling is the port's, but the arithmetic is in shared code and is what
    // this target can check.
    const unsigned long long need = OutputBufferBytesNeeded(100000000L);
    EXPECT_EQ(16ULL * 1024 * 1024 + 100000000ULL * 60, need);
    EXPECT_TRUE(need > 4ULL * 1024 * 1024 * 1024);

    // 0 lines is "unlimited" and has its own warning; no per-line bytes here.
    EXPECT_EQ(16ULL * 1024 * 1024, OutputBufferBytesNeeded(0L));
}

TEST(PrefsHelpersR6, TheLastColumnIsFittedOnlyWhenTheUserHasNoWidthOfTheirOwn)
{
    // Seed still in place = the user has never resized this column, so fit it:
    // 600 wide, less a 15-pixel scrollbar, less 160 of other columns.
    EXPECT_EQ(425, FittedLastColumnWidth(150, 150, 600, 15, 160));

    // A restored width that differs is the user's own: leave it alone.
    EXPECT_EQ(0, FittedLastColumnWidth(150, 320, 600, 15, 160));

    // A list too narrow for the other columns is not a reason to set a silly
    // width — 0 means "leave the column alone" here too.
    EXPECT_EQ(0, FittedLastColumnWidth(150, 150, 100, 15, 160));
    EXPECT_EQ(0, FittedLastColumnWidth(150, 150, 175, 15, 160));
}

// ── 44f0737's eighth defect, which has no runtime observable at all ────────
//
// LoadPrefsP7 indexed the ALIAS page's column arrays with the TRIGGER page's
// CPrefsP8::eColumnSequence.  Two independent enums that happen to agree, so
// the fix corrected an expression without changing a value, and no test can
// watch it happen.  What CAN be pinned is the coincidence the old code rested
// on: if either enum is ever reordered, this stops compiling, which is exactly
// the notice the original code never got.
//
// Deliberately a static_assert rather than an EXPECT: the hazard is at edit
// time, not run time, and a failing build is the only report that arrives
// before the damage.
static_assert((int) CPrefsP7::eColumnSequence == (int) CPrefsP8::eColumnSequence,
              "The alias and trigger pages' column enums have diverged. "
              "That is legal -- they are independent -- but LoadPrefsP7 in "
              "dialogs/world_prefs/configuration.cpp once indexed one with the "
              "other, and this assertion is what would have caught it.");

// ── GetInfo(249)/(250): the main window's client rect ──────────────────────
//
// `tests/test_info.cpp`'s version of this is `#ifndef _WIN32`, because the
// GetInfo codes around it dereference `Frame`.  These do not, and the
// behaviour they pin is a WINDOWS regression, so they belong here.
//
// The seam could not say three things, only two.  `mainClientRect ()` answered
// `Rect{0,0,0,0}` both for "there is no main window" and for "there is one and
// its client area measures nothing", and `GetMainClientRect` read the empty
// rect as the former.  A MINIMISED main frame is the latter — `GetClientRect`
// on it really does report 0 x 0 — so `GetInfo(249)/(250)` fell through to
// `(m_gui_rows + 2) * m_gui_font_height` and `m_gui_cols * m_gui_font_width`.
// Those four members are written only by code under `platform/`, which the
// Windows project does not compile, so on Windows they are permanently
// 24/80/18/10 and the fallback is a fabricated 468 x 800.  Upstream had no
// fallback at all: `Frame.GetClientRect (&rect); ... rect.bottom`.
//
// Measured in the running client before the fix, one timer reading and asking
// to minimise and a second reading after the message loop had run:
//
//     restored   h=931  w=1616
//     minimised  h=468  w=800     <- the fabrication
//
// `pal::IWorldView::hasMainWindow ()` is the third state, and these tests are
// the three answers.
namespace {

// A CMUSHclientDoc whose m_pWorldView can be swapped, with the real adapter
// put back afterwards -- the doc creates its own in the constructor and its
// destructor expects it.
struct InfoDoc {
    CMUSHclientDoc doc;
    InfoDoc()
    {
        doc.m_gui_rows        = 25;
        doc.m_gui_cols        = 80;
        doc.m_gui_font_height = 10;
        doc.m_gui_font_width  = 6;
    }
};

}   // namespace

TEST(MainClientRectR6, TheRealAdapterFallsBackWhenTheFrameHasNoWindow)
{
    // The host constructs CMainFrame and never Create()s it (mc_test.h
    // MC_TEST_WIN_MAIN_FRAME), so pal/win_worldview.cpp's frame() answers NULL
    // and hasMainWindow() is false.  That is the genuine no-window case and it
    // must still fall back — this is the arm the fix had to leave alone.
    InfoDoc f;
    ASSERT_TRUE(f.doc.m_pWorldView != NULL);
    EXPECT_FALSE(f.doc.m_pWorldView->hasMainWindow());

    int x = 1, y = 1, w = 1, h = 1;
    EXPECT_FALSE(f.doc.GetMainClientRect(x, y, w, h));
    EXPECT_EQ(1, x); EXPECT_EQ(1, y);          // outputs left alone
    EXPECT_EQ(1, w); EXPECT_EQ(1, h);

    EXPECT_EQ(27L * 10L, (long) f.doc.GetInfo(249).lVal);
    EXPECT_EQ(80L * 6L,  (long) f.doc.GetInfo(250).lVal);
}

TEST(MainClientRectR6, AWindowThatMeasuresSomethingIsReportedAsMeasured)
{
    InfoDoc f;
    FakeWorldView view;
    view.hasMainWin  = true;
    view.mainClientR = pal::Rect{ 0, 0, 793, 517 };
    mc_test::ScopedWorldView<CMUSHclientDoc, FakeWorldView> attach(f.doc, &view);

    EXPECT_EQ(517L, (long) f.doc.GetInfo(249).lVal);
    EXPECT_EQ(793L, (long) f.doc.GetInfo(250).lVal);

    // Deliberately not a multiple of the character grid, so neither number can
    // be the fallback wearing a disguise.
    EXPECT_NE((long) f.doc.GetInfo(250).lVal, 80L * 6L);
}

TEST(MainClientRectR6, AMinimisedWindowReportsZeroRatherThanAFabricatedSize)
{
    // The regression, in one flag.  Both halves have an empty rect; they differ
    // only in whether there is a window behind it, and they must answer
    // differently.  Before the fix the second block answered 270 and 480.
    InfoDoc f;
    FakeWorldView view;
    view.mainClientR = pal::Rect{ 0, 0, 0, 0 };
    mc_test::ScopedWorldView<CMUSHclientDoc, FakeWorldView> attach(f.doc, &view);

    view.hasMainWin = false;                    // no window at all: fall back
    EXPECT_EQ(27L * 10L, (long) f.doc.GetInfo(249).lVal);
    EXPECT_EQ(80L * 6L,  (long) f.doc.GetInfo(250).lVal);

    view.hasMainWin = true;                     // minimised: report what it is
    EXPECT_EQ(0L, (long) f.doc.GetInfo(249).lVal);
    EXPECT_EQ(0L, (long) f.doc.GetInfo(250).lVal);
}



// ── A short chat file block must not write bytes that never arrived ────────
//
// `CChatSocket::Process_File_block` writes `expected` bytes into the receiving
// file, where `expected` is the block size agreed at the handshake — or the
// remainder, for the final block. Both numbers come from the PEER: the block
// size from its handshake, `m_iFileSize` from its file-start message. The bytes
// actually present are `strMessage`, and on the **zChat** transport the two need
// not agree: that arm of `OnReceive` reads the block length out of two bytes of
// the peer's own message header and hands over exactly that many, with no
// comparison against the block size. The **MudMaster** arm cannot produce a
// short block — it returns until `m_iFileBlockSize + 2` bytes have arrived,
// which is precisely the check the zChat arm lacks.
//
// So a peer sending a one-byte `CHAT_FILE_BLOCK` caused
// `m_pFile->Write (pBuff, 1024)` out of a one-byte buffer: 1023 bytes past the
// end of a heap allocation, written into the user's file and fed to the SHA
// sumcheck alongside. `docs/UPSTREAM-BUGS.md` 96.
//
// Driven directly rather than over a socket — `tests/test_chat_sockets.cpp` is
// excluded here because it builds AF_UNIX pairs, and every member this setup
// needs is public. Verified non-vacuous: with the guard temporarily replaced by
// `if (false)`, all three assertions below fail.
TEST(ChatFileBlockR6, AShortBlockIsRefusedRatherThanPaddedFromTheHeap)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_chatblk_%d.bin", (int) ::GetCurrentProcessId());
    ::DeleteFileA(path);

    CMUSHclientDoc doc;
    CChatSocket sock(&doc);

    // On the HEAP: StopFileTransfer does `delete m_pFile`, so a stack CFile here
    // is a delete of a stack object.  The debug heap said so immediately, which
    // is how this fixture got caught.
    CFile* f = new CFile;
    ASSERT_TRUE(f->Open(path, CFile::modeWrite | CFile::modeCreate) != FALSE);

    // Mid-transfer state: four 1024-byte blocks promised, none received yet.
    sock.m_bDoingFileTransfer = true;
    sock.m_bSendFile          = false;
    sock.m_pFile              = f;
    sock.m_iFileBlockSize     = 1024;
    sock.m_iFileSize          = 4096;
    sock.m_iFileBlocks        = 4;
    sock.m_iBlocksTransferred = 0;
    sock.m_strOurFileName     = path;

    // One byte, where 1024 was promised.
    sock.Process_File_block(CString("x"));

    // The abort path closes the file, nulls the pointer and REMOVES the
    // half-received file, so "the file is gone" is the whole assertion.
    EXPECT_TRUE(sock.m_pFile == NULL);
    EXPECT_FALSE(sock.m_bDoingFileTransfer);

    // The file's existence, not its size.  On the failing path it is still
    // open, and a second handle cannot read the size of a CFile opened
    // shareCompat -- the first version of this reported "0 bytes" for a file
    // that had just had 1024 written into it, which is exactly the sort of
    // number not to put in a test's output.
    const DWORD attrs = ::GetFileAttributesA(path);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        printf("      (the half-received file survived: the transfer carried on"
               " with bytes that never arrived)\n");
        ::DeleteFileA(path);
    }
    EXPECT_TRUE(attrs == INVALID_FILE_ATTRIBUTES);
}

// ── A second CHAT_FILE_START must not walk over the live transfer ──────────
//
// `Process_File_start` had no in-progress guard: its only gates were
// `m_bCanSendFiles`, the argument count, the slash check, and whether the user
// wanted the file. A second `CHAT_FILE_START` arriving mid-transfer therefore
// ran straight to `m_pFile = new CFile (...)`, **leaking the open CFile** and
// orphaning the half-received file it was writing.
//
// And the failure arm of that same `new` did `delete m_pFile;` without nulling
// it. Harmless on its own — `m_bDoingFileTransfer` is false, so
// `Process_File_block` and `StopFileTransfer` both return early — but combined
// with the missing guard it is a **write through freed memory**: transfer in
// progress (so the flag is true), second FILE_START whose open fails, and the
// next `CHAT_FILE_BLOCK` writes to the deleted `CFile`.
//
// And it bites even when the second transfer is REFUSED: `m_iFileSize = atol
// (strSize)` runs before the accept prompt, so a second offer the user says no
// to still overwrites the running transfer.s size -- which feeds
// `m_iFileBlocks` and the final-block arithmetic. That is what this test
// measures, and with the guard replaced by `if (false)` it reads 2048 where it
// should read 4096.
//
// `docs/UPSTREAM-BUGS.md` 98. Driven directly, like the block test above.
TEST(ChatFileBlockR6, ASecondFileStartIsRefusedWhileATransferIsRunning)
{
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_chatstart_%d.bin", (int) ::GetCurrentProcessId());
    ::DeleteFileA(path);

    CMUSHclientDoc doc;
    CChatSocket sock(&doc);

    CFile* f = new CFile;
    ASSERT_TRUE(f->Open(path, CFile::modeWrite | CFile::modeCreate) != FALSE);

    sock.m_bDoingFileTransfer = true;      // a transfer is already running
    sock.m_bSendFile          = false;
    sock.m_bCanSendFiles      = true;      // ...and files are allowed
    sock.m_pFile              = f;
    sock.m_iFileBlockSize     = 1024;
    sock.m_iFileSize          = 4096;
    sock.m_iFileBlocks        = 4;
    sock.m_iBlocksTransferred = 1;
    sock.m_strOurFileName     = path;

    // A well-formed "filename,filesize" for a SECOND file.
    sock.Process_File_start(CString("second.bin,2048"));

    // The live transfer must be untouched: same CFile, same name, same size.
    EXPECT_TRUE(sock.m_pFile == f);
    EXPECT_TRUE(sock.m_bDoingFileTransfer);
    EXPECT_EQ(4096L, sock.m_iFileSize);
    EXPECT_EQ(std::string(path), std::string((const char*) sock.m_strOurFileName));

    sock.m_bDoingFileTransfer = false;     // stop the dtor from touching it
    sock.m_pFile = NULL;
    f->Close();
    delete f;
    ::DeleteFileA(path);
}

// ── A freshly constructed timer must be enabled, like its two siblings ─────
//
// `CAlias` and `CTrigger` both do `bEnabled = TRUE;` in their constructors.
// `CTimer`'s did not initialise `bEnabled` at all, and that is not cosmetic:
// `Get_XML_boolean` leaves its output **untouched** when the attribute is
// absent and defaults are wanted —
//
//     if (!Get_XML_string (node, sName, strValue, bUseDefault))
//       if (bUseDefault)
//         return false;          // bValue never assigned
//
// — so a `<timer defaults="y">` with no `enabled=` attribute, which is ordinary
// hand-written plugin XML, read whatever was on the heap. `docs/UPSTREAM-BUGS.md`
// 99.
//
// Asserted against the siblings rather than against `TRUE` alone, because the
// value that matters is *the same one the other two use*: a timer that loaded
// disabled where an identically-written alias loaded enabled would be a subtler
// bug than the uninitialised read.
TEST(ConstructorDefaultsR6, ATimerIsEnabledOnConstructionJustLikeAnAliasAndATrigger)
{
    CTimer   timer;
    CAlias   alias;
    CTrigger trigger;

    EXPECT_TRUE(timer.bEnabled != FALSE);
    EXPECT_TRUE(alias.bEnabled != 0);
    EXPECT_TRUE(trigger.bEnabled != 0);

    // ...and the same on the heap, which is the path the XML loader takes and
    // the one where an uninitialised member picks up allocator fill rather than
    // whatever the stack happened to hold.
    CTimer* heap = new CTimer;
    EXPECT_TRUE(heap->bEnabled != FALSE);
    delete heap;
}

// ── Changing wrap_column must resize the line buffer honestly ──────────────
//
// `CLine`'s constructor sizes the text buffer in BYTES from a wrap column
// counted in CHARACTERS, so a UTF-8 world gets four bytes each
// (`Line.cpp`: `iMemoryAllocated = nWrapColumn * 4`). The `OPT_FIX_WRAP_COLUMN`
// arm of `SetOptionItem` reallocated the *current* line's buffer when the
// column changed, and did it with the raw column count — a quarter of what the
// wrap logic is entitled to fill on a UTF-8 world.
//
// Worse, it never updated `iMemoryAllocated`. `DisplayMsg`'s emergency
// bail-out is
//
//     (m_pCurrentLine->len >= m_pCurrentLine->iMemoryAllocated)
//
// under a comment saying it exists so the UTF-8 case "shouldn't overshoot the
// allocated memory" — so the one guard against overrunning this buffer was
// answering for the allocation the line used to have. Set a *smaller* wrap
// column and the guard permits writing past the end of the new buffer;
// `world.SetOption ("wrap_column", n)` is all it takes.
// `docs/UPSTREAM-BUGS.md` 100.
//
// The assertion is `iMemoryAllocated` against the constructor's own arithmetic,
// because that member is simultaneously the size of the allocation and the
// thing the guard trusts — if they agree, both halves are right.
TEST(WrapColumnResizeR6, TheLineBufferAndItsRecordedSizeStayInStep)
{
    // Non-UTF-8 first: one byte per column, and the record must follow.
    {
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = false;
        doc.m_pCurrentLine = new CLine(1, 80, 0, WHITE, BLACK, false);
        doc.m_LineList.AddTail(doc.m_pCurrentLine);
        ASSERT_EQ(80, doc.m_pCurrentLine->iMemoryAllocated);

        doc.SetOption("wrap_column", 200);
        EXPECT_EQ(200, doc.m_pCurrentLine->iMemoryAllocated);
    }

    // UTF-8: four bytes per column, which is where the old arithmetic was a
    // quarter short.
    {
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = true;
        doc.m_pCurrentLine = new CLine(1, 80, 0, WHITE, BLACK, true);
        doc.m_LineList.AddTail(doc.m_pCurrentLine);
        ASSERT_EQ(320, doc.m_pCurrentLine->iMemoryAllocated);   // 80 * 4

        doc.SetOption("wrap_column", 200);
        if (doc.m_pCurrentLine->iMemoryAllocated != 800)
            printf("      (UTF-8 buffer recorded as %d bytes, not 800)\n",
                   doc.m_pCurrentLine->iMemoryAllocated);
        EXPECT_EQ(800, doc.m_pCurrentLine->iMemoryAllocated);   // 200 * 4
    }

    // Shrinking is the dangerous direction: the record must come DOWN too, or
    // the bail-out keeps clearing writes the buffer can no longer take.
    {
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = false;
        doc.m_pCurrentLine = new CLine(1, 500, 0, WHITE, BLACK, false);
        doc.m_LineList.AddTail(doc.m_pCurrentLine);
        ASSERT_EQ(500, doc.m_pCurrentLine->iMemoryAllocated);

        doc.SetOption("wrap_column", 60);
        EXPECT_TRUE(doc.m_pCurrentLine->iMemoryAllocated <= 500);
        EXPECT_TRUE(doc.m_pCurrentLine->iMemoryAllocated >=
                    doc.m_pCurrentLine->len);
    }
}

// ── Replacing an item whose own script is running ──────────────────────────
//
// `ProcessPreviousLine` brackets a trigger's script with
//
//     trigger_item->bExecutingScript = true;     // cannot be deleted now
//     SendTo (...);                              // the script runs here
//     trigger_item->bExecutingScript = false;    // can be deleted now
//
// and `DeleteTrigger` honours that comment: `if (trigger_item->bExecutingScript)
// return eItemInUse;`. `AddTriggerEx`'s `eReplace` arm did not — it went
// straight to `delete trigger_item`. So a script that replaces the very trigger
// that invoked it frees the object, and the `= false` on the line after
// `SendTo` writes into freed memory.
//
// `world.AddTrigger (name, …, eReplace)` from inside that trigger's own script
// is the reproduction, and re-registering yourself is a normal thing for a
// plugin to do. `AddAlias` and `AddTimer` had the identical hole against their
// own `DeleteAlias` / `DeleteTimer` guards. `docs/UPSTREAM-BUGS.md` 102.
TEST(ReplaceWhileRunningR6, AllThreeRefuseToReplaceAnItemWhoseScriptIsOnTheStack)
{
    CMUSHclientDoc doc;

    ASSERT_EQ(eOK, doc.AddTriggerEx("t", "hello", "", eEnabled, -1, 0,
                                    "", "", eSendToWorld, 100));
    ASSERT_EQ(eOK, doc.AddAlias("a", "go", "", eEnabled, ""));
    ASSERT_EQ(eOK, doc.AddTimer("m", 0, 0, 5.0, "", eEnabled, ""));

    CTrigger* t = NULL;  ASSERT_TRUE(doc.GetTriggerMap().Lookup("t", t));
    CAlias*   a = NULL;  ASSERT_TRUE(doc.GetAliasMap().Lookup("a", a));
    CTimer*   m = NULL;  ASSERT_TRUE(doc.GetTimerMap().Lookup("m", m));

    // Each one's script is on the stack, exactly as ProcessPreviousLine and its
    // alias/timer counterparts leave it while SendTo runs.
    t->bExecutingScript = true;
    a->bExecutingScript = true;
    m->bExecutingScript = true;

    EXPECT_EQ(eItemInUse, doc.AddTriggerEx("t", "goodbye", "", eEnabled | eReplace,
                                           -1, 0, "", "", eSendToWorld, 100));
    EXPECT_EQ(eItemInUse, doc.AddAlias("a", "stop", "", eEnabled | eReplace, ""));
    EXPECT_EQ(eItemInUse, doc.AddTimer("m", 0, 0, 9.0, "", eEnabled | eReplace, ""));

    // The objects must be the SAME ones, still in the maps — a replace that
    // deleted and re-created would leave a different pointer behind, which is
    // the freed one the caller is about to write to.
    CTrigger* t2 = NULL; EXPECT_TRUE(doc.GetTriggerMap().Lookup("t", t2));
    CAlias*   a2 = NULL; EXPECT_TRUE(doc.GetAliasMap().Lookup("a", a2));
    CTimer*   m2 = NULL; EXPECT_TRUE(doc.GetTimerMap().Lookup("m", m2));
    EXPECT_EQ((void*) t, (void*) t2);
    EXPECT_EQ((void*) a, (void*) a2);
    EXPECT_EQ((void*) m, (void*) m2);

    // ...and the refusal must be about the SCRIPT, not about replace being
    // broken: with the flag cleared, the same three calls succeed.
    t->bExecutingScript = false;
    a->bExecutingScript = false;
    m->bExecutingScript = false;

    EXPECT_EQ(eOK, doc.AddTriggerEx("t", "goodbye", "", eEnabled | eReplace,
                                    -1, 0, "", "", eSendToWorld, 100));
    EXPECT_EQ(eOK, doc.AddAlias("a", "stop", "", eEnabled | eReplace, ""));
    EXPECT_EQ(eOK, doc.AddTimer("m", 0, 0, 9.0, "", eEnabled | eReplace, ""));
}

// ── An "every" timer may not be given a zero interval ──────────────────────
//
// `AddTimer` refuses the combination outright — *"can't have a zero time for
// 'every' timers"* — because such a timer fires on every poll, forever:
// `CheckTimers` adds the interval to `tFireTime`, which leaves it in the past,
// and its own guard
//
//     // in case clock changes or some such thing, make sure timer will be due
//     // to fire in the future, not the past, or it might go mad and keep firing
//     if (timer_item->tFireTime <= tNow)
//       ResetOneTimer (timer_item);
//
// calls `ResetOneTimer`, which adds the same zero. The comment names the
// symptom exactly; the guard cannot reach this case.
//
// `SetTimerOption` had no such check, so zeroing the components one call at a
// time arrived at the state `AddTimer` exists to prevent.
// `docs/UPSTREAM-BUGS.md` 105.
TEST(TimerIntervalR6, SetTimerOptionRefusesToZeroTheLastComponentOfAnEveryTimer)
{
    CMUSHclientDoc doc;
    ASSERT_EQ(eOK, doc.AddTimer("t", 0, 1, 30.0, "", eEnabled, ""));

    CTimer* t = NULL;
    ASSERT_TRUE(doc.GetTimerMap().Lookup("t", t));
    ASSERT_EQ(CTimer::eInterval, t->iType);

    // Zeroing components while others remain non-zero is fine.
    EXPECT_EQ(eOK, doc.SetTimerOption("t", "second", "0"));
    EXPECT_EQ(0.0, t->fEverySecond);
    EXPECT_EQ(1,   t->iEveryMinute);

    // ...but the last one is not: that is the state AddTimer refuses.
    EXPECT_EQ(eTimeInvalid, doc.SetTimerOption("t", "minute", "0"));
    EXPECT_EQ(1, t->iEveryMinute);         // and nothing was half-written

    // AddTimer's own refusal, for comparison — the two must agree.
    EXPECT_EQ(eTimeInvalid, doc.AddTimer("z", 0, 0, 0.0, "", eEnabled, ""));

    // An "at" timer is a different thing: midnight is a legitimate time.
    ASSERT_EQ(eOK, doc.AddTimer("a", 0, 0, 0.0, "", eEnabled | eAtTime, ""));
    CTimer* a = NULL;
    ASSERT_TRUE(doc.GetTimerMap().Lookup("a", a));
    EXPECT_EQ(CTimer::eAtTime, a->iType);
    EXPECT_EQ(eOK, doc.SetTimerOption("a", "second", "0"));
}

// The consequence, measured rather than argued: a zero-interval "every" timer
// fires on EVERY poll.  `CheckTimers` is called three times back to back with
// no time allowed to pass, and the zero timer matches three times while a
// legitimate five-second one matches at most once.
//
// The state is reached here by writing the field directly, which is what the
// unguarded `SetTimerOption` did one component at a time -- and is also what a
// hand-edited world file can still contain, since the XML loader does not run
// AddTimer's check either.
TEST(TimerIntervalR6, ZeroIntervalTimerFiresOnEveryPoll)
{
    CMUSHclientDoc doc;
    ASSERT_EQ(eOK, doc.AddTimer("runaway", 0, 0, 5.0, "",
                                eEnabled | eActiveWhenClosed | eTimerNote, ""));
    ASSERT_EQ(eOK, doc.AddTimer("control", 0, 0, 5.0, "",
                                eEnabled | eActiveWhenClosed | eTimerNote, ""));

    CTimer* runaway = NULL;  ASSERT_TRUE(doc.GetTimerMap().Lookup("runaway", runaway));
    CTimer* control = NULL;  ASSERT_TRUE(doc.GetTimerMap().Lookup("control", control));

    runaway->fEverySecond = 0.0;         // the state AddTimer refuses
    doc.ResetOneTimer(runaway);
    doc.ResetOneTimer(control);

    for (int i = 0; i < 3; ++i)
        doc.CheckTimers();

    EXPECT_EQ(3L, runaway->nMatched);    // once per poll, forever
    EXPECT_LE(control->nMatched, 1L);    // a real interval waits
}

// ── A timer's interval must survive being saved ────────────────────────────
//
// `Save_XML_double` wrote `%.2f`, but a timer's seconds are declared to FOUR
// decimals everywhere else: the loader reads them with
// `GET_DOUBLE ("second", ..., 0, 59.9999)` and `CTimerDlg::DoDataExchange`
// bounds them with the same `59.9999`.  So a legal sub-0.005 s interval --
// `AddTimer` accepts anything but exactly zero, and the dialog refuses only
// `<= 0` -- was rounded to `0.00` on save, and the loader then REFUSED its own
// output: "Timer for 'every' time cannot have zero interval".
//
// Measured before the fix: the file contained `second="0.00"`, the reload
// dropped `blip` and kept the other two (so the damage is one timer, not the
// section), and the user got a `[WARNING]` line naming a world file the app
// itself had just written.  `docs/UPSTREAM-BUGS.md` 106.
TEST(TimerSaveR6, SubCentisecondIntervalSurvivesASaveAndReload)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mc_timer_prec_%d.mcl", (int)getpid());
    {
        CMUSHclientDoc doc;
        ASSERT_EQ(eOK, doc.AddTimer("blip",   0, 0, 0.001, "", eEnabled, ""));
        ASSERT_EQ(eOK, doc.AddTimer("plain",  0, 0, 5.0,   "", eEnabled, ""));
        CFile f;
        ASSERT_TRUE(f.Open(path,
            CFile::modeWrite | CFile::modeCreate | CFile::shareDenyWrite));
        { CArchive ar(&f, CArchive::store);
          doc.Save_World_XML(ar, (unsigned long)~0, CString("")); ar.Close(); }
        f.Flush(); f.Close();
    }
    std::string c;
    { FILE* fp = fopen(path, "rb"); ASSERT_TRUE(fp != NULL);
      char buf[4096]; size_t n;
      while ((n = fread(buf, 1, sizeof buf, fp)) > 0) c.append(buf, n);
      fclose(fp); }

    EXPECT_NE(c.find("second=\"0.001\""), std::string::npos);  // precision kept
    // No textual negative for "0.00" here: `offset_second="0.00"` is a legal
    // superstring of it, and asserting on the shorter one would match that.
    // The round trip below is what proves the value was not rounded away.

    // A whole number still writes two decimals, so existing world files are
    // byte-identical after a re-save -- the trim is what buys that.
    EXPECT_NE(c.find("second=\"5.00\""),        std::string::npos);
    EXPECT_NE(c.find("offset_second=\"0.00\""), std::string::npos);

    CMUSHclientDoc doc2;
    ASSERT_TRUE(doc2.LoadWorldFromFile(path));
    CTimer* t = NULL;
    ASSERT_TRUE(doc2.GetTimerMap().Lookup("blip", t));         // no longer dropped
    EXPECT_EQ(0.001, t->fEverySecond);
    CTimer* p = NULL;
    ASSERT_TRUE(doc2.GetTimerMap().Lookup("plain", p));
    EXPECT_EQ(5.0, p->fEverySecond);
    unlink(path);
}
