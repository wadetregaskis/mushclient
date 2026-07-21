// stdafx.h : include file for standard system include files,
//  or project specific include files that are used frequently, but
//      are changed infrequently
//
#pragma once

// disable some warnings (especially at warning level 4)
#ifdef _MSC_VER
#pragma warning (disable : 4018)  // '<' : signed/unsigned mismatch
#pragma warning (disable : 4100)  // unreferenced formal parameter
#pragma warning (disable : 4127)  // conditional expression is constant
#pragma warning (disable : 4201)  // nonstandard extension used : nameless struct/union
#pragma warning (disable : 4244)  // conversion from 'int ' to 'char ', possible loss of data
#pragma warning (disable : 4244)  // conversion from 'int' to 'unsigned short', possible loss of data
#pragma warning (disable : 4503)  // decorated name length exceeded, name was truncated
#pragma warning (disable : 4505)  // unreferenced local function has been removed
#pragma warning (disable : 4511)  // copy constructor could not be generated
#pragma warning (disable : 4512)  // assignment operator could not be generated
#pragma warning (disable : 4514)  // unreferenced inline function has been removed
#pragma warning (disable : 4663)  // C++ language change: to explicitly specialize class template yadda yadda
#pragma warning (disable : 4702)  // unreachable code
#pragma warning (disable : 4706)  // assignment within conditional expression
#pragma warning (disable : 4710)  // function 'x' not inlined
#pragma warning (disable : 4786)  // identifier was truncated to 'number' characters in the debug information
#pragma warning (disable : 4996)  // MBCS support in MFC is deprecated and may be removed in a future version of MFC
#endif // _MSC_VER

#ifndef HAVE_CONFIG_H
#define HAVE_CONFIG_H   // for PCRE
#endif

// ── Standard C++ library (needed on all platforms) ─────────────────────────
#include <string>
#include <vector>
#include <deque>
#include <stack>
#include <map>
#include <set>
#include <list>
#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>

// Marks stdafx.h as included (its UMessageBox/TMessageBox are available).
#define _STDAFX_INCLUDED_

#ifdef _WIN32
// ── Windows / MFC ─────────────────────────────��────────────────────────────

#define _AFX_NO_MFC_CONTROLS_IN_DIALOGS
#define VC_EXTRALEAN		// Exclude rarely-used stuff from Windows headers

#pragma warning( push, 3)
#include <afx.h>

#ifdef _DEBUG
  #define _CRTDBG_MAP_ALLOC
  #include <stdlib.h>
  #include <crtdbg.h>

  #define _INC_MALLOC	     // exclude standard memory alloc procedures
#endif

// disable memory checking for speed
//  comment out (middle 2 lines) to re-enable
#ifdef _DEBUG
  #undef DEBUG_NEW
  #define DEBUG_NEW new
#endif

#define USE_PYTHON    // Nick's "use Python" extension ;)
#define USE_TCL       // Nick's "use TCL" extension ;)
#define USE_PHP       // Nick's "use PHP" extension ;)
#define USE_RUBY      // Nick's "use Ruby" extension ;)

//#define PANE  // use pane windows
//#define USE_EXTRA_PANE     // 3rd pane for messages and stuff

#ifdef USE_EXTRA_PANE
  #define EXTRA_PANE 0
  #define OUTPUT_PANE 1
  #define COMMAND_PANE 2
#else
  #define OUTPUT_PANE 0
  #define COMMAND_PANE 1
#endif

#include <afxwin.h>         // MFC core and standard components
#include <afxext.h>         // MFC extensions
#include <afxdisp.h>        // MFC OLE automation classes
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>			// MFC support for Windows Common Controls
#endif // _AFX_NO_AFXCMN_SUPPORT

#include <winsock2.h>
#include <afxsock.h>		// MFC socket extensions
#include <afxtempl.h>
#include <activscp.h>
#include <afxmt.h>
#include <afxpriv.h>

#include "format.h"    // declares CFormat (inherits CString)
#include "exceptions.h"
// Guard format.h from re-inclusion on non-Windows (compat.h defines CFormat inline)
#define FORMAT_H_INCLUDED

#pragma warning (pop)

#else // !_WIN32
// ── macOS / non-Windows ────────────────────────────────────────────────────

//#define PANE  // use pane windows
//#define USE_EXTRA_PANE     // 3rd pane for messages and stuff

#ifdef USE_EXTRA_PANE
  #define EXTRA_PANE 0
  #define OUTPUT_PANE 1
  #define COMMAND_PANE 2
