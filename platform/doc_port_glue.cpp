// platform/doc_port_glue.cpp — genuine platform glue for the headless (mushcore)
// build, surviving the B-3 atomic flip that deleted doc_portable.cpp.
//
// What lives here is code with NO Windows counterpart that mushcore compiles:
//   • the port-only world-load/save entry points over the real XML machinery
//     (LoadWorldFromFile / Load_Set), the Lua-only entry-point resolver
//     (ResolveAllEntryPoints / FindAllEntryPoints), the script-file mtime poll
//     (OnScriptFileChanged), and the display-index helper (AllocateLinePositions);
//   • the doc-side GUI methods whose real bodies are in mushapp-only TUs
//     (methods_output.cpp GetChildFrame / GetWorldWindowPositionX /
//     windowPositionHelper, methods_notepad.cpp AppendToTheNotepad);
//   • the Utilities.cpp free functions mushcore does not compile (GetUniqueID,
//     GetGUID, TFormat, TranslateGeneric, MakeRandomNumber, AdjustColour,
//     GetSelection, SetDefaultAnsiColours/CustomColours) + the MUSHclient.cpp
//     globals (working_dir / file_browsing_dir / xterm_256_colours / g_debug);
//   • POSIX-side no-ops (clipboard/dir navigation, RemoveTrailingBackslash).
//
// Build: compiled into mushcore ONLY.  Like windows_stubs.cpp / doc_headless_-
// stubs.cpp it compiles against the REAL headers (MUSHCLIENT_FULL_BUILD), so
// these bodies bind the doc.h-declared methods and the real free-function decls.
//
// NOT here (now provided by the adopted real TUs, deleting the old twins):
//   • the ctor/dtor            → doc_construct.cpp (with mushcore-only line-seed
//                                 + world-doc registration arms)
//   • CheckObjectName, SeeIfHandlerCanExecute, CheckConnected, Debug_Packets,
//     GetLinePosition, PlaySoundFile, CancelSound, GetClipboardContents,
//     putontoclipboard, IMPLEMENT_DYNAMIC(CVariable)  → doc.cpp
//   • ENDLINE (doc.h:85), GetScriptEngine (doc.h:2637 inline)  → doc.h
//   • the App global           → platform/headless_app.cpp
//   • g_stub_frame / dbg / telopt_name / g_before/after_output → deleted with
//     the retired headless stub (unreferenced post-flip; telnet_phases.cpp has
//     its own locals).

#ifndef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"                 // Frame.SetStatusMessageNow (LoadWorldFromFile)
#include "scripting/scripting.h"     // CScriptEngine
#include "scripting/errors.h"        // eOK / eInvalidObjectLabel / eWorldAction
#include "scripting/shs.h"           // SHS_INFO / shsInit (GetUniqueID / MakeRandomNumber)
#include "UDPsocket.h"
#include "Color.h"                   // CColor (AdjustColour)
#include "speedwalk.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <climits>
#include <cerrno>
#include <ctime>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

// ── MUSHclient.cpp globals (mushcore never compiles MUSHclient.cpp) ──────────
bool     g_debug = false;                  // doc.h:34 extern
char     working_dir       [_MAX_PATH] = "";
char     file_browsing_dir [_MAX_PATH] = "";
COLORREF xterm_256_colours [256];          // filled by Generate256colours (utilities_shared.cpp)

// ════════════════════════════════════════════════════════════════════════════
// Utilities.cpp free functions (mushcore does not compile Utilities.cpp)
// ════════════════════════════════════════════════════════════════════════════

// GetUniqueID (Windows body Utilities.cpp): SHA-1 a fresh GUID → 24 lowercase
// hex chars, byte-for-byte the shape of the CoCreateGuid-based original.
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

// GetGUID (Windows body Utilities.cpp, CoCreateGuid-based): v4 GUID, uppercase.
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

// TFormat / TranslateGeneric (Windows body Utilities.cpp): the headless port
// has no message catalogue, so TranslateGeneric returns the text unchanged.
CString TFormat(const char* sFormat, ...)
{
    // Headless there is no Lua translator (App.m_Translator_Lua == NULL), so the
    // real Utilities.cpp TFormat degrades to a plain format — but it MUST go
    // through CString's FormatV, which rewrites the MSVC %I64d length modifier
    // (mfc_string.h:296).  A raw vsnprintf mis-parses %I64d and misaligns the
    // varargs (crash: e.g. Debug_Packets' "%I64d …%s" read a number as a char*).
    va_list ap;
    va_start(ap, sFormat);
    CString result = CFormat(sFormat, ap);
    va_end(ap);
    return result;
}

CString TranslateGeneric(const char* sText, const char*)
{
    return CString(sText);
}

