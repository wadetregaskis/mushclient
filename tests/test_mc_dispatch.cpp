// tests/test_mc_dispatch.cpp
// The portable IDispatch::Invoke substrate (platform/mc_dispatch.cpp).
//
// This is the single road every non-Lua script takes to the world API.  Python,
// Perl, JavaScript, Tcl and Ruby all convert their own values to VARIANT at the
// edge and hand them to MC_DispatchInvoke; none of them contains a hand-written
// binding for even one of the ~400 methods.  So a defect here is a defect in
// five languages at once -- and it had no test at all, because the engines that
// exercise it cannot be linked into a headless test binary.
//
// The substrate itself CAN be: mc_dispatch.cpp is a mushcore TU and needs no
// interpreter.  That is what this file tests.
//
// The completeness check re-includes doc_dispatch_map.inc -- the same file the
// Windows build compiles into its COM dispatch map -- with name-collecting
// macros, so "every method Windows exposes is reachable here" is proved against
// the map itself rather than a hand-copied list that would rot.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/mc_dispatch.h"
#include "scripting/errors.h"
#include "fake_worldview.h"    // the window-family defaults need a view to land on

// VT_DISPATCH is not in the shim's VT_* set (mc_dispatch.cpp defines it locally
// for the same reason): it only exists at this boundary, where a world method
// hands back another world object.
#ifndef VT_DISPATCH
#define VT_DISPATCH 9
#endif

#include <set>
#include <string>
#include <vector>

// ── helpers ───────────────────────────────────────────────────────────────

