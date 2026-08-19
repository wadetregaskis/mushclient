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
#include "flags.h"                 // eEnabled -- the nil-default battery seeds triggers
#include "platform/mc_dispatch.h"   // MC_VariantDateToUnix
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
                  unsigned short reason = eDontChangeAction,
                  const t_regexp* regexp = nullptr,
                  bool lowercaseWildcards = false)
    {
        DISPID d = id(proc);
        long   count = 0;
        bool   keep  = false;
        const bool err = eng->ExecuteLua(d, proc, reason, "selftest", "selftest",
                                         nparams, sparams, count,
                                         regexp, table, nullptr, &keep,
                                         lowercaseWildcards);
        if (errOut) *errOut = err;
        return keep;
    }

    // Put a known sentinel in a variable, so a handler that silently does
    // nothing is caught by the next var() read instead of passing on a stale
    // value that happens to equal what we expected.
    void seed(const char* name, const char* value)
    { doc->SetVariable(CString (name), CString (value)); }

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
//     bignum(s)   passes 2^62+1 into world.SetVariable as variable "big"
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
    bool        intParams;      // ...does an integral number stringify without ".0"?
    bool        exactBigInt;    // ...are its integers exact past 2^53, or doubles?
    const char* mxpHandler;     // defines OnMXPopenTag(name, args, table)
    const char* mxpTableType;   // what that language calls the table's type
    const char* staleProxy;     // grab(), then useit()/useprop() a proxy to a
                                // closed world (method path, then property path)
    const char* gcChurn;        // allocate hard, then call the world API

    // Strict argument checking (world option "script_strict_arguments").
    //   probe(s)         calls world.SetVariable with ONE argument -- the method
    //                    declares two and neither has a kArgDefaults entry --
    //                    inside the language's error trap; "raised" or "silent".
    //   probedefault(s)  the same shape around world.EnableTrigger with one
    //                    argument, whose second slot DOES have a default, so it
    //                    must stay "silent" even with the option on.
    const char* strictProbe;
    //   introspect(s)    stringifies a bare method reference, the coercion
    //                    Python and JavaScript perform so that COM-style plugins
    //                    can write `world.GetPluginID` without parentheses.
    //                    Compares it against the same call spelled properly, so
    //                    the check does not hardcode a hash.  Null for the
    //                    languages that have no such coercion.
    const char* strictIntrospect;

    // An explicitly-passed nil (world.EnableTrigger('t', None)) must take the
    // same default an OMITTED argument takes, not coerce to zero and mean the
    // opposite.  nilcalls(s) makes the four calls the harness then reads back
    // off the doc: omitted, explicit nil, explicit true, explicit false.
    // Null for a language that cannot express nil at this boundary (Tcl:
    // tcl_to_variant marshals every argument as VT_BSTR, so there is no
    // VT_EMPTY for it to send).
    const char* nilArgs;
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
  "function rawnum(a, b, c, d) world.SetVariable('r', tostring(a)) end\n"
  "function bignum(s) world.SetVariable('big', 4611686018427387905) end\n"
  "function readdate() world.SetVariable('dt', tostring(world.GetInfo(301))) end\n"
  "function stylish()\n"
  "  world.SetVariable('ns', world.GetNoteStyle() == 0 and 'normal' or 'inherited')\n"
  "  world.NoteStyle(4) end\n"
  "function wc(n, l, w)\n"
  "  local t = {}\n"
  "  for i = 0, #w do t[#t+1] = w[i] end\n"
  "  world.SetVariable('wc', table.concat(t, '|')) end\n"
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
  true, true, "Note", false, true, true, false,
  "function OnMXPopenTag(name, args, t)\n"
  "  world.SetVariable('mxptag', tostring(name))\n"
  "  world.SetVariable('mxpargs', tostring(args))\n"
  "  world.SetVariable('mxptype', type(t))\n"
  "end\n", "table",
  "function grab(s) other = world.GetWorld(s) return 'ok' end\n"
  "function useit(s)\n"
  "  if other == nil then return 'noproxy' end\n"
  "  local ok = pcall(function() return other:GetWorldID() end)\n"
  "  return ok and 'used' or 'refused'\n"
  "end\n"
  // Lua has no property syntax -- world.Trace is world.GetTrace() here -- so
  // there is no second road into the doc for this language to guard.
  "function useprop(s) return 'unsupported' end\n",
  "function churn(s)\n"
  "  local acc = {}\n"
  "  for i = 1, 2000 do acc[#acc+1] = tostring(i) .. s end\n"
  "  collectgarbage('collect')\n"
  "  world.SetVariable('gc', world.Trim('  ' .. s .. '  '))\n"
  "  return 'ok'\n"
  "end\n",
  // Lua does NOT travel mc_dispatch -- lua_methods.cpp binds every method by
  // hand -- and my_checkstring already errors on a missing required argument.
  // So Lua raises with the option off as well, which is the whole point of
  // running it here: it is the behaviour strict restores for the other five.
  "function probe(s)\n"
  "  local ok = pcall(function() world.SetVariable('probe') end)\n"
  "  return ok and 'silent' or 'raised'\n"
  "end\n"
  "function probedefault(s)\n"
  "  local ok = pcall(function() world.EnableTrigger('nosuchtrigger') end)\n"
  "  return ok and 'silent' or 'raised'\n"
  "end\n"
  "function probeloud(s) world.SetVariable('probe') end\n",
  nullptr,     // Lua keeps world.Hash as a function; no string coercion
  // Lua is the CONTRACT here, not merely a control: optboolean says in so many
  // words that "nil will default to the default" (lua_methods.cpp:335), and it
  // is that binding's opt* defaults kArgDefaults copies for the other five.
  "function nilcalls(s)\n"
  "  world.EnableTrigger('n_omit')\n"
  "  world.EnableTrigger('n_nil', nil)\n"
  "  world.EnableTrigger('n_true', true)\n"
  "  world.EnableTrigger('n_false', false)\n"
  "  return 'done'\n"
  "end\n" },

