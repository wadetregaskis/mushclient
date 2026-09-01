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

#include <climits>   // LONG_MIN / LONG_MAX -- the clamping test
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
VARIANT vr4(float f)         { VARIANT v; VariantInit(&v); v.vt = VT_R4;   v.fltVal  = f; return v; }
// VARIANT_TRUE is -1, which is what py/js/ruby_to_variant now send and what a
// COM scripting host sends on Windows -- see the boolean note in
// platform/mc_dispatch.cpp.  Writing 1 here would make the fixture disagree
// with every engine that feeds the substrate in the shipping app.
VARIANT vbool(bool b)        { VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = b ? -1 : 0; return v; }
// The language's nil: Python None, Ruby nil, Perl undef and JavaScript
// undefined/null all marshal to VT_EMPTY (py_engine.cpp:79 and friends).
VARIANT vnil()               { VARIANT v; VariantInit(&v); v.vt = VT_EMPTY; v.lVal = 0; return v; }
// VT_NULL: no engine emits it today, but it is the third no-value VARIANT type
// a direct MC_DispatchInvoke caller can hand the substrate.
VARIANT vnull()              { VARIANT v; VariantInit(&v); v.vt = VT_NULL;  v.lVal = 0; return v; }

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

// Add a trigger in a known enabled/disabled state, so an argument-default test
// can start from the state the call under test must CHANGE.
void add_trigger(CMUSHclientDoc& doc, const char* name, bool enabled)
{
    VARIANT r; VariantInit(&r);
    call(doc, "AddTrigger", { vstr(name), vstr("match"), vstr(""),
                              vi4(enabled ? 1 /*eEnabled*/ : 0), vi4(-1),
                              vi4(0), vstr(""), vstr("") }, r);
    MC_VariantClear(r);
}

