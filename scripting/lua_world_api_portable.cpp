// scripting/lua_world_api_portable.cpp
//
// Portable (non-Windows) implementation of the MUSHclient "world" Lua library.
//
// On Windows this is in lua_methods.cpp (~8 000 lines of COM-heavy code).
// Here we implement the essential subset: Note, ColourNote/Tell, Send, Execute,
// SetVariable/GetVariable, trigger/alias/timer CRUD, Simulate, StripANSI, Trim,
// Version, WorldAddress/Port/Name, and utility functions.
//
// Functions that require Windows-only COM/MFC internals are either stubbed or
// omitted until they can be ported properly.
//
// Guard: this file is ONLY compiled on non-Windows, when LUA_FOUND.

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "scripting/scripting.h"
#include "scripting/errors.h"
#include "scripting/shs.h"
#include "flags.h"          // eEnabled, eReplace, eTriggerRegularExpression …
#include "OtherTypes.h"     // eSendToLast, CTimer::eInterval/eAtTime …

#include "lualib.h"

#include <string>
#include <cstdio>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>   // chdir
#include <sys/stat.h>    // stat() for script file mtime
#include <sys/utsname.h> // uname() for OS version
#include <cstdlib>    // free() for g_wxhook_menu_dialog result
#include <ctime>      // clock_gettime, time()
#include <cmath>      // cos() for WindowBlendColor

#include "platform/wx_mfc_hooks.h"  // g_wxhook_pick_colour, g_wxhook_menu_dialog, …
#include "sqlite3/sqlite3.h"         // sqlite3_libversion()
extern "C" {                         // md5.h / sha256.h are C headers (no extern "C")
#include "scripting/md5.h"           // world.MD5
#include "scripting/sha256.h"        // world.SHA256
}
#include <png.h>                     // PNG_LIBPNG_VER_STRING
#ifdef __APPLE__
#  include <mach-o/dyld.h>           // _NSGetExecutablePath
#endif

using std::string;

// ── helpers ───────────────────────────────────────────────────────────────────

// The Lua type name for world userdata objects (mirrors lua_methods.cpp).
// RegisterWorldLib() creates a named metatable under this key so that
// GetWorld("name"):Note("hello") OOP-style calls work.
const char mushclient_typename[] = "mushclient.world";

// Retrieve the doc pointer, following Windows lua_methods.cpp doc() semantics:
//   • If argument 1 is a mushclient.world userdata (from GetWorld()), use that.
//     Remove it from the stack so the rest of the function sees the real args.
//   • Otherwise fall back to the doc stored in the Lua registry.
static CMUSHclientDoc* world_doc(lua_State* L)
{
    // Check if argument 1 is a world userdata (OOP call: w:Note("hello"))
    void* p = lua_touserdata(L, 1);
    if (p && lua_getmetatable(L, 1)) {
        lua_getfield(L, LUA_REGISTRYINDEX, mushclient_typename);
        int ok = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
        if (ok) {
            CMUSHclientDoc* pWanted = *(CMUSHclientDoc**)p;
            lua_remove(L, 1);  // strip userdata so callers see normal arg list
            // Verify the world still exists in the doc list
            for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
                CMUSHclientDoc* pDoc = (CMUSHclientDoc*)App.m_pWorldDocTemplate->GetNextDoc(pos);
                if (pDoc == pWanted) return pDoc;
            }
            luaL_error(L, "world is no longer available");
        }
    }
    // Fall back to the registry-stored doc (normal world.Note() call)
    lua_getfield(L, LUA_REGISTRYINDEX, DOCUMENT_STATE);
    CMUSHclientDoc* pDoc = static_cast<CMUSHclientDoc*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!pDoc)
        luaL_error(L, "world is no longer available");
    return pDoc;
}

// Push a full userdata representing pDoc with the mushclient.world metatable.
// This is what GetWorld()/GetWorldById() return so that OOP calls work.
static void push_world_userdata(lua_State* L, CMUSHclientDoc* pDoc)
{
    CMUSHclientDoc** ud = (CMUSHclientDoc**)lua_newuserdata(L, sizeof(CMUSHclientDoc*));
    *ud = pDoc;
    luaL_getmetatable(L, mushclient_typename);
    lua_setmetatable(L, -2);
}

// __tostring for world userdata objects — mirrors world2string() in lua_methods.cpp.
static int world2string_fn(lua_State* L) { lua_pushliteral(L, "world"); return 1; }

// Concatenate all string arguments on the Lua stack (like world.Note's vararg).
static string concat_args(lua_State* L, const char* delim = "", int first = 1)
{
    int n = lua_gettop(L);
    string out;
    lua_getglobal(L, "tostring");
    for (int i = first; i <= n; ++i) {
        lua_pushvalue(L, -1);   // tostring function
        lua_pushvalue(L, i);    // argument
        lua_call(L, 1, 1);
        const char* s = lua_tostring(L, -1);
        if (!s)
            luaL_error(L, "'tostring' must return a string");
        if (i > first) out += delim;
        out += s;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // pop tostring
    return out;
}

// ── Note / ColourNote / ColourTell ───────────────────────────────────────────

// world.Note(text, ...) — print a line to the output window
static int L_Note(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->Note(concat_args(L).c_str());
    return 0;
}

// world.ColourNote(fg, bg, text [, fg, bg, text ...])
// Each triple writes one segment; the last triple ends with a newline.
static int L_ColourNote(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i += 3) {
        const char* fg   = luaL_optstring(L, i,     "");
        const char* bg   = luaL_optstring(L, i + 1, "");
        const char* text = luaL_checkstring(L, i + 2);
        if ((i + 2) < n)
            pDoc->ColourTell(fg, bg, text);
        else
            pDoc->ColourNote(fg, bg, text);
    }
    return 0;
}

// world.ColourTell(fg, bg, text [, fg, bg, text ...])
static int L_ColourTell(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i += 3) {
        const char* fg   = luaL_optstring(L, i,     "");
        const char* bg   = luaL_optstring(L, i + 1, "");
        const char* text = luaL_checkstring(L, i + 2);
        pDoc->ColourTell(fg, bg, text);
    }
    return 0;
}

// world.Tell(text, ...) — print without newline
static int L_Tell(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    string s = concat_args(L);
    // real Tell, like Windows L_Tell (ColourTell would side-effect the
    // note-colour state via NoteColourName)
    pDoc->Tell(s.c_str());
    return 0;
}

// ── Variables ─────────────────────────────────────────────────────────────────

// world.SetVariable(name, value) → error code
static int L_SetVariable(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name  = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);

    CString strName(name);
    long rc = CheckObjectName(strName);
    if (rc != eOK) {
        lua_pushnumber(L, (lua_Number)rc);
        return 1;
    }

    CVariableMap& vmap = pDoc->GetVariableMap();
    CVariable* var = nullptr;
    if (vmap.Lookup(strName, var))
        delete var;

    var = new CVariable();
    var->strLabel    = strName;
    var->strContents = CString(value);
    vmap.SetAt(strName, var);

    lua_pushnumber(L, eOK);
    return 1;
}

// world.GetVariable(name) → string or nil
static int L_GetVariable(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);

    CString strName(name);
    if (CheckObjectName(strName) != eOK) {
        lua_pushnil(L);
        return 1;
    }

    CVariable* var = nullptr;
    if (!pDoc->GetVariableMap().Lookup(strName, var)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, static_cast<const char*>(var->strContents));
    return 1;
}

// world.DeleteVariable(name) → error code
static int L_DeleteVariable(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    CString strName(name);

    long rc = CheckObjectName(strName);
    if (rc != eOK) {
        lua_pushnumber(L, (lua_Number)rc);
        return 1;
    }

    CVariable* var = nullptr;
    if (!pDoc->GetVariableMap().Lookup(strName, var)) {
        lua_pushnumber(L, eVariableNotFound);
        return 1;
    }

    delete var;
    pDoc->GetVariableMap().RemoveKey(strName);
    lua_pushnumber(L, eOK);
    return 1;
}

// push_option_variant (defined with the Get*Option wrappers further down)
// marshals VARIANT results — incl. VT_ARRAY safearrays — exactly as
// lua_methods.cpp pushVariant does; the list functions below need it early.
static int push_option_variant(lua_State* L, VARIANT& v);

// world.GetVariableList() → table { name → value }
// Mirrors lua_methods.cpp GetVariableListHelper: the Lua binding is an
// extension returning name *and* value (the raw doc method returns a
// safearray of names only); shared with L_GetPluginVariableList.
static void GetVariableListHelper(lua_State* L, CMUSHclientDoc* pDoc)
{
    lua_newtable(L);

    CString strName;
    CVariable* var = nullptr;
    for (POSITION pos = pDoc->GetVariableMap().GetStartPosition(); pos; ) {
        pDoc->GetVariableMap().GetNextAssoc(pos, strName, var);
        lua_pushstring(L, static_cast<const char*>(strName));
        lua_pushstring(L, static_cast<const char*>(var->strContents));
        lua_rawset(L, -3);
    }
}

static int L_GetVariableList(lua_State* L)
{
    GetVariableListHelper(L, world_doc(L));
    return 1;
}

// ── Send / Execute ────────────────────────────────────────────────────────────

// world.Send(text, ...) → error code
static int L_Send(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    string msg = concat_args(L);

    if (pDoc->m_iConnectPhase != eConnectConnectedToMud) {
        lua_pushnumber(L, eWorldClosed);
        return 1;
    }
    if (pDoc->m_bPluginProcessingSent) {
        lua_pushnumber(L, eItemInUse);
        return 1;
    }

    pDoc->SendMsg(CString(msg.c_str()), pDoc->m_display_my_input != 0,
                  /*queue=*/false, /*log=*/false);
    lua_pushnumber(L, eOK);
    return 1;
}

// world.Execute(command) → error code
// The REAL command processor (methods_commands.cpp, adopted into mushcore):
// script prefix, command stacking, per-line split, ON_PLUGIN_COMMAND and
// EvaluateCommand — marshalled exactly as lua_methods.cpp.
static int L_Execute(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->Execute(luaL_checkstring(L, 1)));
    return 1;
}

// ── World information ─────────────────────────────────────────────────────────

// world.GetWorldName() → string
static int L_GetWorldName(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushstring(L, static_cast<const char*>(pDoc->m_mush_name));
    return 1;
}

// world.IsConnected() → bool
static int L_IsConnected(lua_State* L)
{
    lua_pushboolean(L, world_doc(L)->IsConnected());   // real method
    return 1;
}

// ── Error code helpers ────────────────────────────────────────────────────────

// world.ErrorDesc(code) → string description
// NOT rewired to the real CMUSHclientDoc::ErrorDesc: the real body walks
// error_descriptions[], whose full table lives in lua_methods.cpp:7418
// (mushapp-only, a Campaign C target) — mushcore links only the
// windows_stubs.cpp sentinel, so the real method headless would return the
// code formatted as a string.  This twin keeps the common descriptions
// until lua_methods.cpp adopts (then delete both the twin and the sentinel).
static int L_ErrorDesc(lua_State* L)
{
    long code = (long)luaL_checknumber(L, 1);
    // Basic mapping for the most common codes
    const char* desc = "Unknown error";
    switch (code) {
        case eOK:                       desc = "OK";                           break;
        case eWorldClosed:              desc = "World is closed";              break;
        case eItemInUse:                desc = "Item is in use";               break;
        case eCommandsNestedTooDeeply:  desc = "Commands nested too deeply";   break;
        case eVariableNotFound:         desc = "Variable not found";           break;
        default: break;
    }
    lua_pushstring(L, desc);
    return 1;
}

// ── Script name → DISPID helper ───────────────────────────────────────────────
// Resolve a Lua function name to its DISPID.  Returns DISPID_UNKNOWN if the
// engine is absent or the function doesn't exist.
static DISPID resolve_dispid(CMUSHclientDoc* pDoc, const char* name)
{
    if (!name || !*name) return DISPID_UNKNOWN;
    CScriptEngine* eng = pDoc->GetScriptEngine();
    if (!eng || !eng->IsLua()) return DISPID_UNKNOWN;
    return eng->GetLuaDispid(name);
}

// ── Trigger management ────────────────────────────────────────────────────────

// world.IsTrigger(name) → eOK or eTriggerNotFound
static int L_IsTrigger(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CTrigger* t = nullptr;
    lua_pushnumber(L, pDoc->GetTriggerMap().Lookup(name, t) ? eOK : eTriggerNotFound);
    return 1;
}

// world.DeleteTrigger(name) → error code
static int L_DeleteTrigger(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CTrigger* t = nullptr;
    if (!pDoc->GetTriggerMap().Lookup(name, t)) {
        lua_pushnumber(L, eTriggerNotFound); return 1;
    }
    if (t->bExecutingScript) {
        lua_pushnumber(L, eItemInUse); return 1;
    }
    delete t;
    pDoc->GetTriggerMap().RemoveKey(name);
    pDoc->SortTriggers();
    lua_pushnumber(L, eOK); return 1;
}

// world.EnableTrigger(name [, enabled=true]) → error code
static int L_EnableTrigger(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    bool enabled = lua_toboolean(L, 2) != 0 || lua_isnone(L, 2);
    CheckObjectName(name, false);
    CTrigger* t = nullptr;
    if (!pDoc->GetTriggerMap().Lookup(name, t)) {
        lua_pushnumber(L, eTriggerNotFound); return 1;
    }
    t->bEnabled = enabled;
    lua_pushnumber(L, eOK); return 1;
}

// world.AddTriggerEx(name, match, response, flags, colour, wildcard,
//                   sound, script, sendTo [, sequence]) → error code
static int L_AddTriggerEx(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name      = luaL_checkstring(L, 1);
    const char* match     = luaL_checkstring(L, 2);
    const char* response  = luaL_checkstring(L, 3);
    long        flags     = (long)luaL_checknumber(L, 4);
    short       colour    = (short)luaL_checknumber(L, 5);
    short       wildcard  = (short)luaL_checknumber(L, 6);
    const char* sound     = luaL_checkstring(L, 7);
    const char* script    = luaL_optstring(L, 8, "");
    short       sendTo    = (short)luaL_checknumber(L, 9);
    short       sequence  = (short)luaL_optnumber(L, 10, DEFAULT_TRIGGER_SEQUENCE);

    CString strName(name);
    CTrigger* existing = nullptr;
    bool bReplace = false;

    if (strName.IsEmpty())
        strName.Format("*trigger%ld", (long)pDoc->GetTriggerMap().GetCount());
    else {
        long rc = CheckObjectName(strName);
        if (rc != eOK) { lua_pushnumber(L, rc); return 1; }
    }

    if (pDoc->GetTriggerMap().Lookup(strName, existing)) {
        if (flags & eReplace) bReplace = true;
        else { lua_pushnumber(L, eTriggerAlreadyExists); return 1; }
    }
    if (!*match)     { lua_pushnumber(L, eTriggerCannotBeEmpty);   return 1; }
    if (sequence < 0 || sequence > 10000) { lua_pushnumber(L, eTriggerSequenceOutOfRange); return 1; }
    if (sendTo < 0 || sendTo >= eSendToLast)  { lua_pushnumber(L, eTriggerSendToInvalid); return 1; }

    // Resolve script DISPID (soft: if not found, just leave DISPID_UNKNOWN)
    DISPID dispid = resolve_dispid(pDoc, script);
    if (*script && dispid == DISPID_UNKNOWN) {
        lua_pushnumber(L, eScriptNameNotLocated); return 1;
    }

    // Compile regexp
    CString strRegexp;
    if (flags & eTriggerRegularExpression)
        strRegexp = match;
    else
        strRegexp = ConvertToRegularExpression(CString(match));

    t_regexp* regexp = nullptr;
    try {
        regexp = regcomp(strRegexp,
                         (flags & eIgnoreCase ? PCRE_CASELESS : 0));
    } catch (CException* e) { e->Delete(); lua_pushnumber(L, eBadRegularExpression); return 1; }
    catch (...) { lua_pushnumber(L, eBadRegularExpression); return 1; }

    if (bReplace) {
        delete existing;
        pDoc->GetTriggerMap().RemoveKey(strName);
    }

    CTrigger* t = new CTrigger();
    t->strLabel         = strName;
    t->strInternalName  = strName;
    t->trigger          = CString(match);
    t->contents         = CString(response);
    t->colour           = (colour < 0 || colour >= MAX_CUSTOM) ? (short)SAMECOLOUR : colour;
    t->ignore_case      = (flags & eIgnoreCase) != 0;
    t->bOmitFromOutput  = (flags & eOmitFromOutput) != 0;
    t->bKeepEvaluating  = (flags & eKeepEvaluating) != 0;
    t->omit_from_log    = (flags & eOmitFromLog) != 0;
    t->bEnabled         = (flags & eEnabled) != 0;
    t->bRegexp          = (flags & eTriggerRegularExpression) != 0;
    t->bExpandVariables = (flags & eExpandVariables) != 0;
    t->bTemporary       = (flags & eTemporary) != 0;
    t->bLowercaseWildcard = (flags & eLowercaseWildcard) != 0;
    t->bOneShot         = (flags & eTriggerOneShot) != 0;
    t->strProcedure     = CString(script);
    t->iClipboardArg    = (wildcard < 0 || wildcard > 10) ? 0 : wildcard;
    t->sound_to_play    = CString(sound);
    t->dispid           = dispid;
    t->regexp           = regexp;
    t->iSendTo          = sendTo;
    t->iSequence        = sequence;
    t->strVariable      = strName;

    pDoc->GetTriggerMap().SetAt(strName, t);
    pDoc->GetTriggerArray().Add(t);
    pDoc->SortTriggers();

    lua_pushnumber(L, eOK); return 1;
}

// world.AddTrigger(name, match, response, flags, colour, wildcard,
//                  sound [, script]) → error code
static int L_AddTrigger(lua_State* L)
{
    // AddTrigger(name, match, response, flags, colour, wildcard, sound [, script]).
    // The canonical binding tolerates missing trailing arguments (its my_check*
    // readers default them); pad the optional ones (colour/wildcard/sound/script)
    // so the delegated AddTriggerEx always sees a well-formed call, then append
    // its two extra arguments (send-to, sequence).
    while (lua_gettop(L) < 5) lua_pushnumber(L, 0);    // colour
    while (lua_gettop(L) < 6) lua_pushnumber(L, 0);    // wildcard
    while (lua_gettop(L) < 7) lua_pushstring(L, "");   // sound
    while (lua_gettop(L) < 8) lua_pushstring(L, "");   // script
    lua_pushnumber(L, eSendToWorld);                    // send-to (arg 9)
    lua_pushnumber(L, DEFAULT_TRIGGER_SEQUENCE);        // sequence (arg 10)
    return L_AddTriggerEx(L);
}

// ── Alias management ──────────────────────────────────────────────────────────

// world.IsAlias(name) → eOK or eAliasNotFound
static int L_IsAlias(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CAlias* a = nullptr;
    lua_pushnumber(L, pDoc->GetAliasMap().Lookup(name, a) ? eOK : eAliasNotFound);
    return 1;
}

// world.DeleteAlias(name) → error code
static int L_DeleteAlias(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CAlias* a = nullptr;
    if (!pDoc->GetAliasMap().Lookup(name, a)) {
        lua_pushnumber(L, eAliasNotFound); return 1;
    }
    if (a->bExecutingScript) {
        lua_pushnumber(L, eItemInUse); return 1;
    }
    delete a;
    pDoc->GetAliasMap().RemoveKey(name);
    pDoc->SortAliases();
    lua_pushnumber(L, eOK); return 1;
}

// world.EnableAlias(name [, enabled=true]) → error code
static int L_EnableAlias(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    bool enabled = lua_toboolean(L, 2) != 0 || lua_isnone(L, 2);
    CheckObjectName(name, false);
    CAlias* a = nullptr;
    if (!pDoc->GetAliasMap().Lookup(name, a)) {
        lua_pushnumber(L, eAliasNotFound); return 1;
    }
    a->bEnabled = enabled;
    lua_pushnumber(L, eOK); return 1;
}

// world.AddAlias(name, match, response, flags [, script]) → error code
static int L_AddAlias(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name      = luaL_checkstring(L, 1);
    const char* match     = luaL_checkstring(L, 2);
    const char* response  = luaL_checkstring(L, 3);
    long        flags     = (long)luaL_checknumber(L, 4);
    const char* script    = luaL_optstring(L, 5, "");

    CString strName(name);
    CAlias* existing = nullptr;
    bool bReplace = false;

    if (strName.IsEmpty())
        strName.Format("*alias%ld", (long)pDoc->GetAliasMap().GetCount());
    else {
        long rc = CheckObjectName(strName);
        if (rc != eOK) { lua_pushnumber(L, rc); return 1; }
    }

    if (pDoc->GetAliasMap().Lookup(strName, existing)) {
        if (flags & eReplace) bReplace = true;
        else { lua_pushnumber(L, eAliasAlreadyExists); return 1; }
    }
    if (!*match) { lua_pushnumber(L, eAliasCannotBeEmpty); return 1; }

    DISPID dispid = resolve_dispid(pDoc, script);
    if (*script && dispid == DISPID_UNKNOWN) {
        lua_pushnumber(L, eScriptNameNotLocated); return 1;
    }

    CString strRegexp;
    if (flags & eAliasRegularExpression)
        strRegexp = match;
    else
        strRegexp = ConvertToRegularExpression(CString(match));

    t_regexp* regexp = nullptr;
    try {
        regexp = regcomp(strRegexp,
                         (flags & eIgnoreAliasCase ? PCRE_CASELESS : 0));
    } catch (CException* e) { e->Delete(); lua_pushnumber(L, eBadRegularExpression); return 1; }
    catch (...) { lua_pushnumber(L, eBadRegularExpression); return 1; }

    if (bReplace) {
        delete existing;
        pDoc->GetAliasMap().RemoveKey(strName);
    }

    CAlias* a = new CAlias();
    a->strLabel         = strName;
    a->name             = CString(match);
    a->contents         = CString(response);
    a->bEnabled         = (flags & eEnabled) != 0;
    a->bRegexp          = (flags & eAliasRegularExpression) != 0;
    a->bTemporary       = (flags & eTemporary) != 0;
    a->bOneShot         = (flags & eAliasOneShot) != 0;
    a->bOmitFromOutput  = (flags & eAliasOmitFromOutput) != 0;
    a->strProcedure     = CString(script);
    a->dispid           = dispid;
    a->regexp           = regexp;
    a->iSequence        = DEFAULT_TRIGGER_SEQUENCE;

    pDoc->GetAliasMap().SetAt(strName, a);
    pDoc->GetAliasArray().Add(a);
    pDoc->SortAliases();

    lua_pushnumber(L, eOK); return 1;
}

// ── Timer management ──────────────────────────────────────────────────────────

// world.IsTimer(name) → eOK or eTimerNotFound
static int L_IsTimer(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CTimer* t = nullptr;
    lua_pushnumber(L, pDoc->GetTimerMap().Lookup(name, t) ? eOK : eTimerNotFound);
    return 1;
}

// world.DeleteTimer(name) → error code
static int L_DeleteTimer(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CTimer* t = nullptr;
    if (!pDoc->GetTimerMap().Lookup(name, t)) {
        lua_pushnumber(L, eTimerNotFound); return 1;
    }
    delete t;
    pDoc->GetTimerMap().RemoveKey(name);
    lua_pushnumber(L, eOK); return 1;
}

// world.EnableTimer(name [, enabled=true]) → error code
static int L_EnableTimer(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    bool enabled = lua_toboolean(L, 2) != 0 || lua_isnone(L, 2);
    CheckObjectName(name, false);
    CTimer* t = nullptr;
    if (!pDoc->GetTimerMap().Lookup(name, t)) {
        lua_pushnumber(L, eTimerNotFound); return 1;
    }
    t->bEnabled = enabled;
    lua_pushnumber(L, eOK); return 1;
}

// world.ResetTimer(name) → error code
static int L_ResetTimer(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    CString name(luaL_checkstring(L, 1));
    CheckObjectName(name, false);
    CTimer* t = nullptr;
    if (!pDoc->GetTimerMap().Lookup(name, t)) {
        lua_pushnumber(L, eTimerNotFound); return 1;
    }
    pDoc->ResetOneTimer(t);
    lua_pushnumber(L, eOK); return 1;
}

// world.AddTimer(name, hour, minute, second, response, flags [, script]) → error code
static int L_AddTimer(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name      = luaL_checkstring(L, 1);
    short       hour      = (short)luaL_checknumber(L, 2);
    short       minute    = (short)luaL_checknumber(L, 3);
    double      second    = luaL_checknumber(L, 4);
    const char* response  = luaL_checkstring(L, 5);
    long        flags     = (long)luaL_checknumber(L, 6);
    const char* script    = luaL_optstring(L, 7, "");

    CString strName(name);
    CTimer* existing = nullptr;
    bool bReplace = false;

    if (strName.IsEmpty())
        strName.Format("*timer%010ld", (long)pDoc->GetTimerMap().GetCount());
    else {
        long rc = CheckObjectName(strName);
        if (rc != eOK) { lua_pushnumber(L, rc); return 1; }
    }

    if (pDoc->GetTimerMap().Lookup(strName, existing)) {
        if (flags & eReplace) bReplace = true;
        else { lua_pushnumber(L, eTimerAlreadyExists); return 1; }
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0.0 || second > 59.9999) {
        lua_pushnumber(L, eTimeInvalid); return 1;
    }
    if (hour == 0 && minute == 0 && second == 0.0 && !(flags & eAtTime)) {
        lua_pushnumber(L, eTimeInvalid); return 1;
    }

    DISPID dispid = resolve_dispid(pDoc, script);
    if (*script && dispid == DISPID_UNKNOWN) {
        lua_pushnumber(L, eScriptNameNotLocated); return 1;
    }

    if (bReplace) {
        delete existing;
        pDoc->GetTimerMap().RemoveKey(strName);
    }

    CTimer* t = new CTimer();
    t->strLabel      = strName;
    t->iEveryHour    = hour;
    t->iEveryMinute  = minute;
    t->fEverySecond  = second;
    t->strContents   = CString(response);
    t->bEnabled      = (flags & eEnabled) != 0;
    t->bTemporary    = (flags & eTemporary) != 0;
    t->bOneShot      = (flags & eOneShot) != 0;
    t->iType         = (flags & eAtTime) ? CTimer::eAtTime : CTimer::eInterval;
    t->iSendTo       = eSendToWorld;
    t->strProcedure  = CString(script);
    t->dispid        = dispid;

    pDoc->GetTimerMap().SetAt(strName, t);
    pDoc->ResetOneTimer(t);

    lua_pushnumber(L, eOK); return 1;
}

// ── Utilities ─────────────────────────────────────────────────────────────────

// world.Simulate(text) — feed text as if received from the MUD
static int L_Simulate(lua_State* L)
{
    // Real CMUSHclientDoc::Simulate (methods_utilities.cpp, adopted into
    // mushcore); concat_args mirrors lua_methods.cpp concatArgs.
    CMUSHclientDoc* pDoc = world_doc(L);
    string text = concat_args(L);
    pDoc->Simulate(text.c_str());
    return 0;
}

// world.StripANSI(text) → text with ANSI codes removed.
// The free ::StripAnsi (methods_utilities.cpp, adopted into mushcore) —
// exactly as lua_methods.cpp L_StripANSI.
static int L_StripANSI(lua_State* L)
{
    lua_pushstring(L, StripAnsi(luaL_checkstring(L, 1)));
    return 1;
}

