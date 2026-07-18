#pragma once
// platform/doc_stub.h — minimal CMUSHclientDoc substitute for non-Windows builds.
//
// Provides only the types, constants, and class members required by the protocol
// files (ansi.cpp, telnet_phases.cpp, mxp/*.cpp, …).
// Keep Windows builds unaffected: this header must never be included on Windows.
//
// On Windows those files include the real doc.h (via stdafx.h / MUSHclient.h).

#ifndef _WIN32

#include "platform/compat.h"   // CString, CObject, COLORREF, BOOL, etc.
#include "Color.h"             // CColor (ANSI 24-bit colour support)
#include "format.h"            // CFormat : CString with printf constructors
#include <string>
#include <vector>
#include <list>
#include <deque>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <set>
#include <functional>
#include <zlib.h>              // z_stream, Bytef — for compression fields
#include <netinet/in.h>        // sockaddr_in / SOCKADDR_IN for proxy fields

// ── PAL byte-transport seam (ITransport; mirrors doc.h) ───────────────────
namespace pal { class ITransport; }   // pal/itransport.h
class CDocTransportEvents;            // doc_shared.cpp — ITransportEvents → engine bridge

// ── loaded-configuration snapshot classes (doc.h:593-620, verbatim) ────────
class CNumericConfiguration : public CObject
  {
  public:

    long iValue;    // loaded value
    bool bInclude;  // true if from include file

    void Reset (void)  { iValue = 0; bInclude = false; }
    CNumericConfiguration () { Reset (); }

  }; // end of  CConfiguration

typedef CTypedPtrArray <CPtrArray, CNumericConfiguration*> CNumericConfigurationArray;

class CAlphaConfiguration : public CObject
  {
  public:

    CString sValue;   // loaded value
    bool bInclude;    // true if from include file

    void Reset (void)  { sValue.Empty (); bInclude = false; }
    CAlphaConfiguration () { Reset (); }

  }; // end of  CAlphaConfiguration

typedef CTypedPtrArray <CPtrArray, CAlphaConfiguration*> CAlphaConfigurationArray;

#ifndef PLUGIN_UNIQUE_ID_LENGTH
#define PLUGIN_UNIQUE_ID_LENGTH 24    // stdafx.h:532
#endif

class CMUSHclientDoc;

// Deferred TLS consent/fallback prompt message id (stdafx.h owns it in the
// full build: WM_USER + 1004).  Headless there is no message pump, so posts
// go nowhere — TLS failures fail closed, the portable policy.
#ifndef FORUM_URL
#define FORUM_URL "http://www.gammon.com.au/forum/"
#endif

#ifndef WM_USER_SSL_FALLBACK_PROMPT
#define WM_USER_SSL_FALLBACK_PROMPT (0x0400 + 1004)
#endif

// High-resolution counter + async-DNS-cancel shims (mirror mfc_ui.h — some
// mushgui TUs include BOTH headers, hence the shared sentinels; LARGE_INTEGER
// itself comes from windows_types.h).
#include <time.h>
#ifndef MC_QPC_SHIMS_DEFINED
#define MC_QPC_SHIMS_DEFINED
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* li) {
    if (li) { li->QuadPart = 1000000000LL; }  // 1 ns resolution
    return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* li) {
    if (li) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        li->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
    return TRUE;
}
#endif // MC_QPC_SHIMS_DEFINED
#ifndef MAXGETHOSTSTRUCT
#define MAXGETHOSTSTRUCT 1024
#endif
typedef struct sockaddr_in SOCKADDR_IN;

using std::string;
using std::pair;        // methods_arrays.cpp (stdafx.h has `using namespace std`)
using std::make_pair;
using std::vector;
using std::list;
using std::deque;       // methods_speedwalks.cpp RemoveBacktracks
using std::map;
using std::unordered_map;
using std::set;

// ── PCRE internal UTF-8 validator (declared in stdafx.h on Windows) ──────
extern "C" int _pcre_valid_utf(const unsigned char* buf, int length, int* erroroffset);

// ── Colour constants (from stdafx.h) ─────────────────────────────────────
// stdafx.h defines these on all platforms now; guard against redefinition.
#ifndef BLACK
enum { BLACK = 0, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE };
#endif

#define MAX_CUSTOM 16   // maximum custom colours

// ── ANSI codes (from stdafx.h) ────────────────────────────────────────────
#ifndef ESC
#  define ESC '\x1B'   // from doc.h — AnsiNote's escape parser
#endif

// ── Translate* macros (from stdafx.h) — route through TranslateGeneric ────
#ifndef Translate
#  define Translate(string)        TranslateGeneric (string, "messages")
#  define TranslateTime(string)    TranslateGeneric (string, "times")
#  define TranslateHeading(string) TranslateGeneric (string, "headings")
#endif
#define ANSI_RESET              0
#define ANSI_BOLD               1
#define ANSI_BLINK              3
#define ANSI_UNDERLINE          4
#define ANSI_SLOW_BLINK         5
#define ANSI_FAST_BLINK         6
#define ANSI_INVERSE            7
#define ANSI_STRIKEOUT          9
#define ANSI_CANCEL_BOLD       22
#define ANSI_CANCEL_BLINK      23
#define ANSI_CANCEL_UNDERLINE  24
#define ANSI_CANCEL_SLOW_BLINK 25
#define ANSI_CANCEL_INVERSE    27
#define ANSI_CANCEL_STRIKEOUT  29
#define ANSI_TEXT_BLACK        30
#define ANSI_TEXT_RED          31
#define ANSI_TEXT_GREEN        32
#define ANSI_TEXT_YELLOW       33
#define ANSI_TEXT_BLUE         34
#define ANSI_TEXT_MAGENTA      35
#define ANSI_TEXT_CYAN         36
#define ANSI_TEXT_WHITE        37
#define ANSI_TEXT_256_COLOUR   38
#define ANSI_SET_FOREGROUND_DEFAULT 39
#define ANSI_BACK_BLACK        40
#define ANSI_BACK_RED          41
#define ANSI_BACK_GREEN        42
#define ANSI_BACK_YELLOW       43
#define ANSI_BACK_BLUE         44
#define ANSI_BACK_MAGENTA      45
#define ANSI_BACK_CYAN         46
#define ANSI_BACK_WHITE        47
#define ANSI_BACK_256_COLOUR   48
#define ANSI_SET_BACKGROUND_DEFAULT 49

// ── Protocol phase enum (from doc.h) ─────────────────────────────────────
enum {
    NONE,
    HAVE_ESC,
    DOING_CODE,
    HAVE_IAC,
    HAVE_WILL,
    HAVE_WONT,
    HAVE_DO,
    HAVE_DONT,
    HAVE_SB,
    HAVE_SUBNEGOTIATION,
    HAVE_SUBNEGOTIATION_IAC,
    HAVE_COMPRESS,
    HAVE_COMPRESS_WILL,
    HAVE_FOREGROUND_256_START,
    HAVE_FOREGROUND_256_FINISH,
    HAVE_BACKGROUND_256_START,
    HAVE_BACKGROUND_256_FINISH,
    HAVE_FOREGROUND_24B_FINISH,
    HAVE_FOREGROUND_24BR_FINISH,
    HAVE_FOREGROUND_24BG_FINISH,
    HAVE_FOREGROUND_24BB_FINISH,
    HAVE_BACKGROUND_24B_FINISH,
    HAVE_BACKGROUND_24BR_FINISH,
    HAVE_BACKGROUND_24BG_FINISH,
    HAVE_BACKGROUND_24BB_FINISH,
    HAVE_UTF8_CHARACTER,
    HAVE_MXP_ELEMENT,
    HAVE_MXP_COMMENT,
    HAVE_MXP_QUOTE,
    HAVE_MXP_ENTITY,
    HAVE_MXP_ROOM_NAME,
    HAVE_MXP_ROOM_DESCRIPTION,
    HAVE_MXP_ROOM_EXITS,
    HAVE_MXP_WELCOME,
    HAVE_MXP_COLLECT_SPECIAL,
    HAVE_OSC,           // OSC sequence: ESC ] ... BEL/ST — discard silently
};

// ── Style / action flags (from OtherTypes.h) ─────────────────────────────
#define COLOUR_ANSI      0x0000
#define COLOUR_CUSTOM    0x0100
#define COLOUR_RGB       0x0200
#define COLOUR_RESERVED  0x0300
#define ACTION_NONE      0x0000
#define ACTION_SEND      0x0400
#define ACTION_HYPERLINK 0x0800
#define ACTION_PROMPT    0x0C00
#define NORMAL           0x0000
#define HILITE           0x0001
#define UNDERLINE        0x0002
#define BLINK            0x0004
#define INVERSE          0x0008
#define CHANGED          0x0010
#define STRIKEOUT        0x0020
#define COLOURTYPE       0x0300
#define ACTIONTYPE       0x0C00
#define STYLE_BITS       0x0FFF
#define START_TAG        0x1000
#define TEXT_STYLE       0x002F

// ── Telnet protocol constants (from doc.h) ────────────────────────────────
#define COMPRESS_BUFFER_LENGTH 10000   // size of decompression input buffer

enum {
    WILL_END_OF_RECORD      = 0x19, // see RFC 885
    EOR                = 0xEF, // see RFC 885
    SE                 = 0xF0, // end of subnegotiation
    NOP                = 0xF1, // no operation
    DATA_MARK          = 0xF2, // see RFC 854
    BREAK              = 0xF3, // BRK
    INTERRUPT_PROCESS  = 0xF4, // IP
    ABORT_OUTPUT       = 0xF5, // AO
    ARE_YOU_THERE      = 0xF6, // AYT
    ERASE_CHARACTER    = 0xF7, // EC
    ERASE_LINE         = 0xF8, // EL
    GO_AHEAD           = 0xF9, // GA
    SB                 = 0xFA, // subnegotiation
    WILL               = 0xFB,
    WONT               = 0xFC,
    DO                 = 0xFD,
    DONT               = 0xFE,
    IAC                = 0xFF
};

#define TELOPT_ECHO          1
#define TELOPT_NAWS         31   // Negotiate About Window Size
#define TELOPT_TERMINAL_TYPE 24  // terminal type
#define TELOPT_CHARSET       42  // character set
#define TELOPT_COMPRESS      85  // MCCP v1
#define TELOPT_COMPRESS2     86  // MCCP v2
#define TELOPT_MUD_SPECIFIC  102 // Aardwolf / mud-specific
#define SGA                  3   // suppress go-ahead
#define TELOPT_MSP           90  // MUD Sound Protocol
#define TELOPT_MXP           91  // MUD Extension Protocol
#define TELOPT_ZMP           93
#define TELOPT_ATCP          200

// ── MXP mode enum (from doc.h) ────────────────────────────────────────────
enum {
    eMXP_open        = 0,
    eMXP_secure      = 1,
    eMXP_locked      = 2,
    eMXP_reset       = 3,  // mode 3 = reset (used in sMode array in mxpMode.cpp)
    eMXP_secure_once = 4,
    eMXP_perm_open   = 5,
    eMXP_perm_secure = 6,
    eMXP_perm_locked = 7
};

// ── iUseMXP values — MUST match doc.h exactly. The shared engine files
// (telnet_phases.cpp, ProcessPreviousLine.cpp, …) compiled into mushcore compare
// m_iUseMXP against these names, and world files / the OptionsTable use 0-3, so a
// divergent set silently breaks MXP-mode logic and saves out-of-range values.
enum {
    eOnCommandMXP = 0,   // turn on after the MXP telnet sub-negotiation
    eQueryMXP     = 1,   // turn on after option query
    eUseMXP       = 2,   // always on
    eNoMXP        = 3,   // always off
};

// ── MXP debug level enum (from doc.h) ─────────────────────────────────────
enum {
    DBG_ERROR   = 1,
    DBG_WARNING = 2,
    DBG_INFO    = 3,
    DBG_ALL     = 4
};

// ── MXP list modes (from doc.h) ───────────────────────────────────────────
enum {
    eNoList,
    eOrderedList,
    eUnorderedList
};

// ── Notepad types (from doc.h) ────────────────────────────────────────────
enum {
    eNotepadNormal      = 0,
    eNotepadMXPdebug    = 1,
    eNotepadTrigger     = 2,
    eNotepadRecall      = 3,
    eNotepadScript      = 4,
    eNotepadPacketDebug = 5,
    eNotepadLineInfo    = 6
};

// ── eWorldAction (from doc.h) ─────────────────────────────────────────────
enum { eWorldAction = 9 };

// ── ENDLINE constant (from doc.h) ─────────────────────────────────────────
#ifndef ENDLINE
extern const char ENDLINE[];
#endif

// ── NUMITEMS macro ────────────────────────────────────────────────────────
#ifndef NUMITEMS
#  define NUMITEMS(arg) ((unsigned int)(sizeof(arg) / sizeof(arg[0])))
#endif

// ── MIN / MAX macros (MFC provides these on Windows) ─────────────────────
#ifndef MIN
#  define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#  define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

// ── DELETE_MAP / DELETE_LIST macros (from stdafx.h) ───────────────────────
#ifndef DELETE_MAP
#  define DELETE_MAP(mapname, pointertype)  \
   do {                                     \
     pointertype * ptr_;                    \
     CString key_;                          \
     CTypedPtrList<CPtrList, pointertype*> tmp_; \
     POSITION pos_;                         \
     for (pos_ = mapname.GetStartPosition(); pos_; ) { \
       mapname.GetNextAssoc(pos_, key_, ptr_); \
       tmp_.AddTail(ptr_);                  \
     }                                      \
     mapname.RemoveAll();                   \
     for (pos_ = tmp_.GetHeadPosition(); pos_; ) \
       delete tmp_.GetNext(pos_);           \
     tmp_.RemoveAll();                      \
   } while (false)
#endif

#ifndef DELETE_LIST
#  define DELETE_LIST(listname) \
   do {                         \
     while (!listname.IsEmpty())  \
       delete listname.RemoveHead(); \
   } while (false)
#endif

#ifndef MFC_UI_H_INCLUDED
// ── COleDateTime stub (mfc_ui.h provides a fuller version) ───────────────
struct COleDateTime {
    double m_dt = 0.0;
    COleDateTime() = default;
    COleDateTime(double d) : m_dt(d) {}
};
#endif // !MFC_UI_H_INCLUDED

// ── COM interface stubs (private members of CScriptEngine; never used on macOS)
struct IActiveScript       {};
struct IActiveScriptParse  {};
struct CActiveScriptSite   {};
struct IDispatch           {};
// HRESULT is typedef'd in windows_types.h as LONG (int32_t)

// ── DISPID / COM stubs ────────────────────────────────────────────────────
// DISPID is already typedef'd in platform/windows_types.h as LONG
#ifndef DISPID_UNKNOWN
#  define DISPID_UNKNOWN  (-1)
#endif

// VARTYPE tag constants (used by COleVariant::vt)
// Guard: mfc_ui.h defines these as #define macros; if that header was included
// first, skip the enum to avoid "expected identifier before numeric constant".
#ifndef VT_EMPTY
enum { VT_EMPTY = 0, VT_I4 = 3, VT_BSTR = 8, VT_VARIANT = 12 };
#endif

#ifndef MFC_UI_H_INCLUDED
// Minimal COleVariant stub — only needs to compile; all uses are inside
// DISPID_UNKNOWN guards which evaluate to false in the stub build.
struct COleVariant {
    unsigned short vt = VT_EMPTY;
    union {
        long lVal = 0;
        double dblVal;
    };
    COleVariant() = default;
    COleVariant(const char* s)    { vt = VT_BSTR; lVal = 0; (void)s; }
    COleVariant(long v)           { vt = VT_I4;   lVal = v; }
    COleVariant(double v)         { vt = VT_I4;   lVal = (long)v; }
    COleVariant(const CString& s) { vt = VT_BSTR; lVal = 0; (void)s; }
    COleVariant& operator=(const char* s)    { vt = VT_BSTR; lVal = 0; (void)s; return *this; }
    COleVariant& operator=(long v)           { vt = VT_I4;   lVal = v;          return *this; }
    COleVariant& operator=(const CString& s) { vt = VT_BSTR; lVal = 0; (void)s; return *this; }
    void ChangeType(unsigned short t) { vt = t; }
};
typedef COleVariant VARIANTARG;
#endif // !MFC_UI_H_INCLUDED

#ifndef MFC_UI_H_INCLUDED
// DISPPARAMS stub — fields accessed as { rgvarg, rgdispidNamedArgs, cArgs, cNamedArgs }
struct DISPPARAMS {
    VARIANTARG* rgvarg          = nullptr;
    DISPID*     rgdispidNamedArgs = nullptr;
    unsigned int cArgs          = 0;
    unsigned int cNamedArgs     = 0;
};
#endif // !MFC_UI_H_INCLUDED

#ifndef MFC_UI_H_INCLUDED
// COleSafeArray stub (only used inside IsLua() guards which never execute)
struct COleSafeArray {
    void CreateOneDim(unsigned short, unsigned long, const void* = nullptr,
                      long = 0) {}
    void PutElement(long*, void*)  {}
    operator COleVariant() const { return COleVariant(); }
};
#endif // !MFC_UI_H_INCLUDED

// ── tConfigurationAlphaOption (from doc.h) ────────────────────────────────
// Used by serialize.cpp for XML world-settings serialisation.
struct tConfigurationAlphaOption {
    char* pName    = nullptr;   // setting name (e.g. "server")
    char* sDefault = nullptr;   // original default value
    int   iOffset  = 0;         // byte offset inside CMUSHclientDoc
    int   iFlags   = 0;         // OPT_* flags
};
// Option flags
#ifndef OPT_MULTLINE
#  define OPT_MULTLINE      0x000001
#  define OPT_KEEP_SPACES   0x000002
#  define OPT_PASSWORD      0x000004
#  define OPT_COMMAND_STACK 0x000008
#  define OPT_WORLD_ID      0x000010
#endif

// ── Numeric option flags (from doc.h) ─────────────────────────────────────
#ifndef OPT_CUSTOM_COLOUR
#  define OPT_CUSTOM_COLOUR        0x000001
#  define OPT_RGB_COLOUR           0x000002
#  define OPT_DOUBLE               0x000004
#  define OPT_UPDATE_VIEWS         0x000100
#  define OPT_UPDATE_INPUT_FONT    0x000200
#  define OPT_UPDATE_OUTPUT_FONT   0x000400
#  define OPT_FIX_OUTPUT_BUFFER    0x000800
#  define OPT_FIX_WRAP_COLUMN      0x001000
#  define OPT_FIX_SPEEDWALK_DELAY  0x002000
#  define OPT_USE_MXP              0x004000
#  define OPT_PLUGIN_CANNOT_READ   0x100000
#  define OPT_PLUGIN_CANNOT_WRITE  0x200000
#  define OPT_PLUGIN_CANNOT_RW     0x300000
#  define OPT_CANNOT_WRITE         0x400000
#  define OPT_SERVER_CAN_WRITE     0x800000
#  define OPT_FIX_INPUT_WRAP      0x1000000
#  define OPT_FIX_TOOLTIP_VISIBLE 0x2000000
#  define OPT_FIX_TOOLTIP_START   0x4000000
#endif

// ── Chat port default (from stdafx.h) ────────────────────────────────────
#ifndef DEFAULT_CHAT_PORT
#  define DEFAULT_CHAT_PORT 4050
#endif

extern tConfigurationAlphaOption AlphaOptionsTable[];

// ── tConfigurationNumericOption stub (from doc.h) ─────────────────────────
struct tConfigurationNumericOption {
    char*   pName   = nullptr;  // char* (not const) to match MFC usage patterns
    double  iDefault    = 0;
    int     iOffset     = 0;
    int     iLength     = 0;
    double  iMinimum    = 0;
    double  iMaximum    = 0;
    int     iFlags      = 0;
};

// ── Error / result codes (from scripting/errors.h) ───────────────────────
// Include unconditionally; the file is pure-enum, no extra deps.
#include "scripting/errors.h"

// ── Connection type enum (from doc.h) ─────────────────────────────────────
enum { eConnectNormal = 0, eConnectMXP = 3 };

// ── Auto-connect method enum (from doc.h) ────────────────────────────────
// Values must match doc.h exactly (they are stored in world files).
enum {
    eNoAutoConnect  = 0,   // no auto-login
    eConnectMUSH    = 1,   // send "connect <name> <password>"
    eConnectDiku    = 2,   // send <name> CR <password> CR
    // eConnectMXP  = 3,   // already defined above
    eConnectTypeMax = 4,   // sentinel — must be last
};

// ── Proxy server type enum (from doc.h) ──────────────────────────────────
enum {
    eProxyServerNone  = 0,
    eProxyServerSocks4 = 1,
    eProxyServerSocks5 = 2,
    eProxyServerHTTP  = 3,
    eProxyServerLast  = 4,   // sentinel
};

// ── Script-reload option enum (from doc.h) ───────────────────────────────
enum {
    eReloadNever   = 0,
    eReloadConfirm = 1,
    eReloadAlways  = 2,
};

// ── MXP debug level enum additions (from doc.h) ──────────────────────────
// DBG_NONE is the zero level — already implied by the MXP debug enum but
// spelled out here for scriptingoptions.cpp (stub build).
#ifndef DBG_NONE
#  define DBG_NONE 0
#endif

// ── Line width / font constants ───────────────────────────────────────────
#ifndef MAX_LINE_WIDTH
#  define MAX_LINE_WIDTH 500
#endif