#else
  #define OUTPUT_PANE 0
  #define COMMAND_PANE 1
#endif

#include "platform/compat.h"
#include "exceptions.h"
#include "format.h"    // CFormat : CString — declared here, defined in format.cpp

// ── Colour constants — same values as Windows build ──────────────────────
// Use #define (not enum) so the #ifndef BLACK guard below composes cleanly.
#ifndef BLACK
#  define BLACK   0
#  define RED     1
#  define GREEN   2
#  define YELLOW  3
#  define BLUE    4
#  define MAGENTA 5
#  define CYAN    6
#  define WHITE   7
#endif

#endif // _WIN32

#ifdef LUA_52
  extern "C"
    {
    #include "../lua52/src/lua.h"
    #include "../lua52/src/lualib.h"
    #include "../lua52/src/lauxlib.h"
    }

#else
  extern "C"
    {
    #include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
    }
#endif

using namespace std ;

bool IsStringNumber (const string & s, const bool bSigned = false);

#include "regexp.h"
#include "mcdatetime.h" // MUSHclient date/time class

#include "sqlite3/sqlite3.h"  // SQLite3 database

#define PREFERENCES_DATABASE_FILE "mushclient_prefs.sqlite"

// less_than function for arrays
// first checks if the array key is an number, and if so returns
// a numeric less otherwise, a string less
// a number is assumed < a string
// std::binary_function was removed in C++17; provide a compatible base only on older standards.
#ifndef ARRAY_LESS_DEFINED
#define ARRAY_LESS_DEFINED
#if __cplusplus >= 201703L
struct array_less
#else
struct array_less : binary_function <string, string, bool>
#endif
  {

  bool operator() (const string & s1, const string & s2) const
    {
    bool bS1number = IsStringNumber (s1, true),
         bS2number = IsStringNumber (s2, true);

    // if both numbers, compare them as numbers

    if (bS1number && bS2number)
#ifdef _WIN32
      return _atoi64 (s1.c_str ()) < _atoi64 (s2.c_str ());
#else
      return atoll (s1.c_str ()) < atoll (s2.c_str ());
#endif

    // if both strings, compare them as strings

    if (!bS1number && !bS2number)
      return s1 < s2;

    // a number is less than a string (eg. "1" < "a" )

    if (bS1number)
      return true;

    // otherwise >=
    return false;

    }
  }; // end of array_less

// keys compare numericly if possible
typedef map<string, string, array_less> tStringToStringMap;
typedef map<string, tStringToStringMap *, array_less> tStringMapOfMaps;
#endif // ARRAY_LESS_DEFINED

//
// Note - to compile you will also need the zlib library at the
//        same level as MUSHclient.
//
//  eg.   cvs get mushclient
//        cvs get zlib
//
#include "zlib/zlib.h"

extern "C"
  {
#include "scripting/shs.h"
  }

class CActivityDlg;
class CMUSHView;
class CMUSHclientDoc;

//#define USE_REALLOC   // use malloc/realloc/free for line text

// The define below gets the current app and casts it to the correct type
// (on macOS, App and Frame are extern globals provided by the platform layer)
#ifdef _WIN32
#define App (*((CMUSHclientApp*)AfxGetApp()))

// The define below gets the current frame and casts it to the correct type

#define Frame (*((CMainFrame*)App.m_pMainWnd))
#endif // _WIN32

// colours (on macOS these are the #defines above)
#ifdef _WIN32
enum { BLACK = 0, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE };
#endif // _WIN32

#define SAMECOLOUR 65535

#define NO_COLOUR 0xFFFFFFFF   // for COLORREF

#define NOSOUNDLIT "(No sound)"

// helper define for appending an "s" to plural amounts, eg. trigger(s)
#define PLURAL(arg) (arg), (arg) == 1 ? "" : "s"
// helper define for appending an "ies" to plural amounts, eg. entity/entities
#define PLURALIE(arg) (arg), (arg) == 1 ? "y" : "ies"
// helper define for appending an "es" to plural amounts, eg. alias(es)
#define PLURALES(arg) (arg), (arg) == 1 ? "" : "es"

// number of items in an array
#define NUMITEMS(arg) ((unsigned int) (sizeof (arg) / sizeof (arg [0])))

#define DEFAULT_CHAT_PORT 4050
#define DEFAULT_CHAT_NAME "Name-not-set"

