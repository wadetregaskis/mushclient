// tests/test_info.cpp
// Behaviour tests for scripting/methods/methods_info.cpp: the world.GetInfo
// selector (per-code type contract + representative value arms), GetLineInfo,
// GetStyleInfo, host lookups, ErrorDesc and the small info getters.
//
// GetInfo is the scripting API's grab-bag introspection call: each InfoType
// code has a FIXED VARIANT type (string / bool / long / double / date), and
// unknown codes answer VT_NULL.  That per-code type contract is what plugins
// key off, so it is codified here wholesale, plus precise value checks where
// the value is deducible headless.

#include "mc_test.h"
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"   // eOK etc. (self-sufficiency for the
                                // real-header flip; no-op today)

#include <cstring>
#include <cstdio>
#include <string>
#include <unistd.h>

static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
}

static void feed(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

static std::string vstr(const VARIANT& v)
{
    if (v.vt != VT_BSTR) return "<not-a-string>";
    return std::string((const char*)CString(v.bstrVal));
}

// ── GetInfo type contract ─────────────────────────────────────────────────

// Shared loop (audit §4): assert every code in `codes` answers `wantVt`.
template <size_t N>
static void expect_all_vt(CMUSHclientDoc& doc, const int (&codes)[N],
                          int wantVt, const char* label)
{
    for (int c : codes) {
        VARIANT v = doc.GetInfo(c);
        if ((int)v.vt != wantVt) {
            char msg[96];
            snprintf(msg, sizeof msg, "GetInfo(%d) vt=%d, wanted %s", c, (int)v.vt, label);
            mc_test::fail_msg(__FILE__, __LINE__, msg);
        }
    }
}

// methods_info.cpp GetInfo: codes 1-89 are the string group.
TEST(GetInfo, StringCodesAnswerVtBstr)
{
    CMUSHclientDoc doc; make_doc(doc);
    static const int codes[] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
        21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,
        61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,
        81,82,83,84,85,86,87,88,89 };
    expect_all_vt(doc, codes, VT_BSTR, "VT_BSTR");
}

// methods_info.cpp GetInfo: codes 101-125 (as implemented) are booleans.
TEST(GetInfo, BooleanCodesAnswerVtBool)
{
    CMUSHclientDoc doc; make_doc(doc);
    static const int codes[] = {
        101,102,103,104,105,106,107,108,109,110,111,112,113,114,
        115,118,119,120,121,122,123,124,125 };
    expect_all_vt(doc, codes, VT_BOOL, "VT_BOOL");
    // 116/117 were never allocated: they fall to the VT_NULL default
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(116).vt);
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(117).vt);
}

// methods_info.cpp GetInfo: the numeric group answers VT_I4 (a few arms are
// VT_I2 / VT_R8 — codified separately below).
TEST(GetInfo, NumericCodesAnswerVtI4)
{
    CMUSHclientDoc doc; make_doc(doc);
    static const int codes[] = {
        201,202,203,204,205,206,207,208,209,210,
        211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
        226,227,228,229,230,231,235,238,240,241,242,243,244,
        245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,
        260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,
        275,276,277,278,279,280,281,282,283,284,286,287,288,289,290,
        291,292,293,294,295,298,299,300,310 };
    expect_all_vt(doc, codes, VT_I4, "VT_I4");
    EXPECT_EQ(VT_I2,   (int)doc.GetInfo(239).vt);   // action source is a short
    EXPECT_EQ(VT_BOOL, (int)doc.GetInfo(285).vt);   // output-window-available
    EXPECT_EQ(VT_R8,   (int)doc.GetInfo(232).vt);   // timer (epoch fallback)
    EXPECT_EQ(VT_R8,   (int)doc.GetInfo(297).vt);   // counter frequency
    // no high-performance counter headless (App.m_iCounterFrequency == 0):
    // the trigger/alias time arms have no fallback and stay VT_EMPTY
    EXPECT_EQ(VT_EMPTY, (int)doc.GetInfo(233).vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetInfo(234).vt);
    // no world view attached: the command-selection arms stay VT_EMPTY
    EXPECT_EQ(VT_EMPTY, (int)doc.GetInfo(236).vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetInfo(237).vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetInfo(296).vt);  // scroll position, no sink
}

// methods_info.cpp GetInfo: unknown codes answer VT_NULL (not VT_EMPTY) —
// scripts can distinguish "no such code" from "no value yet".
TEST(GetInfo, UnknownCodeAnswersVtNull)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(0).vt);
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(90).vt);      // hole after string block
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(311).vt);
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(9999).vt);
    EXPECT_EQ(VT_NULL, (int)doc.GetInfo(-1).vt);
}

// ── GetInfo value arms ────────────────────────────────────────────────────

// The string arms reflect the live document fields verbatim.
TEST(GetInfo, StringArmsReflectDocumentFields)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_server    = "mud.example.com";
    doc.m_mush_name = "Example World";
    doc.m_notes     = "some world notes";
    doc.m_strTerminalIdentification = "mushclient-test";

    EXPECT_STREQ("mud.example.com",  vstr(doc.GetInfo(1)).c_str());
    EXPECT_STREQ("Example World",    vstr(doc.GetInfo(2)).c_str());
    EXPECT_STREQ("some world notes", vstr(doc.GetInfo(8)).c_str());
    EXPECT_STREQ("mushclient-test",  vstr(doc.GetInfo(42)).c_str());

    // 72 is the client version constant
    EXPECT_STREQ(MUSHCLIENT_VERSION, vstr(doc.GetInfo(72)).c_str());
    // 83 is the sqlite3 runtime version
    EXPECT_STREQ(sqlite3_libversion(), vstr(doc.GetInfo(83)).c_str());
}

