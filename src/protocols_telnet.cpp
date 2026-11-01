// protocols_telnet.cpp -- the document's half of the telnet-level protocols: notes and
// the client's own links, the renegotiation guard, NEW-ENVIRON (MNES), the MSSP TLS
// hint, and OSC 8 links.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "protocols.h"
#include "version.h"
#include "text_edges.h"

#include <string>

// ---------------------------------------------------------------- notes

const char * const kProtoActionID = "c11e47c0de00000000c11e47";

std::string Proto_ActionLink (const char * verb, const std::string & arg)
  {
  return std::string ("!!") + kProtoActionID + ":" + verb + "(" + arg + ")";
  }

void Proto_Note (CMUSHclientDoc & doc, const CProtocolNote & note)
  {
  doc.m_pProto->notes.push_back (note);
  }

void Proto_Note (CMUSHclientDoc & doc, const std::string & text)
  {
  CProtocolNote note;
  note.text = text;
  Proto_Note (doc, note);
  }

// Nothing goes in the middle of a line from the server.  This runs at the end of each
// packet, so a note waits for the line it arrived during.
void Proto_FlushNotes (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.notes.empty () || state.bFlushingNotes)
    return;
  if (!doc.m_pCurrentLine || !doc.m_LinePositions.IsAllocated ())
    return;
  if (doc.m_pCurrentLine->len != 0 && !(doc.m_pCurrentLine->flags & COMMENT))
    return;

  state.bFlushingNotes = true;

  const std::vector <CProtocolNote> notes = state.notes;
  state.notes.clear ();

  for (const CProtocolNote & note : notes)
    {
    doc.Tell (note.text.c_str ());
    if (!note.action.empty ())
      {
      doc.Tell (" ");
      doc.Hyperlink (note.action.c_str (), note.linkText.c_str (), note.linkText.c_str (),
                     "", "", FALSE);
      }
    if (!note.action2.empty ())
      {
      doc.Tell (" ");
      doc.Hyperlink (note.action2.c_str (), note.linkText2.c_str (), note.linkText2.c_str (),
                     "", "", FALSE);
      }
    doc.Note ("");
    }

  state.bFlushingNotes = false;
  }

// One of the client's own links in a note was clicked.  Only a note line gets here
// (doc_hyperlink_shared.cpp), and a server cannot write a note line, so a server can
// offer these only through the code in this file, which says what they do first.
bool Proto_RunAction (CMUSHclientDoc & doc, const CString & verb, const CString & arg)
  {
  CProtocolState & state = *doc.m_pProto;

  if (verb == "tls_switch")
    {
    const int port = atoi (arg);
    if (port < 1 || port > 65535)
      return true;
    doc.m_bUseSSL = true;
    doc.m_port = (unsigned short) port;
    doc.SetModifiedFlag (TRUE);
    Proto_Note (doc, CFormat ("Reconnecting with TLS on port %i. Saving the world keeps this.", port).GetString ());
    Proto_FlushNotes (doc);
    state.reconnectAfterClose = true;
    if (doc.m_iConnectPhase != eConnectNotConnected)
      doc.Disconnect ();
    Proto_Tick (doc);    // connects, if the disconnection is already finished
    return true;
    }

  if (verb == "tls_never")
    {
    doc.m_bMsspTlsHint = false;
    doc.SetModifiedFlag (TRUE);
    Proto_Note (doc, "This world will not offer TLS again (mssp_tls_hint).");
    Proto_FlushNotes (doc);
    return true;
    }

  if (verb == "msp_allow")
    {
    Proto_MspAllowHost (doc, (const char *) arg);
    return true;
    }

  if (verb == "relocate")
    {
    // The link says only that it is the offer on the table; what was offered is here.
    if (state.relocateHost.empty ())
      return true;
    state.relocateOriginalServer = (const char *) doc.m_server;
    state.relocateOriginalPort = doc.m_port;
    doc.m_server = state.relocateHost.c_str ();
    doc.m_port = (unsigned short) state.relocatePort;
    state.bRelocated = true;
    Proto_Note (doc, CFormat ("Connecting to %s, port %i. Your password will not be sent there.",
                              state.relocateHost.c_str (), state.relocatePort).GetString ());
    Proto_FlushNotes (doc);
    state.relocateHost.clear ();
    state.reconnectAfterClose = true;
    if (doc.m_iConnectPhase != eConnectNotConnected)
      doc.Disconnect ();
    Proto_Tick (doc);
    return true;
    }

  return false;
  }

