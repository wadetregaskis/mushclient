// tests/test_notepad_convert.cpp
//
// The notepad Convert menu (notepad_convert_shared.cpp).
//
// These transforms are byte-faithful copies of CTextView::OnConvert*
// (TextView.cpp), which run against a Win32 multiline CEdit whose buffer really
// does hold CRLF.  Every end-of-line one is keyed on the CARRIAGE RETURN.  The
// port drives a wxTextCtrl instead, and wxOSX rewrites every CR to LF on the way
// in — GetValue() can never return a '\r' (verified against this machine's
// wxWidgets 3.3: SetValue("a\r\nb") reads back as "a\n\nb").  Feeding the
// control's text straight to the canonical bodies therefore made
// Remove-end-of-lines a silent no-op and turned DOS→Mac / Mac→DOS into "delete
// every line break in the document".
//
// So the contract pinned here is the BOUNDARY one: NotepadConvertText takes and
// returns text in the control's LF-only convention, and does the CRLF round-trip
// the canonical logic needs internally.  Take the CRLF expansion back out and
// the RemoveEol / DosToMac / MacToDos cases below fail.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/wx_mfc_hooks.h"   // g_wxhook_message_box (QuoteForumCodes + Base64 confirm)

#include <cstdio>
#include <string>
#include "posix_compat.h"

// Convenience: run an op over a C string and return the result as std::string.
static std::string Convert(int op, const char* src)
{
    CString dst("!! not written !!");
    if (!NotepadConvertText(op, CString(src), dst))
        return "<failed>";
    return std::string((const char*) dst);
}

// ── g_wxhook_message_box capture ────────────────────────────────────────────
// Shared by the Base64-decode-failure test below and the QuoteForumCodes
// section further down: counts prompts, answers them without a window, and
// remembers the last message text so a test can assert something was
// actually SAID, not merely that a box was asked.
static int         g_answer      = IDOK;
static int         g_boxCalls    = 0;
static std::string g_lastMessage;
static int FakeMessageBox(const char* text, const char*, unsigned)
{
    g_boxCalls++;
    g_lastMessage = text ? text : "";
    return g_answer;
}

struct MessageBoxHook {
    int (*saved)(const char*, const char*, unsigned);
    MessageBoxHook(int answer) : saved(g_wxhook_message_box)
      {
      g_answer = answer;
      g_boxCalls = 0;
      g_lastMessage.clear();
      g_wxhook_message_box = FakeMessageBox;
      }
    ~MessageBoxHook() { g_wxhook_message_box = saved; }
};

// ── Line endings: the file boundary ─────────────────────────────────────────
//
// A DIFFERENT boundary from the LF↔CRLF one below.  That one is internal to a
// Convert op; this one is about the user's FILE, and getting it wrong was
// silent data corruption: the wx control expands a CRLF into TWO line breaks
// (SetValue("a\r\nb") reads back "a\n\nb" — re-measured against wxWidgets
// 3.3.2 while writing this), so opening any CRLF file double-spaced it and the
// next Save wrote the damage back.  Every bundled plugin is CRLF.
//
// These assertions live at the mushcore layer, over the real functions the
// product calls.  The wx half — that the control really does expand a CRLF,
// and that the frame really does route its loads and saves through here — is
// pinned in platform/notepad_selftest.cpp (--notepad-selftest), because
// nothing in this executable links a wxTextCtrl.

// A scratch file that removes itself, so a failing assertion cannot leave one
// behind to make the NEXT run pass.
struct ScratchFile {
    std::string path;
    explicit ScratchFile(const char* tag)
    {
        char buf[256];
        snprintf(buf, sizeof buf, "/tmp/mushclient_np_eol_%s_%d.txt",
                 tag, (int) getpid());
        path = buf;
        remove(path.c_str());
    }
    ~ScratchFile() { remove(path.c_str()); }

    void write(const std::string& bytes) const
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return;
        if (!bytes.empty())
            fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }

    std::string read() const
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return "<unreadable>";
        std::string out;
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, got);
        fclose(f);
        return out;
    }

    ScratchFile(const ScratchFile&)            = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
};