namespace {

std::string lower(const std::string& s)
{
    std::string r = s;
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return r;
}

// A VARIANT of each input flavour a script can produce.
VARIANT vstr(const char* s)  { VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(s); return v; }
VARIANT vi4(long n)          { VARIANT v; VariantInit(&v); v.vt = VT_I4;   v.lVal    = n; return v; }
VARIANT vi2(short n)         { VARIANT v; VariantInit(&v); v.vt = VT_I2;   v.iVal    = n; return v; }
VARIANT vr8(double d)        { VARIANT v; VariantInit(&v); v.vt = VT_R8;   v.dblVal  = d; return v; }
VARIANT vbool(bool b)        { VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = b ? 1 : 0; return v; }

// Call a method and hand back the result; the caller clears it.
int call(CMUSHclientDoc& doc, const char* name, std::vector<VARIANT> args, VARIANT& out,
         std::vector<VARIANT>* outargs = nullptr)
{
    const int rc = MC_DispatchInvoke(&doc, name, args.empty() ? nullptr : args.data(),
                                     (int) args.size(), out, outargs);
    for (VARIANT& a : args) MC_VariantClear(a);
    return rc;
}

// The string a BSTR result carries (empty for anything else).
std::string bstr_of(const VARIANT& v)
{
    return (v.vt == VT_BSTR && v.bstrVal) ? std::string(v.bstrVal) : std::string();
}

// Set a variable through dispatch, read it back through dispatch, return the
// string the engine stored.  This is the shortest path that exercises argument
// coercion end to end: SetVariable takes two BSTRs, so whatever VARIANT the
// script passed had to survive var_to_cstr.
std::string roundtrip_variable(CMUSHclientDoc& doc, const char* name, VARIANT value)
{
    VARIANT r; VariantInit(&r);
    call(doc, "SetVariable", { vstr(name), value }, r);
    MC_VariantClear(r);

    VARIANT got; VariantInit(&got);
    call(doc, "GetVariable", { vstr(name) }, got);
    const std::string s = bstr_of(got);
    MC_VariantClear(got);
    return s;
}

// ── the dispatch map, collected from the same .inc the shipping code uses ──
// Mirrors mc_dispatch.cpp's build_table(): DISP_FUNCTION rows are methods,
// DISP_PROPERTY* rows are properties, and DISP_PROPERTY_PARAM contributes its
// accessor pair as METHODS (indexed properties have no plain-attribute form).
struct MapNames {
    std::set<std::string> methods;
    std::set<std::string> properties;
};

const MapNames& map_names()
{
    static MapNames m = [] {
        MapNames n;
        #define DISP_FUNCTION(cls, name, fn, ret, ...)         n.methods.insert(lower(name));
        #define DISP_FUNCTION_ID(cls, name, id, fn, ret, ...)  n.methods.insert(lower(name));
        #define DISP_PROPERTY(cls, name, mem, type)            n.properties.insert(lower(name));
        #define DISP_PROPERTY_NOTIFY(cls, name, mem, fn, type) n.properties.insert(lower(name));
        #define DISP_PROPERTY_EX(cls, name, get, set, type)    n.properties.insert(lower(name));
        #define DISP_PROPERTY_PARAM(cls, name, get, set, type, ...) \
                    n.methods.insert(lower(#get)); n.methods.insert(lower(#set));
        #define DISP_DEFVALUE(cls, name)
        #include "doc_dispatch_map.inc"
        #undef DISP_FUNCTION
        #undef DISP_FUNCTION_ID
        #undef DISP_PROPERTY
        #undef DISP_PROPERTY_NOTIFY
        #undef DISP_PROPERTY_EX
        #undef DISP_PROPERTY_PARAM
        #undef DISP_DEFVALUE
        return n;
    }();
    return m;
}

}   // namespace

// ── completeness ──────────────────────────────────────────────────────────

// Every method name in the shared dispatch map resolves.  If a row is ever
// added to doc_dispatch_map.inc that the thunk generator cannot handle, or a
// registration is dropped, five languages lose that method silently -- there is
// no compile error on the script side, only "no such method" at run time.
TEST(McDispatch, EveryMethodInTheSharedMapIsReachable)
{
    const MapNames& n = map_names();
    ASSERT_TRUE(n.methods.size() > 350);      // the map has ~400 rows; sanity

    std::string missing;
    int nmissing = 0;
    for (const std::string& name : n.methods)
        if (!MC_DispatchHas(name.c_str())) {
            if (nmissing < 12) missing += " " + name;
            ++nmissing;
        }
    EXPECT_EQ(0, nmissing);
    if (nmissing) printf("  unreachable methods:%s%s\n", missing.c_str(),
                         nmissing > 12 ? " ..." : "");
}

TEST(McDispatch, EveryPropertyInTheSharedMapIsReachable)
{
    const MapNames& n = map_names();
    ASSERT_TRUE(n.properties.size() > 5);

    std::string missing;
    int nmissing = 0;
    for (const std::string& name : n.properties)
        if (!MC_DispatchHasProperty(name.c_str())) {
            if (nmissing < 12) missing += " " + name;
            ++nmissing;
        }
    EXPECT_EQ(0, nmissing);
    if (nmissing) printf("  unreachable properties:%s\n", missing.c_str());
}

// Methods and properties are separate namespaces, as they are in COM: a script
// says world.Trace = true, not world.Trace().
TEST(McDispatch, MethodsAndPropertiesDoNotBleedIntoEachOther)
{
    EXPECT_TRUE(MC_DispatchHas("Note"));
    EXPECT_FALSE(MC_DispatchHasProperty("Note"));
    EXPECT_TRUE(MC_DispatchHasProperty("Trace"));
    EXPECT_FALSE(MC_DispatchHas("Trace"));
}

// Automation names are case-insensitive on Windows (IDispatch::GetIDsOfNames),
// and real plugins rely on it -- world.note and world.Note are the same call.
TEST(McDispatch, LookupIsCaseInsensitive)
{
    EXPECT_TRUE(MC_DispatchHas("note"));
    EXPECT_TRUE(MC_DispatchHas("NOTE"));
    EXPECT_TRUE(MC_DispatchHas("NoTe"));
    EXPECT_TRUE(MC_DispatchHasProperty("trace"));
    EXPECT_TRUE(MC_DispatchHasProperty("TRACE"));

    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    EXPECT_EQ(MC_DISP_OK, call(doc, "getuniquenumber", {}, r));
    EXPECT_EQ((int) VT_I4, (int) r.vt);
    MC_VariantClear(r);
}

TEST(McDispatch, UnknownNamesAreReportedNotGuessed)
{
    CMUSHclientDoc doc;
    EXPECT_FALSE(MC_DispatchHas("NoSuchMethodAtAll"));
    EXPECT_FALSE(MC_DispatchHasProperty("NoSuchPropertyAtAll"));

    VARIANT r; VariantInit(&r);
    EXPECT_EQ(MC_DISP_UNKNOWN, call(doc, "NoSuchMethodAtAll", { vstr("x") }, r));
    EXPECT_EQ((int) VT_EMPTY, (int) r.vt);       // nothing written on failure
    MC_VariantClear(r);

    VARIANT p; VariantInit(&p);
    EXPECT_FALSE(MC_DispatchGetProperty(&doc, "NoSuchPropertyAtAll", p));
    EXPECT_FALSE(MC_DispatchSetProperty(&doc, "NoSuchPropertyAtAll", vi4(1)));
}

// ── return-value marshalling ──────────────────────────────────────────────
//
// Every VARIANT type a script can be handed comes from to_variant().  Getting
// one wrong shows up in the script as the wrong type, not an error.

TEST(McDispatch, ReturnTypesMatchTheMapsDeclaredTypes)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "TestWorld";

    struct { const char* method; std::vector<VARIANT> args; int vt; } cases[] = {
        { "WorldName",       {},                              VT_BSTR  },  // VT_BSTR
        { "Version",         {},                              VT_BSTR  },
        { "GetUniqueNumber", {},                              VT_I4    },  // VT_I4
        { "SetVariable",     { vstr("a"), vstr("b") },        VT_I4    },
        { "Note",            { vstr("hello") },               VT_EMPTY },  // void
        { "GetVariable",     { vstr("a") },                   VT_BSTR  },  // VT_VARIANT carrying a string
    };

    for (auto& c : cases) {
        VARIANT r; VariantInit(&r);
        ASSERT_EQ(MC_DISP_OK, call(doc, c.method, c.args, r));
        EXPECT_EQ(c.vt, (int) r.vt);
        MC_VariantClear(r);
    }
}

// A method declared VT_I4 must answer with the engine's own error code, not a
// boolean or a string -- scripts compare against world.error constants.
TEST(McDispatch, ErrorCodesComeBackAsNumbers)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "SetVariable", { vstr(""), vstr("x") }, r));
    EXPECT_EQ((int) VT_I4, (int) r.vt);
    EXPECT_EQ((long) eInvalidObjectLabel, r.lVal);   // blank name is rejected
    MC_VariantClear(r);
}