// ── Connection phase enum (from doc.h) — tracks state machine for m_iConnectPhase
// Values must match doc.h exactly so that mushapp and mushcore share world files.
enum {
    eConnectNotConnected            = 0,
    eConnectMudNameLookup           = 1,
    eConnectProxyNameLookup         = 2,
    eConnectConnectingToMud         = 3,
    eConnectConnectingToProxy       = 4,
    eConnectAwaitingProxyResponse1  = 5,  // sent SOCKS method, awaiting confirmation
    eConnectAwaitingProxyResponse2  = 6,  // sent SOCKS user/pass, awaiting confirmation
    eConnectAwaitingProxyResponse3  = 7,  // sent SOCKS CONNECT, awaiting confirmation
    eConnectAwaitingSSLHandshake    = 8,
    eConnectConnectedToMud          = 9,  // fully established, MUD data flowing
    eConnectDisconnecting           = 10,
    // Legacy aliases used in some portable paths
    eConnectConnecting              = eConnectConnectingToMud,
    eConnectConnected               = eConnectConnectedToMud,
};

// ── Trigger evaluation control (from doc.h) ───────────────────────────────
enum {
    eKeepEvaluatingTriggers              = 0,
    eStopEvaluatingTriggers              = 1,
    eStopEvaluatingTriggersInAllPlugins  = 2
};

// ── CChatSocket status (from chatsock.h) ──────────────────────────────────
enum {
    eChatNotConnected = 0,
    eChatConnecting   = 1,
    eChatConnected    = 2
};

// ── Recent-lines buffer size (from doc.h) ─────────────────────────────────
#ifndef MAX_RECENT_LINES
#  define MAX_RECENT_LINES 200
#endif

// OPT_SERVER_CAN_WRITE is defined above in the OPT_CUSTOM_COLOUR block.

// ── Version string (from stdafx.h) ────────────────────────────────────────
#ifndef MUSHCLIENT_VERSION
#  define MUSHCLIENT_VERSION "5.07"
#endif

// ── Sound sentinel (from stdafx.h) ────────────────────────────────────────
#ifndef NOSOUNDLIT
#  define NOSOUNDLIT "(No sound)"
#endif

// ── Colour sentinels (from stdafx.h) ──────────────────────────────────────
#ifndef NO_COLOUR
#  define NO_COLOUR  0xFFFFFFFF
#endif
#ifndef SAMECOLOUR
#  define SAMECOLOUR 65535
#endif

// ── Pueblo start-of-HTML marker (from doc.h) ──────────────────────────────
#ifndef PUEBLO_START
#  define PUEBLO_START "</xch_mudtext>"
#endif

// ── Command queue/echo flags (from doc.h) — used by SendMsg/DoSendMsg ─────
#ifndef QUEUE_WITH_ECHO
#  define QUEUE_WITH_ECHO               'E'
#  define QUEUE_WITHOUT_ECHO            'N'
#  define IMMEDIATE_WITH_ECHO           'I'
#  define IMMEDIATE_WITHOUT_ECHO        'W'
#  define QUEUE_WITH_ECHO_NOLOG         'e'
#  define QUEUE_WITHOUT_ECHO_NOLOG      'n'
#  define IMMEDIATE_WITH_ECHO_NOLOG     'i'
#  define IMMEDIATE_WITHOUT_ECHO_NOLOG  'w'
#endif

// ── Line-scrolling jump size (from doc.h) ─────────────────────────────────
#ifndef JUMP_SIZE
#  define JUMP_SIZE 100
#endif

// ── Pueblo handshake strings (from doc.h) ─────────────────────────────────
#ifndef PUEBLO_ID_STRING1
#  define PUEBLO_ID_STRING1 "This world is Pueblo 1.10 enhanced."
#  define PUEBLO_ID_STRING2 "This world is Pueblo 1.0 Enhanced"
#  define PUEBLO_ID_STRING3 "This world is Pueblo 2.50 enhanced."
#  define PUEBLO_REPLY1     "PUEBLOCLIENT %s md5=\"%s\""
#  define PUEBLO_REPLY2     "PUEBLOCLIENT"
#endif

// ── Chat snoop message type (from chatsock.h) ─────────────────────────────
#ifndef CHAT_SNOOP_DATA
#  define CHAT_SNOOP_DATA 31
#endif

// ── Custom colour sentinel (from stdafx.h) ────────────────────────────────
// OTHER_CUSTOM (16) = "other" custom colour slot used in trigger colour matching.
#ifndef OTHER_CUSTOM
#  define OTHER_CUSTOM 16
#endif

// ── MXP version string (from mxp.h or stdafx.h) ──────────────────────────
#ifndef MXP_VERSION
#  define MXP_VERSION "0.4"
#endif

// ── Spell-checker Lua entry point (stdafx.h:720) ──────────────────────────
#ifndef SPELLCHECKFUNCTION
#  define SPELLCHECKFUNCTION "spellcheck"
#endif

// ── Info / function / error table row types (doc.h:475-481, stdafx.h:965-972)
// The REAL InfoTypes[] table arrives with methods_info.cpp; the sentinel
// InternalFunctionsTable[] / error_descriptions[] stubs live in
// platform/windows_stubs.cpp (functionlist.cpp / lua_methods.cpp own the
// real ones — both are mushapp-only).
typedef struct { int key; const char * val; } int_flags_pair;
typedef struct
  {
  int           iInfoType;       // which info number it is
  const char *  sDescription;    // eg. "Server name"
  } tInfoTypeMapping;
// stdafx.h owns tInternalFunctionsTable (stdafx.h:965-972); several portable
// TUs (regexp.cpp, Line.cpp, …) include stdafx.h AND doc_stub.h, and two
// typedefs of distinct anonymous structs to one name do not compile.
#ifndef _STDAFX_INCLUDED_
typedef struct
  {
  const char *  sFunction;       // name of function (eg. "WindowShow")
  const char *  sArgs;           // arguments (eg. "(Window, Showflag)")
  } tInternalFunctionsTable;
#endif

// ── ci_less / ci_set (doc.h:499-520) — case-insensitive string set ────────
struct ci_less
  {
  // case-independent (ci) compare_less binary function
  struct nocase_compare
    {
    bool operator() (const unsigned char& c1, const unsigned char& c2) const
      { return tolower (c1) < tolower (c2); }
    };

  bool operator() (const std::string & s1, const std::string & s2) const
    {
    return std::lexicographical_compare
          (s1.begin (), s1.end (),   // source range
           s2.begin (), s2.end (),   // dest range
                nocase_compare ());  // comparison
    }
  }; // end of ci_less

typedef std::set<std::string, ci_less> ci_set;

// ── os_version (stdafx.h:406) ─────────────────────────────────────────────
// GetKeyState / GetACP / GetSysColor / ShellExecute / LPHOSTENT / the
// OSVERSIONINFO type & co all come from platform/mfc_ui.h (included via
// compat.h above) — the SAME hook-based shims the mac product uses, inert
// while the g_wxhook_* pointers are null (headless).  Only the os_version
// GLOBAL needs a mushcore home: MUSHclient.cpp (mushapp) owns the real one,
// platform/windows_stubs.cpp defines the zeroed headless one
// (GetInfo(77) → "", GetInfo(265-268) → 0).
extern OSVERSIONINFO os_version;

// ── Full type definitions from OtherTypes.h ───────────────────────────────
// OtherTypes.h has its own non-Windows preamble; including it here gives us
// the real CAction, CStyle, CLine, CAlias, CTrigger, CTimer, CVariable, etc.
#include "OtherTypes.h"

// ── PLURAL / PLURALES macros (from stdafx.h) ─────────────────────────────
// Each expands to TWO arguments: the count and the appropriate suffix.
// Usage: TFormat("... %i item%s ...", PLURAL(n))
#ifndef PLURAL
#  define PLURAL(n)   (n), ((n) == 1 ? "" : "s")
#  define PLURALES(n) (n), ((n) == 1 ? "" : "es")
#endif

// ── TranslateTime — no-op on macOS (returns format string unchanged) ─────
// Guard: stdafx.h defines TranslateTime as a macro calling TranslateGeneric;
// if that is already in scope, skip our simple inline function.
#ifndef TranslateTime
inline const char* TranslateTime(const char* fmt) { return fmt; }
#endif

// ── SSL forward declarations ──────────────────────────────────────────────
// When OpenSSL is present, <openssl/ssl.h> provides the full definition.
// When absent, we supply a hollow stub so SSL* can be used as a pointer type.
// PLATFORM_OPENSSL_SSL_H_INCLUDED is defined by the fallback stub
// platform/openssl_fallback/openssl/ssl.h (only on the include path when no real
// OpenSSL was found); if it's already there, skip this duplicate block.
#if !defined(OPENSSL_FOUND) && !defined(PLATFORM_OPENSSL_SSL_H_INCLUDED)
struct ssl_st {};
typedef struct ssl_st SSL;
struct ssl_ctx_st {};
typedef struct ssl_ctx_st SSL_CTX;
inline int  SSL_pending      (SSL*)                    { return 0;  }
inline int  SSL_read         (SSL*, void*, int)        { return -1; }
inline int  SSL_write        (SSL*, const void*, int)  { return -1; }
inline int  SSL_get_error    (SSL*, int)               { return 0;  }
inline void SSL_free         (SSL*)                    {}
inline int  SSL_shutdown     (SSL*)                    { return 1;  }
#  ifndef SSL_ERROR_WANT_READ
#    define SSL_ERROR_WANT_READ    2
#  endif
#  ifndef SSL_ERROR_WANT_WRITE
#    define SSL_ERROR_WANT_WRITE   3
#  endif
#  ifndef SSL_ERROR_ZERO_RETURN
#    define SSL_ERROR_ZERO_RETURN  6
#  endif
#elif defined(OPENSSL_FOUND)
// Forward-declare only; the full definition comes from <openssl/ssl.h>
// which is included after this header in worldsock.cpp.
struct ssl_st;
typedef struct ssl_st SSL;
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
#endif
// When PLATFORM_OPENSSL_SSL_H_INCLUDED, SSL types are already defined.

// ── PUEBLO_ACTIVE macro ───────────────────────────────────────────────────
// References m_bPuebloActive member of the current CMUSHclientDoc instance.
#define PUEBLO_ACTIVE (m_bPuebloActive)

// ── CScriptEngine — forward declaration only; full definition in scripting/scripting.h
// which is included at the bottom of this file (after CMUSHclientDoc is defined).
class CScriptEngine;

// ── Debug trace macros (no-ops on macOS) ──────────────────────────────────
#ifndef TRACE
#  define TRACE(msg)       ((void)0)
#  define TRACE1(fmt, ...) ((void)0)
#  define TRACE2(fmt, ...) ((void)0)
#endif

// ── Plugin callback names (from plugins.h) ────────────────────────────────
extern const string ON_PLUGIN_CONNECT;
extern const string ON_PLUGIN_DISCONNECT;
extern const string ON_PLUGIN_LINE_RECEIVED;
extern const string ON_PLUGIN_SEND;
extern const string ON_PLUGIN_PARTIAL_LINE;
extern const string ON_PLUGIN_PACKET_RECEIVED;
extern const string ON_PLUGIN_TRACE;
extern const string ON_PLUGIN_SENT;
extern const string ON_PLUGIN_COMMAND_ENTERED;
extern const string ON_PLUGIN_COMMAND_CHANGED;
extern const string ON_PLUGIN_ENABLE;
extern const string ON_PLUGIN_DISABLE;
extern const string ON_PLUGIN_LIST_CHANGED;
extern const string ON_PLUGIN_TABCOMPLETE;
extern const string ON_PLUGIN_IAC_GA;
extern const string ON_PLUGIN_TELNET_REQUEST;
extern const string ON_PLUGIN_TELNET_OPTION;
extern const string ON_PLUGIN_WORLD_SAVE;
extern const string ON_PLUGIN_CLOSE;
extern const string ON_PLUGIN_TELNET_SUBNEGOTIATION;
extern const string ON_PLUGIN_MXP_START;
extern const string ON_PLUGIN_MXP_STOP;
extern const string ON_PLUGIN_MXP_OPENTAG;
extern const string ON_PLUGIN_MXP_CLOSETAG;
extern const string ON_PLUGIN_MXP_SETVARIABLE;
extern const string ON_PLUGIN_MXP_SETENTITY;
extern const string ON_PLUGIN_MXP_ERROR;

// ── Output hooks (for the former CLI (now removed)) ──────────────
// When readline is active, incoming MUD text must temporarily clear the
// input line before printing, then redisplay it afterwards.
// These were set by the former CLI's readline helpers (CLI now removed; inert).
// doc_portable.cpp calls them around every batch of MUD output / Note().
extern void (*g_before_output)();
extern void (*g_after_output)();

// ── Debug logging ─────────────────────────────────────────────────────────
// (Legacy CLI debug flag; the CLI has been removed.)
// dbg() writes to stderr; all protocol events log here when g_debug is true.
// On Windows these are no-ops (defined as macros so no link dependency).
extern bool g_debug;
void dbg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Human-readable name for a telnet option number.
const char* telopt_name(unsigned char opt);

// ── UMessageBox / Translate / TFormat stubs ───────────────────────────────
// UMessageBox is declared in stdafx.h (with inline fallback for non-Windows).
// Provide it here only if stdafx.h has not been included yet.
#ifndef MB_OK
#define MB_OK              0x00000000L
#endif
#ifndef MB_ICONEXCLAMATION
#define MB_ICONEXCLAMATION 0x00000030L
#endif
CString TFormat(const char* sFormat, ...);
CString GetConnectionStatus (const int iStatus);   // doc_shared.cpp (T3b)

// ── Command-ID name mapping (from stdafx.h; guarded because a few TUs
// include stdafx.h before doc_stub.h and the anonymous-struct typedefs may
// be defined only once per TU).  The tables live in accelerators.cpp and
// the lookups in utilities_shared.cpp — both compiled into mushcore. ──
#ifndef _STDAFX_INCLUDED_
typedef struct
  {
  int						iCommandID;			 // eg. ID_EDIT_SELECT_ALL
  const char *	sCommandName;    // eg. "SelectAll"
  } tCommandIDMapping;

typedef struct
  {
  int						iVirtualKey;     // eg. VK_ESCAPE
  const char *	sKeyName;		     // eg. "Esc"
  } tVirtualKeyMapping;
#endif // _STDAFX_INCLUDED_

CString CommandIDToString (const int ID);   // utilities_shared.cpp
int StringToCommandID (const CString str);  // utilities_shared.cpp
CString ExtractDirectory (CString str);     // utilities_shared.cpp

// Splitter pane indexes (stdafx.h; no USE_EXTRA_PANE on the port)
#ifndef OUTPUT_PANE
#  define OUTPUT_PANE 0
#  define COMMAND_PANE 1
#endif

// MUSHclient.h:29 — sound-buffer count (methods_sounds.cpp)
#ifndef MAX_SOUND_BUFFERS
#  define MAX_SOUND_BUFFERS 10
#endif
CString TranslateGeneric(const char* sText, const char* sSection);
// Don't define Translate as a macro when building with wxWidgets — wx/textbuf.h
// declares wxString::Translate(const wxString& text, const wxString& domain) as a
// 2-arg static method, which the preprocessor mangles if Translate is already a macro.
#if !defined(__WXGTK__) && !defined(__WXOSX__) && !defined(__WXMSW__)
#define Translate(str) TranslateGeneric(str, "messages")
#endif
#ifndef _STDAFX_INCLUDED_  // stdafx.h sets this; if missing, provide the stub
inline int UMessageBox(const char* sText, UINT = 0, const char* = "MUSHclient")
  { fprintf(stderr, "MUSHclient: %s\n", sText ? sText : ""); return 0; }
inline int TMessageBox(const char* sText, UINT = 0)
  { fprintf(stderr, "MUSHclient: %s\n", sText ? sText : ""); return 0; }
#endif

// ── SPACES / trim family + GetWord/StringToVector (from stdafx.h) ─────────
// A few TUs (e.g. regexp.cpp) include stdafx.h AND doc_stub.h; stdafx.h
// always comes first and already provides all of these (SPACES is its
// sentinel), so skip the duplicates — default arguments may be declared
// only once per TU.
#ifndef SPACES
#define SPACES " \t\r\n"
inline string trim_right(const string& s, const string& t = SPACES)
  {
  string d (s);
  string::size_type i (d.find_last_not_of (t));
  if (i == string::npos)
    return "";
  else if ((i + 1) >= s.size ())
    return s;
  else
   return d.erase (i + 1) ;
  }  // end of trim_right

inline string trim_left(const string& s, const string& t = SPACES)
  {
  string d (s);
  return d.erase (0, s.find_first_not_of (t)) ;
  }  // end of trim_left

inline string trim(const string& s, const string& t = SPACES)
  {
  string d (s);
  return trim_left (trim_right (d, t), t) ;
  }  // end of trim

// ── GetWord / StringToVector (from stdafx.h — Windows signatures) ─────────
// Single-sourced in utilities_shared.cpp.  NB: the 4th argument is
// trim_spaces, matching Windows; the old portable include_delim variant is
// gone.
string GetWord(string& s,
               const string delim = " ",
               const bool trim_spaces = true);

void StringToVector(const string s,
                    vector<string>& v,
                    const string delim = " ",
                    const bool trim_spaces = true);
#endif // !SPACES (stdafx.h not previously included)

#ifndef MFC_UI_H_INCLUDED
// ── CStringList — value-based list of CStrings ────────────────────────────
// MFC's CStringList stores CStrings by value (unlike CTypedPtrList).
class CStringList {
    std::list<CString> m_data;
public:
    void RemoveAll()                    { m_data.clear(); }
    bool IsEmpty() const                { return m_data.empty(); }
    int  GetCount() const               { return (int)m_data.size(); }
    void AddTail(const CString& s)      { m_data.push_back(s); }
    void AddHead(const CString& s)      { m_data.push_front(s); }
    CString RemoveHead() {
        CString v = m_data.front(); m_data.pop_front(); return v;
    }
    CString RemoveTail() {
        CString v = m_data.back(); m_data.pop_back(); return v;
    }
    const CString& GetHead() const      { return m_data.front(); }
    const CString& GetTail() const      { return m_data.back(); }
    // Find: returns POSITION of first element == val, or nullptr
    POSITION Find(const CString& val) const {
        for (auto it = m_data.begin(); it != m_data.end(); ++it)
            if (*it == val) return (POSITION)&*it;
        return nullptr;
    }

    POSITION GetHeadPosition() const {
        if (m_data.empty()) return nullptr;
        return (POSITION)&m_data.front();
    }
    POSITION GetTailPosition() const {
        if (m_data.empty()) return nullptr;
        return (POSITION)&m_data.back();
    }
    // Iterator-style: walk forward from pos; updates pos to next, returns current.
    CString& GetNext(POSITION& pos) {
        // Walk using iterator from head; find node matching pos ptr, advance.
        for (auto it = m_data.begin(); it != m_data.end(); ++it) {
            if (&*it == (CString*)pos) {
                auto cur = it++;
                pos = (it != m_data.end()) ? (POSITION)&*it : nullptr;
                return *cur;
            }
        }
        pos = nullptr;
        return m_data.back(); // fallback
    }
    const CString& GetNext(POSITION& pos) const {
        for (auto it = m_data.begin(); it != m_data.end(); ++it) {
            if (&*it == (const CString*)pos) {
                auto cur = it++;
                pos = (it != m_data.end()) ? (POSITION)&*it : nullptr;
                return *cur;
            }
        }
        pos = nullptr;
        return m_data.back();
    }
};
#endif // !MFC_UI_H_INCLUDED

#ifndef MFC_UI_H_INCLUDED
// ── CMapStringToString — string-keyed string map ──────────────────────────
class CMapStringToString {
    std::map<string, CString> m_data;
public:
    void RemoveAll()                              { m_data.clear(); }
    bool IsEmpty() const                          { return m_data.empty(); }
    void SetAt(const CString& key, const CString& val) {
        m_data[string(key)] = val;
    }
    bool RemoveKey(const CString& key) {
        return m_data.erase(string(key)) > 0;
    }
    bool Lookup(const CString& key, CString& val) const {
        auto it = m_data.find(string(key));
        if (it == m_data.end()) return false;
        val = it->second;
        return true;
    }
    POSITION GetStartPosition() const {
        if (m_data.empty()) return nullptr;
        return (POSITION)&*m_data.begin();
    }
    void GetNextAssoc(POSITION& pos, CString& key, CString& val) const {
        for (auto it = m_data.begin(); it != m_data.end(); ++it) {
            if (&*it == (std::pair<const string,CString>*)pos) {
                key = CString(it->first.c_str());
                val = it->second;
                ++it;
                pos = (it != m_data.end()) ? (POSITION)&*it : nullptr;
                return;
            }
        }
        pos = nullptr;
    }
};
#endif // !MFC_UI_H_INCLUDED
// ── Forward declarations ──────────────────────────────────────────────────
// (CFont is now fully defined in platform/mfc_ui.h)
class CMUSHclientDoc;

// ── CMUSHView / CSendView stubs ───────────────────────────────────────────
// ProcessPreviousLine.cpp uses RUNTIME_CLASS(CMUSHView) and IsKindOf().
// CView is fully defined in platform/mfc_ui.h (included via compat.h).
// The static classCMUSHView/classCSendView members are defined in
// windows_stubs.cpp (which is part of libmushcore on all platforms).
#ifndef CMUSHView_stub_defined
#define CMUSHView_stub_defined
class CMUSHView : public CView {
public:
    DECLARE_DYNCREATE(CMUSHView)
    CToolTipCtrl m_ToolTip;   // scriptingoptions.cpp tooltip specials (dead headless:
                              // GetFirstViewPosition() is nullptr, loop never runs)
    void Invalidate() {}
    RECT GetTextRectangle() const { RECT r = {0,0,0,0}; return r; }
    CPoint GetScrollPosition() const { return CPoint(0, 0); }
    // no-ops for the real display bodies' view-notify loops (dead headless:
    // GetFirstViewPosition returns nullptr so these are never reached)
    void addedstuff() {}
    void did_jump() {}
};

class CSendView : public CView {
public:
    DECLARE_DYNCREATE(CSendView)
    void CheckForSelectionChange() {}
    // methods_spellchecker.cpp SpellCheckCommand's command-window edit
    // accessor (dead headless: the view walk never finds a CSendView).
    CEdit& GetEditCtrl() { static CEdit e; return e; }
};
#endif // CMUSHView_stub_defined