// function prototypes for minimum string validation in dialog boxes
#ifdef _WIN32
void AFXAPI DDV_MinMaxString(CDataExchange* pDX, CString& value, int nMinChars, int nMaxChars);
void AFXAPI DDX_MinMaxString(CDataExchange* pDX, int nIDC, CString& value);
void AFXAPI DDV_MinMaxCBString(CDataExchange* pDX, CString& value, int nMinChars, int nMaxChars);
void AFXAPI DDX_MinMaxCBString(CDataExchange* pDX, int nIDC, CString& value);
#endif // _WIN32

// clipboard helper function
#ifdef _WIN32
BOOL putontoclipboard (const CString & data, const bool bUnicode = false);
BOOL GetClipboardContents (CString & strClipboard, const bool bUnicode, const bool bWarning = true);
#endif // _WIN32

// label checking

BOOL CheckLabel (const CString & strLabel, const bool bScript = false);

// message used for wrapping up host name resolution. Note that WM_USER and a couple of other
// low WM_USER messages are used by MFC

#define WM_USER_HOST_NAME_RESOLVED (WM_USER + 1000)
#define WM_USER_SCRIPT_FILE_CONTENTS_CHANGED (WM_USER + 1001)
#define WM_USER_SHOW_TIPS (WM_USER + 1003)
#define WM_USER_SSL_FALLBACK_PROMPT (WM_USER + 1004)

// tray stuff

#define WM_USER_TRAY_ICON_ID 1
#define WM_USER_TRAY_ICON_MESSAGE (WM_APP + 1)

#define MAX_CUSTOM 16    // maximum custom colours at present (up to 256)
#define OTHER_CUSTOM 16  // colour for triggers which represents 'other'

// Nick's bit manipulations routines, that require bit numbers, not masks
#define CHECK_BIT_NUMBER(flag, bit) (((flag) & (1 << bit)) != 0)
#define SET_BIT_NUMBER(flag, bit) ((flag) |= (1 << bit))
#define CLEAR_BIT_NUMBER(flag, bit) ((flag) &= ~(1 << bit))
#define TOGGLE_BIT_NUMBER(flag, bit) ((flag) ^= (1 << bit))

// ==================== GENERAL FIND TEXT ROUTINES ==================================

// class used by FindRoutine

class CProgressDlg;

class CFindInfo : public CObject
  {
  public:

  CFindInfo ()
    {
    m_bCanGoBackwards = true;
    m_bForwards = true;
    m_bMatchCase = false;
    m_bRegexp = false;
    m_bUTF8 = false;
    m_pFindPosition = NULL;
    m_pProgressDlg = NULL;
    m_iStartColumn = 0;
    m_iEndColumn = 0;
    m_nTotalLines = 0;
    m_nCurrentLine = 0;
    m_iControlColumns = 0;
    m_regexp = NULL;
    m_bRepeatOnSameLine = false;
    };                 // constructor

  ~CFindInfo () { delete m_regexp; };

  CString m_strTitle;           // title for find box
  bool m_bCanGoBackwards;       // can we find backwards?
  bool m_bForwards;             // direction
  bool m_bMatchCase;            // whether to match on case
  bool m_bAgain;                // repeat last find?
  bool m_bRegexp;               // regular expression?
  bool m_bUTF8;                 // is world UTF-8?
  int  m_iStartColumn;          // first column we found it on
  int  m_iEndColumn;            // last column we found it on
  long m_nTotalLines;           // maximum limit of search
  long m_nCurrentLine;          // which line we are at
  POSITION m_pFindPosition;     // what position we currently at
  CProgressDlg * m_pProgressDlg;// progress dialog
  int  m_iControlColumns;       // number of columns in the control
  t_regexp * m_regexp;          // compiled regular expression
  CStringList m_strFindStringList;  // previous things we found
  bool m_bRepeatOnSameLine;     // keep trying to match on same line
  list<pair<int, int> >  m_MatchesOnLine;  // list of matches on this line
  };

// prototype for "get next line" callback for find routine
// return TRUE if could not get another line

typedef bool (*GetNextLine) (const CObject * pObject,
                             CFindInfo & FindInfo,
                             CString & strLine);

// prototype for "initiate search" callback for find routine

typedef void (*InitiateSearch) (const CObject * pObject,
                                CFindInfo & FindInfo);

// find routine - returns TRUE if wanted text found

bool FindRoutine (const CObject * pObject,       // passed back to callback routines
                  CFindInfo & FindInfo,          // details about find
                  const InitiateSearch pInitiateSearch, // how to re-initiate a find
                  const GetNextLine pGetNextLine);


