// scripting/world_debug_portable.cpp — portable implementation of world.Debug()
//
// On Windows the full implementation lives in world_debug.cpp (mushapp only)
// and displays a rich interactive debug report in the MUSHclient output window.
// This file provides a non-Windows replacement compiled into mushcore.
//
// Supported commands (output via Note/ColourNote, same as Windows):
//   triggerlist   — list all triggers in evaluation order
//   aliaslist     — list all aliases in evaluation order
//   timerlist     — list all timers sorted by next-fire time
//   variablelist  — list all variables alphabetically
//   telnetlist    — show telnet negotiation state for active options
//   pluginlist    — list loaded plugins
//   info          — world summary (host, port, scripting, connection)
//   (anything else) — show available commands

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "OtherTypes.h"

#include <map>
#include <string>
#include <algorithm>
using std::map;
using std::string;

// ── constants (mirrors stdafx.h) ──────────────────────────────────────────
#ifndef PLUGIN_UNIQUE_ID_LENGTH
#  define PLUGIN_UNIQUE_ID_LENGTH 24
#endif

// ── colour constants (mirrors stdafx.h / world_debug.cpp) ─────────────────
#ifndef SCRIPTERRORCONTEXTFORECOLOUR
#  define SCRIPTERRORCONTEXTFORECOLOUR "burlywood"
#endif
#ifndef SCRIPTERRORFORECOLOUR
#  define SCRIPTERRORFORECOLOUR "orangered"
#endif

// ── local helpers ─────────────────────────────────────────────────────────
static const char* yn(bool b)  { return b ? "yes" : "no"; }
static const char* en(bool b)  { return b ? "Enabled" : "DISABLED"; }

// Truncate a CString to maxlen, appending "..." if needed.
static CString Truncate(const CString& s, int maxlen)
{
    if (s.GetLength() <= maxlen)
        return s;
    return s.Left(maxlen - 3) + "...";
}

