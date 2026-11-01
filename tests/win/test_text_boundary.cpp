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