// ── Python ────────────────────────────────────────────────────────────────
{ "python", "Python", "Python",
  "def keep(s):\n    pass\n"
  "def change(s):\n    return 'replaced'\n"
  "def wrap(s):\n    return '<' + s + '>'\n"
  "def size(s):\n    return str(len(s.encode('utf-8', 'surrogateescape')))\n"
  "def boom(s):\n    raise RuntimeError('deliberate')\n"
  // *a: called via both the rich overload (zero args) and the simple one
  // (one string arg, R2-07-F6's DispatchEmbedded coverage) -- unlike Lua,
  // Python raises on an unexpected positional argument.
  "def rnothing(*a):\n    pass\n"
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
  "def rawnum(a, b, c, d):\n"
  "    world.SetVariable('r', str(a))\n"
  "def bignum(s):\n"
  "    world.SetVariable('big', 4611686018427387905)\n"
  "def readdate():\n"
  "    world.SetVariable('dt', str(world.GetInfo(301)))\n"
  "def stylish(*a):\n"
  "    world.SetVariable('ns', 'normal' if world.GetNoteStyle() == 0 else 'inherited')\n"
  "    world.NoteStyle(4)\n"
  "def wc(n, l, w):\n"
  "    world.SetVariable('wc', '|'.join(w))\n"
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
  true, false, "print", true, true, false, true,
  "def OnMXPopenTag(name, args, t=None):\n"
  "    world.SetVariable('mxptag', str(name))\n"
  "    world.SetVariable('mxpargs', str(args))\n"
  "    world.SetVariable('mxptype', type(t).__name__)\n", "dict",
  "def grab(s):\n"
  "    global other\n    other = world.GetWorld(s)\n    return 'ok'\n"
  "def useit(s):\n"
  "    if other is None: return 'noproxy'\n"
  "    try:\n        other.GetWorldID()\n        return 'used'\n"
  "    except Exception:\n        return 'refused'\n"
  "def useprop(s):\n"
  "    if other is None: return 'noproxy'\n"
  "    try:\n        other.SpeedWalkDelay = 7\n"
  "        return 'on' if other.SpeedWalkDelay == 7 else 'off'\n"
  "    except Exception:\n        return 'refused'\n",
  "import gc\n"
  "def churn(s):\n"
  "    acc = [str(i) + s for i in range(2000)]\n"
  "    gc.collect()\n"
  "    world.SetVariable('gc', world.Trim('  ' + s + '  '))\n"
  "    return 'ok'\n",
  "def probe(s):\n"
  "    try:\n        world.SetVariable('probe')\n        return 'silent'\n"
  "    except Exception:\n        return 'raised'\n"
  "def probedefault(s):\n"
  "    try:\n        world.EnableTrigger('nosuchtrigger')\n        return 'silent'\n"
  "    except Exception:\n        return 'raised'\n"
  "def probeloud(s):\n    world.SetVariable('probe')\n",
  // str(world.Hash) invokes Hash with no arguments (py_engine.cpp Method_str).
  // Compared against the same call written out, so the assertion does not
  // depend on which hash function Hash is.  Without the introspection
  // exemption strict refuses the invoke and str() falls back to the
  // "<world method Hash>" placeholder, which cannot match.
  "def introspect(s):\n"
  "    return 'match' if str(world.Hash) == world.Hash('') else 'differ'\n",
  "def nilcalls(s):\n"
  "    world.EnableTrigger('n_omit')\n"
  "    world.EnableTrigger('n_nil', None)\n"
  "    world.EnableTrigger('n_true', True)\n"
  "    world.EnableTrigger('n_false', False)\n"
  "    return 'done'\n" },

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
  "sub rawnum { $world->SetVariable('r', \"$_[0]\"); }\n"
  "sub bignum { $world->SetVariable('big', 4611686018427387905); }\n"
  "sub readdate { $world->SetVariable('dt', '' . $world->GetInfo(301)); }\n"
  "sub stylish {\n"
  "  $world->SetVariable('ns', $world->GetNoteStyle() == 0 ? 'normal' : 'inherited');\n"
  "  $world->NoteStyle(4); }\n"
  "sub wc { my ($n, $l, $w) = @_;\n"
  "  $world->SetVariable('wc', join('|', @$w)); }\n"
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
  true, false, "printf", true, true, true, true,
  "sub OnMXPopenTag {\n"
  "  my ($name, $args, $t) = @_;\n"
  "  $world->SetVariable('mxptag', $name);\n"
  "  $world->SetVariable('mxpargs', $args);\n"
  "  $world->SetVariable('mxptype', ref($t) || 'undef');\n"
  "}\n", "HASH",
  "our $other;\n"
  "sub grab { $other = $world->GetWorld($_[0]); return 'ok'; }\n"
  "sub useit { return 'noproxy' unless defined $other;\n"
  "  my $r = eval { $other->GetWorldID(); 1 };\n"
  "  return $r ? 'used' : 'refused'; }\n"
  "sub useprop { return 'noproxy' unless defined $other;\n"
  "  my $r = eval { $other->SpeedWalkDelay(7); $other->SpeedWalkDelay() == 7 ? 1 : 0 };\n"
  "  return defined($r) ? ($r ? 'on' : 'off') : 'refused'; }\n",
  "sub churn {\n"
  "  my @acc; push @acc, $_ . $_[0] for (1 .. 2000);\n"
  "  $world->SetVariable('gc', $world->Trim('  ' . $_[0] . '  '));\n"
  "  return 'ok';\n"
  "}\n",
  "sub probe { my $r = eval { $world->SetVariable('probe'); 1 };\n"
  "  return $r ? 'silent' : 'raised'; }\n"
  "sub probedefault { my $r = eval { $world->EnableTrigger('nosuchtrigger'); 1 };\n"
  "  return $r ? 'silent' : 'raised'; }\n"
  "sub probeloud { $world->SetVariable('probe'); }\n",
  nullptr,     // Perl calls methods explicitly; no bare-reference coercion
  "sub nilcalls {\n"
  "  $world->EnableTrigger('n_omit');\n"
  "  $world->EnableTrigger('n_nil', undef);\n"
  "  $world->EnableTrigger('n_true', 1);\n"
  "  $world->EnableTrigger('n_false', 0);\n"
  "  return 'done';\n"
  "}\n" },

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
  "function rawnum(a, b, c, d) { world.SetVariable('r', String(a)); }\n"
  "function bignum(s) { world.SetVariable('big', 4611686018427387905); }\n"
  "function readdate() { world.SetVariable('dt', String(world.GetInfo(301))); }\n"
  "function stylish() {\n"
  "  world.SetVariable('ns', world.GetNoteStyle() == 0 ? 'normal' : 'inherited');\n"
  "  world.NoteStyle(4); }\n"
  "function wc(n, l, w) { world.SetVariable('wc', w.join('|')); }\n"
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
  true, false, "parseInt", false, true, true, false,
  "function OnMXPopenTag(name, args, t) {\n"
  "  world.SetVariable('mxptag', String(name));\n"
  "  world.SetVariable('mxpargs', String(args));\n"
  "  world.SetVariable('mxptype', typeof t);\n"
  "}\n", "object",
  "var other = null;\n"
  "function grab(s) { other = world.GetWorld(s); return 'ok'; }\n"
  "function useit(s) {\n"
  "  if (!other) return 'noproxy';\n"
  "  try { other.GetWorldID(); return 'used'; } catch (e) { return 'refused'; }\n"
  "}\n"
  "function useprop(s) {\n"
  "  if (!other) return 'noproxy';\n"
  "  try { other.SpeedWalkDelay = 7;\n"
  "        return other.SpeedWalkDelay == 7 ? 'on' : 'off'; }\n"
  "  catch (e) { return 'refused'; }\n"
  "}\n",
  "function churn(s) {\n"
  "  var acc = [];\n"
  "  for (var i = 0; i < 20000; i++) acc.push(String(i) + s);\n"
  "  world.SetVariable('gc', world.Trim('  ' + s + '  '));\n"
  "  return 'ok';\n"
  "}\n",
  "function probe(s) {\n"
  "  try { world.SetVariable('probe'); return 'silent'; }\n"
  "  catch (e) { return 'raised'; }\n"
  "}\n"
  "function probedefault(s) {\n"
  "  try { world.EnableTrigger('nosuchtrigger'); return 'silent'; }\n"
  "  catch (e) { return 'raised'; }\n"
  "}\n"
  "function probeloud(s) { world.SetVariable('probe'); }\n",
  // '' + world.Hash runs Method_convertToType (js_engine.cpp), the same
  // no-argument coercion Method_str is for Python.
  "function introspect(s) {\n"
  "  try { return ('' + world.Hash) == world.Hash('') ? 'match' : 'differ'; }\n"
  "  catch (e) { return 'differ'; }\n"
  "}\n",
  "function nilcalls(s) {\n"
  "  world.EnableTrigger('n_omit');\n"
  "  world.EnableTrigger('n_nil', undefined);\n"
  "  world.EnableTrigger('n_true', true);\n"
  "  world.EnableTrigger('n_false', false);\n"
  "  return 'done';\n"
  "}\n" },

