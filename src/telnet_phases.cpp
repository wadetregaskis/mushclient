// telnet_phases.cpp - handle telnet phases in incoming text stream

// Full build (Windows native or mushapp on macOS/Linux): use the real headers.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
// No-op stubs for the debug helpers used below — not needed in the full build.
#define dbg(...)          ((void)0)
#define telopt_name(opt)  ""
#ifdef _WIN32
// g_debug is a portable-build global (doc.h declares it only off-Windows).
// The dbg() stub above already makes every use of it below dead code, so a
// false constant keeps Windows byte-for-byte as it was.
static const bool g_debug = false;
#endif

// A hostile or broken server can open a telnet subnegotiation (IAC SB) and then
// stream data forever without ever sending the terminating IAC SE, growing
// m_IAC_subnegotiation_data without bound until memory is exhausted.  No
// legitimate subnegotiation (GMCP, MSDP, MTTS, CHARSET, NAWS ...) comes anywhere
// near this size, so past this cap we drop the connection -- mirroring the MCCP
// input path, which likewise disconnects when its COMPRESS_BUFFER_LENGTH buffer
// would overflow.  Deliberately generous so no real payload is ever clipped;
// any finite bound defeats the memory-exhaustion attack.
#define MAX_SUBNEGOTIATION_LENGTH (256 * 1024)

/* Phases for processing input stream ...


  To allow for these to be broken between packets we only do one
  character at a time, and maintain a "state" (Phase) which indicates
  how far through the sequence we are.

  The two "triggering" codes we might get in normal text (ie. phase NONE) are
  ESC or IAC.

  Tested (5 Feb 2010):

  * MCCP v1
  * MCCP v2
  * IAC GA              \ff\f9
  * IAC EOR             \ff\ef
  * IAC IAC inside subnegotiation   \FF\FD\66 \ff\fa\66hello\ff\ffboys\ff\f0
  * IAC IAC in normal text      what\ff\ffever
  * MXP
  * Charset: \FF\FD\2A \ff\fa\2A\01,UTF-8,US-ASCII\ff\f0
  * terminal type: \FF\FD\18 \ff\fa\18\01\ff\f0
  * NAWS: \FF\FD\1f \ff\fa\1f\ff\f0
  * Chat system


*/

// ESC x

void CMUSHclientDoc::Phase_ESC (const unsigned char c)
  {
  if (c == '[')
    {
    m_phase = DOING_CODE;
    m_code = 0;
    }
  else if (c == ']')
    {
    // OSC (Operating System Command): ESC ] <text> BEL/ST
    // MUDs use this for window titles (ESC]0;title BEL) and colour palette
    // (ESC]4;...).  In CLI mode we have no window title to set, so we simply
    // swallow the entire sequence.
#ifndef _WIN32
    dbg("OSC sequence started - will discard until BEL/ST");
#endif
    m_phase = HAVE_OSC;
    }
  else
    {
    // Other ESC sequences (ESC c = reset, ESC ( = charset, etc.) — ignore.
    m_phase = NONE;
    }

  } // end of Phase_ESC

// OSC - we have ESC ] and are swallowing until BEL or ST (ESC \)
// Compiled on ALL platforms: Phase_ESC above enters HAVE_OSC unconditionally,
// so every build must be able to consume the payload.  (This was historically
// #ifndef _WIN32 on the mistaken belief that Windows never entered HAVE_OSC —
// in fact DisplayMsg simply had no HAVE_OSC case, so OSC payloads leaked into
// the visible output on Windows and the mac product.)
void CMUSHclientDoc::Phase_OSC (const unsigned char c)
  {
  if (c == '\007')          // BEL terminates OSC
    m_phase = NONE;
  else if (c == 0x9C)       // ST as a single C1 byte (rare)
    m_phase = NONE;
  else if (c == '\033')     // ESC starts a possible String Terminator (ESC \)
    m_phase = HAVE_ESC;     // if next char is '\', Phase_ESC→NONE; harmless either way
  // else: still inside the OSC payload — stay in HAVE_OSC and keep discarding
  }

// ANSI - we have ESC [ x

void CMUSHclientDoc::Phase_ANSI (const unsigned char c)
  {
  if (isdigit (c))
    {
    m_code *= 10;
    m_code += c - '0';
    }
  else if (c == 'm')
    {
    if (m_phase != DOING_CODE)
      Interpret256ANSIcode (m_code);
    else
      InterpretANSIcode (m_code);
    m_phase = NONE;
    }
  else if (c == ';' || c == ':')  // separator, eg. ESC[ 38:5:<n>
    {
    if (m_phase != DOING_CODE)
      Interpret256ANSIcode (m_code);
    else
      InterpretANSIcode (m_code);
    m_code = 0;
    }
  else if (c == 'z')    // MXP line security mode
    {
    if (m_code == eMXP_reset)
      MXP_Off ();
    else
      {
//      TRACE1 ("\nMXP security mode now %i\n", m_code);
      MXP_mode_change (m_code);
      }
    m_phase = NONE;
    }
  else
    m_phase = NONE;
  } // end of Phase_ANSI

// IAC - we have IAC x

void CMUSHclientDoc::Handle_IAC_GA ()
  {
  SendToAllPluginCallbacks (ON_PLUGIN_IAC_GA);
  }   // end of  CMUSHclientDoc::Handle_IAC_GA