// GetInfo(64) is the current working directory WITH a trailing slash.
TEST(GetInfo, CurrentDirectoryHasTrailingSlash)
{
    CMUSHclientDoc doc; make_doc(doc);
    std::string dir = vstr(doc.GetInfo(64));
    ASSERT_TRUE(!dir.empty());
    EXPECT_EQ('/', dir[dir.size() - 1]);

    char cwd[1024];
    ASSERT_NE(nullptr, (void*)getcwd(cwd, sizeof cwd));
    EXPECT_EQ(0, strncmp(cwd, dir.c_str(), strlen(cwd)));
}

// headless: os_version is a zeroed stub — GetInfo(77) "" and 265-268 = 0
// (codifies the comment at the top of methods_info.cpp).
TEST(GetInfo, OsVersionStubIsZeroHeadless)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_STREQ("", vstr(doc.GetInfo(77)).c_str());
    EXPECT_EQ(0L, doc.GetInfo(265).lVal);
    EXPECT_EQ(0L, doc.GetInfo(266).lVal);
    EXPECT_EQ(0L, doc.GetInfo(267).lVal);
    EXPECT_EQ(0L, doc.GetInfo(268).lVal);
}

// GetInfo(106)/(107): "world is disconnected" vs "world is connecting" are
// derived from m_iConnectPhase, not stored flags.
TEST(GetInfo, ConnectPhaseBooleansTrackPhase)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.m_iConnectPhase = eConnectConnectedToMud;
    EXPECT_FALSE(doc.GetInfo(106).boolVal);   // not disconnected
    EXPECT_FALSE(doc.GetInfo(107).boolVal);   // not connecting

    doc.m_iConnectPhase = eConnectNotConnected;
    EXPECT_TRUE(doc.GetInfo(106).boolVal);   // disconnected
    EXPECT_FALSE(doc.GetInfo(107).boolVal);   // but not connecting

    doc.m_iConnectPhase = eConnectConnectingToMud;
    EXPECT_TRUE(doc.GetInfo(106).boolVal);    // not yet connected
    EXPECT_TRUE(doc.GetInfo(107).boolVal);    // mid-handshake

    // 227 exposes the raw phase number itself
    EXPECT_EQ((long)eConnectConnectingToMud, doc.GetInfo(227).lVal);
}

// GetInfo(218) counts variables; 224 counts output-buffer lines.
TEST(GetInfo, CountsTrackVariableAndLineStores)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(0L, doc.GetInfo(218).lVal);
    doc.SetVariable("alpha", "1");
    doc.SetVariable("beta",  "2");
    EXPECT_EQ(2L, doc.GetInfo(218).lVal);

    long before = doc.GetInfo(224).lVal;
    EXPECT_EQ((long)doc.m_LineList.GetCount(), before);
    feed(doc, "one\ntwo\n");
    EXPECT_EQ((long)doc.m_LineList.GetCount(), doc.GetInfo(224).lVal);
    EXPECT_GT(doc.GetInfo(224).lVal, before);
}

// GetInfo(285): "is output window available?" — true once the line buffer
// exists (which the constructor guarantees).
TEST(GetInfo, OutputAvailableMirrorsCurrentLine)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ((BOOL)(doc.m_pCurrentLine != NULL), doc.GetInfo(285).boolVal);
    feed(doc, "line\n");
    EXPECT_TRUE(doc.GetInfo(285).boolVal);
}

// headless GUI metric arms: toolbars don't exist (0), world-frame metrics
// derive from the m_gui_* fields, and the text rectangle answers its fields.
TEST(GetInfo, HeadlessGuiMetricsDeriveFromGuiFields)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_gui_rows = 25;
    doc.m_gui_cols = 80;
    doc.m_gui_font_height = 10;
    doc.m_gui_font_width  = 6;

    EXPECT_EQ(0L, doc.GetInfo(251).lVal);            // toolbar height: none
    EXPECT_EQ(0L, doc.GetInfo(256).lVal);            // toolbar width: none
    EXPECT_EQ(0L, doc.GetInfo(259).lVal);            // status bar: none
    EXPECT_EQ(25L * 10L, doc.GetInfo(261).lVal);     // frame height
    EXPECT_EQ(80L * 6L,  doc.GetInfo(262).lVal);     // frame width
    EXPECT_EQ(27L * 10L, doc.GetInfo(249).lVal);     // main client height (+2 rows)
    EXPECT_EQ(80L * 6L,  doc.GetInfo(250).lVal);     // main client width
    EXPECT_EQ(25L * 10L, doc.GetInfo(280).lVal);     // output window height
    EXPECT_EQ(80L * 6L,  doc.GetInfo(281).lVal);     // output window width
    EXPECT_EQ(0L, doc.GetInfo(290).lVal);            // text rect left @ origin
    EXPECT_EQ(0L, doc.GetInfo(291).lVal);            // text rect top @ origin
    EXPECT_EQ(80L * 6L,  doc.GetInfo(292).lVal);     // text rect right
    EXPECT_EQ(25L * 10L, doc.GetInfo(293).lVal);     // text rect bottom

    doc.m_TextRectangle.left = 3;
    doc.m_TextRectangle.top = 4;
    doc.m_TextRectangle.right = 5;
    doc.m_TextRectangle.bottom = 6;
    EXPECT_EQ(3L, doc.GetInfo(272).lVal);
    EXPECT_EQ(4L, doc.GetInfo(273).lVal);
    EXPECT_EQ(5L, doc.GetInfo(274).lVal);
    EXPECT_EQ(6L, doc.GetInfo(275).lVal);
}