// ── xterm 256-colour table ────────────────────────────────────────────────
extern COLORREF xterm_256_colours[256];

// ── Macros ────────────────────────────────────────────────────────────────
#ifndef NEWSTYLE
#define NEWSTYLE       new CStyle
#define DELETESTYLE(p) delete (p)
#endif

// Forward-declare CScriptEngine so CPlugin can hold a pointer to it.
// The full definition comes later via scripting/scripting.h.
class CScriptEngine;

// ── Array map types (needed by CPlugin::m_Arrays and CMUSHclientDoc::m_Arrays)
// Defined here — before CPlugin — so the struct is visible when both use it.
// The same definition appears under #ifndef ARRAY_LESS_DEFINED in stdafx.h on
// Windows.  Guard so it is harmless if a TU pulls in stdafx.h first.
#ifndef ARRAY_LESS_DEFINED
#define ARRAY_LESS_DEFINED
struct array_less
{
    bool operator()(const string& s1, const string& s2) const
    {
        auto isnum = [](const string& s) {
            if (s.empty()) return false;
            size_t i = (s[0] == '-') ? 1 : 0;
            if (i == s.size()) return false;
            for (; i < s.size(); ++i)
                if (!isdigit((unsigned char)s[i])) return false;
            return true;
        };
        bool n1 = isnum(s1), n2 = isnum(s2);
        if (n1 && n2) return atoll(s1.c_str()) < atoll(s2.c_str());
        if (!n1 && !n2) return s1 < s2;
        return n1;  // numbers sort before strings
    }
};
typedef map<string, string, array_less>              tStringToStringMap;
typedef map<string, tStringToStringMap*, array_less> tStringMapOfMaps;
#endif // ARRAY_LESS_DEFINED

// ── CPlugin — CONVERGED onto the REAL plugins.h (Campaign B CPlugin
// convergence).  Everything plugins.h needs is already defined above:
// CObject/CString/CTime (compat.h), DISPID/DISPID_UNKNOWN/__int64/LONGLONG,
// the CTrigger/CAlias/CTimer/CVariable map+array types (OtherTypes.h),
// tStringMapOfMaps (array_less block above), and the CScriptEngine /
// CMUSHclientDoc forward declarations.  The real CPlugin ctor/dtor and the
// ExecutePluginScript family live in plugins.cpp, which mushcore now
// compiles (the old stub CPlugin + platform/plugin_portable.cpp are gone).
#include "plugins.h"

// ── CWorldDocTemplate ─────────────────────────────────────────────────────
// Portable equivalent of MFC CDocTemplate for iterating open world docs.
// Mirrors the App.m_pWorldDocTemplate->GetFirstDocPosition() /
// GetNextDoc() pattern used throughout the codebase.
class CMUSHclientDoc;   // forward declaration
class CWorldDocTemplate {
public:
    std::vector<CMUSHclientDoc*> m_docs;

    void AddDoc(CMUSHclientDoc* doc) {
        m_docs.push_back(doc);
    }
    void RemoveDoc(CMUSHclientDoc* doc) {
        m_docs.erase(std::remove(m_docs.begin(), m_docs.end(), doc),
                     m_docs.end());
    }

    // POSITION is void* (see mfc_collections.h); we store a 1-based index.
    POSITION GetFirstDocPosition() const {
        return m_docs.empty() ? nullptr : (POSITION)1;
    }
    CMUSHclientDoc* GetNextDoc(POSITION& pos) const {
        size_t idx = (size_t)pos - 1;
        CMUSHclientDoc* doc = (idx < m_docs.size()) ? m_docs[idx] : nullptr;
        ++idx;
        pos = (idx < m_docs.size()) ? (POSITION)(idx + 1) : nullptr;
        return doc;
    }
    // Stub so mainfrm_stub.h can call OpenDocumentFile without a crash.
    CMUSHclientDoc* OpenDocumentFile(const char*) { return nullptr; }
};

// ── CMUSHclientApp ────────────────────────────────────────────────────────
// Stub for the global App object; holds shared MXP lookup tables.
struct lua_State;   // <lua.h> arrives later (scripting/scripting.h)
#ifndef CMUSHclientApp_defined
#define CMUSHclientApp_defined
class CMUSHclientApp {
public:
    // ── Spell checker (MUSHclient.h:96-102) ───────────────────────────────
    // Never initialised headless (InitSpellCheck is GUI-side), so the
    // adopted methods_spellchecker.cpp bodies short-circuit to the canonical
    // not-active results: SpellCheck/SpellCheckDlg → empty VARIANT (nil),
    // SpellCheckCommand → -1, AddSpellCheckWord → eSpellCheckNotActive.
    bool        m_bSpellCheckOK    = false;
    lua_State * m_SpellChecker_Lua = nullptr;

    // ── Translator (MUSHclient.h:103-105) — no localisation headless ──────
    lua_State * m_Translator_Lua   = nullptr;
    CString     m_strTranslatorFile;   // GetInfo(69)
    CString     m_strLocale;           // GetInfo(70)

    // ── Fixed-pitch font (MUSHclient.h:172/202) — GetInfo(71/243) ─────────
    CString      m_strFixedPitchFont;
    unsigned int m_iFixedPitchFontSize = 0;

    // ── Global preferences database (MUSHclient.h:117) — none headless ────
    string      m_PreferencesDatabaseName;   // GetInfo(82) → ""

    // ── When the client started executing (MUSHclient.h:61) — GetInfo(305)
    CTime       m_whenClientStarted;

    // ── Global options (MUSHclient.h:272-273).  Headless there is no
    // global-preferences store, so every name is "unknown" (VT_NULL → Lua
    // nil, exactly what the Windows App returns for a bad name) and the
    // list is empty (a never-created safearray → nil).  Bodies in
    // doc_portable.cpp.
    VARIANT GetGlobalOption(LPCTSTR Name);
    VARIANT GetGlobalOptionList();

    // ── CWinApp::OpenDocumentFile (world.Open).  Headless there is no doc
    // template/GUI wiring to open a world through, so opening by path is
    // unsupported → NULL (world.Open returns a null world object, exactly
    // as Windows does when the open fails).
    class CDocument* OpenDocumentFile(LPCTSTR) { return nullptr; }

    CAtomicElementMap  m_ElementMap;
    CMapStringToString m_EntityMap;
    CColoursMap        m_ColoursMap;

    // Scripting / sandboxing
    CString m_strLuaScript;              // sandbox script (empty on macOS — no sandbox)
    CString m_strMUSHclientFileName;     // full path of the executable (PlaySound relative-path base; empty headless)
    string  m_strDefaultLogFileDirectory;// prefix for log files
    string  m_strDefaultWorldFileDirectory;  // prefix for world files (FormatTime %F)
    // Plugin state-file directory + plugins directory (plugins.cpp SaveState;
    // empty headless, so CPlugin::SaveState returns without writing).
    CString m_strDefaultStateFilesDirectory;
    CString m_strPluginsDirectory;
    void SaveGlobalsToDatabase() {}   // GUI persists global prefs; no-op headless
    // Default-set files (GUI global prefs; empty headless, so
    // OnFileReloaddefaults' Load_Set calls are all short-circuited)
    CString m_strDefaultColoursFile;
    CString m_strDefaultTriggersFile;
    CString m_strDefaultAliasesFile;
    CString m_strDefaultTimersFile;
    CString m_strDefaultMacrosFile;
    // Default-font overrides (GUI global prefs; unset headless)
    CString m_strDefaultInputFont;
    CString m_strDefaultOutputFont;
    int m_iDefaultInputFontHeight  = 0;
    int m_iDefaultInputFontWeight  = 0;
    int m_iDefaultInputFontCharset = 0;
    int m_iDefaultOutputFontHeight = 0;
    int m_iDefaultOutputFontCharset = 0;
    unsigned int m_iDefaultInputFontItalic = 0;

    // Performance counter frequency in ticks/second (nanoseconds on POSIX).
    // Non-zero once initialised; zero keeps all timing guards inactive.
    LONGLONG m_iCounterFrequency = 0;

    void MXP_LoadColours();
    void MXP_LoadEntities();
    void MXP_LoadElements();

    // Windows InitInstance (MUSHclient.cpp:742-744) loads the MXP tables at
    // startup; mirror that here so colour NAMES resolve headless too
    // (SetColour / Get_XML_colour / world.ColourNameToRGB all consult
    // App.m_ColoursMap).
    CMUSHclientApp() {
        MXP_LoadElements();
        MXP_LoadEntities();
        MXP_LoadColours();
        m_whenClientStarted = CTime::GetCurrentTime();   // MUSHclient.cpp:240
    }

    // Unique number generator — used for concurrency checks on variable updates.
    long GetUniqueNumber() {
        static long s_counter = 0;
        return ++s_counter;
    }

    // Notification flags (used by worldsock.cpp OnClose and proxy error handling)
    bool m_bNotifyOnDisconnect              = false;
    bool m_bNotifyIfCannotConnect           = false;
    bool m_bErrorNotificationToOutputWindow = false;
    bool m_bReconnectOnLinkFailure          = false;

    // Display / scrolling
    bool    m_bBleedBackground           = false;
    bool    m_bSmoothScrolling           = false;
    bool    m_bSmootherScrolling         = false;

    // Word-delimiter sets (double-click selection)
    CString m_strWordDelimitersDblClick;

    // Activity window
    void*   m_pActivityDoc               = nullptr;
    void*   m_pActivityView              = nullptr;
    bool    m_bUpdateActivity            = false;

    // Input routing
    bool    m_bAllTypingToCommandWindow  = false;
    bool    m_bF1macro                   = false;
    bool    m_bDisableKeyboardMenuActivation = false;

    // Printer settings
    int     m_nPrinterLinesPerPage       = 60;
    int     m_nPrinterLeftMargin         = 1;
    int     m_nPrinterTopMargin          = 1;
    int     m_nPrinterFontSize           = 10;
    CString m_strPrinterFont;

    // String used to make trigger/alias names unique
    CString GetUniqueString() {
        static long s_n = 0;
        CString s; s.Format("%ld", ++s_n); return s;
    }

    // File → New
    void OnFileNew() {}

    // World document registry — portable equivalent of the Windows
    // CDocTemplate.  All open CMUSHclientDoc objects register here so that
    // world.GetWorld(), world.GetWorldList(), etc. can find them.
    CWorldDocTemplate  m_worldDocTemplate;
    CWorldDocTemplate* m_pWorldDocTemplate = &m_worldDocTemplate;
};
#endif // CMUSHclientApp_defined
extern CMUSHclientApp App;

// ── CMainFrame stub ───────────────────────────────────────────────────────
// Forward decl for CStatusBar stub
class CStatusBarStub {
public:
    LRESULT SendMessage(UINT, WPARAM=0, LPARAM=0) { return 0; }
};

// Forward decl for CMDITabsStub
class CMDITabsStub {
public:
    bool InUse() const { return false; }
    void Update() {}
};

#ifndef CMainFrame_defined
#define CMainFrame_defined
class CMainFrame : public CWnd {   // CWnd base as in MFC (Load_Set takes CWnd*)
public:
    // Status bar
    CStatusBarStub  m_wndStatusBar;
    int             m_nPauseItem     = 0;

    // MDI frame background colour (mainfrm.h:132; 0xFFFFFFFF = default).
    // Written by world.SetFrameBackgroundColour — headless nothing repaints,
    // but the value round-trips.
    COLORREF        m_backgroundColour = 0xFFFFFFFF;

    // MDI tabs
    CMDITabsStub    m_wndMDITabs;

    // Auto menu enable (MFC feature)
    BOOL            m_bAutoMenuEnable = FALSE;

    // MDI client window handle (for GetClientRect)
    HWND            m_hWndMDIClient   = nullptr;

    // Status helpers
    void SetStatusMessageNow(const CString&) {}
    void SetStatusMessage    (const CString&) {}
    void SetStatusNormal     ()               {}
    void OnUpdateBtnWorlds   (int, void*)     {}
    void FixUpTitleBar       ()               {}
    bool IsFullScreen        () const         { return false; }
    // Deferred main-loop posts (e.g. the TLS consent prompt): headless has
    // no message pump, so posts go nowhere — TLS failures fail closed.
    BOOL PostMessage         (UINT, WPARAM = 0, LPARAM = 0) { return TRUE; }
    // Timer fallback (real impl drives WM_TIMER-less timers in the GUI)
    void CheckTimerFallback  ()               {}

    // Layout query (returns empty rects on non-Windows)
    void ReturnStatusRect (CRect& r) const { r = CRect(); }
    void ReturnToolbarRect(CRect& r) const { r = CRect(); }

    operator HWND() const { return nullptr; }
};
#endif // CMainFrame_defined
// ── CChildFrame stub (childfrm.h) — the per-world MDI child.  Headless
// there are no views, so CMUSHclientDoc::GetChildFrame() always returns
// nullptr and SetCommandWindowHeight → eBadParameter (the CSplitterWnd
// members are the inert mfc_ui.h stubs; nothing constructs this class). ──
#ifndef CChildFrame_defined
#define CChildFrame_defined
class CChildFrame : public CWnd {
public:
    CSplitterWnd m_wndSplitter;
};
#endif // CChildFrame_defined

// Redefine Frame to return a static stub instance.
extern CMainFrame g_stub_frame;
#ifndef Frame
#define Frame g_stub_frame
#endif

// ── Script action-source values (from doc.h) ─────────────────────────────
enum {
    eDontChangeAction    = 999,
    eUnknownActionSource = 0,
    eUserTyping          = 1,
    eUserMacro           = 2,
    eUserKeypad          = 3,
    eUserAccelerator     = 4,
    eUserMenuAction      = 5,
    eTriggerFired        = 6,
    eTimerFired          = 7,
    eInputFromServer     = 8,
    // eWorldAction = 9 already defined above as enum { eWorldAction = 9 }
    eLuaSandbox          = 10,
    eHotspotCallback     = 11,
};

// ── Note style ────────────────────────────────────────────────────────────
// NORMAL is already defined in paneline.h; guard against redefinition.
#ifndef NORMAL
#define NORMAL 0x0000
#endif

// ── Script error colours (from stdafx.h) ─────────────────────────────────
#define SCRIPTERRORFORECOLOUR "orangered"
#define SCRIPTERRORBACKCOLOUR "black"

// ── Plugin-tick / draw-output-window callback names ───────────────────────
extern const string ON_PLUGIN_TICK;
extern const string ON_PLUGIN_DRAW_OUTPUT_WINDOW;
extern const string ON_PLUGIN_DISCONNECT;

// ── Telnet plugin constants ───────────────────────────────────────────────
extern const string ON_PLUGIN_IAC_GA;
extern const string ON_PLUGIN_TELNET_REQUEST;
extern const string ON_PLUGIN_TELNET_OPTION;
extern const string ON_PLUGIN_TELNET_SUBNEGOTIATION;

// ── Object name validation (from utilities) ───────────────────────────────
// Trims whitespace, optionally validates the label, then lowercases the name
// (matching CMUSHclientDoc::CheckObjectName in doc.cpp).  Returns eOK if the
// name is valid, otherwise a non-zero error code.
// NOTE: takes a non-const ref so the caller's CString is lowercased in place,
// exactly as the Windows member-method version does.
long CheckObjectName(CString& strName, const bool bConform = true);

// ── Free function declarations ────────────────────────────────────────────
unsigned long MakeActionHash(const CString& strAction,
                             const CString& strHint,
                             const CString& strVariable);

// ── The six table-generic option-machinery functions (doc_shared.cpp;
//    doc.h:557-590 signatures) ──────────────────────────────────────────
double GetBaseOptionItem(const int iItem,
                         tConfigurationNumericOption BaseOptionsTable[],
                         const int iItemCount, char* pBase);
CString GetBaseAlphaOptionItem(const int iItem,
                               tConfigurationAlphaOption BaseAlphaOptionsTable[],
                               const int iItemCount, char* pBase);
long SetBaseOptionItem(const int iItem,
                       tConfigurationNumericOption BaseOptionsTable[],
                       const int iItemCount, char* pBase,
                       double Value, bool& bChanged);
long SetBaseAlphaOptionItem(const int iItem,
                            tConfigurationAlphaOption BaseAlphaOptionsTable[],
                            const int iItemCount, char* pBase,
                            CString& strValue, bool& bChanged);
long FindBaseOption(LPCTSTR OptionName,
                    tConfigurationNumericOption BaseOptionsTable[],
                    int& iItem);
long FindBaseAlphaOption(LPCTSTR OptionName,
                         tConfigurationAlphaOption BaseAlphaOptionsTable[],
                         int& iItem);

// ── GetUniqueID (stdafx.h:525; Windows body Utilities.cpp:412) ────────────
// 24 lowercase hex chars — SHA-1 of a fresh GUID, PLUGIN_UNIQUE_ID_LENGTH/8
// digest words.  Portable body in doc_portable.cpp; SetAlphaDefaults uses it
// for the OPT_WORLD_ID option, the ctor for m_strWorldID.
CString GetUniqueID(void);

// ── GetGUID (stdafx.h; Windows body Utilities.cpp) — a fresh GUID string.
// Portable body in doc_portable.cpp; world.CreateGUID wraps it.
CString GetGUID(void);

// ── Name generation (stdafx.h:395-396; Windows body NameGeneration.cpp) ───
// Headless stubs in platform/windows_stubs.cpp: the names files are a GUI
// feature, so ReadNames throws (world.ReadNamesFile → eCouldNotOpenFile) and
// bNamesRead stays false (world.GenerateName → VT_NULL, as on Windows before
// any names file is read).  No default arg here (stdafx.h owns `= false`).
void ReadNames (const LPCTSTR sName, const bool bNoDialog);
CString GenerateName (void);

// ── windowPositionHelper (stdafx.h:946; Windows body methods_output.cpp:802)
// "left,top,width,height" formatting for Get*WindowPosition.  Portable copy
// in doc_portable.cpp until methods_output.cpp adopts.
void windowPositionHelper (CString & str, const RECT & r);

// ── GetSelection (stdafx.h:484; Windows body Utilities.cpp) ───────────────
// Selected text of a command-window CEdit (true = whole text).  Headless the
// only caller (SpellCheckCommand) bails before reaching it — inert definition
// in doc_portable.cpp.
bool GetSelection (CEdit * pEdit, CString & strSelection);

// ── StripAnsi (stdafx.h:944; body methods_utilities.cpp, now in mushcore) ─
CString StripAnsi (const CString strMessage);

bool GetWord(CString& strResult, CString& str);
CString GetArgument(const CArgumentList& ArgumentList,
                    CString strName,
                    const int iPosition,
                    const bool bLowerCase);
bool GetKeyword(CArgumentList& ArgumentList, CString strName);
bool IsNumeric(const CString& str);
bool SetColour(const CString& strName, COLORREF& iColour);
bool IsValidName(const CString& str);
void StringToList(const CString str, const char* delim,
                  CStringList& thelist);
void ListToString(const CStringList& thelist, const char delim,
                  CString& str);

// ── OptionsTable (REAL — defined in scriptingoptions.cpp) ─────────────────
extern tConfigurationNumericOption OptionsTable[];

// ── FindAndReplace (from stdafx.h) ────────────────────────────────────────
// Only define when stdafx.h hasn't already done so.
#ifndef _STDAFX_INCLUDED_
inline string FindAndReplace(const string& source,
                             const string& target,
                             const string& replacement)
{
    string str = source;
    string::size_type pos = 0, found;
    while ((found = str.find(target, pos)) != string::npos) {
        str.replace(found, target.size(), replacement);
        pos = found + replacement.size();
    }
    return str;
}
#endif

// ── Colour adjustment (mirrors Utilities.cpp) ────────────────────────────
// ADJUST_COLOUR_* enum is defined in stdafx.h (pulled in via compat.h).
long    AdjustColour(long Colour, short Method);
CString ColourToName(COLORREF colour);

// ── Base64 (decodeBase64 in utilities_portable.cpp; encodeBase64 shared) ──
CString decodeBase64(const char* coded);
CString encodeBase64(const char* plaintext, bool bMultiLine);

// ── Shared pure-logic free functions (utilities_shared.cpp) ──────────────
int myAtoF(const char* z, double* pResult);
int EditDistance(const std::string source, const std::string target);
// no default arg here (stdafx.h owns the `= false` default for the full build)
bool IsStringNumber(const std::string& s, bool bSigned);
void Generate256colours(void);   // fills xterm_256_colours[] (defined in doc_portable.cpp)
// no default arg (stdafx.h owns the `= false` default; declaring one here too
// would be a redefinition in TUs that see both headers)
BOOL CheckLabel(const CString& strLabel, bool bScript);
CString FixUpGerman(const CString strMessage);   // umlaut → ue/ae/oe transliteration
// no default arg (stdafx.h owns `= false`); defined in xml/xmlparse.cpp
bool IsNumber(const CString& str, bool bAllowSign);
const char* Make_Absolute_Path(CString strFileName);   // prefix working_dir if relative
const char* strnrchr(const char* s, const char c, size_t count);   // last c within count

// ── Mersenne Twister RNG (scripting/mt19937ar.c) ──────────────────────
extern "C" {
    void   init_genrand(unsigned long seed);
    double genrand(void);
}

// ── Free function stubs ──────────────────────────────────────────────────
// These are defined in Utilities.cpp on Windows; stubs live in doc_portable.cpp.
CString FixHTMLString(CString s);
// Element-content escaping (&/</>/tab; newlines preserved) — single-sourced
// in utilities_shared.cpp (stdafx.h:452 signature).
CString FixHTMLMultilineString (const CString strToFix);

