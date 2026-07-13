// doc_shared.cpp — CMUSHclientDoc methods that are pure logic (no view/GUI
// dependency) and were previously reimplemented twice: in doc.cpp (the GUI
// engine) and doc_portable.cpp (the headless test engine). Compiling the single
// canonical implementation into BOTH mushapp and mushcore means it cannot drift
// between the builds (a real bug source: GetStyleRGB once bolded the wrong half
// of an inverse pair, speedwalk lost its C/O/L/K codes). Mirrors ansi.cpp /
// speedwalk.cpp, which likewise define shared CMUSHclientDoc methods.

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "MUSHclient.h"
#include "mainfrm.h"            // Frame (ConnectionClosed's status/button updates)
#include "doc.h"
#include "scripting/errors.h"   // eOK / eBadParameter (full build; doc_stub.h defines them for mushcore)
#include "dialogs/password.h"   // CPasswordDialog (ConnectionEstablished, full build)
#else
#include "platform/doc_stub.h"
// add ::Replace's default for the stub-only build (stdafx.h owns it in the
// full build; doc_stub.h deliberately declares without it - see there)
CString Replace (const CString strSource, const CString strTarget,
                 const CString strReplacement, const bool bAll = true);
#endif

#include "speedwalk.h"   // shared ExpandSpeedwalk / ReverseSpeedwalkString

#ifndef _WIN32
#include "platform/posix_transport.h"   // pal::ITransport + events (T3a bridge below)
#endif

#if defined(OPENSSL_FOUND)
#include <openssl/ssl.h>
#endif

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

void CMUSHclientDoc::GetStyleRGB (CStyle * pStyle, COLORREF & colour1, COLORREF & colour2) const
  {
unsigned short style;
int iForeground,
    iBackground;

  style = pStyle->iFlags & STYLE_BITS;

  if ((style & COLOURTYPE) == COLOUR_CUSTOM)
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < MAX_CUSTOM);
    if (style & INVERSE)    // inverse inverts foreground and background
      {
      // custom colour is stored in iForeColour only
      colour1 = m_customback [pStyle->iForeColour];
      colour2 = m_customtext [pStyle->iForeColour];
      }
    else
      {
      colour1 = m_customtext [pStyle->iForeColour];
      colour2 = m_customback [pStyle->iForeColour];
      }
    }
  // for RGB colour is just itself
  else if ((style & COLOURTYPE) == COLOUR_RGB)
    {
    if (style & INVERSE)    // inverse inverts foreground and background
      {
      colour1 = pStyle->iBackColour;
      colour2 = pStyle->iForeColour;
      }
    else
      {
      colour1 = pStyle->iForeColour;
      colour2 = pStyle->iBackColour;
      }
    }
  else
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < 256);
    ASSERT (pStyle->iBackColour >= 0 && pStyle->iBackColour < 256);

// display bold inverse differently according to user taste

    if (m_bAlternativeInverse)
      {
      iForeground = pStyle->iForeColour;
      iBackground = pStyle->iBackColour;

      if (style & INVERSE)    // inverse inverts foreground and background
        {
        if (iForeground >= 8)
          colour2 = xterm_256_colours [iForeground];
        else
          {
          if (style & HILITE)
            colour2 = m_boldcolour [iForeground];
          else
            colour2 = m_normalcolour [iForeground];
          }
        if (iBackground >= 8)
          colour1 = xterm_256_colours [iBackground];
        else
          colour1 = m_normalcolour [iBackground];
        }
      else
        {
        if (iForeground >= 8)
          colour1 = xterm_256_colours [iForeground];
        else
          {
          if (style & HILITE)
            colour1 = m_boldcolour [iForeground];
          else
            colour1 = m_normalcolour [iForeground];
          }
        if (iBackground >= 8)
          colour2 = xterm_256_colours [iBackground];
        else
          colour2 = m_normalcolour [iBackground];
        }   // end of not inverse


      } // end of alternate way
    else
      {
      if (style & INVERSE)    // inverse inverts foreground and background
        {
        iForeground = pStyle->iBackColour;
        iBackground = pStyle->iForeColour;
        }
      else
        {
        iForeground = pStyle->iForeColour;
        iBackground = pStyle->iBackColour;
        }

      if (style & HILITE)
        {
        if (iForeground >= 8)
          colour1 = xterm_256_colours [iForeground];
        else
          colour1 = m_boldcolour [iForeground];
        }
      else
        {
        if (iForeground >= 8)
          colour1 = xterm_256_colours [iForeground];
        else
          colour1 = m_normalcolour [iForeground];
        }
      if (iBackground >= 8)
        colour2 = xterm_256_colours [iBackground];
      else
        colour2 = m_normalcolour [iBackground];

      } // end of old way

    }   // not custom

  } // end of CMUSHclientDoc::GetStyleRGB 

void CMUSHclientDoc::RememberStyle (const CStyle * pStyle)
  {
  if (!pStyle)
    return;
  
  // for tracking down an obscure bug
  if ((pStyle->iFlags & COLOURTYPE) == COLOUR_CUSTOM)
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < MAX_CUSTOM);
    }
  else
  if ((pStyle->iFlags & COLOURTYPE) == COLOUR_ANSI)
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < 256);
    ASSERT (pStyle->iBackColour >= 0 && pStyle->iBackColour < 256);
    }


  m_iFlags       = pStyle->iFlags & STYLE_BITS; 
  m_iForeColour  = pStyle->iForeColour;         
  m_iBackColour  = pStyle->iBackColour;           
  
  } // end of CMUSHclientDoc::RememberStyle

// ── ShiftTabCompleteItem (Lua API: world.ShiftTabCompleteItem) ─────────────
long CMUSHclientDoc::ShiftTabCompleteItem(LPCTSTR Item)
  {
  if (strlen (Item) <= 0 || strlen (Item) > 30)
    return eBadParameter;

  if (strcmp (Item, "<clear>") == 0)
    m_ExtraShiftTabCompleteItems.clear ();
  else if (strcmp (Item, "<functions>") == 0)
    m_bTabCompleteFunctions = true;
  else if (strcmp (Item, "<nofunctions>") == 0)
    m_bTabCompleteFunctions = false;
  else
    {
    if (!IsValidName (Item))    // alphanumeric, dot, hyphen, underscore
      return eBadParameter;
    m_ExtraShiftTabCompleteItems.insert (Item);
    }
  return eOK;
  }

// ── SSLCleanup — tear down the OpenSSL session/context ─────────────────────
void CMUSHclientDoc::SSLCleanup (void)
  {
#if defined(OPENSSL_FOUND)
  if (m_pSSL)
    {
    if (m_bSSL_Connected)
      SSL_shutdown (m_pSSL);
    SSL_free (m_pSSL);
    m_pSSL = NULL;
    }
  if (m_pSSL_CTX)
    {
    SSL_CTX_free (m_pSSL_CTX);
    m_pSSL_CTX = NULL;
    }
#endif
  m_bSSL_Connected = false;
  }


// ── SOCKS proxy protocol + logging + unique numbers (P1a de-drift) ──────────
// Moved verbatim from doc.cpp (and GetUniqueNumber from methods_utilities.cpp);
// the hand-written doc_portable.cpp twins are deleted. One deliberate fix:
// SendProxyConnectSequence's address is 4 bytes on every platform now (the
// doc.cpp original used unsigned long, malformed on LP64 - see the comment).

void CMUSHclientDoc::WriteToLog (const char * text, size_t len)
  {
  if (!m_logfile || len <= 0)
    return;

size_t count;

  count = fwrite (text, 1, len, m_logfile);

  if (count != len)
    {
    CString str;
    str = TFormat ("An error occurred writing to log file \"%s\"",
                (LPCTSTR) m_logfile_name);
    fclose (m_logfile);
    m_logfile = NULL;
    UMessageBox (str);
    }   // end of error on write

  } // end of WriteToLog


void CMUSHclientDoc::WriteToLog (const CString & strText)
  {
  WriteToLog (strText, strText.GetLength ());
  }  // end of WriteToLog


int CMUSHclientDoc::ProcessProxyResponse1 (char * buff, const int count)
  {
  if (CheckExpectedProxyResponse (5, buff [0]))
    return -1;

  if (count < 2)
    buff [1] = (unsigned char) 0xFF; // no acceptable methods
  const char * p;

  switch (buff [1])
    {
    case 2: // username/password required - send authentication packet

      if (SendProxyUserNameAndPassword ())
        return -1;  // bad username/password lengths

      m_iConnectPhase = eConnectAwaitingProxyResponse2;
      return 2;   // expected bytes in this packet 

    case 0: // no authentication required

      SendProxyConnectSequence ();
      m_iConnectPhase = eConnectAwaitingProxyResponse3;
      return 2;   // expected bytes in this packet 

    case 1: 
      p = "GSSAPI method not supported"; 
      break;

    default: 
      p = "no acceptable methods"; 
      break;
    } // end of switch

  OnConnectionDisconnect ();    // close the world
  if (App.m_bNotifyIfCannotConnect)
    {
    CString strMsg = TFormat ("Proxy server cannot authenticate, reason: %s", 
                      (LPCTSTR) p);

    if (App.m_bErrorNotificationToOutputWindow)
      Note (strMsg);
    else
      UMessageBox (strMsg, MB_ICONEXCLAMATION);
    }
  return -1;    // error

  }   // end of CMUSHclientDoc::ProcessProxyResponse1


int CMUSHclientDoc::ProcessProxyResponse2 (char * buff, const int count)
  {
  if (CheckExpectedProxyResponse (1, buff [0]))
    return -1;

  if (count < 2 || buff [1] != 0)
    {
    OnConnectionDisconnect ();    // close the world
    if (App.m_bNotifyIfCannotConnect)
      {
      if (App.m_bErrorNotificationToOutputWindow)
        Note (Translate ("Proxy server refused authentication"));
      else
        TMessageBox ("Proxy server refused authentication",
                         MB_ICONEXCLAMATION);
      }
    return -1;

    } // end of error result


  SendProxyConnectSequence ();  
  m_iConnectPhase = eConnectAwaitingProxyResponse3;

  return 2;

  }   // end of CMUSHclientDoc::ProcessProxyResponse2


