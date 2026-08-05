// doc_shared.cpp — CMUSHclientDoc methods that are pure logic (no view/GUI
// dependency) and were previously reimplemented twice: in doc.cpp (the GUI
// engine) and doc_portable.cpp (the headless test engine). Compiling the single
// canonical implementation into BOTH mushapp and mushcore means it cannot drift
// between the builds (a real bug source: GetStyleRGB once bolded the wrong half
// of an inverse pair, speedwalk lost its C/O/L/K codes). Mirrors ansi.cpp /
// speedwalk.cpp, which likewise define shared CMUSHclientDoc methods.

#include "stdafx.h"
#include "MUSHclient.h"
#include "mainfrm.h"            // Frame (ConnectionClosed's status/button updates)
#include "doc.h"
#include "MUSHview.h"           // CMUSHView (DisplayMsg/StartNewLine view-notify loops)
#include "scripting/errors.h"   // eOK / eBadParameter
#include "pal/style_colour.h"    // pal::StylePalette / resolveStyleColours
#include "dialogs/password.h"   // CPasswordDialog (ConnectionEstablished, full build)
#ifdef _WIN32
#include <ws2tcpip.h>           // getaddrinfo (LookupHostName's sync fallback —
                                // newly MSVC-compiled at T5; stdafx has only winsock2)
#endif

#include "speedwalk.h"   // shared ExpandSpeedwalk / ReverseSpeedwalkString
#include "scripting/scripting.h"   // CScriptEngine (ExecuteScript delegates to it)

// The transport adapter — the ONE per-platform choice this TU makes (T5).
#ifdef _WIN32
#include "pal/win_transport.h"          // native CAsyncSocket/OpenSSL adapter
#else
#include "platform/posix_transport.h"   // POSIX pump/getaddrinfo/OpenSSL adapter
#endif

#if defined(OPENSSL_FOUND)
#include <openssl/ssl.h>
#endif

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

// ── GetStyleRGB ─────────────────────────────────────────────────────────────
// The arithmetic now lives in pal/style_colour.h so the VIEW can re-run it with
// a palette of its own -- see that header for why.  This is still the only
// caller-facing entry point, and still the only body: nothing was duplicated.
//
// The pal header cannot include OtherTypes.h (it is pulled in by wx TUs that
// must not see the MFC shim), so it mirrors the six style bits.  Assert them
// here, where both definitions are visible, so the copy cannot drift.
static_assert((unsigned short) pal::kStyleHilite     == (unsigned short) HILITE,
              "pal::kStyleHilite must match HILITE");
static_assert((unsigned short) pal::kStyleInverse    == (unsigned short) INVERSE,
              "pal::kStyleInverse must match INVERSE");
static_assert((unsigned short) pal::kStyleColourType == (unsigned short) COLOURTYPE,
              "pal::kStyleColourType must match COLOURTYPE");
static_assert((unsigned short) pal::kColourAnsi      == (unsigned short) COLOUR_ANSI,
              "pal::kColourAnsi must match COLOUR_ANSI");
static_assert((unsigned short) pal::kColourCustom    == (unsigned short) COLOUR_CUSTOM,
              "pal::kColourCustom must match COLOUR_CUSTOM");
static_assert((unsigned short) pal::kColourRgb       == (unsigned short) COLOUR_RGB,
              "pal::kColourRgb must match COLOUR_RGB");
// ...and that the custom tables really are 16 entries, which the struct fixes.
static_assert(MAX_CUSTOM == 16, "pal::StylePalette sizes the custom tables at 16");
// ...and that the palette word really is COLORREF-wide.  Getting this wrong is
// silent: a (const unsigned long *) cast over a 32-bit COLORREF table reads
// two entries per index and yields garbage colours only for xterm-256 runs.
static_assert(sizeof (((pal::StylePalette *) 0)->normal [0]) == sizeof (COLORREF),
              "pal::StylePalette entries must be COLORREF-wide");

// Snapshot the document's palette in the form the resolver wants.
void CMUSHclientDoc::GetStylePalette (pal::StylePalette & out) const
  {
  for (int i = 0; i < 8; i++)
    {
    out.normal [i] = m_normalcolour [i];
    out.bold   [i] = m_boldcolour   [i];
    }
  for (int i = 0; i < MAX_CUSTOM; i++)
    {
    out.customText [i] = m_customtext [i];
    out.customBack [i] = m_customback [i];
    }
  out.alternativeInverse = m_bAlternativeInverse != 0;
  }  // end of CMUSHclientDoc::GetStylePalette

