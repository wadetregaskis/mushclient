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

// Convenience: run an op over a C string and return the result as std::string.
static std::string Convert(int op, const char* src)
{
    CString dst("!! not written !!");
    if (!NotepadConvertText(op, CString(src), dst))
        return "<failed>";
    return std::string((const char*) dst);
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

TEST(NotepadConvert, DosToMacKeepsTheLineStructure)
{
    // The canonical strips LF from a CRLF buffer, leaving CR-separated lines —
    // which is a line ending the control renders identically.  Run raw over
    // LF-only text it deleted every line break and collapsed the document into
    // one enormous line.
    EXPECT_EQ(std::string("a\nb\nc"),
              Convert(eNotepadConvertDosToMac, "a\nb\nc"));
}

TEST(NotepadConvert, MacToDosKeepsTheLineStructure)
{
    EXPECT_EQ(std::string("a\nb\nc"),
              Convert(eNotepadConvertMacToDos, "a\nb\nc"));
}

TEST(NotepadConvert, UnixToDosAndDosToUnixKeepTheLineStructure)
{
    // The control cannot represent the difference, so both are no-ops here.
    // Unix→DOS emitting CRLF into the control DOUBLE-SPACED the document.
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
    // Empty preference falls back to the canonical "> ".
    App.m_strNotepadQuoteString = "";
    EXPECT_EQ(std::string("> a\n> b"), Convert(eNotepadConvertQuoteLines, "a\nb"));
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
    // Canonical CTextView catches the exception and leaves the window alone; the
    // false return is how the caller knows not to touch the control.
    CString dst("untouched");
    EXPECT_FALSE(NotepadConvertText(eNotepadConvertBase64Decode, CString("not!base64"), dst));
    EXPECT_STREQ("untouched", (const char*) dst);
}

TEST(NotepadConvert, UnknownOpIsRefused)
{
    CString dst("untouched");
    EXPECT_FALSE(NotepadConvertText(-1, CString("hello"), dst));
    EXPECT_STREQ("untouched", (const char*) dst);
}

MC_TEST_MAIN()
