// tests/test_database.cpp
// Tests for the world.Database* family (scripting/methods/methods_database.cpp
// plus the Lua-specific column glue in scripting/lua_methods.cpp).
//
// The whole family funnels through a per-doc map of named tDatabase handles;
// expected return codes are the SQLite result codes plus the MUSHclient
// negative error codes documented at the top of methods_database.cpp:
//   -1 unknown database ID          -5 have not stepped to a valid row
//   -2 not opened                   -6 same ID already open on another file
//   -3 already have a prepared stmt -7 requested column out of range
//   -4 no prepared statement yet
// SQLite codes asserted here: OK=0, ERROR=1, CANTOPEN=14, ROW=100, DONE=101.

#ifdef MUSHCLIENT_LUA_SCRIPTING

#include "mc_test.h"
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "scripting/scripting.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstring>
#include <cstdio>
#include <string>
#include <sys/resource.h>   // getrusage (RSS leak check)

lua_State* MakeLuaState();
int RegisterLuaRoutines(lua_State*);

// The bundled Lua `sqlite3` module (lsqlite/lsqlite3.c, compiled into mushcore).
// OpenLuaDelayed opens it for every world; make_lua below deliberately does not,
// so the one test that needs it opens it itself.
extern "C" int luaopen_lsqlite3(lua_State*);

static lua_State* make_lua(CMUSHclientDoc* pDoc)
{
    lua_State* L = MakeLuaState();
    if (!L) return nullptr;
    luaL_openlibs(L);
    lua_pushlightuserdata(L, static_cast<void*>(pDoc));
    lua_setfield(L, LUA_REGISTRYINDEX, DOCUMENT_STATE);
    RegisterLuaRoutines(L);
    lua_settop(L, 0);
    return L;
}

static bool run(lua_State* L, const char* code)
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

// Run a chunk returning one value; leave it at stack top.  -1 on error.
static int run_ret(lua_State* L, const char* code)
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

// Expect the chunk to return a single number equal to `expected`.
#define EXPECT_LUA_NUM(L, expected, code) \
    do { \
        int n_ = run_ret((L), (code)); \
        EXPECT_EQ(1, n_); \
        if (n_ == 1) { \
            EXPECT_TRUE(lua_isnumber((L), -1)); \
            EXPECT_NEAR((double)(expected), lua_tonumber((L), -1), 1e-9); \
        } \
        lua_settop((L), 0); \
    } while (0)

// Fixture: doc + lua with an open in-memory database "db" holding table
// t(id INTEGER PRIMARY KEY, name TEXT, weight REAL, notes) with two rows:
//   (1, 'alpha', 1.5, NULL)  (2, 'beta', 2.25, 'x')
struct DbFixture {
    CMUSHclientDoc doc;
    lua_State* L = nullptr;
    DbFixture()
    {
        L = make_lua(&doc);
        if (!L) return;
        run(L, "assert(world.DatabaseOpen('db', ':memory:', 6) == 0)");
        run(L, "assert(world.DatabaseExec('db', "
               "[[CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, weight REAL, notes);"
               "INSERT INTO t (id, name, weight, notes) VALUES (1, 'alpha', 1.5, NULL);"
               "INSERT INTO t (id, name, weight, notes) VALUES (2, 'beta', 2.25, 'x');]]) == 0)");
    }
    ~DbFixture() { if (L) lua_close(L); }
};

// ── DatabaseOpen ─────────────────────────────────────────────────────────

TEST(DatabaseTest, OpenMemoryDatabaseReturnsOk)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_NUM(L, 0, "return world.DatabaseOpen('db', ':memory:', 6)");
    EXPECT_EQ((size_t)1, doc.m_Databases.size());
    lua_close(L);
}

TEST(DatabaseTest, ReopenSameIdSameFileIsOkAndDoesNotDuplicate)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabaseOpen('db', ':memory:', 6)");
    EXPECT_EQ((size_t)1, f.doc.m_Databases.size());
}

TEST(DatabaseTest, ReopenSameIdDifferentFileReturnsAlreadyExists)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, -6, "return world.DatabaseOpen('db', 'other.db', 6)");
}

TEST(DatabaseTest, OpenFailureDoesNotRegisterDatabase)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    // Flags 2 = SQLITE_OPEN_READWRITE without CREATE on a nonexistent path
    // → SQLITE_CANTOPEN (14), and the handle must not be added to the map.
    EXPECT_LUA_NUM(L, 14,
        "return world.DatabaseOpen('bad', '/nonexistent_mc_dir/x.db', 2)");
    EXPECT_EQ((size_t)0, doc.m_Databases.size());
    // Consequently the id remains unknown:
    EXPECT_LUA_NUM(L, -1, "return world.DatabaseClose('bad')");
    lua_close(L);
}

// sqlite3_open_v2 allocates a connection handle even when it fails (every
// case but out-of-memory) -- the caller must sqlite3_close it regardless.
// DatabaseOpen's failure branch used to just `delete pDatabase`, dropping
// the sqlite3* it had already been handed and leaking libsqlite3's internal
// state once per failed open. Same-name-never-registered means every call
// below re-takes the "not found, allocate + fail to open" path, so a
// reintroduced leak shows up as RSS growth -- same shape as
// LuaScripting.StringReturningMethodsDoNotLeakTheirVariant.
TEST(DatabaseTest, OpenFailureClosesTheHandleSqliteAllocated)
{
    CMUSHclientDoc doc;

    auto rss = [] () -> long {
        // ru_maxrss is a high-water mark, which is what we want: it cannot
        // fall back and hide a leak that has already happened.
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (long) ru.ru_maxrss;      // bytes on macOS, KB on Linux
    };

    auto open_failing = [&] () {
        doc.DatabaseOpen("bad", "/nonexistent_mc_dir/x.db", 2);
    };

    for (int i = 0; i < 500; i++)        // warm up the allocator
        open_failing();

    const long before = rss();
    for (int i = 0; i < 20000; i++)
        open_failing();
    const long after = rss();

    EXPECT_EQ((size_t)0, doc.m_Databases.size());   // still never registered

#ifdef __APPLE__
    const long budget = 2L * 1024 * 1024;      // bytes
#else
    const long budget = 2L * 1024;             // KB
#endif
    EXPECT_LT(after - before, budget);
}

