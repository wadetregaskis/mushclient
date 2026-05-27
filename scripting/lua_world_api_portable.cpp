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
#include <cstdlib>    // free() for g_wxhook_menu_dialog result
#include <ctime>      // clock_gettime, time()
#include <cmath>      // cos() for WindowBlendColor

#include "platform/wx_mfc_hooks.h"  // g_wxhook_pick_colour, g_wxhook_menu_dialog, …
#include "sqlite3/sqlite3.h"         // sqlite3_libversion()
#include <png.h>                     // PNG_LIBPNG_VER_STRING
#ifdef __APPLE__
#  include <mach-o/dyld.h>           // _NSGetExecutablePath
#endif

using std::string;

// ── helpers ───────────────────────────────────────────────────────────────────

// Retrieve the doc pointer stored in the Lua registry by CScriptEngine.
static CMUSHclientDoc* world_doc(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, DOCUMENT_STATE);
    CMUSHclientDoc* pDoc = static_cast<CMUSHclientDoc*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!pDoc)
        luaL_error(L, "world is no longer available");
    return pDoc;
}

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
    pDoc->ColourTell("", "", s.c_str());
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

// world.GetVariableList() → table { name → value }
static int L_GetVariableList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);

    CString strName;
    CVariable* var = nullptr;
    for (POSITION pos = pDoc->GetVariableMap().GetStartPosition(); pos; ) {
        pDoc->GetVariableMap().GetNextAssoc(pos, strName, var);
        lua_pushstring(L, static_cast<const char*>(strName));
        lua_pushstring(L, static_cast<const char*>(var->strContents));
        lua_rawset(L, -3);
    }
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
// Evaluates a command as if the user typed it (runs aliases, triggers, etc.)
static int L_Execute(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* cmd = luaL_checkstring(L, 1);

    if (++pDoc->m_iExecutionDepth > MAX_EXECUTION_DEPTH) {
        --pDoc->m_iExecutionDepth;
        lua_pushnumber(L, eCommandsNestedTooDeeply);
        return 1;
    }

    bool bOmit = false;
    pDoc->EvaluateCommand(CString(cmd), /*count=*/false, bOmit, /*test=*/false);

    --pDoc->m_iExecutionDepth;
    lua_pushnumber(L, eOK);
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
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushboolean(L, pDoc->m_iConnectPhase == eConnectConnectedToMud ? 1 : 0);
    return 1;
}

// ── Error code helpers ────────────────────────────────────────────────────────

// world.ErrorDesc(code) → string description
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
    // Push defaults for the extra AddTriggerEx arguments and delegate
    lua_pushnumber(L, eSendToWorld);
    lua_pushnumber(L, DEFAULT_TRIGGER_SEQUENCE);
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
    CMUSHclientDoc* pDoc = world_doc(L);
    string text = concat_args(L);
    pDoc->m_bDoingSimulate = true;
    pDoc->DisplayMsg(text.c_str(), (int)text.size(), 0);
    pDoc->m_bDoingSimulate = false;
    return 0;
}

// world.StripANSI(text) → text with ANSI codes removed
static int L_StripANSI(lua_State* L)
{
    const char* src = luaL_checkstring(L, 1);
    string out;
    const char* p = src;
    while (*p) {
        if (*p == '\033' && *(p+1) == '[') {
            p += 2;
            while (*p && *p != 'm') p++;
            if (*p) p++;
        } else {
            out += *p++;
        }
    }
    lua_pushlstring(L, out.c_str(), out.size());
    return 1;
}

// world.Trim(text) → trimmed string
static int L_Trim(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    const char* e = s + strlen(s);
    while (e > s && (*(e-1) == ' ' || *(e-1) == '\t' || *(e-1) == '\n' || *(e-1) == '\r')) e--;
    lua_pushlstring(L, s, (size_t)(e - s));
    return 1;
}

// world.Version() → version string
static int L_Version(lua_State* L)
{
    lua_pushstring(L, MUSHCLIENT_VERSION);
    return 1;
}

// world.WorldAddress() → string
static int L_WorldAddress(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushstring(L, static_cast<const char*>(pDoc->m_mush_name));
    return 1;
}

// world.WorldPort() → number
static int L_WorldPort(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)pDoc->m_port);
    return 1;
}

// world.WorldName() → string (alias for GetWorldName)
static int L_WorldName(lua_State* L) { return L_GetWorldName(L); }