// ── Tcl ───────────────────────────────────────────────────────────────────
{ "tcl", "Tcl", "tcl",
  "proc keep {s} { }\n"
  "proc change {s} { return replaced }\n"
  "proc wrap {s} { return \"<$s>\" }\n"
  "proc size {s} { return [string length $s] }\n"
  "proc boom {s} { error deliberate }\n"
  // args: called via both overloads (see the Python rnothing comment above);
  // Tcl also errors on an unexpected positional argument.
  "proc rnothing {args} { }\n"
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
  "proc rawnum {a b c d} { world SetVariable r $a }\n"
  "proc bignum {s} { world SetVariable big 4611686018427387905 }\n"
  "proc readdate {} { world SetVariable dt [world GetInfo 301] }\n"
  "proc stylish {args} {\n"
  "  world SetVariable ns [expr {[world GetNoteStyle] == 0 ? \"normal\" : \"inherited\"}]\n"
  "  world NoteStyle 4 }\n"
  "proc wc {n l w} { world SetVariable wc [join $w |] }\n"
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
  true, false, "puts", true, false, true, true,
  "proc OnMXPopenTag {name a t} {\n"
  "  world SetVariable mxptag $name\n"
  "  world SetVariable mxpargs $a\n"
  "  world SetVariable mxptype [expr {[catch {dict size $t}] ? \"notadict\" : \"dict\"}]\n"
  "}\n", "dict",
  "proc grab {s} { return unsupported }\n"
  "proc useit {s} { return unsupported }\n"
  "proc useprop {s} { return unsupported }\n",
  "proc churn {s} {\n"
  "  set acc {}\n"
  "  for {set i 0} {$i < 2000} {incr i} { lappend acc $i$s }\n"
  "  world SetVariable gc [world Trim \"  $s  \"]\n"
  "  return ok\n"
  "}\n",
  "proc probe {s} {\n"
  "  if {[catch {world SetVariable probe}]} { return raised } else { return silent }\n"
  "}\n"
  "proc probedefault {s} {\n"
  "  if {[catch {world EnableTrigger nosuchtrigger}]} { return raised }\n"
  "  return silent\n"
  "}\n"
  "proc probeloud {s} { world SetVariable probe }\n",
  nullptr,     // `world Foo` is a command invocation; nothing to coerce
  nullptr },   // ...and no nil either: every Tcl argument marshals as VT_BSTR

// ── Ruby ──────────────────────────────────────────────────────────────────
{ "ruby", "Ruby", "ruby",
  "def keep(s); nil; end\n"
  "def change(s); 'replaced'; end\n"
  "def wrap(s); '<' + s + '>'; end\n"
  "def size(s); s.bytesize.to_s; end\n"
  "def boom(s); raise 'deliberate'; end\n"
  // (*a): called via both overloads (see the Python rnothing comment above);
  // Ruby also errors on an unexpected positional argument.
  "def rnothing(*a); nil; end\n"
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
  "def rawnum(a, b, c, d); world.SetVariable('r', a.to_s); end\n"
  "def bignum(s); world.SetVariable('big', 4611686018427387905); end\n"
  "def readdate; world.SetVariable('dt', world.GetInfo(301).to_s); end\n"
  "def stylish(*a)\n"
  "  world.SetVariable('ns', world.GetNoteStyle() == 0 ? 'normal' : 'inherited')\n"
  "  world.NoteStyle(4)\n"
  "end\n"
  "def wc(n, l, w); world.SetVariable('wc', w.join('|')); end\n"
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
  true, false, "inspect", true, true, false, true,
  "def OnMXPopenTag(name, args, t = nil)\n"
  "  world.SetVariable('mxptag', name.to_s)\n"
  "  world.SetVariable('mxpargs', args.to_s)\n"
  "  world.SetVariable('mxptype', t.class.to_s)\n"
  "end\n", "Hash",
  "def grab(s); @other = world.GetWorld(s); 'ok'; end\n"
  "def useit(s)\n"
  "  return 'noproxy' if @other.nil?\n"
  "  begin; @other.GetWorldID(); 'used'; rescue Exception; 'refused'; end\n"
  "end\n"
  "def useprop(s)\n"
  "  return 'noproxy' if @other.nil?\n"
  "  begin\n"
  "    @other.SpeedWalkDelay = 7\n"
  "    @other.SpeedWalkDelay == 7 ? 'on' : 'off'\n"
  "  rescue Exception; 'refused'; end\n"
  "end\n",
  "def churn(s)\n"
  "  acc = []\n"
  "  2000.times { |i| acc << i.to_s + s }\n"
  "  GC.start\n"
  "  world.SetVariable('gc', world.Trim('  ' + s + '  '))\n"
  "  'ok'\n"
  "end\n",
  "def probe(s)\n"
  "  begin; world.SetVariable('probe'); 'silent'; rescue Exception; 'raised'; end\n"
  "end\n"
  "def probedefault(s)\n"
  "  begin; world.EnableTrigger('nosuchtrigger'); 'silent'; rescue Exception; 'raised'; end\n"
  "end\n"
  "def probeloud(s); world.SetVariable('probe'); end\n",
  nullptr,     // Ruby needs the parentheses; no bare-reference coercion
  "def nilcalls(s)\n"
  "  world.EnableTrigger('n_omit')\n"
  "  world.EnableTrigger('n_nil', nil)\n"
  "  world.EnableTrigger('n_true', true)\n"
  "  world.EnableTrigger('n_false', false)\n"
  "  'done'\n"
  "end\n" },
};

