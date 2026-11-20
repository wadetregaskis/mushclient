// tests/test_rex.cpp
// Lua's "rex" library (scripting/lrexlib.c) as a plugin sees it: PCRE 8's flag
// numbers and results, over PCRE2.

#ifdef MUSHCLIENT_LUA_SCRIPTING

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "scripting/scripting.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
int luaopen_rex(lua_State*);
}

#include <cstring>
#include <string>

namespace {

struct Rex {
    lua_State* L;
    Rex() {
        L = luaL_newstate();
        luaL_openlibs(L);
        lua_pushcfunction(L, luaopen_rex);
        lua_call(L, 0, 0);
        lua_settop(L, 0);
    }
    ~Rex() { lua_close(L); }

    // Run a chunk, and give what it returns as a string.  "error: ..." if it fails.
    std::string eval(const char* code) {
        lua_settop(L, 0);
        if (luaL_loadstring(L, code) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
            std::string e = std::string("error: ") + lua_tostring(L, -1);
            lua_settop(L, 0);
            return e;
        }
        const char* s = lua_tostring(L, -1);
        std::string r = s ? s : "nil";
        lua_settop(L, 0);
        return r;
    }
};

}  // namespace

// A plugin may hold any of these numbers.
TEST(Rex, FlagsAreStillPcre8s) {
    Rex x;
    EXPECT_EQ(std::string("1 2 4 8 16 32 64 128 256 512 1024 2048 16384 4096 32768 262144 524288"),
              x.eval("local f = rex.flags() return table.concat({ f.CASELESS, f.MULTILINE, f.DOTALL, f.EXTENDED, "
                     "f.ANCHORED, f.DOLLAR_ENDONLY, f.EXTRA, f.NOTBOL, f.NOTEOL, f.UNGREEDY, f.NOTEMPTY, f.UTF8, "
                     "f.AUTO_CALLOUT, f.NO_AUTO_CAPTURE, f.PARTIAL, f.FIRSTLINE, f.DUPNAMES }, ' ')"));
    EXPECT_EQ(std::string("1048576 2097152 3145728 4194304 5242880"),
              x.eval("local f = rex.flags() return table.concat({ f.NEWLINE_CR, f.NEWLINE_LF, f.NEWLINE_CRLF, "
                     "f.NEWLINE_ANY, f.NEWLINE_ANYCRLF }, ' ')"));
}

TEST(Rex, NumbersWorkAsFlags) {
    Rex x;
    EXPECT_EQ(std::string("2 4"), x.eval("local s, e = rex.new('abc', 1):match('xABC') return s .. ' ' .. e"));
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('abc'):match('xABC'))"));
    EXPECT_EQ(std::string("1"), x.eval("return (rex.new('a.b', rex.flags().DOTALL):match('a\\nb'))"));
    EXPECT_EQ(std::string("3"), x.eval("return (rex.new('^b', rex.flags().MULTILINE):match('a\\nb'))"));
    EXPECT_EQ(std::string("1"), x.eval("return (rex.new('a b', rex.flags().EXTENDED):match('ab'))"));
    EXPECT_EQ(std::string("1"), x.eval("return select(2, rex.new('a+', rex.flags().UNGREEDY):match('aaa'))"));
    // match options
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('^a'):match('a', 1, rex.flags().NOTBOL))"));
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('a$'):match('a', 1, rex.flags().NOTEOL))"));
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('a*'):match('b', 1, rex.flags().NOTEMPTY))"));
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('abc'):match('ab', 1, rex.flags().PARTIAL))"));
    // what isn't an option of that kind
    EXPECT_NE(std::string::npos, x.eval("return rex.new('a', rex.flags().NOTBOL)").find("unknown option"));
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('a'):match('a', 1, rex.flags().CASELESS))"));
}

TEST(Rex, ExtraIsStrict) {
    Rex x;
    EXPECT_EQ(std::string("1"), x.eval("return (rex.new('\\\\j'):match('j'))"));
    EXPECT_NE(std::string::npos, x.eval("return rex.new('\\\\j', rex.flags().EXTRA)").find("error:"));
    // ... and a pattern with no such thing is unaffected by it
    EXPECT_EQ(std::string("1"), x.eval("return (rex.new('j', rex.flags().EXTRA):match('j'))"));
}

