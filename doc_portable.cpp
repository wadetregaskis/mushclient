// doc_portable.cpp — non-Windows implementations of CMUSHclientDoc helpers.
//
// On Windows these methods live in the monolithic doc.cpp / MXP files.
// We provide stubs here so the protocol files and test suite can link
// without dragging in MFC or the scripting engine.
//
// Guard: excluded from the Windows build entirely.

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "platform/wx_mfc_hooks.h"
#include "Color.h"
#include "speedwalk.h"   // shared ExpandSpeedwalk / ReverseSpeedwalkString
#ifdef OPENSSL_FOUND
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#  include <openssl/x509.h>
#endif
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <climits>
#include <zlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <time.h>    // clock_gettime, struct timespec

// ── Output hooks (readline integration) ──────────────────────────────────
void (*g_before_output)() = nullptr;
void (*g_after_output)()  = nullptr;

// ── Debug logging ─────────────────────────────────────────────────────────
bool g_debug = false;

void dbg(const char* fmt, ...)
{
    if (!g_debug) return;
    // Temporarily clear readline's input line if active
    if (g_before_output) g_before_output();
    fprintf(stderr, "\033[2m[dbg] ");  // dim style
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\033[0m\n");
    fflush(stderr);
    if (g_after_output) g_after_output();
}

// Human-readable name for a telnet option number.
const char* telopt_name(unsigned char opt)
{
    switch (opt) {
    case   0: return "BINARY";
    case   1: return "ECHO";
    case   3: return "SGA";            // Suppress Go-Ahead
    case  24: return "TERMINAL-TYPE";
    case  25: return "EOR";            // End-of-Record
    case  31: return "NAWS";
    case  32: return "TERMINAL-SPEED";
    case  34: return "LINEMODE";
    case  36: return "ENVIRON";
    case  39: return "NEW-ENVIRON";
    case  42: return "CHARSET";
    case  85: return "COMPRESS(MCCP1)";
    case  86: return "COMPRESS2(MCCP2)";
    case  90: return "MSP";
    case  91: return "MXP";
    case  93: return "ZMP";
    case 102: return "MUD-SPECIFIC(ATCP)";
    case 200: return "ATCP";
    case 201: return "GMCP";
    default:  {
        // Use a small rotating buffer so callers don't need to free
        static char buf[4][16];
        static int  idx = 0;
        snprintf(buf[idx & 3], sizeof buf[0], "<%d>", (int)opt);
        return buf[idx++ & 3];
    }
    }
}

// ── App global (CMUSHclientApp) ───────────────────────────────────────────
CMUSHclientApp App;

// ── Frame stub global ─────────────────────────────────────────────────────
CMainFrame g_stub_frame;

// ── ENDLINE constant ──────────────────────────────────────────────────────
const char ENDLINE[] = "\r\n";

// OptionsTable[] is defined in scriptingoptions_portable.cpp (real table).

// ── xterm 256-colour table ────────────────────────────────────────────────
COLORREF xterm_256_colours[256];


// ── CMUSHclientDoc constructor ────────────────────────────────────────────
CMUSHclientDoc::CMUSHclientDoc()
{
    static bool s_tableReady = false;
    if (!s_tableReady) { Generate256colours(); s_tableReady = true; }

    m_normalcolour[BLACK]   = RGB(  0,   0,   0);
    m_normalcolour[RED]     = RGB(128,   0,   0);
    m_normalcolour[GREEN]   = RGB(  0, 128,   0);
    m_normalcolour[YELLOW]  = RGB(128, 128,   0);
    m_normalcolour[BLUE]    = RGB(  0,   0, 128);
    m_normalcolour[MAGENTA] = RGB(128,   0, 128);
    m_normalcolour[CYAN]    = RGB(  0, 128, 128);
    m_normalcolour[WHITE]   = RGB(192, 192, 192);

    m_boldcolour[BLACK]   = RGB(128, 128, 128);
    m_boldcolour[RED]     = RGB(255,   0,   0);
    m_boldcolour[GREEN]   = RGB(  0, 255,   0);
    m_boldcolour[YELLOW]  = RGB(255, 255,   0);
    m_boldcolour[BLUE]    = RGB(  0,   0, 255);
    m_boldcolour[MAGENTA] = RGB(255,   0, 255);
    m_boldcolour[CYAN]    = RGB(  0, 255, 255);
    m_boldcolour[WHITE]   = RGB(255, 255, 255);

    for (int i = 0; i < MAX_CUSTOM; i++) {
        m_customtext[i] = m_normalcolour[i % 8];
        m_customback[i] = RGB(0, 0, 0);
    }

    m_strTerminalIdentification = "MUSHCLIENT";
    // Default scripting language (AlphaOptionsTable default for "script_language").
    // The GUI applies the options-table defaults at construction; mirror the key
    // one here so a freshly-constructed doc reports "lua" like Windows does.
    m_strLanguage = "lua";
    m_tWorldOpenTime = time(nullptr);

    // ── Scratch line for ANSI style state ─────────────────────────────────
    // InterpretANSIcode / Interpret256ANSIcode always call
    //   m_pCurrentLine->styleList.GetTail()
    // to read the current running style.  In the CLI build there is no real
    // output line; we keep a single heap-allocated CLine with one zero-length
    // CStyle whose values track the current ANSI state.
    m_pScratchLine = new CLine();
    CStyle* pInitStyle = new CStyle();
    pInitStyle->iFlags      = NORMAL;
    pInitStyle->iForeColour = WHITE;
    pInitStyle->iBackColour = BLACK;
    pInitStyle->iLength     = 0;
    m_pScratchLine->styleList.AddTail(pInitStyle);
    m_pCurrentLine = m_pScratchLine;

    // Register with the global world-document list so that
    // world.GetWorld() / GetWorldList() etc. can find this doc.
    App.m_pWorldDocTemplate->AddDoc(this);
}

// ── CMUSHclientDoc destructor ─────────────────────────────────────────────
// On non-Windows, doc_construct.cpp is not compiled into mushcore.  This
// portable destructor — mirrors doc_construct.cpp's ~CMUSHclientDoc for the
// resources that mushcore owns.
CMUSHclientDoc::~CMUSHclientDoc()
{
    // Unregister from the global world-document list first, so no stale
    // pointer remains visible to GetWorld()/GetWorldList() during teardown.
    App.m_pWorldDocTemplate->RemoveDoc(this);

    // Close socket first so no callbacks fire into a partially-destroyed doc.
    TeardownTransport();   // PAL transport + events bridge (doc_shared.cpp)

    // Delete plugins (CPlugin::~CPlugin() fires OnPluginClose for each).
    // Erase from list during iteration so re-entrant PluginList queries see a
    // consistent list (mirrors the Windows destructor in doc_construct.cpp).
    for (auto it = m_PluginList.begin(); it != m_PluginList.end();
         it = m_PluginList.erase(it))
        delete *it;

    // Tear down the world's own Lua script engine.
    delete m_ScriptEngine;
    m_ScriptEngine = nullptr;

    // Free the doc's own trigger/alias/timer/variable maps.
    DELETE_MAP(m_TriggerMap,  CTrigger);
    DELETE_MAP(m_AliasMap,    CAlias);
    DELETE_MAP(m_TimerMap,    CTimer);
    DELETE_MAP(m_VariableMap, CVariable);

    // Free scripting arrays (world.ArrayCreate etc.).
    for (auto& kv : m_Arrays) {
        kv.second->clear();
        delete kv.second;
    }
    m_Arrays.clear();

    // Free SQLite databases (world.DatabaseOpen etc.).
    for (auto& kv : m_Databases) {
        if (kv.second->pStmt)
            sqlite3_finalize(kv.second->pStmt);
        if (kv.second->db)
            sqlite3_close(kv.second->db);
        delete kv.second;
    }
    m_Databases.clear();

    // Free all buffered output lines (Windows: DELETE_LIST(m_LineList) in
    // doc_construct.cpp). Safe now that StartNewLine's re-entrancy guard prevents
    // the same CLine being AddTail'd twice (a trigger <send> echo used to double-
    // add it, which made a blanket free double-free). Each CLine owns a new[]'d
    // text buffer + a style list.
    DELETE_LIST(m_LineList);

    // Free the in-progress MUD output line. After StartNewLine() this is a fresh
    // CLine NOT in m_LineList, so the DELETE_LIST above can't double-free it.
    // (Its text is new[]'d to match CLine::~CLine's delete[], USE_REALLOC off.)
    if (m_pScratchLine) {
        delete m_pScratchLine;
        m_pScratchLine = nullptr;
        m_pCurrentLine = nullptr;
    }
}



// ── AddStyle (action-pointer overload) ───────────────────────────────────
CStyle* CMUSHclientDoc::AddStyle(const unsigned short iFlags,
                                  const COLORREF iForeColour,
                                  const COLORREF iBackColour,
                                  const int iLength,
                                  CAction* pAction,
                                  CLine* pLine)
{
    if (!pLine) pLine = m_pCurrentLine;
    if (!pLine) return nullptr;

    if (pAction) pAction->AddRef();

    if (!pLine->styleList.IsEmpty()) {
        CStyle* pOld = pLine->styleList.GetTail();
        if (pOld->iLength == 0 && (pOld->iFlags & START_TAG) == 0) {
            DELETESTYLE(pOld);
            pLine->styleList.RemoveTail();
        }
    }

    CStyle* p     = NEWSTYLE;
    p->iFlags      = iFlags;
    p->iForeColour = iForeColour;
    p->iBackColour = iBackColour;
    p->iLength     = (unsigned short)iLength;
    p->pAction     = pAction;
    pLine->styleList.AddTail(p);

    // Keep m_current_action/hint/fg/bg in sync so DisplayMsg and AddToLine
    // can populate SOutputRun correctly.
    m_current_style_fg = iForeColour;
    m_current_style_bg = iBackColour;
    if (pAction && (iFlags & (ACTION_HYPERLINK | ACTION_SEND))) {
        m_current_action = static_cast<const char*>(pAction->m_strAction);
        m_current_hint   = static_cast<const char*>(pAction->m_strHint);
    } else if (!(iFlags & (ACTION_HYPERLINK | ACTION_SEND))) {
        m_current_action.clear();
        m_current_hint.clear();
    }

    return p;
}

// ── AddStyle (7-arg string overload: action + hint + variable) ───────────
CStyle* CMUSHclientDoc::AddStyle(const unsigned short iFlags,
                                  const COLORREF iForeColour,
                                  const COLORREF iBackColour,
                                  const int iLength,
                                  CString, CString, CString,
                                  CLine* pLine)
{
    return AddStyle(iFlags, iForeColour, iBackColour, iLength,
                    static_cast<CAction*>(nullptr), pLine);
}

// ── CheckObjectName ───────────────────────────────────────────────────────
// Mirrors CMUSHclientDoc::CheckObjectName in doc.cpp:
//   1. Trim leading/trailing whitespace.
//   2. If bConform: validate with CheckLabel — return eInvalidObjectLabel on fail.
//   3. Lowercase the name (keys in the trigger/alias/timer maps are lowercase).
// Returns eOK on success.

// CheckLabel: first char must be alpha; remaining chars alpha/digit/underscore.
// Returns TRUE if the label is INVALID (matching doc.cpp convention).
static BOOL CheckLabel(const CString& strLabel)
{
    if (strLabel.IsEmpty())
        return TRUE;  // empty = invalid
    if (!isalpha((unsigned char)strLabel[0]))
        return TRUE;
    for (int i = 1; i < strLabel.GetLength(); i++) {
        char c = strLabel[i];
        if (!isalpha((unsigned char)c) && !isdigit((unsigned char)c) && c != '_')
            return TRUE;
    }
    return FALSE;  // valid
}

long CheckObjectName(CString& strName, const bool bConform)
{
    strName.TrimLeft();
    strName.TrimRight();

    if (bConform)
        if (CheckLabel(strName))
            return eInvalidObjectLabel;

    strName.MakeLower();
    return eOK;
}

// ── Plugin callback string constants ─────────────────────────────────────
const string ON_PLUGIN_TICK                  = "OnPluginTick";
const string ON_PLUGIN_DRAW_OUTPUT_WINDOW    = "OnPluginDrawOutputWindow";
const string ON_PLUGIN_IAC_GA                = "OnPlugin_IAC_GA";
const string ON_PLUGIN_TELNET_REQUEST        = "OnPluginTelnetRequest";
const string ON_PLUGIN_TELNET_OPTION         = "OnPluginTelnetOption";
const string ON_PLUGIN_TELNET_SUBNEGOTIATION = "OnPluginTelnetSubnegotiation";
const string ON_PLUGIN_MXP_START             = "OnPluginMXPstart";
const string ON_PLUGIN_MXP_STOP              = "OnPluginMXPstop";
const string ON_PLUGIN_MXP_OPENTAG           = "OnPluginMXPopenTag";
const string ON_PLUGIN_MXP_CLOSETAG          = "OnPluginMXPcloseTag";
const string ON_PLUGIN_MXP_SETVARIABLE       = "OnPluginMXPsetVariable";
const string ON_PLUGIN_MXP_SETENTITY         = "OnPluginMXPsetEntity";
const string ON_PLUGIN_MXP_ERROR             = "OnPluginMXPerror";
const string ON_PLUGIN_CONNECT               = "OnPluginConnect";
const string ON_PLUGIN_DISCONNECT            = "OnPluginDisconnect";
const string ON_PLUGIN_WORLD_SAVE            = "OnPluginWorldSave";
const string ON_PLUGIN_CLOSE                 = "OnPluginClose";
const string ON_PLUGIN_INSTALL               = "OnPluginInstall";

// ── MakeActionHash ────────────────────────────────────────────────────────
// Simple FNV-1a-like hash of the three action strings.
unsigned long MakeActionHash(const CString& a, const CString& b, const CString& c)
{
    unsigned long h = 2166136261UL;
    auto mix = [&](const char* p) {
        for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
    };
    mix((const char*)a);
    mix((const char*)b);
    mix((const char*)c);
    return h;
}

// ── FindBaseOption stub ───────────────────────────────────────────────────
// OptionsTable[] is empty (sentinel at index 0), so always returns -1.
long FindBaseOption(const char* OptionName,
                    tConfigurationNumericOption BaseOptionsTable[],
                    int& iItem)
{
    iItem = -1;
    for (int i = 0; BaseOptionsTable[i].pName; i++) {
        if (strcasecmp(BaseOptionsTable[i].pName, OptionName) == 0) {
            iItem = i;
            return 0; // eOK
        }
    }
    return -1; // not found
}

// ── Plugin callback dispatch ──────────────────────────────────────────────
//
// Walk m_PluginList and invoke `callbackName` on every enabled plugin that
// has a Lua engine and has that function defined.  Each overload matches one
// of the Windows CMUSHclientDoc::SendToAllPluginCallbacks signatures so the
// call sites in doc_portable.cpp (and the trigger/alias/timer evaluation
// paths) don't need to be touched.

// Overload 1 — fire callback, ignore return value.
void CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName)
{
    CPlugin* pSaved = m_CurrentPlugin;
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams;
        list<string> sparams;
        long nInvocations = 0;
        m_CurrentPlugin = p;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations);
        m_CurrentPlugin = pSaved;
    }
    m_CurrentPlugin = pSaved;
}

