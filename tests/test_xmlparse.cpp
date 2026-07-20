// tests/test_xmlparse.cpp
// Behaviour tests for xml/xmlparse.cpp — the hand-written XML parser every
// world/plugin/state file goes through.  Codifies the tree it builds for
// well-formed input (elements, attributes, content, CDATA, comments, the
// <?xml?> declaration, DOCTYPE + internal DTD entities) and the exact error
// message each class of malformed input raises (CErrorException via
// ThrowErrorException).

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "xml/xmlparse.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

// ── helpers ───────────────────────────────────────────────────────────────

struct ParseOutcome {
    bool ok = false;
    std::string error;      // first exception's message when !ok
};

// Write the XML to a temp file and run the parser over it.
static ParseOutcome parse_xml(CXMLparser& parser, const std::string& xml,
                              bool binary_exact = false)
{
    static int s_seq = 0;
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_xml_%d_%d.xml",
             (int)getpid(), ++s_seq);
    FILE* f = fopen(path, "wb");
    if (!f) return ParseOutcome{false, "temp file"};
    fwrite(xml.data(), 1, xml.size(), f);
    fclose(f);
    (void)binary_exact;

    ParseOutcome out;
    CFile file(path, CFile::modeRead);
    try {
        parser.BuildStructure(&file);
        out.ok = true;
    }
    catch (CException* e) {
        char buf[512] = "";
        e->GetErrorMessage(buf, sizeof buf);
        out.error = buf;
        e->Delete();
    }
    return out;
}

static CXMLelement* find_child(CXMLelement& node, const char* name)
{
    for (POSITION pos = node.ChildrenList.GetHeadPosition(); pos; ) {
        CXMLelement* c = node.ChildrenList.GetNext(pos);
        if (c->strName == name) return c;
    }
    return nullptr;
}

static std::string attr(CXMLelement& node, const char* name)
{
    CAttribute* a = nullptr;
    if (!node.AttributeMap.Lookup(name, a) || !a) return "<missing>";
    return std::string((const char*)a->strValue);
}

// ── well-formed documents ─────────────────────────────────────────────────

// A complete document builds the expected tree: the <?xml?> declaration
// appears as an empty "?xml" node, DOCTYPE sets m_strDocumentName, elements
// carry attributes/content/children, and <br/> is flagged empty.
TEST(XmlParse, WellFormedDocumentTree)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p,
        "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
        "<!DOCTYPE muclient>\n"
        "<muclient>\n"
        "  <world name=\"Example\" port=\"4000\">the content</world>\n"
        "  <br/>\n"
        "</muclient>\n");
    ASSERT_TRUE(r.ok);

    EXPECT_STREQ("muclient", (const char*)p.m_strDocumentName);

    CXMLelement* decl = find_child(p.m_xmlRoot, "?xml");
    ASSERT_NE(nullptr, decl);
    EXPECT_TRUE(decl->bEmpty);
    EXPECT_STREQ("1.0", attr(*decl, "version").c_str());

    CXMLelement* mu = find_child(p.m_xmlRoot, "muclient");
    ASSERT_NE(nullptr, mu);
    CXMLelement* world = find_child(*mu, "world");
    ASSERT_NE(nullptr, world);
    EXPECT_STREQ("Example", attr(*world, "name").c_str());
    EXPECT_STREQ("4000",    attr(*world, "port").c_str());
    EXPECT_STREQ("the content", (const char*)world->strContent);
    EXPECT_FALSE(world->bEmpty);
    EXPECT_EQ(4U, world->iLine);            // 1-based line tracking

    CXMLelement* br = find_child(*mu, "br");
    ASSERT_NE(nullptr, br);
    EXPECT_TRUE(br->bEmpty);
}

// Single-quoted attribute values are as legal as double-quoted ones.
TEST(XmlParse, SingleQuotedAttributes)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<a x='one two'></a>");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("one two", attr(*a, "x").c_str());
}