// GetInfo(294): headless there is no keyboard hook, so no modifier bits.
// GetInfo(299)/(300): the shim code pages are the classic Windows defaults.
TEST(GetInfo, KeyStateAndCodePagesHeadless)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(0L,    doc.GetInfo(294).lVal);
    EXPECT_EQ(1252L, doc.GetInfo(299).lVal);   // ANSI: Western European
    EXPECT_EQ(437L,  doc.GetInfo(300).lVal);   // OEM: US
}

// date arms: zero times answer VT_EMPTY ("in case dates are zero"), the
// current-time arms always answer VT_DATE.
TEST(GetInfo, ZeroDatesAnswerEmptyLiveDatesAnswerDate)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(VT_DATE, (int)doc.GetInfo(301).vt);    // B-3: real ctor sets
                                                     // m_tConnectTime=now (doc_construct.cpp:345)
    EXPECT_EQ(VT_DATE,  (int)doc.GetInfo(304).vt);   // now
    EXPECT_EQ(VT_DATE,  (int)doc.GetInfo(305).vt);   // client start
    EXPECT_EQ(VT_DATE,  (int)doc.GetInfo(306).vt);   // world start

    doc.m_tConnectTime = CTime::GetCurrentTime();
    EXPECT_EQ(VT_DATE, (int)doc.GetInfo(301).vt);    // now it has a value
}

// ── GetLineInfo ───────────────────────────────────────────────────────────

// GetLineInfo: line numbers are 1-based; out-of-range answers VT_EMPTY and
// a bad InfoType answers VT_NULL (same convention as GetStyleInfo).
TEST(GetLineInfo, RangeAndTypeConventions)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "hello world\n");

    EXPECT_EQ(VT_EMPTY, (int)doc.GetLineInfo(0, 1).vt);      // 0: below range
    EXPECT_EQ(VT_EMPTY, (int)doc.GetLineInfo(-5, 1).vt);
    EXPECT_EQ(VT_EMPTY,
              (int)doc.GetLineInfo(doc.m_LineList.GetCount() + 1, 1).vt);
    EXPECT_EQ(VT_NULL,  (int)doc.GetLineInfo(1, 99).vt);     // bad info type
    EXPECT_EQ(VT_NULL,  (int)doc.GetLineInfo(1, 0).vt);
}

// GetLineInfo type 1..11: text, length, hard-return flag, note flag, line
// number and style count of a real received line.
TEST(GetLineInfo, DescribesReceivedLine)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "hello world\n");

    VARIANT v = doc.GetLineInfo(1, 1);                   // text
    ASSERT_EQ(VT_BSTR, (int)v.vt);
    EXPECT_STREQ("hello world", vstr(v).c_str());

    EXPECT_EQ((short)11, doc.GetLineInfo(1, 2).iVal);    // length
    EXPECT_TRUE(doc.GetLineInfo(1, 3).boolVal);   // hard return
    EXPECT_FALSE(doc.GetLineInfo(1, 4).boolVal);  // not a note
    EXPECT_FALSE(doc.GetLineInfo(1, 5).boolVal);  // not user input
    EXPECT_EQ(1L, doc.GetLineInfo(1, 10).lVal);          // line number, 1-based
    EXPECT_GE(doc.GetLineInfo(1, 11).lVal, 1L);          // has style runs
    EXPECT_EQ(VT_DATE, (int)doc.GetLineInfo(1, 9).vt);   // arrival time
}

// GetLineInfo type 4: a world.Note line reports the COMMENT flag.
TEST(GetLineInfo, NoteLineReportsCommentFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Note("a note");
    EXPECT_TRUE(doc.GetLineInfo(1, 4).boolVal);
    EXPECT_STREQ("a note", vstr(doc.GetLineInfo(1, 1)).c_str());
}

// ── GetStyleInfo ──────────────────────────────────────────────────────────

// GetStyleInfo range conventions mirror GetLineInfo: bad line OR bad style
// answer VT_EMPTY, bad info type answers VT_NULL.
TEST(GetStyleInfo, RangeAndTypeConventions)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "styled\n");

    EXPECT_EQ(VT_EMPTY, (int)doc.GetStyleInfo(0, 1, 1).vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetStyleInfo(999, 1, 1).vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetStyleInfo(1, 0, 1).vt);
    EXPECT_EQ(VT_EMPTY, (int)doc.GetStyleInfo(1, 99, 1).vt);
    EXPECT_EQ(VT_NULL,  (int)doc.GetStyleInfo(1, 1, 99).vt);
}

// GetStyleInfo 1-4 on a plain MUD line: full text in one non-action run
// starting at column 1.
TEST(GetStyleInfo, PlainRunTextColumnAndAction)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "plain text\n");

    EXPECT_STREQ("plain text", vstr(doc.GetStyleInfo(1, 1, 1)).c_str());
    EXPECT_EQ((short)10, doc.GetStyleInfo(1, 1, 2).iVal);   // run length
    EXPECT_EQ((short)1,  doc.GetStyleInfo(1, 1, 3).iVal);   // 1-based column
    EXPECT_EQ((short)0,  doc.GetStyleInfo(1, 1, 4).iVal);   // ACTION_NONE
    EXPECT_STREQ("", vstr(doc.GetStyleInfo(1, 1, 5)).c_str());  // no action
    EXPECT_STREQ("", vstr(doc.GetStyleInfo(1, 1, 6)).c_str());  // no hint
    EXPECT_STREQ("", vstr(doc.GetStyleInfo(1, 1, 7)).c_str());  // no variable
}