// find-and-replace for strings (Replace.cpp, compiled into mushcore too).
// No default argument here: stdafx.h declares the same function WITH the
// default, and several portable TUs include both headers - a repeated
// default would be a redefinition. Stub-only TUs that need the default
// restate it (legal: a redeclaration may add a default).
CString Replace (const CString strSource,
                 const CString strTarget,
                 const CString strReplacement,
                 const bool bAll);
CString FixupEscapeSequences(const CString& s);
// Guard: stdafx.h declares this with the same signature (and default args).
// Only redeclare here for TUs that don't include stdafx.h.
#ifndef _STDAFX_INCLUDED_
CString ConvertToRegularExpression(const CString& strMatchString,
                                   bool bWholeLine = true,
                                   bool bMakeAsterisksWildcards = true);
#endif
void    ThrowErrorException(const char* fmt, ...)
            __attribute__((format(printf,1,2)));
// GetClipboardContents and putontoclipboard are declared in platform/mfc_ui.h
// as returning BOOL — we do not redeclare them here.
void    ChangeToFileBrowsingDirectory();
void    ChangeToStartupDirectory();
bool    IsArchiveXML(class CArchive&);
string  tolower(const string& s);
CString AnsiCode(short iCode);   // convert ANSI attribute number to ESC sequence
// MakeRandomNumber forward-declaration uses SHS_INFO from scripting/shs.h.
// Include the header here so all TUs that include doc_stub.h share the type.
#include "scripting/shs.h"
void    MakeRandomNumber(CMUSHclientDoc* pDoc, SHS_INFO& shsInfo);

// CPaneLine::AddStyle needs the full CPaneLine type — include the header.
// paneline.h is cross-platform (uses only CTime, COLORREF, string, vector).
#include "paneline.h"

// ── Windows-only class stubs ──────────────────────────────────────────────
// CChatSocket, CSendView, CTimerWnd, CMUSHView are Windows MFC classes.
// ProcessPreviousLine.cpp, timers.cpp and serialize.cpp reference their
// methods and static CRuntimeClass members.  We provide minimal stubs so
// those files compile and link on non-Windows without pulling in MFC headers.

// Forward-declare to avoid duplicate-definition conflicts with MUSHview.h /
// sendvw.h / chatsock.h / TimerWnd.h which may be included before us.
// The real DECLARE_DYNCREATE definitions in those headers are identical to
// what we need here.

#ifndef CChatSocket_stub_defined
#define CChatSocket_stub_defined
class CChatSocket {
public:
    int  m_iChatStatus   = eChatNotConnected;
    bool m_bCanSnoop     = false;
    bool m_bHeIsSnooping = false;
    void Process_Snoop   (CString) {}
    void SendChatMessage (int, CString, long = 0) {}
};
// CChatSocketList — list of CChatSocket pointers (normally in chatsock.h)
typedef CTypedPtrList<CPtrList, CChatSocket*> CChatSocketList;
#endif // CChatSocket_stub_defined

// CPluginList / PluginListIterator now come from the real plugins.h
// (included above — CPlugin convergence).

#ifndef CTimerWnd_stub_defined
#define CTimerWnd_stub_defined
class CTimerWnd {
public:
    void ChangeTimerRate(int) {}
};
#endif // CTimerWnd_stub_defined

// CMUSHView and CSendView declare static CRuntimeClass members via
// DECLARE_DYNCREATE in MUSHview.h / sendvw.h.  Those headers may or may
// not be included before this file.  We can't add definitions here (that
// would be duplicate definitions if the headers are included), so we just
// ensure the forward declarations are visible; the actual
//   CRuntimeClass CMUSHView::classCMUSHView = { "CMUSHView" };
//   CRuntimeClass CSendView::classCSendView = { "CSendView" };
// are defined in doc_portable.cpp after including the full headers.

// ── ScriptItem (from doc.h) — deferred script execution record ───────────
class ScriptItem {
public:
    ScriptItem(CPlugin* pPlugin, const string sText, const string sSource)
        : pWhichPlugin(pPlugin), sScriptText(sText), sScriptSource(sSource) {}
    CPlugin*      pWhichPlugin;
    const string  sScriptText;
    const string  sScriptSource;
};
typedef list<ScriptItem> ScriptItemMap;

// ── OneShotItem (from doc.h) — one-shot trigger deletion record ───────────
class OneShotItem {
public:
    OneShotItem(CPlugin* pPlugin, const string sKey)
        : pWhichPlugin(pPlugin), sItemKey(sKey) {}
    CPlugin*      pWhichPlugin;
    const string  sItemKey;
};
typedef list<OneShotItem> OneShotItemMap;

// ── GUI output abstraction ────────────────────────────────────────────────
// SOutputRun / SOutputLine are defined in platform/output_types.h so they can
// be shared between the mushcore (doc_stub.h) and mushapp (doc.h) builds.
#include "platform/output_types.h"
#include "pal/iworldview.h"   // pal::IWorldView — the typed output seam

// ── SQLite database types (mirrors doc.h) ─────────────────────────────────────
#include "sqlite3/sqlite3.h"

typedef struct {
    sqlite3*      db;        // open database handle (NULL if closed)
    sqlite3_stmt* pStmt;     // current prepared statement (NULL if none)
    bool          bValidRow; // true if last DatabaseStep returned SQLITE_ROW
    string        db_name;   // filename used when opening
    int           iColumns;  // column count of prepared statement
} tDatabase;

typedef map<string, tDatabase*> tDatabaseMap;
typedef tDatabaseMap::iterator   tDatabaseMapIterator;

// ── XML load/save mask bits (doc.h:1950-1964, verbatim) ───────────────────
// plugins.cpp (CPlugin::SaveState → Save_World_XML (ar, XML_VARIABLES, …))
// needs these headless.
#ifndef XML_GENERAL
#define XML_GENERAL    0x0001
#define XML_TRIGGERS   0x0002
#define XML_ALIASES    0x0004
#define XML_TIMERS     0x0008
#define XML_MACROS     0x0010
#define XML_VARIABLES  0x0020
#define XML_COLOURS    0x0040
#define XML_KEYPAD     0x0080
#define XML_PRINTING   0x0100
#define XML_INCLUDES   0x0200
#define XML_PLUGINS    0x0400
#define XML_NO_PLUGINS 0x0800
#define XML_OVERWRITE  0x1000
#define XML_PASTE_DUPLICATE 0x2000
#define XML_IMPORT_MAIN_FILE_ONLY 0x4000
#endif // XML_GENERAL

// ── Save_XML_* free-function decls (stdafx.h:491-517 signatures) ──────────
// The REAL bodies are compiled into mushcore from xml/xml_save_world.cpp
// (world-file saver adoption); the old inert doc_portable.cpp no-ops are gone.
// Guard matches the other stdafx.h-owned declarations above (default args may
// be declared only once per TU).
#ifndef _STDAFX_INCLUDED_
void Save_XML_number  (class CArchive& ar,
                       const char * sName,
                       const long iNumber,
                       const bool sameline = false);
void Save_XML_boolean (class CArchive& ar,
                       const char * sName,
                       const long iNumber,
                       const bool sameline = false);
void Save_XML_string  (class CArchive& ar,
                       const char * sName,
                       const CString & str,
                       const bool sameline = false);
void Save_XML_multi   (class CArchive& ar,
                       const char * sName,
                       const CString & str);
void Save_XML_date    (class CArchive& ar,
                       const char * sName,
                       const CTime & date,
                       const bool sameline = false);
void Save_XML_colour  (class CArchive& ar,
                       const char * sName,
                       const COLORREF & colour,
                       const bool sameline = false);
void Save_XML_double  (class CArchive& ar,
                       const char * sName,
                       const double & fNumber,
                       const bool sameline = false);
void SeeIfBase64 (CString & strText);   // stdafx.h:519 (utilities_shared.cpp)

// keypad indexes (stdafx.h:660-696, verbatim) — m_keypad[] / strKeypadNames[]
enum
  {
  eKeypad_0            ,
  eKeypad_1            ,
  eKeypad_2            ,
  eKeypad_3            ,
  eKeypad_4            ,
  eKeypad_5            ,
  eKeypad_6            ,
  eKeypad_7            ,
  eKeypad_8            ,
  eKeypad_9            ,
  eKeypad_Dot          ,
  eKeypad_Slash        ,
  eKeypad_Star         ,
  eKeypad_Dash         ,
  eKeypad_Plus         ,
  eCtrl_Keypad_0       ,
  eCtrl_Keypad_1       ,
  eCtrl_Keypad_2       ,
  eCtrl_Keypad_3       ,
  eCtrl_Keypad_4       ,
  eCtrl_Keypad_5       ,
  eCtrl_Keypad_6       ,
  eCtrl_Keypad_7       ,
  eCtrl_Keypad_8       ,
  eCtrl_Keypad_9       ,
  eCtrl_Keypad_Dot     ,
  eCtrl_Keypad_Slash   ,
  eCtrl_Keypad_Star    ,
  eCtrl_Keypad_Dash    ,
  eCtrl_Keypad_Plus    ,

  eKeypad_Max_Items   // this must be last   !!!
  };
#endif // !_STDAFX_INCLUDED_

// ── world-file format version (doc.h:81, verbatim) ────────────────────────
// Written by Save_Version_And_Date_XML as world_file_version.
const int VERSION = 15;                  // version number written to world file

// ── Forward declaration for the UDP listener map (doc.h uses UDPsocket*) ──
class UDPsocket;

// ── Minimal CMUSHclientDoc ────────────────────────────────────────────────
class CMUSHclientDoc {
public:
    // ── Colour tables ──────────────────────────────────────────────────
    COLORREF m_normalcolour[8];
    COLORREF m_boldcolour[8];
    COLORREF       m_customtext[MAX_CUSTOM];
    COLORREF       m_customback[MAX_CUSTOM];
    map<COLORREF, COLORREF> m_ColourTranslationMap;   // doc.h:1461 — world.MapColour

    // ── doc-option members (option-machinery convergence step: every member
    // named by scriptingoptions_numeric.inc / scriptingoptions_alpha.inc now
    // exists with doc.h's exact type, so the real OptionsTable offsets are
    // valid against this class.  Declarations copied from doc.h; the ctor
    // (doc_portable.cpp) runs SetDefaults/SetAlphaDefaults over the real
    // tables, exactly like doc_construct.cpp on Windows.) ──
    CString m_file_postamble;
    CString m_file_preamble ;
    CString m_line_postamble;
    CString m_line_preamble ;
    CString m_strScriptEditorArgument;
    unsigned short m_bArrowsChangeHistory = 0;
    unsigned short m_bConfirmOnPaste = 0;
    COLORREF m_input_background_colour = 0;
    unsigned short  m_bExcludeMacros = 0;
    unsigned short  m_bExcludeNonAlpha = 0;
    unsigned short  m_bConfirmBeforeReplacingTyping = 0;
    unsigned short  m_bAltArrowRecallsPartial = 0;     // alt+up arrow recalls partially entered command
    unsigned short  m_bKeepFreezeAtBottom = 0; // don't automatically unfreeze after returning to bottom of buffer
    unsigned short  m_bAutoRepeat = 0;    // auto repeat last command
    unsigned short  m_bDoubleClickInserts = 0;      // double-click inserts word into command buffer
    unsigned short  m_bConfirmOnSend = 0;        // confirm preamble, postamble on file send
    unsigned short  m_bFlashIcon = 0;     // flash icon on taskbar for new activity?
    unsigned short  m_bArrowKeysWrap = 0;     // arrow keys wrap command history?
    LONG  m_bEditScriptWithNotepad = 0;            // use inbuilt notepad for editing scripts
    unsigned short m_bDoubleClickSends = 0;   // double-click sends to MUD
    unsigned short m_bAlwaysRecordCommandHistory = 0; // record command history even if echo off
    unsigned short m_bDoNotShowOutstandingLines = 0;  // don't show outstanding lines in tabbed windows
    unsigned short m_bAutoCopyInHTML = 0;     // auto-copy to clipboard in HTML
    unsigned short m_bAutoResizeCommandWindow = 0;   // change command window height automatically
    unsigned short m_iAutoResizeMinimumLines = 0;    // command window minimum number of lines
    unsigned short m_iAutoResizeMaximumLines = 0;    // command window maximum number of lines
    unsigned short m_bDoNotAddMacrosToCommandHistory = 0;    // macros not into command history
    unsigned short m_iDefaultTriggerSendTo = 0;            // default send-to location for triggers
    unsigned short m_iDefaultTriggerSequence = 0;          // default sequence for triggers
    unsigned short m_bDefaultTriggerRegexp = 0;            // default regular expression flag for triggers
    unsigned short m_bDefaultTriggerExpandVariables = 0;   // default expand-variables flag for triggers
    unsigned short m_bDefaultTriggerKeepEvaluating = 0;    // default keep-evaluating flag for triggers
    unsigned short m_bDefaultTriggerIgnoreCase = 0;        // default ignore-case flag for triggers
    unsigned short m_iDefaultAliasSendTo = 0;              // default send-to location for aliases
    unsigned short m_iDefaultAliasSequence = 0;            // default sequence for aliases
    unsigned short m_bDefaultAliasRegexp = 0;              // default regular expression flag for aliases
    unsigned short m_bDefaultAliasExpandVariables = 0;     // default expand-variables flag for aliases
    unsigned short m_bDefaultAliasKeepEvaluating = 0;      // default keep-evaluating flag for aliases
    unsigned short m_bDefaultAliasIgnoreCase = 0;          // default ignore-case flag for aliases
    unsigned short m_iDefaultTimerSendTo = 0;              // default send-to location for timers
    CString m_strOutputLinePreambleOutput;         // put at start of each output line - MUD output
    CString m_strOutputLinePreambleInput;          // put at start of each output line - input
    CString m_strOutputLinePreambleNotes;          // put at start of each output line - world.Note
    unsigned short m_iFadeOutputBufferAfterSeconds = 0; // fade output buffer after these many seconds (0 = disable)
    unsigned short m_FadeOutputOpacityPercent = 0;      // what opacity to fade to (0 to 100 percent)
    unsigned short m_FadeOutputSeconds = 0;             // how many seconds to fade over
    unsigned short m_bCtrlBackspaceDeletesLastWord = 0; // Ctrl+Backspace deletes last word in command window
    BOOL m_bArrowRecallsPartial = 0;    // arrow key (without ALT) recalls partial command
    BOOL m_bCtrlZGoesToEndOfBuffer = 0;       // Ctrl+Z goes to end of output buffer
    BOOL m_bCtrlPGoesToPreviousCommand = 0;   // Ctrl+P gets previous command
    BOOL m_bCtrlNGoesToNextCommand = 0;       // Ctrl+N gets next command
    BOOL m_bHyperlinkAddsToCommandHistory = 0;  // MXP hyperlink goes into history
    BOOL m_bEchoHyperlinkInOutputWindow = 0;    // MXP hyperlink is echoed
    unsigned  short m_IncomingChatPort = 0;   // default 4050
    unsigned  short m_bAcceptIncomingChatConnections = 0;   // do we listen for incoming chats?
    unsigned  short m_bIgnoreChatColours = 0;   // throw away incoming ANSI colours
    unsigned  short m_bValidateIncomingCalls = 0; // ask before accepting chat connections
    unsigned  short m_bAutoAllowSnooping = 0;   // once snooping is permitted don't ask again
    unsigned  short m_bAutoAllowFiles = 0;      // don't query if we can send a particular file
    COLORREF        m_cChatForegroundColour = 0;  // default colour for chats
    COLORREF        m_cChatBackgroundColour = 0;  //  - background for above
    long            m_iMaxChatLinesPerMessage = 0;   // max lines per chat message
    long            m_iMaxChatBytesPerMessage = 0;   // max bytes per chat message
    COLORREF m_OutputLinePreambleInputBackColour = 0;  // back colour - input preamble
    COLORREF m_OutputLinePreambleInputTextColour = 0;  // text colour - input preamble
    COLORREF m_OutputLinePreambleNotesBackColour = 0;  // back colour - Note preamble
    COLORREF m_OutputLinePreambleNotesTextColour = 0;  // text colour - Note preamble
    COLORREF m_OutputLinePreambleOutputBackColour = 0; // back colour - MUD output preamble
    COLORREF m_OutputLinePreambleOutputTextColour = 0; // text colour - MUD output preamble
    COLORREF m_input_text_colour = 0;
    CString         m_strChatMessagePrefix;   // <CHAT> or whatever
    CString         m_strChatSaveDirectory;     // where to save files
    CString         m_strOurChatName;     // who we are on chat lists
    CString m_strAliasesFilter;           // Lua code to filter aliases
    CString m_strEditorWindowName;               // What window to bring to the front after editing a text file
    CString m_strTimersFilter;            // Lua code to filter timers
    CString m_strTriggersFilter;          // Lua code to filter triggers
    CString m_strVariablesFilter;         // Lua code to filter variables
    LONG  m_bSaveDeletedCommand = 0;            // save deleted command in command history on <esc>
    LONG  m_bTranslateBackslashSequences = 0;            // interpret \n \r etc. on a send
    LONG  m_bUseDefaultOutputFont = 0;            // Default output font overrides
    LONG  m_bWarnIfScriptingInactive = 0;       // warn if we can't invoke a script
    LONG  m_nFileDelay = 0;            // delay in milliseconds before sending each line
    LONG  m_nFileDelayPerLines = 0;            // how many lines to send before the delay
    LONG  m_nPasteDelay = 0;           // delay in milliseconds before pasting each line
    LONG  m_nPasteDelayPerLines = 0;            // how many lines to send before the delay
    LONG  m_nReloadOption = 0;      // option for reloading changed script file
    unsigned int m_iTabCompletionLines = 0;          // lines to search
    unsigned int m_iToolTipStartTime = 0;           // Time before tooltip appears (milliseconds)
    unsigned int m_iToolTipVisibleTime = 0;         // Time tooltip stays visible (milliseconds)
    unsigned short  m_bFileCommentedSoftcode = 0;    // are we sending commented softcode?
    unsigned short  m_bLowerCaseTabCompletion = 0;  // tab complete words in lower case
    unsigned short  m_bPasteCommentedSoftcode = 0;   // are we pasting commented softcode?
    unsigned short  m_bReEvaluateAutoSay = 0; // send auto-say through command interpreter
    unsigned short  m_bShowBold = 0;   // show bold, italic, underline in fonts
    unsigned short  m_bShowItalic = 0;     // italic text to be shown in italic?
    unsigned short  m_bShowUnderline = 0;     // underlined text to be underlined?
    unsigned short  m_bSpellCheckOnSend = 0;        // spell check when you send something
    unsigned short  m_iPixelOffset = 0;     // pixel offset of text from side of window
    unsigned short m_bAutoWrapInput = 0;			      // Match input wrap to output?
    unsigned short m_bLineInformation = 0;
    unsigned short m_bOmitSavedDateFromSaveFiles = 0; // if set, do not write the date saved to save files
    unsigned short m_bPasteEcho = 0;
    unsigned short m_bPlaySoundsInBackground = 0;   // if true, PlaySound uses a global sound buffer
    unsigned short m_bStartPaused = 0;
    unsigned short m_bTabCompletionSpace = 0;        // insert space after word?
    unsigned short m_bTreeviewAliases = 0;          // show aliases in tree view?
    unsigned short m_bTreeviewTimers = 0;           // show timers in tree view?
    unsigned short m_bTreeviewTriggers = 0;         // show triggers in tree view?
    unsigned short m_bUseDefaultAliases = 0;
    unsigned short m_bUseDefaultColours = 0;
    unsigned short m_bUseDefaultInputFont = 0;
    unsigned short m_bUseDefaultMacros = 0;
    unsigned short m_bUseDefaultTimers = 0;
    unsigned short m_bUseDefaultTriggers = 0;
    unsigned short m_iLineSpacing = 0;        // line spacing, 0 to use font height
    unsigned short m_keypad_enable = 0;
    CNumericConfigurationArray m_NumericConfiguration;   // doc.h:1385 — loaded-value snapshots
    CAlphaConfigurationArray   m_AlphaConfiguration;      // doc.h:1386
    CString        m_strCustomColourName[MAX_CUSTOM];
    unsigned short m_echo_colour = SAMECOLOUR; // colour for echoed input in logs

    // ── ANSI parse state ───────────────────────────────────────────────
    BOOL m_bAlternativeInverse      = FALSE;
    BOOL m_bCustom16isDefaultColour = FALSE;
    CLine* m_pCurrentLine           = nullptr;
    int    m_phase                  = NONE;
    int    m_code                   = 0;

    // ── RememberStyle cache ────────────────────────────────────────────
    unsigned short m_iFlags      = 0;
    COLORREF       m_iForeColour = WHITE;
    COLORREF       m_iBackColour = BLACK;

    // ── Telnet negotiation state ───────────────────────────────────────
    unsigned short m_bConvertGAtoNewline = 0;
    long           m_last_line_with_IAC_GA = 0;
    long           m_total_lines           = 0;

    int    m_subnegotiation_type = 0;
    string m_IAC_subnegotiation_data;

    bool m_bClient_sent_IAC_DO   [256] = {};
    bool m_bClient_sent_IAC_DONT [256] = {};
    bool m_bClient_sent_IAC_WILL [256] = {};
    bool m_bClient_sent_IAC_WONT [256] = {};
    bool m_bClient_got_IAC_DO    [256] = {};
    bool m_bClient_got_IAC_DONT  [256] = {};
    bool m_bClient_got_IAC_WILL  [256] = {};
    bool m_bClient_got_IAC_WONT  [256] = {};

    long m_nCount_IAC_DO   = 0;
    long m_nCount_IAC_DONT = 0;
    long m_nCount_IAC_WILL = 0;
    long m_nCount_IAC_WONT = 0;
    long m_nCount_IAC_SB   = 0;

    // ── Echo / NAWS ────────────────────────────────────────────────────
    bool           m_bNoEcho    = false;
    BOOL m_bNoEchoOff = FALSE;  // type converged to doc.h
    BOOL           m_bNAWS        = FALSE;
    BOOL           m_bNAWS_wanted = FALSE;
    unsigned short m_nWrapColumn  = 80;