// Comments vanish: they produce no nodes and do not disturb content.
TEST(XmlParse, CommentsAreSkippedEntirely)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p,
        "<!-- leading comment -->\n"
        "<a><!-- inner --><b/></a>\n"
        "<!-- trailing -->");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    EXPECT_NE(nullptr, find_child(*a, "b"));
    EXPECT_EQ(1, (int)a->ChildrenList.GetCount());   // only <b/>
}

// CDATA sections deliver their bytes verbatim — markup characters included.
TEST(XmlParse, CdataPreservesMarkupCharacters)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<a><![CDATA[if (x < 2 && y > 1) "
                                  "print \"<b>\"]]></a>");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    CString c = a->strContent;
    EXPECT_NE(-1, c.Find("x < 2 && y > 1"));
    EXPECT_NE(-1, c.Find("<b>"));
}

// The three entity families in content: a DTD-defined custom entity, a
// predefined named entity, and numeric (decimal + hex) references.
TEST(XmlParse, EntityReplacementInContent)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p,
        "<!DOCTYPE muclient [ <!ENTITY who \"Nick\"> ]>"
        "<a>&who; &amp; &#65;&#x41;</a>");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("Nick & AA", (const char*)a->strContent);
}

// Entities are replaced inside attribute values too.
TEST(XmlParse, EntityReplacementInAttributes)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<a x=\"1 &lt; 2\"></a>");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("1 < 2", attr(*a, "x").c_str());
}

// A UTF-8 byte-order mark is recognised and skipped.
TEST(XmlParse, Utf8BomIsSkipped)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, std::string("\xEF\xBB\xBF") + "<a>x</a>");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("x", (const char*)a->strContent);
}

// Tabs in the document are normalised to spaces before parsing.
TEST(XmlParse, TabsBecomeSpaces)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<a>x\ty</a>");
    ASSERT_TRUE(r.ok);
    CXMLelement* a = find_child(p.m_xmlRoot, "a");
    ASSERT_NE(nullptr, a);
    EXPECT_STREQ("x y", (const char*)a->strContent);
}

// DOCTYPE may carry SYSTEM "dtd" or PUBLIC "id" "dtd" — accepted and
// ignored "for future compatibility".
TEST(XmlParse, DoctypeSystemAndPublicAccepted)
{
    CXMLparser p1;
    EXPECT_TRUE(parse_xml(p1,
        "<!DOCTYPE muclient SYSTEM \"my.dtd\"><a/>").ok);
    EXPECT_STREQ("muclient", (const char*)p1.m_strDocumentName);

    CXMLparser p2;
    EXPECT_TRUE(parse_xml(p2,
        "<!DOCTYPE muclient PUBLIC \"pubid\" \"my.dtd\"><a/>").ok);
    EXPECT_STREQ("muclient", (const char*)p2.m_strDocumentName);
}

// ── malformed documents: each class raises its own diagnostic ─────────────

static void expect_error(const char* xml, const char* fragment)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, xml);
    if (r.ok) {
        char msg[256];
        snprintf(msg, sizeof msg, "expected failure for: %s", xml);
        mc_test::fail_msg(__FILE__, __LINE__, msg);
        return;
    }
    if (r.error.find(fragment) == std::string::npos) {
        char msg[512];
        snprintf(msg, sizeof msg, "for %s wanted error containing \"%s\", got \"%s\"",
                 xml, fragment, r.error.c_str());
        mc_test::fail_msg(__FILE__, __LINE__, msg);
    }
}

TEST(XmlParse, ZeroLengthFileRejected)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("Zero length XML file"));
}