// An IAC GA or IAC EOR means "go ahead, it is your turn" -- the text that has
// arrived since the last line break IS the server's prompt.  Remember which
// line that was, so a later "clear everything above the prompt" can find it
// even after more output has pushed it up the buffer.
//
// Two server conventions, both handled:
//
//   "...\r\n> " IAC GA    the usual one.  The prompt was NOT terminated, so it
//                         is still the in-progress line -- stamp that.
//   "...\r\n" IAC EOR     GA/EOR as an end-of-record marker.  The prompt is
//                         already committed and the in-progress line is a fresh
//                         empty one, so stamp the line before it.
//
// An empty line is never a prompt, which is what distinguishes the two.
//
// m_last_line_with_IAC_GA keeps its upstream meaning (the line number current
// when the GA was seen, reported by world.GetInfo(289)); m_last_prompt_line is
// the line actually stamped, which differs in the second case above.
void CMUSHclientDoc::MarkPromptLine ()
  {
  m_last_line_with_IAC_GA = m_total_lines;

  CLine * pPrompt = NULL;

  if (m_pCurrentLine && m_pCurrentLine->len > 0)
    pPrompt = m_pCurrentLine;
  else if (!m_LineList.IsEmpty ())
    {
    // The in-progress line is empty (or absent): the prompt, if there is one,
    // is the last committed line.
    POSITION pos = m_LineList.GetTailPosition ();
    CLine * pLast = m_LineList.GetPrev (pos);
    if (pLast == m_pCurrentLine)          // skip the empty in-progress line
      pLast = pos ? m_LineList.GetPrev (pos) : NULL;
    if (pLast && pLast->len > 0)
      pPrompt = pLast;
    }

  if (!pPrompt)
    return;

  pPrompt->flags |= PROMPT;
  m_last_prompt_line = pPrompt->m_nLineNumber;
  }   // end of CMUSHclientDoc::MarkPromptLine

void CMUSHclientDoc::Phase_IAC (unsigned char & c)
  {
  const char * p;
  unsigned char new_c = 0;    // returning zero stops further processing of c

  switch (c)
    {
    case EOR                 : 
          m_phase = NONE; 
          p = "EOR"; 
          if (m_bConvertGAtoNewline)
            new_c = '\n';
          MarkPromptLine ();
          Handle_IAC_GA ();
          break;
    case GO_AHEAD            :
          m_phase = NONE;
          p = "GA";
          if (m_bConvertGAtoNewline)
            new_c = '\n';
          MarkPromptLine ();
          Handle_IAC_GA ();
          break;

    case SE                  : m_phase = NONE;      p = "SE"; break;
    case NOP                 : m_phase = NONE;      p = "NOP"; break;
    case DATA_MARK           : m_phase = NONE;      p = "DM"; break;
    case BREAK               : m_phase = NONE;      p = "BRK"; break;
    case INTERRUPT_PROCESS   : m_phase = NONE;      p = "IP"; break;
    case ABORT_OUTPUT        : m_phase = NONE;      p = "AO"; break;
    case ARE_YOU_THERE       : m_phase = NONE;      p = "AYT"; break;
    case ERASE_CHARACTER     : m_phase = NONE;      p = "EC"; break;
    case ERASE_LINE          : m_phase = NONE;      p = "EL"; break;
    case SB                  : m_phase = HAVE_SB;   p = "SB"; break;
    case WILL                : m_phase = HAVE_WILL; p = "WILL"; break;
    case WONT                : m_phase = HAVE_WONT; p = "WONT"; break;
    case DO                  : m_phase = HAVE_DO;   p = "DO"; break;
    case DONT                : m_phase = HAVE_DONT; p = "DONT"; break;
    default                  : m_phase = NONE;      p = "none"; break;
    } // end of switch
  TRACE1 ("<%s>", p);
  m_subnegotiation_type = 0;    // no subnegotiation type yet
  c = new_c;

  } // end of Phase_IAC

// send to all plugins: function OnPluginTelnetRequest (num, type)
//    eg. num = 200, type = DO
// They reply true or false to handle or not handle that telnet type
 
bool CMUSHclientDoc::Handle_Telnet_Request (const int iNumber, const string sType)

  {

  return SendToAllPluginCallbacks (ON_PLUGIN_TELNET_REQUEST,
                                    iNumber,
                                    sType,
                                    true,     // stop on true response
                                    false);

  } // end of CMUSHclientDoc::Handle_Telnet_Request 

void CMUSHclientDoc::Send_IAC_DO (const unsigned char c)
  {
  // if we are already in a mode do not agree again - see RFC 854 
  // and forum subject 3061                           

  if (m_bClient_sent_IAC_DO [c])
      return;

  unsigned char do_do_it [3]   = { IAC, DO, c };

  TRACE1 ("\nSending IAC DO <%d>\n", c);
  dbg("send IAC DO %s", telopt_name(c));
  SendPacket (do_do_it, sizeof do_do_it);
  m_bClient_sent_IAC_DO [c]   = true;
  m_bClient_sent_IAC_DONT [c] = false;

  } // end of CMUSHclientDoc::Send_IAC_DO

