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

lua_State* MakeLuaState();
int RegisterLuaRoutines(lua_State*);

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

MC_TEST_MAIN()

#else  // !MUSHCLIENT_LUA_SCRIPTING

int main() { return 0; }

#endif