// world.Trim(text) → trimmed string — the real CMUSHclientDoc::Trim
// (methods_utilities.cpp: CString TrimLeft/TrimRight).
static int L_Trim(lua_State* L)
{
    BSTR str = world_doc(L)->Trim(luaL_checkstring(L, 1));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// world.Version() → version string (real method, methods_info.cpp)
static int L_Version(lua_State* L)
{
    BSTR str = world_doc(L)->Version();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// world.WorldAddress() → string (real method: the server host, m_server)
static int L_WorldAddress(lua_State* L)
{
    BSTR str = world_doc(L)->WorldAddress();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// world.WorldPort() → number (real method)
static int L_WorldPort(lua_State* L)
{
    lua_pushnumber(L, (lua_Number)world_doc(L)->WorldPort());
    return 1;
}

// world.WorldName() → string (real method: m_mush_name)
static int L_WorldName(lua_State* L)
{
    BSTR str = world_doc(L)->WorldName();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// world.AnsiNote(text) — like Note but preserves ANSI codes
static int L_AnsiNote(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    // the real AnsiNote (interprets embedded ANSI codes), like Windows
    pDoc->AnsiNote(concat_args(L).c_str());
    return 0;
}

// world.StopEvaluatingTriggers() — halt further trigger matching for this line
static int L_StopEvaluatingTriggers(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_iStopTriggerEvaluation = eStopEvaluatingTriggers;
    return 0;
}

// ── List functions ────────────────────────────────────────────────────────────
// Thin wrappers over the REAL CMUSHclientDoc list methods (methods_triggers/
// aliases/timers.cpp, adopted into mushcore), marshalled through
// push_option_variant's VT_ARRAY branch exactly as lua_methods.cpp does.
// Windows shape: a 1-based array of labels, or nil when the list is empty.

static int L_GetTriggerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetTriggerList();
    return push_option_variant(L, v);
}

static int L_GetAliasList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetAliasList();
    return push_option_variant(L, v);
}

static int L_GetTimerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetTimerList();
    return push_option_variant(L, v);
}

// Portable-only extra — Windows lua_methods.cpp has no GetVariableNameList
// (scripts get names as the keys of GetVariableList).  Kept for the headless
// test surface; always returns a table (possibly empty).
static int L_GetVariableNameList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int idx = 1;
    CString strLabel;
    CVariable* pv;
    for (POSITION pos = pDoc->GetVariableMap().GetStartPosition(); pos; ) {
        pDoc->GetVariableMap().GetNextAssoc(pos, strLabel, pv);
        lua_pushstring(L, (const char*)strLabel);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// ── GetTrigger / GetAlias / GetTimer ─────────────────────────────────────────
// Return multiple values matching the classic MUSHclient signatures.

static int L_GetTrigger(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    CString strLabel(name);
    CheckObjectName(strLabel, false);
    CTrigger* pt = nullptr;
    if (!pDoc->GetTriggerMap().Lookup(strLabel, pt) || !pt) {
        lua_pushnumber(L, eTriggerNotFound);
        lua_pushstring(L, ""); lua_pushstring(L, "");
        lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        lua_pushstring(L, ""); lua_pushstring(L, "");
        return 8;
    }
    long flags = 0;
    if (pt->bEnabled)            flags |= eEnabled;
    if (pt->bRegexp)             flags |= eTriggerRegularExpression;
    if (pt->ignore_case)         flags |= eIgnoreCase;
    if (pt->bOmitFromOutput)     flags |= eOmitFromOutput;
    if (pt->bKeepEvaluating)     flags |= eKeepEvaluating;
    if (pt->omit_from_log)       flags |= eOmitFromOutput;  // mirrors Windows
    if (pt->bLowercaseWildcard)  flags |= eLowercaseWildcard;
    if (pt->bTemporary)          flags |= eTemporary;
    if (pt->bOneShot)            flags |= eTriggerOneShot;

    short colour = (pt->colour == SAMECOLOUR) ? -1 : (short)pt->colour;

    lua_pushnumber(L, eOK);                                  // 1 result code
    lua_pushstring(L, (const char*)pt->trigger);             // 2 match text
    lua_pushstring(L, (const char*)pt->contents);            // 3 send text
    lua_pushnumber(L, flags);                                // 4 flags
    lua_pushnumber(L, colour);                               // 5 colour
    lua_pushnumber(L, pt->iClipboardArg);                    // 6 wildcard arg
    lua_pushstring(L, (const char*)pt->sound_to_play);       // 7 sound file
    lua_pushstring(L, (const char*)pt->strProcedure);        // 8 script name
    return 8;
}

static int L_GetAlias(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    CString strLabel(name);
    CheckObjectName(strLabel, false);
    CAlias* pa = nullptr;
    if (!pDoc->GetAliasMap().Lookup(strLabel, pa) || !pa) {
        lua_pushnumber(L, eAliasNotFound);
        lua_pushstring(L, ""); lua_pushstring(L, "");
        lua_pushnumber(L, 0); lua_pushstring(L, "");
        return 5;
    }
    long flags = 0;
    if (pa->bEnabled)                               flags |= eEnabled;
    if (pa->bRegexp)                                flags |= eAliasRegularExpression;
    if (pa->bIgnoreCase)                            flags |= eIgnoreAliasCase;
    if (pa->bOmitFromLog)                           flags |= eOmitFromLogFile;
    if (pa->bOmitFromOutput)                        flags |= eAliasOmitFromOutput;
    if (pa->bExpandVariables)                       flags |= eExpandVariables;
    if (pa->iSendTo == eSendToSpeedwalk)            flags |= eAliasSpeedWalk;
    if (pa->iSendTo == eSendToCommandQueue)         flags |= eAliasQueue;
    if (pa->bMenu)                                  flags |= eAliasMenu;
    if (pa->bTemporary)                             flags |= eTemporary;
    if (pa->bOneShot)                               flags |= eAliasOneShot;

    lua_pushnumber(L, eOK);                          // 1 result code
    lua_pushstring(L, (const char*)pa->name);        // 2 match text
    lua_pushstring(L, (const char*)pa->contents);    // 3 send text
    lua_pushnumber(L, flags);                        // 4 flags
    lua_pushstring(L, (const char*)pa->strProcedure);// 5 script name
    return 5;
}

static int L_GetTimer(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    CString strLabel(name);
    CheckObjectName(strLabel, false);
    CTimer* pt = nullptr;
    if (!pDoc->GetTimerMap().Lookup(strLabel, pt) || !pt) {
        lua_pushnumber(L, eTimerNotFound);
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        lua_pushstring(L, ""); lua_pushnumber(L, 0); lua_pushstring(L, "");
        return 7;
    }
    long flags = 0;
    if (pt->bEnabled)                    flags |= eEnabled;
    if (pt->iType == CTimer::eAtTime)    flags |= eAtTime;
    if (pt->bOneShot)                    flags |= eOneShot;
    if (pt->iSendTo == eSendToSpeedwalk) flags |= eTimerSpeedWalk;
    if (pt->iSendTo == eSendToOutput)    flags |= eTimerNote;
    if (pt->bActiveWhenClosed)           flags |= eActiveWhenClosed;

    int    hour   = (pt->iType == CTimer::eAtTime) ? pt->iAtHour    : pt->iEveryHour;
    int    minute = (pt->iType == CTimer::eAtTime) ? pt->iAtMinute  : pt->iEveryMinute;
    double second = (pt->iType == CTimer::eAtTime) ? pt->fAtSecond  : pt->fEverySecond;

    lua_pushnumber(L, eOK);                          // 1 result code
    lua_pushnumber(L, hour);                         // 2 hour
    lua_pushnumber(L, minute);                       // 3 minute
    lua_pushnumber(L, second);                       // 4 second
    lua_pushstring(L, (const char*)pt->strContents); // 5 send text
    lua_pushnumber(L, flags);                        // 6 flags
    lua_pushstring(L, (const char*)pt->strProcedure);// 7 script name
    return 7;
}

// ── Group enable/disable ──────────────────────────────────────────────────────

static int L_EnableTriggerGroup(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* group = luaL_checkstring(L, 1);
    bool enabled = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    long count = 0;
    CString strLabel; CTrigger* pt;
    for (POSITION pos = pDoc->GetTriggerMap().GetStartPosition(); pos; ) {
        pDoc->GetTriggerMap().GetNextAssoc(pos, strLabel, pt);
        if (pt->strGroup == CString(group)) {
            pt->bEnabled = enabled ? TRUE : FALSE;
            count++;
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

static int L_EnableAliasGroup(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* group = luaL_checkstring(L, 1);
    bool enabled = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    long count = 0;
    CString strLabel; CAlias* pa;
    for (POSITION pos = pDoc->GetAliasMap().GetStartPosition(); pos; ) {
        pDoc->GetAliasMap().GetNextAssoc(pos, strLabel, pa);
        if (pa->strGroup == CString(group)) {
            pa->bEnabled = enabled ? TRUE : FALSE;
            count++;
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

static int L_EnableTimerGroup(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* group = luaL_checkstring(L, 1);
    bool enabled = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    long count = 0;
    CString strLabel; CTimer* pt;
    for (POSITION pos = pDoc->GetTimerMap().GetStartPosition(); pos; ) {
        pDoc->GetTimerMap().GetNextAssoc(pos, strLabel, pt);
        if (pt->strGroup == CString(group)) {
            pt->bEnabled = enabled ? TRUE : FALSE;
            count++;
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

static int L_EnableGroup(lua_State* L)
{
    // Real CMUSHclientDoc::EnableGroup (methods_utilities.cpp →
    // EnableTriggerGroup + EnableAliasGroup + EnableTimerGroup).
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* group = luaL_checkstring(L, 1);
    bool enabled = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    lua_pushnumber(L, pDoc->EnableGroup(group, enabled ? TRUE : FALSE));
    return 1;
}

// ── Line count / received line access ────────────────────────────────────────

static int L_GetLineCount(lua_State* L)
{
    // Real CMUSHclientDoc::GetLineCount (methods_info.cpp:57-60): the count
    // of lines received (m_total_lines).  The old twin returned
    // m_newlines_received — that is GetInfo(310), not GetLineCount.
    lua_pushnumber(L, world_doc(L)->GetLineCount());
    return 1;
}

// ── GetInfo ───────────────────────────────────────────────────────────────────
// Returns world / session info fields by numeric type.
// Type numbers match the Windows COM API (see scripting/methods/methods_info.cpp).
//
// Strings (configuration — cases 1-89):
//   1   = server address (m_server)
//   2   = world name (m_mush_name)
//   3   = character name (m_name)
//   51  = current log file name
//   52  = last immediate script expression (always "")
//   72  = MUSHclient version string
//   87  = last command sent
//   88  = window title (set by SetTitle)
//   89  = main window title (set by SetMainTitle)
// Booleans (cases 101-125):
//   101 = no echo (TELNET ECHO suppressed)
//   104 = MXP active
//   106 = world close flag (not connected)
//   107 = currently connecting
//   109 = trace mode on
//   113 = world is active (has a GUI view)
//   114 = output window paused (user scrolled up)
// Numbers (cases 201-289):
//   201 = total lines received
//   202 = new (unseen) lines
//   203 = total lines sent
//   212 = output font height in pixels
//   213 = output font width (average) in pixels
//   214 = input font height (same as output)
//   215 = input font width (same as output)
//   216 = total bytes received
//   217 = total bytes sent
//   218 = number of variables
//   219 = number of triggers
//   220 = number of timers
//   221 = number of aliases
//   222 = queued commands (speed-walk queue)
//   223 = number of map waypoints
//   224 = lines currently in output buffer
//   225 = custom MXP elements
//   226 = custom MXP entities
//   227 = connect phase (eConnect* enum value)
//   230 = script execution depth
//   231 = log file size in bytes
//   235 = number of command windows (always 1)
//   236 = command selection start column
//   237 = command selection end column
//   239 = current action source (eUnknownActionSource etc.)
//   240 = output font width (duplicate of 213)
//   241 = output font height (duplicate of 212)
//   244 = triggers evaluated count
//   245 = triggers matched count
//   246 = aliases evaluated count
//   247 = aliases matched count
//   248 = timers fired count
//   289 = last line with IAC GA

static int L_GetInfo(lua_State* L)
{
    // The REAL CMUSHclientDoc::GetInfo (methods_info.cpp, adopted into
    // mushcore) marshalled through push_option_variant — byte-for-byte the
    // lua_methods.cpp L_GetInfo shape.  The 400-line hand-mirrored switch
    // that lived here is gone; view-metric types use the file's own
    // non-Windows arms (m_gui_* pane metrics, IWorldView seams), toolbar
    // and OS-version types return the headless zeros, and the VT_DATE types
    // (301-306) marshal as Unix seconds exactly as Windows pushVariant.
    // (The old twin's extra "case 141" alias for 201 was twin-only: Windows
    // GetInfo(141) is unassigned → VT_NULL → nil.)
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetInfo((long)luaL_checknumber(L, 1));
    return push_option_variant(L, v);
}

// ── GetOption / SetOption ─────────────────────────────────────────────────────
// Thin wrappers over the REAL CMUSHclientDoc::GetOption / SetOption
// (scriptingoptions.cpp, compiled into mushcore since the option
// convergence), mirroring lua_methods.cpp L_GetOption / L_SetOption
// marshalling exactly.  The former hand-mirrored live-member switches and
// the m_numericOptionCache / m_alphaOptionCache backing store are gone.

// push_option_variant (forward-declared near the top of this file) is defined
// further down with the Get/Set{Trigger,...}Option wrappers.

static int L_GetOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->GetOption(luaL_checkstring(L, 1)));
    return 1;
}

static int L_SetOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_Number option;

    // if boolean convert to 0 or 1 (lua_methods.cpp L_SetOption)
    if (lua_isboolean(L, 2))
        option = lua_toboolean(L, 2) ? 1 : 0;
    else if (lua_isnil(L, 2))
        option = 0;
    else
        option = luaL_checknumber(L, 2);

    lua_pushnumber(L, pDoc->SetOption(
        luaL_checkstring(L, 1),   // OptionName
        option                    // Value
        ));
    return 1;
}

// ── GetTriggerInfo / GetAliasInfo / GetTimerInfo ──────────────────────────────
// Return individual properties by numeric InfoType.
// Returns nil if the named object doesn't exist or type is unknown.
// Shared helpers (also used by GetPlugin*Info variants).

static void push_trigger_info(lua_State* L, CTrigger* t, int infoType)
{
    switch (infoType) {
        case  1: lua_pushstring(L, (const char*)t->trigger);           return;
        case  2: lua_pushstring(L, (const char*)t->contents);          return;
        case  3: lua_pushstring(L, (const char*)t->sound_to_play);     return;
        case  4: lua_pushstring(L, (const char*)t->strProcedure);      return;
        case  5: lua_pushboolean(L, t->omit_from_log);                 return;
        case  6: lua_pushboolean(L, t->bOmitFromOutput);               return;
        case  7: lua_pushboolean(L, t->bKeepEvaluating);               return;
        case  8: lua_pushboolean(L, t->bEnabled);                      return;
        case  9: lua_pushboolean(L, t->bRegexp);                       return;
        case 10: lua_pushboolean(L, t->ignore_case);                   return;
        case 11: lua_pushboolean(L, t->bRepeat);                       return;
        case 12: lua_pushboolean(L, t->bSoundIfInactive);              return;
        case 13: lua_pushboolean(L, t->bExpandVariables);              return;
        case 14: lua_pushnumber(L, t->iClipboardArg);                  return;
        case 15: lua_pushnumber(L, t->iSendTo);                        return;
        case 16: lua_pushnumber(L, t->iSequence);                      return;
        case 17: lua_pushnumber(L, t->iMatch);                         return;
        case 18: lua_pushnumber(L, t->iStyle);                         return;
        case 19: lua_pushnumber(L, t->colour);                         return;
        case 20: lua_pushnumber(L, t->nInvocationCount);               return;
        case 21: lua_pushnumber(L, t->nMatched);                       return;
        case 22: {  // last-matched time as Unix timestamp (0 = never)
            time_t ts = t->tWhenMatched.GetTime();
            if (ts) lua_pushnumber(L, (double)ts); else lua_pushnil(L);
            return;
        }
        case 23: lua_pushboolean(L, t->bTemporary);                    return;
        case 24: lua_pushboolean(L, t->bIncluded);                     return;
        case 25: lua_pushboolean(L, t->bLowercaseWildcard);            return;
        case 26: lua_pushstring(L, (const char*)t->strGroup);          return;
        case 27: lua_pushstring(L, (const char*)t->strVariable);       return;
        case 28: lua_pushnumber(L, t->iUserOption);                    return;
        case 29: lua_pushnumber(L, t->iOtherForeground);               return;
        case 30: lua_pushnumber(L, t->iOtherBackground);               return;
        case 31: lua_pushnumber(L, t->regexp ? t->regexp->m_iCount : 0);          return;
        case 32: lua_pushstring(L, t->regexp ? t->regexp->m_sTarget.c_str() : ""); return;
        case 33: lua_pushboolean(L, t->bExecutingScript);              return;
        case 34: lua_pushboolean(L, t->dispid != DISPID_UNKNOWN);      return;
        case 35: lua_pushnumber(L, (t->regexp && t->regexp->m_program == NULL)
                                    ? t->regexp->m_iExecutionError : 0); return;
        case 36: lua_pushboolean(L, t->bOneShot);                      return;
        case 37: lua_pushnumber(L, 0); return;  // regexp timing (no perf counter on non-Windows)
        case 38: lua_pushnumber(L, t->regexp ? (double)t->regexp->m_iMatchAttempts : 0); return;
        // Wildcard results: 101..109 = wildcards[1..9], 110 = wildcards[0]
        case 101: case 102: case 103: case 104: case 105:
        case 106: case 107: case 108: case 109: {
            int w = infoType - 100;
            lua_pushstring(L, t->wildcards[w].c_str()); return;
        }
        case 110: lua_pushstring(L, t->wildcards[0].c_str()); return;
        default: lua_pushnil(L); return;
    }
}

static int L_GetTriggerInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int infoType = (int)luaL_checknumber(L, 2);
    CTrigger* t = nullptr;
    if (!pDoc->GetTriggerMap().Lookup(CString(name), t) || !t) {
        lua_pushnil(L); return 1;
    }
    push_trigger_info(L, t, infoType);
    return 1;
}

static void push_alias_info(lua_State* L, CAlias* a, int infoType)
{
    switch (infoType) {
        case  1: lua_pushstring(L, (const char*)a->name);              return;
        case  2: lua_pushstring(L, (const char*)a->contents);          return;
        case  3: lua_pushstring(L, (const char*)a->strProcedure);      return;
        case  4: lua_pushboolean(L, a->bOmitFromLog);                  return;
        case  5: lua_pushboolean(L, a->bOmitFromOutput);               return;
        case  6: lua_pushboolean(L, a->bEnabled);                      return;
        case  7: lua_pushboolean(L, a->bRegexp);                       return;
        case  8: lua_pushboolean(L, a->bIgnoreCase);                   return;
        case  9: lua_pushboolean(L, a->bExpandVariables);              return;
        case 10: lua_pushnumber(L, a->nInvocationCount);               return;
        case 11: lua_pushnumber(L, a->nMatched);                       return;
        case 12: lua_pushboolean(L, a->bMenu);                         return;
        case 13: {  // last-matched time as Unix timestamp (nil = never)
            time_t ts = a->tWhenMatched.GetTime();
            if (ts) lua_pushnumber(L, (double)ts); else lua_pushnil(L);
            return;
        }
        case 14: lua_pushboolean(L, a->bTemporary);                    return;
        case 15: lua_pushboolean(L, a->bIncluded);                     return;
        case 16: lua_pushstring(L, (const char*)a->strGroup);          return;
        case 17: lua_pushstring(L, (const char*)a->strVariable);       return;
        case 18: lua_pushnumber(L, a->iSendTo);                        return;
        case 19: lua_pushboolean(L, a->bKeepEvaluating);               return;
        case 20: lua_pushnumber(L, a->iSequence);                      return;
        case 21: lua_pushboolean(L, a->bEchoAlias);                    return;
        case 22: lua_pushboolean(L, a->bOmitFromCommandHistory);       return;
        case 23: lua_pushnumber(L, a->iUserOption);                    return;
        case 24: lua_pushnumber(L, a->regexp ? a->regexp->m_iCount : 0);          return;
        case 25: lua_pushstring(L, a->regexp ? a->regexp->m_sTarget.c_str() : ""); return;
        case 26: lua_pushboolean(L, a->bExecutingScript);              return;
        case 27: lua_pushboolean(L, a->dispid != DISPID_UNKNOWN);      return;
        case 28: lua_pushnumber(L, (a->regexp && a->regexp->m_program == NULL)
                                    ? a->regexp->m_iExecutionError : 0); return;
        case 29: lua_pushboolean(L, a->bOneShot);                      return;
        case 30: lua_pushnumber(L, 0); return;  // regexp timing (no perf counter on non-Windows)
        case 31: lua_pushnumber(L, a->regexp ? (double)a->regexp->m_iMatchAttempts : 0); return;
        // Wildcard results: 101..109 = wildcards[1..9], 110 = wildcards[0]
        case 101: case 102: case 103: case 104: case 105:
        case 106: case 107: case 108: case 109: {
            int w = infoType - 100;
            lua_pushstring(L, a->wildcards[w].c_str()); return;
        }
        case 110: lua_pushstring(L, a->wildcards[0].c_str()); return;
        default: lua_pushnil(L); return;
    }
}

static int L_GetAliasInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int infoType = (int)luaL_checknumber(L, 2);
    CAlias* a = nullptr;
    if (!pDoc->GetAliasMap().Lookup(CString(name), a) || !a) {
        lua_pushnil(L); return 1;
    }
    push_alias_info(L, a, infoType);
    return 1;
}

static void push_timer_info(lua_State* L, CTimer* t, int infoType)
{
    bool atTime = (t->iType == CTimer::eAtTime);
    switch (infoType) {
        case  1: lua_pushnumber(L, atTime ? t->iAtHour    : t->iEveryHour);    return;
        case  2: lua_pushnumber(L, atTime ? t->iAtMinute  : t->iEveryMinute);  return;
        case  3: lua_pushnumber(L, atTime ? t->fAtSecond  : t->fEverySecond);  return;
        case  4: lua_pushstring(L, (const char*)t->strContents);               return;
        case  5: lua_pushstring(L, (const char*)t->strProcedure);              return;
        case  6: lua_pushboolean(L, t->bEnabled);                              return;
        case  7: lua_pushboolean(L, t->bOneShot);                              return;
        case  8: lua_pushboolean(L, atTime);                                   return;
        case  9: lua_pushnumber(L, t->nInvocationCount);                       return;
        case 10: lua_pushnumber(L, t->nMatched);                               return;
        case 11: {  // last-fired time as Unix timestamp (nil = never)
            // CmcDateTime stores OLE dates (days since 1899-12-30); convert to Unix epoch
            double ole = t->tWhenFired.GetTime();
            if (ole != 0.0) lua_pushnumber(L, (ole - 25569.0) * 86400.0);
            else             lua_pushnil(L);
            return;
        }
        case 12: {  // next scheduled fire time as Unix timestamp (nil = not set)
            double ole = t->tFireTime.GetTime();
            if (ole != 0.0) lua_pushnumber(L, (ole - 25569.0) * 86400.0);
            else             lua_pushnil(L);
            return;
        }
        case 13: {
            CmcDateTime tNow = CmcDateTime::GetTimeNow();
            if (t->tFireTime <= tNow) { lua_pushnumber(L, 0); }
            else {
                CmcDateTimeSpan ts = t->tFireTime - tNow;
                lua_pushnumber(L, ts.GetTotalSeconds());
            }
            return;
        }
        case 14: lua_pushboolean(L, t->bTemporary);                            return;
        case 15: lua_pushboolean(L, t->iSendTo == eSendToSpeedwalk);           return;
        case 16: lua_pushboolean(L, t->iSendTo == eSendToOutput);              return;
        case 17: lua_pushboolean(L, t->bActiveWhenClosed);                     return;
        case 18: lua_pushboolean(L, t->bIncluded);                             return;
        case 19: lua_pushstring(L, (const char*)t->strGroup);                  return;
        case 20: lua_pushnumber(L, t->iSendTo);                                return;
        case 21: lua_pushnumber(L, t->iUserOption);                            return;
        case 22: lua_pushstring(L, (const char*)t->strLabel);                  return;
        case 23: lua_pushboolean(L, t->bOmitFromOutput);                       return;
        case 24: lua_pushboolean(L, t->bOmitFromLog);                          return;
        case 25: lua_pushboolean(L, t->bExecutingScript);                      return;
        case 26: lua_pushboolean(L, t->dispid != DISPID_UNKNOWN);              return;
        default: lua_pushnil(L); return;
    }
}

static int L_GetTimerInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int infoType = (int)luaL_checknumber(L, 2);
    CTimer* t = nullptr;
    if (!pDoc->GetTimerMap().Lookup(CString(name), t) || !t) {
        lua_pushnil(L); return 1;
    }
    push_timer_info(L, t, infoType);
    return 1;
}

// ── GetUniqueNumber ───────────────────────────────────────────────────────────
// Returns a monotonically increasing integer, unique within this session.

static int L_GetUniqueNumber(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetUniqueNumber());   // real method
    return 1;
}

// ── Connection control ────────────────────────────────────────────────────────

// world.IsConnected() → bool  (already registered above as L_IsConnected)
// world.Connect() — initiate connection if not already connected.
static int L_ConnectWorld(lua_State* L)
{
    // Real CMUSHclientDoc::Connect (methods_utilities.cpp): eOK when a
    // connection was initiated, eWorldOpen when already open — the old twin
    // always returned eOK.
    lua_pushnumber(L, world_doc(L)->Connect());
    return 1;
}

// world.Disconnect() → error_code
static int L_DisconnectWorld(lua_State* L)
{
    // Real CMUSHclientDoc::Disconnect (methods_utilities.cpp): eOK when a
    // disconnect was initiated, eWorldClosed when already closed — the old
    // twin always returned eOK.
    lua_pushnumber(L, world_doc(L)->Disconnect());
    return 1;
}

// ── World identity ────────────────────────────────────────────────────────────

// world.GetWorldID() → string  (stable UUID-like ID for this world)
// On Windows this is persisted in the world file.  On portable we generate
// one on first access using the world name + port as a seed.
static int L_GetWorldID(lua_State* L)
{
    // Real CMUSHclientDoc::GetWorldID (methods_info.cpp) — the ctor seeds
    // m_strWorldID with GetUniqueID() exactly as doc_construct.cpp, so the
    // old on-demand pseudo-UUID fallback is dead.
    BSTR str = world_doc(L)->GetWorldID();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── Status bar ────────────────────────────────────────────────────────────────

// world.SetStatus(text) — update the GUI status bar (if a callback is wired)
static int L_SetStatus(lua_State* L)
{
    // Real CMUSHclientDoc::SetStatus (methods_utilities.cpp): stores
    // m_strStatusMessage (GetInfo(53)) and ShowStatusLine()s it.  Keep the
    // IWorldView status push too — that is the port GUI surface the old twin
    // drove (the pal-worldview conformance test observes it).
    CMUSHclientDoc* pDoc = world_doc(L);
    string text = concat_args(L);   // lua_methods.cpp uses concatArgs here
    pDoc->SetStatus(text.c_str());
    pDoc->EmitStatus(text.c_str());
    return 0;
}

// ── Colour note helpers ───────────────────────────────────────────────────────

// world.NoteColourRGB(foreRGB, backRGB) — set colour for subsequent Note calls
static int L_NoteColourRGB(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // matches the real NoteColourRGB semantics
    pDoc->m_iNoteColourFore = (COLORREF)(long)luaL_checknumber(L, 1);
    pDoc->m_iNoteColourBack = (COLORREF)(long)luaL_checknumber(L, 2);
    return 0;
}

// world.NoteColourFore(foreRGB) — set foreground note colour only
static int L_NoteColourFore(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // real NoteColourFore/SetNoteColourFore set this
    pDoc->m_iNoteColourFore = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

// world.NoteColourBack(backRGB) — set background note colour only
static int L_NoteColourBack(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // real NoteColourBack/SetNoteColourBack set this
    pDoc->m_iNoteColourBack = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

// world.NormalColour() — reset to default note colours
static int L_NormalColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // resets to RGB defaults, so RGB mode
    pDoc->m_iNoteColourFore = RGB(192, 192, 192);
    pDoc->m_iNoteColourBack = RGB(0,   0,   0);
    return 0;
}

// ── Line buffer access ────────────────────────────────────────────────────────

// world.GetLinesInBufferCount() → number of lines in the output buffer
static int L_GetLinesInBufferCount(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (double)pDoc->m_LineList.GetCount());
    return 1;
}

// Helper: walk m_LineList to the nth line (0-based from head)
static CLine* GetLineAt(CMUSHclientDoc* pDoc, long n)
{
    long count = pDoc->m_LineList.GetCount();
    if (n < 0) n = count + n;       // negative = from tail
    if (n < 0 || n >= count) return nullptr;
    POSITION pos = pDoc->m_LineList.GetHeadPosition();
    for (long i = 0; i < n && pos; ++i)
        pDoc->m_LineList.GetNext(pos);
    return pos ? pDoc->m_LineList.GetAt(pos) : nullptr;
}

// world.GetLineInfo(lineNumber [, infoType]) → table or specific value
// lineNumber is 1-based (positive from head, negative from tail).
// infoType 0 (default) returns a full table; higher types return scalars.
static int L_GetLineInfo(lua_State* L)
{
    // lua_methods.cpp L_GetLineInfo shape: InfoType omitted/0 → a table of
    // ALL fields (Lua extension); otherwise the REAL
    // CMUSHclientDoc::GetLineInfo scalar via push_option_variant (type 9
    // arrives as Unix seconds, as Windows pushVariant delivers VT_DATE).
    // Out-of-range lines: no results for the table form, nil for the scalar
    // form — exactly Windows.  (The old twin also accepted NEGATIVE line
    // numbers counting from the end; that extension is gone — Windows
    // rejects them.)
    CMUSHclientDoc* pDoc = world_doc(L);
    long iLine = (long)luaL_checknumber(L, 1);
    int  iType = (int)luaL_optnumber(L, 2, 0);

    if (iType == 0) {
        // check line exists
        if (iLine <= 0 || iLine > pDoc->m_LineList.GetCount())
            return 0;   // no result for no line

        CLine* pLine = pDoc->m_LineList.GetAt(pDoc->GetLinePosition(iLine - 1));

        lua_newtable(L);
        lua_pushlstring(L, pLine->text ? pLine->text : "", (size_t)pLine->len);
        lua_setfield(L, -2, "text");                                    // 1
        lua_pushnumber(L, pLine->len);
        lua_setfield(L, -2, "length");                                  // 2
        lua_pushboolean(L, pLine->hard_return ? 1 : 0);
        lua_setfield(L, -2, "newline");                                 // 3
        lua_pushboolean(L, (pLine->flags & COMMENT)    ? 1 : 0);
        lua_setfield(L, -2, "note");                                    // 4
        lua_pushboolean(L, (pLine->flags & USER_INPUT) ? 1 : 0);
        lua_setfield(L, -2, "user");                                    // 5
        lua_pushboolean(L, (pLine->flags & LOG_LINE)   ? 1 : 0);
        lua_setfield(L, -2, "log");                                     // 6
        lua_pushboolean(L, (pLine->flags & BOOKMARK)   ? 1 : 0);
        lua_setfield(L, -2, "bookmark");                                // 7
        lua_pushboolean(L, (pLine->flags & HORIZ_RULE) ? 1 : 0);
        lua_setfield(L, -2, "hr");                                      // 8
        time_t t = (time_t)pLine->m_theTime.GetTime();
        lua_pushnumber(L, (double)t);
        lua_setfield(L, -2, "time");                                    // 9a
        {   // 9b "timestr" — Windows: MakeTableItem(COleDateTime); the port
            // formats the same instant as a local-time string.
            struct tm* tm_info = localtime(&t);
            char buf[32] = "";
            if (tm_info)
                strftime(buf, sizeof(buf), "%m/%d/%Y %I:%M:%S %p", tm_info);
            lua_pushstring(L, buf);
            lua_setfield(L, -2, "timestr");
        }
        lua_pushnumber(L, (double)pLine->m_nLineNumber);
        lua_setfield(L, -2, "line");                                    // 10
        lua_pushnumber(L, (double)pLine->styleList.GetCount());
        lua_setfield(L, -2, "styles");                                  // 11
        {   // "ticks" — Windows: line QPC time / counter frequency (0 when
            // no high-resolution counter, as headless).
            double ticks = 0;
            if (App.m_iCounterFrequency)
                ticks = (double)pLine->m_lineHighPerformanceTime.QuadPart /
                        (double)App.m_iCounterFrequency;
            lua_pushnumber(L, ticks);
            lua_setfield(L, -2, "ticks");
        }
        {   // "elapsed" — seconds since the world started (the CTime
            // fallback arm of lua_methods.cpp when no counter frequency).
            double fElapsedTime;
            if (App.m_iCounterFrequency)
                fElapsedTime = ((double)(pLine->m_lineHighPerformanceTime.QuadPart -
                                         pDoc->m_whenWorldStartedHighPrecision.QuadPart)) /
                               ((double)App.m_iCounterFrequency);
            else
                fElapsedTime = pLine->m_theTime.GetTime() -
                               (double)pDoc->m_whenWorldStarted.GetTime();
            lua_pushnumber(L, fElapsedTime);
            lua_setfield(L, -2, "elapsed");
        }
        return 1;   // one table
    }

    // normal behaviour — the real method
    VARIANT v = pDoc->GetLineInfo(iLine, (short)iType);
    return push_option_variant(L, v);
}

// world.GetCurrentValue(optionName) → current value of a world option
// Bridges to GetOption / GetAlphaOption depending on what the option is.
static int L_GetCurrentValue(lua_State* L)
{
    // Thin wrapper over the real doc method (methods_defaults.cpp), the same
    // marshalling as Windows lua_methods.cpp.
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetCurrentValue(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

// ── Speedwalk ────────────────────────────────────────────────────────────────

// world.EvaluateSpeedwalk(text) → expanded command string
static int L_EvaluateSpeedwalk(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* sw = luaL_checkstring(L, 1);
    CString result = pDoc->DoEvaluateSpeedwalk(sw);
    lua_pushstring(L, (const char*)result);
    return 1;
}

// ── Hashing ───────────────────────────────────────────────────────────────────

// world.Hash(text) → hex SHA1-style hash string
// Uses the portable SHS (SHA-1) implementation already in mushcore.
static int L_Hash(lua_State* L)
{
    BSTR str = world_doc(L)->Hash(luaL_checkstring(L, 1));   // real method
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// world.MD5(text) → 32-char lowercase hex MD5 digest (vendored md5.c).
static int L_MD5(lua_State* L)
{
    size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    md5_context ctx;
    unsigned char digest[16];
    md5_starts(&ctx);
    md5_update(&ctx, (unsigned char*)text, (unsigned int)len);
    md5_finish(&ctx, digest);
    char buf[33];
    for (int i = 0; i < 16; ++i) snprintf(buf + i * 2, 3, "%02x", digest[i]);
    lua_pushstring(L, buf);
    return 1;
}

// world.SHA256(text) → 64-char lowercase hex SHA-256 digest (vendored sha256.c).
static int L_SHA256(lua_State* L)
{
    size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    sha256_context ctx;
    unsigned char digest[32];
    sha256_starts(&ctx);
    sha256_update(&ctx, (unsigned char*)text, (unsigned int)len);
    sha256_finish(&ctx, digest);
    char buf[65];
    for (int i = 0; i < 32; ++i) snprintf(buf + i * 2, 3, "%02x", digest[i]);
    lua_pushstring(L, buf);
    return 1;
}

// ── Plugin helpers ────────────────────────────────────────────────────────────

// world.GetPluginName() → name of the currently-executing plugin, or ""
static int L_GetPluginName(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_CurrentPlugin)
        lua_pushstring(L, (const char*)pDoc->m_CurrentPlugin->m_strName);
    else
        lua_pushstring(L, "");
    return 1;
}

static int L_IsPlugin(lua_State* L);  // forward declaration

// world.IsPluginInstalled(pluginID) → bool
// Thin wrapper over the REAL CMUSHclientDoc::IsPluginInstalled — mirrors
// lua_methods.cpp L_IsPluginInstalled.
static int L_IsPluginInstalled(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushboolean(L, pDoc->IsPluginInstalled(luaL_checkstring(L, 1)));
    return 1;
}

// world.EnablePlugin(pluginID [, enabled]) → error_code
// Thin wrapper over the REAL CMUSHclientDoc::EnablePlugin (methods_plugins.cpp,
// adopted into mushcore) — mirrors lua_methods.cpp L_EnablePlugin marshalling
// (enabled flag defaults to true).
static int L_EnablePlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    BOOL enable = (lua_isnoneornil(L, 2)) ? TRUE : (lua_toboolean(L, 2) ? TRUE : FALSE);
    lua_pushnumber(L, pDoc->EnablePlugin(id, enable));
    return 1;
}

// ── Plugin API ────────────────────────────────────────────────────────────────

// world.LoadPlugin(path) → error_code
// Loads a plugin from the given file path.  Returns eOK on success,
// ePluginFileNotFound (or a generic non-zero code) on failure.
// NOT rewired to the real CMUSHclientDoc::LoadPlugin (methods_plugins.cpp):
// that body reports failure only via the CFileException the Windows
// InternalLoadPlugin throws, but the portable InternalLoadPlugin returns
// bool — going through the real method would turn every load failure into
// eOK.  Revisit when the real xml_load_world.cpp loader adopts (Campaign B).
static int L_LoadPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* path = luaL_checkstring(L, 1);
    bool ok = pDoc->InternalLoadPlugin(CString(path));
    lua_pushnumber(L, ok ? eOK : ePluginFileNotFound);
    return 1;
}

// world.IsPlugin(pluginID) → true/false
// Returns true if a plugin with the given ID is currently loaded.
static int L_IsPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    for (auto* p : pDoc->m_PluginList) {
        if (strcmp((const char*)p->m_strID, id) == 0) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// world.GetPluginList() → table of plugin IDs (1-based array), nil when none.
// Thin wrapper over the REAL CMUSHclientDoc::GetPluginList (methods_plugins.cpp)
// mirroring lua_methods.cpp L_GetPluginList.
static int L_GetPluginList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetPluginList();
    return push_option_variant(L, v);
}

// world.GetPluginInfo(pluginID, infoType) → value or nil
// InfoType constants (matching Windows MUSHClient):
//   1=name  2=author  3=description  4=script  5=language  6=source
//   7=id    8=purpose 9=triggerCount 10=aliasCount 11=timerCount
//   12=variableCount  16=scriptingEnabled  17=enabled
//   18=requiredVersion  19=version  20=directory
// Thin wrapper over the REAL CMUSHclientDoc::GetPluginInfo — mirrors
// lua_methods.cpp L_GetPluginInfo (pushVariant).  Date-valued infotypes
// (13/14/22) come back nil pending VT_DATE support in push_option_variant.
static int L_GetPluginInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetPluginInfo(luaL_checkstring(L, 1),
                                    (short)luaL_checknumber(L, 2));
    return push_option_variant(L, v);
}

// world.SetPluginInfo(pluginID, infoType, value) → error_code
// Currently supports infoType 17 (enabled toggle) matching EnablePlugin.
static int L_SetPluginInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id   = luaL_checkstring(L, 1);
    int         info = (int)luaL_checknumber(L, 2);

    CPlugin* found = nullptr;
    for (auto* p : pDoc->m_PluginList) {
        if (strcmp((const char*)p->m_strID, id) == 0) { found = p; break; }
    }
    if (!found) { lua_pushnumber(L, eNoSuchPlugin); return 1; }

    switch (info) {
        case 17:
            found->m_bEnabled = lua_toboolean(L, 3) ? TRUE : FALSE;
            lua_pushnumber(L, eOK);
            break;
        default:
            lua_pushnumber(L, eUnknownOption);
            break;
    }
    return 1;
}

// ── Delayed execution (DoAfterSpecial / DoAfter / DoAfterNote) ───────────────

// world.DoAfterSpecial(seconds, sendText, sendTo) → error_code
// Creates a temporary one-shot timer that fires after `seconds` seconds and
// executes `sendText` via the given send-to destination.
// Mirrors CMUSHclientDoc::DoAfterSpecial() from methods_timers.cpp.
static int L_DoAfterSpecial(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    double      seconds = luaL_checknumber(L, 1);
    const char* text    = luaL_checkstring(L, 2);
    int         sendTo  = (int)luaL_checknumber(L, 3);

    if (seconds < 0.1) { lua_pushnumber(L, eTimeInvalid); return 1; }
    if (sendTo < 0 || sendTo >= eSendToLast) {
        lua_pushnumber(L, eOptionOutOfRange); return 1;
    }

    int iHours   = (int)(seconds / 3600);  seconds -= iHours * 3600;
    int iMinutes = (int)(seconds / 60);    seconds -= iMinutes * 60;

    if (iHours > 23) { lua_pushnumber(L, eTimeInvalid); return 1; }

    // Generate a unique auto-name for the temporary timer
    CString strName;
    strName.Format("*timer%010lu", CTimer::GetNextTimerSequence());

    CTimer* t = new CTimer();
    t->strLabel        = strName;
    t->iEveryHour      = iHours;
    t->iEveryMinute    = iMinutes;
    t->fEverySecond    = seconds;
    t->iType           = CTimer::eInterval;
    t->strContents     = CString(text);
    t->bEnabled        = TRUE;
    t->bOneShot        = TRUE;
    t->bTemporary      = TRUE;
    t->bActiveWhenClosed = TRUE;
    t->iSendTo         = sendTo;
    t->strGroup        = CString("");

    pDoc->GetTimerMap().SetAt(strName, t);
    pDoc->ResetOneTimer(t);
    pDoc->SortTimers();

    lua_pushnumber(L, eOK);
    return 1;
}

// world.DoAfter(seconds, sendText) → error_code
// Shorthand for DoAfterSpecial(seconds, sendText, eSendToWorld).
static int L_DoAfter(lua_State* L)
{
    // Push eSendToWorld as the third argument, then delegate
    lua_pushnumber(L, eSendToWorld);
    lua_insert(L, 3);
    return L_DoAfterSpecial(L);
}

// world.DoAfterNote(seconds, noteText) → error_code
// Shorthand for DoAfterSpecial(seconds, noteText, eSendToOutput).
static int L_DoAfterNote(lua_State* L)
{
    lua_pushnumber(L, eSendToOutput);
    lua_insert(L, 3);
    return L_DoAfterSpecial(L);
}

// ── Inter-plugin communication ────────────────────────────────────────────────

// world.CallPlugin(pluginID, routineName [, argument]) → error_code
// Calls a named Lua function in another plugin, passing an optional string
// argument.  Sets m_CurrentPlugin so the callee's GetPluginID() etc. work.
// Thin wrapper over the REAL CMUSHclientDoc::CallPlugin (methods_plugins.cpp):
// one optional string argument, proper eNoSuchPlugin/ePluginDisabled/
// eNoSuchRoutine codes, m_strCallingPluginID maintained.  The rich multi-
// argument / multi-return marshalling of lua_methods.cpp L_CallPlugin stays a
// Campaign C item (it copies values across lua_States).
static int L_CallPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->CallPlugin(luaL_checkstring(L, 1),
                                       luaL_checkstring(L, 2),
                                       luaL_optstring(L, 3, "")));
    return 1;
}

// world.BroadcastPlugin(message, text) → count of plugins that handled it
// Fires OnPluginBroadcast(message, senderID, senderName, text) in every
// enabled plugin *except* the caller.
// Thin wrapper over the REAL CMUSHclientDoc::BroadcastPlugin — mirrors
// lua_methods.cpp L_BroadcastPlugin (returns the handled-count).
static int L_BroadcastPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->BroadcastPlugin((long)luaL_checknumber(L, 1),
                                            luaL_optstring(L, 2, "")));
    return 1;
}

