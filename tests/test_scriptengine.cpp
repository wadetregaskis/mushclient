// tests/test_scriptengine.cpp
// The CScriptEngine contract, with Lua as the reference implementation.
//
// Six languages run scripts in this product and they must behave identically at
// this seam, because the engine's call sites -- triggers, aliases, timers,
// plugin callbacks, MXP -- do not know which language they are talking to.  Lua
// is the language the product was built around and the one Windows also runs, so
// its behaviour IS the specification.  This file pins that specification in the
// headless suite; platform/engines_selftest.cpp holds the other five to the same
// battery inside the app, where their interpreters are linked.
//
// So: a change here that is not mirrored there (or vice versa) is the bug.
//
// The two overloads are genuinely different contracts and both are load-bearing:
//
//   RICH   (list<double>&, list<string>&, ..., bool* result)
//          triggers/aliases/timers/plugin callbacks.  `result` is
//          "keep processing" -- false means "this handler consumed the event".
//   SIMPLE (CString strParam, ..., CString& result)
//          CPlugin::ExecutePluginScriptRtn, where ONE CString is both the
//          argument and the result -- so "the script returned nothing" must
//          leave the caller's data alone, not blank it.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting.h"
#include "scripting/errors.h"

#include <list>
#include <map>
#include <string>

// ── helpers ───────────────────────────────────────────────────────────────

namespace {

// A doc plus a Lua engine with `code` compiled into it.
struct Engine {
    CMUSHclientDoc doc;
    CScriptEngine* eng = nullptr;

    explicit Engine(const char* code = nullptr, const char* language = "Lua")
    {
        // World Properties > Scripting > "Note errors to the output window".
        // With it off, LuaError raises the Scripting Error DIALOG instead, which
        // is an inert IDCANCEL stub headless -- so a test that wants to read the
        // error text has to tick the box, exactly as a user would.
        doc.m_bScriptErrorsToOutputWindow = TRUE;
        eng = new CScriptEngine(&doc, language);
        eng->CreateScriptEngine();
        if (code) eng->Parse(CString(code), "<test>");
    }
    ~Engine() { delete eng; }

    DISPID id(const char* fn) { return eng->GetDispid(fn); }

    // The RICH overload.  Returns the engine's "was there an error" flag; the
    // keep-processing answer lands in `keepGoing`.
    bool rich(DISPID& dispid, const char* fn, bool& keepGoing,
              std::list<double> nparams = {}, std::list<std::string> sparams = {},
              std::map<std::string, std::string>* table = nullptr,
              unsigned short reason = eDontChangeAction)
    {
        long count = 0;
        keepGoing = false;
        return eng->ExecuteLua(dispid, fn, reason, "test", "test",
                               nparams, sparams, count,
                               nullptr, table, nullptr, &keepGoing);
    }

    // The SIMPLE overload, in ExecutePluginScriptRtn's shape: one CString in,
    // the same CString out.
    // `in` is a CString, not a const char*: the argument may contain NUL bytes
    // (MUD packets do) and a char* would truncate before the engine ever saw it.
    CString simple(DISPID& dispid, const char* fn, const CString& in,
                   long* countOut = nullptr, bool* errorOut = nullptr)
    {
        long count = 0;
        CString io = in;
        const bool err = eng->ExecuteLua(dispid, fn, eDontChangeAction,
                                         "test", "test", io, count, io);
        if (countOut) *countOut = count;
        if (errorOut) *errorOut = err;
        return io;
    }
};

// Everything the doc has printed, concatenated -- how a headless test sees a
// ColourNote or a script error report.
std::string all_output(CMUSHclientDoc& doc)
{
    std::string out;
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc.m_LineList.GetNext(pos);
        out.append(l->text, l->len);
        out += "\n";
    }
    return out;
}

bool saw(CMUSHclientDoc& doc, const char* needle)
{
    return all_output(doc).find(needle) != std::string::npos;
}

}   // namespace

// ── language selection ────────────────────────────────────────────────────
//
// CreateScriptEngine maps the world's language string to an engine.  Getting
// this wrong is silent: the world simply never runs a script.

TEST(ScriptEngine, LuaIsSelectedCaseInsensitively)
{
    for (const char* name : { "Lua", "lua", "LUA", "LuA" }) {
        CMUSHclientDoc doc;
        CScriptEngine e(&doc, name);
        e.CreateScriptEngine();
        EXPECT_TRUE(e.IsLua());
        EXPECT_FALSE(saw(doc, "not currently supported"));
    }
}