// ── DatabaseClose ────────────────────────────────────────────────────────

TEST(DatabaseTest, CloseUnknownIdReturnsIdNotFound)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_NUM(L, -1, "return world.DatabaseClose('nope')");
    lua_close(L);
}

TEST(DatabaseTest, CloseRemovesIdSoSecondCloseFails)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabaseClose('db')");
    EXPECT_EQ((size_t)0, f.doc.m_Databases.size());
    EXPECT_LUA_NUM(f.L, -1, "return world.DatabaseClose('db')");
}

TEST(DatabaseTest, CloseFinalizesOutstandingStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT * FROM t') == 0)");
    // close must finalize the live statement and still succeed
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabaseClose('db')");
}

// ── DatabasePrepare / DatabaseFinalize ───────────────────────────────────

TEST(DatabaseTest, PrepareUnknownIdReturnsIdNotFound)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_NUM(L, -1, "return world.DatabasePrepare('nope', 'SELECT 1')");
    lua_close(L);
}

TEST(DatabaseTest, PrepareTwiceReturnsHavePreparedStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, 0,  "return world.DatabasePrepare('db', 'SELECT id FROM t')");
    EXPECT_LUA_NUM(f.L, -3, "return world.DatabasePrepare('db', 'SELECT id FROM t')");
}

TEST(DatabaseTest, PrepareSyntaxErrorReturnsSqliteError)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, 1, "return world.DatabasePrepare('db', 'NOT REAL SQL')");
    // failed prepare leaves no statement behind:
    EXPECT_LUA_NUM(f.L, -4, "return world.DatabaseFinalize('db')");
}

TEST(DatabaseTest, FinalizeWithoutPrepareReturnsNoPreparedStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, -4, "return world.DatabaseFinalize('db')");
}

TEST(DatabaseTest, FinalizeClearsStatementStateForReuse)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT * FROM t') == 0)");
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabaseFinalize('db')");
    // a fresh prepare is allowed again
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabasePrepare('db', 'SELECT id FROM t')");
}

// ── DatabaseColumns ──────────────────────────────────────────────────────

TEST(DatabaseTest, ColumnsAvailableStraightAfterPrepare)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id, name, weight FROM t') == 0)");
    EXPECT_LUA_NUM(f.L, 3, "return world.DatabaseColumns('db')");
}

TEST(DatabaseTest, ColumnsWithoutStatementReturnsNoPreparedStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, -4, "return world.DatabaseColumns('db')");
}

// ── DatabaseStep ─────────────────────────────────────────────────────────

TEST(DatabaseTest, StepReturnsRowThenDone)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t ORDER BY id') == 0)");
    EXPECT_LUA_NUM(f.L, 100, "return world.DatabaseStep('db')");  // row 1
    EXPECT_LUA_NUM(f.L, 100, "return world.DatabaseStep('db')");  // row 2
    EXPECT_LUA_NUM(f.L, 101, "return world.DatabaseStep('db')");  // done
}

TEST(DatabaseTest, StepWithoutPrepareReturnsNoPreparedStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, -4, "return world.DatabaseStep('db')");
}

// ── DatabaseReset ────────────────────────────────────────────────────────

TEST(DatabaseTest, ResetRewindsToFirstRow)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t ORDER BY id') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabaseReset('db')");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    // back on row 1:
    EXPECT_LUA_NUM(f.L, 1, "return world.DatabaseColumnValue('db', 1)");
}

TEST(DatabaseTest, ResetWithoutStatementReturnsNoPreparedStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, -4, "return world.DatabaseReset('db')");
}

// ── DatabaseError ────────────────────────────────────────────────────────

TEST(DatabaseTest, ErrorUnknownIdSaysIdNotFound)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L, "return world.DatabaseError('nope')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("database id not found", lua_tostring(L, -1));
    lua_close(L);
}