// ---------------------------------------------------------------- the guard

bool Proto_NegotiationAllowed (CMUSHclientDoc & doc, const unsigned char option)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.guard.Allow (option, state.Now ()))
    return true;

  if (!state.bGuardNoted)
    {
    state.bGuardNoted = true;
    Proto_Note (doc, CFormat ("This server keeps renegotiating telnet option %i, which "
                              "looks like a loop. Ignoring it for 10 seconds at a time.",
                              (int) option).GetString ());
    }
  return false;
  }

// See the comment where this is called, in telnet_phases.cpp.
bool Proto_ShouldRerunHandshake (CProtocolState & state, uint64_t * lastRun)
  {
  const uint64_t now = state.Now ();
  if (*lastRun && now - *lastRun < proto::NegotiationGuard::kWindowMs)
    return false;
  *lastRun = now;
  return true;
  }

// ---------------------------------------------------------------- MNES

static unsigned int WorldCodePageNumber (CMUSHclientDoc & doc)
  {
#ifdef _WIN32
  return WorldCodePage (&doc);
#else
  (void) doc;
  return 1252;
#endif
  }

std::vector <std::string> Proto_WorldCharsetNames (CMUSHclientDoc & doc)
  {
  return proto::CharsetNames (doc.m_bUTF_8 != 0, WorldCodePageNumber (doc));
  }

int Proto_MttsBits (CMUSHclientDoc & doc)
  {
  // ANSI=1, 256 COLORS=8, TRUECOLOR=256 (see telnet_phases.cpp for what these claim),
  // MNES=512.
  int bits = 1 | 8 | 256 | 512;
  if (doc.m_bUTF_8)
    bits |= 4;
  if (doc.m_bMttsScreenReader)
    bits |= 64;
  if (doc.m_bSSL_Connected)
    bits |= 2048;
  return bits;
  }

// The name this client goes by.  terminal_identification exists so a player can send
// something else, and that holds here too.
static std::string ClientName (CMUSHclientDoc & doc)
  {
  if (doc.m_strTerminalIdentification.IsEmpty () ||
      doc.m_strTerminalIdentification.CompareNoCase ("mushclient") == 0)
    return "MUSHclient";
  return (const char *) doc.m_strTerminalIdentification;
  }

void Proto_HandleNewEnviron (CMUSHclientDoc & doc)
  {
  proto::EnvVars vars;
  const auto add = [&vars] (const char * name, const std::string & value)
    {
    vars.push_back (std::make_pair (std::string (name), value));
    };

  add ("CLIENT_NAME", ClientName (doc));
  add ("CLIENT_VERSION", (const char *) MUSHCLIENT_VERSION);
  add ("CHARSET", proto::MnesCharsetName (doc.m_bUTF_8 != 0, WorldCodePageNumber (doc)));
  add ("MTTS", std::to_string (Proto_MttsBits (doc)));
  add ("TERMINAL_TYPE", "ANSI");

  // Beyond MNES, as Mudlet sends them: what this client can do, so a server need not
  // decode the MTTS bits or guess.  SCREEN_READER is 0 unless the player has opted in.
  add ("ANSI", "1");
  add ("256_COLORS", "1");
  add ("TRUECOLOR", "1");
  add ("UTF-8", doc.m_bUTF_8 ? "1" : "0");
  add ("TLS", doc.m_bSSL_Connected ? "1" : "0");
  add ("OSC_HYPERLINKS", doc.m_bOsc8Links ? "1" : "0");
  add ("OSC_HYPERLINKS_SEND", doc.m_bOsc8Links ? "1" : "0");
  add ("OSC_HYPERLINKS_PROMPT", doc.m_bOsc8Links ? "1" : "0");
  add ("SCREEN_READER", doc.m_bMttsScreenReader ? "1" : "0");
  add ("WORD_WRAP", std::to_string ((int) doc.m_nWrapColumn));

  const std::string reply = proto::BuildEnvironReply (doc.m_IAC_subnegotiation_data, vars);
  if (reply.empty ())
    return;

  std::string packet;
  packet += (char) IAC;
  packet += (char) SB;
  packet += (char) proto::kTelOptNewEnviron;
  packet += reply;
  packet += (char) IAC;
  packet += (char) SE;
  doc.SendPacket (packet.data (), (int) packet.size ());
  }