void CMUSHclientDoc::Send_IAC_DONT (const unsigned char c)
  {
  // if we are already in a mode do not agree again - see RFC 854 
  // and forum subject 3061                           
  if (m_bClient_sent_IAC_DONT [c])
      return;

  unsigned char dont_do_it [3] = { IAC, DONT, c };

  TRACE1 ("\nSending IAC DONT <%d>\n", c);
  dbg("send IAC DONT %s", telopt_name(c));
  SendPacket (dont_do_it, sizeof dont_do_it);
  m_bClient_sent_IAC_DONT [c] = true;
  m_bClient_sent_IAC_DO [c]   = false;

  } // end of CMUSHclientDoc::Send_IAC_DONT

void CMUSHclientDoc::Send_IAC_WILL (const unsigned char c)
  {
  // if we are already in a mode do not agree again - see RFC 854 
  // and forum subject 3061                           
  if (m_bClient_sent_IAC_WILL [c])
      return;

  unsigned char will_do_it [3]   = { IAC, WILL, c };

  TRACE1 ("\nSending IAC WILL <%d>\n", c);
  dbg("send IAC WILL %s", telopt_name(c));
  SendPacket (will_do_it, sizeof will_do_it);
  m_bClient_sent_IAC_WILL [c] = true;
  m_bClient_sent_IAC_WONT [c] = false;

  } // end of CMUSHclientDoc::Send_IAC_WILL

void CMUSHclientDoc::Send_IAC_WONT (const unsigned char c)
  {
  // if we are already in a mode do not agree again - see RFC 854 
  // and forum subject 3061                           
  if (m_bClient_sent_IAC_WONT [c])
      return;

  unsigned char wont_do_it [3] = { IAC, WONT, c };

  TRACE1 ("\nSending IAC WONT <%d>\n", c);
  dbg("send IAC WONT %s", telopt_name(c));
  SendPacket (wont_do_it, sizeof wont_do_it);
  m_bClient_sent_IAC_WONT [c] = true;
  m_bClient_sent_IAC_WILL [c] = false;

  } // end of CMUSHclientDoc::Send_IAC_WONT


// WILL - we have IAC WILL x   - reply DO or DONT (generally based on client option settings)
// for unknown types we query plugins: function OnPluginTelnetRequest (num, type) 
//    eg. num = 200, type = WILL
// They reply true or false to handle or not handle that telnet type

void CMUSHclientDoc::Phase_WILL (const unsigned char c)
  {

// telnet negotiation : in response to WILL, we say DONT
// (except for compression, MXP, TERMINAL_TYPE and SGA), we *will* handle that)

  TRACE1 ("<%d>", c);
  dbg("recv IAC WILL %s", telopt_name(c));
  m_phase = NONE;  // back to normal text after this character

  m_nCount_IAC_WILL++;
  m_bClient_got_IAC_WILL [c] = true;

  switch (c)
    {
    case TELOPT_COMPRESS2:
    case TELOPT_COMPRESS:
      // initialise compression library if not already decompressing
      if (!m_bCompressInitOK && !m_bCompress)
        m_bCompressInitOK = InitZlib (m_zCompress);
      if (m_bCompressInitOK && !m_bDisableCompression)
        {
        if (!m_CompressOutput)
          m_CompressOutput = (Bytef *) malloc (m_nCompressionOutputBufferSize);
        if (!m_CompressInput)
          m_CompressInput = (Bytef *) malloc (COMPRESS_BUFFER_LENGTH);

        if (m_CompressOutput && !m_CompressInput)
          {
          free (m_CompressOutput);    // only got one? may as well free it
          m_CompressOutput = NULL;
          }

        if (m_CompressOutput && m_CompressInput &&     // we got memory - we can do it
            !(c == TELOPT_COMPRESS && m_bSupports_MCCP_2)) // don't agree to MCCP1 and MCCP2
          {
          Send_IAC_DO (c);
          if (c == TELOPT_COMPRESS2)
            {
            m_bSupports_MCCP_2 = true;
            dbg("MCCP2 compression negotiated - stream will compress after SB");
            }
          else
            dbg("MCCP1 compression negotiated - stream will compress after SB");
          }
        else
          {   // not enough memory or already agreed to MCCP 2 - no compression
          Send_IAC_DONT (c);
          }
        }   // end of compression wanted and zlib engine initialised
      else
        {
        Send_IAC_DONT (c);
        }
      break;    // end of TELOPT_COMPRESS

    // here for SGA (Suppress GoAhead) 
    case SGA:
          Send_IAC_DO (c);
          break;  // end of SGA 

    // here for TELOPT_MUD_SPECIFIC 
    case TELOPT_MUD_SPECIFIC:
          Send_IAC_DO (c);
          break;  // end of TELOPT_MUD_SPECIFIC 

    case TELOPT_ECHO:
          if (!m_bNoEchoOff)
            {
            m_bNoEcho = true;
            TRACE ("Echo turned off\n");
            Send_IAC_DO (c);
            }
          else
            Send_IAC_DONT (c);
          break; // end of TELOPT_ECHO

    case TELOPT_MXP:
          if (m_iUseMXP == eNoMXP)
            {
            Send_IAC_DONT (c);
            }     // end of no MXP wanted
          else
            {
            Send_IAC_DO (c);
            if (m_iUseMXP == eQueryMXP)     // turn MXP on now
              {
              dbg("MXP enabled (server WILL MXP)");
              MXP_On ();
              }
            } // end of MXP wanted
          break;  // end of MXP

    // here for EOR (End of record)
    case WILL_END_OF_RECORD:
          if (m_bConvertGAtoNewline)
            Send_IAC_DO (c);
          else
            Send_IAC_DONT (c);
          break;  // end of WILL_END_OF_RECORD


    // character set negotiations
    case TELOPT_CHARSET:
        Send_IAC_DO (c);
        break;

    default:
        if (Handle_Telnet_Request (c, "WILL"))
          {
          Send_IAC_DO (c);
          Handle_Telnet_Request (c, "SENT_DO");
          }
        else
          Send_IAC_DONT (c);
        break;  // end of others

    } // end of switch


  } // end of Phase_WILL