TEST(DatabaseTest, ErrorAfterStepRowSaysRowReady)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    int n = run_ret(f.L, "return world.DatabaseError('db')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("row ready", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ErrorAfterDoneSaysFinished)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t WHERE id > 90') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 101)");
    int n = run_ret(f.L, "return world.DatabaseError('db')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("finished", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ErrorAfterSyntaxErrorContainsSqliteMessage)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'NOT REAL SQL') == 1)");
    int n = run_ret(f.L, "return string.find(world.DatabaseError('db'), 'syntax error') ~= nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

// ── DatabaseColumnName / DatabaseColumnNames ─────────────────────────────

TEST(DatabaseTest, ColumnNameIsOneBased)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id, name FROM t') == 0)");
    int n = run_ret(f.L, "return world.DatabaseColumnName('db', 1), world.DatabaseColumnName('db', 2)");
    ASSERT_EQ(2, n);
    EXPECT_STREQ("id",   lua_tostring(f.L, -2));
    EXPECT_STREQ("name", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnNameOutOfRangeReturnsEmptyString)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    int n = run_ret(f.L, "return world.DatabaseColumnName('db', 0), world.DatabaseColumnName('db', 2)");
    ASSERT_EQ(2, n);
    EXPECT_STREQ("", lua_tostring(f.L, -2));   // below range
    EXPECT_STREQ("", lua_tostring(f.L, -1));   // above range
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnNamesReturnsTableOfAllNames)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id, name, weight FROM t') == 0)");
    int n = run_ret(f.L,
        "local t = world.DatabaseColumnNames('db')\n"
        "return #t, t[1], t[2], t[3]");
    ASSERT_EQ(4, n);
    EXPECT_EQ(3.0, lua_tonumber(f.L, -4));
    EXPECT_STREQ("id",     lua_tostring(f.L, -3));
    EXPECT_STREQ("name",   lua_tostring(f.L, -2));
    EXPECT_STREQ("weight", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnNamesWithoutStatementReturnsNoNames)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    // Windows contract: a never-created COleSafeArray detaches as VT_EMPTY →
    // Lua nil (the shim's Detach now matches MFC).
    int n = run_ret(f.L,
        "local t = world.DatabaseColumnNames('db')\n"
        "return t == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

// ── DatabaseColumnText / DatabaseColumnValue (Lua glue) ──────────────────

TEST(DatabaseTest, ColumnTextReturnsTextOfCurrentRow)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT name FROM t ORDER BY id') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    int n = run_ret(f.L, "return world.DatabaseColumnText('db', 1)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("alpha", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnTextBeforeStepReturnsNil)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT name FROM t') == 0)");
    int n = run_ret(f.L, "return world.DatabaseColumnText('db', 1) == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnTextCoercesNumericColumnToString)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t ORDER BY id') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    int n = run_ret(f.L, "return world.DatabaseColumnText('db', 1)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("1", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnValueTypesRoundTrip)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', "
             "'SELECT id, name, weight, notes FROM t ORDER BY id') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    int n = run_ret(f.L,
        "return world.DatabaseColumnValue('db', 1), "
        "world.DatabaseColumnValue('db', 2), "
        "world.DatabaseColumnValue('db', 3), "
        "world.DatabaseColumnValue('db', 4)");
    ASSERT_EQ(4, n);
    EXPECT_EQ(1.0, lua_tonumber(f.L, -4));          // INTEGER → number
    EXPECT_STREQ("alpha", lua_tostring(f.L, -3));   // TEXT → string
    EXPECT_NEAR(1.5, lua_tonumber(f.L, -2), 1e-9);  // REAL → number
    EXPECT_TRUE(lua_isnil(f.L, -1));                // NULL → nil
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnValueOutOfRangeReturnsNil)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    int n = run_ret(f.L, "return world.DatabaseColumnValue('db', 5) == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnValuesReturnsWholeRowWithNullHole)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', "
             "'SELECT id, notes, name FROM t ORDER BY id') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    int n = run_ret(f.L,
        "local t = world.DatabaseColumnValues('db')\n"
        "return t[1], t[2] == nil, t[3]");
    ASSERT_EQ(3, n);
    EXPECT_EQ(1.0, lua_tonumber(f.L, -3));
    EXPECT_TRUE(lua_toboolean(f.L, -2));       // NULL column → hole
    EXPECT_STREQ("alpha", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, ColumnValuesBeforeStepReturnsNil)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    int n = run_ret(f.L, "return world.DatabaseColumnValues('db') == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

// ── DatabaseColumnType ───────────────────────────────────────────────────

TEST(DatabaseTest, ColumnTypeReportsSqliteTypeCodes)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', "
             "'SELECT id, weight, name, notes FROM t ORDER BY id') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    EXPECT_LUA_NUM(f.L, 1, "return world.DatabaseColumnType('db', 1)");  // INTEGER
    EXPECT_LUA_NUM(f.L, 2, "return world.DatabaseColumnType('db', 2)");  // FLOAT
    EXPECT_LUA_NUM(f.L, 3, "return world.DatabaseColumnType('db', 3)");  // TEXT
    EXPECT_LUA_NUM(f.L, 5, "return world.DatabaseColumnType('db', 4)");  // NULL
}

TEST(DatabaseTest, ColumnTypeBeforeStepReturnsNoValidRow)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    EXPECT_LUA_NUM(f.L, -5, "return world.DatabaseColumnType('db', 1)");
}

TEST(DatabaseTest, ColumnTypeOutOfRangeReturnsColumnOutOfRange)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    EXPECT_LUA_NUM(f.L, -7, "return world.DatabaseColumnType('db', 0)");
    EXPECT_LUA_NUM(f.L, -7, "return world.DatabaseColumnType('db', 2)");
}

// ── Changes / TotalChanges / LastInsertRowid ─────────────────────────────

TEST(DatabaseTest, ChangesReflectsLastStatementTotalChangesAccumulates)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    // fixture did 2 single-row inserts (CREATE TABLE changes nothing)
    EXPECT_LUA_NUM(f.L, 1, "return world.DatabaseChanges('db')");
    EXPECT_LUA_NUM(f.L, 2, "return world.DatabaseTotalChanges('db')");
    run(f.L, "assert(world.DatabaseExec('db', \"UPDATE t SET name = 'z'\") == 0)");
    EXPECT_LUA_NUM(f.L, 2, "return world.DatabaseChanges('db')");       // both rows hit
    EXPECT_LUA_NUM(f.L, 4, "return world.DatabaseTotalChanges('db')");
}

TEST(DatabaseTest, ChangesUnknownIdReturnsIdNotFound)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_NUM(L, -1, "return world.DatabaseChanges('nope')");
    EXPECT_LUA_NUM(L, -1, "return world.DatabaseTotalChanges('nope')");
    lua_close(L);
}

TEST(DatabaseTest, LastInsertRowidIsStringOfLastRowid)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L, "return world.DatabaseLastInsertRowid('db')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("2", lua_tostring(f.L, -1));   // fixture's last insert had id 2
    lua_settop(f.L, 0);
    run(f.L, "assert(world.DatabaseExec('db', "
             "\"INSERT INTO t (id, name) VALUES (40, 'gamma')\") == 0)");
    n = run_ret(f.L, "return world.DatabaseLastInsertRowid('db')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("40", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, LastInsertRowidUnknownIdReturnsEmptyString)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    int n = run_ret(L, "return world.DatabaseLastInsertRowid('nope')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("", lua_tostring(L, -1));
    lua_close(L);
}

// ── DatabaseList / DatabaseInfo ──────────────────────────────────────────

TEST(DatabaseTest, ListEmptyReturnsNoEntries)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    // nil on Windows: no databases → never-created safe array → VT_EMPTY.
    int n = run_ret(L,
        "local t = world.DatabaseList()\n"
        "return t == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_close(L);
}