    // ── MXP basic ──────────────────────────────────────────────────────
    bool           m_bMXP       = false;
    unsigned short m_iUseMXP    = eOnCommandMXP;  // OptionsTable default (== Windows); was the
                                                  // out-of-range eNoMXP under the old broken enum
    int            m_iMXP_mode  = eMXP_open;

    // ── MXP parse state ────────────────────────────────────────────────
    CString m_strMXPstring;           // element being collected
    CString m_strMXPtagcontents;      // inside a tag (for &text;)
    char    m_cMXPquoteTerminator = 0;// ' or "
    int     m_iMXP_defaultMode   = eMXP_open;
    int     m_iMXP_previousMode  = eMXP_open;

    // ── MXP debug / counters ───────────────────────────────────────────
    unsigned short m_iMXPdebugLevel = 0; // DBG_ERROR by default (only errors)
    long long      m_iMXPerrors     = 0;
    long long      m_iMXPtags       = 0;
    long long      m_iMXPentities   = 0;
    int            m_iLastOutstandingTagCount = 0;

    // ── MXP mode flags ─────────────────────────────────────────────────
    bool m_bInParagraph  = false;
    bool m_bMXP_script   = false;
    bool m_bPreMode      = false;
    bool m_bPuebloActive = false;
    int  m_iListMode     = eNoList;
    int  m_iListCount    = 0;

    // ── MXP collections ────────────────────────────────────────────────
    CElementMap        m_CustomElementMap;
    CMapStringToString m_CustomEntityMap;
    CActiveTagList     m_ActiveTagList;
    CVariableMap       m_VariableMap;

    // ── Trigger / alias / timer collections ────────────────────────────
    CTriggerMap        m_TriggerMap;
    CTriggerRevMap     m_TriggerRevMap;
    CTriggerArray      m_TriggerArray;   // sorted array for ProcessOneTriggerSequence
    CTriggerList       m_TriggerList;
    CAliasMap          m_AliasMap;
    CAliasRevMap       m_AliasRevMap;
    CAliasList         m_AliasList;
    CAliasArray        m_AliasArray;
    CTimerMap          m_TimerMap;
    CTimerRevMap       m_TimerRevMap;

    // Accessors — mirror doc.h: when inside a plugin context (m_CurrentPlugin ≠
    // nullptr) return the plugin's own collection; otherwise the doc's.
    // m_CurrentPlugin is declared later in this class; C++ allows forward refs
    // to other members in inline method bodies because the whole class is parsed
    // before any body is compiled.
    CTriggerMap&    GetTriggerMap()    { return m_CurrentPlugin ? m_CurrentPlugin->m_TriggerMap    : m_TriggerMap;    }
    CTriggerRevMap& GetTriggerRevMap() { return m_CurrentPlugin ? m_CurrentPlugin->m_TriggerRevMap : m_TriggerRevMap; }
    CTriggerArray&  GetTriggerArray()  { return m_CurrentPlugin ? m_CurrentPlugin->m_TriggerArray  : m_TriggerArray;  }
    CAliasMap&      GetAliasMap()      { return m_CurrentPlugin ? m_CurrentPlugin->m_AliasMap      : m_AliasMap;      }
    CAliasRevMap&   GetAliasRevMap()   { return m_CurrentPlugin ? m_CurrentPlugin->m_AliasRevMap   : m_AliasRevMap;   }
    CAliasArray&    GetAliasArray()    { return m_CurrentPlugin ? m_CurrentPlugin->m_AliasArray    : m_AliasArray;    }
    CTimerMap&      GetTimerMap()      { return m_CurrentPlugin ? m_CurrentPlugin->m_TimerMap      : m_TimerMap;      }
    CTimerRevMap&   GetTimerRevMap()   { return m_CurrentPlugin ? m_CurrentPlugin->m_TimerRevMap   : m_TimerRevMap;   }
    CVariableMap&   GetVariableMap()   { return m_CurrentPlugin ? m_CurrentPlugin->m_VariableMap   : m_VariableMap;   }
    CString         GetLanguage()      { return m_strLanguage;   }

    // ── Scripting arrays (world.ArrayCreate / ArrayGet / etc.) ────────
    tStringMapOfMaps  m_Arrays;
    tStringMapOfMaps& GetArrayMap()    { return m_CurrentPlugin ? m_CurrentPlugin->m_Arrays        : m_Arrays;        }

    // ── SQLite databases (world.DatabaseOpen / etc.) ───────────────────
    tDatabaseMap      m_Databases;

    bool LoadWorldFromFile(const char* path);
    bool InternalLoadPlugin(const CString& strPluginPath);

    // ── Mud list (MUD type database) ───────────────────────────────────
    CMudMap            m_MudMap;

    // ── Variable / mapping state ───────────────────────────────────────────
    bool        m_bVariablesChanged = false;
    CString     m_strCurrentLine;           // line currently received from MUD (set during trigger processing)
    bool        m_bMapping          = false;
    CStringList m_strMapList;

    // ── Hyperlink display options ──────────────────────────────────────────
    BOOL m_bUseCustomLinkColour = FALSE;  // type converged to doc.h
    BOOL m_bMudCanChangeLinkColour = FALSE;  // type converged to doc.h
    BOOL m_bUnderlineHyperlinks = FALSE;  // type converged to doc.h
    BOOL m_bMudCanRemoveUnderline = FALSE;  // type converged to doc.h
    COLORREF m_iHyperlinkColour        = 0;

    // ── Colour / display options ──────────────────────────────────────────
    BOOL m_bIgnoreMXPcolourChanges = FALSE;  // type converged to doc.h
    bool m_bSuppressNewline            = false;

    // ── AFK / options ─────────────────────────────────────────────────────
    unsigned short m_bSendMXP_AFK_Response = 0;  // type converged to doc.h
    unsigned short m_bMudCanChangeOptions = 0;  // type converged to doc.h
    CTime m_tLastPlayerInput;

    // ── Connection / world identity ───────────────────────────────────────
    CString m_name;
    CString m_password;
    CString m_connect_text;       // text sent on connect
    CString m_strScriptPrefix;    // prefix to invoke scripts from command window
    CString m_strPathName;   // path to the loaded world file (CDocument's name
                             // for it, so world.Save's `strName = m_strPathName`
                             // compiles untouched)

    // ── World properties (absent from original stub; added for GetInfo parity) ─
    CString m_new_activity_sound;       // GetInfo(9):  new-activity beep file
    CString m_strScriptEditor;          // GetInfo(10): script editor path
    CString m_font_name;                // GetInfo(20): output font name
    LONG    m_font_height   = 0;        // output font height (0=unset; neg=LOGFONT px; pos=pt)
    LONG    m_font_weight   = 0;        // output font weight (0 = FW_DONTCARE)
    DWORD   m_font_charset  = 1;        // output font charset (1 = DEFAULT_CHARSET)
    CString m_input_font_name;          // GetInfo(23): input-bar font name
    LONG    m_input_font_height  = 0;   // input font height (0=unset)
    BYTE    m_input_font_italic  = 0;   // input font italic flag
    LONG    m_input_font_weight  = 0;   // input font weight (0 = FW_DONTCARE)
    DWORD   m_input_font_charset = 1;   // input font charset (1 = DEFAULT_CHARSET)
    CString m_paste_postamble;          // GetInfo(24): text appended after paste
    CString m_paste_preamble;           // GetInfo(25): text prepended before paste
    CString m_pasteline_postamble;      // GetInfo(26): appended after each paste line
    CString m_pasteline_preamble;       // GetInfo(27): prepended before each paste line
    CString m_strAutoSayString;         // GetInfo(37): auto-say prefix
    CString m_strOverridePrefix;        // GetInfo(38): override-prefix char
    CString m_strTabCompletionDefaults; // GetInfo(39): default tab-completion words
    CString m_strRecallLinePreamble;    // GetInfo(41): recall-window line preamble
    CString m_strBeepSound;             // GetInfo(50): beep sound file

    // ── GetInfo backing state, second tranche (methods_info.cpp adoption).
    // Types and defaults mirror doc.h / doc_construct.cpp:54-64,133,451.
    long     m_iOutputWindowRedrawCount = 0;   // doc.h:1084, GetInfo(295)
    CPoint   m_lastMousePosition;              // doc.h:1142, GetInfo(283/284)
    ci_set   m_strSpecialFontName;             // doc.h:1404, GetInfo(76) — empty headless
    CString  m_strBackgroundImageName;         // doc.h:1410, GetInfo(79)
    int      m_iBackgroundMode = 0;            // doc.h:1412, GetInfo(270)
    COLORREF m_iBackgroundColour = NO_COLOUR;  // doc.h:1414, GetInfo(271)
    CString  m_strForegroundImageName;         // doc.h:1417, GetInfo(78)
    int      m_iForegroundMode = 0;            // doc.h:1419, GetInfo(269)
    CRect    m_TextRectangle = CRect(0,0,0,0); // doc.h:1438, GetInfo(272-275)
    long     m_TextRectangleBorderOffset = 0,  // doc.h:1439-1443, GetInfo(276-282)
             m_TextRectangleBorderColour = 0,
             m_TextRectangleBorderWidth = 0,
             m_TextRectangleOutsideFillColour = 0,
             m_TextRectangleOutsideFillStyle = 0;
    BOOL     m_bScrollBarWanted = TRUE;        // doc.h:1468, GetInfo(120)
    CString  m_strWordUnderMenu;               // doc.h:1476, GetInfo(86)
    // Font metrics (doc.h:1254-1259, GetInfo 212-215/240/241).  The GUI keeps
    // these updated from the real output/input fonts; headless they stay at
    // the same defaults as the m_gui_font_* pane metrics above.
    int      m_FontHeight = 18,
             m_FontWidth  = 10;
    int      m_InputFontHeight = 18,
             m_InputFontWidth  = 10;
    CString  m_strLastImmediateExpression;     // doc.h:1313, GetInfo(52)
    CTime    m_timeScriptFileMod;              // doc.h:1318, GetInfo(303)
    CTime    m_whenWorldStarted;               // doc.h:1137, GetInfo(306)
    LARGE_INTEGER m_whenWorldStartedHighPrecision = {}; // doc.h:1138, GetLineInfo(13)

    // ── Current hyperlink state (set by AddStyle, consumed by DisplayMsg) ─
    // Cleared automatically when the style with ACTION_HYPERLINK/ACTION_SEND
    // is replaced by one without an action.


    unsigned short m_connect_now = 0;  // type converged to doc.h
    long    m_nTotalLinesSent  = 0;

    // ── Proxy / SSL / keep-alives (mirrored from doc.h) ──────────────────────
    unsigned short m_bUseSSL               = 0;
    unsigned short m_bSendKeepAlives       = 0;
    unsigned short m_bSaveWorldAutomatically = 0;
    unsigned short m_iSocksProcessing      = 0;   // 0=none,1=SOCKS4,2=SOCKS5
    unsigned short m_iProxyServerPort      = 1080;
    CString        m_strProxyServerName;
    CString        m_strProxyUserName;
    CString        m_strProxyPassword;

    // ── Connection phase (the socket itself is the PAL transport below) ───
    int           m_iConnectPhase    = eConnectNotConnected;

    // ── PAL: the byte-transport seam (ITransport T3a; mirrors doc.h) ──────
    // mushcore drives the SAME POSIX transport adapter as the mac product.
    // m_pTransportEvents outlives every transport (destroyed with the doc
    // via TeardownTransport).
    pal::ITransport*     m_pTransport       = nullptr;
    CDocTransportEvents* m_pTransportEvents = nullptr;
    // The "socket exists" half of the classic connectivity test (doc.h twin;
    // this header is never compiled for Windows, so no #ifdef fork).
    bool ConnectionOpen (void) const { return m_pTransport != nullptr; }


    // ── Traffic counters ──────────────────────────────────────────────────
    long m_nBytesOut                 = 0;
    long m_nBytesIn                  = 0;
    long m_nTotalLinesReceived       = 0;
    long m_iInputPacketCount         = 0;
    __int64 m_iOutputPacketCount     = 0;    // count of packets sent (doc.h twin)
    bool m_bDebugIncomingPackets     = false; // packet-debug notepad wanted
    LONGLONG m_iCompressionTimeTaken = 0;    // time spent in MCCP inflate
    long m_nTotalCompressed          = 0;
    long m_nTotalUncompressed        = 0;

    // ── View / window number (1–10; 0 = not shown) ───────────────────────
    int  m_view_number               = 0;

    // ── Session statistics ────────────────────────────────────────────────
    long m_iTriggersMatchedThisSessionCount = 0;
    long m_iAliasesMatchedThisSessionCount  = 0;
    long m_iTimersFiredCount                = 0;  // total timers fired (all sessions)
    long m_iTimersFiredThisSessionCount     = 0;
    LONG m_maxlines = 5000;  // type converged to doc.h

    // ── Connect/disconnect timestamps ────────────────────────────────────
    CTime   m_tConnectTime;
    CTimeSpan m_tsConnectDuration;   // timespan we have been connected (doc.h twin)
    time_t  m_tWorldOpenTime = 0;  // when this world document was constructed

    // ── Disconnect behaviour ──────────────────────────────────────────────
    BOOL m_bShowConnectDisconnect = FALSE;  // type converged to doc.h
    bool    m_bDisconnectOK          = false;

    // ── Log file ──────────────────────────────────────────────────────────
    CString m_strAutoLogFileName;

    // ── World-level script callbacks ──────────────────────────────────────
    CString m_strWorldOpen;
    DISPID  m_dispidWorldOpen        = DISPID_UNKNOWN;
    CString m_strWorldClose;
    DISPID  m_dispidWorldClose       = DISPID_UNKNOWN;
    CString m_strWorldSave;
    DISPID  m_dispidWorldSave        = DISPID_UNKNOWN;
    CString m_strWorldConnect;
    DISPID  m_dispidWorldConnect     = DISPID_UNKNOWN;
    CString m_strWorldDisconnect;
    DISPID  m_dispidWorldDisconnect  = DISPID_UNKNOWN;
    CString m_strWorldGetFocus;
    DISPID  m_dispidWorldGetFocus    = DISPID_UNKNOWN;
    CString m_strWorldLoseFocus;
    DISPID  m_dispidWorldLoseFocus   = DISPID_UNKNOWN;

    // ── Line list + action list ────────────────────────────────────────
    CLineList   m_LineList;
    CActionList m_ActionList;

    // ── World name / address ──────────────────────────────────────────
    CString        m_mush_name;
    CString        m_server;       // host name / IP address
    unsigned short m_port          = 0;
    SOCKADDR_IN    m_sockAddr      = {};  // resolved MUD address (used by proxy)
    SOCKADDR_IN    m_ProxyAddr     = {};  // resolved proxy server address
    CString        m_strWorldID;   // unique UUID-style ID for this world
    CString        m_strSpeedWalkFiller;  // filler direction (default empty)

    // Note-colour state — the doc.h model (member names + types match doc.h so
    // the real note bodies can be single-sourced; campaign-b display step 1).
    // Port-preserving defaults: the headless twin always behaved as "notes in
    // RGB" with silver-on-black, so m_bNotesInRGB starts true here (doc.h's
    // Windows ctor starts false = palette mode; that ctor is not used headless).
    bool           m_bNotesInRGB = true;      // RGB mode vs custom-palette mode
    COLORREF       m_iNoteColourFore = RGB(192, 192, 192);  // RGB note text
    COLORREF       m_iNoteColourBack = RGB(0,   0,   0);    // RGB note background
    unsigned short m_iNoteTextColour = SAMECOLOUR;  // custom-palette index (unused while m_bNotesInRGB)
    bool           m_bNotesNotWantedNow = false;    // defer notes (telnet-negotiation callbacks)

    // ── Display-family surface (doc.h names/types; inert until the real
    //    display bodies are single-sourced — campaign-b display step 2) ────
    // Defaults chosen to preserve current headless behaviour when the real
    // bodies land: wrap/indent off, commands on their own line.  The flip
    // step must wire these to the loaded world options (like m_nWrapColumn).
    unsigned short m_bKeepCommandsOnSameLine = 0;   // commands share the prompt line
    unsigned short m_wrap = 0;                      // wrap long lines at m_nWrapColumn
    unsigned short m_indent_paras = 0;              // indent wrapped continuation lines
    unsigned short m_bDoNotTranslateIACtoIACIAC = 0; // don't double outgoing IAC
    long           m_gui_lines_emitted = 0;         // drain cursor (mushapp UpdateAllViews walk)
    CTime          m_tStatusDisplayed;              // when the status line was last shown
    // (m_DisplayFindInfo deferred to the flip commit — CFindInfo lives in
    //  stdafx.h, which this standalone header does not include)

    // ── Scripting callbacks (all DISPID_UNKNOWN → never fires) ────────
    DISPID m_dispidOnMXP_Start      = DISPID_UNKNOWN;
    DISPID m_dispidOnMXP_Stop       = DISPID_UNKNOWN;
    DISPID m_dispidOnMXP_OpenTag    = DISPID_UNKNOWN;
    DISPID m_dispidOnMXP_CloseTag   = DISPID_UNKNOWN;
    DISPID m_dispidOnMXP_SetVariable= DISPID_UNKNOWN;
    DISPID m_dispidOnMXP_Error      = DISPID_UNKNOWN;

    CString m_strOnMXP_Start;
    CString m_strOnMXP_Stop;
    CString m_strOnMXP_OpenTag;
    CString m_strOnMXP_CloseTag;
    CString m_strOnMXP_SetVariable;
    CString m_strOnMXP_Error;

    // ── Plugin process flags ──────────────────────────────────────────
    bool m_bPluginProcessesOpenTag    = false;
    bool m_bPluginProcessesCloseTag   = false;
    bool m_bPluginProcessesSetVariable= false;
    bool m_bPluginProcessesSetEntity  = false;
    bool m_bPluginProcessesError      = false;

    // ── Compression (zlib) ─────────────────────────────────────────────
    bool           m_bCompressInitOK     = false;
    bool           m_bCompress           = false;
    bool           m_bSupports_MCCP_2    = false;
    unsigned short m_bDisableCompression = 0;
    z_stream       m_zCompress           = {};
    Bytef*         m_CompressInput       = nullptr;
    Bytef*         m_CompressOutput      = nullptr;
    long           m_nCompressionOutputBufferSize = 32768;
    int            m_iMCCP_type          = 0;

    // ── Terminal type (MTTS) ───────────────────────────────────────────
    int     m_ttype_sequence           = 0;
    CString m_strTerminalIdentification;

    // ── SSL ────────────────────────────────────────────────────────────
    bool     m_bSSL_Connected = false;
    SSL*     m_pSSL           = nullptr;
    SSL_CTX* m_pSSL_CTX       = nullptr;
    CString  m_strSSLLastError;
    bool     m_bSSLAllowInvalidCert  = false;  // user consent, session only
    unsigned short m_bEnableAutoSay = 0;  // type converged to doc.h

    // ── UTF-8 ──────────────────────────────────────────────────────────
    unsigned short m_bUTF_8          = 0;
    unsigned char  m_UTF8Sequence[5] = {};
    int            m_iUTF8BytesLeft  = 0;
    char           m_cLastChar       = 0;

    // ── Font (GUI; always nullptr on macOS) ───────────────────────────
    CFont* m_font[16] = {};

    // ── Plugin pointer ────────────────────────────────────────────────
    CPlugin* m_CurrentPlugin = nullptr;

    // ── Plugin processing re-entrancy guards ──────────────────────────
    bool m_bPluginProcessingSend = false;
    bool m_bPluginProcessingSent = false;

    // ── Plugin list (loaded from the world; walked by doc_portable.cpp) ────
    CPluginList m_PluginList;

    // ── Alias / trigger options ────────────────────────────────────────
    unsigned short m_enable_aliases     = 1;
    unsigned short m_enable_speed_walk  = 0;
    CString        m_speed_walk_prefix;
    unsigned short m_iSpeedWalkDelay    = 0;
    CTimerWnd*     m_pTimerWnd          = nullptr;  // doc.h:1379 — speed-walk delay timer (inert headless)
    unsigned short m_enable_command_stack   = 0;
    CString        m_strCommandStackCharacter;

    // ── Command echoing and queuing ───────────────────────────────────
    unsigned short m_display_my_input   = 1;  // echo typed commands
    CStringList    m_QueuedCommandsList;       // speed-walk queue
    short          m_log_input          = 0;
    unsigned short m_bTranslateGerman = 0;  // type converged to doc.h
    long           m_new_lines          = 0;  // unseen line count
    CStringList    m_CommandHistory;           // typed-command history
    CString        m_CurrentCommand;           // command being typed
    LONG           m_nHistoryLines      = 500; // max command-history entries
    unsigned short m_bEscapeDeletesInput = 0;  // Esc clears the input bar
    unsigned short m_bCopySelectionToClipboard = 0;  // auto-copy selection
    unsigned short m_bUnpauseOnSend     = 1;   // cancel pause on send
    BOOL           m_bAutoWrapWindowWidth = FALSE; // wrap to window width

    void AddToCommandHistory(const CString& cmd) {
        if (!cmd.IsEmpty()) {
            m_CommandHistory.AddTail(cmd);
            // cap history at m_nHistoryLines entries (min 50)
            long cap = m_nHistoryLines > 50 ? m_nHistoryLines : 500;
            while (m_CommandHistory.GetCount() > (size_t)cap)
                m_CommandHistory.RemoveHead();
        }
    }

    // ── Spam prevention ───────────────────────────────────────────────
    unsigned short m_bEnableSpamPrevention = 0;  // type converged to doc.h
    unsigned short m_iSpamLineCount = 0;  // type converged to doc.h
    CString m_strSpamMessage;
    CString m_strLastCommandSent;
    int     m_iLastCommandCount     = 0;

    // ── Status message ────────────────────────────────────────────────
    CString m_strStatusMessage;