// Received: IAC WONT x

void CMUSHclientDoc::Phase_WONT (const unsigned char c)
  {

// telnet negotiation : in response to WONT, we say DONT

  TRACE1 ("<%d>", c);
  dbg("recv IAC WONT %s", telopt_name(c));
  m_phase = NONE;

  m_nCount_IAC_WONT++;
  m_bClient_got_IAC_WONT [c] = true;

  switch (c)
    {
    case TELOPT_ECHO:
        if (!m_bNoEchoOff)
          {
          m_bNoEcho = false;
          TRACE ("Echo turned on\n");
          }
        Send_IAC_DONT (c);
        break; // end of TELOPT_ECHO

    default:
      Send_IAC_DONT (c);
      break;
    } // end of switch
  } // end of Phase_WONT

// Received: IAC DO x

// for unknown types we query plugins: function OnPluginTelnetRequest (num, type) 
//    eg. num = 200, type = DO
// They reply true or false to handle or not handle that telnet type

void CMUSHclientDoc::Phase_DO (const unsigned char c)
  {
// telnet negotiation : in response to DO, we say WILL for:
//  <102> (Aardwolf), SGA, echo, NAWS, CHARSET, MXP and Terminal type
// for others we query plugins to see if they want to handle it or not

  TRACE1 ("<%d>", c);
  dbg("recv IAC DO %s", telopt_name(c));
  m_phase = NONE;

  m_nCount_IAC_DO++;
  m_bClient_got_IAC_DO [c] = true;

  switch (c)
    {
    case SGA:
    case TELOPT_MUD_SPECIFIC:
    case TELOPT_ECHO:
    case TELOPT_CHARSET:
        Send_IAC_WILL (c);
        break; // end of things we will do 
            
    // for MTTS start back at sequence 0
    case TELOPT_TERMINAL_TYPE:   
        m_ttype_sequence = 0;
        Send_IAC_WILL (c);
        break;

    case TELOPT_NAWS:
      // option off - must be server initiated
      if (m_bNAWS)
        {
        Send_IAC_WILL (c);
        m_bNAWS_wanted = true;
        SendWindowSizes (m_nWrapColumn);
        }
      else
        Send_IAC_WONT (c);
      break;

    case TELOPT_MXP:
          if (m_iUseMXP == eNoMXP)
            Send_IAC_WONT (c);
          else
            {
            Send_IAC_WILL (c);
            if (m_iUseMXP == eQueryMXP)     // turn MXP on now
              {
              dbg("MXP enabled (server DO MXP)");
              MXP_On ();
              }
            } // end of MXP wanted
          break;  // end of MXP

    default:
          if (Handle_Telnet_Request (c, "DO"))
            {
            Send_IAC_WILL (c);
            Handle_Telnet_Request (c, "SENT_WILL");
            }
          else
            Send_IAC_WONT (c);
          break;    // end of others
    }   // end of switch

  } // end of Phase_DO

// Received: IAC DONT x

void CMUSHclientDoc::Phase_DONT (const unsigned char c)
  {
// telnet negotiation : in response to DONT, we say WONT

  TRACE1 ("<%d>", c);
  dbg("recv IAC DONT %s", telopt_name(c));
  m_phase = NONE;
  Send_IAC_WONT (c);

  m_nCount_IAC_DONT++;
  m_bClient_got_IAC_DONT [c] = true;

  switch (c)
    {

    case TELOPT_MXP:
          if (m_bMXP)
            MXP_Off (true);
          break;  // end of MXP

    // for MTTS start back at sequence 0
    case TELOPT_TERMINAL_TYPE:   
        m_ttype_sequence = 0;
        break;

    }   // end of switch

  } // end of Phase_DONT

// SUBNEGOTIATION - we have IAC SB c 
// remember c (the type) and start collecting the data, as in:
// IAC SB c <data> IAC SE

void CMUSHclientDoc::Phase_SB (const unsigned char c)
  {
  TRACE1 ("<%d>", c);

  // note IAC SB COMPRESS is a special case because they forgot to specify
  // the IAC SE, and thus we can't use normal negotiation
  if (c == TELOPT_COMPRESS)
    m_phase = HAVE_COMPRESS;
  else
    {
    m_subnegotiation_type = c;
    m_IAC_subnegotiation_data.erase ();
    m_phase = HAVE_SUBNEGOTIATION;
    }
  } // end of CMUSHclientDoc::Phase_SB 

