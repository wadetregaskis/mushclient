// xml/xml_save_world_portable.cpp
//
// Portable (non-Windows) implementation of CMUSHclientDoc::Serialize_World_XML.
//
// Saves a world file in the same XML format as the Windows xml_save_world.cpp,
// reading world settings via live doc members and the option caches rather than
// via the offsetof-based GetOptionItem / GetAlphaOptionItem path used by the
// Windows build (which requires AllocateConfigurationArrays() / scriptingoptions.cpp,
// neither of which is compiled into mushcore).
//
// The resulting files are fully interchangeable with those written by the Windows
// build and can be loaded by xml_load_world.cpp or xml_load_world_portable.cpp.
//
// Guard: compiled only on non-Windows (mushcore / mushcli target).

#ifndef _WIN32

#include "../platform/doc_stub.h"

#include <cstring>
#include <climits>

// ── compile-time assertions ───────────────────────────────────────────────────
// CTimer::eAtTime must still == 1 for the hour/minute/second logic below.
static_assert(CTimer::eAtTime == 1, "CTimer::eAtTime changed — update save logic");

// ── constants ─────────────────────────────────────────────────────────────────
#define NL "\r\n"

// ── helpers: XML escaping ─────────────────────────────────────────────────────
// FixHTMLString (for attribute values) is declared in doc_stub.h / defined in
// doc_portable.cpp.  We provide a local FixMulti for element content where
// newlines must be preserved and quote-escaping is not needed.

static CString FixMulti(CString s)
{
    // Order matters: & must be replaced first.
    s.Replace("&", "&amp;");
    s.Replace("<", "&lt;");
    s.Replace(">", "&gt;");
    s.Replace("\t", "&#9;");
    return s;
}

// ── XML write helpers ─────────────────────────────────────────────────────────
// These mirror the Save_XML_* helpers in xml_save_world.cpp.

// Write a non-empty string attribute.  Newlines in the value are collapsed to
// spaces (same behaviour as Save_XML_string in the Windows build).
static void wStr(CArchive& ar, const char* name, const CString& val,
                 bool sameline = false)
{
    if (val.IsEmpty()) return;
    CString v = val;
    v.Replace("\r\n", " ");
    v.Replace("\n",   " ");
    ar.WriteString(CFormat("%s%s=\"%s\"%s",
                   sameline ? "" : "   ",
                   name,
                   (LPCTSTR)FixHTMLString(v),
                   sameline ? " " : NL));
}

// Write a non-zero long attribute.
static void wNum(CArchive& ar, const char* name, long val,
                 bool sameline = false)
{
    if (!val) return;
    ar.WriteString(CFormat("%s%s=\"%ld\"%s",
                   sameline ? "" : "   ",
                   name, val,
                   sameline ? " " : NL));
}

// Write a boolean attribute as "y"; omit if false.
static void wBool(CArchive& ar, const char* name, long val,
                  bool sameline = false)
{
    if (!val) return;
    ar.WriteString(CFormat("%s%s=\"y\"%s",
                   sameline ? "" : "   ",
                   name,
                   sameline ? " " : NL));
}

// Write a COLORREF as a colour-name attribute.
static void wColour(CArchive& ar, const char* name, COLORREF c,
                    bool sameline = false)
{
    CString cname = ColourToName(c);
    if (cname.IsEmpty()) return;
    ar.WriteString(CFormat("%s%s=\"%s\"%s",
                   sameline ? "" : "   ",
                   name,
                   (LPCTSTR)FixHTMLString(cname),
                   sameline ? " " : NL));
}

// Write a double attribute (formatted to 2 d.p., comma → period).
static void wDouble(CArchive& ar, const char* name, double val,
                    bool sameline = false)
{
    CString s = CFormat("%.2f", val);
    s.Replace(",", ".");
    ar.WriteString(CFormat("%s%s=\"%s\"%s",
                   sameline ? "" : "   ",
                   name,
                   (LPCTSTR)FixHTMLString(s),
                   sameline ? " " : NL));
}