// GetStyleInfo 4-7 on a Hyperlink note: action type 2 with the action and
// hint recorded on the style's CAction.
TEST(GetStyleInfo, HyperlinkRunCarriesActionAndHint)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Hyperlink("http://example.com/", "click here", "the hint",
                  "white", "black", FALSE);
    doc.Note("");    // close the hyperlink line

    // find the style run holding the link text
    long styles = doc.GetLineInfo(1, 11).lVal;
    ASSERT_TRUE(styles >= 1L);
    long link = -1;
    for (long s = 1; s <= styles; s++)
        if (vstr(doc.GetStyleInfo(1, s, 1)) == "click here") { link = s; break; }
    ASSERT_NE(-1L, link);

    EXPECT_EQ((short)1, doc.GetStyleInfo(1, link, 4).iVal);   // ACTION_SEND (URL=FALSE)
    EXPECT_STREQ("http://example.com/",
                 vstr(doc.GetStyleInfo(1, link, 5)).c_str()); // action
    EXPECT_STREQ("the hint",
                 vstr(doc.GetStyleInfo(1, link, 6)).c_str()); // hint
    EXPECT_EQ(VT_BOOL, (int)doc.GetStyleInfo(1, link, 8).vt); // bold flag typed

    // with URL=TRUE the same call makes a real hyperlink action (type 2)
    CMUSHclientDoc doc2; make_doc(doc2);
    doc2.Hyperlink("http://example.com/", "web link", "", "white", "black", TRUE);
    doc2.Note("");
    long styles2 = doc2.GetLineInfo(1, 11).lVal;
    long link2 = -1;
    for (long s = 1; s <= styles2; s++)
        if (vstr(doc2.GetStyleInfo(1, s, 1)) == "web link") { link2 = s; break; }
    ASSERT_NE(-1L, link2);
    EXPECT_EQ((short)2, doc2.GetStyleInfo(1, link2, 4).iVal); // ACTION_HYPERLINK
}

// ── the small info getters ────────────────────────────────────────────────

// GetSelection*: no output pane attached headless — all zeros (the
// "no selection" contract).
TEST(InfoGetters, NoSelectionWithoutView)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(0L, doc.GetSelectionStartLine());
    EXPECT_EQ(0L, doc.GetSelectionEndLine());
    EXPECT_EQ(0L, doc.GetSelectionStartColumn());
    EXPECT_EQ(0L, doc.GetSelectionEndColumn());
}

// GetSentBytes/GetReceivedBytes mirror the byte counters; GetConnectDuration
// is zero when never connected.
TEST(InfoGetters, ByteCountersAndDuration)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_nBytesOut = 123;
    doc.m_nBytesIn  = 456;
    EXPECT_EQ(123L, doc.GetSentBytes());
    EXPECT_EQ(456L, doc.GetReceivedBytes());

    // duration is 0 while not connected; once connected it is measured from
    // m_tConnectTime
    doc.m_iConnectPhase = eConnectNotConnected;
    EXPECT_EQ(0L, doc.GetConnectDuration());
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_tConnectTime = CTime::GetCurrentTime();
    EXPECT_LE(0L, doc.GetConnectDuration());
    EXPECT_GT(5L, doc.GetConnectDuration());     // just connected: < 5s
}

// WorldName/WorldAddress/WorldPort/GetWorldID reflect the document.
TEST(InfoGetters, WorldIdentityGetters)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_mush_name = "My World";
    doc.m_server    = "mud.host.org";
    doc.m_port      = 4000;

    EXPECT_STREQ("My World",     (const char*)CString(doc.WorldName()));
    EXPECT_STREQ("mud.host.org", (const char*)CString(doc.WorldAddress()));
    EXPECT_EQ(4000L, doc.WorldPort());
    EXPECT_STREQ((const char*)doc.m_strWorldID,
                 (const char*)CString(doc.GetWorldID()));
    EXPECT_EQ(24, (int)strlen((const char*)CString(doc.GetWorldID())));
}

// ErrorDesc: known codes translate to their message; unknown codes echo the
// number itself as a string.
TEST(InfoGetters, ErrorDescKnownAndUnknown)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_STREQ("No error", (const char*)CString(doc.ErrorDesc(0)));
    EXPECT_STREQ("The world is already open",
                 (const char*)CString(doc.ErrorDesc(30001)));
    EXPECT_STREQ("12345", (const char*)CString(doc.ErrorDesc(12345)));
    EXPECT_STREQ("-7",    (const char*)CString(doc.ErrorDesc(-7)));
}

// GetHostAddress: blank name gives an empty VARIANT; "localhost" resolves to
// an array containing 127.0.0.1.  GetHostName: blank or invalid input gives
// "" (never an error).
TEST(InfoGetters, HostLookups)
{
    CMUSHclientDoc doc; make_doc(doc);

    // MFC parity: a never-created COleSafeArray detaches as VT_EMPTY, so a
    // blank host name answers an empty VARIANT (nil in Lua).
    VARIANT v = doc.GetHostAddress("");
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);

    v = doc.GetHostAddress("localhost");
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    // one of the returned addresses must be the loopback
    COleSafeArray sa;
    sa.Attach(v);
    long n = sa.GetOneDimSize();
    ASSERT_TRUE(n >= 1);
    bool found = false;
    for (long i = 0; i < n; i++) {
        VARIANT e;
        VariantInit(&e);
        sa.GetElement(&i, &e);
        if (e.vt == VT_BSTR && vstr(e) == "127.0.0.1") found = true;
    }
    EXPECT_TRUE(found);

    EXPECT_STREQ("", (const char*)CString(doc.GetHostName("")));
    EXPECT_STREQ("", (const char*)CString(doc.GetHostName("not.an.ip")));
    // reverse lookup of loopback answers the canonical local name
    CString rev(doc.GetHostName("127.0.0.1"));
    EXPECT_TRUE(rev.GetLength() > 0);
}