// Overload 2 — fire callback with one integer and one string param;
// returns bDefaultReturn unless a plugin returns a different bool.
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName,
                                               long nParam,
                                               const string& sParam,
                                               bool /*bStop*/,
                                               bool bDefaultReturn)
{
    CPlugin* pSaved = m_CurrentPlugin;
    bool result = bDefaultReturn;
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams = { (double)nParam };
        list<string> sparams = { sParam };
        bool pluginResult = bDefaultReturn;
        long nInvocations = 0;
        m_CurrentPlugin = p;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations,
                                       nullptr, nullptr, nullptr,
                                       &pluginResult);
        m_CurrentPlugin = pSaved;
        result = pluginResult;
    }
    m_CurrentPlugin = pSaved;
    return result;
}

// Overload 3 — fire callback with one CString param; returns true if
// no plugin intercepted (all returned true / didn't intercept).
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName,
                                               const CString& sParam)
{
    CPlugin* pSaved = m_CurrentPlugin;
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams;
        list<string> sparams = { string((const char*)sParam) };
        bool intercepted = false;
        long nInvocations = 0;
        m_CurrentPlugin = p;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations,
                                       nullptr, nullptr, nullptr,
                                       &intercepted);
        m_CurrentPlugin = pSaved;
        if (intercepted) return false;  // plugin intercepted — stop
    }
    m_CurrentPlugin = pSaved;
    return true;  // no interception
}

// Overload 4 — fire callback with one CString param; returns true if
// any plugin handled the event.
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName,
                                               const CString& sParam,
                                               bool /*bStop*/)
{
    CPlugin* pSaved = m_CurrentPlugin;
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams;
        list<string> sparams = { string((const char*)sParam) };
        bool handled = false;
        long nInvocations = 0;
        m_CurrentPlugin = p;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations,
                                       nullptr, nullptr, nullptr,
                                       &handled);
        m_CurrentPlugin = pSaved;
        if (handled) return true;
    }
    m_CurrentPlugin = pSaved;
    return false;
}

// Rtn variant — string in/out: each plugin may transform strResult.
// Used by OnPluginCommandEntered: plugins can modify, discard ("\t"), or
// silently ignore ("\r") the command the user typed.
// Mirrors plugins.cpp CMUSHclientDoc::SendToAllPluginCallbacksRtn.
void CMUSHclientDoc::SendToAllPluginCallbacksRtn(const string& callbackName,
                                                  CString& strResult)
{
    CPlugin* pSavedPlugin = m_CurrentPlugin;
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        long nInvocations = 0;
        m_CurrentPlugin = p;
        p->m_ScriptEngine->ExecuteLua(dispid, (LPCTSTR)strName,
                                       eDontChangeAction,
                                       "plugin", callbackName.c_str(),
                                       strResult,
                                       nInvocations,
                                       strResult);
        m_CurrentPlugin = pSavedPlugin;
        // "\t" = discard command, "\r" = ignore silently — stop on either
        if (strResult == "\t" || strResult == "\r") break;
    }
    m_CurrentPlugin = pSavedPlugin;
}

// SendPacket: single-sourced in doc_shared.cpp (T3b) — the old portable
// twin wrote SSL bytes with no WANT_* retry; the shared transport path buffers.


// ── Debug_Packets — no-op off the full build ────────────────────────────────
// The real implementation (doc.cpp) pretty-prints packets into a notepad
// window via SendToFirstPluginCallbacks/AppendToTheNotepad — GUI tooling the
// headless engine does not link.  The shared SendPacket/OnBytesReceived call
// it only when m_bDebugIncomingPackets is set.
void CMUSHclientDoc::Debug_Packets(LPCTSTR, LPCTSTR, int, const __int64)
{
}

// ── AddToLine ────────────────────────────────────────────────────────────────
// Called by Lua Hyperlink() to inject a styled run into the output stream.
// On Windows, this appends to the current line being assembled by DisplayMsg.
// On non-Windows, there is no global "current line"; we flush text immediately.
//
// GUI path: build a single SOutputRun with the colour/action from the most
//   recent AddStyle() call and deliver it as a complete line.
// CLI path: print the text to stdout (styled output) followed by a newline.
void CMUSHclientDoc::AddToLine(const char* text, const int /*flags*/)
{
    if (!text || !*text) return;

    if (HaveOutputSink()) {
        // GUI path: emit as a one-run styled line so the output pane can
        // render the link with the correct colour and click action.
        SOutputRun run;
        run.text      = text;
        run.fg        = m_current_style_fg;
        run.bg        = m_current_style_bg;
        run.underline = !m_current_action.empty();  // underline links by default
        run.action    = m_current_action;
        run.hint      = m_current_hint;
        SOutputLine line;
        line.push_back(std::move(run));
        EmitOutputLine(std::move(line));
    } else {
        // CLI path: plain text with newline.
        puts(text);
        fflush(stdout);
    }
}

