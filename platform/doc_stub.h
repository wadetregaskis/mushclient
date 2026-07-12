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
typedef struct sockaddr_in SOCKADDR_IN;

using std::string;
using std::vector;
using std::list;
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
#  define OPT_PLUGIN_CANNOT_READ  0x000020
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
// spelled out here for scriptingoptions_portable.cpp.
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

// ── StringToVector (from stdafx.h) ────────────────────────────────────────
void StringToVector(const string s,
                    vector<string>& v,
                    const string delim,
                    bool include_delim);

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
class CWorldSocket;      // defined in worldsock.h; used as CMUSHclientDoc::m_pSocket

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
    void Invalidate() {}
    RECT GetTextRectangle() const { RECT r = {0,0,0,0}; return r; }
    CPoint GetScrollPosition() const { return CPoint(0, 0); }
};

class CSendView : public CView {
public:
    DECLARE_DYNCREATE(CSendView)
    void CheckForSelectionChange() {}
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

// ── CPlugin stub ─────────────────────────────────────────────────────────
// Fields accessed from lua_scripting.cpp, ProcessPreviousLine.cpp,
// evaluate.cpp, and the portable plugin loader.
struct CPlugin {
    // Identity
    CString        m_strName;
    CString        m_strAuthor;
    CString        m_strPurpose;
    CString        m_strDescription;
    CString        m_strID;              // 20-char hex unique ID
    CString        m_strSource;          // path to the plugin .xml file
    CString        m_strDirectory;       // directory of m_strSource
    // Scripting
    CString        m_strLanguage;        // "lua", "vbscript", etc.
    CString        m_strScript;          // accumulated inline <script> content
    CScriptEngine* m_ScriptEngine        = nullptr;
    // Version
    double         m_dVersion            = 0.0;
    double         m_dRequiredVersion    = 0.0;
    // Runtime state
    int            m_iSequence           = 0;
    BOOL           m_bEnabled            = TRUE;
    bool           m_bSendToScriptUsed   = false;
    bool           m_bSaveState          = false;
    LONGLONG       m_iScriptTimeTaken    = 0;
    // Per-plugin trigger/alias/timer/variable collections
    CTriggerMap    m_TriggerMap;
    CTriggerArray  m_TriggerArray;
    CTriggerRevMap m_TriggerRevMap;
    CAliasMap      m_AliasMap;
    CAliasArray    m_AliasArray;
    CAliasRevMap   m_AliasRevMap;
    CTimerMap      m_TimerMap;
    CTimerRevMap   m_TimerRevMap;
    CVariableMap   m_VariableMap;
    tStringMapOfMaps m_Arrays;           // scripting arrays (world.ArrayCreate etc.)
    // Back-pointer
    CMUSHclientDoc* m_pDoc               = nullptr;
    // Constructor / destructor
    explicit CPlugin(CMUSHclientDoc* pDoc = nullptr) : m_pDoc(pDoc) {}
    // Destructor declared here; defined in platform/plugin_portable.cpp so it
    // sees only the stub CPlugin layout (not the full plugins.h version).
    ~CPlugin();
};

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
#ifndef CMUSHclientApp_defined
#define CMUSHclientApp_defined
class CMUSHclientApp {
public:
    CAtomicElementMap  m_ElementMap;
    CMapStringToString m_EntityMap;
    CColoursMap        m_ColoursMap;

    // Scripting / sandboxing
    CString m_strLuaScript;              // sandbox script (empty on macOS — no sandbox)
    string  m_strDefaultLogFileDirectory;// prefix for log files

    // Performance counter frequency in ticks/second (nanoseconds on POSIX).
    // Non-zero once initialised; zero keeps all timing guards inactive.
    LONGLONG m_iCounterFrequency = 0;

    void MXP_LoadColours();
    void MXP_LoadEntities();
    void MXP_LoadElements();

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
class CMainFrame {
public:
    // Status bar
    CStatusBarStub  m_wndStatusBar;
    int             m_nPauseItem     = 0;

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

    // Layout query (returns empty rects on non-Windows)
    void ReturnStatusRect (CRect& r) const { r = CRect(); }
    void ReturnToolbarRect(CRect& r) const { r = CRect(); }