// A method returning a SAFEARRAY hands the script a real array.  MFC's
// COleSafeArray detaches as VT_EMPTY when it was never created, and the engines
// rely on that to mean "no items".
TEST(McDispatch, ArrayReturnsAreEmptyBeforeAnyItemsExist)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariableList", {}, r));
    EXPECT_EQ((int) VT_EMPTY, (int) r.vt);
    MC_VariantClear(r);

    VARIANT set; VariantInit(&set);
    call(doc, "SetVariable", { vstr("one"), vstr("1") }, set);
    MC_VariantClear(set);
    call(doc, "SetVariable", { vstr("two"), vstr("2") }, set);
    MC_VariantClear(set);

    VARIANT list; VariantInit(&list);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariableList", {}, list));
    ASSERT_EQ((int) (VT_ARRAY | VT_VARIANT), (int) list.vt);
    ASSERT_TRUE(list.parray != nullptr);
    EXPECT_EQ(2L, (long) list.parray->rgsabound[0].cElements);
    MC_VariantClear(list);
}

// A method returning LPDISPATCH answers VT_DISPATCH.  A null one is how the
// engine says "no such world"; each language turns that into its own nothing.
TEST(McDispatch, MissingWorldIsANullDispatchNotAnError)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetWorld", { vstr("no such world at all") }, r));
    EXPECT_EQ((int) VT_DISPATCH, (int) r.vt);
    EXPECT_TRUE(r.byref == nullptr);
    MC_VariantClear(r);
}

// ── argument coercion ─────────────────────────────────────────────────────
//
// Scripts are loosely typed: a Lua number, a Python int, a Tcl string and a JS
// boolean can all land in the same BSTR parameter.  var_to_cstr is the one
// place that decides what the engine then stores.