// Write a multi-line child element; omit if empty.
static void wMulti(CArchive& ar, const char* name, const CString& val)
{
    if (val.IsEmpty()) return;
    ar.WriteString(CFormat("  <%s>%s</%s>" NL,
                   name,
                   (LPCTSTR)FixMulti(val),
                   name));
}

// ── Option value retrieval ────────────────────────────────────────────────────
// Three-tier lookup (live member → cache → LONG_MIN) mirroring L_GetOption
// in lua_world_api_portable.cpp.  Callers must substitute the OptionsTable
// default when LONG_MIN is returned.

static long get_numeric_value(const CMUSHclientDoc* doc, const char* opt)
{
    if (!strcmp(opt, "connect_method"))        return doc->m_connect_now;
    if (!strcmp(opt, "detect_pueblo"))         return doc->m_bPueblo              ? 1 : 0;
    if (!strcmp(opt, "disable_compression"))   return doc->m_bDisableCompression  ? 1 : 0;
    if (!strcmp(opt, "display_my_input"))      return doc->m_display_my_input     ? 1 : 0;
    if (!strcmp(opt, "enable_aliases"))        return doc->m_enable_aliases       ? 1 : 0;
    if (!strcmp(opt, "enable_speed_walk"))     return doc->m_enable_speed_walk    ? 1 : 0;
    if (!strcmp(opt, "enable_timers"))         return doc->m_bEnableTimers        ? 1 : 0;
    if (!strcmp(opt, "enable_trigger_sounds")) return doc->m_enable_trigger_sounds? 1 : 0;
    if (!strcmp(opt, "enable_triggers"))       return doc->m_enable_triggers      ? 1 : 0;
    if (!strcmp(opt, "log_html"))              return doc->m_bLogHTML             ? 1 : 0;
    if (!strcmp(opt, "log_in_colour"))         return doc->m_bLogInColour         ? 1 : 0;
    if (!strcmp(opt, "log_input"))             return doc->m_log_input;
    if (!strcmp(opt, "log_notes"))             return doc->m_bLogNotes;
    if (!strcmp(opt, "log_output"))            return doc->m_bLogOutput;
    if (!strcmp(opt, "log_raw"))               return doc->m_bLogRaw              ? 1 : 0;
    if (!strcmp(opt, "max_output_lines"))      return doc->m_maxlines;
    if (!strcmp(opt, "naws"))                  return doc->m_bNAWS                ? 1 : 0;
    if (!strcmp(opt, "port"))                  return doc->m_port;
    if (!strcmp(opt, "trace"))                 return doc->m_bTrace               ? 1 : 0;
    if (!strcmp(opt, "use_mxp"))               return doc->m_iUseMXP;
    if (!strcmp(opt, "utf_8"))                 return doc->m_bUTF_8               ? 1 : 0;
    if (!strcmp(opt, "wrap_column"))           return doc->m_nWrapColumn;
    if (!strcmp(opt, "use_ssl"))              return doc->m_bUseSSL;
    if (!strcmp(opt, "proxy_type"))           return doc->m_iSocksProcessing;
    if (!strcmp(opt, "proxy_port"))           return doc->m_iProxyServerPort;
    if (!strcmp(opt, "send_keep_alives"))     return doc->m_bSendKeepAlives;
    if (!strcmp(opt, "save_world_automatically")) return doc->m_bSaveWorldAutomatically;

    auto it = doc->m_numericOptionCache.find(opt);
    if (it != doc->m_numericOptionCache.end()) return it->second;

    return LONG_MIN;
}

// Same for string options, mirroring L_GetAlphaOption.
// Returns empty CString when the name is not a known live member and not in
// the cache; callers use AlphaOptionsTable[i].sDefault as the default.