TEST(DatabaseTest, ListReturnsIdsInMapOrder)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    run(L, "assert(world.DatabaseOpen('zeta',  ':memory:', 6) == 0)");
    run(L, "assert(world.DatabaseOpen('alpha', ':memory:', 6) == 0)");
    // std::map iterates in sorted key order regardless of open order
    int n = run_ret(L, "local t = world.DatabaseList() return #t, t[1], t[2]");
    ASSERT_EQ(3, n);
    EXPECT_EQ(2.0, lua_tonumber(L, -3));
    EXPECT_STREQ("alpha", lua_tostring(L, -2));
    EXPECT_STREQ("zeta",  lua_tostring(L, -1));
    lua_settop(L, 0);
    lua_close(L);
}

TEST(DatabaseTest, InfoReportsDiskNameStatementRowAndColumns)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L, "return world.DatabaseInfo('db', 1)");
    ASSERT_EQ(1, n);
    EXPECT_STREQ(":memory:", lua_tostring(f.L, -1));   // 1 = disk name
    lua_settop(f.L, 0);

    // before prepare: no statement, no valid row, 0 columns
    n = run_ret(f.L, "return world.DatabaseInfo('db', 2), world.DatabaseInfo('db', 3),"
                     " world.DatabaseInfo('db', 4)");
    ASSERT_EQ(3, n);
    EXPECT_FALSE(lua_toboolean(f.L, -3));
    EXPECT_FALSE(lua_toboolean(f.L, -2));
    EXPECT_EQ(0.0, lua_tonumber(f.L, -1));
    lua_settop(f.L, 0);

    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id, name FROM t') == 0)");
    run(f.L, "assert(world.DatabaseStep('db') == 100)");
    n = run_ret(f.L, "return world.DatabaseInfo('db', 2), world.DatabaseInfo('db', 3),"
                     " world.DatabaseInfo('db', 4)");
    ASSERT_EQ(3, n);
    EXPECT_TRUE(lua_toboolean(f.L, -3));    // have statement
    EXPECT_TRUE(lua_toboolean(f.L, -2));    // on a valid row
    EXPECT_EQ(2.0, lua_tonumber(f.L, -1));  // 2 columns
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, InfoUnknownIdOrTypeReturnsNil)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L, "return world.DatabaseInfo('nope', 1) == nil, "
                         "world.DatabaseInfo('db', 99) == nil");
    ASSERT_EQ(2, n);
    EXPECT_TRUE(lua_toboolean(f.L, -2));
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

// ── DatabaseExec ─────────────────────────────────────────────────────────

TEST(DatabaseTest, ExecRejectedWhilePreparedStatementOutstanding)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    run(f.L, "assert(world.DatabasePrepare('db', 'SELECT id FROM t') == 0)");
    EXPECT_LUA_NUM(f.L, -3, "return world.DatabaseExec('db', 'SELECT 1')");
}

TEST(DatabaseTest, ExecSyntaxErrorReturnsSqliteError)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    EXPECT_LUA_NUM(f.L, 1, "return world.DatabaseExec('db', 'NOT REAL SQL')");
}

TEST(DatabaseTest, ExecUnknownIdReturnsIdNotFound)
{
    CMUSHclientDoc doc;
    lua_State* L = make_lua(&doc);
    ASSERT_NE(nullptr, L);
    EXPECT_LUA_NUM(L, -1, "return world.DatabaseExec('nope', 'SELECT 1')");
    lua_close(L);
}

// ── DatabaseGetField ─────────────────────────────────────────────────────

TEST(DatabaseTest, GetFieldReturnsRowOneColumnOne)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L,
        "return world.DatabaseGetField('db', 'SELECT name FROM t ORDER BY id')");
    ASSERT_EQ(1, n);
    EXPECT_STREQ("alpha", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);
    // and the statement was finalized — a fresh prepare is legal:
    EXPECT_LUA_NUM(f.L, 0, "return world.DatabasePrepare('db', 'SELECT 1')");
}

TEST(DatabaseTest, GetFieldNumericAggregate)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L, "return world.DatabaseGetField('db', 'SELECT COUNT(*) FROM t')");
    ASSERT_EQ(1, n);
    EXPECT_EQ(2.0, lua_tonumber(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, GetFieldNoRowsReturnsNil)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L,
        "return world.DatabaseGetField('db', 'SELECT id FROM t WHERE id > 90') == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
}

TEST(DatabaseTest, GetFieldBadSqlReturnsNilAndLeavesNoStatement)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    int n = run_ret(f.L, "return world.DatabaseGetField('db', 'NOT REAL SQL') == nil");
    ASSERT_EQ(1, n);
    EXPECT_TRUE(lua_toboolean(f.L, -1));
    lua_settop(f.L, 0);
    EXPECT_LUA_NUM(f.L, -4, "return world.DatabaseFinalize('db')");
}

// ── Direct C++ (BSTR/VARIANT) variants ───────────────────────────────────
// Lua has its own column glue, so the COM-shaped methods (used by the
// dispatch-based engines) are exercised directly here.  On the port a BSTR
// is a char* copy and VARIANTs use the mfc_ui.h shim.

TEST(DatabaseTest, CppColumnTextReturnsTextAndEmptyOutOfRange)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    f.doc.DatabasePrepare("db", "SELECT name FROM t ORDER BY id");
    f.doc.DatabaseStep("db");
    BSTR s = f.doc.DatabaseColumnText("db", 1);
    EXPECT_STREQ("alpha", (const char*)s);
    SysFreeString(s);
    s = f.doc.DatabaseColumnText("db", 9);      // out of range → empty
    EXPECT_STREQ("", (const char*)s);
    SysFreeString(s);
}

TEST(DatabaseTest, CppColumnValueVariantTypes)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    f.doc.DatabasePrepare("db", "SELECT id, name, weight, notes FROM t ORDER BY id");
    f.doc.DatabaseStep("db");

    VARIANT v = f.doc.DatabaseColumnValue("db", 1);   // INTEGER → VT_I4
    EXPECT_EQ(VT_I4, (int)v.vt);
    EXPECT_EQ(1L, (long)v.lVal);

    v = f.doc.DatabaseColumnValue("db", 2);           // TEXT → VT_BSTR
    EXPECT_EQ(VT_BSTR, (int)v.vt);
    EXPECT_STREQ("alpha", (const char*)v.bstrVal);
    VariantClear(&v);

    v = f.doc.DatabaseColumnValue("db", 3);           // REAL → VT_R8
    EXPECT_EQ(VT_R8, (int)v.vt);
    EXPECT_NEAR(1.5, v.dblVal, 1e-9);

    v = f.doc.DatabaseColumnValue("db", 4);           // SQL NULL → VT_EMPTY
    EXPECT_EQ(VT_EMPTY, (int)v.vt);

    v = f.doc.DatabaseColumnValue("db", 9);           // out of range → VT_NULL
    EXPECT_EQ(VT_NULL, (int)v.vt);
}