// Give up on a subnegotiation that has passed MAX_SUBNEGOTIATION_LENGTH and
// close the world.
//
// The order matters, and so does the state reset.  These helpers are called
// from DisplayMsg's byte loop as `Phase_SUBNEGOTIATION (c); continue;` — a
// void call that cannot stop the loop — so if the phase and the collected data
// were left alone, every REMAINING byte of the packet would re-enter the guard
// and put up another modal box.  Measured on Windows 2026-08-20: eight boxes
// from one hostile stream where one is correct.  Leaving the subnegotiation
// before the disconnect also matters because UMessageBox pumps messages, and a
// re-entrant ReceiveMsg during that pump would otherwise find the phase machine
// still inside a subnegotiation that we have already abandoned.
//
// The caller stops as well (doc_shared.cpp's HAVE_SUBNEGOTIATION arms watch for
// the disconnect, exactly as the MCCP arms beside them already do), so the rest
// of the packet is discarded rather than rendered as MUD output.  That is
// belt-and-braces on purpose: this is a hostile-input path, and the two halves
// fail independently.
void CMUSHclientDoc::AbandonSubnegotiation (void)
  {
  m_IAC_subnegotiation_data.erase ();
  m_subnegotiation_type = 0;
  m_phase = NONE;
  OnConnectionDisconnect ();    // close the world
  UMessageBox ("Telnet subnegotiation too long - closing connection",
               MB_ICONEXCLAMATION);
  } // end of CMUSHclientDoc::AbandonSubnegotiation

// SUBNEGOTIATION - we have IAC SB c (data)
// if we get an IAC remember it, because it may or may not be followed by IAC or SE
void CMUSHclientDoc::Phase_SUBNEGOTIATION (const unsigned char c)
  {

  if (c == IAC)
    {
    // have IAC SB x <data> IAC
    // we may or may not have another IAC or possibly an SE
    m_phase = HAVE_SUBNEGOTIATION_IAC;
    }
  else
    {
    // just collect the data until IAC SE
    if (m_IAC_subnegotiation_data.size () >= MAX_SUBNEGOTIATION_LENGTH)
      {
      AbandonSubnegotiation ();
      return;
      }
    m_IAC_subnegotiation_data += c;
    }


  } // end of Phase_SUBNEGOTIATION

// SUBNEGOTIATION - we have IAC SB x (data) IAC c
// if the c after IAC is IAC then that becomes a single IAC (which we store now)
// otherwise it should be SE, and we assume it is, otherwise we have an invalid sequence
void CMUSHclientDoc::Phase_SUBNEGOTIATION_IAC (const unsigned char c)
  {

  if (c == IAC)
    {
    // have IAC SB x <data> IAC IAC
    // store the single IAC
    if (m_IAC_subnegotiation_data.size () >= MAX_SUBNEGOTIATION_LENGTH)
      {
      AbandonSubnegotiation ();
      return;
      }
    m_IAC_subnegotiation_data += c;
    // press on with subnegotiation
    m_phase = HAVE_SUBNEGOTIATION;
    return;
    }

  // see: http://www.gammon.com.au/forum/?id=10043
  // we have to assume that anything other than IAC is a SE, because 
  // the spec is silent on what to do otherwise
  if (c == SE)        // end of subnegotiation
    TRACE ("<SE>");
  else
    TRACE1 ("<%d> (invalid)", c);

  m_phase = NONE;      // negotiation is over, next byte is plain text

  m_nCount_IAC_SB++;

  // subnegotiation is complete ...
  // we have IAC SB <m_subnegotiation_type> <m_IAC_subnegotiation_data> IAC SE

  // Log every subnegotiation when debug is on.
  // For text-safe protocols (GMCP/ATCP) show the data directly; for binary
  // ones (COMPRESS2) just show the option number and byte count.
  if (g_debug)
    {
    bool bTextSafe = true;
    for (unsigned char b : m_IAC_subnegotiation_data)
      if (b < 0x20 && b != '\t' && b != '\n') { bTextSafe = false; break; }

    if (bTextSafe)
      {
      // Show up to 200 chars of the payload
      string preview = m_IAC_subnegotiation_data.substr(0, 200);
      if (m_IAC_subnegotiation_data.size() > 200) preview += "...";
      dbg("SB %s  %s", telopt_name(m_subnegotiation_type), preview.c_str());
      }
    else
      dbg("SB %s  (%zu bytes, binary)", telopt_name(m_subnegotiation_type),
          m_IAC_subnegotiation_data.size());
    }

  switch (m_subnegotiation_type)
    {
    case TELOPT_COMPRESS2:      
      Handle_TELOPT_COMPRESS2 ();     
      break;

    case TELOPT_MXP:            
      Handle_TELOPT_MXP ();           
      break;

    case TELOPT_TERMINAL_TYPE:  
      Handle_TELOPT_TERMINAL_TYPE (); 
      break;

    case TELOPT_CHARSET:        
      Handle_TELOPT_CHARSET ();       
      break;


    case TELOPT_MUD_SPECIFIC:   
        Handle_TELOPT_MUD_SPECIFIC ();  
    // NOTE: no break, fall through and also do the default

    default:
      {
      SendToAllPluginCallbacks (ON_PLUGIN_TELNET_SUBNEGOTIATION,
                                m_subnegotiation_type,
                                m_IAC_subnegotiation_data,
                                false,
                                false);

      }
      break;  // end of default

    } // end of switch

  } // end of  CMUSHclientDoc::Phase_SUBNEGOTIATION_IAC 


