// doc.cpp : implementation of the CMUSHclientDoc class
//

#include "stdafx.h"
#include "MUSHclient.h"
#ifdef _WIN32
#include <process.h>
#endif

// #define SHOW_ALL_COMMS // for debugging connecting and other obscure problems

#include "doc.h"
#include "MUSHview.h"
#include "mainfrm.h"
#include "childfrm.h"
#include "sendvw.h"
#include "TextView.h"
#include "TextDocument.h"
#include "dialogs/ChooseNotepadDlg.h"
#include "dialogs/DebugWorldInputDlg.h"
#include "dialogs/ImportXMLdlg.h"
#include "dialogs/plugins/PluginsDlg.h"
#include "dialogs/KeyNameDlg.h"

#include "dialogs/logdlg.h"   // for logging session
#include "dialogs/RecallSearchDlg.h"
#include "dialogs/chat/ChatListDlg.h"

#include "dialogs/world_prefs/PrefsPropertySheet.h"

#include "dialogs/password.h"

#include "ActivityDoc.h"
#include "dialogs/ConfirmPreamble.h"

#include "dialogs/ProgDlg.h"
#ifdef _WIN32
#include "genprint.h"
#endif
#include "scripting/errors.h"
#include "flags.h"

#include "png/png.h"  // for version

#ifdef _WIN32
#include <malloc.h>   // _msize etc.
#else
#include <stdlib.h>   // malloc/free/realloc on POSIX
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#ifdef _WIN32
#include <wincrypt.h>
#else
#include "platform/posix_transport.h"   // pal::ITransport — the port's socket (T2d)
#endif

// AfxGetFileTitle is provided by mfc_ui.h on non-Windows; on Windows it's in
// the MFC library and just needs this forward declaration.
#ifdef _WIN32
UINT AFXAPI AfxGetFileTitle(LPCTSTR lpszPathName, LPTSTR lpszTitle, UINT nMax);
#else
// On non-Windows, App/Frame globals are defined in platform/app_globals.cpp
// (for the actual GUI build) or remain as link-time stubs during compilation.
extern CMUSHclientApp App;
extern CMainFrame     Frame;
// CScriptEngine is fully defined in scripting/scripting.h; include it here so
// that call sites in this file can invoke methods on CScriptEngine*.
#include "scripting/scripting.h"
#endif

#ifdef _WIN32
#ifndef _UNICODE
//#include <strstrea.h>
#endif
#endif

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

#ifdef _DEBUG
// for documenting menus, accelerators
void ListAccelerators (CDocument * pDoc, const int iType);
void ListDialogs (void);
#endif 

IMPLEMENT_DYNAMIC(CTimer, CObject)
IMPLEMENT_DYNAMIC(CTrigger, CObject)
IMPLEMENT_DYNAMIC(CAlias, CObject)
IMPLEMENT_DYNAMIC(CVariable, CObject)

extern bool bNamesRead;
extern char working_dir [_MAX_PATH];

// count number of documents open

int gdoccount = 0;

// tcp_keepalive / SIO_* block: moved to doc_shared.cpp with ConnectSocket (T3c)


/////////////////////////////////////////////////////////////////////////////
// CMUSHclientDoc

IMPLEMENT_DYNCREATE(CMUSHclientDoc, CDocument)

BEGIN_MESSAGE_MAP(CMUSHclientDoc, CDocument)
	//{{AFX_MSG_MAP(CMUSHclientDoc)
	ON_COMMAND(ID_FILE_LOGSESSION, OnFileLogsession)
	ON_UPDATE_COMMAND_UI(ID_FILE_LOGSESSION, OnUpdateFileLogsession)
	ON_UPDATE_COMMAND_UI(ID_STATUSLINE_LINES, OnUpdateStatuslineLines)
	ON_UPDATE_COMMAND_UI(ID_STATUSLINE_MUSHNAME, OnUpdateStatuslineMushname)
	ON_COMMAND(ID_GAME_WRAPLINES, OnGameWraplines)
	ON_UPDATE_COMMAND_UI(ID_GAME_WRAPLINES, OnUpdateGameWraplines)
	ON_UPDATE_COMMAND_UI(ID_STATUSLINE_TIME, OnUpdateStatuslineTime)
	ON_UPDATE_COMMAND_UI(ID_STATUSLINE_LOG, OnUpdateStatuslineLog)
	ON_COMMAND(ID_GAME_PREFERENCES, OnGamePreferences)
	ON_COMMAND(ID_GAME_PASTEFILE, OnGamePastefile)
	ON_COMMAND(ID_CONNECTION_CONNECT, OnConnectionConnect)
	ON_UPDATE_COMMAND_UI(ID_CONNECTION_CONNECT, OnUpdateConnectionConnect)
	ON_COMMAND(ID_CONNECTION_DISCONNECT, OnConnectionDisconnect)
	ON_UPDATE_COMMAND_UI(ID_CONNECTION_DISCONNECT, OnUpdateConnectionDisconnect)
	ON_COMMAND(ID_EDIT_PASTETOMUSH, OnEditPastetomush)
	ON_UPDATE_COMMAND_UI(ID_GAME_PASTEFILE, OnUpdateSessionOpen)
	ON_UPDATE_COMMAND_UI(ID_EDIT_PASTETOMUSH, OnUpdateEditPastetomush)
	ON_UPDATE_COMMAND_UI(ID_GAME_PREFERENCES, OnUpdateGamePreferences)
	ON_COMMAND(ID_GAME_IMMEDIATE, OnGameImmediate)
	ON_UPDATE_COMMAND_UI(ID_GAME_IMMEDIATE, OnUpdateGameImmediate)
	ON_COMMAND(ID_GAME_AUTOSAY, OnGameAutosay)
	ON_UPDATE_COMMAND_UI(ID_GAME_AUTOSAY, OnUpdateGameAutosay)
	ON_COMMAND(ID_CONNECT_DISCONNECT, OnConnectDisconnect)
	ON_UPDATE_COMMAND_UI(ID_CONNECT_DISCONNECT, OnUpdateConnectDisconnect)
	ON_COMMAND(ID_GAME_RELOAD_SCRIPT_FILE, OnGameReloadScriptFile)
	ON_UPDATE_COMMAND_UI(ID_GAME_RELOAD_SCRIPT_FILE, OnUpdateGameReloadScriptFile)
	ON_COMMAND(ID_DISPLAY_CLEAR_OUTPUT, OnDisplayClearOutput)
	ON_COMMAND(ID_GAME_CONFIGURE_MUDADDRESS, OnGameConfigureMudaddress)
	ON_COMMAND(ID_GAME_CONFIGURE_NAME_AND_PASSWORD, OnGameConfigureNameAndPassword)
	ON_COMMAND(ID_GAME_CONFIGURE_LOGGING, OnGameConfigureLogging)
	ON_COMMAND(ID_GAME_CONFIGURE_INFO, OnGameConfigureInfo)
	ON_COMMAND(ID_GAME_CONFIGURE_NOTES, OnGameConfigureNotes)
	ON_COMMAND(ID_GAME_CONFIGURE_COLOURS, OnGameConfigureColours)
	ON_COMMAND(ID_GAME_CONFIGURE_PRINTING, OnGameConfigurePrinting)
	ON_COMMAND(ID_GAME_CONFIGURE_OUTPUT, OnGameConfigureOutput)
	ON_COMMAND(ID_GAME_CONFIGURE_HIGHLIGHTING, OnGameConfigureHighlighting)
	ON_COMMAND(ID_GAME_CONFIGURE_COMMANDS, OnGameConfigureCommands)
	ON_COMMAND(ID_GAME_CONFIGURE_KEYPAD, OnGameConfigureKeypad)
	ON_COMMAND(ID_GAME_CONFIGURE_MACROS, OnGameConfigureMacros)
	ON_COMMAND(ID_GAME_CONFIGURE_AUTOSAY, OnGameConfigureAutosay)
	ON_COMMAND(ID_GAME_CONFIGURE_PASTETOWORLD, OnGameConfigurePastetoworld)
	ON_COMMAND(ID_GAME_CONFIGURE_SENDFILE, OnGameConfigureSendfile)
	ON_COMMAND(ID_GAME_CONFIGURE_SCRIPTING, OnGameConfigureScripting)
	ON_COMMAND(ID_GAME_CONFIGURE_VARIABLES, OnGameConfigureVariables)
	ON_COMMAND(ID_GAME_CONFIGURE_TRIGGERS, OnGameConfigureTriggers)
	ON_COMMAND(ID_GAME_CONFIGURE_ALIASES, OnGameConfigureAliases)
	ON_COMMAND(ID_GAME_CONFIGURE_TIMERS, OnGameConfigureTimers)
	ON_COMMAND(ID_GAME_RESETALLTIMERS, OnGameResetalltimers)
	ON_UPDATE_COMMAND_UI(ID_GAME_RESETALLTIMERS, OnUpdateGameResetalltimers)
	ON_COMMAND(ID_GAME_EDITSCRIPTFILE, OnGameEditscriptfile)
	ON_UPDATE_COMMAND_UI(ID_GAME_EDITSCRIPTFILE, OnUpdateGameEditscriptfile)
	ON_UPDATE_COMMAND_UI(ID_GAME_CONFIGURE_MUDADDRESS, OnUpdateGameConfigureMudaddress)
	ON_COMMAND(ID_DISPLAY_RECALLTEXT, OnDisplayRecalltext)
	ON_COMMAND(ID_GAME_CONFIGURE_CUSTOM_COLOURS, OnGameConfigureCustomColours)
	ON_COMMAND(ID_FILE_RELOADDEFAULTS, OnFileReloaddefaults)
	ON_UPDATE_COMMAND_UI(ID_FILE_RELOADDEFAULTS, OnUpdateFileReloaddefaults)
	ON_COMMAND(ID_GAME_TRACE, OnGameTrace)
	ON_UPDATE_COMMAND_UI(ID_GAME_TRACE, OnUpdateGameTrace)
	ON_COMMAND(ID_EDIT_FLIPTONOTEPAD, OnEditFliptonotepad)
	ON_COMMAND(ID_FILE_OPEN, OnFileOpen)
	ON_COMMAND(ID_FILE_SAVE, OnFileSave)
	ON_UPDATE_COMMAND_UI(ID_FILE_SAVE, OnUpdateFileSave)
	ON_COMMAND(ID_FILE_SAVE_AS, OnFileSaveAs)
	ON_UPDATE_COMMAND_UI(ID_FILE_SAVE_AS, OnUpdateFileSaveAs)
	ON_COMMAND(ID_GAME_MAPPER, OnGameMapper)
	ON_UPDATE_COMMAND_UI(ID_GAME_MAPPER, OnUpdateGameMapper)
	ON_COMMAND(ID_INPUT_DISCARDQUEUEDCOMMANDS, OnInputDiscardqueuedcommands)
	ON_UPDATE_COMMAND_UI(ID_INPUT_DISCARDQUEUEDCOMMANDS, OnUpdateInputDiscardqueuedcommands)
	ON_COMMAND(ID_GAME_DOMAPPERSPECIAL, OnGameDomapperspecial)
	ON_UPDATE_COMMAND_UI(ID_GAME_DOMAPPERSPECIAL, OnUpdateGameDomapperspecial)
	ON_COMMAND(ID_FILE_PREFERENCES, OnFilePreferences)
	ON_COMMAND(ID_DEBUG_WORLD_INPUT, OnDebugWorldInput)
	ON_COMMAND(ID_DISPLAY_NOCOMMANDECHO, OnDisplayNocommandecho)
	ON_UPDATE_COMMAND_UI(ID_DISPLAY_NOCOMMANDECHO, OnUpdateDisplayNocommandecho)
	ON_COMMAND(ID_EDIT_DEBUGINCOMINGPACKETS, OnEditDebugincomingpackets)
	ON_UPDATE_COMMAND_UI(ID_EDIT_DEBUGINCOMINGPACKETS, OnUpdateEditDebugincomingpackets)
	ON_COMMAND(ID_GAME_CONFIGURE_MXPPUEBLO, OnGameConfigureMxppueblo)
	ON_COMMAND(ID_FILE_IMPORT, OnFileImport)
	ON_COMMAND(ID_FILE_PLUGINS, OnFilePlugins)
	ON_COMMAND(ID_FILE_PLUGINWIZARD, OnFilePluginwizard)
	ON_COMMAND(ID_GAME_DOMAPPERCOMMENT, OnGameDomappercomment)
	ON_UPDATE_COMMAND_UI(ID_GAME_DOMAPPERCOMMENT, OnUpdateGameDomappercomment)
	ON_COMMAND(ID_GAME_CHATSESSIONS, OnGameChatsessions)
	ON_COMMAND(ID_GAME_CONFIGURE_CHAT, OnGameConfigureChat)
	ON_COMMAND(ID_GAME_RESETCONNECTEDTIME, OnGameResetconnectedtime)
	ON_COMMAND(ID_DISPLAY_STOPSOUNDPLAYING, OnDisplayStopsoundplaying)
	ON_COMMAND(ID_INPUT_KEYNAME, OnInputKeyname)
	ON_COMMAND(ID_EDIT_CONVERTCLIPBOARDFORUMCODES, OnEditConvertclipboardforumcodes)
	//}}AFX_MSG_MAP
	ON_UPDATE_COMMAND_UI_RANGE(0, 0xFFFF, OnFixMenus)
END_MESSAGE_MAP()


#ifdef _WIN32
BEGIN_DISPATCH_MAP(CMUSHclientDoc, CDocument)
#include "doc_dispatch_map.inc"
END_DISPATCH_MAP()
#endif // _WIN32

/*

  These are the prototypes for the methods when we had panes in ...

	DISP_FUNCTION(CMUSHclientDoc, "ClosePane", ClosePane, VT_I4, VTS_BSTR)
	DISP_FUNCTION(CMUSHclientDoc, "CreatePane", CreatePane, VT_I4, VTS_BSTR VTS_BSTR VTS_I4 VTS_I4 VTS_I4 VTS_I4 VTS_I4 VTS_I4 VTS_I4 VTS_BSTR VTS_I2 VTS_I4 VTS_I4)
	DISP_FUNCTION(CMUSHclientDoc, "PaneText", PaneText, VT_I4, VTS_BSTR VTS_BSTR VTS_I4 VTS_I4 VTS_I4)
	DISP_FUNCTION(CMUSHclientDoc, "PaneLine", PaneLine, VT_I4, VTS_BSTR VTS_BSTR VTS_I4 VTS_I4 VTS_I4)
	DISP_FUNCTION(CMUSHclientDoc, "ClearPane", ClearPane, VT_I4, VTS_BSTR)
*/

// Note: we add support for IID_IMUSHclient to support typesafe binding
//  from VBA.  This IID must match the GUID that is attached to the 
//  dispinterface in the .ODL file.

// {11DFC5E8-AD6F-11D0-8EAE-00A0247B3BFD}
static const IID IID_IMUSHclient =
{ 0x11dfc5e8, 0xad6f, 0x11d0, { 0x8e, 0xae, 0x0, 0xa0, 0x24, 0x7b, 0x3b, 0xfd } };

#ifdef _WIN32
BEGIN_INTERFACE_MAP(CMUSHclientDoc, CDocument)
	INTERFACE_PART(CMUSHclientDoc, IID_IMUSHclient, Dispatch)
END_INTERFACE_MAP()
#endif // _WIN32


// zlib helpers for memory allocation

/*
static void * zlib_alloc (void * opaque, unsigned int items, unsigned int size)
  {
  size_t total_size = items * size;
  void * p = malloc (total_size);
  if (p == NULL)
    {
    TRACE1 ("Could not allocate zlib memory: %i bytes\n", total_size);

    ::TMessageBox (TFormat ("Could not allocate zlib memory: %i bytes",
                      total_size), MB_ICONEXCLAMATION);
    }
  else
    {
    TRACE2 ("Allocated zlib memory: %p, %i bytes\n", p, total_size);
    }
  return p;
  } // end of zlib_alloc

static void zlib_free (void * opaque, void * address)
  {
  free (address);
  } // end of zlib_free

*/


void CMUSHclientDoc::OutputOutstandingLines (void)
  {

  // minimal work ...
  if (m_OutstandingLines.empty ())
    return;

  // save old colours
  bool bOldNotesInRGB = m_bNotesInRGB;
  COLORREF iOldNoteColourFore = m_iNoteColourFore;
  COLORREF iOldNoteColourBack = m_iNoteColourBack;
  unsigned short  iOldNoteStyle = m_iNoteStyle;

  m_bNotesInRGB = true;

  // output saved lines

  list<CPaneStyle>::iterator it;
  
  for (it = m_OutstandingLines.begin (); it != m_OutstandingLines.end (); it++)
    {
    m_iNoteColourFore = it->m_cText;
    m_iNoteColourBack = it->m_cBack;
    m_iNoteStyle = it->m_iStyle;
    Tell (it->m_sText.c_str ());
    }

  m_OutstandingLines.clear ();

  // put the colours back
  if (bOldNotesInRGB)
    {
    m_iNoteColourFore = iOldNoteColourFore;
    m_iNoteColourBack = iOldNoteColourBack;
    }
  else  
    m_bNotesInRGB = false;

  m_iNoteStyle = iOldNoteStyle;


  } // end of CMUSHclientDoc::OutputOutstandingLines


void CMUSHclientDoc::SetUpOutputWindow (void)
  {

  // Accept incoming chat calls
  if (m_bAcceptIncomingChatConnections)
    ChatAcceptCalls (0);

// document details will be loaded now, so set screen font

#ifdef _WIN32
  ChangeFont (m_font_height,
              m_font_name,
              m_font_weight,
              m_font_charset,
              m_bShowBold,
              m_bShowItalic,
              m_bShowUnderline,
              m_iLineSpacing);

  ChangeInputFont (m_input_font_height,
                  m_input_font_name,
                  m_input_font_weight,
                  m_input_font_charset,
                  m_input_font_italic);
#endif // _WIN32


// we defer allocating positions array to now, because m_maxlines is read from the
// document file.

  if (!m_pLinePositions)
    {
    m_pLinePositions = new POSITION [(m_maxlines / JUMP_SIZE) + 1];

  // clear all elements

    for (int i = 0; i <= m_maxlines / JUMP_SIZE; i++)
      m_pLinePositions [i] = NULL;

    m_pLinePositions [0] = m_LineList.GetHeadPosition ();

    }   // end of not having a positions array yet

// we defer allocating the first line to now, because m_nWrapColumn is read from the
// document file.

// put first line in line list

  if (!m_pCurrentLine)
    {
    m_total_lines = 0;
    m_pCurrentLine = new CLine (++m_total_lines, m_nWrapColumn, 0, WHITE, BLACK, m_bUTF_8);
    m_LineList.AddTail (m_pCurrentLine);
    }

  Note ("");
  Note (TFormat ("Welcome to MUSHclient version %s!", (LPCTSTR) MUSHCLIENT_VERSION));
  Note (Translate ("Written by Nick Gammon."));
  Note ("");
  // show compilation date
  Note (TFormat ("Compiled: %s at %s.", __DATE__, __TIME__)); 
  // show included library versions
  Note (TFormat ("Using: %s, PCRE %s, PNG %s, SQLite3 %s, Zlib %s", 
        LUA_RELEASE, 
        XSTRING(PCRE_MAJOR.PCRE_MINOR), 
        PNG_LIBPNG_VER_STRING, 
        SQLITE_VERSION, 
        ZLIB_VERSION));
  Note ("");
  Note (Translate ("For information and assistance about MUSHclient visit our forum at:"));
  Tell ("   ");
  Hyperlink (MUSHCLIENT_FORUM_URL, FORUM_URL, Translate ("MUSHclient forum"), 
             "deepskyblue", "black", TRUE);
  Note ("");
  Tell (Translate ("Can you trust your plugins?  See: "));
  Hyperlink (SECURITY_URL, SECURITY_URL, Translate ("How to trust plugins"), 
             "deepskyblue", "black", TRUE);
  Note ("");



  // output stuff that appeared before we set up the output buffer
  OutputOutstandingLines ();

  // set output window(s) to "pause" if wanted

#ifndef _WIN32
  if (m_bStartPaused)
    {
    m_gui_paused = true;
    EmitPaused (true);
    }
#else
  for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
		  CMUSHView* pmyView = (CMUSHView*)pView;

		  pmyView->m_freeze = m_bStartPaused;

	    }
    } // end of looping through views
#endif

// ------ load global plugins (unless already loaded from world file)

  CString strPath;

  try

    {

    vector<string> v;
    PluginListIterator pit;

    StringToVector ((LPCTSTR) App.m_strPluginList, v, "*");

    for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
      {
      strPath = i->c_str ();
      string sGlobalPluginPath (Make_Absolute_Path (strPath));
      bool bAlreadyLoaded = false;

      // see if we already have this one
      for (pit = m_PluginList.begin (); 
           pit != m_PluginList.end (); 
           ++pit)
        {
        CPlugin * p = *pit;
        // convert filename to absolute path for a better comparison
        string sThisPluginPath (Make_Absolute_Path (p->m_strSource));

        if (sGlobalPluginPath == sThisPluginPath)
          {
           bAlreadyLoaded = true;
           p->m_bGlobal = true;
           break;
          } // end of same plugin
        }  // end of searching for it

      if (!bAlreadyLoaded)
        {
        InternalLoadPlugin (strPath);
        // mark it as loaded globally
        for (pit = m_PluginList.begin (); 
             pit != m_PluginList.end (); 
             ++pit)
          {
          CPlugin * p = *pit;
          // convert filename to absolute path for a better comparison
          string sThisPluginPath (Make_Absolute_Path (p->m_strSource));

          if (sGlobalPluginPath == sThisPluginPath)
            {
             p->m_bGlobal = true;
             break;
            } // end of same plugin
          }  // end of searching for it

        }

      }   // end of loading each plugin

    } // end of try block

  catch (CFileException * e)
    {
    UMessageBox (TFormat ("Unable to open or read %s",
                      (LPCTSTR) strPath), MB_ICONEXCLAMATION);
    e->Delete ();
    } // end of catching a file exception

  catch (CArchiveException* e) 
    {
    UMessageBox (TFormat ("There was a problem loading the plugin %s. "
                     "See the output window for more details",
                     (LPCTSTR) strPath), MB_ICONEXCLAMATION);
    e->Delete ();
    }

    SetModifiedFlag (FALSE);   // loading plugins doesn't count

  }  // end of CMUSHclientDoc::SetUpOutputWindow


bool CMUSHclientDoc::SeeIfHandlerCanExecute (const CString & strName)
  {
  if (!strName.IsEmpty () && strName [0] == '!')
    {
    Execute (strName.Mid (1));
    return false;
    }

  return true;  // go ahead and call normal script

  } // end of SeeIfHandlerCanExecute

