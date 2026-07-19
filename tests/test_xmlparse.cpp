// tests/test_xmlparse.cpp
// Behaviour tests for xml/xmlparse.cpp — the hand-written XML parser every
// world/plugin/state file goes through.  Codifies the tree it builds for
// well-formed input (elements, attributes, content, CDATA, comments, the
// <?xml?> declaration, DOCTYPE + internal DTD entities) and the exact error
// message each class of malformed input raises (CErrorException via
// ThrowErrorException).

#include "mc_test.h"
#include "platform/doc_stub.h"
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

MC_TEST_MAIN()