// world.AnsiNote(text) — like Note but preserves ANSI codes
static int L_AnsiNote(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->Note(concat_args(L).c_str());
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
// Return a Lua table (array) of labels for all triggers/aliases/timers.

static int L_GetTriggerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int idx = 1;
    CString strLabel;
    CTrigger* pt;
    for (POSITION pos = pDoc->GetTriggerMap().GetStartPosition(); pos; ) {
        pDoc->GetTriggerMap().GetNextAssoc(pos, strLabel, pt);
        lua_pushstring(L, (const char*)strLabel);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int L_GetAliasList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int idx = 1;
    CString strLabel;
    CAlias* pa;
    for (POSITION pos = pDoc->GetAliasMap().GetStartPosition(); pos; ) {
        pDoc->GetAliasMap().GetNextAssoc(pos, strLabel, pa);
        lua_pushstring(L, (const char*)strLabel);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int L_GetTimerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int idx = 1;
    CString strLabel;
    CTimer* pt;
    for (POSITION pos = pDoc->GetTimerMap().GetStartPosition(); pos; ) {
        pDoc->GetTimerMap().GetNextAssoc(pos, strLabel, pt);
        lua_pushstring(L, (const char*)strLabel);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

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
    // Enables/disables triggers, aliases, and timers in the named group.
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* group = luaL_checkstring(L, 1);
    bool enabled = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    long count = 0;
    CString strLabel;
    { CTrigger* pt;
      for (POSITION pos = pDoc->GetTriggerMap().GetStartPosition(); pos; ) {
          pDoc->GetTriggerMap().GetNextAssoc(pos, strLabel, pt);
          if (pt->strGroup == CString(group)) { pt->bEnabled = enabled; count++; }
      }
    }
    { CAlias* pa;
      for (POSITION pos = pDoc->GetAliasMap().GetStartPosition(); pos; ) {
          pDoc->GetAliasMap().GetNextAssoc(pos, strLabel, pa);
          if (pa->strGroup == CString(group)) { pa->bEnabled = enabled; count++; }
      }
    }
    { CTimer* pt;
      for (POSITION pos = pDoc->GetTimerMap().GetStartPosition(); pos; ) {
          pDoc->GetTimerMap().GetNextAssoc(pos, strLabel, pt);
          if (pt->strGroup == CString(group)) { pt->bEnabled = enabled; count++; }
      }
    }
    lua_pushnumber(L, count);
    return 1;
}

// ── Line count / received line access ────────────────────────────────────────

static int L_GetLineCount(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (double)pDoc->m_newlines_received);
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
    CMUSHclientDoc* pDoc = world_doc(L);
    int iType = (int)luaL_checknumber(L, 1);
    switch (iType) {
        // ── Strings ──────────────────────────────────────────────────────────
        case 1:   lua_pushstring(L, (const char*)pDoc->m_server);              return 1;
        case 2:   lua_pushstring(L, (const char*)pDoc->m_mush_name);           return 1;
        case 3:   lua_pushstring(L, (const char*)pDoc->m_name);                return 1;
        // 4-7: file/line preamble/postamble — not in portable CMUSHclientDoc
        case 4: case 5: case 6: case 7: lua_pushstring(L, "");                 return 1;
        case 8:   lua_pushstring(L, (const char*)pDoc->m_notes);               return 1;
        // 9, 10, 20, 23-27: fields absent from portable doc_stub
        case 9: case 10: case 20:
        case 23: case 24: case 25: case 26: case 27:
            lua_pushstring(L, "");                                              return 1;
        case 19:  lua_pushstring(L, (const char*)pDoc->m_strSpeedWalkFiller);  return 1;
        case 22:  lua_pushstring(L, (const char*)pDoc->m_connect_text);        return 1;
        case 11:  lua_pushstring(L, (const char*)pDoc->m_strLogFilePreamble);  return 1;
        case 12:  lua_pushstring(L, (const char*)pDoc->m_strLogFilePostamble); return 1;
        case 13:  lua_pushstring(L, (const char*)pDoc->m_strLogLinePreambleInput);   return 1;
        case 14:  lua_pushstring(L, (const char*)pDoc->m_strLogLinePreambleNotes);   return 1;
        case 15:  lua_pushstring(L, (const char*)pDoc->m_strLogLinePreambleOutput);  return 1;
        case 16:  lua_pushstring(L, (const char*)pDoc->m_strLogLinePostambleInput);  return 1;
        case 17:  lua_pushstring(L, (const char*)pDoc->m_strLogLinePostambleNotes);  return 1;
        case 18:  lua_pushstring(L, (const char*)pDoc->m_strLogLinePostambleOutput); return 1;
        case 21:  lua_pushstring(L, (const char*)pDoc->m_speed_walk_prefix);   return 1;
        case 28:  lua_pushstring(L, (const char*)pDoc->m_strLanguage);         return 1;
        case 29:  lua_pushstring(L, (const char*)pDoc->m_strWorldOpen);        return 1;
        case 30:  lua_pushstring(L, (const char*)pDoc->m_strWorldClose);       return 1;
        case 31:  lua_pushstring(L, (const char*)pDoc->m_strWorldConnect);     return 1;
        case 32:  lua_pushstring(L, (const char*)pDoc->m_strWorldDisconnect);  return 1;
        case 33:  lua_pushstring(L, (const char*)pDoc->m_strWorldGetFocus);    return 1;
        case 34:  lua_pushstring(L, (const char*)pDoc->m_strWorldLoseFocus);   return 1;
        // 37-39, 41, 50: fields absent from portable doc_stub
        case 37: case 38: case 39: case 41: case 50:
            lua_pushstring(L, "");                                              return 1;
        case 35:  lua_pushstring(L, (const char*)pDoc->m_strScriptFilename);   return 1;
        case 36:  lua_pushstring(L, (const char*)pDoc->m_strScriptPrefix);     return 1;
        case 40:  lua_pushstring(L, (const char*)pDoc->m_strAutoLogFileName);  return 1;
        case 42:  lua_pushstring(L, (const char*)pDoc->m_strTerminalIdentification); return 1;
        case 43:  lua_pushstring(L, (const char*)pDoc->m_strMappingFailure);   return 1;
        case 44:  lua_pushstring(L, (const char*)pDoc->m_strOnMXP_Start);      return 1;
        case 45:  lua_pushstring(L, (const char*)pDoc->m_strOnMXP_Stop);       return 1;
        case 46:  lua_pushstring(L, (const char*)pDoc->m_strOnMXP_Error);      return 1;
        case 47:  lua_pushstring(L, (const char*)pDoc->m_strOnMXP_OpenTag);    return 1;
        case 48:  lua_pushstring(L, (const char*)pDoc->m_strOnMXP_CloseTag);   return 1;
        case 49:  lua_pushstring(L, (const char*)pDoc->m_strOnMXP_SetVariable);return 1;
        case 51:  lua_pushstring(L, (const char*)pDoc->m_logfile_name);        return 1;
        case 52:  lua_pushstring(L, "");   // last immediate expression
                  return 1;
        case 53:  lua_pushstring(L, (const char*)pDoc->m_strStatusMessage);    return 1;
        case 54:  lua_pushstring(L, (const char*)pDoc->GetPathName());          return 1;
        case 55:  lua_pushstring(L, (const char*)pDoc->GetTitle());             return 1;
        // 56-60, 66, 69-71, 74, 82, 85: App object fields not available in portable build
        case 56: case 57: case 58: case 60: case 66: case 69:
        case 70: case 71: case 74: case 82: case 85:
            lua_pushstring(L, "");                                              return 1;
        case 59:  {   // directory containing the executable
            char buf[4096] = ".";
#ifdef __APPLE__
            uint32_t sz = sizeof(buf);
            if (_NSGetExecutablePath(buf, &sz) == 0) {
                char* p = strrchr(buf, '/'); if (p) *p = '\0';
            }
#else
            ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
            if (n > 0) { buf[n] = '\0'; char* p = strrchr(buf, '/'); if (p) *p = '\0'; }
#endif
            lua_pushstring(L, buf);
        }   return 1;
        case 61:  {   // server IP as dotted string (resolve if connected)
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            const char* host = static_cast<const char*>(pDoc->m_server);
            char ip[INET_ADDRSTRLEN] = "";
            if (host && *host && getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
                inet_ntop(AF_INET,
                    &reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr,
                    ip, sizeof(ip));
                freeaddrinfo(res);
            }
            lua_pushstring(L, ip);
        }   return 1;
        case 63:  {   // local hostname
            char host[256] = "";
            gethostname(host, sizeof(host));
            lua_pushstring(L, host);
        }   return 1;
        case 64:  {   // current working directory (trailing slash)
            char* cwd = getcwd(nullptr, 0);
            if (cwd) {
                string s(cwd); free(cwd);
                if (s.empty() || s.back() != '/') s += '/';
                lua_pushstring(L, s.c_str());
            } else { lua_pushstring(L, ""); }
        }   return 1;
        case 65:  lua_pushstring(L, (const char*)pDoc->m_strWorldSave);        return 1;
        case 67:  {   // directory containing the world file
            CString path = pDoc->GetPathName();
            const char* p = static_cast<const char*>(path);
            const char* slash = strrchr(p, '/');
            if (slash) lua_pushlstring(L, p, (size_t)(slash - p + 1));
            else        lua_pushstring(L, "");
        }   return 1;
        case 72:  lua_pushstring(L, MUSHCLIENT_VERSION);                        return 1;
        case 73:  lua_pushstring(L, __DATE__ " " __TIME__);                    return 1;
        case 75:  lua_pushstring(L, pDoc->m_IAC_subnegotiation_data.c_str());  return 1;
        // 76-79, 81, 84, 86: font/image/browser fields not available
        case 76: case 77: case 78: case 79: case 81: case 84: case 86:
            lua_pushstring(L, "");                                              return 1;
        case 80:  lua_pushstring(L, PNG_LIBPNG_VER_STRING);                    return 1;
        case 83:  lua_pushstring(L, sqlite3_libversion());                      return 1;
        case 87:  lua_pushstring(L, (const char*)pDoc->m_strLastCommandSent);  return 1;
        case 88:  lua_pushstring(L, (const char*)pDoc->m_strWindowTitle);      return 1;
        case 89:  lua_pushstring(L, (const char*)pDoc->m_strMainWindowTitle);  return 1;
        // ── Booleans ─────────────────────────────────────────────────────────
        case 101: lua_pushboolean(L, pDoc->m_bNoEcho);                         return 1;
        case 102: lua_pushboolean(L, false);  // debug packets — always false
                  return 1;
        case 103: lua_pushboolean(L, pDoc->m_bCompress);                        return 1;
        case 104: lua_pushboolean(L, pDoc->m_bMXP);                            return 1;
        case 105: lua_pushboolean(L, pDoc->m_bPuebloActive);                   return 1;
        case 106: lua_pushboolean(L,
                      pDoc->m_iConnectPhase != eConnectConnectedToMud);         return 1;
        case 107: lua_pushboolean(L,
                      pDoc->m_iConnectPhase != eConnectNotConnected &&
                      pDoc->m_iConnectPhase != eConnectConnectedToMud);         return 1;
        case 108: lua_pushboolean(L, pDoc->m_bDisconnectOK);                   return 1;
        case 109: lua_pushboolean(L, pDoc->m_bTrace);                          return 1;
        case 110: lua_pushboolean(L, false);  // script file changed — not tracked
                  return 1;
        case 111: lua_pushboolean(L, pDoc->IsModified() != FALSE);              return 1;
        case 112: lua_pushboolean(L, pDoc->m_bMapping);                        return 1;
        case 113: lua_pushboolean(L,
                      pDoc->m_iConnectPhase == eConnectConnectedToMud);         return 1;
        case 114: lua_pushboolean(L, pDoc->m_gui_paused);                      return 1;
        case 115: lua_pushboolean(L, false);  // localization not supported     return 1;
        case 118: lua_pushboolean(L, pDoc->m_bVariablesChanged);               return 1;
        case 119: lua_pushboolean(L, pDoc->m_ScriptEngine != nullptr);         return 1;
        case 120: lua_pushboolean(L, false);  // scroll-bar-wanted not tracked  return 1;
        case 121: lua_pushboolean(L, true);   // clock_gettime always available return 1;
        case 122: lua_pushboolean(L, sqlite3_threadsafe() != 0);               return 1;
        case 123: lua_pushboolean(L, pDoc->m_bDoingSimulate);                  return 1;
        case 124: lua_pushboolean(L, pDoc->m_bLineOmittedFromOutput);          return 1;
        case 125: lua_pushboolean(L, false);  // fullscreen not supported       return 1;
        // ── Numbers ──────────────────────────────────────────────────────────
        case 141: // fall through — same field, both type numbers used in the wild
        case 201: lua_pushnumber(L, pDoc->m_total_lines);                     return 1;
        case 203: lua_pushnumber(L, pDoc->m_nTotalLinesSent);                 return 1;
        case 204: lua_pushnumber(L, pDoc->m_iInputPacketCount);               return 1;
        case 205: lua_pushnumber(L, 0);  // output packet count not tracked   return 1;
        case 206: lua_pushnumber(L, pDoc->m_nTotalUncompressed);              return 1;
        case 207: lua_pushnumber(L, pDoc->m_nTotalCompressed);                return 1;
        case 208: lua_pushnumber(L, pDoc->m_iMCCP_type);                      return 1;
        case 209: lua_pushnumber(L, (double)pDoc->m_iMXPerrors);              return 1;
        case 210: lua_pushnumber(L, (double)pDoc->m_iMXPtags);                return 1;
        case 211: lua_pushnumber(L, (double)pDoc->m_iMXPentities);            return 1;
        case 212: lua_pushnumber(L, pDoc->m_gui_font_height);                 return 1;
        case 213: lua_pushnumber(L, pDoc->m_gui_font_width);                  return 1;
        case 214: lua_pushnumber(L, pDoc->m_gui_font_height); return 1; // input same as output
        case 215: lua_pushnumber(L, pDoc->m_gui_font_width);                  return 1;
        case 216: lua_pushnumber(L, pDoc->m_nBytesIn);                        return 1;
        case 217: lua_pushnumber(L, pDoc->m_nBytesOut);                       return 1;
        case 218: lua_pushnumber(L, (double)pDoc->GetVariableMap().GetCount()); return 1;
        case 219: lua_pushnumber(L, (double)pDoc->GetTriggerMap().GetCount()); return 1;
        case 220: lua_pushnumber(L, (double)pDoc->GetTimerMap().GetCount());  return 1;
        case 221: lua_pushnumber(L, (double)pDoc->GetAliasMap().GetCount());  return 1;
        case 202: lua_pushnumber(L, pDoc->m_new_lines);                       return 1;
        case 222: lua_pushnumber(L, (double)pDoc->m_QueuedCommandsList.GetCount()); return 1;
        case 223: lua_pushnumber(L, (double)pDoc->m_strMapList.GetCount());   return 1;
        case 224: lua_pushnumber(L, (double)pDoc->m_LineList.GetCount());     return 1;
        case 225: lua_pushnumber(L, (double)pDoc->m_CustomElementMap.GetCount()); return 1;
        case 226: lua_pushnumber(L, (double)pDoc->m_CustomEntityMap.GetCount()); return 1;
        case 227: lua_pushnumber(L, pDoc->m_iConnectPhase);                   return 1;
        case 230: lua_pushnumber(L, pDoc->m_iExecutionDepth);                 return 1;
        case 231: // log file size
            lua_pushnumber(L, pDoc->m_logfile ? (double)ftell(pDoc->m_logfile) : 0.0);
            return 1;
        case 235: lua_pushnumber(L, 1);                                       return 1; // always 1 command window
        case 236: // command selection start column
        case 237: // command selection end column
            {
                long selStart = 0, selEnd = 0;
                if (pDoc->m_fn_get_command_selection)
                    pDoc->m_fn_get_command_selection(selStart, selEnd);
                lua_pushnumber(L, iType == 236 ? (double)selStart : (double)selEnd);
            }
            return 1;
        case 239: lua_pushnumber(L, pDoc->m_iCurrentActionSource);            return 1;
        case 240: lua_pushnumber(L, pDoc->m_gui_font_width);                  return 1;
        case 241: lua_pushnumber(L, pDoc->m_gui_font_height);                 return 1;
        case 244: lua_pushnumber(L, pDoc->m_iTriggersEvaluatedCount);         return 1;
        case 245: lua_pushnumber(L, pDoc->m_iTriggersMatchedCount);           return 1;
        case 246: lua_pushnumber(L, pDoc->m_iAliasesEvaluatedCount);          return 1;
        case 247: lua_pushnumber(L, pDoc->m_iAliasesMatchedCount);            return 1;
        case 248: lua_pushnumber(L, pDoc->m_iTimersFiredCount);               return 1;
        case 232: {   // high-resolution timer (seconds since epoch as double)
            struct timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            lua_pushnumber(L, (double)ts.tv_sec + ts.tv_nsec * 1e-9);
        }   return 1;
        case 233: lua_pushnumber(L, 0.0); return 1; // trigger exec time — not tracked portably
        case 234: lua_pushnumber(L, 0.0); return 1; // alias exec time — not tracked portably
        case 238: lua_pushnumber(L, 0);   return 1; // world window display flags — N/A
        case 242: lua_pushnumber(L, 0);   return 1; // bad UTF-8 lines — not tracked
        case 243: lua_pushnumber(L, 0);   return 1; // fixed pitch font size — N/A
        case 286: lua_pushnumber(L, pDoc->m_iTriggersMatchedThisSessionCount); return 1;
        case 287: lua_pushnumber(L, pDoc->m_iAliasesMatchedThisSessionCount);  return 1;
        case 288: lua_pushnumber(L, pDoc->m_iTimersFiredThisSessionCount);     return 1;
        case 289: lua_pushnumber(L, pDoc->m_last_line_with_IAC_GA);           return 1;
        case 298: {   // SQLite3 version as integer (e.g. 3044000)
            lua_pushnumber(L, sqlite3_libversion_number());
        }   return 1;
        case 310: lua_pushnumber(L, pDoc->m_newlines_received);               return 1;
        // ── Dates — returned as Unix timestamps (doubles) ─────────────────
        case 301: {
            time_t t = pDoc->m_tConnectTime.GetTime();
            if (t) lua_pushnumber(L, (double)t); else lua_pushnil(L);
        }   return 1;
        case 302: {
            time_t t = pDoc->m_LastFlushTime.GetTime();
            if (t) lua_pushnumber(L, (double)t); else lua_pushnil(L);
        }   return 1;
        case 303: lua_pushnil(L);  return 1; // script file mod time — not tracked portably
        case 304: lua_pushnumber(L, (double)time(nullptr));                   return 1;
        case 305: lua_pushnil(L);  return 1; // client start time — not tracked portably
        case 306: lua_pushnil(L);  return 1; // world open time — not tracked portably
        default:  lua_pushnil(L);                                              return 1;
    }
}

// ── GetOption / SetOption ─────────────────────────────────────────────────────
// GetOption returns a numeric value for a named option.
// SetOption sets a named option and returns eOK or an error code.
//
// Strategy (portable build):
//   1. Hand-coded members  — the few fields that exist in CMUSHclientDoc and are
//      actively used by the engine; these are read/written directly for accuracy.
//   2. Option cache        — m_numericOptionCache / m_alphaOptionCache hold values
//      for any option ever written via SetOption / SetAlphaOption.
//   3. OptionsTable default — fallback default value for valid options not yet set.
//   Unknown option names return -1 (Get) or eUnknownOption (Set).

extern tConfigurationNumericOption OptionsTable[];
extern tConfigurationAlphaOption   AlphaOptionsTable[];

// Return OptionsTable default for a numeric option, or LONG_MIN if not found.
static long opt_numeric_default(const char* name)
{
    for (int i = 0; OptionsTable[i].pName; ++i)
        if (strcmp(OptionsTable[i].pName, name) == 0)
            return (long)OptionsTable[i].iDefault;
    return LONG_MIN;   // sentinel: not a valid option name
}

// Return AlphaOptionsTable default for an alpha option, or nullptr if not found.
static const char* opt_alpha_default(const char* name)
{
    for (int i = 0; AlphaOptionsTable[i].pName; ++i)
        if (strcmp(AlphaOptionsTable[i].pName, name) == 0)
            return AlphaOptionsTable[i].sDefault ? AlphaOptionsTable[i].sDefault : "";
    return nullptr;    // sentinel: not a valid option name
}

// Macro: emit a live-member read and return immediately.
#define OPT_GET_MEMBER(field) do { lua_pushnumber(L, (lua_Number)(field)); return 1; } while(0)

static int L_GetOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* opt = luaL_checkstring(L, 1);

    // 1. Known live members (read from the real field).
    if      (!strcmp(opt, "enable_triggers"))   OPT_GET_MEMBER(pDoc->m_enable_triggers  ? 1 : 0);
    else if (!strcmp(opt, "enable_aliases"))    OPT_GET_MEMBER(pDoc->m_enable_aliases   ? 1 : 0);
    else if (!strcmp(opt, "enable_timers"))     OPT_GET_MEMBER(pDoc->m_bEnableTimers    ? 1 : 0);
    else if (!strcmp(opt, "enable_trigger_sounds")) OPT_GET_MEMBER(pDoc->m_enable_trigger_sounds ? 1 : 0);
    else if (!strcmp(opt, "enable_speed_walk")) OPT_GET_MEMBER(pDoc->m_enable_speed_walk? 1 : 0);
    else if (!strcmp(opt, "display_my_input"))  OPT_GET_MEMBER(pDoc->m_display_my_input ? 1 : 0);
    else if (!strcmp(opt, "log_output"))        OPT_GET_MEMBER(pDoc->m_bLogOutput);
    else if (!strcmp(opt, "log_notes"))         OPT_GET_MEMBER(pDoc->m_bLogNotes);
    else if (!strcmp(opt, "log_input"))         OPT_GET_MEMBER(pDoc->m_log_input);
    else if (!strcmp(opt, "log_html"))          OPT_GET_MEMBER(pDoc->m_bLogHTML);
    else if (!strcmp(opt, "log_raw"))           OPT_GET_MEMBER(pDoc->m_bLogRaw ? 1 : 0);
    else if (!strcmp(opt, "log_in_colour"))     OPT_GET_MEMBER(pDoc->m_bLogInColour ? 1 : 0);
    else if (!strcmp(opt, "port"))              OPT_GET_MEMBER(pDoc->m_port);
    else if (!strcmp(opt, "trace"))             OPT_GET_MEMBER(pDoc->m_bTrace ? 1 : 0);
    else if (!strcmp(opt, "wrap_column"))       OPT_GET_MEMBER(pDoc->m_nWrapColumn);
    else if (!strcmp(opt, "max_output_lines"))  OPT_GET_MEMBER(pDoc->m_maxlines);
    else if (!strcmp(opt, "utf_8"))             OPT_GET_MEMBER(pDoc->m_bUTF_8 ? 1 : 0);
    else if (!strcmp(opt, "use_mxp"))           OPT_GET_MEMBER(pDoc->m_iUseMXP);
    else if (!strcmp(opt, "detect_pueblo"))     OPT_GET_MEMBER(pDoc->m_bPueblo ? 1 : 0);
    else if (!strcmp(opt, "naws"))              OPT_GET_MEMBER(pDoc->m_bNAWS ? 1 : 0);
    else if (!strcmp(opt, "disable_compression")) OPT_GET_MEMBER(pDoc->m_bDisableCompression ? 1 : 0);
    else if (!strcmp(opt, "connect_method"))    OPT_GET_MEMBER(pDoc->m_connect_now);
    // use_ssl / proxy_port / proxy_type: not stored as live members in the
    // portable stub — fall through to cache / OptionsTable default below.

    // 2. Cache — value previously written by SetOption.
    {
        auto it = pDoc->m_numericOptionCache.find(opt);
        if (it != pDoc->m_numericOptionCache.end()) {
            lua_pushnumber(L, (lua_Number)it->second);
            return 1;
        }
    }

    // 3. OptionsTable default (returns LONG_MIN for unknown names).
    long def = opt_numeric_default(opt);
    lua_pushnumber(L, (lua_Number)(def == LONG_MIN ? -1L : def));
    return 1;
}

#undef OPT_GET_MEMBER

static int L_SetOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* opt = luaL_checkstring(L, 1);
    long val = (long)luaL_checknumber(L, 2);

    // 1. Known live members — update the real field.
    // These are kept symmetric with the live-member reads in L_GetOption.
    bool is_member = true;
    if      (!strcmp(opt, "enable_triggers"))      pDoc->m_enable_triggers        = (val != 0);
    else if (!strcmp(opt, "enable_aliases"))       pDoc->m_enable_aliases         = (val != 0);
    else if (!strcmp(opt, "enable_timers"))        pDoc->m_bEnableTimers          = (val != 0);
    else if (!strcmp(opt, "display_my_input"))     pDoc->m_display_my_input       = (val != 0);
    else if (!strcmp(opt, "log_output"))           pDoc->m_bLogOutput             = (short)val;
    else if (!strcmp(opt, "log_notes"))            pDoc->m_bLogNotes              = (short)val;
    else if (!strcmp(opt, "log_input"))            pDoc->m_log_input              = (short)val;
    else if (!strcmp(opt, "trace"))                pDoc->m_bTrace                 = (val != 0);
    else if (!strcmp(opt, "enable_trigger_sounds"))pDoc->m_enable_trigger_sounds  = (val != 0);
    else if (!strcmp(opt, "enable_speed_walk"))    pDoc->m_enable_speed_walk      = (val != 0);
    else if (!strcmp(opt, "log_html"))             pDoc->m_bLogHTML               = (val != 0);
    else if (!strcmp(opt, "log_raw"))              pDoc->m_bLogRaw                = (val != 0);
    else if (!strcmp(opt, "log_in_colour"))        pDoc->m_bLogInColour           = (val != 0);
    else if (!strcmp(opt, "utf_8"))                pDoc->m_bUTF_8                 = (val != 0);
    else if (!strcmp(opt, "use_mxp"))              pDoc->m_iUseMXP                = (short)val;
    else if (!strcmp(opt, "detect_pueblo"))        pDoc->m_bPueblo                = (val != 0);
    else if (!strcmp(opt, "naws"))                 pDoc->m_bNAWS                  = (val != 0);
    else if (!strcmp(opt, "disable_compression"))  pDoc->m_bDisableCompression    = (val != 0);
    else if (!strcmp(opt, "connect_method"))       pDoc->m_connect_now            = (short)val;
    else if (!strcmp(opt, "wrap_column")) {
        if (val >= 20 && val <= MAX_LINE_WIDTH) {
            pDoc->m_nWrapColumn = (unsigned short)val;
        }
    }
    else if (!strcmp(opt, "max_output_lines")) {
        if (val >= 200 && val <= 500000) {
            pDoc->m_maxlines = val;
            if (pDoc->m_fn_set_max_lines) pDoc->m_fn_set_max_lines((int)val);
        }
    }
    else                                           is_member = false;

    // 2. Validate: reject completely unknown option names.
    if (!is_member && opt_numeric_default(opt) == LONG_MIN) {
        lua_pushnumber(L, eUnknownOption);
        return 1;
    }

    // 3. Store in cache (always, so GetOption sees the updated value).
    pDoc->m_numericOptionCache[opt] = val;
    lua_pushnumber(L, eOK);
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
    static long counter = 0;
    lua_pushnumber(L, ++counter);
    return 1;
}