TEST(NotepadLineEndings, DetectionIsByMajorityNotByTheFirstBreak)
{
    // The uniform cases — what nearly every real file is.
    EXPECT_EQ(eLineEndingsCRLF, NotepadDetectLineEndings("a\r\nb\r\n", 6));
    EXPECT_EQ(eLineEndingsLF,   NotepadDetectLineEndings("a\nb\n", 4));
    EXPECT_EQ(eLineEndingsCR,   NotepadDetectLineEndings("a\rb\r", 4));

    // No line breaks at all, and the empty file: nothing to preserve, so the
    // platform default.  LF here.
    EXPECT_EQ(eLineEndingsDefault, NotepadDetectLineEndings("no breaks", 9));
    EXPECT_EQ(eLineEndingsDefault, NotepadDetectLineEndings("", 0));
    EXPECT_EQ(eLineEndingsLF,      eLineEndingsDefault);

    // MAJORITY, and specifically not first-wins: a Unix file whose FIRST break
    // happens to be a CRLF is still a Unix file.  Under first-wins this would
    // answer CRLF and the next save would rewrite every line of the file.
    EXPECT_EQ(eLineEndingsLF,   NotepadDetectLineEndings("a\r\nb\nc\nd\n", 9));
    // ...and the mirror: one stray LF does not make a DOS file a Unix one.
    EXPECT_EQ(eLineEndingsCRLF, NotepadDetectLineEndings("a\nb\r\nc\r\nd\r\n", 11));
}

TEST(NotepadLineEndings, TiesResolveTowardsWhatTheControlCannotHold)
{
    // Documented rule: ties go CRLF > CR > LF, because those are the styles
    // the user LOSES by defaulting — LF is what the control holds anyway.
    EXPECT_EQ(eLineEndingsCRLF, NotepadDetectLineEndings("a\r\nb\nc", 6));
    EXPECT_EQ(eLineEndingsCR,   NotepadDetectLineEndings("a\rb\nc", 5));
    EXPECT_EQ(eLineEndingsCRLF, NotepadDetectLineEndings("a\r\nb\rc\nd", 8));
}

TEST(NotepadLineEndings, ACRLFBecomesONELineBreakInTheControl)
{
    // The bug in one assertion.  wx makes this "a\n\nb"; the control must be
    // handed "a\nb" instead.
    EXPECT_EQ(std::string("a\nb"),   NotepadBytesToControlText("a\r\nb", 4));
    EXPECT_EQ(std::string("a\nb"),   NotepadBytesToControlText("a\rb", 3));
    EXPECT_EQ(std::string("a\nb"),   NotepadBytesToControlText("a\nb", 3));
    EXPECT_EQ(std::string("x\ny\nz\n"),
              NotepadBytesToControlText("x\r\ny\r\nz\r\n", 9));
    // A lone CR followed by a lone LF is two breaks, not one — only an
    // ADJACENT pair is a single CRLF.
    EXPECT_EQ(std::string("a\n\nb"), NotepadBytesToControlText("a\n\rb", 4));
}

TEST(NotepadLineEndings, WritingBackUsesTheDocumentsOwnStyle)
{
    EXPECT_EQ(std::string("a\r\nb\r\n"),
              NotepadControlTextToBytes("a\nb\n", eLineEndingsCRLF));
    EXPECT_EQ(std::string("a\nb\n"),
              NotepadControlTextToBytes("a\nb\n", eLineEndingsLF));
    EXPECT_EQ(std::string("a\rb\r"),
              NotepadControlTextToBytes("a\nb\n", eLineEndingsCR));
    // No line breaks: the style cannot show, and must not add anything.
    EXPECT_EQ(std::string("one line"),
              NotepadControlTextToBytes("one line", eLineEndingsCRLF));
}

// ── (a) The regression that matters: load → save with no edit ───────────────