TEST(XmlParse, StructuralErrorsDiagnosed)
{
    // elements closed in the wrong order
    expect_error("<a><b></a>", "terminated out of sequence");
    // closing tag with no opening tag
    expect_error("</a>", "no corresponding opening tag");
    // unclosed element at end of file
    expect_error("<a>", "Unexpected end-of-file while looking for </a>");
    // stray characters after a closing tag's name
    expect_error("<a></a junk>", "Surplus characters after '</a'");
    // bare content at document root
    expect_error("stray", "content not permitted here");
    // CDATA terminator loose in content
    expect_error("<a>x]]>y</a>", "CDATA terminator ']]>' invalid");
    // unterminated CDATA section
    expect_error("<a><![CDATA[x", "Unexpected end-of-file inside [CDATA[");
    // CDATA at root level
    expect_error("<![CDATA[x]]>", "<![CDATA[ invalid at root level");
}

TEST(XmlParse, AttributeErrorsDiagnosed)
{
    expect_error("<a x=\"1\" x=\"2\"></a>", "Duplicate attribute name");
    expect_error("<a x></a>",               "not followed by '='");
    expect_error("<a x=1></a>",             "quote");     // unquoted value
    expect_error("<a x=\"1",                "not terminated (no closing quote)");
    expect_error("<a x=\"1\"",              "Unexpected end-of-file inside element definition");
}

TEST(XmlParse, DeclarationErrorsDiagnosed)
{
    expect_error("<!BOGUS thing><a/>",  "Unrecognised declaration");
    expect_error("<!DOCTYPE muclient [ <!ELEMENT foo> ]><a/>",
                 "Unsupported declaration <!ELEMENT");
    expect_error("<!DOCTYPE muclient [ <!ENTITY xmlish \"x\"> ]><a/>",
                 "reserved");
    expect_error("<!DOCTYPE muclient [ junk ]><a/>", "Expected '<!'");
    expect_error("<!DOCTYPE muclient [ <!ENTITY e \"x\"> ",
                 "Unexpected end-of-file inside DTD");
    expect_error("<!DOCTYPE muclient FUNKY \"x\"><a/>",
                 "Unsupported declaration \"FUNKY\" inside <!DOCTYPE");
}

TEST(XmlParse, EntityErrorsDiagnosed)
{
    expect_error("<a>&bogus;</a>",  "Unknown XML entity");
    expect_error("<a>&#7;</a>",     "Disallowed number in XML entity");
    expect_error("<a>&#999;</a>",   "Disallowed number in XML entity");
    expect_error("<a>&#12z;</a>",   "Invalid number in XML entity");
    expect_error("<a>&#xZZ;</a>",   "Invalid hex number in XML entity");
    expect_error("<a>&#x1FF;</a>",  "maximum of 2 hex digits");
}

TEST(XmlParse, NonPrintableCharactersRejected)
{
    expect_error("<a>x\x01y</a>", "Non-printable character");
    expect_error("<a><![CDATA[x\x02y]]></a>", "Non-printable character");
}

// ── Get_XML_Entity direct edges ───────────────────────────────────────────

// The whitespace control characters are the ONLY sub-space codes allowed:
// tab, newline and carriage-return translate; everything else throws
// (covered above), and 8-bit codes up to 255 come back as one raw byte.
TEST(XmlParse, NumericEntityWhitespaceAndHighBytes)
{
    CXMLparser p;
    CString tab("#9"), nl("#10"), cr("#13"), space("#32"), high("#255");
    EXPECT_STREQ("\t", (const char*)p.Get_XML_Entity(tab));
    EXPECT_STREQ("\n", (const char*)p.Get_XML_Entity(nl));
    EXPECT_STREQ("\r", (const char*)p.Get_XML_Entity(cr));
    EXPECT_STREQ(" ",  (const char*)p.Get_XML_Entity(space));
    CString v = p.Get_XML_Entity(high);
    EXPECT_EQ(1, v.GetLength());
    EXPECT_EQ((unsigned char)255, (unsigned char)v[0]);
}

