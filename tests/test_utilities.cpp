// tests/test_utilities.cpp
//
// Unit tests for the single-sourced pure-logic utilities.  These live in
// utilities_shared.cpp (myAtoF, encodeBase64, EditDistance) and
// utilities_portable.cpp (decodeBase64) — both compiled into mushcore, which is
// the SAME utilities_shared.cpp the shipping product links.  So unlike the old
// portable twins, these exercise the real shipping code, not a reimplementation.

#include "mc_test.h"
#include <chrono>
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"   // CString + prototypes: myAtoF, EditDistance,
#include "scripting/errors.h"  // eOK / eBadParameter / eCannotPlaySound
#include "resource.h"          // ID_DISPLAY_STOP* — the sound menu commands
                                 // encodeBase64, decodeBase64
#include <cstring>
#include <set>
#include <sys/utsname.h>       // OSVersionString's oracle (tests never build on Windows)

extern CString OSVersionString (void);   // utilities_shared.cpp

// ── encodeBase64 / decodeBase64 ─────────────────────────────────────────────

// Single source of truth (audit §4): RFC 4648 §10 canonical vectors, shared by the
// encode test, the decode test and Base64RoundTrip.  The last row (0xFB 0xFF 0xBF →
// "+/+/") closes the '+'(62)/'/'(63) alphabet gap noted in the coverage audit; its
// plaintext bytes are high but contain no NUL, so strlen/strcmp handle them.
static const struct { const char* plain; const char* b64; } kRfc4648[] = {
    { "",       ""         },
    { "f",      "Zg=="     },
    { "fo",     "Zm8="     },
    { "foo",    "Zm9v"     },
    { "foob",   "Zm9vYg==" },
    { "fooba",  "Zm9vYmE=" },
    { "foobar", "Zm9vYmFy" },
    { "\xFB\xFF\xBF", "+/+/" },   // exercises '+' and '/' in the output alphabet
};

TEST(Utilities, EncodeBase64_Rfc4648Vectors)
{
    for (const auto& v : kRfc4648)
        EXPECT_STREQ(v.b64, (const char*)encodeBase64(v.plain, false));
}

TEST(Utilities, DecodeBase64_Rfc4648Vectors)
{
    for (const auto& v : kRfc4648)
        EXPECT_STREQ(v.plain, (const char*)decodeBase64(v.b64));
}

TEST(Utilities, DecodeBase64_IgnoresWhitespace)
{
    // decodeBase64 strips spaces/tabs/newlines before decoding.
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9v\r\nYmFy"));
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9v YmFy"));
}

TEST(Utilities, Base64RoundTrip)
{
    // the canonical vectors (single source of truth) ...
    for (const auto& v : kRfc4648)
    {
        CString enc = encodeBase64(v.plain, false);
        EXPECT_STREQ(v.plain, (const char*)decodeBase64((const char*)enc));
    }
    // ... plus a few longer free-form samples for good measure
    const char* samples[] = {
        "A", "AB", "ABC", "ABCD", "Hello, world!",
        "The quick brown fox jumps over the lazy dog."
    };
    for (const char* s : samples)
    {
        CString enc = encodeBase64(s, false);
        EXPECT_STREQ(s, (const char*)decodeBase64((const char*)enc));
    }
}

TEST(Utilities, EncodeBase64_MultiLineWrapsAt76)
{
    // 70 input bytes → 96 base64 chars; multi-line inserts a CRLF after the
    // first 76 output chars (i.e. after 57 input bytes = 19 quartets).  Pin the
    // wrap *position*, not merely that some CRLF exists.
    CString enc = encodeBase64(
        "0123456789012345678901234567890123456789012345678901234567890123456789", true);
    const char* p  = (const char*)enc;
    const char* nl = strstr(p, "\r\n");
    ASSERT_NE((const char*)NULL, nl);
    EXPECT_EQ(76, (int)(nl - p));   // first line is exactly 76 base64 chars
}

TEST(Utilities, DecodeBase64_EmptyInputNoOverread)
{
    // Over-read fix: empty / all-whitespace input must not walk before the
    // buffer; it decodes to the empty string (length is 0, i.e. mod-4, no throw).
    EXPECT_STREQ("", (const char*)decodeBase64(""));
    EXPECT_STREQ("", (const char*)decodeBase64("    "));
    EXPECT_STREQ("", (const char*)decodeBase64("\r\n\t"));
}

TEST(Utilities, DecodeBase64_ThrowsOnInvalid)
{
    // Restored contract: invalid input throws, which world.Base64Decode catches
    // and turns into a nil return.  mushcore's ThrowErrorException throws by
    // value, so catch(...) covers whatever the build throws.
    bool threwLen = false, threwChar = false;
    try { decodeBase64("abc"); }  catch (...) { threwLen  = true; }  // not mod-4
    try { decodeBase64("@@@@"); } catch (...) { threwChar = true; }  // invalid chars
    EXPECT_TRUE(threwLen);
    EXPECT_TRUE(threwChar);
}

// ── myAtoF ──────────────────────────────────────────────────────────────────

static double atof_val(const char* s) { double d = -999.0; myAtoF(s, &d); return d; }

TEST(Utilities, MyAtoF_Values)
{
    EXPECT_NEAR(0.0,     atof_val("0"),       1e-12);
    EXPECT_NEAR(42.0,    atof_val("42"),      1e-12);
    EXPECT_NEAR(-42.0,   atof_val("-42"),     1e-12);
    EXPECT_NEAR(0.5,     atof_val("0.5"),     1e-12);
    EXPECT_NEAR(0.25,    atof_val("0.25"),    1e-12);
    EXPECT_NEAR(-0.25,   atof_val("-0.25"),   1e-12);
    EXPECT_NEAR(3.14,    atof_val("3.14"),    1e-9);
    EXPECT_NEAR(1000.0,  atof_val("1e3"),     1e-9);
    EXPECT_NEAR(0.001,   atof_val("1e-3"),    1e-12);
    EXPECT_NEAR(12.5,    atof_val("  12.5"),  1e-12);  // leading whitespace skipped
}

TEST(Utilities, MyAtoF_ReturnsCharsConsumed)
{
    double d;
    EXPECT_EQ(2, myAtoF("42abc", &d));   // stops at 'a'
    EXPECT_EQ(4, myAtoF("3.14", &d));
    EXPECT_EQ(0, myAtoF("abc", &d));     // consumes nothing
}

// ── EditDistance ────────────────────────────────────────────────────────────

TEST(Utilities, EditDistance_Basics)
{
    EXPECT_EQ(0, EditDistance("", ""));
    EXPECT_EQ(3, EditDistance("", "abc"));
    EXPECT_EQ(3, EditDistance("abc", ""));
    EXPECT_EQ(0, EditDistance("kitten", "kitten"));
    EXPECT_EQ(3, EditDistance("kitten", "sitting"));   // classic Levenshtein = 3
    EXPECT_EQ(2, EditDistance("book", "back"));
}

TEST(Utilities, EditDistance_DamerauTransposition)
{
    // The algorithm's step 6A credits an adjacent transposition as a single
    // edit (for positions >2): "abcd" → "abdc" is 1, not 2.
    EXPECT_EQ(1, EditDistance("abcd", "abdc"));
}

TEST(Utilities, EditDistance_CapsAt20)
{
    // Each side is clamped to 20 chars, so distance can never exceed 20.
    std::string a(50, 'a');
    std::string b(50, 'b');
    EXPECT_EQ(20, EditDistance(a, b));
}

// ── line-positions index regression ─────────────────────────────────────────
// The shared ProcessPreviousLine omit-from-output branch writes the jump index
// slots unguarded whenever the line count passes a JUMP_SIZE boundary during
// tail deletion.  mushcore historically never allocated the array — a latent
// NULL-deref.  StartNewLine now lazily allocates it (mirroring the deferred
// allocation in doc.cpp SetUpOutputWindow); this test crosses the boundary with
// an omitted line to prove the branch is safe.

TEST(Utilities, OmitFromOutputAcrossJumpBoundaryDoesNotCrash)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;

    // an omit-from-output trigger
    CTrigger* t = new CTrigger();
    t->trigger         = CString("spamline");
    t->strLabel        = CString("spamline");
    t->bEnabled        = TRUE;
    t->bRegexp         = false;
    t->iSendTo         = eSendToOutput;
    t->iSequence       = DEFAULT_TRIGGER_SEQUENCE;
    t->bOmitFromOutput = TRUE;
    doc.m_TriggerMap.SetAt(t->strLabel, t);
    doc.m_TriggerArray.Add(t);

    // Exactly 100 fillers put the omitted line at in-list position 101, so the
    // omit branch's tail-deletion (and the fresh-line re-add) crosses the
    // ≡1 (mod JUMP_SIZE=100) boundary, which writes a jump-index chunk.
    // Unallocated → SIGSEGV; allocated → harmless.
    char buf[64];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof buf, "filler line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }
    doc.DisplayMsg("spamline\n", 9, 0);

    EXPECT_TRUE(doc.m_LinePositions.IsAllocated());   // lazily allocated
    EXPECT_GT((long)doc.m_LineList.GetCount(), 0L);
}

// ── OSC consumption ──────────────────────────────────────────────────────────
// ESC ] <payload> BEL (OSC — e.g. window-title strings) must be swallowed, not
// rendered.  Phase_ESC enters HAVE_OSC on every platform; the real DisplayMsg
// historically had no HAVE_OSC case, so the payload leaked into the output on
// Windows and the mac product (fixed 2026-07).  This locks the contract.

TEST(Utilities, OscSequencePayloadIsConsumed)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;

    const char* s = "before\033]0;window title\007after";
    doc.DisplayMsg(s, (int)strlen(s), 0);

    EXPECT_TRUE(doc.m_pCurrentLine != NULL);
    // the visible line must be exactly "beforeafter" — no OSC payload bytes
    std::string visible(doc.m_pCurrentLine->text, doc.m_pCurrentLine->len);
    EXPECT_STREQ("beforeafter", visible.c_str());
}

// ── FormatTime ────────────────────────────────────────────────────────────────
// The real FormatTime substitutes %N (world name) / %P (player name) / %E/%F/%L
// (directories) before strftime.  The old headless twin skipped all of them —
// log-file names and preambles silently lost the world/player names.

TEST(Utilities, FormatTimeSubstitutesWorldTokens)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "TestWorld";
    doc.m_name      = "Alice";
    CString s = doc.FormatTime(CTime::GetCurrentTime(), "%N-%P", false);
    EXPECT_STREQ("TestWorld-Alice", (const char*)s);
}

// ── IsStringNumber ──────────────────────────────────────────────────────────

TEST(Utilities, IsStringNumber_Unsigned)
{
    EXPECT_TRUE(IsStringNumber("0", false));
    EXPECT_TRUE(IsStringNumber("12345", false));
    EXPECT_FALSE(IsStringNumber("", false));      // empty is not a number
    EXPECT_FALSE(IsStringNumber("12a", false));   // trailing non-digit
    EXPECT_FALSE(IsStringNumber("1.5", false));   // '.' is not a digit
}

TEST(Utilities, IsStringNumber_HonoursBSigned)
{
    // A leading sign is accepted ONLY when bSigned is true.
    EXPECT_FALSE(IsStringNumber("-5", false));    // sign rejected when unsigned
    EXPECT_FALSE(IsStringNumber("+5", false));
    EXPECT_TRUE(IsStringNumber("-5", true));      // sign accepted when signed
    EXPECT_TRUE(IsStringNumber("+5", true));
    EXPECT_TRUE(IsStringNumber("42", true));      // no sign, still fine
    EXPECT_FALSE(IsStringNumber("-", true));      // just a sign is not a number
    EXPECT_FALSE(IsStringNumber("+", true));
    EXPECT_FALSE(IsStringNumber("5-3", true));    // sign only allowed at the front
}

// ── Free-function twins single-sourced from doc_portable.cpp ──────────────────

extern COLORREF xterm_256_colours[256];

TEST(Utilities, Generate256Colours)
{
    Generate256colours();
    EXPECT_EQ(RGB(0, 0, 0),       xterm_256_colours[0]);
    EXPECT_EQ(RGB(255, 255, 255), xterm_256_colours[15]);
    EXPECT_EQ(RGB(0, 0, 0),       xterm_256_colours[16]);    // 6x6x6 cube start (0,0,0)
    EXPECT_EQ(RGB(255, 255, 255), xterm_256_colours[231]);   // cube end (5,5,5)
    EXPECT_EQ(RGB(8, 8, 8),       xterm_256_colours[232]);   // grey ramp start
    EXPECT_EQ(RGB(238, 238, 238), xterm_256_colours[255]);   // grey ramp end (8+23*10)
}

TEST(Utilities, AnsiCode_EscSequence)
{
    EXPECT_STREQ("\033[0m",  (const char*)AnsiCode(0));
    EXPECT_STREQ("\033[1m",  (const char*)AnsiCode(1));
    EXPECT_STREQ("\033[31m", (const char*)AnsiCode(31));
}

TEST(Utilities, FixHTMLString_Escapes)
{
    EXPECT_STREQ("plain",                (const char*)FixHTMLString("plain"));
    EXPECT_STREQ("&lt;a&gt;&amp;&quot;", (const char*)FixHTMLString("<a>&\""));
    EXPECT_STREQ("x &amp; y",            (const char*)FixHTMLString("x & y"));
}

