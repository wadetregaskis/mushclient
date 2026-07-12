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
#else
#include "platform/doc_stub.h"
// add ::Replace's default for the stub-only build (stdafx.h owns it in the
// full build; doc_stub.h deliberately declares without it - see there)
CString Replace (const CString strSource, const CString strTarget,
                 const CString strReplacement, const bool bAll = true);
#endif

#include "speedwalk.h"   // shared ExpandSpeedwalk / ReverseSpeedwalkString

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