// SendWindowSizes — send IAC SB NAWS w_hi w_lo h_hi h_lo IAC SE to the MUD.
// iNewWidth is the new column count (already stored in m_nWrapColumn by caller).
// We read the actual terminal size from the OS; fall back to iNewWidth x 24.
void CMUSHclientDoc::SendWindowSizes(const int iNewWidth)
{
    if (!ConnectionOpen()) return;

    unsigned short cols = (unsigned short)iNewWidth;
    unsigned short rows = 24;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row > 0 ? ws.ws_row : 24;
    }

    dbg("NAWS: sending %dx%d", (int)cols, (int)rows);

    unsigned char pkt[9];
    pkt[0] = (unsigned char)IAC;
    pkt[1] = (unsigned char)SB;
    pkt[2] = (unsigned char)TELOPT_NAWS;
    pkt[3] = HIBYTE(cols);
    pkt[4] = LOBYTE(cols);
    pkt[5] = HIBYTE(rows);
    pkt[6] = LOBYTE(rows);
    pkt[7] = (unsigned char)IAC;
    pkt[8] = (unsigned char)SE;
    SendPacket(pkt, sizeof(pkt));
}

bool CMUSHclientDoc::InitZlib(z_stream& zs)
{
    zs.zalloc = Z_NULL;
    zs.zfree  = Z_NULL;
    zs.opaque = Z_NULL;
    zs.avail_in  = 0;
    zs.next_in   = Z_NULL;
    return (inflateInit(&zs) == Z_OK);
}

// OnConnectionDisconnect: single-sourced in doc_shared.cpp (T3c).

// ── FireWorldClose — fire the on_world_close Lua callback ────────────────────
// Mirrors the on_world_disconnect path in OnConnectionDisconnect().
// Fired on world/document teardown so that scripts can do any final cleanup
// (e.g. saving state, logging).
void CMUSHclientDoc::FireWorldClose()
{
    if (m_dispidWorldClose == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldClose,
                       m_strWorldClose,
                       eWorldAction,
                       "world", "close",
                       nparams, sparams,
                       nInvocations);
}

// ── FireWorldSave — fire the on_world_save Lua callback ──────────────────────
// Called from OnSaveDocument (doc_stub.h) before the file is written.
// Mirrors the ExecuteScript(m_dispidWorldSave,...) call in doc.cpp DoSave().
void CMUSHclientDoc::FireWorldSave()
{
    if (m_dispidWorldSave == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldSave,
                       m_strWorldSave,
                       eWorldAction,
                       "world", "save",
                       nparams, sparams,
                       nInvocations);
}

// ── PluginListChanged — fire OnPluginListChanged with re-entrancy guard ──────
// Mirrors CMUSHclientDoc::PluginListChanged() in plugins.cpp.
// Called after InternalLoadPlugin / L_UnloadPlugin so every loaded plugin
// is notified that the plugin roster has changed.

// ── FireWorldGetFocus / FireWorldLoseFocus ────────────────────────────────────
// Fire the on_world_get_focus / on_world_lose_focus Lua callbacks.
// Called from wx_world_child.cpp OnActivate() — mirrors mushview.cpp OnSetFocus /
// OnKillFocus which call ExecuteScript(m_dispidWorldGetFocus/LoseFocus, ...) and
// then SendToAllPluginCallbacks(ON_PLUGIN_GETFOCUS/LOSEFOCUS).
// Note: plugin callbacks are fired in wx_world_child.cpp (where m_bWorldClosing is
// accessible) rather than here.
void CMUSHclientDoc::FireWorldGetFocus()
{
    if (m_dispidWorldGetFocus == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldGetFocus,
                       m_strWorldGetFocus,
                       eWorldAction,
                       "world", "get focus",
                       nparams, sparams,
                       nInvocations);
}

void CMUSHclientDoc::FireWorldLoseFocus()
{
    if (m_dispidWorldLoseFocus == DISPID_UNKNOWN) return;
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua()) return;
    list<double> nparams;
    list<string> sparams;
    long nInvocations = 0;
    engine->ExecuteLua(m_dispidWorldLoseFocus,
                       m_strWorldLoseFocus,
                       eWorldAction,
                       "world", "lose focus",
                       nparams, sparams,
                       nInvocations);
}

// ── TermSyncStyle / TermResetStyle — emit ANSI escapes to the real terminal ───
//
// The ANSI/256/24-bit colour state machine in ansi.cpp updates m_iFlags,
// m_iForeColour, m_iBackColour via RememberStyle() after each ANSI sequence.
// TermSyncStyle() converts that state to terminal escape codes using
// GetStyleRGB() and emits them only when the style has actually changed.
// TermResetStyle() emits SGR 0 (reset) and clears the tracking state.

// Map an RGB COLORREF to the nearest index in the xterm 256-colour palette.
// Uses minimum squared Euclidean distance in RGB space.
// Called only when the style changes (not per-character), so cost is trivial.
static int colorref_to_xterm256(COLORREF c)
{
    int r = (int)GetRValue(c);
    int g = (int)GetGValue(c);
    int b = (int)GetBValue(c);
    int best_idx  = 0;
    int best_dist = INT_MAX;
    for (int i = 0; i < 256; i++) {
        int dr = r - (int)GetRValue(xterm_256_colours[i]);
        int dg = g - (int)GetGValue(xterm_256_colours[i]);
        int db = b - (int)GetBValue(xterm_256_colours[i]);
        int dist = dr*dr + dg*dg + db*db;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx  = i;
            if (dist == 0) break;  // exact match — stop early
        }
    }
    return best_idx;
}

void CMUSHclientDoc::TermSyncStyle()
{
    // Build a temporary CStyle from the remembered state and resolve to RGB.
    CStyle tmp;
    tmp.iFlags      = m_iFlags;
    tmp.iForeColour = m_iForeColour;
    tmp.iBackColour = m_iBackColour;

    COLORREF fg, bg;
    GetStyleRGB(&tmp, fg, bg);

    bool bold      = (m_iFlags & HILITE)    != 0;
    bool underline = (m_iFlags & UNDERLINE) != 0;

    if (fg        == m_term_last_fg        &&
        bg        == m_term_last_bg        &&
        bold      == m_term_last_bold      &&
        underline == m_term_last_underline)
        return;  // nothing changed

    // Map to the nearest xterm 256-colour palette entries.
    int fg256 = colorref_to_xterm256(fg);
    int bg256 = colorref_to_xterm256(bg);

    // Emit a single combined SGR sequence: reset + attributes + fg + bg.
    printf("\033[0");
    if (bold)      printf(";1");
    if (underline) printf(";4");
    printf(";38;5;%d;48;5;%dm", fg256, bg256);

    m_term_last_fg        = fg;
    m_term_last_bg        = bg;
    m_term_last_bold      = bold;
    m_term_last_underline = underline;
}

void CMUSHclientDoc::TermResetStyle()
{
    // Emit SGR 0 (reset all attributes) and invalidate tracking so the next
    // TermSyncStyle() call re-emits even if the style didn't change.
    printf("\033[0m");
    m_term_last_fg        = (COLORREF)-1;
    m_term_last_bg        = (COLORREF)-1;
    m_term_last_bold      = false;
    m_term_last_underline = false;
}

// ── DisplayMsg — portable telnet/ANSI/MXP parser driving stdout ──────────────
//
// Mirrors the per-character dispatch loop from doc.cpp DisplayMsg.
// GUI-rendering concerns (line buffers, style lists, triggers) are omitted;
// visible characters are written straight to stdout.