    // ── Chat connections (MUD chat protocol sockets) ──────────────────
    CChatSocketList m_ChatList;

    // ── World notes & tracing ─────────────────────────────────────────
    CString         m_notes;
    bool            m_bTrace               = false;

    // ── Behaviour options not in doc.h subset above ───────────────────
    // Reached through the real OptionsTable machinery (scriptingoptions.cpp)
    // by offset — types must stay doc.h-exact.
    unsigned short  m_bEnableScripts            = 1;    // enable scripting
    unsigned short  m_bAutoFreeze               = 1;    // freeze when not at bottom
    unsigned short  m_enable_beeps              = 1;    // play BEL character
    unsigned short  m_bCarriageReturnClearsLine = 0;    // \r wipes current line
    unsigned short  m_bSendEcho                 = 0;    // send echo negotiation

    // ── Scripting state ───────────────────────────────────────────────
    unsigned short  m_iCurrentActionSource = eUnknownActionSource;
    unsigned short  m_iNoteStyle           = NORMAL;
    LONGLONG        m_iScriptTimeTaken     = 0;
    unsigned short m_bScriptErrorsToOutputWindow = 0;  // type converged to doc.h
    unsigned short m_bLogScriptErrors = 0;  // type converged to doc.h
    CScriptEngine*  m_ScriptEngine         = nullptr;

    // ── Trigger evaluation control ────────────────────────────────────
    unsigned short         m_iStopTriggerEvaluation = eKeepEvaluatingTriggers;
    bool                   m_bLineOmittedFromOutput  = false;
    bool                   m_bInSendToScript         = false;
    int                    m_iExecutionDepth         = 0;
    bool                   m_bDoingSimulate          = false;
    bool                   m_bPluginProcessingCommand = false; // plugin is doing ON_PLUGIN_COMMAND (doc.h:1390)
    __int64                m_iUniqueDocumentNumber   = 0;   // doc.h:1372 (SendTo eSendToNotepad)
#ifndef MAX_EXECUTION_DEPTH
#  define MAX_EXECUTION_DEPTH 20
#endif
    long                   m_newlines_received       = 0;
    std::deque<string>     m_sRecentLines;
    std::list<CPaneStyle>  m_OutstandingLines;

    // ── Log file ──────────────────────────────────────────────────────
    FILE*          m_logfile                  = nullptr;
    unsigned short m_bWriteWorldNameToLog = 0;  // type converged to doc.h
    CString        m_logfile_name;
    short          m_bLogOutput               = 0;
    short          m_bLogNotes                = 0;
    BOOL           m_bLogInColour             = FALSE;
    unsigned short m_bLogHTML                 = 0;
    BOOL           m_bLogRaw                  = FALSE;
    CTime          m_LastFlushTime;                     // for periodic log flush in CheckTimers
    CString        m_strLogFilePreamble;
    CString        m_strLogFilePostamble;
    CString        m_strLogLinePreambleInput;
    CString        m_strLogLinePostambleInput;
    CString        m_strLogLinePreambleOutput;
    CString        m_strLogLinePostambleOutput;
    CString        m_strLogLinePreambleNotes;
    CString        m_strLogLinePostambleNotes;

    // ── Pueblo ────────────────────────────────────────────────────────
    BOOL           m_bPueblo                  = FALSE;
    CString        m_iPuebloLevel;
    CString        m_strPuebloMD5;

    // ── Map failure regexp ────────────────────────────────────────────
    t_regexp*      m_MapFailureRegexp          = nullptr;
    unsigned short m_bMapFailureRegexp = 0;  // type converged to doc.h
    CString        m_strMappingFailure;

    // ── Sound / active views ──────────────────────────────────────────
    unsigned short m_enable_trigger_sounds     = 0;
    CSendView*     m_pActiveCommandView        = nullptr;
    CMUSHView*     m_pActiveOutputView         = nullptr;

    // ── Alias / trigger evaluation counters ───────────────────────────
    unsigned short m_enable_triggers              = 1;
    long           m_iAliasesEvaluatedCount       = 0;
    long           m_iAliasesMatchedCount         = 0;
    long           m_iTriggersEvaluatedCount      = 0;
    long           m_iTriggersMatchedCount        = 0;
    bool           m_bOmitFromCommandHistory      = false;

    // ── Line positions array (for fast scroll; not used on non-Windows) ──
    POSITION*      m_pLinePositions               = nullptr;

    // ── UTF-8 error tracking ──────────────────────────────────────────────
    long           m_iUTF8ErrorCount              = 0;

    // ── Macros (function-key bindings) ────────────────────────────────
    CString        m_macros     [MACRO_COUNT];
    unsigned short m_macro_type [MACRO_COUNT] = {};
    CString        m_macro_name [MACRO_COUNT];

    // numeric keypad sends (doc.h:740) — saved by Save_Keypad_XML
    CString        m_keypad [eKeypad_Max_Items];

    // print styles (doc.h:812-813) — saved by Save_Printing_XML.
    // FONT_NORMAL (0) everywhere headless, as after the Windows ctor.
    int m_nNormalPrintStyle [8] = {};
    int m_nBoldPrintStyle [8] = {};

    // root-level include files (doc.h:1382) — saved by Save_Includes_XML
    CStringList m_strIncludeFileList;

    // true if we have loaded from disk (doc.h:1073) — set by the
    // Serialize_World_XML loading branch (dead headless; LoadWorldFromFile
    // is the portable load path)
    bool m_bLoaded = false;

    // ── Script language / file / inline text ──────────────────────────
    CString        m_strLanguage;
    CString        m_strScriptFilename;  // external .lua file (world attribute)
    CString        m_strScript;          // accumulated inline <script> content

    // ── Script file auto-reload (non-Windows mtime polling) ───────────
    time_t         m_scriptFileMtime        = 0;     // mtime recorded at last load
    bool           m_bInScriptFileChanged   = false; // re-entrancy guard

    // ── Timer enable ───────────────────────────────────────────────────
    unsigned short m_bEnableTimers = 1;

    // ── Window titles (SetTitle / SetMainTitle) ───────────────────────
    CString        m_strWindowTitle;
    CString        m_strMainWindowTitle;

    // ── CDocument-compatible accessors (inline — no layout change) ────
    // The Windows CMUSHclientDoc inherits GetPathName/GetTitle/IsModified
    // from CDocument.  In the portable stub we provide them as inline
    // methods over existing fields.
    CString GetPathName()  const { return m_strPathName; }
    CString GetTitle()     const { return m_mush_name; }
    BOOL    IsModified()   const { return FALSE; }  // not tracked portably
    void    SetPathName(const CString& path) { m_strPathName = path; }
    void    SetTitle(const CString& title)   { m_mush_name = title; }

    // ── Constructor / destructor / move ──────────────────────────────
    CMUSHclientDoc();
    ~CMUSHclientDoc();   // defined in doc_portable.cpp; fires OnPluginClose

    // Restore the move constructor that the user-defined destructor would
    // otherwise suppress.  The tests' make_doc() returns CMUSHclientDoc by
    // value; NRVO always elides the move there, so the "raw pointer member
    // is not zeroed in the moved-from object" issue never manifests in
    // practice.  CMUSHclientDoc objects are never actually moved at runtime.
    CMUSHclientDoc(CMUSHclientDoc&&) = default;

    // ── ANSI parser methods ───────────────────────────────────────────
    void    InterpretANSIcode(const int iCode);
    void    Interpret256ANSIcode(const int iCode);
    void    GetStyleRGB(CStyle* pStyle,
                        COLORREF& colour1, COLORREF& colour2) const;
    CStyle* AddStyle(const unsigned short iFlags,
                     const COLORREF iForeColour, const COLORREF iBackColour,
                     const int iLength = 0, CAction* pAction = nullptr,
                     CLine* pLine = nullptr);
    CStyle* AddStyle(const unsigned short iFlags,
                     const COLORREF iForeColour, const COLORREF iBackColour,
                     const int iLength = 0,
                     CString strAction = "", CString strHint = "",
                     CString strVariable = "",
                     CLine* pLine = nullptr);
    void RememberStyle(const CStyle* pStyle);

    // ── Telnet phase methods ───────────────────────────────────────────
    void Phase_ESC   (const unsigned char c);
    void Phase_ANSI  (const unsigned char c);
#ifndef _WIN32
    void Phase_OSC   (const unsigned char c);  // OSC discard (all platforms)
#endif
    void Phase_IAC   (unsigned char& c);
    void Phase_WILL  (const unsigned char c);
    void Phase_WONT  (const unsigned char c);
    void Phase_DO    (const unsigned char c);
    void Phase_DONT  (const unsigned char c);
    void Phase_SB    (const unsigned char c);
    void Phase_SUBNEGOTIATION    (const unsigned char c);
    void Phase_SUBNEGOTIATION_IAC(const unsigned char c);
    void Phase_COMPRESS          (const unsigned char c);
    void Phase_COMPRESS_WILL     (const unsigned char c);
    void Phase_UTF8              (const unsigned char c);
    void OutputBadUTF8characters ();
    void Handle_IAC_GA();
    bool Handle_Telnet_Request(const int iNumber, const string sType);
    void Handle_TELOPT_COMPRESS2();
    void Handle_TELOPT_MUD_SPECIFIC();
    void Handle_TELOPT_MXP();
    void Handle_TELOPT_TERMINAL_TYPE();
    void Handle_TELOPT_CHARSET();
    void Send_IAC_DO  (const unsigned char c);
    void Send_IAC_DONT(const unsigned char c);
    void Send_IAC_WILL(const unsigned char c);
    void Send_IAC_WONT(const unsigned char c);

    // ── MXP phase methods (mxp_phases.cpp) ───────────────────────────
    void Phase_MXP_ELEMENT        (const unsigned char c);
    void Phase_MXP_COMMENT        (const unsigned char c);
    void Phase_MXP_QUOTE          (const unsigned char c);
    void Phase_MXP_ENTITY         (const unsigned char c);
    void Phase_MXP_COLLECT_SPECIAL(const unsigned char c);

    // ── MXP core methods ─────────────────────────────────────────────
    void    MXP_collected_element();
    void    MXP_collected_entity();
    void    MXP_unterminated_element(const char* pReason);
    void    MXP_error(const int iLevel, const long iMessageNumber,
                      CString strMessage);
    void    MXP_Definition(CString strTag);
    void    MXP_Element(CString strName, CString strTag);
    void    MXP_Entity (CString strName, CString strTag);
    void    MXP_Attlist(CString strName, CString strTag);
    void    MXP_StartTag(CString strTag);
    void    MXP_EndTag  (CString strTag);
    void    MXP_CloseTag(CString strTag, const bool bOpen = false);
    void    MXP_CloseOpenTags();
    void    MXP_CloseAllTags();
    void    MXP_CloseAtomicTag(const int iAction, const CString& strText,
                               const POSITION firstlinepos,
                               const POSITION firststylepos);
    void    MXP_OpenAtomicTag(const CString strTag, int iAction,
                              CStyle* pStyle,
                              CString& strAction, CString& strHint,
                              CString& strVariable,
                              CArgumentList& ArgumentList);
    CString MXP_GetEntity(CString& strName);
    bool    MXP_StartTagScript(const CString& strName,
                               const CString& strArguments,
                               CArgumentList& ArgumentList);

    // ── MXP inline helpers ─────────────────────────────────────────────
    inline bool MXP_Secure() const {
        return m_iMXP_mode == eMXP_secure ||
               m_iMXP_mode == eMXP_secure_once ||
               m_iMXP_mode == eMXP_perm_secure;
    }
    inline bool MXP_Open() const {
        return m_iMXP_mode == eMXP_open ||
               m_iMXP_mode == eMXP_perm_open;
    }
    inline void MXP_Restore_Mode() {
        if (m_iMXP_mode == eMXP_secure_once)
            m_iMXP_mode = m_iMXP_previousMode;
    }

    // ── MXP On/Off (mxpOnOff.cpp) ─────────────────────────────────────
    void MXP_On (const bool bPueblo = false, const bool bManual = false);
    void MXP_Off(const bool bCompletely = false);

    // ── MXP mode change (mxpMode.cpp) ─────────────────────────────────
    void MXP_mode_change(int iNewMode);

    // ── Mapping helpers (no-ops in stub) ──────────────────────────────
    void DrawMappingStatusLine() {}

    // ── Line / display helpers ────────────────────────────────────────
    // StartNewLine: the real body, single-sourced in doc_shared.cpp (step 7).
    void StartNewLine(bool hard_break, int flags);
    void SetNewLineColour(const int flags);   // real body in doc_shared.cpp (display step 6)

    // ── Option machinery — the REAL bodies (scriptingoptions.cpp, option
    // convergence).  Signatures mirror doc.h:2507-2525 + 2906/2908/2929-2931
    // exactly (no default arguments there, none here).
    long GetOptionIndex(LPCTSTR OptionName);
    long GetOptionItem(const int iItem);
    long SetOptionItem(const int iItem, long Value,
                       const bool bDoSpecial, const bool bInclude);
    void SetDefaults(const bool bDoSpecial);
    long GetOption(LPCTSTR OptionName);
    long SetOption(LPCTSTR OptionName, long Value);
    VARIANT GetOptionList();

    long GetAlphaOptionIndex(LPCTSTR OptionName);
    CString GetAlphaOptionItem(const int iItem);
    long SetAlphaOptionItem(const int iItem, LPCTSTR sValue,
                            const bool bDoSpecial, const bool bInclude);
    void SetAlphaDefaults(const bool bDoSpecial);
    VARIANT GetAlphaOption(LPCTSTR OptionName);
    long SetAlphaOption(LPCTSTR OptionName, LPCTSTR Value);
    VARIANT GetAlphaOptionList();

    void AllocateConfigurationArrays(void);
    void DeleteConfigurationArrays(void);
    void ResetConfigurationArrays(void);

    // ── View fix-ups reached from Set(Alpha)OptionItem's bDoSpecial path.
    // View-side; no-op headless — except FixUpOutputBuffer, whose Windows
    // body (doc.cpp:1891) is doc-side work: trim m_LineList and rebuild
    // m_pLinePositions.  Headless that is exactly AllocateLinePositions()
    // (SetBaseOptionItem already stored the new m_maxlines before the
    // special runs).
    BOOL FixUpOutputBuffer(int /*nNewBufferSize*/) {
        if (!m_pCurrentLine)
            return FALSE;             // too early for this crap (doc.cpp)
        AllocateLinePositions();
        return TRUE;
    }
    void FixInputWrap() {}            // view-side; no-op headless
    void ChangeFont(const int /*nHeight*/, const char* /*lpszFacename*/,
                    const int /*nWeight*/, const DWORD /*iFontCharset*/,
                    const BOOL /*bShowBold*/, const BOOL /*bShowItalic*/,
                    const BOOL /*bShowUnderline*/,
                    const unsigned short /*iLineSpacing*/) {}  // view-side; no-op headless
    void ChangeInputFont(const int /*nHeight*/, const char* /*lpszFacename*/,
                         const int /*nWeight*/, const DWORD /*iCharset*/,
                         const BYTE /*bItalic*/) {}            // view-side; no-op headless

    // ── MXP argument helpers (mxputils.cpp) ──────────────────────────
    bool BuildArgumentList(CArgumentList& ArgumentList, CString strTag);
    CAction* GetAction(const CString& strAction, const CString& strHint,
                       const CString& strVariable);
    void CheckArgumentsUsed(const CString strTag, CArgumentList& ArgumentList);

    // ── Alias / trigger / timer evaluation ────────────────────────────
    // Implemented in evaluate.cpp / ProcessPreviousLine.cpp (in mushcore).
    BOOL EvaluateCommand(const CString& full_input,
                         bool bCountThem,
                         bool& bOmitFromLog,
                         bool bTest = false);   // default converged to doc.h:2134
    bool ProcessPreviousLine();
    void ProcessOneTriggerSequence(CString& strCurrentLine,
                                   CPaneLine& StyledLine,
                                   CString& strResponse,
                                   POSITION prevpos,
                                   bool& bNoLog,
                                   bool& bNoOutput,
                                   bool& bChangedColour,
                                   CTriggerList& triggerList,
                                   CString& strExtraOutput,
                                   ScriptItemMap& mapDeferredScripts,
                                   OneShotItemMap& mapOneShotItems);
    bool ProcessOneAliasSequence(const CString strCurrentLine,
                                 bool bCountThem,
                                 bool& bOmitFromLog,
                                 bool& bEchoAlias,
                                 CAliasList& AliasList,
                                 OneShotItemMap& mapOneShotItems);
    bool ExecuteAliasScript(CAlias* alias_item,
                            const CString strCurrentLine);

    // ── SendMsg / SendTo — real implementations in doc_portable.cpp ──────
    void SendMsg(CString strText, bool bEchoIt, bool bQueueIt, bool bLogIt);