const int kDialectCount = (int) (sizeof kDialects / sizeof kDialects[0]);

// ── driving a REAL call site ──────────────────────────────────────────────
//
// Everything else here calls CScriptEngine directly, which leaves the code that
// BUILDS the callback's arguments untested -- and that is where the
// language-specific damage lived: mxpStart.cpp's rich arm gated on IsLua(), so
// a world scripted in any other language fell through to the COM arm, whose
// SAFEARRAY the port's CScriptEngine::Execute drops ("default: break;"), and
// those five received no arguments at all.
//
// The world's own MXP handler is the call site to drive, because it needs only
// a script engine on the doc -- no plugin, and therefore no main frame, which
// does not exist this early.

void battery_mxp_callsite(const Dialect& d)
{
    const char* L = d.display;

    CMUSHclientDoc doc;
    doc.m_bScriptErrorsToOutputWindow = TRUE;
    doc.SetUpOutputWindow();
    doc.m_bMXP      = true;
    doc.m_iUseMXP   = eUseMXP;
    doc.m_iMXP_mode = eMXP_secure;

    doc.m_ScriptEngine = new CScriptEngine(&doc, d.language);
    doc.m_ScriptEngine->CreateScriptEngine();
    if (doc.m_ScriptEngine->Parse(CString(d.mxpHandler), "selftest-mxp")) {
        skip(L, "an MXP open tag reaches the world's handler", "the MXP dialect failed to compile");
        return;
    }
    doc.m_strOnMXP_OpenTag    = "OnMXPopenTag";
    doc.m_dispidOnMXP_OpenTag = doc.m_ScriptEngine->GetDispid("OnMXPopenTag");
    if (doc.m_dispidOnMXP_OpenTag == DISPID_UNKNOWN) {
        skip(L, "an MXP open tag reaches the world's handler", "the handler did not resolve");
        return;
    }

    const char* text = "<font color=red>hi</font>";
    doc.DisplayMsg(text, (int) strlen(text), 0);

    VARIANT v = doc.GetVariable("mxptag");
    const CString tag (v.bstrVal ? v.bstrVal : "");
    check(tag.Find("font") == 0, L,
          "an MXP open tag reaches the world's handler with its tag name");

    v = doc.GetVariable("mxpargs");
    const CString args (v.bstrVal ? v.bstrVal : "");
    check(args.Find("color") >= 0, L, "...and with the argument text");

    // The third argument is the tell: only the rich callback path carries it.
    v = doc.GetVariable("mxptype");
    const CString ttype (v.bstrVal ? v.bstrVal : "");
    check(ttype == d.mxpTableType, L,
          "...and with a mapping of the parsed arguments, which the COM path drops");
}

// The second real call site: a TRIGGER, matched off actual MUD output.
//
// battery_wildcards calls CScriptEngine directly, which proves each engine
// honours the flag but not that anything ever SETS it -- the read of
// trigger_item->bLowercaseWildcard lives in CMUSHclientDoc::ExecuteTriggerScript
// (doc_shared.cpp), one layer above.  Drop that argument and every check in
// battery_wildcards still passes.  So drive the whole chain: a real regexp
// trigger with the box ticked, fed a real line.
void battery_trigger_callsite(const Dialect& d, bool lowercase)
{
    const char* L = d.display;

    CMUSHclientDoc doc;
    doc.m_bScriptErrorsToOutputWindow = TRUE;
    doc.SetUpOutputWindow();
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_iFlags        = COLOUR_ANSI;
    doc.m_iForeColour   = WHITE;
    doc.m_iBackColour   = BLACK;
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));

    doc.m_ScriptEngine = new CScriptEngine(&doc, d.language);
    doc.m_ScriptEngine->CreateScriptEngine();
    const char* what = lowercase
        ? "a matched trigger delivers lower-cased wildcards when the box is ticked"
        : "a matched trigger delivers wildcards unfolded when the box is CLEAR";

    if (doc.m_ScriptEngine->Parse(CString(d.source), "selftest-trigger")) {
        skip(L, what, "the dialect failed to compile");
        return;
    }

    CTrigger* t = new CTrigger;
    t->trigger      = CString("^(\\w+) shouts (\\w+)$");
    t->strLabel     = CString("shouter");
    t->strProcedure = CString("wc");
    t->bEnabled     = TRUE;
    t->bRegexp      = TRUE;
    t->bLowercaseWildcard = lowercase ? TRUE : FALSE;   // the box the user ticks
    t->iSendTo      = eSendToWorld;     // not eSendToScript: no escaping in play
    t->iSequence    = DEFAULT_TRIGGER_SEQUENCE;
    t->regexp       = regcomp(t->trigger, 0);

    doc.GetTriggerMap().SetAt(t->strLabel, t);
    doc.GetTriggerArray().Add(t);
    doc.GetTriggerRevMap()[t] = "shouter";
    // What FindAllEntryPoints does for each trigger, inlined: the real one
    // wants a document template we do not have this early.  (It used to have a
    // headless twin, ResolveAllEntryPoints; that is gone.)
    t->dispid = doc.m_ScriptEngine->GetDispid(t->strProcedure);

    if (t->dispid == DISPID_UNKNOWN) {
        skip(L, what, "the handler did not resolve");
        return;
    }

    doc.SetVariable(CString ("wc"), CString ("trigger never fired"));

    const char* line = "GandalF shouts LOUDLY\n";
    doc.DisplayMsg(line, (int) strlen(line), 0);

    VARIANT v = doc.GetVariable("wc");
    const std::string got (v.bstrVal ? v.bstrVal : "");

    // Lua ignores the option entirely, so BOTH runs expect the raw text from it.
    const bool wantFolded = lowercase && strcmp(d.key, "lua") != 0;

    if (wantFolded)
        check(got == "gandalf shouts loudly|gandalf|loudly", L, what);
    else if (lowercase)
        check(got == "GandalF shouts LOUDLY|GandalF|LOUDLY", L,
              "a matched trigger delivers wildcards RAW to Lua, box ticked or not");
    else
        check(got == "GandalF shouts LOUDLY|GandalF|LOUDLY", L, what);
}

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

    // An integral value must not gain a decimal point on the way in.  nparams is
    // a list<double> because that is the widest thing the seam carries, but a
    // trigger's line number arriving as "16.0" is wrong wherever the script
    // interpolates it -- and in Tcl it is worse than wrong, because a double
    // object cannot be `incr`ed or used to index a string.  Python and Ruby
    // genuinely model it as a float and say so.
    e.callRich("rawnum", &err, { 16, 0 }, { "x", "y" });
    if (d.intParams)
        check(e.var("r") == "16", L,
              "an integral numeric parameter stringifies without a decimal point");
    else
        skip(L, "an integral numeric parameter stringifies without a decimal point",
             "the language models it as a float, and says so");

    std::map<std::string, std::string> t{ { "key", "value" } };
    err = false;
    e.callRich("tbl", &err, {}, { "line" }, &t);
    check(!err && e.var("t") == "value", L,
          "the optional table (named wildcards) arrives as a mapping");
}

