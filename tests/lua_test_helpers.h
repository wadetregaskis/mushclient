// tests/lua_test_helpers.h
// Shared Lua-test scaffolding (audit §2/§4): make_lua(), run(), run_ret(), the
// LuaDoc RAII fixture and the EXPECT_LUA_OK macro were copy-pasted verbatim across
// test_lua_scripting.cpp and test_lua_surface.cpp.  Hoisted here so there is a single
// source of truth for the marshalling fidelity across those suites.
//
// The including TU must ALREADY have pulled in (in this order):
//   mc_test.h, stdafx.h, MUSHclient.h, doc.h, scripting/scripting.h
//   and the Lua headers inside an  extern "C" { ... }  block.
// It must be compiled under MUSHCLIENT_LUA_SCRIPTING.

#ifndef MUSHCLIENT_TESTS_LUA_TEST_HELPERS_H
#define MUSHCLIENT_TESTS_LUA_TEST_HELPERS_H

#include <cstdio>

// Provided by the real engine (also declared in scripting/scripting.h).
lua_State* MakeLuaState();
int RegisterLuaRoutines(lua_State*);
extern "C" int luaopen_compress(lua_State*);   // utils.md5/sha256/tohex/... (lua_compress.c)

// A fresh Lua state bound to pDoc, registered exactly as OpenLuaDelayed does.
// static inline so an unused copy in any TU raises no -Wunused-function warning.
static inline lua_State* make_lua(CMUSHclientDoc* pDoc)
{
    lua_State* L = MakeLuaState();
    if (!L) return nullptr;
    luaL_openlibs(L);
    // Bind the doc pointer into the Lua registry (mirrors OpenLuaDelayed)
    lua_pushlightuserdata(L, static_cast<void*>(pDoc));
    lua_setfield(L, LUA_REGISTRYINDEX, DOCUMENT_STATE);
    RegisterLuaRoutines(L);
    luaopen_compress(L);            // mirrors OpenLuaDelayed (lua_scripting.cpp:203)
    lua_settop(L, 0);
    return L;
}

// Run a Lua chunk; report the Lua error (with a marker) and return false on failure.
static inline bool run(lua_State* L, const char* code)
{
    if (luaL_loadstring(L, code) != 0) {
        fprintf(stderr, "  Lua load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        fprintf(stderr, "  Lua runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Run a Lua chunk expected to succeed, leaving results on the stack.
// Returns the number of results (or -1 on load/runtime error).
static inline int run_ret(lua_State* L, const char* code)
{
    int top = lua_gettop(L);
    if (luaL_loadstring(L, code) != 0) {
        fprintf(stderr, "  Lua load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        fprintf(stderr, "  Lua runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return lua_gettop(L) - top;
}

// RAII Lua+Doc pair: close the state in the dtor so an early ASSERT return no
// longer leaks the lua_State (the reason to prefer this over the hand-rolled
// make_lua()+lua_close() prologue).
struct LuaDoc {
    CMUSHclientDoc doc;
    lua_State* L;
    LuaDoc() { L = make_lua(&doc); }
    ~LuaDoc() { if (L) lua_close(L); }
};

// Run a chunk that ends in `return true`; any embedded assert() failure
// surfaces as a Lua error (run_ret returns -1).
#define EXPECT_LUA_OK(L, code) \
    do { \
        int n_ = run_ret((L), (code)); \
        EXPECT_EQ(1, n_); \
        if (n_ == 1) EXPECT_TRUE(lua_toboolean((L), -1)); \
        lua_settop((L), 0); \
    } while (0)

#endif // MUSHCLIENT_TESTS_LUA_TEST_HELPERS_H
