// platform/engines_selftest.cpp — in-app conformance suite for the six scripting
// languages (--engines-selftest[=<language>]).
//
// WHY IT LIVES HERE.  Five of the six embed a whole interpreter (libpython,
// libperl, JavaScriptCore, libtcl, libruby) and those must not be linked into
// the 40 headless test executables -- so the engines cannot be tested the usual
// way.  The PAL selftests established the alternative: a hidden flag that runs a
// battery inside the real app binary and exits before the GUI boots.  ctest
// registers one entry per language, and a language whose interpreter was not
// found at configure time reports SKIPPED (exit 77) rather than passing
// vacuously -- scripting support is expected but not required.
//
// WHAT IT ASSERTS.  The same contract tests/test_scriptengine.cpp pins for Lua
// in the headless suite, applied to all six.  Lua is the control: it is the
// language the product was built around and the one Windows also runs, so its
// behaviour IS the specification, and it runs here too.  A rule that holds in
// test_scriptengine.cpp but not here (or vice versa) is a bug in one of them.
//
// HOW THE DIALECTS WORK.  Each language supplies source defining the same set of
// named handlers; the battery is written once against those names.  Where a
// language genuinely cannot express something the dialect says so and the
// battery skips that one check for that language, printing why -- never silently.
//
// Writing this suite is what found, so far: the Tcl blank-packet bug; byte loss
// in JavaScript and Python (both directions, both engines); the Perl
// blessed-null bug; Ruby's zero-is-truthy divergence; NUL truncation in
// JavaScript, Tcl and Ruby; a non-string return overwriting the caller's packet
// in Python, Perl, JavaScript and Ruby; the missing "Executing ... script" Trace
// line in all five; and Tcl and Ruby answering "yes, that callback exists" for
// every built-in command / inherited Object method.

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting.h"
#include "scripting/errors.h"
#include "childfrm.h"
#include "MUSHview.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <string>

namespace {

int g_pass = 0, g_fail = 0, g_skip = 0;

void check(bool ok, const char* lang, const char* what)
{
    printf("  [%s] %-7s %s\n", ok ? "PASS" : "FAIL", lang, what);
    (ok ? g_pass : g_fail)++;
}

void skip(const char* lang, const char* what, const char* why)
{
    printf("  [SKIP] %-7s %s  (%s)\n", lang, what, why);
    g_skip++;
}

// ── the engine under test ─────────────────────────────────────────────────

struct Engine {
    CMUSHclientDoc* doc = nullptr;
    CScriptEngine*  eng = nullptr;
    bool            ok  = false;

    Engine(const char* language, const char* code)
    {
        doc = new CMUSHclientDoc();
        // Route script errors to the output window; otherwise LuaError and the
        // engines' report_error raise the Scripting Error dialog, which is inert
        // here.  This is the same box a user ticks in World Properties.
        doc->m_bScriptErrorsToOutputWindow = TRUE;
        // The GUI build seeds the first output line in SetUpOutputWindow, which
        // the framework calls at view creation -- and no view is created here.
        // Without it m_pCurrentLine is null and world.Note has nowhere to land,
        // so any assertion about the output window would be vacuous.
        doc->SetUpOutputWindow();
        eng = new CScriptEngine(doc, language);
        eng->CreateScriptEngine();
        if (!eng->IsLua()  && !eng->IsPython() && !eng->IsPerl() &&
            !eng->IsJS()   && !eng->IsTcl()    && !eng->IsRuby())
            return;                          // interpreter not built in
        ok = !eng->Parse(CString(code), "selftest");
    }
    ~Engine() { delete eng; delete doc; }

    DISPID id(const char* fn) { return eng->GetDispid(fn); }

    // The SIMPLE overload, in CPlugin::ExecutePluginScriptRtn's shape: one
    // CString is both the argument and the result.
    CString callRtn(const char* proc, const CString& in, bool* errOut = nullptr)
    {
        DISPID d = id(proc);
        long   count = 0;
        CString io = in;
        const bool err = eng->ExecuteLua(d, proc, eDontChangeAction,
                                         "selftest", "selftest", io, count, io);
        if (errOut) *errOut = err;
        return io;
    }