// world.PluginSupports(pluginID, routineName) → true/false
// Returns true if the named plugin has a function with the given name.
// Thin wrapper over the REAL CMUSHclientDoc::PluginSupports — mirrors
// lua_methods.cpp L_PluginSupports: returns the error CODE (eOK /
// eNoSuchPlugin / eNoSuchRoutine), the Windows behaviour, not a boolean.
static int L_PluginSupports(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->PluginSupports(luaL_checkstring(L, 1),
                                           luaL_checkstring(L, 2)));
    return 1;
}

// world.GetPluginID() → string  (ID of the currently-executing plugin, or "")
static int L_GetPluginID(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_CurrentPlugin)
        lua_pushstring(L, (const char*)pDoc->m_CurrentPlugin->m_strID);
    else
        lua_pushstring(L, "");
    return 1;
}

// ── String-valued option access ───────────────────────────────────────────────
// Thin wrappers over the REAL CMUSHclientDoc::GetAlphaOption /
// SetAlphaOption (scriptingoptions.cpp), mirroring lua_methods.cpp
// L_GetAlphaOption / L_SetAlphaOption marshalling exactly: Get returns the
// VARIANT (string, or nil/VT_EMPTY for an unknown or plugin-forbidden
// option), Set returns the error code.

// world.GetAlphaOption(option_name) → string value (nil if unknown)
static int L_GetAlphaOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetAlphaOption(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

// world.SetAlphaOption(option_name, value) → error_code
static int L_SetAlphaOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->SetAlphaOption(
        luaL_checkstring(L, 1),   // OptionName
        luaL_checkstring(L, 2)    // Value
        ));
    return 1;
}

// ── ANSI escape builder ───────────────────────────────────────────────────────
// world.ANSI(code [, code2, ...]) → "\x1B[code;...m"

static int L_ANSI(lua_State* L)
{
    world_doc(L);  // validate world
    int n = lua_gettop(L);
    string s = "\x1B[";
    for (int i = 1; i <= n; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)luaL_checknumber(L, i));
        if (i > 1) s += ';';
        s += buf;
    }
    s += 'm';
    lua_pushstring(L, s.c_str());
    return 1;
}

// ── AdjustColour / ColourNameToRGB / RGBColourToName ─────────────────────────

static int L_AdjustColour(lua_State* L)
{
    world_doc(L);
    long   colour = (long)luaL_checknumber(L, 1);
    short  method = (short)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)AdjustColour(colour, method));
    return 1;
}

static int L_ColourNameToRGB(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    COLORREF c = 0;
    if (SetColour(CString(name), c))
        lua_pushnumber(L, -1);
    else
        lua_pushnumber(L, (lua_Number)(long)c);
    return 1;
}

static int L_RGBColourToName(lua_State* L)
{
    world_doc(L);
    long colour = (long)luaL_checknumber(L, 1);
    CString s = ColourToName((COLORREF)colour);
    lua_pushstring(L, (const char*)s);
    return 1;
}

// ── NoteColourName ────────────────────────────────────────────────────────────
// world.NoteColourName(foreColourName, backColourName, text)

static int L_NoteColourName(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* fg   = luaL_checkstring(L, 1);
    const char* bg   = luaL_checkstring(L, 2);
    const char* text = luaL_checkstring(L, 3);
    pDoc->ColourNote(fg, bg, text);
    return 0;
}

// ── NoteStyle / GetNoteStyle ──────────────────────────────────────────────────

static int L_NoteStyle(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    short style = (short)luaL_checknumber(L, 1);
    pDoc->m_iNoteStyle = (unsigned short)(style & 0x0F);  // TEXT_STYLE bits
    return 0;
}

static int L_GetNoteStyleLua(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)(pDoc->m_iNoteStyle & 0x0F));
    return 1;
}

// ── GetNoteColourFore/Back, SetNoteColourFore/Back ────────────────────────────

static int L_GetNoteColourFore(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)(long)pDoc->m_iNoteColourFore);
    return 1;
}

static int L_GetNoteColourBack(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)(long)pDoc->m_iNoteColourBack);
    return 1;
}

static int L_SetNoteColourFore(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // real SetNoteColourFore sets this
    pDoc->m_iNoteColourFore = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

static int L_SetNoteColourBack(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // real SetNoteColourBack sets this
    pDoc->m_iNoteColourBack = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

// ── NoteHr ────────────────────────────────────────────────────────────────────
// Outputs a 60-char horizontal rule as a note line.

static int L_NoteHr(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->Note(CString(
        "------------------------------------------------------------"));
    return 0;
}

// ── DeleteLines / DeleteOutput ────────────────────────────────────────────────

static int L_DeleteLines(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long n = (long)luaL_checknumber(L, 1);
    if (n <= 0) return 0;
    // Display step 7: the in-progress line is the list TAIL — deleting it
    // would leave m_pCurrentLine dangling (use-after-free on the next output).
    // Delete up to n COMPLETED lines from just before it.
    for (long i = 0; i < n; ++i) {
        POSITION pos = pDoc->m_LineList.GetTailPosition();
        if (!pos) break;
        CLine* pLine = pDoc->m_LineList.GetAt(pos);
        if (pLine == pDoc->m_pCurrentLine) {
            pDoc->m_LineList.GetPrev(pos);       // step past the in-progress tail
            if (!pos) break;                     // nothing completed left
            pLine = pDoc->m_LineList.GetAt(pos);
        }
        pDoc->m_LineList.RemoveAt(pos);
        delete pLine;
    }
    // Invalidate index slots beyond the shrunken count and clamp the drain
    // cursor so already-emitted accounting can't point past the list.
    long count = pDoc->m_LineList.GetCount();
    for (long s = (count / JUMP_SIZE) + 1; s <= pDoc->m_maxlines / JUMP_SIZE; ++s)
        pDoc->m_pLinePositions[s] = NULL;
    if (pDoc->m_gui_lines_emitted > count - 1)
        pDoc->m_gui_lines_emitted = (count > 0) ? count - 1 : 0;
    return 0;
}

static int L_DeleteOutput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    // free every line INCLUDING the in-progress tail...
    POSITION pos = pDoc->m_LineList.GetHeadPosition();
    while (pos) {
        CLine* pLine = pDoc->m_LineList.GetNext(pos);
        delete pLine;
    }
    pDoc->m_LineList.RemoveAll();
    // ...then re-seed a fresh in-list current line and re-prime the index,
    // mirroring the real doc.cpp ClearOutput (display step 7 lifecycle).
    pDoc->m_total_lines = 0;
    pDoc->m_pCurrentLine = new CLine(++pDoc->m_total_lines, pDoc->m_nWrapColumn,
                                     0, WHITE, BLACK, pDoc->m_bUTF_8);
    pDoc->m_LineList.AddTail(pDoc->m_pCurrentLine);
    pDoc->AllocateLinePositions();
    pDoc->m_gui_lines_emitted = 0;
    return 0;
}

// ── GetConnectDuration ────────────────────────────────────────────────────────

static int L_GetConnectDuration(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetConnectDuration());   // real method
    return 1;
}

// ── SendNoEcho ────────────────────────────────────────────────────────────────

static int L_SendNoEcho(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud) {
        lua_pushnumber(L, eWorldClosed); return 1;
    }
    const char* msg = luaL_checkstring(L, 1);
    pDoc->SendMsg(CString(msg), false, false, false);
    lua_pushnumber(L, eOK);
    return 1;
}

// ── GetTrace / SetTrace / TraceOut ────────────────────────────────────────────

static int L_GetTrace(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushboolean(L, pDoc->m_bTrace ? 1 : 0);
    return 1;
}

static int L_SetTrace(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bTrace = (lua_toboolean(L, 1) != 0);
    return 0;
}

static int L_TraceOut(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (!pDoc->m_bTrace) return 0;
    // Collect all args into one string
    int n = lua_gettop(L);
    string out;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = lua_tolstring(L, i, &len);
        if (s) out.append(s, len);
    }
    // Output as a note (yellow if RGB available, plain otherwise)
    pDoc->Note(CString(("[TRACE] " + out).c_str()));
    return 0;
}

// ── DiscardQueue ──────────────────────────────────────────────────────────────

static int L_DiscardQueue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long count = (long)pDoc->m_QueuedCommandsList.GetCount();
    pDoc->m_QueuedCommandsList.RemoveAll();
    lua_pushnumber(L, (lua_Number)count);
    return 1;
}

// ── GetNotes / SetNotes ───────────────────────────────────────────────────────

static int L_GetNotes(lua_State* L)
{
    BSTR str = world_doc(L)->GetNotes();   // real method
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

static int L_SetNotes(lua_State* L)
{
    world_doc(L)->SetNotes(luaL_checkstring(L, 1));   // real method
    return 0;
}

// ── CreateGUID / GetUniqueID ──────────────────────────────────────────────────
// CreateGUID  → "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
// GetUniqueID → SHA-1 hash of a GUID (no hyphens, 40 hex chars → first 5 words)

static int L_CreateGUID(lua_State* L)
{
    // Real CMUSHclientDoc::CreateGUID (methods_utilities.cpp → ::GetGUID,
    // now in doc_portable.cpp with the exact Windows output shape).
    BSTR str = world_doc(L)->CreateGUID();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

static int L_GetUniqueID(lua_State* L)
{
    // Real CMUSHclientDoc::GetUniqueID (methods_utilities.cpp → the
    // single-sourced free ::GetUniqueID — 24 hex chars, not the 40 the old
    // twin produced; 24 is the Windows/PLUGIN_UNIQUE_ID_LENGTH shape).
    BSTR str = world_doc(L)->GetUniqueID();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── GetRecentLines ────────────────────────────────────────────────────────────

static int L_GetRecentLines(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long count = (long)luaL_checknumber(L, 1);
    if (count <= 0) { lua_pushstring(L, ""); return 1; }
    long total = (long)pDoc->m_sRecentLines.size();
    long start = total - count;
    if (start < 0) start = 0;
    string result;
    for (long i = start; i < total; ++i) {
        if (i > start) result += '\n';
        result += pDoc->m_sRecentLines[(size_t)i];
    }
    lua_pushstring(L, result.c_str());
    return 1;
}

// ── DeleteTemporaryTriggers / Aliases / Timers / Group ────────────────────────

static int L_DeleteTemporaryTriggers(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long count = 0;
    CString name; CTrigger* t;
    for (POSITION pos = pDoc->GetTriggerMap().GetStartPosition(); pos; ) {
        pDoc->GetTriggerMap().GetNextAssoc(pos, name, t);
        if (t->bTemporary && !t->bExecutingScript) {
            delete t;
            pDoc->GetTriggerMap().RemoveKey(name);
            ++count;
        }
    }
    pDoc->SortTriggers();
    lua_pushnumber(L, (lua_Number)count);
    return 1;
}

static int L_DeleteTemporaryAliases(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long count = 0;
    CString name; CAlias* a;
    for (POSITION pos = pDoc->GetAliasMap().GetStartPosition(); pos; ) {
        pDoc->GetAliasMap().GetNextAssoc(pos, name, a);
        if (a->bTemporary && !a->bExecutingScript) {
            delete a;
            pDoc->GetAliasMap().RemoveKey(name);
            ++count;
        }
    }
    pDoc->SortAliases();
    lua_pushnumber(L, (lua_Number)count);
    return 1;
}

static int L_DeleteTemporaryTimers(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long count = 0;
    CString name; CTimer* t;
    for (POSITION pos = pDoc->GetTimerMap().GetStartPosition(); pos; ) {
        pDoc->GetTimerMap().GetNextAssoc(pos, name, t);
        if (t->bTemporary && !t->bExecutingScript) {
            delete t;
            pDoc->GetTimerMap().RemoveKey(name);
            ++count;
        }
    }
    if (count) pDoc->SortTimers();
    lua_pushnumber(L, (lua_Number)count);
    return 1;
}

static int L_DeleteGroup(lua_State* L)
{
    // Real CMUSHclientDoc::DeleteGroup (methods_utilities.cpp →
    // DeleteTriggerGroup + DeleteAliasGroup + DeleteTimerGroup).
    lua_pushnumber(L, world_doc(L)->DeleteGroup(luaL_checkstring(L, 1)));
    return 1;
}

// ── Get/Set{Trigger,Alias,Timer}Option ────────────────────────────────────────
// Thin wrappers over the REAL CMUSHclientDoc methods (methods_triggers.cpp /
// methods_aliases.cpp / methods_timers.cpp, adopted into mushcore) using the
// same marshalling as Windows lua_methods.cpp (pushVariant / get_option_value).
// The former hand-mirrored twins drifted from the Windows option tables (e.g.
// boolean options came back as Lua booleans where Windows returns numbers).

// push a VARIANT option result exactly as lua_methods.cpp pushVariant does
static int push_option_variant(lua_State* L, VARIANT& v)
{
    switch (v.vt) {
    case VT_BSTR: lua_pushstring(L, v.bstrVal ? v.bstrVal : ""); VariantClear(&v); break;
    case VT_I2:   lua_pushnumber(L, v.iVal);   break;
    case VT_I4:   lua_pushnumber(L, v.lVal);   break;
    case VT_R4:   lua_pushnumber(L, v.fltVal); break;
    case VT_R8:   lua_pushnumber(L, v.dblVal); break;
    case VT_BOOL: lua_pushboolean(L, v.boolVal ? 1 : 0); break;

    case VT_DATE:
        // lua_methods.cpp converts VT_DATE via COleDateTime → SYSTEMTIME →
        // CTime::GetTime → lua number (Unix seconds).  On the port every
        // SetUpVariantDate site stores COleDateTime(CTime::GetTime()) and the
        // port COleDateTime's operator double() puts those RAW UNIX SECONDS
        // straight into v.date — so pushing v.date IS the Windows-visible
        // value (GetInfo 301-306, GetLineInfo 9, GetPluginInfo 13/14/22).
        lua_pushnumber(L, v.date);
        break;

    case (VT_ARRAY | VT_VARIANT):   // array of variants (Get*List/ArrayList*/…)
    {
        // A Windows COleSafeArray that was never CreateOneDim'd detaches as
        // VT_EMPTY (→ nil); the port COleSafeArray::Detach leaves a null
        // parray instead — map it to the same Windows result.
        if (!v.parray) { VariantClear(&v); lua_pushnil(L); break; }

        lua_newtable(L);

        SAFEARRAY* psa = v.parray;
        long lLBound = 0, lUBound = -1;

        SafeArrayGetLBound(psa, 1, &lLBound);
        SafeArrayGetUBound(psa, 1, &lUBound);

        VARIANT val;
        int extra = 0;
        if (lLBound == 0)
            extra = 1;
        for (long i = lLBound; i <= lUBound; i++) {
            long index[2];
            index[0] = i;
            index[1] = 0;
            SafeArrayGetElement(psa, index, &val);

            push_option_variant(L, val);          // recurse to push the element value
            lua_rawseti(L, -2, (int)(i + extra)); // make 1-relative
        }   // end of doing each row
        VariantClear(&v);   // frees the SafeArray + the element strings it owns
        break;
    }   // end of VT_ARRAY

    default:      lua_pushnil(L);              break;
    }
    return 1;
}

// Lua arg → option-value string, as lua_methods.cpp get_option_value:
// booleans become "1"/"0", nil becomes "0", numbers/strings via tostring.
static const char* option_value_string(lua_State* L, int narg)
{
    if (lua_isboolean(L, narg)) return lua_toboolean(L, narg) ? "1" : "0";
    if (lua_isnil(L, narg))     return "0";
    return luaL_checkstring(L, narg);
}

static int L_GetTriggerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetTriggerOption(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return push_option_variant(L, v);
}

static int L_SetTriggerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->SetTriggerOption(luaL_checkstring(L, 1),
                                             luaL_checkstring(L, 2),
                                             option_value_string(L, 3)));
    return 1;
}

static int L_GetAliasOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetAliasOption(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return push_option_variant(L, v);
}

static int L_SetAliasOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->SetAliasOption(luaL_checkstring(L, 1),
                                           luaL_checkstring(L, 2),
                                           option_value_string(L, 3)));
    return 1;
}

