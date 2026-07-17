// xml/xml_save_world_portable.cpp
//
// Portable (non-Windows) implementation of CMUSHclientDoc::Serialize_World_XML.
//
// Saves a world file in the same XML format as the Windows xml_save_world.cpp,
// reading world settings through the REAL offsetof-based GetOptionItem /
// GetAlphaOptionItem machinery (scriptingoptions.cpp, compiled into mushcore
// since the option convergence).  Unlike Windows this saver omits options that
// equal their table default; because the loader keeps defaults for absent
// attributes, every non-default value is written EXPLICITLY (booleans as y/n,
// zeros verbatim, empty strings as name="").
//
// The resulting files are fully interchangeable with those written by the Windows
// build and can be loaded by xml_load_world.cpp or xml_load_world_portable.cpp.
//
// Guard: compiled only on non-Windows (mushcore target).

#ifndef _WIN32

#include "../platform/doc_stub.h"

#include <cstring>
#include <climits>
#include <functional>

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

// ── Section writers ───────────────────────────────────────────────────────────

static void save_general(CArchive& ar, const CMUSHclientDoc& doc)
{
    // GetOptionItem / GetAlphaOptionItem are non-const on Windows (doc.h);
    // cast the const away rather than fork the canonical signatures.
    CMUSHclientDoc& ncDoc = const_cast<CMUSHclientDoc&>(doc);

    ar.WriteString("<world" NL);
    ar.WriteString(CFormat("   muclient_version=\"%s\"" NL, MUSHCLIENT_VERSION));
    ar.WriteString("   world_file_version=\"1\"" NL NL);

    // ── single-line alpha options ─────────────────────────────────────────
    for (int i = 0; AlphaOptionsTable[i].pName; i++) {
        if (AlphaOptionsTable[i].iFlags & OPT_MULTLINE)  continue;

        CString val = ncDoc.GetAlphaOptionItem(i);
        const char* def = AlphaOptionsTable[i].sDefault ? AlphaOptionsTable[i].sDefault : "";
        if (val == CString(def)) continue;

        // Passwords are base64-encoded with a <name>_base64="y" flag, like Windows.
        // (Build the flag name by concatenation, not CFormat: a single char* arg
        // exact-matches the CFormat(LPCTSTR, va_list) overload — va_list is char*
        // on arm64 — and would be misread as a va_list.)
        if ((AlphaOptionsTable[i].iFlags & OPT_PASSWORD) && !val.IsEmpty()) {
            CString flag = CString(AlphaOptionsTable[i].pName) + "_base64";
            wBool(ar, (const char*) flag, true);
            val = encodeBase64(val, false);
        }

        // Written explicitly even when empty: the value differs from the table
        // default and the loader keeps defaults for absent attributes, so an
        // empty non-default value must still appear (wStr would omit it).
        {
            CString v = val;
            v.Replace("\r\n", " ");
            v.Replace("\n",   " ");
            ar.WriteString(CFormat("   %s=\"%s\"" NL,
                           AlphaOptionsTable[i].pName,
                           (LPCTSTR)FixHTMLString(v)));
        }
    }

    ar.WriteString(NL);

    // ── numeric options ───────────────────────────────────────────────────
    for (int i = 0; OptionsTable[i].pName; i++) {
        long val = ncDoc.GetOptionItem(i);

        if (val == (long)OptionsTable[i].iDefault) continue;

        bool is_bool   = (OptionsTable[i].iMinimum == 0 && OptionsTable[i].iMaximum == 0);
        bool is_colour = (OptionsTable[i].iFlags & OPT_RGB_COLOUR) != 0;

        // Written explicitly even for false/zero (wBool/wNum would omit them
        // and the loader would then restore the default): booleans as y/n,
        // numbers verbatim.  ColourToName never returns empty portably
        // ("#RRGGBB" fallback), so wColour never omits.
        if (is_bool)
            ar.WriteString(CFormat("   %s=\"%s\"" NL,
                           OptionsTable[i].pName, val ? "y" : "n"));
        else if (is_colour)
            wColour(ar, OptionsTable[i].pName, (COLORREF)val);
        else
            ar.WriteString(CFormat("   %s=\"%ld\"" NL,
                           OptionsTable[i].pName, val));
    }

    ar.WriteString("   > <!-- end of general world attributes -->" NL);

    // ── multi-line alpha options ──────────────────────────────────────────
    for (int i = 0; AlphaOptionsTable[i].pName; i++) {
        if (!(AlphaOptionsTable[i].iFlags & OPT_MULTLINE)) continue;

        CString val = ncDoc.GetAlphaOptionItem(i);
        const char* def = AlphaOptionsTable[i].sDefault ? AlphaOptionsTable[i].sDefault : "";
        if (val == CString(def)) continue;

        // Explicit empty element for an empty non-default value (see above).
        ar.WriteString(CFormat("  <%s>%s</%s>" NL,
                       AlphaOptionsTable[i].pName,
                       (LPCTSTR)FixMulti(val),
                       AlphaOptionsTable[i].pName));
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

static void save_colours(CArchive& ar, const CMUSHclientDoc& doc)
{
    ar.WriteString("<colours>" NL);
    ar.WriteString(NL "<ansi>" NL);

    ar.WriteString(NL " <normal>" NL);
    for (int i = 0; i < 8; i++) {
        ar.WriteString(CFormat("   <colour seq=\"%d\" rgb=\"%s\"/>" NL,
                               i + 1, (LPCTSTR)ColourToName(doc.m_normalcolour[i])));
    }
    ar.WriteString(NL " </normal>" NL);

    ar.WriteString(NL " <bold>" NL);
    for (int i = 0; i < 8; i++) {
        ar.WriteString(CFormat("   <colour seq=\"%d\" rgb=\"%s\"/>" NL,
                               i + 1, (LPCTSTR)ColourToName(doc.m_boldcolour[i])));
    }
    ar.WriteString(NL " </bold>" NL);

    ar.WriteString(NL "</ansi>" NL);

    ar.WriteString(NL "<custom>" NL);
    for (int i = 0; i < MAX_CUSTOM; i++) {
        ar.WriteString(CFormat("  <colour seq=\"%d\" name=\"%s\" text=\"%s\" back=\"%s\"/>" NL,
                               i + 1,
                               (LPCTSTR)FixHTMLString(doc.m_strCustomColourName[i]),
                               (LPCTSTR)ColourToName(doc.m_customtext[i]),
                               (LPCTSTR)ColourToName(doc.m_customback[i])));
    }
    ar.WriteString(NL "</custom>" NL);

    ar.WriteString(NL "</colours>" NL);
}

// ── Single-item save helpers (for ExportXML Lua API) ─────────────────────────
// Each helper serialises one named item (trigger / alias / timer / variable)
// to an in-memory XML string that matches the format ExportXML returns on
// Windows.  An empty CString is returned when the item does not exist.

static CString save_item_to_string(
        const std::function<void(CArchive&)>& write_header,
        const std::function<void(CArchive&)>& write_item,
        const std::function<void(CArchive&)>& write_footer)
{
    CMemFile f;
    CArchive ar(&f, CArchive::store);
    ar.WriteString("<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>" NL);
    ar.WriteString("<!DOCTYPE muclient>" NL);
    ar.WriteString("<muclient>" NL);
    write_header(ar);
    write_item(ar);
    write_footer(ar);
    ar.WriteString("</muclient>" NL);
    ar.Close();
    UINT n = f.GetLength();
    BYTE* p = f.Detach();
    CString result;
    if (p) {
        result = CString(reinterpret_cast<char*>(p), n);
        free(p);
    }
    return result;
}

CString CMUSHclientDoc::ExportXML_Trigger(const CString& strName)
{
    CTrigger* t = nullptr;
    if (!GetTriggerMap().Lookup(strName, t) || !t)
        return CString();
    return save_item_to_string(
        [](CArchive& a){ a.WriteString("<triggers>" NL); },
        [t](CArchive& a){
            a.WriteString("  <trigger" NL);
            wNum  (a, "back_colour",        (t->iMatch >> 8) & 0x0F);
            wBool (a, "bold",               t->iMatch & HILITE);
            wNum  (a, "clipboard_arg",      t->iClipboardArg);
            if (t->colour != SAMECOLOUR)
                wNum(a, "custom_colour",    t->colour + 1);
            wNum  (a, "colour_change_type", t->iColourChangeType);
            wBool (a, "enabled",            t->bEnabled);
            wBool (a, "expand_variables",   t->bExpandVariables);
            wStr  (a, "group",              t->strGroup);
            wBool (a, "ignore_case",        t->ignore_case);
            wBool (a, "inverse",            t->iMatch & INVERSE);
            wBool (a, "italic",             t->iMatch & BLINK);
            wNum  (a, "lines_to_match",     t->iLinesToMatch);
            wBool (a, "keep_evaluating",    t->bKeepEvaluating);
            wBool (a, "make_bold",          t->iStyle & HILITE);
            wBool (a, "make_italic",        t->iStyle & BLINK);
            wBool (a, "make_underline",     t->iStyle & UNDERLINE);
            wStr  (a, "match",              t->trigger);
            wBool (a, "match_back_colour",  t->iMatch & TRIGGER_MATCH_BACK);
            wBool (a, "match_bold",         t->iMatch & TRIGGER_MATCH_HILITE);
            wBool (a, "match_inverse",      t->iMatch & TRIGGER_MATCH_INVERSE);
            wBool (a, "match_italic",       t->iMatch & TRIGGER_MATCH_BLINK);
            wBool (a, "match_text_colour",  t->iMatch & TRIGGER_MATCH_TEXT);
            wBool (a, "match_underline",    t->iMatch & TRIGGER_MATCH_UNDERLINE);
            wBool (a, "multi_line",         t->bMultiLine);
            wStr  (a, "name",               t->strLabel);
            wBool (a, "one_shot",           t->bOneShot);
            wBool (a, "omit_from_log",      t->omit_from_log);
            wBool (a, "omit_from_output",   t->bOmitFromOutput);
            wBool (a, "regexp",             t->bRegexp);
            wBool (a, "repeat",             t->bRepeat);
            wStr  (a, "script",             t->strProcedure);
            wNum  (a, "send_to",            t->iSendTo);
            wNum  (a, "sequence",           t->iSequence);
            wStr  (a, "sound",              t->sound_to_play);
            wBool (a, "sound_if_inactive",  t->bSoundIfInactive);
            wBool (a, "lowercase_wildcard", t->bLowercaseWildcard);
            wBool (a, "temporary",          t->bTemporary);
            wNum  (a, "text_colour",        (t->iMatch >> 4) & 0x0F);
            wNum  (a, "user",               t->iUserOption);
            wStr  (a, "variable",           t->strVariable);
            if (t->iOtherForeground)
                wColour(a, "other_text_colour", t->iOtherForeground);
            if (t->iOtherBackground)
                wColour(a, "other_back_colour", t->iOtherBackground);
            a.WriteString("  >" NL);
            wMulti(a, "send", t->contents);
            a.WriteString("  </trigger>" NL);
        },
        [](CArchive& a){ a.WriteString("</triggers>" NL); });
}

CString CMUSHclientDoc::ExportXML_Alias(const CString& strName)
{
    CAlias* al = nullptr;
    if (!GetAliasMap().Lookup(strName, al) || !al)
        return CString();
    return save_item_to_string(
        [](CArchive& a){ a.WriteString("<aliases>" NL); },
        [al](CArchive& a){
            a.WriteString("  <alias" NL);
            wStr  (a, "name",                      al->strLabel);
            wStr  (a, "script",                    al->strProcedure);
            wStr  (a, "match",                     al->name);
            wBool (a, "enabled",                   al->bEnabled);
            wBool (a, "echo_alias",                al->bEchoAlias);
            wBool (a, "expand_variables",          al->bExpandVariables);
            wStr  (a, "group",                     al->strGroup);
            wStr  (a, "variable",                  al->strVariable);
            wBool (a, "omit_from_command_history", al->bOmitFromCommandHistory);
            wBool (a, "omit_from_log",             al->bOmitFromLog);
            wBool (a, "regexp",                    al->bRegexp);
            wNum  (a, "send_to",                   al->iSendTo);
            wBool (a, "omit_from_output",          al->bOmitFromOutput);
            wBool (a, "one_shot",                  al->bOneShot);
            wBool (a, "menu",                      al->bMenu);
            wBool (a, "ignore_case",               al->bIgnoreCase);
            wBool (a, "keep_evaluating",           al->bKeepEvaluating);
            wNum  (a, "sequence",                  al->iSequence);
            wBool (a, "temporary",                 al->bTemporary);
            wNum  (a, "user",                      al->iUserOption);
            a.WriteString("  >" NL);
            wMulti(a, "send", al->contents);
            a.WriteString("  </alias>" NL);
        },
        [](CArchive& a){ a.WriteString("</aliases>" NL); });
}

CString CMUSHclientDoc::ExportXML_Timer(const CString& strName)
{
    CTimer* t = nullptr;
    if (!GetTimerMap().Lookup(strName, t) || !t)
        return CString();
    return save_item_to_string(
        [](CArchive& a){ a.WriteString("<timers>" NL); },
        [t](CArchive& a){
            a.WriteString("  <timer ");
            wStr   (a, "name",             t->strLabel,     true);
            wStr   (a, "script",           t->strProcedure, true);
            wBool  (a, "enabled",          t->bEnabled,     true);
            wNum   (a, "hour",    (long)(t->iType == CTimer::eAtTime ? t->iAtHour    : t->iEveryHour),   true);
            wNum   (a, "minute",  (long)(t->iType == CTimer::eAtTime ? t->iAtMinute  : t->iEveryMinute), true);
            wDouble(a, "second",           t->iType == CTimer::eAtTime ? t->fAtSecond  : t->fEverySecond, true);
            wNum   (a, "offset_hour",   (long)t->iOffsetHour,   true);
            wNum   (a, "offset_minute", (long)t->iOffsetMinute, true);
            wDouble(a, "offset_second",    t->fOffsetSecond, true);
            wNum   (a, "send_to",       (long)t->iSendTo);
            wBool  (a, "temporary",        t->bTemporary);
            wNum   (a, "user",             t->iUserOption);
            wBool  (a, "at_time",          t->iType,        true);
            wStr   (a, "group",            t->strGroup,     true);
            wStr   (a, "variable",         t->strVariable);
            wBool  (a, "one_shot",         t->bOneShot,     true);
            wBool  (a, "omit_from_output", t->bOmitFromOutput, true);
            wBool  (a, "omit_from_log",    t->bOmitFromLog,    true);
            wBool  (a, "active_closed",    t->bActiveWhenClosed, true);
            a.WriteString(">" NL);
            wMulti(a, "send", t->strContents);
            a.WriteString(NL "  </timer>" NL);
        },
        [](CArchive& a){ a.WriteString("</timers>" NL); });
}

CString CMUSHclientDoc::ExportXML_Variable(const CString& strName)
{
    CVariable* v = nullptr;
    if (!GetVariableMap().Lookup(strName, v) || !v)
        return CString();
    return save_item_to_string(
        [](CArchive& a){ a.WriteString("<variables>" NL); },
        [v](CArchive& a){
            a.WriteString(CFormat("  <variable name=\"%s\">%s</variable>" NL,
                            (LPCTSTR)v->strLabel,
                            (LPCTSTR)FixMulti(v->strContents)));
        },
        [](CArchive& a){ a.WriteString("</variables>" NL); });
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
    save_colours  (ar, *this);

    // Emit <include plugin="y" name="..."/> for each loaded plugin,
    // mirroring Save_Plugins_XML in xml_save_world.cpp.  On non-Windows
    // there is no m_bGlobal concept — all loaded plugins are treated as local.
    if (!m_PluginList.empty()) {
        ar.WriteString(NL "<!-- plugins -->" NL);
        for (CPlugin* p : m_PluginList) {
            ar.WriteString("<include ");
            wStr (ar, "name",   p->m_strSource, /*sameline=*/true);
            wBool(ar, "plugin", true,            /*sameline=*/true);
            ar.WriteString("/>" NL);
        }
    }

    ar.WriteString("</muclient>" NL);
}

#endif // !_WIN32