TEST(NotepadLineEndings, LoadThenSaveUneditedIsByteIdentical)
{
    // Read the actual file bytes back and compare them.  Before the fix the
    // CRLF case came back double-spaced and half again as long.
    struct Case { const char* tag; const char* bytes; int style; };
    const Case kCases[] = {
        { "crlf", "line one\r\nline two\r\nline three\r\n", eLineEndingsCRLF },
        { "lf",   "line one\nline two\nline three\n",       eLineEndingsLF   },
        { "cr",   "line one\rline two\rline three\r",       eLineEndingsCR   },
        // No trailing break — a real shape, and the one an off-by-one in the
        // writer would corrupt.
        { "notrail", "alpha\r\nbeta",                       eLineEndingsCRLF },
        // A blank line in the middle must survive as ONE blank line.
        { "blank", "a\r\n\r\nb\r\n",                        eLineEndingsCRLF },
    };

    for (const Case& c : kCases)
    {
        ScratchFile f(c.tag);
        const std::string original(c.bytes);
        f.write(original);

        std::string text;
        int style = -1;
        EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
        EXPECT_EQ(c.style, style);
        // Whatever came in, the control only ever sees LF.
        EXPECT_TRUE(text.find('\r') == std::string::npos);

        EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, style));
        EXPECT_EQ(original, f.read());
    }
}

// ── (b) The same, with one line edited ──────────────────────────────────────

TEST(NotepadLineEndings, EditingOneLineChangesOnlyThatLine)
{
    // The acceptance criterion, at this layer: open a CRLF file, change one
    // line, save — everything else comes back byte for byte, still CRLF.
    ScratchFile f("edit");
    f.write("first\r\nsecond\r\nthird\r\n");

    std::string text;
    int style = -1;
    EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
    EXPECT_EQ(eLineEndingsCRLF, style);
    EXPECT_EQ(std::string("first\nsecond\nthird\n"), text);

    // The edit a user would make in the control, in the control's convention.
    const size_t at = text.find("second");
    EXPECT_TRUE(at != std::string::npos);
    text.replace(at, 6, "SECOND EDITED");

    EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, style));
    EXPECT_EQ(std::string("first\r\nSECOND EDITED\r\nthird\r\n"), f.read());
}

TEST(NotepadLineEndings, SavingAnLFFileDoesNotGiveItCRLF)
{
    // The control's convention is LF, so a bug that unconditionally wrote CRLF
    // would pass every CRLF case above and corrupt Unix files instead.
    ScratchFile f("lfedit");
    f.write("alpha\nbeta\n");

    std::string text;
    int style = -1;
    EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
    text += "gamma\n";
    EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, style));
    EXPECT_EQ(std::string("alpha\nbeta\ngamma\n"), f.read());
}

// ── (d) A file whose endings are mixed ──────────────────────────────────────

TEST(NotepadLineEndings, AMixedFileIsNormalisedToItsMajorityStyle)
{
    // Documented rule: mixed endings do NOT round-trip byte-for-byte — they
    // cannot, since the control holds one kind — they are unified to the
    // majority style.  Pinned so the behaviour is a decision, not an accident.
    ScratchFile f("mixed");
    f.write("dos\r\nunix\ndos\r\ndos\r\n");     // 3 CRLF vs 1 LF

    std::string text;
    int style = -1;
    EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
    EXPECT_EQ(eLineEndingsCRLF, style);
    EXPECT_EQ(std::string("dos\nunix\ndos\ndos\n"), text);

    EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, style));
    // The stray LF is now a CRLF like its neighbours — the file got MORE
    // consistent, not less, which is the point of picking the majority.
    EXPECT_EQ(std::string("dos\r\nunix\r\ndos\r\ndos\r\n"), f.read());
}

TEST(NotepadLineEndings, ReadAndWriteRefuseAPathTheyCannotUse)
{
    std::string text("unchanged");
    int style = -1;
    EXPECT_FALSE(NotepadReadFile("/tmp/mushclient_np_eol_definitely_absent", text, style));
    EXPECT_FALSE(NotepadReadFile(nullptr, text, style));
    EXPECT_FALSE(NotepadReadFile("", text, style));
    // A refused read must not have touched the caller's buffer.
    EXPECT_EQ(std::string("unchanged"), text);

    EXPECT_FALSE(NotepadWriteFile("/no/such/directory/at/all.txt", "x", eLineEndingsLF));
    EXPECT_FALSE(NotepadWriteFile(nullptr, "x", eLineEndingsLF));
    EXPECT_FALSE(NotepadWriteFile("", "x", eLineEndingsLF));
}

