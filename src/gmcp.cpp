// gmcp.cpp -- a helper for plugins that want GMCP without each having to handle the
// telnet option itself.  Additive: a plugin using OnPluginTelnetRequest,
// OnPluginTelnetSubnegotiation and SendPkt sees exactly what it always did.
//
// The rules, each there because something already exists that must not break:
//
//   1. Plugins are asked first.  If one says yes to WILL, the handshake is its own: the
//      client sends no Core.Hello and no Core.Supports.Set.  (Quow's does this.)
//   2. With no plugin and no subscriber the answer is DONT, as it always was.
//   3. The client only ever sends Core.Supports.Add, never Set, which would replace what
//      a plugin asked for.  And only after the SENT_DO chain, for the same reason.
//   4. A plugin's Set replaces earlier lists, including ours, so if one goes out we send
//      Add again after it.
//   5. Raw callbacks are unchanged.
//
// Names keep the spelling the subscriber used, since Discworld's are lower case and
// IRE's are not, and match without regard to case when a message arrives.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "protocols.h"
#include "version.h"
#include "scripting/errors.h"

namespace {

// Packets of ours going out, so the watch on outgoing data does not react to them.
void SendGmcpText (CMUSHclientDoc & doc, const std::string & text)
  {
  CProtocolState & state = *doc.m_pProto;
  const std::string packet = proto::BuildGmcpPacket (text);
  const bool bSaved = state.bGmcpSendingOurs;
  state.bGmcpSendingOurs = true;
  doc.SendPacket (packet.data (), (int) packet.size ());
  state.bGmcpSendingOurs = bSaved;
  }

std::string SupportsItem (const CGmcpSubscription & subscription)
  {
  return subscription.version.empty () ? subscription.package
                                       : subscription.package + " " + subscription.version;
  }

// The server is talking GMCP to us, and we can talk back.
bool GmcpFlowing (CMUSHclientDoc & doc)
  {
  return doc.m_iConnectPhase == eConnectConnectedToMud &&
         doc.m_bClient_sent_IAC_DO [proto::kTelOptGmcp];
  }

void SendAddForAll (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  if (state.gmcpSubs.empty () || !GmcpFlowing (doc))
    return;
  std::vector <std::string> items;
  for (const auto & item : state.gmcpSubs)
    items.push_back (SupportsItem (item.second));
  SendGmcpText (doc, proto::BuildGmcpSupports ("Add", items));
  }

void SendCoreHello (CMUSHclientDoc & doc)
  {
  std::string name = "MUSHclient";
  if (!doc.m_strTerminalIdentification.IsEmpty () &&
      doc.m_strTerminalIdentification.CompareNoCase ("mushclient") != 0)
    name = (const char *) doc.m_strTerminalIdentification;

  // what JSON cannot have raw in a string
  std::string safeName;
  for (const unsigned char c : name)
    if (c >= ' ' && c != '"' && c != '\\')
      safeName += (char) c;

  SendGmcpText (doc, std::string ("Core.Hello { \"client\": \"") + safeName +
                     "\", \"version\": \"" + (const char *) MUSHCLIENT_VERSION + "\" }");
  }

// Our own handshake, when no plugin has one.
void CoreHandshake (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  doc.Send_IAC_DO (proto::kTelOptGmcp);
  state.bGmcpCoreHandshook = true;
  SendCoreHello (doc);
  SendAddForAll (doc);
  }

// The ID the caller goes by: its plugin's, or "" for the world's own script.
std::string CallerID (CMUSHclientDoc & doc)
  {
  return doc.m_CurrentPlugin ? (const char *) doc.m_CurrentPlugin->m_strID : "";
  }

void ForgetOwner (CMUSHclientDoc & doc, const std::string & key, const std::string & owner)
  {
  CProtocolState & state = *doc.m_pProto;

  const auto it = state.gmcpSubs.find (key);
  if (it == state.gmcpSubs.end ())
    return;

  it->second.owners.erase (owner);
  if (!it->second.owners.empty ())
    return;

  const std::string item = it->second.package;
  state.gmcpSubs.erase (it);

  if (GmcpFlowing (doc))
    SendGmcpText (doc, proto::BuildGmcpSupports ("Remove", std::vector <std::string> (1, item)));
  }

} // anonymous namespace