// turn MCCP v2 on
void CMUSHclientDoc::Handle_TELOPT_COMPRESS2 ()
  {
  CString strMessage;

  m_iMCCP_type = 2;

  // initialise compression library if not already done
  if (!m_bCompressInitOK && !m_bCompress)
    m_bCompressInitOK = InitZlib (m_zCompress);

  if (!(m_bCompressInitOK && m_CompressOutput && m_CompressInput))
    strMessage = Translate ("Cannot process compressed output. World closed.");
  else
    {

    int izError = inflateReset (&m_zCompress);

    if (izError == Z_OK)
      {
      m_bCompress  = true;
      TRACE ("Compression on\n");
      return;
      }

    if (m_zCompress.msg)
      strMessage = TFormat ("Could not reset zlib decompression engine: %s",
                               m_zCompress.msg);
    else
      strMessage = TFormat ("Could not reset zlib decompression engine: %i",
                               izError);
    } 

  OnConnectionDisconnect ();    // close the world
  UMessageBox (strMessage, MB_ICONEXCLAMATION);
  } // end of CMUSHclientDoc::Handle_TELOPT_COMPRESS2


// turn MXP on, if required on subnegotiation
void CMUSHclientDoc::Handle_TELOPT_MXP ()
  {
  if (m_iUseMXP == eOnCommandMXP)   // if wanted now
    MXP_On ();
  } // end of CMUSHclientDoc::Handle_TELOPT_MXP ()

// IAC SB CHARSET REQUEST DELIMITER <name> DELIMITER
/*

For backwards compatibility:

Server sends:  IAC DO CHARSET
Client sends:  IAC WILL CHARSET

  or:

See: https://tools.ietf.org/html/rfc2066

Server sends:  IAC WILL CHARSET
Client sends:  IAC DO CHARSET

Server sends:  IAC SB CHARSET REQUEST DELIM NAME IAC SE
Client sends:  IAC SB CHARSET ACCEPTED NAME IAC SE
or
Client sends:  IAC SB CHARSET REJECTED IAC SE

where:

  CHARSET: 0x2A
  REQUEST: 0x01
  ACCEPTED:0x02
  REJECTED:0x03
  DELIM:   some character that does not appear in the charset name, other than IAC, eg. comma, space
  NAME:    the character string "UTF-8" (or some other name like "S-JIS")

*/

void CMUSHclientDoc::Handle_TELOPT_CHARSET ()
  {
  // must have at least REQUEST DELIM NAME [ DELIM NAME2 ...]
  if (m_IAC_subnegotiation_data.size () < 3)
    return;  

  int tt = m_IAC_subnegotiation_data [0];

  if (tt != 1) 
    return;  // not a REQUEST

  string delim = m_IAC_subnegotiation_data.substr (1, 1);

  vector <string> v;
  StringToVector (m_IAC_subnegotiation_data.substr (2), v, delim, false);

  bool found = false;
  CString strCharset = "US-ASCII"; // default

  // (upstream tested `if (m_font)` here -- an array, so always true -- and
  // nothing below uses the font)
    {

    // hack! ugh.
    if (m_bUTF_8)
       strCharset = "UTF-8";

    for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
      if (i->c_str () == strCharset)
        {
        found = true;

        unsigned char p1 [] = { IAC, SB, TELOPT_CHARSET, 2 };  // 2 = accepted
        unsigned char p2 [] = { IAC, SE }; 
        unsigned char sResponse [40];
        int iLength = 0;

        // build up response, eg. IAC, SB, TELOPT_CHARSET, 2, "UTF-8", IAC, SE 

        // preamble
        memcpy (sResponse, p1, sizeof p1);
        iLength += sizeof p1;

        // ensure max of 20 so we don't overflow the field
        CString strTemp = strCharset.Left (20);

        memcpy (&sResponse [iLength], strTemp, strTemp.GetLength ());
        iLength += strTemp.GetLength ();

        // postamble
        memcpy (&sResponse [iLength], p2, sizeof p2);
        iLength += sizeof p2;

        SendPacket (sResponse, iLength);

        }
    } // end of having an output font

  if (!found)
    {
    unsigned char p [] = { IAC, SB, TELOPT_CHARSET, 3, IAC, SE };    // 3 = rejected
    SendPacket (p, sizeof p);
    }  // end of charset not in use


  } // end of CMUSHclientDoc::Handle_TELOPT_CHARSET ()


// stuff for Aardwolf (telopt 102) - call specific plugin handler: OnPluginTelnetOption
void CMUSHclientDoc::Handle_TELOPT_MUD_SPECIFIC ()
  {
  CString strReceived (m_IAC_subnegotiation_data.c_str ());
  SendToAllPluginCallbacks (ON_PLUGIN_TELNET_OPTION, strReceived);
  } // end of CMUSHclientDoc::Handle_TELOPT_MUD_SPECIFIC ()

#define TTYPE_IS 0
#define TTYPE_SEND 1