// ---------------------------------------------------------------- MSSP

void Proto_HandleMssp (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (!doc.m_bMsspTlsHint || doc.m_bUseSSL || doc.m_bSSL_Connected || state.bTlsOffered)
    return;

  const int port = proto::FindMsspTlsPort (doc.m_IAC_subnegotiation_data);
  if (!port)
    return;

  state.bTlsOffered = true;

  CProtocolNote note;
  note.text = (const char *) CFormat ("This server also offers an encrypted connection, on port %i.", port);
  note.action = Proto_ActionLink ("tls_switch", std::to_string (port));
  note.linkText = "Reconnect with TLS";
  note.action2 = Proto_ActionLink ("tls_never", "");
  note.linkText2 = "Don't offer again";
  Proto_Note (doc, note);
  }

// ---------------------------------------------------------------- OSC 8
//
// ESC ] 8 ; params ; URI ST opens a link, and ESC ] 8 ; ; ST closes it.  ST is BEL,
// ESC \ or a C1 byte.  A relayed ESC can forge one of these, so what they can do is
// narrow: http and https, as any MXP link; send: and prompt:, which say on hover what
// they would send and then go the way MXP's do.  Anything else leaves the text plain.
//
// Malformed sequences must not eat text.  A URI has no spaces or control characters,
// so one of those ends it: what was taken for a URI goes back as text, as does anything
// over the length cap.  A newline ends it the same way (doc_shared.cpp's loop).

static void OpenLink (CMUSHclientDoc & doc, const proto::OscLinkKind kind, const std::string & action)
  {
  CProtocolState & state = *doc.m_pProto;

  if (!doc.m_pCurrentLine || doc.m_pCurrentLine->styleList.IsEmpty ())
    return;

  CStyle * pStyle = doc.m_pCurrentLine->styleList.GetTail ();

  COLORREF fore, back;
  doc.GetStyleRGB (pStyle, fore, back);

  state.linkSavedFlags = pStyle->iFlags & STYLE_BITS;
  state.linkSavedFore = pStyle->iForeColour;
  state.linkSavedBack = pStyle->iBackColour;

  unsigned short flags = (unsigned short) (pStyle->iFlags & STYLE_BITS & ~ACTIONTYPE);
  COLORREF newFore = pStyle->iForeColour;
  COLORREF newBack = pStyle->iBackColour;

  CString strHint;
  switch (kind)
    {
    case proto::eOscLinkSend:
      flags |= ACTION_SEND;
      strHint = CFormat ("Send to the MUD: %s", action.c_str ());
      break;
    case proto::eOscLinkPrompt:
      flags |= ACTION_PROMPT;
      strHint = CFormat ("Put in the command window: %s", action.c_str ());
      break;
    default:
      flags |= ACTION_HYPERLINK;     // the hover shows the URL
      break;
    }

  if (doc.m_bUnderlineHyperlinks)
    flags |= UNDERLINE;

  state.bLinkColoured = false;
  if (doc.m_bUseCustomLinkColour)
    {
    flags &= ~COLOURTYPE;
    flags |= COLOUR_RGB;
    newFore = doc.m_iHyperlinkColour;
    newBack = back;
    state.bLinkColoured = true;
    state.linkFore = newFore;
    state.linkBack = newBack;
    }

  doc.AddStyle (flags, newFore, newBack, 0, CString (action.c_str ()), strHint, "");
  state.bLinkOpen = true;
  }

void Proto_OscCloseLink (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (!state.bLinkOpen)
    return;
  state.bLinkOpen = false;

  if (!doc.m_pCurrentLine || doc.m_pCurrentLine->styleList.IsEmpty ())
    return;

  CStyle * pStyle = doc.m_pCurrentLine->styleList.GetTail ();

  // Keep what the server has changed since the link opened: a colour it set inside
  // the link is its business.  Undo only what the link itself added.
  unsigned short flags = (unsigned short) (pStyle->iFlags & STYLE_BITS & ~ACTIONTYPE);
  COLORREF fore = pStyle->iForeColour;
  COLORREF back = pStyle->iBackColour;

  if (!(state.linkSavedFlags & UNDERLINE))
    flags &= ~UNDERLINE;

  if (state.bLinkColoured && (flags & COLOURTYPE) == COLOUR_RGB &&
      fore == state.linkFore && back == state.linkBack)
    {
    flags &= ~COLOURTYPE;
    flags |= state.linkSavedFlags & COLOURTYPE;
    fore = state.linkSavedFore;
    back = state.linkSavedBack;
    }

  doc.AddStyle (flags, fore, back, 0, (CAction *) NULL);
  }