TEST(NotepadLineEndings, AnEmptyFileRoundTripsAsEmpty)
{
    ScratchFile f("empty");
    f.write("");

    std::string text("stale");
    int style = -1;
    EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
    EXPECT_EQ(std::string(""), text);
    EXPECT_EQ(eLineEndingsDefault, style);
    EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, style));
    EXPECT_EQ(std::string(""), f.read());
}

// ── The four line-ending Convert ops ────────────────────────────────────────
//
// These were provable no-ops: the control cannot hold a CR, so the canonical
// CR-keyed transforms had nothing to rewrite, and the test that "covered" them
// asserted the text came back unchanged — which it does whether the op works or
// does nothing at all.  With a tag on the document they mean what the menu says,
// and the discriminator is the SAVED BYTES, not the text.

TEST(NotepadLineEndings, EachConversionNamesTheStyleItTargets)
{
    int style = -1;

    EXPECT_TRUE(NotepadConvertLineEndingTarget(eNotepadConvertUnixToDos, &style));
    EXPECT_EQ(eLineEndingsCRLF, style);
    EXPECT_TRUE(NotepadConvertLineEndingTarget(eNotepadConvertMacToDos, &style));
    EXPECT_EQ(eLineEndingsCRLF, style);
    EXPECT_TRUE(NotepadConvertLineEndingTarget(eNotepadConvertDosToUnix, &style));
    EXPECT_EQ(eLineEndingsLF, style);
    EXPECT_TRUE(NotepadConvertLineEndingTarget(eNotepadConvertDosToMac, &style));
    EXPECT_EQ(eLineEndingsCR, style);

    // The control: every OTHER op must be refused, or the Convert menu would
    // start retagging the document for Base64 and UPPER CASE too.
    const int kNotLineEndings[] = {
        eNotepadConvertUpperCase,   eNotepadConvertLowerCase,
        eNotepadConvertRemoveEol,   eNotepadConvertRemoveBlanks,
        eNotepadConvertQuoteLines,  eNotepadConvertQuoteForum,
        eNotepadConvertBase64Encode, eNotepadConvertBase64Decode,
        eNotepadConvertHtmlSpecial,
    };
    for (int op : kNotLineEndings)
        EXPECT_FALSE(NotepadConvertLineEndingTarget(op, &style));
    EXPECT_FALSE(NotepadConvertLineEndingTarget(-1, &style));

    // ...and a refusal must not have written through the out-parameter.
    style = 12345;
    EXPECT_FALSE(NotepadConvertLineEndingTarget(eNotepadConvertUpperCase, &style));
    EXPECT_EQ(12345, style);

    // A null out-parameter is allowed (a caller may only want the predicate).
    EXPECT_TRUE(NotepadConvertLineEndingTarget(eNotepadConvertDosToMac, nullptr));
}

TEST(NotepadLineEndings, EachConversionChangesTheSavedBytes)
{
    // The assertion the old test could not make.  Start from a Unix file, apply
    // each conversion, and read back what actually lands on disk.
    struct Case { int op; const char* expected; };
    const Case kCases[] = {
        { eNotepadConvertUnixToDos, "a\r\nb\r\n" },
        { eNotepadConvertMacToDos,  "a\r\nb\r\n" },
        { eNotepadConvertDosToMac,  "a\rb\r"     },
        { eNotepadConvertDosToUnix, "a\nb\n"     },
    };

    for (const Case& c : kCases)
    {
        ScratchFile f("conv");
        f.write("a\nb\n");

        std::string text;
        int style = -1;
        EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
        EXPECT_EQ(eLineEndingsLF, style);

        // What the Convert menu now does: retag, and leave the text alone.
        int target = -1;
        EXPECT_TRUE(NotepadConvertLineEndingTarget(c.op, &target));
        CString dst;
        EXPECT_TRUE(NotepadConvertText(c.op, CString(text.c_str()), dst));
        EXPECT_EQ(text, std::string((const char*) dst));   // the TEXT is untouched

        EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, target));
        EXPECT_EQ(std::string(c.expected), f.read());
    }
}