TEST(Utilities, ConvertToRegularExpression_Wildcards)
{
    // plain text: escaped, anchored only when bWholeLine
    EXPECT_STREQ("^abc$", (const char*)ConvertToRegularExpression("abc", true, true));
    EXPECT_STREQ("abc",   (const char*)ConvertToRegularExpression("abc", false, true));
    // '*' → capturing wildcard when enabled, escaped literal when not
    EXPECT_STREQ("^a(.*?)b$", (const char*)ConvertToRegularExpression("a*b", true, true));
    EXPECT_STREQ("\\*",       (const char*)ConvertToRegularExpression("*", false, false));
}

// ── Reconcile-first twins (adopted the Windows body) ─────────────────────────

TEST(Utilities, MakeActionHash_Deterministic)
{
    // Same inputs → same hash; different inputs → (almost certainly) different.
    EXPECT_EQ(MakeActionHash("act", "hint", "var"),
              MakeActionHash("act", "hint", "var"));
    EXPECT_NE(MakeActionHash("act", "hint", "var"),
              MakeActionHash("other", "hint", "var"));
}

TEST(Utilities, FixupEscapeSequences_Escapes)
{
    EXPECT_STREQ("\n\t",  (const char*)FixupEscapeSequences("\\n\\t"));
    EXPECT_STREQ("aAb",   (const char*)FixupEscapeSequences("a\\x41b"));  // \x41 == 'A'
    EXPECT_STREQ("q\"q",  (const char*)FixupEscapeSequences("q\\\"q"));
    // parity with Windows: an embedded NUL from \x00 truncates the string
    EXPECT_STREQ("a",     (const char*)FixupEscapeSequences("a\\x00b"));
}