BOOL CMUSHclientDoc::OpenSession (void)
  {


// initialise scripting engine if necessary
 
  if (m_bEnableScripts &&     // provided wanted
     !m_ScriptEngine)         // and not already going
      CreateScriptEngine ();  // create scripting engine


  // execute "open" script
  if (m_ScriptEngine)
    {

    if (SeeIfHandlerCanExecute (m_strWorldOpen))
      {
      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      ExecuteScript (m_dispidWorldOpen,  
                   m_strWorldOpen,
                   eWorldAction,
                   "world open", 
                   "opening world",
                   params, 
                   nInvocationCount); 
      }
    } // end of executing open script

  if (App.m_bAutoConnectWorlds)
	  if (ConnectSocket())
		  return TRUE;

  return FALSE;

  } // end of OpenSession



/////////////////////////////////////////////////////////////////////////////
// CMUSHclientDoc diagnostics

#ifdef _DEBUG
void CMUSHclientDoc::AssertValid() const
{

	CDocument::AssertValid();

// a few little checks to make sure our document is OK

// but what will they be?

}

void CMUSHclientDoc::Dump(CDumpContext& dc) const
{
	CDocument::Dump(dc);
}
#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// CMUSHclientDoc commands

// ConnectSocket: single-sourced in doc_shared.cpp (ITransport T3c)

// SendMsg sends a message (command) to the MUD.
// If there is already a queue (for speedwalking etc.) it is placed 
// at the end of the queue. The message is marked to indicate whether
// a delay is needed after it, based on the bQueueIt flag.

void CMUSHclientDoc::SendMsg(CString strText, 
                             const bool bEchoIt,
                             const bool bQueueIt,
                             const bool bLogIt)
  {

  // cannot change what we are sending in OnPluginSent
  if (m_bPluginProcessingSent)
    return;

  bool bEcho = bEchoIt;

  // test to see if world has suppressed echoing

  if (m_bNoEcho)
     bEcho = false;

  // strip trailing endline - that would trigger an extra blank line
  if (strText.Right (2) == ENDLINE)
       strText = strText.Left (strText.GetLength () - 2);

  // fix up German umlauts
  if (m_bTranslateGerman)
    strText = FixUpGerman (strText);

  // to make sure each individual line ends up on the output window marked as user
  // input (and in the right colour) break up the string into individual lines
  CStringList strList;
  StringToList (strText, ENDLINE, strList);

  // if list is empty, make sure we send at least one empty line
  if (strList.IsEmpty ())
    strList.AddTail (""); 

  for (POSITION pos = strList.GetHeadPosition (); pos; )
    {
    CString strLine = strList.GetNext (pos);

    // it needs to be queued if queuing is requested
    // it also needs to be queued regardless if there is already something in the queue

    if (m_iSpeedWalkDelay &&
        (bQueueIt || !m_QueuedCommandsList.IsEmpty ()) )
      {
      CString strEchoFlag;

        // work out how it is to be queued

      if (bQueueIt)
        if (bEcho)
           strEchoFlag = QUEUE_WITH_ECHO;
        else
           strEchoFlag = QUEUE_WITHOUT_ECHO;
      else
        if (bEcho)
           strEchoFlag = IMMEDIATE_WITH_ECHO;
        else
           strEchoFlag = IMMEDIATE_WITHOUT_ECHO;

       // nolog is the lower-case version of the flag
      if (!bLogIt)
        strEchoFlag.MakeLower ();

      // queue it
      m_QueuedCommandsList.AddTail (strEchoFlag + strLine);

      }    // end of having a speedwalk delay
    else
      DoSendMsg (strLine, bEcho, bLogIt);  // just send it
    } // end of breaking it into lines

  if (!m_QueuedCommandsList.IsEmpty ())
    ShowQueuedCommands ();    // update status line
  }

// DoSendMsg is the actual command (message) sender - it should not
// be called directly, except from SendMsg or from CTimerWnd as it empties
// the command queue.

void CMUSHclientDoc::DoSendMsg(const CString & strText, 
                               const bool bEchoIt,
                               const bool bLogIt)
{
CString str = strText;

  // cannot change what we are sending in OnPluginSent
  if (m_bPluginProcessingSent)
    return;

  if (!ConnectionOpen ())
    return;

// append an end-of-line if there isn't one already

  if (str.Right (2) != ENDLINE)
    str += ENDLINE;

  // "OnPluginSend" - script can cancel send

  if (!m_bPluginProcessingSend)
    {
    m_bPluginProcessingSend = true;  // so we don't go into a loop
    if (!SendToAllPluginCallbacks (ON_PLUGIN_SEND, str.Left (str.GetLength () - 2)))
      {
      m_bPluginProcessingSend = false;
      return;     // plugin declines to send this line
      }
    m_bPluginProcessingSend = false;
    }

// count number of times we sent this

  if (str == m_strLastCommandSent)
    m_iLastCommandCount++;    // same one - count them
  else
    {
    m_strLastCommandSent = str;   // new one - remember it
    m_iLastCommandCount = 1;
    }

  if (m_bEnableSpamPrevention &&  // provided they want it
      m_iSpamLineCount > 2 &&   // otherwise we might loop
      !m_strSpamMessage.IsEmpty ())   // not much point without something to send
    {
    if (m_iLastCommandCount > m_iSpamLineCount)
      {
      m_iLastCommandCount = 0;      // so we don't recurse again
      DoSendMsg (m_strSpamMessage, m_display_my_input, LoggingInput ()); // recursive call
      m_strLastCommandSent = str;   // remember it for next time
      m_iLastCommandCount = 1;
      }   // end of time to do it

    } // end of spam prevention active


  // "OnPluginSent" - we are definitely sending this
  // See: http://www.gammon.com.au/forum/bbshowpost.php?bbsubject_id=7244

  if (!m_bPluginProcessingSent)
    {
    m_bPluginProcessingSent = true;  // so we don't go into a loop
    SendToAllPluginCallbacks (ON_PLUGIN_SENT, str.Left (str.GetLength () - 2));
    m_bPluginProcessingSent = false;
    }

// echo sent text if required

  if (bEchoIt)
    DisplayMsg (str, str.GetLength (), USER_INPUT | (bLogIt ? LOG_LINE : 0));

// log sent text if required

  if (bLogIt)
    LogCommand (str);

// add to mapper if required

  if (m_bMapping)
    AddToMap (str);

  // for MXP debugging
#ifdef SHOW_ALL_COMMS
   Debug_MUD (_T("++Sending to MUD   : "), str);
#endif

   m_nTotalLinesSent++;   // count lines sent

// double IAC to IAC IAC

   if (!m_bDoNotTranslateIACtoIACIAC)
     str.Replace (_T("\xFF"), _T("\xFF\xFF"));

// send it

   SendPacket (str, str.GetLength ());

} // end of CMUSHclientDoc::DoSendMsg

// ReceiveMsg: gone (T5) — bytes arrive via the transport events bridge
// (CDocTransportEvents::onBytes -> OnBytesReceived, doc_shared.cpp).

// OnBytesReceived: single-sourced in doc_shared.cpp (ITransport T3c)


void CMUSHclientDoc::StartNewLine_KeepPreviousStyle (const int flags)
  {
  CStyle * pPreviousStyle,
         * pThisStyle;
  CLine * pPreviousLine = m_pCurrentLine; // remember this line

  // if saved_count is indeed zero we better start a new line anyway
   StartNewLine (false, flags);
   // get old style
   pPreviousStyle = pPreviousLine->styleList.GetTail ();
   // get new style
   pThisStyle = m_pCurrentLine->styleList.GetTail ();

   if (pThisStyle && pPreviousStyle)    // sanity check
     {
     // copy style across so new line has same style as old one
     pThisStyle->iFlags = pPreviousStyle->iFlags & STYLE_BITS;
     pThisStyle->iForeColour = pPreviousStyle->iForeColour;
     pThisStyle->iBackColour = pPreviousStyle->iBackColour;
     pThisStyle->pAction = pPreviousStyle->pAction;
     if (pThisStyle->pAction)
       pThisStyle->pAction->AddRef ();    // we are using it again
     }  // end of valid pointers
  }  // end of CMUSHclientDoc::StartNewLine_KeepPreviousStyle

// called from DisplayMsg to actually add to the current line
// and also from the MXP routines to put stuff there
void CMUSHclientDoc::AddToLine (LPCTSTR lpszText, const int flags)
  {
const char * p ;
unsigned char c;
int saved_count;

  // incoming text from the MUD (only) is remembered also in m_strCurrentLine for triggers
//  if (flags == 0)
//    m_strCurrentLine += lpszText;

  for (p = lpszText; *p; p++)
    {
    c = *p;
    int iLineLength = m_pCurrentLine->len;

    // for Unicode the width of the line is characters, not stored bytes
    if (m_bUTF_8)
       iLineLength = MultiByteToWideChar (CP_UTF8, 0, m_pCurrentLine->text, m_pCurrentLine->len, NULL, 0);

/*
Unicode range              UTF-8 bytes

0x00000000 - 0x0000007F    0 xxxxxxx
0x00000080 - 0x000007FF    110 xxxxx 10 xxxxxx
0x00000800 - 0x0000FFFF    1110 xxxx 10 xxxxxx 10 xxxxxx
0x00010000 - 0x001FFFFF    11110 xxx 10 xxxxxx 10 xxxxxx 10 xxxxxx

  From above table, any character starting 10xxxxxx will *not* be the start 
  of a character (ie. it is the middle of a Unicode sequence) and thus we
  do *not* want to split it by putting it onto a new line.

*/
  
    int bNewCharacter = true;    // normally each character is a new one

    // but UTF-8 characters which have the high-order 2 bits == 80 are not
    if (m_bUTF_8 && ((c & 0xC0) == 0x80))
       bNewCharacter = false;

  // in the Unicode case we are allocating 4 bytes per character so we shouldn't
  // overshoot the allocated memory, but just in case, we test that we don't

    if (((iLineLength >= m_nWrapColumn) && bNewCharacter) ||  // usual case
        (m_pCurrentLine->len >= m_pCurrentLine->iMemoryAllocated))  // emergency bail-out
      {

      int last_space = -1;

      // do auto line wrapping here

      // see if we can split at a multibyte character or a space (whichever comes last)

      if (m_pCurrentLine->len >= m_nWrapColumn &&
          m_pCurrentLine->len >= 2)
        {
        for (int i = 0; i < m_pCurrentLine->len - 1; i++)
          {
          unsigned char c1 = m_pCurrentLine->text [i];
          unsigned char c2 = m_pCurrentLine->text [i + 1];
          // don't test for Big5 if output buffer is UTF-8 (the tests would be wrong)
          if (c1 >= 0x81 && c1 <= 0xFE &&                               // first  Big5 character
            ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0xA1 && c2 <= 0xFE))  // second Big5 character
              && !m_bUTF_8) // not for UTF-8
            last_space = i++;  // remember position, skip the second byte
          else if (c1 >= 0xA1 && c1 <= 0xF7 && // first  GB2132 character
                   c2 >= 0xA1 && c2 <= 0xFE && // second GB2132 character
                   !m_bUTF_8)  // // not for UTF-8
            last_space = i++;  // remember position, skip the second byte
          else if (c1 == ' ' && m_wrap)
            last_space = i;    // or split at a space
          }

        // allow for space being the final thing on the line
        if (m_pCurrentLine->text [m_pCurrentLine->len - 1] == ' ' && m_wrap)
          last_space = m_pCurrentLine->len - 1;

        } // end of checking for a space, Big5 or GB2132 break point

      if (last_space < 0 ||   // if no break point found, break anyway at end of line
        (m_pCurrentLine->len - last_space) >= m_nWrapColumn)
          StartNewLine_KeepPreviousStyle (flags);
      else
        {
        saved_count = m_pCurrentLine->len - last_space;

        // note - saved_count should not be zero because length is 1-relative
        // (eg. 1) and last_space is zero-relative (eg. 0)
        if (!m_indent_paras)
          {
          saved_count--;    // one less to copy
          last_space++;  // one more on this line (the space)
          m_pCurrentLine->len = last_space; // this line is longer
          }   // end of indenting not wanted

        // saved_count might be zero now, because of no indenting
        if (saved_count > 0)
          {
          // save portion of text destined for new line
          CString strText = CString (&m_pCurrentLine->text [last_space],
                                     saved_count); 
          m_pCurrentLine->len = last_space;

          CLine * pPreviousLine = m_pCurrentLine; // remember this line

          StartNewLine (false, flags);

          CStyle * pStyle;

          // delete empty style item new line already has
          pStyle = m_pCurrentLine->styleList.GetTail ();
          DELETESTYLE (pStyle);
          m_pCurrentLine->styleList.RemoveTail ();
        
          memcpy (m_pCurrentLine->text, (LPCTSTR) strText, saved_count);
          m_pCurrentLine->len = saved_count;

          // now move the styles over to the new line

          int iCount = 0,
              iOldLength = 0,
              iLength = 0;
          POSITION pos;

          // find number that have to move
          for (pos = pPreviousLine->styleList.GetHeadPosition(); pos; )
            {
            pStyle = pPreviousLine->styleList.GetNext (pos);
            iLength += pStyle->iLength;
            if (iLength > pPreviousLine->len)
              iCount++;   // this one has to move
            else 
              iOldLength += pStyle->iLength;
            }   // end of counting number to move

          // move them  - copy from tail of old to head of new (going backwards)
          for (pos = pPreviousLine->styleList.GetTailPosition(); iCount > 0 && pos; iCount--)
            {
            pStyle = pPreviousLine->styleList.RemoveTail ();
            m_pCurrentLine->styleList.AddHead (pStyle);
            }   // end of moving them

          // if one style is shared we have to make a copy and adjust lengths
          if (iOldLength < pPreviousLine->len)
            {
            int iDiff = pPreviousLine->len - iOldLength;  // amount we are short
            // was copied - find out its details

            pStyle =  m_pCurrentLine->styleList.GetHead ();
            pStyle->iLength -= iDiff;  // this line is that much smaller
            CAction * pAction = pStyle->pAction;
          
            AddStyle (pStyle->iFlags & STYLE_BITS, 
                      pStyle->iForeColour, 
                      pStyle->iBackColour, 
                      iDiff,  // old line has this much
                      pAction,
                      pPreviousLine);  // add to end of previous line

            } // end of shared style
          }  // end of having something to move to the next line
        else  
          {   // saved_count == 0
          StartNewLine_KeepPreviousStyle (flags);
          }  // end saved_count == 0

        } // end of line wrapping wanted and possible
      }   // end of line being full

    ASSERT (m_pCurrentLine->text);

    // add character to line
    m_pCurrentLine->text [m_pCurrentLine->len] = c;
    m_pCurrentLine->len++;    // line has one more character
//BUG    m_pCurrentLine->flags = flags;  // ensure we retain existing flags

    // style spans one more character now
    m_pCurrentLine->styleList.GetTail ()->iLength++; 

    } // end of processing each character
  } // end of AddToLine


// called when starting a new line to get colours right

void CMUSHclientDoc::SetNewLineColour (const int flags)
  {

  // just in case we call this before the output window exists
  if (m_pCurrentLine == NULL)
    return;

  // find current style
  CStyle * pStyle = m_pCurrentLine->styleList.GetTail ();

  // if non-zero for some reason, better make another one
  if (pStyle->iLength)
    pStyle = AddStyle (m_iFlags, m_iForeColour, m_iBackColour, 0, NULL);

  // start with default from previous line
  pStyle->iFlags = m_iFlags;
  pStyle->iForeColour = m_iForeColour;
  pStyle->iBackColour = m_iBackColour;

  if ((flags & USER_INPUT) && m_echo_colour != SAMECOLOUR)
    { // user input and (same colour not wanted)
    pStyle->iFlags = COLOUR_CUSTOM;  
    pStyle->iForeColour = m_echo_colour;
    pStyle->iBackColour = BLACK;
    } // end of user input 
  else
    if (flags & COMMENT)
      { // user input and (same colour not wanted)
      if (m_bNotesInRGB)
        {
        pStyle->iFlags = COLOUR_RGB | m_iNoteStyle;  
        pStyle->iForeColour = m_iNoteColourFore;
        pStyle->iBackColour = m_iNoteColourBack;
        } // end of RGB notes
      else
        if (m_iNoteTextColour == SAMECOLOUR)
          {
          if (m_bCustom16isDefaultColour)
            {
            pStyle->iFlags = COLOUR_CUSTOM | m_iNoteStyle;  
            pStyle->iForeColour = 15;
            pStyle->iBackColour = 0;
            }
          else
            {
            pStyle->iFlags = COLOUR_ANSI | m_iNoteStyle;  
            pStyle->iForeColour = WHITE;
            pStyle->iBackColour = BLACK;
            }
          } // end of "same colour"
        else
          {
          pStyle->iFlags = COLOUR_CUSTOM | m_iNoteStyle;  
          pStyle->iForeColour = m_iNoteTextColour;
          pStyle->iBackColour = BLACK;
          }
      } // end of note 

/*
  finally I think I will get this right! :)
  ... and *now* record the type of line (from flags)
  previously I recorded it when the line was created but this was
  really the type of the end of the previous line, eg.

  (user line) \n

  The \n triggered another user line. OR
  Recording the line type of the current line when we changed lines, which
  meant that an output line would carry over into a user line.

  *This* is the logical spot to do it - on the first character for the new line.
  (I think).

*/

  m_pCurrentLine->flags = flags;    // mark line as user input or comment as required

  } // end of CMUSHclientDoc::SetNewLineColour