void CMUSHclientDoc::GetStyleRGB (CStyle * pStyle, COLORREF & colour1, COLORREF & colour2) const
  {
  pal::StylePalette palette;
  GetStylePalette (palette);

  unsigned int c1 = 0, c2 = 0;
  pal::resolveStyleColours ((unsigned short) (pStyle->iFlags & STYLE_BITS),
                            pStyle->iForeColour, pStyle->iBackColour,
                            palette, (const unsigned int *) xterm_256_colours,
                            c1, c2);
  colour1 = (COLORREF) c1;
  colour2 = (COLORREF) c2;
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

// ── mapper: typed-direction auto-mapping (P-Stage3 de-drift) ────────────────
// AddToMap moved verbatim from Mapping.cpp so BOTH builds share one body
// (the headless stub was a silent no-op, disabling world.SetMapping tracking).
// The view half (DrawMappingStatusLine → Frame status bar) stays platform-
// specific: Mapping.cpp for the products, a no-op headless.
void CMUSHclientDoc::AddToMap (CString str)
  {
  MapDirectionsIterator i;

  str.MakeLower ();
  str.TrimLeft ();

  int iNewline;

  while ((iNewline = str.Find (ENDLINE)) != -1)
    {
    CString strDirection = str.Left (iNewline);

    // see if they have sent a "direction"
    i = MapDirectionsMap.find ((LPCTSTR) strDirection);

    // if not, give up
    if (i != MapDirectionsMap.end ())
      {

      // Remove reverses ...

      // If they have gone, for example, N then S, then the net result 
      // is that they haven't gone anywhere, so remove the N rather than adding the S
      
      // We let them disable this because sometimes rooms are not symmetrical, and
      // you might go North, and then South, and *not* end up where you were.


      if (m_bRemoveMapReverses &&
          !m_strMapList.IsEmpty ())
        {
        CString strComment = m_strMapList.GetTail ();

        // if last thing was comment get it and try second last thing
        if (strComment.GetLength () >= 2 &&
            strComment.Left (1) == "{" &&
            strComment.Right (1) == "}" &&
            m_strMapList.GetCount () > 1)
              m_strMapList.RemoveTail ();
        else
          strComment.Empty ();
        
        // if this direction had a non-standard reverse, extract that
        CString strLast = m_strMapList.GetTail ();

        int iSlashPos = strLast.Find ("/");
        if (iSlashPos != -1)
          {
          strLast = strLast.Mid (iSlashPos + 1);  // this is the reverse direction
        
          MapDirectionsIterator i2;

          i2 = MapDirectionsMap.find ((LPCTSTR) strLast);   // convert "west" to "w"

          // if this is the reverse direction, remove the earlier one rather than
          // adding this one
          if (i2 != MapDirectionsMap.end () && 
              i2->second.m_sDirectionToLog == i->second.m_sDirectionToLog)
           m_strMapList.RemoveTail ();
          else
            m_strMapList.AddTail (i->second.m_sDirectionToLog.c_str ());
          }
         else if (m_strMapList.GetTail () == i->second.m_sReverseDirection.c_str ())
             m_strMapList.RemoveTail ();
         else
           {
           // different direction? Put comment back if we had one
           if (!strComment.IsEmpty ())
            m_strMapList.AddTail (strComment);
           // and add the new direction
           m_strMapList.AddTail (i->second.m_sDirectionToLog.c_str ());
           }
        }
      else
        m_strMapList.AddTail (i->second.m_sDirectionToLog.c_str ());

      // update status line
      DrawMappingStatusLine ();
      } // end of finding the direction

    str = str.Mid (iNewline + strlen (ENDLINE));
    }   // end of breaking the text into lines

  }   // end of CMUSHclientDoc::AddToMap



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

  // "n/m lines (x% full)" makes no sense with no limit set, and the percentage
  // would divide by zero.
  if (m_maxlines > 0)
    strInfo = TFormat ("--- Output buffer has %i/%i line%s in it (%.1f%% full).",
                  m_LineList.GetCount (),
                  m_maxlines,
                  (m_LineList.GetCount ()) == 1 ? "" : "s",
                  (double) m_LineList.GetCount () / (double) m_maxlines * 100.0
                  );
  else
    strInfo = TFormat ("--- Output buffer has %i line%s in it (no limit set).",
                  m_LineList.GetCount (),
                  (m_LineList.GetCount ()) == 1 ? "" : "s"
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

// (the tcp_keepalive / SIO_* block moved into pal/win_transport.cpp at T5 —
// the adapter owns the keepalive mechanics on every platform)


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

// The connection runs through the PAL transport on EVERY platform (T5):
// the adapter (native CAsyncSocket/WSAAsyncSelect on Windows, the POSIX
// pump elsewhere) creates the fd at connect time and applies the keepalive
// policy itself (TransportOptions carries on/off; the 120 s / 6 s / 6-probe
// timing is fixed engine policy inside it).

  CreateTransport ();

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
         // next_out points into the buffer being reallocated — remember the
         // offset and re-point it afterwards (realloc may move the block;
         // leaving next_out at the old address writes into freed memory)
         size_t iOutputOffset = m_zCompress.next_out - m_CompressOutput;
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

         m_zCompress.next_out = m_CompressOutput + iOutputOffset;
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

// (any host name lookup in progress is cancelled by the transport below)

  App.m_bUpdateActivity = TRUE;   // new activity!

  MXP_Off (true);   // turn off MXP now

// clean up SSL before closing the socket

  SSLCleanup ();

// close the socket

  // tear down the transport (the socket, every platform since T5): cancel
  // any DNS lookup in flight, shut the stream down, run the disconnect
  // bookkeeping, destroy — the CWorldSocket shutdown/OnClose/delete sequence.
  if (m_pTransport)
    {
    m_pTransport->cancelResolve ();
    m_pTransport->close ();
    ConnectionClosed (0);
    delete m_pTransport;
    m_pTransport = NULL;
    }

  m_iConnectPhase = eConnectNotConnected;

  }


// StartSSLHandshake (T3c; all platforms since T5): the transport owns the
// WHOLE TLS session (context, non-blocking handshake stepping, verification,
// encrypted I/O, the decrypted-bytes drain).  Policy stays in the engine:
// the outcome lands in OnTlsEstablished / OnTlsFailed via the events bridge.
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


  } // end of CMUSHclientDoc::InitiateConnection


// ── Async DNS (single-sourced, T3a; transport-only since T5) ────────────────
// Every platform resolves through the transport; the completion lands in
// LookupHostNameComplete below.