    operator HWND() const { return nullptr; }
};
#endif // CMainFrame_defined
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

long FindBaseOption(const char* OptionName,
                    tConfigurationNumericOption BaseOptionsTable[],
                    int& iItem);

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

// ── OptionsTable (empty stub) ─────────────────────────────────────────────
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

// ── Base64 (utilities_portable.cpp) ─────────────────────────────────────
CString decodeBase64(const char* coded);
CString encodeBase64(const char* plaintext, bool bMultiLine);

// ── Mersenne Twister RNG (scripting/mt19937ar.c) ──────────────────────
extern "C" {
    void   init_genrand(unsigned long seed);
    double genrand(void);
}

// ── Free function stubs ──────────────────────────────────────────────────
// These are defined in Utilities.cpp on Windows; stubs live in doc_portable.cpp.
CString FixHTMLString(CString s);

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

// CPluginList — list of CPlugin pointers (normally in plugins.h)
// CPlugin is defined above this point in doc_stub.h.
#ifndef CPluginList_defined
#define CPluginList_defined
typedef list<CPlugin*> CPluginList;
typedef CPluginList::iterator PluginListIterator;
#endif // CPluginList_defined

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

// ── Minimal CMUSHclientDoc ────────────────────────────────────────────────
class CMUSHclientDoc {
public:
    // ── Colour tables ──────────────────────────────────────────────────
    COLORREF m_normalcolour[8];
    COLORREF m_boldcolour[8];
    COLORREF       m_customtext[MAX_CUSTOM];
    COLORREF       m_customback[MAX_CUSTOM];
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
    unsigned short m_bNoEchoOff = 0;
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
    bool     m_bUseCustomLinkColour    = false;
    bool     m_bMudCanChangeLinkColour = false;
    bool     m_bUnderlineHyperlinks    = false;
    bool     m_bMudCanRemoveUnderline  = false;
    COLORREF m_iHyperlinkColour        = 0;

    // ── Colour / display options ──────────────────────────────────────────
    bool m_bIgnoreMXPcolourChanges     = false;
    bool m_bSuppressNewline            = false;

    // ── AFK / options ─────────────────────────────────────────────────────
    bool m_bSendMXP_AFK_Response       = false;
    bool m_bMudCanChangeOptions        = false;
    CTime m_tLastPlayerInput;

    // ── Connection / world identity ───────────────────────────────────────
    CString m_name;
    CString m_password;
    CString m_connect_text;       // text sent on connect
    CString m_strScriptPrefix;    // prefix to invoke scripts from command window
    CString m_strWorldFilePath;   // path to the loaded world file

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

    // ── Current hyperlink state (set by AddStyle, consumed by DisplayMsg) ─
    // Cleared automatically when the style with ACTION_HYPERLINK/ACTION_SEND
    // is replaced by one without an action.
    std::string m_current_action;       // action string for current MXP/Lua link
    std::string m_current_hint;         // hint/tooltip for current link
    COLORREF    m_current_style_fg = 0x00C0C0C0u;  // fg from most recent AddStyle (light grey)
    COLORREF    m_current_style_bg = 0x00000000u;  // bg from most recent AddStyle (black)

    // ── GUI ColourNote/ColourTell line buffer ──────────────────────────────
    // ColourTell segments accumulate here until ColourNote (or Note) flushes.
    // Enables multi-segment colour lines (ColourTell…ColourTell…ColourNote)
    // to arrive at the output pane as a single SOutputLine with multiple runs.
    SOutputLine m_gui_note_buf;

    int     m_connect_now      = 0;
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

    // ── Socket / connection phase (worldsock.cpp) ─────────────────────────
    CWorldSocket* m_pSocket          = nullptr;
    int           m_iConnectPhase    = eConnectNotConnected;

    // ── Traffic counters ──────────────────────────────────────────────────
    long m_nBytesOut                 = 0;
    long m_nBytesIn                  = 0;
    long m_nTotalLinesReceived       = 0;
    long m_iInputPacketCount         = 0;
    long m_nTotalCompressed          = 0;
    long m_nTotalUncompressed        = 0;

    // ── View / window number (1–10; 0 = not shown) ───────────────────────
    int  m_view_number               = 0;