// Is it enabled?  (GetTriggerInfo info type 8.)
bool trigger_enabled(CMUSHclientDoc& doc, const char* name)
{
    VARIANT info; VariantInit(&info);
    call(doc, "GetTriggerInfo", { vstr(name), vi2(8) }, info);
    const bool on = (info.vt == VT_BOOL && info.boolVal != 0);
    MC_VariantClear(info);
    return on;
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
    // "-1", not "1": VarBstrFromBool renders the VARIANT_BOOL number, and
    // MFC passes no VARIANT_ALPHABOOL, so a Windows plugin doing
    // world.SetVariable("flag", True) then testing == "-1" is right.
    EXPECT_EQ(std::string("-1"),    roundtrip_variable(doc, "bt", vbool(true)));
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

// ── a fraction meeting an integer parameter: oleaut ROUNDS ────────────────
//
// On Windows these five languages ride the real IDispatch::Invoke, whose
// VariantChangeType coercion (VarI4FromR8 / VarI4FromStr) rounds to nearest
// with ties to EVEN.  var_to_long used a C cast and atol, i.e. truncation
// toward zero, so every fraction of .5 or more landed one short -- silently,
// eOK either way.  JavaScript feels it most (all its arithmetic is double and
// js_engine routes a non-integral number as VT_R8); Tcl feels it through the
// string arm, since tcl_to_variant marshals EVERY argument as VT_BSTR.
//
// MoveWorldWindowX's four coordinates are the observable: VTS_I4 each, stored
// on the view verbatim -- signed and unmasked, unlike the colour properties --
// and four at a time, so one call pins four values.
//
// The pairs are chosen to discriminate the three candidate rules from each
// other, which a single .5 case cannot do:
//     truncate      10.5 -> 10   11.5 -> 11   -10.5 -> -10
//     half-away     10.5 -> 11   11.5 -> 12   -10.5 -> -11
//     half-to-even  10.5 -> 10   11.5 -> 12   -10.5 -> -10   (oleaut, and ours)

namespace {

struct Coords { long x, y, w, h; };

// Poison the destination first: a coercion that did nothing at all -- or a
// method that never ran -- must not be able to read as a pass.
Coords move4(FakeWorldView& view, CMUSHclientDoc& doc,
             VARIANT a, VARIANT b, VARIANT c, VARIANT d)
{
    view.mwX = view.mwY = view.mwW = view.mwH = 0x5EED;
    VARIANT r; VariantInit(&r);
    call(doc, "MoveWorldWindowX", { a, b, c, d, vi2(1) }, r);
    MC_VariantClear(r);
    return { view.mwX, view.mwY, view.mwW, view.mwH };
}

}   // namespace

TEST(McDispatch, FractionalNumbersRoundHalfToEvenIntoIntegerParameters)
{
    FakeWorldView view;
    CMUSHclientDoc doc;
    doc.m_pWorldView = &view;

    // The two ties are what separate half-to-even from half-away-from-zero;
    // 10.7 and 10.2 separate rounding from truncation at all.
    Coords c = move4(view, doc, vr8(10.5), vr8(11.5), vr8(10.7), vr8(10.2));
    EXPECT_EQ(10L, c.x);        // tie -> 10 is even
    EXPECT_EQ(12L, c.y);        // tie -> 12 is even   (truncation gave 11)
    EXPECT_EQ(11L, c.w);        // truncation gave 10
    EXPECT_EQ(10L, c.h);

    // Negatives round by the same rule -- toward the even neighbour, not
    // toward or away from zero.
    Coords n = move4(view, doc, vr8(-10.5), vr8(-11.5), vr8(-10.7), vr8(-10.2));
    EXPECT_EQ(-10L, n.x);
    EXPECT_EQ(-12L, n.y);
    EXPECT_EQ(-11L, n.w);       // truncation gave -10
    EXPECT_EQ(-10L, n.h);

    // VT_R4 travels the same road (Perl's NV and Ruby's Float arrive as R8,
    // but a direct caller can hand a float).  These four are all exact in
    // binary32, so the float itself contributes no rounding of its own.
    Coords f = move4(view, doc, vr4(10.5f), vr4(11.5f), vr4(-10.5f), vr4(0.5f));
    EXPECT_EQ(10L, f.x);
    EXPECT_EQ(12L, f.y);
    EXPECT_EQ(-10L, f.w);
    EXPECT_EQ(0L,  f.h);        // 0 is even

    // Whole numbers are untouched -- the fix must not perturb the common case.
    Coords w = move4(view, doc, vr8(0), vr8(-1), vr8(4096), vr8(2147483647.0));
    EXPECT_EQ(0L, w.x);
    EXPECT_EQ(-1L, w.y);
    EXPECT_EQ(4096L, w.w);
    EXPECT_EQ(2147483647L, w.h);
}

TEST(McDispatch, NumericStringsRoundTheSameWayAsNumbers)
{
    FakeWorldView view;
    CMUSHclientDoc doc;
    doc.m_pWorldView = &view;

    // Tcl reaches this arm for every argument it sends, so it is the widest
    // of the three.  atol stopped at the '.' and answered 45 for "45.7".
    Coords c = move4(view, doc, vstr("10.5"), vstr("11.5"), vstr("10.7"), vstr("-10.5"));
    EXPECT_EQ(10L, c.x);
    EXPECT_EQ(12L, c.y);
    EXPECT_EQ(11L, c.w);
    EXPECT_EQ(-10L, c.h);

    // Plain integers, leading whitespace and a sign are unchanged: this is a
    // rounding fix, not a re-parse.
    Coords i = move4(view, doc, vstr("42"), vstr("-7"), vstr("  8"), vstr("+3"));
    EXPECT_EQ(42L, i.x);
    EXPECT_EQ(-7L, i.y);
    EXPECT_EQ(8L,  i.w);
    EXPECT_EQ(3L,  i.h);

    // ...and the strings atol read as 0 still read as 0.  The port's leniency
    // for a non-numeric string is deliberate (D-24 / UPSTREAM-DIVERGENCES §17
    // keep strict checking to arity, never types, because Tcl marshals
    // everything as a string); strtod would otherwise have widened the set
    // silently, accepting C99 hex floats and "inf"/"nan" that oleaut's decimal
    // parser rejects.
    Coords z = move4(view, doc, vstr("abc"), vstr("0x10"), vstr("inf"), vstr(""));
    EXPECT_EQ(0L, z.x);
    EXPECT_EQ(0L, z.y);
    EXPECT_EQ(0L, z.w);
    EXPECT_EQ(0L, z.h);

    // The boolean words Tcl sends still win over the numeric read.
    Coords b = move4(view, doc, vstr("true"), vstr("off"), vstr("yes"), vstr("no"));
    EXPECT_EQ(1L, b.x);
    EXPECT_EQ(0L, b.y);
    EXPECT_EQ(1L, b.w);
    EXPECT_EQ(0L, b.h);
}

// ── a language boolean is VARIANT_TRUE, and it stays -1 ───────────────────
//
// The scripting hosts the Windows build rides marshal `true` as VARIANT_TRUE,
// whose value is -1.  Where the dispatch map declares a NUMBER or a STRING,
// oleaut's VarI4FromBool / VarBstrFromBool pass that number through, so the
// method sees -1 and a string parameter receives "-1".  The port sent 1 and
// coerced to 1 / "1", so the same plugin line left the two platforms with
// different world state, silently and successfully both times.
//
// A VTS_BOOL parameter is a separate question and is unchanged: VARIANT_BOOL
// is 16-bit where MFC's BOOL is 32-bit, so that conversion cannot be a copy,
// and every shipping method with a BOOL parameter normalises for itself.
TEST(McDispatch, ALanguageBooleanReachesANumericParameterAsMinusOne)
{
    FakeWorldView view;
    CMUSHclientDoc doc;
    doc.m_pWorldView = &view;

    // MoveWorldWindowX's four coordinates are VTS_I4 and land on the view
    // verbatim, so the number the method actually received is readable.
    Coords c = move4(view, doc, vbool(true), vbool(false), vbool(true), vi4(7));
    EXPECT_EQ(-1L, c.x);        // was 1
    EXPECT_EQ(0L,  c.y);
    EXPECT_EQ(-1L, c.w);
    EXPECT_EQ(7L,  c.h);        // the control: a real number is untouched

    // The consequence a plugin actually meets.  Every boolean world option's
    // row is min 0 / max 1 (SetBaseOptionItem turns a 0,0 row into 0,1), so
    // -1 is out of range -- Windows refuses this call, and now so does the
    // port, rather than quietly succeeding where Windows quietly failed.
    VARIANT before; VariantInit(&before);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetOption", { vstr("enable_triggers") }, before));
    const long was = before.lVal;
    MC_VariantClear(before);

    VARIANT s; VariantInit(&s);
    ASSERT_EQ(MC_DISP_OK, call(doc, "SetOption",
        { vstr("enable_triggers"), vbool(true) }, s));
    EXPECT_EQ((long) eOptionOutOfRange, s.lVal);
    MC_VariantClear(s);

    VARIANT after; VariantInit(&after);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetOption", { vstr("enable_triggers") }, after));
    EXPECT_EQ(was, after.lVal);          // ...and the option really is unchanged
    MC_VariantClear(after);

    // The control for that: an explicit 1 is still accepted, so the refusal
    // above is about the VALUE -1 and not about SetOption being broken.
    VARIANT ok; VariantInit(&ok);
    ASSERT_EQ(MC_DISP_OK, call(doc, "SetOption",
        { vstr("enable_triggers"), vi4(1) }, ok));
    EXPECT_EQ((long) eOK, ok.lVal);
    MC_VariantClear(ok);
}

// ...and the other side of that split: a parameter or property the map
// declares VT_BOOL still stores canonical truth, so -1 cannot reach a BOOL
// member that is written raw.  m_bMapping is the one such member whose stored
// value is readable -- GetInfo 112 hands it back through SetUpVariantBool,
// which copies the BOOL into boolVal without normalising, so this assertion
// sees the number itself and fails if var_to_bool is removed.
TEST(McDispatch, ABooleanParameterStillStoresCanonicalTruth)
{
    CMUSHclientDoc doc;
    doc.m_bMapping = FALSE;

    ASSERT_TRUE(MC_DispatchSetProperty(&doc, "Mapping", vbool(true)));
    EXPECT_EQ(1, (int) doc.m_bMapping);           // 1, not VARIANT_TRUE's -1

    VARIANT info; VariantInit(&info);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetInfo", { vi2(112) }, info));
    EXPECT_EQ((int) VT_BOOL, (int) info.vt);
    EXPECT_EQ(1, (int) info.boolVal);
    MC_VariantClear(info);

    ASSERT_TRUE(MC_DispatchSetProperty(&doc, "Mapping", vbool(false)));
    EXPECT_EQ(0, (int) doc.m_bMapping);

    // A VTS_BOOL argument behaves the same way, and the effect is unchanged by
    // any of this: the trigger starts disabled, so a call that did nothing
    // fails here.
    add_trigger(doc, "b_true", false);
    VARIANT e; VariantInit(&e);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("b_true"), vbool(true) }, e));
    EXPECT_EQ((long) eOK, e.lVal);
    MC_VariantClear(e);
    EXPECT_TRUE(trigger_enabled(doc, "b_true"));

    VARIANT d; VariantInit(&d);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("b_true"), vbool(false) }, d));
    MC_VariantClear(d);
    EXPECT_FALSE(trigger_enabled(doc, "b_true"));
}