bool CMUSHclientDoc::LookupHostName (LPCTSTR sName)
  {

  // Async DNS through the PAL transport (every platform since T5 — the
  // Windows adapter keeps the WSAAsyncGetHostByName mechanics the doc used,
  // delivered to its own window).  The completion arrives via the events
  // bridge -> LookupHostNameComplete().  The connect phase is the resolve
  // token: the shipping staleness guard drops superseded completions.
  {

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

  } // end of CMUSHclientDoc::LookupHostName


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


// ═════════════════════════════════════════════════════════════════════════
// ITransport (T3a; all platforms since T5): the transport events bridge +
// engine TLS policy — the single connection substrate for the product, the
// headless engine, AND Windows (CWorldSocket is gone).
// ═════════════════════════════════════════════════════════════════════════

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

#ifdef _WIN32
  m_pTransport = pal::CreateWinTransport (*m_pTransportEvents);
#else
  m_pTransport = pal::CreatePosixTransport (*m_pTransportEvents);
#endif
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


// ── ExecuteScript — invoke a resolved dispatch ID on the script engine ────────
// Single-sourced (was: scripting/scripting.cpp on Windows/mushapp, and a
// `return false` STUB in doc_portable.cpp for mushcore).  The stub meant the
// headless test engine silently no-op'd every world callback (connect/disconnect,
// etc.) and send-to-script alias/timer — even though CScriptEngine::Execute is
// fully functional in mushcore (scripting/scriptengine.cpp delegates to
// ExecuteLua for the embedded engines).  Adopting the real body brings mushcore
// to parity with the product; behaviour is unchanged where the dispid is
// unresolved (DISPID_UNKNOWN → false, exactly as the stub returned).
bool CMUSHclientDoc::ExecuteScript (DISPID & dispid,
                                    LPCTSTR szProcedure,
                                    const unsigned short iReason,
                                    LPCTSTR szType,
                                    LPCTSTR szReason,
                                    DISPPARAMS & params,
                                    long & nInvocationCount,
                                    COleVariant * result)
  {
  if (GetScriptEngine () && dispid != DISPID_UNKNOWN)
    return GetScriptEngine ()->Execute (dispid, szProcedure, iReason, szType, szReason,
                                        params, nInvocationCount, result);
  else
    return false;
  }   // end of CMUSHclientDoc::ExecuteScript


// ═══ The send family (campaign-b display step 5) ═══════════════════════════
// SendMsg / DoSendMsg (from doc.cpp) and ShowQueuedCommands (from
// scripting/methods/methods_speedwalks.cpp), relocated verbatim so all three
// builds share one send path.  Replaces the simplified doc_portable.cpp twin
// (which had no queueing, no spam prevention, no logging/mapper hooks, no IAC
// doubling, and echoed before OnPluginSent instead of after).
//
// Headless caveat: with m_iSpeedWalkDelay > 0, queued commands never drain in
// mushcore (the queue is emptied by CTimerWnd on Windows / the wx timer in the
// GUI, neither of which exists headless).  Benign at the default delay of 0 —
// everything sends immediately — but a test that sets a delay must drain
// m_QueuedCommandsList itself.

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


void CMUSHclientDoc::ShowQueuedCommands (void)
  {

  // show previous status line
  if (m_QueuedCommandsList.IsEmpty ())
    {
    ShowStatusLine (true);    // show it now
    return;    
    }

  CString strQueued = "Queued: ";
  const int MAX_SHOWN = 50;

  CString str;
  CString strLastDir;

  int iCount = 0;
  POSITION pos;

  for (pos = m_QueuedCommandsList.GetHeadPosition (); 
      pos && strQueued.GetLength () < MAX_SHOWN; )
    {
    // get next direction from list
    str = m_QueuedCommandsList.GetNext (pos).Mid (1);
    
    // empty lines look a bit silly
    if (str.IsEmpty ())
      continue;

    // if same as before, count them
    if (str == strLastDir)
      iCount++;
    else
      {
      // add to string
      // if direction is not a single char (eg. ne) then we must put it in brackets
      if (strLastDir.GetLength () > 1)
        strLastDir = "(" + strLastDir + ")";
      if (iCount == 1)
        strQueued += strLastDir + " ";
      else if (iCount > 1)
        strQueued += CFormat ("%i%s ", iCount, (LPCTSTR) strLastDir);

      strLastDir = str;
      iCount = 1;
      }   // end of a direction change                                

    }   // end of processing each command

  // output final command
  // if direction is not a single char (eg. ne) then we must put it in brackets
  if (strLastDir.GetLength () > 1)
    strLastDir = "(" + strLastDir + ")";
  if (iCount == 1)
    strQueued += strLastDir + " ";
  else if (iCount > 1)
    strQueued += CFormat ("%i%s ", iCount, (LPCTSTR) strLastDir);

  if (pos)
    strQueued += " ...";

  Frame.SetStatusMessageNow (strQueued);
  m_tStatusDisplayed = CTime::GetCurrentTime ();
  }    // end of CMUSHclientDoc::ShowQueuedCommands



// ═══ The note family's shared pieces (campaign-b display step 6) ════════════
// SetNewLineColour + OutputOutstandingLines relocated verbatim from doc.cpp;
// the note methods themselves live in scripting/methods/methods_noting.cpp,
// which mushcore now compiles too.  The Note(CString) delegator is declared
// only off-Windows (doc.h guards it #ifndef _WIN32).

#ifndef _WIN32
void CMUSHclientDoc::Note(const CString& s)
{
    Note((LPCTSTR) s);
}
#endif // !_WIN32

void CMUSHclientDoc::OutputOutstandingLines (void)
  {

  // minimal work ...
  if (m_OutstandingLines.empty ())
    return;

  // Move the list out BEFORE iterating.  Tell() below can re-enter this
  // function (Tell -> forced line change -> StartNewLine ->
  // ProcessPreviousLine -> OutputOutstandingLines); a re-entrant
  // m_OutstandingLines.clear() would then invalidate this loop's iterator
  // (use-after-free).  Swapping into a local empties the member up front, so
  // any re-entrant call sees it empty and returns immediately, and the loop
  // walks the detached copy — immune to re-entrant mutation.
  list<CPaneStyle> outstanding;
  outstanding.swap (m_OutstandingLines);

  // save old colours
  bool bOldNotesInRGB = m_bNotesInRGB;
  COLORREF iOldNoteColourFore = m_iNoteColourFore;
  COLORREF iOldNoteColourBack = m_iNoteColourBack;
  unsigned short  iOldNoteStyle = m_iNoteStyle;

  m_bNotesInRGB = true;

  // output saved lines

  list<CPaneStyle>::iterator it;

  for (it = outstanding.begin (); it != outstanding.end (); it++)
    {
    m_iNoteColourFore = it->m_cText;
    m_iNoteColourBack = it->m_cBack;
    m_iNoteStyle = it->m_iStyle;
    Tell (it->m_sText.c_str ());
    }

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



// ═══ The core display pipeline (campaign-b display step 7 — THE FLIP) ═══════
// DisplayMsg / StartNewLine (+_KeepPreviousStyle) / AddToLine / AddStyle (both
// overloads) / RemoveChunk / SendLineToPlugin, relocated verbatim from doc.cpp.
// One engine on every build now: mushcore's hand-written display twins (the
// out-of-list line lifecycle, per-'\n' push, stdout ANSI emission) are gone —
// the headless build shares the real m_LineList lifecycle and emits through
// the same UpdateAllViews drain as the mac product (doc_display_shared.cpp).

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
          // This first-byte range 0x81-0xFE also covers GB2312 (0xA1-0xF7) with
          // the second-byte 0xA1-0xFE disjunct, and the split action is identical,
          // so a separate GB2312 arm here was dead (unreachable) and was removed.
          if (c1 >= 0x81 && c1 <= 0xFE &&                               // first  DBCS (Big5 / GB2312) character
            ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0xA1 && c2 <= 0xFE))  // second DBCS character
              && !m_bUTF_8) // not for UTF-8
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
  if (!m_LinePositions.IsAllocated ())
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
  //
  // m_maxlines == 0 is the "unlimited scrollback" sentinel (OPT_ZERO_UNLIMITED
  // on max_output_lines): never evict.  This is safe only because the jump
  // index sizes itself from the content rather than from the cap — with the old
  // fixed array, skipping the eviction wrote past the end of the allocation on
  // the first line beyond the old boundary.  See line_position_index.h.

  if (m_maxlines > 0 && m_LineList.GetCount () >= m_maxlines)
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
      m_LinePositions.Set (m_LineList.GetCount () / JUMP_SIZE, pos);

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
// (IsWorldViewActive is that same test on Windows; on the port it is the flag
// the wx frame maintains, because the two CView pointers are never set there)

  if (!IsWorldViewActive ())
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