// Every spelling the world-properties page and existing world files use must be
// recognised.  Headless the interpreters are stubbed out, so the observable is
// the absence of the "not supported" complaint -- recognition, not activation.
TEST(ScriptEngine, EverySupportedLanguageSpellingIsRecognised)
{
    static const char* const kNames[] = {
        "Lua", "Python", "python",
        "perl", "perlscript", "PerlScript",
        "jscript", "javascript", "js", "JavaScript",
        "tcl", "tclscript", "Tcl",
        "ruby", "rubyscript", "Ruby",
    };
    for (const char* name : kNames) {
        CMUSHclientDoc doc;
        CScriptEngine e(&doc, name);
        e.CreateScriptEngine();
        EXPECT_FALSE(saw(doc, "not currently supported"));
    }
}

// VBScript is COM-only and PHP has no embeddable engine here.  Both must say so
// out loud rather than leaving the user with a world whose scripts never run.
TEST(ScriptEngine, UnsupportedLanguagesSaySo)
{
    for (const char* name : { "VBScript", "vbscript", "PHP", "php", "Brainfuck" }) {
        CMUSHclientDoc doc;
        CScriptEngine e(&doc, name);
        e.CreateScriptEngine();
        EXPECT_TRUE(saw(doc, "not currently supported"));
        EXPECT_TRUE(saw(doc, name));                 // names the language
        EXPECT_TRUE(saw(doc, "Lua"));                // and lists what does work
    }
}

// A world with scripting switched off has an empty language and must stay quiet.
TEST(ScriptEngine, NoLanguageIsNotAnError)
{
    CMUSHclientDoc doc;
    CScriptEngine e(&doc, "");
    e.CreateScriptEngine();
    EXPECT_FALSE(saw(doc, "not currently supported"));
}

// ── compiling ─────────────────────────────────────────────────────────────

TEST(ScriptEngine, ParseReportsFalseForGoodSourceAndTrueForBad)
{
    Engine ok("function f() end\n");
    EXPECT_FALSE(ok.eng->Parse(CString("x = 1\n"), "<more>"));

    Engine bad;
    EXPECT_TRUE(bad.eng->Parse(CString("function ( bad syntax\n"), "<bad>"));
    EXPECT_TRUE(saw(bad.doc, "Compile error"));   // and says so in the output window
}

// Definitions accumulate across Parse calls -- a world's script file, then each
// plugin's, then anything the user types.
TEST(ScriptEngine, ParsedDefinitionsAccumulate)
{
    Engine e("function one() end\n");
    ASSERT_FALSE(e.eng->Parse(CString("function two() end\n"), "<two>"));
    EXPECT_NE(DISPID_UNKNOWN, e.id("one"));
    EXPECT_NE(DISPID_UNKNOWN, e.id("two"));
}

// ── resolving handlers ────────────────────────────────────────────────────

TEST(ScriptEngine, GetDispidFindsDefinedFunctionsAndOnlyThose)
{
    Engine e("function present() end\nnotafunction = 42\n");
    EXPECT_NE(DISPID_UNKNOWN, e.id("present"));
    EXPECT_EQ(DISPID_UNKNOWN, e.id("absent"));
    EXPECT_EQ(DISPID_UNKNOWN, e.id(""));
    EXPECT_EQ(DISPID_UNKNOWN, e.id("notafunction"));   // exists, but not callable
}

// Handlers can live inside a table -- plugins written as modules rely on it.
TEST(ScriptEngine, GetDispidResolvesNestedNames)
{
    Engine e("mod = {}\nfunction mod.handler() end\n");
    EXPECT_NE(DISPID_UNKNOWN, e.id("mod.handler"));
    EXPECT_EQ(DISPID_UNKNOWN, e.id("mod.missing"));
    EXPECT_EQ(DISPID_UNKNOWN, e.id("nosuchmod.handler"));
}

// ── the SIMPLE overload: one CString in and out ───────────────────────────