    // ── Session statistics ────────────────────────────────────────────────
    long m_iTriggersMatchedThisSessionCount = 0;
    long m_iAliasesMatchedThisSessionCount  = 0;
    long m_iTimersFiredCount                = 0;  // total timers fired (all sessions)
    long m_iTimersFiredThisSessionCount     = 0;
    long m_maxlines                         = 5000;

    // ── Connect/disconnect timestamps ────────────────────────────────────
    CTime   m_tConnectTime;
    time_t  m_tWorldOpenTime = 0;  // when this world document was constructed

    // ── Disconnect behaviour ──────────────────────────────────────────────
    bool    m_bShowConnectDisconnect = false;
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

    // Note colour overrides (set by NoteColourRGB / NoteColourFore / Back)
    COLORREF       m_NoteColourFore = RGB(192, 192, 192);
    COLORREF       m_NoteColourBack = RGB(0,   0,   0);

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
    unsigned short m_enable_command_stack   = 0;
    CString        m_strCommandStackCharacter;

    // ── Command echoing and queuing ───────────────────────────────────
    unsigned short m_display_my_input   = 1;  // echo typed commands
    CStringList    m_QueuedCommandsList;       // speed-walk queue
    short          m_log_input          = 0;
    bool           m_bTranslateGerman   = false;
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
    bool    m_bEnableSpamPrevention = false;
    int     m_iSpamLineCount        = 0;
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
    // Kept in sync with GetOption/SetOption live-member lists in
    // lua_world_api_portable.cpp and with xml_load/save_world_portable.cpp.
    unsigned short  m_bEnableScripts            = 1;    // enable scripting
    unsigned short  m_bAutoFreeze               = 1;    // freeze when not at bottom
    unsigned short  m_enable_beeps              = 1;    // play BEL character
    unsigned short  m_bCarriageReturnClearsLine = 0;    // \r wipes current line
    unsigned short  m_bSendEcho                 = 0;    // send echo negotiation

    // ── Scripting state ───────────────────────────────────────────────
    unsigned short  m_iCurrentActionSource = eUnknownActionSource;
    unsigned short  m_iNoteStyle           = NORMAL;
    LONGLONG        m_iScriptTimeTaken     = 0;
    bool            m_bScriptErrorsToOutputWindow = false;
    bool            m_bLogScriptErrors     = false;
    CScriptEngine*  m_ScriptEngine         = nullptr;

    // ── Trigger evaluation control ────────────────────────────────────
    unsigned short         m_iStopTriggerEvaluation = eKeepEvaluatingTriggers;
    bool                   m_bLineOmittedFromOutput  = false;
    bool                   m_bInSendToScript         = false;
    int                    m_iExecutionDepth         = 0;
    bool                   m_bDoingSimulate          = false;
#ifndef MAX_EXECUTION_DEPTH
#  define MAX_EXECUTION_DEPTH 20
#endif
    long                   m_newlines_received       = 0;
    std::deque<string>     m_sRecentLines;
    std::list<CPaneStyle>  m_OutstandingLines;

    // ── Log file ──────────────────────────────────────────────────────
    FILE*          m_logfile                  = nullptr;
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
    bool           m_bMapFailureRegexp         = false;
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
    CString GetPathName()  const { return m_strWorldFilePath; }
    CString GetTitle()     const { return m_mush_name; }
    BOOL    IsModified()   const { return FALSE; }  // not tracked portably
    void    SetPathName(const CString& path) { m_strWorldFilePath = path; }
    void    SetTitle(const CString& title)   { m_mush_name = title; }

    // ── Option caches (portable GetOption / SetOption backing store) ──
    // Stores values for numeric/alpha options that don't have a dedicated
    // CMUSHclientDoc member in the portable build.  Pre-populated from
    // OptionsTable defaults by the constructor; updated by SetOption /
    // SetAlphaOption so that subsequent GetOption reads are consistent.
    unordered_map<string, long>   m_numericOptionCache;
    unordered_map<string, string> m_alphaOptionCache;

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
    void Phase_OSC   (const unsigned char c);  // OSC discard — CLI only
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
    // StartNewLine is implemented in doc_portable.cpp (fires triggers).
    void StartNewLine(bool hard_break, int flags);
    void SetNewLineColour(int)      {}

