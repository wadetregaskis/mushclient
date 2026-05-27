// doc_portable.cpp — non-Windows implementations of CMUSHclientDoc helpers.
//
// On Windows these methods live in the monolithic doc.cpp / MXP files.
// We provide stubs here so the protocol files and test suite can link
// without dragging in MFC or the scripting engine.
//
// Guard: excluded from the Windows build entirely.

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "Color.h"
#include "worldsock.h"
#ifdef OPENSSL_FOUND
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#endif
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <climits>
#include <zlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

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

void Generate256colours()
{
    const unsigned char values[6] = { 0, 95, 135, 175, 215, 255 };

    xterm_256_colours[ 0] = RGB(  0,   0,   0);
    xterm_256_colours[ 1] = RGB(128,   0,   0);
    xterm_256_colours[ 2] = RGB(  0, 128,   0);
    xterm_256_colours[ 3] = RGB(128, 128,   0);
    xterm_256_colours[ 4] = RGB(  0,   0, 128);
    xterm_256_colours[ 5] = RGB(128,   0, 128);
    xterm_256_colours[ 6] = RGB(  0, 128, 128);
    xterm_256_colours[ 7] = RGB(192, 192, 192);
    xterm_256_colours[ 8] = RGB(128, 128, 128);
    xterm_256_colours[ 9] = RGB(255,   0,   0);
    xterm_256_colours[10] = RGB(  0, 255,   0);
    xterm_256_colours[11] = RGB(255, 255,   0);
    xterm_256_colours[12] = RGB(  0,   0, 255);
    xterm_256_colours[13] = RGB(255,   0, 255);
    xterm_256_colours[14] = RGB(  0, 255, 255);
    xterm_256_colours[15] = RGB(255, 255, 255);

    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                xterm_256_colours[16 + r*36 + g*6 + b] =
                    RGB(values[r], values[g], values[b]);

    for (int grey = 0; grey < 24; grey++) {
        unsigned char v = (unsigned char)(8 + grey * 10);
        xterm_256_colours[232 + grey] = RGB(v, v, v);
    }
}

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
}

// ── GetStyleRGB ───────────────────────────────────────────────────────────
void CMUSHclientDoc::GetStyleRGB(CStyle* pStyle,
                                  COLORREF& colour1,
                                  COLORREF& colour2) const
{
    unsigned short style = pStyle->iFlags & STYLE_BITS;

    if ((style & COLOURTYPE) == COLOUR_CUSTOM) {
        if (style & INVERSE) {
            colour1 = m_customback[pStyle->iForeColour];
            colour2 = m_customtext[pStyle->iForeColour];
        } else {
            colour1 = m_customtext[pStyle->iForeColour];
            colour2 = m_customback[pStyle->iForeColour];
        }
    } else if ((style & COLOURTYPE) == COLOUR_RGB) {
        if (style & INVERSE) {
            colour1 = pStyle->iBackColour;
            colour2 = pStyle->iForeColour;
        } else {
            colour1 = pStyle->iForeColour;
            colour2 = pStyle->iBackColour;
        }
    } else {
        int iForeground = pStyle->iForeColour;
        int iBackground = pStyle->iBackColour;

        if (m_bAlternativeInverse) {
            if (style & INVERSE) {
                colour2 = (iForeground >= 8) ? xterm_256_colours[iForeground]
                    : ((style & HILITE) ? m_boldcolour[iForeground]
                                       : m_normalcolour[iForeground]);
                colour1 = (iBackground >= 8) ? xterm_256_colours[iBackground]
                    : m_normalcolour[iBackground];
            } else {
                colour1 = (iForeground >= 8) ? xterm_256_colours[iForeground]
                    : ((style & HILITE) ? m_boldcolour[iForeground]
                                       : m_normalcolour[iForeground]);
                colour2 = (iBackground >= 8) ? xterm_256_colours[iBackground]
                    : m_normalcolour[iBackground];
            }
        } else {
            COLORREF fg = (iForeground >= 8) ? xterm_256_colours[iForeground]
                : ((style & HILITE) ? m_boldcolour[iForeground]
                                    : m_normalcolour[iForeground]);
            COLORREF bg = (iBackground >= 8) ? xterm_256_colours[iBackground]
                : m_normalcolour[iBackground];
            if (style & INVERSE) { colour1 = bg; colour2 = fg; }
            else                 { colour1 = fg; colour2 = bg; }
        }
    }
}

// ── RememberStyle ─────────────────────────────────────────────────────────
void CMUSHclientDoc::RememberStyle(const CStyle* pStyle)
{
    if (!pStyle) return;
    m_iFlags      = pStyle->iFlags & STYLE_BITS;
    m_iForeColour = pStyle->iForeColour;
    m_iBackColour = pStyle->iBackColour;
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
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams;
        list<string> sparams;
        long nInvocations = 0;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations);
    }
}

// Overload 2 — fire callback with one integer and one string param;
// returns bDefaultReturn unless a plugin returns a different bool.
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName,
                                               long nParam,
                                               const string& sParam,
                                               bool /*bStop*/,
                                               bool bDefaultReturn)
{
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
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations,
                                       nullptr, nullptr, nullptr,
                                       &pluginResult);
        result = pluginResult;
    }
    return result;
}