int CMUSHclientDoc::ProcessProxyResponse3 (char * buff, const int count)
  {
int iBytesToMove;

  if (CheckExpectedProxyResponse 
      (m_iSocksProcessing == eProxyServerSocks4 ? 0 : 5, 
        buff [0]))
    return -1;

  if (m_iSocksProcessing == eProxyServerSocks4)
    {  // SOCKS 4

    if (count < 2 || buff [1] != 90)
      {
      if (count < 2)
        buff [1] = 91; // request failed
      const char * p;
      switch (buff [1])
        {
        case 91: p = "request rejected or failed"; break;
        case 92: p = "cannot connect to ident server"; break;
        case 93: p = "client and identd report different user ids"; break;
        default: p = "unknown error reason"; break;
        } // end of switch

      OnConnectionDisconnect ();    // close the world
      if (App.m_bNotifyIfCannotConnect)
        {
        CString strMsg = TFormat ("Proxy server refused connection, reason: %s", 
                          (LPCTSTR) p);

        if (App.m_bErrorNotificationToOutputWindow)
          Note (strMsg);
        else
          UMessageBox (strMsg, MB_ICONEXCLAMATION);
        }
      return -1;

      } // end of error result

    iBytesToMove = 8;
    }  // SOCKS 4
  else
    {  // SOCKS 5

    if (count < 2 || buff [1] != 0)
      {
      if (count < 2)
        buff [1] = 1; // general failure
      const char * p;
      switch (buff [1])
        {
        case 1: p = "general SOCKS server failure"; break;
        case 2: p = "connection not allowed by ruleset"; break;
        case 3: p = "Network unreachable"; break;
        case 4: p = "Host unreachable"; break;
        case 5: p = "Connection refused"; break;
        case 6: p = "TTL expired"; break;
        case 7: p = "Command not supported"; break;
        case 8: p = "Address type not supported"; break;
        default: p = "unknown error reason"; break;
        } // end of switch

      OnConnectionDisconnect ();    // close the world
      if (App.m_bNotifyIfCannotConnect)
        {
        CString strMsg = TFormat ("Proxy server refused connection, reason: %s", 
                          (LPCTSTR) p);
        if (App.m_bErrorNotificationToOutputWindow)
          Note (strMsg);
        else
          UMessageBox (strMsg, MB_ICONEXCLAMATION);
        }
      return -1;

      } // end of error result
    iBytesToMove = 10;
    } // SOCKS 5
  
  if (m_bUseSSL)
    {
    StartSSLHandshake ();
    return iBytesToMove;
    }

  m_iConnectPhase = eConnectConnectedToMud;

  // let's get on with it ...
  ConnectionEstablished ();

  return iBytesToMove;

  }   // end of CMUSHclientDoc::ProcessProxyResponse3


bool CMUSHclientDoc::CheckExpectedProxyResponse (const char cExpected,
                                                 const char cReceived)
  {
  if (cReceived != cExpected)
    {
    OnConnectionDisconnect ();    // close the world
    if (App.m_bNotifyIfCannotConnect)
      {
      CString strMsg = TFormat ("Unexpected proxy server response %i, expected %i",
                        (int) cReceived,
                        (int) cExpected);
      if (App.m_bErrorNotificationToOutputWindow)
        Note (strMsg);
      else
        UMessageBox (strMsg, MB_ICONEXCLAMATION);
      }
    return true;   // not OK
    }

  return false;   // OK
  }  // end of CMUSHclientDoc::CheckExpectedProxyResponse


void CMUSHclientDoc::SendProxyConnectSequence (void)
  {
  TRACE ("\nSending SOCKS connect request\n");

  // 5 = SOCKS version
  // 1 = connect 
  // 0 = reserved
  // 1 = address is ip address
  // address in network order
  // port in network order
  // exactly 4 bytes on every platform: unsigned long is 8 bytes on LP64
  // (macOS/Linux), which would put a malformed 8-byte address on the wire;
  // on Win32 unsigned int is the same 4 bytes this always sent
  unsigned int address = m_sockAddr.sin_addr.s_addr;  // MUD address
  short port = m_sockAddr.sin_port;       // MUD port
  unsigned char socks_address [4 + sizeof address + sizeof port] = { 5, 1, 0, 1 };
  int socks_address_length = 4;  // ie. the 4 bytes above
  // address
  memcpy (&socks_address [socks_address_length], &address, sizeof address);
  socks_address_length += sizeof address;     // ie. 4
  // port
  memcpy (&socks_address [socks_address_length], &port, sizeof port);
  socks_address_length += sizeof port;     // ie. 2
  // send it
  SendPacket (socks_address, socks_address_length);

  }  // end of CMUSHclientDoc::SendProxyConnectSequence


bool CMUSHclientDoc::SendProxyUserNameAndPassword (void)
  {
  // lengths > 255 can't fit into the length byte
  if (m_strProxyUserName.GetLength () > 255 || 
      m_strProxyPassword.GetLength () > 255)
    {
    OnConnectionDisconnect ();    // close the world
    TMessageBox (
      "Proxy server username or password lengths cannot be > 255 characters",
                     MB_ICONEXCLAMATION);
    return true;
    }

  TRACE ("\nSending SOCKS username/password\n");

  // 1 = authentication version
  // x = username length
  // xxx = username
  // x = password length
  // xxx = password
  unsigned char authentication [3 + 255 + 255] = { 1 };
  int authentication_length = 1;  // ie. the method byte above
  unsigned char username_length = (unsigned char) m_strProxyUserName.GetLength ();
  unsigned char password_length = (unsigned char) m_strProxyPassword.GetLength ();
  // next byte is username length
  authentication [authentication_length++] = username_length;
  // followed by the username
  memcpy (&authentication [authentication_length], 
          (LPCTSTR) m_strProxyUserName, 
          username_length);
  // adjust index to bypass name
  authentication_length += username_length;   
  // next byte is password length
  authentication [authentication_length++] = password_length;
  // followed by password
  memcpy (&authentication [authentication_length], 
          (LPCTSTR) m_strProxyPassword, password_length);
  // adjust index to bypass password
  authentication_length += password_length;    
  // send authentication
  SendPacket (authentication, authentication_length);

  return false; // OK
  } // end of CMUSHclientDoc::SendProxyUserNameAndPassword


long CMUSHclientDoc::GetUniqueNumber() 
{
  return (long) (App.GetUniqueNumber () & 0x7FFFFFFF);
}  // end of CMUSHclientDoc::GetUniqueNumber



// ── speedwalk direction lookups + the map-direction table (P1b de-drift) ────
// Moved verbatim from methods_speedwalks.cpp and Mapping.cpp; the hardcoded
// doc_portable.cpp approximations are deleted. Every build now expands and
// reverses speedwalks through the SAME MapDirectionsMap the GUI uses
// (including the "f" filler entry and the ENDLINE = CRLF terminator the
// portable copies had drifted from).

CMapDirectionsMap MapDirectionsMap;

void LoadMapDirections (void)
  {
  //-------------------------------------------------------------------
  //                direction                 log        full     reverse
                                                        
  MapDirectionsMap ["north"]  = CMapDirection ("n",     "north",  "s");
  MapDirectionsMap ["south"]  = CMapDirection ("s",     "south",  "n");
  MapDirectionsMap ["east"]   = CMapDirection ("e",     "east",   "w");
  MapDirectionsMap ["west"]   = CMapDirection ("w",     "west",   "e");
  MapDirectionsMap ["up"]     = CMapDirection ("u",     "up",     "d");
  MapDirectionsMap ["down"]   = CMapDirection ("d",     "down",   "u");
  MapDirectionsMap ["ne"]     = CMapDirection ("ne",    "ne",     "sw");
  MapDirectionsMap ["nw"]     = CMapDirection ("nw",    "nw",     "se");
  MapDirectionsMap ["se"]     = CMapDirection ("se",    "se",     "nw");
  MapDirectionsMap ["sw"]     = CMapDirection ("sw",    "sw",     "ne");

  // filler
  MapDirectionsMap ["f"]      = CMapDirection ("f",     "f",      "f");

  // now do abbreviations (copy above entries)
  MapDirectionsMap ["n"]      = MapDirectionsMap ["north"];
  MapDirectionsMap ["s"]      = MapDirectionsMap ["south"];
  MapDirectionsMap ["e"]      = MapDirectionsMap ["east"];
  MapDirectionsMap ["w"]      = MapDirectionsMap ["west"];
  MapDirectionsMap ["u"]      = MapDirectionsMap ["up"];
  MapDirectionsMap ["d"]      = MapDirectionsMap ["down"];

  } // end of LoadMapDirections

// The GUI app loads the default directions from InitInstance; the headless
// engine has no InitInstance, so load them at static-initialisation time.
// (On the GUI builds the InitInstance call then re-assigns identical
// entries — idempotent, behaviour unchanged.)
static struct SLoadMapDirectionsAtStartup
  { SLoadMapDirectionsAtStartup () { LoadMapDirections (); } } zz_LoadMapDirections;

static CString SpeedwalkForwardDir (char letter)
  {
  return CString (MapDirectionsMap [string (1, letter)].m_sDirectionToSend.c_str ());
  }

static CString SpeedwalkReverseDir (char letter)
  {
  return CString (MapDirectionsMap [string (1, letter)].m_sReverseDirection.c_str ());
  }

static CString SpeedwalkReverseWord (const CString & word)
  {
  MapDirectionsIterator i = MapDirectionsMap.find ((LPCTSTR) word);
  if (i != MapDirectionsMap.end ())
    return CString (i->second.m_sReverseDirection.c_str ());
  return CString ();
  }

CString CMUSHclientDoc::DoEvaluateSpeedwalk(LPCTSTR SpeedWalkString)
{
  return ExpandSpeedwalk (SpeedWalkString, m_strSpeedWalkFiller, ENDLINE,
                          SpeedwalkForwardDir);
}   // end of CMUSHclientDoc::DoEvaluateSpeedwalk

CString CMUSHclientDoc::DoReverseSpeedwalk(LPCTSTR SpeedWalkString)
{
  return ReverseSpeedwalkString (SpeedWalkString, ENDLINE,
                                 SpeedwalkReverseDir, SpeedwalkReverseWord);
}   // end of CMUSHclientDoc::DoReverseSpeedwalk



// ── logging + plugin-list notification (P1c de-drift) ───────────────────────
// CloseLog (from methods_logging.cpp), LogCommand (doc.cpp) and
// PluginListChanged (plugins.cpp), moved verbatim; the doc_portable.cpp
// paraphrases are deleted. The headless engine now writes log preambles/
// postambles, HTML colour spans and the CloseLog return code exactly as the
// products do.