// find-and-replace for strings

CString Replace (const CString strSource,
                 const CString strTarget,
                 const CString strReplacement,
                 const bool bAll = true);

int InitZlib (z_stream & m_zCompress);

void ReadNames (const LPCTSTR sName, const bool bNoDialog = false);
CString GenerateName (void);

CString GetText (const CWnd & theWindow);

// which OS do we have here?
#ifdef _WIN32
extern bool bWinNT;
extern bool bWin95;
extern bool bWin98;
extern bool bWine;
extern OSVERSIONINFO os_version;
#endif // _WIN32

BOOL Set_Up_Set_Strings (const int set_type,
                         CString & suggested_name,
                         CString & filter,
                         CString & title,
                         CString & suggested_extension);

CString ConvertToRegularExpression (const CString & strMatchString,
                                    const bool bWholeLine = true,
                                    const bool bMakeAsterisksWildcards = true);


// Helper routine for setting the font in certain description windows to a
// fixed-pitch courier, to make editing easier.
// Declared for every platform: the port compiles the real Utilities.cpp body,
// and if this declaration is not visible at the call sites they silently bind
// to some other overload instead (which is exactly what used to happen — see
// the note in platform/mfc_ui.h where the old no-op stub lived).
typedef CFont * ptrCFont;     // for changing fonts in edit windows

void FixFont (ptrCFont & pFont,
              CEdit & editctrl,
              const CString strName,
              const int iSize,
              const int iWeight,
              const DWORD iCharset);

// for escaping out things like \r in a trigger
CString FixupEscapeSequences (const CString & strSource);

// translates "send to" numbers
CString GetSendToString (const unsigned short iWhere);

// translates connection status to text
CString GetConnectionStatus (const int iStatus);

// fixes German sequences

CString FixUpGerman (const CString strMessage);

// find our host name and IP address(es)

void GetHostNameAndAddresses (CString & strHostName, CString & strAddresses);

// for fixing up HTML text
CString FixHTMLString (const CString strToFix);
CString FixHTMLMultilineString (const CString strToFix);


// for tooltips
#ifdef _WIN32
BOOL OnNeedText(UINT id, NMHDR* pNMHDR, LRESULT* pResult);
bool CreateTextWindow (const char * sText,
                       const char * sTitle,
                       CMUSHclientDoc * pDoc,
                       __int64 iUniqueDocumentNumber,
                       const CString strFontName,
                       const LONG     iFontSize,
                       const LONG     iFontWeight,
                       const DWORD    iFontCharset,
                       const COLORREF textColour,
                       const COLORREF backColour,
                       const CString  strSearchString,
                       const CString  strRecallLinePreamble,
                       const bool bMatchCase,
                       const bool bRegexp,
                       const bool bCommands,
                       const bool bOutput,
                       const bool bNotes,
                       const int iLines,
                       const int iNotepadType);
#endif // _WIN32

CString ConvertSetTypeToString (const int set_type);
unsigned long MakeActionHash (const CString & strAction,
                              const CString & strHint,
                              const CString & strVariable);
bool GetClipboardColour (COLORREF & colour);
bool GetSelection (CEdit * pEdit, CString & strSelection);

// forum codes

CString QuoteForumCodes(const CString & strText) ;

// different ways of saving things — implementations in xml/xml_save_world.cpp
void Save_XML_number  (CArchive& ar,
                       const char * sName,
                       const long iNumber,
                       const bool sameline = false);
void Save_XML_boolean (CArchive& ar,
                       const char * sName,
                       const long iNumber,
                       const bool sameline = false);
void Save_XML_string  (CArchive& ar,
                       const char * sName,
                       const CString & str,
                       const bool sameline = false);
void Save_XML_multi   (CArchive& ar,
                       const char * sName,
                       const CString & str);
void Save_XML_date    (CArchive& ar,
                       const char * sName,
                       const CTime & date,
                       const bool sameline = false);
void Save_XML_colour  (CArchive& ar,
                       const char * sName,
                       const COLORREF & colour,
                       const bool sameline = false);
void Save_XML_double    (CArchive& ar,
                       const char * sName,
                       const double & fNumber,
                       const bool sameline = false);