// A double too large for a long used to be an undefined cast (and, on the
// string arm, a saturating atol).  Windows answers DISP_E_OVERFLOW, which this
// substrate has no channel to report; clamping is the honest stand-in, and it
// is at least well defined.
TEST(McDispatch, OutOfRangeNumbersClampInsteadOfBeingUndefined)
{
    FakeWorldView view;
    CMUSHclientDoc doc;
    doc.m_pWorldView = &view;

    Coords c = move4(view, doc, vr8(1e300), vr8(-1e300), vr8(0.0 / 0.0), vstr("1e400"));
    EXPECT_EQ(LONG_MAX, c.x);
    EXPECT_EQ(LONG_MIN, c.y);
    EXPECT_EQ(0L, c.w);          // NaN -- what the cast it replaces produced
    EXPECT_EQ(LONG_MAX, c.h);
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

// ...and a default that is a STRING, not a number.
//
// kArgDefaults could only carry a `long`, so the three Lua opt* defaults that
// are strings -- ArrayExport/ArrayExportKeys' Delimiter (slot 1) and
// ArrayImport's (slot 2), all "," -- were silently absent, and the omitted
// delimiter zero-filled to "".  That is not merely a different default: all
// three methods reject a delimiter whose length is not 1, so the export
// methods returned the NUMBER eBadDelimiter where Lua returns the exported
// string, and the import stored nothing.  A plugin written against the
// Lua-flavoured docs got an integer where it expected text, with no error.
//
// Every assertion below fails on the unfixed code -- the exports return VT_I4
// and the import returns eBadDelimiter -- so "the padding did nothing" cannot
// pass.
TEST(McDispatch, OmittedStringArgsTakeTheLuaStringDefault)
{
    CMUSHclientDoc doc;

    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayCreate", { vstr("a") }, r));
    ASSERT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    // Two entries, so the JOINING delimiter is exercised and not just the
    // key/value one: a one-entry array would leave the two roles degenerate.
    VARIANT s1; VariantInit(&s1);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArraySet", { vstr("a"), vstr("k1"), vstr("v1") }, s1));
    MC_VariantClear(s1);
    VARIANT s2a; VariantInit(&s2a);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArraySet", { vstr("a"), vstr("k2"), vstr("v2") }, s2a));
    MC_VariantClear(s2a);

    // ArrayExport(name) -- Lua's my_optstring(L, 2, ",") default.
    VARIANT e; VariantInit(&e);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayExport", { vstr("a") }, e));
    EXPECT_EQ((int) VT_BSTR, (int) e.vt);            // was VT_I4 eBadDelimiter
    EXPECT_EQ(std::string("k1,v1,k2,v2"), bstr_of(e));
    MC_VariantClear(e);

    // ...and a SUPPLIED delimiter still wins over the default.
    VARIANT e2; VariantInit(&e2);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayExport", { vstr("a"), vstr("|") }, e2));
    EXPECT_EQ(std::string("k1|v1|k2|v2"), bstr_of(e2));
    MC_VariantClear(e2);

    // ArrayExportKeys(name) -- same slot, same default.
    VARIANT k; VariantInit(&k);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayExportKeys", { vstr("a") }, k));
    EXPECT_EQ((int) VT_BSTR, (int) k.vt);
    EXPECT_EQ(std::string("k1,k2"), bstr_of(k));
    MC_VariantClear(k);

    // ArrayImport(name, values) -- the default is slot 2 here.
    VARIANT c; VariantInit(&c);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayCreate", { vstr("b") }, c));
    MC_VariantClear(c);
    VARIANT i; VariantInit(&i);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayImport",
        { vstr("b"), vstr("k,v,k2,v2") }, i));
    EXPECT_EQ((long) eOK, i.lVal);                   // was eBadDelimiter
    MC_VariantClear(i);
    VARIANT g; VariantInit(&g);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayGet", { vstr("b"), vstr("k2") }, g));
    EXPECT_EQ(std::string("v2"), bstr_of(g));
    MC_VariantClear(g);

    // An explicit nil takes the string default too -- the padding treats
    // "omitted" and "nil" as one rule, and a string row must not escape it.
    VARIANT n; VariantInit(&n);
    ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayExport", { vstr("a"), vnil() }, n));
    EXPECT_EQ(std::string("k1,v1,k2,v2"), bstr_of(n));
    MC_VariantClear(n);

    // ...and with strict checking ON the delimiter is no longer demanded:
    // slot_has_default reads the same table, so a string row must shorten
    // required_args exactly as a numeric one does.
    doc.m_bStrictScriptArguments = TRUE;
    EXPECT_EQ(1, MC_DispatchRequiredArgs("ArrayExport"));
    EXPECT_EQ(2, MC_DispatchRequiredArgs("ArrayImport"));
    VARIANT s2; VariantInit(&s2);
    EXPECT_EQ(MC_DISP_OK, call(doc, "ArrayExport", { vstr("a") }, s2));
    EXPECT_EQ(std::string("k1,v1,k2,v2"), bstr_of(s2));
    MC_VariantClear(s2);
    doc.m_bStrictScriptArguments = FALSE;
}