// ---------------------------------------------------------------- from the server

void Proto_GmcpWill (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  const unsigned char c = proto::kTelOptGmcp;

  state.bGmcpWillSeen = true;

  const bool bAlreadyAgreed = doc.m_bClient_sent_IAC_DO [c];

  // Rule 1.  The same two questions the generic path asks, in the same order.
  if (doc.Handle_Telnet_Request (c, "WILL"))
    {
    state.bGmcpPluginClaimed = true;
    doc.Send_IAC_DO (c);

    if (!bAlreadyAgreed || Proto_ShouldRerunHandshake (state, &state.lastSentDo [c]))
      {
      if (!bAlreadyAgreed)
        state.lastSentDo [c] = state.Now ();

      state.bGmcpInSentDoChain = true;
      doc.Handle_Telnet_Request (c, "SENT_DO");
      state.bGmcpInSentDoChain = false;

      // Rule 3.  After it, so a Set in the chain comes before our Add.
      state.bGmcpResendAdd = false;
      SendAddForAll (doc);
      }
    return;
    }

  // Rule 2.
  if (state.gmcpSubs.empty ())
    {
    doc.Send_IAC_DONT (c);
    return;
    }

  if (bAlreadyAgreed)
    {
    // A repeated WILL with our handshake already done: the copy-over case, as above.
    if (Proto_ShouldRerunHandshake (state, &state.lastSentDo [c]))
      {
      SendCoreHello (doc);
      SendAddForAll (doc);
      }
    return;
    }

  state.lastSentDo [c] = state.Now ();
  CoreHandshake (doc);
  }

void Proto_GmcpWont (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  state.bGmcpWillSeen = false;
  state.bGmcpPluginClaimed = false;
  state.bGmcpCoreHandshook = false;
  }

void Proto_GmcpReceived (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  std::string package, rawText;
  proto::SplitGmcp (doc.m_IAC_subnegotiation_data, &package, &rawText);
  if (package.empty ())
    return;

  // Kept whoever did the handshake.  A lot of distinct names from a server that means
  // no harm is a few hundred, so this cap is only ever reached on purpose.
  const std::string key = proto::Lower (package);
  if (state.gmcpLast.size () < 256 || state.gmcpLast.count (key))
    state.gmcpLast [key] = rawText;

  // OnPluginGMCP (package, rawText).  To a plugin with a subscription that covers this,
  // and to one with none at all, which has said nothing about what it wants.
  CPlugin * pSavedPlugin = doc.m_CurrentPlugin;
  const bool bSavedNotes = doc.m_bNotesNotWantedNow;
  doc.m_bNotesNotWantedNow = true;

  for (PluginListIterator pit = doc.m_PluginList.begin (); pit != doc.m_PluginList.end (); ++pit)
    {
    CPlugin * pPlugin = *pit;
    if (!pPlugin->m_bEnabled)
      continue;

    CScriptDispatchID & dispid = pPlugin->m_PluginCallbacks [ON_PLUGIN_GMCP];
    if (!dispid.isvalid ())
      continue;

    const std::string owner = (const char *) pPlugin->m_strID;
    bool bHasAny = false, bCovered = false;
    for (const auto & item : state.gmcpSubs)
      if (item.second.owners.count (owner))
        {
        bHasAny = true;
        if (proto::GmcpPackageMatches (item.second.package, package))
          bCovered = true;
        }
    if (bHasAny && !bCovered)
      continue;

    doc.m_CurrentPlugin = pPlugin;
    CScriptCallInfo callinfo (ON_PLUGIN_GMCP, dispid);
    pPlugin->ExecutePluginScript (callinfo, package.c_str (), rawText.c_str ());
    doc.m_CurrentPlugin = pSavedPlugin;
    }

  doc.m_bNotesNotWantedNow = bSavedNotes;
  }

// ---------------------------------------------------------------- to the server

void Proto_GmcpWatchSent (CMUSHclientDoc & doc, const char * data, const int length)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.gmcpSubs.empty () || state.bGmcpSendingOurs || length < 20)
    return;

  if (!proto::SendsGmcpSupportsSet (data, (size_t) length))
    return;

  // Rule 4.  During a SENT_DO chain the Add comes at the end of the chain anyway.
  if (state.bGmcpInSentDoChain)
    return;

  SendAddForAll (doc);
  }