// Give up on what looked like OSC 8: the part that was taken for a URI is text after all.
static void OscGiveBack (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  const std::string payload = state.osc;
  state.osc.clear ();
  const size_t sep = payload.find (';', 2);
  if (sep != std::string::npos && sep + 1 < payload.size () && doc.m_pCurrentLine)
    doc.AddToLine (payload.c_str () + sep + 1, 0);
  }

static void OscFinished (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  const std::string payload = state.osc;
  state.osc.clear ();

  if (payload.compare (0, 2, "8;") != 0)
    return;                      // a title or a palette: nothing we do anything with

  const size_t sep = payload.find (';', 2);
  if (sep == std::string::npos)
    return;

  const std::string uri = payload.substr (sep + 1);

  Proto_OscCloseLink (doc);     // links do not nest
  if (uri.empty ())
    return;

  std::string action;
  const proto::OscLinkKind kind = proto::ClassifyOsc8Target (uri, &action);
  if (kind != proto::eOscLinkNone)
    OpenLink (doc, kind, action);
  }

void Proto_OscByte (CMUSHclientDoc & doc, const unsigned char c)
  {
  CProtocolState & state = *doc.m_pProto;

  // BEL ends it.  So does ST as a single C1 byte, except in UTF-8, where 0x9C is the
  // tail of ordinary characters.  (ESC never gets here: see Proto_OscInterrupted.)
  if (c == '\007' || (c == 0x9C && !doc.m_bUTF_8))
    {
    OscFinished (doc);
    doc.m_phase = NONE;
    return;
    }

  state.oscCount++;

  // Is this an OSC 8?  Decided by its first two bytes.  Anything else is only counted.
  if (state.oscCount == 1)
    {
    if (c == '8' && doc.m_bOsc8Links)
      state.osc = "8";
    }
  else if (state.oscCount == 2 && !state.osc.empty ())
    {
    if (c == ';')
      state.osc = "8;";
    else
      state.osc.clear ();
    }
  else if (!state.osc.empty ())
    {
    const bool bInUri = state.osc.find (';', 2) != std::string::npos;
    const bool bBad = bInUri ? (c <= ' ' || c == 0x7F) : (c < ' ' || c == 0x7F);
    const bool bTooLong = state.osc.size () >= (bInUri ? proto::kOsc8MaxPayload
                                                       : proto::kOsc8MaxParams + 2);
    if (bBad || bTooLong)
      {
      OscGiveBack (doc);
      doc.m_phase = NONE;
      state.bOscReprocess = true;    // this byte is text as well
      return;
      }
    state.osc += (char) c;
    }
  else if (state.oscCount > proto::kOscMaxPayload)
    {
    doc.m_phase = NONE;              // no terminator is coming
    state.bOscReprocess = true;
    }
  }

// \n, \r, ESC or IAC arrived inside an OSC.  ESC is the start of the ST form of
// terminator, and terminates it whatever follows, as terminals do.  The others mean it
// was never going to be finished.
void Proto_OscInterrupted (CMUSHclientDoc & doc, const unsigned char c)
  {
  if (c == ESC)
    OscFinished (doc);
  else
    OscGiveBack (doc);
  }

// ---------------------------------------------------------------- the timer

void Proto_Tick (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.reconnectAfterClose && doc.m_iConnectPhase == eConnectNotConnected)
    {
    state.reconnectAfterClose = false;
    doc.ConnectSocket ();
    }

  Proto_MspTick (doc);
  Proto_FlushNotes (doc);
  }

// The connection is gone.  A world that moved at a server's word goes back to the
// address the player gave it, unless it is moving now.
void Proto_ConnectionClosed (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  Proto_MspStopAll (doc);
  state.bLinkOpen = false;

  if (state.bRelocated && !state.reconnectAfterClose)
    {
    state.bRelocated = false;
    doc.m_server = state.relocateOriginalServer.c_str ();
    doc.m_port = state.relocateOriginalPort;
    }
  }