void CMUSHclientDoc::DisplayMsg(const char* lpszText, int size,
                                 const int flags, const bool /*fake*/)
{
    if (size <= 0 || !lpszText) return;
    if (g_before_output) g_before_output();

    for (const char* p = lpszText; size > 0; ++p, --size) {
        unsigned char c = (unsigned char)*p;

        // ── Phase reset on line-terminator / ESC / IAC ────────────────────
        // ESC and IAC can interrupt any in-progress state (e.g. a partial
        // MXP tag).  Clean up the old state first, then transition:
        //   ESC → HAVE_ESC    (start of an ANSI sequence)
        //   IAC → HAVE_IAC    (start of a telnet command)
        //   \n/\r → NONE      (fall through to output)
        if (!(flags & NOTE_OR_COMMAND) &&
            m_phase != HAVE_SB &&
            m_phase != HAVE_SUBNEGOTIATION &&
            m_phase != HAVE_SUBNEGOTIATION_IAC &&
            !(m_phase == HAVE_IAC && c == IAC))
        {
            if (c == 0x1B || (c == 0xFF && m_phase != HAVE_IAC) ||
                c == '\n' || c == '\r')
            {
                // clean up any interrupted MXP state
                switch (m_phase) {
                case HAVE_MXP_ELEMENT:
                case HAVE_MXP_COMMENT:
                case HAVE_MXP_QUOTE:
                case HAVE_MXP_ENTITY:
                    MXP_unterminated_element("got <NEWLINE/ESC/IAC>");
                    break;
                case HAVE_MXP_ROOM_NAME:
                case HAVE_MXP_ROOM_DESCRIPTION:
                case HAVE_MXP_ROOM_EXITS:
                case HAVE_MXP_WELCOME:
                    MXP_mode_change(-1);
                    break;
                default:
                    break;
                }
                // transition to the appropriate new phase
                if (c == 0x1B) {
                    m_phase = HAVE_ESC;
                    continue;   // ESC byte itself is not output
                } else if (c == 0xFF) {
                    m_phase = HAVE_IAC;
                    continue;   // IAC byte itself is not output
                } else {
                    m_phase = NONE; // \n or \r — fall through to output
                }
            }
        }

        // ── Per-phase dispatch (MUD data only) ────────────────────────────
        if (!(flags & NOTE_OR_COMMAND)) {
            switch (m_phase) {
            case HAVE_ESC:            Phase_ESC(c);  continue;
            case HAVE_OSC:            Phase_OSC(c);  continue;
            case HAVE_UTF8_CHARACTER: Phase_UTF8(c); continue;

            case HAVE_FOREGROUND_256_START:   case HAVE_FOREGROUND_256_FINISH:
            case HAVE_BACKGROUND_256_START:   case HAVE_BACKGROUND_256_FINISH:
            case HAVE_FOREGROUND_24B_FINISH:  case HAVE_FOREGROUND_24BR_FINISH:
            case HAVE_FOREGROUND_24BG_FINISH: case HAVE_FOREGROUND_24BB_FINISH:
            case HAVE_BACKGROUND_24B_FINISH:  case HAVE_BACKGROUND_24BR_FINISH:
            case HAVE_BACKGROUND_24BG_FINISH: case HAVE_BACKGROUND_24BB_FINISH:
            case DOING_CODE:
                Phase_ANSI(c); continue;

            case HAVE_IAC:
                if (c != IAC)
                    Phase_IAC(c);   // modifies c: GA->\n or 0, others->0
                if (c)
                    break;          // c == '\n' or IAC itself — fall through to output
                else
                    continue;       // c == 0 — consumed, skip

            case HAVE_WILL:      Phase_WILL(c); continue;
            case HAVE_WONT:      Phase_WONT(c); continue;
            case HAVE_DO:        Phase_DO(c);   continue;
            case HAVE_DONT:      Phase_DONT(c); continue;
            case HAVE_SB:        Phase_SB(c);   continue;
            case HAVE_SUBNEGOTIATION:     Phase_SUBNEGOTIATION(c); continue;
            case HAVE_SUBNEGOTIATION_IAC: {
                bool bWasCompressing = m_bCompress;
                Phase_SUBNEGOTIATION_IAC(c);
                if (!bWasCompressing && m_bCompress) {
                    dbg("MCCP compression ACTIVE (stream now zlib-compressed)");
                    ++p; --size;  // skip SE byte consumed by transition
                    if (size > 0) memmove(m_CompressInput, p, size);
                    m_zCompress.next_in   = m_CompressInput;
                    m_zCompress.avail_in  = size;
                    m_zCompress.next_out  = m_CompressOutput;
                    m_zCompress.avail_out = (uInt)m_nCompressionOutputBufferSize;
                    return;
                }
                continue;
            }
            case HAVE_COMPRESS:       Phase_COMPRESS(c); continue;
            case HAVE_COMPRESS_WILL: {
                bool bWasCompressing = m_bCompress;
                Phase_COMPRESS_WILL(c);
                if (!bWasCompressing && m_bCompress) {
                    dbg("MCCP1 compression ACTIVE (stream now zlib-compressed)");
                    ++p; --size;
                    if (size > 0) memmove(m_CompressInput, p, size);
                    m_zCompress.next_in   = m_CompressInput;
                    m_zCompress.avail_in  = size;
                    m_zCompress.next_out  = m_CompressOutput;
                    m_zCompress.avail_out = (uInt)m_nCompressionOutputBufferSize;
                    return;
                }
                continue;
            }
            case HAVE_MXP_ELEMENT:  Phase_MXP_ELEMENT(c); continue;
            case HAVE_MXP_COMMENT:  Phase_MXP_COMMENT(c); continue;
            case HAVE_MXP_QUOTE:    Phase_MXP_QUOTE(c);   continue;
            case HAVE_MXP_ENTITY:   Phase_MXP_ENTITY(c);  continue;
            case HAVE_MXP_ROOM_NAME:
            case HAVE_MXP_ROOM_DESCRIPTION:
            case HAVE_MXP_ROOM_EXITS:
            case HAVE_MXP_WELCOME:
                Phase_MXP_COLLECT_SPECIAL(c); continue;
            default: break;  // NONE — fall through to output
            }

            // ── MXP guard: discard characters inside server-side scripts ──
            if (m_bMXP) {
                if (m_bMXP_script && !MXP_Secure())
                    continue;
                if (c != '<') {
                    if (m_iMXP_mode == eMXP_secure_once)
                        MXP_Restore_Mode();
                    if (m_bMXP_script)
                        continue;
                }
            }
        } // end of MUD-data-only block

        // ── Phase NONE: output visible characters ─────────────────────────
        if (c == '\n') {
            if (HaveOutputSink()) {
                // GUI path: deliver the assembled styled line and clear buffer.
                EmitOutputLine(std::move(m_gui_line_buf));
                m_gui_line_buf.clear();
            } else {
                // CLI path: reset colours at line end (avoids bg bleeding).
                TermResetStyle();
                putchar('\n');
            }
            // Finalise the current CLine, fire triggers, start fresh line.
            StartNewLine(/*hard_break=*/true, flags);
        } else if (c == '\r') {
            /* suppress CR in CR-LF sequences */
        } else if (c >= 0x20 || (c >= 0x80)) {
            // UTF-8 passthrough: handle all bytes ≥ 0x20 or continuation bytes.

            // ── Accumulate into m_pCurrentLine for trigger matching ────────
            // iLength and len are byte counts (not Unicode character counts).
            if (m_pCurrentLine) {
                if (m_pCurrentLine->len >= m_pCurrentLine->iMemoryAllocated) {
                    int newSize = (m_pCurrentLine->iMemoryAllocated < 64)
                                  ? 128
                                  : m_pCurrentLine->iMemoryAllocated * 2;
                    // new[]/delete[] (NOT realloc) so the buffer matches how
                    // CLine::~CLine frees it (delete[], since USE_REALLOC is off).
                    char* nt = new char[newSize];
                    if (m_pCurrentLine->text) {
                        memcpy(nt, m_pCurrentLine->text, m_pCurrentLine->len);
                        delete[] m_pCurrentLine->text;
                    }
                    m_pCurrentLine->text = nt;
                    m_pCurrentLine->iMemoryAllocated = newSize;
                }
                m_pCurrentLine->text[m_pCurrentLine->len++] = (char)c;
                if (!m_pCurrentLine->styleList.IsEmpty())
                    m_pCurrentLine->styleList.GetTail()->iLength++;
            }

            if (HaveOutputSink()) {
                // GUI path: accumulate into styled run buffer.
                // On lead bytes (not 0x80–0xBF), resolve current style.
                if ((c & 0xC0) != 0x80) {
                    CStyle tmp;
                    tmp.iFlags      = m_iFlags;
                    tmp.iForeColour = m_iForeColour;
                    tmp.iBackColour = m_iBackColour;
                    COLORREF fg, bg;
                    GetStyleRGB(&tmp, fg, bg);
                    bool bold          = (m_iFlags & HILITE)    != 0;
                    bool underline     = (m_iFlags & UNDERLINE) != 0;
                    bool italic        = (m_iFlags & BLINK)     != 0;  // MUSHclient maps BLINK → italic
                    bool strikethrough = (m_iFlags & STRIKEOUT) != 0;
                    const std::string& cur_action = m_current_action;
                    const std::string& cur_hint   = m_current_hint;
                    // Append to current run if style unchanged, else start new run.
                    if (m_gui_line_buf.empty()                               ||
                        m_gui_line_buf.back().fg            != fg            ||
                        m_gui_line_buf.back().bg            != bg            ||
                        m_gui_line_buf.back().bold          != bold          ||
                        m_gui_line_buf.back().underline     != underline     ||
                        m_gui_line_buf.back().italic        != italic        ||
                        m_gui_line_buf.back().strikethrough != strikethrough ||
                        m_gui_line_buf.back().action        != cur_action    ||
                        m_gui_line_buf.back().hint          != cur_hint)
                    {
                        SOutputRun run;
                        run.text          = std::string(1, (char)c);
                        run.fg            = fg;
                        run.bg            = bg;
                        run.bold          = bold;
                        run.underline     = underline;
                        run.italic        = italic;
                        run.strikethrough = strikethrough;
                        run.action        = cur_action;
                        run.hint          = cur_hint;
                        m_gui_line_buf.push_back(std::move(run));
                    } else {
                        m_gui_line_buf.back().text += (char)c;
                    }
                } else {
                    // Continuation byte: append to last run (same style).
                    if (!m_gui_line_buf.empty())
                        m_gui_line_buf.back().text += (char)c;
                    else
                        m_gui_line_buf.push_back(SOutputRun{std::string(1, (char)c)});
                }
            } else {
                // CLI path: emit ANSI escapes to the real terminal.
                // Only call TermSyncStyle() on lead bytes, not continuation bytes.
                if ((c & 0xC0) != 0x80)
                    TermSyncStyle();
                putchar((int)c);
            }
        }

    } // end of per-character loop

    if (!HaveOutputSink()) {
        fflush(stdout);
        if (g_after_output) g_after_output();
    }
}