// The borrowed string default must survive being handed out repeatedly, and
// must not be freed with the call: `padded` holds a shallow VIEW of a static
// literal, so a single var_clear of it would corrupt the table for every later
// call.  Two hundred invocations in a row is the cheapest way to say so --
// under a sanitizer or Guard Malloc a freed literal fails here, not somewhere
// unrelated later.
TEST(McDispatch, TheBorrowedStringDefaultIsNotConsumedByTheCall)
{
    CMUSHclientDoc doc;
    VARIANT r; VariantInit(&r);
    call(doc, "ArrayCreate", { vstr("a") }, r);
    MC_VariantClear(r);
    VARIANT s; VariantInit(&s);
    call(doc, "ArraySet", { vstr("a"), vstr("k"), vstr("v") }, s);
    MC_VariantClear(s);

    for (int n = 0; n < 200; ++n)
    {
        VARIANT e; VariantInit(&e);
        ASSERT_EQ(MC_DISP_OK, call(doc, "ArrayExport", { vstr("a") }, e));
        ASSERT_EQ(std::string("k,v"), bstr_of(e));
        MC_VariantClear(e);
    }
}


// ── an EXPLICIT nil is an omitted argument, not a zero ─────────────────────
//
// Python None, Ruby nil, Perl undef and JavaScript undefined/null all reach the
// substrate as VT_EMPTY.  The padding above used to fill only slots BEYOND the
// supplied count, so an explicit nil sailed past the default table and
// var_to_long read it as 0 -- which made the two spellings of one call mean
// OPPOSITE things: world.EnableTrigger("t") enabled the trigger and
// world.EnableTrigger("t", None) DISABLED it, both returning eOK.
//
// Lua, whose opt* defaults kArgDefaults copies, never had that split:
// optboolean (lua_methods.cpp:335) says "nil will default to the default", and
// my_optnumber is luaL_opt, i.e. lua_isnoneornil.  That is shipping Windows
// source, so this is Windows parity for the language where MUSHclient's
// optional-argument defaults are actually defined.
//
// The fixture is chosen so "the call did nothing" fails every row: the rows
// that must ENABLE start disabled, and the row that must DISABLE starts
// enabled.  Asserting the enable/disable OUTCOME rather than the coerced number
// is the point -- the defect was an inverted effect, not a wrong integer.
TEST(McDispatch, ExplicitNilTakesTheSameDefaultAsAnOmittedArgument)
{
    CMUSHclientDoc doc;

    add_trigger(doc, "t_omit",  false);
    add_trigger(doc, "t_nil",   false);
    add_trigger(doc, "t_null",  false);
    add_trigger(doc, "t_true",  false);
    add_trigger(doc, "t_false", true);
    ASSERT_FALSE(trigger_enabled(doc, "t_omit"));      // the fixture really is
    ASSERT_TRUE (trigger_enabled(doc, "t_false"));     // non-degenerate

    // omitted -- the behaviour the nil rows must match.
    VARIANT o; VariantInit(&o);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t_omit") }, o));
    EXPECT_EQ((long) eOK, o.lVal);
    MC_VariantClear(o);
    EXPECT_TRUE(trigger_enabled(doc, "t_omit"));

    // explicit nil -- the bug: this used to disable.
    VARIANT n; VariantInit(&n);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t_nil"), vnil() }, n));
    EXPECT_EQ((long) eOK, n.lVal);
    MC_VariantClear(n);
    EXPECT_TRUE(trigger_enabled(doc, "t_nil"));

    // explicit VT_NULL -- same rule, from the same reasoning.
    VARIANT z; VariantInit(&z);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t_null"), vnull() }, z));
    MC_VariantClear(z);
    EXPECT_TRUE(trigger_enabled(doc, "t_null"));

    // explicit true -- unchanged, and not vacuous: it starts disabled.
    VARIANT t; VariantInit(&t);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t_true"), vbool(true) }, t));
    MC_VariantClear(t);
    EXPECT_TRUE(trigger_enabled(doc, "t_true"));

    // explicit false -- THE control.  A supplied value is never defaulted, so a
    // sloppier fix ("anything falsy means absent") passes every row above and
    // fails here.
    VARIANT f; VariantInit(&f);
    ASSERT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t_false"), vbool(false) }, f));
    MC_VariantClear(f);
    EXPECT_FALSE(trigger_enabled(doc, "t_false"));
}