TEST(Utilities, CheckLabel_HonoursBScript)
{
    EXPECT_FALSE(CheckLabel("valid_1", false));  // ok label → not-bad
    EXPECT_TRUE(CheckLabel("", false));          // empty → bad
    EXPECT_TRUE(CheckLabel("1abc", false));      // must start with a letter
    EXPECT_TRUE(CheckLabel("a b", false));       // space not allowed
    // '.' allowed only when bScript (the param the port had dropped)
    EXPECT_TRUE(CheckLabel("a.b", false));       // bad when not a script name
    EXPECT_FALSE(CheckLabel("a.b", true));       // ok when a script name
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

#include <string>
#include <vector>
using std::string;
using std::vector;

// utilities_shared.cpp SeeIfBase64: "If every non-whitespace byte is base64
// and the count is a multiple of 4, decode in place ... might be base64 if
// only base64 characters and mod-4 [length]".
TEST(Utilities, SeeIfBase64_DecodesOnlyMod4Base64)
{
    CString s1("Zm9vYmFy");           // valid base64, mod-4 → decoded
    SeeIfBase64(s1);
    EXPECT_STREQ("foobar", (const char*)s1);

    CString s2("<world>");            // contains '<': plain XML, untouched
    SeeIfBase64(s2);
    EXPECT_STREQ("<world>", (const char*)s2);

    CString s3("Zm9vY");              // base64 chars but not mod-4: untouched
    SeeIfBase64(s3);
    EXPECT_STREQ("Zm9vY", (const char*)s3);

    CString s4("Zm9v YmFy");          // whitespace ignored when counting
    SeeIfBase64(s4);
    EXPECT_STREQ("foobar", (const char*)s4);
}

// utilities_shared.cpp StringToVector: "an empty string ... will give an
// empty vector. However, a non-empty string (with no delimiter) will give
// one item. After that, you get an item per delimiter" — the old portable
// twin drifted to [""] for empty input.
TEST(Utilities, StringToVector_ElementCountContract)
{
    vector<string> v;

    StringToVector("", v, ",", true);
    EXPECT_EQ((size_t)0, v.size());               // empty → []

    StringToVector("a", v, ",", true);
    EXPECT_EQ((size_t)1, v.size());               // no delimiter → ["a"]
    EXPECT_STREQ("a", v[0].c_str());

    StringToVector("a,b", v, ",", true);
    EXPECT_EQ((size_t)2, v.size());               // ["a","b"]
    EXPECT_STREQ("a", v[0].c_str());
    EXPECT_STREQ("b", v[1].c_str());

    StringToVector("a,b,", v, ",", true);
    EXPECT_EQ((size_t)3, v.size());               // trailing delim → ["a","b",""]
    EXPECT_STREQ("", v[2].c_str());
}

// utilities_shared.cpp FixHTMLMultilineString: escapes < > & and tab but
// passes double quotes and newlines through unchanged (unlike FixHTMLString,
// which escapes quotes) — the world-file save format contract.
TEST(Utilities, FixHTMLMultilineString_PreservesQuotesAndNewlines)
{
    EXPECT_STREQ("&lt;a&gt; &amp; b&#9;c",
                 (const char*)FixHTMLMultilineString("<a> & b\tc"));
    // quotes and newlines survive verbatim
    EXPECT_STREQ("say \"hi\"\nline2",
                 (const char*)FixHTMLMultilineString("say \"hi\"\nline2"));
    // contrast: FixHTMLString escapes the quotes
    EXPECT_STREQ("say &quot;hi&quot;",
                 (const char*)FixHTMLString("say \"hi\""));
}

// utilities_shared.cpp ColourToName: the full HTML colour table resolves
// names headless (the old twin knew only 16 names and wrote #RRGGBB).
TEST(Utilities, ColourToNameUsesFullNameTableElseHex)
{
    EXPECT_STREQ("dodgerblue", (const char*)ColourToName(RGB(30, 144, 255)));
    // an off-palette colour falls back to uppercase hex
    EXPECT_STREQ("#010203", (const char*)ColourToName(RGB(1, 2, 3)));
}

// ColourToName's answer for a colour carrying TWO names is pinned, because it
// is the text written into every saved world file: an unpinned answer comes
// from CMapStringToPtr's bucket order, which changes with the hash, the bucket
// count and the table's contents, and re-saving an untouched world then
// rewrites `magenta` as `fuchsia` for no reason anyone would connect to it.
//
// All nine ambiguous RGB values in mxp/mxpinit.cpp are listed, which is what
// makes this a regression test rather than a sample: the three pairs 5.06
// already shipped (aqua/cyan, fuchsia/magenta, lightgray/lightgrey) keep 5.06's
// spelling, and the six -grey spellings added in 5.07 answer -gray, the only
// spelling that existed when those colours were last written to a file.
//
// Mutating kCanonicalColourNames — dropping an entry, or flipping one to its
// alias — must fail here.  Dropping one falls back to the alphabetically first
// match, which differs from the canonical answer for every pair below except
// darkslategray/darkslategrey and the five other -gray/-grey pairs where -gray
// already sorts first; those four rows (cyan, magenta, lightgrey, and the
// alphabetical control) are the ones that catch it.
TEST(Utilities, ColourToNameIsDeterministicForColoursWithTwoNames)
{
    // 5.06 shipped both spellings of these three and wrote the second one
    EXPECT_STREQ("cyan",     (const char*)ColourToName(RGB(0x00, 0xFF, 0xFF)));
    EXPECT_STREQ("magenta",  (const char*)ColourToName(RGB(0xFF, 0x00, 0xFF)));
    EXPECT_STREQ("lightgrey",(const char*)ColourToName(RGB(0xD3, 0xD3, 0xD3)));

    // the six whose -grey spelling arrived in 5.07: -gray is what any existing
    // world file carries
    EXPECT_STREQ("gray",          (const char*)ColourToName(RGB(0x80, 0x80, 0x80)));
    EXPECT_STREQ("darkgray",      (const char*)ColourToName(RGB(0xA9, 0xA9, 0xA9)));
    EXPECT_STREQ("dimgray",       (const char*)ColourToName(RGB(0x69, 0x69, 0x69)));
    EXPECT_STREQ("slategray",     (const char*)ColourToName(RGB(0x70, 0x80, 0x90)));
    EXPECT_STREQ("darkslategray", (const char*)ColourToName(RGB(0x2F, 0x4F, 0x4F)));
    EXPECT_STREQ("lightslategray",(const char*)ColourToName(RGB(0x77, 0x88, 0x99)));

    // and the rule for a colour NOT in the canonical list is alphabetical, not
    // bucket order — a single-name colour is unaffected either way, so this is
    // the control that shows the walk still finds a name at all
    EXPECT_STREQ("dodgerblue", (const char*)ColourToName(RGB(30, 144, 255)));
}

// doc_portable.cpp GetUniqueID: "24 lowercase hex chars, byte-for-byte the
// output shape of the CoCreateGuid-based original" — and consecutive calls
// differ.
TEST(Utilities, GetUniqueIDIsTwentyFourLowercaseHexAndUnique)
{
    CString a = GetUniqueID();
    CString b = GetUniqueID();
    ASSERT_EQ(24, a.GetLength());
    ASSERT_EQ(24, b.GetLength());
    for (int i = 0; i < 24; i++) {
        char c = ((const char*)a)[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    EXPECT_TRUE(a != b);
}

// utilities_shared.cpp Make_Absolute_Path: "if the filename starts with ./
// or .\ then drop it, otherwise we get junk like c:\mushclient\.\logs\..."
// (headless working_dir is empty, so relative stays relative).
TEST(Utilities, MakeAbsolutePathStripsLeadingDotSlash)
{
    EXPECT_STREQ("logs/x.txt", Make_Absolute_Path("./logs/x.txt"));
    EXPECT_STREQ("logs\\x.txt", Make_Absolute_Path(".\\logs\\x.txt"));
    // absolute paths are untouched
    EXPECT_STREQ("/a/b", Make_Absolute_Path("/a/b"));
    EXPECT_STREQ("c:x",  Make_Absolute_Path("c:x"));
}

// utilities_shared.cpp ExtractDirectory: "On POSIX paths use '/' but also
// accept '\' in case of cross-platform world files ... if no slash: './'".
TEST(Utilities, ExtractDirectoryAcceptsBackslashSeparators)
{
    EXPECT_STREQ("plugins\\images\\",
                 (const char*)ExtractDirectory("plugins\\images\\x.png"));
    EXPECT_STREQ("plugins/images/",
                 (const char*)ExtractDirectory("plugins/images/x.png"));
    // the LATER of the two separator kinds wins in a mixed path
    EXPECT_STREQ("a\\b/", (const char*)ExtractDirectory("a\\b/c.png"));
    EXPECT_STREQ("./", (const char*)ExtractDirectory("noslash.png"));
}

// ── DefaultWorldFileName ────────────────────────────────────────────────────
// The name Save-As offers for a world that has never been saved.  Table-driven
// because the rule is three independent steps (scrub, trim, ensure suffix) and
// each has an edge that only shows up in combination with the others.
TEST(Utilities, DefaultWorldFileNameScrubsTrimsAndSuffixes)
{
    static const struct { const char* name; const char* want; } kCases[] = {
        // ordinary names just gain the suffix
        { "Discworld",            "Discworld.mcl" },
        { "Discworld.mcl",        "Discworld.mcl" },     // already suffixed
        { "Discworld.MCL",        "Discworld.MCL" },     // ...case-insensitively
        { "Discworld.mclx",       "Discworld.mclx.mcl" },// only an exact tail counts
        { "mcl",                  "mcl.mcl" },           // shorter than the suffix

        // nothing usable left → the generic fallback, never an empty filename
        { "",                     "world.mcl" },
        { "   ",                  "world.mcl" },         // whitespace only
        { "<>\"|?:#%;/\\",        "world.mcl" },         // forbidden chars only
        { "  <>|  ",              "world.mcl" },         // ...and after trimming

        // every character of the forbidden set is stripped, not replaced
        { "a<b>c\"d|e?f:g#h%i;j/k\\l", "abcdefghijkl.mcl" },

        // THE case that matters: '/' is a path separator, so an unscrubbed name
        // would point the save dialog at a directory the user never chose.
        { "Discworld/2",          "Discworld2.mcl" },
        { "/etc/passwd",          "etcpasswd.mcl" },
        { "..\\..\\evil",         "....evil.mcl" },      // separators gone; dots are legal

        // trimming happens AFTER the scrub, so a stripped separator cannot
        // leave a leading or trailing space in the filename
        { "  Mid Space  ",        "Mid Space.mcl" },
        { "/ Discworld /",        "Discworld.mcl" },
    };

    for (const auto& c : kCases)
        EXPECT_STREQ(c.want, (const char*) DefaultWorldFileName(c.name));
}

// Whatever it returns must be a bare filename — no separator survives — or the
// port's save hooks, which join it to a directory themselves, would escape that
// directory.  Asserted over the table rather than per-row so a new row cannot
// quietly opt out.
TEST(Utilities, DefaultWorldFileNameNeverContainsAPathSeparator)
{
    const char* kNames[] = {
        "a/b", "a\\b", "/", "\\", "//../..//", "world/../../etc/passwd",
        "plain", "", "   ",
    };
    for (const char* n : kNames) {
        CString got = DefaultWorldFileName(n);
        EXPECT_EQ(-1, got.Find('/'));
        EXPECT_EQ(-1, got.Find('\\'));
        EXPECT_FALSE(got.IsEmpty());
    }
}

// utilities_shared.cpp: the CommandIDs[] lookups are case-insensitive and
// round-trip; unknown name → 0, ID 0 → "".
extern tCommandIDMapping CommandIDs[];

TEST(Utilities, CommandIDLookupIsCaseInsensitiveRoundTrip)
{
    int checked = 0, roundtripped = 0;
    for (int i = 0; CommandIDs[i].iCommandID; i++) {
        const int id = CommandIDs[i].iCommandID;
        CString name = CommandIDs[i].sCommandName;
        // round-trip (first table entry with this ID wins)
        if (CommandIDToString(id) == name) {
            EXPECT_EQ(id, StringToCommandID(name));
            roundtripped++;
        }
        // case-insensitive
        CString upper = name; upper.MakeUpper();
        EXPECT_EQ(StringToCommandID(name), StringToCommandID(upper));
        checked++;
    }
    EXPECT_GT(checked, 100);                       // the table is substantial
    EXPECT_GT(roundtripped, 100);                  // round-trips actually ran
    EXPECT_EQ(0, StringToCommandID("NoSuchCommandName"));
    EXPECT_STREQ("", (const char*)CommandIDToString(0));
}

// doc_portable.cpp AllocateLinePositions: "Trim FIRST so the index can never
// be smaller than the list it indexes" — after lowering m_maxlines the list
// is trimmed before the new index is built, so entries never point past it.
TEST(Utilities, AllocateLinePositionsTrimsListBeforeRebuildingIndex)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    char buf[32];
    for (int i = 1; i <= 350; i++) {
        snprintf(buf, sizeof buf, "line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }
    ASSERT_EQ(351, (int)doc.m_LineList.GetCount());   // 350 done + 1 pending

    doc.m_maxlines = 200;
    doc.AllocateLinePositions();

    // trimmed in JUMP_SIZE chunks until <= m_maxlines: 351 → 251 → 151
    EXPECT_EQ(151, (int)doc.m_LineList.GetCount());

    // the rebuilt index resolves lines correctly (head is now "line 201")
    VARIANT v = doc.GetLineInfo(1, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    EXPECT_STREQ("line 201", (const char*)CString(v.bstrVal));
    VariantClear(&v);
    v = doc.GetLineInfo(150, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    EXPECT_STREQ("line 350", (const char*)CString(v.bstrVal));
    VariantClear(&v);
}

// The eviction path, which is what DropFirstChunk replaced.  RemoveChunk drops
// JUMP_SIZE lines off the head, so chunk 1 becomes chunk 0 and so on; the array
// did that by copying every slot down one, the deque by popping its front.  A
// renumbering error here would not crash — GetLinePosition would start from the
// WRONG chunk head and walk forward, silently returning a line off by a
// multiple of 100, which world.GetLineInfo and every Recall/Find would inherit.
TEST(Utilities, LinePositionsSurviveRepeatedEviction)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 200;                 // evict early and often
    doc.AllocateLinePositions();

    char buf[32];
    for (int i = 1; i <= 500; i++) {      // ~4 evictions
        snprintf(buf, sizeof buf, "line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }

    const long count = (long) doc.m_LineList.GetCount();
    ASSERT_TRUE(count <= 201);            // trimmed, plus the in-progress line
    const long committed = count - 1;

    // The head is whatever survived; read it, then check every line resolves to
    // its own text — walking each in turn is what proves the chunk heads line
    // up, since a stale head only shows on lines past the first chunk.
    VARIANT v = doc.GetLineInfo(1, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    const CString firstText = CString(v.bstrVal);
    VariantClear(&v);
    long firstNum = 0;
    ASSERT_EQ(1, sscanf((const char*)firstText, "line %ld", &firstNum));

    for (long i = 1; i <= committed; ++i) {
        v = doc.GetLineInfo(i, 1);
        ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
        snprintf(buf, sizeof buf, "line %ld", firstNum + i - 1);
        EXPECT_STREQ(buf, (const char*)CString(v.bstrVal));
        VariantClear(&v);
    }
    EXPECT_EQ(500L, firstNum + committed - 1);   // ...and the last is the newest
}

// ── Unlimited scrollback actually keeps everything ──────────────────────────
//
// m_maxlines == 0 is the OPT_ZERO_UNLIMITED sentinel.  The trim in StartNewLine
// is the only thing that discards lines, so this pins that it is skipped — and,
// with the counterpart below, that a real cap still evicts.  Without both arms
// a build that never trims, or one that always does, passes half of this.
TEST(Utilities, UnlimitedScrollbackNeverEvicts)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 0;                    // unlimited
    doc.AllocateLinePositions();

    char buf[32];
    const int kLines = 1200;               // well past several JUMP_SIZE chunks
    for (int i = 1; i <= kLines; i++) {
        snprintf(buf, sizeof buf, "line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }

    EXPECT_EQ((long)kLines + 1, (long)doc.m_LineList.GetCount());  // +in-progress

    // ...and the jump index still resolves the oldest and newest lines, which
    // is what would break if the index had kept sizing itself from the cap.
    VARIANT v = doc.GetLineInfo(1, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    EXPECT_STREQ("line 1", (const char*)CString(v.bstrVal));
    VariantClear(&v);
    v = doc.GetLineInfo(kLines, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    snprintf(buf, sizeof buf, "line %d", kLines);
    EXPECT_STREQ(buf, (const char*)CString(v.bstrVal));
    VariantClear(&v);
}

TEST(Utilities, ABoundedCapStillEvicts)
{
    // The control for the test above.
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 300;
    doc.AllocateLinePositions();

    char buf[32];
    for (int i = 1; i <= 1200; i++) {
        snprintf(buf, sizeof buf, "line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }
    EXPECT_TRUE((long)doc.m_LineList.GetCount() <= 301);
}

TEST(Utilities, UnlimitedScrollbackAppendRateStaysFlat)
{
    // A complexity pin.  With the old fixed index, "unlimited" was unreachable;
    // now that it is, the thing that would quietly ruin it is an O(N) step on
    // the append path — the index shuffle used to be exactly that, at O(cap /
    // JUMP_SIZE) per eviction.  Time the first block of lines against a much
    // later one: the ratio, not the absolute number, is the assertion.
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 0;
    doc.AllocateLinePositions();

    char buf[48];
    auto block = [&](int n) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) {
            snprintf(buf, sizeof buf, "some ordinary mud output line\n");
            doc.DisplayMsg(buf, (int)strlen(buf), 0);
        }
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    const double first = block(5000);
    block(40000);                       // get well past any early-growth effects
    const double later = block(5000);

    // Loose by an order of magnitude against the regression it guards (an O(N)
    // append at 50,000 lines would be ~10x the cost at 5,000, and worse beyond).
    EXPECT_TRUE(later <= first * 4.0 + 20.0);
    if (later > first * 4.0 + 20.0)
        printf("    append of 5000 lines: at start %.1fms, at 50k %.1fms\n",
               first, later);
}

// ── Coverage gaps from the 2026-07 test audit, section 1 ──────────────────
//
// Each of these is a documented data-integrity path with real logic and no test
// touching it: a wrong mapping would ship silently.

TEST(Utilities, Base64CoversTheWholeAlphabet)
{
    // getBase64Value's '+' (62) and '/' (63) arms were True:0 across the entire
    // suite -- the RFC-4648 vectors and the round-trip samples happen to produce
    // no + or / in their output, so a transposed 62<->63 mapping was invisible.
    // 0xFB 0xFF 0xFE encodes as "+//+", which uses both, twice, in both halves.
    EXPECT_STREQ("+//+", (const char*) encodeBase64("\xFB\xFF\xFE", false));

    CString decoded = decodeBase64("+//+");
    ASSERT_EQ(3, decoded.GetLength());
    EXPECT_EQ('\xFB', decoded[0]);
    EXPECT_EQ('\xFF', decoded[1]);
    EXPECT_EQ('\xFE', decoded[2]);

    // ...and the two are genuinely distinguished, which is what a swapped
    // mapping would break while still round-tripping within itself.
    EXPECT_TRUE(decodeBase64("+AAA") != decodeBase64("/AAA"));
}

TEST(Utilities, FixupEscapeSequencesCoversEveryEscape)
{
    // Eight of the simple escapes were True:0 -- only \n, \t and \x41/\x00 were
    // ever exercised -- so a wrong mapping in any of the others would ship.
    // world.FixupEscapeSequences is what an alias or a send string goes through.
    const CString s = FixupEscapeSequences("[\\a][\\b][\\f][\\r][\\v][\\'][\\\"][\\\\][\\?]");
    ASSERT_EQ(27, s.GetLength());          // 9 groups of [x]
    EXPECT_EQ('\a',  s[1]);
    EXPECT_EQ('\b',  s[4]);
    EXPECT_EQ('\f',  s[7]);
    EXPECT_EQ('\r',  s[10]);
    EXPECT_EQ('\v',  s[13]);
    EXPECT_EQ('\'',  s[16]);
    EXPECT_EQ('"',   s[19]);
    EXPECT_EQ('\\',  s[22]);
    EXPECT_EQ('?',   s[25]);
}

TEST(Utilities, FixupEscapeSequencesHexAcceptsLetters)
{
    // The `if (iNewDigit >= 'A') iNewDigit -= 7;` fold never ran: the only \x
    // test used \x41 and \x00, both all-digits.  That -7 is the gap between '9'
    // and 'A' in ASCII, and getting it wrong turns every hex letter into
    // garbage -- \xFF would come out as 0x99.
    const CString s = FixupEscapeSequences("\\xFF|\\xab|\\x7f");
    ASSERT_EQ(5, s.GetLength());
    EXPECT_EQ('\xFF', s[0]);
    EXPECT_EQ('|',    s[1]);
    EXPECT_EQ('\xAB', s[2]);   // lower case too (toupper first)
    EXPECT_EQ('\x7F', s[4]);
}

TEST(Utilities, ConvertToRegularExpressionEscapesControlCharacters)
{
    // The c < ' ' arm (\xhh) and the \n special case were both untested; only
    // "abc", "a*b" and "*" were ever converted.  These are what a trigger's
    // match text becomes, so a wrong escape silently changes what matches.
    EXPECT_STREQ("^a\\nb$",
                 (const char*) ConvertToRegularExpression("a\nb", true, true));
    EXPECT_STREQ("^a\\x01b$",
                 (const char*) ConvertToRegularExpression("a\x01" "b", true, true));
    EXPECT_STREQ("^a\\x1bb$",
                 (const char*) ConvertToRegularExpression("a\x1b" "b", true, true));
}

TEST(Utilities, ConvertToRegularExpressionPassesHighBytesThrough)
{
    // c >= 0x80 goes through unescaped so a UTF-8 trigger keeps matching; the
    // branch was True:0.  A byte-by-byte escape here would break every
    // non-English world.
    EXPECT_STREQ("^caf\xc3\xa9$",
                 (const char*) ConvertToRegularExpression("caf\xc3\xa9", true, true));
    // ...and the ASCII punctuation around it is still escaped.
    EXPECT_STREQ("^\\.caf\xc3\xa9\\.$",
                 (const char*) ConvertToRegularExpression(".caf\xc3\xa9.", true, true));
}

// ── DeleteHeadLines: the primitive behind the macOS clear commands ─────────
//
// RemoveChunk is fixed at JUMP_SIZE, so there was no way to drop an arbitrary
// number of the OLDEST lines.  Clear Scrollback (everything above the first
// visible line) and Clear to Previous Mark (everything above the last user
// input) are both that operation with different cut points, and both are the
// only clear that genuinely returns memory on a long unlimited session --
// Clear Output Buffer frees everything, Clear Screen frees nothing.
TEST(Utilities, DeleteHeadLinesDropsTheOldestAndRenumbers)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 0;                      // unlimited: nothing else may trim
    doc.AllocateLinePositions();

    char buf[32];
    for (int i = 1; i <= 500; i++) {
        snprintf(buf, sizeof buf, "line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }
    ASSERT_EQ(501, (int)doc.m_LineList.GetCount());   // 500 done + the in-progress one

    doc.DeleteHeadLines(200);
    EXPECT_EQ(301, (int)doc.m_LineList.GetCount());

    // The survivors keep their text and are addressable through the rebuilt
    // jump index -- an index left stale here does not crash, it silently
    // returns a line a multiple of 100 away.
    VARIANT v = doc.GetLineInfo(1, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    EXPECT_STREQ("line 201", (const char*)CString(v.bstrVal));
    VariantClear(&v);
    v = doc.GetLineInfo(300, 1);
    ASSERT_EQ((unsigned short)VT_BSTR, v.vt);
    EXPECT_STREQ("line 500", (const char*)CString(v.bstrVal));
    VariantClear(&v);
}

TEST(Utilities, DeleteHeadLinesNeverTakesTheInProgressLine)
{
    // The in-progress line is the list's tail and the drain has not emitted it,
    // so taking it would strand the view's pending prompt row.  Asking for more
    // than there are clamps to the committed count.
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 0;
    doc.AllocateLinePositions();

    doc.DisplayMsg("one\ntwo\n", 8, 0);
    doc.DisplayMsg("a prompt with no newline", 24, 0);
    const int before = (int)doc.m_LineList.GetCount();
    EXPECT_EQ(3, before);                    // two committed + the prompt

    doc.DeleteHeadLines(9999);
    EXPECT_EQ(1, (int)doc.m_LineList.GetCount());   // only the prompt survives
    EXPECT_TRUE(doc.m_pCurrentLine != NULL);
    EXPECT_EQ(24, doc.m_pCurrentLine->len);

    doc.DeleteHeadLines(5);                  // nothing committed left to take
    EXPECT_EQ(1, (int)doc.m_LineList.GetCount());
    doc.DeleteHeadLines(0);
    doc.DeleteHeadLines(-1);
    EXPECT_EQ(1, (int)doc.m_LineList.GetCount());
}

// ── CountLinesBeforePromptBlock: the cut point for "Clear to Start" ────────
//
// Clear to Start scrolls the current prompt to the top of the window by
// throwing away everything above it.  The block that survives is the line the
// last IAC GA/EOR stamped, its soft-wrap group, and any immediately preceding
// line that was itself stamped -- with a fallback to the last line when the
// server never sends GA, which is most of them.

static void make_buffer_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_maxlines = 0;                      // unlimited: nothing else may trim
    doc.AllocateLinePositions();
}

static void mud_text(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

// One IAC GA, followed by whatever character Phase_IAC substituted -- which is
// how the real caller behaves, and is what makes convert_ga_to_newline
// actually terminate the line.
static void mud_ga(CMUSHclientDoc& doc)
{
    unsigned char c = GO_AHEAD;
    doc.Phase_IAC(c);
    if (c) {
        char ch = (char) c;
        doc.DisplayMsg(&ch, 1, 0);
    }
}

// Text of every line still in the buffer, joined with '|'.
static std::string buffer_text(CMUSHclientDoc& doc)
{
    std::string out;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (!out.empty()) out += '|';
        out.append(l->text, l->len);
    }
    return out;
}

// Every line's text, concatenated -- a wrap group reassembled.
static std::string buffer_joined(CMUSHclientDoc& doc)
{
    std::string out;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        out.append(l->text, l->len);
    }
    return out;
}

TEST(Utilities, ClearToStartWithNoPromptKeepsTheLastLine)
{
    // No GA ever arrived, so there is no known prompt.  The specified
    // fallback is "everything but the last line" -- and the empty in-progress
    // line does not count as that last line, or the window would end up blank.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    mud_text(doc, "one\ntwo\nthree\n");
    ASSERT_EQ(4, (int)doc.m_LineList.GetCount());   // 3 committed + in-progress
    EXPECT_EQ(0L, doc.m_last_prompt_line);

    EXPECT_EQ(2L, doc.CountLinesBeforePromptBlock());
    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("three|", buffer_text(doc).c_str());
}

TEST(Utilities, ClearToStartWithNoPromptKeepsTheWholeWrapGroup)
{
    // A wrapped line is several CLines; keeping only the last would show the
    // user its tail fragment.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_wrap = TRUE;
    doc.m_nWrapColumn = 10;
    mud_text(doc, "first\n");
    mud_text(doc, "AAAA BBBB CCCC DDDD\n");
    ASSERT_GT((int)doc.m_LineList.GetCount(), 3);   // it really did wrap

    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());

    // "first" is gone; every segment of the long line survived, in order.
    EXPECT_STREQ("AAAA BBBB CCCC DDDD", buffer_joined(doc).c_str());
}

TEST(Utilities, ClearToStartKeepsTheStampedPromptEvenWithOutputBelowIt)
{
    // The point of the PROMPT flag: once the server has sent something after
    // the prompt, "the last line" is no longer the prompt, and only the stamp
    // can find it.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_bConvertGAtoNewline = 1;         // so the prompt becomes a real line

    mud_text(doc, "one\ntwo\n");
    mud_text(doc, "HP:100 > ");
    mud_ga(doc);
    mud_text(doc, "A raven flies overhead.\n");

    ASSERT_EQ(5, (int)doc.m_LineList.GetCount());
    EXPECT_EQ(2L, doc.CountLinesBeforePromptBlock());

    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("HP:100 > |A raven flies overhead.|", buffer_text(doc).c_str());
}

TEST(Utilities, ClearToStartKeepsEverySegmentOfAWrappedPrompt)
{
    // Only the LAST segment carries PROMPT (the GA arrives after the text), so
    // the block has to be extended backwards over the wrap group.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_bConvertGAtoNewline = 1;
    doc.m_wrap = TRUE;
    doc.m_nWrapColumn = 10;

    mud_text(doc, "one\ntwo\n");
    mud_text(doc, "HP:100 MP:50 EXP:25 > ");
    mud_ga(doc);
    mud_text(doc, "A raven flies overhead.\n");

    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("HP:100 MP:50 EXP:25 > A raven flies overhead.",
                 buffer_joined(doc).c_str());
}

TEST(Utilities, ClearToStartKeepsConsecutivePromptLinesAsOneBlock)
{
    // Telnet marks the END of a prompt and never the start, so a prompt
    // spanning several HARD lines is identifiable only when the server sends a
    // GA after each of them.  When it does, all of them are kept.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_bConvertGAtoNewline = 1;

    mud_text(doc, "one\ntwo\n");
    mud_text(doc, "HP: 100");   mud_ga(doc);
    mud_text(doc, "Mana: 50");  mud_ga(doc);
    mud_text(doc, "> ");        mud_ga(doc);

    EXPECT_EQ(2L, doc.CountLinesBeforePromptBlock());
    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("HP: 100|Mana: 50|> |", buffer_text(doc).c_str());
}

TEST(Utilities, ClearToStartFallsBackWhenThePromptHasBeenEvicted)
{
    // m_last_prompt_line names a line by number.  Once RemoveChunk or an
    // earlier clear has dropped that line the number is a phantom, and the
    // head's own number is what proves it -- an O(1) test, so a stale value
    // cannot send the search down the whole buffer.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_bConvertGAtoNewline = 1;

    mud_text(doc, "HP:100 > ");
    mud_ga(doc);
    const long stamped = doc.m_last_prompt_line;
    ASSERT_GT(stamped, 0L);

    mud_text(doc, "alpha\nbeta\ngamma\n");
    doc.DeleteHeadLines(1);                  // takes the prompt itself
    EXPECT_EQ(stamped, doc.m_last_prompt_line);          // still recorded...
    EXPECT_GT(doc.m_LineList.GetHead()->m_nLineNumber, stamped);  // ...but gone

    EXPECT_EQ(2L, doc.CountLinesBeforePromptBlock());    // fallback: last line
    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("gamma|", buffer_text(doc).c_str());
}

TEST(Utilities, ClearOutputForgetsTheRememberedPromptAndGaLine)
{
    // ClearOutput restarts the numbering at zero, so a line number carried
    // over from before names a line that cannot exist.  Upstream leaves
    // m_last_line_with_IAC_GA stale and world.GetInfo(289) reports the
    // phantom; see docs/UPSTREAM-BUGS.md #11.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_bConvertGAtoNewline = 1;

    mud_text(doc, "one\ntwo\nHP:100 > ");
    mud_ga(doc);
    ASSERT_GT(doc.m_last_prompt_line, 0L);
    ASSERT_GT(doc.m_last_line_with_IAC_GA, 0L);

    doc.ClearOutput();

    EXPECT_EQ(0L, doc.m_last_prompt_line);
    EXPECT_EQ(0L, doc.m_last_line_with_IAC_GA);

    // and the fallback is what applies from here on
    mud_text(doc, "alpha\nbeta\n");
    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("beta|", buffer_text(doc).c_str());
}

// ── CountLinesBeforeLastUserInput: the cut point for "Clear to Previous Mark"
//
// Terminal.app's mark is a shell prompt, drawn from shell integration.  The
// MUD equivalent is already in the buffer: the echoed command, flagged
// USER_INPUT by the send path (evaluate.cpp:928).

static void user_typed(CMUSHclientDoc& doc, const char* s)
{
    std::string line(s);
    line += '\n';
    doc.DisplayMsg(line.c_str(), (int)line.size(), USER_INPUT);
}

TEST(Utilities, ClearToPreviousMarkKeepsTheLastCommandAndEverythingSince)
{
    CMUSHclientDoc doc; make_buffer_doc(doc);
    mud_text(doc, "You are in a forest.\n");
    user_typed(doc, "north");
    mud_text(doc, "You walk north.\n");
    user_typed(doc, "look");
    mud_text(doc, "A clearing.\nA raven watches.\n");

    EXPECT_EQ(3L, doc.CountLinesBeforeLastUserInput());
    doc.DeleteHeadLines(doc.CountLinesBeforeLastUserInput());
    EXPECT_STREQ("look|A clearing.|A raven watches.|", buffer_text(doc).c_str());
}

TEST(Utilities, ClearToPreviousMarkStopsAtTheLastMarkNotAnEarlierOne)
{
    // Two commands back to back: the cut is at the LATER one, which is what
    // "clear to the previous mark" means -- an implementation that ran the
    // wrap-group walk across preceding USER_INPUT lines would keep both.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    mud_text(doc, "intro\n");
    user_typed(doc, "north");
    user_typed(doc, "look");
    mud_text(doc, "A clearing.\n");

    doc.DeleteHeadLines(doc.CountLinesBeforeLastUserInput());
    EXPECT_STREQ("look|A clearing.|", buffer_text(doc).c_str());
}

TEST(Utilities, ClearToPreviousMarkKeepsAWrappedCommandWhole)
{
    CMUSHclientDoc doc; make_buffer_doc(doc);
    doc.m_wrap = TRUE;
    doc.m_nWrapColumn = 10;
    mud_text(doc, "intro\n");
    user_typed(doc, "say something rather long");
    mud_text(doc, "You say it.\n");

    doc.DeleteHeadLines(doc.CountLinesBeforeLastUserInput());
    EXPECT_STREQ("say something rather longYou say it.",
                 buffer_joined(doc).c_str());
}

TEST(Utilities, ClearToPreviousMarkDoesNothingWithNoMark)
{
    // A fresh connection, or anything after a Clear Output Buffer.  Guessing at
    // a cut point here would throw away exactly the output not yet read.
    CMUSHclientDoc doc; make_buffer_doc(doc);
    mud_text(doc, "one\ntwo\nthree\n");

    EXPECT_EQ(0L, doc.CountLinesBeforeLastUserInput());
    doc.DeleteHeadLines(doc.CountLinesBeforeLastUserInput());
    EXPECT_STREQ("one|two|three|", buffer_text(doc).c_str());
}

TEST(Utilities, ClearToStartOnAnEmptyOrSingleLineBufferDoesNothing)
{
    CMUSHclientDoc doc; make_buffer_doc(doc);
    EXPECT_EQ(0L, doc.CountLinesBeforePromptBlock());     // no lines at all

    mud_text(doc, "solitary");                            // one, in progress
    EXPECT_EQ(1, (int)doc.m_LineList.GetCount());
    EXPECT_EQ(0L, doc.CountLinesBeforePromptBlock());

    doc.DeleteHeadLines(doc.CountLinesBeforePromptBlock());
    EXPECT_STREQ("solitary", buffer_text(doc).c_str());
}

// doc_portable.cpp GetLinePosition: "sanity check (Santa Claus): nLine >// doc_portable.cpp GetLinePosition: "sanity check (Santa Claus): nLine >
// count → clamped, nLine < 0 → 0; ... not there? assume last one in the
// list" — out-of-range line numbers always yield a valid POSITION.
TEST(Utilities, GetLinePositionClampsOutOfRangeLineNumbers)
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.DisplayMsg("one\ntwo\nthree\n", 14, 0);

    POSITION head = doc.GetLinePosition(-5);       // negative → head
    ASSERT_TRUE(head != NULL);
    EXPECT_TRUE(doc.m_LineList.GetAt(head) == doc.m_LineList.GetHead());

    POSITION tail = doc.GetLinePosition(9999);     // beyond count → tail
    ASSERT_TRUE(tail != NULL);
    EXPECT_TRUE(doc.m_LineList.GetAt(tail) == doc.m_LineList.GetTail());
}

// ── Double Metaphone (Dmetaph.cpp) ──────────────────────────────────────────
// MString::DoubleMetaphone is the canonical Lawrence Philips implementation;
// expected codes below are hand-derived from the branch logic in Dmetaph.cpp
// (cited per test).  maxlength is 4 unless a test says otherwise (matching the
// classic usage).  metaph2 is only assigned when an alternate pronunciation
// was recorded, so a sentinel detects "no alternate".

#include "dmetaph.h"

struct DM {
    CString p, s;
    bool alt;           // true if an alternate (secondary) code was produced
    explicit DM(const char* word, int max = 4)
        : p("<unset>"), s("<unset>")
    {
        MString m(word, max);
        m.DoubleMetaphone(p, s);
        alt = !(s == "<unset>");
        if (!alt) s = "";
    }
};

TEST(Metaphone, EmptyWordLeavesOutputsUntouched)
{
    // DoubleMetaphone returns before assigning anything for a 0-length input.
    DM d("");
    EXPECT_STREQ("<unset>", (const char*)d.p);
    EXPECT_FALSE(d.alt);
}

TEST(Metaphone, InputIsUppercasedFirst)
{
    // MakeUpper() runs before the main loop: case must not matter.
    DM lower("smith"), upper("SMITH");
    EXPECT_STREQ((const char*)upper.p, (const char*)lower.p);
    EXPECT_STREQ((const char*)upper.s, (const char*)lower.s);
}

TEST(Metaphone, InitialSilentClustersAreSkipped)
{
    // GN/KN/PN/WR/PS at position 0 advance past the first letter.
    EXPECT_STREQ("NT", (const char*)DM("KNIGHT").p);   // silent K, silent GH after I
    EXPECT_STREQ("RT", (const char*)DM("WRIGHT").p);   // silent W
    EXPECT_STREQ("NX", (const char*)DM("GNOCCHI").p);  // silent G; CCH → X (Italian)
}

TEST(Metaphone, InitialXSoundsAsS)
{
    DM d("XAVIER");
    EXPECT_STREQ("SF", (const char*)d.p);
    EXPECT_STREQ("SFR", (const char*)d.s);   // French -IER final R alternate
}

TEST(Metaphone, InitialVowelsMapToA_LaterVowelsDropped)
{
    EXPECT_STREQ("AT", (const char*)DM("OTTO").p);
    EXPECT_FALSE(DM("OTTO").alt);
}

TEST(Metaphone, AnglicisedSVariants)
{
    // initial S before M/N/L/W → S with X alternate ('smith' matches 'schmidt')
    DM smith("SMITH");
    EXPECT_STREQ("SM0", (const char*)smith.p);
    EXPECT_STREQ("XMT", (const char*)smith.s);
    DM snider("SNIDER");
    EXPECT_STREQ("SNTR", (const char*)snider.p);
    EXPECT_STREQ("XNTR", (const char*)snider.s);
    // SCH- + consonant at word start → X primary, S secondary
    DM schmidt("SCHMIDT");
    EXPECT_STREQ("XMT", (const char*)schmidt.p);
    EXPECT_STREQ("SMT", (const char*)schmidt.s);
}

TEST(Metaphone, CFamilyBranches)
{
    EXPECT_STREQ("SSR",  (const char*)DM("CAESAR").p);    // special-cased caesar
    EXPECT_STREQ("KNT",  (const char*)DM("CHIANTI").p);   // Italian CHIA → K
    EXPECT_STREQ("KMST", (const char*)DM("CHEMISTRY").p); // Greek root CHEM → K
    EXPECT_STREQ("XR",   (const char*)DM("CHORE").p);     // CHORE excluded from Greek rule
    EXPECT_STREQ("MK",   (const char*)DM("MCHUGH").p);    // McX → K; Parker's rule eats GH
    EXPECT_STREQ("FKX",  (const char*)DM("FOCACCIA").p);  // -CIA → X
    EXPECT_STREQ("SNS",  (const char*)DM("SCIENCE").p);   // SC+I → S, CE → S
    DM michael("MICHAEL");                                // -CHAE- → K/X
    EXPECT_STREQ("MKL", (const char*)michael.p);
    EXPECT_STREQ("MXL", (const char*)michael.s);
    DM arch("ARCH");                                      // plain arch → X/K
    EXPECT_STREQ("ARX", (const char*)arch.p);
    EXPECT_STREQ("ARK", (const char*)arch.s);
    EXPECT_STREQ("ARKT", (const char*)DM("ARCHITECT").p); // ARCHIT- exception → K
    EXPECT_STREQ("ARKS", (const char*)DM("ORCHESTRA").p); // ORCHES- exception → K
    DM czerny("CZERNY");                                  // CZ → S/X
    EXPECT_STREQ("SRN", (const char*)czerny.p);
    EXPECT_STREQ("XRN", (const char*)czerny.s);
    EXPECT_STREQ("AKST", (const char*)DM("ACCIDENT").p);  // A+CC+I → KS
    EXPECT_STREQ("MKLL", (const char*)DM("MCCLELLAN").p); // McC exception: plain K
}

TEST(Metaphone, DFamilyBranches)
{
    EXPECT_STREQ("AJ",   (const char*)DM("EDGE").p);   // DG+E → J
    EXPECT_STREQ("ATKR", (const char*)DM("EDGAR").p);  // DG+consonant → TK
    EXPECT_STREQ("TM",   (const char*)DM("DUMB").p);   // -UMB final: silent B
}

TEST(Metaphone, GFamilyBranches)
{
    EXPECT_STREQ("H",  (const char*)DM("HUGH").p);     // Parker's rule: silent GH
    EXPECT_STREQ("LF", (const char*)DM("LAUGH").p);    // -UGH after L → F
    EXPECT_STREQ("KF", (const char*)DM("COUGH").p);    // -UGH after C → F
    EXPECT_STREQ("JRTL", (const char*)DM("GHIRADELLI").p); // initial GHI → J
    DM agnes("AGNES");                                 // vowel+GN → KN/N
    EXPECT_STREQ("AKNS", (const char*)agnes.p);
    EXPECT_STREQ("ANS",  (const char*)agnes.s);
    EXPECT_STREQ("KKN", (const char*)DM("CAGNEY").p);  // -GNEY keeps the G
    DM signed_("SIGNED");                              // medial GN → N/KN
    EXPECT_STREQ("SNT",  (const char*)signed_.p);
    EXPECT_STREQ("SKNT", (const char*)signed_.s);
    DM tagliaro("TAGLIARO");                           // -GLI- Italian → KL/L
    EXPECT_STREQ("TKLR", (const char*)tagliaro.p);
    EXPECT_STREQ("TLR",  (const char*)tagliaro.s);
    DM german("GERMAN");                               // initial G+ER → K/J
    EXPECT_STREQ("KRMN", (const char*)german.p);
    EXPECT_STREQ("JRMN", (const char*)german.s);
    DM danger("DANGER");                               // DANGER excluded from -GER rule → J/K
    EXPECT_STREQ("TNJR", (const char*)danger.p);
    EXPECT_STREQ("TNKR", (const char*)danger.s);
    DM biaggi("BIAGGI");                               // Italian -AGGI → J/K
    EXPECT_STREQ("PJ", (const char*)biaggi.p);
    EXPECT_STREQ("PK", (const char*)biaggi.s);
}

TEST(Metaphone, JFamilyBranches)
{
    EXPECT_STREQ("HS", (const char*)DM("JOSE").p);     // Spanish José → H
    EXPECT_FALSE(DM("JOSE").alt);
    EXPECT_STREQ("SNHS", (const char*)DM("SAN JACINTO").p); // SAN + J → H
    DM jumble("JUMBLE");                               // initial J → J/A
    EXPECT_STREQ("JMPL", (const char*)jumble.p);
    EXPECT_STREQ("AMPL", (const char*)jumble.s);
    DM bajador("BAJADOR");                             // vowel+J+A Spanish → J/H
    EXPECT_STREQ("PJTR", (const char*)bajador.p);
    EXPECT_STREQ("PHTR", (const char*)bajador.s);
    DM raj("RAJ");                                     // final J → J / silent
    EXPECT_STREQ("RJ", (const char*)raj.p);
    EXPECT_STREQ("R",  (const char*)raj.s);
}

TEST(Metaphone, LFamilySpanishLL)
{
    DM cabrillo("CABRILLO");                           // -ILLO: L / silent
    EXPECT_STREQ("KPRL", (const char*)cabrillo.p);
    EXPECT_STREQ("KPR",  (const char*)cabrillo.s);
    DM gallegos("GALLEGOS");                           // -ALLE- + -OS: L / silent
    EXPECT_STREQ("KLKS", (const char*)gallegos.p);
    EXPECT_STREQ("KKS",  (const char*)gallegos.s);
}

TEST(Metaphone, MFamilySilentBAfterUM)
{
    DM thumb("THUMB");                                 // TH → 0/T; silent B
    EXPECT_STREQ("0M", (const char*)thumb.p);
    EXPECT_STREQ("TM", (const char*)thumb.s);
}

TEST(Metaphone, PFamilyBranches)
{
    EXPECT_STREQ("FN",   (const char*)DM("PHONE").p);      // PH → F
    EXPECT_STREQ("KMPL", (const char*)DM("CAMPBELL").p);   // -PB- collapses
    EXPECT_STREQ("RSPR", (const char*)DM("RASPBERRY").p);
    EXPECT_STREQ("KN",   (const char*)DM("QUEEN").p);      // Q → K
}

TEST(Metaphone, RFamilyFrenchFinal)
{
    DM rogier("ROGIER");                               // French -IER: silent R primary
    EXPECT_STREQ("RJ",  (const char*)rogier.p);
    EXPECT_STREQ("RJR", (const char*)rogier.s);
    // 'hochmeier' explicitly excluded (ME before -IER)
    DM hochmeier("HOCHMEIER");
    EXPECT_STREQ("HKMR", (const char*)hochmeier.p);
    EXPECT_FALSE(hochmeier.alt);
}

TEST(Metaphone, SFamilyBranches)
{
    EXPECT_STREQ("ALNT", (const char*)DM("ISLAND").p);   // -ISL-: silent S
    DM sugar("SUGAR");                                   // SUGAR- → X/S
    EXPECT_STREQ("XKR", (const char*)sugar.p);
    EXPECT_STREQ("SKR", (const char*)sugar.s);
    EXPECT_STREQ("SKL", (const char*)DM("SCHOOL").p);    // SCH+OO Dutch → SK
    EXPECT_STREQ("SKT", (const char*)DM("SCAT").p);      // SC+consonant → SK
    DM resnais("RESNAIS");                               // French final -AIS: S / silent
    EXPECT_STREQ("RSN",  (const char*)resnais.p);
    EXPECT_STREQ("RSNS", (const char*)resnais.s);
    DM wasserman("WASSERMAN");                           // initial W+vowel → A/F
    EXPECT_STREQ("ASRM", (const char*)wasserman.p);
    EXPECT_STREQ("FSRM", (const char*)wasserman.s);
}

TEST(Metaphone, TFamilyBranches)
{
    EXPECT_STREQ("NXN", (const char*)DM("NATION").p);    // -TION → X
    EXPECT_STREQ("TMS", (const char*)DM("THOMAS").p);    // TH+OM special → T
    EXPECT_FALSE(DM("THOMAS").alt);
    DM thatch("THATCH");                                 // TH → 0/T; TCH → X
    EXPECT_STREQ("0X", (const char*)thatch.p);
    EXPECT_STREQ("TX", (const char*)thatch.s);
    DM matthews("MATTHEWS");                             // TTH treated as TH
    EXPECT_STREQ("M0S", (const char*)matthews.p);
    EXPECT_STREQ("MTS", (const char*)matthews.s);
    DM catherine("CATHERINE");
    EXPECT_STREQ("K0RN", (const char*)catherine.p);
    EXPECT_STREQ("KTRN", (const char*)catherine.s);
}

TEST(Metaphone, WFamilyBranches)
{
    DM arnow("ARNOW");                                   // final W after vowel → alt F
    EXPECT_STREQ("ARN",  (const char*)arnow.p);
    EXPECT_STREQ("ARNF", (const char*)arnow.s);
    DM wachtler("WACHTLER");                             // W+vowel start → A/F; ACH → K
    EXPECT_STREQ("AKTL", (const char*)wachtler.p);
    EXPECT_STREQ("FKTL", (const char*)wachtler.s);
    DM filipowicz("FILIPOWICZ", 5);                      // Polish -WICZ → TS/FX
    EXPECT_STREQ("FLPTS", (const char*)filipowicz.p);
    EXPECT_STREQ("FLPFX", (const char*)filipowicz.s);
}

TEST(Metaphone, XAndZFamilies)
{
    EXPECT_STREQ("PR", (const char*)DM("BREAUX").p);     // French -EAUX: silent X
    EXPECT_STREQ("AKS", (const char*)DM("AX").p);        // short word: negative StringAt start
    EXPECT_STREQ("J", (const char*)DM("ZHAO").p);        // ZH → J
    DM pizza("PIZZA");                                   // Z+ZA → S/TS
    EXPECT_STREQ("PS",  (const char*)pizza.p);
    EXPECT_STREQ("PTS", (const char*)pizza.s);
}

TEST(Metaphone, Latin1CedillaAndEnye)
{
    // '\xC7' (Ç) → S and '\xD1' (Ñ) → N (already-uppercase Latin-1 input).
    EXPECT_STREQ("KRSN", (const char*)DM("GAR\xC7ON").p);
    EXPECT_STREQ("SNR",  (const char*)DM("SE\xD1OR").p);
}

TEST(Metaphone, MaxlengthTruncationEmbedsNulWithoutShortening)
{
    // The tail truncation is SetAt(maxlength, '\0'): the CString keeps its
    // full length with an embedded NUL (MFC semantics) — the C-string view
    // stops at maxlength.  SCHENKER's secondary runs to 5 chars (SKNKR).
    DM schenker("SCHENKER");
    EXPECT_STREQ("XNKR", (const char*)schenker.p);
    ASSERT_TRUE(schenker.alt);
    EXPECT_EQ(5, schenker.s.GetLength());        // length NOT shortened
    EXPECT_STREQ("SKNK", (const char*)schenker.s); // C-string view truncated
}

TEST(Metaphone, LoopStopsOncePrimaryAndSecondaryReachMaxlength)
{
    // With maxlength 2 the main loop exits as soon as both codes have 2
    // chars, so CATHERINE never gets past K0/KT (plus NUL-truncation).
    DM cath("CATHERINE", 2);
    EXPECT_STREQ("K0", (const char*)cath.p);
    EXPECT_STREQ("KT", (const char*)cath.s);
}

// ═══ MacroCommandIdToSlot ══════════════════════════════════════════════════
// The macro command id -> m_macros[] slot table, hoisted out of
// CSendView::OnMacroCommand so the port dispatches through the same one.  On the
// port those ids used to reach nothing at all -- CSendView is never constructed
// -- so world.DoCommand("MacroF1"), and any menu/toolbar item or accelerator
// bound to the command NAME, silently did nothing while returning eOK.  Pressing
// the key still worked, which is what hid it.
TEST(MacroCommandIds, EveryMacroCommandIdMapsToItsOwnSlot)
{
    struct Row { unsigned id; int slot; const char* name; };
    static const Row kRows[] = {
        { ID_MACRO_F1,  MAC_F1,  "MacroF1"  }, { ID_MACRO_F2,  MAC_F2,  "MacroF2"  },
        { ID_MACRO_F3,  MAC_F3,  "MacroF3"  }, { ID_MACRO_F4,  MAC_F4,  "MacroF4"  },
        { ID_MACRO_F5,  MAC_F5,  "MacroF5"  }, { ID_MACRO_F6,  MAC_F6,  "MacroF6"  },
        { ID_MACRO_F7,  MAC_F7,  "MacroF7"  }, { ID_MACRO_F8,  MAC_F8,  "MacroF8"  },
        { ID_MACRO_F9,  MAC_F9,  "MacroF9"  }, { ID_MACRO_F10, MAC_F10, "MacroF10" },
        { ID_MACRO_F11, MAC_F11, "MacroF11" }, { ID_MACRO_F12, MAC_F12, "MacroF12" },

        { ID_MACRO_CTRL_F1,  MAC_CTRL_F1,  "MacroCtrlF1"  },
        { ID_MACRO_CTRL_F2,  MAC_CTRL_F2,  "MacroCtrlF2"  },
        { ID_MACRO_CTRL_F3,  MAC_CTRL_F3,  "MacroCtrlF3"  },
        { ID_MACRO_CTRL_F5,  MAC_CTRL_F5,  "MacroCtrlF5"  },
        { ID_MACRO_CTRL_F6,  MAC_CTRL_F6,  "MacroCtrlF6"  },
        { ID_MACRO_CTRL_F7,  MAC_CTRL_F7,  "MacroCtrlF7"  },
        { ID_MACRO_CTRL_F8,  MAC_CTRL_F8,  "MacroCtrlF8"  },
        { ID_MACRO_CTRL_F9,  MAC_CTRL_F9,  "MacroCtrlF9"  },
        { ID_MACRO_CTRL_F10, MAC_CTRL_F10, "MacroCtrlF10" },
        { ID_MACRO_CTRL_F11, MAC_CTRL_F11, "MacroCtrlF11" },
        { ID_MACRO_CTRL_F12, MAC_CTRL_F12, "MacroCtrlF12" },

        { ID_MACRO_SHIFT_F1,  MAC_SHIFT_F1,  "MacroShiftF1"  },
        { ID_MACRO_SHIFT_F2,  MAC_SHIFT_F2,  "MacroShiftF2"  },
        { ID_MACRO_SHIFT_F3,  MAC_SHIFT_F3,  "MacroShiftF3"  },
        { ID_MACRO_SHIFT_F4,  MAC_SHIFT_F4,  "MacroShiftF4"  },
        { ID_MACRO_SHIFT_F5,  MAC_SHIFT_F5,  "MacroShiftF5"  },
        { ID_MACRO_SHIFT_F6,  MAC_SHIFT_F6,  "MacroShiftF6"  },
        { ID_MACRO_SHIFT_F7,  MAC_SHIFT_F7,  "MacroShiftF7"  },
        { ID_MACRO_SHIFT_F8,  MAC_SHIFT_F8,  "MacroShiftF8"  },
        { ID_MACRO_SHIFT_F9,  MAC_SHIFT_F9,  "MacroShiftF9"  },
        { ID_MACRO_SHIFT_F10, MAC_SHIFT_F10, "MacroShiftF10" },
        { ID_MACRO_SHIFT_F11, MAC_SHIFT_F11, "MacroShiftF11" },
        { ID_MACRO_SHIFT_F12, MAC_SHIFT_F12, "MacroShiftF12" },

        { ID_ALT_A, MAC_ALT_A, "AltA" }, { ID_ALT_B, MAC_ALT_B, "AltB" },
        { ID_ALT_J, MAC_ALT_J, "AltJ" }, { ID_ALT_K, MAC_ALT_K, "AltK" },
        { ID_ALT_L, MAC_ALT_L, "AltL" }, { ID_ALT_M, MAC_ALT_M, "AltM" },
        { ID_ALT_N, MAC_ALT_N, "AltN" }, { ID_ALT_O, MAC_ALT_O, "AltO" },
        { ID_ALT_P, MAC_ALT_P, "AltP" }, { ID_ALT_Q, MAC_ALT_Q, "AltQ" },
        { ID_ALT_R, MAC_ALT_R, "AltR" }, { ID_ALT_S, MAC_ALT_S, "AltS" },
        { ID_ALT_T, MAC_ALT_T, "AltT" }, { ID_ALT_U, MAC_ALT_U, "AltU" },
        { ID_ALT_X, MAC_ALT_X, "AltX" }, { ID_ALT_Y, MAC_ALT_Y, "AltY" },
        { ID_ALT_Z, MAC_ALT_Z, "AltZ" },
    };

    std::set<int> seen;
    for (const Row& r : kRows) {
        const int got = MacroCommandIdToSlot(r.id);
        EXPECT_EQ(r.slot, got);
        // In range, or FireMacro would index past m_macros[].
        EXPECT_TRUE(got >= 0 && got < MACRO_COUNT);
        // No two commands may share a slot, or one macro fires for two keys.
        EXPECT_TRUE(seen.insert(got).second);
    }
    // All distinct, and the expected population: 12 F + 11 Ctrl (no F4) +
    // 12 Shift + 17 Alt.  The literal catches a row silently going missing.
    EXPECT_EQ(sizeof(kRows) / sizeof(kRows[0]), seen.size());
    EXPECT_EQ((size_t) 52, seen.size());

    // ID_ALT_V is published as a command name with no MAC_ slot behind it
    // (docs/UPSTREAM-BUGS.md) -- it must answer "not a macro", not slot 0.
    EXPECT_EQ(-1, MacroCommandIdToSlot(ID_ALT_V));

    // And an id that is not a macro at all.
    EXPECT_EQ(-1, MacroCommandIdToSlot(ID_DISPLAY_STOPSOUNDPLAYING));
    EXPECT_EQ(-1, MacroCommandIdToSlot(0));
}

// ═══ world.Sound / PlaySound / StopSound / GetSoundStatus ══════════════════
// The _NOSOUND fork of methods_sounds.cpp: the Lua sound API drives the
// g_wxhook_sound_* seam (miniaudio in the product).  Fakes here record the
// calls and simulate buffer status, exercising the same parameter clamping
// and buffer contract Windows DirectSound had.

namespace fakesound {
    inline int  lastBuffer = -99;
    inline std::string lastPath;
    inline int  lastLoop = -1;
    inline double lastVolume = 99, lastPan = 99;
    inline int  playResult = 1;
    inline int  statusResult = 1;
    inline int  adjustResult = 1;
    inline int  stoppedBuffer = -99;
    // Which doc each call named.  The buffers are per world on Windows (the ten
    // LPDIRECTSOUNDBUFFERs are a CMUSHclientDoc member), so every hook carries
    // the owner and these record it.
    inline const void* lastOwner = nullptr;
    inline const void* stoppedOwner = nullptr;
    inline const void* releasedOwner = nullptr;
    inline int  releaseCount = 0;

    inline int hk_play(const void* owner, int buffer, const char* path, int loop,
                       double volume, double pan) {
        lastOwner = owner;
        lastBuffer = buffer; lastPath = path ? path : "";
        lastLoop = loop; lastVolume = volume; lastPan = pan;
        return playResult;
    }
    inline int hk_play_memory(const void* owner, int buffer, const void*, size_t len,
                              int loop, double volume, double pan) {
        lastOwner = owner;
        lastBuffer = buffer; lastLoop = loop;
        lastVolume = volume; lastPan = pan;
        lastPath = "<memory:" + std::to_string(len) + ">";
        return playResult;
    }
    inline int  hk_status(const void* owner, int) { lastOwner = owner; return statusResult; }
    inline int  hk_adjust(const void* owner, int buffer, int loop, double volume, double pan) {
        lastOwner = owner;
        lastBuffer = buffer; lastLoop = loop;
        lastVolume = volume; lastPan = pan;
        return adjustResult;
    }
    inline void hk_stop(const void* owner, int buffer) {
        stoppedOwner = owner; stoppedBuffer = buffer;
    }
    inline void hk_release(const void* owner) { releasedOwner = owner; ++releaseCount; }
    // CMainFrame::CancelSound's port arm — the app-wide MCI/PlaySound channel
    // (MSP + trigger sounds + the Test buttons), NOT any world's buffers.
    inline int appChannelStops = 0;
    inline void hk_cancel_app() { ++appChannelStops; }

    struct Install {
        Install() {
            lastBuffer = -99; lastPath.clear(); lastLoop = -1;
            lastVolume = 99; lastPan = 99;
            playResult = 1; statusResult = 1; adjustResult = 1;
            stoppedBuffer = -99;
            lastOwner = stoppedOwner = releasedOwner = nullptr;
            releaseCount = 0; appChannelStops = 0;
            savedStop = g_wxhook_stop_sound;
            g_wxhook_stop_sound          = hk_cancel_app;
            g_wxhook_sound_release_owner = hk_release;
            g_wxhook_sound_play        = hk_play;
            g_wxhook_sound_play_memory = hk_play_memory;
            g_wxhook_sound_status      = hk_status;
            g_wxhook_sound_adjust      = hk_adjust;
            g_wxhook_sound_stop_buffer = hk_stop;
        }
        ~Install() {
            g_wxhook_sound_play        = nullptr;
            g_wxhook_sound_play_memory = nullptr;
            g_wxhook_sound_status      = nullptr;
            g_wxhook_sound_adjust      = nullptr;
            g_wxhook_sound_stop_buffer = nullptr;
            g_wxhook_sound_release_owner = nullptr;
            g_wxhook_stop_sound          = savedStop;
        }
        void (*savedStop)() = nullptr;
    };
}

TEST(Sounds, NoBackendReturnsCannotPlayAndUnavailableStatus)
{
    CMUSHclientDoc doc;
    EXPECT_EQ((long)eCannotPlaySound, doc.PlaySound(0, "/x.wav", FALSE, 0, 0));
    EXPECT_EQ(-3L, doc.GetSoundStatus(1));      // sound system unavailable
    EXPECT_EQ((long)eOK, doc.StopSound(0));     // stop is always happy
    EXPECT_EQ((long)eNoNameSpecified, doc.Sound(""));
}

TEST(Sounds, PlaySoundClampsVolumeAndPan)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;
    EXPECT_EQ((long)eOK, doc.PlaySound(2, "/abs/sound.wav", TRUE, 50 /*>0*/, 200 /*>100*/));
    EXPECT_EQ(2, fakesound::lastBuffer);
    EXPECT_STREQ("/abs/sound.wav", fakesound::lastPath.c_str());
    EXPECT_EQ(1, fakesound::lastLoop);
    EXPECT_NEAR(0.0, fakesound::lastVolume, 1e-12);   // out-of-range -> 0
    EXPECT_NEAR(0.0, fakesound::lastPan, 1e-12);
    EXPECT_EQ((long)eOK, doc.PlaySound(1, "/abs/s.wav", FALSE, -6, -50));
    EXPECT_NEAR(-6.0, fakesound::lastVolume, 1e-12);  // in-range passes through
    EXPECT_NEAR(-50.0, fakesound::lastPan, 1e-12);
}

TEST(Sounds, RelativePathResolvesUnderSoundsDirectory)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;
    CString savedExe = App.m_strMUSHclientFileName;
    App.m_strMUSHclientFileName = "/opt/mc/MUSHclient";
    EXPECT_EQ((long)eOK, doc.PlaySound(0, "beep.wav", FALSE, 0, 0));
    EXPECT_STREQ("/opt/mc/sounds/beep.wav", fakesound::lastPath.c_str());
    App.m_strMUSHclientFileName = savedExe;
}

TEST(Sounds, PlaySoundParameterValidation)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;
    EXPECT_EQ((long)eBadParameter, doc.PlaySound(0, "x", FALSE, 0, 0));   // name < 2 chars
    EXPECT_EQ((long)eBadParameter,
              doc.PlaySound(MAX_SOUND_BUFFERS + 1, "/s.wav", FALSE, 0, 0));
    EXPECT_EQ((long)eBadParameter, doc.PlaySound(-1, "/s.wav", FALSE, 0, 0));
    fakesound::playResult = 0;   // backend failure
    EXPECT_EQ((long)eCannotPlaySound, doc.PlaySound(0, "/s.wav", FALSE, 0, 0));
}

// The ten buffers belong to the WORLD, not the process.  On Windows they are
// LPDIRECTSOUNDBUFFER m_pDirectSoundSecondaryBuffer[MAX_SOUND_BUFFERS], a member
// of CMUSHclientDoc (doc.h), so two worlds can both be using buffer 1 and one
// world's StopSound never touches the other's.  A single process-wide array got
// that wrong in both directions: world B's PlaySound(1,...) replaced world A's
// voice, and B's StopSound(0) -- a very common plugin idiom on install and on
// close -- silenced A.
TEST(Sounds, BuffersBelongToTheWorldThatPlayedThem)
{
    fakesound::Install fake;
    CMUSHclientDoc a, b;
    ASSERT_NE(&a, &b);

    EXPECT_EQ((long)eOK, a.PlaySound(1, "/abs/a.wav", TRUE, 0, 0));
    EXPECT_EQ((const void*)&a, fakesound::lastOwner);
    EXPECT_EQ((long)eOK, b.PlaySound(1, "/abs/b.wav", TRUE, 0, 0));
    EXPECT_EQ((const void*)&b, fakesound::lastOwner);   // same buffer NUMBER,
                                                        // different owner

    // StopSound(0) names its own world, so the engine can leave the other alone.
    EXPECT_EQ((long)eOK, b.StopSound(0));
    EXPECT_EQ(0, fakesound::stoppedBuffer);
    EXPECT_EQ((const void*)&b, fakesound::stoppedOwner);
    EXPECT_EQ((long)eOK, a.StopSound(3));
    EXPECT_EQ(3, fakesound::stoppedBuffer);
    EXPECT_EQ((const void*)&a, fakesound::stoppedOwner);

    // The query side too: GetSoundStatus(1) must be a question about the world
    // it was asked of, or one world reports the other's sound as its own.
    fakesound::lastOwner = nullptr;
    EXPECT_EQ(1L, a.GetSoundStatus(1));
    EXPECT_EQ((const void*)&a, fakesound::lastOwner);
    EXPECT_EQ(1L, b.GetSoundStatus(1));
    EXPECT_EQ((const void*)&b, fakesound::lastOwner);

    // ...and the retune-in-place and memory-image paths.
    EXPECT_EQ((long)eOK, a.PlaySound(2, "", TRUE, -3, 0));
    EXPECT_EQ((const void*)&a, fakesound::lastOwner);
    EXPECT_EQ((long)eOK, b.PlaySoundHelper(2, NULL, FALSE, 0, 0, "RIFFxxxx", 8));
    EXPECT_EQ((const void*)&b, fakesound::lastOwner);
}

// Closing a world releases its buffers.  Windows does this in ~CMUSHclientDoc
// (doc_construct.cpp, inside #ifdef _WIN32): Stop() then Release() on each of
// its ten.  With no port arm a looping world.PlaySound outlived its window and
// kept playing until the app quit.
// Display > Stop Sound Playing has always stopped BOTH kinds of sound at once:
// the world's own voice (CMainFrame::CancelSound -- the MCI/PlaySound channel
// the MSP, trigger and Test-button sounds use) and the scripts' voices (this
// document's ten world.PlaySound buffers).  These two commands are those halves
// on their own, so one can be silenced without the other.
//
// Each assertion names BOTH counters, because "the right thing happened" and
// "the other thing did not" are different claims and only the pair pins the
// split.
TEST(Sounds, TheTwoHalvesOfStopSoundPlayingActSeparately)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;

    // Driven through the message map, which is how the menu command really
    // arrives -- so this covers the ON_COMMAND wiring as well as the bodies.
    // (The handlers themselves are protected, as they should be.)
    ASSERT_TRUE(AfxDispatchCommand(&doc, 0, ID_DISPLAY_STOPTRIGGERSOUNDS));

    // Stop Trigger Sounds: the app channel, and not the buffers.
    EXPECT_EQ(1, fakesound::appChannelStops);
    EXPECT_EQ(-99, fakesound::stoppedBuffer);          // untouched sentinel

    // Stop Script Sounds: this document's buffers, and not the app channel.
    ASSERT_TRUE(AfxDispatchCommand(&doc, 0, ID_DISPLAY_STOPSCRIPTSOUNDS));
    EXPECT_EQ(1, fakesound::appChannelStops);          // still just the one
    EXPECT_EQ(0, fakesound::stoppedBuffer);            // 0 = all of them
    EXPECT_EQ((const void*)&doc, fakesound::stoppedOwner);

    // And the combined command is still both -- CancelSound calls
    // Frame.CancelSound() and then StopSound(0).
    fakesound::appChannelStops = 0;
    fakesound::stoppedBuffer = -99;
    fakesound::stoppedOwner = nullptr;
    ASSERT_TRUE(AfxDispatchCommand(&doc, 0, ID_DISPLAY_STOPSOUNDPLAYING));
    EXPECT_EQ(1, fakesound::appChannelStops);
    EXPECT_EQ(0, fakesound::stoppedBuffer);
    EXPECT_EQ((const void*)&doc, fakesound::stoppedOwner);
}

TEST(Sounds, ClosingAWorldReleasesItsBuffers)
{
    fakesound::Install fake;
    const void* addr = nullptr;
    {
        CMUSHclientDoc doomed;
        addr = (const void*)&doomed;
        EXPECT_EQ((long)eOK, doomed.PlaySound(1, "/abs/loop.wav", TRUE, 0, 0));
        EXPECT_EQ(0, fakesound::releaseCount);   // ...not before it is destroyed
    }
    // The destructor must release THIS world's buffers, and only once.
    EXPECT_EQ(1, fakesound::releaseCount);
    EXPECT_EQ(addr, fakesound::releasedOwner);
}

TEST(Sounds, EmptyFilenameAdjustsPlayingBuffer)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;
    fakesound::statusResult = 1;   // playing
    EXPECT_EQ((long)eOK, doc.PlaySound(3, "", TRUE, -3, 25));
    EXPECT_EQ(3, fakesound::lastBuffer);
    EXPECT_EQ(1, fakesound::lastLoop);
    EXPECT_NEAR(-3.0, fakesound::lastVolume, 1e-12);
    fakesound::statusResult = -2;  // free buffer: nothing to adjust
    EXPECT_EQ((long)eBadParameter, doc.PlaySound(3, "", FALSE, 0, 0));
    fakesound::statusResult = 0;   // loaded but not playing
    EXPECT_EQ((long)eCannotPlaySound, doc.PlaySound(3, "", FALSE, 0, 0));
    fakesound::statusResult = 1;
    fakesound::adjustResult = 0;   // retune refused
    EXPECT_EQ((long)eCannotPlaySound, doc.PlaySound(3, "", FALSE, 0, 0));
}

TEST(Sounds, MemoryImagePlayback)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;
    static const char wav[] = "RIFFdata";
    EXPECT_EQ((long)eOK,
              doc.PlaySoundHelper(0, "", FALSE, 0, 0, wav, sizeof wav));
    EXPECT_STREQ("<memory:9>", fakesound::lastPath.c_str());
    EXPECT_EQ((long)eBadParameter,
              doc.PlaySoundHelper(-1, "", FALSE, 0, 0, wav, sizeof wav));
    fakesound::playResult = 0;
    EXPECT_EQ((long)eCannotPlaySound,
              doc.PlaySoundHelper(0, "", FALSE, 0, 0, wav, sizeof wav));
}

TEST(Sounds, StopAndStatusContract)
{
    fakesound::Install fake;
    CMUSHclientDoc doc;
    EXPECT_EQ((long)eOK, doc.StopSound(4));
    EXPECT_EQ(4, fakesound::stoppedBuffer);
    EXPECT_EQ((long)eOK, doc.StopSound(0));    // 0 = all
    EXPECT_EQ(0, fakesound::stoppedBuffer);
    EXPECT_EQ(-1L, doc.GetSoundStatus(0));     // out of range
    EXPECT_EQ(-1L, doc.GetSoundStatus(MAX_SOUND_BUFFERS + 1));
    fakesound::statusResult = 2;               // playing + looping
    EXPECT_EQ(2L, doc.GetSoundStatus(1));
}

TEST(Sounds, SoundRoutesThroughPlaySoundFile)
{
    static int s_plays;
    static bool s_result;
    s_plays = 0;
    s_result = true;
    g_wxhook_play_sound = [](const char*, bool) -> bool { ++s_plays; return s_result; };
    CMUSHclientDoc doc;
    EXPECT_EQ((long)eOK, doc.Sound("/tmp/beep.wav"));
    EXPECT_EQ(1, s_plays);
    s_result = false;
    EXPECT_EQ((long)eCannotPlaySound, doc.Sound("/tmp/beep.wav"));
    g_wxhook_play_sound = nullptr;
}


TEST(Utilities, FixupEscapeSequences_SimpleAndHexLetter)
{
    // The simple escapes at the top of the switch (utilities_shared.cpp ~539-549),
    // each decoding to its C control/literal byte.  (The existing
    // FixupEscapeSequences_Escapes test covers only \n \t \x41 \" \x00.)
    EXPECT_STREQ("\a", (const char*)FixupEscapeSequences("\\a"));   // 0x07
    EXPECT_STREQ("\b", (const char*)FixupEscapeSequences("\\b"));   // 0x08
    EXPECT_STREQ("\f", (const char*)FixupEscapeSequences("\\f"));   // 0x0C
    EXPECT_STREQ("\r", (const char*)FixupEscapeSequences("\\r"));   // 0x0D
    EXPECT_STREQ("\v", (const char*)FixupEscapeSequences("\\v"));   // 0x0B
    EXPECT_STREQ("'",  (const char*)FixupEscapeSequences("\\'"));   // 0x27
    EXPECT_STREQ("\\", (const char*)FixupEscapeSequences("\\\\"));  // 0x5C
    EXPECT_STREQ("?",  (const char*)FixupEscapeSequences("\\?"));   // 0x3F
    // hex path where a nibble is a LETTER (utilities_shared.cpp ~556: iNewDigit>='A'
    // subtracts 7).  \x4A: 4 then A(=10) -> 0x4A == 'J'; \x6f exercises the
    // lowercase (toupper) letter path -> 0x6F == 'o'.
    EXPECT_STREQ("J", (const char*)FixupEscapeSequences("\\x4A"));
    EXPECT_STREQ("o", (const char*)FixupEscapeSequences("\\x6f"));
}

TEST(Utilities, ConvertToRegularExpression_ControlNewlineHighByte)
{
    // a control char (<0x20, not newline) -> lowercase two-digit \xhh
    // (utilities_shared.cpp ~472-478); bWholeLine=false so no ^/$ anchors.
    EXPECT_STREQ("\\x03", (const char*)ConvertToRegularExpression("\x03", false, true));
    // newline is special-cased FIRST to the literal escape "\n" (~470-471),
    // NOT the generic \x0a control form.
    EXPECT_STREQ("\\n", (const char*)ConvertToRegularExpression("\n", false, true));
    // a >=0x80 byte (UTF-8 lead + continuation) passes straight through (~479).
    EXPECT_STREQ("\xC3\xA9", (const char*)ConvertToRegularExpression("\xC3\xA9", false, true));
}

TEST(Utilities, MyAtoF_ExponentScalingAndPlusSigns)
{
    // a large exponent drives the >=64 loop then the >=16 loop
    // (utilities_shared.cpp ~110-111): 1e64*1e16*1e16*1e4 == 1e100.
    EXPECT_NEAR(1e100, atof_val("1e100"), 1e90);
    // a negative exponent past 16 drives the >=16 loop twice + the esign<0
    // division branch (line ~114).
    EXPECT_NEAR(1e-40, atof_val("1e-40"), 1e-50);
    // a leading '+' on the mantissa (line ~79) is consumed, not rejected.
    EXPECT_NEAR(2.5, atof_val("+2.5"), 1e-12);
    // an explicit '+' on the exponent (line ~108).
    EXPECT_NEAR(1000.0, atof_val("1e+3"), 1e-9);
}

// -- ScanMatchingBrace -------------------------------------------------------
//
// utilities_shared.cpp, moved out of Utilities.cpp::FindMatchingBrace.  It is
// the only reader of the seven File > Preferences > Notepad brace options, and
// it used to sit behind CSendView/CTextView -- views the wx port never
// constructs -- so on the port those checkboxes did nothing.  Each case below
// is run with the flag ON and OFF and lands somewhere different, which is what
// makes the option observable.

// Match from `caret`, no selection: on success both ends collapse to the index
// of the matching delimiter.  Returns -1 where the scanner fails (Windows beeps).
static int match_at(const char* buf, int caret, unsigned int flags)
{
    int s = caret, e = caret;
    if (!ScanMatchingBrace(buf, (int) strlen(buf), s, e, false, flags))
        return -1;
    EXPECT_EQ(s, e);          // no-select collapses to a caret position
    return e;
}

// "do braces nest? eg. ( [)] )" -- ON, the ) inside the brackets belongs to the
// bracket group and is skipped; OFF, only the same delimiter counts, so the
// first ) wins.  This one is ON by default, so the port was wrong out of the box.
TEST(ScanMatchingBrace, NestBracesDecidesWhetherOtherDelimitersCount)
{
    //                 0123456
    const char* buf = "( [)] )";
    EXPECT_EQ(6, match_at(buf, 0, PAREN_NEST_BRACES));
    EXPECT_EQ(3, match_at(buf, 0, 0));
}

// "\\ escapes next character, eg. (  \\)  )" -- also ON by default.
TEST(ScanMatchingBrace, BackslashEscapesHideTheNextDelimiter)
{
    //                 01234567
    const char* buf = "(a \\) b)";
    EXPECT_EQ(7, match_at(buf, 0, PAREN_BACKSLASH_ESCAPES));
    EXPECT_EQ(4, match_at(buf, 0, 0));
}

// "% escapes next character, eg. (  %)  )" -- the third default-ON option.
TEST(ScanMatchingBrace, PercentEscapesHideTheNextDelimiter)
{
    //                 01234567
    const char* buf = "(a %) b)";
    EXPECT_EQ(7, match_at(buf, 0, PAREN_PERCENT_ESCAPES));
    EXPECT_EQ(4, match_at(buf, 0, 0));
}

TEST(ScanMatchingBrace, QuoteOptionsHideDelimitersInsideQuotes)
{
    //                  0123456789
    const char* dq   = "(a \"b) c\" d)";
    EXPECT_EQ(11, match_at(dq, 0, PAREN_DOUBLE_QUOTES));
    EXPECT_EQ(5,  match_at(dq, 0, 0));

    const char* sq   = "(a 'b) c' d)";
    EXPECT_EQ(11, match_at(sq, 0, PAREN_SINGLE_QUOTES));
    EXPECT_EQ(5,  match_at(sq, 0, 0));
}

// "escape inside double quotes, eg. \"Press \\\"enter\\\" \"" -- with the option ON the
// \\" does not close the quoted run, so the ) inside it stays hidden.
TEST(ScanMatchingBrace, EscapedQuotesInsideQuotesKeepTheRunOpen)
{
    const char* buf = "(a \"b \\\" c) d\" e)";
    const unsigned int base = PAREN_DOUBLE_QUOTES | PAREN_BACKSLASH_ESCAPES;
    EXPECT_EQ((int) strlen(buf) - 1,
              match_at(buf, 0, base | PAREN_ESCAPE_DOUBLE_QUOTES));
    // Without it the quoted run ends at the escaped quote, so the next ) matches.
    EXPECT_NE((int) strlen(buf) - 1, match_at(buf, 0, base));
}

// A closing delimiter under the caret searches BACKWARDS.
TEST(ScanMatchingBrace, ClosingDelimiterSearchesBackwards)
{
    //                 0123456
    const char* buf = "( [)] )";
    EXPECT_EQ(0, match_at(buf, 6, PAREN_NEST_BRACES));
}

// bSelect keeps both ends and includes the closing delimiter.
TEST(ScanMatchingBrace, SelectSpansBothDelimitersInclusive)
{
    const char* buf = "x(abc)y";
    int s = 1, e = 1;
    EXPECT_TRUE(ScanMatchingBrace(buf, (int) strlen(buf), s, e, true, 0));
    EXPECT_EQ(1, s);
    EXPECT_EQ(6, e);          // one past the ) at index 5
}

// The caret one PAST a delimiter still finds it (the scanner backs up one).
TEST(ScanMatchingBrace, CaretJustAfterADelimiterStillMatches)
{
    const char* buf = "(abc)";
    EXPECT_EQ(4, match_at(buf, 1, 0));     // caret after the ( at 0
}

TEST(ScanMatchingBrace, NoDelimiterOrNoMatchFails)
{
    EXPECT_EQ(-1, match_at("plain text", 3, 0));    // nothing to match
    EXPECT_EQ(-1, match_at("(abc", 0, 0));          // never closed
    EXPECT_EQ(-1, match_at("abc)", 3, 0));          // never opened
    int s = 0, e = 0;
    EXPECT_FALSE(ScanMatchingBrace("", 0, s, e, false, 0));   // empty buffer
}

// ── UTF8NextChar ──────────────────────────────────────────────────────────
//
// The single walker behind both "how many characters is this run" and "what
// characters are they" in the output pane.  Its contract is
// MultiByteToWideChar(CP_UTF8, 0, ...): consume an ill-formed subpart and
// substitute U+FFFD, so a bad byte costs one glyph and never the rest.

namespace {
// Walk a whole buffer; return the code points, so a test can assert both the
// sequence AND (through .size()) the character count the pane will use.
std::vector<unsigned int> utf8_walk(const std::string& s)
{
    std::vector<unsigned int> cps;
    const unsigned char* p = (const unsigned char*) s.data();
    for (size_t i = 0; i < s.size(); ) {
        unsigned int cp = 0;
        const size_t used = UTF8NextChar(p + i, s.size() - i, cp);
        EXPECT_TRUE(used >= 1);         // never zero, or the caller spins
        i += used;
        cps.push_back(cp);
    }
    return cps;
}
} // namespace

TEST(Utf8NextCharTest, AsciiIsOneByteOneCharacter) {
    const std::vector<unsigned int> cps = utf8_walk("abc");
    ASSERT_EQ(3u, cps.size());
    EXPECT_EQ('a', (int) cps[0]);
    EXPECT_EQ('c', (int) cps[2]);
}

TEST(Utf8NextCharTest, DecodesTwoThreeAndFourByteSequences) {
    // U+00E9 e-acute, U+20AC euro, U+1F600 grinning face.
    const std::vector<unsigned int> cps = utf8_walk("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    ASSERT_EQ(3u, cps.size());
    EXPECT_EQ(0x00E9u, cps[0]);
    EXPECT_EQ(0x20ACu, cps[1]);
    EXPECT_EQ(0x1F600u, cps[2]);
}

// The bug this exists for: a stray Latin-1 byte among valid text.  Everything
// around it must survive — one glyph lost, not the run.
TEST(Utf8NextCharTest, StrayContinuationByteCostsExactlyOneCharacter) {
    const std::vector<unsigned int> cps = utf8_walk("a\xE9z");
    ASSERT_EQ(3u, cps.size());
    EXPECT_EQ('a',     (int) cps[0]);
    EXPECT_EQ(0xFFFDu,       cps[1]);
    EXPECT_EQ('z',     (int) cps[2]);
}

// A lead byte whose continuation never arrives must consume only itself, so the
// byte that interrupted it is judged on its own merits rather than swallowed.
TEST(Utf8NextCharTest, TruncatedSequenceDoesNotSwallowTheNextCharacter) {
    const std::vector<unsigned int> cps = utf8_walk("\xE2\x82Z");
    ASSERT_EQ(2u, cps.size());
    EXPECT_EQ(0xFFFDu,       cps[0]);   // the E2 82 pair, cut short
    EXPECT_EQ('Z',     (int) cps[1]);   // NOT eaten by the broken sequence
}

TEST(Utf8NextCharTest, TruncatedAtEndOfBufferTerminates) {
    const std::vector<unsigned int> cps = utf8_walk("ok\xF0\x9F");
    ASSERT_EQ(3u, cps.size());
    EXPECT_EQ('o', (int) cps[0]);
    EXPECT_EQ('k', (int) cps[1]);
    EXPECT_EQ(0xFFFDu,   cps[2]);
}

// Right shape, wrong value.  These decode "successfully" in a naive walker and
// are exactly what a security-minded decoder must reject.
TEST(Utf8NextCharTest, RejectsOverlongEncodings) {
    // C0 AF and E0 80 AF are both overlong encodings of '/'.
    std::vector<unsigned int> cps = utf8_walk("\xC0\xAF");
    ASSERT_EQ(2u, cps.size());               // C0 is never a valid lead...
    EXPECT_EQ(0xFFFDu, cps[0]);
    EXPECT_EQ(0xFFFDu, cps[1]);              // ...and AF is then a stray

    cps = utf8_walk("\xE0\x80\xAF");
    ASSERT_EQ(1u, cps.size());               // well-shaped 3-byte, overlong value
    EXPECT_EQ(0xFFFDu, cps[0]);
}

TEST(Utf8NextCharTest, RejectsSurrogateHalves) {
    // ED A0 80 is CESU-8 for U+D800, which is not a valid Unicode scalar.
    const std::vector<unsigned int> cps = utf8_walk("\xED\xA0\x80");
    ASSERT_EQ(1u, cps.size());
    EXPECT_EQ(0xFFFDu, cps[0]);
}

TEST(Utf8NextCharTest, RejectsCodePointsBeyondUnicode) {
    // F5 .. FF can only encode > U+10FFFF, so they are rejected as leads.
    const std::vector<unsigned int> cps = utf8_walk("\xF5\x80\x80\x80");
    ASSERT_EQ(4u, cps.size());               // lead rejected, then three strays
    for (size_t i = 0; i < cps.size(); i++)
        EXPECT_EQ(0xFFFDu, cps[i]);
}

// The invariant the output pane depends on: for WELL-FORMED input the walker's
// character count equals the lead-byte count, which is what the pane's column
// and wrap bookkeeping assumed before the walker existed.
// ── UTF8ByteToCharOffset / UTF8CharToByteOffset ───────────────────────────
//
// The command-bar seam: wxTextCtrl indexes CHARACTERS, GetCommandText() and
// every shared body scanning it index BYTES (as Windows' CEdit offsets do over
// its ANSI buffer).  Tab completion took wx character offsets straight into the
// byte-indexed shared body, so one non-ASCII character earlier in the line threw
// every subsequent offset out by one byte per extra byte.

namespace {
// "héllo wörld" — 11 characters, 13 bytes.
const char* kMixed = "h\xC3\xA9llo w\xC3\xB6rld";
const size_t kMixedBytes = 13;
} // namespace

TEST(Utf8OffsetTest, RoundTripsEveryCharacterBoundary) {
    for (size_t chars = 0; chars <= 11; chars++) {
        const size_t bytes = UTF8CharToByteOffset(kMixed, kMixedBytes, chars);
        EXPECT_EQ(chars, UTF8ByteToCharOffset(kMixed, kMixedBytes, bytes));
    }
}

TEST(Utf8OffsetTest, AccountsForMultiByteCharacters) {
    // Before the é the two agree; after it they differ by one byte, and after
    // the ö by two.  This drift IS the bug.
    EXPECT_EQ(1u,  UTF8CharToByteOffset(kMixed, kMixedBytes, 1));   // "h"
    EXPECT_EQ(3u,  UTF8CharToByteOffset(kMixed, kMixedBytes, 2));   // "hé"
    EXPECT_EQ(8u,  UTF8CharToByteOffset(kMixed, kMixedBytes, 7));   // "héllo w"
    EXPECT_EQ(13u, UTF8CharToByteOffset(kMixed, kMixedBytes, 11));  // all of it

    EXPECT_EQ(1u,  UTF8ByteToCharOffset(kMixed, kMixedBytes, 1));
    EXPECT_EQ(2u,  UTF8ByteToCharOffset(kMixed, kMixedBytes, 3));
    EXPECT_EQ(7u,  UTF8ByteToCharOffset(kMixed, kMixedBytes, 8));
    EXPECT_EQ(11u, UTF8ByteToCharOffset(kMixed, kMixedBytes, 13));
}

TEST(Utf8OffsetTest, PureAsciiIsTheIdentity) {
    const char* s = "north";
    for (size_t i = 0; i <= 5; i++) {
        EXPECT_EQ(i, UTF8CharToByteOffset(s, 5, i));
        EXPECT_EQ(i, UTF8ByteToCharOffset(s, 5, i));
    }
}

// Out of range must saturate, not read past the buffer: a caller can hand these
// a stale selection after the text shrank.
TEST(Utf8OffsetTest, OutOfRangeOffsetsClamp) {
    EXPECT_EQ(kMixedBytes, UTF8CharToByteOffset(kMixed, kMixedBytes, 99));
    EXPECT_EQ(11u,         UTF8ByteToCharOffset(kMixed, kMixedBytes, 99));
    EXPECT_EQ(0u,          UTF8CharToByteOffset(kMixed, kMixedBytes, 0));
    EXPECT_EQ(0u,          UTF8ByteToCharOffset(kMixed, kMixedBytes, 0));
}

TEST(Utf8OffsetTest, EmptyBufferIsAlwaysZero) {
    EXPECT_EQ(0u, UTF8CharToByteOffset("", 0, 5));
    EXPECT_EQ(0u, UTF8ByteToCharOffset("", 0, 5));
}

// A byte offset landing INSIDE a multi-byte character cannot be honoured
// exactly; it must not loop forever or run past the end.  It rounds up to the
// end of that character, which is the only self-consistent answer.
TEST(Utf8OffsetTest, ByteOffsetInsideACharacterDoesNotOverrun) {
    const size_t chars = UTF8ByteToCharOffset(kMixed, kMixedBytes, 2);  // mid-é
    EXPECT_EQ(2u, chars);
    EXPECT_TRUE(UTF8CharToByteOffset(kMixed, kMixedBytes, chars) <= kMixedBytes);
}

TEST(Utf8NextCharTest, CharacterCountMatchesLeadByteCountWhenWellFormed) {
    const std::string s = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80!";
    int leadBytes = 0;
    for (unsigned char c : s)
        if ((c & 0xC0u) != 0x80u) ++leadBytes;
    EXPECT_EQ((size_t) leadBytes, utf8_walk(s).size());
}

// ── OSVersionString ─────────────────────────────────────────────────────────

// world.Debug ("summary") names the operating system through this.  Off Windows
// os_version holds whatever the GetVersionEx shim invented — a fixed platform 2
// / major 10 — so running the Windows version switch over it answered
// "Windows 10" on macOS.  The answer has to come from the kernel instead, and
// pinning it against uname is what makes a fall-back to that switch fail.
TEST(Utilities, OSVersionString_ReportsThisKernelNotWindows) {
    struct utsname u;
    ASSERT_EQ(0, uname(&u));

    const CString sVersion = OSVersionString();
    const std::string expected = std::string(u.sysname) + " " + u.release;

    EXPECT_STREQ(expected.c_str(), (const char*) sVersion);

    // Belt and braces: neither of the two answers the old code could give.
    EXPECT_TRUE(strstr((const char*) sVersion, "Windows") == NULL);
    EXPECT_TRUE(strstr((const char*) sVersion, "Unknown (Platform") == NULL);
}

MC_TEST_MAIN()