// The headline rule.  ExecutePluginScriptRtn passes the packet/command as both
// the argument and the result, so a handler that returns nothing must leave it
// untouched -- otherwise every MUD packet arrives blank.
TEST(ScriptEngine, SimpleCallbackReturningNothingLeavesTheCallersStringAlone)
{
    Engine e("function keep(s) end\n");
    DISPID d = e.id("keep");
    ASSERT_NE(DISPID_UNKNOWN, d);
    EXPECT_EQ(CString("original text"), e.simple(d, "keep", "original text"));
}

TEST(ScriptEngine, SimpleCallbackReturningAStringReplacesIt)
{
    Engine e("function change(s) return 'replaced' end\n");
    DISPID d = e.id("change");
    EXPECT_EQ(CString("replaced"), e.simple(d, "change", "original text"));
}

TEST(ScriptEngine, SimpleCallbackSeesItsArgument)
{
    Engine e("function wrap(s) return '<' .. s .. '>' end\n");
    DISPID d = e.id("wrap");
    EXPECT_EQ(CString("<round trip>"), e.simple(d, "wrap", "round trip"));
}

// A non-string return is not a replacement: only lua_isstring counts.  (Numbers
// ARE strings to Lua, so a number does replace -- pinned so the coercion is
// deliberate rather than incidental.)
TEST(ScriptEngine, SimpleCallbackReturningANumberReplacesWithItsText)
{
    Engine e("function num(s) return 42 end\n"
             "function tab(s) return {} end\n");
    DISPID n = e.id("num");
    EXPECT_EQ(CString("42"), e.simple(n, "num", "before"));
    DISPID t = e.id("tab");
    EXPECT_EQ(CString("before"), e.simple(t, "tab", "before"));   // table: no
}

// Arbitrary bytes cross both ways.  MUD text is regularly not valid UTF-8, and
// this overload carries whole packets.
TEST(ScriptEngine, SimpleCallbackIsABytePipe)
{
    Engine e("function wrap(s) return '<' .. s .. '>' end\n"
             "function size(s) return tostring(#s) end\n");
    DISPID w = e.id("wrap");
    DISPID z = e.id("size");
    EXPECT_EQ(CString("<a\xE9z>"), e.simple(w, "wrap", "a\xE9z"));       // Latin-1
    EXPECT_EQ(CString("<a\xC3\xA9z>"), e.simple(w, "wrap", "a\xC3\xA9z"));// UTF-8
    EXPECT_EQ(CString("3"), e.simple(z, "size", "a\xE9z"));
}

// MUD packets contain NUL bytes -- telnet negotiation, MCCP, binary payloads --
// and this overload carries whole packets, so the boundary has to be
// length-explicit in both directions rather than going through a C string.
TEST(ScriptEngine, SimpleCallbackCarriesNulBytes)
{
    Engine e("function wrap(s) return '<' .. s .. '>' end\n"
             "function size(s) return tostring(#s) end\n");
    const CString nul ("a\0z", 3);
    const CString nulw("<a\0z>", 5);
    DISPID z = e.id("size");
    EXPECT_EQ(CString("3"), e.simple(z, "size", nul));
    DISPID w = e.id("wrap");
    EXPECT_EQ(nulw, e.simple(w, "wrap", nul));
}

// The other direction: a value the script hands BACK to the world API.  Every
// engine decodes incoming bytes leniently; the return leg has to be just as
// lenient, or a wildcard captured from a Latin-1 world can be read but never
// stored, sent or noted.  (Python was failing exactly this.)
TEST(ScriptEngine, ValuesPassedToTheWorldApiKeepTheirBytes)
{
    Engine e("function store(s) world.SetVariable('echoed', s) end\n");
    DISPID d = e.id("store");
    ASSERT_NE(DISPID_UNKNOWN, d);

    struct { const char* what; CString in; } cases[] = {
        { "ascii",   CString("plain") },
        { "utf-8",   CString("caf\xC3\xA9") },
        { "latin-1", CString("caf\xE9") },
    };
    for (auto& c : cases) {
        e.simple(d, "store", c.in);
        VARIANT v = e.doc.GetVariable("echoed");
        EXPECT_EQ(c.in, CString(v.bstrVal ? v.bstrVal : ""));
    }
}

