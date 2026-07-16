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

MC_TEST_MAIN()