TEST(McDispatch, EveryScalarVariantCoercesToTheDeclaredStringParameter)
{
    CMUSHclientDoc doc;
    EXPECT_EQ(std::string("plain"), roundtrip_variable(doc, "s", vstr("plain")));
    EXPECT_EQ(std::string("42"),    roundtrip_variable(doc, "i4", vi4(42)));
    EXPECT_EQ(std::string("-7"),    roundtrip_variable(doc, "i2", vi2(-7)));
    EXPECT_EQ(std::string("2.5"),   roundtrip_variable(doc, "r8", vr8(2.5)));
    EXPECT_EQ(std::string("1"),     roundtrip_variable(doc, "bt", vbool(true)));
    EXPECT_EQ(std::string("0"),     roundtrip_variable(doc, "bf", vbool(false)));
}

TEST(McDispatch, StringsCoerceToNumericParameters)
{
    CMUSHclientDoc doc;
    // AddAlias's fourth parameter is VTS_I4 (the flags word).  Passing it as a
    // string -- which Tcl does for everything -- must still enable the alias.
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "AddAlias",
        { vstr("al"), vstr("hi"), vstr("there"), vstr("1") /*eEnabled*/, vstr("") }, r));
    EXPECT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    VARIANT info; VariantInit(&info);
    // GetAliasInfo(name, 6) is "enabled" -- true only if the "1" reached the flags.
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetAliasInfo", { vstr("al"), vi2(6) }, info));
    EXPECT_EQ((int) VT_BOOL, (int) info.vt);
    EXPECT_TRUE(info.boolVal != 0);
    MC_VariantClear(info);
}

// Arguments the script did not supply arrive as VT_ERROR ("not supplied") and
// read as the zero value for their type — EXCEPT the ~38 methods whose Lua
// binding documents a non-zero default (EnableTrigger's flag, WindowShow's
// show, DatabaseOpen's sqlite flags...), which mc_dispatch's kArgDefaults
// table now fills with that default instead (see the tests below).  For
// everything else, zero-fill is the documented behaviour rather than an
// accident of reading past the end of the array.
TEST(McDispatch, MissingArgumentsAreTheZeroValueNotGarbage)
{
    CMUSHclientDoc doc;

    VARIANT r; VariantInit(&r);
    // SetVariable takes two BSTRs; supply only the name.
    ASSERT_EQ(MC_DISP_OK, call(doc, "SetVariable", { vstr("lonely") }, r));
    EXPECT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    VARIANT got; VariantInit(&got);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariable", { vstr("lonely") }, got));
    EXPECT_EQ(std::string(""), bstr_of(got));       // empty, not a stray pointer
    MC_VariantClear(got);

    // ...and with no arguments at all.
    VARIANT t; VariantInit(&t);
    ASSERT_EQ(MC_DISP_OK, call(doc, "Trim", {}, t));
    EXPECT_EQ((int) VT_BSTR, (int) t.vt);
    EXPECT_EQ(std::string(""), bstr_of(t));
    MC_VariantClear(t);
}

// ...and the substrate must not read past the end of the argument array to get
// there.  Hand it a buffer whose next slot holds a value that would be visible
// if it did: nargs is the contract, not the buffer length.
TEST(McDispatch, MissingArgumentsDoNotReadPastTheEndOfTheArray)
{
    CMUSHclientDoc doc;

    VARIANT buf[3];
    buf[0] = vstr("beyond");                       // the name
    buf[1] = vstr("POISON");                       // would become the value...
    buf[2] = vstr("POISON2");
    VARIANT r; VariantInit(&r);
    // ...but we tell it there is only ONE argument.
    ASSERT_EQ(MC_DISP_OK, MC_DispatchInvoke(&doc, "SetVariable", buf, 1, r, nullptr));
    MC_VariantClear(r);
    for (VARIANT& v : buf) MC_VariantClear(v);

    VARIANT got; VariantInit(&got);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariable", { vstr("beyond") }, got));
    EXPECT_EQ(std::string(""), bstr_of(got));
    MC_VariantClear(got);
}

TEST(McDispatch, ExtraArgumentsAreIgnored)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "Trim",
        { vstr("  padded  "), vstr("ignored"), vi4(99) }, r));
    EXPECT_EQ(std::string("padded"), bstr_of(r));
    MC_VariantClear(r);
}