long CMUSHclientDoc::CloseLog() 
{

  if (m_logfile)
    {

  // write log file Postamble if wanted

    if (!m_strLogFilePostamble.IsEmpty () && !m_bLogRaw)
      {
      CTime theTime;
      theTime = CTime::GetCurrentTime();

      // allow %n for newline
      CString strPostamble = ::Replace (m_strLogFilePostamble, "%n", "\n");

      // allow for time-substitution strings
      strPostamble = FormatTime (theTime, strPostamble, m_bLogHTML);

      // this is open in text mode, don't want \r\r\n
      strPostamble.Replace (ENDLINE, "\n");

      WriteToLog (strPostamble); 
      WriteToLog ("\n", 1);
      }

    fclose (m_logfile);
    m_logfile = NULL;
    return eOK;
    }

	return eLogFileNotOpen;
}   // end of CMUSHclientDoc::CloseLog


void CMUSHclientDoc::LogCommand (const char * text)
  {

  if (m_logfile && 
      LoggingInput () &&
      !m_bLogRaw)
    {

    CString strMessage = text;

    // strip trailing endline
    if (strMessage.Right (2) == ENDLINE)
      strMessage = strMessage.Left (strMessage.GetLength () - 2);

    // this is open in text mode, don't want \r\r\n
    strMessage.Replace (ENDLINE, "\n");

    // get appropriate preamble
    CString strPreamble = m_strLogLinePreambleInput;

    // allow %n for newline
    strPreamble.Replace ("%n", "\n");

    if (strPreamble.Find ('%') != -1)
      strPreamble = FormatTime (CTime::GetCurrentTime(), strPreamble, m_bLogHTML);

    // get appropriate Postamble
    CString strPostamble = m_strLogLinePostambleInput;

    // allow %n for newline
    strPostamble.Replace ("%n", "\n");

    if (strPostamble.Find ('%') != -1)
      strPostamble = FormatTime (CTime::GetCurrentTime(), strPostamble, m_bLogHTML);

    // line preamble
    WriteToLog (strPreamble); 
    // line itself
    // fix up HTML sequences
    if (m_bLogHTML)
      {
      strMessage = FixHTMLString (strMessage);

      // change to command colour if wanted
      if (m_bLogInColour && m_echo_colour != SAMECOLOUR)
        {
        COLORREF colour = m_customtext [m_echo_colour];
        WriteToLog (CFormat ("<font color=\"#%02X%02X%02X\">",
                              GetRValue (colour),
                              GetGValue (colour),
                              GetBValue (colour)));
        }  // end of logging commands in a different colour
      } // end of logging in HTML

    WriteToLog (strMessage);

    // cancel command colour
    if (m_bLogHTML && 
        m_bLogInColour && 
        m_echo_colour != SAMECOLOUR)
        WriteToLog ("</font>");

    // line Postamble
    WriteToLog (strPostamble); 
    WriteToLog ("\n", 1);
    }   // end of logging wanted

  } // end of LogCommand


void  CMUSHclientDoc::PluginListChanged (void)

  {

static bool bInPluginListChanged = false;

  // don't recurse into infinite loops
  if (bInPluginListChanged)
    return;

  bInPluginListChanged = true;
  SendToAllPluginCallbacks (ON_PLUGIN_LIST_CHANGED);
  bInPluginListChanged = false;

  }    // end CMUSHclientDoc::PluginListChanged 


// ── ConnectionClosed ─────────────────────────────────────────────────────────
// The engine's disconnect bookkeeping: scripts, plugin callbacks, auto-log
// close, session-statistics Notes, notify UX, phase reset.  Moved verbatim
// from CWorldSocket::OnClose (worldsock.cpp) — ITransport migration step T2a:
// this is ENGINE logic, not socket mechanics, and the transport adapter's
// onClosed event will land here once the seam is live.  Runs in both builds
// (the socket TU is shared too, so this was already dual-build code).

void CMUSHclientDoc::ConnectionClosed (int nErrorCode)
  {

bool bWasClosed = m_iConnectPhase == eConnectNotConnected;

  TRACE1 ("CMUSHclientDoc::ConnectionClosed, error code %i\n", nErrorCode);

  SSLCleanup ();

  m_iConnectPhase = eConnectDisconnecting;

  UpdateAllViews (NULL);

  MXP_Off (true);   // turn off MXP now

  // update button bar - make button red
  if (m_view_number >= 1 &&
      m_view_number <= 10)
    Frame.OnUpdateBtnWorlds (m_view_number, NULL);

  // execute "disconnect" script
  if (m_ScriptEngine)
    {
    if (SeeIfHandlerCanExecute (m_strWorldDisconnect))
      {
      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      ExecuteScript (m_dispidWorldDisconnect,  
                     m_strWorldDisconnect,
                     eWorldAction,
                     "world disconnect", 
                     "disconnecting from world",
                     params, 
                     nInvocationCount); 
      }
    } // end of executing disconnect script

  SendToAllPluginCallbacks (ON_PLUGIN_DISCONNECT);

  // close log file if we auto-opened it
  if (!m_strAutoLogFileName.IsEmpty ())
    CloseLog ();

  if (m_bShowConnectDisconnect && !bWasClosed)
    {
    CTime theTime;
    theTime = CTime::GetCurrentTime();

    CString strConnected;
    strConnected = theTime.Format (TranslateTime ("--- Disconnected on %A, %B %d, %Y, %#I:%M %p ---"));

    Note (strConnected);  

    // find time spent connected
    CTimeSpan ts = CTime::GetCurrentTime() - m_tConnectTime;
      
    CString strDuration = TFormat ("--- Connected for %i day%s, %i hour%s, %i minute%s, %i second%s. ---",
                  PLURAL ((long) ts.GetDays()),
                  PLURAL ((long) ts.GetHours()),
                  PLURAL ((long) ts.GetMinutes()),
                  PLURAL ((long) ts.GetSeconds()));

    Note (strDuration);  

    // and a horizontal rule

    m_pCurrentLine->flags = HORIZ_RULE;  
    StartNewLine (true, 0); 
    }  // end of message in world window wanted

  CString strInfo = TFormat ("--- Received %i line%s, sent %i line%s.",
                PLURAL (m_nTotalLinesReceived),
                PLURAL (m_nTotalLinesSent)
                );
  
  Note (strInfo);  

  strInfo = TFormat ("--- Output buffer has %i/%i line%s in it (%.1f%% full).",
                m_LineList.GetCount (),
                m_maxlines,
                (m_LineList.GetCount ()) == 1 ? "" : "s",
                (double) m_LineList.GetCount () / (double) m_maxlines * 100.0
                );

  Note (strInfo);  

  strInfo = TFormat ("--- Matched %i trigger%s, %i alias%s, and %i timer%s fired.",
                PLURAL (m_iTriggersMatchedThisSessionCount),   
                PLURALES (m_iAliasesMatchedThisSessionCount),    
                PLURAL (m_iTimersFiredThisSessionCount)       
                );

  Note (strInfo);  

CString str;

  str = TFormat ("The \"%s\" server has closed the connection", 
              (const char *) m_mush_name);

  if (App.m_bNotifyOnDisconnect && !m_bDisconnectOK)
    {
    if (App.m_bErrorNotificationToOutputWindow)
      Note (str);
    else
      ::UMessageBox (str, MB_ICONEXCLAMATION);
    }
  else
    Frame.SetStatusMessage (str);
  m_iConnectPhase = eConnectNotConnected;

  UpdateAllViews  (NULL);     // force window title to be redrawn

  } // end of CMUSHclientDoc::ConnectionClosed


// ── Session establishment (single-sourced, T3d) ─────────────────────────────

long CMUSHclientDoc::OpenLog(LPCTSTR LogFileName, BOOL Append) 
{

  if (m_logfile)
  	return eLogFileAlreadyOpen;

  m_logfile_name = LogFileName;

  // if no log file name, use auto-log file name if we can

  if (m_logfile_name.IsEmpty ())
    {
    CTime theTime;
    theTime = CTime::GetCurrentTime();

    m_logfile_name = FormatTime (theTime, m_strAutoLogFileName, false);

    }

  // no file name? can't open it then
  if (m_logfile_name.IsEmpty ())
    return eCouldNotOpenFile;

  m_logfile = fopen (m_logfile_name, Append ? "a+" : "w");
	
  // close and re-open to make sure it is in the disk directory
  if (m_logfile)
    {
    fclose (m_logfile);
    m_logfile = fopen (m_logfile_name, Append ? "a+" : "w");
    }

	if (m_logfile)
    return eOK;
  else
    return eCouldNotOpenFile;

}   // end of CMUSHclientDoc::OpenLog