// ── StartNewLine — finalise current line, fire triggers, begin a new one ─────
//
// This is the portable equivalent of doc.cpp's StartNewLine(bool, int).
// Called from DisplayMsg on every \n.
//
// Flow:
//   1. Timestamp and move m_pCurrentLine into m_LineList (transfers ownership).
//   2. For real MUD lines (not notes/commands), call ProcessPreviousLine() so
//      triggers, plugins, and logging all see the new line.
//   3. Trim m_LineList to m_maxlines by deleting from the head.
//   4. Allocate a fresh m_pCurrentLine and seed it with the current ANSI style
//      so the next line inherits the running colour state.
void CMUSHclientDoc::StartNewLine(const bool hard_break, const int flags)
{
    if (!m_pCurrentLine) return;

    // Re-entrancy guard: ProcessPreviousLine() below fires triggers, and a trigger
    // that world.Send/<send>s echoes back through DisplayMsg → StartNewLine while
    // m_pCurrentLine still points at the line we just AddTail'd. Bail so we don't
    // add (and later double-free) that same CLine twice. The echo's text still
    // streams to the display via m_gui_line_buf, which is handled separately.
    if (m_bInStartNewLine) return;
    m_bInStartNewLine = true;

    // Timestamp the finished line
    m_pCurrentLine->m_theTime = CTime::GetCurrentTime();
    m_pCurrentLine->m_lineHighPerformanceTime.QuadPart = 0;
    m_pCurrentLine->flags = (unsigned char)flags;

    // Transfer to line buffer
    m_LineList.AddTail(m_pCurrentLine);
    if (!(flags & NOTE_OR_COMMAND))
        ++m_nTotalLinesReceived;

    // Fire triggers on real MUD data lines (not notes, not echoed commands)
    if (hard_break && !(flags & NOTE_OR_COMMAND))
        ProcessPreviousLine();

    // Trim line buffer to configured maximum
    while ((long)m_LineList.GetCount() > m_maxlines) {
        CLine* pOld = m_LineList.RemoveHead();
        delete pOld;
    }

    // Fresh line — inherits current ANSI colour/style state
    m_pCurrentLine = new CLine();
    m_pScratchLine = m_pCurrentLine;   // keep alias in sync
    AddStyle(m_iFlags, m_iForeColour, m_iBackColour, 0,
             static_cast<CAction*>(nullptr));

    m_bInStartNewLine = false;
}

// ReceiveMsg: gone — bytes arrive via the transport bridge
// (CDocTransportEvents::onBytes -> OnBytesReceived, doc_shared.cpp; T3c/T5).
// OnBytesReceived: single-sourced in doc_shared.cpp (T3c).
// OnConnect: single-sourced in doc_shared.cpp (T3b).

// ConnectionEstablished: single-sourced in doc_shared.cpp (T3d).

// ── SOCKS proxy helpers ───────────────────────────────────────────────────────







// ── SSL implementation ────────────────────────────────────────────────────────


// StartSSLHandshake: single-sourced in doc_shared.cpp (T3c); the TLS
// session itself lives inside the transport adapter (no doc-level
// handshake stepping off-Windows — ContinueSSLHandshake is gone).

// ── CloseLog ─────────────────────────────────────────────────────────────────
// Writes the log-file postamble (if any) and closes m_logfile.
// Mirrors the implementation in scripting/methods/methods_logging.cpp.

// ── Note / ColourNote / ColourTell ───────────────────────────────────────────

// ── GUI helper: flush m_gui_note_buf via the IWorldView seam ──────────────
// Called before any Note() or ColourNote() emits its line so that pending
// ColourTell segments land in the output pane.
static void flush_gui_note_buf(CMUSHclientDoc* doc)
{
    if (!doc->HaveOutputSink()) return;
    if (doc->m_gui_note_buf.empty()) return;
    doc->EmitOutputLine(std::move(doc->m_gui_note_buf));
    doc->m_gui_note_buf.clear();
}

// ── GUI helper: resolve a colour-name or #RRGGBB string to COLORREF ──────
// Returns defColour when name is null/empty or unrecognised.
static COLORREF resolve_colour(const char* name, COLORREF defColour)
{
    if (!name || !*name) return defColour;
    COLORREF c = defColour;
    SetColour(CString(name), c);
    return c;
}

// ── CLI helper: map a colour name to an ANSI SGR escape string ────────────
// Returns "" for empty / unknown (lets the terminal keep its default).
static const char* colour_to_ansi(const char* name, bool bg)
{
    if (!name || !*name) return "";
    struct { const char* name; int fg; int bg; } table[] = {
        { "black",       30,  40 },
        { "darkblue",    34,  44 },
        { "darkgreen",   32,  42 },
        { "darkcyan",    36,  46 },
        { "darkred",     31,  41 },
        { "darkmagenta", 35,  45 },
        { "darkyellow",  33,  43 },
        { "brown",       33,  43 },
        { "gray",        37,  47 },
        { "grey",        37,  47 },
        { "darkgray",    90, 100 },
        { "darkgrey",    90, 100 },
        { "blue",        94, 104 },
        { "green",       92, 102 },
        { "cyan",        96, 106 },
        { "red",         91, 101 },
        { "magenta",     95, 105 },
        { "yellow",      93, 103 },
        { "white",       97, 107 },
        { nullptr, 0, 0 }
    };
    CString lc(name);
    lc.MakeLower();
    for (int i = 0; table[i].name; ++i) {
        if (lc == table[i].name) {
            static char buf[16];
            snprintf(buf, sizeof(buf), "\033[%dm", bg ? table[i].bg : table[i].fg);
            return buf;
        }
    }
    return "";
}

static void colour_print(const char* fg, const char* bg, const char* text, bool newline)
{
    const char* fc = colour_to_ansi(fg, false);
    const char* bc = colour_to_ansi(bg, true);
    bool coloured = (*fc || *bc);
    if (coloured) fprintf(stdout, "%s%s", fc, bc);
    fputs(text ? text : "", stdout);
    if (coloured) fputs("\033[0m", stdout);
    if (newline)  fputc('\n', stdout);
    fflush(stdout);
}

// ── Note ─────────────────────────────────────────────────────────────────────
void CMUSHclientDoc::Note(const CString& s)
{
    if (m_pWorldView) {
        // Flush any in-progress ColourTell segments as a separate line first.
        flush_gui_note_buf(this);
        EmitNote(std::string((const char*)s));
        return;
    }
    if (g_before_output) g_before_output();
    puts((const char*)s);
    fflush(stdout);
    if (g_after_output) g_after_output();
}

void CMUSHclientDoc::Note(const char* s)
{
    if (m_pWorldView) {
        flush_gui_note_buf(this);
        EmitNote(s ? std::string(s) : std::string());
        return;
    }
    if (g_before_output) g_before_output();
    puts(s ? s : "");
    fflush(stdout);
    if (g_after_output) g_after_output();
}

// ── ColourNote / ColourTell ───────────────────────────────────────────────────

// ── GUI implementation: append a run to the note line buffer ─────────────
static void gui_colour_segment(CMUSHclientDoc* doc,
                               const char* fg, const char* bg,
                               const char* text, bool flush)
{
    if (!doc->HaveOutputSink()) return;
    if (text && *text) {
        SOutputRun run;
        run.text = text;
        run.fg   = resolve_colour(fg, doc->m_NoteColourFore);
        run.bg   = resolve_colour(bg, doc->m_NoteColourBack);
        doc->m_gui_note_buf.push_back(std::move(run));
    }
    if (flush) {
        // Emit even if empty (produces a blank line, matching Note() behaviour).
        doc->EmitOutputLine(std::move(doc->m_gui_note_buf));
        doc->m_gui_note_buf.clear();
    }
}

void CMUSHclientDoc::ColourNote(const char* fg, const char* bg, const CString& text)
{
    ColourNote(fg, bg, static_cast<const char*>(text));
}

void CMUSHclientDoc::ColourNote(const char* fg, const char* bg, const char* text)
{
    if (HaveOutputSink()) {
        gui_colour_segment(this, fg, bg, text, /*flush=*/true);
    } else {
        colour_print(fg, bg, text, /*newline=*/true);
    }
}

void CMUSHclientDoc::ColourTell(const char* fg, const char* bg, const char* text)
{
    if (HaveOutputSink()) {
        gui_colour_segment(this, fg, bg, text, /*flush=*/false);
    } else {
        colour_print(fg, bg, text, /*newline=*/false);
    }
}

// ── UpdateAllViews ────────────────────────────────────────────────────────────
void CMUSHclientDoc::UpdateAllViews(void*) {}

bool CMUSHclientDoc::AppendToTheNotepad(const CString strTitle,
                                         const CString strText,
                                         const bool bReplace,
                                         const int  iNotepadType)
{
    // Try to find and update an existing notepad by title.
    if (g_wxhook_append_to_notepad)
    {
        const char* title = static_cast<const char*>(strTitle);
        const char* text  = static_cast<const char*>(strText);
        if (g_wxhook_append_to_notepad(title, text, bReplace))
            return true;
    }

    // No existing notepad — create a new one with the given text.
    return CreateTextWindow(
        static_cast<const char*>(strText),
        static_cast<const char*>(strTitle),
        this,
        0,                              // iUniqueDocNum (not tracked portably)
        App.m_strLuaScript,             // use default font name (fallback)
        10,                             // default font size
        400,                            // FW_NORMAL
        0,                              // charset
        RGB(0,   0,   0),               // black text
        RGB(255, 255, 255),             // white background
        CString(""),                    // no search string
        CString(""),                    // no line preamble
        false, false, false, false, false,
        0,
        iNotepadType);
}

bool CMUSHclientDoc::SeeIfHandlerCanExecute(const CString&)
{ return false; }