// Overload 3 — fire callback with one CString param; returns true if
// no plugin intercepted (all returned true / didn't intercept).
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName,
                                               const CString& sParam)
{
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams;
        list<string> sparams = { string((const char*)sParam) };
        bool intercepted = false;
        long nInvocations = 0;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations,
                                       nullptr, nullptr, nullptr,
                                       &intercepted);
        if (intercepted) return false;  // plugin intercepted — stop
    }
    return true;  // no interception
}

// Overload 4 — fire callback with one CString param; returns true if
// any plugin handled the event.
bool CMUSHclientDoc::SendToAllPluginCallbacks(const string& callbackName,
                                               const CString& sParam,
                                               bool /*bStop*/)
{
    for (CPlugin* p : m_PluginList) {
        if (!p->m_bEnabled || !p->m_ScriptEngine) continue;
        CString strName(callbackName.c_str());
        DISPID dispid = p->m_ScriptEngine->GetLuaDispid(strName);
        if (dispid == DISPID_UNKNOWN) continue;
        list<double> nparams;
        list<string> sparams = { string((const char*)sParam) };
        bool handled = false;
        long nInvocations = 0;
        p->m_ScriptEngine->ExecuteLua(dispid, strName,
                                       eInputFromServer,
                                       "plugin", callbackName.c_str(),
                                       nparams, sparams,
                                       nInvocations,
                                       nullptr, nullptr, nullptr,
                                       &handled);
        if (handled) return true;
    }
    return false;
}

void CMUSHclientDoc::SendPacket(const char* lpBuf, const int nBufLen)
{
    if (!m_pSocket || nBufLen <= 0) return;

#ifdef OPENSSL_FOUND
    if (m_pSSL && m_bSSL_Connected) {
        SSL_write(m_pSSL, lpBuf, nBufLen);
        return;
    }
#endif
    m_pSocket->m_outstanding_data.append(lpBuf, nBufLen);
    // Trigger send attempt — fire OnSend(0) if there's data queued
    m_nBytesOut += nBufLen;
}

void CMUSHclientDoc::SendPacket(const unsigned char* lpBuf, const int nBufLen)
{
    SendPacket(reinterpret_cast<const char*>(lpBuf), nBufLen);
}

void CMUSHclientDoc::AddToLine(const char*, const int)      {}

