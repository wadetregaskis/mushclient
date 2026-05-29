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
#define GET_COLOUR(name, var) \
        Get_XML_colour(node, name, var, bUseDefault)

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

        // Colour 17 (MAX_CUSTOM + 1) = "other" colour (iOtherForeground/Background).
        // Stored 1-relative so zero means "no custom colour" (SAMECOLOUR).
        if (Get_XML_ushort(node, "custom_colour", t->colour, bUseDefault, 0, MAX_CUSTOM + 1)) {
            if (t->colour == 0) t->colour = SAMECOLOUR; else t->colour--;
        } else {
            t->colour = SAMECOLOUR;
        }
        GET_USHORT     ("clipboard_arg",     t->iClipboardArg, 0, 10);
        GET_USHORT     ("colour_change_type",t->iColourChangeType,
                        TRIGGER_COLOUR_CHANGE_BOTH, TRIGGER_COLOUR_CHANGE_BACKGROUND);
        GET_BOOL_USHORT("enabled",           t->bEnabled);
        GET_BOOL_USHORT("expand_variables",  t->bExpandVariables);
        GET_STRING_TRIM("group",             t->strGroup);
        GET_BOOL_USHORT("ignore_case",       t->ignore_case);
        GET_USHORT     ("lines_to_match",    t->iLinesToMatch, 0, MAX_RECENT_LINES);
        GET_BOOL_USHORT("keep_evaluating",   t->bKeepEvaluating);
        GET_BOOL_USHORT_MASK("make_bold",    t->iStyle, HILITE);
        GET_BOOL_USHORT_MASK("make_italic",  t->iStyle, BLINK);
        GET_BOOL_USHORT_MASK("make_underline",t->iStyle, UNDERLINE);
        GET_STRING     ("match",             t->trigger);
        GET_BOOL_USHORT("multi_line",        t->bMultiLine);
        GET_STRING_TRIM("name",              t->strLabel);
        GET_BOOL_BOOL  ("one_shot",          t->bOneShot);
        GET_BOOL_USHORT("omit_from_log",     t->omit_from_log);
        GET_BOOL_USHORT("omit_from_output",  t->bOmitFromOutput);
        GET_BOOL_USHORT("regexp",            t->bRegexp);
        GET_BOOL_USHORT("repeat",            t->bRepeat);
        GET_STRING_TRIM("script",            t->strProcedure);
        GET_USHORT     ("send_to",           t->iSendTo,      0, eSendToLast - 1);
        GET_USHORT     ("sequence",          t->iSequence,    0, 10000);
        GET_STRING_TRIM("sound",             t->sound_to_play);
        GET_BOOL_USHORT("sound_if_inactive", t->bSoundIfInactive);
        GET_BOOL       ("lowercase_wildcard",t->bLowercaseWildcard);
        GET_BOOL       ("temporary",         t->bTemporary);
        GET_LONG       ("user",              t->iUserOption,  LONG_MIN, LONG_MAX);
        GET_STRING_TRIM("variable",          t->strVariable);

        // iMatch flag bits — match conditions and trigger colour
        {
            unsigned short iColour = 0;
            if (GET_USHORT("text_colour", iColour, 0, 15)) {
                t->iMatch &= ~0x0F0u;
                t->iMatch |= (iColour << 4);
            }
            if (GET_USHORT("back_colour", iColour, 0, 15)) {
                t->iMatch &= ~0xF00u;
                t->iMatch |= (iColour << 8);
            }
        }
        GET_BOOL_USHORT_MASK("bold",             t->iMatch, HILITE);
        GET_BOOL_USHORT_MASK("match_back_colour",t->iMatch, TRIGGER_MATCH_BACK);
        GET_BOOL_USHORT_MASK("match_bold",       t->iMatch, TRIGGER_MATCH_HILITE);
        GET_BOOL_USHORT_MASK("match_inverse",    t->iMatch, TRIGGER_MATCH_INVERSE);
        GET_BOOL_USHORT_MASK("match_italic",     t->iMatch, TRIGGER_MATCH_BLINK);
        GET_BOOL_USHORT_MASK("match_text_colour",t->iMatch, TRIGGER_MATCH_TEXT);
        GET_BOOL_USHORT_MASK("match_underline",  t->iMatch, TRIGGER_MATCH_UNDERLINE);
        GET_BOOL_USHORT_MASK("inverse",          t->iMatch, INVERSE);
        GET_BOOL_USHORT_MASK("italic",           t->iMatch, BLINK);

        // "Other" foreground/background: full COLORREF colour (written by wColour as colour name)
        GET_COLOUR("other_text_colour", t->iOtherForeground);
        GET_COLOUR("other_back_colour", t->iOtherBackground);

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
        GET_BOOL_USHORT("echo_alias",                a->bEchoAlias);
        GET_BOOL_USHORT("menu",                      a->bMenu);
        GET_BOOL_USHORT("omit_from_command_history", a->bOmitFromCommandHistory);
        GET_BOOL       ("temporary",                 a->bTemporary);
        GET_BOOL_BOOL  ("one_shot",                  a->bOneShot);
        GET_USHORT     ("send_to",                   a->iSendTo,   0, eSendToLast - 1);
        GET_USHORT     ("sequence",                  a->iSequence, 0, 10000);
        GET_LONG       ("user",                      a->iUserOption, LONG_MIN, LONG_MAX);
        GET_STRING_TRIM("group",                     a->strGroup);
        GET_STRING_TRIM("variable",                  a->strVariable);

        // Backward-compat boolean overrides for files saved before MUSHclient 3.35
        // (these are now encoded into iSendTo).
        {
            bool bSpeedWalk = false, bVariable = false, bQueue = false;
            GET_BOOL("speed_walk",   bSpeedWalk);
            GET_BOOL("set_variable", bVariable);
            GET_BOOL("queue",        bQueue);
            if (bSpeedWalk && (a->iSendTo == eSendToWorld || a->iSendTo == eSendToSpeedwalk))
                a->iSendTo = eSendToSpeedwalk;
            if (bVariable && (a->iSendTo == eSendToWorld || a->iSendTo == eSendToVariable))
                a->iSendTo = eSendToVariable;
            if (bQueue && (a->iSendTo == eSendToWorld || a->iSendTo == eSendToCommandQueue))
                a->iSendTo = eSendToCommandQueue;
        }

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

        // "at_time" boolean: true = fire at a specific time of day (eAtTime),
        // false = fire every N hours/minutes/seconds (eInterval).
        {
            bool bAtTime = false;
            Get_XML_boolean(node, "at_time", bAtTime, bUseDefault);
            t->iType = bAtTime ? CTimer::eAtTime : CTimer::eInterval;
        }

        if (t->iType == CTimer::eAtTime) {
            GET_INT   ("hour",   t->iAtHour,   0, 23);
            GET_INT   ("minute", t->iAtMinute, 0, 59);
            GET_DOUBLE("second", t->fAtSecond, 0.0, 59.9999);
        } else {
            GET_INT   ("hour",   t->iEveryHour,   0, 23);
            GET_INT   ("minute", t->iEveryMinute, 0, 59);
            GET_DOUBLE("second", t->fEverySecond, 0.0, 59.9999);
            if (!(t->iEveryHour || t->iEveryMinute || t->fEverySecond > 0.0))
                ThrowErrorException("Timer for 'every' time cannot have zero interval");
        }

        GET_INT   ("offset_hour",   t->iOffsetHour,   0, 23);
        GET_INT   ("offset_minute", t->iOffsetMinute, 0, 59);
        GET_DOUBLE("offset_second", t->fOffsetSecond, 0.0, 59.9999);

        GET_BOOL_BOOL("active_closed", t->bActiveWhenClosed);

        // Backward-compat boolean overrides for files saved before MUSHclient 3.35.
        {
            bool bSpeedWalk = false, bNote = false;
            GET_BOOL("speed_walk", bSpeedWalk);
            GET_BOOL("note",       bNote);
            if (bSpeedWalk && (t->iSendTo == eSendToWorld || t->iSendTo == eSendToSpeedwalk))
                t->iSendTo = eSendToSpeedwalk;
            if (bNote && (t->iSendTo == eSendToWorld || t->iSendTo == eSendToOutput))
                t->iSendTo = eSendToOutput;
        }

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
// Option dispatch helpers — mirror L_SetOption / L_SetAlphaOption in
// lua_world_api_portable.cpp so OptionsTable-driven loading can set real
// struct members rather than only writing to the option caches.
// ══════════════════════════════════════════════════════════════════════════