void CMUSHclientDoc::DisplayMsg(LPCTSTR lpszText, int size, const int flags, const bool fake)
{
const char * p ;
unsigned char c;
int spaces,
    i;
char cOneCharacterLine [2] = {0, 0};

CLine * pOriginalLine = m_pCurrentLine;
CString strOriginalText;
CString strLine (lpszText, size);

  // cannot go very far without this - must be called at world loadup from a plugin OnPluginInstall
  if (m_pLinePositions == NULL)
    return;

  // decompressed data has a size, not a null terminator.
  // Also, compressed data may have imbedded nulls.


  if (!(flags & NOTE_OR_COMMAND))  // input from MUD
    {

      // for MXP debugging
    #ifdef SHOW_ALL_COMMS
      Debug_MUD ("++Received from MUD: ", strLine);
    #endif

    // hex debug  (debug packets) - unless we have faked an input line from MXP processing or similar
    if (m_bDebugIncomingPackets && !fake)
      Debug_Packets ("Incoming", lpszText, size, m_iInputPacketCount);

    m_iCurrentActionSource = eInputFromServer;

    // let plugin change the input packet unless we have faked an input line from MXP processing or similar
    if (!fake)
      SendToAllPluginCallbacksRtn (ON_PLUGIN_PACKET_RECEIVED, strLine);

    m_iCurrentActionSource = eUnknownActionSource;

    // change line to what the plugin(s) left it at
    lpszText = strLine;
    size = strLine.GetLength ();
    if (size <= 0)
      return;   // plugin discarded it
    
    m_iNoteStyle = NORMAL;  // MUD input cancels style flags
    }   // end of input from MUD

  // at the very start we may not have a current line
  if (m_pCurrentLine)
    {
    strOriginalText = CString (m_pCurrentLine->text, m_pCurrentLine->len);

    // If we get user input at the end of a "normal" line then the normal line
    // should really be terminated and scanned for triggers etc.

    // make sure notes start on a new line
    if ((flags & COMMENT) != (m_pCurrentLine->flags & COMMENT) && 
        m_pCurrentLine->len > 0)
      StartNewLine (true, flags);
    else
      {
      if (m_bKeepCommandsOnSameLine)  // for Simen Brekken
        {   // switch to command colour

// warning (bug?) triggers will not match if the command is on the same line

        if ((flags & USER_INPUT) && m_echo_colour != SAMECOLOUR)
          { // user input and (same colour not wanted)
          // create new style item
          AddStyle (COLOUR_CUSTOM, m_echo_colour, BLACK, 0, NULL);
          } // end of user input 
        } // end of commands on the same line
      else
        {
        if ((flags & NOTE_OR_COMMAND) != (m_pCurrentLine->flags & NOTE_OR_COMMAND) && 
            m_pCurrentLine->len > 0)
            StartNewLine (true, flags);
        } // end of commands going onto a new line
      }   // end of not changing to/from a note
    }
  else
    StartNewLine (true, 0);

// if line length is currently zero (ie. we are starting a new one)
// then we will set the default style depending on the flags, this will
// affect the rest of the line

  if (m_pCurrentLine->len == 0)
    SetNewLineColour (flags | (m_pCurrentLine->flags & BOOKMARK));

  for (p = lpszText; size > 0; p++, size--)
    {

    c = *p;

    // bail out of UTF-8 collection if a non-high order bit is found in the incoming stream
    if (!(flags & NOTE_OR_COMMAND) && 
        m_phase == HAVE_UTF8_CHARACTER && 
        (c & 0x80) == 0)
        OutputBadUTF8characters ();

    // note that CR, LF, ESC and IAC can appear inside telnet negotiation now (version 4.48)
    if (m_phase != HAVE_SB &&
        m_phase != HAVE_SUBNEGOTIATION && 
        m_phase != HAVE_SUBNEGOTIATION_IAC &&
        ! (m_phase == HAVE_IAC && c == IAC) )
      {
      // the following characters will terminate any collection/negotiation phases
      //  newline, carriage-return, escape, IAC
      if (!(flags & NOTE_OR_COMMAND) && strchr ("\n\r\x1B\xFF", c))
        {
        const char * pReason = "unknown";

        // we may need to get our MXP processing to deal with it
        switch (m_phase)
          {
          case HAVE_MXP_ELEMENT:
          case HAVE_MXP_COMMENT:
          case HAVE_MXP_QUOTE:
          case HAVE_MXP_ENTITY:
              switch (c)
                {
                case '\r':
                case '\n':   pReason = "got <NEWLINE>"; break;
                case '\x1B': pReason = "got <ESC>"; break;
                case '\xFF': pReason = "got <IAC>"; break;

                } // end of switch
              MXP_unterminated_element (pReason);
              break;
          case HAVE_MXP_ROOM_NAME:
          case HAVE_MXP_ROOM_DESCRIPTION:
          case HAVE_MXP_ROOM_EXITS:
          case HAVE_MXP_WELCOME:
              MXP_mode_change (-1);
              break;
          } // end of switch

        m_phase = NONE;   // cannot be in middle of escape sequence
        }

      } // end of not collecting telnet subnegotiation
    
// my own input won't interfere with incoming escape sequences

    if (!(flags & NOTE_OR_COMMAND))
      {

      switch (m_phase)
        {
        case HAVE_ESC:            Phase_ESC (c); continue;
        case HAVE_OSC:            Phase_OSC (c); continue;
        case HAVE_UTF8_CHARACTER: Phase_UTF8 (c); continue;

        case HAVE_FOREGROUND_256_START:    // these 12 are similar to Phase_ANSI
        case HAVE_FOREGROUND_256_FINISH:
        case HAVE_BACKGROUND_256_START:
        case HAVE_BACKGROUND_256_FINISH:
        case HAVE_FOREGROUND_24B_FINISH:
        case HAVE_FOREGROUND_24BR_FINISH:
        case HAVE_FOREGROUND_24BG_FINISH:
        case HAVE_FOREGROUND_24BB_FINISH:
        case HAVE_BACKGROUND_24B_FINISH:
        case HAVE_BACKGROUND_24BR_FINISH:
        case HAVE_BACKGROUND_24BG_FINISH:
        case HAVE_BACKGROUND_24BB_FINISH:
        case DOING_CODE:          
          Phase_ANSI (c); continue;

        case HAVE_IAC: 
          
          if (c != IAC)
            Phase_IAC (c);      // not IAC IAC? Process single IAC

          if (c)                // eg. IAC GA becomes \n, and IAC IAC becomes IAC
            break;
          else
            continue;

        case HAVE_WILL:           Phase_WILL (c); continue;
        case HAVE_WONT:           Phase_WONT (c); continue;
        case HAVE_DO:             Phase_DO (c); continue;
        case HAVE_DONT:           Phase_DONT (c); continue;
        case HAVE_SB:             Phase_SB (c); continue;
        case HAVE_SUBNEGOTIATION: Phase_SUBNEGOTIATION (c); continue;
        case HAVE_COMPRESS:       Phase_COMPRESS (c); continue;
        
        case HAVE_COMPRESS_WILL:  
            {
            bool bWasCompressing = m_bCompress;
            Phase_COMPRESS_WILL (c); 
            // just turned on compression?  special case, can't keep treating the
            // data as it it was not compressed
            if (!bWasCompressing && m_bCompress)
              {
              p++;    // skip SE  (normally done at end of loop)
              size--; // one less of these
              if (size)  // copy compressed data to compression buffer
                memmove (m_CompressInput, p, size);
              m_zCompress.next_in = m_CompressInput;
              m_zCompress.avail_in = size;
              m_zCompress.next_out = m_CompressOutput;
              m_zCompress.avail_out = m_nCompressionOutputBufferSize;
              m_nTotalCompressed += size;
              return;  // done with this loop, now it needs to be decompressed
              }
            }
            continue;
            
        case HAVE_SUBNEGOTIATION_IAC: 
          {
            bool bWasCompressing = m_bCompress;
            Phase_SUBNEGOTIATION_IAC (c); 
            // just turned on compression?  special case, can't keep treating the
            // data as it it was not compressed
            if (!bWasCompressing && m_bCompress)
              {
              p++;    // skip SE  (normally done at end of loop)
              size--; // one less of these
              if (size)  // copy compressed data to compression buffer
                memmove (m_CompressInput, p, size);
              m_zCompress.next_in = m_CompressInput;
              m_zCompress.avail_in = size;
              m_zCompress.next_out = m_CompressOutput;
              m_zCompress.avail_out = m_nCompressionOutputBufferSize;
              m_nTotalCompressed += size;
              return;  // done with this loop, now it needs to be decompressed
              }

            continue;

          }   // end of HAVE_SUBNEGOTIATION_IAC

        // MXP phases             
        case HAVE_MXP_ELEMENT:     Phase_MXP_ELEMENT (c); continue;
        case HAVE_MXP_COMMENT:     Phase_MXP_COMMENT (c); continue;
        case HAVE_MXP_QUOTE:       Phase_MXP_QUOTE (c); continue;
        case HAVE_MXP_ENTITY:      Phase_MXP_ENTITY (c); continue;

        case HAVE_MXP_ROOM_NAME:
        case HAVE_MXP_ROOM_DESCRIPTION:
        case HAVE_MXP_ROOM_EXITS:
        case HAVE_MXP_WELCOME:
              Phase_MXP_COLLECT_SPECIAL (c); continue;

        default: 
          break; // keep going (phase NONE)
        }   // end of switch on phase

      }   // end of not being my own input

// special test for MXP

//  1. Cancel once-only secure mode unless we get a <
//  2. Throw away incoming text if we are in script collection mode
//     unless it seems likely that script collection will be cancelled.

    if (m_bMXP && !(flags & NOTE_OR_COMMAND))
      {

      if (m_bMXP_script && !MXP_Secure ())
        continue;   // discard incoming script
      
      if (c != '<')
        {
        // in secure-once mode we *must* get an opening tag right away
        if (m_iMXP_mode == eMXP_secure_once)
          MXP_Restore_Mode ();  // back to what it was

        if (m_bMXP_script)
          continue;   // discard incoming script
        } // end of not having an opening tag

      // in <p> mode treat tabs as spaces
      if (c == '\t' && m_bInParagraph)
        c = ' ';

      } // end of special MXP tests on incoming data

// kludgy test for Pueblo - assumes it is all in one packet - bleech!

    // work out the length once to save time
    static int iPuebloStartLength = 0;

    if (iPuebloStartLength == 0)
      iPuebloStartLength = strlen (PUEBLO_START);

    // quick initial tests
    if (!(flags & NOTE_OR_COMMAND) &&   // only on incoming stuff
        c == '<' &&           // might be it
        !m_bMXP &&            // and not MXP 
        m_bPueblo &&          // Pueblo wanted
        !m_bPuebloActive &&   // and not active yet
        size >= iPuebloStartLength)
      if (memcmp (p, PUEBLO_START, iPuebloStartLength) == 0)
        {
        MXP_On (true);  // turn Pueblo on
        m_iMXP_defaultMode = m_iMXP_mode = eMXP_perm_secure;  // assume secure mode
        TRACE ("Pueblo on.\n");
        p += iPuebloStartLength - 1;     // one is added at end of loop
        size -= iPuebloStartLength - 1;  // ditto
        continue;   // back to main loop
        } // end of Pueblo startup

// here when phase is NONE

    // do not display UTF-8 characters until they have completely arrived
    // check if high-order bit is set
    if (!(flags & NOTE_OR_COMMAND) && m_bUTF_8 && (c & 0x80) && (c != IAC || m_phase == HAVE_IAC))
      {
      m_UTF8Sequence [0] = c;
      m_UTF8Sequence [1] = 0;  // null terminator
      m_phase = HAVE_UTF8_CHARACTER;

      // 0x00000080 - 0x000007FF    110 xxxxx 10 xxxxxx
      if ((c & 0xE0) == 0xC0)
        m_iUTF8BytesLeft = 1;
      // 0x00000800 - 0x0000FFFF    1110 xxxx 10 xxxxxx 10 xxxxxx
      else if ((c & 0xF0) == 0xE0)
        m_iUTF8BytesLeft = 2;
      // 0x00010000 - 0x001FFFFF    11110 xxx 10 xxxxxx 10 xxxxxx 10 xxxxxx
      else if ((c & 0xF8) == 0xF0)
        m_iUTF8BytesLeft = 3;
      else
      // some bogus byte with the high-order bit set
        OutputBadUTF8characters ();
      continue;  // we are done for now with this byte
      }  // end of high-order bit set with UTF-8 enabled

    cOneCharacterLine [0] = c;    // in case we need to use it as a string

    switch (c)
      {
      case '\n':      // start a new line
            m_iNoteStyle = NORMAL;  // new line cancels style flags
            m_nTotalLinesReceived++;

            if (m_bMXP && !(flags & NOTE_OR_COMMAND) && !m_bPuebloActive)
              {
              if (!MXP_Open ())
                MXP_CloseOpenTags (); // close all open tags
              MXP_mode_change (-1);  // switch to default mode
              }   // end of MXP stuff
            if (m_bInParagraph && !(flags & NOTE_OR_COMMAND))  // for <P>
              {
              if (m_cLastChar == c)
                {  // two newlines in a row - start a real new line
                // we'll do two because the original text had a blank line.
                StartNewLine (true, flags);
                m_pCurrentLine->flags = flags;    // remember flags for this line
                StartNewLine (true, flags);   // and another
                m_pCurrentLine->flags = flags;    // remember flags for this line
                }  // end of \n\n
              else
                {
                // work out where the last space is
                const char * space = strnrchr(m_pCurrentLine->text, ' ', m_pCurrentLine->len);
                int last_space = -1;
                if (space)
                  last_space = space - m_pCurrentLine->text;

                // don't run words together - if a newline follows a word,
                // insert a space
                if (last_space != (m_pCurrentLine->len - 1))
                  {
                  if (m_cLastChar == '.' && m_pCurrentLine->len < m_nWrapColumn)
                    AddToLine ("  ", flags);  // two spaces after period
                  else
                    AddToLine (" ", flags);  // convert newline to space
                  }   // end of newline which does not follow a space
                }  // end of not two newlines in a row
              m_cLastChar = c;  // remember it was a newline
              }  // end of <p> mode 
            else
              if (!(m_bSuppressNewline && m_bMXP) ||    //   -  unless told not to
                  (!m_bSuppressNewline && m_bPreMode) || // <PRE> mode honours newlines
                  (flags & NOTE_OR_COMMAND)           // input/note mode honours newlines
                  )
                {
                StartNewLine (true, flags);
                SetNewLineColour (flags);
                }
            break;  // end of newline

      case '\r':      // return - ignore, or clear line content

            if (m_bCarriageReturnClearsLine && !(flags & NOTE_OR_COMMAND) && p [1] != '\n')
              {

              // delete existing styles list

              for (POSITION pos = m_pCurrentLine->styleList.GetHeadPosition(); pos; )
                  DELETESTYLE (m_pCurrentLine->styleList.GetNext (pos));

              m_pCurrentLine->styleList.RemoveAll();

              // add back one default style

              CStyle * pStyle; 

              // have at least one style item in the list
              m_pCurrentLine->styleList.AddTail (pStyle = NEWSTYLE);

              pStyle->iFlags = 0;
              pStyle->iForeColour = WHITE;
              pStyle->iBackColour = BLACK;

              m_pCurrentLine->hard_return = false;
              m_pCurrentLine->len = 0;

              }   // end of letting a \r delete line contents

            break;
            
      case '\a':      // beep
            if (m_enable_beeps)
               {
               if (m_strBeepSound.IsEmpty ())
                 ::MessageBeep (MB_ICONEXCLAMATION);
               else
                 PlaySoundFile (m_strBeepSound);
               }
            break;    // end of beep

      case ' ':     // space
            if (m_bInParagraph && !(flags & NOTE_OR_COMMAND))  // for <P>
              {
              // work out where the last space is
              const char * space = strnrchr(m_pCurrentLine->text, ' ', m_pCurrentLine->len);
              int last_space = -1;
              if (space)
                last_space = space - m_pCurrentLine->text;

              // multiple consecutive spaces - discard extras
              if (last_space == (m_pCurrentLine->len - 1))
                {
                if (m_cLastChar != '\n')
                    m_cLastChar = c;  // remember it
                break;
                }

              // if you have period-space, make it period-space-space
              if ((m_cLastChar == '.' || m_cLastChar == '!' || m_cLastChar == '?')
                  && m_pCurrentLine->len < m_nWrapColumn)
                {
                AddToLine ("  ", flags);
                m_cLastChar = c;  // remember it
                break;
                }

              }   // end of <p> mode            

            AddToLine (" ", flags);

            // a newline followed by only a space still counts as a newline
            if (m_cLastChar != '\n' && !(flags & NOTE_OR_COMMAND))
              m_cLastChar = c;  // remember it
            break;  // end of space

      case '\t':  i = ((m_pCurrentLine->len + 8) & 0xFFF8);
                  if (m_pCurrentLine->len >= m_nWrapColumn)
                    StartNewLine (false, flags);
                  else
                    {
                    spaces = i - m_pCurrentLine->len;  // no. of spaces
                    for (i = 0; i < spaces; i++)
                        AddToLine (" ", flags);

                    }   // end of being inside wrap column
                  break;    // end of tab

      case ESC:   if (!(flags & NOTE_OR_COMMAND))
                    m_phase = HAVE_ESC;   // start of ANSI escape sequence
                  break;

      case IAC:   // Interpret As Command (however IAC IAC is just IAC)
                
                  if (!(flags & NOTE_OR_COMMAND)) 
                    {
                    if (m_phase == HAVE_IAC)
                      {
                      AddToLine (cOneCharacterLine, flags);
                      m_cLastChar = c;  // remember it
                      m_phase = NONE;
                      }
                    else
                      m_phase = HAVE_IAC;   // start of telnet protocol
                    break;    
                    }
                  // note NO break here, if not input from MUD we FALL THROUGH

      case '<':   if (!(flags & NOTE_OR_COMMAND) && 
                      m_bMXP && 
                      (MXP_Open () || MXP_Secure ())
                     )
                  {
                  m_strMXPstring.Empty ();    // no string yet
                  m_phase = HAVE_MXP_ELEMENT;
//                  TRACE ("\nStarting MXP element collection.\n");
                  break;
                  } // end of starting an MXP element
                  // note NO break here, if not in MXP mode we FALL THROUGH

      case '&':   if (!(flags & NOTE_OR_COMMAND) && 
                      m_bMXP && 
                      (MXP_Open () || MXP_Secure ())
                     )
                  {
                  m_strMXPstring.Empty ();    // no string yet
                  m_phase = HAVE_MXP_ENTITY;
//                  TRACE ("\nStarting MXP entity collection.\n");
                  break;
                  } // end of starting an MXP entity
                  // note NO break here, if not in MXP mode we FALL THROUGH

      default:
                  AddToLine (cOneCharacterLine, flags);
                  if (!(flags & NOTE_OR_COMMAND))
                    m_cLastChar = c;  // remember it
                  break;
      } // end of switch    

  } // end of processing input
      

  // to avoid flicker, only update the line if:
  //   a) It is MUD output (eg. a prompt line); or
  //   b) The line has changed

  if (!(flags & NOTE_OR_COMMAND) || pOriginalLine != m_pCurrentLine)

    // don't update views if no change to current line (some nulls maybe?)
    if (pOriginalLine != m_pCurrentLine ||
        strOriginalText != CString (m_pCurrentLine->text, m_pCurrentLine->len))
      {
      // get the view to refresh

      for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
	      {
	      CView* pView = GetNextView(pos);
	      
	      if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	      {
		      CMUSHView* pmyView = (CMUSHView*)pView;

		      pmyView->addedstuff();
	        }	
        }

      // new - for people on the forum who insist on getting lines without a \n at
      // the end - tell plugins about this line

      SendLineToPlugin ();

      }   // end of this line changing

}



void CMUSHclientDoc::StartNewLine (const bool hard_break, const int flags)
  {
POSITION pos;

  // we may not have a current line
  if (m_pCurrentLine)
    {
    // the time for the line is the time it finished arriving
    m_pCurrentLine->m_theTime = CTime::GetCurrentTime();
    QueryPerformanceCounter (&(m_pCurrentLine->m_lineHighPerformanceTime));

    // new - for people on the forum who insist on getting lines without a \n at
    // the end - tell plugins about this line

    if (!(flags & NOTE_OR_COMMAND))
      SendLineToPlugin ();

  //  TRACE1 ("Received line: %s\n", (LPCTSTR) CString (m_pCurrentLine->text, m_pCurrentLine->len));

    if (hard_break)
      {
      CLine * pSavedLine = m_pCurrentLine;
      LARGE_INTEGER saved_time = m_pCurrentLine->m_lineHighPerformanceTime;
      if (ProcessPreviousLine () &&
          m_pCurrentLine->len == 0)
        return;   // return if omit from output (no need to add another line)

      // if the line has changed then the trigger or script added a new one,
      // so we don't need to add a second.
      // Version 3.33 - provided the new line is empty :)
      // (see forum subject 2320 - a script might have done a world.colourtell
      // which would leave a line which was not terminated).

      // added in version 4.41, also check timestamp, due to problem with lines being deleted
      // by DeleteLines in a trigger script.
      if ((pSavedLine != m_pCurrentLine || 
          saved_time.QuadPart != m_pCurrentLine->m_lineHighPerformanceTime.QuadPart) &&
          m_pCurrentLine->len == 0)
        return;

      }
    }


  // if our buffer is full, remove the JUMP_SIZE items

  if (m_LineList.GetCount () >= m_maxlines)
    {

    RemoveChunk ();   // get rid of JUMP_SIZE lines

  // notify view to update their selection ranges

  	for(pos = GetFirstViewPosition(); pos != NULL; )
    	{
  		CView* pView = GetNextView(pos);
		
  		if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  	{
  			CMUSHView* pmyView = (CMUSHView*)pView;

  			pmyView->did_jump ();
  		  }	  // end of being an output view
  	  }   // end of doing each view
  
    }     // end of buffer full

// try, oh try, to allocate a new line

  try
    {

  // we may not have a current line
    if (m_pCurrentLine)
      {

  // We are about to move onto a new line. For space reasons, reallocate them
  // memory used by the pointers. However to keep from getting a null pointer,
  // keep at least a single character

    // save current line text
     CString strLine = CString (m_pCurrentLine->text, m_pCurrentLine->len);

     m_pCurrentLine->iMemoryAllocated = MAX (m_pCurrentLine->len, 1);

#ifdef USE_REALLOC
      m_pCurrentLine->text  = (char *) realloc (m_pCurrentLine->text, 
                                               m_pCurrentLine->iMemoryAllocated);

#else

    delete [] m_pCurrentLine->text;
    m_pCurrentLine->text = new char [m_pCurrentLine->iMemoryAllocated];


#endif
      
    ASSERT (m_pCurrentLine->text);

    // put text back
    memcpy (m_pCurrentLine->text, (LPCTSTR) strLine, m_pCurrentLine->len);

      // if we have more than one style, and the last one is empty, get rid of it
      // unless it is a start tag marker
      /*

  // Commented out because of bug #418 - a style change at the very end
  // of the line was being discarded.    Changed in version 3.18.

      if (m_pCurrentLine->styleList.GetCount () > 1)
        {
        // find current style
        CStyle * pStyle = m_pCurrentLine->styleList.GetTail ();

        if (pStyle->iLength == 0 && (pStyle->iFlags & START_TAG) == 0)
          {
          DELETESTYLE (pStyle);
          m_pCurrentLine->styleList.RemoveTail ();
          }
        }   // end of having more than one style
        */

      } // end of having a current line

// start a new line

    int      iFlags = m_iFlags;             
    COLORREF iForeColour = m_iForeColour;  
    COLORREF iBackColour = m_iBackColour;  

    if ((flags & USER_INPUT) && m_echo_colour != SAMECOLOUR)
      { // user input and (same colour not wanted)
      iFlags = COLOUR_CUSTOM;  
      iForeColour = m_echo_colour;
      iBackColour = BLACK;
      } // end of user input 
    else
      if (flags & COMMENT) 
        {
        if (m_iNoteTextColour == SAMECOLOUR)
          {
          if (m_bCustom16isDefaultColour)
            {
            iFlags = COLOUR_CUSTOM;  
            iForeColour = 15;
            iBackColour = 0;
            }
          else
            {
            iFlags = COLOUR_ANSI;  
            iForeColour = WHITE;
            iBackColour = BLACK;
            }
          } // end of "same colour"
        else
          {
          iFlags = COLOUR_CUSTOM;  
          iForeColour = m_iNoteTextColour;
          iBackColour = BLACK;
          }
        } // end of note 

    m_pCurrentLine = new CLine (++m_total_lines, 
                                m_nWrapColumn,
                                iFlags,       // style flags
                                iForeColour,  
                                iBackColour,
                                m_bUTF_8);

    m_pCurrentLine->flags = flags;
    pos = m_LineList.AddTail (m_pCurrentLine);

// add every "JUMP_SIZE" line positions to the positions array

// we match the modulus on 1, not zero, because by this time the line has already been
// added to the line count, so the line count is 1-relative, not 0-relative, however
// we want to place the line in the line positions array zero relative.

    if (m_LineList.GetCount () % JUMP_SIZE == 1)
      m_pLinePositions [m_LineList.GetCount () / JUMP_SIZE] = pos;

    }   // end of try block

// here for memory exception - free up some memory, warn the user and close the world

  catch (CMemoryException * e)
    {

    RemoveChunk ();   // get rid of JUMP_SIZE lines
    OnConnectionDisconnect ();    // close the world
    TMessageBox ("Ran out of memory. The world has been closed.");

    e->Delete ();

    } // end of catch block

      
// if this document does not have an active view, note we have a new message for it

  if (m_pActiveCommandView == NULL && m_pActiveOutputView == NULL)
    {
    if (0 == m_new_lines)
      {

// play the new activity sound if requested

      if (!m_new_activity_sound.IsEmpty () && m_new_activity_sound != NOSOUNDLIT)
        PlaySoundFile (m_new_activity_sound);

      }   // end of being first new stuff

    m_new_lines++;    // count unseen lines

    }   // end of not having an active view

// amend activity dialog if we have one (regardless, because we want to count total lines)

  App.m_bUpdateActivity = TRUE;

  }   // end of CMUSHclientDoc::StartNewLine

const bool CMUSHclientDoc::CheckScriptingAvailable (const char * sWhat,
                                                    const DISPID dispid,
                                                    const char * sScript)
  {

  if (sScript [0] == 0)
    return false;   // OK return - no script name given

  if (!GetScriptEngine ())   // no script engine?
    {
    if (!m_bWarnIfScriptingInactive)    // no warning
      return true;
    ColourNote ("white", "red", 
                TFormat ("%s function \"%s\" cannot execute "
                         "- scripting disabled/parse error.", 
               sWhat,     // trigger/alias/timer
               sScript)); // name of function
    return true;
    }

  if (dispid == DISPID_UNKNOWN)        // no dispatch id?
    {
    if (!m_bWarnIfScriptingInactive)    // no warning
      return true;
    ColourNote ("white", "red", 
                TFormat ("%s function \"%s\" not found "
                         "or had a previous error.", 
               sWhat,     // trigger/alias/timer
               sScript)); // name of function
    return true;    // error return
    }

  return false;   // OK return
  }

void CMUSHclientDoc::ExecuteTriggerScript (CTrigger * trigger_item,
                                            const CString strCurrentLine,
                                            CPaneLine & StyledLine)
  {

  if (CheckScriptingAvailable ("Trigger", trigger_item->dispid, trigger_item->strProcedure))
     return;

  CString strType = "trigger";
  CString strReason =  TFormat ("processing trigger \"%s\" when matching line: \"%s\"", 
                            (LPCTSTR) trigger_item->strLabel, (LPCTSTR) strCurrentLine);

  // get unlabelled trigger's internal name
  const char * pLabel = trigger_item->strLabel;
  if (pLabel [0] == 0)
     pLabel = GetTriggerRevMap () [trigger_item].c_str ();

  if (GetScriptEngine () && GetScriptEngine ()->UsesEmbeddedCallback ())
    {
    list<double> nparams;
    list<string> sparams;
    sparams.push_back (pLabel);
    sparams.push_back ((LPCTSTR) strCurrentLine);
    trigger_item->bExecutingScript = true;     // cannot be deleted now
    GetScriptEngine ()->ExecuteLua (trigger_item->dispid, 
                                   trigger_item->strProcedure, 
                                   eTriggerFired,
                                   strType, 
                                   strReason, 
                                   nparams,
                                   sparams, 
                                   trigger_item->nInvocationCount,
                                   trigger_item->regexp,
                                   NULL,        // no map of strings
                                   &StyledLine);  // but we *do* have a styled line
    trigger_item->bExecutingScript = false;     // can be deleted now
    return;
    }   // end of Lua

long i = 1;

  // prepare for the arguments:
  //   1. Trigger name
  //   2. Line that triggered the trigger
  //   3. The 10 wildcards (1-relative array)

  // WARNING - arguments should appear in REVERSE order to what the sub expects them!
  
  enum
    {
    eWildcards,
    eCurrentLine,
    eTriggerName,
    eArgCount,     // this MUST be last
    };    

  COleSafeArray sa;   // for wildcard list
  COleVariant args [eArgCount]; // arguments to script
  DISPPARAMS params = { args, NULL, eArgCount, 0 };

//    ::AfxMessageBox (trigger_item->trigger);

  args [eTriggerName] = pLabel;
  args [eCurrentLine] = strCurrentLine;

  // --------------- set up wildcards array ---------------------------
  sa.Clear ();
  // nb - to be consistent with %1, %2 etc. we will make array 1-relative
  sa.CreateOneDim (VT_VARIANT, MAX_WILDCARDS, NULL, 1);
  for (i = 1; i < MAX_WILDCARDS; i++)
    {
    COleVariant v (trigger_item->wildcards [i].c_str ());
    sa.PutElement (&i, &v);
    }
  // i should be MAX_WILDCARDS (10) now ;)
  COleVariant v (trigger_item->wildcards [0].c_str ()); // the whole matching line
  sa.PutElement (&i, &v);
  args [eWildcards] = sa;

  trigger_item->bExecutingScript = true;     // cannot be deleted now
  ExecuteScript (trigger_item->dispid,  
                 trigger_item->strProcedure,
                 eTriggerFired,
                 strType, 
                 strReason,
                 params, 
                 trigger_item->nInvocationCount); 
  trigger_item->bExecutingScript = false;     // can be deleted now

  } // end of CMUSHclientDoc::ExecuteTriggerScript 