// A second method, so the rule is not one fixture's accident: a NUMERIC default
// rather than a boolean one, on a method with no world state at all.
// BlendPixel's Opacity defaults to 1.0 (fully apply the blend colour); the
// zero-coerced nil made it 0.0, which returns the BASE colour instead -- the
// same inversion wearing different clothes.
TEST(McDispatch, ExplicitNilTakesANumericDefaultToo)
{
    CMUSHclientDoc doc;
    const long blend = 0x0000FF, base = 0x00FF00;   // mode 1 = Blend_Normal

    VARIANT full; VariantInit(&full);
    ASSERT_EQ(MC_DISP_OK, call(doc, "BlendPixel",
        { vi4(blend), vi4(base), vi2(1), vr8(1.0) }, full));
    VARIANT none; VariantInit(&none);
    ASSERT_EQ(MC_DISP_OK, call(doc, "BlendPixel",
        { vi4(blend), vi4(base), vi2(1), vr8(0.0) }, none));
    // The two endpoints must differ, or nothing below can discriminate.
    ASSERT_NE(full.lVal, none.lVal);

    VARIANT omitted; VariantInit(&omitted);
    ASSERT_EQ(MC_DISP_OK, call(doc, "BlendPixel",
        { vi4(blend), vi4(base), vi2(1) }, omitted));
    EXPECT_EQ(full.lVal, omitted.lVal);

    VARIANT nil; VariantInit(&nil);
    ASSERT_EQ(MC_DISP_OK, call(doc, "BlendPixel",
        { vi4(blend), vi4(base), vi2(1), vnil() }, nil));
    EXPECT_EQ(full.lVal, nil.lVal);        // not none.lVal, which was the bug

    MC_VariantClear(full); MC_VariantClear(none);
    MC_VariantClear(omitted); MC_VariantClear(nil);
}

// Filling a nil slot must not SHORTEN the list.  Most defaulted slots are a
// method's LAST, where the distinction cannot show; five window methods put
// theirs in the middle (WindowDrawImage's Mode, WindowBlendImage's Opacity,
// WindowFont's Charset, ...), and there a length computed from the defaulted
// slot alone throws away every argument after it -- for DrawImage, the source
// rectangle, which then reads as 0,0,0,0 and means "the whole image".
//
// So this drives the real thing, through the surface the headless tests raster
// into.  Three distinct colours make all three outcomes distinguishable:
//   drawn from the right sub-rectangle -> RED    (correct)
//   drawn from the whole image         -> BLUE   (the length was truncated)
//   not drawn at all                   -> GREEN  (Mode's nil never took its
//                                                 default, so no case matched)
TEST(McDispatch, AnExplicitNilDoesNotTruncateTheArgumentsAfterIt)
{
    const long kRed = 0x0000FF, kBlue = 0xFF0000, kGreen = 0x00FF00;
    CMUSHclientDoc doc;

    // A 4x4 blue source window whose bottom-right 2x2 quadrant is red.
    VARIANT w; VariantInit(&w);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowCreate",
        { vstr("src"), vi4(0), vi4(0), vi4(4), vi4(4), vi2(0), vi4(0), vi4(kBlue) }, w));
    ASSERT_EQ((long) eOK, w.lVal);
    MC_VariantClear(w);
    VARIANT fill; VariantInit(&fill);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowRectOp",
        { vstr("src"), vi2(2), vi4(2), vi4(2), vi4(4), vi4(4), vi4(kRed), vi4(0) }, fill));
    ASSERT_EQ((long) eOK, fill.lVal);
    MC_VariantClear(fill);

    // The fixture is non-degenerate: the two source quadrants really differ.
    VARIANT p; VariantInit(&p);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowGetPixel", { vstr("src"), vi4(0), vi4(0) }, p));
    ASSERT_EQ(kBlue, p.lVal);
    MC_VariantClear(p);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowGetPixel", { vstr("src"), vi4(3), vi4(3) }, p));
    ASSERT_EQ(kRed, p.lVal);
    MC_VariantClear(p);

    // A green destination, and the source window captured into an image on it.
    VARIANT d; VariantInit(&d);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowCreate",
        { vstr("dst"), vi4(0), vi4(0), vi4(4), vi4(4), vi2(0), vi4(0), vi4(kGreen) }, d));
    ASSERT_EQ((long) eOK, d.lVal);
    MC_VariantClear(d);
    VARIANT img; VariantInit(&img);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowImageFromWindow",
        { vstr("dst"), vstr("img"), vstr("src") }, img));
    ASSERT_EQ((long) eOK, img.lVal);
    MC_VariantClear(img);

    // Mode (slot 6) explicitly nil -> its default 1, a straight copy; the source
    // rectangle after it names the red quadrant.
    VARIANT drew; VariantInit(&drew);
    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowDrawImage",
        { vstr("dst"), vstr("img"), vi4(0), vi4(0), vi4(0), vi4(0), vnil(),
          vi4(2), vi4(2), vi4(4), vi4(4) }, drew));
    ASSERT_EQ((long) eOK, drew.lVal);
    MC_VariantClear(drew);

    ASSERT_EQ(MC_DISP_OK, call(doc, "WindowGetPixel", { vstr("dst"), vi4(0), vi4(0) }, p));
    EXPECT_EQ(kRed, p.lVal);
    MC_VariantClear(p);
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