TEST(NotepadLineEndings, ConversionsAreNotConditionalOnTheCurrentStyle)
{
    // A deliberate difference from Windows, recorded so it is not "fixed" back.
    // CTextView::OnConvertUnixtodos drops every CR and only inserts one before
    // an LF, so on Windows applying it to a Mac (CR-only) buffer DELETES every
    // line break in the file.  Here the op names its target, so a mismatched
    // source is harmless.
    ScratchFile f("mac2dos");
    f.write("a\rb\rc\r");

    std::string text;
    int style = -1;
    EXPECT_TRUE(NotepadReadFile(f.path.c_str(), text, style));
    EXPECT_EQ(eLineEndingsCR, style);
    EXPECT_EQ(std::string("a\nb\nc\n"), text);

    int target = -1;
    EXPECT_TRUE(NotepadConvertLineEndingTarget(eNotepadConvertUnixToDos, &target));
    EXPECT_TRUE(NotepadWriteFile(f.path.c_str(), text, target));
    // Three lines still, now DOS -- not one line with the breaks eaten.
    EXPECT_EQ(std::string("a\r\nb\r\nc\r\n"), f.read());
}

// ── The boundary itself ─────────────────────────────────────────────────────

TEST(NotepadConvert, ToCanonicalExpandsControlLineEndings)
{
    // The control only ever holds LF; expand to the CRLF the canonical bodies
    // are written against.  A lone CR is handled defensively, and an existing
    // CRLF must not become CR CR LF.
    EXPECT_STREQ("a\r\nb",       (const char*) NotepadTextToCanonical("a\nb"));
    EXPECT_STREQ("a\r\nb",       (const char*) NotepadTextToCanonical("a\rb"));
    EXPECT_STREQ("a\r\nb",       (const char*) NotepadTextToCanonical("a\r\nb"));
    EXPECT_STREQ("a\r\n\r\nb",   (const char*) NotepadTextToCanonical("a\n\nb"));
    EXPECT_STREQ("no line ends", (const char*) NotepadTextToCanonical("no line ends"));
}

TEST(NotepadConvert, FromCanonicalCollapsesToWhatTheControlHolds)
{
    // CRLF collapses to ONE LF.  Writing the CRLF back into the control instead
    // is what double-spaced the document: wx expands each CR to its own LF.
    EXPECT_STREQ("a\nb",     (const char*) NotepadTextFromCanonical("a\r\nb"));
    EXPECT_STREQ("a\nb",     (const char*) NotepadTextFromCanonical("a\rb"));
    EXPECT_STREQ("a\nb",     (const char*) NotepadTextFromCanonical("a\nb"));
    EXPECT_STREQ("a\n\nb",   (const char*) NotepadTextFromCanonical("a\r\n\r\nb"));
}

TEST(NotepadConvert, RoundTripIsIdentityForControlText)
{
    const char* kTexts[] = { "", "one line", "a\nb\nc", "a\n\nb\n", "\n", "\n\n" };
    for (const char* t : kTexts)
        EXPECT_STREQ(t, (const char*)
                     NotepadTextFromCanonical(NotepadTextToCanonical(t)));
}

// ── The transforms that were broken by the CR mismatch ──────────────────────

TEST(NotepadConvert, RemoveEndOfLinesJoinsLines)
{
    // The canonical body does nothing at all unless it sees a CR, so with the
    // control's LF-only text this used to return the input unchanged.
    EXPECT_EQ(std::string("one two three"),
              Convert(eNotepadConvertRemoveEol, "one\ntwo\nthree"));
}

TEST(NotepadConvert, RemoveEndOfLinesKeepsParagraphBreaks)
{
    // A blank line (optionally holding spaces/tabs) is a paragraph break and
    // survives as a single line ending; the lines within a paragraph fold to a
    // space.  This is the canonical rule, verbatim.
    EXPECT_EQ(std::string("para one\npara two"),
              Convert(eNotepadConvertRemoveEol, "para\none\n\npara\ntwo"));
    EXPECT_EQ(std::string("para one\npara two"),
              Convert(eNotepadConvertRemoveEol, "para\none\n \t \npara\ntwo"));
}

TEST(NotepadConvert, RemoveEndOfLinesDoesNotDoubleSpaceAfterAParagraph)
{
    // The paragraph break the canonical emits is a CRLF; writing that straight
    // into the control produced two blank lines.
    EXPECT_EQ(std::string("a\nb"),
              Convert(eNotepadConvertRemoveEol, "a\n\nb"));
}