// MakeRandomNumber (Windows body Utilities.cpp): hash world name, rand, pid,
// time and the doc pointer for entropy (Pueblo challenge / chat nonces).
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

// AdjustColour (Windows body Utilities.cpp): the world.AdjustColour operations.
long AdjustColour(long Colour, short Method)
{
    COLORREF c = (COLORREF)(Colour & 0xFFFFFF);
    CColor clr;
    clr.SetColor(c);
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

// GetSelection (stdafx.h; Windows body Utilities.cpp): the selected text of a
// command-window CEdit.  Headless there is never a CEdit, so it is unreachable
// (SpellCheckCommand's view walk bails before this) — an inert def satisfies the
// adopted methods_spellchecker.cpp.
class CEdit;
bool GetSelection (CEdit * /*pEdit*/, CString & strSelection)
{
    strSelection.Empty();
    return true;
}

// SetDefaultAnsiColours / SetDefaultCustomColours (Windows body Utilities.cpp:
// 1117 / 1138): the ANSI + curated custom colour defaults.  doc_construct.cpp's
// real ctor calls these (doc_construct.cpp:313/316); mushcore does not compile
// Utilities.cpp, so the bodies live here (byte-copy of the Windows originals).
void SetDefaultAnsiColours (COLORREF * normalcolour, COLORREF * boldcolour)
  {
  normalcolour [BLACK]   = RGB (0, 0, 0);
  normalcolour [RED]     = RGB (128, 0, 0);
  normalcolour [GREEN]   = RGB (0, 128, 0);
  normalcolour [YELLOW]  = RGB (128, 128, 0);
  normalcolour [BLUE]    = RGB (0, 0, 128);
  normalcolour [MAGENTA] = RGB (128, 0, 128);
  normalcolour [CYAN]    = RGB (0, 128, 128);
  normalcolour [WHITE]   = RGB (192, 192, 192);

  boldcolour  [BLACK]    = RGB (128, 128, 128);
  boldcolour  [RED]      = RGB (255, 0, 0);
  boldcolour  [GREEN]    = RGB (0, 255, 0);
  boldcolour  [YELLOW]   = RGB (255, 255, 0);
  boldcolour  [BLUE]     = RGB (0, 0, 255);
  boldcolour  [MAGENTA]  = RGB (255, 0, 255);
  boldcolour  [CYAN]     = RGB (0, 255, 255);
  boldcolour  [WHITE]    = RGB (255, 255, 255);
  }   // SetDefaultAnsiColours

void SetDefaultCustomColours (COLORREF * customtext, COLORREF * customback)
  {
  for (int i = 0; i < MAX_CUSTOM; i++)
    {
    customtext [i] = RGB (255, 255, 255);   // white on black
    customback [i] = RGB (0, 0, 0);
    }

  // set up some nice defaults
  customtext [0] = RGB (255, 128, 128);
  customtext [1] = RGB (255, 255, 128);
  customtext [2] = RGB (128, 255, 128);
  customtext [3] = RGB (128, 255, 255);
  customtext [4] = RGB (0, 128, 255);
  customtext [5] = RGB (255, 128, 192);
  customtext [6] = RGB (255, 0, 0);
  customtext [7] = RGB (0, 128, 192);
  customtext [8] = RGB (255, 0, 255);
  customtext [9] = RGB (128, 64, 64);
  customtext [10] = RGB (255, 128, 64);
  customtext [11] = RGB (0, 128, 128);
  customtext [12] = RGB (0, 64, 128);
  customtext [13] = RGB (255, 0, 128);
  customtext [14] = RGB (0, 128, 0);
  customtext [15] = RGB (0, 0, 255);
  } // end of SetDefaultCustomColours

// ── POSIX-side no-ops ───────────────────────────────────────────────────────
void ChangeToFileBrowsingDirectory() {}
void ChangeToStartupDirectory()      {}

// RemoveTrailingBackslash (Windows original is inside #ifdef _WIN32 in
// dialogs/global_prefs/GlobalPrefs.cpp; reachable headless via CPlugin::SaveState).
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

// ════════════════════════════════════════════════════════════════════════════
// Doc-side GUI methods whose real bodies live in mushapp-only TUs
// ════════════════════════════════════════════════════════════════════════════

// AllocateLinePositions — the headless equivalent of the deferred allocation in
// doc.cpp SetUpOutputWindow; (re)builds the JUMP_SIZE index from m_maxlines.
// Called from doc_construct.cpp's mushcore-only ctor arm and by the world loader.
void CMUSHclientDoc::AllocateLinePositions()
{
    // Trim FIRST so the index can never be smaller than the list it indexes.
    if (m_pLinePositions)
        while ((long)m_LineList.GetCount() > m_maxlines)
            RemoveChunk();

    delete [] m_pLinePositions;
    m_pLinePositions = new POSITION[(m_maxlines / JUMP_SIZE) + 1];
    for (long i = 0; i <= m_maxlines / JUMP_SIZE; i++)
        m_pLinePositions[i] = NULL;
    m_pLinePositions[0] = m_LineList.GetHeadPosition();
}

// AppendToTheNotepad — real body methods_notepad.cpp (mushapp-only).  Headless
// route through the wx hook, else create a text window (mfc_ui.h inline).
bool CMUSHclientDoc::AppendToTheNotepad(const CString strTitle,
                                         const CString strText,
                                         const bool bReplace,
                                         const int  iNotepadType)
{
    if (g_wxhook_append_to_notepad)
    {
        const char* title = static_cast<const char*>(strTitle);
        const char* text  = static_cast<const char*>(strText);
        if (g_wxhook_append_to_notepad(title, text, bReplace))
            return true;
    }

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

// GetChildFrame — real body methods_output.cpp (mushapp-only).  Headless there
// are no views, so the view walk finds nothing → nullptr (methods_commands.cpp
// SetCommandWindowHeight then returns eBadParameter).
CChildFrame * CMUSHclientDoc::GetChildFrame (void)
{
    return nullptr;
}

// windowPositionHelper — byte-copy of methods_output.cpp (mushapp-only).
void windowPositionHelper (CString & str, const RECT & r)
  {
  str.Format ("%ld,%ld,%ld,%ld",
              r.left,
              r.top,
              r.right - r.left,  // Width
              r.bottom - r.top);  // Height
  } // end of windowPositionHelper

// GetWorldWindowPositionX — non-Windows arm of methods_output.cpp (mushapp-only):
// the world's one child-frame rect via the IWorldView seam (Which is always 1).
BSTR CMUSHclientDoc::GetWorldWindowPositionX(short Which)
{
CString strResult;

  int x = 0, y = 0, w = 800, h = 600;
  if (Which == 1 && GetOutputRect (x, y, w, h))
    strResult.Format ("%d,%d,%d,%d", x, y, w, h);

	return strResult.AllocSysString();
}   // end of CMUSHclientDoc::GetWorldWindowPositionX

// ════════════════════════════════════════════════════════════════════════════
// Port-only scripting / world-load entry points over the real machinery
// ════════════════════════════════════════════════════════════════════════════

// ResolveAllEntryPoints — the Lua-only headless equivalent of
// CMUSHclientDoc::FindAllEntryPoints (scripting/scripting.cpp, mushapp-only):
// resolve each trigger/alias/timer strProcedure dispid + the world/MXP callbacks
// (only when not inside a plugin), matching the scripting.cpp plugin/world split.
void CMUSHclientDoc::ResolveAllEntryPoints()
{
    CScriptEngine* engine = GetScriptEngine();
    if (!engine || !engine->IsLua())
        return;

    auto resolve = [&](const CString& name) -> DISPID {
        if (name.IsEmpty()) return DISPID_UNKNOWN;
        return engine->GetLuaDispid(name);
    };

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

// FindAllEntryPoints — real body scripting/scripting.cpp:169 (mushapp-only).
// Headless the Lua-only ResolveAllEntryPoints covers the same ground; false =
// "no errors" so the loader never throws "Could not find all required routines".
bool CMUSHclientDoc::FindAllEntryPoints(void)
{
    ResolveAllEntryPoints();
    return false;
}

// Option tables (scriptingoptions.cpp) — per-TU externs (Windows convention).
extern tConfigurationNumericOption OptionsTable [];
extern tConfigurationAlphaOption   AlphaOptionsTable [];

// Port equivalent of CMUSHclientDoc::CreateScriptEngine (scripting.cpp): create
// the world's Lua engine, read + parse the external script file, resolve entry
// points.  Headless deviations (both deliberate): Lua only; the engine is created
// only when a script FILE is configured.
static void PortCreateWorldScriptEngine(CMUSHclientDoc& doc)
{
    if (doc.m_strScriptFilename.IsEmpty())
        return;                      // nothing to run (see deviation note)

    CString strLanguage = doc.m_strLanguage;
    strLanguage.MakeLower();
    if (strLanguage.IsEmpty())
        strLanguage = "lua";         // table default
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
    doc.m_ScriptEngine = engine;     // scripting.cpp (set before parsing)

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

        if (engine->Parse(doc.m_strScript, "Script file")) {
            fprintf(stderr, "[world load] error parsing script \"%s\"\n",
                    (const char*)doc.m_strScriptFilename);
            delete engine;                       // DisableScripting equivalent
            doc.m_ScriptEngine = nullptr;
            return;
        }
    }

    doc.FindAllEntryPoints();
}

// LoadWorldFromFile — port-only public world-load entry (the test suite path).
// Thin driver over the REAL loader (xml/xml_load_world.cpp): the Windows
// Serialize loading branch (serialize.cpp) then the OpenSession scripting steps
// (doc.cpp) the GUI performs after a world opens.  Returns true on success.
BOOL CMUSHclientDoc::LoadWorldFromFile(const char* path)
{
    CFile f;
    if (!f.Open(path, CFile::modeRead | CFile::shareDenyWrite)) {
        fprintf(stderr, "[world load] cannot open \"%s\"\n", path);
        return false;
    }
    CArchive ar(&f, CArchive::load);

    Frame.SetStatusMessageNow(TFormat("Opening world \"%s\"", path));
    SetPathName(ar.GetFile()->GetFilePath());

    if (!IsArchiveXML(ar)) {
        fprintf(stderr, "[world load] \"%s\" does not have a valid MUSHclient "
                        "XML signature\n", path);
        return false;
    }

    try {
        Load_World_XML(ar, (unsigned long) ~(XML_PLUGINS | XML_NO_PLUGINS |
                                             XML_PASTE_DUPLICATE |
                                             XML_IMPORT_MAIN_FILE_ONLY));
        m_bLoaded = true;
    }
    catch (CException* e) {
        e->Delete();
        return false;
    }
    catch (...) {
        return false;
    }

    if (m_bCustom16isDefaultColour) {
        m_iForeColour = 15;   // use custom colour 16
        m_iBackColour = 15;
        m_iFlags |= COLOUR_CUSTOM;
    }

    OnFileReloaddefaults();

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

    if (m_pTimerWnd)
        m_pTimerWnd->ChangeTimerRate(m_iSpeedWalkDelay);

    {
        long i;
        for (i = 0; OptionsTable[i].pName; i++)
            if (!m_NumericConfiguration[i]->bInclude)
                m_NumericConfiguration[i]->iValue = GetOptionItem(i);
        for (i = 0; AlphaOptionsTable[i].pName; i++)
            if (!m_AlphaConfiguration[i]->bInclude)
                m_AlphaConfiguration[i]->sValue = GetAlphaOptionItem(i);
    }

    // ── OpenSession scripting steps (doc.cpp) ──────────────────────────────
    if (m_bEnableScripts &&      // provided wanted
        !m_ScriptEngine)         // and not already going
        PortCreateWorldScriptEngine(*this);

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

    return true;
}

// Load_Set — headless world-set loader.  An EMPTY name behaves like the GUI
// dialog-cancel (returns TRUE); a supplied name loads the set through the REAL
// archive loader (replace=TRUE, the dialog's default answer with no prompt).
// The Windows body (evaluate.cpp:586) is #ifdef _WIN32; mushapp gets its own from
// mushapp_stubs.cpp.
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

// OnScriptFileChanged — port script-file auto-reload (polls m_scriptFileMtime).
// Real body scripting/scripting.cpp (mushapp-only, GUI-prompt based); headless
// always reloads (eReloadAlways), so bForce is ignored.
void CMUSHclientDoc::OnScriptFileChanged(const bool /*bForce*/)
{
    if (m_bInScriptFileChanged)
        return;

    if (m_strScriptFilename.IsEmpty())
        return;

    struct stat st;
    if (::stat((const char*)m_strScriptFilename, &st) != 0)
        return;  // file disappeared or unreadable — leave engine as-is

    if (st.st_mtime == m_scriptFileMtime)
        return;  // unchanged

    m_bInScriptFileChanged = true;

    fprintf(stderr, "[script] \"%s\" changed — reloading\n",
            (const char*)m_strScriptFilename);

    delete m_ScriptEngine;
    m_ScriptEngine = nullptr;

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

    m_scriptFileMtime = st.st_mtime;

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

// ── Lua function stubs (only when Lua is NOT compiled in) ────────────────────
// ProcessPreviousLine.cpp builds a Lua table for OnPluginLineReceived; without
// liblua5.1 these stubs let the TU link.  With Lua the real library supplies
// them (hence the guard) — mushcore normally has Lua, so this is dead.
#ifndef MUSHCLIENT_LUA_SCRIPTING

struct lua_State;

extern "C" {
void lua_createtable(lua_State*, int, int) {}
void lua_rawseti    (lua_State*, int, int) {}
void lua_setfield   (lua_State*, int, const char*) {}
}

void MakeTableItem(lua_State*, const char*, const std::string&) {}
void MakeTableItem(lua_State*, const char*, double) {}

#endif // !MUSHCLIENT_LUA_SCRIPTING

#endif // !_WIN32