// Custom entities defined via the parser's own map take effect only after
// the built-in (global) table is consulted first.
TEST(XmlParse, CustomEntityMapConsultedAfterGlobal)
{
    CXMLparser p;
    p.m_CustomEntityMap.SetAt("mine", "custom-value");
    CString mine("mine");
    EXPECT_STREQ("custom-value", (const char*)p.Get_XML_Entity(mine));

    // a custom entity cannot shadow a global one ("amp" resolves globally)
    p.m_CustomEntityMap.SetAt("amp", "SHADOWED");
    CString amp("amp");
    EXPECT_STREQ("&", (const char*)p.Get_XML_Entity(amp));
}

// ═══ Round-4 coverage: more malformed-input arms ═══════════════════════════

TEST(XmlParseR4, DoubleHyphenInsideComment)
{
    expect_error("<!DOCTYPE muclient><muclient><!-- bad -- comment --></muclient>",
                 "Invalid '--' inside comment");
}

TEST(XmlParseR4, EofAndCommentPlacementArms)
{
    // EOF while looking for an attribute value
    expect_error("<!DOCTYPE muclient><muclient><x attr=",
                 "Unexpected end-of-file");
    // comment where a value was expected
    expect_error("<!DOCTYPE muclient><muclient><x attr=<!-- c -->/></muclient>",
                 "Comment not valid here");
    // EOF after a closing-tag name
    expect_error("<!DOCTYPE muclient><muclient><x>text</x",
                 "Unexpected end-of-file");
}

TEST(XmlParseR4, DoctypeDeclarationArms)
{
    // EOF inside DOCTYPE
    expect_error("<!DOCTYPE muclient [", "end-of-file");
    // unsupported declaration type inside DOCTYPE
    expect_error("<!DOCTYPE muclient [ <!WEIRD foo> ]><muclient/>",
                 "Unsupported declaration");
    // missing '>' after entity definition
    expect_error("<!DOCTYPE muclient [ <!ENTITY a \"b\" extra> ]><muclient/>",
                 "Expected '>'");
}

TEST(XmlParseR4, EntityMissingSemicolonInContent)
{
    expect_error("<!DOCTYPE muclient><muclient><x>text &unterminated</x></muclient>",
                 "No closing \";\"");
}

// UTF-16LE input (FF FE BOM) goes through the wide-char conversion arm.
TEST(XmlParseR4, Utf16LittleEndianInputConverted)
{
    std::string doc8 = "<!DOCTYPE muclient><muclient><w name=\"A\"/></muclient>";
    std::string doc16 = "\xFF\xFE";
    for (char c : doc8) { doc16 += c; doc16 += '\0'; }

    CXMLparser p;
    ParseOutcome r = parse_xml(p, doc16);
    ASSERT_TRUE(r.ok);
    EXPECT_STREQ("muclient", (const char*)p.m_strDocumentName);
    CXMLelement* mu = find_child(p.m_xmlRoot, "muclient");
    ASSERT_NE(nullptr, mu);
    CXMLelement* w = find_child(*mu, "w");
    ASSERT_NE(nullptr, w);
    EXPECT_STREQ("A", attr(*w, "name").c_str());
}

// ── round 5: the typed attribute getters (Get_XML_*) ──────────────────────
// These free functions (xmlparse.h) are the loader's whole attribute type
// system; every error arm below is an exact product message.

// Parse a one-element document and hand back the element the getters read.
static CXMLelement* one_element(CXMLparser& p, const std::string& attrs)
{
    ParseOutcome r = parse_xml(p, "<a " + attrs + "/>");
    if (!r.ok) return nullptr;
    return find_child(p.m_xmlRoot, "a");
}

// Run fn and capture the CException message it throws ("" = no throw).
template <typename F>
static std::string thrown(F fn)
{
    try { fn(); }
    catch (CException* e) {
        char buf[512] = "";
        e->GetErrorMessage(buf, sizeof buf);
        e->Delete();
        return buf;
    }
    return "";
}

