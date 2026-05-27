// scripting/lua_utils_portable.cpp
//
// Portable (non-Windows) implementations of Lua utility functions that live in
// Utilities.cpp and lua_methods.cpp on Windows.
//
// On Windows, Utilities.cpp and lua_methods.cpp are compiled via the .vcxproj.
// This file is compiled ONLY on non-Windows, and ONLY when Lua is found
// (it is added to mushcore via the if(LUA_FOUND) block in CMakeLists.txt).
//
// Guard: this entire file is excluded from Windows builds.

#ifndef _WIN32

#include "platform/doc_stub.h"

#ifdef LUA_VERSION  // Lua headers available (always true here — file only compiled with LUA_FOUND)

#include "lualib.h"  // luaL_openlibs

#include <string>
#include <vector>
#include <cstdlib>   // malloc/realloc/free
#include <cstdio>    // fprintf
#include <ctime>    // time()
#include <cstdlib>  // getcwd, free
#include <vector>   // std::vector in utils functions
#include <unistd.h> // getcwd

using std::string;
using std::vector;

// ── Lua allocator ─────────────────────────────────────────────────────────────
// Standard pass-through to malloc/realloc/free; identical to the Windows version
// in Utilities.cpp (minus the AfxMessageBox panic handler).

static void * l_alloc (void * /*ud*/, void * ptr, size_t /*osize*/, size_t nsize)
{
    if (nsize == 0) { free(ptr); return nullptr; }
    return realloc(ptr, nsize);
}

static int l_panic (lua_State *L)
{
    fprintf(stderr, "PANIC: unprotected error in call to Lua API (%s)\n",
            lua_tostring(L, -1));
    return 0;
}

// ── MakeLuaState ──────────────────────────────────────────────────────────────
// Create and initialise a new Lua VM.  On Windows this lives in Utilities.cpp.

lua_State * MakeLuaState (void)
{
    lua_State * L = lua_newstate(l_alloc, nullptr);
    if (L) lua_atpanic(L, l_panic);
    return L;
}

// ── GetNestedFunction ─────────────────────────────────────────────────────────
// Navigate a dot-separated name like "string.gsub" through Lua tables, leaving
// the function (if found) on the stack.  Returns true on success.