// 3a-quater. VT_DATE.
//
// The world API returns dates as VT_DATE -- GetInfo(301..306), GetLineInfo(9),
// GetTimerInfo, GetPluginInfo(13/14/22).  Lua converts them to Unix seconds
// (lua_methods.cpp:142-159); the five embedded engines had no VT_DATE arm in
// their variant switches at all, so every one of those calls returned
// nil/None/undefined and a script could not read a date in five of six
// languages.
void battery_dates(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    // A fixed instant, so the check does not drift with the clock.
    const time_t kWhen = 1000000000;      // 2001-09-09T01:46:40Z
    e.doc->m_tConnectTime = CTime(kWhen);

    // What the conversion should yield, computed the same way the engines do.
    // Comparing against a hard-coded 1000000000 instead would make this a test
    // of the shim's COleDateTime/CTime timezone handling rather than of whether
    // the engine marshals VT_DATE at all -- which is the actual bug.
    VARIANT vd;
    VariantInit(&vd);
    vd.vt   = VT_DATE;
    vd.date = COleDateTime(kWhen);
    char expected[32];
    snprintf(expected, sizeof expected, "%ld", MC_VariantDateToUnix(vd));

    e.seed("dt", "handler never ran");
    e.callRich("readdate");
    check(e.var("dt") == expected, L,
          "a VT_DATE result (GetInfo 301) arrives as a number, not nil");
}

// 3a-ter. The SURROUNDINGS a callback runs in.
//
// ExecuteLua does two things around the Lua call that the five embedded engines
// were dispatched out BEFORE ever reaching:
//
//   * m_iNoteStyle is saved, reset to NORMAL for the callback, and restored
//     unconditionally afterwards (lua_scripting.cpp:765).  Without it a style a
//     handler sets with world.NoteStyle leaks back out and tints every later
//     note in the world -- and the next callback starts in whatever style the
//     previous one left.
//   * the elapsed time is added to the world's total, and to the current
//     plugin's, but only when the call did NOT error -- which is where Lua's
//     `if (error) return true;` sits relative to its timing block.  Without it
//     world.GetScriptTime and the Trace timings read zero for five of six
//     languages.
//
// Both assertions are made from C++ rather than from the script, so they read
// the same way in every language and cannot be confused by how each one
// stringifies a number.
void battery_call_surroundings(Engine& e, const Dialect& d)
{
    const char* L = d.display;

    // The world's own note style before anything is called.
    e.doc->m_iNoteStyle = HILITE;

    bool err = false;
    e.seed("ns", "handler never ran");
    e.callRich("stylish", &err);
    check(!err, L, "a callback may set the note style without erroring");
    // The RESET: the handler must not inherit the style the world was left in.
    check(e.var("ns") == "normal", L,
          "a callback starts with the note style reset to NORMAL");
    // The RESTORE: what the handler set must not survive the call.
    check(e.doc->m_iNoteStyle == HILITE, L,
          "a note style set inside a callback does not leak back to the world");

    // R2-07-F6: the SIMPLE overload (ExecutePluginScriptRtn's shape) gives its
    // five embedded engines the identical surroundings, from what used to be a
    // second ~35-line near-clone of the block above -- now DispatchEmbedded,
    // shared by both.  Same handler, same assertions, so a "refactor" that only
    // services one copy's surroundings fails exactly one half of this function.
    e.doc->m_iNoteStyle = HILITE;
    e.seed("ns", "handler never ran");
    bool errSimple = false;
    e.callRtn("stylish", "unused", &errSimple);
    check(!errSimple, L, "[simple] a callback may set the note style without erroring");
    check(e.var("ns") == "normal", L,
          "[simple] a callback starts with the note style reset to NORMAL");
    check(e.doc->m_iNoteStyle == HILITE, L,
          "[simple] a note style set inside a callback does not leak back to the world");

    // GetScriptTime divides by App.m_iCounterFrequency, which InitInstance
    // normally fills in -- and InitInstance has not run this early, so seed it
    // or the whole timing path is skipped and the check is vacuous.
    if (App.m_iCounterFrequency == 0) {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f)) App.m_iCounterFrequency = f.QuadPart;
    }
    if (App.m_iCounterFrequency == 0) {
        skip(L, "a successful callback accumulates script time",
             "no performance counter on this machine");
        return;
    }

    const double before = e.doc->GetScriptTime();
    // rnothing, NOT bump: bump mutates a global that battery_state asserts on
    // later, and this loop would leave it at the wrong value.
    for (int i = 0; i < 50; i++) e.callRich("rnothing");
    const double after = e.doc->GetScriptTime();
    check(after > before, L, "a successful callback accumulates script time");

    // Same accumulation check, through the SIMPLE overload's copy.
    const double beforeSimple = e.doc->GetScriptTime();
    for (int i = 0; i < 50; i++) e.callRtn("rnothing", "x");
    const double afterSimple = e.doc->GetScriptTime();
    check(afterSimple > beforeSimple, L,
          "[simple] a successful callback accumulates script time");
}

