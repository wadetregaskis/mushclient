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
                                 // encodeBase64, decodeBase64
#include <cstring>

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

// doc_portable.cpp GetLinePosition: "sanity check (Santa Claus): nLine >
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

    inline int hk_play(int buffer, const char* path, int loop,
                       double volume, double pan) {
        lastBuffer = buffer; lastPath = path ? path : "";
        lastLoop = loop; lastVolume = volume; lastPan = pan;
        return playResult;
    }
    inline int hk_play_memory(int buffer, const void*, size_t len,
                              int loop, double volume, double pan) {
        lastBuffer = buffer; lastLoop = loop;
        lastVolume = volume; lastPan = pan;
        lastPath = "<memory:" + std::to_string(len) + ">";
        return playResult;
    }
    inline int  hk_status(int) { return statusResult; }
    inline int  hk_adjust(int buffer, int loop, double volume, double pan) {
        lastBuffer = buffer; lastLoop = loop;
        lastVolume = volume; lastPan = pan;
        return adjustResult;
    }
    inline void hk_stop(int buffer) { stoppedBuffer = buffer; }

    struct Install {
        Install() {
            lastBuffer = -99; lastPath.clear(); lastLoop = -1;
            lastVolume = 99; lastPan = 99;
            playResult = 1; statusResult = 1; adjustResult = 1;
            stoppedBuffer = -99;
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
        }
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

MC_TEST_MAIN()