static CString get_alpha_value(const CMUSHclientDoc* doc, const char* opt)
{
    if (!strcmp(opt, "auto_log_file_name"))      return doc->m_strAutoLogFileName;
    if (!strcmp(opt, "connect_text"))            return doc->m_connect_text;
    if (!strcmp(opt, "id"))                      return doc->m_strWorldID;
    if (!strcmp(opt, "log_file_postamble"))      return doc->m_strLogFilePostamble;
    if (!strcmp(opt, "log_file_preamble"))       return doc->m_strLogFilePreamble;
    if (!strcmp(opt, "log_line_postamble_input"))   return doc->m_strLogLinePostambleInput;
    if (!strcmp(opt, "log_line_postamble_notes"))   return doc->m_strLogLinePostambleNotes;
    if (!strcmp(opt, "log_line_postamble_output"))  return doc->m_strLogLinePostambleOutput;
    if (!strcmp(opt, "log_line_preamble_input"))    return doc->m_strLogLinePreambleInput;
    if (!strcmp(opt, "log_line_preamble_notes"))    return doc->m_strLogLinePreambleNotes;
    if (!strcmp(opt, "log_line_preamble_output"))   return doc->m_strLogLinePreambleOutput;
    if (!strcmp(opt, "mapping_failure"))         return doc->m_strMappingFailure;
    if (!strcmp(opt, "name"))                    return doc->m_mush_name;
    if (!strcmp(opt, "notes"))                   return doc->m_notes;
    if (!strcmp(opt, "on_mxp_close_tag"))        return doc->m_strOnMXP_CloseTag;
    if (!strcmp(opt, "on_mxp_error"))            return doc->m_strOnMXP_Error;
    if (!strcmp(opt, "on_mxp_open_tag"))         return doc->m_strOnMXP_OpenTag;
    if (!strcmp(opt, "on_mxp_set_variable"))     return doc->m_strOnMXP_SetVariable;
    if (!strcmp(opt, "on_mxp_start"))            return doc->m_strOnMXP_Start;
    if (!strcmp(opt, "on_mxp_stop"))             return doc->m_strOnMXP_Stop;
    if (!strcmp(opt, "on_world_close"))          return doc->m_strWorldClose;
    if (!strcmp(opt, "on_world_connect"))        return doc->m_strWorldConnect;
    if (!strcmp(opt, "on_world_disconnect"))     return doc->m_strWorldDisconnect;
    if (!strcmp(opt, "on_world_get_focus"))      return doc->m_strWorldGetFocus;
    if (!strcmp(opt, "on_world_lose_focus"))     return doc->m_strWorldLoseFocus;
    if (!strcmp(opt, "on_world_open"))           return doc->m_strWorldOpen;
    if (!strcmp(opt, "on_world_save"))           return doc->m_strWorldSave;
    if (!strcmp(opt, "password"))                return doc->m_password;
    if (!strcmp(opt, "player"))                  return doc->m_name;
    if (!strcmp(opt, "script_filename"))         return doc->m_strScriptFilename;
    if (!strcmp(opt, "script_language"))         return doc->m_strLanguage;
    if (!strcmp(opt, "script_prefix"))           return doc->m_strScriptPrefix;
    if (!strcmp(opt, "site"))                    return doc->m_server;
    if (!strcmp(opt, "speed_walk_filler"))       return doc->m_strSpeedWalkFiller;
    if (!strcmp(opt, "speed_walk_prefix"))       return doc->m_speed_walk_prefix;
    if (!strcmp(opt, "terminal_identification")) return doc->m_strTerminalIdentification;
    if (!strcmp(opt, "proxy_server"))          return doc->m_strProxyServerName;
    if (!strcmp(opt, "proxy_username"))        return doc->m_strProxyUserName;
    if (!strcmp(opt, "proxy_password"))        return doc->m_strProxyPassword;

    auto it = doc->m_alphaOptionCache.find(opt);
    if (it != doc->m_alphaOptionCache.end()) return CString(it->second.c_str());

    return CString();
}

// ── Section writers ───────────────────────────────────────────────────────────