// ── CMUSHclientDoc::ResolveAllEntryPoints ────────────────────────────────
// Portable equivalent of CMUSHclientDoc::FindAllEntryPoints() (scripting.cpp).
//
// After the Lua engine is initialised and the script parsed, every trigger,
// alias, and timer that has a strProcedure name needs its dispid resolved so
// that ExecuteLua() will actually call the function (it skips calls when
// dispid == DISPID_UNKNOWN).
//
// When m_CurrentPlugin is set, the function resolves the plugin's own
// triggers/aliases/timers (via GetTriggerMap() etc.) using the plugin's Lua
// engine (via GetScriptEngine()).  World-level callbacks are only resolved
// when m_CurrentPlugin is null.  This mirrors the split between
// FindAllEntryPoints() and FindGlobalEntryPoints() in scripting.cpp.
void CMUSHclientDoc::ResolveAllEntryPoints()
{
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua())
        return;

    // Helper: look up a function name and return its dispid (or DISPID_UNKNOWN).
    auto resolve = [&](const CString& name) -> DISPID {
        if (name.IsEmpty()) return DISPID_UNKNOWN;
        return engine->GetLuaDispid(name);
    };

    // ── World-level and MXP callbacks — only for the doc's own script ─────
    // Mirrors FindGlobalEntryPoints() which is only called from
    // FindAllEntryPoints() when m_CurrentPlugin == nullptr.
    if (!m_CurrentPlugin) {
        m_dispidWorldOpen       = resolve(m_strWorldOpen);
        m_dispidWorldClose      = resolve(m_strWorldClose);
        m_dispidWorldSave       = resolve(m_strWorldSave);
        m_dispidWorldConnect    = resolve(m_strWorldConnect);
        m_dispidWorldDisconnect = resolve(m_strWorldDisconnect);
        m_dispidWorldGetFocus   = resolve(m_strWorldGetFocus);
        m_dispidWorldLoseFocus  = resolve(m_strWorldLoseFocus);

        m_dispidOnMXP_Start       = resolve(m_strOnMXP_Start);
        m_dispidOnMXP_Stop        = resolve(m_strOnMXP_Stop);
        m_dispidOnMXP_OpenTag     = resolve(m_strOnMXP_OpenTag);
        m_dispidOnMXP_CloseTag    = resolve(m_strOnMXP_CloseTag);
        m_dispidOnMXP_SetVariable = resolve(m_strOnMXP_SetVariable);
        m_dispidOnMXP_Error       = resolve(m_strOnMXP_Error);
    }

    // ── Triggers ──────────────────────────────────────────────────────────
    // Use GetTriggerMap() not m_TriggerMap — when m_CurrentPlugin is set this
    // returns the plugin's own map, matching FindAllEntryPoints() in scripting.cpp.
    {
        CTrigger* pTrigger;
        CString   strName;
        CTriggerMap& tmap = GetTriggerMap();
        for (POSITION pos = tmap.GetStartPosition(); pos; )
        {
            tmap.GetNextAssoc(pos, strName, pTrigger);
            pTrigger->dispid = resolve(pTrigger->strProcedure);
        }
    }

    // ── Aliases ───────────────────────────────────────────────────────────
    {
        CAlias* pAlias;
        CString strName;
        CAliasMap& amap = GetAliasMap();
        for (POSITION pos = amap.GetStartPosition(); pos; )
        {
            amap.GetNextAssoc(pos, strName, pAlias);
            pAlias->dispid = resolve(pAlias->strProcedure);
        }
    }

    // ── Timers ────────────────────────────────────────────────────────────
    {
        CTimer* pTimer;
        CString strName;
        CTimerMap& timap = GetTimerMap();
        for (POSITION pos = timap.GetStartPosition(); pos; )
        {
            timap.GetNextAssoc(pos, strName, pTimer);
            pTimer->dispid = resolve(pTimer->strProcedure);
        }
    }
}

CScriptEngine* CMUSHclientDoc::GetScriptEngine()
{
    // On non-Windows, plugins have their own script engines (m_ScriptEngine).
    // If we're currently executing inside a plugin, return that engine.
    if (m_CurrentPlugin)
        return m_CurrentPlugin->m_ScriptEngine;
    return m_ScriptEngine;
}

bool CMUSHclientDoc::ExecuteScript(DISPID&, const char*, const unsigned short,
                                    const char*, const char*,
                                    DISPPARAMS&, long&, COleVariant*)
{ return false; }

// MXP_StartTagScript is defined in mxpStart.cpp (real implementation).

// ── TFormat ──────────────────────────────────────────────────────────────
CString TFormat(const char* sFormat, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, sFormat);
    vsnprintf(buf, sizeof buf, sFormat, ap);
    va_end(ap);
    return CString(buf);
}

// ── TranslateGeneric ──────────────────────────────────────────────────────
CString TranslateGeneric(const char* sText, const char*)
{
    return CString(sText);
}

// UMessageBox is provided as an inline in stdafx.h's non-Windows branch.

// ── StringToVector ────────────────────────────────────────────────────────
void StringToVector(const string s, vector<string>& v,
                    const string delim, bool include_delim)
{
    v.clear();
    if (delim.empty()) { v.push_back(s); return; }
    size_t start = 0, pos;
    while ((pos = s.find(delim, start)) != string::npos) {
        string token = s.substr(start, pos - start);
        if (include_delim) token += delim;
        v.push_back(token);
        start = pos + delim.size();
    }
    if (start <= s.size()) v.push_back(s.substr(start));
}

// ── CVariable IMPLEMENT_DYNAMIC (needed by DECLARE_DYNAMIC) ──────────────
IMPLEMENT_DYNAMIC(CVariable, CObject)

// ── Additional plugin callback string constants ───────────────────────────
const string ON_PLUGIN_LINE_RECEIVED   = "OnPluginLineReceived";
const string ON_PLUGIN_SEND            = "OnPluginSend";
const string ON_PLUGIN_SENT            = "OnPluginSent";
const string ON_PLUGIN_COMMAND_ENTERED = "OnPluginCommandEntered";
const string ON_PLUGIN_COMMAND_CHANGED = "OnPluginCommandChanged";
const string ON_PLUGIN_ENABLE          = "OnPluginEnable";
const string ON_PLUGIN_DISABLE         = "OnPluginDisable";
const string ON_PLUGIN_LIST_CHANGED    = "OnPluginListChanged";
const string ON_PLUGIN_TABCOMPLETE     = "OnPluginTabComplete";

// AlphaOptionsTable[] is defined in scriptingoptions_portable.cpp (real table).

// ── tolower(string) — locale-independent ASCII lowercase ──────────────────
string tolower(const string& s)
{
    string d(s);
    for (char& c : d) c = (char)::tolower((unsigned char)c);
    return d;
}


// ── ThrowErrorException ───────────────────────────────────────────────────
void ThrowErrorException(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw std::runtime_error(buf);
}


// ── FixupEscapeSequences — convert C-style escape sequences ──────────────
// Mirrors Utilities.cpp::FixupEscapeSequences.
// Converts \n \t \r \a \b \f \v \' \" \\ \? \x## in the source string.
CString FixupEscapeSequences(const CString& strSource)
{
    CString strDest;
    const char* p = static_cast<const char*>(strSource);
    for (; *p; ++p) {
        if (*p != '\\') { strDest += *p; continue; }
        char c = *++p;
        if (!c) break;
        switch (c) {
        case 'a':  strDest += '\a'; break;
        case 'b':  strDest += '\b'; break;
        case 'f':  strDest += '\f'; break;
        case 'n':  strDest += '\n'; break;
        case 'r':  strDest += '\r'; break;
        case 't':  strDest += '\t'; break;
        case 'v':  strDest += '\v'; break;
        case '\'': strDest += '\''; break;
        case '"':  strDest += '"';  break;
        case '\\': strDest += '\\'; break;
        case '?':  strDest += '?';  break;
        case 'x': {
            // hex escape: up to 2 hex digits
            unsigned char val = 0;
            int digits = 0;
            for (++p; *p && isxdigit((unsigned char)*p) && digits < 2; ++p, ++digits) {
                int d = toupper((unsigned char)*p);
                val = (unsigned char)((val << 4) + (d >= 'A' ? d - 'A' + 10 : d - '0'));
            }
            --p;  // will be incremented by the outer for loop
            strDest += (char)val;
            break;
        }
        default:   strDest += c; break;
        }
    }
    return strDest;
}


// ── GetClipboardContents — clipboard read (no-op on non-Windows) ──────────
// Return type matches the mfc_ui.h declaration: BOOL.
BOOL GetClipboardContents(CString& /*str*/, bool /*bAppend*/, bool /*bLine*/)
{
    return FALSE;
}

// ── putontoclipboard — clipboard write (no-op on non-Windows) ────────────
// Return type matches the mfc_ui.h declaration: BOOL.
BOOL putontoclipboard(const CString& /*str*/, bool /*bAppend*/)
{
    return FALSE;
}

// ── Directory navigation (no-ops on non-Windows) ──────────────────────────
void ChangeToFileBrowsingDirectory() {}
void ChangeToStartupDirectory()      {}

// ── IsArchiveXML — always false (CArchive not implemented) ────────────────
bool IsArchiveXML(class CArchive& /*ar*/)
{
    return false;
}

// ── MakeRandomNumber — SHS-based random seed ──────────────────────────────
// Mirrors Utilities.cpp::MakeRandomNumber on Windows: hash together the
// world name, rand(), pid, current time, and the doc pointer for entropy.
// Used for Pueblo MD5 challenge-response and chat-socket nonces.
void MakeRandomNumber(CMUSHclientDoc* pDoc, SHS_INFO& shsInfo)
{
    char buf[256];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int n = snprintf(buf, sizeof(buf), "%s %d %d %ld %ld %p",
                     pDoc ? static_cast<const char*>(pDoc->m_mush_name) : "",
                     rand(), (int)getpid(),
                     (long)ts.tv_sec, ts.tv_nsec,
                     static_cast<void*>(pDoc));
    shsInit(&shsInfo);
    shsUpdate(&shsInfo, reinterpret_cast<unsigned char*>(buf), n);
    shsFinal(&shsInfo);
}

// ── CMUSHclientDoc::Trace — debug trace with printf format ───────────────
void CMUSHclientDoc::Trace(const char* fmt, ...)
{
    if (!g_debug) return;
    if (g_before_output) g_before_output();
    fprintf(stderr, "\033[2m[trace] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\033[0m\n");
    fflush(stderr);
    if (g_after_output) g_after_output();
}

// ── CMUSHclientDoc::WriteToLog ────────────────────────────────────────────
// Writes raw bytes to the open log file.  On write error the file is closed
// and a Note() is emitted so the user sees the problem.
// Mirrors the implementation in doc.cpp (which uses UMessageBox instead).


