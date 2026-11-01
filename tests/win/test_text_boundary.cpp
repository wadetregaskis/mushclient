// tests/win/test_text_boundary.cpp — text crossing between the world and the
// embedded script engines, whatever the machine's code page.
//
// A world's text is bytes.  On Windows the engines get it through a BSTR, which
// is UTF-16 converted from those bytes with the thread's ANSI code page, and
// they convert it back the same way to recover the bytes.  On Western Windows
// (1252) that is lossless.  On a Chinese, Japanese or Korean system, or one with
// Windows' UTF-8 option on, it is not: bytes get read in pairs, or as invalid
// UTF-8, and come back damaged.
//
// A thread's code page follows its locale, so these tests stand in for such a
// machine by setting the thread's locale (ThreadOnCodePage).  This is what MFC's
// conversions consult too (_AtlGetConversionACP is CP_THREAD_ACP).

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/mc_dispatch.h"

#include <string>
#include <vector>

namespace {

struct Locale { LCID lcid; UINT codePage; const char* name; };

const Locale kLocales[] = {
    { MAKELCID (MAKELANGID (LANG_JAPANESE, SUBLANG_DEFAULT), SORT_DEFAULT),          932, "Japanese" },
    { MAKELCID (MAKELANGID (LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), SORT_DEFAULT), 936, "Chinese (Simplified)" },
    { MAKELCID (MAKELANGID (LANG_KOREAN, SUBLANG_DEFAULT), SORT_DEFAULT),            949, "Korean" },
    { MAKELCID (MAKELANGID (LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL), SORT_DEFAULT), 950, "Chinese (Traditional)" },
    { MAKELCID (MAKELANGID (LANG_RUSSIAN, SUBLANG_DEFAULT), SORT_DEFAULT),           1251, "Russian" },
    { MAKELCID (MAKELANGID (LANG_GREEK, SUBLANG_DEFAULT), SORT_DEFAULT),             1253, "Greek" },
    { MAKELCID (MAKELANGID (LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT),        1252, "Western" },
};

struct ThreadOnCodePage {
    LCID saved;
    explicit ThreadOnCodePage (LCID lcid) : saved (::GetThreadLocale ()) { ::SetThreadLocale (lcid); }
    ~ThreadOnCodePage () { ::SetThreadLocale (saved); }
};

#ifdef MUSHCLIENT_EMBEDDED_ENGINES   // the 64-bit builds; Win32 keeps ActiveScript
VARIANT Text (const std::string& bytes)
{
    VARIANT v;
    ::VariantInit (&v);
    v.vt = VT_BSTR;
    v.bstrVal = MC_BstrFromBytes (bytes.c_str ());
    return v;
}

// A world method taking text and one giving it back, the way every embedded
// engine calls them.
std::string ThroughTheWorld (CMUSHclientDoc& doc, const std::string& bytes)
{
    VARIANT args[2] = { Text ("boundary"), Text (bytes) };
    VARIANT r;
    ::VariantInit (&r);
    MC_DispatchInvoke (&doc, "SetVariable", args, 2, r, nullptr);
    MC_VariantClear (r);

    VARIANT got;
    ::VariantInit (&got);
    MC_DispatchInvoke (&doc, "GetVariable", args, 1, got, nullptr);
    std::string out = got.vt == VT_BSTR ? MC_BstrBytes (got.bstrVal) : std::string ();
    MC_VariantClear (got);
    MC_VariantClear (args[0]);
    MC_VariantClear (args[1]);
    return out;
}

std::string EveryByte ()
{
    std::string s;
    for (int i = 1; i < 256; i++)
        s += (char) i;
    return s;
}
#endif // MUSHCLIENT_EMBEDDED_ENGINES

} // namespace

#ifdef MUSHCLIENT_EMBEDDED_ENGINES
TEST(TextBoundary, EveryByteSurvivesAWorldMethodOnAnyCodePage)
{
    CMUSHclientDoc doc;
    const std::string bytes = EveryByte ();
    const std::string utf8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";   // cafe, nihongo
    const std::string latin1 = "caf\xE9";

    for (const Locale& l : kLocales) {
        if (!::IsValidCodePage (l.codePage)) continue;
        ThreadOnCodePage on (l.lcid);
        const bool everyByte = ThroughTheWorld (doc, bytes) == bytes;
        const bool u = ThroughTheWorld (doc, utf8) == utf8;
        const bool x = ThroughTheWorld (doc, latin1) == latin1;
        if (!everyByte || !u || !x)
            printf ("      (%s: every byte %s, UTF-8 %s, Latin-1 %s)\n", l.name,
                    everyByte ? "ok" : "DAMAGED", u ? "ok" : "DAMAGED", x ? "ok" : "DAMAGED");
        EXPECT_TRUE (everyByte);
        EXPECT_TRUE (u);
        EXPECT_TRUE (x);
    }
}

// The call must leave the thread as it found it: the switch is for the call.
TEST(TextBoundary, AWorldMethodLeavesTheThreadsLocaleAlone)
{
    CMUSHclientDoc doc;
    ThreadOnCodePage on (kLocales[0].lcid);
    ThroughTheWorld (doc, "abc");
    EXPECT_EQ ((long) kLocales[0].lcid, (long) ::GetThreadLocale ());
}
#endif // MUSHCLIENT_EMBEDDED_ENGINES

// A UTF-8 world receiving a byte that is not UTF-8 reads it as Windows-1252,
// which is what this always meant on the Western Windows it was written for, and
// what the port's table does.  It went through the thread's code page, where a
// lone lead byte converts to nothing.
TEST(TextBoundary, ABadUtf8ByteFromTheServerIsReadAs1252OnAnyCodePage)
{
    for (const Locale& l : kLocales) {
        if (!::IsValidCodePage (l.codePage)) continue;
        ThreadOnCodePage on (l.lcid);
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = TRUE;
        doc.m_iConnectPhase = eConnectConnectedToMud;
        char caf[] = "caf\xE9 ok\n";
        doc.DisplayMsg (caf, (int) strlen (caf), 0);
        CLine* line = doc.m_LineList.GetTail ();
        for (POSITION p = doc.m_LineList.GetTailPosition (); p; ) {
            CLine* c = doc.m_LineList.GetPrev (p);
            if (c->len > 0 && std::string (c->text, c->len).find ("caf") != std::string::npos) { line = c; break; }
        }
        const std::string got (line->text, line->len);
        if (got.find ("caf\xC3\xA9 ok") == std::string::npos)
            printf ("      (%s: got \"%s\")\n", l.name, got.c_str ());
        EXPECT_TRUE (got.find ("caf\xC3\xA9 ok") != std::string::npos);
    }
}

// ── controls, in both kinds of world ───────────────────────────────────────
// These hold on any process code page, and run in the UTF-8 64-bit builds and the
// ANSI Win32 build alike: the wide APIs do not consult it.

namespace {

const char* const kUtf8Text   = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC x";    // cafe nihon x
const char* const kLatin1Text = "caf\xE9 bytes";

struct HiddenEdit {
    HWND parent, edit;
    HiddenEdit ()
    {
        parent = ::CreateWindowExA (0, "STATIC", "", WS_POPUP, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
        edit = ::CreateWindowExA (0, "EDIT", "", WS_CHILD | ES_MULTILINE, 0, 0, 90, 90,
                                  parent, (HMENU) (INT_PTR) 100, NULL, NULL);
    }
    ~HiddenEdit () { ::DestroyWindow (parent); }
};

} // namespace

TEST(TextEdges, AWorldsCodePageIsItsOwn)
{
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = TRUE;
    EXPECT_EQ (CP_UTF8, (int) WorldCodePage (&doc));
    doc.m_bUTF_8 = FALSE;
    doc.m_font_charset = ANSI_CHARSET;
    EXPECT_EQ ((int) SystemAnsiCodePage (), (int) WorldCodePage (&doc));
    doc.m_font_charset = GB2312_CHARSET;     // what its output window draws with
    EXPECT_EQ (936, (int) WorldCodePage (&doc));
    // never UTF-8 from here, whatever GetACP says under the manifest
    EXPECT_TRUE (SystemAnsiCodePage () != CP_UTF8);
}

TEST(TextEdges, AControlHoldsAUtf8WorldsRealText)
{
    HiddenEdit e;
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = TRUE;
    WorldToWindowText (e.edit, &doc, kUtf8Text);

    // real characters, not its bytes taken as Windows-1252
    wchar_t w[32] = {0};
    ::GetWindowTextW (e.edit, w, 32);
    EXPECT_EQ (0x00E9, (int) w[3]);
    EXPECT_EQ (0x65E5, (int) w[5]);

    CString back;
    WindowTextToWorld (e.edit, &doc, back);
    EXPECT_STREQ (kUtf8Text, (LPCTSTR) back);
}

TEST(TextEdges, ALatin1WorldsBytesSurviveAControlExactly)
{
    HiddenEdit e;
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = FALSE;
    doc.m_font_charset = ANSI_CHARSET;
    if (SystemAnsiCodePage () != 1252) return;      // this test's bytes are Windows-1252

    WorldToWindowText (e.edit, &doc, kLatin1Text);
    wchar_t w[32] = {0};
    ::GetWindowTextW (e.edit, w, 32);
    EXPECT_EQ (0x00E9, (int) w[3]);                 // shown as e-acute

    CString back;
    WindowTextToWorld (e.edit, &doc, back);
    EXPECT_STREQ (kLatin1Text, (LPCTSTR) back);     // and written back as E9, not U+FFFD
}

TEST(TextEdges, EveryWindows1252ByteSurvivesAControl)
{
    HiddenEdit e;
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = FALSE;
    doc.m_font_charset = ANSI_CHARSET;
    if (SystemAnsiCodePage () != 1252) return;
    std::string bytes;
    for (int i = 0x20; i < 256; i++)
        bytes += (char) i;
    WorldToWindowText (e.edit, &doc, bytes.c_str ());
    CString back;
    WindowTextToWorld (e.edit, &doc, back);
    EXPECT_STREQ (bytes.c_str (), (LPCTSTR) back);
}

TEST(TextEdges, AGbkWorldsBytesSurviveAControl)
{
    HiddenEdit e;
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = FALSE;
    doc.m_font_charset = GB2312_CHARSET;
    if (!::IsValidCodePage (936)) return;
    const char* const kGbk = "\xD6\xD0\xCE\xC4 gbk";       // zhongwen
    WorldToWindowText (e.edit, &doc, kGbk);
    wchar_t w[16] = {0};
    ::GetWindowTextW (e.edit, w, 16);
    EXPECT_EQ (0x4E2D, (int) w[0]);
    CString back;
    WindowTextToWorld (e.edit, &doc, back);
    EXPECT_STREQ (kGbk, (LPCTSTR) back);
}

// EM_GETSEL counts UTF-16 units; the world's text, which the command-box code cuts
// up with those numbers, is bytes.
TEST(TextEdges, ASelectionIsInTheWorldsBytes)
{
    HiddenEdit e;
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = TRUE;
    WorldToWindowText (e.edit, &doc, kUtf8Text);

    ::SendMessageW (e.edit, EM_SETSEL, 5, 7);        // the two kanji, as characters
    int a = 0, b = 0;
    EditGetSelBytes (e.edit, &doc, a, b);
    EXPECT_EQ (6, a);                                // c a f C3 A9 space
    EXPECT_EQ (12, b);

    EditSetSelBytes (e.edit, &doc, 6, 12);           // and back
    DWORD s = 0, f = 0;
    ::SendMessageW (e.edit, EM_GETSEL, (WPARAM) &s, (LPARAM) &f);
    EXPECT_EQ (5, (int) s);
    EXPECT_EQ (7, (int) f);

    EditReplaceSel (e.edit, &doc, "\xC3\xBC");       // u-umlaut over them
    CString back;
    WindowTextToWorld (e.edit, &doc, back);
    EXPECT_STREQ ("caf\xC3\xA9 \xC3\xBC x", (LPCTSTR) back);
}

#ifdef MUSHCLIENT_EMBEDDED_ENGINES   // Win32 has no UTF-8 manifest: its narrow window has no way to hold a kanji
// The command box is an edit control that MFC has subclassed, so its window
// procedure is narrow, and wide messages to it go through a Windows thunk that drops
// any character of three UTF-8 bytes.  A plain control does not show this.
TEST(TextEdges, KanjiSurviveAnEditThatMfcHasSubclassed)
{
    HiddenEdit e;
    CEdit edit;
    ASSERT_TRUE (edit.SubclassWindow (e.edit) != FALSE);
    {
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = TRUE;

        WorldToWindowText (e.edit, &doc, kUtf8Text);
        CString back;
        WindowTextToWorld (e.edit, &doc, back);
        EXPECT_STREQ (kUtf8Text, (LPCTSTR) back);

        // typed
        MSG m;
        memset (&m, 0, sizeof m);
        m.hwnd = e.edit;
        m.message = WM_CHAR;
        m.wParam = 0x8A9E;
        ::SendMessageW (e.edit, EM_SETSEL, 0, -1);
        EXPECT_TRUE (HandleWideCharMessage (m));
        WindowTextToWorld (e.edit, &doc, back);
        EXPECT_STREQ ("\xE8\xAA\x9E", (LPCTSTR) back);

        // replacing a selection
        EditReplaceSel (e.edit, &doc, "\xE6\x97\xA5");
        WindowTextToWorld (e.edit, &doc, back);
        EXPECT_STREQ ("\xE8\xAA\x9E\xE6\x97\xA5", (LPCTSTR) back);

        int a = 0, b = 0;
        ::SendMessageW (e.edit, EM_SETSEL, 1, 2);
        EditGetSelBytes (e.edit, &doc, a, b);
        EXPECT_EQ (3, a);
        EXPECT_EQ (6, b);
    }
    edit.UnsubclassWindow ();
}
#endif // MUSHCLIENT_EMBEDDED_ENGINES

// The whole of DDX_Text in a world's dialog, both ways.
TEST(TextEdges, DdxTextUsesTheWorldsEncodingInAWorldsDialog)
{
    HiddenEdit e;
    CWnd parent;
    parent.Attach (e.parent);
    {
        CMUSHclientDoc doc;
        doc.m_bUTF_8 = TRUE;
        CDialogsForWorld inThisWorld (&doc);

        CString value (kUtf8Text);
        { CDataExchange load (&parent, FALSE); DDX_Text (&load, 100, value); }
        wchar_t w[32] = {0};
        ::GetWindowTextW (e.edit, w, 32);
        EXPECT_EQ (0x65E5, (int) w[5]);

        value.Empty ();
        { CDataExchange save (&parent, TRUE); DDX_Text (&save, 100, value); }
        EXPECT_STREQ (kUtf8Text, (LPCTSTR) value);
    }
    parent.Detach ();
}

TEST(TextEdges, EveryCharacterTypedReachesAnEditWhole)
{
    HiddenEdit e;
    CMUSHclientDoc doc;
    doc.m_bUTF_8 = TRUE;

    MSG m;
    memset (&m, 0, sizeof m);
    m.hwnd = e.edit;
    m.message = WM_CHAR;
    m.wParam = 0x65E5;                                // a kanji
    EXPECT_TRUE (HandleWideCharMessage (m));
    m.wParam = 0xD83D;                                // an emoji, in two halves
    EXPECT_TRUE (HandleWideCharMessage (m));
    m.wParam = 0xDE00;
    EXPECT_TRUE (HandleWideCharMessage (m));

    CString back;
    WindowTextToWorld (e.edit, &doc, back);
    EXPECT_STREQ ("\xE6\x97\xA5\xF0\x9F\x98\x80", (LPCTSTR) back);

    m.wParam = 'a';                                   // ASCII goes the usual way
    EXPECT_FALSE (HandleWideCharMessage (m));
    m.wParam = 0x65E5;
    m.hwnd = e.parent;                                // and so does anything not an edit
    EXPECT_FALSE (HandleWideCharMessage (m));
}

// Everything a Lua script reads through a VARIANT, as well as the embedded engines.
#ifdef MUSHCLIENT_EMBEDDED_ENGINES
TEST(TextBoundary, AWorldMethodsVariantHoldsTheBytesWhateverTheyAre)
{
    CMUSHclientDoc doc;
    const std::string bytes = EveryByte ();
    doc.SetVariable ("v", bytes.c_str ());
    VARIANT got = doc.GetVariable ("v");
    EXPECT_EQ (VT_BSTR, (int) got.vt);
    CString back = ScriptBytes (got.bstrVal);
    EXPECT_STREQ (bytes.c_str (), (LPCTSTR) back);
    ::VariantClear (&got);
}

TEST(TextBoundary, TheProcessIsUtf8InThe64BitBuilds)
{
    EXPECT_EQ (CP_UTF8, (int) ::GetACP ());      // the manifest took, on this Windows
}
#else
TEST(TextBoundary, TheProcessKeepsTheMachinesCodePageInWin32)
{
    EXPECT_TRUE (::GetACP () != CP_UTF8);
}
#endif