static void save_general(CArchive& ar, const CMUSHclientDoc& doc)
{
    ar.WriteString("<world" NL);
    ar.WriteString(CFormat("   muclient_version=\"%s\"" NL, MUSHCLIENT_VERSION));
    ar.WriteString("   world_file_version=\"1\"" NL NL);

    // ── single-line alpha options ─────────────────────────────────────────
    for (int i = 0; AlphaOptionsTable[i].pName; i++) {
        if (AlphaOptionsTable[i].iFlags & OPT_MULTLINE)  continue;
        if (AlphaOptionsTable[i].iFlags & OPT_PASSWORD)  continue;

        CString val = get_alpha_value(&doc, AlphaOptionsTable[i].pName);
        const char* def = AlphaOptionsTable[i].sDefault ? AlphaOptionsTable[i].sDefault : "";
        if (val == CString(def)) continue;

        wStr(ar, AlphaOptionsTable[i].pName, val);
    }

    ar.WriteString(NL);

    // ── numeric options ───────────────────────────────────────────────────
    for (int i = 0; OptionsTable[i].pName; i++) {
        long val = get_numeric_value(&doc, OptionsTable[i].pName);
        if (val == LONG_MIN) val = (long)OptionsTable[i].iDefault;

        if (val == (long)OptionsTable[i].iDefault) continue;

        bool is_bool   = (OptionsTable[i].iMinimum == 0 && OptionsTable[i].iMaximum == 0);
        bool is_colour = (OptionsTable[i].iFlags & OPT_RGB_COLOUR) != 0;

        if (is_bool)
            wBool  (ar, OptionsTable[i].pName, val);
        else if (is_colour)
            wColour(ar, OptionsTable[i].pName, (COLORREF)val);
        else
            wNum   (ar, OptionsTable[i].pName, val);
    }

    ar.WriteString("   > <!-- end of general world attributes -->" NL);

    // ── multi-line alpha options ──────────────────────────────────────────
    for (int i = 0; AlphaOptionsTable[i].pName; i++) {
        if (!(AlphaOptionsTable[i].iFlags & OPT_MULTLINE)) continue;

        CString val = get_alpha_value(&doc, AlphaOptionsTable[i].pName);
        const char* def = AlphaOptionsTable[i].sDefault ? AlphaOptionsTable[i].sDefault : "";
        if (val == CString(def)) continue;

        wMulti(ar, AlphaOptionsTable[i].pName, val);
    }

    ar.WriteString(NL "</world>" NL);
}