// ── Connection control ────────────────────────────────────────────────────────

// world.IsConnected() → bool  (already registered above as L_IsConnected)
// world.Connect() — not applicable in single-world portable mode; no-op.
static int L_ConnectWorld(lua_State* L)
{
    lua_pushnumber(L, eOK);
    return 1;
}

// world.Disconnect() → error_code
static int L_DisconnectWorld(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_pSocket) {
        pDoc->OnConnectionDisconnect();
    }
    lua_pushnumber(L, eOK);
    return 1;
}

// ── World identity ────────────────────────────────────────────────────────────

// world.GetWorldID() → string  (stable UUID-like ID for this world)
// On Windows this is persisted in the world file.  On portable we generate
// one on first access using the world name + port as a seed.
static int L_GetWorldID(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_strWorldID.IsEmpty()) {
        // Generate a stable pseudo-UUID from world name + port
        char buf[64];
        unsigned long h = 2166136261UL;
        for (const char* p = (const char*)pDoc->m_mush_name; *p; ++p)
            h = (h ^ (unsigned char)*p) * 16777619UL;
        h ^= (unsigned long)pDoc->m_port;
        snprintf(buf, sizeof(buf), "%08lx-0000-0000-0000-%012lx", h, h * 6364136223846793005ULL);
        pDoc->m_strWorldID = CString(buf);
    }
    lua_pushstring(L, (const char*)pDoc->m_strWorldID);
    return 1;
}

// ── Status bar ────────────────────────────────────────────────────────────────

// world.SetStatus(text) — update the GUI status bar (if a callback is wired)
static int L_SetStatus(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_optstring(L, 1, "");
    if (pDoc->m_fn_status)
        pDoc->m_fn_status(std::string(msg));
    return 0;
}

// ── Colour note helpers ───────────────────────────────────────────────────────

// world.NoteColourRGB(foreRGB, backRGB) — set colour for subsequent Note calls
static int L_NoteColourRGB(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_NoteColourFore = (COLORREF)(long)luaL_checknumber(L, 1);
    pDoc->m_NoteColourBack = (COLORREF)(long)luaL_checknumber(L, 2);
    return 0;
}