    // ── Display-family decls (doc.h signatures; definitions arrive when the
    //    real bodies are single-sourced — campaign-b display steps 5-7) ─────
    void Tell(LPCTSTR Message);
    void SetUpVariantString (VARIANT& tVariant, const CString& strContents);
    void SetUpVariantShort  (VARIANT& tVariant, const short iContents);
    void SetUpVariantBool   (VARIANT& tVariant, const BOOL iContents);
    void SetUpVariantLong   (VARIANT& tVariant, const long iContents);
    void SetUpVariantDate   (VARIANT& tVariant, const COleDateTime iContents);
    void SetUpVariantDouble (VARIANT& tVariant, const double dContents);
    // world.* clipboard/infobar/logging/sending/worlds families (Campaign C)
    BSTR GetClipboard();
    void SetClipboard(LPCTSTR Text);
    void ShowInfoBar(BOOL Visible);
    void InfoClear();
    void Info(LPCTSTR Message);
    void InfoFont(LPCTSTR FontName, short Size, short Style);
    void InfoColour(LPCTSTR Name);
    void InfoBackground(LPCTSTR Name);
    long WriteLog(LPCTSTR Message);
    BOOL IsLogOpen();
    long FlushLog();
    long Send(LPCTSTR Message);
    long SendImmediate(LPCTSTR Message);
    long SendNoEcho(LPCTSTR Message);
    void AddToCommandHistory(LPCTSTR Message);   // LPCTSTR overload (doc.h); the
                                                 // CString inline above is a port helper
    long SendPush(LPCTSTR Message);
    long SendSpecial(LPCTSTR Message, BOOL Echo, BOOL Queue, BOOL Log, BOOL History);
    long LogSend(LPCTSTR Message);
    LPDISPATCH GetWorld(LPCTSTR WorldName);
    VARIANT GetWorldList();
    VARIANT GetWorldIdList();
    LPDISPATCH GetWorldById(LPCTSTR WorldID);
    // world.Database* (scripting/methods/methods_database.cpp — doc.h sigs)
    long DatabaseOpen(LPCTSTR Name, LPCTSTR Filename, long Flags);
    long DatabaseClose(LPCTSTR Name);
    long DatabasePrepare(LPCTSTR Name, LPCTSTR Sql);
    long DatabaseFinalize(LPCTSTR Name);
    long DatabaseColumns(LPCTSTR Name);
    long DatabaseStep(LPCTSTR Name);
    BSTR DatabaseError(LPCTSTR Name);
    BSTR DatabaseColumnName(LPCTSTR Name, long Column);
    long DatabaseColumnType(LPCTSTR Name, long Column);
    BSTR DatabaseColumnText(LPCTSTR Name, long Column);
    VARIANT DatabaseColumnValue(LPCTSTR Name, long Column);
    long DatabaseTotalChanges(LPCTSTR Name);
    long DatabaseChanges(LPCTSTR Name);
    BSTR DatabaseLastInsertRowid(LPCTSTR Name);
    VARIANT DatabaseList();
    VARIANT DatabaseInfo(LPCTSTR Name, long InfoType);
    long DatabaseExec(LPCTSTR Name, LPCTSTR Sql);
    VARIANT DatabaseColumnNames(LPCTSTR Name);
    VARIANT DatabaseColumnValues(LPCTSTR Name);
    long DatabaseReset(LPCTSTR Name);
    VARIANT DatabaseGetField(LPCTSTR Name, LPCTSTR Sql);
    long SetVariable(LPCTSTR VariableName, LPCTSTR Contents);
    VARIANT GetVariable(LPCTSTR VariableName);
    VARIANT GetVariableList();
    long DeleteVariable(LPCTSTR VariableName);
    // world.*Timer* family (methods_timers.cpp — doc.h signatures)
    long EnableTimer(LPCTSTR TimerName, BOOL Enabled);
    void ResetTimers();
    long ResetTimer(LPCTSTR TimerName);
    long AddTimer(LPCTSTR TimerName, short Hour, short Minute, double Second,
                  LPCTSTR ResponseText, long Flags, LPCTSTR ScriptName);
    long DeleteTimer(LPCTSTR TimerName);
    long IsTimer(LPCTSTR TimerName);
    VARIANT GetTimerList();
    long GetTimer(LPCTSTR TimerName, VARIANT FAR* Hour, VARIANT FAR* Minute,
                  VARIANT FAR* Second, VARIANT FAR* ResponseText,
                  VARIANT FAR* Flags, VARIANT FAR* ScriptName);
    VARIANT GetTimerInfo(LPCTSTR TimerName, short InfoType);
    long DoAfterSpecial(double Seconds, LPCTSTR SendText, short SendTo);
    long DoAfter(double Seconds, LPCTSTR SendText);
    long DoAfterSpeedWalk(double Seconds, LPCTSTR SendText);
    long DoAfterNote(double Seconds, LPCTSTR NoteText);
    long DeleteTemporaryTimers();
    long EnableTimerGroup(LPCTSTR GroupName, BOOL Enabled);
    long DeleteTimerGroup(LPCTSTR GroupName);
    VARIANT GetTimerOption(LPCTSTR TimerName, LPCTSTR OptionName);
    long SetTimerOption(LPCTSTR TimerName, LPCTSTR OptionName, LPCTSTR Value);
    // world.*Alias* family (methods_aliases.cpp — doc.h signatures)
    long DeleteAlias(LPCTSTR AliasName);
    long EnableAlias(LPCTSTR AliasName, BOOL Enabled);
    long GetAlias(LPCTSTR AliasName, VARIANT FAR* MatchText,
                  VARIANT FAR* ResponseText, VARIANT FAR* Parameter,
                  VARIANT FAR* Flags, VARIANT FAR* ScriptName);
    VARIANT GetAliasList();
    long IsAlias(LPCTSTR AliasName);
    long AddAlias(LPCTSTR AliasName, LPCTSTR MatchText, LPCTSTR ResponseText,
                  long Flags, LPCTSTR ScriptName);
    VARIANT GetAliasInfo(LPCTSTR AliasName, short InfoType);
    long DeleteTemporaryAliases();
    long EnableAliasGroup(LPCTSTR GroupName, BOOL Enabled);
    long DeleteAliasGroup(LPCTSTR GroupName);
    VARIANT GetAliasOption(LPCTSTR AliasName, LPCTSTR OptionName);
    long SetAliasOption(LPCTSTR AliasName, LPCTSTR OptionName, LPCTSTR Value);
    VARIANT GetAliasWildcard(LPCTSTR AliasName, LPCTSTR WildcardName);
    // world.*Trigger* family (methods_triggers.cpp — doc.h signatures)
    long DeleteTrigger(LPCTSTR TriggerName);
    long AddTrigger(LPCTSTR TriggerName, LPCTSTR MatchText, LPCTSTR ResponseText,
                    long Flags, short Colour, short Wildcard,
                    LPCTSTR SoundFileName, LPCTSTR ScriptName);
    long AddTriggerEx(LPCTSTR TriggerName, LPCTSTR MatchText, LPCTSTR ResponseText,
                      long Flags, short Colour, short Wildcard,
                      LPCTSTR SoundFileName, LPCTSTR ScriptName,
                      short SendTo, short Sequence);
    long EnableTrigger(LPCTSTR TriggerName, BOOL Enabled);
    long GetTrigger(LPCTSTR TriggerName, VARIANT FAR* MatchText,
                    VARIANT FAR* ResponseText, VARIANT FAR* Flags,
                    VARIANT FAR* Colour, VARIANT FAR* Wildcard,
                    VARIANT FAR* SoundFileName, VARIANT FAR* ScriptName);
    long IsTrigger(LPCTSTR TriggerName);
    VARIANT GetTriggerList();
    VARIANT GetTriggerInfo(LPCTSTR TriggerName, short InfoType);
    long DeleteTemporaryTriggers();
    long EnableTriggerGroup(LPCTSTR GroupName, BOOL Enabled);
    long DeleteTriggerGroup(LPCTSTR GroupName);
    VARIANT GetTriggerOption(LPCTSTR TriggerName, LPCTSTR OptionName);
    long SetTriggerOption(LPCTSTR TriggerName, LPCTSTR OptionName, LPCTSTR Value);
    VARIANT GetTriggerWildcard(LPCTSTR TriggerName, LPCTSTR WildcardName);
    void StopEvaluatingTriggers(BOOL AllPlugins);
    // world.*Speedwalk*/Queue family (methods_speedwalks.cpp — doc.h signatures)
    BSTR EvaluateSpeedwalk(LPCTSTR SpeedWalkString);
    BSTR ReverseSpeedwalk(LPCTSTR SpeedWalkString);
    long Queue(LPCTSTR Message, BOOL Echo);
    long DiscardQueue();
    short GetSpeedWalkDelay();
    void SetSpeedWalkDelay(short nNewValue);
    void OnInputDiscardqueuedcommands();
    void OnUpdateInputDiscardqueuedcommands(CCmdUI* pCmdUI);
    BSTR RemoveBacktracks(LPCTSTR Path);
    // auto-mapper family (methods_mapping.cpp — doc.h signatures)
    CString CalculateSpeedWalkString(const bool bOmitComments = false);
    long AddToMapper(LPCTSTR Direction, LPCTSTR Reverse);
    long AddMapperComment(LPCTSTR Comment);
    long GetMappingCount();
    VARIANT GetMappingItem(long Item);
    VARIANT GetMappingString();
    long DeleteLastMapItem();
    long DeleteAllMapItems();
    void EnableMapping(BOOL Enabled);
    // world.Array* family (methods_arrays.cpp — doc.h signatures)
    long ArrayCreate(LPCTSTR Name);
    long ArrayDelete(LPCTSTR Name);
    VARIANT ArrayListAll();
    long ArrayImport(LPCTSTR Name, LPCTSTR Values, LPCTSTR Delimiter);
    VARIANT ArrayGet(LPCTSTR Name, LPCTSTR Key);
    long ArraySet(LPCTSTR Name, LPCTSTR Key, LPCTSTR Value);
    VARIANT ArrayExport(LPCTSTR Name, LPCTSTR Delimiter);
    VARIANT ArrayListKeys(LPCTSTR Name);
    long ArrayClear(LPCTSTR Name);
    long ArrayDeleteKey(LPCTSTR Name, LPCTSTR Key);
    BOOL ArrayExists(LPCTSTR Name);
    BOOL ArrayKeyExists(LPCTSTR Name, LPCTSTR Key);
    VARIANT ArrayExportKeys(LPCTSTR Name, LPCTSTR Delimiter);
    long ArrayCount();
    long ArraySize(LPCTSTR Name);
    VARIANT ArrayGetFirstKey(LPCTSTR Name);
    VARIANT ArrayGetLastKey(LPCTSTR Name);
    VARIANT ArrayListValues(LPCTSTR Name);
    // colour family (methods_colours.cpp — doc.h signatures)
    long ColourNameToRGB(LPCTSTR Name);
    BSTR RGBColourToName(long Colour);
    BSTR ANSI(short Code);
    long AdjustColour(long Colour, short Method);
    void MapColour(long Original, long Replacement);
    long GetMapColour(long Which);
    VARIANT MapColourList();
    long PickColour(long Suggested);
    BSTR GetCustomColourName(short WhichColour);
    long SetCustomColourName(short WhichColour, LPCTSTR Name);
    long GetNormalColour(short WhichColour);
    void SetNormalColour(short WhichColour, long nNewValue);
    long GetBoldColour(short WhichColour);
    void SetBoldColour(short WhichColour, long nNewValue);
    long GetCustomColourText(short WhichColour);
    void SetCustomColourText(short WhichColour, long nNewValue);
    long GetCustomColourBackground(short WhichColour);
    void SetCustomColourBackground(short WhichColour, long nNewValue);
    const COLORREF TranslateColour(const COLORREF& source, const double opacity) const;
    DISPID GetProcedureDispid(const CString& strName, const CString& strType,
                              const CString& strLabel, CString& strErrorMessage);
    void TraceOut(LPCTSTR Message);
    BOOL GetTrace();
    void SetTrace(BOOL bNewValue);
    void OnGameTrace();
    bool SendToFirstPluginCallbacks(const std::string& sName, const char* sText);
    void NoteColourName(LPCTSTR Foreground, LPCTSTR Background);
    // the rest of the methods_noting.cpp surface (doc.h signatures, afx_msg dropped)
    short GetNoteColour();
    void SetNoteColour(short nNewValue);
    long GetNoteColourFore();
    void SetNoteColourFore(long nNewValue);
    long GetNoteColourBack();
    void SetNoteColourBack(long nNewValue);
    void NoteColourRGB(long Foreground, long Background);
    void AnsiNote(LPCTSTR Text);
    void Hyperlink_Helper(LPCTSTR Action, LPCTSTR Text, LPCTSTR Hint,
                          LPCTSTR TextColour, LPCTSTR BackColour,
                          BOOL URL, BOOL NoUnderline);
    void Hyperlink(LPCTSTR Action, LPCTSTR Text, LPCTSTR Hint,
                   LPCTSTR TextColour, LPCTSTR BackColour, BOOL URL);
    void NoteStyle(short Style);
    short GetNoteStyle();
    void NoteHr();
    void DoSendMsg(const CString& strText, const bool bEchoIt, const bool bLogIt);
    void StartNewLine_KeepPreviousStyle(const int flags);
    void SendLineToPlugin(void);
    void RemoveChunk(void);
    void ShowQueuedCommands(void);
    void AddToMap(CString /*str*/) {}   // auto-mapper accumulation: no-op headless
    void SendTo(unsigned short iWhere, CString strSendText,
                bool bOmitFromOutput, bool bOmitFromLog,
                CString strDescription, CString strVariable,
                CString& strOutput);

    // ── Connection helpers ─────────────────────────────────────────────
    BOOL CheckConnected();   // returns FALSE (0) if connected; non-zero if not
    BOOL ConnectSocket(void);   // single-sourced in doc_shared.cpp (T3c)
    long OpenLog(LPCTSTR LogFileName, BOOL Append);   // doc_shared.cpp (T3d)

    // ── Trigger execution (stub — scripting not wired yet) ────────────
    void ExecuteTriggerScript(CTrigger* pTrigger, CString strLine,
                              CPaneLine& StyledLine);
    void TriggerToPane       (CTrigger* pTrigger, CPaneLine& StyledLine);

    // ── Style-run finder (GUI-only) ──────────────────────────────────────
    bool FindStyle(const CLine* pLine, int iWanted, int& iCol,
                   CStyle*& pStyle, void*& oldpos) const { return false; }

    // ── Logging / tracing (no-ops on non-Windows) ─────────────────────
    void WriteToLog(const CString& sText);
    void WriteToLog(const char* pText, unsigned long nLen);
    void LogCommand(const char* sText);
    void Trace(const char* fmt, ...) __attribute__((format(printf,2,3)));

    // ── Output / display (no-ops) ──────────────────────────────────────
    void Screendraw(long /*iType*/, long /*bLog*/, const char* /*str*/) {}
    void OutputOutstandingLines();
    void LogLineInHTMLcolour(POSITION startpos);

    // ── View iteration (MFC-only; always returns null on non-Windows) ──
    POSITION GetFirstViewPosition() const { return nullptr; }
    class CView* GetNextView(POSITION& /*pos*/) const { return nullptr; }

    // ── Trigger / alias evaluation helpers (evaluate.cpp) ────────────
    CTrigger* EvaluateTrigger(const CString& input, CString& output,
                              int& iItem, int& iStartCol, int& iEndCol);
    CString FixSendText(const CString strSource, int iSendTo,
                        const t_regexp* regexp, const char* sLanguage,
                        bool bMakeWildcardsLower, bool bExpandVariables,
                        bool bExpandWildcards, bool bFixRegexps,
                        bool bIsRegexp, bool bThrowExceptions,
                        const char* sName);
    bool PlaySoundFile(CString strFile);   // doc.h signature (bool: did it play)
    void CancelSound();

    // ── Sound methods ── the REAL scripting/methods/methods_sounds.cpp is
    // compiled into mushcore (_NOSOUND fork: drives the g_wxhook_sound_*
    // miniaudio hooks; null hooks headless → eCannotPlaySound / -3).
    long Sound(LPCTSTR SoundFileName);
    long PlaySoundHelper(short Buffer, LPCTSTR FileName, BOOL Loop, double Volume, double Pan, LPCTSTR MemoryBuffer, size_t MemLen);
    long PlaySound(short Buffer, LPCTSTR FileName, BOOL Loop, double Volume, double Pan);
    long StopSound(short Buffer);
    long GetSoundStatus(short Buffer);

    // ── Command window / command handling ── the REAL
    // scripting/methods/methods_commands.cpp is compiled into mushcore.
    // The view-facing ones route through the IWorldView seam helpers
    // (GetCommandText & co) and no-op gracefully with no view attached.
    VARIANT GetCommandList(long Count);
    BSTR PushCommand();
    void SelectCommand();
    BSTR GetCommand();
    long SetCommand(LPCTSTR Message);
    BSTR PasteCommand(LPCTSTR Text);
    void DeleteCommandHistory();
    long Execute(LPCTSTR Command);
    long DoCommand(LPCTSTR Command);
    VARIANT GetInternalCommandsList();
    BOOL GetEchoInput();
    void SetEchoInput(BOOL bNewValue);
    VARIANT GetQueue();
    void SetInputFont(LPCTSTR FontName, short PointSize, short Weight, BOOL Italic);
    long SetCommandWindowHeight(short Height);
    long SetCommandSelection(long First, long Last);
    CChildFrame* GetChildFrame(void);   // stub def in doc_portable.cpp: no views → nullptr

    // ── Spellchecker family ── the REAL scripting/methods/methods_spellchecker.cpp
    // is compiled into mushcore (doc.h:3072-3123 signatures).  App.m_bSpellCheckOK
    // is never set headless, so the spellcheck ones return the canonical
    // not-active results; Metaphone/EditDistance are pure.
    VARIANT SpellCheck(LPCTSTR Text);
    long SpellCheckCommand(long StartCol, long EndCol);
    VARIANT SpellCheckDlg(LPCTSTR Text);
    long AddSpellCheckWord(LPCTSTR OriginalWord, LPCTSTR ActionCode, LPCTSTR ReplacementWord);
    BSTR Metaphone(LPCTSTR Word, short Length);
    long EditDistance(LPCTSTR Source, LPCTSTR Target);

    // ── Utilities family ── the REAL scripting/methods/methods_utilities.cpp
    // is compiled into mushcore (doc.h signatures; afx_msg dropped).  GUI
    // corners headless: Activate → IWorldView activateClient (no-op with no
    // view); Menu → g_wxhook_menu_dialog (null headless → ""); OpenBrowser →
    // ShellExecute stub → eCouldNotOpenFile; Help → inert ShowHelp/
    // ShowFunctionslist; SetFrameBackgroundColour → g_stub_frame only.
    BSTR StripANSI(LPCTSTR Message);
    void Activate();
    BSTR Replace(LPCTSTR Source, LPCTSTR SearchFor, LPCTSTR ReplaceWith, BOOL Multiple);
    BSTR FixupEscapeSequences(LPCTSTR Source);
    BSTR Trim(LPCTSTR Source);
    BSTR FixupHTML(LPCTSTR StringToConvert);
    VARIANT GenerateName();
    long ReadNamesFile(LPCTSTR FileName);
    VARIANT Base64Decode(LPCTSTR Text);
    VARIANT Base64Encode(LPCTSTR Text, BOOL MultiLine);
    BSTR CreateGUID();
    BSTR Hash(LPCTSTR Text);
    BSTR GetUniqueID();
    BSTR TranslateGerman(LPCTSTR Text);
    BSTR MakeRegularExpression(LPCTSTR Text);
    void MtSrand(long Seed);
    double MtRand();
    long ChangeDir(LPCTSTR Path);
    void SetStatus(LPCTSTR Message);
    LPDISPATCH Open(LPCTSTR FileName);
    long EnableGroup(LPCTSTR GroupName, BOOL Enabled);
    long DeleteGroup(LPCTSTR GroupName);
    void SetChanged(BOOL ChangedFlag);
    void Simulate(LPCTSTR Text);
    void ResetIP();
    long OpenBrowser(LPCTSTR URL);
    long SendPkt(LPCTSTR Packet);
    long TranslateDebug(LPCTSTR Message);
    void SetNotes(LPCTSTR Message);
    long Connect();
    long Disconnect();
    BOOL Save(LPCTSTR Name);
    void Help(LPCTSTR Name);
    BSTR Menu(LPCTSTR Items, LPCTSTR Default);
    void SetSelection(long StartLine, long EndLine, long StartColumn, long EndColumn);
    void SetFrameBackgroundColour(long Colour);

    // Save support (doc.h:2240 / doc.cpp:2152).  Headless there is no
    // Save-As dialog: an empty path fails, otherwise the canonical XML
    // saver (xml/xml_save_world.cpp) runs via OnSaveDocument.
    BOOL DoSave(LPCTSTR lpszPathName, BOOL /*bReplace*/) {
        CString strName(lpszPathName ? lpszPathName : "");
        if (strName.IsEmpty())
            return FALSE;                  // no dialog to prompt with
        if (!OnSaveDocument(strName))
            return FALSE;
        m_strPathName = strName;      // CDocument::DoSave keeps the new path
        return TRUE;
    }
    // doc.h:2695 / doc.cpp:1871 — the menu handler is just ConnectSocket().
    void OnConnectionConnect() { ConnectSocket(); }
    // Help display (doc.h; bodies in mushview.cpp / functionlist.cpp — GUI
    // only).  Headless world.Help shows nothing.
    void ShowHelp(LPCTSTR, LPCTSTR) {}
    void ShowFunctionslist(CString, bool) {}

    // ── Info family ── the REAL scripting/methods/methods_info.cpp is
    // compiled into mushcore (doc.h:2816-3161 signatures).  View-backed
    // GetInfo types use the file's own #ifndef _WIN32 arms (m_gui_* metrics,
    // IWorldView seams); toolbar/frame metrics are 0 headless.
    long GetLineCount();
    BOOL IsConnected();
    BSTR WorldName();
    BSTR Version();
    VARIANT GetInfo(long InfoType);
    long GetSentBytes();
    long GetReceivedBytes();
    long GetConnectDuration();
    double GetScriptTime();
    BSTR WorldAddress();
    long WorldPort();
    VARIANT GetLineInfo(long LineNumber, short InfoType);
    long GetLinesInBufferCount();
    VARIANT GetStyleInfo(long LineNumber, long StyleNumber, short InfoType);
    long GetSelectionStartLine();
    long GetSelectionEndLine();
    long GetSelectionStartColumn();
    long GetSelectionEndColumn();
    VARIANT GetHostAddress(LPCTSTR HostName);
    BSTR GetHostName(LPCTSTR IPaddress);
    BSTR GetWorldID();
    long GetSysColor(long Index);
    long GetSystemMetrics(long Index);
    long GetDeviceCaps(long Index);
    BSTR GetNotes();
    BSTR ErrorDesc(long Code);
    BSTR GetMainWindowPosition();
    BSTR GetWorldWindowPosition();
    VARIANT GetGlobalOption(LPCTSTR Name);
    VARIANT GetGlobalOptionList();
    class CMUSHView* GetFirstOutputWindow();   // no views headless → NULL
    // doc.h:2228 / doc.cpp:1974 — JUMP_SIZE line-position lookup.  Portable
    // body (byte-copy) in doc_portable.cpp until doc.cpp itself adopts.
    POSITION GetLinePosition(long nLine);
    // methods_output.cpp:175 — the non-Windows arm is pure IWorldView seam;
    // portable copy in doc_portable.cpp until methods_output.cpp adopts.
    BSTR GetWorldWindowPositionX(short Which);
    void Redraw() {}
    void DoFixMenus(void*) {}
    void ShowStatusLine(bool) {}

    // ── Timer methods (timers.cpp) ────────────────────────────────────
    void ResetOneTimer (CTimer* timer_item);
    void ResetAllTimers(CTimerMap& TimerMap);
    void CheckTimerList(CTimerMap& TimerMap);
    void CheckTimers   ();

    // ── Sorting (no-ops) ──────────────────────────────────────────────
    void SortAliases()  {}
    void SortTriggers() {}
    void SortTimers()   {}

    // ── FormatTime (doc.h semantics: %E/%N/%P/%F/%L + bFixHTML) ────────
    CString FormatTime(const CTime theTime, LPCTSTR pFormat, const bool bFixHTML);

    // ── Speedwalk ─────────────────────────────────────────────────────
    CString DoEvaluateSpeedwalk(const char* strInput);
    static CString DoReverseSpeedwalk(LPCTSTR SpeedWalkString);

    // ── Scripting ─────────────────────────────────────────────────────
    // After the Lua engine is created and script parsed, walk all
    // triggers/aliases/timers and resolve their strProcedure names to
    // Lua dispids so ExecuteLua can call them.
    void ResolveAllEntryPoints();
    bool CheckScriptingAvailable(const char*, int, const char*) { return false; }
    void OnScriptFileChanged(bool bForce = false);

    // ── Debug (world.Debug Lua API) ───────────────────────────────────────
    VARIANT Debug(LPCTSTR Command);
    void    DebugHelper(const CString strAction, CString strArgument);
    void    DebugShow(const char* sTitle, const char* sString);
    void    DebugShow(const char* sTitle, const long iNumber);
    void    DebugShow(const char* sTitle, const bool iBoolean);
    void    DebugShow(const char* sTitle, const CmcDateTime theDate);
    void    DebugShow(const char* sTitle, const CTime theDate);
    void    DebugShowD(const char* sTitle, const double fNumber);
    void    ShowOptions();
    void    ShowAlphaOptions();

    // ── World load/save ───────────────────────────────────────────────
    // Load_World_XML: doc.h:2001-2012 signature.  The REAL loader
    // (xml/xml_load_world.cpp) is NOT adopted yet — a strong no-op body
    // (returns 0) lives in windows_stubs.cpp; headless world loading goes
    // through LoadWorldFromFile (xml/xml_load_world_portable.cpp).
    UINT Load_World_XML(class CArchive& ar,
                        const unsigned long iMask,
                        const unsigned long iFlags = 0,
                        UINT * piTriggers = NULL,
                        UINT * piAliases = NULL,
                        UINT * piTimers = NULL,
                        UINT * piMacros = NULL,
                        UINT * piVariables = NULL,
                        UINT * piColours = NULL,
                        UINT * piKeypad = NULL,
                        UINT * piPrinting = NULL);
    // Save_World_XML: doc.h:1966-1968 signature.  The REAL saver
    // (xml/xml_save_world.cpp) is compiled into mushcore — world files,
    // plugin state files (CPlugin::SaveState) and ExportXML all write the
    // canonical Windows format.
    void Save_World_XML(class CArchive& ar,
                        const unsigned long iMask,
                        CString strComment = "");
    // The REAL Serialize_World_XML (xml/xml_serialize.cpp): storing →
    // ensure world ID + Save_World_XML (ar, ~0) + per-plugin SaveState.
    void Serialize_World_XML(class CArchive&);