void Proto_GmcpPluginGone (CMUSHclientDoc & doc, const std::string & pluginID)
  {
  CProtocolState & state = *doc.m_pProto;

  std::vector <std::string> keys;
  for (const auto & item : state.gmcpSubs)
    if (item.second.owners.count (pluginID))
      keys.push_back (item.first);

  for (const std::string & key : keys)
    ForgetOwner (doc, key, pluginID);
  }

bool Proto_GmcpLast (CMUSHclientDoc & doc, const std::string & package, std::string * rawText)
  {
  CProtocolState & state = *doc.m_pProto;
  const auto it = state.gmcpLast.find (proto::Lower (package));
  if (it == state.gmcpLast.end ())
    return false;
  *rawText = it->second;
  return true;
  }

// ---------------------------------------------------------------- the world methods

// world.GMCP_Subscribe (package [, version]).  Calling it twice from one plugin is once.
long CMUSHclientDoc::GMCP_Subscribe (LPCTSTR Package, LPCTSTR Version)
  {
  CProtocolState & state = *m_pProto;

  const std::string package = Package;
  if (!proto::IsValidGmcpPackage (package))
    return eBadParameter;

  const std::string version = Version ? Version : "";
  for (const unsigned char c : version)
    if (!proto::IsAsciiAlnum (c) && c != '.')
      return eBadParameter;

  const std::string key = proto::Lower (package);
  const bool bNew = state.gmcpSubs.find (key) == state.gmcpSubs.end ();

  CGmcpSubscription & subscription = state.gmcpSubs [key];
  if (bNew)
    {
    subscription.package = package;
    subscription.version = version;
    }
  subscription.owners.insert (CallerID (*this));

  if (!bNew || m_iConnectPhase != eConnectConnectedToMud)
    return eOK;

  // In the chain the Add comes at the end of it.
  if (state.bGmcpInSentDoChain)
    return eOK;

  if (m_bClient_sent_IAC_DO [proto::kTelOptGmcp])
    {
    std::vector <std::string> items (1, SupportsItem (subscription));
    SendGmcpText (*this, proto::BuildGmcpSupports ("Add", items));
    }
  // The server offered GMCP before there was anyone to want it, and heard DONT.  Now
  // there is: a DO, late, is still a DO.
  else if (state.bGmcpWillSeen)
    {
    state.lastSentDo [proto::kTelOptGmcp] = state.Now ();
    CoreHandshake (*this);
    }

  return eOK;
  }

long CMUSHclientDoc::GMCP_Unsubscribe (LPCTSTR Package)
  {
  const std::string package = Package;
  if (!proto::IsValidGmcpPackage (package))
    return eBadParameter;
  ForgetOwner (*this, proto::Lower (package), CallerID (*this));
  return eOK;
  }

// world.GMCP_Send (package, json)
long CMUSHclientDoc::GMCP_Send (LPCTSTR Package, LPCTSTR Json)
  {
  const std::string package = Package;
  if (!proto::IsValidGmcpPackage (package))
    return eBadParameter;

  if (m_iConnectPhase != eConnectConnectedToMud)
    return eWorldClosed;

  if (!m_bClient_sent_IAC_DO [proto::kTelOptGmcp])
    return eOptionOutOfRange;      // not agreed with the server, so it would not understand

  std::string text = package;
  if (Json && Json [0])
    {
    text += " ";
    text += Json;
    }

  // Through the same door as SendPkt, so a Set sent like this is watched for too.
  const std::string packet = proto::BuildGmcpPacket (text);
  SendPacket (packet.data (), (int) packet.size ());
  return eOK;
  }

// world.GMCP_Get (package): the raw text of the last message for it, or NULL.
VARIANT CMUSHclientDoc::GMCP_Get (LPCTSTR Package)
  {
  VARIANT vaResult;
  VariantInit (&vaResult);
  vaResult.vt = VT_NULL;

  std::string rawText;
  if (Proto_GmcpLast (*this, Package, &rawText))
    SetUpVariantString (vaResult, rawText.c_str ());

  return vaResult;
  }