    // The RICH overload.  Returns "keep processing"; *errOut gets the error flag.
    bool callRich(const char* proc, bool* errOut = nullptr,
                  std::list<double> nparams = {},
                  std::list<std::string> sparams = {},
                  std::map<std::string, std::string>* table = nullptr,
                  unsigned short reason = eDontChangeAction)
    {
        DISPID d = id(proc);
        long   count = 0;
        bool   keep  = false;
        const bool err = eng->ExecuteLua(d, proc, reason, "selftest", "selftest",
                                         nparams, sparams, count,
                                         nullptr, table, nullptr, &keep);
        if (errOut) *errOut = err;
        return keep;
    }

    // What the script left in a world variable.
    std::string var(const char* name)
    {
        VARIANT v = doc->GetVariable(name);
        return (v.vt == VT_BSTR && v.bstrVal) ? std::string(v.bstrVal) : std::string();
    }

    std::string output() const
    {
        std::string out;
        for (POSITION pos = doc->m_LineList.GetHeadPosition(); pos; ) {
            CLine* l = doc->m_LineList.GetNext(pos);
            out.append(l->text, l->len);
            out += "\n";
        }
        return out;
    }
    // The doc's variables, minus the ones this harness itself wrote, so a count
    // assertion does not depend on which batteries ran first.
    long variableCount() const { return (long) doc->m_VariableMap.GetCount(); }
    bool saw(const char* needle) { return output().find(needle) != std::string::npos; }
};

// ── the dialects ──────────────────────────────────────────────────────────
//
// Every dialect defines the same handlers:
//
//   simple overload (one string in, one string out)
//     keep(s)     returns nothing at all
//     change(s)   returns the fixed string "replaced"
//     wrap(s)     returns "<" + s + ">"      (NOT a plain echo: see below)
//     size(s)     returns the argument's BYTE length, as a string
//     boom(s)     raises a runtime error
//
//   rich overload (keep-processing)
//     rnothing()  returns nothing
//     rtrue()     returns the language's true
//     rfalse()    returns the language's false
//     rzero()     returns numeric 0
//     rone()      returns numeric 1
//     rnil()      returns the language's nil/None/undef
//     params(a,b,c,d)  writes "a|b|c|d" (ints) into world variable "p"
//     tbl(s, t)   writes t["key"] into world variable "t"
//     bump()      increments a global and writes it into "n"
//     shared()    writes the global `shared` (or its absence) into "v"
//     who()       writes world.GetWorldID() into "w"
//     api()       exercises the world API: a Note, a variable round trip, an
//                 error code, and the length of an array-returning call
//
// wrap rather than echo is deliberate: an engine that fails to convert its
// result leaves the caller's string untouched, which an echo test cannot tell
// from success.  Both engine byte-loss bugs hid behind exactly that shape.

struct Dialect {
    const char* key;        // command-line name: --engines-selftest=tcl
    const char* display;
    const char* language;   // what CScriptEngine::CreateScriptEngine wants
    const char* source;
    const char* sourceB;    // a SECOND engine, for the isolation check
    const char* badSource;  // source that must fail to compile
    bool        zeroStops;  // does returning numeric 0 stop processing?
    bool        nilStops;   // can the engine tell "returned nil" from "returned nothing"?
    const char* builtin;    // a name the LANGUAGE defines but the script does not:
                            // GetDispid must not claim to have found a handler
    bool        hidesBuiltins;  // ...can this engine tell them apart at all?
    bool        typedResult;    // ...can it tell a string result from a structure?
};

const Dialect kDialects[] = {

// ── Lua — the control ─────────────────────────────────────────────────────
{ "lua", "Lua", "Lua",
  "function keep(s) end\n"
  "function change(s) return 'replaced' end\n"
  "function wrap(s) return '<' .. s .. '>' end\n"
  "function size(s) return tostring(#s) end\n"
  "function boom(s) error('deliberate') end\n"
  "function rnothing() end\n"
  "function rtrue() return true end\n"
  "function rfalse() return false end\n"
  "function rzero() return 0 end\n"
  "function rone() return 1 end\n"
  "function rnil() return nil end\n"
  "function params(a, b, c, d)\n"
  "  world.SetVariable('p', string.format('%d|%d|%s|%s', a, b, c, d)) end\n"
  "function tbl(s, t) world.SetVariable('t', t.key or '?') end\n"
  "n = 0\n"
  "function bump() n = n + 1 world.SetVariable('n', tostring(n)) end\n"
  "shared = 'from A'\n"
  "function readshared() world.SetVariable('v', tostring(shared)) end\n"
  "function who() world.SetVariable('w', world.GetWorldID()) end\n"
  "function store(s) world.SetVariable('echoed', s) end\n"
  "function rettable(s) return {} end\n"
  "function api()\n"
  "  world.Note('a note')\n"
  "  world.SetVariable('rt', 'value')\n"
  "  world.SetVariable('got', world.GetVariable('rt') or 'nil')\n"
  "  world.SetVariable('err', tostring(world.SetVariable('', 'x')))\n"
  "  local l = world.GetVariableList()\n"
  "  local c = 0\n"
  "  for _ in pairs(l) do c = c + 1 end\n"
  "  world.SetVariable('cnt', tostring(c))\n"
  "end\n",
  "function readshared() world.SetVariable('v', tostring(shared)) end\n"
  "function who() world.SetVariable('w', world.GetWorldID()) end\n",
  "function ( this is not lua\n",
  true, true, "Note", false, true },

// ── Python ────────────────────────────────────────────────────────────────
{ "python", "Python", "Python",
  "def keep(s):\n    pass\n"
  "def change(s):\n    return 'replaced'\n"
  "def wrap(s):\n    return '<' + s + '>'\n"
  "def size(s):\n    return str(len(s.encode('utf-8', 'surrogateescape')))\n"
  "def boom(s):\n    raise RuntimeError('deliberate')\n"
  "def rnothing():\n    pass\n"
  "def rtrue():\n    return True\n"
  "def rfalse():\n    return False\n"
  "def rzero():\n    return 0\n"
  "def rone():\n    return 1\n"
  "def rnil():\n    return None\n"
  "def params(a, b, c, d):\n"
  "    world.SetVariable('p', '%d|%d|%s|%s' % (int(a), int(b), c, d))\n"
  "def tbl(s, t):\n    world.SetVariable('t', t.get('key', '?'))\n"
  "n = 0\n"
  "def bump():\n"
  "    global n\n    n = n + 1\n    world.SetVariable('n', str(n))\n"
  "shared = 'from A'\n"
  "def readshared():\n"
  "    world.SetVariable('v', str(globals().get('shared')).lower() if 'shared' not in globals() else shared)\n"
  "def who():\n    world.SetVariable('w', world.GetWorldID())\n"
  "def store(s):\n"
  "    world.SetVariable('echoed', s)\n"
  "def rettable(s):\n"
  "    return {}\n"
  "def api():\n"
  "    world.Note('a note')\n"
  "    world.SetVariable('rt', 'value')\n"
  "    world.SetVariable('got', world.GetVariable('rt') or 'nil')\n"
  "    world.SetVariable('err', str(world.SetVariable('', 'x')))\n"
  "    l = world.GetVariableList()\n"
  "    world.SetVariable('cnt', str(len(l) if l else 0))\n",
  "def readshared():\n"
  "    world.SetVariable('v', shared if 'shared' in globals() else 'nil')\n"
  "def who():\n    world.SetVariable('w', world.GetWorldID())\n",
  "def broken(:\n",
  true, false, "print", true, true },

// ── Perl ──────────────────────────────────────────────────────────────────
{ "perl", "Perl", "perl",
  "sub keep { return; }\n"
  "sub change { return 'replaced'; }\n"
  "sub wrap { return '<' . $_[0] . '>'; }\n"
  "sub size { return length($_[0]); }\n"
  "sub boom { die 'deliberate'; }\n"
  "sub rnothing { return; }\n"
  "sub rtrue { return 1; }\n"
  "sub rfalse { return 0; }\n"
  "sub rzero { return 0; }\n"
  "sub rone { return 1; }\n"
  "sub rnil { return undef; }\n"
  "sub params { $world->SetVariable('p', sprintf('%d|%d|%s|%s', @_)); }\n"
  "sub tbl { my ($s, $t) = @_; $world->SetVariable('t', $t->{key} || '?'); }\n"
  "our $n = 0;\n"
  "sub bump { $n = $n + 1; $world->SetVariable('n', \"$n\"); }\n"
  "our $shared = 'from A';\n"
  "sub readshared { $world->SetVariable('v', defined($shared) ? $shared : 'nil'); }\n"
  "sub who { $world->SetVariable('w', $world->GetWorldID()); }\n"
  "sub store { $world->SetVariable('echoed', $_[0]); }\n"
  "sub rettable { return {}; }\n"
  "sub api {\n"
  "  $world->Note('a note');\n"
  "  $world->SetVariable('rt', 'value');\n"
  "  $world->SetVariable('got', $world->GetVariable('rt') || 'nil');\n"
  "  $world->SetVariable('err', '' . $world->SetVariable('', 'x'));\n"
  "  my $l = $world->GetVariableList();\n"
  "  $world->SetVariable('cnt', '' . (ref($l) eq 'ARRAY' ? scalar(@$l) : 0));\n"
  "}\n",
  "sub readshared { $world->SetVariable('v', defined($shared) ? $shared : 'nil'); }\n"
  "sub who { $world->SetVariable('w', $world->GetWorldID()); }\n",
  "sub broken { $x = ; }\n",
  true, false, "printf", true, true },

// ── JavaScript ────────────────────────────────────────────────────────────
{ "js", "JS", "javascript",
  "function keep(s) { }\n"
  "function change(s) { return 'replaced'; }\n"
  "function wrap(s) { return '<' + s + '>'; }\n"
  "function size(s) { return String(s.length); }\n"
  "function boom(s) { throw new Error('deliberate'); }\n"
  "function rnothing() { }\n"
  "function rtrue() { return true; }\n"
  "function rfalse() { return false; }\n"
  "function rzero() { return 0; }\n"
  "function rone() { return 1; }\n"
  "function rnil() { return null; }\n"
  "function params(a, b, c, d) {\n"
  "  world.SetVariable('p', [Math.round(a), Math.round(b), c, d].join('|')); }\n"
  "function tbl(s, t) { world.SetVariable('t', t.key || '?'); }\n"
  "var n = 0;\n"
  "function bump() { n = n + 1; world.SetVariable('n', String(n)); }\n"
  "var shared = 'from A';\n"
  "function readshared() {\n"
  "  world.SetVariable('v', typeof shared === 'undefined' ? 'nil' : shared); }\n"
  "function who() { world.SetVariable('w', world.GetWorldID()); }\n"
  "function store(s) { world.SetVariable('echoed', s); }\n"
  "function rettable(s) { return {}; }\n"
  "function api() {\n"
  "  world.Note('a note');\n"
  "  world.SetVariable('rt', 'value');\n"
  "  world.SetVariable('got', world.GetVariable('rt') || 'nil');\n"
  "  world.SetVariable('err', String(world.SetVariable('', 'x')));\n"
  "  var l = world.GetVariableList();\n"
  "  world.SetVariable('cnt', String(l ? l.length : 0));\n"
  "}\n",
  "function readshared() {\n"
  "  world.SetVariable('v', typeof shared === 'undefined' ? 'nil' : shared); }\n"
  "function who() { world.SetVariable('w', world.GetWorldID()); }\n",
  "function broken( {\n",
  true, false, "parseInt", false, true },

// ── Tcl ───────────────────────────────────────────────────────────────────
{ "tcl", "Tcl", "tcl",
  "proc keep {s} { }\n"
  "proc change {s} { return replaced }\n"
  "proc wrap {s} { return \"<$s>\" }\n"
  "proc size {s} { return [string length $s] }\n"
  "proc boom {s} { error deliberate }\n"
  "proc rnothing {} { }\n"
  "proc rtrue {} { return 1 }\n"
  "proc rfalse {} { return 0 }\n"
  "proc rzero {} { return 0 }\n"
  "proc rone {} { return 1 }\n"
  "proc rnil {} { return {} }\n"
  "proc params {a b c d} {\n"
  "  world SetVariable p [format \"%d|%d|%s|%s\" [expr {int($a)}] [expr {int($b)}] $c $d] }\n"
  "proc tbl {s t} {\n"
  "  if {[dict exists $t key]} { world SetVariable t [dict get $t key] } \\\n"
  "  else { world SetVariable t ? } }\n"
  "set n 0\n"
  "proc bump {} { global n; incr n; world SetVariable n $n }\n"
  "set shared {from A}\n"
  "proc readshared {} {\n"
  "  global shared\n"
  "  if {[info exists shared]} { world SetVariable v $shared } \\\n"
  "  else { world SetVariable v nil } }\n"
  "proc who {} { world SetVariable w [world GetWorldID] }\n"
  "proc store {s} { world SetVariable echoed $s }\n"
  "proc rettable {s} { return [dict create a 1] }\n"
  "proc api {} {\n"
  "  world Note {a note}\n"
  "  world SetVariable rt value\n"
  "  world SetVariable got [world GetVariable rt]\n"
  "  world SetVariable err [world SetVariable {} x]\n"
  "  set l [world GetVariableList]\n"
  "  world SetVariable cnt [llength $l]\n"
  "}\n",
  "proc readshared {} {\n"
  "  global shared\n"
  "  if {[info exists shared]} { world SetVariable v $shared } \\\n"
  "  else { world SetVariable v nil } }\n"
  "proc who {} { world SetVariable w [world GetWorldID] }\n",
  "proc broken {  { \n",
  true, false, "puts", true, false },

// ── Ruby ──────────────────────────────────────────────────────────────────
{ "ruby", "Ruby", "ruby",
  "def keep(s); nil; end\n"
  "def change(s); 'replaced'; end\n"
  "def wrap(s); '<' + s + '>'; end\n"
  "def size(s); s.bytesize.to_s; end\n"
  "def boom(s); raise 'deliberate'; end\n"
  "def rnothing; nil; end\n"
  "def rtrue; true; end\n"
  "def rfalse; false; end\n"
  "def rzero; 0; end\n"
  "def rone; 1; end\n"
  "def rnil; nil; end\n"
  "def params(a, b, c, d)\n"
  "  world.SetVariable('p', '%d|%d|%s|%s' % [a.to_i, b.to_i, c, d])\n"
  "end\n"
  "def tbl(s, t); world.SetVariable('t', t['key'] || '?'); end\n"
  "@n = 0\n"
  "def bump; @n = (@n || 0) + 1; world.SetVariable('n', @n.to_s); end\n"
  "@shared = 'from A'\n"
  "def readshared; world.SetVariable('v', @shared || 'nil'); end\n"
  "def who; world.SetVariable('w', world.GetWorldID()); end\n"
  "def store(s); world.SetVariable('echoed', s); end\n"
  "def rettable(s); {}; end\n"
  "def api\n"
  "  world.Note('a note')\n"
  "  world.SetVariable('rt', 'value')\n"
  "  world.SetVariable('got', world.GetVariable('rt') || 'nil')\n"
  "  world.SetVariable('err', world.SetVariable('', 'x').to_s)\n"
  "  l = world.GetVariableList()\n"
  "  world.SetVariable('cnt', (l ? l.length : 0).to_s)\n"
  "end\n",
  "def readshared; world.SetVariable('v', @shared || 'nil'); end\n"
  "def who; world.SetVariable('w', world.GetWorldID()); end\n",
  "def broken(\n",
  true, false, "inspect", true, true },
};

const int kDialectCount = (int) (sizeof kDialects / sizeof kDialects[0]);

// ── the battery ───────────────────────────────────────────────────────────

// 1. The SIMPLE overload: one CString is both argument and result.
void battery_simple(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    check(e.callRtn("keep", "original text") == "original text", L,
          "returning nothing leaves the caller's string alone");
    check(e.callRtn("change", "original text") == "replaced", L,
          "returning a string replaces the caller's string");
    check(e.callRtn("wrap", "round trip") == "<round trip>", L,
          "the argument reaches the script and the result comes back");

    // A Latin-1 byte (0xE9) is not valid UTF-8 on its own; MUD output is full of
    // them.  Both directions must be byte-transparent.
    CString latin1;  latin1  = "a\xE9z";
    CString wrapped; wrapped = "<a\xE9z>";
    check(e.callRtn("size", latin1) == "3", L,
          "a non-UTF-8 argument arrives with all its bytes");
    check(e.callRtn("wrap", latin1) == wrapped, L,
          "a non-UTF-8 argument survives the round trip byte for byte");

    CString utf8;     utf8     = "a\xC3\xA9z";
    CString utf8wrap; utf8wrap = "<a\xC3\xA9z>";
    check(e.callRtn("wrap", utf8) == utf8wrap, L,
          "a valid UTF-8 argument survives the round trip byte for byte");

    // Only a STRING result replaces the caller's data.  Lua checks lua_isstring,
    // so a returned table leaves the packet alone; an engine that stringifies
    // anything non-null turns a handler's stray return value into MUD output.
    // Tcl has one type.  A dict IS its string representation, so there is nothing
    // to distinguish -- the others all can, and must.
    if (d.typedResult)
        check(e.callRtn("rettable", "untouched") == "untouched", L,
              "returning a table/object/dict does NOT replace the caller's string");
    else
        skip(L, "returning a table/object/dict does NOT replace the caller's string",
             "every value in this language IS a string");

    // MUD packets contain NUL bytes (telnet negotiation, MCCP, binary data).  A
    // conversion that goes through a C string truncates there.
    const CString nul ("a\0z", 3);
    const CString nulw("<a\0z>", 5);
    check(e.callRtn("size", nul) == "3", L,
          "an argument containing a NUL byte arrives at full length");
    check(e.callRtn("wrap", nul) == nulw, L,
          "an argument containing a NUL byte survives the round trip");
}

// 2. The RICH overload: the keep-processing answer.
void battery_keepgoing(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    check(e.callRich("rnothing") == true,  L, "returning nothing keeps processing");
    check(e.callRich("rtrue")    == true,  L, "returning true keeps processing");
    check(e.callRich("rfalse")   == false, L, "returning false stops processing");
    check(e.callRich("rone")     == true,  L, "returning 1 keeps processing");

    // The rule that surprises people, and the one Windows also implements: a
    // handler returning numeric 0 STOPS, even in languages where 0 is truthy.
    // Lua reads the value with tonumber for exactly this reason.
    if (d.zeroStops)
        check(e.callRich("rzero") == false, L, "returning 0 stops processing");
    else
        skip(L, "returning 0 stops processing", "language treats 0 as true");

    // Lua can tell "returned nil" from "returned nothing" -- an empty result
    // stack versus a nil on it -- so in Lua an explicit nil STOPS.  In the other
    // five, a function that falls off the end returns None/undef/null/nil, so
    // the engine cannot distinguish the two and must choose.  Each chose "keep
    // going", which is the safe reading: a handler with no return statement is
    // overwhelmingly the common case and must not silently suppress the event.
    if (d.nilStops)
        check(e.callRich("rnil") == false, L,
              "returning nil explicitly stops processing");
    else
        skip(L, "returning nil explicitly stops processing",
             "indistinguishable here from a handler with no return statement");
}

// 3. Parameters: the shape triggers, aliases and timers all use.
void battery_parameters(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    bool err = false;
    e.callRich("params", &err, { 7, 8 }, { "first", "second" });
    check(!err && e.var("p") == "7|8|first|second", L,
          "numeric then string parameters arrive in order");

    std::map<std::string, std::string> t{ { "key", "value" } };
    err = false;
    e.callRich("tbl", &err, {}, { "line" }, &t);
    check(!err && e.var("t") == "value", L,
          "the optional table (named wildcards) arrives as a mapping");
}

// 3b. The OTHER direction: a value the script hands BACK to the world API.
//     Every engine decodes incoming bytes leniently; the return leg has to be
//     just as lenient or a wildcard captured from a Latin-1 world cannot be
//     stored in a variable, sent to the MUD, or written to a note.
void battery_toworld(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    e.callRtn("store", "plain ascii");
    check(e.var("echoed") == "plain ascii", L,
          "a string the script passes to the world API arrives intact");

    CString utf8; utf8 = "caf\xC3\xA9";
    e.callRtn("store", utf8);
    check(e.var("echoed") == "caf\xC3\xA9", L,
          "valid UTF-8 passed to the world API arrives intact");

    CString latin1; latin1 = "caf\xE9";
    e.callRtn("store", latin1);
    check(e.var("echoed") == "caf\xE9", L,
          "non-UTF-8 bytes passed to the world API arrive intact");
}

// 4. Errors: reported AND the handler disabled, so a broken trigger does not
//    spam once per line of MUD output.
void battery_errors(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    bool err = false;
    const CString kept = e.callRtn("boom", "untouched", &err);
    check(err, L, "a runtime error is reported as an error");
    check(kept == "untouched", L,
          "a failed callback leaves the caller's string alone");
    check(e.saw("deliberate"), L, "the error text reaches the output window");

    // The dispid the caller holds must have been set to DISPID_UNKNOWN so the
    // engine stops calling it.  callRtn resolves a fresh dispid each time, so
    // drive it the way a real call site does: keep the dispid across calls.
    DISPID d2 = e.id("boom");
    long   count = 0;
    CString io = "x";
    e.eng->ExecuteLua(d2, "boom", eDontChangeAction, "selftest", "selftest",
                      io, count, io);
    check(d2 == DISPID_UNKNOWN, L, "a failed handler is disabled, not retried");
    check(count == 0, L, "a failed call is not counted as an invocation");
}

// 5. State: globals persist between calls, and engines do not see each other's.
void battery_state(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    for (int i = 0; i < 3; i++) e.callRich("bump");
    check(e.var("n") == "3", L, "a global persists across calls");

    Engine b(d.language, d.sourceB);
    if (!b.ok) { skip(L, "a second engine does not see the first's globals",
                      "second engine failed to open"); return; }
    e.callRich("readshared");
    b.callRich("readshared");
    check(e.var("v") == "from A", L, "an engine sees its own globals");
    check(b.var("v") == "nil",    L, "a second engine does NOT see the first's globals");

    // ...and each talks to ITS OWN world.
    e.doc->m_strWorldID = "aaaaaaaaaaaaaaaaaaaaaaaa";
    b.doc->m_strWorldID = "bbbbbbbbbbbbbbbbbbbbbbbb";
    e.callRich("who");
    b.callRich("who");
    check(e.var("w") == "aaaaaaaaaaaaaaaaaaaaaaaa" &&
          b.var("w") == "bbbbbbbbbbbbbbbbbbbbbbbb", L,
          "each engine's world object is its own document");
}

// 6. The world API itself, reached through the shared dispatch substrate.
void battery_worldapi(const Dialect& d)
{
    const char* L = d.display;

    // A FRESH engine: the world-variable count below has to be deterministic,
    // and the other batteries leave variables behind.
    Engine e(d.language, d.source);
    if (!e.ok) { skip(L, "the world API is reachable from a handler", "engine failed to open"); return; }

    bool err = false;
    e.callRich("api", &err);
    check(!err, L, "a handler can call the world API without erroring");
    check(e.saw("a note"), L, "world.Note reaches the output window");
    check(e.var("got") == "value", L, "a variable set from script reads back");
    check(e.var("err") == "30008", L,
          "a world method's error code arrives as a number (eInvalidObjectLabel)");
    // api() sets exactly rt, got and err before asking for the list, so the
    // array it gets back has three elements.
    check(e.var("cnt") == "3", L,
          "an array-returning method arrives with the right length");
}

// 6b. Tracing.  With Trace on, every reference path announces the callback it is
//     about to run -- it is how a user debugs "why did my trigger not fire?".
void battery_trace(const Dialect& d)
{
    const char* L = d.display;
    Engine e(d.language, d.source);
    if (!e.ok) { skip(L, "a traced call announces itself", "engine failed to open"); return; }

    e.doc->m_bTrace = TRUE;
    e.callRich("rnothing");
    check(e.saw("Executing"), L, "with Trace on, running a handler is announced");
}

// 7. Compiling and resolving.
void battery_compile(const Dialect& d)
{
    const char* L = d.display;

    Engine bad(d.language, "");
    if (!bad.ok) { skip(L, "a compile error is reported", "engine failed to open"); return; }
    check(bad.eng->Parse(CString(d.badSource), "selftest-bad"), L,
          "source that does not compile is reported as an error");

    Engine e(d.language, d.source);
    if (!e.ok) return;
    check(e.id("keep")   != DISPID_UNKNOWN, L, "a defined handler resolves");
    check(e.id("absent") == DISPID_UNKNOWN, L, "an undefined handler does not");
    check(e.id("")       == DISPID_UNKNOWN, L, "an empty handler name does not");
    // A name the LANGUAGE (or the world library) happens to define is not a
    // handler the SCRIPT defined.  Claiming it turns a plugin's typo'd callback
    // name into a call to something else entirely, and makes "is this callback
    // implemented?" -- which is how plugins are dispatched -- always answer yes.
    // Lua resolves through _G's metatable, whose __index is the world library,
    // and JavaScriptCore's built-ins are own properties of the global object --
    // in neither can the engine separate "the script defined this" from "the
    // language did" without changing resolution semantics that plugins rely on.
    // Recorded rather than tested, so the gap is visible instead of absent.
    if (d.hidesBuiltins)
        check(e.id(d.builtin) == DISPID_UNKNOWN, L,
              "a name the language itself defines is not a handler");
    else
        skip(L, "a name the language itself defines is not a handler",
             "resolution cannot separate script definitions from the language's own");
}

// One language, end to end.  Returns false if the interpreter is not built in.
bool run_language(const Dialect& d)
{
    Engine e(d.language, d.source);
    if (!e.ok) {
        // Distinguish "not built in" from "the dialect does not compile", which
        // would otherwise look like an absent engine and silently skip.
        Engine probe(d.language, "");
        if (!probe.ok) return false;                     // interpreter absent
        printf("  [FAIL] %-7s the selftest dialect itself failed to compile\n", d.display);
        printf("%s", probe.output().c_str());
        g_fail++;
        return true;
    }

    battery_simple    (e, d);
    battery_keepgoing (e, d);
    battery_parameters(e, d);
    battery_toworld   (e, d);
    battery_worldapi  (d);
    battery_state     (e, d);
    battery_trace     (d);
    battery_errors    (e, d);   // last: it disables handlers in this engine
    battery_compile   (d);
    return true;
}

} // namespace