// ── the Lua-default table (2026-08-12) ─────────────────────────────────────
// Methods whose Lua binding documents a NON-ZERO optional default get that
// default here too.  Before the kArgDefaults table, the zero-fill INVERTED
// them: world.EnableTrigger("t") from Python/Perl/JS/Tcl/Ruby disabled the
// trigger it was asked to enable, and DatabaseOpen without flags passed 0 to
// sqlite3_open_v2, which refuses to open.  (Windows COM refused a short list
// outright — DISP_E_BADPARAMCOUNT — so nothing working relied on zero-fill.)
TEST(McDispatch, OmittedTrailingArgsTakeTheLuaDefaults)
{
    CMUSHclientDoc doc;

    // An ENABLED trigger + EnableTrigger with only the name must stay enabled.
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "AddTrigger",
        { vstr("t"), vstr("match"), vstr(""), vi4(1) /*eEnabled*/, vi4(-1),
          vi4(0), vstr(""), vstr("") }, r));
    EXPECT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    VARIANT e; VariantInit(&e);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t") }, e));
    EXPECT_EQ((long) eOK, e.lVal);
    MC_VariantClear(e);

    VARIANT info; VariantInit(&info);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetTriggerInfo", { vstr("t"), vi2(8) }, info));
    EXPECT_EQ((int) VT_BOOL, (int) info.vt);
    EXPECT_TRUE(info.boolVal != 0);   // still enabled (zero-fill disabled it)
    MC_VariantClear(info);

    // The control: an explicit 0 still disables — the default must not win
    // over a SUPPLIED argument.
    VARIANT d; VariantInit(&d);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t"), vi4(0) }, d));
    MC_VariantClear(d);
    VARIANT info2; VariantInit(&info2);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetTriggerInfo", { vstr("t"), vi2(8) }, info2));
    EXPECT_TRUE(info2.boolVal == 0);
    MC_VariantClear(info2);

    // DatabaseOpen with two args gets READWRITE|CREATE, not flags 0.
    char dbpath[256];
    snprintf(dbpath, sizeof dbpath, "/tmp/mc_test_dispdb_%d.sqlite", (int)getpid());
    ::unlink(dbpath);
    VARIANT db; VariantInit(&db);
    ASSERT_EQ(MC_DISP_OK, call(doc, "DatabaseOpen", { vstr("db"), vstr(dbpath) }, db));
    EXPECT_EQ(0L, db.lVal);           // SQLITE_OK — flags 0 fails with MISUSE/CANTOPEN
    MC_VariantClear(db);
    VARIANT dbc; VariantInit(&dbc);
    call(doc, "DatabaseClose", { vstr("db") }, dbc);
    MC_VariantClear(dbc);
    ::unlink(dbpath);
}

// The window default belongs to the methods that HAVE a Which parameter.  The
// table used to name MoveWorldWindow / GetWorldWindowPosition, which do not
// take one at all (they pass 1 to the X variant themselves), so those two rows
// never fired while the methods that needed them -- MoveWorldWindowX with four
// arguments, GetWorldWindowPositionX with none -- got Which = 0 and quietly did
// nothing.
TEST(McDispatch, TheWindowMethodsDefaultToTheFirstWindow)
{
    FakeWorldView view;
    CMUSHclientDoc doc;
    doc.m_pWorldView = &view;

    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "MoveWorldWindowX",
        { vi4(10), vi4(20), vi4(300), vi4(200) }, r));   // no Which supplied
    MC_VariantClear(r);
    EXPECT_TRUE(view.mwX == 10 && view.mwY == 20 &&
                view.mwW == 300 && view.mwH == 200);

    VARIANT p; VariantInit(&p);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetWorldWindowPositionX", {}, p));
    EXPECT_EQ(std::string("10,20,640,480"), bstr_of(p));   // the view's outputRect
    MC_VariantClear(p);

    // The control: Which is read, not ignored -- a second window (which the
    // port does not have) is still answered with nothing.  Without it, a method
    // that dropped Which entirely would pass the two assertions above.
    view.mwX = -1;
    VARIANT r2; VariantInit(&r2);
    ASSERT_EQ(MC_DISP_OK, call(doc, "MoveWorldWindowX",
        { vi4(1), vi4(2), vi4(3), vi4(4), vi2(2) }, r2));
    MC_VariantClear(r2);
    EXPECT_TRUE(view.mwX == -1);

    VARIANT p2; VariantInit(&p2);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetWorldWindowPositionX", { vi2(2) }, p2));
    EXPECT_EQ(std::string(""), bstr_of(p2));
    MC_VariantClear(p2);

    doc.m_pWorldView = nullptr;
}

