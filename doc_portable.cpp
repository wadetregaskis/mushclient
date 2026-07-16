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

    // ── The real line lifecycle (display step 7) ──────────────────
    // Mirror doc.cpp SetUpOutputWindow: the current line lives IN m_LineList
    // from creation, and m_pLinePositions indexes every JUMP_SIZE'th line.
    // Windows defers both until the world file is read; headless we seed at
    // construction with the defaults, and AllocateLinePositions() re-sizes the
    // index if a loaded world later raises m_maxlines.
    m_total_lines = 0;
    m_pCurrentLine = new CLine(++m_total_lines, m_nWrapColumn, 0, WHITE, BLACK, m_bUTF_8);
    m_LineList.AddTail(m_pCurrentLine);
    AllocateLinePositions();

    // Register with the global world-document list so that
    // world.GetWorld() / GetWorldList() etc. can find this doc.
    App.m_pWorldDocTemplate->AddDoc(this);
}

// ── CMUSHclientDoc destructor ─────────────────────────────────────────────
// On non-Windows, doc_construct.cpp is not compiled into mushcore.  This
// portable destructor — mirrors doc_construct.cpp's ~CMUSHclientDoc for the
// resources that mushcore owns.
// ── AllocateLinePositions ──────────────────────────────
// (Re)build the JUMP_SIZE line-position index from m_maxlines -- the headless
// equivalent of the deferred allocation in doc.cpp SetUpOutputWindow.  Called
// at construction and again by the world loader when m_maxlines changes
// (an under-sized index would let the ProcessPreviousLine omit-branch write
// past the end).  Zeroed entries are tolerated by every reader.
void CMUSHclientDoc::AllocateLinePositions()
{
    // Trim FIRST so the index can never be smaller than the list it indexes
    // (mirrors doc.cpp ChangeBufferSize; the loader min of 200 > JUMP_SIZE
    // guarantees RemoveChunk never reaches the in-progress tail).
    if (m_pLinePositions)
        while ((long)m_LineList.GetCount() > m_maxlines)
            RemoveChunk();

    delete [] m_pLinePositions;
    m_pLinePositions = new POSITION[(m_maxlines / JUMP_SIZE) + 1];
    for (long i = 0; i <= m_maxlines / JUMP_SIZE; i++)
        m_pLinePositions[i] = NULL;
    m_pLinePositions[0] = m_LineList.GetHeadPosition();
}

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

    // Free the line-positions index (allocated by AllocateLinePositions).
    delete [] m_pLinePositions;
    m_pLinePositions = nullptr;

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

    // The in-progress line lives IN m_LineList (display step 7), so the
    // DELETE_LIST above owns and frees it.
    m_pCurrentLine = nullptr;
}





// ── CheckObjectName ───────────────────────────────────────────────────────
// Mirrors CMUSHclientDoc::CheckObjectName in doc.cpp:
//   1. Trim leading/trailing whitespace.
//   2. If bConform: validate with CheckLabel — return eInvalidObjectLabel on fail.
//   3. Lowercase the name (keys in the trigger/alias/timer maps are lowercase).
// Returns eOK on success.

// CheckLabel: first char must be alpha; remaining chars alpha/digit/underscore.
// Returns TRUE if the label is INVALID (matching doc.cpp convention).

long CheckObjectName(CString& strName, const bool bConform)
{
    strName.TrimLeft();
    strName.TrimRight();

    if (bConform)
        if (CheckLabel(strName, false))
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

// FireWorld{Close,Save,GetFocus,LoseFocus} are single-sourced in
// doc_scripting_shared.cpp (shared with mushapp; was a duplicate here).


// The Term*/stdout ANSI emission died at the display flip (step 7) — the
// drain in doc_display_shared.cpp is the only output path now.

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
const string ON_PLUGIN_PARTIAL_LINE    = "OnPluginPartialLine";
const string ON_PLUGIN_PACKET_RECEIVED = "OnPluginPacketReceived";

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


// ── CMUSHclientDoc::PlaySoundFile / CancelSound — MSP audio ─────────────
// On non-Windows, delegate to the wxWidgets sound hook so that trigger
// sounds and MSP audio actually play in mushgui.  A headless build silently drops
// sound requests (no hook installed → nullptr check is sufficient).
bool CMUSHclientDoc::PlaySoundFile(CString strFile)
{
    if (g_wxhook_play_sound)
        return g_wxhook_play_sound((const char*)strFile, false);
    return false;   // headless: no audio backend
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

// SendMsg / DoSendMsg are single-sourced in doc_shared.cpp — the real send
// family (queueing, spam prevention, OnPluginSend/Sent ordering, IAC
// doubling, logging) replaced the simplified twin here.

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
// ExecuteTriggerScript: single-sourced in doc_shared.cpp (the real body —
// all-engine UsesEmbeddedCallback guard + scripting-inactive warning — replaced
// the Lua-only twin here).

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