// world.NoteColourFore(foreRGB) — set foreground note colour only
static int L_NoteColourFore(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_NoteColourFore = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

// world.NoteColourBack(backRGB) — set background note colour only
static int L_NoteColourBack(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_NoteColourBack = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

// world.NormalColour() — reset to default note colours
static int L_NormalColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_NoteColourFore = RGB(192, 192, 192);
    pDoc->m_NoteColourBack = RGB(0,   0,   0);
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
    CMUSHclientDoc* pDoc = world_doc(L);
    long lineNum = (long)luaL_checknumber(L, 1);
    int  infoType = (int)luaL_optnumber(L, 2, 0);

    long count = pDoc->m_LineList.GetCount();
    if (count == 0) { lua_pushnil(L); return 1; }

    // Convert 1-based API to 0-based index
    long idx = (lineNum > 0) ? (lineNum - 1) : (count + lineNum);
    CLine* pLine = GetLineAt(pDoc, idx);
    if (!pLine) { lua_pushnil(L); return 1; }

    if (infoType == 0) {
        // Return full table matching Windows infoType=0
        lua_newtable(L);
        lua_pushstring(L, pLine->text ? std::string(pLine->text, pLine->len).c_str() : "");
        lua_setfield(L, -2, "text");
        lua_pushnumber(L, pLine->len);
        lua_setfield(L, -2, "length");
        lua_pushboolean(L, pLine->hard_return ? 1 : 0);
        lua_setfield(L, -2, "newline");
        lua_pushboolean(L, (pLine->flags & COMMENT)    ? 1 : 0);
        lua_setfield(L, -2, "note");
        lua_pushboolean(L, (pLine->flags & USER_INPUT) ? 1 : 0);
        lua_setfield(L, -2, "user");
        lua_pushboolean(L, (pLine->flags & LOG_LINE)   ? 1 : 0);
        lua_setfield(L, -2, "log");
        lua_pushnumber(L, (double)pLine->m_theTime.GetTime());
        lua_setfield(L, -2, "time");
        lua_pushnumber(L, (double)pLine->m_nLineNumber);
        lua_setfield(L, -2, "line");
        return 1;
    }

    // Scalar infoTypes (subset matching Windows API)
    switch (infoType) {
        case 1:  // text
            lua_pushstring(L, pLine->text
                           ? std::string(pLine->text, pLine->len).c_str() : "");
            break;
        case 2:  // length
            lua_pushnumber(L, pLine->len);
            break;
        case 3:  // hard return
            lua_pushboolean(L, pLine->hard_return ? 1 : 0);
            break;
        case 4:  // is note/comment
            lua_pushboolean(L, (pLine->flags & COMMENT) ? 1 : 0);
            break;
        case 5:  // is user input
            lua_pushboolean(L, (pLine->flags & USER_INPUT) ? 1 : 0);
            break;
        case 9:  // timestamp
            lua_pushnumber(L, (double)pLine->m_theTime.GetTime());
            break;
        case 10: // line number
            lua_pushnumber(L, (double)pLine->m_nLineNumber);
            break;
        default:
            lua_pushnil(L);
            break;
    }
    return 1;
}

// world.GetCurrentValue(optionName) → current value of a world option
// Bridges to GetOption / GetAlphaOption depending on what the option is.
static int L_GetCurrentValue(lua_State* L)
{
    // Delegate to GetOption first (numeric), fall back to alpha
    const char* opt = luaL_checkstring(L, 1);
    // Try numeric options
    lua_getglobal(L, WORLD_LIBRARY);
    lua_getfield(L, -1, "GetOption");
    lua_remove(L, -2);
    if (!lua_isnil(L, -1)) {
        lua_pushvalue(L, 1);   // option name
        lua_call(L, 1, 1);
        // If result is -1 (unknown), try alpha
        if (lua_isnumber(L, -1) && lua_tonumber(L, -1) == -1.0) {
            lua_pop(L, 1);
            lua_getglobal(L, WORLD_LIBRARY);
            lua_getfield(L, -1, "GetAlphaOption");
            lua_remove(L, -2);
            lua_pushvalue(L, 1);
            lua_call(L, 1, 1);
        }
    }
    return 1;
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
    const char* text = luaL_checkstring(L, 1);
    size_t len = strlen(text);

    // SHS_INFO is available via shs.h (already pulled in by doc_stub.h)
    SHS_INFO sha;
    shsInit(&sha);
    shsUpdate(&sha, (BYTE*)text, (int)len);
    shsFinal(&sha);

    char buf[41];
    snprintf(buf, sizeof(buf), "%08lx%08lx%08lx%08lx%08lx",
             (unsigned long)sha.digest[0], (unsigned long)sha.digest[1],
             (unsigned long)sha.digest[2], (unsigned long)sha.digest[3],
             (unsigned long)sha.digest[4]);
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

// world.IsPluginInstalled(pluginID) → bool  (alias for IsPlugin)
static int L_IsPluginInstalled(lua_State* L)
{
    return L_IsPlugin(L);
}

// world.EnablePlugin(pluginID [, enabled]) → error_code
static int L_EnablePlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    bool enable = lua_toboolean(L, 2) || lua_isnone(L, 2);  // defaults to true

    for (auto* p : pDoc->m_PluginList) {
        if (strcmp((const char*)p->m_strID, id) == 0) {
            p->m_bEnabled = enable ? TRUE : FALSE;
            lua_pushnumber(L, eOK);
            return 1;
        }
    }
    lua_pushnumber(L, eNoSuchPlugin);
    return 1;
}

// ── Plugin API ────────────────────────────────────────────────────────────────

// world.LoadPlugin(path) → error_code
// Loads a plugin from the given file path.  Returns eOK on success,
// ePluginFileNotFound (or a generic non-zero code) on failure.
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

// world.GetPluginList() → table of plugin IDs (1-based array)
static int L_GetPluginList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int i = 1;
    for (auto* p : pDoc->m_PluginList) {
        lua_pushstring(L, (const char*)p->m_strID);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// world.GetPluginInfo(pluginID, infoType) → value or nil
// InfoType constants (matching Windows MUSHClient):
//   1=name  2=author  3=description  4=script  5=language  6=source
//   7=id    8=purpose 9=triggerCount 10=aliasCount 11=timerCount
//   12=variableCount  16=scriptingEnabled  17=enabled
//   18=requiredVersion  19=version  20=directory
static int L_GetPluginInfo(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id   = luaL_checkstring(L, 1);
    int         info = (int)luaL_checknumber(L, 2);

    CPlugin* found = nullptr;
    for (auto* p : pDoc->m_PluginList) {
        if (strcmp((const char*)p->m_strID, id) == 0) { found = p; break; }
    }
    if (!found) { lua_pushnil(L); return 1; }

    switch (info) {
        case  1: lua_pushstring(L,  (const char*)found->m_strName);        break;
        case  2: lua_pushstring(L,  (const char*)found->m_strAuthor);      break;
        case  3: lua_pushstring(L,  (const char*)found->m_strDescription); break;
        case  4: lua_pushstring(L,  (const char*)found->m_strScript);      break;
        case  5: lua_pushstring(L,  (const char*)found->m_strLanguage);    break;
        case  6: lua_pushstring(L,  (const char*)found->m_strSource);      break;
        case  7: lua_pushstring(L,  (const char*)found->m_strID);          break;
        case  8: lua_pushstring(L,  (const char*)found->m_strPurpose);     break;
        case  9: lua_pushnumber(L,  (double)found->m_TriggerMap.GetCount()); break;
        case 10: lua_pushnumber(L,  (double)found->m_AliasMap.GetCount());   break;
        case 11: lua_pushnumber(L,  (double)found->m_TimerMap.GetCount());   break;
        case 12: lua_pushnumber(L,  (double)found->m_VariableMap.GetCount()); break;
        case 16: lua_pushboolean(L, found->m_ScriptEngine != nullptr ? 1 : 0); break;
        case 17: lua_pushboolean(L, found->m_bEnabled ? 1 : 0);            break;
        case 18: lua_pushnumber(L,  found->m_dRequiredVersion);             break;
        case 19: lua_pushnumber(L,  found->m_dVersion);                    break;
        case 20: lua_pushstring(L,  (const char*)found->m_strDirectory);   break;
        default: lua_pushnil(L); break;
    }
    return 1;
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
static int L_CallPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id      = luaL_checkstring(L, 1);
    const char* routine = luaL_checkstring(L, 2);
    const char* arg     = luaL_optstring(L, 3, "");

    // Find the target plugin
    CPlugin* found = nullptr;
    for (auto* p : pDoc->m_PluginList) {
        if (strcmp((const char*)p->m_strID, id) == 0) { found = p; break; }
    }
    if (!found) { lua_pushnumber(L, eNoSuchPlugin);  return 1; }
    if (!found->m_bEnabled)   { lua_pushnumber(L, ePluginDisabled); return 1; }
    if (!found->m_ScriptEngine) { lua_pushnumber(L, eNoSuchRoutine); return 1; }

    CString strRoutine(routine);
    DISPID dispid = found->m_ScriptEngine->GetLuaDispid(strRoutine);
    if (dispid == DISPID_UNKNOWN) { lua_pushnumber(L, eNoSuchRoutine); return 1; }

    // Switch current-plugin context so the callee can introspect itself
    CPlugin* pSaved = pDoc->m_CurrentPlugin;
    pDoc->m_CurrentPlugin = found;

    list<double> nparams;
    list<string> sparams = { string(arg) };
    long nInvocations = 0;
    found->m_ScriptEngine->ExecuteLua(dispid, strRoutine,
                                       eDontChangeAction,
                                       CString("Plugin ") + found->m_strName,
                                       CString("CallPlugin -> ") + strRoutine,
                                       nparams, sparams,
                                       nInvocations);

    pDoc->m_CurrentPlugin = pSaved;
    lua_pushnumber(L, eOK);
    return 1;
}

// world.BroadcastPlugin(message, text) → count of plugins that handled it
// Fires OnPluginBroadcast(message, senderID, senderName, text) in every
// enabled plugin *except* the caller.
static int L_BroadcastPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long        message = (long)luaL_checknumber(L, 1);
    const char* text    = luaL_optstring(L, 2, "");

    CPlugin* pCaller = pDoc->m_CurrentPlugin;  // nullptr if called from world script
    CString  callerID, callerName;
    if (pCaller) {
        callerID   = pCaller->m_strID;
        callerName = pCaller->m_strName;
    }

    // ON_PLUGIN_BROADCAST constant
    static const CString kBroadcast("OnPluginBroadcast");
    long count = 0;

    CPlugin* pSaved = pDoc->m_CurrentPlugin;
    for (auto* p : pDoc->m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        if (p == pCaller) continue;  // don't broadcast to self

        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(kBroadcast);
        if (dispid == DISPID_UNKNOWN) continue;

        pDoc->m_CurrentPlugin = p;
        list<double> nparams = { (double)message };
        list<string> sparams = { string((const char*)callerID),
                                  string((const char*)callerName),
                                  string(text) };
        long nInvocations = 0;
        p->m_ScriptEngine->ExecuteLua(dispid, kBroadcast,
                                       eDontChangeAction,
                                       CString("Plugin ") + p->m_strName,
                                       "BroadcastPlugin",
                                       nparams, sparams,
                                       nInvocations);
        ++count;
    }
    pDoc->m_CurrentPlugin = pSaved;

    lua_pushnumber(L, count);
    return 1;
}

// world.PluginSupports(pluginID, routineName) → true/false
// Returns true if the named plugin has a function with the given name.
static int L_PluginSupports(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id      = luaL_checkstring(L, 1);
    const char* routine = luaL_checkstring(L, 2);

    for (auto* p : pDoc->m_PluginList) {
        if (strcmp((const char*)p->m_strID, id) != 0) continue;
        if (!p->m_bEnabled || !p->m_ScriptEngine) {
            lua_pushboolean(L, 0); return 1;
        }
        DISPID d = p->m_ScriptEngine->GetLuaDispid(CString(routine));
        lua_pushboolean(L, d != DISPID_UNKNOWN ? 1 : 0);
        return 1;
    }
    lua_pushboolean(L, 0);
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
// Same three-tier strategy as L_GetOption / L_SetOption (see above).

// Macro: push a live CString member and return.
#define OPT_GET_STR(field) do { lua_pushstring(L, (const char*)(field)); return 1; } while(0)

// world.GetAlphaOption(option_name) → string value or ""
static int L_GetAlphaOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* opt = luaL_checkstring(L, 1);

    // 1. Known live members.
    if      (!strcmp(opt, "name"))             OPT_GET_STR(pDoc->m_mush_name);
    else if (!strcmp(opt, "world_name"))       OPT_GET_STR(pDoc->m_mush_name);
    else if (!strcmp(opt, "site"))             OPT_GET_STR(pDoc->m_server);
    else if (!strcmp(opt, "player"))           OPT_GET_STR(pDoc->m_name);
    else if (!strcmp(opt, "password"))         OPT_GET_STR(pDoc->m_password);
    else if (!strcmp(opt, "script_filename"))  OPT_GET_STR(pDoc->m_strScriptFilename);
    else if (!strcmp(opt, "script_language"))  OPT_GET_STR(pDoc->m_strLanguage);
    else if (!strcmp(opt, "script_prefix"))    OPT_GET_STR(pDoc->m_strScriptPrefix);
    else if (!strcmp(opt, "notes"))            OPT_GET_STR(pDoc->m_notes);
    else if (!strcmp(opt, "terminal_identification")) OPT_GET_STR(pDoc->m_strTerminalIdentification);
    else if (!strcmp(opt, "mapping_failure"))  OPT_GET_STR(pDoc->m_strMappingFailure);
    // Connection text
    else if (!strcmp(opt, "connect_text"))     OPT_GET_STR(pDoc->m_connect_text);
    else if (!strcmp(opt, "auto_log_file_name")) OPT_GET_STR(pDoc->m_strAutoLogFileName);
    // Speed walk
    else if (!strcmp(opt, "speed_walk_prefix"))  OPT_GET_STR(pDoc->m_speed_walk_prefix);
    else if (!strcmp(opt, "speed_walk_filler"))  OPT_GET_STR(pDoc->m_strSpeedWalkFiller);
    // World ID
    else if (!strcmp(opt, "id"))               OPT_GET_STR(pDoc->m_strWorldID);
    // World callbacks
    else if (!strcmp(opt, "on_world_open"))       OPT_GET_STR(pDoc->m_strWorldOpen);
    else if (!strcmp(opt, "on_world_close"))      OPT_GET_STR(pDoc->m_strWorldClose);
    else if (!strcmp(opt, "on_world_save"))       OPT_GET_STR(pDoc->m_strWorldSave);
    else if (!strcmp(opt, "on_world_connect"))    OPT_GET_STR(pDoc->m_strWorldConnect);
    else if (!strcmp(opt, "on_world_disconnect")) OPT_GET_STR(pDoc->m_strWorldDisconnect);
    else if (!strcmp(opt, "on_world_get_focus"))  OPT_GET_STR(pDoc->m_strWorldGetFocus);
    else if (!strcmp(opt, "on_world_lose_focus")) OPT_GET_STR(pDoc->m_strWorldLoseFocus);
    // MXP callbacks
    else if (!strcmp(opt, "on_mxp_start"))        OPT_GET_STR(pDoc->m_strOnMXP_Start);
    else if (!strcmp(opt, "on_mxp_stop"))         OPT_GET_STR(pDoc->m_strOnMXP_Stop);
    else if (!strcmp(opt, "on_mxp_error"))        OPT_GET_STR(pDoc->m_strOnMXP_Error);
    else if (!strcmp(opt, "on_mxp_open_tag"))     OPT_GET_STR(pDoc->m_strOnMXP_OpenTag);
    else if (!strcmp(opt, "on_mxp_close_tag"))    OPT_GET_STR(pDoc->m_strOnMXP_CloseTag);
    else if (!strcmp(opt, "on_mxp_set_variable")) OPT_GET_STR(pDoc->m_strOnMXP_SetVariable);
    // Log preambles/postambles
    else if (!strcmp(opt, "log_file_preamble"))         OPT_GET_STR(pDoc->m_strLogFilePreamble);
    else if (!strcmp(opt, "log_file_postamble"))        OPT_GET_STR(pDoc->m_strLogFilePostamble);
    else if (!strcmp(opt, "log_line_preamble_input"))   OPT_GET_STR(pDoc->m_strLogLinePreambleInput);
    else if (!strcmp(opt, "log_line_preamble_notes"))   OPT_GET_STR(pDoc->m_strLogLinePreambleNotes);
    else if (!strcmp(opt, "log_line_preamble_output"))  OPT_GET_STR(pDoc->m_strLogLinePreambleOutput);
    else if (!strcmp(opt, "log_line_postamble_input"))  OPT_GET_STR(pDoc->m_strLogLinePostambleInput);
    else if (!strcmp(opt, "log_line_postamble_notes"))  OPT_GET_STR(pDoc->m_strLogLinePostambleNotes);
    else if (!strcmp(opt, "log_line_postamble_output")) OPT_GET_STR(pDoc->m_strLogLinePostambleOutput);

    // 2. Cache — value previously written by SetAlphaOption.
    {
        auto it = pDoc->m_alphaOptionCache.find(opt);
        if (it != pDoc->m_alphaOptionCache.end()) {
            lua_pushstring(L, it->second.c_str());
            return 1;
        }
    }

    // 3. AlphaOptionsTable default (nullptr means unknown option → return "").
    const char* def = opt_alpha_default(opt);
    lua_pushstring(L, def ? def : "");
    return 1;
}

#undef OPT_GET_STR

// world.SetAlphaOption(option_name, value) → error_code
static int L_SetAlphaOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* opt = luaL_checkstring(L, 1);
    const char* val = luaL_checkstring(L, 2);

    // 1. Known live members — update the real field.
    bool is_member = true;
    if      (!strcmp(opt, "name") || !strcmp(opt, "world_name"))
                                               pDoc->m_mush_name            = CString(val);
    else if (!strcmp(opt, "site"))             pDoc->m_server               = CString(val);
    else if (!strcmp(opt, "player"))           pDoc->m_name                 = CString(val);
    else if (!strcmp(opt, "password"))         pDoc->m_password             = CString(val);
    else if (!strcmp(opt, "script_filename"))  pDoc->m_strScriptFilename    = CString(val);
    else if (!strcmp(opt, "script_language"))  pDoc->m_strLanguage          = CString(val);
    else if (!strcmp(opt, "script_prefix"))    pDoc->m_strScriptPrefix      = CString(val);
    else if (!strcmp(opt, "notes"))            pDoc->m_notes                = CString(val);
    else if (!strcmp(opt, "terminal_identification"))
                                               pDoc->m_strTerminalIdentification = CString(val);
    else if (!strcmp(opt, "mapping_failure"))  pDoc->m_strMappingFailure    = CString(val);
    // Connection text
    else if (!strcmp(opt, "connect_text"))     pDoc->m_connect_text         = CString(val);
    else if (!strcmp(opt, "auto_log_file_name")) pDoc->m_strAutoLogFileName = CString(val);
    // Speed walk
    else if (!strcmp(opt, "speed_walk_prefix"))  pDoc->m_speed_walk_prefix  = CString(val);
    else if (!strcmp(opt, "speed_walk_filler"))  pDoc->m_strSpeedWalkFiller = CString(val);
    // World ID
    else if (!strcmp(opt, "id"))               pDoc->m_strWorldID           = CString(val);
    // World callbacks
    else if (!strcmp(opt, "on_world_open"))       pDoc->m_strWorldOpen       = CString(val);
    else if (!strcmp(opt, "on_world_close"))      pDoc->m_strWorldClose      = CString(val);
    else if (!strcmp(opt, "on_world_save"))       pDoc->m_strWorldSave       = CString(val);
    else if (!strcmp(opt, "on_world_connect"))    pDoc->m_strWorldConnect    = CString(val);
    else if (!strcmp(opt, "on_world_disconnect")) pDoc->m_strWorldDisconnect = CString(val);
    else if (!strcmp(opt, "on_world_get_focus"))  pDoc->m_strWorldGetFocus   = CString(val);
    else if (!strcmp(opt, "on_world_lose_focus")) pDoc->m_strWorldLoseFocus  = CString(val);
    // MXP callbacks
    else if (!strcmp(opt, "on_mxp_start"))        pDoc->m_strOnMXP_Start       = CString(val);
    else if (!strcmp(opt, "on_mxp_stop"))         pDoc->m_strOnMXP_Stop        = CString(val);
    else if (!strcmp(opt, "on_mxp_error"))        pDoc->m_strOnMXP_Error       = CString(val);
    else if (!strcmp(opt, "on_mxp_open_tag"))     pDoc->m_strOnMXP_OpenTag     = CString(val);
    else if (!strcmp(opt, "on_mxp_close_tag"))    pDoc->m_strOnMXP_CloseTag    = CString(val);
    else if (!strcmp(opt, "on_mxp_set_variable")) pDoc->m_strOnMXP_SetVariable = CString(val);
    // Log preambles/postambles
    else if (!strcmp(opt, "log_file_preamble"))         pDoc->m_strLogFilePreamble        = CString(val);
    else if (!strcmp(opt, "log_file_postamble"))        pDoc->m_strLogFilePostamble       = CString(val);
    else if (!strcmp(opt, "log_line_preamble_input"))   pDoc->m_strLogLinePreambleInput   = CString(val);
    else if (!strcmp(opt, "log_line_preamble_notes"))   pDoc->m_strLogLinePreambleNotes   = CString(val);
    else if (!strcmp(opt, "log_line_preamble_output"))  pDoc->m_strLogLinePreambleOutput  = CString(val);
    else if (!strcmp(opt, "log_line_postamble_input"))  pDoc->m_strLogLinePostambleInput  = CString(val);
    else if (!strcmp(opt, "log_line_postamble_notes"))  pDoc->m_strLogLinePostambleNotes  = CString(val);
    else if (!strcmp(opt, "log_line_postamble_output")) pDoc->m_strLogLinePostambleOutput = CString(val);
    else if (!strcmp(opt, "output_font_name")) {
        // Immediately apply the face name to the output pane.
        if (pDoc->m_fn_set_output_font && val && *val)
            pDoc->m_fn_set_output_font(std::string(val), 0, false, false);
    }
    else                                       is_member = false;

    // 2. Validate: reject completely unknown option names.
    if (!is_member && opt_alpha_default(opt) == nullptr) {
        lua_pushnumber(L, eUnknownOption);
        return 1;
    }

    // 3. Store in cache.
    pDoc->m_alphaOptionCache[opt] = val;
    lua_pushnumber(L, eOK);
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
    lua_pushnumber(L, (lua_Number)(long)pDoc->m_NoteColourFore);
    return 1;
}

static int L_GetNoteColourBack(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushnumber(L, (lua_Number)(long)pDoc->m_NoteColourBack);
    return 1;
}

static int L_SetNoteColourFore(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_NoteColourFore = (COLORREF)(long)luaL_checknumber(L, 1);
    return 0;
}

static int L_SetNoteColourBack(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_NoteColourBack = (COLORREF)(long)luaL_checknumber(L, 1);
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
    long count = pDoc->m_LineList.GetCount();
    long toDelete = (n > count) ? count : n;
    for (long i = 0; i < toDelete; ++i) {
        POSITION pos = pDoc->m_LineList.GetTailPosition();
        if (!pos) break;
        CLine* pLine = pDoc->m_LineList.GetAt(pos);
        delete pLine;
        pDoc->m_LineList.RemoveTail();
    }
    return 0;
}

static int L_DeleteOutput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    POSITION pos = pDoc->m_LineList.GetHeadPosition();
    while (pos) {
        CLine* pLine = pDoc->m_LineList.GetNext(pos);
        delete pLine;
    }
    pDoc->m_LineList.RemoveAll();
    return 0;
}

// ── GetConnectDuration ────────────────────────────────────────────────────────

static int L_GetConnectDuration(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_iConnectPhase != eConnectConnectedToMud) {
        lua_pushnumber(L, 0);
        return 1;
    }
    CTimeSpan ts = CTime::GetCurrentTime() - pDoc->m_tConnectTime;
    lua_pushnumber(L, (lua_Number)ts.GetTotalSeconds());
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
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushstring(L, (const char*)pDoc->m_notes);
    return 1;
}

static int L_SetNotes(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_notes = CString(luaL_checkstring(L, 1));
    return 0;
}

// ── CreateGUID / GetUniqueID ──────────────────────────────────────────────────
// CreateGUID  → "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
// GetUniqueID → SHA-1 hash of a GUID (no hyphens, 40 hex chars → first 5 words)

static int L_CreateGUID(lua_State* L)
{
    world_doc(L);
    // Read from the kernel's UUID source (Linux) or fall back to /dev/urandom
    char buf[37] = {};
    FILE* f = fopen("/proc/sys/kernel/random/uuid", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            // strip trailing newline
            size_t len = strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        }
        fclose(f);
    } else {
        // fallback: 16 random bytes formatted as UUID v4
        unsigned char r[16] = {};
        FILE* u = fopen("/dev/urandom", "rb");
        if (u) { fread(r, 1, 16, u); fclose(u); }
        r[6] = (r[6] & 0x0F) | 0x40;  // version 4
        r[8] = (r[8] & 0x3F) | 0x80;  // variant 1
        snprintf(buf, sizeof(buf),
                 "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 r[0],r[1],r[2],r[3], r[4],r[5], r[6],r[7],
                 r[8],r[9], r[10],r[11],r[12],r[13],r[14],r[15]);
    }
    // uppercase to match Windows behaviour
    for (char* p = buf; *p; ++p) *p = (char)toupper((unsigned char)*p);
    lua_pushstring(L, buf);
    return 1;
}