// IsConnected/GetLineCount plain getters.
TEST(InfoGetters, ConnectionAndLineCount)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_TRUE(doc.IsConnected());
    doc.m_iConnectPhase = eConnectNotConnected;
    EXPECT_FALSE(doc.IsConnected());

    long before = doc.GetLinesInBufferCount();
    feed(doc, "a\nb\n");
    EXPECT_EQ((long)doc.m_LineList.GetCount(), doc.GetLinesInBufferCount());
    EXPECT_GT(doc.GetLinesInBufferCount(), before);
}

// ── global options (globalregistryoptions.cpp) ────────────────────────────

// GetGlobalOption resolves numeric options as VT_I4, alpha options as
// VT_BSTR, matches names case-insensitively, and answers VT_EMPTY for
// unknown names.
TEST(GlobalOptions, LookupNumericAlphaAndUnknown)
{
    CMUSHclientDoc doc; make_doc(doc);

    App.m_bAutoConnectWorlds = 1;
    VARIANT v = App.GetGlobalOption("AutoConnectWorlds");
    ASSERT_EQ(VT_I4, (int)v.vt);
    EXPECT_EQ(1L, v.lVal);
    App.m_bAutoConnectWorlds = 0;
    EXPECT_EQ(0L, App.GetGlobalOption("AutoConnectWorlds").lVal);
    // case-insensitive name match
    EXPECT_EQ(0L, App.GetGlobalOption("autoconnectworlds").lVal);

    App.m_strDefaultInputFont = "SomeFont";
    v = App.GetGlobalOption("DefaultInputFont");
    ASSERT_EQ(VT_BSTR, (int)v.vt);
    EXPECT_STREQ("SomeFont", vstr(v).c_str());

    EXPECT_EQ(VT_EMPTY, (int)App.GetGlobalOption("NoSuchOption").vt);

    // the doc-level method is a straight forwarder to App
    EXPECT_EQ(0L, doc.GetGlobalOption("AutoConnectWorlds").lVal);
    App.m_bAutoConnectWorlds = 1;
}

// GetGlobalOptionList enumerates the name of every numeric and alpha option.
TEST(GlobalOptions, ListEnumeratesEveryOptionName)
{
    CMUSHclientDoc doc; make_doc(doc);
    VARIANT v = doc.GetGlobalOptionList();
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    COleSafeArray sa;
    sa.Attach(v);
    long n = sa.GetOneDimSize();
    EXPECT_GT(n, 50L);                 // dozens of options, both families
    bool numeric = false, alpha = false;
    for (long i = 0; i < n; i++) {
        VARIANT e; VariantInit(&e);
        sa.GetElement(&i, &e);
        std::string name = vstr(e);
        if (name == "AutoConnectWorlds") numeric = true;
        if (name == "DefaultInputFont")  alpha = true;
    }
    EXPECT_TRUE(numeric);
    EXPECT_TRUE(alpha);
}

// headless database plumbing: PopulateDatabase / SaveGlobalsToDatabase are
// inert (writes swallowed, SQLITE_OK), and LoadGlobalsFromDatabase resolves
// every option back to its table default.
TEST(GlobalOptions, HeadlessDatabaseRoundTripRestoresDefaults)
{
    EXPECT_EQ(SQLITE_OK, App.PopulateDatabase());
    App.SaveGlobalsToDatabase();       // must not crash / throw

    App.m_bAutoConnectWorlds = 0;      // perturb away from the default (1)
    App.LoadGlobalsFromDatabase();
    EXPECT_EQ(1L, App.GetGlobalOption("AutoConnectWorlds").lVal);
}

// ShowGlobalOptions notes every option (name = value) into the world.
// The one candidate skipped in the intent-comment sweep, now testable: the
// prefs-DB path is injectable via App.m_PreferencesDatabaseName, so we can
// seed a FRESH database in the scratch area and prove the comment at
// globalregistryoptions.cpp:132 — the POSIX directory defaults are shared by
// PopulateDatabase (seed) and the read path, so a freshly-populated DB can
// never diverge from what a re-read resolves (the historical bug was a
// CWD-relative ".\worlds\plugins" surviving in a fresh DB).
//
// LoadGlobalsFromDatabase resets EVERY App-global option field to its
// compiled default (globalregistryoptions.cpp's GlobalOptionsTable /
// AlphaGlobalOptionsTable are both file-static, so there is no way to
// snapshot/restore the ~77 fields individually from outside that TU) —
// calling it against the live App singleton used to silently clobber
// whatever any EARLIER TEST() in this binary had already set on App.  Both
// methods write through `this`-relative offsets, so a throwaway
// CMUSHclientApp takes exactly the same writes without touching the live App
// at all.  PopulateDatabase stays on the real App, though: it aliases a
// STATIC table entry's sDefault into `this->m_strFixedPitchFont`'s buffer
// (the fixed-pitch-font fixup a few lines into its alpha-option loop), which
// must outlive the call — a throwaway object's buffer would dangle the
// instant it went out of scope, corrupting that table entry for every LATER
// PopulateDatabase() call in the process.
//
// The TEST immediately below perturbs an unrelated App global away from its
// compiled default; the TEST immediately after
// FreshDatabaseSeedAndReadShareDirectoryDefaults confirms it survived.
// mc_test runs TEST()s in the order they appear in the file, so this
// three-part bracket is deliberate — it is what makes the fix above
// falsifiable: point FreshDatabaseSeedAndReadShareDirectoryDefaults's
// LoadGlobalsFromDatabase() call back at App (instead of `scratch`) and the
// assertion after it fails, because m_bAutoExpand flips back to its
// compiled default (1) along with everything else App resets.
TEST(GlobalOptions, PerturbAutoExpandBeforeDatabaseReset)
{
    App.m_bAutoExpand = 0;   // compiled default (AutoExpandConfig) is 1
}