// The four line-ending ops, at the TEXT layer.
//
// These three fixtures assert only half the contract, and on their own they
// could never fail: "the text is unchanged" is equally true of a working
// conversion and of one that does nothing, which is exactly what these four
// were until the document gained a line-ending tag.  What they are worth is
// the other half — the text must survive the op INTACT, so a body that
// collapsed the document into one line (which running the canonical CR-keyed
// transform over LF-only text really did) still fails here.
//
// The half that discriminates is NotepadLineEndings.EachConversionChangesThe-
// SavedBytes above, which reads the file back, and the menu-driven version in
// platform/notepad_selftest.cpp.  Do not read these three as coverage of the
// conversions themselves.

TEST(NotepadConvert, DosToMacLeavesTheTEXTIntact)
{
    // The canonical strips LF from a CRLF buffer, leaving CR-separated lines —
    // which is a line ending the control renders identically.  Run raw over
    // LF-only text it deleted every line break and collapsed the document into
    // one enormous line.
    EXPECT_EQ(std::string("a\nb\nc"),
              Convert(eNotepadConvertDosToMac, "a\nb\nc"));
}

TEST(NotepadConvert, MacToDosLeavesTheTEXTIntact)
{
    EXPECT_EQ(std::string("a\nb\nc"),
              Convert(eNotepadConvertMacToDos, "a\nb\nc"));
}

TEST(NotepadConvert, UnixToDosAndDosToUnixLeaveTheTEXTIntact)
{
    // The control cannot represent the difference, so neither op touches the
    // text; the document's tag is where the conversion lands.  Unix→DOS
    // emitting CRLF into the control DOUBLE-SPACED the document.
    EXPECT_EQ(std::string("a\nb\nc"),
              Convert(eNotepadConvertUnixToDos, "a\nb\nc"));
    EXPECT_EQ(std::string("a\nb\nc"),
              Convert(eNotepadConvertDosToUnix, "a\nb\nc"));
}

TEST(NotepadConvert, NoTransformLeaksACarriageReturnIntoTheControl)
{
    // Whatever a transform does internally, what comes back must be something
    // the control can hold verbatim — otherwise wx silently rewrites it and the
    // caller's selection arithmetic is wrong too.
    const int kOps[] = {
        eNotepadConvertUpperCase, eNotepadConvertLowerCase,
        eNotepadConvertUnixToDos, eNotepadConvertDosToUnix,
        eNotepadConvertMacToDos,  eNotepadConvertDosToMac,
        eNotepadConvertRemoveEol, eNotepadConvertRemoveBlanks,
        eNotepadConvertQuoteLines, eNotepadConvertHtmlSpecial,
    };
    for (int op : kOps)
    {
        const std::string out = Convert(op, "alpha\nbeta\n\ngamma\n");
        EXPECT_TRUE(out.find('\r') == std::string::npos);
    }
}

// ── The transforms that never depended on the line ending ───────────────────

TEST(NotepadConvert, CaseTransforms)
{
    EXPECT_EQ(std::string("ABC\nDEF"), Convert(eNotepadConvertUpperCase, "abc\nDef"));
    EXPECT_EQ(std::string("abc\ndef"), Convert(eNotepadConvertLowerCase, "aBc\nDEF"));
}

TEST(NotepadConvert, RemoveExtraBlanks)
{
    EXPECT_EQ(std::string("a b c"), Convert(eNotepadConvertRemoveBlanks, "a   b \t c"));
}

TEST(NotepadConvert, QuoteLinesUsesThePreferenceAndQuotesEveryLine)
{
    App.m_strNotepadQuoteString = "| ";
    EXPECT_EQ(std::string("| a\n| b\n| "), Convert(eNotepadConvertQuoteLines, "a\nb\n"));
    // An explicitly-cleared preference is a NO-OP quote — the canonical
    // CTextView::OnConvertQuotelines has no "> " fallback of its own, so the
    // port must not invent one.  A body that still falls back to "> " here
    // fails this assertion.
    App.m_strNotepadQuoteString = "";
    EXPECT_EQ(std::string("a\nb"), Convert(eNotepadConvertQuoteLines, "a\nb"));
}

TEST(NotepadConvert, HtmlSpecialCharacters)
{
    EXPECT_EQ(std::string("&lt;b&gt; &amp; &quot;x&quot;"),
              Convert(eNotepadConvertHtmlSpecial, "<b> & \"x\""));
}