// Tcl marshals every argument as a string; the boolean words its own model
// treats as true/false everywhere reached atol as 0, so the idiomatic
// `world EnableTrigger t true` DISABLED the trigger while returning eOK.
TEST(McDispatch, BooleanWordsCoerceToNumericParameters)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "AddTrigger",
        { vstr("bw"), vstr("m"), vstr(""), vi4(1), vi4(-1),
          vi4(0), vstr(""), vstr("") }, r));
    MC_VariantClear(r);

    // Disable with "false", then enable with "true" — both words, as Tcl sends.
    VARIANT d; VariantInit(&d);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("bw"), vstr("false") }, d));
    MC_VariantClear(d);
    VARIANT i1; VariantInit(&i1);
    call(doc, "GetTriggerInfo", { vstr("bw"), vi2(8) }, i1);
    EXPECT_TRUE(i1.boolVal == 0);     // "false" really disabled it
    MC_VariantClear(i1);

    VARIANT e; VariantInit(&e);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("bw"), vstr("true") }, e));
    MC_VariantClear(e);
    VARIANT i2; VariantInit(&i2);
    call(doc, "GetTriggerInfo", { vstr("bw"), vi2(8) }, i2);
    EXPECT_TRUE(i2.boolVal != 0);     // "true" enabled, not atol()=0-disabled
    MC_VariantClear(i2);

    // A string PARAMETER is untouched: "on" stays the text "on".
    EXPECT_EQ(std::string("on"), roundtrip_variable(doc, "word", vstr("on")));
}

// %g kept six significant digits, so numbers passed into string parameters
// (SetVariable's value above all) silently lost precision: 1234567.89 stored
// as "1.23457e+06", and every JS integer >= 2^31 rides the VT_R8 path.
// %.14g is LUA_NUMBER_FMT — the six languages now agree byte-for-byte.
TEST(McDispatch, DoublesKeepPrecisionThroughStringParameters)
{
    CMUSHclientDoc doc;
    EXPECT_EQ(std::string("1234567.89"),
              roundtrip_variable(doc, "f", vr8(1234567.89)));
    EXPECT_EQ(std::string("4294967295"),
              roundtrip_variable(doc, "big", vr8(4294967295.0)));
    EXPECT_EQ(std::string("2.5"), roundtrip_variable(doc, "r8", vr8(2.5)));
}

// ── [out] parameters ──────────────────────────────────────────────────────
//
// GetTrigger / GetAlias / GetTimer return their detail through VARIANT* [out]
// parameters -- the one place a script gets more than one value back.  The
// engines each read outargs in declaration order, so the order is the contract.

TEST(McDispatch, OutParametersComeBackInDeclarationOrder)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "AddAlias",
        { vstr("greet"), vstr("hi*"), vstr("hello there"), vi4(1), vstr("") }, r));
    ASSERT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    // GetAlias(name, [out] match, [out] response, [out] flags, [out] script,
    //          [out] plugin) -- five out-params after the name.
    std::vector<VARIANT> outs;
    VARIANT rc; VariantInit(&rc);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetAlias", { vstr("greet") }, rc, &outs));
    EXPECT_EQ((long) eOK, rc.lVal);
    MC_VariantClear(rc);

    ASSERT_EQ((size_t) 5, outs.size());
    EXPECT_EQ(std::string("hi*"),         bstr_of(outs[0]));
    EXPECT_EQ(std::string("hello there"), bstr_of(outs[1]));
    for (VARIANT& v : outs) MC_VariantClear(v);
}