TEST(GlobalOptions, FreshDatabaseSeedAndReadShareDirectoryDefaults)
{
    const char* dbPath = "/tmp/mc_test_prefs_seed.sqlite";
    remove(dbPath);
    std::string savedDb = App.m_PreferencesDatabaseName;

    App.m_PreferencesDatabaseName = dbPath;
    App.PopulateDatabase();            // seed a fresh DB via the shared defaults
    App.m_PreferencesDatabaseName = savedDb;

    // CWinApp()'s ctor hijacks the process-global g_pWinApp to point at
    // whatever CWinApp-derived object was constructed most recently; put it
    // back immediately so AfxGetApp() keeps answering the real App for every
    // other test in this binary.
    CWinApp* savedWinApp = g_pWinApp;
    CMUSHclientApp scratch;
    g_pWinApp = savedWinApp;

    scratch.m_PreferencesDatabaseName = dbPath;
    scratch.m_strPluginsDirectory = "@@clobbered@@";
    scratch.LoadGlobalsFromDatabase();   // read path must restore the same value

    EXPECT_TRUE(scratch.m_strPluginsDirectory != "@@clobbered@@");
    // never a CWD-relative Windows path
    EXPECT_TRUE(scratch.m_strPluginsDirectory.GetLength() > 0);
    EXPECT_NE('.', scratch.m_strPluginsDirectory[0]);
    EXPECT_EQ(-1, scratch.m_strPluginsDirectory.Find("\\"));

    remove(dbPath);
}

// See PerturbAutoExpandBeforeDatabaseReset above: the perturbed value must
// have survived FreshDatabaseSeedAndReadShareDirectoryDefaults running in
// between.
TEST(GlobalOptions, AutoExpandSurvivesDatabaseResetAfterwards)
{
    EXPECT_EQ(0u, App.m_bAutoExpand);
    App.m_bAutoExpand = 1;   // restore the real default for any later test
}

TEST(GlobalOptions, ShowGlobalOptionsNotesEveryOption)
{
    CMUSHclientDoc doc; make_doc(doc);
    long before = (long)doc.m_LineList.GetCount();
    App.ShowGlobalOptions(&doc);
    EXPECT_GT((long)doc.m_LineList.GetCount(), before + 50);

    bool found = false;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        if (CString(l->text, l->len).Find("AutoConnectWorlds") != -1)
            { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ═══ Round-4 coverage: timing/selection/scroll/key-state GetInfo arms ═══════

#include "fake_worldview.h"

// Timing-family codes with a performance-counter frequency published.
TEST(GetInfoR4, TimingCodesWithCounterFrequency)
{
    LONGLONG saved = App.m_iCounterFrequency;
    App.m_iCounterFrequency = 1000000;
    CMUSHclientDoc doc; make_doc(doc);

    // give a trigger and an alias a compiled regexp so the sum loops run
    doc.AddTrigger("t1", "match this", "", 1, -1, 0, "", "");
    doc.AddAlias("a1", "match that", "", 1, "");

    VARIANT v = doc.GetInfo(232);      // high-resolution timer
    EXPECT_EQ((int)VT_R8, (int)v.vt);
    v = doc.GetInfo(233);              // time taken doing triggers
    EXPECT_EQ((int)VT_R8, (int)v.vt);
    v = doc.GetInfo(234);              // time taken doing aliases
    EXPECT_EQ((int)VT_R8, (int)v.vt);

    App.m_iCounterFrequency = saved;
}

// Log-file size arm (231) with a live log.
TEST(GetInfoR4, LogFileSizeWhileOpen)
{
    CMUSHclientDoc doc; make_doc(doc);
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_info_%d.log", (int)getpid());
    ASSERT_EQ((long)eOK, doc.OpenLog(path, FALSE));
    doc.WriteToLog("0123456789", 10);
    VARIANT v = doc.GetInfo(231);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(10, (int)v.lVal);
    doc.CloseLog();
    unlink(path);
}

// Command-selection (236/237) and scroll-position (296) arms answer through
// the world-view seam.
TEST(GetInfoR4, SelectionAndScrollThroughWorldView)
{
    CMUSHclientDoc doc; make_doc(doc);
    FakeWorldView view;
    view.cmd = "hello world";
    view.cmdSel.start = 2;
    view.cmdSel.end = 7;
    view.scroll = 42;
    doc.m_pWorldView = &view;

    VARIANT v = doc.GetInfo(236);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(3, (int)v.lVal);         // 1-based start
    v = doc.GetInfo(237);
    EXPECT_EQ(7, (int)v.lVal);
    v = doc.GetInfo(296);
    EXPECT_EQ(42, (int)v.lVal);

    doc.m_pWorldView = nullptr;
}

// Virtual-key state bitmask (294) through the key-state hook.
TEST(GetInfoR4, KeyStateBitmaskThroughHook)
{
    g_wxhook_get_key_state = [](int vk) -> short {
        switch (vk) {
            case VK_SHIFT: case VK_CONTROL: case VK_MENU:
            case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
            case VK_LMENU: case VK_RMENU:
            case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON:
                return 0x8000;                    // held down
            case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
                return 0x8001;                    // held AND toggled
            default: return 0;
        }
    };
    CMUSHclientDoc doc; make_doc(doc);
    VARIANT v = doc.GetInfo(294);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(0x7EFFF & ~0x1000, (int)(v.lVal & 0x7FFFF));
    g_wxhook_get_key_state = nullptr;
}

// Date codes 302/303 answer VT_DATE once the timestamps are non-zero.
TEST(GetInfoR4, FlushAndScriptModDates)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_LastFlushTime = CTime::GetCurrentTime();
    doc.m_timeScriptFileMod = CTime::GetCurrentTime();
    VARIANT v = doc.GetInfo(302);
    EXPECT_EQ((int)VT_DATE, (int)v.vt);
    v = doc.GetInfo(303);
    EXPECT_EQ((int)VT_DATE, (int)v.vt);
}

// GetLineInfo flag codes 6/7/8 and the timing codes 12/13.
TEST(GetInfoR4, LineInfoFlagAndTimingCodes)
{
    LONGLONG saved = App.m_iCounterFrequency;
    App.m_iCounterFrequency = 1000000;
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "plain line\r\n");

    VARIANT v = doc.GetLineInfo(1, 6);   // LOG_LINE flag
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetLineInfo(1, 7);           // BOOKMARK flag
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetLineInfo(1, 8);           // HORIZ_RULE flag
    EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v = doc.GetLineInfo(1, 12);          // high-precision timestamp
    EXPECT_EQ((int)VT_R8, (int)v.vt);
    v = doc.GetLineInfo(1, 13);          // seconds since world start
    EXPECT_EQ((int)VT_R8, (int)v.vt);

    // without a counter frequency, code 13 takes the wall-clock arm
    App.m_iCounterFrequency = 0;
    v = doc.GetLineInfo(1, 13);
    EXPECT_EQ((int)VT_R8, (int)v.vt);

    App.m_iCounterFrequency = saved;
}