// ── DeleteHeadLines ─────────────────────────────────────────────────────────
// Discard the OLDEST `count` committed lines, freeing them.
//
// RemoveChunk is the only other head deletion and it is fixed at JUMP_SIZE, so
// there was no way to drop an arbitrary number.  This is what the macOS-shaped
// clear commands need:
//
//   Clear Scrollback        drop everything above the first VISIBLE line
//   Clear to Previous Mark  drop everything above the last USER_INPUT line
//
// Both are "keep the recent part, free the rest", which is the one operation
// that genuinely returns memory on a long unlimited-scrollback session --
// Clear Output Buffer (the canonical Windows command) frees everything, and
// Clear Screen frees nothing.
//
// The jump index cannot renumber a partial chunk the way DropFirstChunk does,
// so it is rebuilt; that is O(surviving lines), paid once per explicit user
// command rather than per line.
void CMUSHclientDoc::DeleteHeadLines (long count)
  {
  if (count <= 0)
    return;
  // Never take the in-progress line: it is the list's tail and the drain has
  // not emitted it, so removing it would strand the view's pending row.
  const long committed = m_LineList.GetCount () - (m_pCurrentLine ? 1 : 0);
  if (count > committed)
    count = committed;
  if (count <= 0)
    return;

  for (long i = 0; i < count && !m_LineList.IsEmpty (); i++)
    {
    delete m_LineList.GetHead ();
    m_LineList.RemoveHead ();
    }

#ifndef _WIN32
  // The GUI has already seen these lines; tell the view how many to drop and
  // wind the emission cursor back by the same amount, exactly as RemoveChunk
  // does for its own eviction.
  const long dropped = std::min (count, m_gui_lines_emitted);
  m_gui_lines_emitted = std::max (0L, m_gui_lines_emitted - count);
  if (dropped > 0)
    DeleteOutputHeadLines (dropped);
#endif

  // Rebuild the jump index from what survived (one Append per chunk head).
  m_LinePositions.BeginRebuild ();
  long i = 0;
  for (POSITION pos = m_LineList.GetHeadPosition (); pos; i++)
    {
    if (i % JUMP_SIZE == 0)
      m_LinePositions.Append (pos);
    m_LineList.GetNext (pos);
    }
  if (i == 0)
    m_LinePositions.Reset (m_LineList.GetHeadPosition ());

  // A find cursor into a freed line is a dangling read, so reset it -- the same
  // reason ClearOutput does (doc.cpp).
  m_DisplayFindInfo.m_nCurrentLine  = 0;
  m_DisplayFindInfo.m_pFindPosition = NULL;
  m_DisplayFindInfo.m_bAgain        = FALSE;

  UpdateAllViews (NULL);
  }  // end of CMUSHclientDoc::DeleteHeadLines


// ── CountLinesBeforePromptBlock ─────────────────────────────────────────────
// "Clear to Start": scroll the current prompt to the top of the window by
// throwing away everything above it.  This returns how many head lines that
// is, so the caller can just hand it to DeleteHeadLines.
//
// WHAT COUNTS AS THE PROMPT BLOCK
//
// The block ends at the line MarkPromptLine last stamped (m_last_prompt_line),
// and extends BACKWARDS over:
//
//   * the soft-wrap group that line belongs to -- a prompt wider than the wrap
//     column is several CLines, and only the last carries the PROMPT flag,
//     because the GA byte arrives after all of the text.  A line is a
//     continuation of its predecessor exactly when that predecessor has no
//     hard_return, which is how ProcessPreviousLine reassembles logical lines.
//
//   * any immediately preceding line that is itself flagged PROMPT (plus that
//     line's own wrap group).  That is the only way a prompt spanning several
//     HARD lines can be recognised: telnet marks the end of a prompt, never
//     the start, so a block is identifiable only if the server sent a GA/EOR
//     after each of its lines.  Servers that send one GA for a multi-line
//     prompt give us no signal at all, and only the final line is kept.
//
// If no prompt has ever been seen -- no GA/EOR, which is most of them -- the
// block is the last line and its wrap group, which is the fallback the macOS
// Clear to Start command specifies.
//
// The backwards search for the block's end is bounded by m_last_prompt_line
// rather than running to the head, so a world whose server never sends GA does
// not walk the buffer at all.  A recorded number that no longer names a live
// line (evicted by RemoveChunk, or renumbered by ClearOutput) simply fails to
// match and takes the fallback.
long CMUSHclientDoc::CountLinesBeforePromptBlock (void) const
  {
  const long total = m_LineList.GetCount ();
  if (total <= 1)
    return 0;         // nothing above the last line

  // ── find the block's LAST line ───────────────────────────────────────────
  POSITION blockEnd = m_LineList.GetTailPosition ();
  long     after    = 0;      // lines strictly after blockEnd

  // The in-progress line is the buffer's tail even when nothing has arrived on
  // it yet -- it is where the next character will land, not content.  Keeping
  // only THAT would clear the window completely, so the fallback starts one
  // line further back.
    {
    POSITION pos = blockEnd;
    const CLine * pTail = m_LineList.GetPrev (pos);
    if (pTail == m_pCurrentLine && pTail->len == 0 && pos)
      {
      blockEnd = pos;
      after    = 1;
      }
    }

  // The head's line number is the oldest one the buffer still holds, so this
  // is an O(1) "has the prompt been evicted?" test.  Without it, a prompt that
  // RemoveChunk has already dropped would send the search below all the way to
  // the head -- the whole buffer, which with unlimited scrollback is unbounded.
  const bool bPromptStillHeld =
      m_last_prompt_line > 0 &&
      m_LineList.GetHead ()->m_nLineNumber <= m_last_prompt_line;

  if (bPromptStillHeld)
    {
    long n = 0;
    for (POSITION pos = m_LineList.GetTailPosition (); pos; n++)
      {
      const POSITION here = pos;
      const CLine * pLine = m_LineList.GetPrev (pos);
      if (pLine->m_nLineNumber < m_last_prompt_line)
        break;        // walked past it: the prompt is gone
      if (pLine->m_nLineNumber == m_last_prompt_line &&
          (pLine->flags & PROMPT))
        {
        blockEnd = here;
        after    = n;
        break;
        }
      }
    }

  // ── extend backwards over the whole block ────────────────────────────────
  long blockLen = 0;
  for (POSITION pos = blockEnd; pos; )
    {
    m_LineList.GetPrev (pos);   // pos now names the line BEFORE this one
    blockLen++;
    if (!pos)
      break;                    // reached the head of the buffer
    const CLine * pPrev = m_LineList.GetAt (pos);
    if (!pPrev->hard_return)
      continue;                 // this line is a wrapped continuation of pPrev
    if (pPrev->flags & PROMPT)
      continue;                 // pPrev is a prompt line in its own right
    break;
    }

  const long before = total - after - blockLen;
  return before > 0 ? before : 0;
  }  // end of CMUSHclientDoc::CountLinesBeforePromptBlock

 void CMUSHclientDoc::RemoveChunk (void)
   {
  int i;

// remove JUMP_SIZE lines

  for (i = 0; i < JUMP_SIZE && !m_LineList.IsEmpty (); i++)
    {
    delete m_LineList.GetHead ();   // delete contents of head iten
    m_LineList.RemoveHead ();
    }
  // The emptiness test is defensive only: max_output_lines has a floor of 200
  // and JUMP_SIZE is 100, so a legal cap always leaves at least 100 lines
  // behind.  An out-of-range cap set by writing m_maxlines directly (which the
  // tests and the world loader can do) used to walk off the end of the list.

#ifndef _WIN32
  // Keep the GUI emission counter in sync with lines removed from the head.
  m_gui_lines_emitted = std::max (0L, m_gui_lines_emitted - JUMP_SIZE);
#endif

// chunk 1 becomes chunk 0, and so on
//
// This was a shuffle of every slot in the array — O(m_maxlines / JUMP_SIZE) per
// eviction, i.e. 5,000 pointer copies at a 500,000-line cap, paid every time
// the buffer wrapped.  Dropping the deque's front does the same renumbering in
// constant time.

  m_LinePositions.DropFirstChunk ();

// our "last found" line needs adjusting

  m_DisplayFindInfo.m_nCurrentLine -= JUMP_SIZE;

  // has last found line has disappeared off the front of the buffer?
  if (m_DisplayFindInfo.m_nCurrentLine < 0)
    m_DisplayFindInfo.m_nCurrentLine = 0;

   }  // end of CMUSHclientDoc::RemoveChunk