TEST(Rex, MatchAndExecResults) {
    Rex x;
    EXPECT_EQ(std::string("2 4 b d false e"),
              x.eval("local s, e, t = rex.new('(b)c?(d)|(z)'):match('abcde') "
                     "return table.concat({ s, e, t[1], t[2], tostring(t[3]), 'e' }, ' ')"));
    EXPECT_EQ(std::string("1 3 1 1 3 3 false false"),
              x.eval("local s, e, t = rex.new('(a)b(c)|(z)'):exec('abc') "
                     "return table.concat({ s, e, t[1], t[2], t[3], t[4], tostring(t[5]), tostring(t[6]) }, ' ')"));
    // named, and a name that took no part
    EXPECT_EQ(std::string("a false"),
              x.eval("local s, e, t = rex.new('(?<one>a)|(?<two>b)'):match('a') "
                     "return tostring(t.one) .. ' ' .. tostring(t.two)"));
    // duplicates
    EXPECT_EQ(std::string("b"),
              x.eval("local s, e, t = rex.new('(?J)(?<n>a)|(?<n>b)'):match('b') return tostring(t.n)"));
    // a start position, and from the end
    EXPECT_EQ(std::string("3"), x.eval("return (rex.new('a'):match('aba', 2))"));
    EXPECT_EQ(std::string("3"), x.eval("return (rex.new('a'):match('aba', -1))"));
    // past the end: no match, no error
    EXPECT_EQ(std::string("nil"), x.eval("return (rex.new('a'):match('aba', 99))"));
}

TEST(Rex, Gmatch) {
    Rex x;
    EXPECT_EQ(std::string("3 a:1,b:2,c:3,"),
              x.eval("local r = '' local n = rex.new('(\\\\w):(\\\\d)'):gmatch('a:1 b:2 c:3', "
                     "function (m, t) r = r .. t[1] .. ':' .. t[2] .. ',' end) return n .. ' ' .. r"));
    EXPECT_EQ(std::string("2"),
              x.eval("return rex.new('a'):gmatch('aaaa', function () end, 2)"));
    EXPECT_EQ(std::string("1"),
              x.eval("return rex.new('a'):gmatch('aaaa', function () return true end)"));
}

// PCRE 8's capture_last was -1 for none: PCRE2's is 0.
TEST(Rex, CalloutsSeeWhatTheyAlwaysSaw) {
    Rex x;
    EXPECT_EQ(std::string("1 |1 1|ab 1 2 1 1;2 |1 1 2 2|ab 1 3 2 2;"),
              x.eval("local r = '' "
                     "local s = rex.new('(a)(?C1)(b)(?C2)'):match('ab', 1, 0, function (n, ov, subj, start, cur, top, last) "
                     "  r = r .. n .. ' |' .. table.concat(ov, ' ') .. '|' .. subj .. ' ' .. start .. ' ' .. cur .. ' ' .. top .. ' ' .. last .. ';' "
                     "  return 0 end) return r"));
    EXPECT_EQ(std::string("7 0 -1"),
              x.eval("local r "
                     "rex.new('(?C7)a'):match('a', 1, 0, function (n, ov, subj, start, cur, top, last) r = n .. ' ' .. top .. ' ' .. last return 0 end) "
                     "return r"));
    // an unset group among them is false, false
    EXPECT_EQ(std::string("false false 1 1"),
              x.eval("local r "
                     "rex.new('(z)?(b)(?C1)'):match('b', 1, 0, function (n, ov) r = tostring(ov[1]) .. ' ' .. tostring(ov[2]) .. ' ' .. tostring(ov[3]) .. ' ' .. tostring(ov[4]) return 0 end) "
                     "return r"));
    // returning > 0 fails that point; < 0 fails the match
    EXPECT_EQ(std::string("nil"),
              x.eval("return (rex.new('a(?C1)'):match('a', 1, 0, function () return 1 end))"));
    EXPECT_EQ(std::string("nil"),
              x.eval("return (rex.new('a(?C1)|b'):match('b', 1, 0, function () return -1 end) and rex.new('a(?C1)'):match('a', 1, 0, function () return -1 end))"));
    // gmatch has it as argument 6
    EXPECT_EQ(std::string("3 3"),
              x.eval("local c = 0 local n = rex.new('a(?C1)'):gmatch('aaa', function () end, 0, 0, function () c = c + 1 return 0 end) return n .. ' ' .. c"));
    // AUTO_CALLOUT: number 255
    EXPECT_EQ(std::string("255"),
              x.eval("local r rex.new('a', rex.flags().AUTO_CALLOUT):match('a', 1, 0, function (n) r = n return 0 end) return r"));
}