void CMUSHclientDoc::ExecuteHotspotScript (DISPID & dispid,  // dispatch ID, will be set to DISPID_UNKNOWN on an error
                          LPCTSTR szProcedure,      // what name was in the hotspot callback
                          long iFlags,              // flags: ctrl, shift, whatever
                          LPCTSTR szHotspotID       // which hotspot
                          )

  {
  long nInvocationCount = 0;     // don't care

  if (CheckScriptingAvailable ("Hotspot", dispid, szProcedure))
   return;

  CString strType = "hotspot";
  CString strReason =  Translate ("processing hotspot callback");

  if (GetScriptEngine () && GetScriptEngine ()->UsesEmbeddedCallback ())
    {
    list<double> nparams;
    list<string> sparams;
    nparams.push_back (iFlags);
    sparams.push_back ((LPCTSTR) szHotspotID);
    GetScriptEngine ()->ExecuteLua (dispid, 
                                   szProcedure, 
                                   eHotspotCallback,
                                   strType, 
                                   strReason, 
                                   nparams,
                                   sparams, 
                                   nInvocationCount,
                                   NULL,        // no regexp
                                   NULL,        // no map of strings
                                   NULL);       // no styled line
    return;
    }   // end of Lua

  // prepare for the arguments:
  //   1. Flags
  //   2. Hotspot ID

  // WARNING - arguments should appear in REVERSE order to what the sub expects them!
  
  enum
    {
    eHotspotID,
    eFlags,
    eArgCount,     // this MUST be last
    };    

  COleVariant args [eArgCount]; // arguments to script
  DISPPARAMS params = { args, NULL, eArgCount, 0 };


  args [eHotspotID] = szHotspotID;
  args [eFlags] = iFlags;

  ExecuteScript (dispid,  
                 szProcedure,
                 eHotspotCallback,
                 strType, 
                 strReason,
                 params, 
                 nInvocationCount); 

  }   // end of CMUSHclientDoc::ExecuteHotspotScript

void CMUSHclientDoc::OnFileLogsession() 
{

// close log file if already open

  if (m_logfile)
   {

    // check they want to close it, if desired
    if (App.m_bConfirmLogFileClose)
      {
      CString str;
      str = TFormat ("Close log file %s?", (const char *) m_logfile_name);
      if (UMessageBox (str, MB_OKCANCEL | MB_ICONQUESTION)
            == IDCANCEL)
        return;
      }   // end of confirmation wanted

    CloseLog ();    // this writes out the log file postamble as well
    return;
    }

BOOL bAppendToLogFile = false;
int iLines = 0;  
BOOL bWriteWorldName = m_bWriteWorldNameToLog;
CString strPreamble;

if (!m_bLogRaw)
  {
  CLogDlg dlg;
  int lines_then = m_LineList.GetCount ();

    dlg.m_doc = this;
    dlg.m_lines = m_LineList.GetCount ();   // maximum number of lines to capture
    dlg.m_bAppendToLogFile = App.m_bAppendToLogFiles;
    dlg.m_bWriteWorldName = m_bWriteWorldNameToLog;
    dlg.m_strPreamble = m_strLogFilePreamble;
    dlg.m_bLogOutput = m_bLogOutput;
    dlg.m_bLogInput = m_log_input;
    dlg.m_bLogNotes = m_bLogNotes;

    if (dlg.DoModal () != IDOK)
      return;

    // remember whether they wanted to log notes and/or input

    m_bLogOutput = dlg.m_bLogOutput;
    m_log_input = dlg.m_bLogInput;
    m_bLogNotes = dlg.m_bLogNotes;

  // allow for extra lines added to the buffer since we put up the dialog

    if (dlg.m_lines > 0 && m_LineList.GetCount () > lines_then)
      dlg.m_lines += m_LineList.GetCount () - lines_then;

    bAppendToLogFile = dlg.m_bAppendToLogFile;
    iLines = dlg.m_lines;
    bWriteWorldName = dlg.m_bWriteWorldName;
    strPreamble = dlg.m_strPreamble;

  }   // end of not a raw log

// find the output file name

  CString suggested_name;
  CString filename;
  if (!m_strAutoLogFileName.IsEmpty ())
    {
    CTime theTime;
    theTime = CTime::GetCurrentTime();

    suggested_name = FormatTime (theTime, m_strAutoLogFileName);
    }
  else
    {
    CString strWorldName = m_mush_name;
    // fix up name to remove characters that are invalid

    int i;
    while ((i = strWorldName.FindOneOf ("<>\"|?:#%;/\\")) != -1)
      strWorldName = strWorldName.Left (i) + strWorldName.Mid (i + 1);
    suggested_name = Make_Absolute_Path (App.m_strDefaultLogFileDirectory);
    suggested_name += strWorldName;
    suggested_name += " log";
    }

  CFileDialog filedlg (FALSE,   // saving the file
                   "txt",    // default extension
                   "",
                   OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                   (bAppendToLogFile ? 0 : OFN_OVERWRITEPROMPT),
                   "Text files (*.txt)|*.txt|All files (*.*)|*.*||",    // filter 
                   NULL);  // parent window


  filedlg.m_ofn.lpstrTitle = "Log file name";
  filedlg.m_ofn.lpstrFile = filename.GetBuffer (_MAX_PATH); // needed!! (for Win32s)  
  if (App.platform == VER_PLATFORM_WIN32s)
    strcpy (filedlg.m_ofn.lpstrFile, "");
  else
    strcpy (filedlg.m_ofn.lpstrFile, suggested_name);

  ChangeToFileBrowsingDirectory ();
	int nResult = filedlg.DoModal();
  ChangeToStartupDirectory ();

  if (nResult != IDOK)
    return;

  m_logfile_name = filedlg.GetPathName ();

  m_logfile = fopen (m_logfile_name, bAppendToLogFile ? "a+" : "w");
	
  // close and re-open to make sure it is in the disk directory
  if (m_logfile)
    {
    fclose (m_logfile);
    m_logfile = fopen (m_logfile_name, bAppendToLogFile ? "a+" : "w");
    }

	if (!m_logfile)
	  {
    CString str;

    str = TFormat ("Unable to open log file \"%s\"", (LPCTSTR) m_logfile_name);
	  UMessageBox (str);
	  
    return;
	  }	

  // no preamble or other crap for a raw log file
   if (m_bLogRaw)
     return;

POSITION pos = NULL;
int line;

// if they want previous part of session written, do it

  if (iLines > 0)
    {

// if they want to use whole buffer, just use it

    if (iLines >= m_LineList.GetCount ())
      pos = m_LineList.GetHeadPosition ();
    else
      {

// otherwise count backwards the required number of lines

      pos = m_LineList.GetTailPosition ();

      for (line = 0; line < iLines && pos; line++)
        m_LineList.GetPrev (pos);

// if no pos, start at top

      if (!pos)
        pos = m_LineList.GetHeadPosition ();
      }   // end of not starting from the top

    } // end of wanting retrospective logging
  else
    pos = NULL;

  CLine * pLine;
  CString strTime;
  CTime theTime;

  if (pos)
    theTime = m_LineList.GetAt (pos)->m_theTime;
  else
    theTime = CTime::GetCurrentTime();

// write log file preamble if wanted

  if (!strPreamble.IsEmpty ())
    {
    // allow %n for newline
    strPreamble.Replace ("%n", "\n");

    // allow for time-substitution strings
    strPreamble = FormatTime (theTime, strPreamble, m_bLogHTML);

    // this is open in text mode, don't want \r\r\n
    strPreamble.Replace (ENDLINE, "\n");

    WriteToLog (strPreamble); 
    WriteToLog ("\n", 1);
    }

  if (bWriteWorldName)
    {

  // write preamble so they know what this is all about


    strTime = theTime.Format (TranslateTime ("%A, %B %d, %Y, %#I:%M %p"));

    CString strPreamble = m_mush_name;
    strPreamble += " - ";
    strPreamble += strTime;

    if (m_bLogHTML)
      {
      if (bAppendToLogFile)
        WriteToLog ("<br>\n");     // put blank line after previous output
      WriteToLog (FixHTMLString (strPreamble)); 
      WriteToLog ("<br>\n");     // put blank line after previous output
      }
    else
      {
      if (bAppendToLogFile)
        WriteToLog ("\n", 1);     // put blank line after previous output
      WriteToLog (strPreamble); 
      WriteToLog ("\n", 1);
      }

  // turn previous line into a line of hyphens, and print that

    CString strHyphens ('-', strPreamble.GetLength ());

    WriteToLog (strHyphens); 
    if (m_bLogHTML)
       WriteToLog ("<br><br>");
    else
      WriteToLog ("\n\n");
    }   // end of wanting world name written

// now output retrospective log lines, if any

  bool bNewLine = true;

  while (pos)
    {
    pLine = m_LineList.GetNext (pos);

    // respect "log my input" flags
    if ((pLine->flags & LOG_LINE) || (pLine->flags & NOTE_OR_COMMAND))   // log this line
      if (!((pLine->flags & USER_INPUT) && !m_log_input)) // this is input and we want it
        if (!((pLine->flags & COMMENT) && !m_bLogNotes))    // this is a note and we want it
          {
          if (bNewLine)
            {
            // get appropriate preamble
            CString strPreamble = m_strLogLinePreambleOutput;
            if (pLine->flags & USER_INPUT)
               strPreamble = m_strLogLinePreambleInput;
            else if (pLine->flags & COMMENT)
               strPreamble = m_strLogLinePreambleNotes;

            // allow %n for newline
            strPreamble.Replace ("%n", "\n");

            if (strPreamble.Find ('%') != -1)
              strPreamble = FormatTime (pLine->m_theTime, strPreamble, m_bLogHTML);

            // line preamble
            WriteToLog (strPreamble); 
            }   // end of starting a new line

          // line itself
          CString strLine = CString (pLine->text, pLine->len);

          // fix up HTML sequences

          if (m_bLogHTML && m_bLogInColour)
            {
             if (!pLine->styleList.IsEmpty ())
               {

                int iCol = 0;

                for (POSITION style_pos = pLine->styleList.GetHeadPosition(); style_pos; )
                  {
                  COLORREF colour1,
                           colour2;

                  CStyle * pStyle = pLine->styleList.GetNext (style_pos);

                  int iLength = pStyle->iLength;

                  // ignore zero length styles
                  if (iLength <= 0)
                     continue;

                  GetStyleRGB (pStyle, colour1, colour2); // find what colour this style is

                  WriteToLog (CFormat ("<font color=\"#%02X%02X%02X\">",
                                        GetRValue (colour1),
                                        GetGValue (colour1),
                                        GetBValue (colour1)));

                  if (pStyle->iFlags & UNDERLINE)
                    WriteToLog ("<u>");

                  WriteToLog (FixHTMLString (strLine.Mid (iCol, iLength)));

                  if (pStyle->iFlags & UNDERLINE)
                    WriteToLog ("</u>");

                  iCol += iLength; // new column
                  WriteToLog ("</font>");

                  }   // end of doing each style

               }  // end of having at least one style

            WriteToLog ("\n", 1);

            }
          else if (m_bLogHTML)
            WriteToLog (FixHTMLString (strLine));
          else                 
            WriteToLog (strLine);

          if (pLine->hard_return)
            {
            // get appropriate Postamble
            CString strPostamble = m_strLogLinePostambleOutput;
            if (pLine->flags & USER_INPUT)
               strPostamble = m_strLogLinePostambleInput;
            else if (pLine->flags & COMMENT)
               strPostamble = m_strLogLinePostambleNotes;

            // allow %n for newline
            strPostamble.Replace ("%n", "\n");

            if (strPostamble.Find ('%') != -1)
              strPostamble = FormatTime (pLine->m_theTime, strPostamble, m_bLogHTML);

            // line Postamble
            WriteToLog (strPostamble); 

            if (!(m_bLogHTML && m_bLogInColour))  // colour logging has already got a newline
              WriteToLog ("\n", 1);
            }

          }   // end of logging this line

    // remember so we know whether to write a preamble at the start of the
    // *next* line
    bNewLine = pLine->hard_return; 
    }

  fflush (m_logfile);

} // end of CMUSHclientDoc::OnFileLogsession

void CMUSHclientDoc::OnUpdateFileLogsession(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus

  pCmdUI->SetCheck (m_logfile != NULL);

}

void CMUSHclientDoc::OnUpdateStatuslineLines(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus

CString str;

long nLines;

  nLines = m_total_lines;

// if last line is empty, don't count it

  if (m_pCurrentLine->len == 0)
    nLines--;

  if (nLines < 0)
    nLines = 0;

  str.Format ("%ld", nLines);

  pCmdUI->Enable(TRUE);
  pCmdUI->SetText(str);	
}

void CMUSHclientDoc::OnUpdateStatuslineMushname(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable(TRUE);
  pCmdUI->SetText(m_mush_name);	
}






void CMUSHclientDoc::OnGameWraplines() 
{
  m_wrap = !m_wrap;
  SetModifiedFlag (TRUE);
}

void CMUSHclientDoc::OnUpdateGameWraplines(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
pCmdUI->SetCheck (m_wrap);
}


void CMUSHclientDoc::OnUpdateStatuslineTime(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
CString strMsg;

  if (m_LineList.IsEmpty ())
    return;

  CTimeSpan ts = CTime::GetCurrentTime() - m_tStatusTime;

  if (m_iConnectPhase != eConnectConnectedToMud)
    strMsg = "";
  else
    if (ts.GetDays () > 0)
      strMsg = ts.Format ("%Dd %Hh %Mm %Ss");
    else
      if (ts.GetHours () > 0)
        strMsg = ts.Format ("%Hh %Mm %Ss");
      else
        if (ts.GetMinutes () > 0)
          strMsg = ts.Format ("%Mm %Ss");
        else
          strMsg = ts.Format ("%Ss");

  pCmdUI->Enable(TRUE);
  pCmdUI->SetText(strMsg);	
	
}

void CMUSHclientDoc::OnUpdateStatuslineLog(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
pCmdUI->Enable (m_logfile != NULL);
pCmdUI->SetText("LOG");
}

BOOL CMUSHclientDoc::OnOpenDocument(LPCTSTR lpszPathName) 
{
	if (!CDocument::OnOpenDocument(lpszPathName))
		return FALSE;
	
  SetUpOutputWindow ();
  OpenSession ();
  
  return TRUE;


}



void CMUSHclientDoc::ChangeFont (const int nHeight, 
                                 const char * lpszFacename,
                                 const int nWeight, 
                                 const DWORD iFontCharset,
                                 const BOOL bShowBold,
                                 const BOOL bShowItalic,
                                 const BOOL bShowUnderline,
                                 const unsigned short iLineSpacing)
{

// Load the font we want to use.

int i;

  for (i = 0; i < NUMITEMS (m_font); i++)  
    {
    delete m_font [i];         // get rid of old font
    m_font [i] = NULL;
    }

  CDC dc;

  dc.CreateCompatibleDC (NULL);

  for (i = 0; i < NUMITEMS (m_font); i++)  
    {
     m_font [i] = new CFont;    // create new font

     if (!m_font [i])
      {
      for (int j = 0; j < NUMITEMS (m_font); j++)  
        {
        delete m_font [j];         // get rid of old font
        m_font [j] = NULL;
        }
      TMessageBox ("Unable to allocate memory for screen font");
      return;
      }

    
     // if height is zero, default to 10 so it doesn't look stupid
     int lfHeight = -MulDiv(nHeight ? nHeight : 10, dc.GetDeviceCaps(LOGPIXELSY), 72);

     m_font [i]->CreateFont(lfHeight, // int nHeight, 
				    0, // int nWidth, 
				    0, // int nEscapement, 
				    0, // int nOrientation, 
            bShowBold ? ((i & HILITE) ? FW_BOLD : FW_NORMAL) : nWeight, // int nWeight, 
            bShowItalic ? (i & BLINK) != 0 : 0, // BYTE bItalic, 
            bShowUnderline ? (i & UNDERLINE) != 0 : 0, // BYTE bUnderline, 
            i >= 8,     // BYTE cStrikeOut, 
            iFontCharset, // BYTE nCharSet, 
            0, // BYTE nOutPrecision, 
            0, // BYTE nClipPrecision, 
            0, // BYTE nQuality, 
            MUSHCLIENT_FONT_FAMILY, // BYTE nPitchAndFamily,    // was  FF_DONTCARE
            lpszFacename);// LPCTSTR lpszFacename );

    }   // end of allocating 16 fonts

   // Get the metrics of the font - use the bold one - it will probably be wider

    dc.SelectObject(m_font [HILITE]);
    
    TEXTMETRIC tm;
    dc.GetTextMetrics(&tm);

    if (iLineSpacing)
      m_FontHeight = iLineSpacing;    // override
    else
      m_FontHeight = tm.tmHeight; 

    m_FontWidth = tm.tmAveCharWidth; 

    for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
    	{
    	CView* pView = GetNextView(pos);
	
    	if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
      	{
    		CMUSHView* pmyView = (CMUSHView*)pView;

    		pmyView->sizewindow ();

    	  }	
      }

    

} // end of CMUSHclientDoc::ChangeFont

void CMUSHclientDoc::ChangeInputFont (const int nHeight, 
                            const char * lpszFacename,
                            const int nWeight, 
                            const DWORD iCharset,
                            const BYTE bItalic)
{
// Load the font we want to use.

   delete m_input_font;         // get rid of old font

   m_input_font = new CFont;    // create new font

   if (!m_input_font)
    {
    TMessageBox ("Unable to allocate memory for screen font");
    return;
    }

CDC dc;

dc.CreateCompatibleDC (NULL);

   int lfHeight = -MulDiv(nHeight ? nHeight : 10, dc.GetDeviceCaps(LOGPIXELSY), 72);

   m_input_font->CreateFont(lfHeight, // int nHeight, 
				  0, // int nWidth, 
				  0, // int nEscapement, 
				  0, // int nOrientation, 
				  nWeight, // int nWeight, 
				  bItalic, // BYTE bItalic, 
				  0, // BYTE bUnderline, 
          0, // BYTE cStrikeOut, 
          iCharset, // BYTE nCharSet, 
          0, // BYTE nOutPrecision, 
          0, // BYTE nClipPrecision, 
          0, // BYTE nQuality, 
          MUSHCLIENT_FONT_FAMILY, // BYTE nPitchAndFamily,   // was FF_DONTCARE
          lpszFacename);// LPCTSTR lpszFacename );

    // Get the metrics of the font.

    dc.SelectObject(m_input_font);
    
    TEXTMETRIC tm;
    dc.GetTextMetrics(&tm);

    m_InputFontHeight = tm.tmHeight; 
    m_InputFontWidth = tm.tmAveCharWidth; 

    // fix up all input windows
    if (m_input_font)
      {
#ifndef _WIN32
      // Non-Windows: no CSendView; use the wx callback to update the input bar.
      if (m_pWorldView)
        {
        bool bold   = (nWeight >= 600);  // 600 = FW_SEMIBOLD
        bool italic = (bItalic != 0);
        EmitInputFont (std::string (lpszFacename ? lpszFacename : ""),
                             nHeight, bold, italic);
        }
#else
      for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
        {
        CView* pView = GetNextView(pos);

        if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
          {
          CSendView* pmyView = (CSendView*)pView;

          pmyView->SendMessage (WM_SETFONT,
                                     (WPARAM) m_input_font->m_hObject,
                                     MAKELPARAM (TRUE, 0));

          pmyView->AdjustCommandWindowSize ();
          }	  // end of being a CSendView
        }
#endif
      }

} // end of CMUSHclientDoc::ChangeInputFont



void CMUSHclientDoc::setupstrings (void)
  {
  m_macros  [MAC_UP]        = "up";
  m_macros  [MAC_DOWN]      = "down";   
  m_macros  [MAC_NORTH]     = "north";  
  m_macros  [MAC_SOUTH]     = "south";  
  m_macros  [MAC_EAST]      = "east";   
  m_macros  [MAC_WEST]      = "west";   
  m_macros  [MAC_EXAMINE]   = "examine ";
  m_macros  [MAC_LOOK]      = "look ";   
  m_macros  [MAC_PAGE]      = "page ";   
  m_macros  [MAC_SAY]       = "say ";    
  m_macros  [MAC_WHISPER]   = "whisper ";
  m_macros  [MAC_DOING]     = "DOING";  
  m_macros  [MAC_WHO]       = "WHO";    
  m_macros  [MAC_DROP]      = "drop ";   
  m_macros  [MAC_TAKE]      = "take ";   
  m_macros  [MAC_LOGOUT]    = "LOGOUT";   
  m_macros  [MAC_QUIT]      = "QUIT";   

  m_keypad  [eKeypad_0    ]  = "look";        
  m_keypad  [eKeypad_1    ]  = "sw";        
  m_keypad  [eKeypad_2    ]  = "south";        
  m_keypad  [eKeypad_3    ]  = "se";        
  m_keypad  [eKeypad_4    ]  = "west";        
  m_keypad  [eKeypad_5    ]  = "WHO";        
  m_keypad  [eKeypad_6    ]  = "east";        
  m_keypad  [eKeypad_7    ]  = "nw";        
  m_keypad  [eKeypad_8    ]  = "north";        
  m_keypad  [eKeypad_9    ]  = "ne";        
  m_keypad  [eKeypad_Dot  ]  = "hide";      
  m_keypad  [eKeypad_Slash]  = "inventory";    
  m_keypad  [eKeypad_Star ]  = "score";     
  m_keypad  [eKeypad_Dash ]  = "up";     
  m_keypad  [eKeypad_Plus ]  = "down";      

  m_macro_type  [MAC_UP]       = SEND_NOW;
  m_macro_type  [MAC_DOWN]     = SEND_NOW;   
  m_macro_type  [MAC_NORTH]    = SEND_NOW;  
  m_macro_type  [MAC_SOUTH]    = SEND_NOW;  
  m_macro_type  [MAC_EAST]     = SEND_NOW;   
  m_macro_type  [MAC_WEST]     = SEND_NOW;   
  m_macro_type  [MAC_EXAMINE]  = REPLACE_COMMAND;
  m_macro_type  [MAC_LOOK]     = REPLACE_COMMAND;   
  m_macro_type  [MAC_PAGE]     = REPLACE_COMMAND;   
  m_macro_type  [MAC_SAY]      = REPLACE_COMMAND;    
  m_macro_type  [MAC_WHISPER]  = REPLACE_COMMAND;
  m_macro_type  [MAC_DOING]    = SEND_NOW;  
  m_macro_type  [MAC_WHO]      = SEND_NOW;    
  m_macro_type  [MAC_DROP]     = REPLACE_COMMAND;   
  m_macro_type  [MAC_TAKE]     = REPLACE_COMMAND;   
  m_macro_type  [MAC_LOGOUT]   = SEND_NOW;   
  m_macro_type  [MAC_QUIT]     = SEND_NOW;   

  } // end of CMUSHclientDoc::setupstrings