// terminal type request
void CMUSHclientDoc::Handle_TELOPT_TERMINAL_TYPE ()
  {

  int tt = m_IAC_subnegotiation_data [0];

  if (tt != TTYPE_SEND) 
    return;  // not a SEND

  TRACE ("<SEND>");
  // we reply: IAC SB TERMINAL-TYPE IS ... IAC SE
  // see: RFC 930 and RFC 1060
  // also see: http://tintin.sourceforge.net/mtts/

  unsigned char p1 [4] = { IAC, SB, TELOPT_TERMINAL_TYPE, TTYPE_IS };
  unsigned char p2 [2] = { IAC, SE };
  unsigned char sResponse [40];
  int iLength = 0;

  // build up response, eg. IAC, SB, TELOPT_TERMINAL_TYPE, 0, "MUSHCLIENT", IAC, SE 

  // preamble
  memcpy (sResponse, p1, 4);
  iLength += 4;

  // ensure max of 20 so we don't overflow the field
  CString strTemp;
  

  /*
  On the first TTYPE SEND request the client should return its name, preferably without a version number and in all caps.

  On the second TTYPE SEND request the client should return a terminal type, preferably in all caps. 
    Console clients should report the name of the terminal emulator, 
    other clients should report one of the four most generic terminal types.

      "DUMB"              Terminal has no ANSI color or VT100 support.
      "ANSI"              Terminal supports all ANSI color codes. Supporting blink and underline is optional.
      "VT100"             Terminal supports most VT100 codes, including ANSI color codes.
      "XTERM"             Terminal supports all VT100 and ANSI color codes, xterm 256 colors, mouse tracking, and the OSC color palette.

  If 256 color detection for non MTTS compliant servers is a must it's an option 
    to report "ANSI-256COLOR", "VT100-256COLOR", or "XTERM-256COLOR". 
    The terminal is expected to support VT100, mouse tracking, and the OSC color palette if "XTERM-256COLOR" is reported.

  On the third TTYPE SEND request the client should return MTTS followed by a bitvector. The bit values and their names are defined below.

          1 "ANSI"              Client supports all ANSI color codes. Supporting blink and underline is optional.
          2 "VT100"             Client supports most VT100 codes.
          4 "UTF-8"             Client is using UTF-8 character encoding.
          8 "256 COLORS"        Client supports all xterm 256 color codes.
         16 "MOUSE TRACKING"    Client supports xterm mouse tracking.
         32 "OSC COLOR PALETTE" Client supports the OSC color palette.
         64 "SCREEN READER"     Client is using a screen reader.
        128 "PROXY"             Client is a proxy allowing different users to connect from the same IP address.
        256 "TRUECOLOR"         Client supports truecolor codes using semicolon notation.
        512 "MNES"              Client supports the Mud New Environment Standard for information exchange.
       1024 "MSLP"              Client supports the Mud Server Link Protocol for clickable link handling.
       2048 "SSL"               Client supports SSL for data encryption, preferably TLS 1.3 or higher.
  */

  switch (m_ttype_sequence)
    {
    case 0:
        strTemp = m_strTerminalIdentification.Left (20);
        m_ttype_sequence++;
        break;

    case 1:
        // "ANSI", not "XTERM-256COLOR".  Per the MTTS text above, reporting
        // XTERM-256COLOR asserts mouse tracking and the OSC colour palette as
        // well, neither of which this client has on any platform; the 256-colour
        // capability is stated properly by bit 8 of the bitvector below.
        strTemp = "ANSI";
        m_ttype_sequence++;
        break;

    case 2:
        {
        // ANSI=1, 256 COLORS=8, TRUECOLOR=256.
        //
        // The TRUECOLOR bit was upstream's deliberate addition (d9d45b3), and
        // it is honest here: ansi.cpp parses ESC[38;2;r;g;b into COLOUR_RGB and
        // the pane renders the literal RGB.  A guard sweep from the mushcli era
        // had dropped it, along with the terminal name above, on the reasoning
        // that a command-line client could not show 24-bit colour -- true of
        // mushcli, which no longer exists, and never true of the GUI.  MUDs
        // that gate 24-bit output on this bit were sending banded 256-colour
        // approximations to a client that could display the real thing.
        int iMTTS = 265;
        if (m_bUTF_8)
          iMTTS |= 4;     // UTF-8
        if (m_bSSL_Connected)
          iMTTS |= 2048;  // TLS/SSL
        strTemp.Format ("MTTS %d", iMTTS);
        }
        break;

    } // end of switch

  dbg("TERMINAL-TYPE reply #%d: %s", m_ttype_sequence - 1, (LPCTSTR)strTemp);

  memcpy (&sResponse [iLength], (LPCTSTR) strTemp, strTemp.GetLength ());
  iLength += strTemp.GetLength ();

  // postamble
  memcpy (&sResponse [iLength], p2, 2);
  iLength += 2;

  SendPacket (sResponse, iLength);

  } // end of CMUSHclientDoc::Handle_TELOPT_TERMINAL_TYPE ()


// COMPRESSION - we have IAC SB COMPRESS x
void CMUSHclientDoc::Phase_COMPRESS (const unsigned char c)
  {
  if (c == WILL)      // should get COMPRESS WILL
    {
    TRACE ("<WILL>");
    m_phase = HAVE_COMPRESS_WILL;
    }
  else
    {
    m_phase = NONE;   // error
    TRACE1 ("<%d>", c);
    }
  }

// COMPRESSION - we have IAC SB COMPRESS IAC/WILL x   (MCCP v1)

// we will return one of:
//  0 - error in starting compression - close world and display strMessage
//  1 - got IAC or unexpected input, do nothing
//  2 - compression OK - prepare for it