// ── strict argument checking (world option "script_strict_arguments") ──────
//
// Opt-in, default OFF.  With it off the substrate zero-fills every parameter a
// short list did not supply -- world.SetVariable("x") sets "x" to "" -- which
// is this port's invention: the real IDispatch::Invoke the Windows build rides
// answers DISP_E_BADPARAMCOUNT instead (the dispatch map declares no
// VTS_VARIANT anywhere, so every declared parameter is required), and Lua's own
// binding errors too.  With it on, the five embedded engines match both.
//
// ARITY ONLY, never types: tcl_engine.cpp marshals every argument as VT_BSTR by
// design, so a type-strict check would reject every numeric parameter Tcl
// passes -- the whole language.

// The control.  Nothing about the default path may move: a short list still
// zero-fills and still succeeds.  Every other test in this file runs with the
// option off, so this is what they all rely on.
TEST(McDispatch, StrictOffLeavesTheZeroFillAlone)
{
    CMUSHclientDoc doc;
    ASSERT_FALSE(doc.m_bStrictScriptArguments);   // the shipped default

    VARIANT r; VariantInit(&r);
    EXPECT_EQ(MC_DISP_OK, call(doc, "SetVariable", { vstr("v") }, r));
    MC_VariantClear(r);

    // ...and the method really ran: the variable exists, holding the zero-fill.
    VARIANT g; VariantInit(&g);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariable", { vstr("v") }, g));
    EXPECT_EQ((int) VT_BSTR, (int) g.vt);
    EXPECT_EQ(std::string(""), bstr_of(g));
    MC_VariantClear(g);
}

// Strict on: the same call is refused, and -- the half that matters -- the
// method does not run.  An error code the caller ignores would be no better
// than the zero-fill if the world had already been mutated.
TEST(McDispatch, StrictOnRefusesAShortArgumentList)
{
    CMUSHclientDoc doc;
    doc.m_bStrictScriptArguments = TRUE;

    VARIANT r; VariantInit(&r);
    EXPECT_EQ(MC_DISP_BADARGS, call(doc, "SetVariable", { vstr("v") }, r));
    MC_VariantClear(r);

    VARIANT g; VariantInit(&g);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariable", { vstr("v") }, g));
    EXPECT_NE((int) VT_BSTR, (int) g.vt);        // never created
    MC_VariantClear(g);

    // A COMPLETE list is untouched by the option.
    VARIANT ok; VariantInit(&ok);
    EXPECT_EQ(MC_DISP_OK, call(doc, "SetVariable", { vstr("v"), vstr("set") }, ok));
    MC_VariantClear(ok);
    VARIANT g2; VariantInit(&g2);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariable", { vstr("v") }, g2));
    EXPECT_EQ(std::string("set"), bstr_of(g2));
    MC_VariantClear(g2);
}

// The ordering crux.  The check runs AFTER the kArgDefaults padding, so a slot
// the Lua-parity table fills counts as supplied.
//
// The two halves below are what stops a naive "padded nargs < arity" check from
// passing this file: EnableTrigger's default sits at its LAST slot, so padding
// always brings a 1-argument call up to full arity -- and would bring a
// 0-argument call up too, silently enabling a trigger named "".  Only a check
// that asks whether each omitted slot HAS a default tells those two apart.
TEST(McDispatch, StrictOnStillHonoursTheArgumentDefaults)
{
    CMUSHclientDoc doc;
    doc.m_bStrictScriptArguments = TRUE;

    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "AddTrigger",
        { vstr("t"), vstr("match"), vstr(""), vi4(1) /*eEnabled*/, vi4(-1),
          vi4(0), vstr(""), vstr("") }, r));
    ASSERT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    // Slot 1 has a default, so ONE argument is a complete call.
    VARIANT e; VariantInit(&e);
    EXPECT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("t") }, e));
    EXPECT_EQ((long) eOK, e.lVal);
    MC_VariantClear(e);
    VARIANT info; VariantInit(&info);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetTriggerInfo", { vstr("t"), vi2(8) }, info));
    EXPECT_TRUE(info.boolVal != 0);               // the default still applied
    MC_VariantClear(info);

    // Slot 0 has none, so no argument at all is still an error -- even though
    // the padding would happily manufacture a full-length list.
    VARIANT z; VariantInit(&z);
    EXPECT_EQ(MC_DISP_BADARGS, call(doc, "EnableTrigger", {}, z));
    MC_VariantClear(z);

    // DatabaseOpen: two supplied, the sqlite flags defaulted.  A different
    // method with a different arity, so the pair is not one fixture twice.
    char dbpath[256];
    snprintf(dbpath, sizeof dbpath, "/tmp/mc_test_strictdb_%d.sqlite", (int)getpid());
    ::unlink(dbpath);
    VARIANT db; VariantInit(&db);
    EXPECT_EQ(MC_DISP_OK, call(doc, "DatabaseOpen", { vstr("db"), vstr(dbpath) }, db));
    EXPECT_EQ(0L, db.lVal);                       // SQLITE_OK
    MC_VariantClear(db);
    VARIANT dbc; VariantInit(&dbc);
    call(doc, "DatabaseClose", { vstr("db") }, dbc);
    MC_VariantClear(dbc);
    ::unlink(dbpath);
}