bool CMUSHclientDoc::SendToMushHelper (CFile * f, 
                                       CString & strPreamble,
                                       CString & strLinePreamble,
                                       CString & strLinePostamble,
                                       CString & strPostamble,
                                       const BOOL bCommentedSoftcode,
                                       const long nLineDelay,
                                       const long nPerLines,
                                       const BOOL bConfirm,
                                       const BOOL bEcho)
  {
CString str;
CString full_line;

CConfirmPreamble dlg;

// find paste size and number of lines

DWORD nLength = f->GetLength ();
DWORD nLines = 0,
      nCurrentLine = 0;


  try
    {
    CArchive ar (f, CArchive::load);
    while (ar.ReadString (str))
      nLines++;
    ar.Close ();
    }
  catch (...)
    {
    TMessageBox ("An error occurred calculating amount to send to world", 
                      MB_ICONEXCLAMATION);
    return false;
    } // end of catching an exception

  f->SeekToBegin ();
  
// ask user to confirm preamble, postamble etc., and warn them of file size

  dlg.m_strPasteMessage.Format ("About to send: %ld character%s, %ld line%s to %s.",
                                nLength,
                                nLength == 1 ? "" : "s",
                                nLines,
                                nLines == 1 ? "" : "s",
                                (LPCTSTR) m_mush_name);
      
  dlg.m_strPreamble = strPreamble;
  dlg.m_strLinePreamble = strLinePreamble;
  dlg.m_strLinePostamble = strLinePostamble;
  dlg.m_strPostamble = strPostamble;
  dlg.m_bCommentedSoftcode = bCommentedSoftcode;
  dlg.m_iLineDelay = nLineDelay;
  dlg.m_nLineDelayPerLines = nPerLines;
  dlg.m_bEcho = bEcho;
 
  if (bConfirm)
    if (dlg.DoModal () != IDOK)
      return false;

  CArchive ar (f, CArchive::load);

  CProgressDlg ProgressDlg;                   
  ProgressDlg.Create ();                           
  ProgressDlg.SetStatus (Translate ("Sending to world..."));               
  ProgressDlg.SetRange (0, nLines);
  ProgressDlg.SetWindowText (Translate ("Sending..."));                              

  try
    {

    // send preamble

    if (!dlg.m_strPreamble.IsEmpty ())
      SendMsg (dlg.m_strPreamble, dlg.m_bEcho, false, LoggingInput ());
    
    CString strSoftcode;
    bool bHashCommenting = false;
    bool bFirstTime = true;
    int iLineCount = 0;

    while (ar.ReadString (str))
      {

      nCurrentLine++;
      ProgressDlg.SetPos (nCurrentLine); 

      if (ProgressDlg.CheckCancelButton())     // abort if user cancels
        break;

      if (dlg.m_bCommentedSoftcode)
        {

        str.TrimLeft ();

        // commented softcode will ignore blank lines
        if (str.IsEmpty ())
          continue;

        // if we find a # on the first non-blank line, assume hash-commenting
        if (bFirstTime && str [0] == '#')
          bHashCommenting = true;

        bFirstTime = false;

        // a line containing "-" ends the line
        if (str == "-")
          {
          full_line = dlg.m_strLinePreamble;
          full_line += strSoftcode;
          full_line += dlg.m_strLinePostamble;

          SendMsg (full_line, dlg.m_bEcho, false, LoggingInput ());   // send the line
          if (dlg.m_iLineDelay > 0)
            {
            if (++iLineCount >= dlg.m_nLineDelayPerLines)
              {
              Sleep (dlg.m_iLineDelay);
              iLineCount = 0;
              }
            }
          strSoftcode.Empty ();
          continue;
          }

        if (bHashCommenting)
          {
          // if hash commenting, ignore lines starting with a hash
          if (str [0] == '#')
            continue;
          } // end of hash commenting
        else
          {
          int iPos = str.Find ("@@");   // find the @@
          if (iPos != -1)
            {
            str = str.Left (iPos);
            } // end of finding @@
          } // not hash commenting

//        str.TrimRight ();  // no trailing spaces
        str.TrimLeft ();  // no leading spaces
        strSoftcode += str;

        continue;   // don't write this line yet
        } // end of commented softcode

// send the final line - attach preamble and postamble

      full_line = dlg.m_strLinePreamble;
      full_line += str;
      full_line += dlg.m_strLinePostamble;
      SendMsg (full_line, dlg.m_bEcho, false, LoggingInput ());   // send the line
      if (dlg.m_iLineDelay > 0)
        {
        if (++iLineCount >= dlg.m_nLineDelayPerLines)
          {
          Sleep (dlg.m_iLineDelay);
          iLineCount = 0;
          }
        }
      }   // end of reading in the file

    if (dlg.m_bCommentedSoftcode)
      {
      full_line = dlg.m_strLinePreamble;
      full_line += strSoftcode;
      full_line += dlg.m_strLinePostamble;
      SendMsg (full_line, dlg.m_bEcho, false, LoggingInput ());   // send the line
      if (dlg.m_iLineDelay > 0)
        {
        if (++iLineCount >= dlg.m_nLineDelayPerLines)
          {
          Sleep (dlg.m_iLineDelay);
          iLineCount = 0;
          }
        }
      }   // end of sending commented softcode
    
    } // end of try block

  catch (...)
    {
    TMessageBox ("An error occurred when sending/pasting to this world", MB_ICONEXCLAMATION);
    } // end of catching an exception

  ar.Close ();

// send postamble

  if (!dlg.m_strPostamble.IsEmpty ())
    SendMsg (dlg.m_strPostamble, dlg.m_bEcho, false, LoggingInput ());

  return true;
  } // end of CMUSHclientDoc::SendToMushHelper

void CMUSHclientDoc::OnGamePastefile() 
{

CStdioFile * f = NULL;
CString str;
CString filename;

  CFileDialog filedlg (TRUE,   // loading the file
                       "txt",    // default extension
                       "",  // suggested name
                       OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                       "MUD files (*.mud;*.mush)|*.mud; *.mush|"
                       "Text files (*.txt)|*.txt|All files (*.*)|*.*||",    // filter 
                       NULL);  // parent window

  str = "File to paste into ";
  str += m_mush_name;
  filedlg.m_ofn.lpstrTitle = str;
  filedlg.m_ofn.lpstrFile = filename.GetBuffer (_MAX_PATH); // needed!! (for Win32s)  
  strcpy (filedlg.m_ofn.lpstrFile, "");

  ChangeToFileBrowsingDirectory ();
	int nResult = filedlg.DoModal();
  ChangeToStartupDirectory ();

  if (nResult != IDOK)
    return;    // cancelled dialog

  try
    {
    f = new CStdioFile (filedlg.GetPathName (), CFile::modeRead | CFile::shareDenyWrite);

    SendToMushHelper (f, 
                     m_file_preamble,
                     m_line_preamble,
                     m_line_postamble,
                     m_file_postamble,
                     m_bFileCommentedSoftcode,
                     m_nFileDelay,
                     m_nFileDelayPerLines,
                     m_bConfirmOnSend,
                     m_bSendEcho);
    
    } // end of try block

  catch (CFileException * e)
    {
    if (e->m_cause != CFileException::endOfFile)
      TMessageBox ("Unable to open or read the requested file", MB_ICONEXCLAMATION);
    e->Delete ();
    } // end of catching a file exception

  delete f;       // delete file

}

BOOL GetClipboardContents (CString & strClipboard, const bool bUnicode, const bool bWarning)
  {

  strClipboard.Empty ();

#ifndef _WIN32
  // On non-Windows use the wxWidgets clipboard hook installed by wx_mfc_impl.cpp.
  if (g_wxhook_get_clipboard_text) {
    char* text = g_wxhook_get_clipboard_text();
    if (!text) {
      if (bWarning)
        TMessageBox("Unable to get Clipboard data");
      return FALSE;
    }
    strClipboard = CString(text);
    free(text);
    return TRUE;
  }
  return FALSE;
#else

// get the contents of the clipboard into a local string

HGLOBAL hData = NULL;
bool bHaveUnicode = false;

// Open clipboard

  if (!::OpenClipboard(Frame.GetSafeHwnd ()) )
       {
       if (bWarning)
          TMessageBox( "Cannot open the Clipboard" );
       return FALSE;
       }


/* debugging - enumerate clipboard formats 

// note: CF_UNICODETEXT = 13

UINT iEnum = 0;

  do
    {
    iEnum = EnumClipboardFormats (iEnum);
    TRACE1 ("Clipboard contains %i\n", iEnum);
    } while (iEnum);

*/

// Get the clipboard data - Unicode first if required

  if (bUnicode)
    hData = ::GetClipboardData( CF_UNICODETEXT);

  bHaveUnicode = hData != NULL;

// No Unicode? Try text ,,,

  if (!bHaveUnicode)
    if ( (hData = ::GetClipboardData( CF_TEXT )) == NULL )
     {
        if (bWarning)
          TMessageBox( "Unable to get Clipboard data" );
        ::CloseClipboard();
        return FALSE;
     }

// Lock the handle in order to get a pointer to real memory

   char * p = (char *) GlobalLock (hData);

   if (!p)
      {
      if (bWarning)
        TMessageBox( "Unable to lock memory for Clipboard data" );
      ::CloseClipboard();
      return FALSE;
      }

   // convert Unicode to UTF-8
   if (bHaveUnicode)
     {
     // count number of bytes needed
     int iLength = WideCharToMultiByte (CP_UTF8, 0, (LPCWSTR) p, -1, NULL, 0, NULL, NULL);
     char * buf = strClipboard.GetBuffer (iLength);
     WideCharToMultiByte (CP_UTF8, 0, (LPCWSTR) p, -1, buf, iLength, NULL, NULL);
     strClipboard.ReleaseBuffer (iLength);
     }
   else
     strClipboard = CString (p, strlen (p));

   GlobalUnlock (hData);

// Close the clipboard

   CloseClipboard();

   return TRUE;

#endif // !_WIN32
  } // end of GetClipboardContents

/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       putontoclipboard                              */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */


// puts "data" onto the clipboard

BOOL putontoclipboard (const CString & data, const bool bUnicode)
  {

#ifndef _WIN32
  // On non-Windows use the wxWidgets clipboard hook installed by wx_mfc_impl.cpp.
  if (g_wxhook_set_clipboard_text)
    g_wxhook_set_clipboard_text(static_cast<const char*>(data));
  return FALSE;   // FALSE = success (MFC convention for this function)
#else

// Open clipboard

  if (!::OpenClipboard(Frame.GetSafeHwnd ()) )
       {
       TMessageBox( "Cannot open the Clipboard" );
       return TRUE;
       }

// Empty the clipboard, which also gives ownership to us

    ::EmptyClipboard ();

// Allocate memory for the clipboard


   HGLOBAL hData = GlobalAlloc (GMEM_MOVEABLE, strlen (data) + 1);

   if (!hData)
      {
      TMessageBox( "Unable to allocate memory for Clipboard data" );
      ::CloseClipboard();
      return TRUE;
      }

// Lock the handle in order to get a pointer to real memory

   char * p = (char *) GlobalLock (hData);

   if (!p)
      {
      TMessageBox( "Unable to lock memory for Clipboard text data" );
      ::CloseClipboard();
      return TRUE;
      }

// Copy the field into the allocated memory

   strncpy (p, data, strlen (data) + 1);

   GlobalUnlock (hData);

// Set the clipboard data

   if ( ::SetClipboardData( CF_TEXT, hData ) == NULL )
     {
      TMessageBox( "Unable to set Clipboard text data" );
      ::CloseClipboard();
      return TRUE;
     }


   if (bUnicode)
     {

  // Allocate memory for the clipboard

     int iLength = MultiByteToWideChar (CP_UTF8, 0, data, -1, NULL, 0);

     HGLOBAL hData = GlobalAlloc (GMEM_MOVEABLE, iLength * sizeof(WCHAR));

     if (!hData)
        {
        TMessageBox( "Unable to allocate memory for Clipboard Unicode data" );
        ::CloseClipboard();
        return TRUE;
        }

  // Lock the handle in order to get a pointer to real memory

     char * p = (char *) GlobalLock (hData);

     if (!p)
        {
        TMessageBox( "Unable to lock memory for Clipboard data" );
        ::CloseClipboard();
        return TRUE;
        }

  // Copy the field into the allocated memory

     MultiByteToWideChar (CP_UTF8, 0,    
                            data, -1,  // input
                            (LPWSTR) p, iLength);         // output

     GlobalUnlock (hData);

  // Set the clipboard data

     if ( ::SetClipboardData( CF_UNICODETEXT, hData ) == NULL )
       {
        TMessageBox( "Unable to set Clipboard Unicode data" );
        ::CloseClipboard();
        return TRUE;
       }


     }  // end of Unicode as well

// Close the clipboard

  ::CloseClipboard();

  return FALSE;

#endif // !_WIN32
  } // end of putontoclipboard

// CheckLabel is single-sourced in utilities_shared.cpp (declared in stdafx.h).

void CMUSHclientDoc::OnEditPastetomush()
{

CString strClipboard;

if (!GetClipboardContents (strClipboard, m_bUTF_8))
  return;

CMemFile f ((unsigned char *) (const char *) strClipboard, strClipboard.GetLength ());

  SendToMushHelper (&f, 
                   m_paste_preamble,
                   m_pasteline_preamble,
                   m_pasteline_postamble,
                   m_paste_postamble,
                   m_bPasteCommentedSoftcode,
                   m_nPasteDelay,
                   m_nPasteDelayPerLines,
                   m_bConfirmOnPaste,
                   m_bPasteEcho);

}

void CMUSHclientDoc::OnUpdateSessionOpen(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (m_iConnectPhase == eConnectConnectedToMud);
}


// OnConnect: single-sourced in doc_shared.cpp (ITransport T3b)


// HostNameResolved: gone (T5) — the Windows transport adapter owns the
// WSAAsyncGetHostByName completion (pal/win_transport.cpp).

void CMUSHclientDoc::OnConnectionConnect() 
{
  ConnectSocket();
}

void CMUSHclientDoc::OnUpdateConnectionConnect(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
	pCmdUI->Enable (m_iConnectPhase == eConnectNotConnected && 
                  m_server != "0.0.0.0");
}

// OnConnectionDisconnect: single-sourced in doc_shared.cpp (ITransport T3c)

void CMUSHclientDoc::OnUpdateConnectionDisconnect(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
	pCmdUI->Enable (m_iConnectPhase != eConnectNotConnected);
}

BOOL CMUSHclientDoc::FixUpOutputBuffer (int nNewBufferSize)
  {

POSITION pos;
long i;

  if (!m_pCurrentLine)
    return FALSE;             // too early for this crap

  Frame.SetStatusMessageNow (Translate ("Recalculating line positions"));

// delete from head the excess lines

  while (m_LineList.GetCount () > nNewBufferSize)
    RemoveChunk ();

// do a new line positions array

  delete [] m_pLinePositions;
  m_pLinePositions = new POSITION [(nNewBufferSize / JUMP_SIZE) + 1];

// clear all elements

  for (i = 0; i <= nNewBufferSize / JUMP_SIZE; i++)
    m_pLinePositions [i] = NULL;

// re-seed positions array

  for(i = 0, pos=m_LineList.GetHeadPosition();
      pos;
      i++)
        {
        if (i % JUMP_SIZE == 0)
          m_pLinePositions [i / JUMP_SIZE] = pos;
        m_LineList.GetNext (pos);
        } // end of for loop

// refresh view to show different scroll bars

  for(pos=GetFirstViewPosition();pos!=NULL;)
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
		  CMUSHView* pmyView = (CMUSHView*)pView;

		  pmyView->addedstuff();
	    }
    }

#ifndef _WIN32
  // Non-Windows: the view loop above is a no-op (no CMUSHView registered).
  // Tell the wxWidgets output pane to repaint so that the scroll-bar state
  // and visible content reflect the newly resized line buffer.
  UpdateAllViews (NULL);
#endif

  ShowStatusLine ();

  return TRUE;

  } // end of FixUpOutputBuffer



void CMUSHclientDoc::OnUpdateEditPastetomush(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (m_iConnectPhase == eConnectConnectedToMud && 
                  IsClipboardFormatAvailable (CF_TEXT));
}

void CMUSHclientDoc::OnUpdateGamePreferences(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (m_iConnectPhase == eConnectNotConnected ||
                  m_iConnectPhase == eConnectConnectedToMud);   // no changing of address whilst connecting
}


// finds a line in the line list

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

long CMUSHclientDoc::GetLastLine (void)
  {

  // before this is allocated, we have no lines
  if (!m_pCurrentLine)
    return 0;

  long lastline = m_LineList.GetCount () - 1;
  if (m_pCurrentLine->len > 0)
    lastline++;

  return lastline;

  } // end of  CMUSHclientDoc::GetLastLine

 void CMUSHclientDoc::RemoveChunk (void)
   {
  int i;

// remove JUMP_SIZE lines

  for (i = 0; i < JUMP_SIZE; i++)
    {
    delete m_LineList.GetHead ();   // delete contents of head iten
    m_LineList.RemoveHead ();
    }

#ifndef _WIN32
  // Keep the GUI emission counter in sync with lines removed from the head.
  m_gui_lines_emitted = std::max (0L, m_gui_lines_emitted - JUMP_SIZE);
#endif

// shuffle positions backwards

  for (i = 0; i < (m_maxlines / JUMP_SIZE); i++)
    m_pLinePositions [i] = m_pLinePositions [i + 1];

  m_pLinePositions [i] = NULL;    // last one is now empty

// our "last found" line needs adjusting

  m_DisplayFindInfo.m_nCurrentLine -= JUMP_SIZE;

  // has last found line has disappeared off the front of the buffer?
  if (m_DisplayFindInfo.m_nCurrentLine < 0)
    m_DisplayFindInfo.m_nCurrentLine = 0;

   }  // end of CMUSHclientDoc::RemoveChunk


void CMUSHclientDoc::ShowStatusLine (const bool bNow)
  {
  // don't overwrite mapper status for 5 seconds
  if (m_bShowingMapperStatus)
    {
    m_bShowingMapperStatus = false;
    return;
    }

  // only show status messages for active windows
  if (m_pActiveCommandView || m_pActiveOutputView)
    if (bNow)
      Frame.SetStatusMessageNow (m_strStatusMessage);
    else
      Frame.SetStatusMessage (m_strStatusMessage);
#ifndef _WIN32
  // On non-Windows there are no CMUSHView / CSendView instances; forward to
  // the wxWidgets status bar so world.SetStatus() still updates the UI.
  else if (g_wxhook_set_status)
    g_wxhook_set_status (static_cast<const char*>(m_strStatusMessage));
#endif
  m_tStatusDisplayed = CTime::GetCurrentTime ();
  }




BOOL CMUSHclientDoc::SaveModified() 
{
CString str;

  if (ConnectionOpen () &&
      m_iConnectPhase == eConnectConnectedToMud &&
      App.m_bConfirmBeforeClosingWorld)
    {
    str = TFormat ("This will end your %s session.", (const char *) m_mush_name);
  
    if (UMessageBox (str, MB_OKCANCEL | MB_ICONINFORMATION)
          == IDCANCEL)
      return FALSE;
    }
	
  // save world automatically on close if desired
  if (m_bSaveWorldAutomatically && IsModified ())
    DoSave (m_strPathName, TRUE);

  if (!IsModified () && 
      m_bVariablesChanged &&
      App.m_bConfirmBeforeSavingVariables)
    {
	  // get name/title of document
	  CString name;
	  if (m_strPathName.IsEmpty())
	    {
		  // get name based on caption
		  name = m_strTitle;
		  if (name.IsEmpty())
			  VERIFY(name.LoadString(AFX_IDS_UNTITLED));
	    }
	  else
	    {
		  // get name based on file title of path name
		  name = m_strPathName;
	    }

    int i = UMessageBox (TFormat ("World internal variables (only) have changed.\n\n"
                             "Save changes to %s?",
                             (LPCTSTR) name),
           MB_YESNOCANCEL | MB_ICONQUESTION);

    switch (i)
      {
      case IDCANCEL: return FALSE;
      case IDYES:    DoSave (m_strPathName, TRUE); break;
      }
    } // end of variables only changing

  m_bWorldClosing =  CDocument::SaveModified();

  if (m_bWorldClosing)
    MXP_Off (true);   // turn MXP off

  // execute "close" script
  if (m_bWorldClosing && m_ScriptEngine)
    {
    if (SeeIfHandlerCanExecute (m_strWorldClose))
      {
      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      ExecuteScript (m_dispidWorldClose,  
                   m_strWorldClose,
                   eWorldAction,
                   "world close", 
                   "closing world",
                   params, 
                   nInvocationCount); 
      }
    } // end of executing close script

  return m_bWorldClosing;
}