// With Trace on, every callback announces itself -- it is how a user answers
// "why did my trigger not fire?".  The five embedded engines were silent here
// until the shared TraceCallback helper; this pins the reference behaviour.
TEST(ScriptEngine, TraceAnnouncesTheCallbackItIsAboutToRun)
{
    Engine e("function f() end\n");
    e.doc.m_bTrace = TRUE;
    DISPID d = e.id("f");
    bool keep = false;
    ASSERT_FALSE(e.rich(d, "f", keep));
    EXPECT_TRUE(saw(e.doc, "Executing"));
    EXPECT_TRUE(saw(e.doc, "f"));
}

// ── the RICH overload: keep-processing ────────────────────────────────────
//
// `result` answers "should the engine carry on with the next handler?".  Lua's
// rule is deliberately NOT plain truthiness: 0 is true in Lua, and a trigger
// that returns 0 means "stop", so the engine reads the value as a NUMBER unless
// it is a boolean.  Every other engine has to reproduce that.

TEST(ScriptEngine, RichCallbackReturningNothingKeepsProcessing)
{
    Engine e("function f() end\n");
    DISPID d = e.id("f");
    bool keep = false;
    EXPECT_FALSE(e.rich(d, "f", keep));
    EXPECT_TRUE(keep);
}

TEST(ScriptEngine, RichCallbackBooleanResultIsUsedDirectly)
{
    Engine e("function t() return true end\nfunction f() return false end\n");
    bool keep = false;
    DISPID t = e.id("t");
    ASSERT_FALSE(e.rich(t, "t", keep));
    EXPECT_TRUE(keep);
    DISPID f = e.id("f");
    ASSERT_FALSE(e.rich(f, "f", keep));
    EXPECT_FALSE(keep);
}

// The rule that surprises people: returning 0 stops processing even though 0 is
// truthy in Lua.  Returning nil likewise stops -- it is not "returned nothing".
TEST(ScriptEngine, RichCallbackNumericResultIsReadAsANumberNotAsTruthiness)
{
    Engine e("function zero() return 0 end\n"
             "function one() return 1 end\n"
             "function nilret() return nil end\n");
    bool keep = true;
    DISPID z = e.id("zero");
    ASSERT_FALSE(e.rich(z, "zero", keep));
    EXPECT_FALSE(keep);                       // 0 -> stop, despite Lua truthiness

    DISPID o = e.id("one");
    ASSERT_FALSE(e.rich(o, "one", keep));
    EXPECT_TRUE(keep);

    DISPID n = e.id("nilret");
    ASSERT_FALSE(e.rich(n, "nilret", keep));
    EXPECT_FALSE(keep);                       // returning nil is NOT returning nothing
}