void CMUSHclientDoc::SendLineToPlugin (void)
  {

  CString strPartialLine = CString (m_pCurrentLine->text, m_pCurrentLine->len);
  SendToAllPluginCallbacks (ON_PLUGIN_PARTIAL_LINE, strPartialLine);

  } // end of CMUSHclientDoc::SendLineToPlugin



// ═══ ExecuteTriggerScript (the approved scripting reconcile, part 2) ═════════
// Relocated verbatim from doc.cpp.  The old mushcore twin fired Lua only and
// skipped CheckScriptingAvailable; the real body guards on
// UsesEmbeddedCallback() (all six engines) and warns when scripting is
// disabled — the COM fallback path is unreachable off-Windows (inert stubs).

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
                                   &StyledLine, // but we *do* have a styled line
                                   NULL,        // nowhere to put a result
                                   // "Convert wildcards to lower case".  Lua
                                   // ignores it (as on Windows); the embedded
                                   // engines apply it, because the COM path
                                   // below hands ActiveScript the already
                                   // lower-cased trigger_item->wildcards[].
                                   trigger_item->bLowercaseWildcard);
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


// ═══ FormatTime (quick-win: real %E/%N/%P/%F/%L substitution headless) ═══════
// Relocated verbatim from doc.cpp.  The old mushcore twin was a bare strftime
// wrapper that ignored the substitutions (log names/preambles lost the world/
// player names) and misnamed the third parameter bUTC — the doc_shared.cpp
// callers always passed m_bLogHTML, i.e. the real bFixHTML semantics.
extern char working_dir [_MAX_PATH];   // MUSHclient.cpp (GUI) / doc_portable.cpp (headless)

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


// ═══ SendWindowSizes / InitZlib (quick wins) ═════════════════════════════════
// SendWindowSizes relocated verbatim from doc.cpp (its non-Windows branch was
// already in the real body) — the ioctl twin is gone and the NAWS/phase guards
// apply headless.  InitZlib (declared #ifndef _WIN32 in doc.h) was duplicated
// between doc_portable.cpp and mushapp_stubs.cpp; one copy now.
// (SendTo stays in doc.cpp for now: its real body needs Execute and
// SetVariable, which arrive with the methods_commands/methods_variables
// Campaign-C adoptions.)

// ── Bookmark ────────────────────────────────────────────────────────────────
// world.Bookmark (LineNumber, Set) — relocated verbatim from
// scripting/methods/methods_output.cpp, except that the "alter appearance of
// all output windows" CMUSHView walk is now the pal::IWorldView seam.  The
// Windows adapter (pal/win_worldview.cpp setBookmark) runs exactly that walk,
// so Windows is unchanged; the wx adapter mirrors the flag into the output
// pane, which used to keep an entirely separate bookmark store that
// world.Bookmark never touched and GetLineInfo (n, 7) could not see.
//
// It has to be shared rather than GUI-only for the same reason tab completion
// did: methods_output.cpp is mushgui-only, so the headless build linked an
// inert stub and no test could reach the real body.
void CMUSHclientDoc::Bookmark(long LineNumber, BOOL Set)
{
  // check line exists
  if (LineNumber <= 0 || LineNumber > m_LineList.GetCount ())
    return;

  LineNumber--;  // zero relative
  // get pointer to line in question

CLine * pLine = m_LineList.GetAt (GetLinePosition (LineNumber));


  if (Set)
    pLine->flags |= BOOKMARK;
  else
    pLine->flags &= ~BOOKMARK;

  // alter appearance of all output windows

  EmitBookmark (LineNumber, Set != 0);

}  // end of CMUSHclientDoc::Bookmark

#ifndef _WIN32

// ── SetWorldViewActive ──────────────────────────────────────────────────────
// The port's stand-in for the bookkeeping both Windows views do when the
// world's window is activated or deactivated (CMUSHView::OnActivateView,
// mushview.cpp:3394-3402; CSendView::OnActivateView, sendvw.cpp:946-954).
// Windows records "this view is active" in m_pActiveOutputView /
// m_pActiveCommandView and zeroes the unread-line counter; here the wx frame
// calls this from its activate/deactivate handling instead.
void CMUSHclientDoc::SetWorldViewActive (bool bActive)
  {
  m_bWorldViewActive = bActive;

  if (!bActive)
    return;

  // "the user is looking at this world now" — its unread lines are read.
  // (Windows: `if (m_new_lines && App.m_pActivityDoc) { m_new_lines = 0;
  //  App.m_bUpdateActivity = TRUE; } else m_new_lines = 0;`)
  if (m_new_lines && App.m_pActivityDoc)
    App.m_bUpdateActivity = TRUE;

  m_new_lines = 0;
  }  // end of CMUSHclientDoc::SetWorldViewActive

// ── WorldWindowTitle ────────────────────────────────────────────────────────
// What this world's window title should read, derived the way
// CMUSHView::FixupTitle derives it on Windows (mushview.cpp:3499-3543): a
// script-set title (world.SetTitle) wins outright and suppresses the status
// suffix; otherwise it is the world name plus the connection phase.
//
// The phase wording is the port's own (Windows spells out the address and port
// in a "[...]" suffix); what matters here is that there is ONE expression.  The
// frame's phase-change tick used to rebuild the title from the world name alone,
// so any title a plugin had set was destroyed by the next connect, disconnect or
// close — and never came back.
CString CMUSHclientDoc::WorldWindowTitle () const
  {
  if (!m_strWindowTitle.IsEmpty ())
    return m_strWindowTitle;

  CString strTitle = m_mush_name;
  if (strTitle.IsEmpty ())
    strTitle = "World";

  const char * szStatus = NULL;   // NULL → no suffix

  switch (m_iConnectPhase)
    {
    case eConnectMudNameLookup:
    case eConnectProxyNameLookup:        szStatus = "Resolving\xe2\x80\xa6";     break;
    case eConnectConnectingToMud:
    case eConnectConnectingToProxy:      szStatus = "Connecting\xe2\x80\xa6";    break;
    case eConnectAwaitingProxyResponse1:
    case eConnectAwaitingProxyResponse2:
    case eConnectAwaitingProxyResponse3: szStatus = "Proxy\xe2\x80\xa6";         break;
    case eConnectAwaitingSSLHandshake:   szStatus = "TLS\xe2\x80\xa6";           break;
    case eConnectConnectedToMud:         szStatus = NULL;                        break;
    case eConnectDisconnecting:          szStatus = "Disconnecting\xe2\x80\xa6"; break;
    default:                             szStatus = "Closed";                    break;
    }

  if (szStatus)
    {
    strTitle += " (";
    strTitle += szStatus;
    strTitle += ")";
    }

  return strTitle;
  }  // end of CMUSHclientDoc::WorldWindowTitle