TEST(Rex, ACalloutMayRunTheSamePattern) {
    Rex x;
    EXPECT_EQ(std::string("1 3 1 3"),
              x.eval("local re = rex.new('(\\\\w)(?C1)(\\\\w)(\\\\w)') "
                     "local inner_s, inner_e "
                     "local depth = 0 "
                     "local s, e = re:match('abc', 1, 0, function () "
                     "  depth = depth + 1 "
                     "  if depth == 1 then inner_s, inner_e = re:match('abc') end "
                     "  return 0 end) "
                     "return s .. ' ' .. e .. ' ' .. inner_s .. ' ' .. inner_e"));
}

// Another pattern, matched from the callout, must not disturb the match paused in
// it.  Both are JIT-compiled, and every JIT match starts at the top of the thread's
// one JIT stack: the inner one's frames would land on the outer one's.  The outer
// pattern has to backtrack through those frames after the callout to show it.
TEST(Rex, ACalloutMayRunAnotherPattern) {
    Rex x;
    EXPECT_EQ(std::string("1 9 b"),
              x.eval("local outer = rex.new('^(?:(a+)(?C1)|(a+)b)+$') "
                     "local inner = rex.new('(?:x|y|(z))+w') "
                     "local s, e, t = outer:match('aaaaaaaab', 1, 0, function () "
                     "  inner:match(('xyz'):rep(40) .. 'q') "
                     "  return 0 end) "
                     "return s .. ' ' .. e .. ' ' .. string.sub('aaaaaaaab', e, e)"));
}

TEST(Rex, ACalloutMayRaiseAnError) {
    Rex x;
    EXPECT_NE(std::string::npos,
              x.eval("rex.new('a(?C1)'):match('a', 1, 0, function () error('deliberate') end)").find("deliberate"));
    // the pattern can still be used
    EXPECT_EQ(std::string("1 1"),
              x.eval("local re = rex.new('a(?C1)') "
                     "pcall(function () re:match('a', 1, 0, function () error('deliberate') end) end) "
                     "local s, e = re:match('a') return s .. ' ' .. e"));
}

TEST(Rex, VersionAndErrors) {
    Rex x;
    EXPECT_EQ(0u, x.eval("return rex.version()").find("10."));
    EXPECT_NE(std::string::npos, x.eval("return rex.new('(a')").find("(pattern offset: "));
    EXPECT_EQ(std::string("1"), x.eval("return (rex.new('\\\\w', 0, 'C'):match('a'))"));
    EXPECT_NE(std::string::npos, x.eval("return tostring(rex.new('a'))").find("pcre_regex"));
}

TEST(Rex, CompatibilityIsTheSameAsForTriggers) {
    Rex x;
    EXPECT_EQ(std::string("1 2"), x.eval("local s, e = rex.new('[\\\\w-_]+'):match('a-!') return s .. ' ' .. e"));
    EXPECT_EQ(std::string("1 5"), x.eval("local s, e = rex.new('x{,3}'):match('x{,3}') return s .. ' ' .. e"));
}

MC_TEST_MAIN()

#else  // !MUSHCLIENT_LUA_SCRIPTING

int main() { return 0; }

#endif
