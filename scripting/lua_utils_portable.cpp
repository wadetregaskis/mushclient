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

    // miniwin table — mirrors miniwindow_flags[] in lua_methods.cpp.
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
    // WindowCreate flags (inline: MINIWINDOW_* values from miniwindow.h)
    MW("create_underneath",        0x01) MW("create_absolute_location", 0x02)
    MW("create_transparent",       0x04) MW("create_ignore_mouse",      0x08)
    MW("create_keep_hotspots",     0x10)
    // Pen styles (PS_* from mfc_ui.h)
    MW("pen_solid",        PS_SOLID)      MW("pen_dash",       PS_DASH)
    MW("pen_dot",          PS_DOT)        MW("pen_null",       PS_NULL)
    // Rect op actions
    MW("rect_frame",1) MW("rect_fill",2) MW("rect_invert",3) MW("rect_3d_rect",4)
    MW("rect_draw_edge",5) MW("rect_flood_fill_border",6) MW("rect_flood_fill_surface",7)
    // Circle op actions
    MW("circle_ellipse",1) MW("circle_rectangle",2) MW("circle_round_rectangle",3)
    MW("circle_chord",4)   MW("circle_pie",5)
    // Gradient modes
    MW("gradient_horizontal",1) MW("gradient_vertical",2) MW("gradient_texture",3)
    // Font charset
    MW("font_charset_ansi",    ANSI_CHARSET)
    MW("font_charset_default", DEFAULT_CHARSET)
    // Font family
    MW("font_family_any",     FF_DONTCARE)  MW("font_family_roman",    FF_ROMAN)
    MW("font_family_swiss",   FF_SWISS)     MW("font_family_modern",   FF_MODERN)
    MW("font_family_script",  FF_SCRIPT)    MW("font_family_decorative",FF_DECORATIVE)
    // Font pitch
    MW("font_pitch_default",  DEFAULT_PITCH) MW("font_pitch_fixed",   FIXED_PITCH)
    MW("font_pitch_variable", VARIABLE_PITCH)
    // DrawImage modes
    MW("image_copy",1) MW("image_stretch",2) MW("image_transparent_copy",3)
    // ImageOp actions
    MW("image_fill_ellipse",1) MW("image_fill_rectangle",2)
    MW("image_fill_round_fill_rectangle",3)
    // Brush styles
    MW("brush_solid",0) MW("brush_null",1)
    MW("brush_hatch_horizontal",2) MW("brush_hatch_vertical",3)
    MW("brush_hatch_forwards_diagonal",4) MW("brush_hatch_backwards_diagonal",5)
    MW("brush_hatch_cross",6) MW("brush_hatch_cross_diagonal",7)
    // Blend modes
    MW("blend_normal",1)       MW("blend_average",2)       MW("blend_interpolate",3)
    MW("blend_dissolve",4)     MW("blend_darken",5)        MW("blend_multiply",6)
    MW("blend_colour_burn",7)  MW("blend_linear_burn",8)   MW("blend_subtract",10)
    MW("blend_lighten",11)     MW("blend_screen",12)       MW("blend_colour_dodge",13)
    // Filter operations
    MW("filter_noise",1)       MW("filter_monochrome_noise",2) MW("filter_blur",3)
    MW("filter_sharpen",4)     MW("filter_find_edges",5)   MW("filter_emboss",6)
    MW("filter_brightness",7)  MW("filter_contrast",8)     MW("filter_gamma",9)
    MW("filter_grayscale",19)  MW("filter_normal_grayscale",20)
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
