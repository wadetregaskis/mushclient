// xml/xml_load_world_portable.cpp
//
// Non-Windows world-file loader.  Parses MUSHclient .mcl / .mup XML files
// and populates the trigger, alias, variable and timer maps of a
// CMUSHclientDoc without any MFC UI, CArchive, Frame or App dependencies.
//
// The full Windows implementation lives in xml_load_world.cpp (not compiled
// on non-Windows).  This file is the portable replacement, compiled only on
// non-Windows.

#ifndef _WIN32

#include "../platform/doc_stub.h"
#include "xmlparse.h"
#include "../regexp.h"
#include "../scripting/scripting.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

// ── helpers shared with xmlparse.cpp ─────────────────────────────────────
// (declared in xmlparse.h, implemented in xmlparse.cpp)
extern UINT iLineLastItemFound;

// ── local helper: generate a unique name for unnamed items ──────────────
static long g_unique_counter = 0;
static CString UniqueString() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", ++g_unique_counter);
    return CString(buf);
}

// ── local macros matching the style in xml_load_world.cpp ───────────────
#define DEFAULTS_LIT "defaults"

#define GET_BOOL_USHORT(name, var) \
        Get_XML_boolean_ushort(node, name, var, bUseDefault)
#define GET_BOOL(name, var) \
        Get_XML_boolean(node, name, var, bUseDefault)
#define GET_BOOL_BOOL(name, var) \
        Get_XML_boolean_BOOL(node, name, var, bUseDefault)
#define GET_BOOL_USHORT_MASK(name, var, mask) \
        Get_XML_boolean_ushort_mask(node, name, var, mask, bUseDefault)
#define GET_STRING(name, var) \
        Get_XML_string(node, name, var, bUseDefault)
#define GET_STRING_TRIM(name, var) \
        Get_XML_string(node, name, var, bUseDefault, true)
#define GET_USHORT(name, var, lo, hi) \
        Get_XML_ushort(node, name, var, bUseDefault, lo, hi)
#define GET_INT(name, var, lo, hi) \
        Get_XML_int(node, name, var, bUseDefault, lo, hi)
#define GET_LONG(name, var, lo, hi) \
        Get_XML_long(node, name, var, bUseDefault, lo, hi)
#define GET_DOUBLE(name, var, lo, hi) \
        Get_XML_double(node, name, var, bUseDefault, lo, hi)