static void save_triggers(CArchive& ar, CMUSHclientDoc& doc)
{
    ar.WriteString("<triggers>" NL);

    POSITION pos = doc.m_TriggerMap.GetStartPosition();
    while (pos) {
        CString key;
        CTrigger* t = nullptr;
        doc.m_TriggerMap.GetNextAssoc(pos, key, t);
        if (!t || t->bTemporary || t->bIncluded) continue;

        ar.WriteString("  <trigger" NL);
        wNum  (ar, "back_colour",        (t->iMatch >> 8) & 0x0F);
        wBool (ar, "bold",               t->iMatch & HILITE);
        wNum  (ar, "clipboard_arg",      t->iClipboardArg);
        if (t->colour != SAMECOLOUR)
            wNum(ar, "custom_colour",    t->colour + 1);
        wNum  (ar, "colour_change_type", t->iColourChangeType);
        wBool (ar, "enabled",            t->bEnabled);
        wBool (ar, "expand_variables",   t->bExpandVariables);
        wStr  (ar, "group",              t->strGroup);
        wBool (ar, "ignore_case",        t->ignore_case);
        wBool (ar, "inverse",            t->iMatch & INVERSE);
        wBool (ar, "italic",             t->iMatch & BLINK);
        wNum  (ar, "lines_to_match",     t->iLinesToMatch);
        wBool (ar, "keep_evaluating",    t->bKeepEvaluating);
        wBool (ar, "make_bold",          t->iStyle & HILITE);
        wBool (ar, "make_italic",        t->iStyle & BLINK);
        wBool (ar, "make_underline",     t->iStyle & UNDERLINE);
        wStr  (ar, "match",              t->trigger);
        wBool (ar, "match_back_colour",  t->iMatch & TRIGGER_MATCH_BACK);
        wBool (ar, "match_bold",         t->iMatch & TRIGGER_MATCH_HILITE);
        wBool (ar, "match_inverse",      t->iMatch & TRIGGER_MATCH_INVERSE);
        wBool (ar, "match_italic",       t->iMatch & TRIGGER_MATCH_BLINK);
        wBool (ar, "match_text_colour",  t->iMatch & TRIGGER_MATCH_TEXT);
        wBool (ar, "match_underline",    t->iMatch & TRIGGER_MATCH_UNDERLINE);
        wBool (ar, "multi_line",         t->bMultiLine);
        wStr  (ar, "name",               t->strLabel);
        wBool (ar, "one_shot",           t->bOneShot);
        wBool (ar, "omit_from_log",      t->omit_from_log);
        wBool (ar, "omit_from_output",   t->bOmitFromOutput);
        wBool (ar, "regexp",             t->bRegexp);
        wBool (ar, "repeat",             t->bRepeat);
        wStr  (ar, "script",             t->strProcedure);
        wNum  (ar, "send_to",            t->iSendTo);
        wNum  (ar, "sequence",           t->iSequence);
        wStr  (ar, "sound",              t->sound_to_play);
        wBool (ar, "sound_if_inactive",  t->bSoundIfInactive);
        wBool (ar, "lowercase_wildcard", t->bLowercaseWildcard);
        wBool (ar, "temporary",          t->bTemporary);
        wNum  (ar, "text_colour",        (t->iMatch >> 4) & 0x0F);
        wNum  (ar, "user",               t->iUserOption);
        wStr  (ar, "variable",           t->strVariable);
        if (t->iOtherForeground)
            wColour(ar, "other_text_colour", t->iOtherForeground);
        if (t->iOtherBackground)
            wColour(ar, "other_back_colour", t->iOtherBackground);

        ar.WriteString("  >" NL);
        wMulti(ar, "send", t->contents);
        ar.WriteString("  </trigger>" NL);
    }

    ar.WriteString("</triggers>" NL);
}

static void save_aliases(CArchive& ar, CMUSHclientDoc& doc)
{
    ar.WriteString("<aliases>" NL);

    POSITION pos = doc.m_AliasMap.GetStartPosition();
    while (pos) {
        CString key;
        CAlias* a = nullptr;
        doc.m_AliasMap.GetNextAssoc(pos, key, a);
        if (!a || a->bTemporary || a->bIncluded) continue;

        ar.WriteString("  <alias" NL);
        wStr  (ar, "name",                      a->strLabel);
        wStr  (ar, "script",                    a->strProcedure);
        wStr  (ar, "match",                     a->name);
        wBool (ar, "enabled",                   a->bEnabled);
        wBool (ar, "echo_alias",                a->bEchoAlias);
        wBool (ar, "expand_variables",          a->bExpandVariables);
        wStr  (ar, "group",                     a->strGroup);
        wStr  (ar, "variable",                  a->strVariable);
        wBool (ar, "omit_from_command_history", a->bOmitFromCommandHistory);
        wBool (ar, "omit_from_log",             a->bOmitFromLog);
        wBool (ar, "regexp",                    a->bRegexp);
        wNum  (ar, "send_to",                   a->iSendTo);
        wBool (ar, "omit_from_output",          a->bOmitFromOutput);
        wBool (ar, "one_shot",                  a->bOneShot);
        wBool (ar, "menu",                      a->bMenu);
        wBool (ar, "ignore_case",               a->bIgnoreCase);
        wBool (ar, "keep_evaluating",           a->bKeepEvaluating);
        wNum  (ar, "sequence",                  a->iSequence);
        wBool (ar, "temporary",                 a->bTemporary);
        wNum  (ar, "user",                      a->iUserOption);

        ar.WriteString("  >" NL);
        wMulti(ar, "send", a->contents);
        ar.WriteString("  </alias>" NL);
    }

    ar.WriteString("</aliases>" NL);
}