TEST(DatabaseTest, CppColumnValuesSafearrayTypes)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    f.doc.DatabasePrepare("db", "SELECT id, name, weight, notes FROM t ORDER BY id");
    f.doc.DatabaseStep("db");
    VARIANT v = f.doc.DatabaseColumnValues("db");
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    ASSERT_TRUE(v.parray != NULL);
    VARIANT* elems = (VARIANT*)v.parray->pvData;
    ASSERT_TRUE(elems != NULL);
    EXPECT_EQ(VT_I4, (int)elems[0].vt);
    EXPECT_EQ(1L, (long)elems[0].lVal);
    EXPECT_EQ(VT_BSTR, (int)elems[1].vt);
    EXPECT_STREQ("alpha", (const char*)elems[1].bstrVal);
    EXPECT_EQ(VT_R8, (int)elems[2].vt);
    EXPECT_NEAR(1.5, elems[2].dblVal, 1e-9);
    // SQL NULL element: ChangeType(VT_NULL) stamps VT_NULL (Windows parity)
    EXPECT_EQ(VT_NULL, (int)elems[3].vt);
    VariantClear(&v);
}

TEST(DatabaseTest, CppColumnValuesWithoutRowIsNotAnArray)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    f.doc.DatabasePrepare("db", "SELECT id FROM t");
    VARIANT v = f.doc.DatabaseColumnValues("db");   // no step yet
    EXPECT_TRUE(v.parray == NULL);                  // empty detach
    VariantClear(&v);
}

TEST(DatabaseTest, CppGetFieldReturnsFirstFieldVariant)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    VARIANT v = f.doc.DatabaseGetField("db", "SELECT name FROM t ORDER BY id DESC");
    EXPECT_EQ(VT_BSTR, (int)v.vt);
    EXPECT_STREQ("beta", (const char*)v.bstrVal);
    VariantClear(&v);
    // statement finalized: fresh prepare allowed
    EXPECT_EQ(0L, f.doc.DatabasePrepare("db", "SELECT 1"));
}

TEST(DatabaseTest, CppGetFieldBadSqlOrNoRowsIsEmptyVariant)
{
    DbFixture f;
    ASSERT_NE(nullptr, f.L);
    VARIANT v = f.doc.DatabaseGetField("db", "NOT REAL SQL");
    EXPECT_EQ(VT_EMPTY, (int)v.vt);
    v = f.doc.DatabaseGetField("db", "SELECT id FROM t WHERE id > 90");
    EXPECT_EQ(VT_EMPTY, (int)v.vt);
}

// ── 64-bit INTEGER columns (all four readers) ────────────────────────────
// SQLite INTEGER is a signed 64-bit value; every reader used to fetch it with
// sqlite3_column_int (32 bits) and truncate.  scripting/db_int64.h now bands it:
//   32-bit range:              the integral form (VT_I4 / Lua number) -- as before;
//   survives (int64)(double)v: a double (VT_R8 / Lua number) -- exact;
//   otherwise:                 a decimal string (BSTR / Lua string) -- lossless.
//
// The middle band is a test on the VALUE, not on its magnitude: it is what the
// bundled lsqlite3 module reaches for (lsqlite/lsqlite3.c:115-121), and the two
// SQLite surfaces the app ships used to disagree about every exactly
// representable value above 2^53 -- 2^60 being the obvious one.
//
// So the fixture below deliberately carries BOTH answers at the SAME magnitude:
// 2^53+1 (odd, rounds) against 2^53+2 (even, exact), and 2^60+1 against 2^60.
// A magnitude rule cannot separate those pairs, which is what stops this suite
// from passing if the round trip is ever quietly replaced by a threshold again.
// Both bands and every boundary are checked through all four readers.

// One row, one column per interesting magnitude.  Column numbers are 1-based
// and are used as-is by the single-column readers.
#define BIG_COL_I32MAX   1
#define BIG_COL_I32MIN   2
#define BIG_COL_P31      3
#define BIG_COL_P40      4
#define BIG_COL_N31      5
#define BIG_COL_P53      6
#define BIG_COL_N53      7
#define BIG_COL_P53P1    8
#define BIG_COL_N53M1    9
#define BIG_COL_P60     10
#define BIG_COL_I64MAX  11
#define BIG_COL_I64MIN  12
#define BIG_COL_P53P2   13
#define BIG_COL_N53M2   14
#define BIG_COL_P60P1   15
#define BIG_COLUMNS     15

struct BigIntFixture {
    CMUSHclientDoc doc;
    lua_State* L = nullptr;
    BigIntFixture()
    {
        L = make_lua(&doc);
        if (!L) return;
        run(L, "assert(world.DatabaseOpen('db', ':memory:', 6) == 0)");
        // NOTE the last value: SQLite's tokenizer reads -9223372036854775808 as
        // unary minus applied to a literal that does NOT fit in an int64, and
        // yields a REAL.  Written as a subtraction it stays an INTEGER --
        // BigIntColumnsAreAllIntegers below is the guard that says so.
        run(L, "assert(world.DatabaseExec('db', [[\n"
               "CREATE TABLE big (i32max INTEGER, i32min INTEGER, p31 INTEGER,\n"
               "  p40 INTEGER, n31 INTEGER, p53 INTEGER, n53 INTEGER,\n"
               "  p53p1 INTEGER, n53m1 INTEGER, p60 INTEGER, i64max INTEGER,\n"
               "  i64min INTEGER, p53p2 INTEGER, n53m2 INTEGER, p60p1 INTEGER);\n"
               "INSERT INTO big VALUES (2147483647, -2147483648, 2147483648,\n"
               "  1099511627776, -2147483649, 9007199254740992,\n"
               "  -9007199254740992, 9007199254740993, -9007199254740993,\n"
               "  1152921504606846976, 9223372036854775807,\n"
               "  -9223372036854775807 - 1,\n"
               "  9007199254740994, -9007199254740994,\n"
               "  1152921504606846977);]]) == 0)");
    }
    ~BigIntFixture() { if (L) lua_close(L); }