// we do our own DoSave, because the default one stops at the first space
BOOL CMUSHclientDoc::DoSave(LPCTSTR lpszPathName, BOOL bReplace)
  {
	CString strNewName;

  // empty name - possibly caused by "save as"
  if (lpszPathName == NULL || lpszPathName [0] == 0)
    {

    // if we know the path name, suggest that, otherwise take the MUD name
	  strNewName = m_strPathName;
	  if (bReplace && strNewName.IsEmpty())
	   {
    
      strNewName = m_mush_name;

      // fix up name to remove characters that are invalid

      int i;
      while ((i = strNewName.FindOneOf ("<>\"|?:#%;/\\")) != -1)
        strNewName = strNewName.Left (i) + strNewName.Mid (i + 1);

      CString strFixedName = Make_Absolute_Path (App.m_strDefaultWorldFileDirectory);
      strFixedName += strNewName;

      strNewName = strFixedName;
      }   // end of no path name known

	  CDocTemplate* pTemplate = GetDocTemplate();
	  ASSERT(pTemplate != NULL);

    BOOL bResult = AfxGetApp()->DoPromptFileName(strNewName,
		    AFX_IDS_SAVEFILE,
		    OFN_HIDEREADONLY | OFN_PATHMUSTEXIST, FALSE, pTemplate);

    if (!bResult)
		    return FALSE;       // don't even attempt to save

    }     // end of no path name supplied
  else
    strNewName = lpszPathName;

 // execute "save" script
  if (m_ScriptEngine)
    {
    if (SeeIfHandlerCanExecute (m_strWorldSave))
      {

      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      ExecuteScript (m_dispidWorldSave,  
                   m_strWorldSave,
                   eWorldAction,
                   "world save", 
                   "saving world",
                   params, 
                   nInvocationCount); 
      }
    } // end of executing save script


  // now do plugins "world save"
  SendToAllPluginCallbacks (ON_PLUGIN_WORLD_SAVE);

  BOOL bSuccess = CDocument::DoSave (strNewName, bReplace);

  if (bSuccess)
    m_bVariablesChanged = false;

  return bSuccess;
  }


void CMUSHclientDoc::OnGameAutosay() 
{
m_bEnableAutoSay = !m_bEnableAutoSay;
}

void CMUSHclientDoc::OnUpdateGameAutosay(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
pCmdUI->Enable (!m_strAutoSayString.IsEmpty ());
pCmdUI->SetCheck (m_bEnableAutoSay);
}

// GetSocketError: single-sourced in doc_shared.cpp (ITransport T3b)

void CMUSHclientDoc::SetUpVariantString (VARIANT & tVariant, const CString & strContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_BSTR;
  tVariant.bstrVal = strContents.AllocSysString (); 
  }   // end of CMUSHclientDoc::SetUpVariantString

void CMUSHclientDoc::SetUpVariantShort (VARIANT & tVariant, const short iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_I2;
  tVariant.iVal = iContents; 
  }   // end of CMUSHclientDoc::SetUpVariantShort

void CMUSHclientDoc::SetUpVariantBool (VARIANT & tVariant, const BOOL iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_BOOL;
  tVariant.boolVal = iContents; 
  }   // end of CMUSHclientDoc::SetUpVariantBool

void CMUSHclientDoc::SetUpVariantLong (VARIANT & tVariant, const long iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_I4;
  tVariant.lVal = iContents; 
  }   // end of CMUSHclientDoc::SetUpVariantLong

void CMUSHclientDoc::SetUpVariantDate (VARIANT & tVariant, const COleDateTime iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_DATE;
  tVariant.date = iContents; 
  }   // end of CMUSHclientDoc::SetUpVariantDate

void CMUSHclientDoc::SetUpVariantDouble (VARIANT & tVariant, const double dContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_R8;
  tVariant.dblVal = dContents; 
  }   // end of CMUSHclientDoc::SetUpVariantDouble

long CMUSHclientDoc::CheckObjectName (CString & strObjectName,
                                      const bool bConform)
  {

  // remove leading and trailing spaces
  strObjectName.TrimLeft ();
  strObjectName.TrimRight ();

  // check label conforms to naming conventions
  if (bConform)
    if (CheckLabel (strObjectName))
      return eInvalidObjectLabel;

  // object names are keyed in lower case
  strObjectName.MakeLower ();

  // name is OK
  return eOK;

  }   // end of CMUSHclientDoc::CheckObjectName


void CMUSHclientDoc::OnConnectDisconnect() 
{
if (m_iConnectPhase == eConnectNotConnected)
  OnConnectionConnect ();
else
  OnConnectionDisconnect ();
}

void CMUSHclientDoc::OnUpdateConnectDisconnect(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
pCmdUI->Enable ();
pCmdUI->SetCheck (m_iConnectPhase != eConnectNotConnected);
}


void CMUSHclientDoc::OnGameReloadScriptFile() 
{
  DisableScripting ();
  CreateScriptEngine ();	
}

void CMUSHclientDoc::OnUpdateGameReloadScriptFile(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus

BOOL bEnable = TRUE;

  // scripting not active, or some serious error, give up
  if (!m_ScriptEngine && !m_bSyntaxErrorOnly)
    bEnable = FALSE;
  
  // no script file - can't reprocess
  // commented out in 3.80 - they still might want to reload scripting
//  if (m_strScriptFilename.IsEmpty ())
//    bEnable = FALSE;

  pCmdUI->Enable (bEnable);
  
}

// Also called from script function

void CMUSHclientDoc::ClearOutput (void)
  {

  if (m_pLinePositions == NULL)
    return;

POSITION pos;

// delete lines list

  DELETE_LIST (m_LineList);

// put one line in line list

  m_total_lines = 0;
  m_pCurrentLine = new CLine (++m_total_lines, 
                              m_nWrapColumn,
                              0, WHITE, BLACK,
                              m_bUTF_8);

  m_LineList.AddTail (m_pCurrentLine);
//  m_strCurrentLine.Empty ();

  // clear all elements in our positions array

  for (int i = 0; i <= m_maxlines / JUMP_SIZE; i++)
    m_pLinePositions [i] = NULL;

  m_pLinePositions [0] = m_LineList.GetHeadPosition ();

  // previous find won't work now

  m_DisplayFindInfo.m_nCurrentLine = 0;
  m_DisplayFindInfo.m_pFindPosition = NULL;
  m_DisplayFindInfo.m_bAgain = FALSE;  

  // we need to force views to update scroll bar positions and things like that

  // clear the world view's output buffer (via the seam) and reset
  // the GUI-emission counter so UpdateAllViews starts from zero.
  EmitClearOutput ();
  m_gui_lines_emitted = 0;

  // no recent trigger lines
  m_sRecentLines.clear ();

  // redraw all views

  UpdateAllViews (NULL);

  } // end of CMUSHclientDoc::ClearOutput