// ── CMUSHclientDoc::DebugHelper ───────────────────────────────────────────
void CMUSHclientDoc::DebugHelper(const CString strAction, CString strArgument)
{
    // Plugin prefix: _<id>_  — switch context to that plugin.
    // (Portable build supports it for completeness; most commands still
    // use the world context when no plugin is active.)
    CPlugin* pSavedPlugin = m_CurrentPlugin;
    m_CurrentPlugin = nullptr;

    if (strArgument.GetLength() >= PLUGIN_UNIQUE_ID_LENGTH + 2 &&
        strArgument.Left(1) == "_" &&
        strArgument.Mid(PLUGIN_UNIQUE_ID_LENGTH + 1, 1) == "_")
    {
        CString plugID = strArgument.Mid(1, PLUGIN_UNIQUE_ID_LENGTH);
        strArgument    = strArgument.Mid(PLUGIN_UNIQUE_ID_LENGTH + 2);
        for (CPlugin* p : m_PluginList)
            if (p->m_strID == plugID) { m_CurrentPlugin = p; break; }
        if (!m_CurrentPlugin) {
            Note(TFormat("Plugin ID %s does not exist.", (LPCTSTR)plugID));
            m_CurrentPlugin = pSavedPlugin;
            return;
        }
        Note("");
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   TFormat("(For plugin: %s)", (LPCTSTR)m_CurrentPlugin->m_strName));
        if (!m_CurrentPlugin->m_bEnabled)
            ColourNote(SCRIPTERRORFORECOLOUR, "",
                       TFormat("Warning: Plugin '%s' disabled.",
                               (LPCTSTR)m_CurrentPlugin->m_strName));
    }
    else
    {
        Note("");
    }

    strArgument.MakeLower();

    // Resolve trigger/alias/timer/variable maps: prefer plugin context.
    CTriggerArray&  triggers  = m_CurrentPlugin ? m_CurrentPlugin->m_TriggerArray  : GetTriggerArray();
    CTriggerRevMap& trigRevMap= m_CurrentPlugin ? m_CurrentPlugin->m_TriggerRevMap : GetTriggerRevMap();
    CAliasArray&    aliases   = m_CurrentPlugin ? m_CurrentPlugin->m_AliasArray    : GetAliasArray();
    CAliasRevMap&   aliRevMap = m_CurrentPlugin ? m_CurrentPlugin->m_AliasRevMap   : GetAliasRevMap();
    CTimerMap&      timers    = m_CurrentPlugin ? m_CurrentPlugin->m_TimerMap      : GetTimerMap();
    CTimerRevMap&   timRevMap = m_CurrentPlugin ? m_CurrentPlugin->m_TimerRevMap   : GetTimerRevMap();
    CVariableMap&   variables = m_CurrentPlugin ? m_CurrentPlugin->m_VariableMap   : GetVariableMap();

    // ── triggerlist ───────────────────────────────────────────────────────
    if (strAction == "triggerlist")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ Trigger List (evaluation order) ------");
        if (!m_enable_triggers)
            ColourNote(SCRIPTERRORFORECOLOUR, "", "Warning: All triggers disabled.");
        Note("");

        int n = 0;
        for (int i = 0; i < triggers.GetSize(); i++, n++)
        {
            CTrigger* t = triggers[i];
            const char* label = t->strLabel[0] ? (const char*)t->strLabel
                                                : trigRevMap[t].c_str();
            ColourNote(t->bEnabled ? "white" : "gray", "",
                       CFormat("%3i. %6s, %8s : %-14s  %s",
                               n + 1,
                               t->bRegexp ? "Regexp" : "Normal",
                               en(t->bEnabled),
                               label,
                               (LPCTSTR)Truncate(t->trigger, m_nWrapColumn - 44)));
        }
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   TFormat("%i trigger%s.", PLURAL(n)));
    }

    // ── aliaslist ─────────────────────────────────────────────────────────
    else if (strAction == "aliaslist")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ Alias List (evaluation order) ------");
        if (!m_enable_aliases)
            ColourNote(SCRIPTERRORFORECOLOUR, "", "Warning: All aliases disabled.");
        Note("");

        int n = 0;
        for (int i = 0; i < aliases.GetSize(); i++, n++)
        {
            CAlias* a = aliases[i];
            const char* label = a->strLabel[0] ? (const char*)a->strLabel
                                               : aliRevMap[a].c_str();
            ColourNote(a->bEnabled ? "white" : "gray", "",
                       CFormat("%3i. %6s, %8s : %-14s  %s",
                               n + 1,
                               a->bRegexp ? "Regexp" : "Normal",
                               en(a->bEnabled),
                               label,
                               (LPCTSTR)Truncate(a->name, m_nWrapColumn - 44)));
        }
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   TFormat("%i alias%s.", PLURAL(n)));
    }

    // ── timerlist ─────────────────────────────────────────────────────────
    else if (strAction == "timerlist")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ Timer List (firing order) ------");
        if (!m_bEnableTimers)
            ColourNote(SCRIPTERRORFORECOLOUR, "", "Warning: All timers disabled.");
        if (m_iConnectPhase != eConnectConnectedToMud)
            ColourNote(SCRIPTERRORFORECOLOUR, "",
                       "Warning: Not connected — some timers may not fire.");
        Note("");

        // Sort by fire time
        map<double, CTimer*> sorted;
        for (POSITION pos = timers.GetStartPosition(); pos; )
        {
            CTimer* tm; CString nm;
            timers.GetNextAssoc(pos, nm, tm);
            sorted[tm->tFireTime.GetTime()] = tm;
        }

        int n = 0;
        CmcDateTime now = CmcDateTime::GetTimeNow();
        for (auto& kv : sorted)
        {
            CTimer* tm = kv.second;
            const char* label = tm->strLabel[0] ? (const char*)tm->strLabel
                                                : timRevMap[tm].c_str();
            const char* typ = (tm->iType == CTimer::eInterval) ? "Every" : "At";

            CString line = CFormat("%3i. %8s : %-14s  %5s %02i:%02i:%04.2f",
                                   ++n,
                                   en(tm->bEnabled),
                                   label,
                                   typ,
                                   tm->iAtHour + tm->iEveryHour,
                                   tm->iAtMinute + tm->iEveryMinute,
                                   tm->fAtSecond + tm->fEverySecond);
            if (tm->tFireTime.GetTime() > 0) {
                CmcDateTimeSpan ts = tm->tFireTime - now;
                line += TFormat(" — fires in %8.1f s", ts.GetTotalSeconds());
            }
            ColourNote(tm->bEnabled ? "white" : "gray", "", line);
        }
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   TFormat("%i timer%s.", PLURAL(n)));
    }

    // ── variablelist ──────────────────────────────────────────────────────
    else if (strAction == "variablelist")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ Variable List (alphabetic order) ------");
        Note("");

        map<string, string> sorted;
        for (POSITION pos = variables.GetStartPosition(); pos; )
        {
            CString name; CVariable* v;
            variables.GetNextAssoc(pos, name, v);
            CString val = v->strContents;
            // Normalise newlines, truncate
            val.Replace("\r", "");
            val.Replace("\n", "\\n");
            sorted[(LPCTSTR)name] = (LPCTSTR)Truncate(val, m_nWrapColumn - 34);
        }

        for (auto& kv : sorted)
            Note(CFormat("%-30s = %s", kv.first.c_str(), kv.second.c_str()));

        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   TFormat("%i variable%s.", PLURAL((int)sorted.size())));
    }

    // ── telnetlist ────────────────────────────────────────────────────────
    else if (strAction == "telnetlist")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ Telnet Negotiation List (numeric order) ------");
        Note("");

        static const struct { int opt; const char* name; } kTelnetNames[] = {
            {  1, "Echo" },
            {  3, "Suppress Go-ahead (SGA)" },
            {  5, "Status" },
            {  6, "Timing Mark" },
            { 24, "Terminal type" },
            { 25, "End of record (EOR)" },
            { 31, "Window Size (NAWS)" },
            { 32, "Terminal Speed" },
            { 33, "Remote Flow Control" },
            { 34, "Line Mode" },
            { 36, "Environment Variables" },
            { 42, "Character set" },
            { 85, "MCCP1" },
            { 86, "MCCP2" },
            { 90, "MSP (MUD Sound Protocol)" },
            { 91, "MXP (MUD eXtension Protocol)" },
            { 93, "ZMP" },
            {102, "Aardwolf" },
            {200, "ATCP" },
            {201, "ATCP2/GMCP" },
            {255, "Extended Options" },
            { -1, nullptr }
        };

        for (int i = 0; i < 256; i++)
        {
            if (!m_bClient_sent_IAC_DO[i]   && !m_bClient_sent_IAC_DONT[i] &&
                !m_bClient_sent_IAC_WILL[i]  && !m_bClient_sent_IAC_WONT[i] &&
                !m_bClient_got_IAC_DO[i]     && !m_bClient_got_IAC_DONT[i] &&
                !m_bClient_got_IAC_WILL[i]   && !m_bClient_got_IAC_WONT[i])
                continue;

            const char* meaning = "";
            for (int k = 0; kTelnetNames[k].opt >= 0; k++)
                if (kTelnetNames[k].opt == i) { meaning = kTelnetNames[k].name; break; }

            Note(CFormat("%3i%s%s", i, meaning[0] ? " " : "", meaning));
            ColourTell("cyan", "", "     Got:  ");
            Note(CFormat("DO: %-3s  DONT: %-3s  WILL: %-3s  WONT: %-3s",
                         yn(m_bClient_got_IAC_DO[i]),
                         yn(m_bClient_got_IAC_DONT[i]),
                         yn(m_bClient_got_IAC_WILL[i]),
                         yn(m_bClient_got_IAC_WONT[i])));
            ColourTell("cyan", "", "     Sent: ");
            Note(CFormat("WILL: %-3s  WONT: %-3s  DO: %-3s  DONT: %-3s",
                         yn(m_bClient_sent_IAC_WILL[i]),
                         yn(m_bClient_sent_IAC_WONT[i]),
                         yn(m_bClient_sent_IAC_DO[i]),
                         yn(m_bClient_sent_IAC_DONT[i])));
        }
    }

    // ── pluginlist ────────────────────────────────────────────────────────
    else if (strAction == "pluginlist")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ Plugin List ------");
        Note("");
        int n = 0;
        for (CPlugin* p : m_PluginList)
        {
            ColourNote(p->m_bEnabled ? "white" : "gray", "",
                       CFormat("%3i. %-20s  %s  ID: %s",
                               ++n,
                               (LPCTSTR)p->m_strName,
                               en(p->m_bEnabled),
                               (LPCTSTR)p->m_strID));
        }
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   TFormat("%i plugin%s.", PLURAL(n)));
    }

    // ── info ──────────────────────────────────────────────────────────────
    else if (strAction == "info")
    {
        ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
                   "------ World Info ------");
        Note(CFormat("  World name   : %s", (LPCTSTR)m_mush_name));
        Note(CFormat("  Host         : %s  port %u", (LPCTSTR)m_server, (unsigned)m_port));
        Note(CFormat("  Connected    : %s",
                     m_iConnectPhase == eConnectConnectedToMud ? "yes" : "no"));
        Note(CFormat("  Script file  : %s",
                     m_strScriptFilename.IsEmpty() ? "(none)"
                                                   : (LPCTSTR)m_strScriptFilename));
        Note(CFormat("  Language     : %s",
                     m_strLanguage.IsEmpty() ? "(none)" : (LPCTSTR)m_strLanguage));
        Note(CFormat("  Scripting    : %s",
                     m_ScriptEngine ? "active" : "inactive"));
        Note(CFormat("  Triggers     : %s  (%i defined)",
                     en(m_enable_triggers), GetTriggerArray().GetSize()));
        Note(CFormat("  Aliases      : %s  (%i defined)",
                     en(m_enable_aliases), GetAliasArray().GetSize()));
        Note(CFormat("  Timers       : %s",  en(m_bEnableTimers)));
        Note(CFormat("  Plugins      : %i", (int)m_PluginList.size()));
    }

    // ── unknown ───────────────────────────────────────────────────────────
    else
    {
        ColourNote(SCRIPTERRORFORECOLOUR, "",
                   TFormat("Unknown debug command: \"%s\"", (LPCTSTR)strAction));
        Note("Available commands:");
        Note("  triggerlist   — list triggers");
        Note("  aliaslist     — list aliases");
        Note("  timerlist     — list timers");
        Note("  variablelist  — list variables");
        Note("  telnetlist    — show telnet negotiation");
        Note("  pluginlist    — list plugins");
        Note("  info          — world summary");
    }

    m_CurrentPlugin = pSavedPlugin;
}