// ── CMUSHclientDoc::LogCommand ────────────────────────────────────────────
// Logs a command sent to the MUD when input logging is active.
// Mirrors the implementation in doc.cpp, minus the Windows-only UI calls.

// ── CMUSHclientDoc::OutputOutstandingLines ────────────────────────────────
// Re-outputs note/tell lines that were deferred while a trigger was
// processing (e.g. when a trigger omits an incoming MUD line).
// Mirrors the doc.cpp implementation, adapted for the headless/GUI paths:
// instead of calling Tell() (which is in methods_noting.cpp / mushapp only),
// we write directly through DisplayMsg() using the COLOUR_RGB style flag so
// that GetStyleRGB() treats m_iForeColour/m_iBackColour as raw COLOREFs.
// For the GUI path (a view attached) each item becomes a styled run;
// for the CLI path it becomes an ANSI-coloured terminal line.
void CMUSHclientDoc::OutputOutstandingLines()
{
    if (m_OutstandingLines.empty())
        return;

    // Save and restore the current MUD-stream style so that the note lines
    // don't pollute the style state used for subsequent MUD output.
    unsigned short savedFlags  = m_iFlags;
    COLORREF       savedFore   = m_iForeColour;
    COLORREF       savedBack   = m_iBackColour;

    for (const CPaneStyle& item : m_OutstandingLines) {
        // Set COLOUR_RGB so GetStyleRGB() treats iForeColour/iBackColour as
        // raw COLOREFs — exactly what CPaneStyle stores.
        m_iFlags      = (unsigned short)((item.m_iStyle & ~COLOURTYPE) | COLOUR_RGB);
        m_iForeColour = item.m_cText;
        m_iBackColour = item.m_cBack;

        const char* text = item.m_sText.c_str();
        int         len  = (int)item.m_sText.size();

        if (len > 0)
            DisplayMsg(text, len, NOTE_OR_COMMAND);

        // Each pending item represents one logical line; append a newline
        // (unless the text already ends with one, e.g. ENDLINE entries).
        if (len == 0 || item.m_sText.back() != '\n')
            DisplayMsg("\n", 1, NOTE_OR_COMMAND);
    }

    m_OutstandingLines.clear();

    m_iFlags      = savedFlags;
    m_iForeColour = savedFore;
    m_iBackColour = savedBack;

    // Re-sync terminal colours after restoring the stream state.
    if (!HaveOutputSink())
        TermResetStyle();
}

// ── CMUSHclientDoc::PlaySoundFile / CancelSound — MSP audio ─────────────
// On non-Windows, delegate to the wxWidgets sound hook so that trigger
// sounds and MSP audio actually play in mushgui.  A headless build silently drops
// sound requests (no hook installed → nullptr check is sufficient).
void CMUSHclientDoc::PlaySoundFile(CString strFile)
{
    if (g_wxhook_play_sound)
        g_wxhook_play_sound((const char*)strFile, false);
}

void CMUSHclientDoc::CancelSound()
{
    if (g_wxhook_stop_sound)
        g_wxhook_stop_sound();
}

// ── CMUSHclientDoc::FormatTime ─────────────────────────────────────────────
// Converts a CTime to a formatted string.  Used for log line timestamps.
CString CMUSHclientDoc::FormatTime(CTime t, const char* fmt, bool /*bUTC*/)
{
    if (!fmt || !*fmt) return CString("");
    time_t tt = (time_t)t.GetTime();
    struct tm* tm_info = localtime(&tt);
    if (!tm_info) return CString("");
    char buf[256];
    strftime(buf, sizeof buf, fmt, tm_info);
    return CString(buf);
}

// ── Speedwalk (shared) ─────────────────────────────────────────────────────
// The speedwalk parser lives in the shared speedwalk.cpp (compiled into both
// mushcore and mushapp) so it cannot drift from the Windows original. These thin
// wrappers inject the portable built-in n/s/e/w/u/d direction tables — the
// GUI-only MapDirectionsMap is not populated in the portable/CLI build. The line

// ── CMUSHclientDoc::CheckConnected ────────────────────────────────────────
// MUSHclient convention: returns 0 (FALSE) if connected, non-zero if not.
// evaluate.cpp checks "if (CheckConnected()) return true;" to bail early.
BOOL CMUSHclientDoc::CheckConnected()
{
    return (m_iConnectPhase != eConnectConnectedToMud) ? TRUE : FALSE;
}

// ── CMUSHclientDoc::SendMsg ────────────────────────────────────────────────
// Sends a command line to the MUD.  bEchoIt → echo to output window.
// bQueueIt is ignored (no speedwalk queue on non-Windows).
void CMUSHclientDoc::SendMsg(CString strText,
                              const bool bEchoIt,
                              const bool /*bQueueIt*/,
                              const bool /*bLogIt*/)
{
    if (m_bPluginProcessingSent) return;

    // Strip trailing \r\n so we add exactly one
    while (strText.GetLength() >= 2 &&
           strText.Right(2) == CString(ENDLINE))
        strText = strText.Left(strText.GetLength() - 2);

    // "OnPluginSend" — plugins can cancel the send by returning false.
    // Guard with m_bPluginProcessingSend to prevent infinite recursion
    // (a plugin calling world.Send() inside OnPluginSend).
    if (!m_bPluginProcessingSend) {
        m_bPluginProcessingSend = true;
        bool ok = SendToAllPluginCallbacks(ON_PLUGIN_SEND, strText);
        m_bPluginProcessingSend = false;
        if (!ok) return;  // plugin vetoed the send
    }

    // Echo sent text to the output window
    if (bEchoIt && !m_bNoEcho)
        DisplayMsg(((const char*)strText + string("\n")).c_str(),
                   (int)strlen((const char*)strText) + 1, NOTE_OR_COMMAND);

    // Send to MUD
    CString toSend = strText + CString(ENDLINE);
    SendPacket((const char*)toSend, toSend.GetLength());

    ++m_nTotalLinesSent;

    // "OnPluginSent" — notify plugins that the line was sent.
    if (!m_bPluginProcessingSent) {
        m_bPluginProcessingSent = true;
        SendToAllPluginCallbacks(ON_PLUGIN_SENT, strText);
        m_bPluginProcessingSent = false;
    }
}

// ── CMUSHclientDoc::SendTo ────────────────────────────────────────────────
// Routes a trigger/alias response to the appropriate destination.
// Enum values from OtherTypes.h (included via doc_stub.h):
//   0=World,  1=Command,   2=Output,     3=Status,   4=Notepad,
//   5=Append, 6=LogFile,   7=Replace,    8=CmdQueue, 9=Variable,
//  10=Execute,11=Speedwalk,12=Script,   13=Immediate,14=ScriptAfterOmit,
//  15=Pane
void CMUSHclientDoc::SendTo(const unsigned short iWhere,
                             const CString strSendText,
                             const bool bOmitFromOutput,
                             const bool bOmitFromLog,
                             const CString strDescription,
                             const CString strVariable,
                             CString& /*strOutput*/)
{
    switch (iWhere) {
    case eSendToWorld:        // 0 — send to MUD socket
    case eSendToCommandQueue: // 8 — queue then send
        SendMsg(strSendText,
                bOmitFromOutput ? false : (bool)m_display_my_input,
                (iWhere == eSendToCommandQueue),
                !bOmitFromLog);
        break;

    case eSendToCommand:      // 1 — put text in command bar (only if empty)
        // SetCommandText returns false without overwriting if the bar is
        // non-empty, matching the Windows CSendView iteration behaviour.
        SetCommandText(std::string((const char*)strSendText));
        break;

    case eSendToOutput:       // 2 — display in output window without sending
        if (!bOmitFromOutput)
            Note(strSendText);
        break;

    case eSendToStatus:       // 3 — update status message
        m_strStatusMessage = strSendText;
        break;

    case eSendToNotepad:      // 4 — open new notepad (create or append)
    case eAppendToNotepad:    // 5 — append to existing notepad with this title
    case eReplaceNotepad:     // 7 — replace contents of notepad with this title
        {
            // strDescription is the notepad title.
            // AppendToTheNotepad() will create a new window if one with that
            // title doesn't exist yet, so all three variants work through it.
            bool bReplace = (iWhere == eReplaceNotepad);
            AppendToTheNotepad(strDescription,
                               strSendText + ENDLINE,
                               bReplace,
                               eNotepadTrigger);
        }
        break;

    case eSendToLogFile:      // 6 — append text to the open log file
        if (m_logfile && !m_bLogRaw) {
            WriteToLog(strSendText);
            WriteToLog("\n", 1);
        }
        break;

    case eSendToVariable:     // 9 — set a world variable by name
        if (!strVariable.IsEmpty()) {
            CVariable* var = nullptr;
            if (GetVariableMap().Lookup(strVariable, var))
                delete var;
            var = new CVariable();
            var->strLabel    = strVariable;
            var->strContents = strSendText;
            m_bVariablesChanged = true;
            GetVariableMap().SetAt(strVariable, var);
        }
        break;

    case eSendToExecute:      // 10 — execute as a MUSHclient command
        {
            bool bOmit = bOmitFromLog;
            EvaluateCommand(strSendText, /*countThem=*/false,
                            bOmit, /*test=*/false);
        }
        break;

    case eSendToSpeedwalk:    // 11 — evaluate and send as speedwalk sequence
        {
            CString strEvaluated = DoEvaluateSpeedwalk(strSendText);
            if (!strEvaluated.IsEmpty()) {
                if (strEvaluated[0] == '*') {
                    // Error message from DoEvaluateSpeedwalk — display it.
                    Note(strEvaluated.Mid(1));
                } else {
                    SendMsg(strEvaluated,
                            bOmitFromOutput ? false : (bool)m_display_my_input,
                            /*bQueueIt=*/true,
                            bOmitFromLog ? false : (bool)LoggingInput());
                }
            }
        }
        break;

    case eSendToScript:           // 12 — pass text to the script engine
    case eSendToScriptAfterOmit:  // 14 — same but after omit processing
        {
            CScriptEngine* engine = GetScriptEngine();
            if (engine)
                engine->Parse(strSendText, strDescription);
            else
                ColourNote("white", "red",
                    "Send-to-script cannot execute because scripting is not enabled.");
        }
        break;

    case eSendImmediate:      // 13 — send directly (bypasses spam prevention)
        // On Windows this calls DoSendMsg() which additionally runs plugin
        // hooks and spam-prevention.  In the portable build those aren't
        // available, so we fall back to SendMsg() with no queuing.
        SendMsg(strSendText,
                bOmitFromOutput ? false : (bool)m_display_my_input,
                /*bQueueIt=*/false,
                !bOmitFromLog);
        break;

    default:
        // eSendToPane(15) and any future destinations — not implemented.
        break;
    }
}