// 3a-bis. The POSITIONAL wildcards -- the parameter every trigger and alias
//         callback receives, and the only one whose content is language-policy
//         rather than plumbing.
//
// Shape is Lua's, on both platforms: [0] is the whole match and [1..n] are the
// numbered captures.  (The Windows COM arm builds a different array -- 1-based,
// always 10 long, whole match LAST at index 10 -- but that is the shape its
// ActiveScript hosts have always seen; the port's engines follow the shape its
// own Lua sets, so a script ported between the six languages here indexes the
// same way.)
//
// CONTENT is where the two arms genuinely differ, and this is the asymmetry the
// battery exists to pin.  "Convert wildcards to lower case"
// (CTrigger::bLowercaseWildcard) is applied by FixWildcard when evaluate.cpp
// fills trigger_item->wildcards[] -- the array the COM callback and
// GetTriggerInfo(101..110) read.  So:
//
//   * the five embedded engines stand in for ActiveScript hosts and MUST fold;
//   * Lua MUST NOT -- its callback reads the regexp raw on Windows too, and
//     tests/test_triggers.cpp pins that same split from the headless side.
//
// Before this existed, all five ignored the option entirely: a trigger with the
// box ticked delivered mixed-case wildcards to Python/Perl/JS/Tcl/Ruby.
void battery_wildcards(Engine& e, const Dialect& d)
{
    const char* L = d.display;
    const bool  isLua = strcmp(d.key, "lua") == 0;

    // Two captures, so the join below proves ORDER as well as content: get the
    // whole match and the captures the wrong way round and it cannot pass.
    t_regexp* re = regcomp("^(\\w+) shouts (\\w+)$");
    if (re == nullptr) {                       // regcomp throws rather than
        skip(L, "wildcards reach the callback", "regexp would not compile");
        return;                                // returns null, but be safe
    }
    if (regexec(re, "GandalF shouts LOUDLY") == 0) {
        skip(L, "wildcards reach the callback", "the test regexp did not match");
        delete re;
        return;
    }

    const std::string raw    = "GandalF shouts LOUDLY|GandalF|LOUDLY";
    const std::string folded = "gandalf shouts loudly|gandalf|loudly";

    e.seed("wc", "handler never ran");
    e.callRich("wc", nullptr, {}, { "tname", "GandalF shouts LOUDLY" },
               nullptr, eDontChangeAction, re, false);
    check(e.var("wc") == raw, L,
          "wildcards arrive as [whole match, capture 1, capture 2], case intact");

    e.seed("wc", "handler never ran");
    e.callRich("wc", nullptr, {}, { "tname", "GandalF shouts LOUDLY" },
               nullptr, eDontChangeAction, re, true);
    if (isLua)
        check(e.var("wc") == raw, L,
              "\"lower case wildcards\" is IGNORED (Lua reads the regexp raw, as on Windows)");
    else
        check(e.var("wc") == folded, L,
              "\"lower case wildcards\" folds every entry, whole match included");

    delete re;
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

// 5b. Where a script error GOES is the world's choice, not the engine's.
//     World Properties > Scripting > "Note errors to the output window"; with it
//     clear, the error raises the canonical CScriptErrorDlg instead (inert here),
//     which is also how a user ticks the box from the error itself.  Each engine
//     used to ColourNote unconditionally, so the setting was a dead reader for
//     five of the six languages.
//
//     Run BOTH ways in the same engine: an "the output window is empty"
//     assertion on its own passes when nothing happened at all.
void battery_error_routing(const Dialect& d)
{
    const char* L = d.display;

    {   // the box is ticked: the error text lands in the output window
        Engine e(d.language, d.source);
        if (!e.ok) { skip(L, "the error-routing setting is obeyed", "engine failed to open"); return; }
        e.doc->m_bScriptErrorsToOutputWindow = TRUE;
        e.callRtn("boom", "x");
        check(e.saw("deliberate"), L,
              "with 'note errors to the output window' ON, the error text is noted");
    }
    {   // the box is clear: it does not -- it goes to the dialog instead
        Engine e(d.language, d.source);
        if (!e.ok) return;
        e.doc->m_bScriptErrorsToOutputWindow = FALSE;
        e.callRtn("boom", "x");
        check(!e.saw("deliberate"), L,
              "with it OFF, the error is NOT noted (it raises the error dialog)");
    }
}

// 5c. A world proxy must not outlive its world.
//     world.GetWorld returns a bare CMUSHclientDoc* here -- no COM, so nothing
//     AddRefs it the way pDoc->GetIDispatch(TRUE) does on Windows.  A script
//     that stores the proxy and uses it after that world closes was reading
//     freed memory; every engine now checks MC_DispatchDocIsLive first.
void battery_stale_proxy(const Dialect& d)
{
    const char* L = d.display;

    std::string src(d.source);
    src += d.staleProxy;
    Engine e(d.language, src.c_str());
    if (!e.ok) { skip(L, "a proxy to a closed world is refused", "engine failed to open"); return; }

    // A SECOND world, registered so GetWorld can find it by name.
    CMUSHclientDoc* other = new CMUSHclientDoc();
    other->m_mush_name  = "SelftestOtherWorld";
    other->m_strWorldID = "cccccccccccccccccccccccc";
    App.m_pWorldDocTemplate->AddDocument(other);

    if (e.callRtn("grab", "SelftestOtherWorld") == "unsupported") {
        App.m_pWorldDocTemplate->RemoveDocument(other);
        delete other;
        skip(L, "a proxy to a closed world is refused",
             "this language has no representation for another world");
        return;
    }
    check(e.callRtn("useit", "") == "used", L,
          "a proxy to an OPEN world works");     // the positive control

    // A dispatch PROPERTY is the SECOND road into the same doc, and the method
    // guard cannot see it: world.SpeedWalkDelay reads and writes the members
    // through MC_DispatchGet/SetProperty, no Invoke anywhere.  Python and
    // JavaScript took it without asking whether the world was still open.
    const CString propOpen = e.callRtn("useprop", "");
    const bool hasProps = propOpen != "unsupported";
    if (hasProps)
        check(propOpen == "on", L,
              "a property on a proxy to an OPEN world works");   // the positive control
    else
        skip(L, "a property on a proxy to a CLOSED world is refused",
             "this language reaches properties through the method path");

    App.m_pWorldDocTemplate->RemoveDocument(other);
    delete other;                                 // the world closes

    check(e.callRtn("useit", "") == "refused", L,
          "a proxy to a CLOSED world is refused, not dereferenced");
    if (hasProps)
        check(e.callRtn("useprop", "") == "refused", L,
              "...and so is a property on it, which is a road of its own");
}

// 5d. Under collector pressure.
//     JavaScriptCore scans the C stack conservatively but not arbitrary heap, and
//     MRI does not scan the C++ heap at all -- so a value whose only reference is
//     in a std::vector, or an interior pointer taken out of a Ruby array, is
//     invisible to the collector and can be freed mid-call.  Make the script
//     allocate hard (and collect explicitly where the language lets it) around a
//     world-API call, and check the call still produced the right answer.
//
//     HONEST LIMIT: this is a smoke check, not a proof.  Reverting the
//     JSValueProtect / RB_GC_GUARD fixes does NOT fail it -- the collector has
//     to fire at exactly the wrong instant, which no test can schedule.  What it
//     does catch is a fix that BREAKS the call path (an over-eager unprotect, a
//     double free), and it puts allocation pressure on paths that previously saw
//     none at all.  The fixes themselves rest on the documented contracts:
//     JavaScriptCore scans the C stack, not the heap; MRI cannot see a VALUE
//     that only an interior pointer references.
void battery_gc_pressure(const Dialect& d)
{
    const char* L = d.display;

    std::string src(d.source);
    src += d.gcChurn;
    Engine e(d.language, src.c_str());
    if (!e.ok) { skip(L, "a world call survives a collection", "engine failed to open"); return; }

    check(e.callRtn("churn", "payload") == "ok", L,
          "a handler that allocates hard around a world call still returns");
    check(e.var("gc") == "payload", L,
          "...and the world call's arguments and result survived the collection");
}

// 5e. A 64-bit integer must not lose precision crossing into the VARIANT.
//     Ruby's Bignum arm sent every one of them to VT_R8, and CRuby's tagged
//     Fixnum stops at 2^62 -- so 2^62+1, which the port's 64-bit lVal holds
//     exactly, arrived rounded to 2^62 and reached a string parameter as
//     "4.6116860184274e+18".  Languages whose numbers ARE doubles (Lua,
//     JavaScript) cannot express the value in the first place and say so.
void battery_big_integer(const Dialect& d)
{
    const char* L = d.display;

    if (!d.exactBigInt) {
        skip(L, "a 2^62 integer reaches the world API exactly",
             "this language's numbers are doubles");
        return;
    }

    Engine e(d.language, d.source);
    if (!e.ok) { skip(L, "a 2^62 integer reaches the world API exactly",
                      "engine failed to open"); return; }

    e.seed("big", "not set");     // so a handler that does nothing fails too
    e.callRtn("bignum", "");
    check(e.var("big") == "4611686018427387905", L,
          "a 2^62 integer reaches the world API exactly, not as a rounded double");
}

// 5f. Strict argument checking -- world option "script_strict_arguments".
//
//     Off by default, a short argument list is zero-filled: world.SetVariable
//     with one argument quietly sets that variable to "".  That leniency is the
//     port's own invention.  The Windows build rides the real
//     IDispatch::Invoke, which refuses the same call with DISP_E_BADPARAMCOUNT
//     -- the shared dispatch map declares no VTS_VARIANT anywhere, so every
//     declared parameter is required there -- and Lua's hand-written binding
//     errors too.  The option lets a plugin author opt back into that while
//     developing.
//
//     Lua is a genuine control here rather than a skip: it does not travel
//     mc_dispatch at all, so it raises with the option OFF as well, and what it
//     shows is precisely the behaviour strict restores for the other five.
//
//     ARITY ONLY, never types: tcl_engine.cpp marshals every argument as
//     VT_BSTR by design (tcl_to_variant), so a type-strict check would reject
//     every numeric parameter Tcl passes -- the entire language.
void battery_strict_arguments(const Dialect& d)
{
    const char* L = d.display;
    // Lua's own binding checks arguments, so the option cannot change it.
    const bool luaAlwaysStrict = (strcmp(d.key, "lua") == 0);

    std::string src(d.source);
    src += d.strictProbe;
    if (d.strictIntrospect) src += d.strictIntrospect;

    // ── the control: option OFF, today's behaviour ────────────────────────
    {
        Engine e(d.language, src.c_str());
        if (!e.ok) { skip(L, "a short argument list is refused when strict is on",
                          "engine failed to open"); return; }
        // A sentinel, so "the call did nothing" is distinguishable from "the
        // call zero-filled": var() cannot tell an absent variable from an
        // empty one, and both halves of this battery turn on that difference.
        e.seed("probe", "sentinel");
        check(e.callRtn("probe", "") == (luaAlwaysStrict ? "raised" : "silent"), L,
              luaAlwaysStrict
                ? "strict OFF: Lua still checks its own arguments"
                : "strict OFF: a short argument list is accepted, as it always was");
        check(e.var("probe") == (luaAlwaysStrict ? "sentinel" : ""), L,
              luaAlwaysStrict
                ? "strict OFF: ...so nothing was written"
                : "strict OFF: ...and the zero-fill really ran");
    }

    // ── option ON ─────────────────────────────────────────────────────────
    {
        Engine e(d.language, src.c_str());
        if (!e.ok) return;
        e.doc->m_bStrictScriptArguments = TRUE;
        e.seed("probe", "sentinel");

        check(e.callRtn("probe", "") == "raised", L,
              "strict ON: a short argument list raises an error the script can catch");
        check(e.var("probe") == "sentinel", L,
              "strict ON: ...and the method did not run");

        // The ordering crux: the check sits AFTER the kArgDefaults padding, so
        // EnableTrigger's documented default still fills its second slot.
        check(e.callRtn("probedefault", "") == "silent", L,
              "strict ON: a method whose missing slot HAS a default is still fine");

        if (d.strictIntrospect)
            // Stringifying a bare method reference invokes it with no
            // arguments.  That is the language coercing, not the author
            // calling, so strict must not fire -- or every COM-style
            // `world.Foo` in a plugin turns into a placeholder string.
            check(e.callRtn("introspect", "") == "match", L,
                  "strict ON: the no-argument string coercion is exempt");
        else
            skip(L, "strict ON: the no-argument string coercion is exempt",
                 "this language has no bare-method-reference coercion");
    }

    // ── and the author actually SEES it ───────────────────────────────────
    // Everything above catches the error in the script, which proves it is
    // catchable but says nothing about the uncaught case -- and uncaught is
    // how a plugin author meets it.  The Engine constructor ticks "note
    // errors to the output window", the same box battery_error_routing
    // drives, so an unhandled error lands in the output where saw() can read
    // it.  Asserting the TEXT is the point: a bare "bad arguments" would pass
    // a mere did-it-error check while telling the author nothing.
    {
        Engine e(d.language, src.c_str());
        if (!e.ok) return;
        e.doc->m_bStrictScriptArguments = TRUE;

        bool err = false;
        e.callRtn("probeloud", "", &err);
        check(err, L, "strict ON: an uncaught short call is reported as an error");
        if (luaAlwaysStrict)
            // Lua's own binding raises this one, with its own wording.
            check(e.saw("bad argument"), L,
                  "strict ON: ...and Lua's argument error names the argument");
        else
            check(e.saw("expected at least 2 arguments, got 1"), L,
                  "strict ON: ...and the message gives the expected and supplied counts");
    }
}

// 5g. An explicitly-passed nil is an OMITTED argument, not a zero.
//
//     The five embedded engines all marshal their nothing-value to VT_EMPTY
//     (Python None, Ruby nil, Perl undef, JavaScript undefined/null), and
//     mc_dispatch used to fill a default only into slots BEYOND the supplied
//     count -- so an explicit nil skipped the default and var_to_long read it as
//     0.  world.EnableTrigger("t") ENABLED and world.EnableTrigger("t", None)
//     DISABLED: two spellings of one call, opposite effects, eOK both times.
//
//     Lua is the contract rather than a bystander: optboolean treats nil as
//     absent by design ("nil will default to the default", lua_methods.cpp:335)
//     and it is Lua's opt* defaults that kArgDefaults copies.  It is also the
//     one language here that does NOT travel mc_dispatch, so it stays green
//     across the fix and shows what the other five were supposed to do.
//
//     Asserting the trigger's enabled STATE, not the number the argument
//     coerced to, is the whole point -- the defect was an inverted effect.  The
//     fixture makes "the call did nothing" fail every row: the three that must
//     enable start disabled, the one that must disable starts enabled.
void battery_explicit_nil(const Dialect& d, const bool strict)
{
    const char* L = d.display;
    const char* what = strict
        ? "strict ON: an explicit nil still takes the argument's default"
        : "an explicit nil takes the same default as an omitted argument";
    if (!d.nilArgs) {
        skip(L, what, "this language has no nil: every argument marshals as a string");
        return;
    }

    std::string src(d.source);
    src += d.nilArgs;
    Engine e(d.language, src.c_str());
    if (!e.ok) { skip(L, what, "engine failed to open"); return; }
    e.doc->m_bStrictScriptArguments = strict ? TRUE : FALSE;

    struct Row { const char* name; bool before; bool after; const char* what; };
    static const Row rows[] = {
        { "n_omit",  false, true,  "omitted -> the documented default (enabled)" },
        { "n_nil",   false, true,  "explicit nil -> the SAME default, not 0" },
        { "n_true",  false, true,  "explicit true -> enabled" },
        { "n_false", true,  false, "explicit false -> disabled (a supplied value wins)" },
    };
    for (const Row& r : rows)
        e.doc->AddTrigger(r.name, "match", "", r.before ? eEnabled : 0,
                          -1, 0, "", "");

    // The fixture really is non-degenerate before the script runs.
    bool seeded = true;
    for (const Row& r : rows) {
        VARIANT v = e.doc->GetTriggerInfo(r.name, 8);
        if (!(v.vt == VT_BOOL && (v.boolVal != 0) == r.before)) seeded = false;
    }
    check(seeded, L, strict
          ? "strict ON: the nil fixture starts in the state the calls must change"
          : "the nil fixture starts in the state the calls must change");

    check(e.callRtn("nilcalls", "") == "done", L, strict
          ? "strict ON: the four spellings of EnableTrigger all ran"
          : "the four spellings of EnableTrigger all ran");

    for (const Row& r : rows) {
        VARIANT v = e.doc->GetTriggerInfo(r.name, 8);
        const bool on = (v.vt == VT_BOOL && v.boolVal != 0);
        std::string label = strict ? std::string("strict ON: ") : std::string();
        label += r.what;
        check(on == r.after, L, label.c_str());
    }
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
    battery_wildcards (e, d);
    battery_call_surroundings(e, d);
    battery_dates     (e, d);
    battery_toworld   (e, d);
    battery_worldapi  (d);
    battery_state     (e, d);
    battery_trace     (d);
    battery_error_routing(d);
    battery_stale_proxy  (d);
    battery_gc_pressure  (d);
    battery_big_integer  (d);
    battery_strict_arguments(d);
    battery_explicit_nil(d, false);       // option off -- the everyday path
    battery_explicit_nil(d, true);        // ...and on: strict must not read an
                                          // explicit nil as a MISSING argument
    battery_mxp_callsite(d);
    battery_trigger_callsite(d, true);    // box ticked
    // The negative control at the SAME layer: without it, hard-coding the flag
    // to `true` in ExecuteTriggerScript passes every other check in this file.
    battery_trigger_callsite(d, false);   // box clear
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
    // The MXP element / entity / colour tables are loaded by
    // CMUSHclientApp::InitInstance (MUSHclient.cpp:743), which we deliberately
    // run before -- so without this, MXP recognises no tags at all and the
    // call-site battery below would silently observe nothing happening.
    App.MXP_LoadElements ();
    App.MXP_LoadEntities ();
    App.MXP_LoadColours  ();

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