// GetStyleInfo with a style number beyond the end answers empty.
TEST(GetInfoR4, StyleInfoBeyondLastStyleEmpty)
{
    CMUSHclientDoc doc; make_doc(doc);
    feed(doc, "styled\r\n");
    VARIANT v = doc.GetStyleInfo(1, 99, 1);
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
}


// GetInfo date codes 301/302/303 take the zero-time FALSE branch of their
// guards (methods_info.cpp:1138/1142/1146): when the underlying CTime reads 0,
// the arm skips SetUpVariantDate and the VARIANT stays VT_EMPTY (set at :408).
// The headless ctor seeds m_tConnectTime/m_LastFlushTime = now
// (doc_construct.cpp:239/345) and m_timeScriptFileMod = 0 (:347), so the sibling
// date tests only ever hit the non-zero TRUE branch; an explicit zero is the
// only way to exercise the guard for all three.  CTime() default-constructs to
// t_=0 so GetTime()==0 (platform/mfc_time.h:71,126).
TEST(GetInfoR5, ZeroDateCodesTakeEmptyBranch)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_tConnectTime      = CTime();   // GetTime() == 0 -> guard 1138 false
    doc.m_LastFlushTime     = CTime();   //                   guard 1142 false
    doc.m_timeScriptFileMod = CTime();   //                   guard 1146 false

    EXPECT_EQ((int)VT_EMPTY, (int)doc.GetInfo(301).vt);
    EXPECT_EQ((int)VT_EMPTY, (int)doc.GetInfo(302).vt);
    EXPECT_EQ((int)VT_EMPTY, (int)doc.GetInfo(303).vt);

    // sanity: the TRUE branch still answers VT_DATE once the time is non-zero
    doc.m_tConnectTime = CTime::GetCurrentTime();
    EXPECT_EQ((int)VT_DATE, (int)doc.GetInfo(301).vt);
}

// GetStyleInfo reads a style run OTHER than the first: the sequential search
// advances the column by each preceding run's length
// (methods_info.cpp:1373  iCol += pStyle->iLength), so run 2 reports its true
// start column (case 3 = iCol+1) and its text via Mid(iCol,len) (:1388).
// Build the line by hand: AddStyle drops the seed line's empty tail style and
// AddToLine grows the tail style one char per byte (doc_shared.cpp:2973), so the
// two runs and their lengths are deterministic.
TEST(GetInfoR5, StyleInfoSecondRunColumnAdvances)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.AddStyle(COLOUR_RGB, (COLORREF) 0x00FFFFFF, (COLORREF) 0x00000000,
                 0, (CAction *) NULL);
    doc.AddToLine("AAAA", COMMENT);          // run 1: 4 chars
    doc.AddStyle(COLOUR_RGB, (COLORREF) 0x0000FF00, (COLORREF) 0x00000000,
                 0, (CAction *) NULL);
    doc.AddToLine("BBB", COMMENT);           // run 2: 3 chars

    long ln = (long) doc.m_LineList.GetCount();   // current (last) line = the seed
    EXPECT_STREQ("AAAABBB", vstr(doc.GetLineInfo(ln, 1)).c_str());
    ASSERT_TRUE(doc.GetLineInfo(ln, 11).lVal >= 2L);   // two style runs

    // run 2 starts one past run 1's length (iCol accumulated run1.iLength == 4)
    EXPECT_EQ((short) 5, doc.GetStyleInfo(ln, 2, 3).iVal);   // 1-based start column
    EXPECT_EQ((short) 3, doc.GetStyleInfo(ln, 2, 2).iVal);   // run length
    EXPECT_STREQ("BBB", vstr(doc.GetStyleInfo(ln, 2, 1)).c_str());  // Mid(iCol,len)
    // run 1 (loop breaks before the accumulate) still reports column 1
    EXPECT_EQ((short) 1, doc.GetStyleInfo(ln, 1, 3).iVal);
}