static int L_GetTimerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetTimerOption(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return push_option_variant(L, v);
}

static int L_SetTimerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->SetTimerOption(luaL_checkstring(L, 1),
                                           luaL_checkstring(L, 2),
                                           option_value_string(L, 3)));
    return 1;
}

// ── GetTriggerWildcard / GetAliasWildcard ─────────────────────────────────────

static int L_GetTriggerWildcard(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* trigName = luaL_checkstring(L, 1);
    const char* wildName = luaL_checkstring(L, 2);
    CTrigger* t = nullptr;
    CString key(trigName);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetTriggerMap().Lookup(key, t) || !t || !t->regexp || wildName[0] == 0) {
        lua_pushnil(L); return 1;
    }
    string val = t->regexp->GetWildcard(string(wildName));
    lua_pushstring(L, val.c_str());
    return 1;
}

static int L_GetAliasWildcard(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* aliasName = luaL_checkstring(L, 1);
    const char* wildName  = luaL_checkstring(L, 2);
    CAlias* a = nullptr;
    CString key(aliasName);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetAliasMap().Lookup(key, a) || !a || !a->regexp || wildName[0] == 0) {
        lua_pushnil(L); return 1;
    }
    string val = a->regexp->GetWildcard(string(wildName));
    lua_pushstring(L, val.c_str());
    return 1;
}

// ── AtTime ────────────────────────────────────────────────────────────────────
// world.AtTime(hour, minute, second, what [, label [, plugin_id]])

static int L_AtTime(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int hour   = (int)luaL_checknumber(L, 1);
    int minute = (int)luaL_checknumber(L, 2);
    double sec = luaL_checknumber(L, 3);
    const char* what  = luaL_checkstring(L, 4);
    const char* label = luaL_optstring(L, 5, "");

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || sec < 0 || sec > 59.999)
    { lua_pushnumber(L, eOptionOutOfRange); return 1; }

    CTimer* pTimer = new CTimer();
    pTimer->iType       = CTimer::eAtTime;
    pTimer->iAtHour     = hour;
    pTimer->iAtMinute   = minute;
    pTimer->fAtSecond   = (float)sec;
    pTimer->strContents = CString(what);
    pTimer->bEnabled    = true;
    pTimer->bTemporary  = true;
    pTimer->bOneShot    = false;
    pTimer->iSendTo     = eSendToWorld;
    pTimer->nInvocationCount = 0;
    pTimer->nMatched    = 0;
    pTimer->dispid      = DISPID_UNKNOWN;

    // Compute first fire time (next occurrence of hh:mm:ss)
    CmcDateTime tNow = CmcDateTime::GetTimeNow();
    CmcDateTime tFire = CmcDateTime(
        tNow.GetYear(), tNow.GetMonth(), tNow.GetDay(),
        hour, minute, (int)sec);
    if (tFire <= tNow)
        tFire += CmcDateTimeSpan(1, 0, 0, 0); // tomorrow
    pTimer->tFireTime = tFire;

    // Generate unique label if none given
    CString strLabel(label);
    if (strLabel.IsEmpty())
        strLabel = CTimer::GetNextTimerSequence();

    pDoc->GetTimerMap().SetAt(strLabel, pTimer);
    pDoc->SortTimers();
    lua_pushnumber(L, eOK);
    lua_pushstring(L, (const char*)strLabel);
    return 2;
}

// ── Hyperlink ─────────────────────────────────────────────────────────────────
// Injects a styled clickable run into the note output stream.
// Signature: Hyperlink(action, text, hint, fore_colour, back_colour, url, no_underline)
// fore/back are either "#RRGGBB" strings or colour names; empty string = default.

static int L_Hyperlink(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);

    const char* action      = luaL_optstring(L, 1, "");
    const char* text        = luaL_optstring(L, 2, "");
    const char* hint        = luaL_optstring(L, 3, "");
    const char* fore_str    = luaL_optstring(L, 4, "");
    const char* back_str    = luaL_optstring(L, 5, "");
    // arg 6 = url (bool) — determines ACTION_HYPERLINK vs ACTION_SEND flag
    // arg 7 = no_underline (bool)
    bool url          = lua_toboolean(L, 6) != 0;
    bool no_underline = lua_toboolean(L, 7) != 0;

    if (!action || action[0] == 0) return 0;
    if (!text   || text[0]   == 0) text = action;
    if (!hint   || hint[0]   == 0) hint = action;

    // Resolve fore/back colours.
    COLORREF fg = pDoc->m_iHyperlinkColour;
    COLORREF bg = 0x00000000u;
    if (fore_str && fore_str[0]) SetColour(CString(fore_str), fg);
    if (back_str && back_str[0]) SetColour(CString(back_str), bg);

    // Push a note-line style carrying the action (the drain reads pStyle->pAction).
    unsigned short flags = COLOUR_RGB |
                           (url ? ACTION_HYPERLINK : ACTION_SEND) |
                           (no_underline ? 0 : UNDERLINE);
    CAction* pAction = pDoc->GetAction(CString(action),
                                       CString(hint),
                                       CString(""));
    pDoc->AddStyle(flags, fg, bg, 0, pAction);
    if (pAction) pAction->Release();

    // Output the link text so it gets picked up by DisplayMsg.
    pDoc->AddToLine(text, COMMENT);

    // Restore the normal note style.
    pDoc->AddStyle(COLOUR_RGB, pDoc->m_iNoteColourFore, pDoc->m_iNoteColourBack, 0,
                   static_cast<CAction*>(nullptr));
    return 0;
}

// ── OpenBrowser ───────────────────────────────────────────────────────────────

static int L_OpenBrowser(lua_State* L)
{
    // Real CMUSHclientDoc::OpenBrowser (methods_utilities.cpp): the same
    // http/https/mailto whitelist, then mfc_ui.h ShellExecute (`open` on
    // macOS / xdg-open on Linux) — the identical path the mac GUI uses.
    lua_pushnumber(L, world_doc(L)->OpenBrowser(luaL_checkstring(L, 1)));
    return 1;
}

// ── ResetTimers ───────────────────────────────────────────────────────────────

static int L_ResetTimers(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->ResetAllTimers(pDoc->GetTimerMap());
    return 0;
}

// ── GetReceivedBytes / GetSentBytes ───────────────────────────────────────────

static int L_GetReceivedBytes(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)pDoc->m_nBytesIn);
    return 1;
}

static int L_GetSentBytes(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)pDoc->m_nBytesOut);
    return 1;
}

// ── GetScriptTime ─────────────────────────────────────────────────────────────
// Returns cumulative seconds spent executing Lua scripts in this world.

static int L_GetScriptTime(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetScriptTime());   // real method
    return 1;
}

// ── MakeRegularExpression ─────────────────────────────────────────────────────

static int L_MakeRegularExpression(lua_State* L)
{
    // Same shape as lua_methods.cpp L_MakeRegularExpression: it too calls
    // the free ConvertToRegularExpression directly ("avoids using BSTR").
    world_doc(L);
    lua_pushstring(L, ConvertToRegularExpression(luaL_checkstring(L, 1)));
    return 1;
}

// ── FixupEscapeSequences ──────────────────────────────────────────────────────

static int L_FixupEscapeSequences(lua_State* L)
{
    BSTR str = world_doc(L)->FixupEscapeSequences(luaL_checkstring(L, 1));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── FixupHTML ─────────────────────────────────────────────────────────────────

static int L_FixupHTML(lua_State* L)
{
    BSTR str = world_doc(L)->FixupHTML(luaL_checkstring(L, 1));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── SetTitle / SetMainTitle ───────────────────────────────────────────────────

static int L_SetTitle(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_optstring(L, 1, "");
    pDoc->m_strWindowTitle = title;
    pDoc->EmitTitle(title);
    return 0;
}

static int L_SetMainTitle(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_optstring(L, 1, "");
    pDoc->m_strMainWindowTitle = title;
    pDoc->EmitMainTitle(std::string(title));
    return 0;
}

// ── GetHostName / GetHostAddress ──────────────────────────────────────────────

static int L_GetHostName(lua_State* L)
{
    BSTR str = world_doc(L)->GetHostName(luaL_checkstring(L, 1));   // real method
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

static int L_GetHostAddress(lua_State* L)
{
    // Real CMUSHclientDoc::GetHostAddress + pushVariant shape: a 1-based
    // array of dotted addresses, or nil when the lookup finds none (the old
    // twin returned an EMPTY TABLE there — Windows returns nil).
    VARIANT v = world_doc(L)->GetHostAddress(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

// ── Log functions ─────────────────────────────────────────────────────────────

static int L_IsLogOpen(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushboolean(L, pDoc->m_logfile != nullptr ? 1 : 0);
    return 1;
}

static int L_OpenLog(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* filename = luaL_optstring(L, 1, "");
    int append = lua_toboolean(L, 2);

    if (pDoc->m_logfile)
    {
        lua_pushnumber(L, eLogFileAlreadyOpen);
        return 1;
    }

    pDoc->m_logfile_name = filename;

    if (pDoc->m_logfile_name.IsEmpty())
    {
        lua_pushnumber(L, eCouldNotOpenFile);
        return 1;
    }

    pDoc->m_logfile = fopen(static_cast<const char*>(pDoc->m_logfile_name),
                            append ? "a+" : "w");
    if (pDoc->m_logfile)
    {
        // close and reopen to flush to disk directory (matches Windows behaviour)
        fclose(pDoc->m_logfile);
        pDoc->m_logfile = fopen(static_cast<const char*>(pDoc->m_logfile_name),
                                append ? "a+" : "w");
    }

    lua_pushnumber(L, pDoc->m_logfile ? eOK : eCouldNotOpenFile);
    return 1;
}

static int L_CloseLog(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_logfile)
    {
        fclose(pDoc->m_logfile);
        pDoc->m_logfile = nullptr;
        lua_pushnumber(L, eOK);
    }
    else
    {
        lua_pushnumber(L, eLogFileNotOpen);
    }
    return 1;
}

static int L_FlushLog(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_logfile)
    {
        fflush(pDoc->m_logfile);
        lua_pushnumber(L, eOK);
    }
    else
    {
        lua_pushnumber(L, eLogFileNotOpen);
    }
    return 1;
}

static int L_WriteLog(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_checkstring(L, 1);
    if (!pDoc->m_logfile)
    {
        lua_pushnumber(L, eLogFileNotOpen);
        return 1;
    }
    string s(msg);
    if (s.size() < 2 || s.substr(s.size()-2) != "\r\n")
        s += "\n";
    size_t written = fwrite(s.c_str(), 1, s.size(), pDoc->m_logfile);
    lua_pushnumber(L, written == s.size() ? eOK : eLogFileBadWrite);
    return 1;
}

static int L_LogSend(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_checkstring(L, 1);
    // Send the command
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud)
    {
        lua_pushnumber(L, eWorldClosed);
        return 1;
    }
    pDoc->SendMsg(CString(msg), pDoc->m_display_my_input != 0, false, false);
    // Log to the command log file if open
    if (pDoc->m_logfile)
        pDoc->LogCommand(msg);
    lua_pushnumber(L, eOK);
    return 1;
}

// ── GetLogInput / SetLogInput / GetLogNotes / SetLogNotes /
//    GetLogOutput / SetLogOutput ─────────────────────────────────────────────

static int L_GetLogInput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->m_log_input);
    return 1;
}

static int L_SetLogInput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_log_input = (short)luaL_checkinteger(L, 1);
    return 0;
}

static int L_GetLogNotes(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->m_bLogNotes);
    return 1;
}

static int L_SetLogNotes(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bLogNotes = (short)luaL_checkinteger(L, 1);
    return 0;
}

static int L_GetLogOutput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->m_bLogOutput);
    return 1;
}

static int L_SetLogOutput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bLogOutput = (short)luaL_checkinteger(L, 1);
    return 0;
}

// ── Colour palette functions ──────────────────────────────────────────────────

static int L_GetNormalColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    if (which < 1 || which > 8) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, (lua_Number)pDoc->m_normalcolour[which - 1]);
    return 1;
}

static int L_SetNormalColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    long colour = (long)luaL_checkinteger(L, 2);
    if (which >= 1 && which <= 8)
        pDoc->m_normalcolour[which - 1] = (COLORREF)(colour & 0x00FFFFFF);
    return 0;
}

static int L_GetBoldColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    if (which < 1 || which > 8) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, (lua_Number)pDoc->m_boldcolour[which - 1]);
    return 1;
}

static int L_SetBoldColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    long colour = (long)luaL_checkinteger(L, 2);
    if (which >= 1 && which <= 8)
        pDoc->m_boldcolour[which - 1] = (COLORREF)(colour & 0x00FFFFFF);
    return 0;
}

static int L_GetCustomColourText(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    if (which < 1 || which > MAX_CUSTOM) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, (lua_Number)pDoc->m_customtext[which - 1]);
    return 1;
}

static int L_SetCustomColourText(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    long colour = (long)luaL_checkinteger(L, 2);
    if (which >= 1 && which <= MAX_CUSTOM)
        pDoc->m_customtext[which - 1] = (COLORREF)(colour & 0x00FFFFFF);
    return 0;
}

static int L_GetCustomColourBackground(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    if (which < 1 || which > MAX_CUSTOM) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, (lua_Number)pDoc->m_customback[which - 1]);
    return 1;
}

static int L_SetCustomColourBackground(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    long colour = (long)luaL_checkinteger(L, 2);
    if (which >= 1 && which <= MAX_CUSTOM)
        pDoc->m_customback[which - 1] = (COLORREF)(colour & 0x00FFFFFF);
    return 0;
}

static int L_GetCustomColourName(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    if (which < 1 || which > MAX_CUSTOM) { lua_pushstring(L, ""); return 1; }
    lua_pushstring(L, static_cast<const char*>(pDoc->m_strCustomColourName[which - 1]));
    return 1;
}

static int L_SetCustomColourName(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_checkinteger(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (which < 1 || which > MAX_CUSTOM) { lua_pushnumber(L, eOptionOutOfRange); return 1; }
    if (!name || !*name)           { lua_pushnumber(L, eNoNameSpecified);   return 1; }
    if (strlen(name) > 30)        { lua_pushnumber(L, eInvalidObjectLabel); return 1; }
    pDoc->m_strCustomColourName[which - 1] = name;
    lua_pushnumber(L, eOK);
    return 1;
}

// ── GetQueue ──────────────────────────────────────────────────────────────────
// The REAL GetQueue (methods_commands.cpp): a 1-based array of queued
// commands (echo flag stripped), or nil when the queue is empty — exactly
// the Windows shape via push_option_variant's VT_ARRAY branch.

static int L_GetQueue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetQueue();
    return push_option_variant(L, v);
}

// ── SendImmediate ─────────────────────────────────────────────────────────────

static int L_SendImmediate(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_checkstring(L, 1);
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud)
    {
        lua_pushnumber(L, eWorldClosed);
        return 1;
    }
    // Send bypassing the queue, with current echo & log settings
    pDoc->SendMsg(CString(msg), pDoc->m_display_my_input != 0, false,
                  pDoc->m_log_input != 0);
    lua_pushnumber(L, eOK);
    return 1;
}

// ── DeleteAliasGroup / DeleteTimerGroup / DeleteTriggerGroup ──────────────────

static int L_DeleteTriggerGroup(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* grp = luaL_checkstring(L, 1);
    CTriggerMap& m = pDoc->GetTriggerMap();
    vector<string> keys;
    { POSITION pos = m.GetStartPosition(); while (pos) {
        CString k; CTrigger* v;
        m.GetNextAssoc(pos, k, v);
        if (v && v->strGroup == grp)
            keys.push_back(static_cast<const char*>(k));
    } }
    for (auto& k : keys)
    {
        CTrigger* v = nullptr;
        CString ck(k.c_str());
        m.Lookup(ck, v);
        m.RemoveKey(ck);
        delete v;
    }
    lua_pushnumber(L, (lua_Number)keys.size());
    return 1;
}

static int L_DeleteAliasGroup(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* grp = luaL_checkstring(L, 1);
    CAliasMap& m = pDoc->GetAliasMap();
    vector<string> keys;
    { POSITION pos = m.GetStartPosition(); while (pos) {
        CString k; CAlias* v;
        m.GetNextAssoc(pos, k, v);
        if (v && v->strGroup == grp)
            keys.push_back(static_cast<const char*>(k));
    } }
    for (auto& k : keys)
    {
        CAlias* v = nullptr;
        CString ck(k.c_str());
        m.Lookup(ck, v);
        m.RemoveKey(ck);
        delete v;
    }
    lua_pushnumber(L, (lua_Number)keys.size());
    return 1;
}

static int L_DeleteTimerGroup(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* grp = luaL_checkstring(L, 1);
    CTimerMap& m = pDoc->GetTimerMap();
    vector<string> keys;
    { POSITION pos = m.GetStartPosition(); while (pos) {
        CString k; CTimer* v;
        m.GetNextAssoc(pos, k, v);
        if (v && v->strGroup == grp)
            keys.push_back(static_cast<const char*>(k));
    } }
    for (auto& k : keys)
    {
        CTimer* v = nullptr;
        CString ck(k.c_str());
        m.Lookup(ck, v);
        m.RemoveKey(ck);
        delete v;
    }
    lua_pushnumber(L, (lua_Number)keys.size());
    return 1;
}

// ── Plugin variable / trigger / alias / timer accessors ───────────────────────

// Helper: find plugin by ID string
static CPlugin* FindPlugin(CMUSHclientDoc* pDoc, const char* id)
{
    for (CPlugin* p : pDoc->m_PluginList)
        if (p && static_cast<const char*>(p->m_strID) == string(id))
            return p;
    return nullptr;
}

static int L_GetPluginVariable(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id  = luaL_checkstring(L, 1);
    const char* var = luaL_checkstring(L, 2);
    CPlugin* p = FindPlugin(pDoc, id);
    if (!p) { lua_pushnil(L); return 1; }
    CVariable* pVar = nullptr;
    if (!p->m_VariableMap.Lookup(CString(var), pVar) || !pVar)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, static_cast<const char*>(pVar->strContents));
    return 1;
}

// Mirrors lua_methods.cpp L_GetPluginVariableList: a Lua extension that swaps
// m_CurrentPlugin and reuses GetVariableListHelper, so the result is the same
// { name → value } table GetVariableList returns (empty ID = the world itself;
// unknown non-empty ID = no results).  The raw doc method is names-only and
// is NOT what the Windows Lua binding uses.
static int L_GetPluginVariableList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);

    // plugin name
    const char* PluginID = luaL_checkstring(L, 1);

    // lookup plugin
    CPlugin* pPlugin = NULL;
    if (strlen(PluginID) > 0) {
        pPlugin = pDoc->GetPlugin(PluginID);
        if (!pPlugin)
            return 0;   // no results - non-empty plugin not found
    }
    // save current plugin
    CPlugin* pOldPlugin = pDoc->m_CurrentPlugin;
    pDoc->m_CurrentPlugin = pPlugin;

    // now get the variable list
    GetVariableListHelper(L, pDoc);

    // restore current plugin
    pDoc->m_CurrentPlugin = pOldPlugin;

    return 1;     // one result (one table)
}

// The remaining GetPluginXxxList are thin wrappers over the REAL doc methods
// (methods_plugins.cpp GET_PLUGIN_STUFF), mirroring lua_methods.cpp: a 1-based
// array of labels; nil when the plugin is unknown or its list is empty; empty
// ID = the world's own list.
static int L_GetPluginTriggerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetPluginTriggerList(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

static int L_GetPluginAliasList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetPluginAliasList(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

static int L_GetPluginTimerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetPluginTimerList(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

// ── Plugin GetXxxInfo / GetXxxOption ─────────────────────────────────────────
// These delegate to the same per-item logic as the world versions, but look up
// items from a named plugin's maps rather than the world's maps.

static int L_GetPluginTriggerInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    int infoType         = (int)luaL_checkinteger(L, 3);
    CPlugin* p = FindPlugin(pDoc, pluginID);
    if (!p) { lua_pushnil(L); return 1; }
    CTrigger* t = nullptr;
    if (!p->m_TriggerMap.Lookup(CString(itemName), t) || !t)
        { lua_pushnil(L); return 1; }
    push_trigger_info(L, t, infoType);
    return 1;
}

static int L_GetPluginAliasInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    int infoType         = (int)luaL_checkinteger(L, 3);
    CPlugin* p = FindPlugin(pDoc, pluginID);
    if (!p) { lua_pushnil(L); return 1; }
    CAlias* a = nullptr;
    if (!p->m_AliasMap.Lookup(CString(itemName), a) || !a)
        { lua_pushnil(L); return 1; }
    push_alias_info(L, a, infoType);
    return 1;
}

static int L_GetPluginTimerInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    int infoType         = (int)luaL_checkinteger(L, 3);
    CPlugin* p = FindPlugin(pDoc, pluginID);
    if (!p) { lua_pushnil(L); return 1; }
    CTimer* t = nullptr;
    if (!p->m_TimerMap.Lookup(CString(itemName), t) || !t)
        { lua_pushnil(L); return 1; }
    push_timer_info(L, t, infoType);
    return 1;
}

// The GetPlugin*Option trio mirrors the Windows GET_PLUGIN_STUFF idiom
// (methods_plugins.cpp): swap m_CurrentPlugin to the target plugin (empty ID
// = the world itself), call the REAL Get*Option, restore.  Replaces the old
// hand-mirrored map lookups, which also missed the empty-ID = global case and
// the lower-cased name lookup.
template <typename GetOptionFn>
static int push_plugin_option(lua_State* L, GetOptionFn get_option)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    const char* optName  = luaL_checkstring(L, 3);
    CPlugin* pPlugin = NULL;
    if (pluginID[0]) {
        pPlugin = FindPlugin(pDoc, pluginID);
        if (!pPlugin) { lua_pushnil(L); return 1; }
    }
    CPlugin* pOldPlugin = pDoc->m_CurrentPlugin;
    pDoc->m_CurrentPlugin = pPlugin;
    VARIANT v = get_option(pDoc, itemName, optName);
    pDoc->m_CurrentPlugin = pOldPlugin;
    return push_option_variant(L, v);
}

static int L_GetPluginTriggerOption(lua_State* L)
{
    return push_plugin_option(L, [](CMUSHclientDoc* d, const char* n, const char* o)
                                 { return d->GetTriggerOption(n, o); });
}

static int L_GetPluginAliasOption(lua_State* L)
{
    return push_plugin_option(L, [](CMUSHclientDoc* d, const char* n, const char* o)
                                 { return d->GetAliasOption(n, o); });
}

static int L_GetPluginTimerOption(lua_State* L)
{
    return push_plugin_option(L, [](CMUSHclientDoc* d, const char* n, const char* o)
                                 { return d->GetTimerOption(n, o); });
}

// ── UnloadPlugin / ReloadPlugin ───────────────────────────────────────────────

// Thin wrapper over the REAL CMUSHclientDoc::UnloadPlugin — mirrors
// lua_methods.cpp L_UnloadPlugin (falls back to lookup by name, fires the
// real OnPluginClose/SaveState via the CPlugin destructor).
static int L_UnloadPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->UnloadPlugin(luaL_checkstring(L, 1)));
    return 1;
}

// Thin wrapper over the REAL CMUSHclientDoc::ReloadPlugin — mirrors
// lua_methods.cpp L_ReloadPlugin.
static int L_ReloadPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->ReloadPlugin(luaL_checkstring(L, 1)));
    return 1;
}

// ── Queue ─────────────────────────────────────────────────────────────────────
// Adds a command to the send queue (respects speed-walk throttle).

static int L_Queue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg  = luaL_checkstring(L, 1);
    int         echo = lua_toboolean(L, 2);  // optional; default false
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud)
        { lua_pushnumber(L, eWorldClosed); return 1; }
    pDoc->SendMsg(CString(msg), echo != 0, true, false);  // queue=true
    lua_pushnumber(L, eOK);
    return 1;
}

// ── SendPush ──────────────────────────────────────────────────────────────────
// Pushes a command to the front of the queue.

static int L_SendPush(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_checkstring(L, 1);
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud)
        { lua_pushnumber(L, eWorldClosed); return 1; }
    // Add echo-flag byte ('\x01' = echo) at front of each line, then prepend to list
    CString entry = CString("\x01") + CString(msg);
    pDoc->m_QueuedCommandsList.AddHead(entry);
    lua_pushnumber(L, eOK);
    return 1;
}