#endif // !_WIN32

// ── ClampWrapColumn ─────────────────────────────────────────────────────────
// The legal range for m_nWrapColumn, in one place.  It is the range the option
// table declares for "wrap_column" (scriptingoptions_numeric.inc: 20 ..
// MAX_LINE_WIDTH), which SetOption enforces — but the auto-wrap path writes the
// member directly and so has to clamp for itself.  Windows did that inline in
// CMUSHView::AutoWrapWindowWidth; the wx port clamped to 1 .. 65535 instead,
// which let a wide window or a small font store a wrap column outside the
// option's own range and then save it into the world file.
int CMUSHclientDoc::ClampWrapColumn (int iColumn)
  {
  if (iColumn < 20)              return 20;
  if (iColumn > MAX_LINE_WIDTH)  return MAX_LINE_WIDTH;
  return iColumn;
  }

// ── AutoWrapColumnFor ───────────────────────────────────────────────────────
// The "does the window width drive the wrap column?" decision, so that both
// frontends read the same option and neither has to remember the clamp.  0 means
// no: keep whatever the user configured.  See doc.h for the contract.
int CMUSHclientDoc::AutoWrapColumnFor (int iTextColumns) const
  {
  if (!m_bAutoWrapWindowWidth)
    return 0;

  if (iTextColumns <= 0)
    return 0;      // pane not measured yet — nothing trustworthy to derive from

  return ClampWrapColumn (iTextColumns);
  }