static void save_timers(CArchive& ar, CMUSHclientDoc& doc)
{
    ar.WriteString("<timers>" NL);

    POSITION pos = doc.m_TimerMap.GetStartPosition();
    while (pos) {
        CString key;
        CTimer* t = nullptr;
        doc.m_TimerMap.GetNextAssoc(pos, key, t);
        if (!t || t->bTemporary || t->bIncluded) continue;

        ar.WriteString("  <timer ");
        wStr   (ar, "name",             t->strLabel,     true);
        wStr   (ar, "script",           t->strProcedure, true);
        wBool  (ar, "enabled",          t->bEnabled,     true);
        wNum   (ar, "hour",    (long)(t->iType == CTimer::eAtTime ? t->iAtHour    : t->iEveryHour),   true);
        wNum   (ar, "minute",  (long)(t->iType == CTimer::eAtTime ? t->iAtMinute  : t->iEveryMinute), true);
        wDouble(ar, "second",           t->iType == CTimer::eAtTime ? t->fAtSecond  : t->fEverySecond, true);
        wNum   (ar, "offset_hour",   (long)t->iOffsetHour,   true);
        wNum   (ar, "offset_minute", (long)t->iOffsetMinute, true);
        wDouble(ar, "offset_second",    t->fOffsetSecond, true);
        wNum   (ar, "send_to",       (long)t->iSendTo);
        wBool  (ar, "temporary",        t->bTemporary);
        wNum   (ar, "user",             t->iUserOption);
        wBool  (ar, "at_time",          t->iType,        true);
        wStr   (ar, "group",            t->strGroup,     true);
        wStr   (ar, "variable",         t->strVariable);
        wBool  (ar, "one_shot",         t->bOneShot,     true);
        wBool  (ar, "omit_from_output", t->bOmitFromOutput, true);
        wBool  (ar, "omit_from_log",    t->bOmitFromLog,    true);
        wBool  (ar, "active_closed",    t->bActiveWhenClosed, true);
        ar.WriteString(">" NL);
        wMulti(ar, "send", t->strContents);
        ar.WriteString(NL "  </timer>" NL);
    }

    ar.WriteString("</timers>" NL);
}

static void save_variables(CArchive& ar, CMUSHclientDoc& doc)
{
    ar.WriteString("<variables>" NL);

    POSITION pos = doc.GetVariableMap().GetStartPosition();
    while (pos) {
        CString key;
        CVariable* v = nullptr;
        doc.GetVariableMap().GetNextAssoc(pos, key, v);
        if (!v) continue;
        ar.WriteString(CFormat("  <variable name=\"%s\">%s</variable>" NL,
                        (LPCTSTR)v->strLabel,
                        (LPCTSTR)FixMulti(v->strContents)));
    }

    ar.WriteString("</variables>" NL);
}

// ── CMUSHclientDoc::Serialize_World_XML ──────────────────────────────────────

void CMUSHclientDoc::Serialize_World_XML(CArchive& ar)
{
    // Portable save only; loading uses xml_load_world_portable.cpp.
    if (!ar.IsStoring()) return;

    ar.WriteString("<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>" NL);
    ar.WriteString("<!DOCTYPE muclient>" NL);
    ar.WriteString(CFormat("<!-- MuClient version %s -->" NL, MUSHCLIENT_VERSION));
    ar.WriteString("<!-- Written by MUSHclient -->" NL);
    ar.WriteString("<muclient>" NL);

    save_general  (ar, *this);
    save_triggers (ar, *this);
    save_aliases  (ar, *this);
    save_timers   (ar, *this);
    save_variables(ar, *this);

    ar.WriteString("</muclient>" NL);
}

#endif // !_WIN32