// ── Trace ─────────────────────────────────────────────────────────────────────
// Outputs a trace line if tracing is enabled (same as TraceOut).

static int L_Trace(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_bTrace)
    {
        const char* msg = luaL_optstring(L, 1, "");
        pDoc->Note(string("[TRACE] ") + msg);
    }
    return 0;
}

// ── SetUnseenLines ────────────────────────────────────────────────────────────

static int L_SetUnseenLines(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_new_lines = (long)luaL_checkinteger(L, 1);
    return 0;
}

// ── Pause / Unpause ───────────────────────────────────────────────────────────
// Pause(bPause): scroll-lock (true) or release (false) the output pane.
// On Windows this freeze/unfreezes the CMUSHView.  On non-Windows we
// toggle auto-scroll via IWorldView::setPaused (the world child frame)
// and record the state in m_gui_paused.

static int L_Pause(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    bool bPause = lua_toboolean(L, 1) != 0;
    pDoc->EmitPaused(bPause);
    // m_gui_paused is kept up to date by m_fn_autoscroll_changed;
    // set it here too in case the pane callback fires asynchronously.
    pDoc->m_gui_paused = bPause;
    return 0;
}

// ── ReverseSpeedwalk ──────────────────────────────────────────────────────────

static int L_ReverseSpeedwalk(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* sw = luaL_checkstring(L, 1);
    // DoReverseSpeedwalk is a static member of CMUSHclientDoc
    CString result = CMUSHclientDoc::DoReverseSpeedwalk(CString(sw));
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

// ── GetSpeedWalkDelay / SetSpeedWalkDelay ─────────────────────────────────────

static int L_GetSpeedWalkDelay(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->m_iSpeedWalkDelay);
    return 1;
}

static int L_SetSpeedWalkDelay(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_iSpeedWalkDelay = (short)luaL_checkinteger(L, 1);
    return 0;
}

// ── MtSrand / MtRand ─────────────────────────────────────────────────────────

static int L_MtSrand(lua_State* L)
{
    // Real CMUSHclientDoc::MtSrand for the scalar case.  (lua_methods.cpp
    // additionally accepts a TABLE of seeds via init_by_array — a Lua-only
    // extension not carried here yet; scalar seeding matches Windows.)
    world_doc(L)->MtSrand((long)luaL_checkinteger(L, 1));
    return 0;
}

static int L_MtRand(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->MtRand());   // real method
    return 1;
}

// ── Base64Encode / Base64Decode ───────────────────────────────────────────────

static int L_Base64Encode(lua_State* L)
{
    VARIANT v = world_doc(L)->Base64Encode(          // real method
        luaL_checkstring(L, 1),                      // Text
        lua_toboolean(L, 2) ? TRUE : FALSE);         // MultiLine (default false)
    return push_option_variant(L, v);
}

static int L_Base64Decode(lua_State* L)
{
    VARIANT v = world_doc(L)->Base64Decode(luaL_checkstring(L, 1));  // real
    return push_option_variant(L, v);
}

// ── ChangeDir ─────────────────────────────────────────────────────────────────

static int L_ChangeDir(lua_State* L)
{
    // Real CMUSHclientDoc::ChangeDir (methods_utilities.cpp): chdir + track
    // working_dir; returns 1 (ok) / 0 (failed) — the old twin returned
    // eOK/eBadParameter, which is not the Windows shape.
    lua_pushnumber(L, world_doc(L)->ChangeDir(luaL_checkstring(L, 1)));
    return 1;
}

// ── GetEchoInput / SetEchoInput ───────────────────────────────────────────────

static int L_GetEchoInput(lua_State* L)
{
    lua_pushboolean(L, world_doc(L)->GetEchoInput());
    return 1;
}

static int L_SetEchoInput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    // Windows optboolean(L, 1, 1): missing/nil defaults to yes
    pDoc->SetEchoInput(lua_isnoneornil(L, 1) ? 1 : lua_toboolean(L, 1));
    return 0;
}

// ── SetNoteColour ─────────────────────────────────────────────────────────────
// Sets both foreground and background note colours at once (RGB values).

static int L_SetNoteColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_bNotesInRGB = true;   // real path is RGB (NoteColourRGB-alike)
    COLORREF fg = (COLORREF)luaL_checkinteger(L, 1);
    COLORREF bg = (COLORREF)luaL_checkinteger(L, 2);
    pDoc->m_iNoteColourFore = fg & 0x00FFFFFF;
    pDoc->m_iNoteColourBack = bg & 0x00FFFFFF;
    lua_pushnumber(L, eOK);
    return 1;
}

// ── Array subsystem ───────────────────────────────────────────────────────────
// MUSHclient scripting arrays: named string→string maps with numeric-aware sort.

static int L_ArrayCreate(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tStringMapOfMaps& am = pDoc->GetArrayMap();
    if (am.find(name) != am.end())
    {
        lua_pushnumber(L, eArrayAlreadyExists);
        return 1;
    }
    am[name] = new tStringToStringMap();
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_ArrayDelete(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tStringMapOfMaps& am = pDoc->GetArrayMap();
    auto it = am.find(name);
    if (it == am.end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }
    delete it->second;
    am.erase(it);
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_ArrayExists(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, pDoc->GetArrayMap().find(name) != pDoc->GetArrayMap().end() ? 1 : 0);
    return 1;
}

static int L_ArrayCount(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)pDoc->GetArrayMap().size());
    return 1;
}

static int L_ArraySize(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)it->second->size());
    return 1;
}

static int L_ArrayGet(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    const char* key  = luaL_checkstring(L, 2);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end()) { lua_pushnil(L); return 1; }
    auto ki = it->second->find(key);
    if (ki == it->second->end())         { lua_pushnil(L); return 1; }
    lua_pushstring(L, ki->second.c_str());
    return 1;
}

static int L_ArraySet(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name  = luaL_checkstring(L, 1);
    const char* key   = luaL_checkstring(L, 2);
    const char* value = luaL_checkstring(L, 3);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }
    auto result = it->second->insert(make_pair(string(key), string(value)));
    if (!result.second)
    {
        result.first->second = value;
        lua_pushnumber(L, eSetReplacingExistingValue);
    }
    else
    {
        lua_pushnumber(L, eOK);
    }
    return 1;
}

static int L_ArrayDeleteKey(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    const char* key  = luaL_checkstring(L, 2);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }
    auto ki = it->second->find(key);
    if (ki == it->second->end())
    {
        lua_pushnumber(L, eKeyDoesNotExist);
        return 1;
    }
    it->second->erase(ki);
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_ArrayKeyExists(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    const char* key  = luaL_checkstring(L, 2);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, it->second->find(key) != it->second->end() ? 1 : 0);
    return 1;
}

static int L_ArrayClear(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }
    it->second->clear();
    lua_pushnumber(L, eOK);
    return 1;
}

// ArrayListAll/Keys/Values: thin wrappers over the REAL doc methods
// (methods_arrays.cpp), mirroring lua_methods.cpp — a 1-based array, or nil
// when there is nothing to list (no arrays / unknown array / empty array).
static int L_ArrayListAll(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->ArrayListAll();
    return push_option_variant(L, v);
}

static int L_ArrayListKeys(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->ArrayListKeys(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

static int L_ArrayListValues(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->ArrayListValues(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

static int L_ArrayGetFirstKey(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end() || it->second->empty())
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, it->second->begin()->first.c_str());
    return 1;
}

static int L_ArrayGetLastKey(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end() || it->second->empty())
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, it->second->rbegin()->first.c_str());
    return 1;
}

// ArrayImport: populate array from a delimited key,value,key,value... string.
// Escaped delimiters (backslash+delimiter) are preserved through import.
static int L_ArrayImport(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name  = luaL_checkstring(L, 1);
    const char* vals  = luaL_checkstring(L, 2);
    const char* delim = luaL_optstring  (L, 3, ",");

    string sDelimiter(delim);
    if (sDelimiter.size() != 1 || sDelimiter == "\\")
    {
        lua_pushnumber(L, eBadDelimiter);
        return 1;
    }

    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }

    string sValues(vals);
    string sTemp;
    string sEscapedDelimiter = "\\" + sDelimiter;

    if (sValues.find(sEscapedDelimiter) != string::npos)
    {
        // find unused placeholder char
        int iTemp;
        for (iTemp = 1; iTemp <= 255; ++iTemp)
        {
            sTemp = string(1, (char)iTemp);
            if (sValues.find(sTemp) == string::npos) break;
        }
        if (iTemp > 255) { lua_pushnumber(L, eCannotImport); return 1; }
        sValues = FindAndReplace(sValues, sEscapedDelimiter, sTemp);
    }

    // Split by delimiter
    vector<string> parts;
    StringToVector(sValues, parts, sDelimiter, false);

    if (parts.size() & 1)
    {
        lua_pushnumber(L, eArrayNotEvenNumberOfValues);
        return 1;
    }

    int iDuplicates = 0;
    for (size_t i = 0; i < parts.size(); i += 2)
    {
        string sKey   = parts[i];
        string sValue = parts[i+1];
        if (!sTemp.empty())
        {
            sKey   = FindAndReplace(sKey,   sTemp, sDelimiter);
            sValue = FindAndReplace(sValue, sTemp, sDelimiter);
        }
        sKey   = FindAndReplace(sKey,   "\\\\", "\\");
        sValue = FindAndReplace(sValue, "\\\\", "\\");

        auto result = it->second->insert(make_pair(sKey, sValue));
        if (!result.second)
        {
            result.first->second = sValue;
            ++iDuplicates;
        }
    }

    lua_pushnumber(L, iDuplicates ? eImportedWithDuplicates : eOK);
    return 1;
}

// ArrayExport: serialize array as a delimited key,value,key,value... string.
static int L_ArrayExport(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name  = luaL_checkstring(L, 1);
    const char* delim = luaL_optstring  (L, 2, ",");

    string sDelimiter(delim);
    if (sDelimiter.size() != 1 || sDelimiter == "\\")
    {
        lua_pushnumber(L, eBadDelimiter);
        return 1;
    }

    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }

    if (it->second->empty()) { lua_pushstring(L, ""); return 1; }

    string sResult;
    size_t iCount = 0;
    size_t total  = it->second->size();
    for (auto& kv : *it->second)
    {
        string sKey  = FindAndReplace(kv.first,  "\\", "\\\\");
        sKey  = FindAndReplace(sKey,  sDelimiter, "\\" + sDelimiter);
        string sVal  = FindAndReplace(kv.second, "\\", "\\\\");
        sVal  = FindAndReplace(sVal,  sDelimiter, "\\" + sDelimiter);
        sResult += sKey + sDelimiter + sVal;
        if (++iCount < total) sResult += sDelimiter;
    }
    lua_pushstring(L, sResult.c_str());
    return 1;
}

// ArrayExportKeys: serialize only the keys as a delimited string.
static int L_ArrayExportKeys(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name  = luaL_checkstring(L, 1);
    const char* delim = luaL_optstring  (L, 2, ",");

    string sDelimiter(delim);
    if (sDelimiter.size() != 1 || sDelimiter == "\\")
    {
        lua_pushnumber(L, eBadDelimiter);
        return 1;
    }

    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
    {
        lua_pushnumber(L, eArrayDoesNotExist);
        return 1;
    }

    if (it->second->empty()) { lua_pushstring(L, ""); return 1; }

    string sResult;
    size_t iCount = 0;
    size_t total  = it->second->size();
    for (auto& kv : *it->second)
    {
        string sKey = FindAndReplace(kv.first,  "\\", "\\\\");
        sKey = FindAndReplace(sKey, sDelimiter, "\\" + sDelimiter);
        sResult += sKey;
        if (++iCount < total) sResult += sDelimiter;
    }
    lua_pushstring(L, sResult.c_str());
    return 1;
}

// ── Database subsystem ────────────────────────────────────────────────────────
// Wraps sqlite3 via the same named-handle map as the Windows lua_methods.cpp.
// Error codes: negative MUSHclient DB errors or positive SQLite result codes.

#define DB_ERR_ID_NOT_FOUND             -1
#define DB_ERR_NOT_OPEN                 -2
#define DB_ERR_HAVE_STMT                -3
#define DB_ERR_NO_STMT                  -4
#define DB_ERR_NO_VALID_ROW             -5
#define DB_ERR_ALREADY_EXISTS           -6
#define DB_ERR_COL_OOB                  -7

// Helper: look up named database; returns nullptr if not found.
static tDatabase* FindDB(CMUSHclientDoc* pDoc, const char* name)
{
    auto it = pDoc->m_Databases.find(name);
    return (it != pDoc->m_Databases.end()) ? it->second : nullptr;
}