// ── CMUSHclientDoc::Debug ─────────────────────────────────────────────────
// Entry point called from world.Debug(command) in Lua.
// Parses the first word as the action and passes the rest as the argument.
VARIANT CMUSHclientDoc::Debug(LPCTSTR Command)
{
    VARIANT vaResult;
    VariantInit(&vaResult);

    CString cmd(Command);
    cmd.TrimLeft();
    cmd.TrimRight();
    if (cmd.IsEmpty()) {
        // No command — show help
        DebugHelper("", "");
        return vaResult;
    }

    // Split first word (action) from remainder (argument)
    int sp = cmd.Find(' ');
    CString action, argument;
    if (sp < 0) {
        action   = cmd;
        argument = "";
    } else {
        action   = cmd.Left(sp);
        argument = cmd.Mid(sp + 1);
        argument.TrimLeft();
    }
    action.MakeLower();

    DebugHelper(action, argument);
    return vaResult;
}

// ── DebugShow overloads ───────────────────────────────────────────────────
// On Windows these write to the debug output pane.  Portably they use Note().
void CMUSHclientDoc::DebugShow(const char* sTitle, const char* sString)
{
    Note(CFormat("%-30s = %s", sTitle, sString));
}
void CMUSHclientDoc::DebugShow(const char* sTitle, const long iNumber)
{
    Note(CFormat("%-30s = %ld", sTitle, iNumber));
}
void CMUSHclientDoc::DebugShow(const char* sTitle, const bool iBoolean)
{
    Note(CFormat("%-30s = %s", sTitle, yn(iBoolean)));
}
void CMUSHclientDoc::DebugShow(const char* sTitle, const CmcDateTime theDate)
{
    Note(CFormat("%-30s = %s", sTitle, (LPCTSTR)theDate.Format("%Y-%m-%d %H:%M:%S")));
}
void CMUSHclientDoc::DebugShow(const char* sTitle, const CTime theDate)
{
    Note(CFormat("%-30s = %s", sTitle, (LPCTSTR)theDate.Format("%Y-%m-%d %H:%M:%S")));
}
void CMUSHclientDoc::DebugShowD(const char* sTitle, const double fNumber)
{
    Note(CFormat("%-30s = %g", sTitle, fNumber));
}

// ── ShowOptions / ShowAlphaOptions ────────────────────────────────────────
void CMUSHclientDoc::ShowOptions()
{
    // Full OptionsTable not available in mushcore — list option cache instead.
    ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
               "------ World Numeric Options ------");
    for (auto& kv : m_numericOptionCache)
        Note(CFormat("%-40s = %ld", kv.first.c_str(), kv.second));
}

void CMUSHclientDoc::ShowAlphaOptions()
{
    ColourNote(SCRIPTERRORCONTEXTFORECOLOUR, "",
               "------ World Alpha Options ------");
    for (auto& kv : m_alphaOptionCache)
        Note(CFormat("%-40s = %s", kv.first.c_str(), kv.second.c_str()));
}

#endif // !_WIN32