// we come here when we have connected to a normal MUD,
// or been validated on a proxy server
void CMUSHclientDoc::ConnectionEstablished (void)
  {

#ifdef _DEBUG
//  ListAccelerators (this, 1);     // for documenting menus, accelerators
//  ListDialogs ();

#endif 

// we have connected!!!!!!!!!!!!!!!

  m_iConnectPhase = eConnectConnectedToMud;
  m_tConnectTime = CTime::GetCurrentTime();           
  m_tLastPlayerInput = CTime::GetCurrentTime();           
	m_bEnableAutoSay = FALSE;		// auto-say off at start of session
  App.m_bUpdateActivity = TRUE;   // new activity!
  m_bCompress = FALSE;        // not compressing yet
  m_nTotalLinesSent = 0;    // no lines sent yet
  m_nTotalLinesReceived = 0;  // no lines received yet
  m_iTriggersMatchedThisSessionCount = 0;   
  m_iAliasesMatchedThisSessionCount = 0;    
  m_iTimersFiredThisSessionCount = 0;       
  m_bSuppressNewline = false; 
  m_iLastCommandCount = 0;
  m_sRecentLines.clear ();
  m_newlines_received = 0;
  m_strLastCommandSent.Empty ();  // no command sent yet
  m_iNoteStyle = NORMAL;    // back to default style
  m_ttype_sequence = 0;     // back to sending terminal name


  ZeroMemory (&m_bClient_sent_IAC_DO,   sizeof m_bClient_sent_IAC_DO);
  ZeroMemory (&m_bClient_sent_IAC_DONT, sizeof m_bClient_sent_IAC_DONT);
  ZeroMemory (&m_bClient_sent_IAC_WILL, sizeof m_bClient_sent_IAC_WILL);
  ZeroMemory (&m_bClient_sent_IAC_WONT, sizeof m_bClient_sent_IAC_WONT);

  ZeroMemory (&m_bClient_got_IAC_DO,   sizeof m_bClient_got_IAC_DO);
  ZeroMemory (&m_bClient_got_IAC_DONT, sizeof m_bClient_got_IAC_DONT);
  ZeroMemory (&m_bClient_got_IAC_WILL, sizeof m_bClient_got_IAC_WILL);
  ZeroMemory (&m_bClient_got_IAC_WONT, sizeof m_bClient_got_IAC_WONT);

  m_phase = NONE;   // not in middle of telnet/mxp sequence yet
  m_IAC_subnegotiation_data.erase ();
  m_subnegotiation_type = 0;

  Note ("");   // ensure connection starts on new line and that pixel offset doesn't chop message
  if (m_bShowConnectDisconnect)
    {

    CString strConnected;
    strConnected = m_tConnectTime.Format (TranslateTime ("--- Connected on %A, %B %d, %Y, %#I:%M %p ---"));

    Note (strConnected);  

    }

  if (m_iUseMXP == eUseMXP)
    MXP_On ();    // if MXP is wanted turn it on now


  SetTitle(m_mush_name);

// ********* debugging ********

  // this is so I can work out what a good <hr> character will be

#if 0
  for (int i = 0; i <= 255; i++)
    {
    CString strLine;

    strLine = CFormat ("char = %i %c%c%c%c",
              i,
              (char) i,
              (char) i,
              (char) i,
              (char) i);

    AddToLine (strLine, 0);
    StartNewLine (true, 0); // now finish this line

    }
#endif 

// ********* end debugging *********


  /*
  SOCKADDR_IN sockaddr;
  int sockaddr_length = sizeof sockaddr;

  if (m_pSocket->GetSockName ((SOCKADDR*) &sockaddr, &sockaddr_length))
    {
    CString strIpAddress = inet_ntoa (sockaddr.sin_addr);
    ::AfxMessageBox ( strIpAddress);
    }
    */
           /*
unsigned long ulAddress = inet_addr ("127.0.0.1");
LPHOSTENT lpHostEntry = gethostbyaddr ((const char *) &ulAddress, sizeof ulAddress, AF_INET);
CString strIpAddress = inet_ntoa (*((struct in_addr *) lpHostEntry->h_addr));
::AfxMessageBox ( strIpAddress);
            */

// auto log if necessary

  if (!m_strAutoLogFileName.IsEmpty ())
    {
    CTime theTime;
    theTime = CTime::GetCurrentTime();

    CString strName = FormatTime (theTime, m_strAutoLogFileName, m_bLogHTML);

    if (OpenLog (strName, true) == eCouldNotOpenFile)
      UMessageBox (TFormat ("Could not open log file \"%s\"",
            (LPCTSTR) strName), MB_ICONINFORMATION);
    else
      {
    // write log file preamble if wanted

      if (!m_strLogFilePreamble.IsEmpty ())
        {

        // allow %n for newline
        CString strPreamble = ::Replace (m_strLogFilePreamble, "%n", "\n");

        // allow for time-substitution strings
        strPreamble = FormatTime (theTime, strPreamble, m_bLogHTML);
        // this is open in text mode, don't want \r\r\n
        strPreamble.Replace (ENDLINE, "\n");

        WriteToLog (strPreamble); 
        WriteToLog ("\n", 1);

        }   // end of writing preamble

      if (m_bWriteWorldNameToLog)
        {        

      // write preamble so they know what this is all about

        CString strTime;

        strTime = theTime.Format (TranslateTime ("%A, %B %d, %Y, %#I:%M %p"));

        CString strPreamble = m_mush_name;
        strPreamble += " - ";
        strPreamble += strTime;

        if (m_bLogHTML)
          {
          WriteToLog ("<br>\n");     // put blank line after previous output
          WriteToLog (FixHTMLString (strPreamble)); 
          WriteToLog ("<br>\n");     // put blank line after previous output
          }
        else
          {
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

      }   // end of opened OK
    } // end of auto log file name

// ask for password if necessary, and send connection string

  CString password = m_password;

// they didn't supply a password, so ask for it now

    if (password.IsEmpty () && 
          (
          (m_connect_now && !m_name.IsEmpty ()) ||
          (m_connect_text.Find ("%password%") != -1)
          )
        )
      {
#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
      CPasswordDialog dlg;

        dlg.m_character = m_name;

        if (dlg.DoModal () == IDOK)
          password = dlg.m_password;
#else
      // headless: no password prompt — an empty password stays empty (the
      // auto-login below then sends nothing, like a cancelled prompt)
#endif
      }

  switch (m_connect_now)
    {

    // send CONNECT name password
    case eConnectMUSH:

      if (!password.IsEmpty ())
        {
        CString str;
        str.Format ("connect %s %s", 
                    (const char *) m_name,
                    (const char *) password);

        // don't echo password on screen for all to see (or log file)
        SendMsg (str, false, false, false);

        }   // end of having a password
      break;

    // send:  name
    //        password

    case eConnectDiku:
        SendMsg (m_name, m_display_my_input, false, false);
        if (!password.IsEmpty ())
          SendMsg (password, false, false, false);
        break;

    default:
      // do nothing here
      break;

    }   // end of connecting now

// now send the extra connect string

  if (!m_connect_text.IsEmpty ())
    {
    CString strText = ::Replace (m_connect_text, "%name%", m_name);
    strText.Replace ("%password%", password);
    // don't display in case it contains a password
    SendMsg (strText, false, false, false);
    }

// get timers going

  ResetAllTimers (m_TimerMap);
  
  // defer resetting this so timers don't get going too soon
  m_iConnectPhase = eConnectConnectedToMud;

  App.m_bUpdateActivity = TRUE;   // new activity!

// negotiate about window size (NAWS)

  if (m_bNAWS)
    {
    // offer to negotiate about window size
    unsigned char p [3] = { IAC, WILL, TELOPT_NAWS };
    SendPacket (p, sizeof p);
    }

// update windows titles, etc.
  
  UpdateAllViews (NULL);

  Frame.SetStatusNormal (); 

  // execute "connect" script
  if (m_ScriptEngine)
    {
    if (SeeIfHandlerCanExecute (m_strWorldConnect))
      {

      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      ExecuteScript (m_dispidWorldConnect,  
                   m_strWorldConnect,
                   eWorldAction,
                   "world connect", 
                   "connecting to world",
                   params, 
                   nInvocationCount); 
      }
    } // end of executing open script

  SendToAllPluginCallbacks (ON_PLUGIN_CONNECT);

  } // end of CMUSHclientDoc::ConnectionEstablished


// ── Connect / receive-tail / disconnect (single-sourced, T3c) ───────────────

//#include <mstcpip.h>
// From: mstcpip.h
/* Argument structure for SIO_KEEPALIVE_VALS */

#ifdef _WIN32
struct tcp_keepalive {
    ULONG onoff;
    ULONG keepalivetime;
    ULONG keepaliveinterval;
};
#endif // _WIN32

//
// New WSAIoctl Options
// 
#ifdef _WIN32
#define SIO_RCVALL            _WSAIOW(IOC_VENDOR,1)
#define SIO_RCVALL_MCAST      _WSAIOW(IOC_VENDOR,2)
#define SIO_RCVALL_IGMPMCAST  _WSAIOW(IOC_VENDOR,3)
#define SIO_KEEPALIVE_VALS    _WSAIOW(IOC_VENDOR,4)
#define SIO_ABSORB_RTRALERT   _WSAIOW(IOC_VENDOR,5)
#define SIO_UCAST_IF          _WSAIOW(IOC_VENDOR,6)
#define SIO_LIMIT_BROADCASTS  _WSAIOW(IOC_VENDOR,7)
#define SIO_INDEX_BIND        _WSAIOW(IOC_VENDOR,8)
#define SIO_INDEX_MCASTIF     _WSAIOW(IOC_VENDOR,9)
#define SIO_INDEX_ADD_MCAST   _WSAIOW(IOC_VENDOR,10)
#define SIO_INDEX_DEL_MCAST   _WSAIOW(IOC_VENDOR,11)
//      SIO_UDP_CONNRESET     _WSAIOW(IOC_VENDOR,12)
#define SIO_RCVALL_MCAST_IF   _WSAIOW(IOC_VENDOR,13)
#define SIO_RCVALL_IF         _WSAIOW(IOC_VENDOR,14)
#endif // _WIN32


BOOL CMUSHclientDoc::ConnectSocket(void)
{

CString str;

  // no IP? don't connect
  if (m_server == "0.0.0.0")
    return FALSE;

// a few checks, in case they slip in blank names/site/port via configuration

  if (m_mush_name.IsEmpty ())
    {
    TMessageBox ("Cannot connect. World name not specified");
		  return FALSE;
    }

  if (m_server.IsEmpty ())
    {
    UMessageBox (TFormat ("Cannot connect to \"%s\", TCP/IP address not specified", 
                    (const char *) m_mush_name));
		  return FALSE;
    }

  if (m_port <= 0)
    {
    UMessageBox (TFormat ("Cannot connect to \"%s\", port number not specified", 
                    (const char *) m_mush_name));
		  return FALSE;
    }

  switch (m_iSocksProcessing)
    {
    case eProxyServerNone:
      break;    // nothing needed here

    case eProxyServerSocks4:     // socks 4
    case eProxyServerSocks5:     // socks 5
      if (m_strProxyServerName.IsEmpty ())
          {
          TMessageBox("The proxy server address cannot be blank.");
		      return FALSE;
          }     // end of proxy server name being blank

      if (m_iProxyServerPort == 0)
          {
          TMessageBox("The proxy server port must be specified.");
		      return FALSE;
          }     // end of proxy server port being zero
      break;

    default:
          UMessageBox(TFormat ("Unknown proxy server type: %d.", m_iSocksProcessing));
		      return FALSE;
          break;

    } // end of switch

	m_bEnableAutoSay = FALSE;		// auto-say off at start of session

  m_bDisconnectOK = false;    // not OK to disconnect
  
  m_bCompress = FALSE;        // not compressing yet
  m_iMCCP_type = 0;           // no MCCP type yet
  m_bSupports_MCCP_2 = false; // haven't offered MCCP 2 yet
  m_bNoEcho = false;          // no echo suppression yet
  m_iInputPacketCount = 0;
  m_iOutputPacketCount = 0;
  m_iUTF8ErrorCount = 0;

  str = TFormat ("Connecting to %s, port %d", 
              (const char *) m_mush_name, m_port);
  Frame.SetStatusMessageNow (str);

#ifndef _WIN32

// T2d: off-Windows the connection runs through the PAL transport
// (pal::ITransport, POSIX adapter) — CWorldSocket is not created and
// m_pSocket stays NULL.  The adapter creates the fd at connect time and
// applies the keepalive policy itself (TransportOptions carries on/off;
// the 120 s / 6 s / 6-probe timing is fixed engine policy inside it).

  CreateTransport ();

#else

// get rid of any earlier socket

  delete m_pSocket;

// now create a new one

	m_pSocket = new CWorldSocket(this);

	if (!m_pSocket->Create(0,
                         SOCK_STREAM,
                         FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE,
                         NULL))
	  {

    int iStatus = GetLastError ();

    // tell them socket create failed

    UMessageBox (TFormat ("Unable to create TCP/IP socket for \"%s\", code = %i (%s)",
                    (const char *) m_mush_name,
                    iStatus,
                    (LPCTSTR) GetSocketError (iStatus)));

		  delete m_pSocket;
		  m_pSocket = NULL;

		  return FALSE;
	  }     // end of can't create socket

// stop socket from lingering on close

//  BOOL bLinger = TRUE;
//  m_pSocket->SetSockOpt (SO_DONTLINGER, &bLinger, sizeof bLinger);


  // see: http://www.gammon.com.au/forum/?id=9848
  if (m_bSendKeepAlives)
    {
    DWORD dwBytesReturned;
    tcp_keepalive keepaliveData = {1, 60 * 1000 * 2, 6000};     // every 2 minutes
    WSAIoctl (m_pSocket->m_hSocket, SIO_KEEPALIVE_VALS, &keepaliveData, sizeof (keepaliveData), NULL, 0, &dwBytesReturned, NULL, NULL);
    }

  BOOL bStatus = m_pSocket->AsyncSelect ();
  int iStatus = 0;

  if (!bStatus)
    iStatus = GetLastError ();

#endif // _WIN32

  m_iConnectPhase = eConnectNotConnected;

  App.m_bUpdateActivity = TRUE;   // new activity!

  m_sockAddr.sin_port = htons((u_short)m_port);   // normal MUD port
  m_ProxyAddr.sin_port = htons((u_short)m_iProxyServerPort);  // proxy server port

// fix up server name to dotted a socket address, if necessary

  if (m_sockAddr.sin_family != AF_INET || m_sockAddr.sin_addr.s_addr == INADDR_NONE)
    {

	  m_sockAddr.sin_family = AF_INET;
	  m_sockAddr.sin_addr.s_addr = inet_addr(m_server);

// if address is INADDR_NONE then address was a name, not a number

	  if (m_sockAddr.sin_addr.s_addr == INADDR_NONE)
	   {
      m_iConnectPhase = eConnectMudNameLookup;

      if (LookupHostName (m_server))
        return FALSE;

      return TRUE;    // we are waiting for host name lookup to finish

	   }   // end of address not being an IP address

    }   // end of not having converted the name to an address

// if we got this far we know the host IP address - how about the proxy server address?

  if (m_iSocksProcessing != eProxyServerNone)
    {
    if (m_ProxyAddr.sin_family != AF_INET || m_ProxyAddr.sin_addr.s_addr == INADDR_NONE)
      {

	    m_ProxyAddr.sin_family = AF_INET;
	    m_ProxyAddr.sin_addr.s_addr = inet_addr(m_strProxyServerName);

  // if address is INADDR_NONE then address was a name, not a number

	    if (m_ProxyAddr.sin_addr.s_addr == INADDR_NONE)
	     {
        m_iConnectPhase = eConnectProxyNameLookup;

        if (LookupHostName (m_strProxyServerName))
          return FALSE;

        return TRUE;    // we are waiting for proxy name lookup to finish

	     }   // end of address not being an IP address

      }   // end of not having converted the name to an address

    }   // end of needing a proxy server address


// it seems both the world and proxy server addresses are known - 
// get on with connecting to one of them

  InitiateConnection ();

	return TRUE;
}


// ── OnBytesReceived ──────────────────────────────────────────────────────────
// Everything that happens to a packet AFTER the transport delivered it:
// proxy-phase parsing, byte counters, MCCP decompression, raw logging, and
// the DisplayMsg pipeline hand-off.  Split out of ReceiveMsg (ITransport
// migration step T2b) — this is where the transport adapter's onBytes event
// lands once the seam is live.  `buff` may be mutated (proxy memmove).

void CMUSHclientDoc::OnBytesReceived (char * buff, int count)
{
  Frame.CheckTimerFallback ();   // see if time is up for timers to fire

//  TRACE1 ("Phase now = %i\n", m_iConnectPhase);
//  TRACE2 ("Buff [0] = %i, Buff [1] = %i\n",
//          (int) buff [0], (int) buff [1]);

// if we are expecting a response from the proxy server, process that

// SOCKS 5 - see RFC 1928

  if (m_iConnectPhase == eConnectAwaitingProxyResponse1 ||
      m_iConnectPhase == eConnectAwaitingProxyResponse2 ||
      m_iConnectPhase == eConnectAwaitingProxyResponse3)
    {
    int iBytesToMove = 0;

    switch (m_iConnectPhase)
      {
      case eConnectAwaitingProxyResponse1: 
         iBytesToMove = ProcessProxyResponse1 (buff, count);
         break;

      case eConnectAwaitingProxyResponse2: 
         iBytesToMove = ProcessProxyResponse2 (buff, count);
         break;

      case eConnectAwaitingProxyResponse3:
         iBytesToMove = ProcessProxyResponse3 (buff, count);
         break;
      
      }   // end of switch

    if (iBytesToMove < 0)
      return;   // error return

    // only the expected response? that's it for that packet
    if (count <= iBytesToMove)
      return;

    // throw away proxy response, and keep going
    memmove (buff, &buff [iBytesToMove], count - iBytesToMove);
    count -= iBytesToMove;

    }   // end of processing proxy server response

  // we aren't connected - discard anything we get here
  if (m_iConnectPhase != eConnectConnectedToMud)
    return;

  m_iInputPacketCount++;       // count packets
  m_nBytesIn += count;    // count bytes in

  if (m_bCompress)   // if we are compressing, put data into zlib buffer
    {
    if ((COMPRESS_BUFFER_LENGTH - m_zCompress.avail_in) < (uInt) count)
      {
      OnConnectionDisconnect ();    // close the world
      TMessageBox ("Insufficient memory in buffer to decompress text", MB_ICONEXCLAMATION);
      return;
      }
    // shuffle existing input to-be-processed to start of buffer
    if (m_zCompress.avail_in)
      memmove (m_CompressInput, m_zCompress.next_in, m_zCompress.avail_in);
    m_zCompress.next_in = m_CompressInput;
    // add new stuff
    memcpy (&m_zCompress.next_in [m_zCompress.avail_in], buff, count);
    m_zCompress.avail_in += count;
    // stats - count compressed bytes
    m_nTotalCompressed += count;
    }

  // now display the message, unless we are getting compressed data

  // decompress until something stops us (like the big goat)
  while (true)
    {

    // if we aren't in decompression mode, just process the text as usual
    if (!m_bCompress)   // not compressed yet
      {
      if (m_logfile && m_bLogRaw)  // raw log if wanted
        WriteToLog (buff, count);  
      DisplayMsg (buff, count, 0);
      }

    // if that didn't throw us into compressed, mode, just exit
    if (!m_bCompress)  
      break;    // still not compressed - exit

    // is compressed now

    // give up if nothing to display
    if (m_zCompress.avail_in <= 0)
       break;    // no data to process

    LARGE_INTEGER start, 
                  finish;

    if (App.m_iCounterFrequency)
      QueryPerformanceCounter (&start);
    else
      {
      start.QuadPart = 0;
      finish.QuadPart = 0;
      }

    // decompress it
    int iCompressResult;
    do {
       iCompressResult = inflate (&m_zCompress, Z_SYNC_FLUSH);

       // buffer too small? (highly compressed text, huh?)
       // make larger, try again - version 4.74
       // See: http://www.gammon.com.au/forum/?id=11160 
       if (iCompressResult == Z_BUF_ERROR)
         {
         m_nCompressionOutputBufferSize += COMPRESS_BUFFER_LENGTH;
         m_zCompress.avail_out += COMPRESS_BUFFER_LENGTH;
         m_CompressOutput = (Bytef *) realloc (m_CompressOutput, m_nCompressionOutputBufferSize);

         if (m_CompressOutput == NULL)
           {
            OnConnectionDisconnect ();    // close the world
            free (m_CompressInput);       // may as well get rid of compression input as well
            m_CompressInput = NULL;
            TMessageBox ("Insufficient memory to decompress MCCP text.", MB_ICONEXCLAMATION);
            return;
           }  // end of cannot get more memory
         }  // end of Z_BUF_ERROR

      } while (iCompressResult == Z_BUF_ERROR);

    if (App.m_iCounterFrequency)
      {
      QueryPerformanceCounter (&finish);
      m_iCompressionTimeTaken += finish.QuadPart - start.QuadPart;
      }

    // error?
    if (iCompressResult < 0)
      {
      char * pMsg = m_zCompress.msg;    // closing may clear the message

      OnConnectionDisconnect ();    // close the world
      if (pMsg)
        UMessageBox (TFormat ("Could not decompress text from MUD: %s",
                          (LPCTSTR) pMsg), MB_ICONEXCLAMATION);
      else
        UMessageBox (TFormat ("Could not decompress text from MUD: %i",
                          iCompressResult), MB_ICONEXCLAMATION);
      return;
      }

    // work out how much we got, and display it
    int iLength = m_nCompressionOutputBufferSize - m_zCompress.avail_out;
    
    // stats - count uncompressed bytes
    m_nTotalUncompressed += iLength;

    if (iLength > 0)
      {
      // display it - hopefully we won't get a compression code in the middle of our
      // compressed data - *that* would cause problems. :)

      if (m_logfile && m_bLogRaw)  // raw log if wanted
        WriteToLog ((LPCTSTR) m_CompressOutput, iLength);  

      DisplayMsg ((LPCTSTR) m_CompressOutput, iLength, 0);    // send uncompressed data to screen
      m_zCompress.next_out = m_CompressOutput;      // reset for more output
      m_zCompress.avail_out = m_nCompressionOutputBufferSize;
      }

    // if end of stream, turn decompression off
    if (iCompressResult == Z_STREAM_END)
      {    // we can stop decompressing
      m_bCompress = false;
      // put remaining stuff back into buff
      memcpy (buff, m_zCompress.next_in, m_zCompress.avail_in);
      count = m_zCompress.avail_in;
      }   // end of Z_STREAM_END

    }   // end of decompression loop

}


void CMUSHclientDoc::OnConnectionDisconnect() 
{

  TRACE ("CMUSHclientDoc::OnConnectionDisconnect\n");

  m_bDisconnectOK = true;     // don't want reconnect on manual disconnect

  // work out how long they were connected
  m_tsConnectDuration += CTime::GetCurrentTime() - m_tConnectTime;

// first cancel any host name lookup that might be in progress

  if (m_hNameLookup)
    WSACancelAsyncRequest (m_hNameLookup);  // cancel host name lookup in progress

  delete [] m_pGetHostStruct;   // delete buffer used by host name lookup
  m_pGetHostStruct = NULL;
  m_hNameLookup = NULL;

  App.m_bUpdateActivity = TRUE;   // new activity!

  MXP_Off (true);   // turn off MXP now

// clean up SSL before closing the socket

  SSLCleanup ();

// close the socket

#ifndef _WIN32
  // T2d: tear down the PAL transport (the port's socket) — mirrors the
  // CWorldSocket sequence below: cancel any DNS lookup in flight, shut the
  // stream down, run the disconnect bookkeeping, destroy.
  if (m_pTransport)
    {
    m_pTransport->cancelResolve ();
    m_pTransport->close ();
    ConnectionClosed (0);
    delete m_pTransport;
    m_pTransport = NULL;
    }
#endif

#ifdef _WIN32
	if (m_pSocket)
	{

    ShutDownSocket (*m_pSocket);

    m_pSocket->OnClose (0);

// delete the socket

    delete m_pSocket;
    m_pSocket = NULL;

    }
#endif // _WIN32

  m_iConnectPhase = eConnectNotConnected;

  }


#ifndef _WIN32
// StartSSLHandshake, port body (T3c): the transport owns the WHOLE TLS
// session (context, non-blocking handshake stepping, verification, encrypted
// I/O, the decrypted-bytes drain).  Policy stays in the engine: the outcome
// lands in OnTlsEstablished / OnTlsFailed via the events bridge.  The
// Windows body (native OpenSSL over CWorldSocket) stays in doc.cpp until T5.
void CMUSHclientDoc::StartSSLHandshake (void)
  {

  TRACE ("CMUSHclientDoc::StartSSLHandshake\n");

  m_iConnectPhase = eConnectAwaitingSSLHandshake;

  if (m_pTransport)
    {
    pal::TlsParams params;
    params.serverName = (const char *) m_server;
    params.allowInvalidCert = m_bSSLAllowInvalidCert;
    m_pTransport->startTls (params);
    }

  }   // end of CMUSHclientDoc::StartSSLHandshake
#endif // !_WIN32


// ── Send + connect-outcome (single-sourced, T3b) ────────────────────────────

CString GetConnectionStatus (const int iStatus)
  {
  CString strResult = Translate ("<unknown>");

  switch (iStatus)
    {
    case eConnectNotConnected:          strResult = Translate ("Closed"); break;
    case eConnectMudNameLookup:         strResult = Translate ("Look up world name"); break;
    case eConnectProxyNameLookup:       strResult = Translate ("Look up proxy name"); break;
    case eConnectConnectingToMud:       strResult = Translate ("Connecting to world"); break;
    case eConnectConnectingToProxy:     strResult = Translate ("Connecting to proxy"); break;
    case eConnectAwaitingProxyResponse1:strResult = Translate ("Awaiting proxy response (1)"); break;
    case eConnectAwaitingProxyResponse2:strResult = Translate ("Awaiting proxy response (2)"); break;
    case eConnectAwaitingProxyResponse3:strResult = Translate ("Awaiting proxy response (3)"); break;
    case eConnectAwaitingSSLHandshake:  strResult = Translate ("Performing TLS handshake"); break;
    case eConnectConnectedToMud:        strResult = Translate ("Open"); break;
    case eConnectDisconnecting:         strResult = Translate ("Disconnecting"); break;
    } // end of switch

  return strResult;
  }    // end of GetConnectionStatus


CString CMUSHclientDoc::GetSocketError (int nError)
  {

#ifdef _WIN32
  switch (nError)
    {
    case WSAEACCES:             return Translate ("Permission denied"); break;
    case WSAEADDRINUSE:         return Translate ("Address already in use"); break;
    case WSAEADDRNOTAVAIL:      return Translate ("Cannot assign requested address"); break;
    case WSAEAFNOSUPPORT :      return Translate ("Address family not supported by protocol family"); break;
    case WSAEALREADY :          return Translate ("Operation already in progress. "); break;
    case WSAECONNABORTED:       return Translate ("Software caused connection abort"); break;
    case WSAECONNREFUSED:       return Translate ("Connection refused"); break;
    case WSAECONNRESET:         return Translate ("Connection reset by peer"); break;
    case WSAEDESTADDRREQ:       return Translate ("Destination address required"); break;
    case WSAEFAULT:             return Translate ("Bad address"); break;
    case WSAEHOSTDOWN:          return Translate ("Host is down"); break;
    case WSAEHOSTUNREACH:       return Translate ("No route to host"); break;
    case WSAEINPROGRESS:        return Translate ("Operation now in progress"); break;
    case WSAEINTR:              return Translate ("Interrupted function call"); break;
    case WSAEINVAL:             return Translate ("Invalid argument"); break;
    case WSAEISCONN:            return Translate ("Socket is already connected"); break;
    case WSAEMFILE:             return Translate ("Too many open files"); break;
    case WSAEMSGSIZE:           return Translate ("Message too long"); break;
    case WSAENETDOWN:           return Translate ("Network is down"); break;
    case WSAENETRESET:          return Translate ("Network dropped connection on reset"); break;
    case WSAENETUNREACH:        return Translate ("Network is unreachable"); break;
    case WSAENOBUFS:            return Translate ("No buffer space available"); break;
    case WSAENOPROTOOPT :       return Translate ("Bad protocol option"); break;
    case WSAENOTCONN:           return Translate ("Socket is not connected"); break;
    case WSAENOTSOCK:           return Translate ("Socket operation on non-socket"); break;
    case WSAEOPNOTSUPP:         return Translate ("Operation not supported"); break;
    case WSAEPFNOSUPPORT:       return Translate ("Protocol family not supported"); break;
    case WSAEPROCLIM:           return Translate ("Too many processes"); break;
    case WSAEPROTONOSUPPORT:    return Translate ("Protocol not supported"); break;
    case WSAEPROTOTYPE:         return Translate ("Protocol wrong type for socket"); break;
    case WSAESHUTDOWN:          return Translate ("Cannot send after socket shutdown"); break;
    case WSAESOCKTNOSUPPORT:    return Translate ("Socket type not supported"); break;
    case WSAETIMEDOUT:          return Translate ("Connection timed out"); break;
    case WSAEWOULDBLOCK:        return Translate ("Resource temporarily unavailable"); break;
    case WSAHOST_NOT_FOUND:     return Translate ("Host not found"); break;
    case WSA_INVALID_HANDLE:    return Translate ("Specified event object handle is invalid"); break;
    case WSA_INVALID_PARAMETER: return Translate ("One or more parameters are invalid"); break;
    case WSAEINVALIDPROCTABLE:   return Translate ("Invalid procedure table from service provider"); break;
    case WSAEINVALIDPROVIDER:    return Translate ("Invalid service provider version number"); break;
    case WSA_IO_PENDING:        return Translate ("Overlapped operations will complete later"); break;
    case WSA_IO_INCOMPLETE:     return Translate ("Overlapped I/O event object not in signaled state"); break;
    case WSA_NOT_ENOUGH_MEMORY: return Translate ("Insufficient memory available"); break;
    case WSANOTINITIALISED:     return Translate ("Successful WSAStartup not yet performed"); break;
    case WSANO_DATA:            return Translate ("Valid name, no data record of requested type"); break;
    case WSANO_RECOVERY:        return Translate ("This is a non-recoverable error"); break;
    case WSAEPROVIDERFAILEDINIT: return Translate ("Unable to initialize a service provider"); break;
    case WSASYSCALLFAILURE:     return Translate ("System call failure"); break;
    case WSASYSNOTREADY:        return Translate ("Network subsystem is unavailable"); break;
    case WSATRY_AGAIN:          return Translate ("Non-authoritative host not found"); break;
    case WSAVERNOTSUPPORTED:    return Translate ("WINSOCK.DLL version out of range"); break;
    case WSAEDISCON:            return Translate ("Graceful shutdown in progress"); break;
    case WSA_OPERATION_ABORTED: return Translate ("Overlapped operation aborted"); break;
    } // end of switch
#else
  // On non-Windows, error codes are POSIX errno values; strerror handles them.
  const char * msg = strerror (nError);
  if (msg)
    return Translate (msg);
#endif

   return Translate ("Unknown error code");

  }   // end of CMUSHclientDoc::GetSocketError


void  CMUSHclientDoc::SendPacket (const char * lpBuf, const int nBufLen)
  {

#ifndef _WIN32

  // T2d: the port sends through the PAL transport (the append-and-it-drains
  // m_outstanding_data semantics, including the TLS WANT_* retry, live in
  // the adapter).  Bytes are counted here ONLY: the historical double-count
  // (here AND again in CWorldSocket::OnSend as each chunk drained) is
  // deliberately dropped on the port.

  if (m_pTransport == NULL)
    return;

  m_iOutputPacketCount++;

  if (m_bDebugIncomingPackets)
    Debug_Packets ("Sent ", lpBuf, nBufLen, m_iOutputPacketCount);

  m_pTransport->send (lpBuf, nBufLen);

  m_nBytesOut += nBufLen;

#else

  if (m_pSocket == NULL)
    return;

  m_iOutputPacketCount++;

  if (m_bDebugIncomingPackets)
    Debug_Packets ("Sent ", lpBuf, nBufLen, m_iOutputPacketCount);

  m_pSocket->m_outstanding_data.append (lpBuf, nBufLen);

  m_pSocket->OnSend (0);   // in case FD_WRITE message got lost, try to send again

  m_nBytesOut += nBufLen;

#endif // _WIN32

  } // end of CMUSHclientDoc::SendPacket

void  CMUSHclientDoc::SendPacket (const unsigned char * lpBuf, const int nBufLen)
  {
  SendPacket ((const char *) lpBuf, nBufLen);
  } // end of CMUSHclientDoc::SendPacket 


void CMUSHclientDoc::OnConnect(int nErrorCode)
  {
BOOL connected = nErrorCode == 0;

  TRACE ("CMUSHclientDoc::OnConnect\n");

  if (!connected)
    {

    TRACE ("However, not connected.\n");
    m_bDisconnectOK = true;     // don't want reconnect
    int iOldPhase = m_iConnectPhase;
    m_iConnectPhase = eConnectNotConnected;

    App.m_bUpdateActivity = TRUE;   // new activity!

    UpdateAllViews (NULL);

    if (App.m_bNotifyIfCannotConnect)
      {
      CString strMsg;

      strMsg = TFormat ("Unable to connect to \"%s\", code = %i (%s)\n\n"
                      "Error occurred during phase: %s",
                      (const char *) m_mush_name,
                      nErrorCode,
                      (LPCTSTR) GetSocketError (nErrorCode),
                      (const char *) GetConnectionStatus (iOldPhase));

      // i18n TODO - fix this up
      switch (iOldPhase)
        {
        case eConnectConnectingToMud:      
          strMsg += ": ";
          strMsg += inet_ntoa (m_sockAddr.sin_addr);
          strMsg += CFormat (", port %d", m_port);
          break; 
        case eConnectConnectingToProxy:    
          strMsg += ": ";
          strMsg += inet_ntoa (m_ProxyAddr.sin_addr);
          strMsg += CFormat (", port %d", m_iProxyServerPort);
          break; 
        } // end of switch


      if (App.m_bErrorNotificationToOutputWindow)
        {
        Note (strMsg);
        Note ("");
        Note (Translate ("For assistance with connection problems see: "));
#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
        Tell ("   ");
        Hyperlink (FORUM_URL "?id=8369", 
                   FORUM_URL "?id=8369", 
                   Translate ("How to resolve network connection problems"), 
                   "deepskyblue", "black", TRUE);
#else
        Note (FORUM_URL "?id=8369");   // headless: no hyperlink rendering
#endif
        Note ("");
        Note ("");

        }
      else
        {
        strMsg += "\n\n";
        strMsg += Translate ("For assistance with connection problems see: ");
        strMsg += FORUM_URL "?id=8369";
        strMsg +=  "\n\n";
        strMsg += Translate ("This message can be suppressed, or displayed in the main window.");
        strMsg +=  "\n";
        strMsg += Translate ("See the File menu -> Global Preferences -> General to do this.");

        UMessageBox (strMsg);
        }
      } // end of notification wanted

    return;

    }


  // we have connected, and have no proxy server, so get on with it
  if (m_iSocksProcessing == eProxyServerNone)
    {
    if (m_bUseSSL)
      {
      StartSSLHandshake ();
      return;
      }
    ConnectionEstablished ();
    return;
    }

  // we have connected to the proxy server - tell them who we really want to talk to

  TRACE ("\nSending SOCKS authentication request\n");

  if (m_iSocksProcessing == eProxyServerSocks4)
    {
    m_iConnectPhase = eConnectAwaitingProxyResponse3;  // no authentication query

    // 4 = SOCKS version
    // 1 = connect 
    // port in network order
    // address in network order
    // xxx = username
    // 0 = terminating byte
    // exactly 4 bytes on every platform: unsigned long is 8 bytes on LP64
    // (macOS/Linux), which would put a malformed 8-byte address on the wire;
    // on Win32 unsigned int is the same 4 bytes this always sent
    unsigned int address = m_sockAddr.sin_addr.s_addr;  // MUD address
    short port = m_sockAddr.sin_port;       // MUD port
    unsigned char socks_address [3 + 255 + sizeof address + sizeof port] = { 4, 1 };
    int socks_address_length = 2;  // ie. the 2 bytes above
    // port
    memcpy (&socks_address [socks_address_length], &port, sizeof port);
    socks_address_length += sizeof port;     // ie. 2
    // address
    memcpy (&socks_address [socks_address_length], &address, sizeof address);
    socks_address_length += sizeof address;     // ie. 4
    // username, might be empty
    if (m_strProxyUserName.GetLength () <= 255)
      {
      memcpy (&socks_address [socks_address_length], 
              (LPCTSTR) m_strProxyUserName, 
              m_strProxyUserName.GetLength ());
      // adjust index to bypass name
      socks_address_length += m_strProxyUserName.GetLength ();   
      } // end of username in range 0 to 255
    // name terminator
    socks_address [socks_address_length++] = 0;
    // send it
    SendPacket (socks_address, socks_address_length);
    }  // end SOCKS 4
  else
    { // SOCKS 5
    m_iConnectPhase = eConnectAwaitingProxyResponse1;
    if (m_strProxyUserName.IsEmpty ())
      {
      // 5 = SOCKS version
      // 1 = 1 method
      // 0 = no authentication
      unsigned char socks_identifier [3] = { 5, 1, 0 };
      SendPacket (socks_identifier, sizeof socks_identifier);
      }
    else
      {
      // 5 = SOCKS version
      // 1 = 2 methods
      // 0 = no authentication
      // 2 = username/password
      unsigned char socks_identifier [4] = { 5, 2, 0, 2 };
      SendPacket (socks_identifier, sizeof socks_identifier);
      }
    } // end SOCKS 5

  
  
  } // end of OnConnect


// we come here when we know (both) IP addresses (mud and proxy)
// and are ready to try to connect  

void CMUSHclientDoc::InitiateConnection (void)
  {

#ifndef _WIN32

  // T2d: connect through the PAL transport.  Every outcome arrives via the
  // events bridge → OnConnect: immediate success is deferred to the next
  // pump tick (never re-entrant), an immediate LOCAL failure is delivered
  // synchronously from connect() and lands in OnConnect's failure path —
  // the same UX the inline error handling below produces.

  // just so we don't get ridiculous times shown if we don't connect
  m_tConnectTime = CTime::GetCurrentTime();

  if (m_pTransport == NULL)
    return;     // no transport — ConnectSocket was never run

  pal::TransportOptions options;
  options.keepalives = m_bSendKeepAlives != 0;

  if (m_iSocksProcessing == eProxyServerNone)
    {
    m_iConnectPhase = eConnectConnectingToMud;
    m_pTransport->connect (m_sockAddr.sin_addr.s_addr,
                           (unsigned short) m_port, options);
    }
  else
    {
    m_iConnectPhase = eConnectConnectingToProxy;
    m_pTransport->connect (m_ProxyAddr.sin_addr.s_addr,
                           (unsigned short) m_iProxyServerPort, options);
    }

  return;

#else

  BOOL connected;

  // just so we don't get ridiculous times shown if we don't connect
  m_tConnectTime = CTime::GetCurrentTime();

// initiate asynchronous connect

  if (m_iSocksProcessing == eProxyServerNone)
    {
    m_iConnectPhase = eConnectConnectingToMud;
    connected = m_pSocket->Connect((SOCKADDR*)&m_sockAddr, sizeof(m_sockAddr));
    }
  else
    {
    m_iConnectPhase = eConnectConnectingToProxy;
    connected = m_pSocket->Connect((SOCKADDR*)&m_ProxyAddr, sizeof(m_ProxyAddr));
    }

  if (connected)
    {
    OnConnect (0);    // we have connected already! Do logon of character etc.
    return;
    }

// if error code is "would block" then it will finish later

  int iStatus = GetLastError ();

  if (iStatus == WSAEWOULDBLOCK)
    return;

// tell them connect failed

  if (App.m_bNotifyIfCannotConnect)
    {
    CString strMsg;

    strMsg = TFormat ("Unable to connect to \"%s\", code = %i (%s)\n\n"
                    "Error occurred during phase: %s",
                    (const char *) m_mush_name,
                    iStatus,
                    (LPCTSTR) GetSocketError (iStatus),
                    (const char *) GetConnectionStatus (m_iConnectPhase));

    // i18n TODO - fix this:
    switch (m_iConnectPhase)
      {
      case eConnectConnectingToMud:      
        strMsg += ": ";
        strMsg += inet_ntoa (m_sockAddr.sin_addr);
        strMsg += CFormat (", port %d", m_port);
        break; 
      case eConnectConnectingToProxy:    
        strMsg += ": ";
        strMsg += inet_ntoa (m_ProxyAddr.sin_addr);
        strMsg += CFormat (", port %d", m_iProxyServerPort);
        break; 
      } // end of switch

    if (App.m_bErrorNotificationToOutputWindow)
      Note (strMsg);
    else
      UMessageBox (strMsg);
    }     // end of notification wanted

  TRACE ("CCMUSHclientDoc::InitiateConnection - cannot connect\n");

  m_bDisconnectOK = true;     // don't want reconnect
  m_iConnectPhase = eConnectNotConnected;

  App.m_bUpdateActivity = TRUE;   // new activity!

#endif // _WIN32

  } // end of CMUSHclientDoc::InitiateConnection


// ── Async DNS (single-sourced, T3a) ─────────────────────────────────────────
// Windows resolves via WSAAsyncGetHostByName (HostNameResolved, doc.cpp);
// the port resolves through the transport (LookupHostNameComplete below).

bool CMUSHclientDoc::LookupHostName (LPCTSTR sName)
  {
  delete [] m_pGetHostStruct;   // delete buffer just in case
  m_pGetHostStruct = new char [MAXGETHOSTSTRUCT];

  if (!m_pGetHostStruct)
    {
    TMessageBox ("Unable to allocate memory for host name lookup");
    return true;
    }

#ifdef _WIN32
  if (Frame.GetSafeHwnd ())   // forget it if we don't have a window yet
    m_hNameLookup = WSAAsyncGetHostByName (Frame.GetSafeHwnd (),
                                           WM_USER_HOST_NAME_RESOLVED,
                                           sName,
                                           m_pGetHostStruct,
                                           MAXGETHOSTSTRUCT);

  if (!m_hNameLookup)
    {
    CString strMsg;

    strMsg = TFormat ("Unable to initiate host name lookup for \"%s\"",
                   sName);
    UMessageBox (strMsg);
    delete [] m_pGetHostStruct;
    m_pGetHostStruct = NULL;
    m_iConnectPhase = eConnectNotConnected;

    App.m_bUpdateActivity = TRUE;   // new activity!

    }

  return false;    // we are waiting for host name lookup to finish

#else  // !_WIN32

  // Non-Windows: async DNS through the PAL transport (T2d) — its getaddrinfo
  // thread posts the completion back to the main thread, where the events
  // bridge calls LookupHostNameComplete().  The connect phase is the resolve
  // token: the shipping staleness guard drops superseded completions.
  {
    delete [] m_pGetHostStruct;
    m_pGetHostStruct = NULL;

    if (m_pTransport)
      {
      m_pTransport->resolve ((const char *) sName, m_iConnectPhase);
      return false;
      }

    // Synchronous fallback (defensive — ConnectSocket creates the transport
    // before any lookup can happen).
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct in_addr resolved{};
    bool ok = (::getaddrinfo (sName, nullptr, &hints, &res) == 0 && res);
    if (ok)
      {
      resolved = ((struct sockaddr_in *) res->ai_addr)->sin_addr;
      ::freeaddrinfo (res);
      }

    LookupHostNameComplete (resolved, m_iConnectPhase, ok);
    return !ok;   // true = error, caller should abort
  }

#endif  // _WIN32


  } // end of CMUSHclientDoc::LookupHostName


#ifndef _WIN32
// Called on the main thread when an async (or synchronous fallback) DNS
// lookup finishes.  addr is the resolved address; phase is the connect phase
// that was active when LookupHostName was called; ok is false on failure.
void CMUSHclientDoc::LookupHostNameComplete (struct in_addr addr, int phase, bool ok)
  {
  // Guard: the user may have disconnected while the DNS thread was running.
  if (m_iConnectPhase != phase)
    return;

  if (!ok)
    {
    m_bDisconnectOK = true;
    m_iConnectPhase = eConnectNotConnected;
    App.m_bUpdateActivity = TRUE;
    UpdateAllViews (NULL);

    if (App.m_bNotifyIfCannotConnect)
      {
      CString strWhich = (phase == eConnectProxyNameLookup)
                         ? m_strProxyServerName
                         : m_server;
      CString strMsg = TFormat ("Unable to resolve host name for \"%s\"",
                                (LPCTSTR) strWhich);
      if (App.m_bErrorNotificationToOutputWindow)
        Note (strMsg);
      else
        UMessageBox (strMsg);
      }
    return;
    }

  if (phase == eConnectMudNameLookup)
    m_sockAddr.sin_addr = addr;
  else if (phase == eConnectProxyNameLookup)
    m_ProxyAddr.sin_addr = addr;
  else
    {
    TMessageBox ("Unexpected phase in LookupHostNameComplete");
    return;
    }

  // If we just resolved the MUD address and a proxy is configured,
  // check whether the proxy address also needs resolving.
  if (phase == eConnectMudNameLookup &&
      m_iSocksProcessing != eProxyServerNone)
    {
    if (m_ProxyAddr.sin_family != AF_INET ||
        m_ProxyAddr.sin_addr.s_addr == INADDR_NONE)
      {
      m_ProxyAddr.sin_family         = AF_INET;
      m_ProxyAddr.sin_addr.s_addr    = inet_addr (m_strProxyServerName);
      if (m_ProxyAddr.sin_addr.s_addr == INADDR_NONE)
        {
        m_iConnectPhase = eConnectProxyNameLookup;
        LookupHostName (m_strProxyServerName);
        return;
        }
      }
    }

  InitiateConnection ();
  } // end of CMUSHclientDoc::LookupHostNameComplete
#endif  // !_WIN32


// ═════════════════════════════════════════════════════════════════════════
// ITransport (T3a): the transport events bridge + engine TLS policy — the
// port's single connection substrate, compiled into BOTH builds so mushcore
// drives the same transport code the product ships.  Windows keeps
// CWorldSocket (doc.cpp) until T5.
// ═════════════════════════════════════════════════════════════════════════
#ifndef _WIN32

// ── CDocTransportEvents ──────────────────────────────────────────────────────
// The doc-side pal::ITransportEvents bridge (ITransport migration step T2d):
// maps transport events onto the engine's connect-phase machine.  Delivery
// contract (pal/itransport.h): main thread, never re-entrant, and any handler
// may destroy the transport mid-event — so handlers that run scripts (which
// can Disconnect and even reconnect) are safe.  One bridge per doc, created
// on first ConnectSocket and destroyed with the doc; it must outlive every
// transport, which holds a reference to it.

class CDocTransportEvents : public pal::ITransportEvents
  {
  public:
    explicit CDocTransportEvents (CMUSHclientDoc * pDoc) : m_pDoc (pDoc) {}

    void onResolved (pal::ResolveToken token, unsigned int addrIPv4, bool ok) override
      {
      // token is the connect phase at resolve time — the staleness guard
      // inside LookupHostNameComplete drops superseded completions
      struct in_addr addr;
      addr.s_addr = addrIPv4;
      m_pDoc->LookupHostNameComplete (addr, token, ok);
      }

    void onConnect (int err) override
      {
      m_pDoc->OnConnect (err);
      }

    void onBytes (const char * data, size_t n) override
      {
      CMUSHclientDoc * pDoc = m_pDoc;
      // the adapter's scratch buffer is ours to mutate in place (the proxy
      // parser memmoves consumed bytes down) — same as ReceiveMsg's stack
      // buffer on the Windows path
      pDoc->OnBytesReceived (const_cast<char *> (data), (int) n);
      // push new lines to the GUI output pane (the CWorldSocket::OnReceive
      // parity call — the MFC invalidation mechanism is absent off-Windows)
      pDoc->UpdateAllViews (NULL);
      }

    void onWritable () override
      {
      // nothing to do: the adapter owns the buffered send queue (the old
      // FD_WRITE flush) and the TLS handshake stepping
      }

    void onClosed (int err) override
      {
      CMUSHclientDoc * pDoc = m_pDoc;
      // the closed transport is done: destroy it BEFORE the bookkeeping so a
      // disconnect-script reconnect (ConnectSocket) cannot collide with it
      // (destroying the transport inside its own event is contract-supported)
      delete pDoc->m_pTransport;
      pDoc->m_pTransport = NULL;
      pDoc->ConnectionClosed (err);
      }

    void onTlsEstablished (const char * version, const char * cipher,
                           const char * verifyFailure) override
      {
      m_pDoc->OnTlsEstablished (version, cipher, verifyFailure);
      }

    void onTlsFailed (pal::TlsFailure kind, const char * reason) override
      {
      m_pDoc->OnTlsFailed (kind == pal::TlsFailure::CertVerify, reason);
      }

  private:
    CMUSHclientDoc * m_pDoc;
  };

// Create a fresh transport for a new connection attempt (destroying any old
// one), lazily creating the per-doc events bridge on first use.  This is the
// single place the concrete adapter is chosen (T5: Windows picks its native
// adapter here).
void CMUSHclientDoc::CreateTransport (void)
  {
  delete m_pTransport;
  m_pTransport = NULL;

  if (!m_pTransportEvents)
    m_pTransportEvents = new CDocTransportEvents (this);

  m_pTransport = pal::CreatePosixTransport (*m_pTransportEvents);
  }

// Destroy the transport + its events bridge (doc destructor path; here
// because both types are complete in this TU).
void CMUSHclientDoc::TeardownTransport (void)
  {
  if (m_pTransport)
    {
    m_pTransport->close ();
    delete m_pTransport;
    m_pTransport = NULL;
    }
  delete m_pTransportEvents;
  m_pTransportEvents = NULL;
  }


// ── ITransport TLS outcomes (T2d) ────────────────────────────────────────────
// The transport adapter owns the TLS session on the port; these are the
// engine-policy halves of ContinueSSLHandshake above: the Notes, the phase
// change, and the deferred consent/fallback prompt
// (WM_USER_SSL_FALLBACK_PROMPT — handled by the frame, which reconnects on
// user consent).

void CMUSHclientDoc::OnTlsEstablished (const char * version, const char * cipher,
                                       const char * verifyFailure)
  {

  if (verifyFailure && *verifyFailure)
    {
    // user already opted to proceed despite the failure — connect, but warn
    CString strMsg;
    strMsg.Format ("--- WARNING: proceeding with UNVERIFIED TLS certificate for \"%s\": %s ---",
                   (LPCTSTR) m_server, verifyFailure);
    Note (strMsg);
    }

  // the OpenSSL objects live inside the adapter now, but "TLS is up" is
  // engine state (MTTS advertises it — telnet_phases.cpp); SSLCleanup
  // (via ConnectionClosed / OnConnectionDisconnect) resets it
  m_bSSL_Connected = true;

  CString strInfo;
  strInfo.Format ("--- Connected via %s (%s) ---", version, cipher);
  Note (strInfo);

  m_iConnectPhase = eConnectConnectedToMud;
  ConnectionEstablished ();

  }   // end of CMUSHclientDoc::OnTlsEstablished


void CMUSHclientDoc::OnTlsFailed (bool bCertFailure, const char * reason)
  {

  CString strMsg;
  if (bCertFailure)
    // Block: the certificate could not be verified and the user has not
    // (yet) chosen to accept it.  The deferred prompt offers "connect
    // anyway" — still over TLS, but accepting this unverifiable certificate
    // for the session (m_bSSLAllowInvalidCert) — and reconnects.
    strMsg.Format ("--- TLS certificate for \"%s\" could NOT be verified: %s ---",
                   (LPCTSTR) m_server, reason);
  else
    strMsg.Format ("--- TLS handshake failed: %s ---", reason);
  Note (strMsg);

  // save error for deferred prompt
  m_strSSLLastError = reason;

  // the adapter has already torn the TLS session down; this closes the
  // transport and runs the disconnect bookkeeping
  OnConnectionDisconnect ();

  // defer the prompt via PostMessage so we're not inside a transport event
  Frame.PostMessage (WM_USER_SSL_FALLBACK_PROMPT, (WPARAM) this,
                     (LPARAM) (bCertFailure ? 1 : 0));

  }   // end of CMUSHclientDoc::OnTlsFailed

#endif // !_WIN32