    // Prepare + step SELECT * FROM big, leaving the single row current.
    bool row()
    {
        return run(L, "assert(world.DatabasePrepare('db', 'SELECT * FROM big') == 0)")
            && run(L, "assert(world.DatabaseStep('db') == 100)");
    }
};

// Evaluate `fmt` with the column number substituted; leave one value on the
// Lua stack.  Returns false if the chunk did not yield exactly one value.
static bool lua_col(lua_State* L, const char* fmt, int col)
{
    char code[160];
    snprintf(code, sizeof code, fmt, col);
    lua_settop(L, 0);
    return run_ret(L, code) == 1;
}

#define LUA_SINGLE "return world.DatabaseColumnValue('db', %d)"
#define LUA_ROW    "local t = world.DatabaseColumnValues('db') return t[%d]"

// A column that must come back as a Lua NUMBER holding exactly `expected`.
// lua_type, not lua_isnumber: a numeric STRING passes lua_isnumber, so only the
// type tag can tell the two bands apart.
#define EXPECT_LUA_EXACT_NUMBER(L, reader, col, expected) \
    do { \
        ASSERT_TRUE(lua_col((L), (reader), (col))); \
        EXPECT_EQ(LUA_TNUMBER, lua_type((L), -1)); \
        EXPECT_EQ((double)(expected), (double)lua_tonumber((L), -1)); \
        lua_settop((L), 0); \
    } while (0)

// A column that must come back as a Lua STRING holding exactly `expected`.
#define EXPECT_LUA_EXACT_STRING(L, reader, col, expected) \
    do { \
        ASSERT_TRUE(lua_col((L), (reader), (col))); \
        EXPECT_EQ(LUA_TSTRING, lua_type((L), -1)); \
        EXPECT_STREQ((expected), lua_tostring((L), -1)); \
        lua_settop((L), 0); \
    } while (0)

// Guard against a degenerate fixture: if any literal above had landed as REAL
// or TEXT the banding tests would be exercising the wrong code path entirely.
TEST(DatabaseTest, BigIntColumnsAreAllIntegers)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    EXPECT_EQ((long)BIG_COLUMNS, f.doc.DatabaseColumns("db"));
    for (int col = 1; col <= BIG_COLUMNS; col++)
        EXPECT_EQ(1L, f.doc.DatabaseColumnType("db", col));   // SQLITE_INTEGER
}

// (a) still inside 32 bits: still a number, exact, and still the same Lua type.
TEST(DatabaseTest, LuaColumnValueKeepsSmallIntegersAsNumbers)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_I32MAX, 2147483647.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_I32MIN, -2147483648.0);
}

// (b)+(d) past 32 bits but within 2^53: a number, still exact, both signs.
TEST(DatabaseTest, LuaColumnValueWidensBeyond32BitsExactly)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_P31, 2147483648.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_P40, 1099511627776.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_N31, -2147483649.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_P53, 9007199254740992.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_N53, -9007199254740992.0);
}

// (c)+(d) a double would round it: the decimal string, byte-exact, both signs.
// i64max is the one that needs the guard in DatabaseIntegerBand: (double)
// INT64_MAX rounds UP to 2^63, and casting THAT back is undefined -- arm64
// saturates to INT64_MAX, so a naive round trip would call it exact.
TEST(DatabaseTest, LuaColumnValueGivesStringWhenADoubleWouldRound)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    EXPECT_LUA_EXACT_STRING(f.L, LUA_SINGLE, BIG_COL_P53P1,  "9007199254740993");
    EXPECT_LUA_EXACT_STRING(f.L, LUA_SINGLE, BIG_COL_N53M1, "-9007199254740993");
    EXPECT_LUA_EXACT_STRING(f.L, LUA_SINGLE, BIG_COL_P60P1,  "1152921504606846977");
    EXPECT_LUA_EXACT_STRING(f.L, LUA_SINGLE, BIG_COL_I64MAX, "9223372036854775807");
}

// (f) THE discriminator: values a magnitude band would stringify but a double
// holds perfectly.  Each is paired above with a neighbour of the same size that
// really does round, so no threshold -- 2^53, 2^60, anything -- can satisfy both
// this test and the one above.  -2^63 is here because it is exactly a power of
// two: the most negative int64 is representable even though INT64_MAX is not.
TEST(DatabaseTest, LuaColumnValueBandsByValueNotByMagnitude)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_P53P2,  9007199254740994.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_N53M2, -9007199254740994.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_P60,    1152921504606846976.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_SINGLE, BIG_COL_I64MIN, -9223372036854775808.0);
}

// (e) the row reader must band identically to the single-column reader.
TEST(DatabaseTest, LuaColumnValuesRowBandsIdentically)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_I32MAX, 2147483647.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_I32MIN, -2147483648.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_P31, 2147483648.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_P40, 1099511627776.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_N31, -2147483649.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_P53, 9007199254740992.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_N53, -9007199254740992.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_P53P2,  9007199254740994.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_N53M2, -9007199254740994.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_P60,    1152921504606846976.0);
    EXPECT_LUA_EXACT_NUMBER(f.L, LUA_ROW, BIG_COL_I64MIN, -9223372036854775808.0);
    EXPECT_LUA_EXACT_STRING(f.L, LUA_ROW, BIG_COL_P53P1,  "9007199254740993");
    EXPECT_LUA_EXACT_STRING(f.L, LUA_ROW, BIG_COL_N53M1, "-9007199254740993");
    EXPECT_LUA_EXACT_STRING(f.L, LUA_ROW, BIG_COL_P60P1,  "1152921504606846977");
    EXPECT_LUA_EXACT_STRING(f.L, LUA_ROW, BIG_COL_I64MAX, "9223372036854775807");
}