    // ── Option access stubs ───────────────────────────────────────────
    long GetOptionItem(int)                         { return 0; }
    long GetOption(const CString&)                  { return 0; }
    bool SetOptionItem(int, long, bool, bool)        { return false; }

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
                         bool bTest);
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
    void SendTo(unsigned short iWhere, CString strSendText,
                bool bOmitFromOutput, bool bOmitFromLog,
                CString strDescription, CString strVariable,
                CString& strOutput);

    // ── Connection helpers ─────────────────────────────────────────────
    BOOL CheckConnected();   // returns FALSE (0) if connected; non-zero if not
    void ConnectSocket();   // implemented in doc_portable.cpp

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
    void PlaySoundFile(CString strFile);
    void CancelSound();
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

    // ── FormatTime ────────────────────────────────────────────────────
    CString FormatTime(CTime t, const char* fmt, bool bUTC);

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
    void Load_World_XML(class CArchive&, unsigned long, unsigned long,
                        unsigned int*, unsigned int*, unsigned int*,
                        unsigned int*, unsigned int*, unsigned int*,
                        unsigned int*, unsigned int*) {}
    void Save_World_XML(class CArchive&, unsigned long, CString) {}
    void Serialize_World_XML(class CArchive&);

    // XML import/export (portable implementations in xml_save/load_world_portable.cpp).
    // ExportXML_* returns the full XML string for the named item, or "" if not found.
    CString ExportXML_Trigger (const CString& name);
    CString ExportXML_Alias   (const CString& name);
    CString ExportXML_Timer   (const CString& name);
    CString ExportXML_Variable(const CString& name);
    // ImportXMLFromString parses xml and adds items to this world.
    // Returns total items added, or -1 on parse error.
    long    ImportXMLFromString(const char* xml);

    // Portable world-file save.  Opens lpszPathName for writing, calls
    // Serialize_World_XML, and returns TRUE on success.  Mirrors the
    // CDocument::OnSaveDocument → Serialize chain that the Windows / mushapp
    // build uses via CDocument inheritance.
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

    void OnFileReloaddefaults() {}
    CString GetAlphaOptionItem(int) { return ""; }

    // ── New plugin callback overload ──────────────────────────────────
    bool SendToAllPluginCallbacks(const string& sName, const char* sData,
                                  bool bDefaultReturn) { return bDefaultReturn; }

    // ── Logging helpers ───────────────────────────────────────────────
    bool LoggingInput() const { return m_log_input != 0; }

    // ── Stub methods (doc_portable.cpp) ──────────────────────────────
    void SendToAllPluginCallbacks(const string& sName);
    bool SendToAllPluginCallbacks(const string& sName, long iArg,
                                  const string& sArg,
                                  bool bStopOnTrue, bool bDefaultReturn);
    // Returns true = "continue processing" (no plugin intercepted).
    // ProcessPreviousLine.cpp uses: if (!SendToAllPluginCallbacks(...)) return;
    bool SendToAllPluginCallbacks(const string& sName, const CString& sData);
    bool SendToAllPluginCallbacks(const string& sName, const CString& sData,
                                  bool bStopOnTrue);
    // Rtn variant: each plugin may transform strResult in place.
    // "\t" return = discard command; "\r" = ignore silently.
    void SendToAllPluginCallbacksRtn(const string& sName, CString& strResult);

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
    void ReceiveMsg();
    void ConnectionEstablished();
    void StartSSLHandshake();
    void ContinueSSLHandshake();
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
    void Note(const CString& s);
    void Note(const char* s);
    void UpdateAllViews(void* hint);

    // ── GUI output callbacks ──────────────────────────────────────────────
    // When m_fn_line_ready is non-null, DisplayMsg() routes output through it
    // instead of stdout.  Set by the mushcore host before
    // connecting.
    // Each '\n' fires m_fn_line_ready with the fully assembled SOutputLine.
    // Note() goes to the attached IWorldView (or falls back to puts()).
    std::function<void(SOutputLine&&)>      m_fn_line_ready;
    std::function<void(const std::string&)> m_fn_status;     // status bar update
    std::function<void(const std::string&)> m_fn_set_title;  // child frame title bar

