// platform/engines_selftest.cpp — in-app conformance test for the five embedded
// scripting engines (--engines-selftest).
//
// The engines can only be tested here.  They live in mushgui_adapters, not
// mushcore, because each drags in a whole interpreter (libpython, libperl,
// JavaScriptCore, libtcl, libruby) and the 31 headless test executables must not
// grow those; so the pattern the PAL selftests established — a hidden flag that
// runs a battery inside the real app binary and exits before the GUI boots —
// is what covers them.  Registered with ctest alongside the PAL selftests.
//
// What it pins:
//
//   1. The simple-callback result contract.  ExecutePluginScriptRtn passes ONE
//      CString as both the argument and the result (OnPluginPacketReceived,
//      OnPluginCommandChanged, …), so an engine that writes the result
//      unconditionally destroys the caller's data whenever the script returns
//      nothing.  Tcl did exactly that: every MUD packet and every typed command
//      arrived blank in a Tcl world with a plugin loaded.
//
//   2. Byte transparency.  MUD text is frequently not valid UTF-8 (Latin-1
//      worlds, raw bytes caught by a trigger wildcard).  JavaScriptCore's
//      JSStringCreateWithUTF8CString answers an EMPTY string for such input,
//      which silently blanked trigger lines and wildcards.
//
//   3. A null dispatch reaches the script as its language's "nothing".  Perl
//      blessed the null pointer instead, handing back a reference that looks
//      live and crashes on first use.

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
#include <string>

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* lang, const char* what)
{
    printf("  [%s] %-6s %s\n", ok ? "PASS" : "FAIL", lang, what);
    (ok ? g_pass : g_fail)++;
}

// Drive one engine end to end: build a doc, open the interpreter for `language`,
// compile `code`, then call `proc` through the simple-callback overload the
// plugin string-returning callbacks use.
struct Engine {
    CMUSHclientDoc* doc = nullptr;
    CScriptEngine*  eng = nullptr;

    bool open(const char* language, const char* code)
    {
        doc = new CMUSHclientDoc();
        eng = new CScriptEngine(doc, language);
        eng->CreateScriptEngine();
        if (!eng->IsLua()  && !eng->IsPython() && !eng->IsPerl() &&
            !eng->IsJS()   && !eng->IsTcl()    && !eng->IsRuby())
            return false;                       // engine not built in
        return !eng->Parse(code, "selftest");    // Parse: true == error
    }

    ~Engine() { delete eng; delete doc; }

    // Returns the result string the engine left behind, given `in` as both the
    // argument and the initial result (ExecutePluginScriptRtn's shape).
    CString callRtn(const char* proc, const CString& in)
    {
        DISPID dispid = eng->GetDispid(proc);
        long   count  = 0;
        CString io = in;
        eng->ExecuteLua(dispid, proc, eDontChangeAction, "selftest", "selftest",
                        io, count, io);
        return io;
    }
};

// The shared battery: every engine must agree on these, whatever its syntax.
//   keep(s)    -- returns nothing
//   change(s)  -- returns a fixed string
//   wrap(s)    -- returns "<" + s + ">"
//   size(s)    -- returns the argument's byte length as a string
//
// wrap rather than a plain echo on purpose: if an engine fails to convert its
// result and therefore leaves the caller's string untouched, an echo test still
// "passes".  The brackets make that failure visible.
void run_battery(const char* lang, const char* language, const char* code)
{
    Engine e;
    if (!e.open(language, code)) {
        printf("  [SKIP] %-6s engine not available in this build\n", lang);
        return;
    }

    check(e.callRtn("keep", "original text") == "original text", lang,
          "a callback that returns nothing leaves the caller's string alone");
    check(e.callRtn("change", "original text") == "replaced", lang,
          "a callback that returns a string replaces the caller's string");
    check(e.callRtn("wrap", "round trip") == "<round trip>", lang,
          "an argument comes back through the script unchanged");

    // A Latin-1 byte (0xE9, e-acute in CP1252) is not valid UTF-8 on its own.
    // The engine must still see three characters, not zero.
    CString latin1;
    latin1 = "a\xE9z";
    check(e.callRtn("size", latin1) == "3", lang,
          "a non-UTF-8 argument arrives with all its bytes");
    CString wrapped;
    wrapped = "<a\xE9z>";
    check(e.callRtn("wrap", latin1) == wrapped, lang,
          "a non-UTF-8 argument survives the round trip byte for byte");
}

} // namespace

int RunEmbeddedEnginesSelfTest()
{
    printf("ENGINES-SELFTEST: the five embedded interpreters\n");

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

    // Lua is the reference implementation — it has always had the contract right,
    // so it is the control for the other four.
    run_battery("Lua", "Lua",
        "function keep(s) end\n"
        "function change(s) return 'replaced' end\n"
        "function wrap(s) return '<' .. s .. '>' end\n"
        "function size(s) return tostring(#s) end\n");

    run_battery("Python", "Python",
        "def keep(s):\n    pass\n"
        "def change(s):\n    return 'replaced'\n"
        "def wrap(s):\n    return '<' + s + '>'\n"
        "def size(s):\n    return str(len(s.encode('utf-8', 'surrogateescape')))\n");

    run_battery("Perl", "perl",
        "sub keep { return; }\n"
        "sub change { return 'replaced'; }\n"
        "sub wrap { return '<' . $_[0] . '>'; }\n"
        "sub size { return length($_[0]); }\n");

    run_battery("JS", "javascript",
        "function keep(s) { }\n"
        "function change(s) { return 'replaced'; }\n"
        "function wrap(s) { return '<' + s + '>'; }\n"
        "function size(s) { return String(s.length); }\n");

    run_battery("Tcl", "tcl",
        "proc keep {s} { }\n"
        "proc change {s} { return replaced }\n"
        "proc wrap {s} { return \"<$s>\" }\n"
        "proc size {s} { return [string length $s] }\n");

    run_battery("Ruby", "ruby",
        "def keep(s); nil; end\n"
        "def change(s); 'replaced'; end\n"
        "def wrap(s); '<' + s + '>'; end\n"
        "def size(s); s.bytesize.to_s; end\n");

    // ── a null dispatch is the language's "nothing", never a live-looking handle
    {
        Engine e;
        if (e.open("perl",
                   "sub probe { my $w = $world->GetWorld('no such world at all');\n"
                   "            return defined($w) ? 'defined' : 'undef'; }\n"))
            check(e.callRtn("probe", "") == "undef", "Perl",
                  "GetWorld on a missing world is undef, not a blessed null");
        else
            printf("  [SKIP] Perl   engine not available in this build\n");
    }

    printf("ENGINES-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

#endif // !_WIN32