TEST(XmlAttrR5, NumberSignsAndDigits)
{
    CXMLparser p;
    CXMLelement* a = one_element(p,
        "neg=\"-5\" pos=\"+5\" bare=\"-\" alpha=\"x\" mid=\"1x\" "
        "huge=\"99999999999999999999\"");
    ASSERT_NE(nullptr, a);
    long v = 0;

    EXPECT_TRUE(Get_XML_number(*a, "neg", v, false, -10, 10));
    EXPECT_EQ(-5, v);                                    // '-' arm + negate
    EXPECT_TRUE(Get_XML_number(*a, "pos", v, false, -10, 10));
    EXPECT_EQ(5, v);                                     // '+' arm

    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_number(*a, "bare", v, false); })
        .find("No number suppied"));                     // (sic) sign then EOF
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_number(*a, "alpha", v, false); })
        .find("Invalid number"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_number(*a, "mid", v, false); })
        .find("Invalid character"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_number(*a, "huge", v, false); })
        .find("too large"));
}

TEST(XmlAttrR5, NumberRangeChecks)
{
    CXMLparser p;
    CXMLelement* a = one_element(p, "twelve=\"12\" neg=\"-5\"");
    ASSERT_NE(nullptr, a);
    long v = 0;

    // the (0, 0) default range is fixed up to (0, 1) before the check
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_number(*a, "twelve", v, false, 0, 0); })
        .find("too large"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_number(*a, "neg", v, false, 0, 100); })
        .find("too small"));
}

TEST(XmlAttrR5, ShortAndIntConversions)
{
    CXMLparser p;
    CXMLelement* a = one_element(p, "sh=\"7\"");
    ASSERT_NE(nullptr, a);

    short s = 0;
    EXPECT_TRUE(Get_XML_short(*a, "sh", s, false));
    EXPECT_EQ(7, s);

    int i = 99;
    EXPECT_FALSE(Get_XML_int(*a, "absent", i, true));    // default kept
    EXPECT_EQ(99, i);
}

TEST(XmlAttrR5, DoubleParseArms)
{
    CXMLparser p;
    CXMLelement* a = one_element(p,
        "ok=\"-1.5\" bare=\"-\" alpha=\"x\" dots=\"1.2.3\" "
        "twoe=\"1E5E5\" expsign=\"1E-5\" exp=\"2E2\"");
    ASSERT_NE(nullptr, a);
    double d = 0;

    EXPECT_TRUE(Get_XML_double(*a, "ok", d, false));
    EXPECT_EQ(-1.5, d);
    EXPECT_TRUE(Get_XML_double(*a, "exp", d, false));
    EXPECT_EQ(200.0, d);

    EXPECT_FALSE(Get_XML_double(*a, "absent", d, true)); // default kept

    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_double(*a, "bare", d, false); })
        .find("No number suppied"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_double(*a, "alpha", d, false); })
        .find("Invalid number"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_double(*a, "dots", d, false); })
        .find("Too many decimal places"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_double(*a, "twoe", d, false); })
        .find("Too many 'E' characters"));
    // Signed exponents now parse (upstream bug fixed: the exponent-sign check
    // inspected the 'E' itself, wrongly rejecting "1E-5" — xml/xmlparse.cpp).
    EXPECT_TRUE(Get_XML_double(*a, "expsign", d, false));
    EXPECT_EQ(1e-5, d);

    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_double(*a, "ok", d, false, 0.0, 100.0); })
        .find("too small"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_double(*a, "exp", d, false, 0.0, 100.0); })
        .find("too large"));
}

TEST(XmlAttrR5, ColourArms)
{
    CXMLparser p;
    CXMLelement* a = one_element(p, "empty=\"\" bad=\"bogusname\" good=\"#010203\"");
    ASSERT_NE(nullptr, a);
    COLORREF c = 0;

    EXPECT_TRUE(Get_XML_colour(*a, "good", c, false));
    EXPECT_EQ((COLORREF)RGB(1, 2, 3), c);

    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_colour(*a, "empty", c, false); })
        .find("No colour supplied"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_colour(*a, "bad", c, false); })
        .find("Invalid colour code"));
}