bool IsArchiveXML (CArchive& ar); // returns true if archive turns out to be XML
bool IsClipboardXML (void); // returns true if clipboard turns out to be XML
void SeeIfBase64 (CString & strText);
#ifndef _WIN32
// utilities_shared.cpp — surrogate-aware UTF-16LE widening for the 4-byte
// wchar_t platforms (used by xmlparse.cpp / xml_serialize.cpp BOM handling)
void WidenUTF16LE (const unsigned char * pBytes, int nBytes,
                   std::vector<wchar_t> & vWide);
#endif
CString decodeBase64(const char *coded);
CString encodeBase64(const char * plaintext, const bool bMultiLine);
CString ColourToName (const COLORREF colour);
CString GetGUID (void);
CString GetUniqueID (void);
bool IsNumber (const CString & str, const bool bAllowSign = false);
void MakeRandomNumber (CMUSHclientDoc * pDoc, SHS_INFO & shsInfo);
void ShutDownSocket (CAsyncSocket & s);
CString ExtractDirectory (CString str);
CString RemoveFinalSlash (CString str);

#define PLUGIN_UNIQUE_ID_LENGTH 24

/////////////////////////////////////////////////////////////////////////////
//  Stuff for the file-change monitoring code
#ifdef _WIN32
struct CThreadData
{
	char *	m_strFilename;    // which file to monitor
	HWND	m_hWnd;             // window to post event to
  DWORD_PTR m_pDoc;         // which document it belongs to (pointer-sized)
	HANDLE	m_hEvent;         // event which will become signalled
};

void ThreadFunc(LPVOID pParam);
void KillThread (HANDLE & pThread, CEvent & eventFileChanged);
HANDLE CreateMonitoringThread(const char * sName, DWORD_PTR pDoc, CEvent & eventFileChanged);
#else  // non-Windows: no-op stubs so call sites compile unchanged
inline void   KillThread(HANDLE&, CEvent&) {}
inline HANDLE CreateMonitoringThread(const char*, DWORD_PTR, CEvent&) { return nullptr; }
#endif // _WIN32

#define WM_USER_FILE_CONTENTS_CHANGED (WM_USER + 1001)

#define REGISTRATION_TIMER_ID 0x1001
#define SPLASH_SCREEN_TIMER_ID 0x1002
#define ACTIVITY_TIMER_ID 0x1003
#define COMMAND_QUEUE_TIMER_ID 0x1004
#define UNREGISTERED_DELAY_TIMER_ID 0x1005
#define TICK_TIMER_ID 0x1006

// macro for deleting everything in a map
// first, copy to a list so we don't have it in the list
// after it is deleted (eg. for a timer to find)
#define DELETE_MAP(mapname, pointertype)  \
do {                      \
  pointertype * pointer;  \
  CString strName;        \
  CTypedPtrList <CPtrList, pointertype*> mylist;    \
  POSITION pos;                                     \
  for (pos = mapname.GetStartPosition(); pos; )     \
    {                                               \
     mapname.GetNextAssoc (pos, strName, pointer);  \
     mylist.AddTail (pointer);                      \
    }                                               \
  mapname.RemoveAll();                              \
  for (pos = mylist.GetHeadPosition(); pos; )       \
     delete mylist.GetNext (pos);                   \
  mylist.RemoveAll ();                              \
  } while (false)

// macro for deleting everything in a list
#define DELETE_LIST(listname)  \
  do {                         \
  while (!listname.IsEmpty ())  \
      delete listname.RemoveHead (); \
  } while (false)

// bit manipulation
#define IS_SET(var, bit)	(((var) & (bit)) != 0)
#define SET_BIT(var, bit)	((var) |= (bit))
#define REMOVE_BIT(var, bit)	((var) &= ~(bit))
#define TOGGLE_BIT(var, bit)	((var) ^= (bit))

class CStyle;

CStyle * GetNewStyle (const char * filename, const long linenumber);
void DeleteStyle (CStyle * pStyle, const char * filename, const long linenumber);

// ANSI Colour Codes

#define ANSI_RESET             0
#define ANSI_BOLD              1
#define ANSI_BLINK             3
#define ANSI_UNDERLINE         4
#define ANSI_SLOW_BLINK        5
#define ANSI_FAST_BLINK        6
#define ANSI_INVERSE           7
#define ANSI_STRIKEOUT         9

#define ANSI_CANCEL_BOLD      22
#define ANSI_CANCEL_BLINK     23
#define ANSI_CANCEL_UNDERLINE 24
#define ANSI_CANCEL_SLOW_BLINK  25
#define ANSI_CANCEL_INVERSE   27
#define ANSI_CANCEL_STRIKEOUT 29