    // ── PAL: typed output view ────────────────────────────────────────────
    // pal::IWorldView is the typed successor to the m_fn_* output callbacks.
    // When m_pWorldView is set the engine drives it directly; otherwise it
    // falls back to m_fn_line_ready (so existing hosts are unaffected). This is
    // the P2 seam-formalisation foothold — see docs/PAL.md.
    pal::IWorldView* m_pWorldView = nullptr;
    bool HaveOutputSink() const { return m_pWorldView != nullptr || (bool)m_fn_line_ready; }
    void EmitOutputLine(SOutputLine&& line) {
        if (m_pWorldView)         m_pWorldView->appendLine(std::move(line));
        else if (m_fn_line_ready) m_fn_line_ready(std::move(line));
    }
    // Typed-seam helpers for the state callbacks: prefer m_pWorldView, else fall
    // back to the matching m_fn_* callback. Twin of the doc.h helpers.
    void EmitPaused(bool paused) {
        if (m_pWorldView)         m_pWorldView->setPaused(paused);
        else if (m_fn_set_paused) m_fn_set_paused(paused);
    }
    void EmitStatus(std::string_view text) {
        if (m_pWorldView)     m_pWorldView->setStatus(text);
        else if (m_fn_status) m_fn_status(std::string(text));
    }
    void EmitTitle(std::string_view title) {
        if (m_pWorldView)        m_pWorldView->setTitle(title);
        else if (m_fn_set_title) m_fn_set_title(std::string(title));
    }
    void EmitClearOutput() {
        if (m_pWorldView)           m_pWorldView->clearOutput();
        else if (m_fn_clear_output) m_fn_clear_output();
    }
    void EmitBackgroundColour(unsigned long col) {
        if (m_pWorldView)                    m_pWorldView->setBackgroundColour((pal::Colour)col);
        else if (m_fn_set_background_colour) m_fn_set_background_colour(col);
    }
    void EmitMaxLines(int n) {
        if (m_pWorldView)            m_pWorldView->setMaxLines(n);
        else if (m_fn_set_max_lines) m_fn_set_max_lines(n);
    }
    void EmitRepaint() {
        if (m_pWorldView)      m_pWorldView->requestRepaint(pal::Rect{});
        else if (m_fn_repaint) m_fn_repaint();
    }
    // ── PAL: frontend → core reads (pulls) ────────────────────────────────
    bool HaveCommandSink() const { return m_pWorldView != nullptr || (bool)m_fn_get_command; }
    std::string GetCommandText() {
        if (m_pWorldView)     return m_pWorldView->currentCommand();
        if (m_fn_get_command) return m_fn_get_command();
        return {};
    }
    bool HaveScrollSink() const { return m_pWorldView != nullptr || (bool)m_fn_get_scroll_pos; }
    long GetScrollPos() {
        if (m_pWorldView)        return m_pWorldView->scrollPos();
        if (m_fn_get_scroll_pos) return m_fn_get_scroll_pos();
        return 0;
    }
    void GetOutputSelection(long& sl, long& sc, long& el, long& ec) {
        sl = sc = el = ec = 0;
        if (m_pWorldView) {
            pal::TextRange r = m_pWorldView->outputSelection();
            sl = r.startLine; sc = r.startCol; el = r.endLine; ec = r.endCol;
        } else if (m_fn_get_output_selection) {
            m_fn_get_output_selection(sl, sc, el, ec);
        }
    }

  // ── PAL: command-input bar (typed-only — P2 command family; the wx frame
  //    implements these on IWorldView, no m_fn_* fallback remains) ──────────
  bool SetCommandText(const std::string& text) {
      return m_pWorldView ? m_pWorldView->setCommand(text) : false;
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
      if (m_pWorldView) m_pWorldView->addToCommandHistory(text);
  }
  void DeleteCommandHistoryEntries() {
      if (m_pWorldView) m_pWorldView->deleteCommandHistory();
  }
  void EmitInputFont(const std::string& face, int pts, bool bold, bool italic) {
      if (m_pWorldView) m_pWorldView->setInputFont(face, pts, bold, italic);
  }