bool GetNestedFunction (lua_State * L, const char * sName, const bool bRaiseError)
{
    // Split "a.b.c" into ["a","b","c"]
    vector<string> v;
    {
        string s(sName), part;
        size_t pos;
        while ((pos = s.find('.')) != string::npos) {
            v.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        v.push_back(s);
    }

    lua_pushvalue(L, LUA_GLOBALSINDEX);

    vector<string>::const_iterator iter = v.begin();
    string sPrev = "_G", sItem;

    for (; iter != v.end(); ++iter) {
        sItem = *iter;
        if (!lua_istable(L, -1))
            break;
        lua_pushstring(L, iter->c_str());
        lua_gettable(L, -2);
        lua_remove(L, -2);
        sPrev = *iter;
    }

    bool bResult = lua_isfunction(L, -1);

    if (!bResult || (bResult && iter != v.end())) {
        if (bResult) sItem = sPrev;
        if (bRaiseError)
            fprintf(stderr, "Cannot find the function '%s' - item '%s' is %s\n",
                    sName, sItem.c_str(),
                    lua_typename(L, lua_type(L, -1)));
        return false;
    }
    return true;
}

// ── FindLuaFunction ───────────────────────────────────────────────────────────
// Return true if a dot-separated Lua function exists, without leaving anything
// on the stack.

bool FindLuaFunction (lua_State * L, const char * sName)
{
    bool bResult = GetNestedFunction(L, sName, false);
    lua_settop(L, 0);
    return bResult;
}

// ── MakeTableItem overloads ───────────────────────────────────────────────────
// Set a key→value pair in a Lua table at -3 on the stack.

void MakeTableItem (lua_State * L, const char * name, const CString & str)
{
    lua_pushstring(L, name);
    lua_pushstring(L, static_cast<const char *>(str));
    lua_rawset(L, -3);
}

void MakeTableItem (lua_State * L, const char * name, const string & str)
{
    lua_pushstring(L, name);
    lua_pushlstring(L, str.c_str(), str.size());
    lua_rawset(L, -3);
}

void MakeTableItem (lua_State * L, const char * name, const double n)
{
    lua_pushstring(L, name);
    lua_pushnumber(L, n);
    lua_rawset(L, -3);
}

void MakeTableItemBool (lua_State * L, const char * name, const bool b)
{
    lua_pushstring(L, name);
    lua_pushboolean(L, b ? 1 : 0);
    lua_rawset(L, -3);
}


// ─────────────────────────────────────────────────────────────────────────────
// utils.* Lua table — portable (non-Windows) implementation
//
// Mirrors the "utils" table registered by lua_utils.cpp in the Windows build.
// Pure-computation functions are copied verbatim; dialog functions dispatch
// through the wx hook system (nullptr → returns nil in CLI/headless builds).
// ─────────────────────────────────────────────────────────────────────────────

#include "Dmetaph.h"              // MString / DoubleMetaphone
// Forward declarations for symbols defined elsewhere in mushcore
int EditDistance(const std::string source, const std::string target);

struct tInfoTypeMapping { int iInfoType; const char* sDescription; };
extern tInfoTypeMapping InfoTypes[];

// tInternalFunctionsTable: matches the typedef in stdafx.h (const char* fields).
struct tInternalFunctionsTable { const char* sFunction; const char* sArgs; };
extern tInternalFunctionsTable InternalFunctionsTable[];

extern std::string PluginCallbacksNames[];

// Self-contained UTF-8 encode/validate helpers.
// These replace calls to _pcre_ord2utf/_pcre_valid_utf8, which are PCRE
// internal symbols not exported from the bundled mcpcre library.

// Encode a Unicode codepoint to UTF-8.  Returns the number of bytes written (1–4).
static int ord2utf8(int c, unsigned char* buf)
{
    if (c < 0x80) {
        buf[0] = (unsigned char)c;
        return 1;
    }
    if (c < 0x800) {
        buf[0] = (unsigned char)(0xC0 | (c >> 6));
        buf[1] = (unsigned char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        buf[0] = (unsigned char)(0xE0 | (c >> 12));
        buf[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (c & 0x3F));
        return 3;
    }
    buf[0] = (unsigned char)(0xF0 | (c >> 18));
    buf[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
    buf[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
    buf[3] = (unsigned char)(0x80 | (c & 0x3F));
    return 4;
}

// Validate a UTF-8 byte sequence of length `len`.
// Mirrors the _pcre_valid_utf8 contract:
//   returns 0             if the entire sequence is valid UTF-8;
//   returns (offset + 1)  (always > 0) at the first invalid byte.
// This lets callers use "result <= 0" to test validity and "result > 0" to
// detect errors — both patterns are used in the utils_utf8* functions below.
static int valid_utf8(const unsigned char* s, int len)
{
    for (int i = 0; i < len; ) {
        unsigned char c = s[i];
        int bytes;
        if      (c < 0x80)           { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { bytes = 2; }
        else if ((c & 0xF0) == 0xE0) { bytes = 3; }
        else if ((c & 0xF8) == 0xF0) { bytes = 4; }
        else return i + 1;
        if (i + bytes > len) return i + 1;
        for (int j = 1; j < bytes; j++)
            if ((s[i+j] & 0xC0) != 0x80) return i + 1;
        i += bytes;
    }
    return 0;
}

// xterm 256-colour table (initialised by lua_world_api_portable.cpp at load)
extern COLORREF xterm_256_colours[256];

// ── UTF-8 encode: codepoints → UTF-8 string ──────────────────────────────────
static int utils_utf8encode(lua_State* L)
{
    int numArgs = lua_gettop(L);
    std::string sOutput;
    unsigned char utf8[10];
    for (int i = 1; i <= numArgs; i++) {
        if (lua_istable(L, i)) {
            for (int j = 1; ; j++) {
                lua_rawgeti(L, i, j);
                if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
                if (!lua_isnumber(L, -1))
                    luaL_error(L, "Index [%d] of table arg #%d must be a number", j, i);
                double f = lua_tonumber(L, -1);
                lua_pop(L, 1);
                int n = (int)f;
                sOutput.append((const char*)utf8, ord2utf8(n, utf8));
            }
        } else {
            double f = luaL_checknumber(L, i);
            int n = (int)f;
            sOutput.append((const char*)utf8, ord2utf8(n, utf8));
        }
    }
    lua_pushlstring(L, sOutput.c_str(), sOutput.size());
    return 1;
}

// ── UTF-8 convert: Latin-1 string → UTF-8 ────────────────────────────────────
static int utils_utf8convert(lua_State* L)
{
    const unsigned char* s = (const unsigned char*)luaL_checkstring(L, 1);
    std::string sOutput;
    unsigned char utf8[10];
    for (const unsigned char* p = s; *p; p++)
        sOutput.append((const char*)utf8, ord2utf8(*p, utf8));
    lua_pushlstring(L, sOutput.c_str(), sOutput.size());
    return 1;
}

// ── UTF-8 decode: UTF-8 string → table of codepoints ─────────────────────────
static int utils_utf8decode(lua_State* L)
{
    size_t len;
    const unsigned char* data = (const unsigned char*)luaL_checklstring(L, 1, &len);
    int iBad = valid_utf8(data, (int)len);
    if (iBad > 0)
        luaL_error(L, "Bad UTF-8 string. Error at column %d", iBad + 1);
    lua_newtable(L);
    int idx = 1;
    for (size_t i = 0; i < len; ) {
        unsigned char c = data[i];
        unsigned long cp;
        int bytes;
        if      (c < 0x80)                       { cp = c;           bytes = 1; }
        else if ((c & 0xE0) == 0xC0)             { cp = c & 0x1F;   bytes = 2; }
        else if ((c & 0xF0) == 0xE0)             { cp = c & 0x0F;   bytes = 3; }
        else if ((c & 0xF8) == 0xF0)             { cp = c & 0x07;   bytes = 4; }
        else                                      { cp = c;           bytes = 1; }
        for (int b = 1; b < bytes && i + b < len; b++)
            cp = (cp << 6) | (data[i + b] & 0x3F);
        lua_pushnumber(L, (lua_Number)cp);
        lua_rawseti(L, -2, idx++);
        i += bytes;
    }
    return 1;
}

// ── UTF-8 valid: return true if string is valid UTF-8 ────────────────────────
static int utils_utf8valid(lua_State* L)
{
    size_t len;
    const unsigned char* s = (const unsigned char*)luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, valid_utf8(s, (int)len) <= 0);
    return 1;
}

// Helper: byte-advance one UTF-8 codepoint
static const unsigned char* utf8_next(const unsigned char* p)
{
    unsigned char c = *p;
    if (c < 0x80)        return p + 1;
    if ((c & 0xE0)==0xC0) return p + 2;
    if ((c & 0xF0)==0xE0) return p + 3;
    if ((c & 0xF8)==0xF0) return p + 4;
    return p + 1;
}

// ── UTF-8 sub: substring by codepoint indices (1-based, negative = from end) ──
static int utils_utf8sub(lua_State* L)
{
    size_t len;
    const unsigned char* s = (const unsigned char*)luaL_checklstring(L, 1, &len);
    int start = (int)luaL_checknumber(L, 2);
    int stop  = (int)luaL_optnumber(L, 3, -1);

    // Count total codepoints
    int total = 0;
    for (const unsigned char* p = s; p < s + len; p = utf8_next(p)) total++;

    if (start < 0) start = total + start + 1;
    if (stop  < 0) stop  = total + stop  + 1;
    if (start < 1) start = 1;
    if (stop  > total) stop = total;

    if (start > stop) { lua_pushlstring(L, "", 0); return 1; }

    const unsigned char* begin_p = s;
    int i = 1;
    for (; i < start; i++) begin_p = utf8_next(begin_p);
    const unsigned char* end_p = begin_p;
    for (; i <= stop; i++) end_p = utf8_next(end_p);
    lua_pushlstring(L, (const char*)begin_p, end_p - begin_p);
    return 1;
}

// ── edit_distance ─────────────────────────────────────────────────────────────
static int utils_edit_distance(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    const char* t = luaL_checkstring(L, 2);
    lua_pushnumber(L, ::EditDistance(std::string(s), std::string(t)));
    return 1;
}

// ── metaphone ─────────────────────────────────────────────────────────────────
static int utils_metaphone(lua_State* L)
{
    const char* word = luaL_checkstring(L, 1);
    int length = (int)luaL_optnumber(L, 2, 4);
    MString m(word, length);
    CString str1, str2;
    m.DoubleMetaphone(str1, str2);
    lua_pushstring(L, (const char*)str1);
    if (str2.IsEmpty())
        lua_pushnil(L);
    else
        lua_pushstring(L, (const char*)str2);
    return 2;
}

// ── colourcube ────────────────────────────────────────────────────────────────
static int utils_colourcube(lua_State* L)
{
    int which = (int)luaL_checknumber(L, 1);
    static const unsigned char values[6] = { 0, 95, 135, 175, 215, 255 };
    static const unsigned char incr = 51;
    switch (which) {
    case 1:
        for (int r = 0; r < 6; r++)
          for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
              xterm_256_colours[16 + r*36 + g*6 + b] =
                RGB(values[r], values[g], values[b]);
        break;
    case 2:
        for (int r = 0; r < 6; r++)
          for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
              xterm_256_colours[16 + r*36 + g*6 + b] =
                RGB(r*incr, g*incr, b*incr);
        break;
    default: luaL_error(L, "Unknown option");
    }
    return 0;
}

// ── timer: seconds since epoch using QPC (high-res on non-Windows) ───────────
static int utils_timer(lua_State* L)
{
    if (App.m_iCounterFrequency) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        lua_pushnumber(L, (double)t.QuadPart / (double)App.m_iCounterFrequency);
    } else {
        lua_pushnumber(L, (double)time(nullptr));
    }
    return 1;
}

// ── infotypes: table of GetInfo selector descriptions ────────────────────────
static int utils_infotypes(lua_State* L)
{
    lua_newtable(L);
    for (int i = 0; InfoTypes[i].iInfoType; i++) {
        lua_pushstring(L, InfoTypes[i].sDescription);
        lua_rawseti(L, -2, InfoTypes[i].iInfoType);
    }
    return 1;
}

// ── functionlist / functionargs / callbackslist ───────────────────────────────
static int utils_functionlist(lua_State* L)
{
    lua_newtable(L);
    for (int i = 0; InternalFunctionsTable[i].sFunction[0]; i++) {
        lua_pushstring(L, InternalFunctionsTable[i].sFunction);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
static int utils_functionargs(lua_State* L)
{
    lua_newtable(L);
    for (int i = 0; InternalFunctionsTable[i].sFunction[0]; i++) {
        lua_pushstring(L, InternalFunctionsTable[i].sFunction);
        lua_pushstring(L, InternalFunctionsTable[i].sArgs);
        lua_rawset(L, -3);
    }
    return 1;
}
static int utils_callbackslist(lua_State* L)
{
    lua_newtable(L);
    for (int i = 0; PluginCallbacksNames[i] != ""; i++) {
        lua_pushstring(L, PluginCallbacksNames[i].c_str());
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ── getfontfamilies: empty table on non-Windows ───────────────────────────────
static int utils_getfontfamilies(lua_State* L)
{
    lua_newtable(L);
    return 1;
}

// ── glyph_available: not supported ───────────────────────────────────────────
static int utils_glyph_available(lua_State* L)
{
    lua_pushnumber(L, 0);
    return 1;
}

// ── info: system information table ───────────────────────────────────────────
static int utils_info(lua_State* L)
{
    lua_newtable(L);
    char* p = getcwd(nullptr, 1024);
    if (p) {
        CString strPath(p);
        free(p);
        if (strPath.Right(1) != "/") strPath += "/";
        lua_pushstring(L, (const char*)strPath);
        lua_setfield(L, -2, "current_directory");
    }
    // log_files_directory is the only directory field available in the
    // CMUSHclientApp stub on non-Windows.
    lua_pushstring(L, App.m_strDefaultLogFileDirectory.c_str());
    lua_setfield(L, -2, "log_files_directory");
    return 1;
}

// ── msgbox / umsgbox: dispatch through UMessageBox ───────────────────────────
static int utils_generic_msgbox(lua_State* L)
{
    const char* msg     = luaL_checkstring(L, 1);
    const char* title   = luaL_optstring(L, 2, "MUSHclient");
    const char* boxtype = luaL_optstring(L, 3, "ok");
    const char* boxicon = luaL_optstring(L, 4, "!");
    int         defbtn  = (int)luaL_optnumber(L, 5, 1);

    UINT flags = MB_APPLMODAL;
    if      (strcmp(boxtype,"ok")==0)                flags = MB_OK;
    else if (strcmp(boxtype,"abortretryignore")==0)  flags = MB_ABORTRETRYIGNORE;
    else if (strcmp(boxtype,"okcancel")==0)           flags = MB_OKCANCEL;
    else if (strcmp(boxtype,"retrycancel")==0)        flags = MB_RETRYCANCEL;
    else if (strcmp(boxtype,"yesno")==0)              flags = MB_YESNO;
    else if (strcmp(boxtype,"yesnocancel")==0)        flags = MB_YESNOCANCEL;
    else luaL_error(L, "msgbox type unknown");

    switch (tolower((unsigned char)boxicon[0])) {
    case '?': flags |= MB_ICONQUESTION;    break;
    case '!': flags |= MB_ICONEXCLAMATION; break;
    case 'i': flags |= MB_ICONINFORMATION; break;
    case '.': flags |= MB_ICONSTOP;        break;
    default:  luaL_error(L, "msgbox icon unknown");
    }
    switch (defbtn) {
    case 1: flags |= MB_DEFBUTTON1; break;
    case 2: flags |= MB_DEFBUTTON2; break;
    case 3: flags |= MB_DEFBUTTON3; break;
    }

    int r = UMessageBox(msg, flags, title);
    const char* res;
    switch (r) {
    case IDYES:    res = "yes";    break;
    case IDNO:     res = "no";     break;
    case IDOK:     res = "ok";     break;
    case IDRETRY:  res = "retry";  break;
    case IDIGNORE: res = "ignore"; break;
    case IDCANCEL: res = "cancel"; break;
    case IDABORT:  res = "abort";  break;
    default:       res = "other";  break;
    }
    lua_pushstring(L, res);
    return 1;
}
static int utils_msgbox(lua_State* L)  { return utils_generic_msgbox(L); }
static int utils_umsgbox(lua_State* L) { return utils_generic_msgbox(L); }

// ── inputbox / editbox ────────────────────────────────────────────────────────
static int utils_gen_inputbox(lua_State* L, bool multiline)
{
    const char* title    = luaL_optstring(L, 1, "Input");
    const char* prompt   = luaL_optstring(L, 2, "");
    const char* defval   = luaL_optstring(L, 3, "");
    if (!g_wxhook_inputbox) { lua_pushnil(L); return 1; }
    char* result = g_wxhook_inputbox(title, prompt, defval, multiline);
    if (!result) { lua_pushnil(L); return 1; }
    lua_pushstring(L, result);
    free(result);
    return 1;
}
static int utils_inputbox(lua_State* L) { return utils_gen_inputbox(L, false); }
static int utils_editbox(lua_State* L)  { return utils_gen_inputbox(L, true);  }

// ── filepicker ────────────────────────────────────────────────────────────────
static int utils_filepicker(lua_State* L)
{
    const char* title   = luaL_optstring(L, 1, "");
    const char* defname = luaL_optstring(L, 2, "");
    const char* defext  = luaL_optstring(L, 3, "");
    // arg 4: filter table (ignored — we pass wildcard)
    bool bSave = lua_toboolean(L, 5) != 0;

    if (!g_wxhook_file_dialog) { lua_pushnil(L); return 1; }
    char* path = g_wxhook_file_dialog(!bSave, title, defname, defext, nullptr, bSave);
    if (!path) { lua_pushnil(L); return 1; }
    lua_pushstring(L, path);
    free(path);
    return 1;
}

// ── directorypicker ───────────────────────────────────────────────────────────
static int utils_directorypicker(lua_State* L)
{
    const char* title   = luaL_optstring(L, 1, "Select directory");
    const char* defpath = luaL_optstring(L, 2, "");
    if (!g_wxhook_directorypicker) { lua_pushnil(L); return 1; }
    char* path = g_wxhook_directorypicker(title, defpath);
    if (!path) { lua_pushnil(L); return 1; }
    lua_pushstring(L, path);
    free(path);
    return 1;
}

// ── choose / listbox: single-choice dialog ────────────────────────────────────
static int utils_choose(lua_State* L)
{
    // args: table-of-items, optional default, optional title
    if (!lua_istable(L, 1)) luaL_error(L, "argument 1 must be a table");
    std::vector<const char*> items;
    std::vector<std::string> storage;
    for (int j = 1; ; j++) {
        lua_rawgeti(L, 1, j);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        storage.push_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    for (auto& s : storage) items.push_back(s.c_str());
    const char* defval = luaL_optstring(L, 2, "");

    if (!g_wxhook_menu_dialog || items.empty()) { lua_pushnil(L); return 1; }
    char* chosen = g_wxhook_menu_dialog(items.data(), (int)items.size(), defval);
    if (!chosen) { lua_pushnil(L); return 1; }
    lua_pushstring(L, chosen);
    free(chosen);
    return 1;
}
static int utils_listbox(lua_State* L)      { return utils_choose(L); }
static int utils_multilistbox(lua_State* L) {
    // Multi-select not yet supported; fall back to single-choice
    return utils_choose(L);
}

// ── appendtonotepad / activatenotepad ─────────────────────────────────────────
static int utils_appendtonotepad(lua_State* L)
{
    const char* title   = luaL_checkstring(L, 1);
    const char* message = luaL_checkstring(L, 2);
    bool bReplace = lua_toboolean(L, 3) != 0;
    // Route through the GUI notepad hook (nullptr in CLI builds).
    bool ok = false;
    if (g_wxhook_append_to_notepad)
        ok = g_wxhook_append_to_notepad(title, message, bReplace);
    lua_pushboolean(L, ok);
    return 1;
}
static int utils_activatenotepad(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    bool ok = false;
    if (g_wxhook_activate_notepad)
        ok = g_wxhook_activate_notepad(title);
    lua_pushboolean(L, ok);
    return 1;
}

// ── showdebugstatus ───────────────────────────────────────────────────────────
extern bool bShowDebugStatus;
static int utils_showdebugstatus(lua_State* L)
{
    bShowDebugStatus = lua_toboolean(L, 1) != 0;
    return 0;
}

// ── xmlread ───────────────────────────────────────────────────────────────────
// Parses an XML string and returns a nested Lua table (same structure as
// the Windows utils.xmlread built on CXMLparser).
#include "xml/xmlparse.h"

static void xmlOutputElement(lua_State* L, CXMLelement& node)
{
    lua_newtable(L);

    lua_pushstring(L, (const char*)node.strName);
    lua_setfield(L, -2, "name");

    lua_pushstring(L, (const char*)node.strContent);
    lua_setfield(L, -2, "content");

    if (node.bEmpty) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "empty");
    }

    lua_pushnumber(L, node.iLine);
    lua_setfield(L, -2, "line");

    if (!node.AttributeMap.IsEmpty()) {
        lua_newtable(L);
        CString strName;
        CAttribute* attr = nullptr;
        for (POSITION pos = node.AttributeMap.GetStartPosition(); pos; ) {
            node.AttributeMap.GetNextAssoc(pos, strName, attr);
            lua_pushstring(L, (const char*)attr->strValue);
            lua_setfield(L, -2, (const char*)strName);
        }
        lua_setfield(L, -2, "attributes");
    }

    if (!node.ChildrenList.IsEmpty()) {
        lua_newtable(L);
        int idx = 1;
        for (POSITION pos = node.ChildrenList.GetHeadPosition(); pos; ) {
            CXMLelement* child = node.ChildrenList.GetNext(pos);
            xmlOutputElement(L, *child);
            lua_rawseti(L, -2, idx++);
        }
        lua_setfield(L, -2, "nodes");
    }
}

static int utils_xmlread(lua_State* L)
{
    size_t xmlLen;
    const char* xml = luaL_checklstring(L, 1, &xmlLen);
    CXMLparser parser;
    try {
        CMemFile f((unsigned char*)xml, (UINT)xmlLen);
        CArchive ar(&f, CArchive::load);
        parser.BuildStructure(ar.GetFile());
    } catch (CException* e) {
        char msg[1000] = {};
        e->GetErrorMessage(msg, sizeof msg);
        e->Delete();
        lua_pushnil(L);
        lua_pushstring(L, msg);
        lua_pushnumber(L, (lua_Number)parser.m_xmlLine);
        return 3;
    }
    xmlOutputElement(L, parser.m_xmlRoot);
    lua_pushstring(L, (const char*)parser.m_strDocumentName);

    int nResults = 2;
    if (!parser.m_CustomEntityMap.IsEmpty()) {
        lua_newtable(L);
        CString k, v;
        for (POSITION pos = parser.m_CustomEntityMap.GetStartPosition(); pos; ) {
            parser.m_CustomEntityMap.GetNextAssoc(pos, k, v);
            lua_pushstring(L, (const char*)v);
            lua_setfield(L, -2, (const char*)k);
        }
        nResults = 3;
    }
    return nResults;
}

// ── sendtofront / shellexecute / fontpicker / filterpicker / spellcheck ───────
// Stubs: return nil or false where a return value is expected.
static int utils_sendtofront(lua_State* L)   { lua_pushboolean(L, 0); return 1; }

static int utils_shellexecute(lua_State* L)
{
    // On non-Windows, use xdg-open (Linux) or open (macOS) for the filename arg.
    const char* file = luaL_checkstring(L, 1);
    std::string cmd;
#ifdef __APPLE__
    cmd = "open ";
#else
    cmd = "xdg-open ";
#endif
    cmd += "'";
    // Simple shell-safe: escape single quotes in file path
    for (const char* p = file; *p; p++) {
        if (*p == '\'') cmd += "'\\''";
        else            cmd += *p;
    }
    cmd += "' &";
    system(cmd.c_str());  // fire-and-forget
    lua_pushboolean(L, 1);
    return 1;
}

static int utils_fontpicker(lua_State* L)    { lua_pushnil(L); return 1; }
static int utils_filterpicker(lua_State* L)  { lua_pushnil(L); return 1; }
static int utils_spellcheckdialog(lua_State* L){ lua_pushnil(L); return 1; }
static int utils_menufontsize(lua_State* L)  { lua_pushnumber(L, 0); return 1; }
static int utils_setbgcolour(lua_State* L)   { return 0; }
static int utils_reload_global_prefs(lua_State* L) { return 0; }

// ── utils table registration ──────────────────────────────────────────────────
static const luaL_Reg utils_lib[] = {
    { "activatenotepad",    utils_activatenotepad  },
    { "appendtonotepad",    utils_appendtonotepad  },
    { "callbackslist",      utils_callbackslist    },
    { "choose",             utils_choose           },
    { "colourcube",         utils_colourcube       },
    { "directorypicker",    utils_directorypicker  },
    { "edit_distance",      utils_edit_distance    },
    { "editbox",            utils_editbox          },
    { "filepicker",         utils_filepicker       },
    { "filterpicker",       utils_filterpicker     },
    { "fontpicker",         utils_fontpicker       },
    { "functionargs",       utils_functionargs     },
    { "functionlist",       utils_functionlist     },
    { "getfontfamilies",    utils_getfontfamilies  },
    { "glyph_available",    utils_glyph_available  },
    { "info",               utils_info             },
    { "infotypes",          utils_infotypes        },
    { "inputbox",           utils_inputbox         },
    { "listbox",            utils_listbox          },
    { "menufontsize",       utils_menufontsize     },
    { "metaphone",          utils_metaphone        },
    { "msgbox",             utils_msgbox           },
    { "multilistbox",       utils_multilistbox     },
    { "reload_global_prefs",utils_reload_global_prefs },
    { "sendtofront",        utils_sendtofront      },
    { "setbackgroundcolour",utils_setbgcolour      },
    { "shellexecute",       utils_shellexecute     },
    { "showdebugstatus",    utils_showdebugstatus  },
    { "spellcheckdialog",   utils_spellcheckdialog },
    { "timer",              utils_timer            },
    { "umsgbox",            utils_umsgbox          },
    { "utf8decode",         utils_utf8decode       },
    { "utf8encode",         utils_utf8encode       },
    { "utf8convert",        utils_utf8convert      },
    { "utf8sub",            utils_utf8sub          },
    { "utf8valid",          utils_utf8valid        },
    { "xmlread",            utils_xmlread          },
    { nullptr, nullptr }
};

static void RegisterUtilsLib(lua_State* L)
{
    luaL_register(L, "utils", utils_lib);
    lua_pop(L, 1);
}

// ── RegisterLuaRoutines ───────────────────────────────────────────────────────
// On Windows this (in lua_methods.cpp) registers the full MUSHclient Lua API
// ("world" library, trigger/alias/timer flag tables, colour name tables, etc.).
// On macOS we the portable world library plus the standard extension
// modules that are compiled on macOS (rex, compress, bits, bc, lpeg).

// Forward declaration — implemented in lua_world_api_portable.cpp.
void RegisterWorldLib(lua_State* L);

extern "C" {
    int luaopen_rex    (lua_State *);
    int luaopen_compress (lua_State *);
    int luaopen_bits   (lua_State *);
    int luaopen_bc     (lua_State *);
    int luaopen_lpeg   (lua_State *);
}

int RegisterLuaRoutines (lua_State * L)
{
    // Open all standard Lua libraries (io, os, string, table, math, package …)
    luaL_openlibs(L);

    // Register the "world" library (Note, SetVariable, Send, Execute, …)
    RegisterWorldLib(L);
    RegisterUtilsLib(L);

    // ── Flag/constant tables (mirror lua_methods.cpp MakeFlagsTable calls) ──
    // trigger_flag table
    lua_newtable(L);
    lua_pushstring(L,"Enabled");          lua_pushnumber(L,    1); lua_rawset(L,-3);
    lua_pushstring(L,"OmitFromLog");      lua_pushnumber(L,    2); lua_rawset(L,-3);
    lua_pushstring(L,"OmitFromOutput");   lua_pushnumber(L,    4); lua_rawset(L,-3);
    lua_pushstring(L,"KeepEvaluating");   lua_pushnumber(L,    8); lua_rawset(L,-3);
    lua_pushstring(L,"IgnoreCase");       lua_pushnumber(L,   16); lua_rawset(L,-3);
    lua_pushstring(L,"RegularExpression");lua_pushnumber(L,   32); lua_rawset(L,-3);
    lua_pushstring(L,"ExpandVariables");  lua_pushnumber(L,  512); lua_rawset(L,-3);
    lua_pushstring(L,"Replace");          lua_pushnumber(L, 1024); lua_rawset(L,-3);
    lua_pushstring(L,"LowercaseWildcard");lua_pushnumber(L, 2048); lua_rawset(L,-3);
    lua_pushstring(L,"Temporary");        lua_pushnumber(L,16384); lua_rawset(L,-3);
    lua_pushstring(L,"OneShot");          lua_pushnumber(L,32768); lua_rawset(L,-3);
    lua_setglobal(L, "trigger_flag");

    // alias_flag table
    lua_newtable(L);
    lua_pushstring(L,"Enabled");          lua_pushnumber(L,    1); lua_rawset(L,-3);
    lua_pushstring(L,"KeepEvaluating");   lua_pushnumber(L,    8); lua_rawset(L,-3);
    lua_pushstring(L,"IgnoreAliasCase");  lua_pushnumber(L,   32); lua_rawset(L,-3);
    lua_pushstring(L,"OmitFromLogFile");  lua_pushnumber(L,   64); lua_rawset(L,-3);
    lua_pushstring(L,"RegularExpression");lua_pushnumber(L,  128); lua_rawset(L,-3);
    lua_pushstring(L,"ExpandVariables");  lua_pushnumber(L,  512); lua_rawset(L,-3);
    lua_pushstring(L,"Replace");          lua_pushnumber(L, 1024); lua_rawset(L,-3);
    lua_pushstring(L,"AliasSpeedWalk");   lua_pushnumber(L, 2048); lua_rawset(L,-3);
    lua_pushstring(L,"AliasQueue");       lua_pushnumber(L, 4096); lua_rawset(L,-3);
    lua_pushstring(L,"AliasMenu");        lua_pushnumber(L, 8192); lua_rawset(L,-3);
    lua_pushstring(L,"Temporary");        lua_pushnumber(L,16384); lua_rawset(L,-3);
    lua_pushstring(L,"OneShot");          lua_pushnumber(L,32768); lua_rawset(L,-3);
    lua_setglobal(L, "alias_flag");

    // timer_flag table
    lua_newtable(L);
    lua_pushstring(L,"Enabled");          lua_pushnumber(L,    1); lua_rawset(L,-3);
    lua_pushstring(L,"AtTime");           lua_pushnumber(L,    2); lua_rawset(L,-3);
    lua_pushstring(L,"OneShot");          lua_pushnumber(L,    4); lua_rawset(L,-3);
    lua_pushstring(L,"TimerSpeedWalk");   lua_pushnumber(L,    8); lua_rawset(L,-3);
    lua_pushstring(L,"TimerNote");        lua_pushnumber(L,   16); lua_rawset(L,-3);
    lua_pushstring(L,"ActiveWhenClosed"); lua_pushnumber(L,   32); lua_rawset(L,-3);
    lua_pushstring(L,"Replace");          lua_pushnumber(L, 1024); lua_rawset(L,-3);
    lua_pushstring(L,"Temporary");        lua_pushnumber(L,16384); lua_rawset(L,-3);
    lua_setglobal(L, "timer_flag");

    // custom_colour table
    lua_newtable(L);
    lua_pushstring(L,"NoChange");  lua_pushnumber(L,-1); lua_rawset(L,-3);
    for (int i = 0; i < 16; ++i) {
        char buf[12]; snprintf(buf,sizeof(buf),"Custom%d",i+1);
        lua_pushstring(L,buf); lua_pushnumber(L,i); lua_rawset(L,-3);
    }
    lua_pushstring(L,"CustomOther"); lua_pushnumber(L,16); lua_rawset(L,-3);
    lua_setglobal(L, "custom_colour");

    // sendto table
    lua_newtable(L);
    lua_pushstring(L,"world");            lua_pushnumber(L, 0); lua_rawset(L,-3);
    lua_pushstring(L,"command");          lua_pushnumber(L, 1); lua_rawset(L,-3);
    lua_pushstring(L,"output");           lua_pushnumber(L, 2); lua_rawset(L,-3);
    lua_pushstring(L,"status");           lua_pushnumber(L, 3); lua_rawset(L,-3);
    lua_pushstring(L,"notepad");          lua_pushnumber(L, 4); lua_rawset(L,-3);
    lua_pushstring(L,"notepadappend");    lua_pushnumber(L, 5); lua_rawset(L,-3);
    lua_pushstring(L,"logfile");          lua_pushnumber(L, 6); lua_rawset(L,-3);
    lua_pushstring(L,"notepadreplace");   lua_pushnumber(L, 7); lua_rawset(L,-3);
    lua_pushstring(L,"commandqueue");     lua_pushnumber(L, 8); lua_rawset(L,-3);
    lua_pushstring(L,"variable");         lua_pushnumber(L, 9); lua_rawset(L,-3);
    lua_pushstring(L,"execute");          lua_pushnumber(L,10); lua_rawset(L,-3);
    lua_pushstring(L,"speedwalk");        lua_pushnumber(L,11); lua_rawset(L,-3);
    lua_pushstring(L,"script");           lua_pushnumber(L,12); lua_rawset(L,-3);
    lua_pushstring(L,"immediate");        lua_pushnumber(L,13); lua_rawset(L,-3);
    lua_pushstring(L,"scriptafteromit"); lua_pushnumber(L,14); lua_rawset(L,-3);
    lua_setglobal(L, "sendto");

    // error_code table — mirrors error_codes[] in lua_methods.cpp
    // Allows scripts to write: if rc == error_code.eOK then ...
    lua_newtable(L);
    #define EC(name) lua_pushstring(L,#name); lua_pushnumber(L,name); lua_rawset(L,-3);
    EC(eOK)
    EC(eWorldOpen)           EC(eWorldClosed)          EC(eNoNameSpecified)
    EC(eCannotPlaySound)     EC(eTriggerNotFound)       EC(eTriggerAlreadyExists)
    EC(eTriggerCannotBeEmpty)EC(eInvalidObjectLabel)    EC(eScriptNameNotLocated)
    EC(eAliasNotFound)       EC(eAliasAlreadyExists)    EC(eAliasCannotBeEmpty)
    EC(eCouldNotOpenFile)    EC(eLogFileNotOpen)        EC(eLogFileAlreadyOpen)
    EC(eLogFileBadWrite)     EC(eTimerNotFound)         EC(eTimerAlreadyExists)
    EC(eVariableNotFound)    EC(eCommandNotEmpty)       EC(eBadRegularExpression)
    EC(eTimeInvalid)         EC(eBadMapItem)            EC(eNoMapItems)
    EC(eUnknownOption)       EC(eOptionOutOfRange)
    EC(eTriggerSequenceOutOfRange)                      EC(eTriggerSendToInvalid)
    EC(eTriggerLabelNotSpecified)
    EC(ePluginFileNotFound)  EC(eProblemsLoadingPlugin) EC(ePluginCannotSetOption)
    EC(ePluginCannotGetOption) EC(eNoSuchPlugin)        EC(eNotAPlugin)
    EC(eNoSuchRoutine)       EC(ePluginDoesNotSaveState)EC(ePluginCouldNotSaveState)
    EC(ePluginDisabled)      EC(eErrorCallingPluginRoutine)
    EC(eCommandsNestedTooDeeply)
    EC(eCannotCreateChatSocket) EC(eCannotLookupDomainName) EC(eNoChatConnections)
    EC(eChatPersonNotFound)  EC(eBadParameter)          EC(eChatAlreadyListening)
    EC(eChatIDNotFound)      EC(eChatAlreadyConnected)  EC(eClipboardEmpty)
    EC(eFileNotFound)        EC(eAlreadyTransferringFile) EC(eNotTransferringFile)
    EC(eNoSuchCommand)       EC(eArrayAlreadyExists)    EC(eArrayDoesNotExist)
    EC(eArrayNotEvenNumberOfValues) EC(eImportedWithDuplicates) EC(eBadDelimiter)
    EC(eSetReplacingExistingValue)  EC(eKeyDoesNotExist) EC(eCannotImport)
    EC(eItemInUse)           EC(eSpellCheckNotActive)
    EC(eCannotAddFont)       EC(ePenStyleNotValid)      EC(eUnableToLoadImage)
    EC(eImageNotInstalled)   EC(eInvalidNumberOfPoints) EC(eInvalidPoint)
    EC(eHotspotPluginChanged)EC(eHotspotNotInstalled)   EC(eNoSuchWindow)
    EC(eBrushStyleNotValid)
    #undef EC
    lua_setglobal(L, "error_code");

    // error_desc table — maps error code number → description string.
    // Mirrors error_descriptions[] / MakeIntFlagsTable(L,"error_desc",...) in lua_methods.cpp.
    lua_newtable(L);
    #define ED(code,desc) lua_pushnumber(L,code); lua_pushstring(L,desc); lua_rawset(L,-3);
    ED(0,      "No error")
    ED(30001,  "The world is already open")
    ED(30002,  "The world is closed, this action cannot be performed")
    ED(30003,  "No name has been specified where one is required")
    ED(30004,  "The sound file could not be played")
    ED(30005,  "The specified trigger name does not exist")
    ED(30006,  "Attempt to add a trigger that already exists")
    ED(30007,  "The trigger \"match\" string cannot be empty")
    ED(30008,  "The name of this object is invalid")
    ED(30009,  "Script name is not in the script file")
    ED(30010,  "The specified alias name does not exist")
    ED(30011,  "Attempt to add a alias that already exists")
    ED(30012,  "The alias \"match\" string cannot be empty")
    ED(30013,  "Unable to open requested file")
    ED(30014,  "Log file was not open")
    ED(30015,  "Log file was already open")
    ED(30016,  "Bad write to log file")
    ED(30017,  "The specified timer name does not exist")
    ED(30018,  "Attempt to add a timer that already exists")
    ED(30019,  "Attempt to delete a variable that does not exist")
    ED(30020,  "Attempt to use SetCommand with a non-empty command window")
    ED(30021,  "Bad regular expression syntax")
    ED(30022,  "Time given to AddTimer is invalid")
    ED(30023,  "Direction given to AddToMapper is invalid")
    ED(30024,  "No items in mapper")
    ED(30025,  "Option name not found")
    ED(30026,  "New value for option is out of range")
    ED(30027,  "Trigger sequence value invalid")
    ED(30028,  "Where to send trigger text to is invalid")
    ED(30029,  "Trigger label not specified/invalid for 'send to variable'")
    ED(30030,  "File name specified for plugin not found")
    ED(30031,  "There was a parsing or other problem loading the plugin")
    ED(30032,  "Plugin is not allowed to set this option")
    ED(30033,  "Plugin is not allowed to get this option")
    ED(30034,  "Requested plugin is not installed")
    ED(30035,  "Only a plugin can do this")
    ED(30036,  "Plugin does not support that subroutine (subroutine not in script)")
    ED(30037,  "Plugin does not support saving state")
    ED(30039,  "Plugin is currently disabled")
    ED(30040,  "Could not call plugin routine")
    ED(30041,  "Calls to \"Execute\" nested too deeply")
    ED(30042,  "Unable to create socket for chat connection")
    ED(30043,  "Unable to do DNS (domain name) lookup for chat connection")
    ED(30044,  "No chat connections open")
    ED(30045,  "Requested chat person not connected")
    ED(30046,  "General problem with a parameter to a script call")
    ED(30047,  "Already listening for incoming chats")
    ED(30048,  "Chat session with that ID not found")
    ED(30049,  "Already connected to that server/port")
    ED(30050,  "Cannot get (text from the) clipboard")
    ED(30051,  "Cannot open the specified file")
    ED(30052,  "Already transferring a file")
    ED(30053,  "Not transferring a file")
    ED(30054,  "There is not a command of that name")
    ED(30055,  "That array already exists")
    ED(30056,  "That array does not exist")
    ED(30057,  "Values to be imported into array are not in pairs")
    ED(30058,  "Import succeeded, however some values were overwritten")
    ED(30059,  "Import/export delimiter must be a single character, other than backslash")
    ED(30060,  "Array element set, existing value overwritten")
    ED(30061,  "Array key does not exist")
    ED(30062,  "Cannot import because cannot find unused temporary character")
    ED(30063,  "Cannot delete trigger/alias/timer because it is executing a script")
    ED(30064,  "Spell checker is not active")
    ED(30065,  "Cannot create requested font")
    ED(30066,  "Invalid settings for pen parameter")
    ED(30067,  "Bitmap image could not be loaded")
    ED(30068,  "Image has not been loaded into window")
    ED(30069,  "Number of points supplied is incorrect")
    ED(30070,  "Point is not numeric")
    ED(30071,  "Hotspot processing must all be in same plugin")
    ED(30072,  "Hotspot has not been defined for this window")
    ED(30073,  "Requested miniwindow does not exist")
    ED(30074,  "Invalid settings for brush parameter")
    #undef ED
    lua_setglobal(L, "error_desc");

    // colour_names table — mirrors the App.m_ColoursMap iteration in lua_methods.cpp.
    // On non-Windows the map is populated by MXP_LoadColours() via mxpcolours.cpp
    // which stores entries in App.m_ColoursMap.  Iterate it here.
    {
        lua_newtable(L);
        POSITION pos = App.m_ColoursMap.GetStartPosition();
        while (pos) {
            CColours* pColour = nullptr;
            CString   strName;
            App.m_ColoursMap.GetNextAssoc(pos, strName, pColour);
            if (pColour) {
                lua_pushstring(L, static_cast<const char*>(strName));
                lua_pushnumber(L, static_cast<double>(pColour->iColour));
                lua_rawset(L, -3);
            }
        }
        lua_setglobal(L, "colour_names");
    }

    // miniwin table — complete mirror of miniwindow_flags[] in lua_methods.cpp.
    // Window* functions are not implemented in the portable build, but the
    // constants table must exist so plugins/scripts that reference miniwin.*
    // at load time do not raise a nil-index error.
    lua_newtable(L);
    #define MW(name,val) lua_pushstring(L,name); lua_pushnumber(L,val); lua_rawset(L,-3);
    // Window position / stretch modes
    MW("pos_stretch_to_view",              0) MW("pos_stretch_to_view_with_aspect", 1)
    MW("pos_stretch_to_owner",             2) MW("pos_stretch_to_owner_with_aspect",3)
    MW("pos_top_left",    4)  MW("pos_top_center",    5)  MW("pos_top_right",    6)
    MW("pos_center_right",7)  MW("pos_bottom_right",  8)  MW("pos_bottom_center",9)
    MW("pos_bottom_left",10)  MW("pos_center_left",  11)  MW("pos_center_all",  12)
    MW("pos_tile",       13)
    // WindowCreate flags
    MW("create_underneath",        0x01) MW("create_absolute_location", 0x02)
    MW("create_transparent",       0x04) MW("create_ignore_mouse",      0x08)
    MW("create_keep_hotspots",     0x10)
    // Pen styles
    MW("pen_solid",        PS_SOLID)      MW("pen_dash",         PS_DASH)
    MW("pen_dot",          PS_DOT)        MW("pen_dash_dot",     PS_DASHDOT)
    MW("pen_dash_dot_dot", PS_DASHDOTDOT) MW("pen_null",         PS_NULL)
    MW("pen_inside_frame", PS_INSIDEFRAME)
    MW("pen_endcap_round", PS_ENDCAP_ROUND)  MW("pen_endcap_square", PS_ENDCAP_SQUARE)
    MW("pen_endcap_flat",  PS_ENDCAP_FLAT)
    MW("pen_join_round",   PS_JOIN_ROUND)    MW("pen_join_bevel",    PS_JOIN_BEVEL)
    MW("pen_join_miter",   PS_JOIN_MITER)
    // Rect op actions
    MW("rect_frame",1) MW("rect_fill",2) MW("rect_invert",3) MW("rect_3d_rect",4)
    MW("rect_draw_edge",5) MW("rect_flood_fill_border",6) MW("rect_flood_fill_surface",7)
    // Rect edge styles (for rect_draw_edge colour1 arg)
    MW("rect_edge_raised", EDGE_RAISED)  MW("rect_edge_etched", EDGE_ETCHED)
    MW("rect_edge_bump",   EDGE_BUMP)    MW("rect_edge_sunken", EDGE_SUNKEN)
    // Rect edge sides (for rect_draw_edge colour2 arg — OR together)
    MW("rect_edge_at_top_left",     BF_TOPLEFT)     MW("rect_edge_at_top_right",    BF_TOPRIGHT)
    MW("rect_edge_at_bottom_left",  BF_BOTTOMLEFT)  MW("rect_edge_at_bottom_right", BF_BOTTOMRIGHT)
    MW("rect_edge_at_all",          BF_RECT)
    MW("rect_diagonal_end_top_left",      BF_DIAGONAL_ENDTOPLEFT)
    MW("rect_diagonal_end_top_right",     BF_DIAGONAL_ENDTOPRIGHT)
    MW("rect_diagonal_end_bottom_left",   BF_DIAGONAL_ENDBOTTOMLEFT)
    MW("rect_diagonal_end_bottom_right",  BF_DIAGONAL_ENDBOTTOMRIGHT)
    MW("rect_option_fill_middle",       BF_MIDDLE)
    MW("rect_option_softer_buttons",    BF_SOFT)
    MW("rect_option_flat_borders",      BF_FLAT)
    MW("rect_option_monochrom_borders", BF_MONO)
    // Circle op actions
    MW("circle_ellipse",1) MW("circle_rectangle",2) MW("circle_round_rectangle",3)
    MW("circle_chord",4)   MW("circle_pie",5)
    // Gradient modes
    MW("gradient_horizontal",1) MW("gradient_vertical",2) MW("gradient_texture",3)
    // Font charset
    MW("font_charset_ansi",    ANSI_CHARSET)
    MW("font_charset_default", DEFAULT_CHARSET)
    MW("font_charset_symbol",  SYMBOL_CHARSET)
    // Font family
    MW("font_family_any",     FF_DONTCARE)  MW("font_family_roman",    FF_ROMAN)
    MW("font_family_swiss",   FF_SWISS)     MW("font_family_modern",   FF_MODERN)
    MW("font_family_script",  FF_SCRIPT)    MW("font_family_decorative",FF_DECORATIVE)
    // Font pitch
    MW("font_pitch_default",    DEFAULT_PITCH)  MW("font_pitch_fixed",     FIXED_PITCH)
    MW("font_pitch_variable",   VARIABLE_PITCH) MW("font_pitch_monospaced", MONO_FONT)
    MW("font_truetype",         TRUETYPE_FONTTYPE)
    // DrawImage modes
    MW("image_copy",1) MW("image_stretch",2) MW("image_transparent_copy",3)
    // ImageOp actions
    MW("image_fill_ellipse",1) MW("image_fill_rectangle",2)
    MW("image_fill_round_fill_rectangle",3)
    // Brush styles
    MW("brush_solid",0)  MW("brush_null",1)
    MW("brush_hatch_horizontal",2)        MW("brush_hatch_vertical",3)
    MW("brush_hatch_forwards_diagonal",4) MW("brush_hatch_backwards_diagonal",5)
    MW("brush_hatch_cross",6)             MW("brush_hatch_cross_diagonal",7)
    MW("brush_fine_pattern",8)            MW("brush_medium_pattern",9)
    MW("brush_coarse_pattern",10)         MW("brush_waves_horizontal",11)
    MW("brush_waves_vertical",12)
    // Blend modes (all 64)
    MW("blend_normal",1)       MW("blend_average",2)               MW("blend_interpolate",3)
    MW("blend_dissolve",4)     MW("blend_darken",5)                MW("blend_multiply",6)
    MW("blend_colour_burn",7)  MW("blend_linear_burn",8)           MW("blend_inverse_colour_burn",9)
    MW("blend_subtract",10)    MW("blend_lighten",11)              MW("blend_screen",12)
    MW("blend_colour_dodge",13)MW("blend_linear_dodge",14)         MW("blend_inverse_colour_dodge",15)
    MW("blend_add",16)         MW("blend_overlay",17)              MW("blend_soft_light",18)
    MW("blend_hard_light",19)  MW("blend_vivid_light",20)          MW("blend_linear_light",21)
    MW("blend_pin_light",22)   MW("blend_hard_mix",23)             MW("blend_difference",24)
    MW("blend_exclusion",25)   MW("blend_reflect",26)              MW("blend_glow",27)
    MW("blend_freeze",28)      MW("blend_heat",29)                 MW("blend_negation",30)
    MW("blend_phoenix",31)     MW("blend_stamp",32)                MW("blend_xor",33)
    MW("blend_and",34)         MW("blend_or",35)
    MW("blend_red",36)         MW("blend_green",37)                MW("blend_blue",38)
    MW("blend_yellow",39)      MW("blend_cyan",40)                 MW("blend_magenta",41)
    MW("blend_green_limited_by_red",42)
    MW("blend_green_limited_by_blue",43)
    MW("blend_green_limited_by_average_of_red_and_blue",44)
    MW("blend_blue_limited_by_red",45)
    MW("blend_blue_limited_by_green",46)
    MW("blend_blue_limited_by_average_of_red_and_green",47)
    MW("blend_red_limited_by_green",48)
    MW("blend_red_limited_by_blue",49)
    MW("blend_red_limited_by_average_of_green_and_blue",50)
    MW("blend_red_only",51)    MW("blend_green_only",52)           MW("blend_blue_only",53)
    MW("blend_discard_red",54) MW("blend_discard_green",55)        MW("blend_discard_blue",56)
    MW("blend_all_red",57)     MW("blend_all_green",58)            MW("blend_all_blue",59)
    MW("blend_hue_mode",60)    MW("blend_saturation_mode",61)      MW("blend_colour_mode",62)
    MW("blend_luminance_mode",63) MW("blend_hsl",64)
    // Filter operations (all 27)
    MW("filter_noise",1)              MW("filter_monochrome_noise",2)  MW("filter_blur",3)
    MW("filter_sharpen",4)            MW("filter_find_edges",5)        MW("filter_emboss",6)
    MW("filter_brightness",7)         MW("filter_contrast",8)          MW("filter_gamma",9)
    MW("filter_red_brightness",10)    MW("filter_red_contrast",11)     MW("filter_red_gamma",12)
    MW("filter_green_brightness",13)  MW("filter_green_contrast",14)   MW("filter_green_gamma",15)
    MW("filter_blue_brightness",16)   MW("filter_blue_contrast",17)    MW("filter_blue_gamma",18)
    MW("filter_grayscale",19)         MW("filter_normal_grayscale",20)
    MW("filter_brightness_multiply",21)
    MW("filter_red_brightness_multiply",22)
    MW("filter_green_brightness_multiply",23)
    MW("filter_blue_brightness_multiply",24)
    MW("filter_lesser_blur",25)       MW("filter_minor_blur",26)       MW("filter_average",27)
    // Cursor modes for WindowAddHotspot
    MW("cursor_none",-1)       MW("cursor_arrow",0)        MW("cursor_hand",1)
    MW("cursor_ibeam",2)       MW("cursor_plus",3)         MW("cursor_wait",4)
    MW("cursor_up",5)          MW("cursor_nw_se_arrow",6)  MW("cursor_ne_sw_arrow",7)
    MW("cursor_ew_arrow",8)    MW("cursor_ns_arrow",9)     MW("cursor_both_arrow",10)
    MW("cursor_x",11)          MW("cursor_help",12)
    // Hotspot flags
    MW("hotspot_report_all_mouseovers", 0x01)
    MW("hotspot_got_shift",    0x01)   MW("hotspot_got_control",  0x02)
    MW("hotspot_got_alt",      0x04)   MW("hotspot_got_lh_mouse", 0x10)
    MW("hotspot_got_rh_mouse", 0x20)   MW("hotspot_got_dbl_click",0x40)
    MW("hotspot_got_not_first",0x80)   MW("hotspot_got_middle_mouse",0x200)
    // MergeImageAlpha modes
    MW("merge_straight",0)     MW("merge_transparent",1)
    // Drag handler callback flags
    MW("drag_got_shift",0x01)  MW("drag_got_control",0x02) MW("drag_got_alt",0x04)
    // Mouse wheel handler callback flags
    MW("wheel_got_shift",0x01) MW("wheel_got_control",0x02) MW("wheel_got_alt",0x04)
    MW("wheel_scroll_back",0x100)
    #undef MW
    lua_setglobal(L, "miniwin");

    // Register vendored C extension modules
    lua_pushcfunction(L, luaopen_rex);      lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_compress); lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_bits);     lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_bc);       lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_lpeg);     lua_call(L, 0, 0);

    // xterm 256-colour table (used by scripts that call extended_colours[n])
    lua_newtable(L);
    for (int i = 0; i < 256; i++) {
        lua_pushnumber(L, xterm_256_colours[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "extended_colours");

    return 0;
}

// ── DisableDLLs ───────────────────────────────────────────────────────────────
// On Windows this removes package.loadlib and the C-extension loaders so scripts
// cannot load arbitrary DLLs.  On macOS we do the same to keep security parity.

int DisableDLLs (lua_State * L)
{
    // Remove package.loadlib and DLL/C-extension loaders (indices 3 & 4)
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_pushnil(L); lua_setfield(L, -2, "loadlib");

        lua_getfield(L, -1, "loaders");
        if (lua_istable(L, -1)) {
            lua_pushnil(L); lua_rawseti(L, -2, 4);
            lua_pushnil(L); lua_rawseti(L, -2, 3);
        }
        lua_pop(L, 1);  // pop loaders table
    }
    lua_settop(L, 0);
    return 0;
}

#endif  // LUA_VERSION

#endif  // !_WIN32
