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