TEST(NotepadConvert, Base64RoundTripsThroughTheControlConvention)
{
    // Encoding sees the canonical CRLF buffer, exactly as it does on Windows,
    // and decoding puts the text back in the control's convention — so a
    // round-trip inside the notepad returns what the user started with.
    const std::string enc = Convert(eNotepadConvertBase64Encode, "line1\nline2\n");
    EXPECT_TRUE(enc != "<failed>");
    EXPECT_EQ(std::string("line1\nline2\n"),
              Convert(eNotepadConvertBase64Decode, enc.c_str()));
}

TEST(NotepadConvert, Base64DecodeFailsCleanlyOnGarbage)
{
    // Canonical CTextView catches the exception, REPORTS it (ReportError —
    // TextView.cpp:1226) and leaves the window alone; the false return is how
    // the caller knows not to touch the control.  The port used to swallow
    // the exception with no report at all, so a garbage decode looked like
    // nothing happened (the wx frontend only beeped, with no indication why).
    MessageBoxHook box(IDOK);
    CString dst("untouched");
    EXPECT_FALSE(NotepadConvertText(eNotepadConvertBase64Decode, CString("not!base64"), dst));
    EXPECT_STREQ("untouched", (const char*) dst);
    EXPECT_EQ(1, g_boxCalls);
    EXPECT_FALSE(g_lastMessage.empty());
}

TEST(NotepadConvert, UnknownOpIsRefused)
{
    CString dst("untouched");
    EXPECT_FALSE(NotepadConvertText(-1, CString("hello"), dst));
    EXPECT_STREQ("untouched", (const char*) dst);
}

// ── QuoteForumCodes: the twelfth op ─────────────────────────────────────────
//
// This op was the one conversion this file did not cover, and that gap is why
// nobody noticed it had no real implementation headless: platform/
// doc_flip_gui_stubs.cpp defined `QuoteForumCodes` as the identity, on the
// (false) grounds that the headless engine never reaches it.  It does — through
// notepad_convert_shared.cpp:244, which is the very function under test here.
//
// The body ends in a confirmation box and returns the input UNCHANGED unless the
// answer is IDOK, so a test that does not install a message-box hook cannot tell
// the real body from the identity stub: headless UMessageBox answers IDCANCEL
// for MB_OKCANCEL (platform/wx_mfc_hooks.h:87).  Both fixtures below therefore
// drive g_wxhook_message_box (the MessageBoxHook / FakeMessageBox pair defined
// near the top of this file), which is the discriminator — and the second one
// pins the cancel arm, so the auto-cancel behaviour is now covered rather than
// merely relied upon.

TEST(NotepadConvert, QuoteForumCodesEscapesBracketsAndBackslash)
{
    MessageBoxHook confirm(IDOK);

    // The discriminator: against the identity stub this returns the input
    // verbatim and fails on the first assertion.
    EXPECT_EQ(std::string("\\[b\\]a\\\\c\\[/b\\]"),
              Convert(eNotepadConvertQuoteForum, "[b]a\\c[/b]"));

    // Control: text with nothing to escape must round-trip UNCHANGED, so a
    // mutation that escapes indiscriminately fails too.  Without this, "return
    // a string with backslashes in it" would pass.
    EXPECT_EQ(std::string("plain text"),
              Convert(eNotepadConvertQuoteForum, "plain text"));

    // The conversion is not silent — it reports the change count.  Asserting the
    // box was actually asked makes "skip the prompt entirely" a failure.
    EXPECT_EQ(2, g_boxCalls);
}

TEST(NotepadConvert, QuoteForumCodesLeavesTextAloneWhenTheUserCancels)
{
    MessageBoxHook decline(IDCANCEL);

    // Utilities.cpp's contract, now utilities_shared.cpp: `!= IDOK` returns the
    // ORIGINAL text, not the converted one.  This is also the arm the headless
    // engine takes by default, so it is worth pinning explicitly.
    EXPECT_EQ(std::string("[b]a\\c[/b]"),
              Convert(eNotepadConvertQuoteForum, "[b]a\\c[/b]"));
    EXPECT_EQ(1, g_boxCalls);
}

MC_TEST_MAIN()