// Strict must not read an explicit nil as a MISSING argument.  The padding
// treats the two alike -- that is the fix above -- but the arity check counts
// what the caller WROTE, so turning the option on cannot start rejecting
// world.EnableTrigger("t", None): a call that works with strict off, and whose
// omitted-argument spelling works with strict on, must work here too.
//
// The pair of halves is what makes it a test rather than a restatement: the
// nil-filled slot is accepted because the slot HAS a default, while the same
// nil in a slot that has none (SetVariable's value) is still a supplied
// argument and so still passes the count -- it is the trailing slot that must
// not be silently un-counted.
TEST(McDispatch, StrictOnAcceptsAnExplicitNil)
{
    CMUSHclientDoc doc;
    doc.m_bStrictScriptArguments = TRUE;

    add_trigger(doc, "s_nil",   false);
    add_trigger(doc, "s_false", true);

    VARIANT n; VariantInit(&n);
    EXPECT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("s_nil"), vnil() }, n));
    EXPECT_EQ((long) eOK, n.lVal);
    MC_VariantClear(n);
    EXPECT_TRUE(trigger_enabled(doc, "s_nil"));       // and it took the default

    // ...and strict did not quietly change what a SUPPLIED false means either.
    VARIANT f; VariantInit(&f);
    EXPECT_EQ(MC_DISP_OK, call(doc, "EnableTrigger", { vstr("s_false"), vbool(false) }, f));
    MC_VariantClear(f);
    EXPECT_FALSE(trigger_enabled(doc, "s_false"));

    // A nil in a slot with NO default is still a supplied argument: the call is
    // accepted (it is not short) and the zero-value coercion stands.
    VARIANT s; VariantInit(&s);
    EXPECT_EQ(MC_DISP_OK, call(doc, "SetVariable", { vstr("sv"), vnil() }, s));
    MC_VariantClear(s);
    VARIANT g; VariantInit(&g);
    ASSERT_EQ(MC_DISP_OK, call(doc, "GetVariable", { vstr("sv") }, g));
    EXPECT_EQ((int) VT_BSTR, (int) g.vt);             // it really was created
    EXPECT_EQ(std::string(""), bstr_of(g));
    MC_VariantClear(g);

    // The control: the option is still ON and still refuses a genuinely short
    // list, so the three acceptances above are not "strict quietly off".
    VARIANT bad; VariantInit(&bad);
    EXPECT_EQ(MC_DISP_BADARGS, call(doc, "SetVariable", { vstr("short") }, bad));
    MC_VariantClear(bad);
}

// The by-ref getters.  GetTrigger/GetAlias/GetTimer declare trailing [out]
// VARIANT* parameters, and on Windows a COM script really does pass by-ref
// variables for all of them.  Off Windows it does not: every engine synthesises
// those slots itself and hands the values back as a tuple/list, so
// world.GetTrigger("t") IS the complete call here with one argument.
//
// Counting the declared arity instead of the script-supplied half rejects all
// three outright -- which is precisely what the over-broad mutation of this
// feature (strict made unconditional) showed, by failing the three long-
// standing OutParameters tests above.
TEST(McDispatch, StrictOnDoesNotCountTheByRefOutputParameters)
{
    CMUSHclientDoc doc;
    doc.m_bStrictScriptArguments = TRUE;

    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "AddTrigger",
        { vstr("t"), vstr("match"), vstr("resp"), vi4(1), vi4(-1),
          vi4(0), vstr(""), vstr("") }, r));
    ASSERT_EQ((long) eOK, r.lVal);
    MC_VariantClear(r);

    // One argument, seven [out] slots the engine fills -- and it must still run.
    std::vector<VARIANT> outs;
    VARIANT g; VariantInit(&g);
    EXPECT_EQ(MC_DISP_OK, call(doc, "GetTrigger", { vstr("t") }, g, &outs));
    EXPECT_EQ((long) eOK, g.lVal);
    EXPECT_EQ((size_t) 7, outs.size());
    MC_VariantClear(g);
    for (auto& o : outs) MC_VariantClear(o);

    // ...so the required count is the SCRIPT's half of the parameter list.
    EXPECT_EQ(1, MC_DispatchRequiredArgs("GetTrigger"));
    EXPECT_EQ(1, MC_DispatchRequiredArgs("GetAlias"));
    EXPECT_EQ(1, MC_DispatchRequiredArgs("GetTimer"));

    // The name is still required, though -- the exemption is for the [out]
    // slots, not a licence to call it with nothing.
    std::vector<VARIANT> outs2;
    VARIANT z; VariantInit(&z);
    EXPECT_EQ(MC_DISP_BADARGS, call(doc, "GetTrigger", {}, z, &outs2));
    MC_VariantClear(z);
    for (auto& o : outs2) MC_VariantClear(o);
}

