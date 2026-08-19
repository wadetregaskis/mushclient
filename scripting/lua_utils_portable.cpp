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

// B-3 atomic flip: real headers (the headless stub is gone; mushcore is FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"  // DOCUMENT_STATE, WORLD_LIBRARY

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

// Date item (lua_helpers.h:7; Utilities.cpp:1022 on Windows).  Windows
// renders via COleDateTime::Format(0,0); the port's Format(DWORD,LCID)
// (platform/mfc_ui.h) renders the same instant as a local-time
// "%m/%d/%Y %I:%M:%S %p" string — the rendering the old portable
// GetLineInfo "timestr" locked in.  Used by lua_methods.cpp:2990 (GetLineInfo).
void MakeTableItem (lua_State * L, const char * name, const COleDateTime d)
{
    lua_pushstring(L, name);
    lua_pushstring(L, d.Format((DWORD) 0, (LCID) 0));
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

// tInfoTypeMapping / tInternalFunctionsTable now come from doc.h
// (methods_info.cpp adoption); InfoTypes[] is the REAL table from
// methods_info.cpp, InternalFunctionsTable[] the windows_stubs.cpp sentinel.
extern tInfoTypeMapping InfoTypes[];
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

// xterm 256-colour table (defined + initialised in doc_portable.cpp)
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

// ── UTF-8 valid ──────────────────────────────────────────────────────────────
// Matches the Windows utils.utf8valid contract: valid -> the string's length in
// code points (a number); invalid -> (nil, offset-of-first-bad-byte).
static int utils_utf8valid(lua_State* L)
{
    size_t len;
    const unsigned char* s = (const unsigned char*)luaL_checklstring(L, 1, &len);
    int iBad = valid_utf8(s, (int)len);    // already 1-based byte offset (== Windows erroroffset+1)
    if (iBad > 0) {
        lua_pushnil(L);
        lua_pushnumber(L, iBad);
        return 2;
    }
    int n = 0;
    for (size_t i = 0; i < len; n++) {
        unsigned char c = s[i];
        if      (c < 0x80)           i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else                         i += 1;
    }
    lua_pushnumber(L, n);
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
    char* path = g_wxhook_file_dialog(!bSave, title, defname, defext, nullptr, bSave,
                                      false /*multiSelect*/);
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
//
// These used to route through g_wxhook_append_to_notepad / _activate_notepad,
// and differed from the canonical bodies (scripting/lua_utils.cpp:1535, :1551)
// in three ways.  All three are fixed here by calling the same App methods
// Windows calls, which live in notepad_app_shared.cpp now:
//
//  * The hook looked a notepad up by TITLE ALONE (CWxNotepadFrame::Find) and
//    returned false when there was none.  App.AppendToTheNotepad matches on
//    title AND m_pRelatedWorld == NULL — an APP notepad, not a world's — and
//    CREATES one when there is no match.  So utils.appendtonotepad on a title
//    that did not exist yet silently did nothing on this platform, where
//    Windows opens a notepad.
//  * The "\n" → ENDLINE conversion was missing, so multi-line text arrived with
//    bare newlines into a control the canonical feeds CRLF.
//  * The notepad type was not passed at all; the canonical passes
//    eNotepadPacketDebug, which is what gives the window its colours.
static int utils_appendtonotepad(lua_State* L)
{
    const char* title   = luaL_checkstring(L, 1);
    const char* message = luaL_checkstring(L, 2);
    bool bReplace = lua_toboolean(L, 3) != 0;

    CString strMessage (message);
    strMessage.Replace ("\n", ENDLINE);

    bool ok = App.AppendToTheNotepad (title, strMessage, bReplace,
                                      eNotepadPacketDebug);
    lua_pushboolean(L, ok);
    return 1;
}
static int utils_activatenotepad(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    lua_pushboolean(L, App.ActivateNotepad (title) != 0);
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
// nil, not 0: there is no system menu font to query headless, and a fabricated
// 0 breaks the read-modify-restore idiom (kept in step with the real twin in
// scripting/lua_utils.cpp, which returns nil when SystemParametersInfo fails).
static int utils_menufontsize(lua_State* L)  { lua_pushnil(L); return 1; }
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

// The real RegisterLuaRoutines (lua_methods.cpp) finishes with
//     luaL_register (L, "utils", ptr_xmllib);
// On Windows ptr_xmllib points at lua_utils.cpp's "utils" grab-bag list
// (xmlread, pickers, msgbox, ...).  Exporting it here, pointing at the FULL
// portable utils_lib, makes that one call register the entire portable utils
// library — no lua_methods.cpp edits needed.
const struct luaL_Reg * ptr_xmllib = utils_lib;

// ── RegisterLuaRoutines / DisableDLLs ────────────────────────────────────────
// GONE (Campaign C flip): the REAL lua_methods.cpp is compiled into mushcore
// now and owns RegisterLuaRoutines (world library + _G metatable + all
// constants tables + luaL_register(L, "utils", ptr_xmllib) → the utils_lib
// above) and DisableDLLs (which also honours App.m_bEnablePackageLibrary and
// installs the debug.debug / os.exit / io.popen replacements the portable
// copy lacked).  The portable world library twin
// (scripting/lua_world_api_portable.cpp) is deleted.

#endif  // LUA_VERSION

#endif  // !_WIN32