void CMUSHclientDoc::OnDisplayClearOutput() 
{

  // check they really want to
  if (UMessageBox 
      (TFormat ("Are you SURE you want to clear all %i lines in the output window?",
                   m_LineList.GetCount ()),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                    return;
    
  ClearOutput ();


}


CString CMUSHclientDoc::RecallText (const CString strSearchString,   // what to search for
                                    const bool bMatchCase,
                                    const bool bRegexp,          // and other params
                                    const bool bCommands,
                                    const bool bOutput,
                                    const bool bNotes,
                                    const int  iLines,
                                    const CString strRecallLinePreamble)
    {
CString strMessage;
t_regexp * regexp = NULL;          // compiled regular expression
int iCurrentLine;

  // compile regular expression if needed
  if (bRegexp)
    regexp = regcomp (strSearchString, (bMatchCase ? 0 : PCRE_CASELESS) | (m_bUTF_8 ? PCRE_UTF8 : 0));

CString strFindString = strSearchString;
CString strStatus = TFormat ("Recalling: %s", (LPCTSTR) strSearchString);

  Frame.SetStatusMessageNow (strStatus);

// find how many more lines we have to search

  long nToGo = m_LineList.GetCount ();
  iCurrentLine = 0;
  
  CProgressDlg * pProgressDlg = NULL;// progress dialog

  if (nToGo > 500)
    {
    pProgressDlg = new CProgressDlg;
    pProgressDlg->Create ();
    pProgressDlg->SetStatus (strStatus);
    pProgressDlg->SetRange (0, nToGo);     
    pProgressDlg->SetWindowText (Translate ("Recalling..."));                              
    }   // end of having enough lines to warrant a progress bar

// go back requested number of lines

  POSITION pos = m_LineList.GetHeadPosition ();

  if (iLines > 0)
    {

// if they want to use whole buffer, just use it

    if (iLines >= m_LineList.GetCount ())
      pos = m_LineList.GetHeadPosition ();
    else
      {

// otherwise count backwards the required number of lines

      pos = m_LineList.GetTailPosition ();

      for (int line = 0; line < iLines && pos; line++)
        m_LineList.GetPrev (pos);

// if no pos, start at top

      if (!pos)
        pos = m_LineList.GetHeadPosition ();
      }   // end of not starting from the top

    } // end of wanting to go back so-many lines
  else
    pos = m_LineList.GetHeadPosition ();

// if case-insensitive search wanted, force "text to find" to lower case

  if (!bMatchCase)
    strFindString.MakeLower ();

  try
    {
    int iMilestone = 0;
    CString strLine;
    int iFlags = 0;

    do
      {

      strLine.Empty ();
      CTime theTime;

      // get lines until a hard return

      while (pos)
        {
        CLine * pLine = m_LineList.GetNext (pos);   // get next line
        strLine += CString (pLine->text, pLine->len);
        theTime = pLine->m_theTime;
        iFlags = pLine->flags;
        iMilestone++;
        iCurrentLine++;
        if (pLine->hard_return)
          break;
        }

    // update progress control

      if (pProgressDlg && iMilestone > 31)   // every 31 lines
        {
        pProgressDlg->SetPos (iCurrentLine); 
        iMilestone = 0;
        if(pProgressDlg->CheckCancelButton())
          break;
        } // end of having a progress control

  // if text found on this line, then we have done it!

      // check it was the right sort of line
      if (!(
           ((iFlags & USER_INPUT)   && bCommands) ||
          (((iFlags & NOTE_OR_COMMAND) == 0)  && bOutput) ||
           ((iFlags & COMMENT)      && bNotes))
           )
          continue;
          
      CString strSearchLine = strLine;

      if (bRegexp )
        {
  // if case-insensitive search wanted, force this line to lower case

        if (regexec (regexp, strSearchLine))
          {
          if (!strRecallLinePreamble.IsEmpty ())
            {
            CString strPreamble = FormatTime (theTime, strRecallLinePreamble);
            strMessage += strPreamble;
            }
          strMessage += strLine;
          strMessage += ENDLINE;
          } // end of found it
        } // end of regular expression
      else
        { // not regular expression 
        // if case-insensitive search wanted, force this line to lower case
        if (!bMatchCase)
          strSearchLine.MakeLower ();
        if (strSearchLine.Find (strFindString) != -1)
          {
          if (!strRecallLinePreamble.IsEmpty ())
            {
            CString strPreamble = FormatTime (theTime, strRecallLinePreamble);
            strMessage += strPreamble;
            }
          strMessage += strLine;
          strMessage += ENDLINE;
          } // end of found it
        } // end of not regular expression

      } while (pos);  // end of looping through each line 

    } // end of try

  catch(CException* e)
    {
    e->ReportError ();
    e->Delete ();
    strMessage.Empty ();
    }


  Frame.SetStatusNormal (); 

  if (pProgressDlg)
    {
    delete pProgressDlg;
    pProgressDlg = NULL;
    }


  if (strMessage.IsEmpty ())
    {
    CString strMsg;
    CString strFindType = "text";
    if (bRegexp)
      strFindType = "regular expression";
    strMsg = TFormat ("The %s \"%s\" was not found", 
                  (LPCTSTR) strFindType,
                  (LPCTSTR) strSearchString);
    UMessageBox (strMsg, MB_ICONINFORMATION);
    }

  return strMessage;    
    } // end of CMUSHclientDoc::RecallText 

void CMUSHclientDoc::DoRecallText (void)
  {
CRecallSearchDlg dlg (m_RecallFindInfo.m_strFindStringList);

  if (!m_RecallFindInfo.m_strFindStringList.IsEmpty ())
    dlg.m_strFindText = m_RecallFindInfo.m_strFindStringList.GetHead ();

  dlg.m_bMatchCase  = m_RecallFindInfo.m_bMatchCase;
  dlg.m_strTitle    = m_RecallFindInfo.m_strTitle;
  dlg.m_bRegexp     = m_RecallFindInfo.m_bRegexp;
  dlg.m_iLines      = m_LineList.GetCount ();
  dlg.m_bCommands   = m_bRecallCommands;
  dlg.m_bOutput     = m_bRecallOutput;
  dlg.m_bNotes      = m_bRecallNotes;
  dlg.m_strRecallLinePreamble = m_strRecallLinePreamble;

  if (dlg.DoModal () != IDOK)
    return;

  m_RecallFindInfo.m_bMatchCase    = dlg.m_bMatchCase;
  m_RecallFindInfo.m_bRegexp       = dlg.m_bRegexp;
  m_RecallFindInfo.m_bUTF8         = m_bUTF_8;
  m_bRecallCommands = dlg.m_bCommands;
  m_bRecallOutput   = dlg.m_bOutput;
  m_bRecallNotes    = dlg.m_bNotes;

  // if they change this, the document is modified
  if (m_strRecallLinePreamble != dlg.m_strRecallLinePreamble)
    SetModifiedFlag ();

  m_strRecallLinePreamble = dlg.m_strRecallLinePreamble;
                                                  
  // add find string to head of list, provided it is not empty, and not the same as before
  if (!dlg.m_strFindText.IsEmpty () &&
      (m_RecallFindInfo.m_strFindStringList.IsEmpty () ||
      m_RecallFindInfo.m_strFindStringList.GetHead () != dlg.m_strFindText))
    m_RecallFindInfo.m_strFindStringList.AddHead (dlg.m_strFindText);

CString strMessage;

  strMessage = RecallText (
              m_RecallFindInfo.m_strFindStringList.GetHead (),   // what to search for
              dlg.m_bMatchCase ,
              dlg.m_bRegexp,          // and other params
              dlg.m_bCommands,
              dlg.m_bOutput,
              dlg.m_bNotes,
              dlg.m_iLines,
              dlg.m_strRecallLinePreamble);

  if (!strMessage.IsEmpty ())
    CreateTextWindow ((LPCTSTR) strMessage,
                      (LPCTSTR) TFormat ("Recall: %s",
                         (LPCTSTR) m_RecallFindInfo.m_strFindStringList.GetHead ()),
                      this,
                      m_iUniqueDocumentNumber,
                      m_font_name,
                      m_font_height,
                      m_font_weight,
                      m_font_charset,
                      m_normalcolour [WHITE],
                      m_normalcolour [BLACK],
                      m_RecallFindInfo.m_strFindStringList.GetHead (),
                      dlg.m_strRecallLinePreamble,
                      dlg.m_bMatchCase,
                      dlg.m_bRegexp,
                      dlg.m_bCommands,
                      dlg.m_bOutput,  
                      dlg.m_bNotes,
                      dlg.m_iLines,
                      eNotepadRecall
                      );
  }  // end of CMUSHclientDoc::DoRecallText

void CMUSHclientDoc::OnDisplayRecalltext() 
{

  DoRecallText ();

}

void DoGlobalPrefs (CMUSHclientDoc * pCurrentDoc);

void CMUSHclientDoc::OnFilePreferences() 
{
  DoGlobalPrefs (this);
}


// finds the style for a given column (column might be in middle of style)
// returns TRUE if found 

bool CMUSHclientDoc::FindStyle (const CLine * pLine,         // which line
                                const int iWantedCol,  // which column we want
                                int & iCol,            // which column style goes up to
                                CStyle * & pStyle,     // return style pointer
                                POSITION & foundpos) const  // and its position
  {
   foundpos = NULL;   // none initially
   iCol = 0;

   // find first style to change
   for (POSITION pos = pLine->styleList.GetHeadPosition(); pos; )
     {
     foundpos = pos;  // position to insert after
     pStyle = pLine->styleList.GetNext (pos);
     iCol += pStyle->iLength;
     if (iCol >= iWantedCol)      // NJG - version 3.73
        break;    // found first style
      }

  return foundpos != NULL;
  }   // end of CMUSHclientDoc::FindStyle 

// find RGB equivalents for a particular style of text



bool CMUSHclientDoc::CheckConnected (void)
  {
// let them know if they are foolishly trying to send to a closed connection

  // no reconnecting while we are trying to disconnect
  if (m_iConnectPhase == eConnectDisconnecting)
    return true;

  // if no socket, ask if they want one
  if (m_iConnectPhase != eConnectConnectedToMud || !ConnectionOpen ())
    {
    // forget it for this address
    if (m_server == "0.0.0.0")
      return true;

    CString str;

    if (m_iConnectPhase != eConnectNotConnected)
      {
      str = TFormat ("The connection to %s is currently being established.", 
                (LPCTSTR) m_mush_name);
      UMessageBox (str, MB_ICONINFORMATION);
      }
    else
      {
      str = TFormat ("The connection to %s is not open. Attempt to reconnect?", 
                (LPCTSTR) m_mush_name);

      if (UMessageBox (str, MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
        ConnectSocket ();
        Frame.FixUpTitleBar ();
        UpdateAllViews (NULL);  // make sure title bar is updated
        }
      }
    return true;
    }

  return false;
  } // end of  CMUSHclientDoc::CheckConnected  


void CMUSHclientDoc::Debug_MUD (const char * sCaption, const char * sData)
  {
#ifdef SHOW_ALL_COMMS

char * subs [] = 
  {
   "\x01", "[01]",        // whatever that is
   "\r",   "[CR]",     
   "\n",   "[LF]",     
   "\t",   "[TAB]",     
   "\x1B", "[ESC]", 
   
   "\xF0", "[SE]",      
   "\xFA", "[SB]",      
   "\xFB", "[WILL]",    
   "\xFC", "[WONT]",    
   "\xFD", "[DO]",      
   "\xFE", "[DONT]",     
   "\xFF", "[IAC]",     

// can't do these - they are printable characters

//   "\x55", "[COMPRESS]",      // 'U'
//   "\x56", "[COMPRESS2]",     // 'V'
//   "\x5A", "[MSP]",           // 'Z'
//   "\x5B", "[MXP]",           // '['

  "", ""  // end of table marker
  };


  CString strTemp = sCaption;
  strTemp += sData;

// replace all non-printables that we want to

  for (int j = 0; subs [j] [0] != 0; j += 2)
    strTemp = Replace (strTemp, subs [j], subs [j + 1]);

  strTemp += ENDLINE;
  AppendToTheNotepad ("MUD debug", strTemp, false);   // append
  AppendToTheNotepad ("MUD debug", ENDLINE, false);   // blank line

#endif

  }  // end of CMUSHclientDoc::Debug_MUD


// save the current style, so that when we go from:
//   mud output ... player input ... mud output
// we keep the correct style


void CMUSHclientDoc::OnDebugWorldInput() 
{
//#ifdef _DEBUG

CDebugWorldInputDlg dlg;

  dlg.m_strText = m_strLastDebugWorldInput;	

  if (dlg.DoModal () != IDOK)
    return;

  // remember for next time
  m_strLastDebugWorldInput = dlg.m_strText;

CString strInput;

const char * p;

  for (p = m_strLastDebugWorldInput; *p; )
    {
    if (*p == '\\')
      {
      p++;
      if (*p == '\\')
        strInput += *p++;   // just one backslash
      else
        {
        char cResult = 0;
        for (int i = 0; i < 2; i++)
          {
          char c = toupper (*p);
          if (isxdigit (c))
            {
            p++;  // skip it
            cResult <<= 4;
            if (c >= 'A')
              c -= 7;
            cResult += (c - '0');
            }   // end of being hex
          } // end of doing 2 bytes
        strInput += cResult;
        } // end of doing hex stuff
      }
    else
      strInput += *p++;

    }  // end of assembling data to send

  // simulate MUD input
  if (!strInput.IsEmpty ())
    DisplayMsg(strInput, strInput.GetLength (), 0);

//#endif    // _DEBUG

} // end of CMUSHclientDoc::OnDebugWorldInput


// adds a new style to the current line
CStyle * CMUSHclientDoc::AddStyle (const unsigned short iFlags,
                                      const COLORREF       iForeColour,
                                      const COLORREF       iBackColour,
                                      const int            iLength ,
                                      CString              strAction,
                                      CString              strHint,
                                      CString              strVariable,
                                      CLine *              pLine)
  {
  // defaults to current line
  if (!pLine)
    pLine = m_pCurrentLine;

  // can't do it without a line
  if (!pLine)
     return NULL;

  if (!pLine->styleList.IsEmpty ())
    {
    // find current style
    CStyle * pOldStyle = pLine->styleList.GetTail ();

    // We want the new style, but did the old one have a text run?
    // if not, we don't really need that

    if (pOldStyle->iLength == 0 && (pOldStyle->iFlags & START_TAG) == 0)
      {
      DELETESTYLE (pOldStyle);
      pLine->styleList.RemoveTail ();
      }   // end of redundant style
    } // end of having at least one style

// create new style item
CStyle * pNewStyle = NEWSTYLE;

// use new styles
   pNewStyle->iFlags      = iFlags;
   pNewStyle->iForeColour = iForeColour;
   pNewStyle->iBackColour = iBackColour;
   pNewStyle->iLength     = iLength;
   pNewStyle->pAction = GetAction (strAction, strHint, strVariable);

// add to line style list
   pLine->styleList.AddTail (pNewStyle); 


   return pNewStyle;

  } // end of CMUSHclientDoc::AddStyle 

// adds a new style to the current line
CStyle * CMUSHclientDoc::AddStyle (const unsigned short iFlags,
                                      const COLORREF       iForeColour,
                                      const COLORREF       iBackColour,
                                      const int            iLength ,
                                      CAction *            pAction,
                                      CLine *              pLine)
  {
  // defaults to current line
  if (!pLine)
    pLine = m_pCurrentLine;

  // can't do it without a line
  if (!pLine)
     return NULL;

  // we are using this action once more
  if (pAction)
    pAction->AddRef ();

  if (!pLine->styleList.IsEmpty ())
    {
    // find current style
    CStyle * pOldStyle = pLine->styleList.GetTail ();

    // We want the new style, but did the old one have a text run?
    // if not, we don't really need that

    if (pOldStyle->iLength == 0 && (pOldStyle->iFlags & START_TAG) == 0)
      {
      DELETESTYLE (pOldStyle);
      pLine->styleList.RemoveTail ();
      }   // end of redundant style
    } // end of having at least one style

// create new style item
CStyle * pNewStyle = NEWSTYLE;

// use new styles
   pNewStyle->iFlags      = iFlags;
   pNewStyle->iForeColour = iForeColour;
   pNewStyle->iBackColour = iBackColour;
   pNewStyle->iLength     = iLength;
   pNewStyle->pAction = pAction;

// add to line style list
   pLine->styleList.AddTail (pNewStyle); 

   return pNewStyle;

  } // end of CMUSHclientDoc::AddStyle 


void CMUSHclientDoc::OnDisplayNocommandecho() 
{
	m_bNoEcho = !m_bNoEcho;
}

void CMUSHclientDoc::OnUpdateDisplayNocommandecho(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->SetCheck (m_bNoEcho);
  pCmdUI->Enable ();
}

void CMUSHclientDoc::OnEditDebugincomingpackets() 
{

	m_bDebugIncomingPackets = !m_bDebugIncomingPackets;
	
}

void CMUSHclientDoc::OnUpdateEditDebugincomingpackets(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->SetCheck (m_bDebugIncomingPackets);
  pCmdUI->Enable ();
}


void CMUSHclientDoc::Debug_Packets (LPCTSTR sCaption, 
                                    LPCTSTR lpszText, 
                                    int size, 
                                    const __int64 iNumber)
  {
  // one for each ascii byte, 

#define MAX_DEBUG_CHARS 16  // so it will fit into an email message without wrapping

char asciibuf [MAX_DEBUG_CHARS + 1];       // 1 for each ascii byte
char hexbuf  [(MAX_DEBUG_CHARS * 3) + 1];  // 2 for each hex byte, 1 for the space between hex bytes
const unsigned char * p = (const unsigned char *) lpszText;
char * pa,
     * ph;
int i;

COleDateTime tNow = COleDateTime::GetCurrentTime();

  CString strTime = tNow.Format (TranslateTime ("%A, %B %d, %Y, %#I:%M:%S %p"));

  CString strTitle = "Packet debug - ";
  strTitle += m_mush_name;


  CString strMsg = TFormat ("%s%s packet: %I64d (%i bytes) at %s%s%s", 
                                  ENDLINE,
                                  sCaption,
                                  iNumber,
                                  size,
                                  (LPCTSTR) strTime,
                                  ENDLINE,
                                  ENDLINE);

  if (!SendToFirstPluginCallbacks (ON_PLUGIN_PACKET_DEBUG, strMsg))
    AppendToTheNotepad (strTitle, 
                        strMsg, 
                        false,   // append
                        eNotepadPacketDebug);
  
// keep going until we have displayed it all
  while (size > 0)
    {

    // do each character
    for (pa = asciibuf, ph = hexbuf, i = 0; 
         size > 0 && i < MAX_DEBUG_CHARS;
         size--, i++, p++)
           {
           // ascii character
           if (isprint (*p))
             *pa++ = *p;
           else
             *pa++ = '.';
           sprintf (ph, " %02x", *p);
           ph += 3;
           }  // end of each byte

    // terminate the strings
    *pa = 0;
    *ph = 0;

    strMsg = CFormat ("%-*s  %s%s",
                                  MAX_DEBUG_CHARS,
                                  asciibuf, 
                                  hexbuf,
                                  ENDLINE);

  if (!SendToFirstPluginCallbacks (ON_PLUGIN_PACKET_DEBUG, strMsg))
    AppendToTheNotepad (strTitle, 
                       strMsg, 
                        false,   // append
                        eNotepadPacketDebug);
    }

  }  // end of CMUSHclientDoc::Debug_Packets 


void CMUSHclientDoc::SendWindowSizes (const int iNewWidth)
  {

  // connection closed - can't do it

  if (m_iConnectPhase != eConnectConnectedToMud ||
      !ConnectionOpen () ||
      !m_bNAWS_wanted)
    return;

  // see RFC 1073

#ifndef _WIN32
  // Non-Windows: the MFC view list is empty, so use the col/row dimensions
  // maintained by the wxWidgets frame (m_gui_rows updated by OnSize/SendNAWS).
  // m_nWrapColumn holds the current column count (updated by scriptingoptions).
  {
    int cols = std::max(1, (int)m_nWrapColumn);
    int rows = std::max(1, m_gui_rows);
    unsigned char p[] = {
        (unsigned char)IAC, (unsigned char)SB, (unsigned char)TELOPT_NAWS,
        (unsigned char)HIBYTE(cols), (unsigned char)LOBYTE(cols),
        (unsigned char)HIBYTE(rows), (unsigned char)LOBYTE(rows),
        (unsigned char)IAC, (unsigned char)SE
    };
    SendPacket(p, sizeof p);
  }
#else
  // Windows: derive dimensions from the MFC view geometry.

  if (!m_FontHeight)
    return;

  // send size of first view we find

  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
		  CMUSHView* pmyView = (CMUSHView*)pView;

      RECT r;

      pmyView->GetTextRect (&r);

      WORD height = (r.bottom - r.top - m_iPixelOffset) / m_FontHeight;

      // now tell them our size
      unsigned char p [] = { IAC, SB, TELOPT_NAWS,
                              HIBYTE (iNewWidth), LOBYTE (iNewWidth),
                              HIBYTE (height), LOBYTE (height),
                              IAC, SE };
      SendPacket (p, sizeof p);

      break;    // found one
      }
    } // end of looping through views
#endif


  }  // end of CMUSHclientDoc::SendWindowSizes 


// SendPacket (both overloads): single-sourced in doc_shared.cpp (ITransport T3b)

void CMUSHclientDoc::OnEditFliptonotepad() 
{
CTextDocument * pTextDoc = NULL;

  for (POSITION docPos = App.m_pNormalDocTemplate->GetFirstDocPosition();
      docPos != NULL; )
    {
    // Iterate the SAME template the position came from (notepad/text docs live
    // in m_pNormalDocTemplate). On Windows GetNextDoc just follows the node's
    // pNext chain so the template object is immaterial, but the portable shim
    // treats POSITION as an index into that template's own list — so using the
    // world template here would wrongly index the world docs and never find the
    // existing notepad, spawning a duplicate.
    pTextDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(docPos);

    // ignore unrelated worlds
    if (pTextDoc->m_pRelatedWorld == this &&
       pTextDoc->m_iUniqueDocumentNumber == m_iUniqueDocumentNumber)
      break;

    pTextDoc = NULL;
    } // end of doing each document

  if (pTextDoc)
    {
#ifndef _WIN32
    // Non-Windows: no CTextView in the MFC view system — use the wx callback.
    if (pTextDoc->m_fn_activate)
      pTextDoc->m_fn_activate ();
#else
    // activate the view
    POSITION pos=pTextDoc->GetFirstViewPosition();

    if (pos)
      {
      CView* pView = pTextDoc->GetNextView(pos);

      if (pView->IsKindOf(RUNTIME_CLASS(CTextView)))
        {
        CTextView* pmyView = (CTextView*)pView;
        pmyView->GetParentFrame ()->ActivateFrame ();
        pmyView->GetParentFrame ()->SetActiveView(pmyView);
        return;
        } // end of having the right type of view
      }   // end of having a view
#endif
    } // end of having an existing notepad document
  else
    CreateTextWindow ("",     // contents
                      (LPCTSTR) TFormat ("Notepad: %s", (LPCTSTR) m_mush_name),     // title
                      this,   // document
                      m_iUniqueDocumentNumber,      // document number
                      m_input_font_name,
                      m_input_font_height,
                      m_input_font_weight,
                      m_input_font_charset,
                      m_input_text_colour,
                      m_input_background_colour,
                      "",       // search string
                      "",       // line preamble
                      false,
                      false,
                      false,
                      false,  
                      false,
                      false,
                      eNotepadNormal
                      );

      
}


void CMUSHclientDoc::OnFileOpen() 
{
  Frame.DoFileOpen ();
	
}

void CMUSHclientDoc::OnFileSave() 
{
int iCount;
CString strDirectory;
CString strOriginalTitle = m_strWindowTitle;

  // find length of current directory
  iCount = GetCurrentDirectory (0, NULL);	
  // get current directory
  GetCurrentDirectory (iCount, strDirectory.GetBuffer (iCount));
  strDirectory.ReleaseBuffer (-1);
  // change to world directory
  SetCurrentDirectory (Make_Absolute_Path (App.m_strDefaultWorldFileDirectory));
  // save world details
  CDocument::OnFileSave ();
  // change back to current directory
  SetCurrentDirectory (strDirectory);
  // put title back
  SetTitle (strOriginalTitle);
}

void CMUSHclientDoc::OnUpdateFileSave(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
pCmdUI->Enable ();
	
	
}

void CMUSHclientDoc::OnFileSaveAs() 
{
int iCount;
CString strDirectory;
CString strOriginalTitle = m_strWindowTitle;

  // find length of current directory
  iCount = GetCurrentDirectory (0, NULL);	
  // get current directory
  GetCurrentDirectory (iCount, strDirectory.GetBuffer (iCount));
  strDirectory.ReleaseBuffer (-1);
  // change to world directory
  SetCurrentDirectory (Make_Absolute_Path (App.m_strDefaultWorldFileDirectory));
  // save world details
  CDocument::OnFileSaveAs ();
  // change back to current directory
  SetCurrentDirectory (strDirectory);
  // put title back
  SetTitle (strOriginalTitle);
}

void CMUSHclientDoc::OnUpdateFileSaveAs(CCmdUI* pCmdUI) 
{
DoFixMenus (pCmdUI);  // remove accelerators from menus
pCmdUI->Enable ();
	
}

/*
void CMUSHclientDoc::ShowHyperlink (const CString strLink)
  {
CStyle * pStyle = m_pCurrentLine->styleList.GetTail ();
COLORREF colour1,
         colour2;

  COLORREF iForeColour = pStyle->iForeColour;
  COLORREF iBackColour = pStyle->iBackColour;
  int iFlags = pStyle->iFlags;

// find current foreground and background RGB values
  GetStyleRGB (pStyle, colour1, colour2);
  
  if (m_bUseCustomLinkColour)
    {
    pStyle->iForeColour = m_iHyperlinkColour;    // use hyperlink colour
    pStyle->iBackColour = colour2;
    pStyle->iFlags &= ~COLOURTYPE;  // clear bits, eg. custom
    pStyle->iFlags |= COLOUR_RGB;
    }

  pStyle->iFlags &= ~ACTIONTYPE;   // cancel old actions
  pStyle->iFlags |= ACTION_HYPERLINK;   // send-to action

  if (m_bUnderlineHyperlinks)
    pStyle->iFlags |= UNDERLINE;   // send-to action

  AddToLine (strLink, 0);

  // have to add the action now, before we start a new line
  pStyle->pAction = GetAction (strLink, "", "");

  // go back to old style (ie. lose the underlining)
  AddStyle (iFlags, 
           iForeColour, 
           iBackColour, 
           0, 
           "");

  }
*/


/*
Delayed sending:

  eg.

  -----------
  say See you soon
  east
  ;
  ; go to shop
  ;
  1; west
  2; north
  3; west
  ;
  ; get some food
  ;
  4; buy food
  ;
  ; go back
  ;
  5; east
  6; south
  7; east
-----------

(You may find the new "trigger edit" box helpful for entering such a long command. Press Ctrl+Enter to start a new line).

The syntax for this delayed sending is:

  1. The "send" text is broken into lines
  2. Blank lines are ignored
  3. Lines starting with a semicolon are ignored (treated as comments)
  4. Lines with a number followed by a semicolon are treated as a delayed send
     - the command after the semicolon is queued to be sent after the nominated
       number of seconds (using a temporary, unlabelled, one-shot timer)
  5. Lines which do not start with a number followed by a semicolon are sent
     immediately (eg. "east")
  6. Lines with zero seconds are sent immediately (eg. "0; west")
  7. If you want to send a semicolon at the start of a line, 
     you can do that by something like this:
     0; ; sigh  <-- immediate send
     5; ; sigh  <-- delayed send
  8. The time must be in the range 0 to 23:59:59.
  9. Leading and trailing spaces are discarded.


  */


CString CMUSHclientDoc::DelayedSend(const CString strMessage, const bool bEchoIt) 
  {

  CString strSendNow;

  CStringList strList;
  // break into lines
  StringToList (strMessage, ENDLINE, strList);
  for (POSITION pos = strList.GetHeadPosition (); pos; )
    {
    CString strLine = strList.GetNext (pos);

    strLine.TrimLeft ();
    strLine.TrimRight ();

    // ignore comments and blank lines
    if (strLine.IsEmpty () || strLine [0] == ';')
      continue;

    const char * p;
    long iSeconds = 0;
    // get number of seconds
    for (p = strLine; isdigit (*p) && iSeconds >= 0; p++)
      {
      iSeconds *= 10;
      iSeconds += *p - '0';
      }

    // discard spaces after the number of seconds
    while (*p == ' ')
      p++;

    bool bSemicolon = *p == ';';
    if (bSemicolon)
      {
      p++;
      strLine = p;  // copy line from past semicolon
      strLine.TrimLeft ();
      }

    bool bSpeedWalk = false;

    // check for speed walk at start of string
    if (strLine.Left (m_speed_walk_prefix.GetLength ()) == m_speed_walk_prefix)
      {
      bSpeedWalk = true;
      strLine = strLine.Mid (m_speed_walk_prefix.GetLength ());
      }

    if (iSeconds > 0)
      if (bSpeedWalk)
        DoAfter (iSeconds, strLine);
      else
        DoAfterSpeedWalk (iSeconds, strLine);
    else
      {  // do now
      // evaluate speed walk now
      if (bSpeedWalk)
        {
        strLine = DoEvaluateSpeedwalk (strLine.Mid (m_speed_walk_prefix.GetLength ()));
        if (!strLine.IsEmpty ())
          {
          if (strLine [0] == '*')    // error in speedwalk string?
            {
            UMessageBox (strLine.Mid (1));  // already translated in  DoEvaluateSpeedwalk
            }
          else
              SendMsg (strLine, bEchoIt, true, LoggingInput ());   // queue it
          } // end of non-empty speedwalk
        } // end of speed walk
      else
        SendMsg (strLine, bEchoIt, false, LoggingInput ());

      }

    } // end of this line

  return strSendNow;  // any world.notes 
  }   // end of send delayed




void CMUSHclientDoc::OnFileImport() 
{
CImportXMLdlg dlg;

  dlg.m_pDoc = this;

  dlg.m_bGeneral = TRUE;
	dlg.m_bTriggers = TRUE;
	dlg.m_bAliases = TRUE;
	dlg.m_bTimers = TRUE;
	dlg.m_bMacros = TRUE;
	dlg.m_bVariables = TRUE;
	dlg.m_bColours = TRUE;
	dlg.m_bKeypad = TRUE;
	dlg.m_bPrinting = TRUE;

  dlg.DoModal ();   // dialog does it all :)
  
}

int CompareTrigger (const void * elem1, const void * elem2)
  {
  CTrigger * trigger1 = (*((CTrigger **) elem1));
  CTrigger * trigger2 = (*((CTrigger **) elem2));

  if (trigger1->iSequence < trigger2->iSequence)
    return -1;
  else if (trigger1->iSequence > trigger2->iSequence)
    return 1;
  else 
    {
      // identical sequence, sort into match order
    if (trigger1->trigger < trigger2->trigger)
      return -1;
    else if (trigger1->trigger > trigger2->trigger)
      return 1;
    else
      return 0;
    }  // end of sequence identical

  }   // end of CompareTrigger


void  CMUSHclientDoc::SortTriggers (void)
  {

int iCount = GetTriggerMap ().GetCount ();
int i;
CString strTriggerName;
CTrigger * pTrigger;
POSITION pos;

  GetTriggerArray ().SetSize (iCount);
  GetTriggerRevMap ().clear ();

  // extract pointers into a simple array
  for (i = 0, pos = GetTriggerMap ().GetStartPosition(); pos; i++)
    {
     GetTriggerMap ().GetNextAssoc (pos, strTriggerName, pTrigger);
     GetTriggerArray ().SetAt (i, pTrigger);
     GetTriggerRevMap () [pTrigger] = strTriggerName;
    }


  // sort the array
  qsort (GetTriggerArray ().GetData (), 
         iCount,
         sizeof (CTrigger *),
         CompareTrigger);

  } // end of CMUSHclientDoc::SortTriggers

static int CompareAlias (const void * elem1, const void * elem2)
  {
  CAlias * alias1 = (*((CAlias **) elem1));
  CAlias * alias2 = (*((CAlias **) elem2));

  if (alias1->iSequence < alias2->iSequence)
    return -1;
  else if (alias1->iSequence > alias2->iSequence)
    return 1;
  else 
    {
      // identical sequence, sort into match order
    if (alias1->name < alias2->name)
      return -1;
    else if (alias1->name > alias2->name)
      return 1;
    else
      return 0;
    }  // end of sequence identical

  }   // end of CompareAlias


void  CMUSHclientDoc::SortAliases (void)
  {

int iCount = GetAliasMap ().GetCount ();
int i;
CString strAliasName;
CAlias * pAlias;
POSITION pos;

  GetAliasArray ().SetSize (iCount);
  GetAliasRevMap ().clear ();

  // extract pointers into a simple array
  for (i = 0, pos = GetAliasMap ().GetStartPosition(); pos; i++)
    {
     GetAliasMap ().GetNextAssoc (pos, strAliasName, pAlias);
     GetAliasArray ().SetAt (i, pAlias); 
     GetAliasRevMap () [pAlias] = strAliasName;
    }


  // sort the array
  qsort (GetAliasArray ().GetData (), 
         iCount,
         sizeof (CAlias *),
         CompareAlias);

  } // end of CMUSHclientDoc::SortAliases



// CTime:Format only allows for a total field size of 128 bytes
CString CMUSHclientDoc::FormatTime (const CTime theTime, 
                                    LPCTSTR pFormat, 
                                    const bool bFixHTML)   
  {
CString strText;
CString strFormat = pFormat;

  if (bFixHTML)
    {
    // %E becomes startup directory
    strFormat.Replace ("%E", ::Replace (FixHTMLString (working_dir), "%", "%%"));
    // %N becomes world name
    strFormat.Replace ("%N", ::Replace (FixHTMLString (m_mush_name), "%", "%%"));
    // %P becomes player name
    strFormat.Replace ("%P", ::Replace (FixHTMLString (m_name), "%", "%%"));
    // %F becomes default world files directory
    strFormat.Replace ("%F", ::Replace (FixHTMLString (Make_Absolute_Path (App.m_strDefaultWorldFileDirectory)), "%", "%%"));
    // %L becomes default log files directory
    strFormat.Replace ("%L", ::Replace (FixHTMLString (Make_Absolute_Path (App.m_strDefaultLogFileDirectory)), "%", "%%"));
    }   // end of HTML fixup needed
  else
    {
    // %E becomes startup directory
    strFormat.Replace ("%E", ::Replace (working_dir, "%", "%%"));
    // %N becomes world name
    strFormat.Replace ("%N", ::Replace (m_mush_name, "%", "%%"));
    // %P becomes player name
    strFormat.Replace ("%P", ::Replace (m_name, "%", "%%"));
    // %F becomes default world files directory
    strFormat.Replace ("%F", ::Replace (Make_Absolute_Path (App.m_strDefaultWorldFileDirectory), "%", "%%"));
    // %L becomes default log files directory
    strFormat.Replace ("%L", ::Replace (Make_Absolute_Path (App.m_strDefaultLogFileDirectory), "%", "%%"));
    }  // end of not HTML

int iLen = strFormat.GetLength () + 1000;

  // allow for their original buffer plus another 1000
	TCHAR * szBuffer = strText.GetBuffer (iLen);
  time_t tTime = theTime.GetTime ();

	struct tm* ptmTemp = localtime(&tTime);
	if (ptmTemp == NULL ||
		!_tcsftime(szBuffer, iLen, strFormat, ptmTemp))
		szBuffer[0] = '\0';
  strText.ReleaseBuffer (-1);
	return strText;
}  // end of CMUSHclientDoc::FormatTime


void CMUSHclientDoc::OnFilePlugins() 
{
CPluginsDlg dlg;
  dlg.m_pDoc = this;
  dlg.DoModal (); 
}



// Generic "send to" routine - for triggers, aliases, timers to send some
//  text somewhere (eg. to the world, output window, command window, execute etc.)
//
// Output to the world window goes into "strOutput" - it is the caller's responsibility
// to output that (so that triggers can omit from output and *then* display the new
// stuff).


void CMUSHclientDoc::SendTo (
         const unsigned short iWhere,   // where to send it, see enum in others.h
         const CString strSendText,     // what to send
         const bool bOmitFromOutput,    // is it omitting from output buffer?
         const bool bOmitFromLog,       // is it omitting from the log file?
         const CString strDescription,  // description (eg. "trigger blah")
         const CString strVariable,     // what variable to set
         CString & strOutput)           // output to be displayed when finished
  {
  POSITION pos;

  // empty send text does absolutely nothing :)  - in some cases :P

  if (iWhere != eSendToNotepad && 
      iWhere != eAppendToNotepad &&
      iWhere != eReplaceNotepad &&
      iWhere != eSendToOutput &&
      iWhere != eSendToLogFile &&
      iWhere != eSendToVariable 
#ifdef PANE
      && iWhere != eSendToPane
#endif // PANE
      )
    if (strSendText.IsEmpty ())
      return;

  switch (iWhere)
    {
    case eSendToCommand:

#ifndef _WIN32
      // Non-Windows: route through the wxWidgets command-bar callback.
      // SetCommandText already enforces the "only if bar is empty" contract
      // (returns false and does NOT overwrite if the bar is non-empty).
      SetCommandText(std::string((const char*)strSendText));
#else
      // put trigger response into command buffer - provided it is empty
      for (pos = GetFirstViewPosition(); pos != NULL; )
	      {
	      CView* pView = GetNextView(pos);

	      if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	      {
		      CSendView* pmyView = (CSendView*)pView;

          CString strCurrent;

          pmyView->GetEditCtrl().GetWindowText (strCurrent);
          if (strCurrent.IsEmpty ())
            {
            pmyView->GetEditCtrl().ReplaceSel (strSendText, TRUE);
            break;    // just do first view that we can use
            }   // end of command being empty
	        }	  // end of being a CMUSHView
        }   // end of loop through views
#endif
      break;

    case eSendToWorld:

        // if this is suppressing output, don't show our response either
      SendMsg (strSendText, 
               bOmitFromOutput ? FALSE : m_display_my_input,
               false,   // don't queue
               bOmitFromLog ? FALSE :  LoggingInput ());
        break;

    case eSendToCommandQueue:
        // if this is suppressing output, don't show our response either
        SendMsg (strSendText,
                 bOmitFromOutput ? FALSE : m_display_my_input, 
                 true,     // queue it
                 bOmitFromLog ? FALSE :  LoggingInput ());
        break;

    case eSendToStatus:
        // omit any subsequent lines
        {
        int i = strSendText.Find (ENDLINE);
        if (i != -1)
          m_strStatusMessage = strSendText.Left (i);
        else
          m_strStatusMessage = strSendText;
        }
        // wait until no queued commands and not mapping
        if (m_QueuedCommandsList.IsEmpty () && !m_bMapping)
          ShowStatusLine (true);    // show it now
        break;

    case eSendToNotepad:
      {
        CString strContents = strSendText;
        strContents += ENDLINE;
        CreateTextWindow ((LPCTSTR) strContents,     // contents
                          (LPCTSTR) strDescription,  // title
                          this,   // document
                          m_iUniqueDocumentNumber,      // document number
                          m_input_font_name,
                          m_input_font_height,
                          m_input_font_weight,
                          m_input_font_charset,
                          m_input_text_colour,
                          m_input_background_colour,
                          "",       // search string
                          "",       // line preamble
                          false,
                          false,
                          false,
                          false,  
                          false,
                          false,
                          eNotepadTrigger
                          );
      }
        break;

    case eAppendToNotepad:
        AppendToTheNotepad (strDescription,
                            strSendText + ENDLINE,  
                            false,            // append mode
                            eNotepadTrigger); 
        break;
    case eReplaceNotepad:
        AppendToTheNotepad (strDescription,
                            strSendText + ENDLINE,
                            true,              // replace mode
                            eNotepadTrigger); 
        break;
        // put into output window
    case eSendToOutput:

        strOutput += strSendText;
        if (strSendText.Right (2) != ENDLINE)
          strOutput += ENDLINE;      // add a new line if necessary
      break;

    case eSendToLogFile:
        // log if log file is open
        if (m_logfile && !m_bLogRaw) 
          {
          // log trigger response
          WriteToLog (strSendText);
          WriteToLog ("\n", 1);
          } // end of having a log file
        break;

    case eSendToVariable:
        SetVariable (strVariable, strSendText);
        break;

    case eSendToExecute:
        {
        // save log-my-input flag
        short bSavedLogFlag = m_log_input;
        // if alias (or whatever) doesn't want to be logged, turn it off
        if (bOmitFromLog)
           m_log_input = false;
        Execute (strSendText);    // execute it
        // put flag back
        m_log_input = bSavedLogFlag;
        }
        break;

/*
    case eSendDelayed:
        if (strSendText.Right (2) != ENDLINE)
          strSendText += ENDLINE;      // add a new line if necessary
        strExtraOutput += DelayedSend (strSendText, trigger_item->bOmitFromOutput ? FALSE : m_display_my_input);
        break;
*/

    case eSendToSpeedwalk:
        {
        CString strEvaluatedSpeedwalk = DoEvaluateSpeedwalk (strSendText);
        if (!strEvaluatedSpeedwalk.IsEmpty ())
          {
          if (strEvaluatedSpeedwalk [0] == '*')    // error in speedwalk string?
            {
            UMessageBox (strEvaluatedSpeedwalk.Mid (1));  // already translated
            return;
            }   // end of error message
        SendMsg (strEvaluatedSpeedwalk, 
                bOmitFromOutput ? FALSE : m_display_my_input, 
                true,   // queue it
                bOmitFromLog ? FALSE :  LoggingInput ());
          }
        }
        break;

    case eSendToScript:

      {
        m_bInSendToScript = true;  // principally to stop DeleteLines being used

        if (GetScriptEngine ())   
          GetScriptEngine ()->Parse (strSendText, strDescription);
        else
          ColourNote ("white", "red", 
          Translate ("Send-to-script cannot execute because scripting is not enabled."));

        // leave  m_bInSendToScript true, don't want it done in plugin callbacks either
      }
        break;

    case eSendImmediate:

        // if this is suppressing output, don't show our response either
      DoSendMsg (strSendText, 
               bOmitFromOutput ? FALSE : m_display_my_input,
               bOmitFromLog ? FALSE :  LoggingInput ());
        break;


    case eSendToScriptAfterOmit:

      {
        if (GetScriptEngine ())   
          GetScriptEngine ()->Parse (strSendText, strDescription);
        else
          ColourNote ("white", "red", 
          Translate ("Send-to-script cannot execute because scripting is not enabled."));

      }
        break;

#ifdef PANE

    case eSendToPane:

      {
      /*
      string sName = "test";
      string sTitle = "test title";

      PaneMapIterator pit;
      
      // see if we already have this pane
      pit = m_PaneMap.find (sName); 

      // not found? Make one ...
      if (pit == m_PaneMap.end ())
        {
        if (CreatePane (sName.c_str (), sTitle.c_str (), 
            0, 0, 0, 0, // Left, Top, Width, Height
            0,          // Flags
            RGB_WHITE,  // text colour
            RGB_BLACK,   // background colour
            "",         // font
            0,          // size
            1000,          // lines
            80)          // width
            != eOK)
          return;  // cannot create pane
        pit = m_PaneMap.find (sName);
        if (pit == m_PaneMap.end ())
          return;   // cannot find new pane
        }

      // send text to pane
      pit->second->AddText ((LPCTSTR) strSendText, 
                            m_iNoteColourFore, 
                            m_iNoteColourBack, 
                            m_iNoteStyle,
                            true);
      */

      }
      break;

#endif // PANE

    default:
      // do nothing
      break;


    } // end of switch on where to send to


  } // end of CMUSHclientDoc::SendTo


// LookupHostName: single-sourced in doc_shared.cpp (ITransport T3a)


// LookupHostNameComplete: single-sourced in doc_shared.cpp (ITransport T3a)


// InitiateConnection: single-sourced in doc_shared.cpp (ITransport T3a)


// ConnectionEstablished: single-sourced in doc_shared.cpp (ITransport T3d)






// ---------------------------------------------------------------------------
// SSL/TLS support
// ---------------------------------------------------------------------------

// StartSSLHandshake / ContinueSSLHandshake: gone (T5) — the transport
// adapter owns the whole TLS session on every platform (the Windows
// mechanics live verbatim in pal/win_transport.cpp; engine policy in
// doc_shared.cpp OnTls{Established,Failed}).













// send a chat message to all connections
int CMUSHclientDoc::SendChatMessageToAll 
              (const int iMessage,         // message type (see enum)
               const CString strMessage,   // the message
               const bool bUnlessIgnoring, // don't send if we are ignoring them
               const bool bServed,         // if true, to incoming only
               const bool bNotServed,      // if true, to outgoing only
               const long nExceptID,       // not to this ID
               const CString strGroup,     // group to send to
               const long iStamp)         // which message stamp to use
  {
int iCount = 0;

  for (POSITION chatpos = m_ChatList.GetHeadPosition (); chatpos; )
    {
    CChatSocket * pSocket = m_ChatList.GetNext (chatpos);
    if (pSocket->m_iChatStatus == eChatConnected)
      {
      // if they only want incoming, and it isn't, skip it
      if (bServed && !pSocket->m_bIncoming)
        continue;

      // if they only want outgoing, and it isn't, skip it
      if (bNotServed && pSocket->m_bIncoming)
        continue;

      // skip him if we are ignoring him
      if (bUnlessIgnoring && pSocket->m_bIgnore)
        continue;

      // don't send to the incoming one
      if (nExceptID == pSocket->m_iChatID)
        continue;

      // if a group is wanted, and it is the wrong group, don't send
      if (!strGroup.IsEmpty () && pSocket->m_strGroup.CompareNoCase (strGroup) != 0)
        continue;

      if (strGroup.IsEmpty ())
        pSocket->m_iCountOutgoingAll++;
      else
        pSocket->m_iCountOutgoingGroup++;

      pSocket->SendChatMessage (iMessage, strMessage, iStamp);  // send it
      iCount++;
      }   // end of chat session not being deleted
    } // end of all chat sessions

  // anti-message loop provision
  if (iCount)
    {
    switch (iMessage)
      {

      case CHAT_TEXT_EVERYBODY:
        m_strLastMessageSent = strMessage;
        m_tLastMessageTime = CTime::GetCurrentTime();
        break;

      case CHAT_TEXT_GROUP:
        m_strLastGroupMessageSent = strMessage;
        m_tLastGroupMessageTime = CTime::GetCurrentTime();
        break;

      } // end of switch
    } // end of having some messages sent

  return iCount;
  }  // end of CMUSHclientDoc::SendChatMessageToAll


CChatSocket * CMUSHclientDoc::GetChatSocket (const long nID)  const
  {
  for (POSITION pos = m_ChatList.GetHeadPosition (); pos; )
    {
    CChatSocket * pSocket = m_ChatList.GetNext (pos);
    if (!pSocket->m_bDeleteMe  && 
        pSocket->m_iChatStatus == eChatConnected &&
        pSocket->m_iChatID == nID)
        return pSocket;
    } // end of all chat sessions


  return NULL;    // not found

  } // end of CMUSHclientDoc::GetChatSocket



void CMUSHclientDoc::OnGameChatsessions() 
{
CChatListDlg dlg;

  // show whether we really are accepting calls
  if (m_bAcceptIncomingChatConnections)
    dlg.m_strStatus.Format ("Accepting incoming calls on port %d",
                      m_IncomingChatPort);
  else
    dlg.m_strStatus = "Not currently accepting incoming calls";

  dlg.m_pDoc = this;

  dlg.DoModal ();
  
}


/*     test enumerating the clipboard
#if 0

CString strMessage;

UINT dFormat = 0;

// Open clipboard

  if (!::OpenClipboard(Frame.GetSafeHwnd ()) )
       {
       ::AfxMessageBox( "Cannot open the Clipboard" );
       return;
       }

  char buf [1000];
  int  iLen;

  do
    {
    dFormat = EnumClipboardFormats (dFormat);

    if (dFormat == 0)
      break;

    iLen = GetClipboardFormatName (dFormat, buf, sizeof buf);

    CString strName = CString (buf, iLen);

    if (strName.IsEmpty ())
      {
      switch (dFormat)
        {

         case CF_TEXT             : strName = "Text            "; break;
         case CF_BITMAP           : strName = "Bitmap          "; break;
         case CF_METAFILEPICT     : strName = "Metafilepict    "; break;
         case CF_SYLK             : strName = "Sylk            "; break;
         case CF_DIF              : strName = "Dif             "; break;
         case CF_TIFF             : strName = "Tiff            "; break;
         case CF_OEMTEXT          : strName = "Oemtext         "; break;
         case CF_DIB              : strName = "Dib             "; break;
         case CF_PALETTE          : strName = "Palette         "; break;
         case CF_PENDATA          : strName = "Pendata         "; break;
         case CF_RIFF             : strName = "Riff            "; break;
         case CF_WAVE             : strName = "Wave            "; break;
         case CF_UNICODETEXT      : strName = "Unicodetext     "; break;
         case CF_ENHMETAFILE      : strName = "Enhmetafile     "; break;
         case CF_HDROP            : strName = "Hdrop           "; break;
         case CF_LOCALE           : strName = "Locale          "; break;
         case CF_MAX              : strName = "Max             "; break;
         case CF_OWNERDISPLAY     : strName = "Ownerdisplay    "; break;
         case CF_DSPTEXT          : strName = "Dsptext         "; break;
         case CF_DSPBITMAP        : strName = "Dspbitmap       "; break;
         case CF_DSPMETAFILEPICT  : strName = "Dspmetafilepict "; break;
         case CF_DSPENHMETAFILE   : strName = "Dspenhmetafile  "; break;
        } // end of switch

      }   // end of format not returned

    strMessage += CFormat ("Format = %04X %s\r\n", dFormat, (LPCTSTR) strName);
    } while (dFormat);


 // Close the clipboard

  CloseClipboard();

  CString strContents = strMessage;
  strContents += ENDLINE;
  CreateTextWindow ((LPCTSTR) strContents,     // contents
                    "Clipboard contents",     // title
                    this,   // document
                    m_iUniqueDocumentNumber,      // document number
                    m_input_font_name,
                    m_input_font_height,
                    m_input_font_weight,
                    m_input_text_colour,
                    m_input_background_colour,
                    "",       // search string
                    "",       // line preamble
                    false,
                    false,
                    false,
                    false,  
                    false,
                    false,
                    eNotepadTrigger
                    );

#endif
*/


void CMUSHclientDoc::SendLineToPlugin (void)
  {

  CString strPartialLine = CString (m_pCurrentLine->text, m_pCurrentLine->len);
  SendToAllPluginCallbacks (ON_PLUGIN_PARTIAL_LINE, strPartialLine);

  } // end of CMUSHclientDoc::SendLineToPlugin

void CMUSHclientDoc::OnGameResetconnectedtime() 
{
  m_tStatusTime = CTime::GetCurrentTime(); 
	
}

HACCEL CMUSHclientDoc::GetDefaultAccelerator()
  {
  return m_accelerator;
  }

void CMUSHclientDoc::OnFixMenus(CCmdUI* pCmdUI) 
  {
  DoFixMenus (pCmdUI);
  pCmdUI->ContinueRouting();
  }   // end of CMUSHclientDoc::OnFixMenus

void CMUSHclientDoc::DoFixMenus(CCmdUI* pCmdUI) 
{

  if (!pCmdUI->m_pMenu)
    return;

  WORD id = pCmdUI->m_nID;

  CString strText;
  
  pCmdUI->m_pMenu->GetMenuString (id, strText, MF_BYCOMMAND);

  vector<string> v;

  // look for tab separating command from accelerator
  StringToVector (string (strText), v, "\t", true);

  if (v.size () <= 1)
    return;

  BYTE fVirt;
  WORD key;
                   
  try
    {
    // see what code it was
    StringToKeyCode (v [1].c_str (), fVirt, key);
    }
  catch (CException* e)
    {
    e->Delete ();
    return;
    } // end of catch


  fVirt |= FNOINVERT;   // no inverting of menus

  long virt_plus_key = ((long) fVirt) << 16 | key;

  // see if we have allocated that accelerator
  map<long, WORD>::const_iterator it = m_AcceleratorToCommandMap.find (virt_plus_key);

  // if not, all is OK
  if (it == m_AcceleratorToCommandMap.end ())
    return;

  // re-set the text to be without the accelerator, which now does something different
  pCmdUI->SetText (v [0].c_str ());

} // end of CMUSHclientDoc::DoFixMenus

// for mapping one colour to another at drawing time
const COLORREF CMUSHclientDoc::TranslateColour (const COLORREF & source, const double opacity) const
  {
  // quick escape
  if (m_ColourTranslationMap.empty ())
    {
    if (opacity == 1.0)
      return source;
    return RGB (opacity * GetRValue (source), opacity * GetGValue (source), opacity * GetBValue (source));
    }

  // search for it
  map<COLORREF, COLORREF>::const_iterator it = m_ColourTranslationMap.find (source);

  // not found, use original colour
  if (it == m_ColourTranslationMap.end ())
    {
    if (opacity == 1.0)
      return source;
    return RGB (opacity * GetRValue (source), opacity * GetGValue (source), opacity * GetBValue (source));
    }

  // return replacement colour
  if (opacity == 1.0)
    return it->second;
  return RGB (opacity * GetRValue (it->second), opacity * GetGValue (it->second), opacity * GetBValue (it->second));
  }   // end of CMUSHclientDoc::TranslateColour


// iType = 0 - output line
//         1 - note
//         2 - command
// iLog  = 0 - not to be logged
//         1 - to be logged
// sText = text of line

void CMUSHclientDoc::Screendraw  (const long iType,
                                  const long iLog,
                                  const char * sText)
  {
static bool bInScreendraw = false;

  // don't recurse into infinite loops
  if (bInScreendraw)
    return;

  bInScreendraw = true;
  SendToAllPluginCallbacks (ON_PLUGIN_SCREENDRAW,
                            iType,
                            iLog,
                            sText,
                            false,
                            false);
  bInScreendraw = false;

  }  // end of CMUSHclientDoc::Screendraw 


bool CMUSHclientDoc::PlaySoundFile (CString strSound)
  {

  // stop infinite loops
  if (!m_bInPlaySoundFilePlugin)
    {
    m_bInPlaySoundFilePlugin = true;
    
    if (SendToFirstPluginCallbacks (ON_PLUGIN_PLAYSOUND, strSound))
        {
        m_bInPlaySoundFilePlugin = false;
        return true;   // handled by plugin? don't do our own sound
        }

    m_bInPlaySoundFilePlugin = false;
    }   // of not in plugin already

  // default sound-play mechanism
  return Frame.PlaySoundFile (strSound);
  }


void CMUSHclientDoc::CancelSound (void)
  {

  // stop infinite loops
  if (!m_bInCancelSoundFilePlugin)
    {
    m_bInCancelSoundFilePlugin = true;

    CString strSound;   // deliberately the empty string
    if (SendToFirstPluginCallbacks (ON_PLUGIN_PLAYSOUND, strSound))
        {
        m_bInCancelSoundFilePlugin = false;
        return;   // handled by plugin? don't do our own sound
        }

    m_bInCancelSoundFilePlugin = false;
    } // end of not in plugin already

  // default sound-cancel mechanism
  Frame.CancelSound ();
  StopSound (0);    // also DirectSound

  }

void CMUSHclientDoc::OnDisplayStopsoundplaying() 
{
  CancelSound ();	
}


void CMUSHclientDoc::OnInputKeyname() 
{
CKeyNameDlg dlg;
  dlg.DoModal ();
}

void CMUSHclientDoc::OnEditConvertclipboardforumcodes() 
{
CString strContents; 

  if (!GetClipboardContents (strContents, false, false))
    return;

 putontoclipboard  (QuoteForumCodes (strContents));
	
}

typedef int (WINAPI *PAFMR)(LPCTSTR, DWORD, void *);
typedef BOOL (WINAPI *PRFMR)(LPCTSTR, DWORD, void *);

static PAFMR pAddFontResourceEx = NULL;
static PRFMR pRemoveFontResourceEx = NULL;
static BOOL initialized = FALSE;

#define FR_PRIVATE     0x10
#define FR_NOT_ENUM    0x20

long CMUSHclientDoc::AddSpecialFont (LPCTSTR PathName) 
{

	// First, see if we can get the API call we need. If we've tried
	// once, we don't need to try again. 
	if (!initialized)
	{
		HMODULE hDLL = LoadLibrary ("gdi32");

    if (hDLL)
      {
		  pAddFontResourceEx = 
			  (PAFMR) GetProcAddress(hDLL, "AddFontResourceExA");

		  pRemoveFontResourceEx = 
			  (PRFMR) GetProcAddress(hDLL, "RemoveFontResourceExA");
      }

		initialized = TRUE;
	}

	if (pAddFontResourceEx == NULL ||
      pRemoveFontResourceEx == NULL)
		return eNoSuchCommand;


  ci_set::const_iterator iter = m_strSpecialFontName.find (PathName);

  // same one as earlier - just leave well enough alone
  if (iter != m_strSpecialFontName.end ())
    return eOK;



  void * pbFont = NULL;
  int cFonts;

  cFonts = pAddFontResourceEx (PathName,     // file name
                               FR_PRIVATE,   // flags 
                               pbFont);      // Reserved. Must be 0.


  if (cFonts == 0) 
    return eFileNotFound;

  m_strSpecialFontName.insert (PathName);  // remember, so we can remove it
	return eOK;
  }  // end of  CMUSHclientDoc::AddSpecialFont 

void CMUSHclientDoc::RemoveSpecialFonts (void) 
  { 
  if (pRemoveFontResourceEx == NULL)
    return;

  for (ci_set::const_iterator iter = m_strSpecialFontName.begin (); iter !=  m_strSpecialFontName.end (); iter++)
    {

    void * pbFont = NULL;

    pRemoveFontResourceEx (iter->c_str (),  // original file name
                           FR_PRIVATE,           // flags 
                           pbFont);              // Reserved. Must be 0.
    }

  m_strSpecialFontName.clear ();
  }


// edit with specified editor, replacing %file by actual name
void CMUSHclientDoc::EditFileWithEditor (CString strName)
  {

  CString strArgument = m_strScriptEditorArgument;

  if (strArgument.IsEmpty ())
    strArgument = "\"%file\"";          // default

  // replace %file
  strArgument.Replace ("%file", strName);

  HINSTANCE hInst = ShellExecute (Frame, _T("open"), m_strScriptEditor, 
            strArgument,   // argument
            NULL, SW_SHOWNORMAL);

  if ((long) hInst <= 32)
      ::UMessageBox(TFormat ("Unable to edit file %s.",
                  (LPCTSTR) strName), 
                     MB_ICONEXCLAMATION);
  else
    {
    // bring editor to the front
    if (!m_strEditorWindowName.IsEmpty ())
      SendToFront (m_strEditorWindowName);
    }

  }




void CMUSHclientDoc::OnGameEditscriptfile() 
{

if (m_bEditScriptWithNotepad)
  {
  CTextDocument * pNewDoc =
    (CTextDocument *) App.OpenDocumentFile (m_strScriptFilename);

  if (pNewDoc)
    {
    pNewDoc->m_pRelatedWorld         = this;
    pNewDoc->m_iUniqueDocumentNumber = m_iUniqueDocumentNumber;   
    pNewDoc->SetTheFont ();
    }
  else
    ::TMessageBox("Unable to edit the script file.", 
                    MB_ICONEXCLAMATION);
  return;
  }   // end of using inbuilt notepad

  EditFileWithEditor (m_strScriptFilename);


	
}

void CMUSHclientDoc::OnUpdateGameEditscriptfile(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (!m_strScriptFilename.IsEmpty ());
	
}

void CMUSHclientDoc::OnUpdateGameConfigureMudaddress(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (m_iConnectPhase == eConnectNotConnected ||
                  m_iConnectPhase == eConnectConnectedToMud);   // no changing of address whilst connecting	
}


void CMUSHclientDoc::OnLogNotesChanged() 
{
  m_bLogNotes = m_bLogNotes != 0;   // make boolean
}


void CMUSHclientDoc::OnLogInputChanged() 
{
  m_log_input = m_log_input != 0;   // make boolean
}


void CMUSHclientDoc::OnLogOutputChanged() 
{
	// TODO: Add notification handler code
}

void CMUSHclientDoc::OnMappingChanged() 
{
	// TODO: Add notification handler code

}  // end of CMUSHclientDoc::OnMappingChanged

void CMUSHclientDoc::OnRemoveMapReversesChanged() 
{
	// TODO: Add notification handler code

}  // end of CMUSHclientDoc::OnRemoveMapReversesChanged


void CMUSHclientDoc::OnGameTrace() 
{

  if (m_bTrace)
    {
    Trace ("Trace off");
    m_bTrace = false;
    }
  else
    {
    // if half-filled line, flush it out
    if (m_pCurrentLine && m_pCurrentLine->len > 0)
       StartNewLine (true, m_pCurrentLine->flags);
    m_bTrace = true;
    Trace ("Trace on");
    }

	
}

void CMUSHclientDoc::OnUpdateGameTrace(CCmdUI* pCmdUI) 
{	
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable ();
  pCmdUI->SetCheck (m_bTrace);
}   // end of CMUSHclientDoc::OnUpdateGameTrace


// sorting for miniwindow z-order
// if equal z-order (eg. 0 which is the default) sort in window name order
// otherwise use z-order which means negative ones will be drawn before named ones (with 0 order)
// and positive ones after named ones (with 0 order)
bool lessWindow (const pair<string, CMiniWindow *> & w1, const pair<string, CMiniWindow *> & w2)
  {
   if (w1.second->m_ZOrder == w2.second->m_ZOrder)
     return w1.first < w2.first;

  return w1.second->m_ZOrder < w2.second->m_ZOrder;
  }   // end of lessWindow

// sort miniwindows vector into Z-order
void CMUSHclientDoc::SortWindows (void)
  {
  // start again
  m_MiniWindowsOrder.clear ();

  // reserve correct space
  m_MiniWindowsOrder.reserve (m_MiniWindows.size ());

  // rebuild vector of existing miniwindows
  for (MiniWindowMapIterator win_it = m_MiniWindows.begin (); 
       win_it != m_MiniWindows.end ();
       win_it++)
    m_MiniWindowsOrder.push_back (make_pair (win_it->first, win_it->second));

  sort (m_MiniWindowsOrder.begin (), m_MiniWindowsOrder.end (), lessWindow); 

  }   // end of CMUSHclientDoc::SortWindows


void CMUSHclientDoc::FixInputWrap()
{
  for(POSITION pos=GetFirstViewPosition();pos!=NULL;)
   {
   CView* pView = GetNextView(pos);

    if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
      {
      CSendView* pmyView = (CSendView*)pView;
      pmyView->UpdateWrap();
      }  // end of if CSendView
    }  // end of for each view
} // end of CMUSHclientDoc::FixInputWrap()