// Numeric and string parameters arrive as arguments, numbers first.  This is the
// shape triggers, aliases and timers all use.
TEST(ScriptEngine, RichCallbackReceivesNumericThenStringParameters)
{
    Engine e("function got(...)\n"
             "  local a = {...}\n"
             "  world.SetVariable('n', tostring(#a))\n"
             "  for i, v in ipairs(a) do world.SetVariable('a' .. i, tostring(v)) end\n"
             "end\n");
    DISPID d = e.id("got");
    ASSERT_NE(DISPID_UNKNOWN, d);
    bool keep = false;
    ASSERT_FALSE(e.rich(d, "got", keep, { 7, 8 }, { "first", "second" }));

    VARIANT v;
    v = e.doc.GetVariable("n");   EXPECT_EQ(CString("4"), CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("a1");  EXPECT_EQ(CString("7"), CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("a2");  EXPECT_EQ(CString("8"), CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("a3");  EXPECT_EQ(CString("first"),  CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("a4");  EXPECT_EQ(CString("second"), CString(v.bstrVal ? v.bstrVal : ""));
}

// The optional `table` (named wildcards, plugin callback details) arrives as an
// extra trailing argument -- only when it is non-null.
TEST(ScriptEngine, RichCallbackReceivesTheOptionalTableOnlyWhenGiven)
{
    Engine e("function count(...)\n"
             "  world.SetVariable('n', tostring(select('#', ...)))\n"
             "  local last = select(select('#', ...), ...)\n"
             "  if type(last) == 'table' then world.SetVariable('k', last.key or '?') end\n"
             "end\n");
    DISPID d = e.id("count");
    bool keep = false;

    ASSERT_FALSE(e.rich(d, "count", keep, {}, { "one" }));
    VARIANT v = e.doc.GetVariable("n");
    EXPECT_EQ(CString("1"), CString(v.bstrVal ? v.bstrVal : ""));

    std::map<std::string, std::string> t{ { "key", "value" } };
    ASSERT_FALSE(e.rich(d, "count", keep, {}, { "one" }, &t));
    v = e.doc.GetVariable("n");
    EXPECT_EQ(CString("2"), CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("k");
    EXPECT_EQ(CString("value"), CString(v.bstrVal ? v.bstrVal : ""));
}

// ── errors ────────────────────────────────────────────────────────────────
//
// A handler that throws must be reported AND disabled: dispid becomes
// DISPID_UNKNOWN so the engine stops calling it.  Without that, a broken trigger
// spams an error for every line the MUD sends.

TEST(ScriptEngine, ARuntimeErrorReportsAndDisablesTheHandler)
{
    Engine e("function boom() error('deliberate') end\n");
    DISPID d = e.id("boom");
    ASSERT_NE(DISPID_UNKNOWN, d);

    bool keep = false;
    EXPECT_TRUE(e.rich(d, "boom", keep));         // true == error
    EXPECT_EQ(DISPID_UNKNOWN, d);                 // and it will not be called again
    EXPECT_TRUE(saw(e.doc, "deliberate"));
}

TEST(ScriptEngine, ADisabledHandlerIsNotCalledAgain)
{
    Engine e("calls = 0\nfunction counted() calls = calls + 1 end\n");
    DISPID d = e.id("counted");
    bool keep = false;
    ASSERT_FALSE(e.rich(d, "counted", keep));

    d = DISPID_UNKNOWN;                            // as an earlier error would leave it
    EXPECT_FALSE(e.rich(d, "counted", keep));      // no error reported...
    // ...and the function really did not run: calls is still 1.
    ASSERT_FALSE(e.eng->Parse(CString("world.SetVariable('calls', tostring(calls))\n"), "<read>"));
    VARIANT v = e.doc.GetVariable("calls");
    EXPECT_EQ(CString("1"), CString(v.bstrVal ? v.bstrVal : ""));
}

// The same on the simple overload, where "did not run" must not blank the
// caller's string either.
TEST(ScriptEngine, ASimpleCallbackErrorLeavesTheCallersStringAlone)
{
    Engine e("function boom(s) error('bang') end\n");
    DISPID d = e.id("boom");
    bool err = false;
    EXPECT_EQ(CString("untouched"), e.simple(d, "boom", "untouched", nullptr, &err));
    EXPECT_TRUE(err);
    EXPECT_EQ(DISPID_UNKNOWN, d);
}

// ── invocation counting ───────────────────────────────────────────────────
//
// nInvocationCount is what the trigger/alias/timer editors display as "N calls".

TEST(ScriptEngine, SuccessfulCallsAreCountedAndFailedOnesAreNot)
{
    Engine e("function ok(s) end\nfunction boom(s) error('bang') end\n");

    DISPID good = e.id("ok");
    long count = 0;
    e.simple(good, "ok", "x", &count);
    EXPECT_EQ(1L, count);
    e.simple(good, "ok", "x", &count);
    EXPECT_EQ(1L, count);                  // the helper starts each call at 0

    DISPID bad = e.id("boom");
    long badCount = 7;
    e.simple(bad, "boom", "x", &badCount);
    EXPECT_EQ(0L, badCount);               // never incremented on the error path
}

// ── action source ─────────────────────────────────────────────────────────
//
// m_iCurrentActionSource tells world.GetInfo(239)-style queries (and the
// send-suppression logic) why the engine is running a script right now.  It must
// be set for the duration and restored afterwards -- and eDontChangeAction means
// "leave whatever the caller set".

TEST(ScriptEngine, ActionSourceIsSetDuringTheCallAndClearedAfter)
{
    Engine e("function probe() world.SetVariable('src', tostring(world.GetInfo(239))) end\n");
    DISPID d = e.id("probe");
    ASSERT_NE(DISPID_UNKNOWN, d);

    bool keep = false;
    e.doc.m_iCurrentActionSource = eUnknownActionSource;
    ASSERT_FALSE(e.rich(d, "probe", keep, {}, {}, nullptr, eTriggerFired));

    VARIANT v = e.doc.GetVariable("src");
    EXPECT_EQ(CString("6"), CString(v.bstrVal ? v.bstrVal : ""));   // eTriggerFired
    EXPECT_EQ((int) eUnknownActionSource, (int) e.doc.m_iCurrentActionSource);
}

TEST(ScriptEngine, DontChangeActionLeavesTheSourceAlone)
{
    Engine e("function probe() world.SetVariable('src', tostring(world.GetInfo(239))) end\n");
    DISPID d = e.id("probe");
    bool keep = false;
    e.doc.m_iCurrentActionSource = eInputFromServer;
    ASSERT_FALSE(e.rich(d, "probe", keep, {}, {}, nullptr, eDontChangeAction));

    VARIANT v = e.doc.GetVariable("src");
    EXPECT_EQ(CString("8"), CString(v.bstrVal ? v.bstrVal : ""));   // eInputFromServer
    EXPECT_EQ((int) eInputFromServer, (int) e.doc.m_iCurrentActionSource);
}

// ── CScriptEngine::Execute (the DISPPARAMS entry) ─────────────────────────
//
// The generic entry point, used by CallPlugin into a non-Lua plugin and by every
// MXP callback.  On Windows it hands rgvarg straight to IDispatch::Invoke; the
// port has to unpack it.  MFC/COM packs rgvarg in REVERSE order -- the callers
// say so in their own comments -- so the LAST array slot is the FIRST argument.
//
// It used to ignore its DISPPARAMS entirely, so every argument arrived as nil.

TEST(ScriptEngine, ExecuteUnpacksDispparamsInDeclarationOrder)
{
    Engine e("function got(...)\n"
             "  local a = {...}\n"
             "  world.SetVariable('n', tostring(#a))\n"
             "  for i, v in ipairs(a) do world.SetVariable('a' .. i, tostring(v)) end\n"
             "end\n");
    DISPID d = e.id("got");
    ASSERT_NE(DISPID_UNKNOWN, d);

    // The call is got("first", "second"): COM packs it reversed.
    COleVariant args[2];
    args[0] = "second";
    args[1] = "first";
    DISPPARAMS params = { args, nullptr, 2, 0 };
    long count = 0;
    ASSERT_FALSE(e.eng->Execute(d, "got", eDontChangeAction, "test", "test",
                                params, count, nullptr));
    EXPECT_EQ(1L, count);

    VARIANT v;
    v = e.doc.GetVariable("n");  EXPECT_EQ(CString("2"), CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("a1"); EXPECT_EQ(CString("first"),  CString(v.bstrVal ? v.bstrVal : ""));
    v = e.doc.GetVariable("a2"); EXPECT_EQ(CString("second"), CString(v.bstrVal ? v.bstrVal : ""));
}

// Numbers survive the same trip, as their own type rather than as text.
TEST(ScriptEngine, ExecuteMarshalsNumericArguments)
{
    Engine e("function got(...)\n"
             "  local a = {...}\n"
             "  world.SetVariable('t', type(a[1]) .. ':' .. tostring(a[1]))\n"
             "  world.SetVariable('n', tostring(#a))\n"
             "end\n");
    DISPID d = e.id("got");

    COleVariant args[1];
    args[0] = (long) 42;
    DISPPARAMS params = { args, nullptr, 1, 0 };
    long count = 0;
    ASSERT_FALSE(e.eng->Execute(d, "got", eDontChangeAction, "test", "test",
                                params, count, nullptr));

    VARIANT v = e.doc.GetVariable("t");
    EXPECT_EQ(CString("number:42"), CString(v.bstrVal ? v.bstrVal : ""));
}

// Execute reports the handler's keep-processing answer through its COleVariant
// out-parameter -- that is how the MXP callbacks decide whether to suppress.
TEST(ScriptEngine, ExecuteReportsTheKeepProcessingResult)
{
    Engine e("function stop() return false end\nfunction go() return true end\n");
    DISPPARAMS none = { nullptr, nullptr, 0, 0 };
    long count = 0;

    COleVariant r1;
    DISPID s = e.id("stop");
    ASSERT_FALSE(e.eng->Execute(s, "stop", eDontChangeAction, "t", "t", none, count, &r1));
    EXPECT_EQ((int) VT_BOOL, (int) r1.vt);
    EXPECT_TRUE(r1.boolVal == 0);

    COleVariant r2;
    DISPID g = e.id("go");
    ASSERT_FALSE(e.eng->Execute(g, "go", eDontChangeAction, "t", "t", none, count, &r2));
    EXPECT_TRUE(r2.boolVal != 0);
}

// ── isolation ─────────────────────────────────────────────────────────────
//
// Every plugin gets its own engine, and they share a process.  A global one
// plugin defines must not be visible to another -- plugins are written by
// different people and collide on names like "state" constantly.

TEST(ScriptEngine, TwoEnginesDoNotShareGlobals)
{
    Engine a("shared = 'from A'\nfunction read() world.SetVariable('v', tostring(shared)) end\n");
    Engine b("function read() world.SetVariable('v', tostring(shared)) end\n");

    DISPID da = a.id("read");
    bool keep = false;
    ASSERT_FALSE(a.rich(da, "read", keep));
    VARIANT v = a.doc.GetVariable("v");
    EXPECT_EQ(CString("from A"), CString(v.bstrVal ? v.bstrVal : ""));

    DISPID db = b.id("read");
    ASSERT_FALSE(b.rich(db, "read", keep));
    v = b.doc.GetVariable("v");
    EXPECT_EQ(CString("nil"), CString(v.bstrVal ? v.bstrVal : ""));
}

// State inside one engine persists between calls -- plugins keep their world
// model in globals across thousands of trigger fires.
TEST(ScriptEngine, StatePersistsBetweenCallsInOneEngine)
{
    Engine e("n = 0\nfunction bump() n = n + 1 world.SetVariable('n', tostring(n)) end\n");
    DISPID d = e.id("bump");
    bool keep = false;
    for (int i = 0; i < 3; i++) ASSERT_FALSE(e.rich(d, "bump", keep));

    VARIANT v = e.doc.GetVariable("n");
    EXPECT_EQ(CString("3"), CString(v.bstrVal ? v.bstrVal : ""));
}

// The world object a script sees is ITS OWN world, not some other one.
TEST(ScriptEngine, EachEngineTalksToItsOwnWorld)
{
    Engine a("function tag() world.SetVariable('who', world.GetWorldID()) end\n");
    Engine b("function tag() world.SetVariable('who', world.GetWorldID()) end\n");
    a.doc.m_strWorldID = "aaaaaaaaaaaaaaaaaaaaaaaa";
    b.doc.m_strWorldID = "bbbbbbbbbbbbbbbbbbbbbbbb";

    bool keep = false;
    DISPID da = a.id("tag"); ASSERT_FALSE(a.rich(da, "tag", keep));
    DISPID db = b.id("tag"); ASSERT_FALSE(b.rich(db, "tag", keep));

    VARIANT v = a.doc.GetVariable("who");
    EXPECT_EQ(CString("aaaaaaaaaaaaaaaaaaaaaaaa"), CString(v.bstrVal ? v.bstrVal : ""));
    v = b.doc.GetVariable("who");
    EXPECT_EQ(CString("bbbbbbbbbbbbbbbbbbbbbbbb"), CString(v.bstrVal ? v.bstrVal : ""));
}

// ── the world surface the engine actually builds ──────────────────────────
//
// RegisterPortableLuaExtras (scripting/lua_utils_portable.cpp) adds one name
// the Windows Lua API does not have, world.ExpandVariables.  No engine-built
// state has ever carried it here: the OpenLuaDelayed call that registered it
// was guarded on !MUSHCLIENT_FULL_BUILD, which the Stage 9 fold made
// permanently false, and it has now been removed.  Only tests/lua_test_helpers.h
// registers it, so every make_lua()-based test sees a surface the product does
// not have -- this one goes through CreateScriptEngine, the way a world does.
//
// It is therefore the test that would notice if the extra were ever wired into
// the product, which is the open question (K-231), not an accident to fix here.
TEST(ScriptEngine, TheEngineBuiltWorldTableCarriesNoPortOnlyExtras)
{
    Engine e("function probe(s)\n"
             "  return tostring (world.ExpandVariables) .. '/' ..\n"
             "         tostring (world.Note ~= nil)\n"
             "end\n");
    DISPID d = e.id("probe");
    ASSERT_NE(DISPID_UNKNOWN, d);
    // The second half is the control: the world table IS populated, so the
    // "nil" is a statement about that one name, not about an empty table.
    EXPECT_EQ(CString("nil/true"), e.simple(d, "probe", ""));
}

MC_TEST_MAIN()