static void apply_numeric_option(CMUSHclientDoc& doc, const char* opt, long val)
{
    // Live members — kept in sync with L_SetOption in lua_world_api_portable.cpp
    // Font options
    if      (!strcmp(opt, "output_font_height"))   doc.m_font_height         = val;
    else if (!strcmp(opt, "output_font_weight"))   doc.m_font_weight         = val;
    else if (!strcmp(opt, "output_font_charset"))  doc.m_font_charset        = (DWORD)val;
    else if (!strcmp(opt, "input_font_height"))    doc.m_input_font_height   = val;
    else if (!strcmp(opt, "input_font_italic"))    doc.m_input_font_italic   = (BYTE)(val != 0);
    else if (!strcmp(opt, "input_font_weight"))    doc.m_input_font_weight   = val;
    else if (!strcmp(opt, "input_font_charset"))   doc.m_input_font_charset  = (DWORD)val;
    // World behaviour options
    else if (!strcmp(opt, "enable_triggers"))      doc.m_enable_triggers        = (val != 0);
    else if (!strcmp(opt, "enable_aliases"))       doc.m_enable_aliases         = (val != 0);
    else if (!strcmp(opt, "enable_timers"))        doc.m_bEnableTimers          = (val != 0);
    else if (!strcmp(opt, "display_my_input"))     doc.m_display_my_input       = (val != 0);
    else if (!strcmp(opt, "log_output"))           doc.m_bLogOutput             = (short)val;
    else if (!strcmp(opt, "log_notes"))            doc.m_bLogNotes              = (short)val;
    else if (!strcmp(opt, "log_input"))            doc.m_log_input              = (short)val;
    else if (!strcmp(opt, "trace"))                doc.m_bTrace                 = (val != 0);
    else if (!strcmp(opt, "enable_trigger_sounds"))doc.m_enable_trigger_sounds  = (val != 0);
    else if (!strcmp(opt, "enable_speed_walk"))    doc.m_enable_speed_walk      = (val != 0);
    else if (!strcmp(opt, "log_html"))             doc.m_bLogHTML               = (val != 0);
    else if (!strcmp(opt, "log_raw"))              doc.m_bLogRaw                = (val != 0);
    else if (!strcmp(opt, "log_in_colour"))        doc.m_bLogInColour           = (val != 0);
    else if (!strcmp(opt, "utf_8"))                doc.m_bUTF_8                 = (val != 0);
    else if (!strcmp(opt, "use_mxp"))              doc.m_iUseMXP                = (short)val;
    else if (!strcmp(opt, "detect_pueblo"))        doc.m_bPueblo                = (val != 0);
    else if (!strcmp(opt, "naws"))                 doc.m_bNAWS                  = (val != 0);
    else if (!strcmp(opt, "disable_compression"))  doc.m_bDisableCompression    = (val != 0);
    else if (!strcmp(opt, "connect_method"))       doc.m_connect_now            = (short)val;
    else if (!strcmp(opt, "use_ssl"))              doc.m_bUseSSL                = (unsigned short)(val != 0);
    else if (!strcmp(opt, "proxy_type"))           doc.m_iSocksProcessing       = (unsigned short)val;
    else if (!strcmp(opt, "proxy_port"))           doc.m_iProxyServerPort       = (unsigned short)val;
    else if (!strcmp(opt, "send_keep_alives"))     doc.m_bSendKeepAlives        = (unsigned short)(val != 0);
    else if (!strcmp(opt, "save_world_automatically")) doc.m_bSaveWorldAutomatically = (unsigned short)(val != 0);
    // Behaviour options
    else if (!strcmp(opt, "enable_scripts"))             doc.m_bEnableScripts            = (unsigned short)(val != 0);
    else if (!strcmp(opt, "auto_pause"))                 doc.m_bAutoFreeze               = (unsigned short)(val != 0);
    else if (!strcmp(opt, "enable_beeps"))               doc.m_enable_beeps              = (unsigned short)(val != 0);
    else if (!strcmp(opt, "carriage_return_clears_line"))doc.m_bCarriageReturnClearsLine = (unsigned short)(val != 0);
    else if (!strcmp(opt, "send_echo"))                  doc.m_bSendEcho                 = (unsigned short)(val != 0);
    // Command / history options
    else if (!strcmp(opt, "enable_command_stack"))   doc.m_enable_command_stack      = (unsigned short)(val != 0);
    else if (!strcmp(opt, "history_lines"))          doc.m_nHistoryLines             = val;
    else if (!strcmp(opt, "escape_deletes_input"))   doc.m_bEscapeDeletesInput       = (unsigned short)(val != 0);
    else if (!strcmp(opt, "copy_selection_to_clipboard")) doc.m_bCopySelectionToClipboard = (unsigned short)(val != 0);
    else if (!strcmp(opt, "unpause_on_send"))        doc.m_bUnpauseOnSend            = (unsigned short)(val != 0);
    else if (!strcmp(opt, "auto_wrap_window_width")) doc.m_bAutoWrapWindowWidth      = (BOOL)(val != 0);
    else if (!strcmp(opt, "speed_walk_delay"))       doc.m_iSpeedWalkDelay           = (unsigned short)val;
    // Spam prevention
    else if (!strcmp(opt, "enable_spam_prevention")) doc.m_bEnableSpamPrevention     = (val != 0);
    else if (!strcmp(opt, "spam_line_count"))        doc.m_iSpamLineCount            = (int)val;
    // Telnet / echo
    else if (!strcmp(opt, "no_echo_off"))            doc.m_bNoEchoOff                = (unsigned short)(val != 0);
    // Hyperlink / display
    else if (!strcmp(opt, "use_custom_link_colour")) doc.m_bUseCustomLinkColour      = (val != 0);
    else if (!strcmp(opt, "hyperlink_colour"))       doc.m_iHyperlinkColour          = (COLORREF)val;
    else if (!strcmp(opt, "show_connect_disconnect"))doc.m_bShowConnectDisconnect    = (val != 0);
    else if (!strcmp(opt, "send_mxp_afk_response"))  doc.m_bSendMXP_AFK_Response    = (val != 0);
    // Script logging
    else if (!strcmp(opt, "script_errors_to_output_window")) doc.m_bScriptErrorsToOutputWindow = (val != 0);
    else if (!strcmp(opt, "log_script_errors"))      doc.m_bLogScriptErrors          = (val != 0);
    else if (!strcmp(opt, "wrap_column")) {
        if (val >= 20 && val <= MAX_LINE_WIDTH)
            doc.m_nWrapColumn = (unsigned short)val;
    }
    else if (!strcmp(opt, "max_output_lines")) {
        if (val >= 200 && val <= 500000)
            doc.m_maxlines = val;
    }
    else if (!strcmp(opt, "port")) {
        if (val >= 1 && val <= USHRT_MAX)
            doc.m_port = (unsigned short)val;
    }
    // Unknown options fall through to the cache (set by the caller's loop)
}