// The widened value is not merely exact in C -- feeding it back into SQL as a
// literal selects the same row again, for the number band and the string band.
TEST(DatabaseTest, LuaWidenedValuesRoundTripThroughAQuery)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());
    // %.0f renders a Lua number with no exponent and no fraction; a string-band
    // value is already decimal, so tostring() is the identity there.
    int n = run_ret(f.L,
        "local p31 = world.DatabaseColumnValue('db', 3)\n"
        "local p40 = world.DatabaseColumnValue('db', 4)\n"
        "local big = world.DatabaseColumnValue('db', 11)\n"
        "world.DatabaseFinalize('db')\n"
        "local sql = 'SELECT count(*) FROM big WHERE p31 = ' ..\n"
        "  string.format('%.0f', p31) .. ' AND p40 = ' ..\n"
        "  string.format('%.0f', p40) .. ' AND i64max = ' .. tostring(big)\n"
        "assert(world.DatabasePrepare('db', sql) == 0)\n"
        "assert(world.DatabaseStep('db') == 100)\n"
        "return world.DatabaseColumnValue('db', 1)");
    ASSERT_EQ(1, n);
    EXPECT_EQ(LUA_TNUMBER, lua_type(f.L, -1));
    EXPECT_EQ(1.0, (double)lua_tonumber(f.L, -1));   // the row matched
    lua_settop(f.L, 0);
}

// ── the same five cases down the VARIANT (dispatch) path ─────────────────
// Python/Perl/JavaScript/Tcl/Ruby all reach these two methods through the
// automation dispatch map, so this is where their banding is pinned.

static void expect_variant_number(VARIANT& v, double expected)
{
    EXPECT_EQ((int)VT_R8, (int)v.vt);
    if (v.vt == VT_R8)
        EXPECT_EQ(expected, v.dblVal);
}

static void expect_variant_string(VARIANT& v, const char* expected)
{
    EXPECT_EQ((int)VT_BSTR, (int)v.vt);
    if (v.vt == VT_BSTR)
        EXPECT_STREQ(expected, (const char*)v.bstrVal);
}

TEST(DatabaseTest, CppColumnValueBandsInt64)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());

    // (a) inside 32 bits: still VT_I4, exactly as before the widening.
    VARIANT v = f.doc.DatabaseColumnValue("db", BIG_COL_I32MAX);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(2147483647L, (long)v.lVal);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_I32MIN);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(-2147483648L, (long)v.lVal);

    // (b)+(d)+(f) a double holds it exactly: VT_R8.  Includes the four the old
    // magnitude band got wrong, so this list is the VARIANT-side discriminator.
    v = f.doc.DatabaseColumnValue("db", BIG_COL_P31);  expect_variant_number(v, 2147483648.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_P40);  expect_variant_number(v, 1099511627776.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_N31);  expect_variant_number(v, -2147483649.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_P53);  expect_variant_number(v, 9007199254740992.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_N53);  expect_variant_number(v, -9007199254740992.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_P53P2); expect_variant_number(v, 9007199254740994.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_N53M2); expect_variant_number(v, -9007199254740994.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_P60);  expect_variant_number(v, 1152921504606846976.0);
    v = f.doc.DatabaseColumnValue("db", BIG_COL_I64MIN); expect_variant_number(v, -9223372036854775808.0);

    // (c)+(d) a double would round it: a BSTR holding the exact decimal.
    static const struct { int col; const char* text; } kStrings[] = {
        { BIG_COL_P53P1,  "9007199254740993" },
        { BIG_COL_N53M1,  "-9007199254740993" },
        { BIG_COL_P60P1,  "1152921504606846977" },
        { BIG_COL_I64MAX, "9223372036854775807" },
    };
    for (size_t i = 0; i < sizeof kStrings / sizeof kStrings[0]; i++) {
        v = f.doc.DatabaseColumnValue("db", kStrings[i].col);
        expect_variant_string(v, kStrings[i].text);
        if (v.vt == VT_BSTR) SysFreeString(v.bstrVal);
    }
}

TEST(DatabaseTest, CppColumnValuesRowBandsInt64)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);
    ASSERT_TRUE(f.row());

    VARIANT v = f.doc.DatabaseColumnValues("db");
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    ASSERT_TRUE(v.parray != NULL);
    VARIANT* e = (VARIANT*)v.parray->pvData;
    ASSERT_TRUE(e != NULL);

    EXPECT_EQ((int)VT_I4, (int)e[BIG_COL_I32MAX - 1].vt);
    EXPECT_EQ(2147483647L, (long)e[BIG_COL_I32MAX - 1].lVal);
    EXPECT_EQ((int)VT_I4, (int)e[BIG_COL_I32MIN - 1].vt);
    EXPECT_EQ(-2147483648L, (long)e[BIG_COL_I32MIN - 1].lVal);

    expect_variant_number(e[BIG_COL_P31 - 1], 2147483648.0);
    expect_variant_number(e[BIG_COL_P40 - 1], 1099511627776.0);
    expect_variant_number(e[BIG_COL_N31 - 1], -2147483649.0);
    expect_variant_number(e[BIG_COL_P53 - 1], 9007199254740992.0);
    expect_variant_number(e[BIG_COL_N53 - 1], -9007199254740992.0);
    expect_variant_number(e[BIG_COL_P53P2 - 1], 9007199254740994.0);
    expect_variant_number(e[BIG_COL_N53M2 - 1], -9007199254740994.0);
    expect_variant_number(e[BIG_COL_P60 - 1], 1152921504606846976.0);
    expect_variant_number(e[BIG_COL_I64MIN - 1], -9223372036854775808.0);

    expect_variant_string(e[BIG_COL_P53P1 - 1],  "9007199254740993");
    expect_variant_string(e[BIG_COL_N53M1 - 1],  "-9007199254740993");
    expect_variant_string(e[BIG_COL_P60P1 - 1],  "1152921504606846977");
    expect_variant_string(e[BIG_COL_I64MAX - 1], "9223372036854775807");

    VariantClear(&v);
}