static int L_GetUniqueID(lua_State* L)
{
    world_doc(L);
    // SHA-1 of a fresh GUID (no hyphens), return first 40 hex chars
    unsigned char r[16] = {};
    FILE* u = fopen("/dev/urandom", "rb");
    if (u) { fread(r, 1, 16, u); fclose(u); }
    char raw[33];
    snprintf(raw, sizeof(raw),
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],
             r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15]);
    SHS_INFO sha;
    shsInit(&sha);
    shsUpdate(&sha, (BYTE*)raw, 32);
    shsFinal(&sha);
    char out[41];
    snprintf(out, sizeof(out), "%08lx%08lx%08lx%08lx%08lx",
             sha.digest[0], sha.digest[1], sha.digest[2],
             sha.digest[3], sha.digest[4]);
    lua_pushstring(L, out);
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
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* grp = luaL_checkstring(L, 1);
    long count = 0;
    { // triggers
        CString name; CTrigger* t;
        for (POSITION pos = pDoc->GetTriggerMap().GetStartPosition(); pos; ) {
            pDoc->GetTriggerMap().GetNextAssoc(pos, name, t);
            if (t->strGroup == CString(grp)) {
                delete t;
                pDoc->GetTriggerMap().RemoveKey(name);
                ++count;
            }
        }
        if (count) pDoc->SortTriggers();
    }
    { // aliases
        CString name; CAlias* a;
        long ac = 0;
        for (POSITION pos = pDoc->GetAliasMap().GetStartPosition(); pos; ) {
            pDoc->GetAliasMap().GetNextAssoc(pos, name, a);
            if (a->strGroup == CString(grp)) {
                delete a;
                pDoc->GetAliasMap().RemoveKey(name);
                ++ac; ++count;
            }
        }
        if (ac) pDoc->SortAliases();
    }
    { // timers
        CString name; CTimer* t;
        long tc = 0;
        for (POSITION pos = pDoc->GetTimerMap().GetStartPosition(); pos; ) {
            pDoc->GetTimerMap().GetNextAssoc(pos, name, t);
            if (t->strGroup == CString(grp)) {
                delete t;
                pDoc->GetTimerMap().RemoveKey(name);
                ++tc; ++count;
            }
        }
        if (tc) pDoc->SortTimers();
    }
    lua_pushnumber(L, (lua_Number)count);
    return 1;
}

// ── GetTriggerOption / SetTriggerOption ───────────────────────────────────────

static void push_trigger_option(lua_State* L, CTrigger* t, const char* option)
{
    string opt(option);
    for (auto& c : opt) c = (char)tolower((unsigned char)c);
    // boolean options
    if (opt=="enabled")              { lua_pushboolean(L, t->bEnabled);          return; }
    if (opt=="expand_variables")     { lua_pushboolean(L, t->bExpandVariables);  return; }
    if (opt=="ignore_case")          { lua_pushboolean(L, t->ignore_case);       return; }
    if (opt=="keep_evaluating")      { lua_pushboolean(L, t->bKeepEvaluating);   return; }
    if (opt=="multi_line")           { lua_pushboolean(L, t->bMultiLine);        return; }
    if (opt=="omit_from_log")        { lua_pushboolean(L, t->omit_from_log);     return; }
    if (opt=="omit_from_output")     { lua_pushboolean(L, t->bOmitFromOutput);   return; }
    if (opt=="regexp")               { lua_pushboolean(L, t->bRegexp);           return; }
    if (opt=="repeat")               { lua_pushboolean(L, t->bRepeat);           return; }
    if (opt=="sound_if_inactive")    { lua_pushboolean(L, t->bSoundIfInactive);  return; }
    if (opt=="lowercase_wildcard")   { lua_pushboolean(L, t->bLowercaseWildcard);return; }
    if (opt=="temporary")            { lua_pushboolean(L, t->bTemporary);        return; }
    if (opt=="one_shot")             { lua_pushboolean(L, t->bOneShot);          return; }
    // numeric options
    if (opt=="clipboard_arg")        { lua_pushnumber(L, t->iClipboardArg);      return; }
    if (opt=="colour_change_type")   { lua_pushnumber(L, t->iColourChangeType);  return; }
    if (opt=="custom_colour")        { lua_pushnumber(L, t->colour);             return; }
    if (opt=="lines_to_match")       { lua_pushnumber(L, t->iLinesToMatch);      return; }
    if (opt=="match_style")          { lua_pushnumber(L, t->iMatch);             return; }
    if (opt=="new_style")            { lua_pushnumber(L, t->iStyle);             return; }
    if (opt=="other_text_colour")    { lua_pushnumber(L, t->iOtherForeground);   return; }
    if (opt=="other_back_colour")    { lua_pushnumber(L, t->iOtherBackground);   return; }
    if (opt=="send_to")              { lua_pushnumber(L, t->iSendTo);            return; }
    if (opt=="sequence")             { lua_pushnumber(L, t->iSequence);          return; }
    if (opt=="user")                 { lua_pushnumber(L, t->iUserOption);        return; }
    // string options
    if (opt=="group")    { lua_pushstring(L, (const char*)t->strGroup);    return; }
    if (opt=="match")    { lua_pushstring(L, (const char*)t->trigger);     return; }
    if (opt=="script")   { lua_pushstring(L, (const char*)t->strProcedure);return; }
    if (opt=="sound")    { lua_pushstring(L, (const char*)t->sound_to_play);return; }
    if (opt=="send")     { lua_pushstring(L, (const char*)t->contents);    return; }
    if (opt=="variable") { lua_pushstring(L, (const char*)t->strVariable); return; }
    lua_pushnil(L);
}

static int L_GetTriggerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name   = luaL_checkstring(L, 1);
    const char* option = luaL_checkstring(L, 2);
    CTrigger* t = nullptr;
    CString key(name);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetTriggerMap().Lookup(key, t) || !t) { lua_pushnil(L); return 1; }
    push_trigger_option(L, t, option);
    return 1;
}

static int L_SetTriggerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name   = luaL_checkstring(L, 1);
    const char* option = luaL_checkstring(L, 2);
    const char* value  = luaL_checkstring(L, 3);
    CTrigger* t = nullptr;
    CString key(name);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetTriggerMap().Lookup(key, t) || !t) {
        lua_pushnumber(L, eTriggerNotFound); return 1;
    }
    string opt(option), val(value);
    for (auto& c : opt) c = (char)tolower((unsigned char)c);
    bool bval = (val == "1" || val == "y" || val == "Y");
    // read-only options (OPT_CANNOT_WRITE in Windows) → ePluginCannotSetOption
    if (opt=="ignore_case" || opt=="regexp")
        { lua_pushnumber(L, ePluginCannotSetOption); return 1; }
    // boolean options
    if (opt=="enabled")             { t->bEnabled = bval; }
    else if (opt=="expand_variables")  { t->bExpandVariables = bval; }
    else if (opt=="keep_evaluating")   { t->bKeepEvaluating = bval; }
    else if (opt=="multi_line")        { t->bMultiLine = bval; }
    else if (opt=="omit_from_log")     { t->omit_from_log = bval; }
    else if (opt=="omit_from_output")  { t->bOmitFromOutput = bval; }
    else if (opt=="repeat")            { t->bRepeat = bval; }
    else if (opt=="sound_if_inactive") { t->bSoundIfInactive = bval; }
    else if (opt=="lowercase_wildcard"){ t->bLowercaseWildcard = bval; }
    else if (opt=="temporary")         { t->bTemporary = bval; }
    else if (opt=="one_shot")          { t->bOneShot = bval; }
    // numeric options
    else if (opt=="clipboard_arg")     { t->iClipboardArg = (unsigned short)atoi(value); }
    else if (opt=="colour_change_type"){ t->iColourChangeType = (unsigned short)atoi(value); }
    else if (opt=="custom_colour")     { t->colour = (unsigned short)atoi(value); }
    else if (opt=="lines_to_match")    { t->iLinesToMatch = (unsigned short)atoi(value); }
    else if (opt=="match_style")       { t->iMatch = (unsigned short)atoi(value); }
    else if (opt=="new_style")         { t->iStyle = (unsigned short)atoi(value); }
    else if (opt=="other_text_colour") { t->iOtherForeground = (COLORREF)atol(value); }
    else if (opt=="other_back_colour") { t->iOtherBackground = (COLORREF)atol(value); }
    else if (opt=="send_to")           { t->iSendTo = (unsigned short)atoi(value); }
    else if (opt=="sequence")          { t->iSequence = atoi(value); }
    else if (opt=="user")              { t->iUserOption = atol(value); }
    // string options
    else if (opt=="group")             { t->strGroup = CString(value); }
    else if (opt=="script")            { t->strProcedure = CString(value); }
    else if (opt=="sound")             { t->sound_to_play = CString(value); }
    else if (opt=="send")              { t->contents = CString(value); }
    else if (opt=="variable")          { t->strVariable = CString(value); }
    else { lua_pushnumber(L, eUnknownOption); return 1; }
    lua_pushnumber(L, eOK);
    return 1;
}

// ── GetAliasOption / SetAliasOption ───────────────────────────────────────────

static void push_alias_option(lua_State* L, CAlias* a, const char* option)
{
    string opt(option);
    for (auto& c : opt) c = (char)tolower((unsigned char)c);
    if (opt=="enabled")                   { lua_pushboolean(L, a->bEnabled);                  return; }
    if (opt=="expand_variables")          { lua_pushboolean(L, a->bExpandVariables);           return; }
    if (opt=="ignore_case")               { lua_pushboolean(L, a->bIgnoreCase);                return; }
    if (opt=="keep_evaluating")           { lua_pushboolean(L, a->bKeepEvaluating);            return; }
    if (opt=="omit_from_log")             { lua_pushboolean(L, a->bOmitFromLog);               return; }
    if (opt=="omit_from_command_history") { lua_pushboolean(L, a->bOmitFromCommandHistory);    return; }
    if (opt=="omit_from_output")          { lua_pushboolean(L, a->bOmitFromOutput);            return; }
    if (opt=="regexp")                    { lua_pushboolean(L, a->bRegexp);                    return; }
    if (opt=="echo_alias")                { lua_pushboolean(L, a->bEchoAlias);                 return; }
    if (opt=="temporary")                 { lua_pushboolean(L, a->bTemporary);                 return; }
    if (opt=="one_shot")                  { lua_pushboolean(L, a->bOneShot);                   return; }
    if (opt=="menu")                      { lua_pushboolean(L, a->bMenu);                      return; }
    if (opt=="send_to")                   { lua_pushnumber(L, a->iSendTo);                     return; }
    if (opt=="sequence")                  { lua_pushnumber(L, a->iSequence);                   return; }
    if (opt=="user")                      { lua_pushnumber(L, a->iUserOption);                 return; }
    if (opt=="group")    { lua_pushstring(L, (const char*)a->strGroup);    return; }
    if (opt=="match")    { lua_pushstring(L, (const char*)a->name);        return; }
    if (opt=="script")   { lua_pushstring(L, (const char*)a->strProcedure);return; }
    if (opt=="send")     { lua_pushstring(L, (const char*)a->contents);    return; }
    if (opt=="variable") { lua_pushstring(L, (const char*)a->strVariable); return; }
    lua_pushnil(L);
}

static int L_GetAliasOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name   = luaL_checkstring(L, 1);
    const char* option = luaL_checkstring(L, 2);
    CAlias* a = nullptr;
    CString key(name);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetAliasMap().Lookup(key, a) || !a) { lua_pushnil(L); return 1; }
    push_alias_option(L, a, option);
    return 1;
}

static int L_SetAliasOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name   = luaL_checkstring(L, 1);
    const char* option = luaL_checkstring(L, 2);
    const char* value  = luaL_checkstring(L, 3);
    CAlias* a = nullptr;
    CString key(name);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetAliasMap().Lookup(key, a) || !a) {
        lua_pushnumber(L, eAliasNotFound); return 1;
    }
    string opt(option);
    for (auto& c : opt) c = (char)tolower((unsigned char)c);
    bool bval = (strcmp(value,"1")==0 || strcmp(value,"y")==0 || strcmp(value,"Y")==0);
    // read-only options
    if (opt=="ignore_case" || opt=="regexp")
        { lua_pushnumber(L, ePluginCannotSetOption); return 1; }
    // boolean options
    if (opt=="enabled")                    { a->bEnabled = bval; }
    else if (opt=="expand_variables")          { a->bExpandVariables = bval; }
    else if (opt=="keep_evaluating")           { a->bKeepEvaluating = bval; }
    else if (opt=="omit_from_log")             { a->bOmitFromLog = bval; }
    else if (opt=="omit_from_command_history") { a->bOmitFromCommandHistory = bval; }
    else if (opt=="omit_from_output")          { a->bOmitFromOutput = bval; }
    else if (opt=="echo_alias")                { a->bEchoAlias = bval; }
    else if (opt=="temporary")                 { a->bTemporary = bval; }
    else if (opt=="one_shot")                  { a->bOneShot = bval; }
    else if (opt=="menu")                      { a->bMenu = bval; }
    // numeric options
    else if (opt=="send_to")                   { a->iSendTo = (unsigned short)atoi(value); }
    else if (opt=="sequence")                  { a->iSequence = atoi(value); }
    else if (opt=="user")                      { a->iUserOption = atol(value); }
    // string options
    else if (opt=="group")                     { a->strGroup = CString(value); }
    else if (opt=="script")                    { a->strProcedure = CString(value); }
    else if (opt=="send")                      { a->contents = CString(value); }
    else if (opt=="variable")                  { a->strVariable = CString(value); }
    else { lua_pushnumber(L, eUnknownOption); return 1; }
    lua_pushnumber(L, eOK);
    return 1;
}

// ── GetTimerOption / SetTimerOption ───────────────────────────────────────────