// GetStyleInfo case 7 returns the run's CAction variable
// (methods_info.cpp:1426  pAction->m_strVariable).  No existing test builds a
// run whose action carries a variable; AddStyle's string overload feeds
// (action,hint,variable) through GetAction (mxp/mxputils.cpp:433), which returns
// a CAction populated with all three (non-null because they are non-empty).
TEST(GetInfoR5, StyleInfoRunActionVariable)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.AddStyle(COLOUR_RGB | ACTION_SEND, (COLORREF) 0x00FFFFFF,
                 (COLORREF) 0x00000000, 0,
                 CString("go north"), CString("move"), CString("myflag"));
    doc.AddToLine("link", COMMENT);

    long ln = (long) doc.m_LineList.GetCount();
    EXPECT_EQ((short) 1, doc.GetStyleInfo(ln, 1, 4).iVal);              // ACTION_SEND -> 1
    EXPECT_STREQ("go north", vstr(doc.GetStyleInfo(ln, 1, 5)).c_str()); // case 5 action
    EXPECT_STREQ("move",     vstr(doc.GetStyleInfo(ln, 1, 6)).c_str()); // case 6 hint
    EXPECT_STREQ("myflag",   vstr(doc.GetStyleInfo(ln, 1, 7)).c_str()); // case 7 variable
}

// methods_output.cpp SetScroll(Position, Visible): the port's pane always shows
// its scrollbar, so the Visible argument used to be discarded outright -- but
// m_bScrollBarWanted has no other writer besides its initialisation
// (doc_construct.cpp:136) and GetInfo(120) reports it, so a script that turned
// the scrollbar off still read back "true".  Windows records it
// (methods_output.cpp:771) before touching the control.
TEST(InfoTest, SetScrollRecordsTheScrollbarWantedFlag)
{
    CMUSHclientDoc doc; make_doc(doc);

    // doc_construct.cpp:136 starts it TRUE.
    EXPECT_TRUE(doc.GetInfo(120).boolVal != 0);

    // -2 is "do not move the view", so this isolates the visibility argument.
    doc.SetScroll(-2, FALSE);
    EXPECT_TRUE(doc.GetInfo(120).boolVal == 0);

    doc.SetScroll(-2, TRUE);
    EXPECT_TRUE(doc.GetInfo(120).boolVal != 0);

    // ...and it is recorded on a scrolling call too, not only the -2 one.
    doc.SetScroll(-1, FALSE);
    EXPECT_TRUE(doc.GetInfo(120).boolVal == 0);
}

// FormatTime translates the MSVC-only %# flag (no leading zero; POSIX %-).
// The bare `#define _tcsftime strftime` fed it through raw, so the Activity
// window's Since column showed "#I:58 PM" and any Windows-authored log
// preamble/postamble/auto-log filename containing %# was mangled the same
// way.  Now mc_tcsftime rewrites it — the same loop CTime::Format uses.
TEST(InfoTest, FormatTimeTranslatesTheWindowsNoLeadingZeroFlag)
{
    CMUSHclientDoc doc;
    const CTime t(2026, 7, 16, 5, 4, 0);
    EXPECT_STREQ("5:04", (LPCTSTR) doc.FormatTime(t, "%#I:%M"));
    // The control: the plain form keeps its leading zero.
    EXPECT_STREQ("05:04", (LPCTSTR) doc.FormatTime(t, "%I:%M"));
}

// headless_app.cpp's App ctor comment claims its printer margin defaults are
// "the Windows table defaults" (globalregistryoptions.cpp: PrinterLeftMargin /
// PrinterTopMargin both 15) -- it used to seed them to 1 instead.  Checked on
// a throwaway `scratch` object, not the live App: an EARLIER TEST() in this
// binary (GlobalOptions.HeadlessDatabaseRoundTripRestoresDefaults) already
// calls App.LoadGlobalsFromDatabase(), which independently resets every App
// global -- including these two -- to the same table default, so reading
// them off the live App here would pass regardless of what the ctor itself
// seeds.  See the comment above FreshDatabaseSeedAndReadShareDirectoryDefaults
// for the g_pWinApp save/restore this pattern requires.
TEST(AppDefaults, PrinterMarginsMatchTheGlobalOptionsTable)
{
    CWinApp* savedWinApp = g_pWinApp;
    CMUSHclientApp scratch;
    g_pWinApp = savedWinApp;

    EXPECT_EQ(15, scratch.m_nPrinterLeftMargin);
    EXPECT_EQ(15, scratch.m_nPrinterTopMargin);
}

// GetUniqueString is supposed to share ONE uniqueness sequence with
// GetUniqueNumber()/m_nUniqueNumber (world.GetUniqueNumber, trigger/alias/timer
// nUpdateNumber stamping, ...), same as MUSHclient.cpp:1608-1613 -- it used to
// keep a private static counter instead, so calling it never advanced
// m_nUniqueNumber and its own output never matched what GetUniqueNumber()
// would return.  Three consecutive calls must land on n, n+1, n+2.  Uses a
// throwaway `scratch` object (see the test above) so the sequence starts
// from the ctor's own m_nUniqueNumber = 0 instead of whatever the live App's
// counter happens to be after earlier tests.
TEST(AppDefaults, GetUniqueStringSharesTheGetUniqueNumberSequence)
{
    CWinApp* savedWinApp = g_pWinApp;
    CMUSHclientApp scratch;
    g_pWinApp = savedWinApp;

    __int64 n = scratch.GetUniqueNumber();
    CString s = scratch.GetUniqueString();
    __int64 n2 = scratch.GetUniqueNumber();

    char expected[32];
    snprintf(expected, sizeof expected, "%lld", (long long) (n + 1));
    EXPECT_STREQ(expected, (LPCTSTR) s);
    EXPECT_EQ(n + 2, n2);
}

MC_TEST_MAIN()