// DatabaseGetField reads its one field through the same two readers -- Lua via
// GetDatabaseColumnValue, everything else via DatabaseColumnValue -- so it
// inherits the banding.  Pinned here because it is the method a script reaches
// for when it wants a single value and never touches Prepare/Step itself.
TEST(DatabaseTest, GetFieldBandsInt64Too)
{
    BigIntFixture f;
    ASSERT_NE(nullptr, f.L);

    int n = run_ret(f.L, "return world.DatabaseGetField('db', 'SELECT p40 FROM big')");
    ASSERT_EQ(1, n);
    EXPECT_EQ(LUA_TNUMBER, lua_type(f.L, -1));
    EXPECT_EQ(1099511627776.0, (double)lua_tonumber(f.L, -1));
    lua_settop(f.L, 0);

    n = run_ret(f.L, "return world.DatabaseGetField('db', 'SELECT i64max FROM big')");
    ASSERT_EQ(1, n);
    EXPECT_EQ(LUA_TSTRING, lua_type(f.L, -1));
    EXPECT_STREQ("9223372036854775807", lua_tostring(f.L, -1));
    lua_settop(f.L, 0);

    VARIANT v = f.doc.DatabaseGetField("db", "SELECT p40 FROM big");
    expect_variant_number(v, 1099511627776.0);
    v = f.doc.DatabaseGetField("db", "SELECT i64max FROM big");
    expect_variant_string(v, "9223372036854775807");
    if (v.vt == VT_BSTR) SysFreeString(v.bstrVal);
}

// ── the OTHER SQLite surface: the bundled Lua `sqlite3` module ───────────
//
// A script can read an SQLite INTEGER two ways: the four world.Database*
// readers above, or lsqlite3 (`sqlite3.open_memory()`, opened for every world
// by OpenLuaDelayed).  db_int64.h adopted lsqlite3's round-trip rule precisely
// so the two stop disagreeing -- so both halves of the comparison belong in the
// suite, or the alignment can rot without anything going red.
//
// Read one INTEGER literal through lsqlite3 and report "<sqlite typeof>/<lua
// type>".  The typeof half is the anti-degenerate guard: SQLite's tokenizer
// turns some big literals into REAL, and if that happened the lua type would be
// answering a question about a float column instead of the int64 path.
struct Lsqlite3Fixture {
    CMUSHclientDoc doc;
    lua_State* L = nullptr;
    Lsqlite3Fixture()
    {
        L = make_lua(&doc);
        if (!L) return;
        lua_pushcfunction(L, luaopen_lsqlite3);       // registers the global `sqlite3`
        if (lua_pcall(L, 0, 0, 0) != 0) { lua_close(L); L = nullptr; }
        else lua_settop(L, 0);
    }
    ~Lsqlite3Fixture() { if (L) lua_close(L); }
};

static CString lsqlite3_reads(lua_State* L, const char* literal)
{
    char code[512];
    snprintf(code, sizeof code,
        "local db = sqlite3.open_memory()\n"
        "assert(db:exec('CREATE TABLE t (v INTEGER);') == sqlite3.OK)\n"
        "assert(db:exec('INSERT INTO t VALUES (%s);') == sqlite3.OK)\n"
        "local ty for x in db:urows('SELECT typeof(v) FROM t') do ty = x end\n"
        "local lt for x in db:urows('SELECT v FROM t') do lt = type(x) end\n"
        "db:close()\n"
        "return ty .. '/' .. lt", literal);
    lua_settop(L, 0);
    if (run_ret(L, code) != 1)
        return CString("<chunk failed>");
    CString result(lua_tostring(L, -1));
    lua_settop(L, 0);
    return result;
}

TEST(DatabaseTest, Lsqlite3ModuleAgreesOnWhatADoubleCanHold)
{
    Lsqlite3Fixture d;
    ASSERT_NE(nullptr, d.L);

    // The values our band now calls numbers -- lsqlite3 always did.  2^60 is
    // the case that used to differ: a magnitude cap at 2^53 stringified it here
    // while lsqlite3 handed back a (perfectly exact) number.
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "1152921504606846976"));
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "9007199254740994"));
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "-9223372036854775807 - 1"));
}

// The residual disagreement, asserted rather than assumed.  lsqlite3's test is
//     lua_Number n = (lua_Number) i64;  if (n == i64) ...
// and C widens the int64 operand to double before comparing, so it reads
// (double) i64 == (double) i64 and is vacuously TRUE -- its string branch
// (lsqlite/lsqlite3.c:121) is unreachable.  lsqlite3 therefore hands back a
// silently rounded number for values a double cannot hold, where we return the
// lossless decimal string.  Vendored third-party behaviour, deliberately left
// alone; pinned here so a change to it -- in either direction -- is visible.
TEST(DatabaseTest, Lsqlite3ModuleStillNumbersWhatWeStringify)
{
    Lsqlite3Fixture d;
    ASSERT_NE(nullptr, d.L);

    // Our four string-band columns, every one of them a number over there.
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "9007199254740993"));
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "-9007199254740993"));
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "1152921504606846977"));
    EXPECT_EQ(CString("integer/number"), lsqlite3_reads(d.L, "9223372036854775807"));

    // ... and the rounding that costs: 2^53+1 comes back as 2^53.  This is the
    // assertion that makes the paragraph above a fact rather than a claim.
    lua_settop(d.L, 0);
    ASSERT_EQ(1, run_ret(d.L,
        "local db = sqlite3.open_memory()\n"
        "assert(db:exec('CREATE TABLE t (v INTEGER);') == sqlite3.OK)\n"
        "assert(db:exec('INSERT INTO t VALUES (9007199254740993);') == sqlite3.OK)\n"
        "local got for x in db:urows('SELECT v FROM t') do got = x end\n"
        "db:close()\n"
        "return string.format('%.0f', got)"));
    EXPECT_STREQ("9007199254740992", lua_tostring(d.L, -1));   // NOT ...93
    lua_settop(d.L, 0);
}

MC_TEST_MAIN()

#else  // !MUSHCLIENT_LUA_SCRIPTING

int main() { return 0; }

#endif
