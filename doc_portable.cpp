// doc_portable.cpp — genuine platform glue for the headless (mushcore) build.
//
// The hand-maintained reimplementations that once lived here are gone
// (campaigns B+C): mushcore compiles the real shipping TUs.  What remains is
// code with no Windows counterpart — the App/Frame globals, the portable
// world-load/save entry points over the real XML machinery, POSIX-side
// helpers, and small inert hooks for view-side effects.
//
// Guard: excluded from the Windows build entirely.

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "platform/wx_mfc_hooks.h"
#include "scripting/errors.h"   // eOK / eInvalidObjectLabel (self-sufficiency
                                // for the real-header flip; no-op today —
                                // doc_stub.h already includes it)
#include "UDPsocket.h"   // ~CMUSHclientDoc frees m_UDPsocketMap (doc_construct.cpp:549)
#include "Color.h"
#include "speedwalk.h"   // shared ExpandSpeedwalk / ReverseSpeedwalkString
#include "scripting/shs.h"   // SHA-1 (GetUniqueID)
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

// ── CMUSHclientDoc::Load_Set ──────────────────────────────────────────────
// Headless: there is no file dialog, so an EMPTY name behaves like the GUI
// dialog-cancel path (evaluate.cpp:586-660 / mushapp_stubs.cpp Load_Set both
// return TRUE on cancel).  A SUPPLIED name loads the set directly through the
// REAL archive-based loader — the post-dialog half of mushapp_stubs.cpp
// Load_Set verbatim-in-spirit (replace=TRUE: with no prompt possible we take
// the dialogs' default answer).  Reachable via OnFileReloaddefaults when the
// stub App's m_strDefault*File members are set (empty by default).
BOOL CMUSHclientDoc::Load_Set(const int set_type, CString strFileName,
                              CWnd* /*parent_window*/)
{
    if (strFileName.IsEmpty())
        return TRUE;   // TRUE = cancelled, matching the GUI cancel path

    const BOOL replace = TRUE;
    CFile*    f  = nullptr;
    CArchive* ar = nullptr;
    try {
        f  = new CFile(strFileName, CFile::modeRead | CFile::shareDenyWrite);
        ar = new CArchive(f, CArchive::load);

        if (IsArchiveXML(*ar)) {
            switch (set_type) {
            case TRIGGER:
                if (replace) DELETE_MAP(m_TriggerMap, CTrigger);
                Load_World_XML(*ar, XML_TRIGGERS | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case ALIAS:
                if (replace) DELETE_MAP(m_AliasMap, CAlias);
                Load_World_XML(*ar, XML_ALIASES  | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case COLOUR:
                Load_World_XML(*ar, XML_COLOURS  | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case MACRO:
                Load_World_XML(*ar, XML_MACROS   | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            case TIMER:
                if (replace) DELETE_MAP(m_TimerMap, CTimer);
                Load_World_XML(*ar, XML_TIMERS   | XML_NO_PLUGINS | XML_IMPORT_MAIN_FILE_ONLY);
                break;
            }
        } else {
            fprintf(stderr, "[set load] \"%s\" does not have a valid MUSHclient XML signature\n",
                    (const char*)strFileName);
            AfxThrowArchiveException(CArchiveException::badSchema);
        }
    }
    catch (CException* e) { e->Delete(); }
    catch (...)           { }

    delete ar;
    delete f;
    SetModifiedFlag(TRUE);
    return FALSE;   // FALSE = OK (matches Windows convention)
}

// ── App global (CMUSHclientApp) ───────────────────────────────────────────
CMUSHclientApp App;

// ── Frame stub global ─────────────────────────────────────────────────────
CMainFrame g_stub_frame;

// ── ENDLINE constant ──────────────────────────────────────────────────────
const char ENDLINE[] = "\r\n";

// OptionsTable[] is defined in scriptingoptions.cpp (the REAL table; option convergence).

// ── xterm 256-colour table ────────────────────────────────────────────────
COLORREF xterm_256_colours[256];

// ── GetUniqueID (Windows body: Utilities.cpp:412) ─────────────────────────
// SHA-1 a fresh GUID (32 uppercase hex chars, dashes removed — the exact
// input shape of Windows' ::Replace(GetGUID(), "-", "")) and keep the first
// PLUGIN_UNIQUE_ID_LENGTH/8 32-bit digest words → 24 lowercase hex chars,
// byte-for-byte the output shape of the CoCreateGuid-based original.
// Randomness from /dev/urandom (same source the portable utils.CreateGUID
// uses), with a time/counter fallback so two IDs can never collide even if
// the device is unreadable.
CString GetUniqueID (void)
{
    unsigned char r[16] = {};
    FILE* u = fopen("/dev/urandom", "rb");
    bool ok = false;
    if (u) {
        ok = fread(r, 1, sizeof r, u) == sizeof r;
        fclose(u);
    }
    if (!ok) {
        static unsigned long s_counter = 0;
        unsigned long t = (unsigned long) time(nullptr);
        unsigned long c = ++s_counter;
        memcpy(r, &t, sizeof t);
        memcpy(r + sizeof t, &c, sizeof c);
    }

    char raw[33];
    snprintf(raw, sizeof raw,
             "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             r[0], r[1], r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);

    SHS_INFO shsInfo;
    shsInit   (&shsInfo);
    shsUpdate (&shsInfo, (unsigned char *) raw, 32);
    shsFinal  (&shsInfo);

    CString strResult;
    for (int i = 0; i < (PLUGIN_UNIQUE_ID_LENGTH / 8); i++)
        strResult += CFormat ("%08x", shsInfo.digest [i]);

    return strResult;
} // end of GetUniqueID


// ── CMUSHclientDoc constructor ────────────────────────────────────────────
CMUSHclientDoc::CMUSHclientDoc()
{
    static bool s_tableReady = false;
    if (!s_tableReady) { Generate256colours(); s_tableReady = true; }

    // ── Option machinery (mirrors doc_construct.cpp:35-51 order exactly:
    // AllocateConfigurationArrays → SetDefaults → SetAlphaDefaults →
    // m_strWorldID = GetUniqueID()).  bDoSpecial=false as on Windows: no
    // view fix-ups during construction.
    AllocateConfigurationArrays ();

    SetDefaults (false);        // set up numeric/boolean defaults
    SetAlphaDefaults (false);   // set up alpha defaults

    m_strWorldID = GetUniqueID ();      // default world ID

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

    // m_strTerminalIdentification ("mushclient") and m_strLanguage ("lua")
    // now come from SetAlphaDefaults above — the same table defaults Windows
    // applies.  (The old hand-set "MUSHCLIENT" was twin drift: Windows sends
    // the lowercase table default in TTYPE negotiation.)

    // World-start clocks (doc_construct.cpp:32-33) — GetInfo(306) and
    // GetLineInfo(13) read these.
    m_whenWorldStarted = CTime::GetCurrentTime();
    QueryPerformanceCounter (&m_whenWorldStartedHighPrecision);

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
    App.m_pWorldDocTemplate->AddDocument(this);
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
    App.m_pWorldDocTemplate->RemoveDocument(this);

    // Close socket first so no callbacks fire into a partially-destroyed doc.
    TeardownTransport();   // PAL transport + events bridge (doc_shared.cpp)

    // Delete plugins (the REAL CPlugin::~CPlugin() — plugins.cpp — fires
    // OnPluginClose and SaveState for each; nothing here pre-fires them).
    // Erase from list during iteration so re-entrant PluginList queries see a
    // consistent list (mirrors the Windows destructor in doc_construct.cpp).
    for (auto it = m_PluginList.begin(); it != m_PluginList.end();
         it = m_PluginList.erase(it))
        delete *it;

    // UDP listening sockets (mirrors doc_construct.cpp:549).
    for (map<int, UDPsocket *>::iterator udpSocketIterator = m_UDPsocketMap.begin ();
         udpSocketIterator != m_UDPsocketMap.end ();
         udpSocketIterator++)
        delete udpSocketIterator->second;
    m_UDPsocketMap.clear ();

    // Tear down the world's own Lua script engine.
    delete m_ScriptEngine;
    m_ScriptEngine = nullptr;

    // Don't need to know what the configuration was any more
    // (mirrors doc_construct.cpp:645).
    DeleteConfigurationArrays ();

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
// The doc.h:2299 MEMBER (portable body of CMUSHclientDoc::CheckObjectName,
// doc.cpp:2247 — was a free-function twin until Stage 1 pre-flip hygiene):
//   1. Trim leading/trailing whitespace.
//   2. If bConform: validate with CheckLabel — return eInvalidObjectLabel on fail.
//   3. Lowercase the name (keys in the trigger/alias/timer maps are lowercase).
// Returns eOK on success.

// CheckLabel: first char must be alpha; remaining chars alpha/digit/underscore.
// Returns TRUE if the label is INVALID (matching doc.cpp convention).

long CMUSHclientDoc::CheckObjectName(CString& strName, const bool bConform)
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
// The MUSHclient working directory (defined in MUSHclient.cpp for the GUI
// builds; empty headless, so %E expands to "" and relative paths stay relative).
char working_dir [_MAX_PATH] = "";

// The file-browsing directory (MUSHclient.cpp:71 on the GUI builds; tracked
// by the file dialogs there).  Headless nothing browses, so it stays empty —
// GetInfo(84) returns "".
char file_browsing_dir [_MAX_PATH] = "";

// ── GetGUID (Windows body: Utilities.cpp:396, CoCreateGuid-based) ─────────
// Same output shape: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", uppercase hex.
// Randomness from /dev/urandom with a time/counter fallback (the same
// scheme GetUniqueID above uses); version/variant bits set as a v4 GUID,
// which is what CoCreateGuid produces.
CString GetGUID (void)
{
    unsigned char r[16] = {};
    FILE* u = fopen("/dev/urandom", "rb");
    bool ok = false;
    if (u) {
        ok = fread(r, 1, sizeof r, u) == sizeof r;
        fclose(u);
    }
    if (!ok) {
        static unsigned long s_counter = 0;
        unsigned long t = (unsigned long) time(nullptr);
        unsigned long c = ++s_counter;
        memcpy(r, &t, sizeof t);
        memcpy(r + sizeof t, &c, sizeof c);
    }
    r[6] = (unsigned char)((r[6] & 0x0F) | 0x40);   // version 4
    r[8] = (unsigned char)((r[8] & 0x3F) | 0x80);   // RFC 4122 variant

    CString strResult;
    strResult.Format ("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                      r[0], r[1], r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
                      r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    return strResult;
}   // end of GetGUID

// The ON_PLUGIN_* callback-name constants are now defined in plugins.cpp
// (compiled into mushcore since the CPlugin convergence) — the partial
// duplicate set that used to live here is gone.


// FindBaseOption (and the other five table-generic base-option functions):
// single-sourced in doc_shared.cpp — the real bodies (trim+lowercase name
// matching, eOK/eUnknownOption returns) replaced the sentinel-drifted stub.

// ── Plugin callback dispatch ──────────────────────────────────────────────
// The SendToAllPluginCallbacks family, SendToFirstPluginCallbacks and
// SendToAllPluginCallbacksRtn are now the REAL bodies in plugins.cpp
// (CPlugin convergence) — the hand-written GetLuaDispid/ExecuteLua twins that
// lived here are deleted.  Dispatch now goes through each plugin's
// m_PluginCallbacks map (populated by the portable loader, mirroring
// xml/xml_load_world.cpp:534) and CPlugin::ExecutePluginScript.

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

// Parity with doc.cpp:459 SeeIfHandlerCanExecute: a handler name starting
// with '!' is executed as a command instead (and does not call the script);
// any other (or empty) name lets the normal dispid dispatch proceed.  The
// old stub returned false unconditionally, so the world connect/disconnect/
// open/close script handlers never ran headless (twin drift).
bool CMUSHclientDoc::SeeIfHandlerCanExecute (const CString & strName)
  {
  if (!strName.IsEmpty () && strName [0] == '!')
    {
    Execute (strName.Mid (1));
    return false;
    }

  return true;  // go ahead and call normal script
  } // end of SeeIfHandlerCanExecute

// ── CMUSHclientDoc::GetChildFrame ────────────────────────────────────────
// The real body (scripting/methods/methods_output.cpp, not yet adopted)
// iterates the doc's views for a CSendView and returns its owning
// CChildFrame.  Headless GetFirstViewPosition() is always null, so the walk
// would find nothing — return what it would compute.  Used by the real
// methods_commands.cpp SetCommandWindowHeight (null → eBadParameter).
CChildFrame * CMUSHclientDoc::GetChildFrame (void)
{
    return nullptr;
}

// ── CMUSHclientDoc::GetLinePosition ──────────────────────────────────────
// BYTE-COPY of doc.cpp:1974-2027 (the JUMP_SIZE line-position lookup used
// by GetLineInfo/GetStyleInfo/Bookmark).  Remove when doc.cpp adopts.
POSITION CMUSHclientDoc::GetLinePosition (long nLine)
  {
POSITION pos = NULL;

  // sanity check  (Santa Claus)
  if (nLine > m_LineList.GetCount ())
    nLine = m_LineList.GetCount ();

  if (nLine < 0)
    nLine = 0;

static long iCount = 0;
int nIndex = nLine / JUMP_SIZE,
    nItem = nIndex * JUMP_SIZE;

  if (m_pLinePositions)
    {
    pos = m_pLinePositions [nIndex];

  // not there, try previous item

    if (!pos && nIndex > 0)
      {
      pos = m_pLinePositions [nIndex - 1];
      nItem -= JUMP_SIZE;
      }
    }   // end of having a positions array

// as a last resort, use the start of the list

  if (!pos)
    {
    pos = m_LineList.GetHeadPosition ();
    nItem = 0;
    }

// now count forwards until we hit the actual one we want

  for (long i = nItem; i < nLine && pos; i++)
    {
    iCount++;
    m_LineList.GetNext (pos);
    }

// not there? assume last one in the list

  if (!pos)
    pos = m_LineList.GetTailPosition ();

// return the desired position

  return pos;

  } // end of CMUSHclientDoc::GetLinePosition

// ── windowPositionHelper ─────────────────────────────────────────────────
// BYTE-COPY of scripting/methods/methods_output.cpp:802-810.  Remove when
// methods_output.cpp adopts.
void windowPositionHelper (CString & str, const RECT & r)
  {
  str.Format ("%ld,%ld,%ld,%ld",
              r.left,
              r.top,
              r.right - r.left,  // Width
              r.bottom - r.top);  // Height
  } // end of windowPositionHelper

// ── CMUSHclientDoc::GetWorldWindowPositionX ──────────────────────────────
// COPY of the non-Windows arm of methods_output.cpp:175-212 (pure IWorldView
// seam: the world's one child frame rect, Which is always 1).  Remove when
// methods_output.cpp adopts.
BSTR CMUSHclientDoc::GetWorldWindowPositionX(short Which)
{
CString strResult;

  // each world has exactly one MDI child frame here; Which is always 1
  int x = 0, y = 0, w = 800, h = 600;
  if (Which == 1 && GetOutputRect (x, y, w, h))
    strResult.Format ("%d,%d,%d,%d", x, y, w, h);

	return strResult.AllocSysString();
}   // end of CMUSHclientDoc::GetWorldWindowPositionX

// ── CMUSHclientApp::GetGlobalOption / GetGlobalOptionList ────────────────
// REAL bodies in globalregistryoptions.cpp (adopted into mushcore): the
// offsetof-driven global-option tables walk the stub App's
// MUSHclient.h-exact members, so headless returns real values for the full
// Windows option-name surface.

// ── GetSelection (stdafx.h:484; Windows body Utilities.cpp) ──────────────
// Returns the selected text of a command-window CEdit (true = whole text).
// Headless there is never a CEdit (SpellCheckCommand's view walk finds
// nothing and bails before this), so the body is unreachable — an inert
// definition satisfies the linker for the adopted methods_spellchecker.cpp.
class CEdit;
bool GetSelection (CEdit * /*pEdit*/, CString & strSelection)
{
    strSelection.Empty();
    return true;
}

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

// ── CMUSHclientDoc::FindAllEntryPoints ───────────────────────────────────
// Real body: scripting/scripting.cpp:169-234 — GetProcedureDispid over the
// current trigger/alias/timer maps plus FindGlobalEntryPoints (world +
// MXP callbacks) when not inside a plugin, then a CMissingEntryPoints
// dialog if anything was unresolved.  The REAL loader calls it after
// parsing a plugin's script (xml/xml_load_world.cpp:527).  Headless the
// Lua-only ResolveAllEntryPoints covers the same ground (same plugin/world
// split, same maps); there is no dialog, and false = "no errors" so the
// loader never throws "Could not find all required script routines".
bool CMUSHclientDoc::FindAllEntryPoints(void)
{
    ResolveAllEntryPoints();
    return false;
}

// ── CMUSHclientDoc::LoadWorldFromFile ────────────────────────────────────
// Port-only public entry (the test suite's world-load path).  Thin driver
// over the REAL loader (xml/xml_load_world.cpp): it does exactly what the
// Windows Serialize loading branch does (serialize.cpp:44-110), then the
// OpenSession scripting steps (doc.cpp:473-497) that the GUI performs right
// after a world opens.  Returns true on success.

extern tConfigurationNumericOption OptionsTable [];   // scriptingoptions.cpp
extern tConfigurationAlphaOption AlphaOptionsTable [];   // scriptingoptions.cpp
                                 // (per-TU externs — the Windows convention,
                                 // cf. world_debug.cpp:24-25, serialize.cpp:26-27)

// Port equivalent of CMUSHclientDoc::CreateScriptEngine (scripting/
// scripting.cpp:20-105): create the world's script engine, read the external
// script file (script_filename) into m_strScript, parse it, and resolve all
// entry points.  Headless deviations, both deliberate:
//   - Lua only (the other five engines are embedded on the mushgui side);
//   - the engine is only created when a script FILE is configured — Windows
//     creates one whenever m_bEnableScripts is on, but headless consumers
//     (tests) treat engine-presence as script-presence.
static void PortCreateWorldScriptEngine(CMUSHclientDoc& doc)
{
    if (doc.m_strScriptFilename.IsEmpty())
        return;                      // nothing to run (see deviation note)

    CString strLanguage = doc.m_strLanguage;
    strLanguage.MakeLower();
    if (strLanguage.IsEmpty())
        strLanguage = "lua";         // table default (scriptingoptions_alpha.inc:60)
    if (strLanguage != "lua") {
        fprintf(stderr, "[world load] scripting language \"%s\" is not supported "
                        "headless — script not loaded\n",
                (const char*)doc.m_strLanguage);
        return;
    }

    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    if (engine->CreateScriptEngine()) {
        fprintf(stderr, "[world load] could not initialise Lua engine\n");
        delete engine;
        return;
    }
    doc.m_ScriptEngine = engine;     // scripting.cpp:33 (set before parsing)

    // Read the script file (scripting.cpp:40-76) and remember its mtime so
    // OnScriptFileChanged doesn't immediately see a "change".
    FILE* f = fopen((const char*)doc.m_strScriptFilename, "rb");
    if (!f) {
        fprintf(stderr, "[world load] cannot open script file \"%s\": %s\n",
                (const char*)doc.m_strScriptFilename, strerror(errno));
        // Windows shows "unable to open" and disables scripting; keep the
        // (empty) engine so the failure is visible but harmless.
    } else {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string buf;
        if (sz > 0) {
            buf.resize((size_t)sz);
            size_t got = fread(&buf[0], 1, (size_t)sz, f);
            buf.resize(got);
        }
        fclose(f);
        doc.m_strScript = CString(buf.c_str());

        struct stat st;
        if (::stat((const char*)doc.m_strScriptFilename, &st) == 0)
            doc.m_scriptFileMtime = st.st_mtime;

        if (engine->Parse(doc.m_strScript, "Script file")) {     // scripting.cpp:78
            fprintf(stderr, "[world load] error parsing script \"%s\"\n",
                    (const char*)doc.m_strScriptFilename);
            delete engine;                       // DisableScripting equivalent
            doc.m_ScriptEngine = nullptr;
            return;
        }
    }

    doc.FindAllEntryPoints();                    // scripting.cpp:102
}

BOOL CMUSHclientDoc::LoadWorldFromFile(const char* path)
{
    CFile f;
    if (!f.Open(path, CFile::modeRead | CFile::shareDenyWrite)) {
        fprintf(stderr, "[world load] cannot open \"%s\"\n", path);
        return false;
    }
    CArchive ar(&f, CArchive::load);

    // serialize.cpp:47-52 — status message + remember the document path
    Frame.SetStatusMessageNow(TFormat("Opening world \"%s\"", path));
    SetPathName(ar.GetFile()->GetFilePath());

    // serialize.cpp:54-61 — XML signature check (TMessageBox + throw there)
    if (!IsArchiveXML(ar)) {
        fprintf(stderr, "[world load] \"%s\" does not have a valid MUSHclient "
                        "XML signature\n", path);
        return false;
    }

    try {
        // The Serialize_World_XML loading branch (xml/xml_serialize.cpp:52-55):
        // load everything except plugins-as-main-content
        Load_World_XML(ar, (unsigned long) ~(XML_PLUGINS | XML_NO_PLUGINS |
                                             XML_PASTE_DUPLICATE |
                                             XML_IMPORT_MAIN_FILE_ONLY));
        m_bLoaded = true;   // this world has been loaded from disk
    }
    catch (CException* e) {   // CArchiveException from Load_World_XML's handler
        e->Delete();
        return false;
    }
    catch (...) {
        return false;
    }

    // serialize.cpp:63-68 — custom-16-as-default-colour
    if (m_bCustom16isDefaultColour) {
        m_iForeColour = 15;   // use custom colour 16
        m_iBackColour = 15;
        m_iFlags |= COLOUR_CUSTOM;
    }

    // serialize.cpp:70-72 — if defaults are wanted, overwrite with them
    OnFileReloaddefaults();

    // serialize.cpp:74-92 — recompile the mapping-failure regexp
    if (m_bMapFailureRegexp && !m_strMappingFailure.IsEmpty()) {
        try {
            m_MapFailureRegexp = regcomp(m_strMappingFailure,
                                         (m_bUTF_8 ? PCRE_UTF8 : 0));
        }
        catch (CException* e) {
            char sMessage[1000];
            e->GetErrorMessage(sMessage, sizeof sMessage);
            fprintf(stderr, "[world load] error \"%s\" processing mapping "
                            "failure regular expression \"%s\"\n",
                    sMessage, (const char*)m_strMappingFailure);
            e->Delete();
            m_bMapFailureRegexp = FALSE;
            m_MapFailureRegexp = NULL;
        }
    }

    // serialize.cpp:94-96 — kick off the speed-walk timer
    if (m_pTimerWnd)
        m_pTimerWnd->ChangeTimerRate(m_iSpeedWalkDelay);

    // serialize.cpp:98-108 — remember loaded option values (GetLoadedValue)
    {
        long i;
        for (i = 0; OptionsTable[i].pName; i++)
            if (!m_NumericConfiguration[i]->bInclude)
                m_NumericConfiguration[i]->iValue = GetOptionItem(i);
        for (i = 0; AlphaOptionsTable[i].pName; i++)
            if (!m_AlphaConfiguration[i]->bInclude)
                m_AlphaConfiguration[i]->sValue = GetAlphaOptionItem(i);
    }

    // ── OpenSession scripting steps (doc.cpp:473-497) ──────────────────
    // On Windows the view-creation path runs OpenSession right after the
    // document loads; headless there is no view, so do it here.
    if (m_bEnableScripts &&      // provided wanted (doc.cpp:475)
        !m_ScriptEngine)         // and not already going
        PortCreateWorldScriptEngine(*this);

    // execute "open" script (doc.cpp:480-497)
    if (m_ScriptEngine) {
        if (SeeIfHandlerCanExecute(m_strWorldOpen)) {
            DISPPARAMS params = { NULL, NULL, 0, 0 };
            long nInvocationCount = 0;
            ExecuteScript(m_dispidWorldOpen,
                          m_strWorldOpen,
                          eWorldAction,
                          "world open",
                          "opening world",
                          params,
                          nInvocationCount);
        }
    }
    // (doc.cpp:493-495 auto-connect is a GUI concern — not done here)

    return true;
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

// StringToVector is single-sourced in utilities_shared.cpp (Windows body;
// the old twin's include_delim semantics are gone).

// ── CVariable IMPLEMENT_DYNAMIC (needed by DECLARE_DYNAMIC) ──────────────
IMPLEMENT_DYNAMIC(CVariable, CObject)

// The remaining ON_PLUGIN_* callback-name constants also come from plugins.cpp
// now (CPlugin convergence) — the second partial duplicate set is gone too.

// AlphaOptionsTable[] is defined in scriptingoptions.cpp (the REAL table; option convergence).

// tolower(string) is single-sourced in utilities_shared.cpp.


// ── ThrowErrorException ───────────────────────────────────────────────────
// REAL body now compiled into mushcore (exceptions.cpp): it throws a
// CErrorException* (a CException*), which the real loader's per-item
// catch (CException*) handlers (xml/xml_load_world.cpp) rely on to turn a
// bad trigger/alias/timer into a warning instead of aborting the whole
// load.  The old twin here threw std::runtime_error — invisible to every
// catch (CException*) site (regexp errors, xmlparse entity errors, ...).




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

// ── RemoveTrailingBackslash — real body (mirrors mushapp_stubs.cpp; the
// Windows original is inside #ifdef _WIN32 in dialogs/global_prefs/
// GlobalPrefs.cpp).  Reachable headless via CPlugin::SaveState (plugins.cpp).
void RemoveTrailingBackslash(CString& str)
{
    str.TrimLeft();
    str.TrimRight();
    if (!str.IsEmpty()) {
        char last = str[str.GetLength() - 1];
        if (last == '\\' || last == '/')
            str = str.Left(str.GetLength() - 1);
    }
}

// The old inert Save_XML_* / Save_*_XML no-ops and the always-false
// IsArchiveXML stub are gone: the REAL bodies are compiled into mushcore
// from xml/xml_save_world.cpp and xml/xml_serialize.cpp (world-file saver
// adoption) — the stubs here would be duplicate symbols.

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

// Trace/TraceOut/Get,SetTrace: the REAL scripting/methods/methods_tracing.cpp
// is compiled into mushcore now (Campaign C file #2 after methods_noting).

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

// FormatTime: single-sourced in doc_shared.cpp (the real body — %E/%N/%P/%F/%L
// substitutions + bFixHTML semantics — replaced the plain-strftime twin whose
// third parameter was misnamed bUTC).

// ── Speedwalk (shared) ─────────────────────────────────────────────────────
// The speedwalk parser lives in the shared speedwalk.cpp (compiled into both
// mushcore and mushapp) so it cannot drift from the Windows original. These thin
// wrappers inject the portable built-in n/s/e/w/u/d direction tables — the
// GUI-only MapDirectionsMap is not populated in the portable/CLI build. The line

// ── CMUSHclientDoc::CheckConnected ────────────────────────────────────────
// MUSHclient convention: returns false if connected, true if not (doc.h:1852
// bool).  evaluate.cpp checks "if (CheckConnected()) return true;" to bail early.
bool CMUSHclientDoc::CheckConnected()
{
    return m_iConnectPhase != eConnectConnectedToMud;
}

// SendMsg / DoSendMsg are single-sourced in doc_shared.cpp — the real send
// family (queueing, spam prevention, OnPluginSend/Sent ordering, IAC
// doubling, logging) replaced the simplified twin here.

// SendTo: single-sourced in doc_shared.cpp (the real doc.cpp body — empty-
// text early-return, caller-displayed eSendToOutput accumulation, Execute-
// based eSendToExecute with log-flag save/restore, DoSendMsg eSendImmediate —
// replaced the simplified twin here).

// ── CMUSHclientDoc::ExecuteTriggerScript ─────────────────────────────────
// Fires the Lua script callback for a trigger match.
#include "paneline.h"
// ExecuteTriggerScript: single-sourced in doc_shared.cpp (the real body —
// all-engine UsesEmbeddedCallback guard + scripting-inactive warning — replaced
// the Lua-only twin here).

// TriggerToPane: gone — doc.h:2154-2167 guards it under #ifdef PANE (defined
// in no build here); its only caller and body in ProcessPreviousLine.cpp are
// likewise PANE-guarded, so no stub body is needed.

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

// ColourToName is single-sourced in utilities_shared.cpp (the real
// App.m_ColoursMap reverse lookup; the 16-name twin is gone).

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