// Asking for something that is not there fills nothing in but still reports.
TEST(McDispatch, OutParametersForAMissingObjectAreNotPopulated)
{
    CMUSHclientDoc doc;
    std::vector<VARIANT> outs;
    VARIANT rc; VariantInit(&rc);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetAlias", { vstr("nope") }, rc, &outs));
    EXPECT_EQ((long) eAliasNotFound, rc.lVal);
    MC_VariantClear(rc);
    for (VARIANT& v : outs) MC_VariantClear(v);
}

// A caller that does not want the out-params passes null, and the substrate has
// to free them itself rather than leak (Holder<VARIANT*>::finalize).
TEST(McDispatch, OutParametersAreFreedWhenTheCallerDoesNotWantThem)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    call(doc, "AddAlias", { vstr("a2"), vstr("m"), vstr("resp"), vi4(1), vstr("") }, r);
    MC_VariantClear(r);

    for (int i = 0; i < 200; i++) {           // leak would be visible to a sanitizer
        VARIANT rc; VariantInit(&rc);
        ASSERT_EQ(MC_DISP_OK, call(doc, "GetAlias", { vstr("a2") }, rc, nullptr));
        MC_VariantClear(rc);
    }
}

// ── properties ────────────────────────────────────────────────────────────

// DISP_PROPERTY_EX: accessor-backed, so the setter's side effects must run.
TEST(McDispatch, AccessorBackedPropertyRoundTrips)
{
    CMUSHclientDoc doc;
    ASSERT_TRUE(MC_DispatchSetProperty(&doc, "NoteColourFore", vi4(0x00FF8040)));
    VARIANT v; VariantInit(&v);
    ASSERT_TRUE(MC_DispatchGetProperty(&doc, "NoteColourFore", v));
    EXPECT_EQ((int) VT_I4, (int) v.vt);
    EXPECT_EQ(0x00FF8040L, v.lVal);
    MC_VariantClear(v);
}

// DISP_PROPERTY_NOTIFY: member-backed, and the change handler has to fire --
// that is the whole point of the NOTIFY form.  m_bLogNotes is the member.
TEST(McDispatch, MemberBackedPropertyWritesTheMemberAndRunsItsHandler)
{
    CMUSHclientDoc doc;
    doc.m_bLogNotes = FALSE;
    ASSERT_TRUE(MC_DispatchSetProperty(&doc, "LogNotes", vbool(true)));
    EXPECT_TRUE(doc.m_bLogNotes != 0);

    VARIANT v; VariantInit(&v);
    ASSERT_TRUE(MC_DispatchGetProperty(&doc, "LogNotes", v));
    EXPECT_TRUE(v.lVal != 0 || v.iVal != 0);
    MC_VariantClear(v);

    ASSERT_TRUE(MC_DispatchSetProperty(&doc, "LogNotes", vbool(false)));
    EXPECT_TRUE(doc.m_bLogNotes == 0);
}

// Property writes coerce the same way arguments do, because a script assigning
// world.Trace = 1 and world.Trace = true must mean the same thing.
TEST(McDispatch, PropertyWritesCoerceLikeArguments)
{
    CMUSHclientDoc doc;
    for (VARIANT on : { vi4(1), vbool(true), vstr("1"), vr8(1.0) }) {
        doc.m_bTrace = FALSE;
        ASSERT_TRUE(MC_DispatchSetProperty(&doc, "Trace", on));
        EXPECT_TRUE(doc.m_bTrace != 0);
        MC_VariantClear(on);
    }
    for (VARIANT off : { vi4(0), vbool(false), vstr("0") }) {
        doc.m_bTrace = TRUE;
        ASSERT_TRUE(MC_DispatchSetProperty(&doc, "Trace", off));
        EXPECT_TRUE(doc.m_bTrace == 0);
        MC_VariantClear(off);
    }
}