static void apply_alpha_option(CMUSHclientDoc& doc, const char* opt, const CString& val)
{
    // Live members — kept in sync with L_SetAlphaOption in lua_world_api_portable.cpp
    // Font name options
    if      (!strcmp(opt, "output_font_name")) doc.m_font_name              = val;
    else if (!strcmp(opt, "input_font_name"))  doc.m_input_font_name        = val;
    // Note: "site" → m_mush_name is intentional for mushcli (cli_main uses m_mush_name as host)
    else if (!strcmp(opt, "site"))             doc.m_mush_name              = val;
    else if (!strcmp(opt, "name"))             {}  // world display name — not needed by CLI
    else if (!strcmp(opt, "player"))           doc.m_name                   = val;
    else if (!strcmp(opt, "script_filename"))  doc.m_strScriptFilename      = val;
    else if (!strcmp(opt, "script_language"))  doc.m_strLanguage            = val;
    else if (!strcmp(opt, "script_prefix"))    doc.m_strScriptPrefix        = val;
    else if (!strcmp(opt, "notes"))            doc.m_notes                  = val;
    else if (!strcmp(opt, "terminal_identification"))
                                               doc.m_strTerminalIdentification = val;
    else if (!strcmp(opt, "mapping_failure"))  doc.m_strMappingFailure      = val;
    else if (!strcmp(opt, "connect_text"))     doc.m_connect_text           = val;
    else if (!strcmp(opt, "auto_log_file_name")) doc.m_strAutoLogFileName   = val;
    else if (!strcmp(opt, "speed_walk_prefix"))  doc.m_speed_walk_prefix    = val;
    else if (!strcmp(opt, "speed_walk_filler"))  doc.m_strSpeedWalkFiller   = val;
    else if (!strcmp(opt, "id"))               doc.m_strWorldID             = val;
    else if (!strcmp(opt, "on_world_open"))       doc.m_strWorldOpen        = val;
    else if (!strcmp(opt, "on_world_close"))      doc.m_strWorldClose       = val;
    else if (!strcmp(opt, "on_world_save"))       doc.m_strWorldSave        = val;
    else if (!strcmp(opt, "on_world_connect"))    doc.m_strWorldConnect     = val;
    else if (!strcmp(opt, "on_world_disconnect")) doc.m_strWorldDisconnect  = val;
    else if (!strcmp(opt, "on_world_get_focus"))  doc.m_strWorldGetFocus    = val;
    else if (!strcmp(opt, "on_world_lose_focus")) doc.m_strWorldLoseFocus   = val;
    else if (!strcmp(opt, "on_mxp_start"))        doc.m_strOnMXP_Start      = val;
    else if (!strcmp(opt, "on_mxp_stop"))         doc.m_strOnMXP_Stop       = val;
    else if (!strcmp(opt, "on_mxp_error"))        doc.m_strOnMXP_Error      = val;
    else if (!strcmp(opt, "on_mxp_open_tag"))     doc.m_strOnMXP_OpenTag    = val;
    else if (!strcmp(opt, "on_mxp_close_tag"))    doc.m_strOnMXP_CloseTag   = val;
    else if (!strcmp(opt, "on_mxp_set_variable")) doc.m_strOnMXP_SetVariable= val;
    else if (!strcmp(opt, "log_file_preamble"))         doc.m_strLogFilePreamble        = val;
    else if (!strcmp(opt, "log_file_postamble"))        doc.m_strLogFilePostamble       = val;
    else if (!strcmp(opt, "log_line_preamble_input"))   doc.m_strLogLinePreambleInput   = val;
    else if (!strcmp(opt, "log_line_preamble_notes"))   doc.m_strLogLinePreambleNotes   = val;
    else if (!strcmp(opt, "log_line_preamble_output"))  doc.m_strLogLinePreambleOutput  = val;
    else if (!strcmp(opt, "log_line_postamble_input"))  doc.m_strLogLinePostambleInput  = val;
    else if (!strcmp(opt, "log_line_postamble_notes"))  doc.m_strLogLinePostambleNotes  = val;
    else if (!strcmp(opt, "log_line_postamble_output")) doc.m_strLogLinePostambleOutput = val;
    else if (!strcmp(opt, "proxy_server"))       doc.m_strProxyServerName    = val;
    else if (!strcmp(opt, "proxy_username"))     doc.m_strProxyUserName      = val;
    // Auto-say
    else if (!strcmp(opt, "auto_say_string"))           doc.m_strAutoSayString         = val;
    else if (!strcmp(opt, "auto_say_override_prefix"))  doc.m_strOverridePrefix        = val;
    // Command stacking / spam
    else if (!strcmp(opt, "command_stack_character"))   doc.m_strCommandStackCharacter = val;
    else if (!strcmp(opt, "spam_message"))              doc.m_strSpamMessage           = val;
    // Tab completion / recall
    else if (!strcmp(opt, "tab_completion_defaults"))   doc.m_strTabCompletionDefaults = val;
    else if (!strcmp(opt, "recall_line_preamble"))      doc.m_strRecallLinePreamble    = val;
    // Script editor
    else if (!strcmp(opt, "script_editor"))             doc.m_strScriptEditor          = val;
    // Sound files
    else if (!strcmp(opt, "beep_sound"))                doc.m_strBeepSound             = val;
    else if (!strcmp(opt, "new_activity_sound"))        doc.m_new_activity_sound       = val;
    // Paste strings
    else if (!strcmp(opt, "paste_preamble"))            doc.m_paste_preamble           = val;
    else if (!strcmp(opt, "paste_postamble"))           doc.m_paste_postamble          = val;
    else if (!strcmp(opt, "paste_line_preamble"))       doc.m_pasteline_preamble       = val;
    else if (!strcmp(opt, "paste_line_postamble"))      doc.m_pasteline_postamble      = val;
    // Unknown options: caller stores in cache
}