  // ── PAL: output-pane management (typed-only — P2 output family) ──────────
  void EmitNote(const std::string& text) {
      if (m_pWorldView) m_pWorldView->note(text);
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
      if (m_pWorldView) m_pWorldView->setOutputFont(face, pts, bold, italic);
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
      if (m_pWorldView) m_pWorldView->setMainTitle(title);
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

    // Pending styled spans for the line currently being assembled.
    // Flushed (via m_fn_line_ready) on each '\n'.
    SOutputLine m_gui_line_buf;

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

    // Called by world.Pause(bPause) to toggle scroll-lock from Lua.
    // Wired by CWorldChildFrame to COutputPane::SetPaused().
    std::function<void(bool)> m_fn_set_paused;

    // Called by world.Repaint() to force a repaint of the output pane.
    std::function<void()> m_fn_repaint;

    // Called by world.ClearOutput() to wipe the wx pane buffer and reset
    // m_gui_lines_emitted so UpdateAllViews starts fresh.
    std::function<void()> m_fn_clear_output;

    // Called by world.SetBackgroundColour(rgb) to update the pane background.
    // Receives a COLORREF; 0xFFFFFFFF means "default" (black).
    std::function<void(unsigned long)> m_fn_set_background_colour;

    // Called when SetOption("max_output_lines", n) changes the scrollback cap.
    // Wired by CWorldChildFrame to COutputPane::SetMaxLines().
    std::function<void(int)> m_fn_set_max_lines;

    // Called by world.GetClipboard() / world.SetClipboard().
    std::function<std::string()>              m_fn_get_clipboard;
    std::function<void(const std::string&)>   m_fn_set_clipboard;

    // ── Command-input callbacks ───────────────────────────────────────────
    // world.GetCommand() / world.SetCommand(): read/write the input bar text.
    std::function<std::string()>              m_fn_get_command;




    // ── Scroll / selection callbacks ──────────────────────────────────────
    // GetInfo(296): y scroll offset in pixels (mirrors CScrollView::GetScrollPosition().y).
    std::function<long()>                     m_fn_get_scroll_pos;

    // ── Window-geometry callbacks ─────────────────────────────────────────
    // world.GetSelectionStart/EndLine/Column(): current output-pane selection.
    std::function<void(long&, long&, long&, long&)> m_fn_get_output_selection;
    std::function<void(int&, int&)>             m_fn_infobar_size;     // (w,h) px, 0,0 if hidden










    // ── Scratch line for ANSI style state ────────────────────────────────
    // InterpretANSIcode and Interpret256ANSIcode always dereference
    // m_pCurrentLine->styleList.GetTail() to read the current style.
    // In the CLI build there is no real line; m_pScratchLine is a single
    // heap-allocated CLine whose style list holds exactly one zero-length
    // CStyle at all times, acting as the style accumulator.
    // Initialised (with one default CStyle) by the CMUSHclientDoc constructor.
    CLine* m_pScratchLine = nullptr;

    // Re-entrancy guard for StartNewLine: a trigger firing in ProcessPreviousLine
    // can world.Send/<send>, whose echo re-enters DisplayMsg → StartNewLine while
    // m_pCurrentLine is still the just-added line. Without this guard that line is
    // added to (and later double-freed from) m_LineList a second time.
    bool   m_bInStartNewLine = false;

    // ── Terminal colour state (for ANSI output on the real terminal) ──────
    // Tracks what colour/attribute codes have already been emitted so we
    // only re-emit when the style actually changes.
    COLORREF m_term_last_fg        = (COLORREF)-1; // sentinel "never emitted"
    COLORREF m_term_last_bg        = (COLORREF)-1;
    bool     m_term_last_bold      = false;
    bool     m_term_last_underline = false;

    // Emit (or skip) ANSI escape sequences to sync the terminal's colour state
    // with the current parsed style (m_iFlags, m_iForeColour, m_iBackColour).
    void TermSyncStyle();
    // Reset terminal colours to default (emitted at start of each new line).
    void TermResetStyle();
};

// ── t_regexp — needed by scripting/scripting.h ────────────────────────────
#include "regexp.h"

// ── Full CScriptEngine declaration (needs CMUSHclientDoc + Lua headers) ──────
// Included here, after CMUSHclientDoc is fully defined.
#include "scripting/scripting.h"

#endif // !_WIN32