#define ANSI_TEXT_BLACK       30
#define ANSI_TEXT_RED         31
#define ANSI_TEXT_GREEN       32
#define ANSI_TEXT_YELLOW      33
#define ANSI_TEXT_BLUE        34
#define ANSI_TEXT_MAGENTA     35
#define ANSI_TEXT_CYAN        36
#define ANSI_TEXT_WHITE       37
#define ANSI_TEXT_256_COLOUR  38

#define ANSI_SET_FOREGROUND_DEFAULT 39

#define ANSI_BACK_BLACK       40
#define ANSI_BACK_RED         41
#define ANSI_BACK_GREEN       42
#define ANSI_BACK_YELLOW      43
#define ANSI_BACK_BLUE        44
#define ANSI_BACK_MAGENTA     45
#define ANSI_BACK_CYAN        46
#define ANSI_BACK_WHITE       47
#define ANSI_BACK_256_COLOUR  48

#define ANSI_SET_BACKGROUND_DEFAULT 49

// for mapping command IDs to names (for accelerators)

typedef struct
  {
  int						iCommandID;			 // eg. ID_EDIT_SELECT_ALL
  const char *	sCommandName;    // eg. "SelectAll"
  } tCommandIDMapping;

// for mapping virtuals to names (for accelerators)

typedef struct
  {
  int						iVirtualKey;     // eg. VK_ESCAPE
  const char *	sKeyName;		     // eg. "Esc"
  } tVirtualKeyMapping;

CString CommandIDToString (const int ID);
int StringToCommandID (const CString str);

// keypad indexes

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