static void push_timer_option(lua_State* L, CTimer* t, const char* option)
{
    string opt(option);
    for (auto& c : opt) c = (char)tolower((unsigned char)c);
    bool atTime = (t->iType == CTimer::eAtTime);
    if (opt=="enabled")          { lua_pushboolean(L, t->bEnabled);         return; }
    if (opt=="at_time")          { lua_pushboolean(L, atTime);              return; }
    if (opt=="one_shot")         { lua_pushboolean(L, t->bOneShot);         return; }
    if (opt=="omit_from_output") { lua_pushboolean(L, t->bOmitFromOutput);  return; }
    if (opt=="omit_from_log")    { lua_pushboolean(L, t->bOmitFromLog);     return; }
    if (opt=="active_closed")    { lua_pushboolean(L, t->bActiveWhenClosed);return; }
    if (opt=="temporary")        { lua_pushboolean(L, t->bTemporary);       return; }
    if (opt=="send_to")          { lua_pushnumber(L, t->iSendTo);           return; }
    if (opt=="user")             { lua_pushnumber(L, t->iUserOption);       return; }
    if (opt=="hour")          { lua_pushnumber(L, atTime ? t->iAtHour    : t->iEveryHour);    return; }
    if (opt=="minute")        { lua_pushnumber(L, atTime ? t->iAtMinute  : t->iEveryMinute);  return; }
    if (opt=="second")        { lua_pushnumber(L, atTime ? t->fAtSecond  : t->fEverySecond);  return; }
    if (opt=="offset_hour")   { lua_pushnumber(L, t->iOffsetHour);          return; }
    if (opt=="offset_minute") { lua_pushnumber(L, t->iOffsetMinute);        return; }
    if (opt=="offset_second") { lua_pushnumber(L, t->fOffsetSecond);        return; }
    if (opt=="group")    { lua_pushstring(L, (const char*)t->strGroup);     return; }
    if (opt=="script")   { lua_pushstring(L, (const char*)t->strProcedure); return; }
    if (opt=="send")     { lua_pushstring(L, (const char*)t->strContents);  return; }
    if (opt=="variable") { lua_pushstring(L, (const char*)t->strVariable);  return; }
    lua_pushnil(L);
}

static int L_GetTimerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name   = luaL_checkstring(L, 1);
    const char* option = luaL_checkstring(L, 2);
    CTimer* t = nullptr;
    CString key(name);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetTimerMap().Lookup(key, t) || !t) { lua_pushnil(L); return 1; }
    push_timer_option(L, t, option);
    return 1;
}

static int L_SetTimerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name   = luaL_checkstring(L, 1);
    const char* option = luaL_checkstring(L, 2);
    const char* value  = luaL_checkstring(L, 3);
    CTimer* t = nullptr;
    CString key(name);
    key.MakeLower(); key.TrimLeft(); key.TrimRight();
    if (!pDoc->GetTimerMap().Lookup(key, t) || !t) {
        lua_pushnumber(L, eTimerNotFound); return 1;
    }
    string opt(option);
    for (auto& c : opt) c = (char)tolower((unsigned char)c);
    bool bval = (strcmp(value,"1")==0 || strcmp(value,"y")==0 || strcmp(value,"Y")==0);
    bool atTime = (t->iType == CTimer::eAtTime);
    if (opt=="enabled")            { t->bEnabled = bval; }
    else if (opt=="one_shot")      { t->bOneShot = bval; }
    else if (opt=="omit_from_output"){ t->bOmitFromOutput = bval; }
    else if (opt=="omit_from_log") { t->bOmitFromLog = bval; }
    else if (opt=="active_closed") { t->bActiveWhenClosed = bval; }
    else if (opt=="temporary")     { t->bTemporary = bval; }
    else if (opt=="send_to")       { t->iSendTo = (unsigned short)atoi(value); }
    else if (opt=="user")          { t->iUserOption = atol(value); }
    else if (opt=="hour")          { if (atTime) t->iAtHour    = atoi(value); else t->iEveryHour    = atoi(value); }
    else if (opt=="minute")        { if (atTime) t->iAtMinute  = atoi(value); else t->iEveryMinute  = atoi(value); }
    else if (opt=="second")        { if (atTime) t->fAtSecond  = atof(value); else t->fEverySecond  = atof(value); }
    else if (opt=="offset_hour")   { t->iOffsetHour   = atoi(value); }
    else if (opt=="offset_minute") { t->iOffsetMinute = atoi(value); }
    else if (opt=="offset_second") { t->fOffsetSecond = atof(value); }
    else if (opt=="group")         { t->strGroup     = CString(value); }
    else if (opt=="script")        { t->strProcedure = CString(value); }
    else if (opt=="send")          { t->strContents  = CString(value); }
    else if (opt=="variable")      { t->strVariable  = CString(value); }
    else { lua_pushnumber(L, eUnknownOption); return 1; }
    lua_pushnumber(L, eOK);
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
// Minimal portable stub — outputs the text as a note (no clickable links yet).

static int L_Hyperlink(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    // action, text, hint, fore, back, url, no_underline
    const char* text = luaL_optstring(L, 2, "");
    if (!text || text[0] == 0)
        text = luaL_optstring(L, 1, "");  // fall back to action
    pDoc->Note(CString(text));
    return 0;
}

// ── OpenBrowser ───────────────────────────────────────────────────────────────

static int L_OpenBrowser(lua_State* L)
{
    world_doc(L);
    const char* url = luaL_checkstring(L, 1);
    // Only allow http/https/mailto
    if (strncmp(url, "http://",  7) != 0 &&
        strncmp(url, "https://", 8) != 0 &&
        strncmp(url, "mailto:",  7) != 0)
    {
        lua_pushnumber(L, eBadParameter); return 1;
    }
    // Try xdg-open (Linux) then open (macOS)
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' 2>/dev/null || open '%s' 2>/dev/null &", url, url);
    system(cmd);
    lua_pushnumber(L, eOK);
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
    CMUSHclientDoc* pDoc = world_doc(L);
    // m_iScriptTimeTaken is in QueryPerformanceCounter ticks.
    // App.m_iCounterFrequency is ticks/sec (0 if not available → return 0).
    double elapsed = 0.0;
    if (App.m_iCounterFrequency > 0)
        elapsed = (double)pDoc->m_iScriptTimeTaken / (double)App.m_iCounterFrequency;
    lua_pushnumber(L, elapsed);
    return 1;
}

// ── MakeRegularExpression ─────────────────────────────────────────────────────