// SendWindowSizes — send IAC SB NAWS w_hi w_lo h_hi h_lo IAC SE to the MUD.
// iNewWidth is the new column count (already stored in m_nWrapColumn by caller).
// We read the actual terminal size from the OS; fall back to iNewWidth x 24.
void CMUSHclientDoc::SendWindowSizes(const int iNewWidth)
{
    if (!m_pSocket) return;

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

void CMUSHclientDoc::OnConnectionDisconnect()
{
    // Fire OnWorldDisconnect Lua callback before tearing down socket
    if (m_iConnectPhase == eConnectConnectedToMud &&
        m_dispidWorldDisconnect != DISPID_UNKNOWN) {
        CScriptEngine* engine = GetScriptEngine();
        if (engine && engine->IsLua()) {
            list<double> nparams;
            list<string> sparams;
            long nInvocations = 0;
            engine->ExecuteLua(m_dispidWorldDisconnect,
                               m_strWorldDisconnect,
                               eInputFromServer,
                               "world", "disconnect",
                               nparams, sparams,
                               nInvocations);
        }
    }

    // Fire OnPluginDisconnect for every loaded plugin
    if (m_iConnectPhase == eConnectConnectedToMud)
        SendToAllPluginCallbacks(ON_PLUGIN_DISCONNECT);

    if (m_pSocket) {
        delete m_pSocket;
        m_pSocket = nullptr;
    }
    m_iConnectPhase = eConnectNotConnected;
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
            if (m_fn_line_ready) {
                // GUI path: deliver the assembled styled line and clear buffer.
                m_fn_line_ready(std::move(m_gui_line_buf));
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
                    m_pCurrentLine->text = (char*)realloc(m_pCurrentLine->text, newSize);
                    m_pCurrentLine->iMemoryAllocated = newSize;
                }
                m_pCurrentLine->text[m_pCurrentLine->len++] = (char)c;
                if (!m_pCurrentLine->styleList.IsEmpty())
                    m_pCurrentLine->styleList.GetTail()->iLength++;
            }

            if (m_fn_line_ready) {
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
                    // Append to current run if style unchanged, else start new run.
                    if (m_gui_line_buf.empty()                               ||
                        m_gui_line_buf.back().fg            != fg            ||
                        m_gui_line_buf.back().bg            != bg            ||
                        m_gui_line_buf.back().bold          != bold          ||
                        m_gui_line_buf.back().underline     != underline     ||
                        m_gui_line_buf.back().italic        != italic        ||
                        m_gui_line_buf.back().strikethrough != strikethrough)
                    {
                        SOutputRun run;
                        run.text          = std::string(1, (char)c);
                        run.fg            = fg;
                        run.bg            = bg;
                        run.bold          = bold;
                        run.underline     = underline;
                        run.italic        = italic;
                        run.strikethrough = strikethrough;
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

    if (!m_fn_line_ready) {
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
}

// ── ReceiveMsg — read from socket, decompress if needed, call DisplayMsg ─────

void CMUSHclientDoc::ReceiveMsg()
{
    char buff[9000];   // < COMPRESS_BUFFER_LENGTH

    if (m_iConnectPhase == eConnectAwaitingSSLHandshake) {
        ContinueSSLHandshake();
        return;
    }

    int count;

#ifdef OPENSSL_FOUND
    if (m_pSSL && m_bSSL_Connected) {
        count = SSL_read(m_pSSL, buff, (int)sizeof(buff) - 1);
        if (count <= 0) {
            int ssl_err = SSL_get_error(m_pSSL, count);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
                return;
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {   // clean TLS shutdown
                if (m_pSocket) m_pSocket->OnClose(0);
                return;
            }
            if (m_iConnectPhase == eConnectDisconnecting) return;
            if (m_pSocket) m_pSocket->OnClose(0);
            delete m_pSocket; m_pSocket = nullptr;
            return;
        }
    } else
#endif
    {
        count = m_pSocket->Receive(buff, (int)sizeof(buff) - 1);
        if (count == SOCKET_ERROR) {
            if (m_iConnectPhase == eConnectDisconnecting) return;
            if (m_pSocket) m_pSocket->OnClose(m_pSocket->GetLastError());
            delete m_pSocket; m_pSocket = nullptr;
            return;
        }
        if (count <= 0) return;
    }

    if (m_iConnectPhase != eConnectConnectedToMud)
        return;

    ++m_iInputPacketCount;
    m_nBytesIn += count;

    // ── MCCP decompression path ───────────────────────────────────────────
    if (m_bCompress) {
        if ((COMPRESS_BUFFER_LENGTH - (int)m_zCompress.avail_in) < count) {
            OnConnectionDisconnect();
            fprintf(stderr, "MUSHclient: MCCP input buffer overflow\n");
            return;
        }
        if (m_zCompress.avail_in)
            memmove(m_CompressInput, m_zCompress.next_in, m_zCompress.avail_in);
        m_zCompress.next_in = m_CompressInput;
        memcpy(&m_zCompress.next_in[m_zCompress.avail_in], buff, count);
        m_zCompress.avail_in  += count;
        m_nTotalCompressed    += count;
    }

    while (true) {
        if (!m_bCompress) {
            DisplayMsg(buff, count, 0);
        }
        if (!m_bCompress)
            break;  // not yet compressed or just turned off

        if (m_zCompress.avail_in <= 0)
            break;

        int iResult;
        do {
            iResult = inflate(&m_zCompress, Z_SYNC_FLUSH);
            if (iResult == Z_BUF_ERROR) {
                m_nCompressionOutputBufferSize += COMPRESS_BUFFER_LENGTH;
                m_zCompress.avail_out += COMPRESS_BUFFER_LENGTH;
                m_CompressOutput = (Bytef*)realloc(m_CompressOutput,
                                                    m_nCompressionOutputBufferSize);
                if (!m_CompressOutput) {
                    OnConnectionDisconnect();
                    fprintf(stderr, "MUSHclient: out of memory during MCCP inflate\n");
                    return;
                }
            }
        } while (iResult == Z_BUF_ERROR);

        if (iResult < 0) {
            const char* msg = m_zCompress.msg;
            OnConnectionDisconnect();
            fprintf(stderr, "MUSHclient: MCCP decompress error: %s\n",
                    msg ? msg : "(unknown)");
            return;
        }

        int iLength = m_nCompressionOutputBufferSize - (int)m_zCompress.avail_out;
        m_nTotalUncompressed += iLength;

        if (iLength > 0) {
            DisplayMsg((const char*)m_CompressOutput, iLength, 0);
            m_zCompress.next_out  = m_CompressOutput;
            m_zCompress.avail_out = (uInt)m_nCompressionOutputBufferSize;
        }

        if (iResult == Z_STREAM_END) {
            m_bCompress = false;
            memcpy(buff, m_zCompress.next_in, m_zCompress.avail_in);
            count = (int)m_zCompress.avail_in;
        }
    }
}

// ── OnConnect — called by CWorldSocket when TCP connection completes ──────────

void CMUSHclientDoc::OnConnect(int nErrorCode)
{
    if (nErrorCode != 0) {
        fprintf(stderr, "MUSHclient: connection failed, error %d\n", nErrorCode);
        m_iConnectPhase = eConnectNotConnected;
        return;
    }

    // No proxy, no SSL for now: go straight to connected state
    m_iConnectPhase  = eConnectConnectedToMud;
    m_tConnectTime   = CTime::GetCurrentTime();
    m_tLastPlayerInput = CTime::GetCurrentTime();
    m_bCompress      = false;
    m_nTotalLinesSent   = 0;
    m_nTotalLinesReceived = 0;
    m_iTriggersMatchedThisSessionCount = 0;
    m_iAliasesMatchedThisSessionCount  = 0;
    m_iTimersFiredThisSessionCount     = 0;
    m_iInputPacketCount = 0;
    m_nBytesIn  = 0;
    m_nBytesOut = 0;
    m_iNoteStyle = NORMAL;
    m_ttype_sequence = 0;

    ZeroMemory(m_bClient_sent_IAC_DO,   sizeof m_bClient_sent_IAC_DO);
    ZeroMemory(m_bClient_sent_IAC_DONT, sizeof m_bClient_sent_IAC_DONT);
    ZeroMemory(m_bClient_sent_IAC_WILL, sizeof m_bClient_sent_IAC_WILL);
    ZeroMemory(m_bClient_sent_IAC_WONT, sizeof m_bClient_sent_IAC_WONT);
    ZeroMemory(m_bClient_got_IAC_DO,    sizeof m_bClient_got_IAC_DO);
    ZeroMemory(m_bClient_got_IAC_DONT,  sizeof m_bClient_got_IAC_DONT);
    ZeroMemory(m_bClient_got_IAC_WILL,  sizeof m_bClient_got_IAC_WILL);
    ZeroMemory(m_bClient_got_IAC_WONT,  sizeof m_bClient_got_IAC_WONT);

    fprintf(stderr, "[Connected to %s]\n", (const char*)m_mush_name);

    // Auto-log: if the world has an auto-log filename, open the log file now.
    // Mirrors the logic in doc.cpp's connect sequence.
    if (!m_strAutoLogFileName.IsEmpty() && !m_logfile) {
        CTime theTime = CTime::GetCurrentTime();
        CString strName = FormatTime(theTime, m_strAutoLogFileName, m_bLogHTML != 0);
        if (!strName.IsEmpty()) {
            m_logfile = fopen((const char*)strName, "a+");
            if (m_logfile) {
                m_logfile_name = strName;
                if (!m_strLogFilePreamble.IsEmpty() && !m_bLogRaw) {
                    CString pre = m_strLogFilePreamble;
                    pre.Replace("%n", "\n");
                    pre = FormatTime(theTime, pre, m_bLogHTML != 0);
                    pre.Replace(ENDLINE, "\n");
                    WriteToLog(pre);
                    WriteToLog("\n", 1);
                }
            } else {
                Note(CString("Could not open auto-log file: ") + strName);
            }
        }
    }

    // Send auto-connect text (login commands, etc.) if configured
    if (!m_connect_text.IsEmpty()) {
        // connect_text may be multi-line; send each line
        CString text = m_connect_text;
        int pos = 0;
        CString line;
        while (pos <= text.GetLength()) {
            int nl = text.Find('\n', pos);
            if (nl < 0) nl = text.GetLength();
            line = text.Mid(pos, nl - pos);
            line.TrimRight('\r');
            bool bOmit = false;
            EvaluateCommand(line, false, bOmit, false);
            pos = nl + 1;
        }
    }

    // Fire OnWorldConnect Lua callback if defined
    if (m_dispidWorldConnect != DISPID_UNKNOWN) {
        CScriptEngine* engine = GetScriptEngine();
        if (engine && engine->IsLua()) {
            list<double> nparams;
            list<string> sparams;
            long nInvocations = 0;
            engine->ExecuteLua(m_dispidWorldConnect,
                               m_strWorldConnect,
                               eInputFromServer,
                               "world", "connect",
                               nparams, sparams,
                               nInvocations);
        }
    }

    // Fire OnPluginConnect for every loaded plugin
    SendToAllPluginCallbacks(ON_PLUGIN_CONNECT);
}

// ── SSLCleanup / ContinueSSLHandshake ─────────────────────────────────────────

void CMUSHclientDoc::SSLCleanup()
{
#ifdef OPENSSL_FOUND
    if (m_pSSL) {
        SSL_shutdown(m_pSSL);
        SSL_free(m_pSSL);
        m_pSSL = nullptr;
    }
#endif
    m_bSSL_Connected = false;
}

void CMUSHclientDoc::ContinueSSLHandshake()
{
    // Phase 4: plain-text only — SSL handshake stub
    // Real implementation requires SSL_connect() loop; left for Phase 5.
    fprintf(stderr, "MUSHclient: SSL not yet supported in CLI mode\n");
    OnConnectionDisconnect();
}

// ── CloseLog ─────────────────────────────────────────────────────────────────
// Writes the log-file postamble (if any) and closes m_logfile.
// Mirrors the implementation in scripting/methods/methods_logging.cpp.
void CMUSHclientDoc::CloseLog()
{
    if (!m_logfile)
        return;

    if (!m_strLogFilePostamble.IsEmpty() && !m_bLogRaw) {
        CTime theTime = CTime::GetCurrentTime();
        CString strPost = m_strLogFilePostamble;
        strPost.Replace("%n", "\n");
        strPost = FormatTime(theTime, strPost, m_bLogHTML != 0);
        strPost.Replace(ENDLINE, "\n");
        WriteToLog(strPost);
        WriteToLog("\n", 1);
    }

    fclose(m_logfile);
    m_logfile = nullptr;
}

// ── Note ─────────────────────────────────────────────────────────────────────
void CMUSHclientDoc::Note(const CString& s)
{
    if (m_fn_note) {
        m_fn_note(std::string((const char*)s));
        return;
    }
    if (g_before_output) g_before_output();
    puts((const char*)s);
    fflush(stdout);
    if (g_after_output) g_after_output();
}

void CMUSHclientDoc::Note(const char* s)
{
    if (m_fn_note) {
        m_fn_note(s ? std::string(s) : std::string());
        return;
    }
    if (g_before_output) g_before_output();
    puts(s ? s : "");
    fflush(stdout);
    if (g_after_output) g_after_output();
}

// ── ColourNote / ColourTell ───────────────────────────────────────────────────
// Map a MUSHclient colour name to an ANSI SGR escape string.
// Returns "" for empty / unknown (lets the terminal keep its default).
static const char* colour_to_ansi(const char* name, bool bg)
{
    if (!name || !*name) return "";
    // Foreground codes: 30-37 / 90-97 (bright)
    // Background codes: 40-47 / 100-107 (bright)
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
    // Case-insensitive compare
    CString lc(name);
    lc.MakeLower();
    for (int i = 0; table[i].name; ++i) {
        if (lc == table[i].name) {
            // Return a static buffer — safe because callers use result immediately
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
    if (coloured) fputs("\033[0m", stdout);   // reset
    if (newline)  fputc('\n', stdout);
    fflush(stdout);
}

void CMUSHclientDoc::ColourNote(const char* fg, const char* bg, const CString& text)
{
    colour_print(fg, bg, static_cast<const char*>(text), /*newline=*/true);
}

void CMUSHclientDoc::ColourNote(const char* fg, const char* bg, const char* text)
{
    colour_print(fg, bg, text, /*newline=*/true);
}

void CMUSHclientDoc::ColourTell(const char* fg, const char* bg, const char* text)
{
    colour_print(fg, bg, text, /*newline=*/false);
}

// ── UpdateAllViews ────────────────────────────────────────────────────────────
void CMUSHclientDoc::UpdateAllViews(void*) {}
long CMUSHclientDoc::GetUniqueNumber()          { return 0; }

bool CMUSHclientDoc::AppendToTheNotepad(const CString, const CString,
                                         const bool, const int)
{ return false; }

bool CMUSHclientDoc::SeeIfHandlerCanExecute(const CString&)
{ return false; }

// ── CMUSHclientDoc::ResolveAllEntryPoints ────────────────────────────────
// Portable equivalent of CMUSHclientDoc::FindAllEntryPoints() (scripting.cpp).
//
// After the Lua engine is initialised and the script parsed, every trigger,
// alias, and timer that has a strProcedure name needs its dispid resolved so
// that ExecuteLua() will actually call the function (it skips calls when
// dispid == DISPID_UNKNOWN).  We also resolve the world-level callbacks
// (OnWorldConnect, OnWorldDisconnect, etc.).
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

    // ── World-level callbacks ─────────────────────────────────────────────
    m_dispidWorldOpen       = resolve(m_strWorldOpen);
    m_dispidWorldClose      = resolve(m_strWorldClose);
    m_dispidWorldSave       = resolve(m_strWorldSave);
    m_dispidWorldConnect    = resolve(m_strWorldConnect);
    m_dispidWorldDisconnect = resolve(m_strWorldDisconnect);
    m_dispidWorldGetFocus   = resolve(m_strWorldGetFocus);
    m_dispidWorldLoseFocus  = resolve(m_strWorldLoseFocus);

    // ── MXP callbacks ─────────────────────────────────────────────────────
    m_dispidOnMXP_Start       = resolve(m_strOnMXP_Start);
    m_dispidOnMXP_Stop        = resolve(m_strOnMXP_Stop);
    m_dispidOnMXP_OpenTag     = resolve(m_strOnMXP_OpenTag);
    m_dispidOnMXP_CloseTag    = resolve(m_strOnMXP_CloseTag);
    m_dispidOnMXP_SetVariable = resolve(m_strOnMXP_SetVariable);
    m_dispidOnMXP_Error       = resolve(m_strOnMXP_Error);

    // ── Triggers ──────────────────────────────────────────────────────────
    {
        CTrigger* pTrigger;
        CString   strName;
        for (POSITION pos = m_TriggerMap.GetStartPosition(); pos; )
        {
            m_TriggerMap.GetNextAssoc(pos, strName, pTrigger);
            pTrigger->dispid = resolve(pTrigger->strProcedure);
        }
    }

    // ── Aliases ───────────────────────────────────────────────────────────
    {
        CAlias* pAlias;
        CString strName;
        for (POSITION pos = m_AliasMap.GetStartPosition(); pos; )
        {
            m_AliasMap.GetNextAssoc(pos, strName, pAlias);
            pAlias->dispid = resolve(pAlias->strProcedure);
        }
    }

    // ── Timers ────────────────────────────────────────────────────────────
    {
        CTimer* pTimer;
        CString strName;
        for (POSITION pos = m_TimerMap.GetStartPosition(); pos; )
        {
            m_TimerMap.GetNextAssoc(pos, strName, pTimer);
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
const string ON_PLUGIN_LINE_RECEIVED = "OnPluginLineReceived";
const string ON_PLUGIN_SEND          = "OnPluginSend";
const string ON_PLUGIN_SENT          = "OnPluginSent";

// AlphaOptionsTable[] is defined in scriptingoptions_portable.cpp (real table).

// ── tolower(string) — locale-independent ASCII lowercase ──────────────────
string tolower(const string& s)
{
    string d(s);
    for (char& c : d) c = (char)::tolower((unsigned char)c);
    return d;
}

// ── AnsiCode — convert a numeric ANSI attribute to an ESC sequence ────────
// Used by ProcessPreviousLine.cpp when assembling log lines with colour.
CString AnsiCode(short iCode)
{
    char buf[16];
    snprintf(buf, sizeof buf, "\033[%dm", (int)iCode);
    return CString(buf);
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

// ── FixHTMLString — escape HTML special characters ────────────────────────
// Mirrors Utilities.cpp::FixHTMLString: < > & " → &lt; &gt; &amp; &quot;
CString FixHTMLString(CString strToFix)
{
    CString strOld = strToFix;
    CString strNew;
    int i;
    while ((i = strOld.FindOneOf("<>&\"")) != -1) {
        strNew += strOld.Left(i);
        switch (strOld[i]) {
        case '<':  strNew += "&lt;";   break;
        case '>':  strNew += "&gt;";   break;
        case '&':  strNew += "&amp;";  break;
        case '"':  strNew += "&quot;"; break;
        }
        strOld = strOld.Mid(i + 1);
    }
    strNew += strOld;
    return strNew;
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

// ── ConvertToRegularExpression — wildcard to regex ────────────────────────
// Mirrors Utilities.cpp implementation (minus _itoa which is Windows-only).
CString ConvertToRegularExpression(const CString& strMatchString,
                                   bool bWholeLine,
                                   bool bMakeAsterisksWildcards)
{
    CString strRegexp;
    int iSize = 0;

    // Count how many extra bytes the output needs vs. the input.
    for (const char* p = strMatchString; *p; p++)
        {
        unsigned char c = (unsigned char)*p;
        if (c < ' ')
            iSize += 3;          // \xhh
        else if (isalnum(c) || *p == ' ' || c >= 0x80)
            /* no change */;
        else if (*p == '*')
            iSize += 4;          // (*.*?) — actually becomes (.*?) = 5 chars vs 1 → +4
        else
            iSize++;             // escape prefix '\'
        }

    char* pOut = strRegexp.GetBuffer(strMatchString.GetLength() +
                                     iSize +
                                     2 +    // ^ and $
                                     10);   // null + padding

    if (bWholeLine)
        *pOut++ = '^';

    for (const char* p = strMatchString; *p; p++)
        {
        unsigned char c = (unsigned char)*p;
        if (*p == '\n')
            { *pOut++ = '\\'; *pOut++ = 'n'; }
        else if (c < ' ')
            {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02x", (unsigned)c);
            *pOut++ = '\\'; *pOut++ = 'x';
            *pOut++ = buf[0]; *pOut++ = buf[1];
            }
        else if (isalnum(c) || *p == ' ' || c >= 0x80)
            *pOut++ = *p;
        else if (*p == '*' && bMakeAsterisksWildcards)
            { *pOut++ = '('; *pOut++ = '.'; *pOut++ = '*'; *pOut++ = '?'; *pOut++ = ')'; }
        else
            { *pOut++ = '\\'; *pOut++ = *p; }
        }

    if (bWholeLine)
        *pOut++ = '$';

    *pOut++ = '\0';
    strRegexp.ReleaseBuffer(-1);
    return strRegexp;
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

// ── MakeRandomNumber — SHS-based random seed (stub) ──────────────────────
// ProcessPreviousLine.cpp uses this to seed the Lua random generator.
// SHS_INFO is forward-declared via doc_stub.h → scripting/shs.h.
void MakeRandomNumber(CMUSHclientDoc* /*pDoc*/, SHS_INFO& shsInfo)
{
    memset(&shsInfo, 0, sizeof shsInfo);
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
void CMUSHclientDoc::WriteToLog(const char* pText, unsigned long nLen)
{
    if (!m_logfile || nLen == 0)
        return;

    size_t written = fwrite(pText, 1, nLen, m_logfile);
    if (written != nLen) {
        CString msg;
        msg.Format("Error writing to log file \"%s\" — logging stopped.",
                   (const char*)m_logfile_name);
        fclose(m_logfile);
        m_logfile = nullptr;
        Note(msg);
    }
}

void CMUSHclientDoc::WriteToLog(const CString& sText)
{
    WriteToLog((const char*)sText, (unsigned long)sText.GetLength());
}

// ── CMUSHclientDoc::LogCommand ────────────────────────────────────────────
// Logs a command sent to the MUD when input logging is active.
// Mirrors the implementation in doc.cpp, minus the Windows-only UI calls.
void CMUSHclientDoc::LogCommand(const char* sText)
{
    if (!m_logfile || !LoggingInput() || m_bLogRaw)
        return;

    CString strMessage = sText;

    // strip trailing CR/LF
    if (strMessage.Right(2) == ENDLINE)
        strMessage = strMessage.Left(strMessage.GetLength() - 2);
    strMessage.Replace(ENDLINE, "\n");

    CTime now = CTime::GetCurrentTime();

    // preamble
    CString pre = m_strLogLinePreambleInput;
    pre.Replace("%n", "\n");
    if (pre.Find('%') != -1)
        pre = FormatTime(now, pre, m_bLogHTML != 0);

    // postamble
    CString post = m_strLogLinePostambleInput;
    post.Replace("%n", "\n");
    if (post.Find('%') != -1)
        post = FormatTime(now, post, m_bLogHTML != 0);

    WriteToLog(pre);

    if (m_bLogHTML) {
        // Escape HTML entities in the command text
        strMessage.Replace("&", "&amp;");
        strMessage.Replace("<", "&lt;");
        strMessage.Replace(">", "&gt;");

        if (m_bLogInColour && m_echo_colour != SAMECOLOUR) {
            COLORREF col = m_customtext[m_echo_colour];
            CString tag;
            tag.Format("<font color=\"#%02X%02X%02X\">",
                       GetRValue(col), GetGValue(col), GetBValue(col));
            WriteToLog(tag);
        }

        WriteToLog(strMessage);

        if (m_bLogInColour && m_echo_colour != SAMECOLOUR)
            WriteToLog("</font>", 7);
    } else {
        WriteToLog(strMessage);
    }

    WriteToLog(post);
    WriteToLog("\n", 1);
}

// ── CMUSHclientDoc::OutputOutstandingLines ────────────────────────────────
// Re-outputs note/tell lines that were deferred while a trigger was
// processing (e.g. when a trigger omits an incoming MUD line).
// Mirrors the doc.cpp implementation, adapted for the mushcli/GUI paths:
// instead of calling Tell() (which is in methods_noting.cpp / mushapp only),
// we write directly through DisplayMsg() using the COLOUR_RGB style flag so
// that GetStyleRGB() treats m_iForeColour/m_iBackColour as raw COLOREFs.
// For the GUI path (m_fn_line_ready set) each item becomes a styled run;
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
    if (!m_fn_line_ready)
        TermResetStyle();
}

// ── CMUSHclientDoc::PlaySoundFile — MSP audio (no-op) ───────────────────
void CMUSHclientDoc::PlaySoundFile(CString /*strFile*/)         {}

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

// ── CMUSHclientDoc::DoEvaluateSpeedwalk ────────────────────────────────────
// Portable speedwalk evaluator.  Mirrors methods_speedwalks.cpp logic using
// a built-in direction table (N/S/E/W/U/D and diagonals) instead of the
// Windows MapDirectionsMap (which is populated by Mapping.cpp / dialogs).
CString CMUSHclientDoc::DoEvaluateSpeedwalk(const char* strInput)
{
    // Built-in direction table: letter code → command to send
    static const struct { char code; const char* cmd; } kDirs[] = {
        { 'n', "north" }, { 's', "south" }, { 'e', "east"  },
        { 'w', "west"  }, { 'u', "up"    }, { 'd', "down"  },
    };

    CString result;
    const char* p = strInput;

    while (*p) {
        // skip whitespace
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        // skip {comment}
        if (*p == '{') {
            while (*p && *p != '}') ++p;
            if (*p == '}') ++p;
            continue;
        }

        // read optional repeat count
        int count = 0;
        while (*p && isdigit((unsigned char)*p))
            count = count * 10 + (*p++ - '0');
        if (count == 0) count = 1;
        if (count > 99) return CString("*Speed walk counter exceeds 99\n");

        // skip whitespace after count
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        // filler 'F' — send m_strSpeedWalkFiller (skip silently if empty)
        if (toupper((unsigned char)*p) == 'F') {
            ++p;
            if (!m_strSpeedWalkFiller.IsEmpty())
                for (int i = 0; i < count; ++i)
                    result += m_strSpeedWalkFiller + "\n";
            continue;
        }

        // custom direction (something/reverse)
        if (*p == '(') {
            ++p;
            CString custom;
            while (*p && *p != ')') {
                if (*p == '/') { while (*p && *p != ')') ++p; break; }
                custom += *p++;
            }
            if (*p == ')') ++p;
            for (int i = 0; i < count; ++i)
                result += custom + "\n";
            continue;
        }

        // check for two-character diagonal (NE NW SE SW)
        char c1 = (char)toupper((unsigned char)*p);
        char c2 = (*(p+1)) ? (char)toupper((unsigned char)*(p+1)) : 0;
        if ((c1=='N'||c1=='S') && (c2=='E'||c2=='W')) {
            char diag[3] = { (char)tolower((unsigned char)c1),
                              (char)tolower((unsigned char)c2), 0 };
            p += 2;
            for (int i = 0; i < count; ++i) { result += diag; result += "\n"; }
            continue;
        }

        // single-letter cardinal direction
        bool found = false;
        for (auto& d : kDirs) {
            if (d.code == (char)tolower((unsigned char)*p)) {
                ++p;
                for (int i = 0; i < count; ++i) {
                    result += d.cmd; result += "\n";
                }
                found = true;
                break;
            }
        }
        if (!found) {
            CString err;
            err.Format("*Invalid direction '%c' in speed walk\n", *p);
            return err;
        }
    }

    return result;
}

// ── CMUSHclientDoc::DoReverseSpeedwalk ─────────────────────────────────────
// Portable reverse-speedwalk.  Walks the input string, reverses each
// direction step, and accumulates the result in reverse order so the
// resulting path is the exact inverse of the original.
//
// Direction reversal table (mirrors Mapping.cpp LoadMapDirections):
//   n↔s  e↔w  u↔d  ne↔sw  nw↔se  f→f
//
// On error, returns a string starting with '*' (MakeSpeedWalkErrorString
// convention).

CString CMUSHclientDoc::DoReverseSpeedwalk(LPCTSTR SpeedWalkString)
{
    // Returns the reverse of a single direction abbreviation, or the
    // input unchanged if it is not a known abbreviation.
    auto reverseDir = [](const std::string& d) -> std::string {
        static const char* tbl[][2] = {
            {"n","s"},{"s","n"},{"e","w"},{"w","e"},
            {"u","d"},{"d","u"},
            {"ne","sw"},{"sw","ne"},{"nw","se"},{"se","nw"},
            {"f","f"},{nullptr,nullptr}
        };
        for (int i = 0; tbl[i][0]; ++i)
            if (d == tbl[i][0]) return tbl[i][1];
        return d;
    };

    CString strResult;
    const char* p = SpeedWalkString;

    while (*p) {
        // Preserve and prepend whitespace
        while (isspace((unsigned char)*p)) {
            if (*p == '\r') { ++p; continue; }
            strResult = CString(*p) + strResult;
            ++p;
        }
        if (!*p) break;

        // Preserve comments  { ... }
        if (*p == '{') {
            CString comment;
            comment += *p++;
            while (*p && *p != '}')
                comment += *p++;
            if (*p != '}')
                return CString("*Comment code of '{' not terminated by a '}'");
            comment += *p++;   // consume '}'
            strResult = comment + strResult;
            continue;
        }

        // Optional numeric counter
        int count = 0;
        while (isdigit((unsigned char)*p))
            count = count * 10 + (*p++ - '0');
        if (count == 0) count = 1;
        if (count > 99)
            return CString("*Speed walk counter exceeds 99");

        // Skip spaces after counter
        while (isspace((unsigned char)*p)) ++p;

        if (count > 1 && *p == 0)
            return CString("*Speed walk counter not followed by an action");
        if (count > 1 && *p == '{')
            return CString("*Speed walk counter may not be followed by a comment");
        if (!*p) break;

        // Optional action code: C O L K
        CString strAction;
        if (strchr("COLKcolk", *p)) {
            if (count > 1)
                return CString("*Action code of C, O, L or K must not follow a speed walk count");
            strAction = CString(*p++);
            while (isspace((unsigned char)*p)) ++p;
            if (!*p || toupper(*p) == 'F' || *p == '{')
                return CString("*Action code of C, O, L or K must be followed by a direction");
        }

        // Direction
        CString dir;
        if (*p == '(') {
            // Special compound direction  (forward/reverse)
            ++p;
            std::string inner;
            while (*p && *p != ')')
                inner += (char)tolower((unsigned char)*p++);
            if (*p != ')')
                return CString("*Action code of '(' not terminated by a ')'");
            ++p;   // consume ')'

            size_t slash = inner.find('/');
            if (slash == std::string::npos) {
                // No slash — look up and swap
                dir = CString(("(" + reverseDir(inner) + ")").c_str());
            } else {
                // Swap the two halves
                std::string left  = inner.substr(0, slash);
                std::string right = inner.substr(slash + 1);
                dir = CString(("(" + right + "/" + left + ")").c_str());
            }
        } else {
            switch (toupper((unsigned char)*p)) {
            case 'N': case 'S': case 'E': case 'W':
            case 'U': case 'D': case 'F': {
                std::string key(1, (char)tolower((unsigned char)*p));
                dir = CString(reverseDir(key).c_str());
                ++p;
                break;
            }
            default:
                return CString(TFormat("*Invalid direction '%c' in speed walk", *p));
            }
        }

        // Prepend (count + action + reversed-direction) to result
        if (count > 1)
            strResult = CString(TFormat("%d", count)) + strAction + dir + strResult;
        else
            strResult = strAction + dir + strResult;
    }

    return strResult;
}

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

    // Echo sent text to the output window
    if (bEchoIt && !m_bNoEcho)
        DisplayMsg(((const char*)strText + string("\n")).c_str(),
                   (int)strlen((const char*)strText) + 1, NOTE_OR_COMMAND);

    // Send to MUD
    CString toSend = strText + CString(ENDLINE);
    SendPacket((const char*)toSend, toSend.GetLength());

    ++m_nTotalLinesSent;
}

// ── CMUSHclientDoc::SendTo ────────────────────────────────────────────────
// Routes a trigger/alias response to the appropriate destination.
// Enum values from OtherTypes.h (included via doc_stub.h):
//   0=World, 1=Command, 2=Output, 3=Status, 4=Notepad, 6=LogFile,
//   8=CommandQueue, 9=Variable, 10=Execute, 12=Script …
void CMUSHclientDoc::SendTo(const unsigned short iWhere,
                             const CString strSendText,
                             const bool bOmitFromOutput,
                             const bool bOmitFromLog,
                             const CString /*strDescription*/,
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
        // m_fn_set_command returns false without overwriting if the bar is
        // non-empty, matching the Windows CSendView iteration behaviour.
        if (m_fn_set_command)
            m_fn_set_command(std::string((const char*)strSendText));
        break;

    case eSendToOutput:       // 2 — display in output window without sending
        if (!bOmitFromOutput)
            Note(strSendText);
        break;

    case eSendToStatus:       // 3 — update status message
        m_strStatusMessage = strSendText;
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

    default:
        // eSendToNotepad(4), eSendToLogFile(6), eSendToPane(15), etc.
        // Not implemented on non-Windows.
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

// ── CMUSHclientDoc::ShiftTabCompleteItem ─────────────────────────────────────
// Lua API: world.ShiftTabCompleteItem(item)
// Adds/removes items from the Shift+Tab completion set, or sets the
// "show functions" flag.  The mushcore build needs this because
// lua_world_api_portable.cpp calls it; the implementation mirrors
// scripting/methods/methods_commands.cpp exactly.
long CMUSHclientDoc::ShiftTabCompleteItem(LPCTSTR Item)
{
    if (strlen(Item) <= 0 || strlen(Item) > 30)
        return eBadParameter;

    if (strcmp(Item, "<clear>") == 0)
        m_ExtraShiftTabCompleteItems.clear();
    else if (strcmp(Item, "<functions>") == 0)
        m_bTabCompleteFunctions = true;
    else if (strcmp(Item, "<nofunctions>") == 0)
        m_bTabCompleteFunctions = false;
    else {
        if (!IsValidName(Item))
            return eBadParameter;
        m_ExtraShiftTabCompleteItems.insert(Item);
    }
    return eOK;
}

#endif // !_WIN32