// The following macros set and clear, respectively, given bits
// of the C runtime library debug flag, as specified by a bitmask.
#ifdef   _DEBUG
  #define  SET_CRT_DEBUG_FIELD(a) \
              _CrtSetDbgFlag((a) | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
  #define  CLEAR_CRT_DEBUG_FIELD(a) \
              _CrtSetDbgFlag(~(a) & _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
//  #define NEWSTYLE GetNewStyle (__FILE__, __LINE__)
//  #define DELETESTYLE(arg) DeleteStyle (arg, __FILE__, __LINE__)
  #define NEWSTYLE new CStyle
  #define DELETESTYLE(arg) delete arg

#else
  #define  SET_CRT_DEBUG_FIELD(a)   ((void) 0)
  #define  CLEAR_CRT_DEBUG_FIELD(a) ((void) 0)

  #define NEWSTYLE new CStyle
  #define DELETESTYLE(arg) delete arg
#endif

#define SPACES " \t\r\n"
#define SPELLCHECKFUNCTION "spellcheck"

// there is a clash in the MS compiler for min so I do my own here
template <typename T>
inline T minimum (const T a, const T b)
  {
  return (a < b) ? a : b;
  } // end of minimum

// there is a clash in the MS compiler for max so I do my own here
template <typename T>
inline T maximum (const T a, const T b)
  {
  return (a > b) ? a : b;
  } // end of maximum

inline string trim_right (const string & s, const string & t = SPACES)
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

inline string trim_left (const string & s, const string & t = SPACES)
  {
  string d (s);
  return d.erase (0, s.find_first_not_of (t)) ;
  }  // end of trim_left

inline string trim (const string & s, const string & t = SPACES)
  {
  string d (s);
  return trim_left (trim_right (d, t), t) ;
  }  // end of trim

// string find-and-replace
inline string FindAndReplace
  (const string& source, const string& target, const string& replacement)
  {
  string str = source;
  string::size_type pos = 0,   // where we are now
                    found;     // where the found data is
  while ((found = str.find (target, pos)) != string::npos)
    {
    str.replace (found, target.size (), replacement);
    pos = found + replacement.size ();
    }
  return str;
  };   // end of FindAndReplace

string GetWord (string & s,
                const string delim = " ",
                const bool trim_spaces = true);

void StringToVector (const string s,
                     vector<string> & v,
                     const string delim = " ",
                     const bool trim_spaces = true);

string VectorToString (const vector<string> & v,
                       const string delim = " ");

void StringToKeyCode (const CString strKey,
                      BYTE & fVirt,
                      WORD & key);

CString KeyCodeToString (const BYTE fVirt, const WORD key);

void ShowFunctionslist (CString & strSelection, int nStartChar = 0, int nEndChar = 0, const bool bLua = false);
CString GetSelectedFunction (CString & strSelection, int & nStartChar, int & nEndChar);
void FunctionMenu (CEdit & editctrl, const bool bLua, set<string> * extraItems = NULL, const bool bFunctions = true);
void ShowHelp (const CString strPrefix, const CString strTopic);

string tolower (const string & s);

void FindMatchingBrace (CEdit & ctlEdit, const bool bSelect);

void GetButtonSize (CWnd & ctlWnd, int & iHeight, int & iWidth);

void metaphone (const char *name, char * metaph, int metalen);
int EditDistance (const string source, const string target);

const char * Make_Absolute_Path (CString strFileName);
const CString Convert_PCRE_Runtime_Error (const int iError);

lua_State *MakeLuaState (void);

// send a window to the front
bool SendToFront (const char * name);

// 1 = invert, 2 = lighter, 3 = darker, 4 = less colour, 5 = more colour

enum
  {
  ADJUST_COLOUR_NO_OP = 0,
  ADJUST_COLOUR_INVERT,   // 1
  ADJUST_COLOUR_LIGHTER,  // 2
  ADJUST_COLOUR_DARKER,   // 3
  ADJUST_COLOUR_LESS_COLOUR, // 4
  ADJUST_COLOUR_MORE_COLOUR // 5
  };


// script numbers - must correspond to script combo-box in
// scripting configuration and plugin creation
enum
  {
  SCRIPT_VBSCRIPT = 0,
  SCRIPT_JSCRIPT = 1,
  SCRIPT_PERL = 2,
  SCRIPT_PYTHON = 3,
  SCRIPT_TCL = 4,
  SCRIPT_LUA = 5,
  SCRIPT_PHP = 6,
  SCRIPT_RUBY = 7
  };

long AdjustColour (long Colour, short Method);

void SetDefaultAnsiColours (COLORREF * normalcolour, COLORREF * boldcolour);
void SetDefaultCustomColours (COLORREF * customtext, COLORREF * customback);

// safe_for_each.  Apply a function to every element of a range.
// should handle correctly an element deleting itself
// See Josuttis p 205.

template <class ITER, class F> inline
F safe_for_each (ITER first, ITER last, F func)
  {
  while (first != last)
    func (*first++);
  return func;
  }

#ifdef _WIN32
#define MUSHCLIENT_FONT_CHARSET DEFAULT_CHARSET
#define MUSHCLIENT_FONT_FAMILY FIXED_PITCH
#endif // _WIN32

#define WINDOW_TABS_NONE 0
#define WINDOW_TABS_TOP 1
#define WINDOW_TABS_BOTTOM 2

// in file  mt19937-2.c
extern "C"
  {
//  void sgenrand (unsigned long seed);
//  double genrand (void);    // obsolete now
  void init_genrand (unsigned long seed);
  double genrand (void) ;
  void init_by_array(unsigned long init_key[], int key_length);
  }

// if timer interval <= 0 use this
#define MIN_TIMER_INTERVAL 100      // milliseconds

// stuff we borrow from PCRE

extern "C" int _pcre_valid_utf(const unsigned char  *buf, int length, int * erroroffset);
extern "C" int _pcre_ord2utf(int cvalue, unsigned char *buffer);
extern "C" const unsigned char _pcre_utf8_table4[];

// i18n stuff

// display a message box, using UTF-8 (not straight ASCII)
#ifdef _WIN32
int UMessageBox (const char * sText, UINT nType = MB_OK, const char * = "MUSHclient");
// translate lpszText and then call UMessageBox
int TMessageBox (const char * sText, UINT nType = MB_OK);
#else
// Non-Windows: implemented in platform/windows_stubs.cpp (mushcore) and
// platform/mushapp_stubs.cpp (mushapp/mushgui).  Both route through
// g_wxhook_message_box when set (wxWidgets GUI); fall back to stderr otherwise.
int UMessageBox (const char * sText, UINT nType = MB_OK, const char * caption = "MUSHclient");
int TMessageBox (const char * sText, UINT nType = MB_OK);
#endif // _WIN32
// translate text
CString TranslateGeneric (const char * sText, const char * sSection);
// translate formatted text
CString TFormat (const char * sFormat, ...);
// translate a static string
#define Translate(string) TranslateGeneric (string, "messages")
// translate a date/time string
#define TranslateTime(string) TranslateGeneric (string, "times")
// translate a heading string (eg. for dialogs lists)
#define TranslateHeading(string) TranslateGeneric (string, "headings")

// check a brush is valid, der
long ValidateBrushStyle (const long BrushStyle,
                          const long PenColour,
                          const long BrushColour,
                          CBrush & br);

// strings marked with this macro are to be translated in the localization file
// however not at runtime (ie. they are in a table, and the table lookup will be translated)
#define Translate_NoOp(string) (string)

#define ALIASES_USE_UTF8 0

#ifdef _WIN32
#define IsCTRLpressed()  ( (GetKeyState(VK_CONTROL) & (1 << (sizeof(SHORT)*8-1))) != 0 )
#define IsSHIFTpressed() ( (GetKeyState(VK_SHIFT) & (1 << (sizeof(SHORT)*8-1))) != 0   )
#else
#define IsCTRLpressed()  false
#define IsSHIFTpressed() false
#endif // _WIN32

// ascii to double
int myAtoF(const char *z, double *pResult);



long LoadPng (LPCTSTR FileName, HBITMAP & hbmp);  // load a PNG file into a bitmap
long LoadPngMemory (unsigned char * Buffer, const size_t Length, HBITMAP & hbmp, const bool bAlpha);  // load a PNG from memory into a bitmap

void ChangeToFileBrowsingDirectory ();
void ChangeToStartupDirectory ();

CString AnsiCode (short Code);
CString StripAnsi (const CString strMessage);
#ifdef _WIN32
void windowPositionHelper (CString & str, const RECT & r);
#else
// RECT is already defined in platform/windows_types.h; just declare the function.
void windowPositionHelper (CString & str, const RECT & r);
#endif // _WIN32

#define SCRIPTERRORFORECOLOUR "orangered"  // "darkorange"
#define SCRIPTERRORBACKCOLOUR "black"
#define SCRIPTERRORCONTEXTFORECOLOUR "burlywood"

/* These macros are the standard way of turning unquoted text into C strings.
They allow macros like PCRE_MAJOR to be defined without quotes, which is
convenient for user programs that want to test its value. */

#define STRING(a)  # a
#define XSTRING(s) STRING(s)

#define DEBUG_PLUGIN_ID "138a692642ab4f9e7a1af63b"  // special plugin ID for Debug "summary"
bool IsPluginID (const char * sID);
bool IsSubName (const char * sName);
const char *strnrchr(const char *s, const char c, size_t count);

typedef struct
  {
  const char *  sFunction;			// name of function (eg. "WindowShow")
  const char *	sArgs;          // arguments (eg. "(Window, Showflag)")
  } tInternalFunctionsTable;

// flag not in my version of the API
#ifndef BIF_NEWDIALOGSTYLE
  #define BIF_NEWDIALOGSTYLE 0x00000040
#endif

#ifdef LUA_52
  extern "C"
    {
    LUALIB_API int (luaL_typerror) (lua_State *L, int narg, const char *tname);
    }
#endif // LUA_52

#ifndef _WIN32
// ── MFC debug macros — no-ops on non-Windows ───────────────────────────────
// Note: windows_types.h may already have defined ASSERT/VERIFY; define the
// remaining MFC-specific macros unconditionally (they aren't in any stub header).
#ifndef ASSERT
#  define ASSERT(x)         ((void)0)
#endif
#ifndef ASSERT_VALID
#  define ASSERT_VALID(x)   ((void)0)
#endif
#ifndef VERIFY
#  define VERIFY(x)         ((void)(x))
#endif
#ifndef DEBUG_NEW
#  define DEBUG_NEW         new
#endif
#ifndef TRACE
#  define TRACE(...)        ((void)0)
#endif

// ── OS-version booleans ────────────────────────────────────────────────────
// On non-Windows, bWinNT/bWin95/bWin98/bWine are ordinary globals defined
// in MUSHclient.cpp (same as Windows).  We just need extern declarations.
#ifndef bWinNT
extern bool bWinNT;
extern bool bWin95;
extern bool bWin98;
extern bool bWine;
#endif

// ── CStaticLink — pull in the real declaration (HyprLink.h compiles fine
//    on non-Windows since it only uses stubbed MFC types).
#ifndef _STATLINK_H
#  include "StatLink.h"
#endif

// ── App / Frame global externs (provided by the platform layer) ────────────
// mushview.cpp and other view files use App.m_xxx and Frame.xxx directly;
// here we just expose the externs that every TU needs.
class CMUSHclientApp;  // fully defined in MUSHclient.h (included before stdafx.h finishes)
class CMainFrame;      // fully defined in mainfrm.h
extern CMUSHclientApp App;
extern CMainFrame Frame;
#endif // !_WIN32