// Extra arguments stay ignored under strict, exactly as they are without it and
// exactly as they are on Windows -- hence "expected AT LEAST n" in the message.
TEST(McDispatch, StrictOnStillIgnoresExtraArguments)
{
    CMUSHclientDoc doc;
    doc.m_bStrictScriptArguments = TRUE;
    VARIANT r; VariantInit(&r);
    ASSERT_EQ(MC_DISP_OK, call(doc, "Trim",
        { vstr("  padded  "), vstr("ignored"), vi4(99) }, r));
    EXPECT_EQ(std::string("padded"), bstr_of(r));
    MC_VariantClear(r);
}

// The introspection landmine.  Python and JavaScript coerce a bare method
// reference to a string by invoking it with NO arguments (py_engine.cpp
// Method_str, js_engine.cpp Method_convertToType), so COM-style plugins that
// write `world.GetPluginID` without parentheses keep working.  That is the
// language's coercion, not a call the author wrote, and strict must not fire on
// it.  Both directions are asserted here, because the flag's whole job is to
// make these two calls -- identical but for it -- differ.
TEST(McDispatch, StrictOnExemptsTheIntrospectionCoercion)
{
    CMUSHclientDoc doc;
    doc.m_bStrictScriptArguments = TRUE;

    VARIANT intro; VariantInit(&intro);
    EXPECT_EQ(MC_DISP_OK, MC_DispatchInvoke(&doc, "GetVariable", nullptr, 0,
                                            intro, nullptr, MC_DISP_F_INTROSPECT));
    MC_VariantClear(intro);

    VARIANT plain; VariantInit(&plain);
    EXPECT_EQ(MC_DISP_BADARGS, MC_DispatchInvoke(&doc, "GetVariable", nullptr, 0,
                                                 plain, nullptr, 0));
    MC_VariantClear(plain);

    // The flag exempts from STRICTNESS, not from the method lookup: an unknown
    // name is still unknown.
    VARIANT bogus; VariantInit(&bogus);
    EXPECT_EQ(MC_DISP_UNKNOWN, MC_DispatchInvoke(&doc, "NoSuchMethodAtAll", nullptr, 0,
                                                 bogus, nullptr, MC_DISP_F_INTROSPECT));
    MC_VariantClear(bogus);
}

// What the five engines put in front of the script author.  The count has to be
// the REQUIRED one, not the declared arity, or EnableTrigger would demand two
// arguments while happily accepting one.
TEST(McDispatch, RequiredArgumentCountsDriveTheErrorText)
{
    EXPECT_EQ(2, MC_DispatchRequiredArgs("SetVariable"));    // no defaults
    EXPECT_EQ(1, MC_DispatchRequiredArgs("EnableTrigger"));  // trailing default
    EXPECT_EQ(1, MC_DispatchRequiredArgs("Trim"));
    EXPECT_EQ(0, MC_DispatchRequiredArgs("WorldName"));      // takes none
    EXPECT_EQ(-1, MC_DispatchRequiredArgs("NoSuchMethodAtAll"));

    // Case-insensitive, like every other lookup here.
    EXPECT_EQ(2, MC_DispatchRequiredArgs("setvariable"));

    EXPECT_EQ(std::string("expected at least 2 arguments, got 1"),
              MC_DispatchBadArgsMessage("SetVariable", 1));
    EXPECT_EQ(std::string("expected at least 1 argument, got 0"),
              MC_DispatchBadArgsMessage("EnableTrigger", 0));
}

// The option is a world option like any other: it reaches the doc through the
// same SetOption the scripts use, which is what makes it save and load with the
// world file.  A member nothing could set from a script would be unusable by
// the plugin author it exists for.
TEST(McDispatch, StrictIsAWorldOptionScriptsCanSet)
{
    CMUSHclientDoc doc;
    doc.SetDefaults(false);
    EXPECT_EQ(0L, doc.GetOption("script_strict_arguments"));   // default OFF

    EXPECT_EQ((long) eOK, doc.SetOption("script_strict_arguments", 1));
    EXPECT_EQ(1L, doc.GetOption("script_strict_arguments"));
    EXPECT_TRUE(doc.m_bStrictScriptArguments);

    VARIANT r; VariantInit(&r);
    EXPECT_EQ(MC_DISP_BADARGS, call(doc, "SetVariable", { vstr("v") }, r));
    MC_VariantClear(r);

    EXPECT_EQ((long) eOK, doc.SetOption("script_strict_arguments", 0));
    EXPECT_FALSE(doc.m_bStrictScriptArguments);
    VARIANT r2; VariantInit(&r2);
    EXPECT_EQ(MC_DISP_OK, call(doc, "SetVariable", { vstr("v") }, r2));
    MC_VariantClear(r2);
}

MC_TEST_MAIN()