void CMUSHclientDoc::Phase_COMPRESS_WILL (const unsigned char c)
  {
  if (c == SE)        // end of subnegotiation
    {        
    TRACE ("<SE>");

    CString strMessage;

    m_iMCCP_type = 1;

    // initialise compression library if not already done
    if (!m_bCompressInitOK && !m_bCompress)
      m_bCompressInitOK = InitZlib (m_zCompress);

    if (!(m_bCompressInitOK && m_CompressOutput && m_CompressInput))
      strMessage = Translate ("Cannot process compressed output. World closed.");
    else
      {

      int izError = inflateReset (&m_zCompress);

      if (izError == Z_OK)
        {
        m_bCompress  = true;
        TRACE ("Compression on\n");
        // Windows parity (doc.cpp Phase_COMPRESSION): leave the telnet phase
        // machine in NONE.  Without this the first DECOMPRESSED byte of an
        // MCCP v1 stream was consumed by the stale HAVE_COMPRESS_WILL phase.
        m_phase = NONE;
        return;
        }

      if (m_zCompress.msg)
        strMessage = TFormat ("Could not reset zlib decompression engine: %s",
                                 m_zCompress.msg);
      else
        strMessage = TFormat ("Could not reset zlib decompression engine: %i",
                                 izError);
      } 

    OnConnectionDisconnect ();    // close the world
    UMessageBox (strMessage, MB_ICONEXCLAMATION);
    }   // end of IAC SB COMPRESS WILL/IAC SE

  
  // not SE? error
  TRACE1 ("<%d>", c);
  m_phase = NONE;
  } // end of Phase_COMPRESS_WILL

// in UTF-8 mode, if we get a "bad" UTF-8 character we assume it is standard ANSI
// and convert it from ANSI into Unicode, and then into UTF-8 and output that instead
void CMUSHclientDoc::OutputBadUTF8characters (void)
  {
  for (int i = 0; m_UTF8Sequence [i]; i++)
    {
#ifdef _WIN32
    // convert ANSI to Unicode, then back to UTF-8
    WCHAR sUnicode [1];
    MultiByteToWideChar (CP_THREAD_ACP, MB_PRECOMPOSED, (const char *) &m_UTF8Sequence [i], 1, sUnicode, 1);
    char sOutput [5];
    memset (sOutput, 0, sizeof sOutput);  // ensure trailing null
    WideCharToMultiByte (CP_UTF8, 0, sUnicode, 1, sOutput, sizeof sOutput, NULL, NULL);
    AddToLine (sOutput, 0);
#else
    // Same conversion, without the Win32 codepage API.  Passing the raw byte
    // through instead (what the port used to do) leaves INVALID UTF-8 in the
    // line buffer, and every consumer downstream then has to cope: the wx
    // output pane's decode is all-or-nothing, so one stray byte blanked the
    // whole coloured run it landed in.  Windows never produces that state,
    // because it re-encodes the byte here.
    //
    // CP_THREAD_ACP is the machine's ANSI codepage, which on the Western
    // Windows this behaviour was written for is CP1252; that is what we
    // reproduce.  0x80-0x9F are CP1252's own assignments (the five it leaves
    // undefined map to the matching C1 control, as MultiByteToWideChar does);
    // 0xA0-0xFF are Latin-1, i.e. the code point equals the byte.
    static const unsigned short cp1252_80_9F [32] = {
      0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
      0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
      };

    const unsigned char c = m_UTF8Sequence [i];
    unsigned int cp = (c >= 0x80 && c <= 0x9F) ? cp1252_80_9F [c - 0x80] : c;

    char sOutput [5];
    memset (sOutput, 0, sizeof sOutput);   // ensure trailing null
    if (cp < 0x80)
      sOutput [0] = (char) cp;
    else if (cp < 0x800)
      {
      sOutput [0] = (char) (0xC0 | (cp >> 6));
      sOutput [1] = (char) (0x80 | (cp & 0x3F));
      }
    else
      {
      sOutput [0] = (char) (0xE0 | (cp >> 12));
      sOutput [1] = (char) (0x80 | ((cp >> 6) & 0x3F));
      sOutput [2] = (char) (0x80 | (cp & 0x3F));
      }
    AddToLine (sOutput, 0);
#endif
    m_cLastChar = m_UTF8Sequence [i];
    }

  m_phase = NONE;
  }  // end of CMUSHclientDoc::OutputBadUTF8characters

// test data: testing \C5\87\C4\A8\C4\86\C4\B6 Gammon and now: \C6 <---

// here when getting second or subsequent bytes of a UTF8 character
void CMUSHclientDoc::Phase_UTF8 (const unsigned char c)
  {

  // append to our UTF8 sequence
  int i = 0;
  while (m_UTF8Sequence [i])
    i++;
  m_UTF8Sequence [i] = c;
  m_UTF8Sequence [i + 1] = 0;  // null terminator

  if ((c & 0xC0) != 0x80)
    {
    OutputBadUTF8characters ();
    return;
    }

  // we are waiting for less of them
  m_iUTF8BytesLeft--;

  // more to go
  if (m_iUTF8BytesLeft > 0)
    return;

  // check the sequence

  int erroroffset;
  int iBad = _pcre_valid_utf ((const unsigned char *) m_UTF8Sequence, strlen ((const char *) m_UTF8Sequence), &erroroffset);
  if (iBad > 0)
    {
    OutputBadUTF8characters ();
    return;
    }

  // valid UTF8 sequence, add to line
  AddToLine ((const char *) m_UTF8Sequence, 0);
  m_phase = NONE;

  }  // end of CMUSHclientDoc::Phase_UTF8