// ══════════════════════════════════════════════════════════════════════════
// Colour table loading (<colours>)
// ══════════════════════════════════════════════════════════════════════════

static unsigned int Load_Colours(CXMLelement& parent, CMUSHclientDoc& doc)
{
    unsigned int count = 0;

    LOAD_LOOP(parent, "colours", pColours)

        // ── ANSI colours: <ansi><normal>…</normal><bold>…</bold></ansi> ─────
        CXMLelement* pAnsi = FindNode(*pColours, "ansi");
        if (pAnsi) {
            for (int group = 0; group < 2; group++) {
                CXMLelement* pGroup = FindNode(*pAnsi, group == 0 ? "normal" : "bold");
                if (!pGroup) continue;

                LOAD_LOOP(*pGroup, "colour", node)
                    long seq = 0;
                    if (!Get_XML_number(*node, "seq", seq, false, 1, 8))
                        continue;
                    seq--;   // make zero-relative
                    COLORREF* table = (group == 0) ? doc.m_normalcolour : doc.m_boldcolour;
                    Get_XML_colour(*node, "rgb", table[seq], false);
                    count++;
                END_LOAD_LOOP
            }
        }

        // ── Custom colours: <custom><colour seq="N" name="…" text="…" back="…"/>…</custom> ──
        CXMLelement* pCustom = FindNode(*pColours, "custom");
        if (pCustom) {
            LOAD_LOOP(*pCustom, "colour", node)
                long seq = 0;
                if (!Get_XML_number(*node, "seq", seq, false, 1, MAX_CUSTOM))
                    continue;
                seq--;   // make zero-relative
                Get_XML_string(*node, "name", doc.m_strCustomColourName[seq], false, true);
                Get_XML_colour(*node, "text", doc.m_customtext[seq], false);
                Get_XML_colour(*node, "back", doc.m_customback[seq], false);
                count++;
            END_LOAD_LOOP
        }

    END_LOAD_LOOP
    return count;
}