TEST(XmlAttrR5, IsNumberEdges)
{
    EXPECT_FALSE(IsNumber(CString(""), true));            // empty
    // LP64: long == __int64, and _atoi64 (strtoll) clamps on overflow, so
    // the "> LONG_MAX" reject arm can never fire here (it is Win32-only).
    EXPECT_TRUE(IsNumber(CString("99999999999999999999"), false));
    EXPECT_TRUE(IsNumber(CString("-3"), true));
    EXPECT_FALSE(IsNumber(CString("3x"), false));
}

TEST(XmlAttrR5, DateParseArms)
{
    CXMLparser p;
    CXMLelement* a = one_element(p,
        "ok=\"2020-05-06 07:08:09\" dateonly=\"2020-05-06\" "
        "parts=\"a b c\" short=\"2020-05\" year=\"x-05-06\" "
        "badyear=\"1200-05-06\" badmonth=\"2020-13-06\" badday=\"2020-05-40\" "
        "manytime=\"2020-05-06 1:2:3:4\" badhour=\"2020-05-06 x:02\" "
        "hour25=\"2020-05-06 25:00:00\" badmin=\"2020-05-06 01:99:00\" "
        "badsec=\"2020-05-06 01:02:99\" minonly=\"2020-05-06 01\"");
    ASSERT_NE(nullptr, a);
    CTime t(0);

    EXPECT_TRUE(Get_XML_date(*a, "ok", t, false));
    EXPECT_EQ(2020, t.GetYear());
    EXPECT_EQ(5,    t.GetMonth());
    EXPECT_EQ(6,    t.GetDay());
    EXPECT_EQ(7,    t.GetHour());

    EXPECT_TRUE(Get_XML_date(*a, "dateonly", t, false)); // no-time arm
    EXPECT_EQ(0, t.GetHour());
    EXPECT_TRUE(Get_XML_date(*a, "minonly", t, false));  // hour, no minute
    EXPECT_EQ(1, t.GetHour());

    // absent attribute: bUseDefault leaves tValue alone, else zeroes it
    EXPECT_FALSE(Get_XML_date(*a, "absent", t, true));
    EXPECT_FALSE(Get_XML_date(*a, "absent", t, false));
    EXPECT_EQ(CTime(0).GetTime(), t.GetTime());

    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "parts", t, false); })
        .find("Date/time must consist of"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "short", t, false); })
        .find("Date must consist of"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "year", t, false); })
        .find("Year is not numeric"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "badyear", t, false); })
        .find("Year"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "badmonth", t, false); })
        .find("Month"));
    // Day range check message corrected (upstream copy-paste typo said "Month").
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "badday", t, false); })
        .find("Day must be in range 1 to 31"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "manytime", t, false); })
        .find("Time must consist of"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "badhour", t, false); })
        .find("Hour"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "hour25", t, false); })
        .find("Hour"));
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "badmin", t, false); })
        .find("Minute"));
    // Second range check message corrected (upstream copy-paste typo said "Minute").
    EXPECT_NE(std::string::npos, thrown([&]{ Get_XML_date(*a, "badsec", t, false); })
        .find("Second must be in range 0 to 59"));
}

TEST(XmlAttrR5, BooleanMaskArms)
{
    CXMLparser p;
    CXMLelement* a = one_element(p, "on=\"y\" off=\"n\"");
    ASSERT_NE(nullptr, a);

    unsigned short us = 0;
    EXPECT_TRUE(Get_XML_boolean_ushort_mask(*a, "on", us, 0x4, false));
    EXPECT_EQ(0x4, us);                                  // set-bit arm
    EXPECT_TRUE(Get_XML_boolean_ushort_mask(*a, "off", us, 0x4, false));
    EXPECT_EQ(0x0, us);

    int i = 0;
    EXPECT_FALSE(Get_XML_boolean_int_mask(*a, "absent", i, 0x2, true));
    EXPECT_EQ(0, i);
}