static int L_MakeRegularExpression(lua_State* L)
{
    world_doc(L);
    const char* text = luaL_checkstring(L, 1);
    CString result = ConvertToRegularExpression(CString(text));
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

// ── FixupEscapeSequences ──────────────────────────────────────────────────────

static int L_FixupEscapeSequences(lua_State* L)
{
    world_doc(L);
    const char* src = luaL_checkstring(L, 1);
    CString result = FixupEscapeSequences(CString(src));
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

// ── FixupHTML ─────────────────────────────────────────────────────────────────

static int L_FixupHTML(lua_State* L)
{
    world_doc(L);
    const char* src = luaL_checkstring(L, 1);
    CString result = FixHTMLString(CString(src));
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

// ── SetTitle / SetMainTitle ───────────────────────────────────────────────────

static int L_SetTitle(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_optstring(L, 1, "");
    pDoc->m_strWindowTitle = title;
    if (pDoc->m_fn_set_title)
        pDoc->m_fn_set_title(std::string(title));
    return 0;
}

static int L_SetMainTitle(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* title = luaL_optstring(L, 1, "");
    pDoc->m_strMainWindowTitle = title;
    if (pDoc->m_fn_set_main_title)
        pDoc->m_fn_set_main_title(std::string(title));
    return 0;
}

// ── GetHostName / GetHostAddress ──────────────────────────────────────────────

static int L_GetHostName(lua_State* L)
{
    world_doc(L);
    const char* ipAddr = luaL_checkstring(L, 1);
    if (ipAddr && *ipAddr)
    {
        unsigned long ul = inet_addr(ipAddr);
        if (ul != INADDR_NONE)
        {
            struct hostent* he = gethostbyaddr((const char*)&ul, sizeof ul, AF_INET);
            if (he)
            {
                lua_pushstring(L, he->h_name);
                return 1;
            }
        }
    }
    lua_pushstring(L, "");
    return 1;
}

static int L_GetHostAddress(lua_State* L)
{
    world_doc(L);
    const char* hostName = luaL_checkstring(L, 1);
    lua_newtable(L);
    if (hostName && *hostName)
    {
        struct hostent* he = gethostbyname(hostName);
        if (he)
        {
            int i = 1;
            for (; he->h_addr_list[i-1] != nullptr; ++i)
            {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[i-1], sizeof addr);
                lua_pushstring(L, inet_ntoa(addr));
                lua_rawseti(L, -2, i);
            }
        }
    }
    return 1;
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
// Returns a table of queued commands (strings), ordered head-to-tail.

static int L_GetQueue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int i = 1;
    POSITION pos = pDoc->m_QueuedCommandsList.GetHeadPosition();
    while (pos)
    {
        CString cmd = pDoc->m_QueuedCommandsList.GetNext(pos);
        // Each entry has a leading echo-flag character; strip it (match Windows)
        const char* s = static_cast<const char*>(cmd);
        if (s && *s) s++;  // skip echo flag byte
        lua_pushstring(L, s ? s : "");
        lua_rawseti(L, -2, i++);
    }
    return 1;
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

static int L_GetPluginVariableList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    lua_newtable(L);
    CPlugin* p = FindPlugin(pDoc, id);
    if (!p) return 1;
    POSITION pos = p->m_VariableMap.GetStartPosition();
    while (pos) {
        CString k; CVariable* v;
        p->m_VariableMap.GetNextAssoc(pos, k, v);
        if (v) {
            lua_pushstring(L, static_cast<const char*>(v->strContents));
            lua_setfield(L, -2, static_cast<const char*>(k));
        }
    }
    return 1;
}

static int L_GetPluginTriggerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    lua_newtable(L);
    CPlugin* p = FindPlugin(pDoc, id);
    if (!p) return 1;
    int i = 1;
    POSITION pos = p->m_TriggerMap.GetStartPosition();
    while (pos) {
        CString k; CTrigger* v;
        p->m_TriggerMap.GetNextAssoc(pos, k, v);
        lua_pushstring(L, static_cast<const char*>(k));
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int L_GetPluginAliasList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    lua_newtable(L);
    CPlugin* p = FindPlugin(pDoc, id);
    if (!p) return 1;
    int i = 1;
    POSITION pos = p->m_AliasMap.GetStartPosition();
    while (pos) {
        CString k; CAlias* v;
        p->m_AliasMap.GetNextAssoc(pos, k, v);
        lua_pushstring(L, static_cast<const char*>(k));
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int L_GetPluginTimerList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    lua_newtable(L);
    CPlugin* p = FindPlugin(pDoc, id);
    if (!p) return 1;
    int i = 1;
    POSITION pos = p->m_TimerMap.GetStartPosition();
    while (pos) {
        CString k; CTimer* v;
        p->m_TimerMap.GetNextAssoc(pos, k, v);
        lua_pushstring(L, static_cast<const char*>(k));
        lua_rawseti(L, -2, i++);
    }
    return 1;
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

static int L_GetPluginTriggerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    const char* optName  = luaL_checkstring(L, 3);
    CPlugin* p = FindPlugin(pDoc, pluginID);
    if (!p) { lua_pushnil(L); return 1; }
    CTrigger* t = nullptr;
    if (!p->m_TriggerMap.Lookup(CString(itemName), t) || !t)
        { lua_pushnil(L); return 1; }
    push_trigger_option(L, t, optName);
    return 1;
}

static int L_GetPluginAliasOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    const char* optName  = luaL_checkstring(L, 3);
    CPlugin* p = FindPlugin(pDoc, pluginID);
    if (!p) { lua_pushnil(L); return 1; }
    CAlias* a = nullptr;
    if (!p->m_AliasMap.Lookup(CString(itemName), a) || !a)
        { lua_pushnil(L); return 1; }
    push_alias_option(L, a, optName);
    return 1;
}

static int L_GetPluginTimerOption(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* pluginID = luaL_checkstring(L, 1);
    const char* itemName = luaL_checkstring(L, 2);
    const char* optName  = luaL_checkstring(L, 3);
    CPlugin* p = FindPlugin(pDoc, pluginID);
    if (!p) { lua_pushnil(L); return 1; }
    CTimer* t = nullptr;
    if (!p->m_TimerMap.Lookup(CString(itemName), t) || !t)
        { lua_pushnil(L); return 1; }
    push_timer_option(L, t, optName);
    return 1;
}

// ── UnloadPlugin / ReloadPlugin ───────────────────────────────────────────────

static int L_UnloadPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    // Find by ID
    CPlugin* pPlugin = nullptr;
    PluginListIterator pit = pDoc->m_PluginList.begin();
    for (; pit != pDoc->m_PluginList.end(); ++pit)
    {
        CPlugin* p = *pit;
        if (p && (string)(const char*)p->m_strID == id)
            { pPlugin = p; break; }
    }
    // Try by name if not found
    if (!pPlugin && *id)
    {
        for (pit = pDoc->m_PluginList.begin(); pit != pDoc->m_PluginList.end(); ++pit)
        {
            CPlugin* p = *pit;
            if (p && (string)(const char*)p->m_strName == id)
                { pPlugin = p; break; }
        }
    }
    if (!pPlugin)          { lua_pushnumber(L, eNoSuchPlugin); return 1; }
    if (pPlugin == pDoc->m_CurrentPlugin)
                           { lua_pushnumber(L, eBadParameter); return 1; }

    pDoc->m_PluginList.erase(pit);
    delete pPlugin;
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_ReloadPlugin(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    CPlugin* pPlugin = nullptr;
    PluginListIterator pit = pDoc->m_PluginList.begin();
    for (; pit != pDoc->m_PluginList.end(); ++pit)
    {
        CPlugin* p = *pit;
        if (p && (string)(const char*)p->m_strID == id)
            { pPlugin = p; break; }
    }
    if (!pPlugin && *id)
    {
        for (pit = pDoc->m_PluginList.begin(); pit != pDoc->m_PluginList.end(); ++pit)
        {
            CPlugin* p = *pit;
            if (p && (string)(const char*)p->m_strName == id)
                { pPlugin = p; break; }
        }
    }
    if (!pPlugin)        { lua_pushnumber(L, eNoSuchPlugin); return 1; }
    if (pPlugin == pDoc->m_CurrentPlugin)
                         { lua_pushnumber(L, eBadParameter); return 1; }

    CString strSource = pPlugin->m_strSource;
    pDoc->m_PluginList.erase(pit);
    delete pPlugin;

    CPlugin* pSave = pDoc->m_CurrentPlugin;
    pDoc->m_CurrentPlugin = nullptr;
    bool ok = pDoc->InternalLoadPlugin(strSource);
    pDoc->m_CurrentPlugin = pSave;

    lua_pushnumber(L, ok ? eOK : ePluginFileNotFound);
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
// toggle auto-scroll via m_fn_set_paused (wired to COutputPane by
// CWorldChildFrame) and record the state in m_gui_paused.

static int L_Pause(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    bool bPause = lua_toboolean(L, 1) != 0;
    if (pDoc->m_fn_set_paused) pDoc->m_fn_set_paused(bPause);
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
    world_doc(L);
    long seed = (long)luaL_checkinteger(L, 1);
    init_genrand((unsigned long)seed);
    return 0;
}

static int L_MtRand(lua_State* L)
{
    world_doc(L);
    lua_pushnumber(L, genrand());
    return 1;
}

// ── Base64Encode / Base64Decode ───────────────────────────────────────────────

static int L_Base64Encode(lua_State* L)
{
    world_doc(L);
    const char* text      = luaL_checkstring(L, 1);
    int         multiline = lua_toboolean(L, 2);
    CString result = encodeBase64(text, multiline != 0);
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

static int L_Base64Decode(lua_State* L)
{
    world_doc(L);
    const char* text = luaL_checkstring(L, 1);
    CString result = decodeBase64(text);
    lua_pushstring(L, static_cast<const char*>(result));
    return 1;
}

// ── ChangeDir ─────────────────────────────────────────────────────────────────

static int L_ChangeDir(lua_State* L)
{
    world_doc(L);
    const char* dir = luaL_checkstring(L, 1);
    int rc = chdir(dir);
    lua_pushnumber(L, rc == 0 ? eOK : eBadParameter);
    return 1;
}

// ── GetEchoInput / SetEchoInput ───────────────────────────────────────────────

static int L_GetEchoInput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_pushboolean(L, pDoc->m_display_my_input ? 1 : 0);
    return 1;
}

static int L_SetEchoInput(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_display_my_input = lua_toboolean(L, 1) ? 1 : 0;
    return 0;
}

// ── SetNoteColour ─────────────────────────────────────────────────────────────
// Sets both foreground and background note colours at once (RGB values).

static int L_SetNoteColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    COLORREF fg = (COLORREF)luaL_checkinteger(L, 1);
    COLORREF bg = (COLORREF)luaL_checkinteger(L, 2);
    pDoc->m_NoteColourFore = fg & 0x00FFFFFF;
    pDoc->m_NoteColourBack = bg & 0x00FFFFFF;
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

static int L_ArrayListAll(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    int i = 1;
    for (auto& kv : pDoc->GetArrayMap())
    {
        lua_pushstring(L, kv.first.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int L_ArrayListKeys(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end()) return 1;
    int i = 1;
    for (auto& kv : *it->second)
    {
        lua_pushstring(L, kv.first.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int L_ArrayListValues(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    auto it = pDoc->GetArrayMap().find(name);
    if (it == pDoc->GetArrayMap().end()) return 1;
    int i = 1;
    for (auto& kv : *it->second)
    {
        lua_pushstring(L, kv.second.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
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
    world_doc(L);
    const char* src  = luaL_checkstring(L, 1);
    const char* srch = luaL_checkstring(L, 2);
    const char* repl = luaL_checkstring(L, 3);
    bool bAll = lua_toboolean(L, 4) != 0;
    CString result = ::Replace(CString(src), CString(srch), CString(repl), bAll);
    lua_pushstring(L, (const char*)result);
    return 1;
}

// ── EditDistance / Metaphone ──────────────────────────────────────────────────

#include "Dmetaph.h"

// Declared in stdafx.h; provide forward declaration here since we don't include stdafx.h
int EditDistance(const string source, const string target);

static int L_EditDistance(lua_State* L)
{
    world_doc(L);
    const char* src = luaL_checkstring(L, 1);
    const char* tgt = luaL_checkstring(L, 2);
    lua_pushnumber(L, (lua_Number)::EditDistance(string(src), string(tgt)));
    return 1;
}

static int L_Metaphone(lua_State* L)
{
    world_doc(L);
    const char* word = luaL_checkstring(L, 1);
    int length = (int)luaL_optnumber(L, 2, 6);
    MString m(word, length);
    CString str1, str2;
    m.DoubleMetaphone(str1, str2);
    CString result = str1;
    if (!str2.IsEmpty()) result += "," + str2;
    lua_pushstring(L, (const char*)result);
    return 1;
}

// ── GetEntity / SetEntity / GetXMLEntity ─────────────────────────────────────

static int L_GetEntity(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    CString result;
    pDoc->m_CustomEntityMap.Lookup(CString(name), result);
    lua_pushstring(L, (const char*)result);
    return 1;
}

static int L_SetEntity(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name  = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    if (value[0])
        pDoc->m_CustomEntityMap.SetAt(CString(name), CString(value));
    else
        pDoc->m_CustomEntityMap.RemoveKey(CString(name));
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_GetXMLEntity(lua_State* L)
{
    world_doc(L);
    const char* entity = luaL_checkstring(L, 1);
    // Resolve standard XML named entities
    static const struct { const char* name; const char* val; } kEntities[] = {
        { "amp",  "&"  }, { "lt",   "<"  }, { "gt",   ">"  },
        { "apos", "'"  }, { "quot", "\"" }, { nullptr, nullptr }
    };
    for (int i = 0; kEntities[i].name; ++i)
        if (strcmp(entity, kEntities[i].name) == 0) {
            lua_pushstring(L, kEntities[i].val);
            return 1;
        }
    lua_pushnil(L);
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
    world_doc(L);
    luaL_checkstring(L, 1);   // consume the arg
    lua_pushnumber(L, eNoSuchCommand);
    return 1;
}

// ── Reset (MXP off) ───────────────────────────────────────────────────────────

static int L_Reset(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->MXP_Off();
    lua_pushnumber(L, eOK);
    return 1;
}

// ── Sound stubs ───────────────────────────────────────────────────────────────

static int L_Sound(lua_State* L)
{
    world_doc(L); luaL_checkstring(L, 1);
    lua_pushnumber(L, eCannotPlaySound); return 1;
}
static int L_StopSound(lua_State* L)
{
    world_doc(L); luaL_optnumber(L, 1, 0);
    lua_pushnumber(L, eOK); return 1;
}
static int L_GetSoundStatus(lua_State* L)
{
    world_doc(L); luaL_optnumber(L, 1, 0);
    lua_pushnumber(L, 0); return 1;   // 0 = not playing
}
static int L_PlaySound(lua_State* L)
{
    world_doc(L);
    // Args: Buffer (1, ignored portably), Filename (2), Loop (3), Volume (4), Pan (5).
    // Buffer-slot management and volume/pan are DirectSound-only; we play the
    // file once via the wxWidgets sound hook and return eOK on success.
    const char* filename = luaL_optstring(L, 2, "");
    if (!filename || !*filename) {
        lua_pushnumber(L, eOK); return 1;
    }
    if (g_wxhook_play_sound) {
        bool ok = g_wxhook_play_sound(filename);
        lua_pushnumber(L, ok ? eOK : eCannotPlaySound);
    } else {
        lua_pushnumber(L, eCannotPlaySound);
    }
    return 1;
}

// ── UDP ───────────────────────────────────────────────────────────────────────

#include <sys/socket.h>
#include <netinet/in.h>

static int L_UdpSend(lua_State* L)
{
    world_doc(L);
    const char* ip   = luaL_checkstring(L, 1);
    int          port = (int)luaL_checknumber(L, 2);
    const char* text = luaL_checkstring(L, 3);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)port);
    if ((sa.sin_addr.s_addr = inet_addr(ip)) == INADDR_NONE)
        { lua_pushnumber(L, -1); return 1; }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { lua_pushnumber(L, -1); return 1; }

    ssize_t sent = sendto(s, text, strlen(text), 0,
                          (struct sockaddr*)&sa, sizeof(sa));
    close(s);
    lua_pushnumber(L, sent >= 0 ? eOK : -1);
    return 1;
}

static int L_UdpListen(lua_State* L)
{
    // Full async UDP listener requires platform integration — stub.
    world_doc(L);
    lua_pushnumber(L, eOK);
    return 1;
}

static int L_GetUdpPort(lua_State* L)
{
    world_doc(L);
    lua_pushnumber(L, 0);   // no UDP listener active
    return 1;
}

// ── TranslateDebug ────────────────────────────────────────────────────────────

static int L_TranslateDebug(lua_State* L)
{
    world_doc(L);
    luaL_checkstring(L, 1);
    lua_pushnumber(L, 1);   // 1 = no translator script loaded
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
        lua_pushnumber(L, (lua_Number)(long)pDoc->m_NoteColourBack);
    else
        lua_pushnumber(L, (lua_Number)(long)pDoc->m_NoteColourFore);
    return 1;
}

// ── Command history ───────────────────────────────────────────────────────────
// On portable we maintain a simple CStringList on the doc as the history.

static int L_GetCommand(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    // world.GetCommand() — returns the current text in the command input bar.
    // The n-th-from-tail history argument is accepted but ignored on
    // non-Windows since command history is kept in the frame, not the doc.
    if (pDoc->m_fn_get_command) {
        std::string s = pDoc->m_fn_get_command();
        lua_pushstring(L, s.c_str());
        return 1;
    }
    lua_pushstring(L, "");
    return 1;
}

static int L_GetCommandList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    int count = (int)luaL_optnumber(L, 1, 0);
    int total = (int)pDoc->m_CommandHistory.GetCount();
    if (count <= 0 || count > total) count = total;
    lua_newtable(L);
    POSITION pos = pDoc->m_CommandHistory.GetTailPosition();
    for (int i = 1; i <= count && pos; ++i) {
        CString cmd = pDoc->m_CommandHistory.GetPrev(pos);
        lua_pushstring(L, (const char*)cmd);
        lua_rawseti(L, -2, i);
    }
    return 1;
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
    CMUSHclientDoc* pDoc = world_doc(L);
    pDoc->m_CommandHistory.RemoveAll();
    lua_pushnumber(L, eOK);
    return 1;
}

// PushCommand / PasteCommand / SelectCommand / SetCommand / GetQueue are
// command-window concepts.
static int L_PushCommand(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    std::string s = pDoc->m_fn_push_command ? pDoc->m_fn_push_command() : std::string();
    lua_pushstring(L, s.c_str()); return 1;
}
static int L_SelectCommand(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_fn_select_command) pDoc->m_fn_select_command();
    lua_pushnumber(L, eOK); return 1;
}

static int L_PasteCommand(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* text = luaL_checkstring(L, 1);
    // Append text to the current input bar and return previously selected text
    // (empty on non-Windows since selection state is not tracked here).
    if (pDoc->m_fn_get_command && pDoc->m_fn_set_command) {
        std::string current = pDoc->m_fn_get_command();
        std::string appended = current + std::string(text);
        // m_fn_set_command requires an empty bar; force-set via get+set.
        // Since PasteCommand replaces selection, we model it as append.
        // Temporarily clear the bar to allow set_command, then set the combined text.
        // This is an approximation: we don't have selection tracking.
        pDoc->m_fn_set_command(appended);   // may fail if bar is non-empty; ignore
    }
    lua_pushstring(L, "");   // return empty string (no selection on non-Windows)
    return 1;
}

static int L_SetCommand(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* cmd = luaL_checkstring(L, 1);
    if (pDoc->m_fn_set_command) {
        bool ok = pDoc->m_fn_set_command(std::string(cmd));
        lua_pushnumber(L, ok ? eOK : eCommandNotEmpty);
    } else {
        pDoc->m_CurrentCommand = CString(cmd);
        lua_pushnumber(L, eOK);
    }
    return 1;
}
static int L_GetCommandListFunc(lua_State* L) { return L_GetCommandList(L); }

// ── InfoBar stubs ─────────────────────────────────────────────────────────────
static int L_Info(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* msg = luaL_checkstring(L, 1);
    pDoc->m_strStatusMessage = CString(msg);
    if (pDoc->m_fn_status) pDoc->m_fn_status(msg);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_InfoBackground(lua_State* L)   { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_InfoClear(lua_State* L)        { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_InfoColour(lua_State* L)       { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_InfoFont(lua_State* L)         { world_doc(L); lua_pushnumber(L, eOK); return 1; }

// ── GUI callbacks ─────────────────────────────────────────────────────────────
static int L_Activate(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_fn_activate_client) pDoc->m_fn_activate_client();
    lua_pushnumber(L, eOK); return 1;
}
static int L_ActivateClient(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_fn_activate_client) pDoc->m_fn_activate_client();
    lua_pushnumber(L, eOK); return 1;
}
static int L_Redraw(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_fn_repaint) pDoc->m_fn_repaint();
    lua_pushnumber(L, eOK); return 1;
}
static int L_Repaint(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    if (pDoc->m_fn_repaint) pDoc->m_fn_repaint();
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_GetSysColor(lua_State* L)      { world_doc(L); lua_pushnumber(L, 0); return 1; }
static int L_GetSystemMetrics(lua_State* L) { world_doc(L); lua_pushnumber(L, 0); return 1; }
static int L_GetDeviceCaps(lua_State* L)    { world_doc(L); lua_pushnumber(L, 0); return 1; }
static int L_GetClipboard(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    std::string text = pDoc->m_fn_get_clipboard ? pDoc->m_fn_get_clipboard() : std::string();
    lua_pushstring(L, text.c_str());
    return 1;
}
static int L_SetClipboard(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* text = luaL_optstring(L, 1, "");
    if (pDoc->m_fn_set_clipboard) pDoc->m_fn_set_clipboard(text);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_SetBackgroundColour(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    unsigned long col = (unsigned long)(long)luaL_optnumber(L, 1, 0xFFFFFFFF);
    if (pDoc->m_fn_set_background_colour)
        pDoc->m_fn_set_background_colour(col);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_SetBackgroundImage(lua_State* L) { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_SetCursor(lua_State* L)         { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_SetChanged(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    // SetChanged(flag) — mark the world file as modified (true) or clean (false).
    BOOL flag = (BOOL)luaL_optinteger(L, 1, TRUE);
    pDoc->SetModifiedFlag(flag);
    lua_pushnumber(L, eOK);
    return 1;
}
static int L_GetMainWindowPosition(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    int x = 0, y = 0, w = 1024, h = 768;
    if (pDoc->m_fn_get_main_rect)
        pDoc->m_fn_get_main_rect(x, y, w, h);
    lua_pushnumber(L, x); lua_pushnumber(L, y);
    lua_pushnumber(L, w); lua_pushnumber(L, h);
    return 4;
}
static int L_GetWorldWindowPosition(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    int x = 0, y = 0, w = 800, h = 600;
    if (pDoc->m_fn_get_output_rect)
        pDoc->m_fn_get_output_rect(x, y, w, h);
    lua_pushnumber(L, x); lua_pushnumber(L, y);
    lua_pushnumber(L, w); lua_pushnumber(L, h);
    return 4;
}
// GetWorldWindowPositionX(which): 0=x, 1=y, 2=width, 3=height of output pane.
static int L_GetWorldWindowPositionX(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    int which = (int)luaL_optinteger(L, 1, 2);   // default = width
    int x = 0, y = 0, w = 800, h = 600;
    if (pDoc->m_fn_get_output_rect)
        pDoc->m_fn_get_output_rect(x, y, w, h);
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
    if (pDoc->m_fn_move_world_window) pDoc->m_fn_move_world_window(l, t, w, h);
    lua_pushnumber(L, eOK); return 1;
}
static int L_MoveWorldWindowX(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    long l = (long)luaL_optnumber(L, 1, 0);
    long t = (long)luaL_optnumber(L, 2, 0);
    long w = (long)luaL_optnumber(L, 3, 0);
    long h = (long)luaL_optnumber(L, 4, 0);
    short which = (short)luaL_optinteger(L, 5, 1);
    if (which == 1 && pDoc->m_fn_move_world_window)
        pDoc->m_fn_move_world_window(l, t, w, h);
    lua_pushnumber(L, eOK); return 1;
}
static int L_FlashIcon(lua_State* L) {
    world_doc(L);
    if (g_wxhook_flash_icon) g_wxhook_flash_icon();
    lua_pushnumber(L, eOK); return 1;
}
static int L_Help(lua_State* L)              { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_Menu(lua_State* L) {
    world_doc(L);
    const char* items_str   = luaL_checkstring(L, 1);
    const char* default_str = luaL_optstring(L, 2, "");

    // Split the pipe-delimited items string into individual entries.
    std::vector<std::string> items;
    for (const char* p = items_str, *start = p; ; ++p) {
        if (*p == '|' || *p == '\0') {
            items.emplace_back(start, p - start);
            if (!*p) break;
            start = p + 1;
        }
    }

    std::string result;
    if (!items.empty() && g_wxhook_menu_dialog) {
        std::vector<const char*> ptrs;
        ptrs.reserve(items.size());
        for (const auto& s : items)
            ptrs.push_back(s.c_str());
        char* chosen = g_wxhook_menu_dialog(ptrs.data(), (int)ptrs.size(), default_str);
        if (chosen) {
            result = chosen;
            free(chosen);
        }
    }
    lua_pushstring(L, result.c_str()); return 1;
}
static int L_PickColour(lua_State* L) {
    world_doc(L);
    long suggested = (long)luaL_optnumber(L, 1, -1);
    long result = g_wxhook_pick_colour ? g_wxhook_pick_colour(suggested) : -1;
    lua_pushnumber(L, result); return 1;
}
static int L_Save(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    bool ok = pDoc->m_fn_save ? pDoc->m_fn_save() : true;
    lua_pushnumber(L, ok ? eOK : eWorldClosed);
    return 1;
}
static int L_SetWorldWindowStatus(lua_State* L) {
    CMUSHclientDoc* pDoc = world_doc(L);
    short param = (short)luaL_optinteger(L, 1, 3);
    if (pDoc->m_fn_set_window_status) pDoc->m_fn_set_window_status(param);
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
// Full XML serialization requires CArchive; stub for now.
static int L_ExportXML(lua_State* L)
{
    world_doc(L);
    lua_pushstring(L, "");  // return empty string — not implemented
    return 1;
}
static int L_ImportXML(lua_State* L)
{
    world_doc(L);
    luaL_checkstring(L, 1);
    lua_pushnumber(L, 0);   // 0 items imported
    return 1;
}

// ── ResetIP / ResetStatusTime / SaveState / OnFileReloaddefaults ──────────────
static int L_ResetIP(lua_State* L)           { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_ResetStatusTime(lua_State* L)   { world_doc(L); lua_pushnumber(L, eOK); return 1; }
static int L_SaveState(lua_State* L)         { world_doc(L); lua_pushnumber(L, eOK); return 1; }

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
    // single value
    push_style_info(L, pDoc, pLine, iStyleNumber, lineText, iType);
    return 1;
}

// ── GetOptionList / GetAlphaOptionList ────────────────────────────────────────
// On portable builds OptionsTable[] and AlphaOptionsTable[] are stubs (empty),
// so these return empty tables rather than crashing.
extern tConfigurationNumericOption OptionsTable[];
extern tConfigurationAlphaOption   AlphaOptionsTable[];

static int L_GetOptionList(lua_State* L)
{
    world_doc(L);
    lua_newtable(L);
    int i = 0, n = 1;
    while (OptionsTable[i].pName) {
        lua_pushstring(L, OptionsTable[i].pName);
        lua_rawseti(L, -2, n++);
        i++;
    }
    return 1;
}

static int L_GetAlphaOptionList(lua_State* L)
{
    world_doc(L);
    lua_newtable(L);
    int i = 0, n = 1;
    while (AlphaOptionsTable[i].pName) {
        lua_pushstring(L, AlphaOptionsTable[i].pName);
        lua_rawseti(L, -2, n++);
        i++;
    }
    return 1;
}

// ── GetDefaultValue / GetLoadedValue ─────────────────────────────────────────
// Looks up the option in OptionsTable[] (numeric) or AlphaOptionsTable[] (string).
// Returns nil if not found (stub tables are empty, so always nil for now).
static int L_GetDefaultValue(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    for (int i = 0; OptionsTable[i].pName; i++) {
        if (strcasecmp(OptionsTable[i].pName, name) == 0) {
            lua_pushnumber(L, OptionsTable[i].iDefault);
            return 1;
        }
    }
    for (int i = 0; AlphaOptionsTable[i].pName; i++) {
        if (strcasecmp(AlphaOptionsTable[i].pName, name) == 0) {
            lua_pushstring(L, AlphaOptionsTable[i].sDefault
                           ? AlphaOptionsTable[i].sDefault : "");
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int L_GetLoadedValue(lua_State* L)
{
    // GetLoadedValue returns the value as loaded from the world file
    // (before any script changes).  In the portable build OptionsTable is a
    // stub, so this mirrors GetDefaultValue — look up the option by name.
    world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    for (int i = 0; OptionsTable[i].pName; i++) {
        if (strcasecmp(OptionsTable[i].pName, name) == 0) {
            lua_pushnumber(L, OptionsTable[i].iDefault);
            return 1;
        }
    }
    for (int i = 0; AlphaOptionsTable[i].pName; i++) {
        if (strcasecmp(AlphaOptionsTable[i].pName, name) == 0) {
            lua_pushstring(L, AlphaOptionsTable[i].sDefault
                           ? AlphaOptionsTable[i].sDefault : "");
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ── GetGlobalOption / GetGlobalOptionList ─────────────────────────────────────
// Global options are app-wide (not per-world).  In the portable single-world
// build there is no App object, so these return nil / empty table.
static int L_GetGlobalOption(lua_State* L)
{
    world_doc(L);
    luaL_checkstring(L, 1);   // consume argument
    lua_pushnil(L);
    return 1;
}

static int L_GetGlobalOptionList(lua_State* L)
{
    world_doc(L);
    lua_newtable(L);
    return 1;
}

// ── GetWorld / GetWorldById / GetWorldIdList / GetWorldList ───────────────────
// In the portable single-world build, "world" == the doc bound to this Lua state.
// GetWorld and GetWorldById return the current world if the name/id matches.
// GetWorldList / GetWorldIdList return a one-element table.
static int L_GetWorld(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* name = luaL_checkstring(L, 1);
    if (strcasecmp(name, static_cast<const char*>(pDoc->m_mush_name)) == 0) {
        // Return a light userdata representing the doc pointer
        lua_pushlightuserdata(L, pDoc);
        return 1;
    }
    return 0;
}

static int L_GetWorldById(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    const char* id = luaL_checkstring(L, 1);
    if (strcasecmp(id, static_cast<const char*>(pDoc->m_strWorldID)) == 0) {
        lua_pushlightuserdata(L, pDoc);
        return 1;
    }
    return 0;
}

static int L_GetWorldList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    lua_pushstring(L, static_cast<const char*>(pDoc->m_mush_name));
    lua_rawseti(L, -2, 1);
    return 1;
}

static int L_GetWorldIdList(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    lua_newtable(L);
    lua_pushstring(L, static_cast<const char*>(pDoc->m_strWorldID));
    lua_rawseti(L, -2, 1);
    return 1;
}

// ── Debug ─────────────────────────────────────────────────────────────────────
// Executes an internal debug command (world.Debug).  In the portable build we
// treat this as a note to stderr; the string result is the empty string.
static int L_Debug(lua_State* L)
{
    world_doc(L);
    const char* cmd = luaL_optstring(L, 1, "");
    if (cmd && *cmd)
        fprintf(stderr, "[DEBUG] %s\n", cmd);
    lua_pushstring(L, "");
    return 1;
}

// ── GenerateName ─────────────────────────────────────────────────────────────
// Generates a random MUD-style name by reading a names file.  In the portable
// build the names file infrastructure is absent; return empty string.
static int L_GenerateName(lua_State* L)
{
    world_doc(L);
    lua_pushstring(L, "");
    return 1;
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
// Returns a table listing the recognised internal slash-commands.
// Full list lives in mushclient's command infrastructure; return empty table.
static int L_GetInternalCommandsList(lua_State* L)
{
    world_doc(L);
    lua_newtable(L);
    return 1;
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
    world_doc(L);
    const char* raw = luaL_checkstring(L, 1);
    CString s(raw);
    s.Replace("\xFC", "ue");   // ü
    s.Replace("\xDC", "Ue");   // Ü
    s.Replace("\xE4", "ae");   // ä
    s.Replace("\xC4", "Ae");   // Ä
    s.Replace("\xF6", "oe");   // ö
    s.Replace("\xD6", "Oe");   // Ö
    s.Replace("\xDF", "ss");   // ß
    lua_pushstring(L, static_cast<const char*>(s));
    return 1;
}

// ── Open ─────────────────────────────────────────────────────────────────────
// Opens a world from a file.  Requires an App/document-template that doesn't
// exist in the portable build; always returns false.
static int L_Open(lua_State* L)
{
    world_doc(L);
    luaL_checkstring(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

// ── GetSelectionStart/EndLine/Column ─────────────────────────────────────────
// Query the current output-pane text selection via m_fn_get_output_selection.
// Returns 0 (no selection) in mushcli; in mushgui the callback is wired to the
// real COutputPane::GetSelectionInfo() which returns 1-based line/col values.
static void GetOutputSelection(CMUSHclientDoc* pDoc,
                                long& sl, long& sc, long& el, long& ec)
{
    sl = sc = el = ec = 0;
    if (pDoc->m_fn_get_output_selection)
        pDoc->m_fn_get_output_selection(sl, sc, el, ec);
}

static int L_GetSelectionStartLine(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long sl, sc, el, ec;
    GetOutputSelection(pDoc, sl, sc, el, ec);
    lua_pushnumber(L, sl);
    return 1;
}
static int L_GetSelectionEndLine(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long sl, sc, el, ec;
    GetOutputSelection(pDoc, sl, sc, el, ec);
    lua_pushnumber(L, el);
    return 1;
}
static int L_GetSelectionStartColumn(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long sl, sc, el, ec;
    GetOutputSelection(pDoc, sl, sc, el, ec);
    lua_pushnumber(L, sc);
    return 1;
}
static int L_GetSelectionEndColumn(lua_State* L)
{
    CMUSHclientDoc* pDoc = world_doc(L);
    long sl, sc, el, ec;
    GetOutputSelection(pDoc, sl, sc, el, ec);
    lua_pushnumber(L, ec);
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

// ── Miniwindow Lua API (mushcore/mushcli stubs) ───────────────────────────────
// lua_world_api_portable.cpp is compiled into mushcore only.  The mushgui build
// uses mushapp which compiles lua_methods.cpp instead — that file already has
// the full real Window* implementations backed by wx_gdi_compat CDC/CBitmap.
//
// The stubs below keep mushcli functional: scripts that call Window* get a
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
static int L_SpellStub(lua_State* L)
{
    world_doc(L);
    lua_pushnumber(L, eSpellCheckNotActive);
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
    // lua_world_api_portable.cpp is only compiled into mushcore (mushcli).
    // mushgui uses mushapp which compiles lua_methods.cpp with real Window* impls.
    // These stubs keep mushcli functional: scripts get eNoSuchWindow gracefully.
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
    { "AddSpellCheckWord",      L_SpellStub },
    { "SpellCheck",             L_SpellStub },
    { "SpellCheckCommand",      L_SpellStub },
    { "SpellCheckDlg",          L_SpellStub },

    // ── Notepad stubs ─────────────────────────────────────────────────────────
    { "ActivateNotepad",        L_GuiOkStub  },
    { "AppendToNotepad",        L_GuiOkStub  },
    { "CloseNotepad",           L_GuiOkStub  },
    { "MoveNotepadWindow",      L_GuiOkStub  },
    { "NotepadColour",          L_GuiOkStub  },
    { "NotepadFont",            L_GuiOkStub  },
    { "NotepadReadOnly",        L_GuiOkStub  },
    { "NotepadSaveMethod",      L_GuiOkStub  },
    { "ReplaceNotepad",         L_GuiOkStub  },
    { "SaveNotepad",            L_GuiOkStub  },
    { "SendToNotepad",          L_GuiOkStub  },
    { "GetNotepadLength",       L_GuiNilStub },
    { "GetNotepadList",         L_WindowListStub },
    { "GetNotepadText",         L_GuiNilStub },
    { "GetNotepadWindowPosition",L_GuiNilStub},

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
    { "ReadNamesFile",          L_GuiOkStub  },
    { "SetCommandSelection",    [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        long first = (long)luaL_checknumber(L, 1);
        long last  = (long)luaL_optnumber(L, 2, -1);
        if (pDoc->m_fn_set_command_selection)
            pDoc->m_fn_set_command_selection(first, last);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetCommandWindowHeight", L_GuiOkStub  },
    { "SetForegroundImage",     L_GuiOkStub  },
    { "SetFrameBackgroundColour",L_GuiOkStub },
    { "SetInputFont",           [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        const char* face = luaL_optstring(L, 1, "");
        int pts    = (int)luaL_optnumber(L, 2, 0);
        bool bold  = lua_toboolean(L, 3) != 0;
        bool italic= lua_toboolean(L, 4) != 0;
        if (pDoc->m_fn_set_output_font)
            pDoc->m_fn_set_output_font(std::string(face), pts, bold, italic);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetOutputFont",          [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        const char* face = luaL_optstring(L, 1, "");
        int pts    = (int)luaL_optnumber(L, 2, 0);
        if (pDoc->m_fn_set_output_font)
            pDoc->m_fn_set_output_font(std::string(face), pts, false, false);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetScroll",              [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        long pos = (long)luaL_checknumber(L, 1);
        // Visible (arg 2) is ignored on non-Windows.
        if (pDoc->m_fn_set_scroll && pos != -2)
            pDoc->m_fn_set_scroll(pos);
        lua_pushnumber(L, eOK); return 1;
    }},
    { "SetSelection",           [](lua_State* L) -> int {
        CMUSHclientDoc* pDoc = world_doc(L);
        int sl = (int)luaL_checknumber(L, 1);
        int el = (int)luaL_checknumber(L, 2);
        int sc = (int)luaL_optnumber(L, 3, 0);
        int ec = (int)luaL_optnumber(L, 4, 0);
        if (pDoc->m_fn_set_selection)
            pDoc->m_fn_set_selection(sl, el, sc, ec);
        lua_pushnumber(L, eOK); return 1;
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
void RegisterWorldLib(lua_State* L)
{
    luaL_register(L, WORLD_LIBRARY, worldlib);
    lua_pop(L, 1);  // pop the table luaL_register left on the stack
}

#endif // !_WIN32