// ══════════════════════════════════════════════════════════════════════════
// World settings loading (host, port, connect_text, etc.)
// ══════════════════════════════════════════════════════════════════════════

static void Load_World_Settings(CXMLelement& root, CMUSHclientDoc& doc)
{
    LOAD_LOOP(root, "world", pWorld)
        bool bUseDefault = false;

        // ── Numeric options via OptionsTable ─────────────────────────────────
        // Iterates all registered options; for each whose attribute appears in
        // the <world> element, applies the value to the right struct member or
        // to the option cache (for non-live-member options).
        for (int i = 0; OptionsTable[i].pName; i++) {
            const char* optName = OptionsTable[i].pName;
            bool is_bool = (OptionsTable[i].iMinimum == 0 &&
                            OptionsTable[i].iMaximum == 0);
            bool is_colour = (OptionsTable[i].iFlags & OPT_RGB_COLOUR) != 0;
            long val = (long)OptionsTable[i].iDefault;

            // Skip entirely if the attribute is not present in the XML.
            // Get_XML_number substitutes "0" for absent attributes when
            // bUseDefault=false, which throws a range error for options
            // whose minimum > 0 (e.g. auto_resize_minimum_lines, range 1-100).
            // Checking attribute presence first avoids that false rejection.
            // Colours and booleans are typically safe (default 0 is valid), but
            // we skip them consistently for correctness.
            {
                CAttribute* attr = nullptr;
                if (!pWorld->AttributeMap.Lookup(optName, attr) || !attr)
                    continue;
            }

            bool got;
            if (is_colour) {
                COLORREF c = (COLORREF)val;
                got = Get_XML_colour(*pWorld, optName, c, bUseDefault);
                val = (long)c;
            } else if (is_bool) {
                bool bFlag = (val != 0);
                got = Get_XML_boolean(*pWorld, optName, bFlag, bUseDefault);
                val = bFlag ? 1 : 0;
            } else {
                got = Get_XML_long(*pWorld, optName, val, bUseDefault,
                                   (long)OptionsTable[i].iMinimum,
                                   (long)OptionsTable[i].iMaximum);
            }
            if (!got) continue;

            apply_numeric_option(doc, optName, val);
            // Also update the cache so GetOption always sees the correct value
            doc.m_numericOptionCache[optName] = val;
        }

        // ── Single-line alpha options via AlphaOptionsTable ──────────────────
        for (int i = 0; AlphaOptionsTable[i].pName; i++) {
            if (AlphaOptionsTable[i].iFlags & OPT_MULTLINE) continue;
            if (AlphaOptionsTable[i].iFlags & OPT_PASSWORD) continue;

            const char* optName = AlphaOptionsTable[i].pName;
            CAttribute* a;
            if (!pWorld->AttributeMap.Lookup(optName, a) || !a) continue;
            a->bUsed = true;
            CString val = a->strValue;

            apply_alpha_option(doc, optName, val);
            doc.m_alphaOptionCache[optName] = std::string((const char*)val);
        }

        // ── Multi-line alpha options (child elements, not attributes) ─────────
        for (int i = 0; AlphaOptionsTable[i].pName; i++) {
            if (!(AlphaOptionsTable[i].iFlags & OPT_MULTLINE)) continue;
            if (AlphaOptionsTable[i].iFlags & OPT_PASSWORD)    continue;

            const char* optName = AlphaOptionsTable[i].pName;
            CString val;
            if (!FindNodeContents(*pWorld, optName, val, false)) continue;

            apply_alpha_option(doc, optName, val);
            doc.m_alphaOptionCache[optName] = std::string((const char*)val);
        }

        // ── Fields handled separately (port, connect_text, script_prefix) ────
        // Port is a ushort stored as a plain decimal attribute; it is also in
        // OptionsTable as "port" so it is handled by the loop above, but the
        // hardcoded path below is kept as a fallback for files that may omit it
        // from the OptionsTable range (very old world files).
        FindNodeContents(*pWorld, "connect_text",  doc.m_connect_text,    true);
        FindNodeContents(*pWorld, "script_prefix", doc.m_strScriptPrefix, true);

        // ── Paste fields (multi-line child elements, NOT in AlphaOptionsTable)
        FindNodeContents(*pWorld, "paste_postamble",      doc.m_paste_postamble,      false);
        FindNodeContents(*pWorld, "paste_preamble",       doc.m_paste_preamble,       false);
        FindNodeContents(*pWorld, "paste_line_postamble", doc.m_pasteline_postamble,  false);
        FindNodeContents(*pWorld, "paste_line_preamble",  doc.m_pasteline_preamble,   false);

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

    // Sort the plugin's own arrays by sequence number so ProcessPreviousLine
    // and evaluate.cpp see triggers/aliases in the correct priority order.
    // m_CurrentPlugin is still set to plugin here, so SortTriggers/SortAliases
    // operate on the plugin's arrays via GetTriggerArray() / GetAliasArray().
    if (nTriggers > 0) SortTriggers();
    if (nAliases  > 0) SortAliases();
    // Reset plugin timers so they know when they are next due to fire.
    // Mirrors xml_load_world.cpp line 460: ResetAllTimers(GetTimerMap()).
    if (nTimers   > 0) ResetAllTimers(GetTimerMap());

    m_CurrentPlugin = pSavedPlugin;

    // Initialise the plugin's Lua engine
    Init_Plugin_Script_Engine(*this, *plugin);

    // Add to plugin list
    m_PluginList.push_back(plugin);

    fprintf(stderr, "[plugin load] \"%s\" (%s): %u trigger(s), %u alias(es), "
                    "%u variable(s), %u timer(s)\n",
            (const char*)plugin->m_strName, path,
            nTriggers, nAliases, nVariables, nTimers);

    // Fire OnPluginInstall on the newly-loaded plugin.
    // Must be done after push_back so the plugin is in the list before its
    // own install handler runs (mirrors xml_load_world.cpp line 573).
    if (plugin->m_ScriptEngine) {
        CString strInstall("OnPluginInstall");
        DISPID dispid = plugin->m_ScriptEngine->GetLuaDispid(strInstall);
        if (dispid != DISPID_UNKNOWN) {
            list<double> nparams;
            list<string> sparams;
            long nInvocations = 0;
            CPlugin* pSaved = m_CurrentPlugin;
            m_CurrentPlugin = plugin;
            plugin->m_ScriptEngine->ExecuteLua(dispid, strInstall,
                                               eInputFromServer,
                                               "plugin", "install",
                                               nparams, sparams,
                                               nInvocations);
            m_CurrentPlugin = pSaved;
        }
    }

    // Notify all other plugins that the plugin list has changed.
    // Mirrors xml_load_world.cpp calling PluginListChanged() after loading plugins.
    PluginListChanged();

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

    // Fire the on_world_open callback now that the engine is ready and
    // all entry points are resolved.  Mirrors OpenSession() in doc.cpp.
    if (doc.m_dispidWorldOpen != DISPID_UNKNOWN) {
        list<double> nparams;
        list<string> sparams;
        long nInvocations = 0;
        engine->ExecuteLua(doc.m_dispidWorldOpen,
                           doc.m_strWorldOpen,
                           eWorldAction,
                           "world", "open",
                           nparams, sparams,
                           nInvocations);
    }

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
        Load_Colours       (top, *this);

        unsigned int nTriggers  = Load_Triggers (top, *this);
        unsigned int nAliases   = Load_Aliases  (top, *this);
        unsigned int nVariables = Load_Variables(top, *this);
        unsigned int nTimers    = Load_Timers   (top, *this);
        unsigned int nScripts   = Load_Scripts  (top, *this);

        // Process <include> elements (regular and plugin)
        Load_Includes(top, *this, path);

        // Sort the doc's own trigger/alias arrays by sequence number.
        // (Plugin arrays are sorted inside InternalLoadPlugin.)
        // Must be called after Load_Includes because regular includes may
        // add more triggers/aliases directly into the doc's maps.
        if (nTriggers > 0) SortTriggers();
        if (nAliases  > 0) SortAliases();

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

// ── CMUSHclientDoc::ImportXMLFromString ───────────────────────────────────────
// Parses an XML string (as returned by ExportXML or produced by hand) and
// imports the triggers/aliases/timers/variables it contains into this world.
// Returns the total count of items added, or -1 on parse error.
// Mirrors the behaviour of the Windows ImportXML() (methods_xml.cpp) which
// uses CMemFile + CArchive + Load_World_XML, but without those Windows classes.

long CMUSHclientDoc::ImportXMLFromString(const char* xml)
{
    if (!xml || !*xml)
        return -1;

    // Quick sanity-check: must start with an XML declaration or a known element.
    // (Mirrors IsArchiveXML which checks for the <?xml ... ?> preamble.)
    const char* p = xml;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (strncmp(p, "<?xml", 5) != 0 && strncmp(p, "<muclient", 9) != 0
        && strncmp(p, "<triggers", 9) != 0 && strncmp(p, "<aliases", 8) != 0
        && strncmp(p, "<timers", 7) != 0  && strncmp(p, "<variables", 10) != 0)
        return -1;   // not recognisable XML

    // Wrap the string in a read-only CMemFile so xmlparse can read it.
    UINT len = (UINT)strlen(xml);
    CMemFile f(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(xml)), len);

    CXMLparser parser;
    try {
        parser.BuildStructure(&f);
    } catch (CException* e) {
        e->Delete();
        return -1;
    } catch (...) {
        return -1;
    }

    CXMLelement& root = parser.m_xmlRoot;
    CXMLelement* pMuClient = FindNode(root, "muclient");
    CXMLelement& top = pMuClient ? *pMuClient : root;

    long count = 0;
    long nTrig = (long)Load_Triggers (top, *this);
    long nAls  = (long)Load_Aliases  (top, *this);
    count += nTrig;
    count += nAls;
    count += (long)Load_Variables(top, *this);
    count += (long)Load_Timers   (top, *this);
    // Re-sort after import so new items land in correct sequence order.
    if (nTrig > 0) SortTriggers();
    if (nAls  > 0) SortAliases();
    return count;
}

#endif // !_WIN32