// `only` is null to run every language, or a Dialect::key to run just one.
// Returns 0 (all passed), 1 (a failure), or 77 (ctest's SKIP code: the requested
// language's interpreter was not built into this binary).
int RunEmbeddedEnginesSelfTest(const char* only)
{
    printf("ENGINES-SELFTEST: %s\n", only ? only : "every scripting language");

    // We run before CMUSHclientApp::InitInstance, so the world doc template does
    // not exist yet.  world.GetWorld walks it unconditionally (as it may: on
    // Windows InitInstance has always run by the time a script can call
    // anything), so give it an empty one to walk.
    if (App.m_pWorldDocTemplate == nullptr)
        App.m_pWorldDocTemplate = new CMultiDocTemplate(
            IDR_MUSHCLTYPE,
            RUNTIME_CLASS(CMUSHclientDoc),
            RUNTIME_CLASS(CChildFrame),
            RUNTIME_CLASS(CMUSHView));

    int ran = 0, absent = 0;
    for (int i = 0; i < kDialectCount; i++) {
        const Dialect& d = kDialects[i];
        if (only && strcmp(only, d.key) != 0) continue;
        if (run_language(d)) ++ran;
        else {
            printf("  [SKIP] %-7s interpreter not built into this binary\n", d.display);
            ++absent;
        }
    }

    if (only && ran == 0 && absent == 0) {
        printf("ENGINES-SELFTEST: no such language \"%s\"\n", only);
        return 1;
    }
    if (ran == 0) {                    // everything asked for was absent
        printf("ENGINES-SELFTEST: skipped (interpreter not built in)\n");
        return 77;                     // ctest SKIP_RETURN_CODE
    }

    printf("ENGINES-SELFTEST: %d passed, %d failed, %d skipped\n",
           g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}

#endif // !_WIN32