    // ImportXMLFromString parses xml and adds items to this world
    // (xml/xml_load_world_portable.cpp).  Returns total items added, or -1
    // on parse error.  (The twin ExportXML_* helpers are gone — the real
    // ExportXML from scripting/methods/methods_xml.cpp replaced them.)
    long    ImportXMLFromString(const char* xml);

    // Portable world-file save.  Opens lpszPathName for writing, runs the
    // REAL Serialize_World_XML → Save_World_XML (ar, ~0) chain (exactly what
    // CDocument::OnSaveDocument → Serialize does on Windows / mushapp), and
    // returns TRUE on success.
    BOOL OnSaveDocument(LPCTSTR lpszPathName) {
        // Fire the on_world_save Lua callback and plugin callbacks before writing,
        // mirroring the DoSave() path in the Windows doc.cpp.
        FireWorldSave();
        SendToAllPluginCallbacks(ON_PLUGIN_WORLD_SAVE);

        CFile f;
        if (!f.Open(lpszPathName,
                    CFile::modeWrite | CFile::modeCreate | CFile::shareDenyWrite))
            return FALSE;
        BOOL ok = TRUE;
        try {
            CArchive ar(&f, CArchive::store);
            Serialize_World_XML(ar);
            ar.Close();
        } catch (...) {
            ok = FALSE;
        }
        f.Flush();
        f.Close();
        return ok;
    }

    void OnFileReloaddefaults();
    void OnUpdateFileReloaddefaults(CCmdUI* pCmdUI);
    // world.Get{Loaded,Default,Current}Value (methods_defaults.cpp — doc.h signatures)
    VARIANT GetLoadedValue(LPCTSTR OptionName);
    VARIANT GetDefaultValue(LPCTSTR OptionName);
    VARIANT GetCurrentValue(LPCTSTR OptionName);
    BOOL Load_Set(const int set_type, CString strFileName, CWnd* parent_window);

    // ── Logging helpers ───────────────────────────────────────────────
    bool LoggingInput() const { return m_log_input != 0; }

    // ── Plugin callback dispatch — the REAL bodies in plugins.cpp (CPlugin
    // convergence).  Signatures copied from doc.h:2340-2378 (the old
    // doc_portable.cpp twins are deleted).
    void SendToAllPluginCallbacks (const string & sName);   // no arguments
    bool SendToAllPluginCallbacks (const string & sName,
                                   const char * sText,   // one argument
                                   const bool bStopOnFalse = false);
    // CString overloads — delegate to the const char* version
    bool SendToAllPluginCallbacks (const string & sName,
                                   const CString & sText);   // 2-arg form
    bool SendToAllPluginCallbacks (const string & sName,
                                   const CString & sText,
                                   const bool bStopOnFalse);
    bool SendToAllPluginCallbacks (const string & sName,
                                   const CString & sText,
                                   const bool bStopOnTrue,
                                   const bool bStopOnFalse);
    // send a number and a string to all plugins, optionally stopping when one
    // returns true or false
    bool SendToAllPluginCallbacks (const string & sName,
                                   const long arg1,      // 2 arguments
                                   const string sText,
                                   const bool bStopOnTrue,
                                   const bool bStopOnFalse);
    // send two numbers and a string to all plugins
    bool SendToAllPluginCallbacks (const string & sName,
                                   const long arg1,      // 3 arguments
                                   const long arg2,
                                   const string sText,
                                   const bool bStopOnTrue,
                                   const bool bStopOnFalse);
    // send strResult to all plugins, allow them to modify it
    void SendToAllPluginCallbacksRtn (const string & sName, CString & strResult);

    // ── world.*Plugin* family (methods_plugins.cpp — doc.h:2950-3224
    // signatures, afx_msg dropped) + GetPlugin (doc.h:2336) ──────────────
    CPlugin * GetPlugin (LPCTSTR PluginID);
    VARIANT GetPluginVariable(LPCTSTR PluginID, LPCTSTR VariableName);
    BSTR GetPluginID();
    VARIANT GetPluginList();
    VARIANT GetPluginInfo(LPCTSTR PluginID, short InfoType);
    long LoadPlugin(LPCTSTR FileName);
    VARIANT GetPluginVariableList(LPCTSTR PluginID);
    VARIANT GetPluginTriggerList(LPCTSTR PluginID);
    VARIANT GetPluginAliasList(LPCTSTR PluginID);
    VARIANT GetPluginTimerList(LPCTSTR PluginID);
    VARIANT GetPluginTriggerInfo(LPCTSTR PluginID, LPCTSTR TriggerName, short InfoType);
    VARIANT GetPluginAliasInfo(LPCTSTR PluginID, LPCTSTR AliasName, short InfoType);
    VARIANT GetPluginTimerInfo(LPCTSTR PluginID, LPCTSTR TimerName, short InfoType);
    BOOL IsPluginInstalled(LPCTSTR PluginID);
    long SaveState();
    long CallPlugin(LPCTSTR PluginID, LPCTSTR Routine, LPCTSTR Argument);
    long PluginSupports(LPCTSTR PluginID, LPCTSTR Routine);
    long EnablePlugin(LPCTSTR PluginID, BOOL Enabled);
    BSTR GetPluginName();
    long ReloadPlugin(LPCTSTR PluginID);
    long BroadcastPlugin(long Message, LPCTSTR Text);
    VARIANT GetPluginTriggerOption(LPCTSTR PluginID, LPCTSTR TriggerName, LPCTSTR OptionName);
    VARIANT GetPluginAliasOption(LPCTSTR PluginID, LPCTSTR AliasName, LPCTSTR OptionName);
    VARIANT GetPluginTimerOption(LPCTSTR PluginID, LPCTSTR TimerName, LPCTSTR OptionName);
    long UnloadPlugin(LPCTSTR PluginID);

    // Plugin wizard (plugins.cpp:899).  Headless the stub CPluginWizardSheet's
    // DoModal returns IDCANCEL, so the body behaves as dialog-cancelled.
    void OnFilePluginwizard();

    // ── world.Udp* family (methods_udp.cpp — doc.h:3085-3103 signatures) ──
    long UdpSend(LPCTSTR IP, short Port, LPCTSTR Text);
    long UdpListen(LPCTSTR IP, short Port, LPCTSTR Script);
    VARIANT UdpPortList();
    long GetUdpPort(long First, long Last);

    // listening for UDP packets (doc.h:1351)
    map<int, UDPsocket *> m_UDPsocketMap;

    // ── XML-save family (doc.h:1970-1993 signatures) — the REAL bodies are
    // compiled into mushcore from xml/xml_save_world.cpp.
    void Save_Header_XML (class CArchive& ar, const char * sName, const bool bShowVersion = true);
    void Save_Footer_XML (class CArchive& ar, const char * sName);
    void Save_General_XML (class CArchive& ar);
    void Save_Triggers_XML (class CArchive& ar);
    void Save_One_Trigger_XML (class CArchive& ar, CTrigger * t);
    void Save_Aliases_XML (class CArchive& ar);
    void Save_One_Alias_XML (class CArchive& ar, CAlias * a);
    void Save_Variables_XML (class CArchive& ar);
    void Save_One_Variable_XML (class CArchive& ar, CVariable * v);
    void Save_Timers_XML (class CArchive& ar);
    void Save_One_Timer_XML (class CArchive& ar, CTimer * t);
    void Save_Macros_XML (class CArchive& ar);
    void Save_One_Macro_XML (class CArchive& ar, const int iIndex);
    void Save_Colours_XML (class CArchive& ar);
    void Save_Keypad_XML (class CArchive& ar);
    void Save_One_Keypad_XML (class CArchive& ar, const int iIndex);
    void Save_Printing_XML (class CArchive& ar);
    void Save_Version_And_Date_XML (class CArchive& ar);
    void Save_Includes_XML (class CArchive& ar);
    void Save_Plugins_XML (class CArchive& ar);
    void Save_World_Numeric_Options_XML (class CArchive& ar);
    void Save_World_Single_Line_Alpha_Options_XML (class CArchive& ar);
    void Save_World_Multi_Line_Alpha_Options_XML (class CArchive& ar);

    // ── XML import/export + entities (scripting/methods/methods_xml.cpp —
    // doc.h:2905/2993/3027/3036/3101-3102 signatures, afx_msg dropped) ────
    BSTR GetEntity(LPCTSTR Name);
    long ImportXML(LPCTSTR XML);
    BSTR ExportXML(short Type, LPCTSTR Name);
    void SetEntity(LPCTSTR Name, LPCTSTR Contents);
    BSTR GetXMLEntity(LPCTSTR Entity);
    void Reset();   // reset MXP (MXP_Off)

    void SendPacket    (const char* lpBuf, const int nBufLen);
    void SendPacket    (const unsigned char* lpBuf, const int nBufLen);
    void AddToLine     (const char* lpszText, const int flags);
    void SendWindowSizes(const int iNewWidth);

    bool    InitZlib(z_stream& zs);
    void    OnConnectionDisconnect();
    void    FireWorldClose();
    void    FireWorldSave();
    void    FireWorldGetFocus();
    void    FireWorldLoseFocus();
    void    PluginListChanged();
    void    DisplayMsg(const char* lpszText, int size,
                       const int flags, const bool fake = false);
    long    GetUniqueNumber();
    bool    AppendToTheNotepad(const CString strTitle, const CString strText,
                               const bool bReplace = false,
                               const int iNotepadType = eNotepadNormal);
    bool    SeeIfHandlerCanExecute(const CString& strName);
    CScriptEngine* GetScriptEngine();
    long    ShiftTabCompleteItem(LPCTSTR Item);
    // Shift+Tab completion — populated by world.ShiftTabCompleteItem() Lua API.
    std::set<std::string> m_ExtraShiftTabCompleteItems;
    bool                  m_bTabCompleteFunctions = false;
    bool    ExecuteScript(DISPID& dispid, const char* szProcedure,
                          const unsigned short iReason,
                          const char* szType, const char* szReason,
                          DISPPARAMS& params, long& nInvocationCount,
                          COleVariant* result = nullptr);

    // ── Scripting / error-reporting stubs ─────────────────────────────
    void Trace(const CString&) {}
    // ColourNote / ColourTell — real implementations in doc_portable.cpp
    // (output to terminal with optional ANSI colour codes).
    void ColourNote(const char* fg, const char* bg, const CString& text);
    void ColourNote(const char* fg, const char* bg, const char* text);
    void ColourTell(const char* fg, const char* bg, const char* text);
    void SetModifiedFlag(BOOL = TRUE) {}  // modification not tracked portably
    void ShowErrorLines(int) {}
    void WriteErrorLines(int, FILE*) {}

    // ── Networking methods (real implementations in doc_portable.cpp) ──────
    void OnBytesReceived(char* buff, int count);   // connected-data tail: counters/MCCP/DisplayMsg (T2b/c)
    void ConnectionEstablished();
    void ConnectionClosed(int nErrorCode);   // disconnect bookkeeping (doc_shared.cpp)
    void StartSSLHandshake();
    void SSLCleanup();
    long CloseLog();   // returns eOK / eLogFileNotOpen, as the full build (P1c)
    // SOCKS proxy helpers
    bool CheckExpectedProxyResponse(char cExpected, char cReceived);
    void SendProxyConnectSequence();
    bool SendProxyUserNameAndPassword();
    int  ProcessProxyResponse1(char* buff, int count);
    int  ProcessProxyResponse2(char* buff, int count);
    int  ProcessProxyResponse3(char* buff, int count);
    void OnConnect(int nErrorCode);
    CString GetSocketError(int nError);                             // doc_shared.cpp (T3b)
    void Debug_Packets(LPCTSTR sCaption, LPCTSTR lpszText, int size,
                       const __int64 iNumber);                      // no-op (doc_portable.cpp)
    bool LookupHostName(LPCTSTR sName);                 // doc_shared.cpp (T3a)
    void LookupHostNameComplete(struct in_addr addr, int phase, bool ok);
    void InitiateConnection(void);                       // doc_shared.cpp (T3c)
    // ITransport TLS outcomes + teardown (doc_shared.cpp; mirror doc.h)
    void OnTlsEstablished(const char* version, const char* cipher,
                          const char* verifyFailure);
    void OnTlsFailed(bool bCertFailure, const char* reason);
    void CreateTransport(void);
    void TeardownTransport(void);
    void Note(const CString& s);
    void Note(const char* s);
    void UpdateAllViews(void* hint);


    // ── PAL: typed output view ────────────────────────────────────────────
    // pal::IWorldView is the engine's typed per-world view seam.  All
    // display/state/pull traffic is typed-only (P2 complete): no view
    // attached → pushes are inert, pulls report empty/zero.  Headless output
    // still reaches stdout via the DisplayMsg/Note fallbacks in
    // doc_portable.cpp.  See docs/PAL.md.
    pal::IWorldView* m_pWorldView = nullptr;
    bool HaveOutputSink() const { return m_pWorldView != nullptr; }
    void EmitOutputLine(SOutputLine&& line) {
        if (m_pWorldView) m_pWorldView->appendLine(std::move(line));
    }
    void EmitPaused(bool paused) {
        if (m_pWorldView) m_pWorldView->setPaused(paused);
    }
    void EmitStatus(const char* text) {
        if (m_pWorldView) m_pWorldView->setStatus(text);
    }
    void EmitTitle(const char* title) {
        if (m_pWorldView) m_pWorldView->setTitle(title);
    }
    void EmitClearOutput() {
        if (m_pWorldView) m_pWorldView->clearOutput();
    }
    void EmitBackgroundColour(unsigned long col) {
        if (m_pWorldView) m_pWorldView->setBackgroundColour((pal::Colour)col);
    }
    void EmitMaxLines(int n) {
        if (m_pWorldView) m_pWorldView->setMaxLines(n);
    }
    void EmitRepaint() {
        if (m_pWorldView) m_pWorldView->requestRepaint(pal::Rect{});
    }
    // ── PAL: frontend → core reads (pulls) ────────────────────────────────
    bool HaveCommandSink() const { return m_pWorldView != nullptr; }
    std::string GetCommandText() {
        return m_pWorldView ? m_pWorldView->currentCommand() : std::string();
    }
    bool HaveScrollSink() const { return m_pWorldView != nullptr; }
    long GetScrollPos() {
        return m_pWorldView ? m_pWorldView->scrollPos() : 0;
    }
    void GetOutputSelection(long& sl, long& sc, long& el, long& ec) {
        sl = sc = el = ec = 0;
        if (m_pWorldView) {
            pal::TextRange r = m_pWorldView->outputSelection();
            sl = r.startLine; sc = r.startCol; el = r.endLine; ec = r.endCol;
        }
    }

  // ── PAL: command-input bar (typed-only — P2 command family; the wx frame
  //    implements these on IWorldView, no m_fn_* fallback remains) ──────────
  bool SetCommandText(const std::string& text) {
      return m_pWorldView ? m_pWorldView->setCommand(text.c_str()) : false;
  }
  void SelectCommandText() {
      if (m_pWorldView) m_pWorldView->selectCommand();
  }
  void GetCommandSelection(long& s, long& e) {
      s = e = 0;
      if (m_pWorldView) {
          pal::Range r = m_pWorldView->commandSelection();
          s = r.start; e = r.end;
      }
  }
  void SetCommandSelectionRange(long first, long last) {
      if (m_pWorldView) m_pWorldView->setCommandSelection(first, last);
  }
  std::string PushCommandText() {
      return m_pWorldView ? m_pWorldView->pushCommand() : std::string();
  }
  std::vector<std::string> GetCommandListEntries(long count) {
      return m_pWorldView ? m_pWorldView->commandList(count)
                          : std::vector<std::string>();
  }
  void AddCommandToHistory(const std::string& text) {
      if (m_pWorldView) m_pWorldView->addToCommandHistory(text.c_str());
  }
  void DeleteCommandHistoryEntries() {
      if (m_pWorldView) m_pWorldView->deleteCommandHistory();
  }
  void EmitInputFont(const std::string& face, int pts, bool bold, bool italic) {
      if (m_pWorldView) m_pWorldView->setInputFont(face.c_str(), pts, bold, italic);
  }

  // ── PAL: output-pane management (typed-only — P2 output family) ──────────
  void EmitNote(const std::string& text) {
      if (m_pWorldView) m_pWorldView->note(text.c_str());
  }
  void BeginOutputBatch() {
      if (m_pWorldView) m_pWorldView->beginBatch();
  }
  void EndOutputBatch() {
      if (m_pWorldView) m_pWorldView->endBatch();
  }
  void DeleteOutputLines(long n) {
      if (m_pWorldView) m_pWorldView->deleteLines(n);
  }
  void EmitScroll(long line) {
      if (m_pWorldView) m_pWorldView->setScroll(line);
  }
  void SetOutputSelectionRange(int sl, int el, int sc, int ec) {
      if (m_pWorldView) m_pWorldView->setOutputSelection(sl, el, sc, ec);
  }
  void EmitOutputFont(const std::string& face, int pts, bool bold, bool italic) {
      if (m_pWorldView) m_pWorldView->setOutputFont(face.c_str(), pts, bold, italic);
  }
  void EmitTextRectChanged() {
      if (m_pWorldView) m_pWorldView->textRectChanged();
  }
  // Fills the screen rect of the output pane; false (outputs untouched) when
  // no view is attached — callers keep their own defaults.
  bool GetOutputRect(int& x, int& y, int& w, int& h) {
      if (!m_pWorldView) return false;
      pal::Rect r = m_pWorldView->outputRect();
      x = r.x; y = r.y; w = r.w; h = r.h;
      return true;
  }

  // ── PAL: world/main window management (typed-only — P2 window family) ────
  void EmitMainTitle(const std::string& title) {
      if (m_pWorldView) m_pWorldView->setMainTitle(title.c_str());
  }
  void MoveWorldWindowRect(long l, long t, long w, long h) {
      if (m_pWorldView) m_pWorldView->moveWorldWindow(l, t, w, h);
  }
  void EmitWindowStatus(int status) {
      if (m_pWorldView) m_pWorldView->setWindowStatus(status);
  }
  void ActivateClientWindow() {
      if (m_pWorldView) m_pWorldView->activateClient();
  }
  bool IsClientFullscreen() const {
      return m_pWorldView && m_pWorldView->isFullscreen();
  }
  // world.Save via the view; true when no view is attached (headless hosts
  // treat Save as trivially successful, matching the old null-callback path).
  bool SaveWorldViaView() {
      return m_pWorldView ? m_pWorldView->saveWorld() : true;
  }
  // Fills the screen rect of the main frame; false (outputs untouched) when
  // no view is attached — callers keep their own defaults.
  bool GetMainRect(int& x, int& y, int& w, int& h) {
      if (!m_pWorldView) return false;
      pal::Rect r = m_pWorldView->mainRect();
      x = r.x; y = r.y; w = r.w; h = r.h;
      return true;
  }

  // ── PAL: the info bar (typed-only — P2 infobar family) ───────────────────
  void EmitInfobarShow(bool visible) {
      if (m_pWorldView) m_pWorldView->infobarShow(visible);
  }
  void EmitInfobarClear() {
      if (m_pWorldView) m_pWorldView->infobarClear();
  }
  void EmitInfobarText(const std::string& text) {
      if (m_pWorldView) m_pWorldView->infobarText(text.c_str());
  }
  void EmitInfobarColour(unsigned long c) {
      if (m_pWorldView) m_pWorldView->infobarColour((pal::Colour)c);
  }
  void EmitInfobarBackground(unsigned long c) {
      if (m_pWorldView) m_pWorldView->infobarBackground((pal::Colour)c);
  }
  void EmitInfobarFont(const std::string& face, int size, int style) {
      if (m_pWorldView) m_pWorldView->infobarFont(face.c_str(), size, style);
  }
  // Fills the info bar's pixel size; (0,0) when hidden or no view attached.
  void GetInfobarSize(int& w, int& h) {
      w = h = 0;
      if (m_pWorldView) {
          pal::Size s = m_pWorldView->infobarSize();
          w = s.w; h = s.h;
      }
  }


    // ── Output pane metrics (updated by CWorldChildFrame on font/size change) ─
    // Exposed to Lua via GetInfo(212), GetInfo(213), etc.
    int  m_gui_font_height = 18;   // pixel height of one line (GetInfo 212)
    int  m_gui_font_width  = 10;   // average pixel width of one char (GetInfo 213)
    int  m_gui_rows        = 24;   // visible rows in the output pane
    int  m_gui_cols        = 80;   // visible columns in the output pane
    // True while the output pane is "paused" (user scrolled up, auto-scroll
    // disengaged).  Updated by CWorldChildFrame via m_fn_autoscroll_changed.
    // Exposed via GetInfo(114) and toggled by world.Pause().
    bool m_gui_paused      = false;











    // ── Display step 7: the real line lifecycle ───────────────────────────
    // The current line lives IN m_LineList from creation (real doc.cpp
    // semantics); the old m_pScratchLine / m_bInStartNewLine / Term* stdout
    // machinery is gone.  AllocateLinePositions (doc_portable.cpp) builds the
    // JUMP_SIZE index at construction and on m_maxlines changes.
    void AllocateLinePositions();

    // Minimal stand-in for doc.h's CFindInfo m_DisplayFindInfo (stdafx.h owns
    // the full class; RemoveChunk only adjusts the current-line cursor).
    struct SDisplayFindInfoStub { long m_nCurrentLine = 0; };
    SDisplayFindInfoStub m_DisplayFindInfo;
};

// ── t_regexp — needed by scripting/scripting.h ────────────────────────────
#include "regexp.h"

// ── Full CScriptEngine declaration (needs CMUSHclientDoc + Lua headers) ──────
// Included here, after CMUSHclientDoc is fully defined.
#include "scripting/scripting.h"

#endif // !_WIN32