static int L_DatabaseOpen(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name     = luaL_checkstring(L, 1);
    const char* filename = luaL_checkstring(L, 2);
    int         flags    = (int)luaL_optinteger(L, 3,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

    auto it = pDoc->m_Databases.find(name);
    if (it != pDoc->m_Databases.end())
    {
        // Reuse if same filename
        lua_pushnumber(L, it->second->db_name == filename
                          ? SQLITE_OK : DB_ERR_ALREADY_EXISTS);
        return 1;
    }

    tDatabase* pDB = new tDatabase{nullptr, nullptr, false, filename, 0};
    int rc = sqlite3_open_v2(filename, &pDB->db, flags, nullptr);
    if (rc == SQLITE_OK && pDB->db)
        pDoc->m_Databases[name] = pDB;
    else
        delete pDB;
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabaseClose(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->m_Databases.find(name);
    if (it == pDoc->m_Databases.end()) { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    tDatabase* pDB = it->second;
    if (!pDB->db) { lua_pushnumber(L, DB_ERR_NOT_OPEN); return 1; }
    if (pDB->pStmt) sqlite3_finalize(pDB->pStmt);
    int rc = sqlite3_close(pDB->db);
    delete pDB;
    pDoc->m_Databases.erase(it);
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabasePrepare(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    const char* sql  = luaL_checkstring(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)         { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)     { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (pDB->pStmt)   { lua_pushnumber(L, DB_ERR_HAVE_STMT);    return 1; }
    pDB->bValidRow = false;
    pDB->iColumns  = 0;
    const char* tail;
    int rc = sqlite3_prepare_v2(pDB->db, sql, -1, &pDB->pStmt, &tail);
    if (rc == SQLITE_OK)
        pDB->iColumns = sqlite3_column_count(pDB->pStmt);
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabaseFinalize(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)       { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)   { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (!pDB->pStmt){ lua_pushnumber(L, DB_ERR_NO_STMT);      return 1; }
    int rc = sqlite3_finalize(pDB->pStmt);
    pDB->pStmt     = nullptr;
    pDB->bValidRow = false;
    pDB->iColumns  = 0;
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabaseStep(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)       { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)   { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (!pDB->pStmt){ lua_pushnumber(L, DB_ERR_NO_STMT);      return 1; }
    int rc = sqlite3_step(pDB->pStmt);
    pDB->bValidRow = (rc == SQLITE_ROW);
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabaseReset(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)       { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)   { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (!pDB->pStmt){ lua_pushnumber(L, DB_ERR_NO_STMT);      return 1; }
    int rc = sqlite3_reset(pDB->pStmt);
    pDB->bValidRow = false;
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabaseExec(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    const char* sql  = luaL_checkstring(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)       { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)   { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (pDB->pStmt) { lua_pushnumber(L, DB_ERR_HAVE_STMT);    return 1; }
    pDB->bValidRow = false;
    pDB->iColumns  = 0;
    int rc = sqlite3_exec(pDB->db, sql, nullptr, nullptr, nullptr);
    lua_pushnumber(L, rc);
    return 1;
}

static int L_DatabaseColumns(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)       { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)   { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (!pDB->pStmt){ lua_pushnumber(L, DB_ERR_NO_STMT);      return 1; }
    lua_pushnumber(L, sqlite3_column_count(pDB->pStmt));
    return 1;
}

static int L_DatabaseColumnName(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int col = (int)luaL_checkinteger(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (pDB && pDB->db && pDB->pStmt && col >= 1 && col <= pDB->iColumns)
    {
        const char* p = sqlite3_column_name(pDB->pStmt, col - 1);
        lua_pushstring(L, p ? p : "");
    }
    else
        lua_pushstring(L, "");
    return 1;
}

static int L_DatabaseColumnText(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int col = (int)luaL_checkinteger(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (pDB && pDB->db && pDB->pStmt && pDB->bValidRow && col >= 1 && col <= pDB->iColumns)
    {
        const unsigned char* p = sqlite3_column_text(pDB->pStmt, col - 1);
        lua_pushstring(L, p ? (const char*)p : "");
    }
    else
        lua_pushstring(L, "");
    return 1;
}

static int L_DatabaseColumnValue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int col = (int)luaL_checkinteger(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB || !pDB->db || !pDB->pStmt || !pDB->bValidRow ||
        col < 1 || col > pDB->iColumns)
    {
        lua_pushnil(L);
        return 1;
    }
    int t = sqlite3_column_type(pDB->pStmt, col - 1);
    switch (t)
    {
    case SQLITE_INTEGER:
        lua_pushnumber(L, (lua_Number)sqlite3_column_int64(pDB->pStmt, col - 1));
        break;
    case SQLITE_FLOAT:
        lua_pushnumber(L, sqlite3_column_double(pDB->pStmt, col - 1));
        break;
    case SQLITE_NULL:
        lua_pushnil(L);
        break;
    default: // SQLITE3_TEXT, SQLITE_BLOB
        {
            const unsigned char* p = sqlite3_column_text(pDB->pStmt, col - 1);
            lua_pushstring(L, p ? (const char*)p : "");
        }
        break;
    }
    return 1;
}

static int L_DatabaseColumnType(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    int col = (int)luaL_checkinteger(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)            { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db)        { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    if (!pDB->pStmt)     { lua_pushnumber(L, DB_ERR_NO_STMT);      return 1; }
    if (!pDB->bValidRow) { lua_pushnumber(L, DB_ERR_NO_VALID_ROW); return 1; }
    if (col < 1 || col > pDB->iColumns) { lua_pushnumber(L, DB_ERR_COL_OOB); return 1; }
    lua_pushnumber(L, sqlite3_column_type(pDB->pStmt, col - 1));
    return 1;
}

static int L_DatabaseColumnNames(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB || !pDB->db || !pDB->pStmt) return 1;
    for (int i = 0; i < pDB->iColumns; ++i)
    {
        const char* p = sqlite3_column_name(pDB->pStmt, i);
        lua_pushstring(L, p ? p : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int L_DatabaseColumnValues(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB || !pDB->db || !pDB->pStmt || !pDB->bValidRow) return 1;
    for (int i = 0; i < pDB->iColumns; ++i)
    {
        int t = sqlite3_column_type(pDB->pStmt, i);
        if (t == SQLITE_INTEGER)
            lua_pushnumber(L, (lua_Number)sqlite3_column_int64(pDB->pStmt, i));
        else if (t == SQLITE_FLOAT)
            lua_pushnumber(L, sqlite3_column_double(pDB->pStmt, i));
        else if (t == SQLITE_NULL)
            lua_pushnil(L);
        else
        {
            const unsigned char* p = sqlite3_column_text(pDB->pStmt, i);
            lua_pushstring(L, p ? (const char*)p : "");
        }
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int L_DatabaseError(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->m_Databases.find(name);
    if (it == pDoc->m_Databases.end())
    {
        lua_pushstring(L, "database id not found");
        return 1;
    }
    tDatabase* pDB = it->second;
    if (!pDB->db)
    {
        lua_pushstring(L, "database not open");
        return 1;
    }
    const char* msg = sqlite3_errmsg(pDB->db);
    lua_pushstring(L, msg ? msg : "");
    return 1;
}

static int L_DatabaseChanges(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)     { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db) { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    lua_pushnumber(L, sqlite3_changes(pDB->db));
    return 1;
}

static int L_DatabaseTotalChanges(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB)     { lua_pushnumber(L, DB_ERR_ID_NOT_FOUND); return 1; }
    if (!pDB->db) { lua_pushnumber(L, DB_ERR_NOT_OPEN);     return 1; }
    lua_pushnumber(L, sqlite3_total_changes(pDB->db));
    return 1;
}

static int L_DatabaseLastInsertRowid(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    tDatabase* pDB = FindDB(pDoc, name);
    if (pDB && pDB->db)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld",
                 (long long)sqlite3_last_insert_rowid(pDB->db));
        lua_pushstring(L, buf);
    }
    else
        lua_pushstring(L, "");
    return 1;
}

static int L_DatabaseList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int i = 1;
    for (auto& kv : pDoc->m_Databases)
    {
        lua_pushstring(L, kv.first.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int L_DatabaseInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name     = luaL_checkstring(L, 1);
    int         infoType = (int)luaL_checkinteger(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB) { lua_pushnil(L); return 1; }
    switch (infoType)
    {
    case 1: lua_pushstring(L, pDB->db_name.c_str()); break;
    case 2: lua_pushboolean(L, pDB->pStmt     != nullptr ? 1 : 0); break;
    case 3: lua_pushboolean(L, pDB->bValidRow ? 1 : 0); break;
    case 4: lua_pushnumber(L, pDB->iColumns); break;
    default: lua_pushnil(L); break;
    }
    return 1;
}

// GetField: one-shot prepare→step→text→finalize for a single-row query
static int L_DatabaseGetField(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    const char* sql  = luaL_checkstring(L, 2);
    tDatabase* pDB = FindDB(pDoc, name);
    if (!pDB || !pDB->db) { lua_pushnil(L); return 1; }
    sqlite3_stmt* stmt = nullptr;
    const char* tail;
    if (sqlite3_prepare_v2(pDB->db, sql, -1, &stmt, &tail) != SQLITE_OK || !stmt)
    {
        lua_pushnil(L);
        return 1;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char* p = sqlite3_column_text(stmt, 0);
        lua_pushstring(L, p ? (const char*)p : "");
    }
    else
        lua_pushnil(L);
    sqlite3_finalize(stmt);
    return 1;
}

// ── Replace ───────────────────────────────────────────────────────────────────
// world.Replace(source, searchFor, replaceWith [, multiple]) → string
// Declared in stdafx.h / Replace.cpp

extern CString Replace(const CString strSource, const CString strTarget,
                       const CString strReplacement, const bool bAll);

static int L_Replace(lua_State* L)
{
    // Real CMUSHclientDoc::Replace with lua_methods.cpp's optboolean(L,4,1):
    // Multiple defaults to TRUE — the old twin defaulted it to false, so
    // world.Replace(s, a, b) replaced only the first occurrence (drift).
    bool bMultiple = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4) != 0;
    BSTR str = world_doc(L)->Replace(
        luaL_checkstring(L, 1),        // Source
        luaL_checkstring(L, 2),        // SearchFor
        luaL_checkstring(L, 3),        // ReplaceWith
        bMultiple ? TRUE : FALSE);     // Multiple
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── EditDistance / Metaphone ──────────────────────────────────────────────────

#include "Dmetaph.h"

// Declared in stdafx.h; provide forward declaration here since we don't include stdafx.h
int EditDistance(const string source, const string target);

static int L_EditDistance(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->EditDistance(   // real method
        luaL_checkstring(L, 1),    // source
        luaL_checkstring(L, 2)));  // target
    return 1;
}

static int L_Metaphone(lua_State* L)
{
    // Real CMUSHclientDoc::Metaphone with lua_methods.cpp's default length
    // of 4 (the old twin defaulted to 6 — drift).
    BSTR str = world_doc(L)->Metaphone(
        luaL_checkstring(L, 1),
        (short)luaL_optnumber(L, 2, 4));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── GetEntity / SetEntity / GetXMLEntity ─────────────────────────────────────
// Thin wrappers over the REAL scripting/methods/methods_xml.cpp methods
// (adopted into mushcore), marshalled exactly as lua_methods.cpp.

static int L_GetEntity(lua_State* L)
{
    BSTR str = world_doc(L)->GetEntity(luaL_checkstring(L, 1));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

static int L_SetEntity(lua_State* L)
{
    // lua_methods.cpp L_SetEntity: optional second arg (default ""), no result.
    world_doc(L)->SetEntity(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""));
    return 0;
}

static int L_GetXMLEntity(lua_State* L)
{
    // Real CXMLparser::Get_XML_Entity resolution (standard + numeric
    // entities), replacing the old 5-entity local table.
    BSTR str = world_doc(L)->GetXMLEntity(luaL_checkstring(L, 1));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── SendSpecial / SendPkt ─────────────────────────────────────────────────────

static int L_SendSpecial(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg  = luaL_checkstring(L, 1);
    bool bEcho       = lua_toboolean(L, 2) != 0;
    bool bQueue      = lua_toboolean(L, 3) != 0;
    bool bLog        = lua_toboolean(L, 4) != 0;
    bool bHistory    = lua_toboolean(L, 5) != 0;
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud)
        { lua_pushnumber(L, eWorldClosed); return 1; }
    if (pDoc->m_bPluginProcessingSent)
        { lua_pushnumber(L, eItemInUse); return 1; }
    pDoc->SendMsg(CString(msg), bEcho, bQueue, bLog);
    if (bHistory)
        pDoc->AddToCommandHistory(CString(msg));
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_SendPkt(lua_State* L)
{
    // Deliberately NOT the doc method: lua_methods.cpp L_SendPkt also
    // bypasses CMUSHclientDoc::SendPkt and calls SendPacket with an explicit
    // length so packets may contain embedded NULs.  This matches it.
    CMUSHclientDoc* pDoc = world_doc(L);
    size_t len;
    const char* pkt = luaL_checklstring(L, 1, &len);
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud)
        { lua_pushnumber(L, eWorldClosed); return 1; }
    pDoc->SendPacket(pkt, (int)len);
    lua_pushnumber(L, eOK);
    return 1;
}

// ── DoCommand ────────────────────────────────────────────────────────────────
// On Windows this dispatches a GUI menu command by name; on portable it's a no-op.

static int L_DoCommand(lua_State* L)
{
    // The REAL DoCommand (methods_commands.cpp): looks the name up in the
    // real CommandIDs[] table (accelerators.cpp) — unknown → eNoSuchCommand,
    // known → eOK after posting WM_COMMAND (the stub Frame drops the post
    // headless; the GUI build dispatches it).
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->DoCommand(luaL_checkstring(L, 1)));
    return 1;
}

// ── Reset (MXP off) ───────────────────────────────────────────────────────────

static int L_Reset(lua_State* L)
{
    // Real CMUSHclientDoc::Reset (methods_xml.cpp: MXP_Off); Windows
    // lua_methods.cpp L_Reset returns no result fields.
    world_doc(L)->Reset();
    return 0;
}

// ── Sound functions ───────────────────────────────────────────────────────────
// Thin wrappers over the REAL CMUSHclientDoc sound methods (methods_sounds.cpp
// _NOSOUND fork, adopted into mushcore: full buffer/volume/pan semantics over
// the g_wxhook_sound_* miniaudio hooks), marshalled exactly as lua_methods.cpp.
// Headless the hooks are null → eCannotPlaySound / GetSoundStatus -3.

static int L_Sound(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->Sound(luaL_checkstring(L, 1)));
    return 1;
}
static int L_StopSound(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->StopSound((short)luaL_optnumber(L, 1, 0)));   // Buffer
    return 1;
}
static int L_GetSoundStatus(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->GetSoundStatus((short)luaL_checknumber(L, 1)));
    return 1;
}
static int L_PlaySound(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->PlaySound(
        (short)luaL_checknumber(L, 1),                       // Buffer
        luaL_optstring(L, 2, ""),                            // Filename
        lua_isnoneornil(L, 3) ? 0 : lua_toboolean(L, 3),     // Repeat
        luaL_optnumber(L, 4, 0),                             // Volume
        luaL_optnumber(L, 5, 0)));                           // Pan
    return 1;
}

// ── UDP ───────────────────────────────────────────────────────────────────────

#include <sys/socket.h>
#include <netinet/in.h>

// Thin wrappers over the REAL CMUSHclientDoc::UdpSend / UdpListen /
// GetUdpPort (methods_udp.cpp, adopted into mushcore) — mirror
// lua_methods.cpp L_UdpSend / L_UdpListen / L_GetUdpPort.  UdpListen now
// actually creates a listener (UDPsocket over the POSIX CAsyncSocket) rather
// than faking eOK.
static int L_UdpSend(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->UdpSend(luaL_checkstring(L, 1),
                                    (short)luaL_checknumber(L, 2),
                                    luaL_checkstring(L, 3)));
    return 1;
}

static int L_UdpListen(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->UdpListen(luaL_checkstring(L, 1),
                                      (short)luaL_checknumber(L, 2),
                                      luaL_checkstring(L, 3)));
    return 1;
}

static int L_GetUdpPort(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->GetUdpPort((long)luaL_checknumber(L, 1),
                                       (long)luaL_checknumber(L, 2)));
    return 1;
}

// ── TranslateDebug ────────────────────────────────────────────────────────────

static int L_TranslateDebug(lua_State* L)
{
    // Real CMUSHclientDoc::TranslateDebug — headless App.m_Translator_Lua is
    // null, so this returns 1 ("no script") exactly as before.
    lua_pushnumber(L, world_doc(L)->TranslateDebug(luaL_optstring(L, 1, "")));
    return 1;
}

// ── GetNoteColour ─────────────────────────────────────────────────────────────

static int L_GetNoteColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* which = luaL_optstring(L, 1, "fore");
    string w(which);
    for (auto& c : w) c = (char)tolower((unsigned char)c);
    if (w == "back")
        lua_pushnumber(L, (lua_Number)(long)pDoc->m_iNoteColourBack);
    else
        lua_pushnumber(L, (lua_Number)(long)pDoc->m_iNoteColourFore);
    return 1;
}

// ── Command history / command window ──────────────────────────────────────────
// Thin wrappers over the REAL methods_commands.cpp methods (adopted into
// mushcore), marshalled exactly as lua_methods.cpp.  Off-Windows BSTR is a
// heap char* (CString::AllocSysString) that the caller frees — the pushBstr
// equivalent here is push-then-delete[].

static int L_GetCommand(lua_State* L)
{
    // world.GetCommand() — the current text in the command input bar (empty
    // with no view attached, e.g. headless).
    BSTR str = world_doc(L)->GetCommand();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

static int L_GetCommandList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetCommandList(
        (long)luaL_checknumber(L, 1)   // Count (0 = all), required as on Windows
      );
    return push_option_variant(L, v);
}

static int L_AddToCommandHistory(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* cmd = luaL_checkstring(L, 1);
    pDoc->AddToCommandHistory(CString(cmd));
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_DeleteCommandHistory(lua_State* L)
{
    world_doc(L)->DeleteCommandHistory();
    return 0;   // Windows returns no result fields
}

static int L_PushCommand(lua_State* L) {
    BSTR str = world_doc(L)->PushCommand();
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}
static int L_SelectCommand(lua_State* L) {
    world_doc(L)->SelectCommand();
    return 0;   // Windows returns no result fields
}

static int L_PasteCommand(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    BSTR str = pDoc->PasteCommand(luaL_checkstring(L, 1));
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

static int L_SetCommand(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, pDoc->SetCommand(luaL_checkstring(L, 1)));
    return 1;
}
static int L_GetCommandListFunc(lua_State* L) { return L_GetCommandList(L); }

// ── InfoBar stubs ─────────────────────────────────────────────────────────────
static int L_Info(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_checkstring(L, 1);
    pDoc->m_strStatusMessage = CString(msg);
    pDoc->EmitStatus(msg);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_InfoBackground(lua_State* L)   { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_InfoClear(lua_State* L)        { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_InfoColour(lua_State* L)       { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_InfoFont(lua_State* L)         { world_doc(L); lua_pushnumber(L, eOK); return 1; }

// ── GUI callbacks ─────────────────────────────────────────────────────────────
static int L_Activate(lua_State* L) {
    // Real CMUSHclientDoc::Activate (methods_utilities.cpp → the same
    // ActivateClientWindow seam).  Windows returns no result fields — the
    // old twin pushed eOK.
    world_doc(L)->Activate();
    return 0;
}
static int L_ActivateClient(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->ActivateClientWindow();
    lua_pushnumber(L, eOK); return 1;
}
static int L_Redraw(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->EmitRepaint();
    lua_pushnumber(L, eOK); return 1;
}
static int L_Repaint(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->EmitRepaint();
    lua_pushnumber(L, eOK);
    return 1;
}
// Portable system-colour / metrics / device-caps implementations.
// These cannot query the OS directly from mushcore (no wxWidgets), so we
// return the most common/neutral values for the constants scripts use most.
// COLORREF format: 0x00BBGGRR (same as Win32 GetSysColor return value).
static int L_GetSysColor(lua_State* L)
{
    // Real CMUSHclientDoc::GetSysColor → ::GetSysColor (mfc_ui.h) — the
    // same hook-based shim the mac GUI resolves real theme colours through
    // (null hooks headless → 0).  The old twin's hand-faked colour table is
    // gone.
    lua_pushnumber(L, world_doc(L)->GetSysColor((long)luaL_checknumber(L, 1)));
    return 1;
}
static int L_GetSystemMetrics(lua_State* L)
{
    // Real CMUSHclientDoc::GetSystemMetrics → ::GetSystemMetrics (mfc_ui.h
    // hook shim; null hooks headless → 0).  The old twin faked 17px
    // scrollbars.
    lua_pushnumber(L, world_doc(L)->GetSystemMetrics((long)luaL_checknumber(L, 1)));
    return 1;
}
static int L_GetDeviceCaps(lua_State* L)
{
    // Real CMUSHclientDoc::GetDeviceCaps → CDC::GetDeviceCaps (mfc_ui.h;
    // real display metrics in the GUI, zeros headless).  The old twin faked
    // 32bpp/96dpi.
    lua_pushnumber(L, world_doc(L)->GetDeviceCaps((long)luaL_checknumber(L, 1)));
    return 1;
}
// No system clipboard in the headless build; the real clipboard path is the
// GetClipboardContents/putontoclipboard globals (an IHostUi concern, P5).
static int L_GetClipboard(lua_State* L)
{
    world_doc(L);
    lua_pushstring(L, "");
    return 1;
}
static int L_SetClipboard(lua_State* L)
{
    world_doc(L);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_SetBackgroundColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    unsigned long col = (unsigned long)(long)luaL_optnumber(L, 1, 0xFFFFFFFF);
    pDoc->EmitBackgroundColour(col);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_SetBackgroundImage(lua_State* L) { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_SetCursor(lua_State* L)         { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_SetChanged(lua_State* L) {
    // Real CMUSHclientDoc::SetChanged.  optboolean semantics as Windows
    // (accepts true/false, defaults true — the old twin's luaL_optinteger
    // errored on booleans); Windows returns no result fields.
    bool flag = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1) != 0;
    world_doc(L)->SetChanged(flag ? TRUE : FALSE);
    return 0;
}
static int L_GetMainWindowPosition(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    int x = 0, y = 0, w = 1024, h = 768;
    pDoc->GetMainRect(x, y, w, h);
    lua_pushnumber(L, x); lua_pushnumber(L, y);
    lua_pushnumber(L, w); lua_pushnumber(L, h);
    return 4;
}
static int L_GetWorldWindowPosition(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    int x = 0, y = 0, w = 800, h = 600;
    pDoc->GetOutputRect(x, y, w, h);
    lua_pushnumber(L, x); lua_pushnumber(L, y);
    lua_pushnumber(L, w); lua_pushnumber(L, h);
    return 4;
}
// GetWorldWindowPositionX(which): 0=x, 1=y, 2=width, 3=height of output pane.
static int L_GetWorldWindowPositionX(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_optinteger(L, 1, 2);   // default = width
    int x = 0, y = 0, w = 800, h = 600;
    pDoc->GetOutputRect(x, y, w, h);
    int vals[4] = { x, y, w, h };
    lua_pushnumber(L, (which >= 0 && which < 4) ? vals[which] : 0);
    return 1;
}
static int L_MoveMainWindow(lua_State* L) {
    world_doc(L);
    long l = (long)luaL_optnumber(L, 1, 0);
    long t = (long)luaL_optnumber(L, 2, 0);
    long w = (long)luaL_optnumber(L, 3, 0);
    long h = (long)luaL_optnumber(L, 4, 0);
    if (g_wxhook_move_main_window) g_wxhook_move_main_window(l, t, w, h);
    lua_pushnumber(L, eOK); return 1;
}
static int L_MoveWorldWindow(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    long l = (long)luaL_optnumber(L, 1, 0);
    long t = (long)luaL_optnumber(L, 2, 0);
    long w = (long)luaL_optnumber(L, 3, 0);
    long h = (long)luaL_optnumber(L, 4, 0);
    pDoc->MoveWorldWindowRect(l, t, w, h);
    lua_pushnumber(L, eOK); return 1;
}
static int L_MoveWorldWindowX(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    long l = (long)luaL_optnumber(L, 1, 0);
    long t = (long)luaL_optnumber(L, 2, 0);
    long w = (long)luaL_optnumber(L, 3, 0);
    long h = (long)luaL_optnumber(L, 4, 0);
    short which = (short)luaL_optinteger(L, 5, 1);
    if (which == 1)
        pDoc->MoveWorldWindowRect(l, t, w, h);
    lua_pushnumber(L, eOK); return 1;
}
static int L_FlashIcon(lua_State* L) {
    world_doc(L);
    if (g_wxhook_flash_icon) g_wxhook_flash_icon();
    lua_pushnumber(L, eOK); return 1;
}
static int L_Help(lua_State* L)              {
    // Real CMUSHclientDoc::Help — headless the ShowHelp/ShowFunctionslist
    // sinks are inert (doc_stub.h), so this resolves the topic and shows
    // nothing; Windows returns no result fields (the old twin pushed eOK).
    world_doc(L)->Help(luaL_optstring(L, 1, ""));
    return 0;
}
static int L_Menu(lua_State* L) {
    // Real CMUSHclientDoc::Menu (methods_utilities.cpp) — its non-Windows
    // arm is the very g_wxhook_menu_dialog popup the old twin re-implemented
    // (null hook headless → "").
    BSTR str = world_doc(L)->Menu(
        luaL_checkstring(L, 1),      // Items
        luaL_optstring(L, 2, ""));   // Default
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}
static int L_PickColour(lua_State* L) {
    world_doc(L);
    long suggested = (long)luaL_optnumber(L, 1, -1);
    long result = g_wxhook_pick_colour ? g_wxhook_pick_colour(suggested) : -1;
    lua_pushnumber(L, result); return 1;
}
static int L_Save(lua_State* L) {
    // Real CMUSHclientDoc::Save via the lua_methods.cpp L_Save shape:
    // boolean result, optional Save-As flag restoring the document path
    // (the port SetPathName takes no MRU flag).  Headless DoSave writes the
    // portable world XML; Save("") with no document path returns false.
    CMUSHclientDoc* pDoc = world_doc(L);
    CString strOldName = pDoc->GetPathName();
    bool bSaveAs = lua_toboolean(L, 2) != 0;
    if (bSaveAs)
        lua_pushboolean(L, pDoc->Save(luaL_checkstring(L, 1)));
    else
        lua_pushboolean(L, pDoc->Save(luaL_optstring(L, 1, "")));
    if (bSaveAs)
        pDoc->SetPathName(strOldName);
    return 1;
}
static int L_SetWorldWindowStatus(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    short param = (short)luaL_optinteger(L, 1, 3);
    pDoc->EmitWindowStatus(param);
    lua_pushnumber(L, eOK); return 1;
}

// ── Mapper stubs ──────────────────────────────────────────────────────────────
static int L_AddMapperComment(lua_State* L)  { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_AddToMapper(lua_State* L)       { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_DeleteAllMapItems(lua_State* L) { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_DeleteLastMapItem(lua_State* L) { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_EnableMapping(lua_State* L)     { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_GetMapColour(lua_State* L)      { world_doc(L); lua_pushnumber(L, 0); return 1; }
static int L_GetMappingCount(lua_State* L)   { world_doc(L); lua_pushnumber(L, 0); return 1; }
static int L_RemoveBacktracks(lua_State* L)  { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_DoAfterSpeedWalk(lua_State* L)  { world_doc(L); lua_pushnumber(L, eOK); return 1; }

// ── ExportXML / ImportXML ─────────────────────────────────────────────────────
// ExportXML(type, name) → XML string or ""
//   type: 0=trigger, 1=alias, 2=timer, 3=macro, 4=variable, 5=keypad
// ImportXML(xml) → count of items imported (or -1 on error)
static int L_ExportXML(lua_State* L)
{
    // Real CMUSHclientDoc::ExportXML (methods_xml.cpp) over the canonical
    // Save_One_*_XML writers (xml/xml_save_world.cpp) — all six item types,
    // byte-identical to the Windows output.  (The twin ExportXML_* helpers
    // in the deleted xml/xml_save_world_portable.cpp are gone.)
    BSTR str = world_doc(L)->ExportXML(
        (short)luaL_checknumber(L, 1),   // Type
        luaL_checkstring(L, 2));         // Name
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}
static int L_ImportXML(lua_State* L)
{
    // Deliberately NOT the real CMUSHclientDoc::ImportXML (methods_xml.cpp):
    // that drives the archive-based Load_World_XML, which is still a no-op
    // headless (the real xml/xml_load_world.cpp is not adopted yet).  The
    // portable ImportXMLFromString (xml/xml_load_world_portable.cpp) does
    // the actual import; rewire when the real loader adopts.
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* xml = luaL_checkstring(L, 1);
    long count = pDoc->ImportXMLFromString(xml);
    lua_pushnumber(L, (lua_Number)count);
    return 1;
}

// ── ResetIP / ResetStatusTime / SaveState / OnFileReloaddefaults ──────────────
static int L_ResetIP(lua_State* L)           {
    // Real CMUSHclientDoc::ResetIP (zeros the cached socket/proxy addresses,
    // forcing a fresh name lookup); Windows returns no result fields.
    world_doc(L)->ResetIP();
    return 0;
}
static int L_ResetStatusTime(lua_State* L)   { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_SaveState(lua_State* L)         { lua_pushnumber(L, world_doc(L)->SaveState()); return 1; }  // real method (Windows semantics: eNotAPlugin outside a plugin)

// ── GetStyleInfo ──────────────────────────────────────────────────────────────
// Returns info about a style run within a specific output line.
// GetStyleInfo(lineNumber, styleNumber, infoType) → value
// If styleNumber==0 and infoType==0: returns a table of tables (all styles).
// If styleNumber==0 and infoType!=0: returns a table (one value per style).
// If styleNumber!=0 and infoType==0: returns a table of all info for that style.
// If both nonzero: returns the single value.
static void push_style_info(lua_State* L, CMUSHclientDoc* pDoc,
                             CLine* pLine, int styleNum,
                             const CString& lineText, int infoType)
{
    // styleNum is 1-based; walk to it and track column
    int iCol = 0;
    int iCount = 1;
    CStyle* pStyle = nullptr;
    POSITION pos = pLine->styleList.GetHeadPosition();
    while (pos) {
        CStyle* s = pLine->styleList.GetNext(pos);
        if (iCount == styleNum) { pStyle = s; break; }
        iCol += s->iLength;
        iCount++;
    }
    if (!pStyle) { lua_pushnil(L); return; }

    CAction* pAction = pStyle->pAction;
    COLORREF colour1, colour2;

    switch (infoType) {
    case 1: {
        // text covered by this style
        CString s = lineText.Mid(iCol, pStyle->iLength);
        lua_pushstring(L, static_cast<const char*>(s));
        break;
    }
    case 2: lua_pushnumber(L, pStyle->iLength); break;
    case 3: lua_pushnumber(L, iCol + 1); break;  // 1-based column
    case 4: {
        int iAction = 0;
        switch (pStyle->iFlags & ACTIONTYPE) {
            case ACTION_NONE:      iAction = 0; break;
            case ACTION_SEND:      iAction = 1; break;
            case ACTION_HYPERLINK: iAction = 2; break;
            case ACTION_PROMPT:    iAction = 3; break;
        }
        lua_pushnumber(L, iAction);
        break;
    }
    case 5:  lua_pushstring(L, pAction ? static_cast<const char*>(pAction->m_strAction)   : ""); break;
    case 6:  lua_pushstring(L, pAction ? static_cast<const char*>(pAction->m_strHint)     : ""); break;
    case 7:  lua_pushstring(L, pAction ? static_cast<const char*>(pAction->m_strVariable) : ""); break;
    case 8:  lua_pushboolean(L, (pStyle->iFlags & HILITE)     != 0); break;
    case 9:  lua_pushboolean(L, (pStyle->iFlags & UNDERLINE)  != 0); break;
    case 10: lua_pushboolean(L, (pStyle->iFlags & BLINK)      != 0); break;
    case 11: lua_pushboolean(L, (pStyle->iFlags & INVERSE)    != 0); break;
    case 12: lua_pushboolean(L, (pStyle->iFlags & CHANGED)    != 0); break;
    case 13: lua_pushboolean(L, (pStyle->iFlags & START_TAG)  != 0); break;
    case 14: pDoc->GetStyleRGB(pStyle, colour1, colour2); lua_pushnumber(L, (long)colour1); break;
    case 15: pDoc->GetStyleRGB(pStyle, colour1, colour2); lua_pushnumber(L, (long)colour2); break;
    default: lua_pushnil(L); break;
    }
}

// Build a table of all info types (1-15) for one style.
static void push_style_table(lua_State* L, CMUSHclientDoc* pDoc,
                              CLine* pLine, int styleNum,
                              const CString& lineText)
{
    lua_newtable(L);
    for (int t = 1; t <= 15; t++) {
        push_style_info(L, pDoc, pLine, styleNum, lineText, t);
        lua_rawseti(L, -2, t);
    }
}

static int L_GetStyleInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int iLine        = (int)luaL_checknumber(L, 1);
    int iStyleNumber = (int)luaL_optnumber(L, 2, 0);
    int iType        = (int)luaL_optnumber(L, 3, 0);

    long lineCount = pDoc->m_LineList.GetCount();
    if (iLine <= 0 || iLine > lineCount)
        return 0;

    CLine* pLine = GetLineAt(pDoc, iLine - 1);
    if (!pLine) return 0;

    CString lineText(pLine->text, pLine->len);
    int styleCount = (int)pLine->styleList.GetCount();

    if (iStyleNumber == 0 && iType == 0) {
        // all styles, all types — table of tables
        lua_newtable(L);
        for (int s = 1; s <= styleCount; s++) {
            push_style_table(L, pDoc, pLine, s, lineText);
            lua_rawseti(L, -2, s);
        }
        return 1;
    }
    if (iStyleNumber == 0) {
        // all styles, one type — table of values
        lua_newtable(L);
        for (int s = 1; s <= styleCount; s++) {
            push_style_info(L, pDoc, pLine, s, lineText, iType);
            lua_rawseti(L, -2, s);
        }
        return 1;
    }
    if (iType == 0) {
        // one style, all types — table of values
        push_style_table(L, pDoc, pLine, iStyleNumber, lineText);
        return 1;
    }
    // single value — the REAL CMUSHclientDoc::GetStyleInfo, marshalled via
    // push_option_variant exactly as lua_methods.cpp's "normal behaviour"
    // arm (the table forms above remain the lua_methods.cpp Lua extension).
    VARIANT v = pDoc->GetStyleInfo(iLine, iStyleNumber, (short)iType);
    return push_option_variant(L, v);
}

// ── GetOptionList / GetAlphaOptionList ────────────────────────────────────────
// Thin wrappers over the REAL doc methods (scriptingoptions.cpp, the full
// 174/70-row option tables), mirroring lua_methods.cpp: a 1-based array of
// option names via push_option_variant's VT_ARRAY branch.

static int L_GetOptionList(lua_State* L)
{
    VARIANT v = world_doc(L)->GetOptionList();
    return push_option_variant(L, v);
}

static int L_GetAlphaOptionList(lua_State* L)
{
    VARIANT v = world_doc(L)->GetAlphaOptionList();
    return push_option_variant(L, v);
}

// ── GetDefaultValue / GetLoadedValue ─────────────────────────────────────────
// Looks up the option in OptionsTable[] (numeric) or AlphaOptionsTable[] (string)
// via the real doc methods.  Returns nil if not found.
static int L_GetDefaultValue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetDefaultValue(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}

static int L_GetLoadedValue(lua_State* L)
{
    // Real Windows semantics at last: reads the m_NumericConfiguration /
    // m_AlphaConfiguration snapshots the loader now populates (the old twin
    // admitted it wrongly mirrored GetDefaultValue).
    CMUSHclientDoc* pDoc = world_doc(L);
    VARIANT v = pDoc->GetLoadedValue(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}
// ── GetGlobalOption / GetGlobalOptionList ─────────────────────────────────────
// Global options are app-wide (not per-world).  The portable CMUSHclientApp
// (doc_stub.h) carries a subset of the Windows App fields; we surface those here
// with the same camel-case names the Windows registry table uses (_stricmp match).
// NOT rewired to the real doc methods (methods_info.cpp → App.GetGlobalOption):
// the stub App has no global-preferences store, so the real path returns
// VT_NULL/empty for everything — these twins keep the richer App-member
// surface until a real global-option table exists headless.
//
// Numeric options (long/bool):
//   AllTypingToCommandWindow, BleedBackground, DisableKeyboardMenuActivation,
//   ErrorNotificationToOutputWindow, F1macro, NotifyOnDisconnect,
//   PrinterFontSize, PrinterLeftMargin, PrinterLinesPerPage, PrinterTopMargin,
//   ReconnectOnLinkFailure, SmoothScrolling, SmootherScrolling
//
// Alpha options (string):
//   DefaultLogFileDirectory, LuaScript, PrinterFont, WordDelimitersDblClick
static int L_GetGlobalOption(lua_State* L)
{
    world_doc(L);
    const char* name = luaL_checkstring(L, 1);

    // ── numeric (bool/int) options ──────────────────────────────────────────
    struct { const char* nm; long val; } nums[] = {
        { "AllTypingToCommandWindow",        App.m_bAllTypingToCommandWindow        },
        { "BleedBackground",                 App.m_bBleedBackground                 },
        { "DisableKeyboardMenuActivation",   App.m_bDisableKeyboardMenuActivation   },
        { "ErrorNotificationToOutputWindow", App.m_bErrorNotificationToOutputWindow },
        { "F1macro",                         App.m_bF1macro                         },
        { "NotifyOnDisconnect",              App.m_bNotifyOnDisconnect              },
        { "PrinterFontSize",                 App.m_nPrinterFontSize                 },
        { "PrinterLeftMargin",               App.m_nPrinterLeftMargin               },
        { "PrinterLinesPerPage",             App.m_nPrinterLinesPerPage             },
        { "PrinterTopMargin",                App.m_nPrinterTopMargin                },
        { "ReconnectOnLinkFailure",          App.m_bReconnectOnLinkFailure          },
        { "SmoothScrolling",                 App.m_bSmoothScrolling                 },
        { "SmootherScrolling",               App.m_bSmootherScrolling               },
        { nullptr, 0 }
    };
    for (int i = 0; nums[i].nm; ++i) {
        if (strcasecmp(name, nums[i].nm) == 0) {
            lua_pushnumber(L, nums[i].val);
            return 1;
        }
    }

    // ── alpha (string) options ──────────────────────────────────────────────
    struct { const char* nm; const char* val; } strs[] = {
        { "DefaultLogFileDirectory", App.m_strDefaultLogFileDirectory.c_str() },
        { "LuaScript",               static_cast<const char*>(App.m_strLuaScript) },
        { "PrinterFont",             static_cast<const char*>(App.m_strPrinterFont) },
        { "WordDelimitersDblClick",  static_cast<const char*>(App.m_strWordDelimitersDblClick) },
        { nullptr, nullptr }
    };
    for (int i = 0; strs[i].nm; ++i) {
        if (strcasecmp(name, strs[i].nm) == 0) {
            lua_pushstring(L, strs[i].val ? strs[i].val : "");
            return 1;
        }
    }

    lua_pushnil(L);   // unknown option
    return 1;
}

static int L_GetGlobalOptionList(lua_State* L)
{
    world_doc(L);
    static const char* const kNames[] = {
        // numeric
        "AllTypingToCommandWindow",
        "BleedBackground",
        "DisableKeyboardMenuActivation",
        "ErrorNotificationToOutputWindow",
        "F1macro",
        "NotifyOnDisconnect",
        "PrinterFontSize",
        "PrinterLeftMargin",
        "PrinterLinesPerPage",
        "PrinterTopMargin",
        "ReconnectOnLinkFailure",
        "SmoothScrolling",
        "SmootherScrolling",
        // alpha
        "DefaultLogFileDirectory",
        "LuaScript",
        "PrinterFont",
        "WordDelimitersDblClick",
        nullptr
    };
    lua_newtable(L);
    for (int i = 0; kNames[i]; ++i) {
        lua_pushstring(L, kNames[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ── GetWorld / GetWorldById / GetWorldIdList / GetWorldList ───────────────────
// These mirror the Windows implementations in lua_methods.cpp: search all open
// worlds in App.m_pWorldDocTemplate and return a mushclient.world userdata so
// that OOP-style calls like GetWorld("name"):Note("hello") work correctly.
static int L_GetWorld(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
        CMUSHclientDoc* pDoc = (CMUSHclientDoc*)App.m_pWorldDocTemplate->GetNextDoc(pos);
        if (pDoc->m_mush_name.CompareNoCase(name) == 0) {
            push_world_userdata(L, pDoc);
            return 1;
        }
    }
    return 0;
}

static int L_GetWorldById(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
        CMUSHclientDoc* pDoc = (CMUSHclientDoc*)App.m_pWorldDocTemplate->GetNextDoc(pos);
        if (pDoc->m_strWorldID.CompareNoCase(id) == 0) {
            push_world_userdata(L, pDoc);
            return 1;
        }
    }
    return 0;
}

static int L_GetWorldList(lua_State* L)
{
    lua_newtable(L);
    int i = 1;
    for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
        CMUSHclientDoc* pDoc = (CMUSHclientDoc*)App.m_pWorldDocTemplate->GetNextDoc(pos);
        lua_pushstring(L, static_cast<const char*>(pDoc->m_mush_name));
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int L_GetWorldIdList(lua_State* L)
{
    lua_newtable(L);
    int i = 1;
    for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; ) {
        CMUSHclientDoc* pDoc = (CMUSHclientDoc*)App.m_pWorldDocTemplate->GetNextDoc(pos);
        lua_pushstring(L, static_cast<const char*>(pDoc->m_strWorldID));
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// ── Debug ─────────────────────────────────────────────────────────────────────
// Executes an internal debug command (world.Debug).  In the portable build we
// treat this as a note to stderr; the string result is the empty string.
static int L_Debug(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* cmd = luaL_optstring(L, 1, "");
    pDoc->Debug(cmd ? cmd : "");
    lua_pushstring(L, "");
    return 1;
}

// ── GenerateName ─────────────────────────────────────────────────────────────
// Generates a random MUD-style name by reading a names file.  In the portable
// build the names file infrastructure is absent; return empty string.
static int L_GenerateName(lua_State* L)
{
    // Real CMUSHclientDoc::GenerateName: VT_NULL → nil while no names file
    // has been read — exactly Windows before ReadNamesFile succeeds (and
    // headless it never does).  The old twin returned "".
    VARIANT v = world_doc(L)->GenerateName();
    return push_option_variant(L, v);
}

// ── ExpandVariables (string expansion) ───────────────────────────────────────
// Expands @variable references in a string using the world variable map.
// world.ExpandVariables("@foo bar @baz") → with @foo="hello", @baz="world"
//   returns "hello bar world"
static int L_ExpandVariablesStr(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* raw = luaL_checkstring(L, 1);
    CString result(raw);

    // Walk the variable map; replace each @varname occurrence
    POSITION pos = pDoc->GetVariableMap().GetStartPosition();
    while (pos) {
        CString key;
        CVariable* pVar;
        pDoc->GetVariableMap().GetNextAssoc(pos, key, pVar);
        if (!pVar) continue;
        CString token("@");
        token += key;
        result.Replace(token, pVar->strContents);
    }
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

// ── GetInternalCommandsList ───────────────────────────────────────────────────
// The REAL method (methods_commands.cpp) over the real CommandIDs[] table
// (accelerators.cpp) — the full 1-based list of internal command names,
// marshalled through push_option_variant exactly as lua_methods.cpp.
static int L_GetInternalCommandsList(lua_State* L)
{
    VARIANT v = world_doc(L)->GetInternalCommandsList();
    return push_option_variant(L, v);
}

// ── ArrayList ─────────────────────────────────────────────────────────────────
// Returns a key→value table for all entries in a named array.
// This is the combined form of ArrayListKeys / ArrayListValues.
static int L_ArrayList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end())
        return 0;  // not found — no result

    lua_newtable(L);
    for (auto& kv : *it->second) {
        lua_pushstring(L, kv.first.c_str());
        lua_pushstring(L, kv.second.c_str());
        lua_rawset(L, -3);
    }
    return 1;
}

// ── TranslateGerman ───────────────────────────────────────────────────────────
// Converts German Umlaut characters (ISO-8859-1) to ASCII digraph equivalents.
// ü→ue, Ü→Ue, ä→ae, Ä→Ae, ö→oe, Ö→Oe, ß→ss
static int L_TranslateGerman(lua_State* L)
{
    BSTR str = world_doc(L)->TranslateGerman(luaL_checkstring(L, 1));  // real
    lua_pushstring(L, str ? str : "");
    delete[] str;
    return 1;
}

// ── Open ─────────────────────────────────────────────────────────────────────
// Opens a world from a file.  Requires an App/document-template that doesn't
// exist in the portable build; always returns false.
static int L_Open(lua_State* L)
{
    // lua_methods.cpp L_Open shape: App.OpenDocumentFile + boolean result.
    // The stub App's OpenDocumentFile always returns NULL headless (no doc
    // template), so this pushes false exactly as before — but through the
    // same call path as the full build.
    world_doc(L);
    lua_pushboolean(L, App.OpenDocumentFile(luaL_checkstring(L, 1)) != NULL);
    return 1;
}

// ── GetSelectionStart/EndLine/Column ─────────────────────────────────────────
// Real methods_info.cpp methods (each reads the IWorldView outputSelection
// seam; zeros with no view attached).
static int L_GetSelectionStartLine(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetSelectionStartLine());
    return 1;
}
static int L_GetSelectionEndLine(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetSelectionEndLine());
    return 1;
}
static int L_GetSelectionStartColumn(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetSelectionStartColumn());
    return 1;
}
static int L_GetSelectionEndColumn(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->GetSelectionEndColumn());
    return 1;
}

// ── Bookmark ──────────────────────────────────────────────────────────────────
// world.Bookmark(lineNumber [, set=true])
// Sets or clears the BOOKMARK flag on a line in the output buffer.
// lineNumber is 1-based.  If set is false the bookmark is removed.
// Visual bookmark indicators require a GUI view; here we just update the flag.
static int L_Bookmark(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long lineNum = (long)luaL_checknumber(L, 1);
    bool set = (lua_gettop(L) < 2) || lua_toboolean(L, 2);

    long count = pDoc->m_LineList.GetCount();
    if (lineNum <= 0 || lineNum > count)
        return 0;

    CLine* pLine = GetLineAt(pDoc, lineNum - 1);
    if (!pLine) return 0;

    if (set)
        pLine->flags |= BOOKMARK;
    else
        pLine->flags &= ~BOOKMARK;

    return 0;
}

// ── BlendPixel ────────────────────────────────────────────────────────────────
// Pure colour-math function — no GUI required.  Ported directly from
// CMUSHclientDoc::BlendPixel in scripting/methods/methods_miniwindows.cpp.
// Uses the same blending.h macros and color.h CColor class.

#include "blending.h"   // Blend_It, Colour_Op, Blend_Normal, …
#include "color.h"      // CColor (HSL conversion) for modes 60-64

#define Blend_It_Local(Op) \
   do { \
     if (Opacity < 1.0) { \
       r = Blend_Opacity(rA, rB, Op, Opacity); \
       g = Blend_Opacity(gA, gB, Op, Opacity); \
       b = Blend_Opacity(bA, bB, Op, Opacity); \
     } else { \
       r = Op(rA, rB); \
       g = Op(gA, gB); \
       b = Op(bA, bB); \
     } \
   } while (false)

#define Colour_Op_Local(fR,fG,fB) \
   do { \
     if (Opacity < 1.0) { \
       r = Simple_Opacity(rB, fR, Opacity); \
       g = Simple_Opacity(gB, fG, Opacity); \
       b = Simple_Opacity(bB, fB, Opacity); \
     } else { r = (fR); g = (fG); b = (fB); } \
   } while (false)

static int L_BlendPixel(lua_State* L)
{
    world_doc(L);
    long Blend   = (long)luaL_checknumber(L, 1);
    long Base    = (long)luaL_checknumber(L, 2);
    short Mode   = (short)luaL_checkinteger(L, 3);
    double Opacity = luaL_optnumber(L, 4, 1.0);

    if (Opacity < 0.0 || Opacity > 1.0) { lua_pushnumber(L, -2); return 1; }

    long rA = GetRValue(Blend), gA = GetGValue(Blend), bA = GetBValue(Blend);
    long rB = GetRValue(Base),  gB = GetGValue(Base),  bB = GetBValue(Base);
    long r, g, b;

    // cos table for Interpolate mode
    static unsigned char cos_table[256];
    static bool bTableComputed = false;
    if (!bTableComputed) {
        double pi_div255 = 3.1415926535898 / 255.0;
        for (int i = 0; i < 256; i++) {
            double a = 64.0 - cos((double)i * pi_div255) * 64.0;
            cos_table[i] = (uint8)(a + 0.5);
        }
        bTableComputed = true;
    }

    switch (Mode) {
    case  1: Blend_It_Local(Blend_Normal);    break;
    case  2: Blend_It_Local(Blend_Average);   break;
    case  3: Blend_It_Local(Blend_Interpolate); break;
    case  4: { double rnd = genrand();
               r = (rnd < Opacity) ? rA : rB;
               g = (rnd < Opacity) ? gA : gB;
               b = (rnd < Opacity) ? bA : bB; } break;
    case  5: Blend_It_Local(Blend_Darken);    break;
    case  6: Blend_It_Local(Blend_Multiply);  break;
    case  7: Blend_It_Local(Blend_ColorBurn); break;
    case  8: Blend_It_Local(Blend_LinearBurn);break;
    case  9: Blend_It_Local(Blend_InverseColorBurn); break;
    case 10: Blend_It_Local(Blend_Subtract);  break;
    case 11: Blend_It_Local(Blend_Lighten);   break;
    case 12: Blend_It_Local(Blend_Screen);    break;
    case 13: Blend_It_Local(Blend_ColorDodge);break;
    case 14: Blend_It_Local(Blend_LinearDodge);break;
    case 15: Blend_It_Local(Blend_InverseColorDodge); break;
    case 16: Blend_It_Local(Blend_Add);       break;
    case 17: Blend_It_Local(Blend_Overlay);   break;
    case 18: Blend_It_Local(Blend_SoftLight); break;
    case 19: Blend_It_Local(Blend_HardLight); break;
    case 20: Blend_It_Local(Blend_VividLight);break;
    case 21: Blend_It_Local(Blend_LinearLight);break;
    case 22: Blend_It_Local(Blend_PinLight);  break;
    case 23: Blend_It_Local(Blend_HardMix);   break;
    case 24: Blend_It_Local(Blend_Difference);break;
    case 25: Blend_It_Local(Blend_Exclusion); break;
    case 26: Blend_It_Local(Blend_Reflect);   break;
    case 27: Blend_It_Local(Blend_Glow);      break;
    case 28: Blend_It_Local(Blend_Freeze);    break;
    case 29: Blend_It_Local(Blend_Heat);      break;
    case 30: Blend_It_Local(Blend_Negation);  break;
    case 31: Blend_It_Local(Blend_Phoenix);   break;
    case 32: Blend_It_Local(Blend_Stamp);     break;
    case 33: Blend_It_Local(Blend_Xor);       break;
    case 34: Blend_It_Local(Blend_And);       break;
    case 35: Blend_It_Local(Blend_Or);        break;
    case 36: Colour_Op_Local(rA, gB, bB); break;
    case 37: Colour_Op_Local(rB, gA, bB); break;
    case 38: Colour_Op_Local(rB, gB, bA); break;
    case 39: Colour_Op_Local(rA, gA, bB); break;
    case 40: Colour_Op_Local(rB, gA, bA); break;
    case 41: Colour_Op_Local(rA, gB, bA); break;
    case 42: Colour_Op_Local(rA, (gA>rA)?rA:gA, bA); break;
    case 43: Colour_Op_Local(rA, (gA>bA)?bA:gA, bA); break;
    case 44: Colour_Op_Local(rA, (gA>((rA+bA)/2))?((rA+bA)/2):gA, bA); break;
    case 45: Colour_Op_Local(rA, gA, (bA>rA)?rA:bA); break;
    case 46: Colour_Op_Local(rA, gA, (bA>gA)?gA:bA); break;
    case 47: Colour_Op_Local(rA, gA, (bA>((rA+gA)/2))?((rA+gA)/2):bA); break;
    case 48: Colour_Op_Local((rA>gA)?gA:rA, gA, bA); break;
    case 49: Colour_Op_Local((rA>bA)?bA:rA, gA, bA); break;
    case 50: Colour_Op_Local((rA>((gA+bA)/2))?((gA+bA)/2):rA, gA, bA); break;
    case 51: Colour_Op_Local(rA, 0, 0);  break;
    case 52: Colour_Op_Local(0, gA, 0);  break;
    case 53: Colour_Op_Local(0, 0, bA);  break;
    case 54: Colour_Op_Local(0, gA, bA); break;
    case 55: Colour_Op_Local(rA, 0, bA); break;
    case 56: Colour_Op_Local(rA, gA, 0); break;
    case 57: Colour_Op_Local(rA, rA, rA); break;
    case 58: Colour_Op_Local(gA, gA, gA); break;
    case 59: Colour_Op_Local(bA, bA, bA); break;
    case 60: { CColor cA, cB; cA.SetRGB(rA,gA,bA); cB.SetRGB(rB,gB,bB);
               cB.SetHue(cA.GetHue());
               r=Simple_Opacity(rB,cB.GetRed(),Opacity); g=Simple_Opacity(gB,cB.GetGreen(),Opacity); b=Simple_Opacity(bB,cB.GetBlue(),Opacity); } break;
    case 61: { CColor cA, cB; cA.SetRGB(rA,gA,bA); cB.SetRGB(rB,gB,bB);
               cB.SetSaturation(cA.GetSaturation());
               r=Simple_Opacity(rB,cB.GetRed(),Opacity); g=Simple_Opacity(gB,cB.GetGreen(),Opacity); b=Simple_Opacity(bB,cB.GetBlue(),Opacity); } break;
    case 62: { CColor cA, cB; cA.SetRGB(rA,gA,bA); cB.SetRGB(rB,gB,bB);
               cB.SetHue(cA.GetHue()); cB.SetSaturation(cA.GetSaturation());
               r=Simple_Opacity(rB,cB.GetRed(),Opacity); g=Simple_Opacity(gB,cB.GetGreen(),Opacity); b=Simple_Opacity(bB,cB.GetBlue(),Opacity); } break;
    case 63: { CColor cA, cB; cA.SetRGB(rA,gA,bA); cB.SetRGB(rB,gB,bB);
               cB.SetLuminance(cA.GetLuminance());
               r=Simple_Opacity(rB,cB.GetRed(),Opacity); g=Simple_Opacity(gB,cB.GetGreen(),Opacity); b=Simple_Opacity(bB,cB.GetBlue(),Opacity); } break;
    case 64: { CColor cA; cA.SetRGB(rA,gA,bA);
               r=Simple_Opacity(rB,cA.GetHue(),Opacity); g=Simple_Opacity(gB,cA.GetSaturation(),Opacity); b=Simple_Opacity(bB,cA.GetLuminance(),Opacity); } break;
    default: lua_pushnumber(L, -1); return 1;
    }

    lua_pushnumber(L, RGB(CLAMP(r), CLAMP(g), CLAMP(b)));
    return 1;
}

// ── Accelerator / AcceleratorTo / AcceleratorList ─────────────────────────────
// On non-Windows, keyboard accelerators (custom keystroke bindings) cannot be
// wired to the Win32 HACCEL / accelerator-table mechanism.  Scripts that call
// these functions receive a graceful error code rather than a crash.

static int L_Accelerator(lua_State* L)    { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_AcceleratorTo(lua_State* L)  { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_AcceleratorList(lua_State* L){ world_doc(L); lua_newtable(L); return 1; }

// ── Miniwindow Lua API (mushcore test-engine stubs) ───────────────────────────────
// lua_world_api_portable.cpp is compiled into mushcore only.  The mushgui build
// uses mushapp which compiles lua_methods.cpp instead — that file already has
// the full real Window* implementations backed by wx_gdi_compat CDC/CBitmap.
//
// The stubs below keep the headless build functional: scripts that call Window* get a
// graceful eNoSuchWindow error rather than a crash.
//
// NOTE: The pushVariant helper below is unused by mushcore (no Window* methods),
// but is kept here as documentation for how the full-build variants work.

// ── Not-available stubs (GUI / Chat / Notepad subsystems) ────────────────────
// These functions exist in the Windows build but require GUI objects that are
// absent in the portable build.  Rather than letting scripts crash with
// "attempt to call a nil value (field 'WindowCreate')", we return a meaningful
// error code so the caller can handle the failure gracefully.


// miniwindow operations → eNoSuchWindow
static int L_WindowStub(lua_State* L)
{
    world_doc(L);  // validate world pointer
    lua_pushnumber(L, eNoSuchWindow);
    return 1;
}

// miniwindow data queries (list, info) → nil/empty table so ipairs/pairs works
static int L_WindowListStub(lua_State* L)
{
    world_doc(L);
    lua_newtable(L);
    return 1;
}


// Chat subsystem → eNoChatConnections
static int L_ChatStub(lua_State* L)
{
    world_doc(L);
    lua_pushnumber(L, eNoChatConnections);
    return 1;
}

// Chat data queries → nil
static int L_ChatDataStub(lua_State* L)
{
    world_doc(L);
    return 0;
}

// Spell-check → eSpellCheckNotActive
// ── Spell check ── the REAL methods_spellchecker.cpp methods (adopted into
// mushcore), marshalled exactly as lua_methods.cpp.  Headless the Lua
// spell-checker is never initialised (App.m_bSpellCheckOK stays false), so:
// SpellCheck/SpellCheckDlg → empty VARIANT → nil (lua_methods.cpp:5675-5680 +
// pushVariant default), SpellCheckCommand → -1, AddSpellCheckWord →
// eSpellCheckNotActive — the same results a Windows build with spell check
// disabled produces.  (The old L_SpellStub returned eSpellCheckNotActive for
// all four — twin-only behaviour.)
static int L_SpellCheck(lua_State* L)
{
    VARIANT v = world_doc(L)->SpellCheck(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}
static int L_SpellCheckDlg(lua_State* L)
{
    VARIANT v = world_doc(L)->SpellCheckDlg(luaL_checkstring(L, 1));
    return push_option_variant(L, v);
}
static int L_SpellCheckCommand(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->SpellCheckCommand(
        (long)luaL_optnumber(L, 1, -1),    // Start column
        (long)luaL_optnumber(L, 2, -1))); // End column
    return 1;
}
static int L_AddSpellCheckWord(lua_State* L)
{
    lua_pushnumber(L, world_doc(L)->AddSpellCheckWord(
        luaL_checkstring(L, 1),        // Original word
        luaL_checkstring(L, 2),        // Action
        luaL_optstring(L, 3, "")));    // Replacement word - optional
    return 1;
}

// ── Notepad API ──────────────────────────────────────────────────────────────
// AppendToNotepad(title, text) → bool
// Creates or appends to a named notepad window.
static int L_AppendToNotepad(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    const char* text  = luaL_optstring(L, 2, "");
    bool ok = pDoc->AppendToTheNotepad(CString(title), CString(text),
                                        false, eNotepadNormal);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ReplaceNotepad(title, text) → bool
// Creates or replaces the content of a named notepad window.
static int L_ReplaceNotepad(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    const char* text  = luaL_optstring(L, 2, "");
    bool ok = pDoc->AppendToTheNotepad(CString(title), CString(text),
                                        true, eNotepadNormal);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// GetNotepadText(title) → string or nil
static int L_GetNotepadText(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    if (!g_wxhook_get_notepad_text) { lua_pushnil(L); return 1; }
    char* text = g_wxhook_get_notepad_text(title);
    if (!text) { lua_pushnil(L); return 1; }
    lua_pushstring(L, text);
    free(text);
    return 1;
}

// GetNotepadLength(title) → number (0 if not found)
static int L_GetNotepadLength(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    if (!g_wxhook_get_notepad_text) { lua_pushnumber(L, 0); return 1; }
    char* text = g_wxhook_get_notepad_text(title);
    long len = text ? (long)strlen(text) : 0;
    free(text);
    lua_pushnumber(L, (lua_Number)len);
    return 1;
}

// GetNotepadList() → table of title strings
static int L_GetNotepadList(lua_State* L)
{
    world_doc(L);
    lua_newtable(L);
    if (!g_wxhook_get_notepad_list) return 1;
    char** list = g_wxhook_get_notepad_list();
    if (!list) return 1;
    for (int i = 0; list[i]; ++i) {
        lua_pushstring(L, list[i]);
        lua_rawseti(L, -2, i + 1);
        free(list[i]);
    }
    free(list);
    return 1;
}

// ── More notepad API ─────────────────────────────────────────────────────────

// ActivateNotepad(title) → eOK or eNoSuchWindow
static int L_ActivateNotepad(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    bool found = g_wxhook_activate_notepad && g_wxhook_activate_notepad(title);
    lua_pushnumber(L, found ? eOK : eNoSuchWindow);
    return 1;
}

// CloseNotepad(title) → eOK or eNoSuchWindow
static int L_CloseNotepad(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    bool found = g_wxhook_close_notepad && g_wxhook_close_notepad(title);
    lua_pushnumber(L, found ? eOK : eNoSuchWindow);
    return 1;
}

// MoveNotepadWindow(title, x, y, width, height) → eOK or eNoSuchWindow
static int L_MoveNotepadWindow(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    int x = (int)luaL_optnumber(L, 2, -1);
    int y = (int)luaL_optnumber(L, 3, -1);
    int w = (int)luaL_optnumber(L, 4, -1);
    int h = (int)luaL_optnumber(L, 5, -1);
    bool found = g_wxhook_move_notepad && g_wxhook_move_notepad(title, x, y, w, h);
    lua_pushnumber(L, found ? eOK : eNoSuchWindow);
    return 1;
}

// GetNotepadWindowPosition(title) → "x y width height" string or nil
static int L_GetNotepadWindowPosition(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    if (!g_wxhook_get_notepad_position) { lua_pushnil(L); return 1; }
    char* pos = g_wxhook_get_notepad_position(title);
    if (!pos) { lua_pushnil(L); return 1; }
    lua_pushstring(L, pos);
    free(pos);
    return 1;
}

// SendToNotepad(title, text) → bool
// Creates a new notepad window with the given text (or appends to existing).
// Uses AppendToTheNotepad (append=false → creates if absent, appends if present).
static int L_SendToNotepad(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    const char* text  = luaL_optstring(L, 2, "");
    // SendToNotepad always creates a fresh window on Windows, but portable
    // behaviour (append-or-create) is equally useful and avoids a separate hook.
    bool ok = pDoc->AppendToTheNotepad(CString(title), CString(text),
                                        false, eNotepadScript);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// SaveNotepad(title, filename, replaceExisting) → 0 (success) or error
static int L_SaveNotepad(lua_State* L)
{
    world_doc(L);
    const char* title    = luaL_checkstring(L, 1);
    const char* filename = luaL_optstring(L, 2, "");
    bool        replace  = lua_toboolean(L, 3) != 0;
    if (!g_wxhook_save_notepad) { lua_pushnumber(L, eNoSuchWindow); return 1; }
    bool ok = g_wxhook_save_notepad(title, filename, replace);
    lua_pushnumber(L, ok ? 0 : eNoSuchWindow);
    return 1;
}

// NotepadColour(title, textColour, backColour) → 0 on success
// Colour strings: "#RRGGBB" or a named colour from the MUSHclient colour table.
// Forward-declare SetColour from mxp/mxputils.cpp (available in mushcore).
bool SetColour(const CString& strName, COLORREF& iColour);

static int L_NotepadColour(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    const char* fgStr = luaL_checkstring(L, 2);
    const char* bgStr = luaL_checkstring(L, 3);
    if (!g_wxhook_notepad_colour) { lua_pushnumber(L, eNoSuchWindow); return 1; }
    COLORREF fg = 0, bg = 0;
    if (SetColour(CString(fgStr), fg)) { lua_pushnumber(L, false); return 1; }
    if (SetColour(CString(bgStr), bg)) { lua_pushnumber(L, false); return 1; }
    bool ok = g_wxhook_notepad_colour(title, (unsigned long)fg, (unsigned long)bg);
    lua_pushnumber(L, ok ? 0 : eNoSuchWindow);
    return 1;
}

// NotepadFont(title, fontName, size, style [, charset]) → 0 on success
// style bits: 1=bold, 2=italic, 4=underline, 8=strikeout.
// size=0 keeps current size; empty fontName keeps current face.
static int L_NotepadFont(lua_State* L)
{
    world_doc(L);
    const char* title    = luaL_checkstring(L, 1);
    const char* fontName = luaL_checkstring(L, 2);
    int         size     = (int)luaL_checknumber(L, 3);
    int         style    = (int)luaL_checknumber(L, 4);
    int         charset  = (int)luaL_optnumber(L, 5, 0);
    if (!g_wxhook_notepad_font) { lua_pushnumber(L, eNoSuchWindow); return 1; }
    bool ok = g_wxhook_notepad_font(title, fontName, size, style, charset);
    lua_pushnumber(L, ok ? 0 : eNoSuchWindow);
    return 1;
}

// NotepadReadOnly(title [, readOnly=true]) → 0 on success
static int L_NotepadReadOnly(lua_State* L)
{
    world_doc(L);
    const char* title = luaL_checkstring(L, 1);
    bool ro = (lua_isnone(L, 2) || lua_isnil(L, 2)) ? true : lua_toboolean(L, 2) != 0;
    if (!g_wxhook_notepad_readonly) { lua_pushnumber(L, eNoSuchWindow); return 1; }
    bool ok = g_wxhook_notepad_readonly(title, ro);
    lua_pushnumber(L, ok ? 0 : eNoSuchWindow);
    return 1;
}

// Notepad / mapping / GUI display actions that have no return value → eOK
static int L_GuiOkStub(lua_State* L)
{
    world_doc(L);
    lua_pushnumber(L, eOK);
    return 1;
}

// Notepad / mapping data queries → nil
static int L_GuiNilStub(lua_State* L)
{
    world_doc(L);
    return 0;
}

// ── Registration table ────────────────────────────────────────────────────────

static const luaL_Reg worldlib[] = {
    // Output
    { "Note",               L_Note           },
    { "Tell",               L_Tell           },
    { "AnsiNote",           L_AnsiNote       },
    { "ColourNote",         L_ColourNote     },
    { "ColourTell",         L_ColourTell     },
    // Variables
    { "SetVariable",        L_SetVariable    },
    { "GetVariable",        L_GetVariable    },
    { "DeleteVariable",     L_DeleteVariable },
    { "GetVariableList",    L_GetVariableList},
    // Sending
    { "Send",               L_Send           },
    { "Execute",            L_Execute        },
    // Triggers
    { "IsTrigger",          L_IsTrigger      },
    { "AddTrigger",         L_AddTrigger     },
    { "AddTriggerEx",       L_AddTriggerEx   },
    { "DeleteTrigger",      L_DeleteTrigger  },
    { "EnableTrigger",      L_EnableTrigger  },
    // Aliases
    { "IsAlias",            L_IsAlias        },
    { "AddAlias",           L_AddAlias       },
    { "DeleteAlias",        L_DeleteAlias    },
    { "EnableAlias",        L_EnableAlias    },
    // Timers
    { "IsTimer",            L_IsTimer        },
    { "AddTimer",           L_AddTimer       },
    { "DeleteTimer",        L_DeleteTimer    },
    { "EnableTimer",        L_EnableTimer    },
    { "ResetTimer",         L_ResetTimer     },
    // Simulation / pipeline
    { "Simulate",           L_Simulate       },
    { "StopEvaluatingTriggers", L_StopEvaluatingTriggers },
    // World info
    { "GetWorldName",       L_GetWorldName   },
    { "WorldName",          L_WorldName      },
    { "WorldAddress",       L_WorldAddress   },
    { "WorldPort",          L_WorldPort      },
    { "IsConnected",        L_IsConnected    },
    { "Version",            L_Version        },
    // List accessors
    { "GetTriggerList",     L_GetTriggerList    },
    { "GetAliasList",       L_GetAliasList      },
    { "GetTimerList",       L_GetTimerList      },
    { "GetVariableList",    L_GetVariableList   },
    { "GetVariableNameList",L_GetVariableNameList},
    // Property readers (summary)
    { "GetTrigger",         L_GetTrigger        },
    { "GetAlias",           L_GetAlias          },
    { "GetTimer",           L_GetTimer          },
    // Property readers (by InfoType)
    { "GetTriggerInfo",     L_GetTriggerInfo    },
    { "GetAliasInfo",       L_GetAliasInfo      },
    { "GetTimerInfo",       L_GetTimerInfo      },
    // Group operations
    { "EnableTriggerGroup", L_EnableTriggerGroup},
    { "EnableAliasGroup",   L_EnableAliasGroup  },
    { "EnableTimerGroup",   L_EnableTimerGroup  },
    { "EnableGroup",        L_EnableGroup       },
    // World info
    { "GetInfo",            L_GetInfo           },
    { "GetOption",          L_GetOption         },
    { "SetOption",          L_SetOption         },
    { "GetLineCount",       L_GetLineCount      },
    { "GetUniqueNumber",    L_GetUniqueNumber   },
    // Connection control
    { "Connect",            L_ConnectWorld      },
    { "Disconnect",         L_DisconnectWorld   },
    // World identity
    { "GetWorldID",         L_GetWorldID        },
    // Status bar
    { "SetStatus",          L_SetStatus         },
    // Colour note helpers
    { "NoteColourRGB",      L_NoteColourRGB     },
    { "NoteColourFore",     L_NoteColourFore    },
    { "NoteColourBack",     L_NoteColourBack    },
    { "NormalColour",       L_NormalColour      },
    // Line buffer
    { "GetLinesInBufferCount", L_GetLinesInBufferCount },
    { "GetLineInfo",        L_GetLineInfo       },
    { "Bookmark",           L_Bookmark          },
    { "GetCurrentValue",    L_GetCurrentValue   },
    // Speedwalk
    { "EvaluateSpeedwalk",  L_EvaluateSpeedwalk },
    // Hashing
    { "Hash",               L_Hash              },
    { "MD5",                L_MD5               },
    { "SHA256",             L_SHA256            },
    // Plugin helpers
    { "GetPluginName",      L_GetPluginName     },
    { "IsPluginInstalled",  L_IsPluginInstalled },
    { "EnablePlugin",       L_EnablePlugin      },
    // Delayed execution
    { "DoAfterSpecial",     L_DoAfterSpecial    },
    { "DoAfter",            L_DoAfter           },
    { "DoAfterNote",        L_DoAfterNote       },
    // Plugin management
    { "LoadPlugin",         L_LoadPlugin        },
    { "IsPlugin",           L_IsPlugin          },
    { "GetPluginList",      L_GetPluginList     },
    { "GetPluginInfo",      L_GetPluginInfo     },
    { "SetPluginInfo",      L_SetPluginInfo     },
    { "CallPlugin",         L_CallPlugin        },
    { "BroadcastPlugin",    L_BroadcastPlugin   },
    { "PluginSupports",     L_PluginSupports    },
    { "GetPluginID",        L_GetPluginID       },
    // String-valued options
    { "GetAlphaOption",     L_GetAlphaOption    },
    { "SetAlphaOption",     L_SetAlphaOption    },
    // String utilities
    { "StripANSI",          L_StripANSI      },
    { "Trim",               L_Trim           },
    // Error helpers
    { "ErrorDesc",          L_ErrorDesc      },
    // ANSI / colour utilities
    { "ANSI",               L_ANSI              },
    { "AdjustColour",       L_AdjustColour      },
    { "ColourNameToRGB",    L_ColourNameToRGB   },
    { "RGBColourToName",    L_RGBColourToName   },
    { "NoteColourName",     L_NoteColourName    },
    // Note style / colour
    { "NoteStyle",          L_NoteStyle         },
    { "GetNoteStyle",       L_GetNoteStyleLua   },
    { "GetNoteColourFore",  L_GetNoteColourFore },
    { "GetNoteColourBack",  L_GetNoteColourBack },
    { "SetNoteColourFore",  L_SetNoteColourFore },
    { "SetNoteColourBack",  L_SetNoteColourBack },
    { "NoteHr",             L_NoteHr            },
    // Output buffer
    { "DeleteLines",        L_DeleteLines       },
    { "DeleteOutput",       L_DeleteOutput      },
    { "GetRecentLines",     L_GetRecentLines    },
    // Connection
    { "GetConnectDuration", L_GetConnectDuration},
    { "SendNoEcho",         L_SendNoEcho        },
    { "DiscardQueue",       L_DiscardQueue      },
    // Tracing
    { "GetTrace",           L_GetTrace          },
    { "SetTrace",           L_SetTrace          },
    { "TraceOut",           L_TraceOut          },
    { "Trace",              L_Trace             },
    // Misc world control
    { "Queue",              L_Queue             },
    { "SendPush",           L_SendPush          },
    { "Pause",              L_Pause             },
    { "SetUnseenLines",     L_SetUnseenLines    },
    { "ReverseSpeedwalk",   L_ReverseSpeedwalk  },
    { "GetSpeedWalkDelay",  L_GetSpeedWalkDelay },
    { "SetSpeedWalkDelay",  L_SetSpeedWalkDelay },
    // World notes
    { "GetNotes",           L_GetNotes          },
    { "SetNotes",           L_SetNotes          },
    // Unique IDs
    { "CreateGUID",         L_CreateGUID        },
    { "GetUniqueID",        L_GetUniqueID       },
    // Trigger/alias/timer options
    { "GetTriggerOption",   L_GetTriggerOption  },
    { "SetTriggerOption",   L_SetTriggerOption  },
    { "GetAliasOption",     L_GetAliasOption    },
    { "SetAliasOption",     L_SetAliasOption    },
    { "GetTimerOption",     L_GetTimerOption    },
    { "SetTimerOption",     L_SetTimerOption    },
    // Wildcards
    { "GetTriggerWildcard", L_GetTriggerWildcard},
    { "GetAliasWildcard",   L_GetAliasWildcard  },
    // Delete temporary / groups
    { "DeleteTemporaryTriggers", L_DeleteTemporaryTriggers },
    { "DeleteTemporaryAliases",  L_DeleteTemporaryAliases  },
    { "DeleteTemporaryTimers",   L_DeleteTemporaryTimers   },
    { "DeleteGroup",        L_DeleteGroup       },
    // At-time timer
    { "AtTime",             L_AtTime            },
    // Hyperlink / browser
    { "Hyperlink",          L_Hyperlink         },
    { "OpenBrowser",        L_OpenBrowser       },
    // RNG
    { "MtSrand",            L_MtSrand           },
    { "MtRand",             L_MtRand            },
    // Base64
    { "Base64Encode",       L_Base64Encode      },
    { "Base64Decode",       L_Base64Decode      },
    // Directory
    { "ChangeDir",          L_ChangeDir         },
    // Echo input
    { "GetEchoInput",       L_GetEchoInput      },
    { "SetEchoInput",       L_SetEchoInput      },
    // Note colour (combined setter)
    { "SetNoteColour",      L_SetNoteColour     },
    // Timers reset
    { "ResetTimers",        L_ResetTimers       },
    // Byte counters
    { "GetReceivedBytes",   L_GetReceivedBytes  },
    { "GetSentBytes",       L_GetSentBytes      },
    // Script timing
    { "GetScriptTime",      L_GetScriptTime     },
    // Regexp / string utilities
    { "MakeRegularExpression", L_MakeRegularExpression },
    { "FixupEscapeSequences",  L_FixupEscapeSequences  },
    { "FixupHTML",             L_FixupHTML             },
    // Window titles
    { "SetTitle",           L_SetTitle          },
    { "SetMainTitle",       L_SetMainTitle      },
    // DNS lookup
    { "GetHostName",        L_GetHostName       },
    { "GetHostAddress",     L_GetHostAddress    },
    // Log file
    { "IsLogOpen",          L_IsLogOpen         },
    { "OpenLog",            L_OpenLog           },
    { "CloseLog",           L_CloseLog          },
    { "FlushLog",           L_FlushLog          },
    { "WriteLog",           L_WriteLog          },
    { "LogSend",            L_LogSend           },
    { "GetLogInput",        L_GetLogInput       },
    { "SetLogInput",        L_SetLogInput       },
    { "GetLogNotes",        L_GetLogNotes       },
    { "SetLogNotes",        L_SetLogNotes       },
    { "GetLogOutput",       L_GetLogOutput      },
    { "SetLogOutput",       L_SetLogOutput      },
    // Colour palette
    { "GetNormalColour",          L_GetNormalColour          },
    { "SetNormalColour",          L_SetNormalColour          },
    { "GetBoldColour",            L_GetBoldColour            },
    { "SetBoldColour",            L_SetBoldColour            },
    { "GetCustomColourText",      L_GetCustomColourText      },
    { "SetCustomColourText",      L_SetCustomColourText      },
    { "GetCustomColourBackground",L_GetCustomColourBackground},
    { "SetCustomColourBackground",L_SetCustomColourBackground},
    { "GetCustomColourName",      L_GetCustomColourName      },
    { "SetCustomColourName",      L_SetCustomColourName      },
    // Queue
    { "GetQueue",           L_GetQueue          },
    { "SendImmediate",      L_SendImmediate     },
    // Group deletion
    { "DeleteTriggerGroup", L_DeleteTriggerGroup},
    { "DeleteAliasGroup",   L_DeleteAliasGroup  },
    { "DeleteTimerGroup",   L_DeleteTimerGroup  },
    // Plugin variable/item access
    { "GetPluginVariable",      L_GetPluginVariable     },
    { "GetPluginVariableList",  L_GetPluginVariableList },
    { "GetPluginTriggerList",   L_GetPluginTriggerList  },
    { "GetPluginAliasList",     L_GetPluginAliasList    },
    { "GetPluginTimerList",     L_GetPluginTimerList    },
    { "GetPluginTriggerInfo",   L_GetPluginTriggerInfo  },
    { "GetPluginAliasInfo",     L_GetPluginAliasInfo    },
    { "GetPluginTimerInfo",     L_GetPluginTimerInfo    },
    { "GetPluginTriggerOption", L_GetPluginTriggerOption},
    { "GetPluginAliasOption",   L_GetPluginAliasOption  },
    { "GetPluginTimerOption",   L_GetPluginTimerOption  },
    { "UnloadPlugin",           L_UnloadPlugin          },
    { "ReloadPlugin",           L_ReloadPlugin          },
    // Array subsystem
    { "ArrayCreate",       L_ArrayCreate       },
    { "ArrayDelete",       L_ArrayDelete       },
    { "ArrayExists",       L_ArrayExists       },
    { "ArrayCount",        L_ArrayCount        },
    { "ArraySize",         L_ArraySize         },
    { "ArrayGet",          L_ArrayGet          },
    { "ArraySet",          L_ArraySet          },
    { "ArrayDeleteKey",    L_ArrayDeleteKey    },
    { "ArrayKeyExists",    L_ArrayKeyExists    },
    { "ArrayClear",        L_ArrayClear        },
    { "ArrayListAll",      L_ArrayListAll      },
    { "ArrayListKeys",     L_ArrayListKeys     },
    { "ArrayListValues",   L_ArrayListValues   },
    { "ArrayGetFirstKey",  L_ArrayGetFirstKey  },
    { "ArrayGetLastKey",   L_ArrayGetLastKey   },
    { "ArrayImport",       L_ArrayImport       },
    { "ArrayExport",       L_ArrayExport       },
    { "ArrayExportKeys",   L_ArrayExportKeys   },
    // Database subsystem
    { "DatabaseOpen",              L_DatabaseOpen              },
    { "DatabaseClose",             L_DatabaseClose             },
    { "DatabasePrepare",           L_DatabasePrepare           },
    { "DatabaseFinalize",          L_DatabaseFinalize          },
    { "DatabaseStep",              L_DatabaseStep              },
    { "DatabaseReset",             L_DatabaseReset             },
    { "DatabaseExec",              L_DatabaseExec              },
    { "DatabaseColumns",           L_DatabaseColumns           },
    { "DatabaseColumnName",        L_DatabaseColumnName        },
    { "DatabaseColumnText",        L_DatabaseColumnText        },
    { "DatabaseColumnValue",       L_DatabaseColumnValue       },
    { "DatabaseColumnType",        L_DatabaseColumnType        },
    { "DatabaseColumnNames",       L_DatabaseColumnNames       },
    { "DatabaseColumnValues",      L_DatabaseColumnValues      },
    { "DatabaseError",             L_DatabaseError             },
    { "DatabaseChanges",           L_DatabaseChanges           },
    { "DatabaseTotalChanges",      L_DatabaseTotalChanges      },
    { "DatabaseLastInsertRowid",   L_DatabaseLastInsertRowid   },
    { "DatabaseList",              L_DatabaseList              },
    { "DatabaseInfo",              L_DatabaseInfo              },
    { "DatabaseGetField",          L_DatabaseGetField          },
    // String utilities
    { "Replace",            L_Replace           },
    { "EditDistance",       L_EditDistance      },
    { "Metaphone",          L_Metaphone         },
    // Entity map
    { "GetEntity",          L_GetEntity         },
    { "SetEntity",          L_SetEntity         },
    { "GetXMLEntity",       L_GetXMLEntity      },
    // XML import/export stubs
    { "ExportXML",          L_ExportXML         },
    { "ImportXML",          L_ImportXML         },
    // Send variants
    { "SendSpecial",        L_SendSpecial       },
    { "SendPkt",            L_SendPkt           },
    { "DoCommand",          L_DoCommand         },
    { "Reset",              L_Reset             },
    // Sound stubs
    { "Sound",              L_Sound             },
    { "StopSound",          L_StopSound         },
    { "GetSoundStatus",     L_GetSoundStatus    },
    { "PlaySound",          L_PlaySound         },
    // UDP
    { "UdpSend",            L_UdpSend           },
    { "UdpListen",          L_UdpListen         },
    { "GetUdpPort",         L_GetUdpPort        },
    // Misc
    { "TranslateDebug",     L_TranslateDebug    },
    { "GetNoteColour",      L_GetNoteColour     },
    // Command history
    { "GetCommand",         L_GetCommand        },
    { "GetCommandList",     L_GetCommandListFunc},
    { "AddToCommandHistory",L_AddToCommandHistory},
    { "DeleteCommandHistory",L_DeleteCommandHistory},
    { "PushCommand",        L_PushCommand       },
    { "PasteCommand",       L_PasteCommand      },
    { "SelectCommand",      L_SelectCommand     },
    { "SetCommand",         L_SetCommand        },
    // InfoBar stubs
    { "Info",               L_Info              },
    { "InfoBackground",     L_InfoBackground    },
    { "InfoClear",          L_InfoClear         },
    { "InfoColour",         L_InfoColour        },
    { "InfoFont",           L_InfoFont          },
    // GUI stubs
    { "Activate",           L_Activate          },
    { "ActivateClient",     L_ActivateClient    },
    { "Redraw",             L_Redraw            },
    { "Repaint",            L_Repaint           },
    { "GetSysColor",        L_GetSysColor       },
    { "GetSystemMetrics",   L_GetSystemMetrics  },
    { "GetDeviceCaps",      L_GetDeviceCaps     },
    { "GetClipboard",       L_GetClipboard      },
    { "SetClipboard",       L_SetClipboard      },
    { "SetBackgroundColour",L_SetBackgroundColour},
    { "SetBackgroundImage", L_SetBackgroundImage},
    { "SetCursor",          L_SetCursor         },
    { "SetChanged",         L_SetChanged        },
    { "GetMainWindowPosition",   L_GetMainWindowPosition   },
    { "GetWorldWindowPosition",  L_GetWorldWindowPosition  },
    { "GetWorldWindowPositionX", L_GetWorldWindowPositionX },
    { "MoveMainWindow",     L_MoveMainWindow    },
    { "MoveWorldWindow",    L_MoveWorldWindow   },
    { "MoveWorldWindowX",   L_MoveWorldWindowX  },
    { "FlashIcon",          L_FlashIcon         },
    { "Help",               L_Help              },
    { "Menu",               L_Menu              },
    { "PickColour",         L_PickColour        },
    { "Save",               L_Save              },
    { "SetWorldWindowStatus",L_SetWorldWindowStatus},
    // Mapper stubs
    { "AddMapperComment",   L_AddMapperComment  },
    { "AddToMapper",        L_AddToMapper       },
    { "DeleteAllMapItems",  L_DeleteAllMapItems },
    { "DeleteLastMapItem",  L_DeleteLastMapItem },
    { "EnableMapping",      L_EnableMapping     },
    { "GetMapColour",       L_GetMapColour      },
    { "GetMappingCount",    L_GetMappingCount   },
    { "RemoveBacktracks",   L_RemoveBacktracks  },
    { "DoAfterSpeedWalk",   L_DoAfterSpeedWalk  },
    // Style info
    { "GetStyleInfo",           L_GetStyleInfo          },
    // Option lists
    { "GetOptionList",          L_GetOptionList         },
    { "GetAlphaOptionList",     L_GetAlphaOptionList    },
    { "GetDefaultValue",        L_GetDefaultValue       },
    { "GetLoadedValue",         L_GetLoadedValue        },
    // Global options (stub — no App object in portable build)
    { "GetGlobalOption",        L_GetGlobalOption       },
    { "GetGlobalOptionList",    L_GetGlobalOptionList   },
    // Multi-world (single-world stub)
    { "GetWorld",               L_GetWorld              },
    { "GetWorldById",           L_GetWorldById          },
    { "GetWorldList",           L_GetWorldList          },
    { "GetWorldIdList",         L_GetWorldIdList        },
    // Debug / name generation
    { "Debug",                  L_Debug                 },
    { "GenerateName",           L_GenerateName          },
    // String expansion
    { "ExpandVariables",        L_ExpandVariablesStr    },
    // Internal commands list (stub)
    { "GetInternalCommandsList",L_GetInternalCommandsList},
    // Array combined list
    { "ArrayList",              L_ArrayList             },
    // German umlaut translation
    { "TranslateGerman",        L_TranslateGerman       },
    // World open (stub — no App object)
    { "Open",                   L_Open                  },
    // GUI selection (stub — no view)
    { "GetSelectionStartLine",  L_GetSelectionStartLine  },
    { "GetSelectionEndLine",    L_GetSelectionEndLine    },
    { "GetSelectionStartColumn",L_GetSelectionStartColumn},
    { "GetSelectionEndColumn",  L_GetSelectionEndColumn  },
    // Accelerator stubs (Win32 accelerator tables not available on non-Windows)
    { "Accelerator",        L_Accelerator       },
    { "AcceleratorTo",      L_AcceleratorTo     },
    { "AcceleratorList",    L_AcceleratorList   },
    // BlendPixel — pure colour math, fully portable
    { "BlendPixel",         L_BlendPixel        },
    // Misc stubs
    { "ResetIP",            L_ResetIP           },
    { "ResetStatusTime",    L_ResetStatusTime   },
    { "SaveState",          L_SaveState         },

    // ── miniwindow (Window*) stubs ────────────────────────────────────────────
    // lua_world_api_portable.cpp is only compiled into mushcore (the test engine).
    // mushgui uses mushapp which compiles lua_methods.cpp with real Window* impls.
    // These stubs keep the headless build functional: scripts get eNoSuchWindow gracefully.
    { "WindowCreate",           L_WindowStub },
    { "WindowDelete",           L_WindowStub },
    { "WindowAddHotspot",       L_WindowStub },
    { "WindowArc",              L_WindowStub },
    { "WindowBezier",           L_WindowStub },
    { "WindowBlendImage",       L_WindowStub },
    { "WindowCircleOp",         L_WindowStub },
    { "WindowCreateImage",      L_WindowStub },
    { "WindowDeleteAllHotspots",L_WindowStub },
    { "WindowDeleteHotspot",    L_WindowStub },
    { "WindowDragHandler",      L_WindowStub },
    { "WindowDrawImage",        L_WindowStub },
    { "WindowDrawImageAlpha",   L_WindowStub },
    { "WindowFilter",           L_WindowStub },
    { "WindowFont",             L_WindowStub },
    { "WindowGetImageAlpha",    L_WindowStub },
    { "WindowGetPixel",         L_WindowStub },
    { "WindowGradient",         L_WindowStub },
    { "WindowImageFromWindow",  L_WindowStub },
    { "WindowImageOp",          L_WindowStub },
    { "WindowLine",             L_WindowStub },
    { "WindowLoadImage",        L_WindowStub },
    { "WindowLoadImageMemory",  L_WindowStub },
    { "WindowMenu",             L_WindowStub },
    { "WindowMergeImageAlpha",  L_WindowStub },
    { "WindowMoveHotspot",      L_WindowStub },
    { "WindowPolygon",          L_WindowStub },
    { "WindowPosition",         L_WindowStub },
    { "WindowRectOp",           L_WindowStub },
    { "WindowResize",           L_WindowStub },
    { "WindowScrollwheelHandler",L_WindowStub},
    { "WindowSetPixel",         L_WindowStub },
    { "WindowSetZOrder",        L_WindowStub },
    { "WindowShow",             L_WindowStub },
    { "WindowText",             L_WindowStub },
    { "WindowTextWidth",        L_WindowStub },
    { "WindowTransformImage",   L_WindowStub },
    { "WindowWrite",            L_WindowStub },
    { "WindowList",             L_WindowListStub },
    { "WindowFontInfo",         L_GuiNilStub     },
    { "WindowFontList",         L_WindowListStub },
    { "WindowHotspotInfo",      L_GuiNilStub     },
    { "WindowHotspotList",      L_WindowListStub },
    { "WindowHotspotTooltip",   L_WindowStub     },
    { "WindowImageInfo",        L_GuiNilStub     },
    { "WindowImageList",        L_WindowListStub },
    { "WindowInfo",             L_GuiNilStub     },

    // ── Pane stubs — legacy API replaced by miniwindows, never fully implemented
    // even on Windows (#ifdef PANE / #if 0).  CreatePane returns eOK so callers
    // don't abort; all others return eNoSuchWindow (30073) since no pane exists.
    { "ClearPane",   [](lua_State* L) -> int { world_doc(L); lua_pushnumber(L, eNoSuchWindow); return 1; } },
    { "ClosePane",   [](lua_State* L) -> int { world_doc(L); lua_pushnumber(L, eNoSuchWindow); return 1; } },
    { "CreatePane",  [](lua_State* L) -> int { world_doc(L); lua_pushnumber(L, eOK);           return 1; } },
    { "PaneLine",    [](lua_State* L) -> int { world_doc(L); lua_pushnumber(L, eNoSuchWindow); return 1; } },
    { "PaneText",    [](lua_State* L) -> int { world_doc(L); lua_pushnumber(L, eNoSuchWindow); return 1; } },

    // ── Chat stubs — return eNoChatConnections ────────────────────────────────
    { "ChatAcceptCalls",        L_ChatStub },
    { "ChatCall",               L_ChatStub },
    { "ChatCallzChat",          L_ChatStub },
    { "ChatDisconnect",         L_ChatStub },
    { "ChatDisconnectAll",      L_ChatStub },
    { "ChatEverybody",          L_ChatStub },
    { "ChatGroup",              L_ChatStub },
    { "ChatID",                 L_ChatStub },
    { "ChatGetID",              L_ChatStub },
    { "ChatMessage",            L_ChatStub },
    { "ChatNameChange",         L_ChatStub },
    { "ChatNote",               L_ChatStub },
    { "ChatPasteEverybody",     L_ChatStub },
    { "ChatPasteText",          L_ChatStub },
    { "ChatPeekConnections",    L_ChatStub },
    { "ChatPersonal",           L_ChatStub },
    { "ChatPing",               L_ChatStub },
    { "ChatRequestConnections", L_ChatStub },
    { "ChatSendFile",           L_ChatStub },
    { "ChatStopAcceptingCalls", L_ChatStub },
    { "ChatStopFileTransfer",   L_ChatStub },
    { "GetChatInfo",            L_ChatDataStub },
    { "GetChatList",            L_ChatDataStub },
    { "GetChatOption",          L_ChatDataStub },
    { "SetChatOption",          L_ChatStub     },
    { "UdpPortList",            L_ChatDataStub },

    // ── Spell-check stubs — return eSpellCheckNotActive ───────────────────────
    { "AddSpellCheckWord",      L_AddSpellCheckWord },
    { "SpellCheck",             L_SpellCheck },
    { "SpellCheckCommand",      L_SpellCheckCommand },
    { "SpellCheckDlg",          L_SpellCheckDlg },

    // ── Notepad stubs ─────────────────────────────────────────────────────────
    { "ActivateNotepad",        L_ActivateNotepad       },
    { "AppendToNotepad",        L_AppendToNotepad },
    { "CloseNotepad",           L_CloseNotepad          },
    { "MoveNotepadWindow",      L_MoveNotepadWindow     },
    { "NotepadColour",          L_NotepadColour   },
    { "NotepadFont",            L_NotepadFont     },
    { "NotepadReadOnly",        L_NotepadReadOnly },
    { "NotepadSaveMethod",      L_GuiOkStub  },
    { "ReplaceNotepad",         L_ReplaceNotepad  },
    { "SaveNotepad",            L_SaveNotepad   },
    { "SendToNotepad",          L_SendToNotepad },
    { "GetNotepadLength",       L_GetNotepadLength },
    { "GetNotepadList",         L_GetNotepadList   },
    { "GetNotepadText",         L_GetNotepadText   },
    { "GetNotepadWindowPosition", L_GetNotepadWindowPosition },

    // ── Extended mapping stubs ────────────────────────────────────────────────
    { "GetMapping",             L_GuiNilStub },
    { "GetMappingItem",         L_GuiNilStub },
    { "GetMappingString",       L_GuiNilStub },
    { "GetRemoveMapReverses",   L_GuiNilStub },
    { "MapColour",              L_GuiNilStub },
    { "MapColourList",          L_WindowListStub },
    { "SetMapping",             L_GuiOkStub  },
    { "SetRemoveMapReverses",   L_GuiOkStub  },

    // ── Misc GUI stubs ────────────────────────────────────────────────────────
    { "AddFont",                L_GuiOkStub  },
    { "FilterPixel",            L_GuiNilStub },
    { "GetFrame",               L_GuiNilStub },
    { "PlaySoundMemory",        L_GuiOkStub  },
    { "ReadNamesFile",          [](lua_State* L) -> int {
        // Real method: headless ReadNames throws (windows_stubs.cpp), so
        // this returns eCouldNotOpenFile — names files are a GUI feature.
        lua_pushnumber(L, world_doc(L)->ReadNamesFile(luaL_checkstring(L, 1)));
        return 1;
    }},
    { "SetCommandSelection",    [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        long first = (long)luaL_checknumber(L, 1);
        long last  = (long)luaL_optnumber(L, 2, -1);
        pDoc->SetCommandSelectionRange(first, last);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetCommandWindowHeight", L_GuiOkStub  },
    { "SetForegroundImage",     L_GuiOkStub  },
    { "SetFrameBackgroundColour",[](lua_State* L) -> int {
        // Real method (methods_utilities.cpp): stores the colour on the
        // stub frame and fires g_wxhook_set_main_background (null headless);
        // Windows returns no result fields.
        world_doc(L)->SetFrameBackgroundColour((long)luaL_checknumber(L, 1));
        return 0;
    }},
    { "SetInputFont",           [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        const char* face = luaL_optstring(L, 1, "");
        int pts    = (int)luaL_optnumber(L, 2, 0);
        bool bold  = lua_toboolean(L, 3) != 0;
        bool italic= lua_toboolean(L, 4) != 0;
        pDoc->EmitInputFont(std::string(face), pts, bold, italic);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetOutputFont",          [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        const char* face = luaL_optstring(L, 1, "");
        int pts    = (int)luaL_optnumber(L, 2, 0);
        pDoc->EmitOutputFont(std::string(face), pts, false, false);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetScroll",              [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        long pos = (long)luaL_checknumber(L, 1);
        // Visible (arg 2) is ignored on non-Windows.
        if (pos != -2)
            pDoc->EmitScroll(pos);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetSelection",           [](lua_State* L) -> int {
        // Real method (methods_utilities.cpp → the same IWorldView
        // setOutputSelection seam); Windows returns no result fields and
        // takes all four coordinates as required args (lua_methods.cpp).
        world_doc(L)->SetSelection(
            (long)luaL_checknumber(L, 1),
            (long)luaL_checknumber(L, 2),
            (long)luaL_checknumber(L, 3),
            (long)luaL_checknumber(L, 4));
        return 0;
    }},
    { "SetToolBarPosition",     L_GuiOkStub  },
    { "ShiftTabCompleteItem",   [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        const char* item = luaL_checkstring(L, 1);
        lua_pushnumber(L, pDoc->ShiftTabCompleteItem(item));
        return 1;
    }},
    { "ShowInfoBar",            L_GuiOkStub  },
    { "TextRectangle",          L_GuiOkStub  },
    { "Transparency",           L_GuiOkStub  },

    { nullptr, nullptr }
};

// Called from RegisterLuaRoutines() in lua_utils_portable.cpp.
// Mirrors the world-registration block in lua_methods.cpp::RegisterLuaRoutines().
void RegisterWorldLib(lua_State* L)
{
    // ── Register the "world" global library ──────────────────────────────────
    // luaL_register leaves the world table on the stack.
    luaL_register(L, WORLD_LIBRARY, worldlib);

    // ── _G metatable so bare calls work: Note("hi") == world.Note("hi") ──────
    // Mirrors: lua_pushvalue(L,LUA_GLOBALSINDEX) / newtable / setfield __index /
    //          setmetatable in lua_methods.cpp.
    lua_pushvalue(L, LUA_GLOBALSINDEX);  // _G
    lua_newtable(L);                      // new metatable for _G
    lua_pushvalue(L, -3);                 // world table (copy)
    lua_setfield(L, -2, "__index");       // metatable.__index = world
    lua_setmetatable(L, -2);              // _G's metatable = our table
    lua_pop(L, 1);                        // pop _G

    // ── mushclient.world named metatable for OOP: w:Note("hi") ───────────────
    // Mirrors the luaL_newmetatable / worldlib_meta / __index = world block.
    luaL_newmetatable(L, mushclient_typename);  // "mushclient.world" metatable
    lua_pushcfunction(L, world2string_fn);
    lua_setfield(L, -2, "__tostring");
    lua_pushvalue(L, -2);                       // world table (still on stack)
    lua_setfield(L, -2, "__index");             // meta.__index = world
    lua_pop(L, 1);                              // pop the named metatable

    lua_pop(L, 1);  // pop the world table
}

#endif // !_WIN32