// Indexed (DISP_PROPERTY_PARAM) rows are exposed as their Get*/Set* methods
// instead, because there is no plain-attribute form for them.
TEST(McDispatch, IndexedPropertiesAreExposedAsMethods)
{
    EXPECT_TRUE(MC_DispatchHas("GetNormalColour"));
    EXPECT_TRUE(MC_DispatchHas("SetNormalColour"));
    EXPECT_FALSE(MC_DispatchHasProperty("NormalColour"));

    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    call(doc, "SetNormalColour", { vi2(1), vi4(0x00123456) }, r);
    MC_VariantClear(r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetNormalColour", { vi2(1) }, r));
    EXPECT_EQ((int) VT_I4, (int) r.vt);
    EXPECT_EQ(0x00123456L, r.lVal);
    MC_VariantClear(r);
}

// ── VARIANT ownership ─────────────────────────────────────────────────────
//
// The engines call MC_VariantClear on everything the substrate hands them and
// MC_VariantCopy to snapshot out-params.  The stock VariantClear in the shim
// only frees arrays, which is why these exist at all.

TEST(McDispatch, VariantClearFreesBstrsAndIsIdempotent)
{
    VARIANT v = vstr("owned");
    MC_VariantClear(v);
    EXPECT_EQ((int) VT_EMPTY, (int) v.vt);
    EXPECT_TRUE(v.lVal == 0);
    MC_VariantClear(v);                 // clearing twice must not double-free
    EXPECT_EQ((int) VT_EMPTY, (int) v.vt);
}

TEST(McDispatch, VariantCopyDeepCopiesStrings)
{
    VARIANT src = vstr("original");
    VARIANT dst; VariantInit(&dst);
    MC_VariantCopy(dst, src);
    ASSERT_EQ((int) VT_BSTR, (int) dst.vt);
    EXPECT_TRUE(dst.bstrVal != src.bstrVal);        // not shared
    EXPECT_EQ(std::string("original"), bstr_of(dst));

    MC_VariantClear(src);                            // src gone...
    EXPECT_EQ(std::string("original"), bstr_of(dst));// ...dst still readable
    MC_VariantClear(dst);
}

// Arrays are deliberately NOT shared by MC_VariantCopy: the shim has no
// SafeArrayCopy, and sharing the pointer would let both VARIANTs destroy it.
// Dropping to VT_EMPTY is the documented choice; pinned so nobody "fixes" it
// into a double free.
TEST(McDispatch, VariantCopyDropsArraysRatherThanSharingThem)
{
    CMUSHclientDoc doc;
    VARIANT set; VariantInit(&set);
    call(doc, "SetVariable", { vstr("x"), vstr("1") }, set);
    MC_VariantClear(set);

    VARIANT src; VariantInit(&src);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariableList", {}, src));
    ASSERT_EQ((int) (VT_ARRAY | VT_VARIANT), (int) src.vt);

    VARIANT dst; VariantInit(&dst);
    MC_VariantCopy(dst, src);
    EXPECT_EQ((int) VT_EMPTY, (int) dst.vt);
    EXPECT_TRUE(dst.parray == nullptr);

    MC_VariantClear(src);
    MC_VariantClear(dst);
}

// Repeated calls that allocate a BSTR result must not accumulate: the caller
// owns and frees each one.  A leak here is per-trigger-fire in a live world.
TEST(McDispatch, StringResultsAreOwnedByTheCallerEveryTime)
{
    CMUSHclientDoc doc;
    doc.m_mush_name = "TestWorld";
    for (int i = 0; i < 500; i++) {
        VARIANT r; VariantInit(&r);
        ASSERT_EQ(MC_DISP_OK, call(doc, "WorldName", {}, r));
        ASSERT_EQ(std::string("TestWorld"), bstr_of(r));
        MC_VariantClear(r);
    }
}

// ── byte transparency ─────────────────────────────────────────────────────
//
// The substrate must not care what is in a string.  MUD text is frequently not
// valid UTF-8, and the engines rely on the VARIANT boundary being a byte pipe.

TEST(McDispatch, ArbitraryBytesSurviveTheVariantBoundary)
{
    CMUSHclientDoc doc;
    // 0xE9 alone is not valid UTF-8; 0xC3 0xA9 is the same character in UTF-8.
    EXPECT_EQ(std::string("a\xE9z"),     roundtrip_variable(doc, "latin1", vstr("a\xE9z")));
    EXPECT_EQ(std::string("a\xC3\xA9z"), roundtrip_variable(doc, "utf8",   vstr("a\xC3\xA9z")));
    EXPECT_EQ(std::string("\xFF\xFE\x01"), roundtrip_variable(doc, "raw",  vstr("\xFF\xFE\x01")));
}

MC_TEST_MAIN()