void CMUSHclientDoc::SendWindowSizes (const int iNewWidth)
  {

  // connection closed - can't do it

  if (m_iConnectPhase != eConnectConnectedToMud ||
      !ConnectionOpen () ||
      !m_bNAWS_wanted)
    return;

  // see RFC 1073

#ifndef _WIN32
  // Non-Windows: the MFC view list is empty, so the row count comes from the
  // wxWidgets frame instead (m_gui_rows, updated by OnSize/SendNAWS).  The
  // column count is the iNewWidth the caller passed, exactly as the Windows arm
  // uses it — this used to read m_nWrapColumn, which is the same value for the
  // three callers that pass m_nWrapColumn but is still the OLD width for the
  // one that matters: CMUSHclientDoc::Configure sends the new wrap column
  // BEFORE storing it (configuration.cpp), so changing "wrap column" in World
  // Properties announced the previous width to the MUD.
  {
    int cols = std::max(1, iNewWidth);
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




#ifndef _WIN32
// MCCP inflate-stream setup (doc.h declares this member off-Windows only).
bool CMUSHclientDoc::InitZlib(z_stream& zs)
{
    zs.zalloc   = Z_NULL;
    zs.zfree    = Z_NULL;
    zs.opaque   = Z_NULL;
    zs.avail_in = 0;
    zs.next_in  = Z_NULL;
    return (inflateInit(&zs) == Z_OK);
}
#endif // !_WIN32


// ═══ OnGameTrace (with the methods_tracing.cpp adoption) ═════════════════════
// Pure engine logic (m_bTrace toggle + Trace notes); relocated verbatim from
// doc.cpp so SetTrace resolves in mushcore too.

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


// ═══ SetUpVariant* (Campaign C substrate) ════════════════════════════════════
// The six VARIANT out-parameter helpers every scripting/methods file uses,
// relocated verbatim from doc.cpp so the world.* API surface can compile
// headless (the portable VARIANT/COleDateTime/AllocSysString shims live in
// platform/mfc_ui.h and platform/mfc_string.h).

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


// ═══ The table-generic option machinery (option-machinery step 1) ════════════
// The six base functions every tConfiguration*Option table user shares —
// relocated verbatim from scriptingoptions.cpp so the alias/timer/trigger
// option families (whose tables target the SHARED CAlias/CTrigger/CTimer
// classes) can compile headless.  Pure logic over (table, count, pBase);
// the LP64 (int*) width fixes are already in these bodies.

long SetBaseOptionItem (const int iItem,
                        tConfigurationNumericOption BaseOptionsTable [],
                        const int iItemCount,
                        char * pBase,
                        double Value,
                        bool & bChanged)

  {

  bChanged = false;

    // ensure in range
  if (iItem < 0 || iItem >= iItemCount)
    return eUnknownOption;

int iMinimum = BaseOptionsTable [iItem].iMinimum;
int iMaximum = BaseOptionsTable [iItem].iMaximum;

  // fix up default 0, 0 to be 0, 1
  if (iMinimum == 0 && iMaximum == 0)
      iMaximum = 1;

  // OPT_ZERO_UNLIMITED: zero is legal below the minimum and means "no limit".
  // Only max_output_lines carries it — the range there is "200 lines at least,
  // OR none at all" (unbounded scrollback), which a plain min/max cannot say.
  // Additive: no other option sets the flag, so nothing else changes.
  const bool bZeroIsUnlimited =
      (BaseOptionsTable [iItem].iFlags & OPT_ZERO_UNLIMITED) != 0 && Value == 0;

  if (!bZeroIsUnlimited && (Value < iMinimum || Value > iMaximum))
      return eOptionOutOfRange;

  // for custom colours, subtract 1 when we set it (-1 = no change (was 0) )
  //                                               (0 = colour 1 (was 1)   )
  if (BaseOptionsTable [iItem].iFlags & OPT_CUSTOM_COLOUR)
     Value--;

  // get variable address
  char * p = pBase + BaseOptionsTable [iItem].iOffset;

  if (BaseOptionsTable [iItem].iFlags & OPT_DOUBLE)
    {
    if (* (double *) p != Value)
      bChanged = true;
    * (double *) p = Value;
    }
  else
    {    // not double

    switch (BaseOptionsTable [iItem].iLength)
      {

      case 1:
        if (*p != (char) Value)
          bChanged = true;
        * p = (char) Value;
        break;

      case 2:
        if (* (short *) p != (short) Value)
          bChanged = true;
        * (short *) p = (short) Value;
        break;

      case 4:
        // iLength == 4 means a 4-byte member; use int (4 bytes on every target).
        // NOT long — long is 8 bytes on LP64 (macOS/Linux), which would write
        // past the member and clobber the next field (and the matching read in
        // GetBaseOptionItem would read 8 bytes → garbage in the high 32 bits).
        if (* (int *) p != Value)
          bChanged = true;
        * (int *) p = Value;
        break;

      case 8:
        if (* (__int64 *) p != Value)
          bChanged = true;
        * (__int64 *) p = Value;
        break;

      default:
        return eUnknownOption;     // size unknown
      } // end of switch
    }   // not double

  return eOK;

  } // end of SetBaseOptionItem


double GetBaseOptionItem (const int iItem,
                        tConfigurationNumericOption BaseOptionsTable [],
                        const int iItemCount,
                        char * pBase)
  {

  // ensure in range
  if (iItem < 0 || iItem >= iItemCount)
    return -1;

  // get variable address
  char * p = pBase + BaseOptionsTable [iItem].iOffset;
  double Value = -1;

  if (BaseOptionsTable [iItem].iFlags & OPT_DOUBLE)
    {
    Value =  * (double *) p;
    }
  else
    {   // not double

    switch (BaseOptionsTable [iItem].iLength)
      {

      case 1:
        Value = * p;
        break;

      case 2:
        Value =  * (unsigned short *) p;
        break;

      case 4:
        Value =  * (int *) p;       // 4-byte member — NOT long (8 bytes on LP64)
        break;

      case 8:
        Value =  * (__int64 *) p;   // warning - possible truncation!!!!
        break;

      } // end of switch
    } // not double

  // for custom colours, add 1 when we get it (-1 = no change (becomes 0) )
  //                                          (0 = colour 1 (becomes 1)   )
  if (BaseOptionsTable [iItem].iFlags & OPT_CUSTOM_COLOUR)
    {
    Value++;
    // however -1 was really 65535 so adding one made 65536, fix that up!
    if (Value == 65536)
      Value = 0;
    }

  // for booleans - make sure in range 0 to 1
  if (BaseOptionsTable [iItem].iMinimum == 0 &&
      BaseOptionsTable [iItem].iMaximum == 0)
      Value = Value != 0; // make sure true or false

  return Value;

  }  // end of CMUSHclientDoc::GetBaseOptionItem


long FindBaseOption (LPCTSTR OptionName, 
                    tConfigurationNumericOption BaseOptionsTable [],
                    int & iItem)
  {
CString strName = OptionName;

  strName.MakeLower();
  strName.TrimLeft ();
  strName.TrimRight ();

  for (iItem = 0; BaseOptionsTable [iItem].pName; iItem++)
    if (strName == BaseOptionsTable [iItem].pName)
      return eOK;   // found it

  return eUnknownOption;


  } // end of FindBaseOption


long FindBaseAlphaOption (LPCTSTR OptionName, 
                    tConfigurationAlphaOption BaseAlphaOptionsTable [],
                    int & iItem)
  {
CString strName = OptionName;

  strName.MakeLower();
  strName.TrimLeft ();
  strName.TrimRight ();

  for (iItem = 0; BaseAlphaOptionsTable [iItem].pName; iItem++)
    if (strName == BaseAlphaOptionsTable [iItem].pName)
      return eOK;   // found it

  return eUnknownOption;

  } // end of FindBaseAlphaOption


long SetBaseAlphaOptionItem (const int iItem,
                        tConfigurationAlphaOption BaseAlphaOptionsTable [],
                        const int iItemCount,
                        char * pBase,
                        CString & strValue,
                        bool & bChanged)

  {
  bChanged = false;

  // ensure in range
  if (iItem < 0 || iItem >= iItemCount)
    return eUnknownOption;

  // ensure they don't slip multi lines into single-line options
  if (!(BaseAlphaOptionsTable [iItem].iFlags & OPT_MULTLINE))
    {
    strValue.Replace ("\n", "");
    strValue.Replace ("\r", "");
    }

  // see if it is changing
  bChanged = * (CString *) (pBase + BaseAlphaOptionsTable [iItem].iOffset) != strValue;

  // set value
  * (CString *) (pBase + BaseAlphaOptionsTable [iItem].iOffset) = strValue;


  return eOK;

  } // end of SetBaseAlphaOptionItem


CString GetBaseAlphaOptionItem (const int iItem,
                                tConfigurationAlphaOption BaseAlphaOptionsTable [],
                                const int iItemCount,
                                char * pBase)
  {

  // ensure in range
  if (iItem < 0 || iItem >= iItemCount)
    return "";

  return * (CString *) (pBase + BaseAlphaOptionsTable [iItem].iOffset);

  }


// ═══ GetProcedureDispid (option-machinery step 2) ════════════════════════════
// Resolves a script subroutine name to a dispid via the engine's language-
// agnostic GetDispid, appending the not-found diagnostics.  Relocated verbatim
// from scripting/scripting.cpp so the alias/timer/trigger trio (whose option
// setters resolve script names) can compile headless.

DISPID CMUSHclientDoc::GetProcedureDispid (const CString & strName, 
                                           const CString & strType,
                                           const CString & strLabel,
                                           CString & strErrorMessage) 
  {
  // ignore if no script name
  if (strName.IsEmpty () || (GetScriptEngine () == NULL))
    return DISPID_UNKNOWN;

  DISPID dispid = GetScriptEngine ()->GetDispid (strName);

  if (dispid != DISPID_UNKNOWN)
    return dispid;

  // append error message to supplied error message string

  CString strMsg;
  if (strLabel.IsEmpty ())
    strMsg = TFormat ("The %s subroutine named \"%s\" could not be found.",
      (LPCTSTR) strType,
      (LPCTSTR) strName);
  else
    strMsg = TFormat ("The %s (%s) subroutine named \"%s\" could not be found.",
      (LPCTSTR) strType,
      (LPCTSTR) strLabel,
      (LPCTSTR) strName);

  // put in error message preamble
  if (strErrorMessage.IsEmpty ())
    {
    if (m_strScriptFilename.IsEmpty ())
      strErrorMessage = Translate ("You have not specified a script file name:");
    else
      strErrorMessage = TFormat ("There was a problem in script file \"%s\":",
      (LPCTSTR) m_strScriptFilename);
    strErrorMessage += ENDLINE;
    }

  strErrorMessage += ENDLINE;

  strErrorMessage += strMsg;

  return DISPID_UNKNOWN;
  } // end of CMUSHclientDoc::GetProcedureDispid  


// ── CalculateSpeedWalkString ────────────────────────────────────────────────
// Relocated verbatim from Mapping.cpp so the auto-mapper string composition
// (used by world.GetMappingString in methods_mapping.cpp) is available in
// every build; the rest of Mapping.cpp is GUI-facing and stays out of
// mushcore.

#define OUTPUT_PREVIOUS_ONE  \
do { \
  if (strLastDir.GetLength () > 1)  \
    strLastDir = "(" + strLastDir + ")";  \
  if (iCount == 1)  \
    strForwards += strLastDir + " ";  \
  else if (iCount > 1)  \
    strForwards += CFormat ("%i%s ", iCount, (LPCTSTR) strLastDir);  \
  strLastDir = str;  \
  iCount = 1;  \
  } while (false)

CString CMUSHclientDoc::CalculateSpeedWalkString (const bool bOmitComments)
  {

  CString str;
  CString strLastDir;
  CString strForwards;

  int iCount = 0;

  for (POSITION pos = m_strMapList.GetHeadPosition (); pos; )
    {
    // get next direction from list
    str = m_strMapList.GetNext (pos);

    if (str.GetLength () > 2 &&
        str.Left (1) == "{" &&
        str.Right (1) == "}")
      {
      OUTPUT_PREVIOUS_ONE;    // output directions up to comment
      if (!bOmitComments)
        {
        if (!strForwards.IsEmpty ())
          strForwards += ENDLINE;  // put comments on their own lines
        strForwards += str;
        strForwards += ENDLINE;
        }
      strLastDir.Empty ();
      iCount = 0;
      }
    else    // not comment
      // if same as before, count them
      if (str == strLastDir && iCount <= 98)
        iCount++;
      else
        OUTPUT_PREVIOUS_ONE;


    }   // end of processing each direction

  // output final one
  OUTPUT_PREVIOUS_ONE;

  return strForwards;
  }  // end of CMUSHclientDoc::CalculateSpeedWalkString 


// ── TranslateColour ─────────────────────────────────────────────────────────
// Relocated verbatim from doc.cpp for the methods_colours.cpp adoption
// (world.GetMapColour) — pure lookup over m_ColourTranslationMap.

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

// ── SendTo (relocated VERBATIM from doc.cpp, 2026-07) ────────────────────
// The generic trigger/alias/timer send-to router.  Every dependency is
// headless now (SendMsg/DoSendMsg, Execute [methods_commands.cpp],
// SetVariable [methods_variables.cpp], DoEvaluateSpeedwalk,
// AppendToTheNotepad, WriteToLog, the SetCommandText seam, CreateTextWindow
// [mfc_ui.h hook shim] and the stub ShowStatusLine / UMessageBox), so
// mushcore compiles the identical Windows body and the hand-written
// doc_portable.cpp twin is deleted.

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

// ── ApplyAutoSay ───────────────────────────────────────────────────────────
// Auto-say pre-processing for a hand-typed command (World Properties > Input >
// Auto Say).  Lifted from CSendView::SendCommand (sendvw.cpp:608-696) so that
// view implementations other than the Windows CSendView can reach it: on the
// port the command bar is CWorldChildFrame::OnInputEnter, CSendView is never
// instantiated, and consequently NONE of the six auto-say settings did anything
// at all — the page rendered and saved correctly, but the feature was inert.
//
// Returns true when auto-say consumed the command; false to let the caller send
// strFullCommand normally.  strFullCommand is modified in place when the
// override prefix is stripped (so the caller sends the stripped text).
//
// NOTE: sendvw.cpp still carries its own copy of this logic rather than calling
// through here.  Collapsing the two is worthwhile but is deliberately NOT done
// blind: in CSendView the "not connected" arm below is a bare `return` that
// exits SendCommand *before* its command-history/cleanup tail (sendvw.cpp:713+),
// so routing it through a bool return would quietly change Windows behaviour.
// That reconciliation needs a Windows build to verify.
bool CMUSHclientDoc::ApplyAutoSay (CString & strFullCommand)
  {
BOOL bAutoSay = m_bEnableAutoSay;

  // if auto-say enabled, see if we can exclude this line, because of override prefix
  if (bAutoSay && !m_strOverridePrefix.IsEmpty ())    // only if we have one
    if (strFullCommand.Left (m_strOverridePrefix.GetLength ()) == m_strOverridePrefix)
      {
      bAutoSay = false;
      strFullCommand = strFullCommand.Mid (m_strOverridePrefix.GetLength ());   // exclude prefix
      }

  // exclude special characters, if wanted

  if (!strFullCommand.IsEmpty ())
    if (bAutoSay && m_bExcludeNonAlpha &&
        !(isalpha (strFullCommand [0]) || isdigit (strFullCommand [0])))
        bAutoSay = false;

  // exclude text of macros, if wanted

  if (bAutoSay && m_bExcludeMacros)
    {
    for (int i = 0; bAutoSay && i < NUMITEMS (m_macro_type); i++)
      if (m_macro_type [i] == REPLACE_COMMAND ||
          m_macro_type [i] == SEND_NOW)
        if ( !m_macros [i].IsEmpty ())    // ignore blank macros
          if (strFullCommand.Left (m_macros [i].GetLength ()) == m_macros [i])
            bAutoSay = false;
    } // end of checking for macros

  // exclude auto-say sequence itself. ie. don't have "page nick =page nick ="

  if (bAutoSay && strFullCommand.Left (m_strAutoSayString.GetLength ()) ==
      m_strAutoSayString)
      bAutoSay = false;

  if (!bAutoSay)
    return false;     // caller sends strFullCommand normally

  // let them know if they are foolishly trying to send to a closed connection

  if (!m_bReEvaluateAutoSay && CheckConnected ())
    return true;      // consumed: nothing to send

  // break up auto-say string into a list, terminated by newlines
  CStringList strList;
  StringToList (strFullCommand, ENDLINE, strList);
  m_bEnableAutoSay = false; // disable to prevent loop

  // disable command stacking
  unsigned short bSaveCommandStack = m_enable_command_stack;
  m_enable_command_stack = false;

  for (POSITION command_pos = strList.GetHeadPosition (); command_pos; )
    {
    CString str = strList.GetNext (command_pos);

    if (m_bReEvaluateAutoSay)
      {
      // evaluate aliases, speed walking, command stacking etc.

      m_iExecutionDepth = 0;    // hand-typed command, assume depth zero

      str = m_strAutoSayString + str;   // prepend auto-say string

      if (Execute (
              m_bTranslateBackslashSequences ?
                ::FixupEscapeSequences (str) : str
          ) != eOK)
        break;    // some obscure error? give up

      }
    else
      {  // just send it
      SendMsg (m_strAutoSayString + str,
               m_display_my_input,
               false,          // don't queue
               LoggingInput ());
      }
    }

  m_bEnableAutoSay = true; // re-enable it
  m_enable_command_stack = bSaveCommandStack; // re-enable it

  return true;      // consumed
  } // end of CMUSHclientDoc::ApplyAutoSay