// ── round 5: parser structural error arms ─────────────────────────────────

// GetName's leading-character check rejects every non-name first character
// up front, which makes its later "name not supplied" / "Comment not valid
// here" arms logically dead — these pin the check that shadows them.
TEST(XmlParseR5, EmptyElementNameRejectedByLeadCharCheck)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<>");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("must start with letter or underscore"));
}

TEST(XmlParseR5, NameStartingWithBadCharacter)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<%bad/>");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("must start with letter or underscore"));
}

TEST(XmlParseR5, DoctypeUnsupportedDeclaration)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<!DOCTYPE muclient junk>\n<a/>");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("Unsupported declaration"));
}

TEST(XmlParseR5, DoctypeEofBeforeClose)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<!DOCTYPE muclient");
    EXPECT_FALSE(r.ok);
    // either the dedicated EOF message or the general truncation error
    EXPECT_FALSE(r.error.empty());
}

// NOTE: the arm xmlparse.cpp:653 ("Unexpected '<![CDATA[' inside element
// definition") is unreachable defensive code — ProcessAttributes → GetName
// rejects an unquoted '<' *before* AssembleContent's second pass runs, and
// CDATA markers inside quoted values / at content level are handled without it.
// So a CDATA marker sitting where an attribute name is expected is caught by
// GetName's leading-character check (the "Attribute" variant); pin THAT, with
// the exact message, rather than only asserting "some error".
TEST(XmlParseR5, CdataMarkerWhereAttributeNameExpectedRejected)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<a <![CDATA[x]]> b=\"c\"/>");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("Attribute name must start with"));
    EXPECT_NE(std::string::npos, r.error.find("\"<\""));   // offending char is '<'
}

TEST(XmlParseR5, EmptyTagCloseAtTopLevelClampsDepth)
{
    // "<?x/>" is an empty PI-form child; the pi[1]=='?' exclusion in
    // AssembleContent skips its iDepth++, so the trailing "/>" drives iDepth to
    // -1 and the clamp at xmlparse.cpp:715 resets it to 0.  With the clamp the
    // following "hello" content is retained; without the iDepth--/clamp iDepth
    // stays -1 and the content-copy guard drops it (strContent comes out empty).
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<r><?x/>hello</r>");
    ASSERT_TRUE(r.ok);
    CXMLelement* e = find_child(p.m_xmlRoot, "r");
    ASSERT_NE(nullptr, e);
    EXPECT_STREQ("hello", (const char*)e->strContent);
}

TEST(XmlParseR5, NewlinesInRootWhitespaceAreCounted)
{
    CXMLparser p;
    ParseOutcome r = parse_xml(p, "<a x=\"1\"></a>\n \n<b y=\"1\"></b>");
    ASSERT_TRUE(r.ok);
    CXMLelement* b = find_child(p.m_xmlRoot, "b");
    ASSERT_NE(nullptr, b);
    EXPECT_EQ(3U, b->iLine);   // the blank lines were counted
}

TEST(XmlParseR5, OversizeFileRejectedUpFront)
{
    // A sparse file > MAX_XML_DOCUMENT_SIZE (100 MB): the length check throws
    // before any read, so this costs no real I/O.
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_xml_huge_%d.xml", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_NE(nullptr, f);
    fseek(f, MAX_XML_DOCUMENT_SIZE + 1, SEEK_SET);
    fputc('x', f);
    fclose(f);

    CXMLparser p;
    std::string err;
    CFile file(path, CFile::modeRead);
    try { p.BuildStructure(&file); }
    catch (CException* e) {
        char buf[512] = "";
        e->GetErrorMessage(buf, sizeof buf);
        err = buf;
        e->Delete();
    }
    unlink(path);
    EXPECT_NE(std::string::npos, err.find("too large"));
}

MC_TEST_MAIN()
