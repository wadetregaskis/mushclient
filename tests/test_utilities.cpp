// tests/test_utilities.cpp
//
// Unit tests for the single-sourced pure-logic utilities.  These live in
// utilities_shared.cpp (myAtoF, encodeBase64, EditDistance) and
// utilities_portable.cpp (decodeBase64) — both compiled into mushcore, which is
// the SAME utilities_shared.cpp the shipping product links.  So unlike the old
// portable twins, these exercise the real shipping code, not a reimplementation.

#include "mc_test.h"
#include "platform/doc_stub.h"   // CString + prototypes: myAtoF, EditDistance,
                                 // encodeBase64, decodeBase64
#include <cstring>

// ── encodeBase64 / decodeBase64 ─────────────────────────────────────────────

// RFC 4648 §10 test vectors — the canonical base64 encodings.
TEST(Utilities, EncodeBase64_Rfc4648Vectors)
{
    EXPECT_STREQ("",         (const char*)encodeBase64("", false));
    EXPECT_STREQ("Zg==",     (const char*)encodeBase64("f", false));
    EXPECT_STREQ("Zm8=",     (const char*)encodeBase64("fo", false));
    EXPECT_STREQ("Zm9v",     (const char*)encodeBase64("foo", false));
    EXPECT_STREQ("Zm9vYg==", (const char*)encodeBase64("foob", false));
    EXPECT_STREQ("Zm9vYmE=", (const char*)encodeBase64("fooba", false));
    EXPECT_STREQ("Zm9vYmFy", (const char*)encodeBase64("foobar", false));
}

TEST(Utilities, DecodeBase64_Rfc4648Vectors)
{
    EXPECT_STREQ("f",      (const char*)decodeBase64("Zg=="));
    EXPECT_STREQ("fo",     (const char*)decodeBase64("Zm8="));
    EXPECT_STREQ("foo",    (const char*)decodeBase64("Zm9v"));
    EXPECT_STREQ("foob",   (const char*)decodeBase64("Zm9vYg=="));
    EXPECT_STREQ("fooba",  (const char*)decodeBase64("Zm9vYmE="));
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9vYmFy"));
}

TEST(Utilities, DecodeBase64_IgnoresWhitespace)
{
    // decodeBase64 strips spaces/tabs/newlines before decoding.
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9v\r\nYmFy"));
    EXPECT_STREQ("foobar", (const char*)decodeBase64("Zm9v YmFy"));
}

TEST(Utilities, Base64RoundTrip)
{
    const char* samples[] = {
        "", "A", "AB", "ABC", "ABCD", "Hello, world!",
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
    // 60 input bytes → 80 base64 chars; multi-line inserts a CRLF after the
    // first 76 output chars (i.e. after 57 input bytes).
    CString enc = encodeBase64(
        "0123456789012345678901234567890123456789012345678901234567890123456789", true);
    const char* p = (const char*)enc;
    EXPECT_TRUE(strstr(p, "\r\n") != NULL);
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

// ── m_pLinePositions regression ──────────────────────────────────────────────
// The shared ProcessPreviousLine omit-from-output branch writes m_pLinePositions
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
    // ≡1 (mod JUMP_SIZE=100) boundary, which writes an m_pLinePositions slot.
    // Unallocated → SIGSEGV; allocated → harmless.
    char buf[64];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof buf, "filler line %d\n", i);
        doc.DisplayMsg(buf, (int)strlen(buf), 0);
    }
    doc.DisplayMsg("spamline\n", 9, 0);

    EXPECT_TRUE(doc.m_pLinePositions != NULL);   // lazily allocated
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

MC_TEST_MAIN()