// ── CMUSHclientDoc::ExecuteTriggerScript ─────────────────────────────────
// Fires the Lua script callback for a trigger match.
#include "paneline.h"
void CMUSHclientDoc::ExecuteTriggerScript(CTrigger* trigger_item,
                                           CString   strLine,
                                           CPaneLine& StyledLine)
{
    if (trigger_item->strProcedure.IsEmpty())
        return;

    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua())
        return;

    // Get the label (name) for this trigger — used as argument 1 to the callback.
    const char* pLabel = trigger_item->strLabel;
    if (pLabel[0] == 0)
        pLabel = GetTriggerRevMap()[trigger_item].c_str();

    list<double> nparams;
    list<string> sparams;
    sparams.push_back(pLabel);
    sparams.push_back((LPCTSTR)strLine);

    trigger_item->bExecutingScript = true;
    engine->ExecuteLua(trigger_item->dispid,
                       trigger_item->strProcedure,
                       eTriggerFired,
                       "trigger",
                       TFormat("trigger \"%s\" matched: \"%s\"",
                               pLabel, (LPCTSTR)strLine),
                       nparams, sparams,
                       trigger_item->nInvocationCount,
                       trigger_item->regexp,
                       nullptr,   // no extra string table
                       &StyledLine);
    trigger_item->bExecutingScript = false;
}

void CMUSHclientDoc::TriggerToPane(CTrigger* /*pTrigger*/,
                                    CPaneLine& /*StyledLine*/)
{
}

// ── Lua function stubs ────────────────────────────────────────────────────
// ── AdjustColour / ColourToName (portable, mirror Utilities.cpp) ─────────

long AdjustColour(long Colour, short Method)
{
    COLORREF c = (COLORREF)(Colour & 0xFFFFFF);
    CColor clr;
    clr.SetColor(c);
    // Values match ADJUST_COLOUR_* enum in stdafx.h: 1=invert, 2=lighter,
    // 3=darker, 4=less colour, 5=more colour.  Use raw literals so this TU
    // does not need to include stdafx.h.
    switch (Method) {
        case 1: // ADJUST_COLOUR_INVERT
            return (long)RGB(255 - GetRValue(c), 255 - GetGValue(c), 255 - GetBValue(c));
        case 2: { // ADJUST_COLOUR_LIGHTER
            double lum = clr.GetLuminance() + 0.02;
            if (lum > 1.0) lum = 1.0;
            clr.SetLuminance(lum);
            return (long)(COLORREF)clr;
        }
        case 3: { // ADJUST_COLOUR_DARKER
            double lum = clr.GetLuminance() - 0.02;
            if (lum < 0.0) lum = 0.0;
            clr.SetLuminance(lum);
            return (long)(COLORREF)clr;
        }
        case 4: { // ADJUST_COLOUR_LESS_COLOUR
            double sat = clr.GetSaturation() - 0.05;
            if (sat < 0.0) sat = 0.0;
            clr.SetSaturation(sat);
            return (long)(COLORREF)clr;
        }
        case 5: { // ADJUST_COLOUR_MORE_COLOUR
            double sat = clr.GetSaturation() + 0.05;
            if (sat > 1.0) sat = 1.0;
            clr.SetSaturation(sat);
            return (long)(COLORREF)clr;
        }
        default:
            return (long)c;
    }
}

// Standard 16 CSS/X11 colour names for reverse lookup
CString ColourToName(COLORREF colour)
{
    static const struct { COLORREF rgb; const char* name; } kNames[] = {
        { RGB(  0,   0,   0), "black"   },
        { RGB(128,   0,   0), "maroon"  },
        { RGB(  0, 128,   0), "green"   },
        { RGB(128, 128,   0), "olive"   },
        { RGB(  0,   0, 128), "navy"    },
        { RGB(128,   0, 128), "purple"  },
        { RGB(  0, 128, 128), "teal"    },
        { RGB(192, 192, 192), "silver"  },
        { RGB(128, 128, 128), "gray"    },
        { RGB(255,   0,   0), "red"     },
        { RGB(  0, 255,   0), "lime"    },
        { RGB(255, 255,   0), "yellow"  },
        { RGB(  0,   0, 255), "blue"    },
        { RGB(255,   0, 255), "fuchsia" },
        { RGB(  0, 255, 255), "aqua"    },
        { RGB(255, 255, 255), "white"   },
    };
    for (auto& e : kNames)
        if (e.rgb == colour) return CString(e.name);
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             GetRValue(colour), GetGValue(colour), GetBValue(colour));
    return CString(buf);
}

// ProcessPreviousLine.cpp calls these to build the Lua table that is passed
// to OnPluginLineReceived.  When Lua is not compiled in (no liblua5.1), the
// stubs are needed so the translation unit links.  When Lua IS compiled in
// (MUSHCLIENT_LUA_SCRIPTING defined), the real Lua library supplies them and
// these stubs must NOT be compiled — hence the #ifndef guard.
#ifndef MUSHCLIENT_LUA_SCRIPTING

// Forward declarations to avoid including lua.h (which may not be present).
struct lua_State;

extern "C" {
void lua_createtable(lua_State*, int, int) {}
void lua_rawseti    (lua_State*, int, int) {}
void lua_setfield   (lua_State*, int, const char*) {}
}

// MakeTableItem overloads (from scripting/lua_utils_portable.cpp when Lua present).
void MakeTableItem(lua_State*, const char*, const std::string&) {}
void MakeTableItem(lua_State*, const char*, double) {}

#endif // !MUSHCLIENT_LUA_SCRIPTING


// ── CMUSHclientDoc::OnScriptFileChanged ──────────────────────────────────────
// Portable replacement for the Windows version (scripting/scripting.cpp).
// Called from the wxWidgets world timer (wx_world_child.cpp) every ~2 seconds.
//
// Checks whether the script file on disk has changed since the last load.
// If it has, the old engine is destroyed and the script is reloaded from the
// file.  On non-Windows there is no GUI prompt: the script is always
// reloaded automatically (equivalent to m_nReloadOption == eReloadAlways).
// The bForce parameter is accepted for API compatibility but has no effect
// on non-Windows (we never prompt, so "force" vs "ask" is the same).
void CMUSHclientDoc::OnScriptFileChanged(bool /*bForce*/)
{
    // Re-entrancy guard (the scripting callbacks could trigger us again).
    if (m_bInScriptFileChanged)
        return;

    // Nothing to watch if there is no external script file.
    if (m_strScriptFilename.IsEmpty())
        return;

    // Check the file's mtime via stat().
    struct stat st;
    if (::stat((const char*)m_strScriptFilename, &st) != 0)
        return;  // file disappeared or unreadable — leave engine as-is

    if (st.st_mtime == m_scriptFileMtime)
        return;  // unchanged

    m_bInScriptFileChanged = true;

    fprintf(stderr, "[script] \"%s\" changed — reloading\n",
            (const char*)m_strScriptFilename);

    // ── Tear down the old engine ──────────────────────────────────────────
    delete m_ScriptEngine;
    m_ScriptEngine = nullptr;

    // ── Read the file ─────────────────────────────────────────────────────
    FILE* f = fopen((const char*)m_strScriptFilename, "rb");
    if (!f) {
        fprintf(stderr, "[script] could not open \"%s\": %s\n",
                (const char*)m_strScriptFilename, strerror(errno));
        m_bInScriptFileChanged = false;
        return;
    }
    fseek(f, 0, SEEK_END);
    long fileLen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(fileLen + 1);
    if (!buf) {
        fclose(f);
        m_bInScriptFileChanged = false;
        return;
    }
    fread(buf, 1, fileLen, f);
    fclose(f);
    buf[fileLen] = '\0';
    m_strScript = CString(buf);
    free(buf);

    // Record new mtime before creating the engine so a re-entrant call
    // during Parse() sees the updated timestamp and does not loop.
    m_scriptFileMtime = st.st_mtime;

    // ── Create and initialise new Lua engine ──────────────────────────────
    CScriptEngine* engine = new CScriptEngine(this, "lua");
    if (engine->CreateScriptEngine()) {
        fprintf(stderr, "[script] could not initialise Lua engine\n");
        delete engine;
        m_bInScriptFileChanged = false;
        return;
    }

    if (engine->Parse(m_strScript, m_strScriptFilename)) {
        fprintf(stderr, "[script] error parsing \"%s\"\n",
                (const char*)m_strScriptFilename);
        delete engine;
        m_bInScriptFileChanged = false;
        return;
    }

    m_ScriptEngine = engine;
    ResolveAllEntryPoints();

    fprintf(stderr, "[script] \"%s\" reloaded successfully\n",
            (const char*)m_strScriptFilename);

    m_bInScriptFileChanged = false;
}

// ── ConnectSocket ─────────────────────────────────────────────────────────────
// Single-sourced in doc_shared.cpp (T3c) — connects through the PAL transport.
//
// Called automatically from timers.cpp's CheckTimers() when:
//   App.m_bReconnectOnLinkFailure is true AND
//   !m_bDisconnectOK AND m_iConnectPhase == eConnectNotConnected
// ConnectSocket: single-sourced in doc_shared.cpp (T3c).

#endif // !_WIN32