#define LOAD_LOOP(parent, type, node) \
    for (POSITION pos##node = (parent).ChildrenList.GetHeadPosition(); pos##node; ) { \
        CXMLelement* node = (parent).ChildrenList.GetNext(pos##node); \
        if (node->strName == (type) && !node->bUsed) { \
            node->bUsed = true; \
            iLineLastItemFound = node->iLine;

#define END_LOAD_LOOP } }

// ── Report a non-fatal load warning to stderr ────────────────────────────
static void LoadWarning(const char* context, const char* msg, int line = 0)
{
    if (line > 0)
        fprintf(stderr, "[world load] line %d: %s — %s\n", line, context, msg);
    else
        fprintf(stderr, "[world load] %s — %s\n", context, msg);
}

// ── Compile a regexp from a pattern (plain or raw regex) ────────────────
// Returns nullptr on error, printing a message.
static t_regexp* compile_pattern(const CString& pattern, bool bRegexp,
                                  bool ignoreCase, bool multiLine, bool utf8)
{
    CString strRegexp;
    if (bRegexp)
        strRegexp = pattern;
    else
        strRegexp = ConvertToRegularExpression(pattern, false, true);

    try {
        return regcomp(strRegexp,
                       (ignoreCase ? PCRE_CASELESS : 0) |
                       (multiLine  ? PCRE_MULTILINE : 0) |
                       (utf8       ? PCRE_UTF8      : 0));
    } catch (CException* e) {
        char msg[512];
        e->GetErrorMessage(msg, sizeof(msg));
        e->Delete();
        fprintf(stderr, "[world load] bad regexp \"%s\": %s\n",
                (const char*)pattern, msg);
        return nullptr;
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Trigger loading
// ══════════════════════════════════════════════════════════════════════════

static bool Load_One_Trigger(CXMLelement& node, CMUSHclientDoc& doc,
                              bool bUseDefault)
{
    CTrigger* t = new CTrigger;
    CString strName;

    try {
        Get_XML_boolean(node, DEFAULTS_LIT, bUseDefault, true);

        GET_STRING     ("match",             t->trigger);
        GET_STRING_TRIM("name",              t->strLabel);
        GET_STRING_TRIM("script",            t->strProcedure);
        GET_BOOL_USHORT("enabled",           t->bEnabled);
        GET_BOOL_USHORT("regexp",            t->bRegexp);
        GET_BOOL_USHORT("ignore_case",       t->ignore_case);
        GET_BOOL_USHORT("keep_evaluating",   t->bKeepEvaluating);
        GET_BOOL_USHORT("expand_variables",  t->bExpandVariables);
        GET_BOOL_USHORT("multi_line",        t->bMultiLine);
        GET_BOOL_USHORT("repeat",            t->bRepeat);
        GET_BOOL_USHORT("omit_from_output",  t->bOmitFromOutput);
        GET_BOOL_USHORT("omit_from_log",     t->omit_from_log);
        GET_BOOL       ("lowercase_wildcard", t->bLowercaseWildcard);
        GET_BOOL       ("temporary",         t->bTemporary);
        GET_BOOL_BOOL  ("one_shot",          t->bOneShot);
        GET_USHORT     ("send_to",           t->iSendTo,      0, eSendToLast - 1);
        GET_USHORT     ("sequence",          t->iSequence,    0, 10000);
        GET_USHORT     ("lines_to_match",    t->iLinesToMatch, 0, MAX_RECENT_LINES);
        GET_LONG       ("user",              t->iUserOption,  LONG_MIN, LONG_MAX);
        GET_STRING_TRIM("group",             t->strGroup);
        GET_STRING_TRIM("variable",          t->strVariable);
        GET_STRING_TRIM("sound",             t->sound_to_play);
        GET_BOOL_USHORT_MASK("make_bold",      t->iStyle, HILITE);
        GET_BOOL_USHORT_MASK("make_italic",    t->iStyle, BLINK);
        GET_BOOL_USHORT_MASK("make_underline", t->iStyle, UNDERLINE);

        FindNodeContents(node, "send", t->contents, bUseDefault);

        if (t->trigger.IsEmpty()) {
            LoadWarning("trigger", "empty match pattern — skipped");
            delete t;
            return false;
        }

        // Generate internal name
        strName = t->strLabel;
        if (strName.IsEmpty() || strName.Left(1) == "*")
            strName.Format("*trigger%s", (LPCTSTR)UniqueString());
        else
            strName.MakeLower();

        // Compile regexp
        t->regexp = compile_pattern(t->trigger, t->bRegexp != 0,
                                    t->ignore_case != 0,
                                    t->bMultiLine   != 0,
                                    doc.m_bUTF_8    != 0);
        if (!t->regexp) {
            delete t;
            return false;
        }

        // Handle duplicate label
        CTrigger* existing;
        if (doc.GetTriggerMap().Lookup(strName, existing))
            delete existing;

        t->strInternalName = strName;
        doc.GetTriggerMap().SetAt(strName, t);
        doc.GetTriggerArray().Add(t);
        doc.GetTriggerRevMap()[t] = std::string((const char*)strName);

    } catch (CException* e) {
        char msg[512];
        e->GetErrorMessage(msg, sizeof(msg));
        e->Delete();
        LoadWarning("trigger", msg, (int)node.iLine);
        delete t;
        return false;
    }

    return true;
}

static unsigned int Load_Triggers(CXMLelement& parent, CMUSHclientDoc& doc)
{
    unsigned int count = 0;
    LOAD_LOOP(parent, "triggers", pTriggers)
        bool bUseDefault = false;
        Get_XML_boolean(*pTriggers, DEFAULTS_LIT, bUseDefault, false);
        LOAD_LOOP(*pTriggers, "trigger", pElem)
            if (Load_One_Trigger(*pElem, doc, bUseDefault))
                count++;
        END_LOAD_LOOP
    END_LOAD_LOOP
    return count;
}

// ══════════════════════════════════════════════════════════════════════════
// Alias loading
// ══════════════════════════════════════════════════════════════════════════

static bool Load_One_Alias(CXMLelement& node, CMUSHclientDoc& doc,
                            bool bUseDefault)
{
    CAlias* a = new CAlias;
    CString strName;

    try {
        Get_XML_boolean(node, DEFAULTS_LIT, bUseDefault, true);

        GET_STRING     ("match",             a->name);
        GET_STRING_TRIM("name",              a->strLabel);
        GET_STRING_TRIM("script",            a->strProcedure);
        GET_BOOL_USHORT("enabled",           a->bEnabled);
        GET_BOOL_USHORT("regexp",            a->bRegexp);
        GET_BOOL_USHORT("ignore_case",       a->bIgnoreCase);
        GET_BOOL_USHORT("keep_evaluating",   a->bKeepEvaluating);
        GET_BOOL_USHORT("expand_variables",  a->bExpandVariables);
        GET_BOOL_USHORT("omit_from_log",     a->bOmitFromLog);
        GET_BOOL_USHORT("omit_from_output",  a->bOmitFromOutput);
        GET_BOOL_USHORT("echo_alias",        a->bEchoAlias);
        GET_BOOL_USHORT("menu",              a->bMenu);
        GET_BOOL       ("temporary",         a->bTemporary);
        GET_BOOL_BOOL  ("one_shot",          a->bOneShot);
        GET_USHORT     ("send_to",           a->iSendTo,   0, eSendToLast - 1);
        GET_USHORT     ("sequence",          a->iSequence, 0, 10000);
        GET_LONG       ("user",              a->iUserOption, LONG_MIN, LONG_MAX);
        GET_STRING_TRIM("group",             a->strGroup);
        GET_STRING_TRIM("variable",          a->strVariable);

        FindNodeContents(node, "send", a->contents, bUseDefault);

        if (a->name.IsEmpty()) {
            LoadWarning("alias", "empty match pattern — skipped");
            delete a;
            return false;
        }

        strName = a->strLabel;
        if (strName.IsEmpty() || strName.Left(1) == "*")
            strName.Format("*alias%s", (LPCTSTR)UniqueString());
        else
            strName.MakeLower();

        // Compile regexp
        a->regexp = compile_pattern(a->name, a->bRegexp != 0,
                                    a->bIgnoreCase != 0,
                                    false,               // aliases never multi-line
                                    false);              // aliases don't use UTF-8 flag
        if (!a->regexp) {
            delete a;
            return false;
        }

        CAlias* existing;
        if (doc.GetAliasMap().Lookup(strName, existing))
            delete existing;

        a->strInternalName = strName;
        doc.GetAliasMap().SetAt(strName, a);
        doc.GetAliasArray().Add(a);
        doc.GetAliasRevMap()[a] = std::string((const char*)strName);

    } catch (CException* e) {
        char msg[512];
        e->GetErrorMessage(msg, sizeof(msg));
        e->Delete();
        LoadWarning("alias", msg, (int)node.iLine);
        delete a;
        return false;
    }

    return true;
}

static unsigned int Load_Aliases(CXMLelement& parent, CMUSHclientDoc& doc)
{
    unsigned int count = 0;
    LOAD_LOOP(parent, "aliases", pAliases)
        bool bUseDefault = false;
        Get_XML_boolean(*pAliases, DEFAULTS_LIT, bUseDefault, false);
        LOAD_LOOP(*pAliases, "alias", pElem)
            if (Load_One_Alias(*pElem, doc, bUseDefault))
                count++;
        END_LOAD_LOOP
    END_LOAD_LOOP
    return count;
}

// ══════════════════════════════════════════════════════════════════════════
// Variable loading
// ══════════════════════════════════════════════════════════════════════════

static unsigned int Load_Variables(CXMLelement& parent, CMUSHclientDoc& doc)
{
    unsigned int count = 0;
    LOAD_LOOP(parent, "variables", pVars)
        LOAD_LOOP(*pVars, "variable", pElem)
            try {
                CVariable* v = new CVariable;
                Get_XML_string(*pElem, "name", v->strLabel, true);
                bool bTrim = false;
                Get_XML_boolean(*pElem, "trim", bTrim, false);
                v->strContents = pElem->strContent;
                if (bTrim) {
                    v->strContents.TrimLeft("\n\r\t ");
                    v->strContents.TrimRight("\n\r\t ");
                }
                CString strVarName = v->strLabel;
                if (strVarName.IsEmpty()) {
                    delete v;
                } else {
                    CVariable* existing;
                    if (doc.GetVariableMap().Lookup(strVarName, existing))
                        delete existing;
                    doc.GetVariableMap().SetAt(strVarName, v);
                    count++;
                }
            } catch (CException* e) {
                char msg[512];
                e->GetErrorMessage(msg, sizeof(msg));
                e->Delete();
                LoadWarning("variable", msg, (int)pElem->iLine);
            }
        END_LOAD_LOOP
    END_LOAD_LOOP
    return count;
}

// ══════════════════════════════════════════════════════════════════════════
// Timer loading
// ══════════════════════════════════════════════════════════════════════════

static bool Load_One_Timer(CXMLelement& node, CMUSHclientDoc& doc,
                            bool bUseDefault)
{
    CTimer* t = new CTimer;
    CString strName;

    try {
        Get_XML_boolean(node, DEFAULTS_LIT, bUseDefault, true);

        GET_STRING_TRIM("name",            t->strLabel);
        GET_STRING_TRIM("script",          t->strProcedure);
        GET_STRING_TRIM("group",           t->strGroup);
        GET_BOOL_BOOL  ("enabled",         t->bEnabled);
        GET_BOOL_USHORT("omit_from_output",t->bOmitFromOutput);
        GET_BOOL_USHORT("omit_from_log",   t->bOmitFromLog);
        GET_BOOL       ("temporary",       t->bTemporary);
        GET_BOOL_BOOL  ("one_shot",        t->bOneShot);
        GET_USHORT     ("send_to",         t->iSendTo, 0, eSendToLast - 1);
        GET_LONG       ("user",            t->iUserOption, LONG_MIN, LONG_MAX);

        // type: 0=interval (eInterval), 1=at-time (eAtTime)
        int iType = CTimer::eInterval;
        Get_XML_int(node, "type", iType, bUseDefault, CTimer::eInterval, CTimer::eAtTime);
        t->iType = iType;

        if (t->iType == CTimer::eAtTime) {
            GET_INT   ("hour",   t->iAtHour,   0, 23);
            GET_INT   ("minute", t->iAtMinute, 0, 59);
            GET_DOUBLE("second", t->fAtSecond, 0.0, 59.9999);
        } else {
            GET_INT   ("hour",   t->iEveryHour,   0, 23);
            GET_INT   ("minute", t->iEveryMinute, 0, 59);
            GET_DOUBLE("second", t->fEverySecond, 0.0, 59.9999);
        }

        GET_INT   ("offset_hour",   t->iOffsetHour,   0, 23);
        GET_INT   ("offset_minute", t->iOffsetMinute, 0, 59);
        GET_DOUBLE("offset_second", t->fOffsetSecond, 0.0, 59.9999);

        FindNodeContents(node, "send", t->strContents, bUseDefault);

        strName = t->strLabel;
        if (strName.IsEmpty() || strName.Left(1) == "*")
            strName.Format("*timer%s", (LPCTSTR)UniqueString());
        else
            strName.MakeLower();

        CTimer* existing;
        if (doc.GetTimerMap().Lookup(strName, existing))
            delete existing;

        doc.GetTimerMap().SetAt(strName, t);
        doc.GetTimerRevMap()[t] = std::string((const char*)strName);

    } catch (CException* e) {
        char msg[512];
        e->GetErrorMessage(msg, sizeof(msg));
        e->Delete();
        LoadWarning("timer", msg, (int)node.iLine);
        delete t;
        return false;
    }

    return true;
}

static unsigned int Load_Timers(CXMLelement& parent, CMUSHclientDoc& doc)
{
    unsigned int count = 0;
    LOAD_LOOP(parent, "timers", pTimers)
        bool bUseDefault = false;
        Get_XML_boolean(*pTimers, DEFAULTS_LIT, bUseDefault, false);
        LOAD_LOOP(*pTimers, "timer", pElem)
            if (Load_One_Timer(*pElem, doc, bUseDefault))
                count++;
        END_LOAD_LOOP
    END_LOAD_LOOP
    return count;
}

// ══════════════════════════════════════════════════════════════════════════
// World settings loading (host, port, connect_text, etc.)
// ══════════════════════════════════════════════════════════════════════════

static void Load_World_Settings(CXMLelement& root, CMUSHclientDoc& doc)
{
    LOAD_LOOP(root, "world", pWorld)
        // Helper: read an attribute directly
        auto getAttr = [&](const char* name, CString& dest) {
            CAttribute* a;
            if (pWorld->AttributeMap.Lookup(name, a) && a) {
                dest = a->strValue;
                a->bUsed = true;
            }
        };

        getAttr("site",                doc.m_mush_name);
        getAttr("script_language",     doc.m_strLanguage);
        getAttr("script_filename",     doc.m_strScriptFilename);
        // World lifecycle callbacks
        getAttr("on_world_open",       doc.m_strWorldOpen);
        getAttr("on_world_close",      doc.m_strWorldClose);
        getAttr("on_world_save",       doc.m_strWorldSave);
        getAttr("on_world_connect",    doc.m_strWorldConnect);
        getAttr("on_world_disconnect", doc.m_strWorldDisconnect);
        getAttr("on_world_get_focus",  doc.m_strWorldGetFocus);
        getAttr("on_world_lose_focus", doc.m_strWorldLoseFocus);
        // MXP callbacks
        getAttr("on_mxp_start",        doc.m_strOnMXP_Start);
        getAttr("on_mxp_stop",         doc.m_strOnMXP_Stop);
        getAttr("on_mxp_open_tag",     doc.m_strOnMXP_OpenTag);
        getAttr("on_mxp_close_tag",    doc.m_strOnMXP_CloseTag);
        getAttr("on_mxp_set_variable", doc.m_strOnMXP_SetVariable);
        getAttr("on_mxp_error",        doc.m_strOnMXP_Error);

        // World properties from OptionsTable (scriptingoptions_portable.cpp)
        getAttr("auto_say_override_prefix",   doc.m_strOverridePrefix);
        getAttr("auto_say_string",            doc.m_strAutoSayString);
        getAttr("beep_sound",                 doc.m_strBeepSound);
        getAttr("input_font_name",            doc.m_input_font_name);
        getAttr("new_activity_sound",         doc.m_new_activity_sound);
        getAttr("output_font_name",           doc.m_font_name);
        getAttr("recall_line_preamble",       doc.m_strRecallLinePreamble);
        getAttr("script_editor",              doc.m_strScriptEditor);
        getAttr("tab_completion_defaults",    doc.m_strTabCompletionDefaults);

        // Paste fields — multi-line, stored as child elements
        FindNodeContents(*pWorld, "paste_postamble",      doc.m_paste_postamble,      false);
        FindNodeContents(*pWorld, "paste_preamble",       doc.m_paste_preamble,       false);
        FindNodeContents(*pWorld, "paste_line_postamble", doc.m_pasteline_postamble,  false);
        FindNodeContents(*pWorld, "paste_line_preamble",  doc.m_pasteline_preamble,   false);

        // Port — stored as attribute
        CAttribute* pPort;
        if (pWorld->AttributeMap.Lookup("port", pPort) && pPort) {
            doc.m_port = (unsigned short)atoi(pPort->strValue);
            pPort->bUsed = true;
        }

        // Multi-line text fields stored as child elements
        FindNodeContents(*pWorld, "connect_text",  doc.m_connect_text,    true);
        FindNodeContents(*pWorld, "script_prefix", doc.m_strScriptPrefix, true);

        // Boolean world flags (stored as unsigned short in the doc)
        bool bUseDefault = false;
        bool bUTF8 = (doc.m_bUTF_8 != 0);
        Get_XML_boolean(*pWorld, "utf_8", bUTF8, bUseDefault);
        doc.m_bUTF_8 = bUTF8 ? 1 : 0;

        // Proxy / SSL / keep-alives
        Get_XML_boolean_ushort(*pWorld, "use_ssl",                  doc.m_bUseSSL,               bUseDefault);
        Get_XML_ushort        (*pWorld, "proxy_type",               doc.m_iSocksProcessing,      bUseDefault, eProxyServerNone, eProxyServerLast - 1);
        Get_XML_ushort        (*pWorld, "proxy_port",               doc.m_iProxyServerPort,      bUseDefault, 0, USHRT_MAX);
        Get_XML_boolean_ushort(*pWorld, "send_keep_alives",         doc.m_bSendKeepAlives,       bUseDefault);
        Get_XML_boolean_ushort(*pWorld, "save_world_automatically", doc.m_bSaveWorldAutomatically, bUseDefault);
        getAttr("proxy_server",   doc.m_strProxyServerName);
        getAttr("proxy_username", doc.m_strProxyUserName);
        // proxy_password is OPT_PASSWORD — saved base64-encoded by Windows,
        // skipped by xml_save_world_portable.cpp, so nothing to load here.

    END_LOAD_LOOP
}

// ══════════════════════════════════════════════════════════════════════════
// Script loading — collect inline <script> elements into doc.m_strScript
// ══════════════════════════════════════════════════════════════════════════

static unsigned int Load_Scripts(CXMLelement& parent, CMUSHclientDoc& doc)
{
    unsigned int count = 0;
    LOAD_LOOP(parent, "script", pScript)
        doc.m_strScript += pScript->strContent;
        doc.m_strScript += "\n";   // ensure trailing newline between blocks
        count++;
    END_LOAD_LOOP
    return count;
}

// Collect inline <script> blocks into a CPlugin's m_strScript.
static unsigned int Load_Plugin_Scripts(CXMLelement& parent, CPlugin& plugin)
{
    unsigned int count = 0;
    LOAD_LOOP(parent, "script", pScript)
        plugin.m_strScript += pScript->strContent;
        plugin.m_strScript += "\n";
        count++;
    END_LOAD_LOOP
    return count;
}

// ══════════════════════════════════════════════════════════════════════════
// Plugin loading — load one plugin XML file into a CPlugin, then merge
// the plugin's triggers/aliases/timers/variables into the doc.
// ══════════════════════════════════════════════════════════════════════════

// Merge a plugin's collections into the world doc and set up its Lua engine.
static bool Init_Plugin_Script_Engine(CMUSHclientDoc& doc, CPlugin& plugin)
{
    CString lang = plugin.m_strLanguage;
    lang.MakeLower();
    if (lang.IsEmpty()) lang = "lua";
    if (lang != "lua") {
        fprintf(stderr, "[plugin load] skipping non-Lua plugin \"%s\" (language: %s)\n",
                (const char*)plugin.m_strName, (const char*)lang);
        return true;  // not an error — just not supported
    }

    plugin.m_strScript.TrimLeft();
    plugin.m_strScript.TrimRight();
    if (plugin.m_strScript.IsEmpty())
        return true;

    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    if (engine->CreateScriptEngine()) {
        fprintf(stderr, "[plugin load] could not initialise Lua engine for \"%s\"\n",
                (const char*)plugin.m_strName);
        delete engine;
        return false;
    }

    CString strSource = plugin.m_strSource.IsEmpty()
                        ? plugin.m_strName
                        : plugin.m_strSource;

    if (engine->Parse(plugin.m_strScript, strSource)) {
        fprintf(stderr, "[plugin load] error parsing script for plugin \"%s\"\n",
                (const char*)plugin.m_strName);
        delete engine;
        return false;
    }

    plugin.m_ScriptEngine = engine;
    return true;
}

// Merge per-plugin collections into the doc's world-level maps.
// On Windows each plugin has its own maps; on non-Windows we flatten them.
static void Merge_Plugin_Into_Doc(CMUSHclientDoc& doc, CPlugin& plugin)
{
    CString strKey; CTrigger* pt;
    for (POSITION pos = plugin.m_TriggerMap.GetStartPosition(); pos; ) {
        plugin.m_TriggerMap.GetNextAssoc(pos, strKey, pt);
        doc.GetTriggerMap().SetAt(strKey, pt);
        doc.GetTriggerArray().Add(pt);
        doc.GetTriggerRevMap()[pt] = std::string((const char*)strKey);
    }
    plugin.m_TriggerMap.RemoveAll();

    CAlias* pa;
    for (POSITION pos = plugin.m_AliasMap.GetStartPosition(); pos; ) {
        plugin.m_AliasMap.GetNextAssoc(pos, strKey, pa);
        doc.GetAliasMap().SetAt(strKey, pa);
        doc.GetAliasArray().Add(pa);
        doc.GetAliasRevMap()[pa] = std::string((const char*)strKey);
    }
    plugin.m_AliasMap.RemoveAll();

    CTimer* pti;
    for (POSITION pos = plugin.m_TimerMap.GetStartPosition(); pos; ) {
        plugin.m_TimerMap.GetNextAssoc(pos, strKey, pti);
        doc.GetTimerMap().SetAt(strKey, pti);
        doc.GetTimerRevMap()[pti] = std::string((const char*)strKey);
    }
    plugin.m_TimerMap.RemoveAll();

    CVariable* pv;
    for (POSITION pos = plugin.m_VariableMap.GetStartPosition(); pos; ) {
        plugin.m_VariableMap.GetNextAssoc(pos, strKey, pv);
        doc.GetVariableMap().SetAt(strKey, pv);
    }
    plugin.m_VariableMap.RemoveAll();
}

// Load a plugin XML file and add it to the doc's plugin list.
// Returns true on success; false logs an error and returns false.
bool CMUSHclientDoc::InternalLoadPlugin(const CString& strPluginPath)
{
    const char* path = (const char*)strPluginPath;

    CFile f;
    if (!f.Open(path, CFile::modeRead)) {
        fprintf(stderr, "[plugin load] cannot open \"%s\"\n", path);
        return false;
    }

    CXMLparser parser;
    try {
        parser.BuildStructure(&f);
    } catch (CException* e) {
        char msg[512];
        e->GetErrorMessage(msg, sizeof(msg));
        e->Delete();
        fprintf(stderr, "[plugin load] XML parse error in \"%s\": %s\n", path, msg);
        return false;
    }
    f.Close();

    CXMLelement& root = parser.m_xmlRoot;
    CXMLelement* pMuClient = FindNode(root, "muclient");
    CXMLelement& top = pMuClient ? *pMuClient : root;

    // Find the <plugin> metadata element
    CXMLelement* pPluginMeta = FindNode(top, "plugin");
    if (!pPluginMeta) {
        fprintf(stderr, "[plugin load] no <plugin> element in \"%s\"\n", path);
        return false;
    }

    CPlugin* plugin = new CPlugin(this);
    plugin->m_strSource = strPluginPath;

    // Extract path directory
    const char* slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    plugin->m_strDirectory = slash ? CString(path, (int)(slash - path + 1)) : CString("");

    // Read plugin metadata attributes
    auto getAttr = [&](const char* name, CString& dest) {
        CAttribute* a = nullptr;
        if (pPluginMeta->AttributeMap.Lookup(name, a) && a) {
            dest = a->strValue;
            a->bUsed = true;
        }
    };
    getAttr("name",     plugin->m_strName);
    getAttr("author",   plugin->m_strAuthor);
    getAttr("id",       plugin->m_strID);
    getAttr("purpose",  plugin->m_strPurpose);
    getAttr("language", plugin->m_strLanguage);

    double ver = 0.0;
    Get_XML_double(*pPluginMeta, "version",  ver, false, 0.0);
    plugin->m_dVersion = ver;
    Get_XML_double(*pPluginMeta, "requires", ver, false, 0.0);
    plugin->m_dRequiredVersion = ver;
    FindNodeContents(*pPluginMeta, "description", plugin->m_strDescription, false);

    if (plugin->m_strName.IsEmpty()) {
        fprintf(stderr, "[plugin load] plugin in \"%s\" has no name — skipping\n", path);
        delete plugin;
        return false;
    }
    if (plugin->m_strID.IsEmpty()) {
        fprintf(stderr, "[plugin load] plugin \"%s\" has no id — skipping\n",
                (const char*)plugin->m_strName);
        delete plugin;
        return false;
    }

    // Check for duplicate ID
    for (auto* p : m_PluginList) {
        if (p->m_strID == plugin->m_strID) {
            fprintf(stderr, "[plugin load] plugin \"%s\" already loaded — skipping\n",
                    (const char*)plugin->m_strName);
            delete plugin;
            return false;
        }
    }

    // Load the plugin's triggers / aliases / timers / variables / scripts
    // into the plugin's own maps temporarily, then merge into doc.
    CPlugin* pSavedPlugin = m_CurrentPlugin;
    m_CurrentPlugin = plugin;

    // Use existing Load_* helpers but with plugin as target.
    // For simplicity on non-Windows, load directly into doc (merged approach).
    unsigned int nTriggers  = Load_Triggers (top, *this);
    unsigned int nAliases   = Load_Aliases  (top, *this);
    unsigned int nVariables = Load_Variables(top, *this);
    unsigned int nTimers    = Load_Timers   (top, *this);
    Load_Plugin_Scripts(top, *plugin);

    m_CurrentPlugin = pSavedPlugin;

    // Initialise the plugin's Lua engine
    Init_Plugin_Script_Engine(*this, *plugin);

    // Add to plugin list
    m_PluginList.push_back(plugin);

    fprintf(stderr, "[plugin load] \"%s\" (%s): %u trigger(s), %u alias(es), "
                    "%u variable(s), %u timer(s)\n",
            (const char*)plugin->m_strName, path,
            nTriggers, nAliases, nVariables, nTimers);

    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// Include file loading — process <include> elements in a world file.
// Handles both regular includes (bPlugin=false) and plugin includes (true).
// ══════════════════════════════════════════════════════════════════════════

static void Load_Includes(CXMLelement& parent, CMUSHclientDoc& doc,
                          const char* worldPath)
{
    // Work out the directory that the world file is in so we can
    // resolve relative paths.
    std::string worldDir;
    if (worldPath) {
        const char* slash = strrchr(worldPath, '/');
        if (!slash) slash = strrchr(worldPath, '\\');
        if (slash)
            worldDir = std::string(worldPath, slash + 1);
    }

    LOAD_LOOP(parent, "include", pInclude)
        bool bUseDefault = false;
        CString strName;
        Get_XML_string(*pInclude, "name", strName, false, true);
        if (strName.IsEmpty()) {
            LoadWarning("include", "name is empty — skipping", pInclude->iLine);
            continue;
        }

        bool bPlugin = false;
        bool bPluginBool = false;
        Get_XML_boolean(*pInclude, "plugin", bPluginBool, false);
        bPlugin = bPluginBool;

        // Resolve relative paths against the world file's directory
        const char* nameStr = (const char*)strName;
        std::string fullPath;
        if (nameStr[0] == '/' || nameStr[0] == '\\' ||
            (nameStr[1] == ':')) {
            fullPath = nameStr;  // absolute
        } else {
            fullPath = worldDir + nameStr;
        }

        if (bPlugin) {
            doc.InternalLoadPlugin(CString(fullPath.c_str()));
        } else {
            // Regular include: parse and merge into doc
            CFile incFile;
            if (!incFile.Open(fullPath.c_str(), CFile::modeRead)) {
                fprintf(stderr, "[world load] cannot open include \"%s\"\n",
                        fullPath.c_str());
                continue;
            }
            CXMLparser incParser;
            try {
                incParser.BuildStructure(&incFile);
            } catch (CException* e) {
                char msg[512];
                e->GetErrorMessage(msg, sizeof(msg));
                e->Delete();
                fprintf(stderr, "[world load] XML parse error in include \"%s\": %s\n",
                        fullPath.c_str(), msg);
                incFile.Close();
                continue;
            }
            incFile.Close();
            CXMLelement& incRoot = incParser.m_xmlRoot;
            CXMLelement* pMC = FindNode(incRoot, "muclient");
            CXMLelement& incTop = pMC ? *pMC : incRoot;
            Load_Triggers (incTop, doc);
            Load_Aliases  (incTop, doc);
            Load_Variables(incTop, doc);
            Load_Timers   (incTop, doc);
            Load_Scripts  (incTop, doc);
        }
    END_LOAD_LOOP
}

// ══════════════════════════════════════════════════════════════════════════
// Script engine initialisation
// Creates a Lua engine, parses inline script and/or external script file.
// Returns true on success (or when there is nothing to do).
// ══════════════════════════════════════════════════════════════════════════

static bool Init_Script_Engine(CMUSHclientDoc& doc, const char* worldPath)
{
    // Default language is Lua on non-Windows; if the world XML specifies
    // something we can't handle (e.g. VBScript), skip.
    if (doc.m_strLanguage.IsEmpty())
        doc.m_strLanguage = "lua";

    CString lang = doc.m_strLanguage;
    lang.MakeLower();
    if (lang != "lua") {
        fprintf(stderr, "[world load] scripting language \"%s\" is not supported "
                        "on this platform — script not loaded\n",
                (const char*)doc.m_strLanguage);
        return false;
    }

    // Load external script file if specified
    if (!doc.m_strScriptFilename.IsEmpty()) {
        // Resolve relative path against the world-file directory
        CString strFilePath = doc.m_strScriptFilename;
        if (strFilePath[0] != '/' && worldPath) {
            // Extract directory of world file
            CString strDir = CString(worldPath);
            int lastSlash = strDir.ReverseFind('/');
            if (lastSlash >= 0)
                strDir = strDir.Left(lastSlash + 1);
            else
                strDir = "";
            strFilePath = strDir + strFilePath;
        }

        FILE* f = fopen((const char*)strFilePath, "r");
        if (!f) {
            fprintf(stderr, "[world load] cannot open script file \"%s\": %s\n",
                    (const char*)strFilePath, strerror(errno));
            // Non-fatal: continue without external script
        } else {
            // Read entire file
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                std::string buf(sz, '\0');
                fread(&buf[0], 1, sz, f);
                doc.m_strScript = CString(buf.c_str()) + "\n" + doc.m_strScript;
            }
            fclose(f);
        }
    }

    // Trim accumulated script
    doc.m_strScript.TrimLeft();
    doc.m_strScript.TrimRight();

    if (doc.m_strScript.IsEmpty()) {
        // Nothing to compile — but mark language so GetLanguage() works
        return true;
    }

    // Create and initialise the script engine
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    if (engine->CreateScriptEngine()) {
        fprintf(stderr, "[world load] could not initialise Lua engine\n");
        delete engine;
        return false;
    }

    CString strSource = doc.m_strScriptFilename.IsEmpty()
                        ? CString("<inline script>")
                        : doc.m_strScriptFilename;

    if (engine->Parse(doc.m_strScript, strSource)) {
        fprintf(stderr, "[world load] error parsing script \"%s\"\n",
                (const char*)strSource);
        delete engine;
        return false;
    }

    doc.m_ScriptEngine = engine;
    fprintf(stderr, "[world load] Lua script engine ready (\"%s\")\n",
            (const char*)strSource);

    // Record the script file's mtime so the auto-reload poll (OnScriptFileChanged)
    // doesn't immediately trigger a false reload on the first check.
    if (!doc.m_strScriptFilename.IsEmpty()) {
        struct stat st;
        if (::stat((const char*)doc.m_strScriptFilename, &st) == 0)
            doc.m_scriptFileMtime = st.st_mtime;
    }

    // Resolve all trigger/alias/timer/world-callback dispids now that
    // the script is parsed — otherwise ExecuteLua silently skips them.
    doc.ResolveAllEntryPoints();

    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// CMUSHclientDoc::LoadWorldFromFile
// Public entry point.  Returns true on success.
// ══════════════════════════════════════════════════════════════════════════

bool CMUSHclientDoc::LoadWorldFromFile(const char* path)
{
    CFile f;
    if (!f.Open(path, CFile::modeRead)) {
        fprintf(stderr, "[world load] cannot open \"%s\"\n", path);
        return false;
    }

    try {
        CXMLparser parser;
        try {
            parser.BuildStructure(&f);
        } catch (CException* e) {
            char msg[512];
            e->GetErrorMessage(msg, sizeof(msg));
            e->Delete();
            fprintf(stderr, "[world load] XML parse error in \"%s\": %s\n", path, msg);
            return false;
        }
        f.Close();

        // The root element may be <muclient> or the sections may be at root level
        CXMLelement& root = parser.m_xmlRoot;

        // Find the <muclient> container if present; otherwise work at root level
        CXMLelement* pMuClient = FindNode(root, "muclient");
        CXMLelement& top = pMuClient ? *pMuClient : root;

        Load_World_Settings(top, *this);

        unsigned int nTriggers  = Load_Triggers (top, *this);
        unsigned int nAliases   = Load_Aliases  (top, *this);
        unsigned int nVariables = Load_Variables(top, *this);
        unsigned int nTimers    = Load_Timers   (top, *this);
        unsigned int nScripts   = Load_Scripts  (top, *this);

        // Process <include> elements (regular and plugin)
        Load_Includes(top, *this, path);

        fprintf(stderr, "[world load] \"%s\": %u trigger(s), %u alias(es), "
                        "%u variable(s), %u timer(s), %u script block(s), "
                        "%zu plugin(s)\n",
                path, nTriggers, nAliases, nVariables, nTimers, nScripts,
                m_PluginList.size());

        // Initialise the Lua script engine if this world has scripting
        if (nScripts > 0 || !m_strScriptFilename.IsEmpty())
            Init_Script_Engine(*this, path);

        // Reset all timers so they know when to fire next
        if (nTimers > 0)
            ResetAllTimers(GetTimerMap());

        m_strWorldFilePath = CString(path);
        return true;

    } catch (const std::runtime_error& ex) {
        fprintf(stderr, "[world load] error in \"%s\": %s\n", path, ex.what());
        return false;
    } catch (...) {
        fprintf(stderr, "[world load] unknown error in \"%s\"\n", path);
        return false;
    }
}

#endif // !_WIN32
