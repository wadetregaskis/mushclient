// chatsock.cpp : implementation file
//

// But does it get goat's blood out?



#include "stdafx.h"
#include "MUSHclient.h"

#include "mainfrm.h"

#include "doc.h"

// The handshake's decisions (prefix -> dialect, the name/address splits, the
// ON_PLUGIN_CHAT_ACCEPT veto) are restated here so they can be tested without
// a socket. This is no longer the only way to reach them: the chat family
// adoption (docs/STUB-ADOPTION-2026-08.md §5 step 2) put chatsock.cpp itself
// into mushcore's CORE_SOURCES, so OnReceive below is the real state machine
// in both builds, not an empty stub.
#include "pal/chat_protocol.h"
#include "pal/path_safety.h"
#include "pal/itransport.h"
#ifdef _WIN32
#include "pal/win_transport.h"
#else
#include "platform/posix_transport.h"
#endif

#include <stddef.h>
#include <set>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

#define CHAT_DEBUG 0

// pal/chat_protocol.h restates these rather than including chatsock.h (it must
// stay MFC-free), so pin the two enumerations equal here.
static_assert ((int) pal::kChatMudMaster == (int) eChatMudMaster,
               "pal::kChatMudMaster must match chatsock.h's eChatMudMaster");
static_assert ((int) pal::kChatZMud == (int) eChatZMud,
               "pal::kChatZMud must match chatsock.h's eChatZMud");
static_assert ((int) pal::kChatFileBlockSize == 500 &&
               (int) pal::kZChatFileBlockSize == 1024,
               "the chat file block sizes are protocol, not tunables");

// ── ChatAppendBytes ─────────────────────────────────────────────────────────
// Append n bytes to a CString, NULs and all.
//
// `dest += src` cannot be used for chat traffic.  Modern ATL's
// CSimpleStringT::Append (atlsimpstr.h) starts with
//
//     //Make sure we don't read pass end of the terminating NULL
//     nLength = StringLengthN(pszSrc, nLength);
//
// so it silently clamps an append to the first NUL even when the caller passed
// an explicit, correct length.  Every route into it -- operator+=,
// Append(const CSimpleStringT&), Append(ptr, len) -- clamps.
//
// That is fatal for zChat, whose framing is 2 bytes of little-endian command
// followed by 2 bytes of little-endian length: for every command below 256 the
// SECOND BYTE IS A NUL.  Measured on Windows 2026-08-09, a 20-byte
// CHAT_VERSION frame reached the wire as one byte, and the same for
// CHAT_STATUS and CHAT_STAMP -- so a zChat peer received a stream of stray
// first bytes and no complete message in either direction.  A MUSHclient 5.06
// binary, built against an older MFC whose Append was a plain memcpy, sends all
// three frames intact; this is a toolchain regression rather than anything the
// port did.  It bites file transfers too, where a block of an ordinary binary
// file is very likely to contain a NUL.
//
// CString's other operations are safe: SetString(ptr, len) -- which is what the
// (ptr, len) constructor, Mid and Left all use -- memcpys the length it is
// given.  So only appending needs this, and it is expressed with those safe
// primitives so both platforms' CString take the same path.
//
// ...and without copying what is already there.  This used to build a
// std::string of dest plus the new bytes and assign it back: two copies of the
// whole buffer per append, so a peer feeding the 1 MB input cap 999 bytes at a
// time cost about a gigabyte of copying.  ATL's GetBuffer grows the buffer
// geometrically (by half again each time) and ReleaseBuffer with an explicit
// length takes it as given, NULs and all, so appending is amortised to the
// bytes appended.  The port's CString is a std::string whose += is binary-safe
// and amortised already, while its GetBuffer copies the lot into a scratch
// buffer -- so each platform takes the path that is cheap on it.
static void ChatAppendBytes (CString & dest, const char * pSrc, const int nLength)
  {
  if (pSrc == NULL || nLength <= 0)
    return;

#ifdef _WIN32
  const int iOldLength = dest.GetLength ();
  char * p = dest.GetBuffer (iOldLength + nLength);
  memcpy (p + iOldLength, pSrc, (size_t) nLength);
  dest.ReleaseBuffer (iOldLength + nLength);
#else
  dest += CString (pSrc, nLength);
#endif
  }   // end of ChatAppendBytes

// A file (or anything) of this name exists already.
static bool FileExists (const CString & strName)
  {
  CFileStatus status;
  return CFile::GetStatus (strName, status) != FALSE;
  }

// A file name a peer offers that can be saved as itself: not empty or dots
// only, no stream (":") or other characters Windows refuses, and not a
// device's name, with or without an extension, which Windows takes for the
// device wherever the folder is.  pal::IsWindowsDeviceName has the list: it
// used to stop at COM1-9/LPT1-9, so "COM0", "LPT0", "CONIN$" and "COM" with a
// superscript digit (byte B9, COM¹, which Windows maps to COM1) got through.
bool IsSafeChatFileName (const CString & strName)
  {
  if (strName.IsEmpty () || strName.SpanIncluding (_T(". ")) == strName)
    return false;
  if (strName.FindOneOf (_T(":*?\"<>|")) != -1)
    return false;
  for (int i = 0; i < strName.GetLength (); i++)
    if ((unsigned char) strName [i] < ' ')
      return false;
  if (pal::IsWindowsDeviceName (std::string (strName.GetString (),
                                             (size_t) strName.GetLength ())))
    return false;
  return true;
  }

IMPLEMENT_DYNAMIC(CChatSocket, CAsyncSocket)

CChatSocket::CChatSocket(CMUSHclientDoc* pDoc)
{
	m_pDoc = pDoc;
  m_pResolver = NULL;
  m_pResolverEvents = NULL;
  m_iServerPort = 0;
  m_bDeleteMe = false;  
  m_bIncoming = false;
  m_bIgnore = false;
  m_bCanSnoop = false;          
  m_bYouAreSnooping = false;
  m_bHeIsSnooping = false;
  m_bCanSendCommands = false;   
  m_bPrivate = false;   
  m_bCanSendFiles = false;
  m_bDoingFileTransfer = false;
  m_bWasConnected = false;
  m_iChatStatus = eChatClosed;
  m_iChatConnectionType = eChatMudMaster;
  m_iUserOption = 0;

  // session ID

  if (++m_pDoc->m_iNextChatID > LONG_MAX)
    m_pDoc->m_iNextChatID = 1;    // wrap around
  
  m_iChatID = m_pDoc->m_iNextChatID;  // unique session ID

  // zChat

  SHS_INFO shsInfo;
  MakeRandomNumber (m_pDoc, shsInfo);

  m_zChatStamp = shsInfo.digest [0];
  m_zChatStatus = 1;      // normal status

  // times

  m_tWhenStarted = CTime::GetCurrentTime();
  m_tLastIncoming = 0;
  m_tLastOutgoing = 0;
  m_iPingStartTime.QuadPart = 0;
  m_fLastPingTime = 0.0;

  // their address, port

  m_iAllegedPort = DEFAULT_CHAT_PORT;
  m_strAllegedAddress = "<Unknown>";

  // file stuff

  m_bDoingFileTransfer = false;  
  m_bSendFile = false;           
  m_iFileSize = 0;           
  m_iFileBlocks = 0;         
  m_iBlocksTransferred = 0;  
  m_pFile = NULL;  
  m_pFileBuffer = NULL;
  m_tStartedFileTransfer = 0;
  m_iFileBlockSize = pal::kChatFileBlockSize;   // 500; zChat raises it at the handshake

  // zero counters

  m_iCountIncomingPersonal = 0; 
  m_iCountIncomingAll = 0;      
  m_iCountIncomingGroup = 0;    
  m_iCountOutgoingPersonal = 0; 
  m_iCountOutgoingAll = 0;      
  m_iCountOutgoingGroup = 0;    
  m_iCountMessages = 0;
  m_iCountFileBytesIn = 0;   
  m_iCountFileBytesOut = 0;  
  
  }

CChatSocket::~CChatSocket()
{

  StopFileTransfer (true);

  // cancel pending host name lookup (destroying the resolver drops it)
  delete m_pResolver;
  delete m_pResolverEvents;

  ShutDownSocket (*this);

  // if he was ever connected, we will tell our plugins he has gone

  if (m_bWasConnected)
    {

    // tell each plugin about the departing user
    m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_CHAT_USERDISCONNECT, 
                                      m_iChatID,     // user ID
                                      string (m_strRemoteUserName),
                                      false, false);
    }   // end of needing to notify about him
}

void CChatSocket::StopFileTransfer (const bool bAbort)
  {
  if (!m_bDoingFileTransfer || m_pDoc == NULL)
    return;

  delete m_pFile;     // close file
  m_pFile = NULL;  

  delete [] m_pFileBuffer;
  m_pFileBuffer = NULL;

  if (bAbort)
    {
    // tell them to cancel it
    if (m_iChatStatus == eChatConnected)
      SendChatMessage (CHAT_FILE_CANCEL, "");

    // half-received - delete it
    if (m_bSendFile)
      m_pDoc->ChatNote (eChatFile,
              CFormat (_T("Aborted sending file %s"), (LPCTSTR) m_strOurFileName));
    else
      {
      m_pDoc->ChatNote (eChatFile,
              CFormat (_T("Aborted receiving file %s"), (LPCTSTR) m_strOurFileName));
      // CFile::Remove throws when the delete fails -- a peer's file named
      // "NUL" opens the device, which cannot then be deleted -- and from the
      // destructor that ended the program (std::terminate), from OnReceive it
      // skipped the reset below and left a transfer "running" with no file.
      try
        {
        CFile::Remove (m_strOurFileName);
        m_pDoc->ChatNote (eChatFile,
                CFormat (_T("File %s deleted."), (LPCTSTR) m_strOurFileName));
        }
      catch (CException * e)
        {
        e->Delete ();
        m_pDoc->ChatNote (eChatFile,
                CFormat (_T("File %s could not be deleted."), (LPCTSTR) m_strOurFileName));
        }
      }
    }

  m_bDoingFileTransfer = false;  
  m_bSendFile = false;           
  m_iFileSize = 0;           
  m_iFileBlocks = 0;         
  m_iBlocksTransferred = 0;  
  m_tStartedFileTransfer = 0;

  } // end of CChatSocket::StopFileTransfer 


void CChatSocket::OnReceive(int nErrorCode)
{

char buff [1000];
int count = Receive (buff, sizeof (buff) - 1);

  if (count == SOCKET_ERROR)
    {
    OnClose (GetLastError ());
    return;
    }

  if (count <= 0)
    return;

  m_tLastIncoming = CTime::GetCurrentTime();

  switch (m_iChatStatus)
    {
    case eChatAwaitingConnectConfirm:
      {

#if CHAT_DEBUG

      m_pDoc->Note (TFormat (_T("Incoming packet on %i: \"%s\""),
                    m_iChatID,
                    (LPCTSTR) CString (buff, count)));

#endif

      const pal::ChatAcceptance acceptance = pal::parseChatAcceptance (buff, count);

      if (!acceptance.ok)
        {
        m_pDoc->ChatNote (eChatConnection,
                _T("Server rejected chat session attempt."));
        OnClose (0);
        return;
        } // end of not being accepted

      m_strRemoteUserName = CString (acceptance.remoteUserName.data (),
                                     (int) acceptance.remoteUserName.size ());

      m_pDoc->ChatNote (eChatSession,
                CFormat (_T("Chat session accepted, remote server: \"%s\""),
              (LPCTSTR) m_strRemoteUserName));

      // get rid of the acceptance message, leaving any chat data behind it
      memmove (buff, &buff [acceptance.consumed], count - acceptance.consumed);
      count -= acceptance.consumed;

      // connected now
      m_iChatStatus = eChatConnected;
      m_bWasConnected = true;

      // identify ourselves
      CString strVersion = "MUSHclient v";
      strVersion += MUSHCLIENT_VERSION;
      SendChatMessage (CHAT_VERSION, strVersion);
      
      if (m_iChatConnectionType == eChatZMud)
        {
        SendChatMessage (CHAT_STATUS, (char) 1);  // normal status
        SendChatMessage (CHAT_STAMP, MakeStamp (m_zChatStamp));  // send it
        }

      // tell each plugin about the new user
      m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_CHAT_NEWUSER, 
                                        m_iChatID,     // user ID
                                        string (m_strRemoteUserName),
                                        false, 
                                        false);

      if (count <= 0)
        return;       // only the negotiation text here

      }
     break;   // end of eChatAwaitingConnectConfirm

    case eChatAwaitingConnectionRequest:
      // The caller's request, and our answer to it: ProcessConnectionRequest.
      ProcessConnectionRequest (buff, count);
      return;
    } // end of switch on m_iChatStatus

// now take the incoming text and break into blocks

  if (m_bDeleteMe)
    return;       // rejected or vetoed: nothing more of theirs to keep

  ChatAppendBytes (m_outstanding_input, buff, count); // add to any left over

  // Nothing legitimate leaves this much unparsed -- a zChat frame is at most
  // 64 KB, MudMaster's a line -- and with no limit a peer could grow it (each
  // append copying the lot) until the client froze: one NUL before
  // MudMaster's terminator wedges its framing for good.
  if (m_outstanding_input.GetLength () > 1024 * 1024)
    {
    m_pDoc->ChatNote (eChatConnection,
                TFormat ("Chat session to %s closed: too much unprocessed input.",
                          (LPCTSTR) m_strRemoteUserName));
    m_outstanding_input.Empty ();
    OnClose (0);
    return;
    }

  while (!m_bDeleteMe && !m_outstanding_input.IsEmpty ())
    {

    switch (m_iChatConnectionType)
      {

      case eChatMudMaster:
        {
        // file blocks are fixed length and might contain nulls

        if (m_outstanding_input [0] == CHAT_FILE_BLOCK)
          {
          int iLength = m_outstanding_input.GetLength ();

          // must have file block size + 2 (command and terminator)
          // otherwise we will get them later
          if (iLength < (m_iFileBlockSize + 2))
            return;

          // make a buffer of exactly the file data
          CString strBuffer = m_outstanding_input.Mid (1, m_iFileBlockSize);

          // discard the file block and message number and terminator byte
          m_outstanding_input = m_outstanding_input.Mid (m_iFileBlockSize + 2);

          // now process the incoming file block
          ProcessChatMessage (CHAT_FILE_BLOCK, strBuffer);

          }   // end of file block
        else
          {   // not a file block - use variable-length terminator

          // pal::chatFindChar, not CString::Find: MFC's Find(char) is _tcschr
          // and gives up at the first embedded NUL, while the port's
          // std::string-backed Find scans the whole length.  m_outstanding_input
          // deliberately carries NULs (see ChatAppendBytes above), so a peer that
          // puts one before the 0xFF terminator was framed here on this platform
          // and left the Windows client's buffer wedged forever.  Searching in
          // MFC's terms is what makes the two builds act on the same bytes --
          // the same conversion the handshake parser already had.
          int iTerminator = pal::chatFindChar (m_outstanding_input.GetString (),
                                               m_outstanding_input.GetLength (),
                                               (char) CHAT_END_OF_COMMAND);

          // if no terminator, wait for one to arrive in the next packet
          if (iTerminator == -1)
            return;   

          CString strChatMessage = m_outstanding_input.Left (iTerminator);
          m_outstanding_input = m_outstanding_input.Mid (iTerminator + 1);

          ProcessChatMessage ((unsigned char) strChatMessage [0], strChatMessage.Mid (1));
          }   // end of not file block
        } // end of MudMaster chat type
        break;

      case eChatZMud:
        {
        int iInputLength = m_outstanding_input.GetLength ();
        // must have 4 bytes (command:2, and length:2) or we cannot have a packet
        if (iInputLength < 4)
          return;    // wait for them

        int iCommand = (int) (unsigned char) m_outstanding_input [0] |
                       (((int) (unsigned char) m_outstanding_input [1]) << 8);

        int iLength = (int) (unsigned char) m_outstanding_input [2] |
                       (((int) (unsigned char) m_outstanding_input [3]) << 8);

        if (iInputLength < (iLength + 4))
          return;   // whole block has not arrived yet

        CString strChatMessage = m_outstanding_input.Mid (4, iLength);
        m_outstanding_input = m_outstanding_input.Mid (iLength + 4);

        ProcessChatMessage (iCommand, strChatMessage);

        } // end of zChat chat type
        break;

      } // end of switch on chat connection type

    }   // end of extracting messages from the incoming stream


}

// The number of accept prompts up, in every world: see ProcessConnectionRequest.
int CChatSocket::s_iAcceptPromptsUp = 0;

// An incoming caller's first packet: "CHAT:name\naddressport" or the zChat
// equivalent.  Decides whether to take the call -- the plugins, then the user
// if the world asks -- and answers YES or NO.
void CChatSocket::ProcessConnectionRequest (const char * buff, const int count)
  {

  // Nothing they send while their call is being asked about counts.  OnReceive
  // runs again inside the box's message loop, and this used to parse the new
  // packet before noticing the box: a caller could re-handshake under the
  // question, so the user said yes to "Fred" on MudMaster and got whatever
  // name, dialect, file block size and call-back address came second.  The
  // bytes were read (so FD_READ keeps coming) and are dropped, as the rest of a
  // request packet always has been.
  if (m_iInModal)
    return;

#if CHAT_DEBUG

  m_pDoc->Note (TFormat ("Incoming packet on %i: \"%s\"",
                m_iChatID,
                (LPCTSTR) CString (buff, count)));

#endif

  const pal::ChatRequest request = pal::parseChatConnectionRequest (buff, count);

  if (!request.ok)
    {
    m_pDoc->ChatNote (eChatConnection,
                  _T("Unexpected chat negotiation."));
    SendData ("NO");
    OnClose (0);
    return;
    } // end of not accepting it

  m_iChatConnectionType = request.connectionType;
  m_iFileBlockSize      = request.fileBlockSize;   // 1024 for zChat
  m_strRemoteUserName   = CString (request.remoteUserName.data (),
                                   (int) request.remoteUserName.size ());

  if (request.haveAllegedAddress)
    {
    m_iAllegedPort = request.allegedPort;
    m_strAllegedAddress = CString (request.allegedAddress.data (),
                                   (int) request.allegedAddress.size ());
    }

  // (anything after the request in this packet is discarded: we can't see how
  // it could reasonably be terminated)

  pal::ChatAcceptDecision decision = pal::chatAcceptAfterPlugins (
          m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_CHAT_ACCEPT,
                    pal::chatAcceptCallbackArgument (
                              pal::FormatAddress (m_ServerAddr).c_str (),
                              string (m_strRemoteUserName)).c_str (),
                    true),   // stop on false response
          m_pDoc->m_bValidateIncomingCalls);

  if (m_bDeleteMe)
    return;     // a plugin closed it

  if (decision == pal::eChatAcceptAsk)
    {
    // One accept question at a time, in the whole program.  Each box runs a
    // message loop in which the next caller's packet arrives and asked again,
    // so callers stacked boxes without limit -- and the listener's cap on
    // unidentified callers did not hold them back, because a caller whose box
    // is up has identified itself.  A second caller while one is being asked
    // about is turned away; it can call again.
    if (s_iAcceptPromptsUp > 0)
      {
      m_pDoc->ChatNote (eChatConnection,
                  TFormat ("Refused call from %s (%s): another call is waiting for an answer.",
                           (LPCTSTR) m_strRemoteUserName,
                           pal::FormatAddress (m_ServerAddr).c_str ()));
      SendData ("NO");
      OnClose (0);
      return;
      }

    m_pDoc->Activate ();
      {
      // The socket is kept while it is asked (m_iInModal: the reaper leaves
      // it, and OnReceive above ignores what it sends).
      ModalGuard guard (this);
      struct PromptCount
        {
        PromptCount () { s_iAcceptPromptsUp++; }
        ~PromptCount () { s_iAcceptPromptsUp--; }
        } prompt;
      decision = pal::chatAcceptAfterUser (::UMessageBox (
            TFormat ("Incoming chat call to world %s from %s, IP address: %s.\n\nAccept it?",
                     (LPCTSTR) m_pDoc->m_mush_name,
                     (LPCTSTR) m_strRemoteUserName,
                     pal::FormatAddress (m_ServerAddr).c_str ()),
          MB_YESNO) == IDYES);
      }
    if (m_bDeleteMe)
      return;     // they hung up while we were asking
    }  // end of needing to validate calls

  if (!pal::chatConnectionAllowed (decision))
    {
    // tell them our rejection
    SendData ("NO");
    OnClose (0);
    return;   // a plugin, or the user, said no
    }

  m_pDoc->ChatNote (eChatSession,
              TFormat ("Chat session accepted, remote user: \"%s\"",
                (LPCTSTR) m_strRemoteUserName));

  // tell them our acceptance, and chat name
  SendData (CFormat ("YES:%s\n", (LPCTSTR) m_pDoc->m_strOurChatName));

  if (m_bDeleteMe)
    return;     // the answer could not be sent: OnSend closed it

  // connected now
  m_iChatStatus = eChatConnected;
  m_bWasConnected = true;

  // identify ourselves
  CString strVersion = "MUSHclient v";
  strVersion += MUSHCLIENT_VERSION;
  SendChatMessage (CHAT_VERSION, strVersion);

  if (m_iChatConnectionType == eChatZMud)
    {
    SendChatMessage (CHAT_STATUS, (char) 1);  // normal status
    SendChatMessage (CHAT_STAMP, MakeStamp (m_zChatStamp));  // send it
    }

  // tell each plugin about the new user
  m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_CHAT_NEWUSER,
                                    m_iChatID,     // user ID
                                    string (m_strRemoteUserName),
                                    false,
                                    false);

  } // end of CChatSocket::ProcessConnectionRequest

void CChatSocket::OnSend(int nErrorCode)
{
int count; 

  // receive nothing if shutting down
  if (m_bDeleteMe)
    return;

   if (nErrorCode)    // had an error, give up!
      return;

// if we have outstanding data to send, do it

  if (m_outstanding_output.GetLength () <= 0)
    return;

  count = Send (m_outstanding_output, m_outstanding_output.GetLength ());

  if (count != SOCKET_ERROR)
    m_pDoc->m_nBytesOut += count; // count bytes out

  if (count > 0)    // good send - do rest later
    m_outstanding_output = m_outstanding_output.Mid (count);
  else
    {
    int nError = GetLastError ();
    if (count == SOCKET_ERROR && nError != WSAEWOULDBLOCK)
      {
      m_pDoc->ChatNote (eChatConnection,
                  TFormat ("Unable to send to \"%s\", code = %i (%s)",
                      (LPCTSTR) m_strServerName,
                      nError,
                      (LPCTSTR) m_pDoc->GetSocketError (nError)));
      ShutDownSocket (*this);
      m_outstanding_output.Empty ();
      OnClose (nError);      // ????
      }   // end of an error other than "would block"
    } // end of an error

}

void CChatSocket::OnClose(int nErrorCode)
  {

  if (m_iChatStatus == eChatConnected)
    m_pDoc->ChatNote (eChatSession,
              CFormat ("Chat session to %s closed.",
                        (LPCTSTR) m_strRemoteUserName));

  m_iChatStatus = eChatClosed;
  m_bDeleteMe = true;

  } // end of OnClose

void CChatSocket::OnConnect(int nErrorCode)
  {

  if (nErrorCode != 0)
    {
    m_pDoc->ChatNote (eChatConnection,
                TFormat ("Unable to connect to \"%s\", code = %i (%s)",
                    (LPCTSTR) m_strServerName,
                    nErrorCode,
                    (LPCTSTR) m_pDoc->GetSocketError (nErrorCode)));
    OnClose (nErrorCode);
    return;
    }

  m_pDoc->ChatNote (eChatSession,
              TFormat ("Session established to %s.", 
                    (LPCTSTR) m_strServerName));

CString strHostName;
CString strAddresses;

  GetHostNameAndAddresses (strHostName, strAddresses);
  
  // if more than one IP address, take first
  int i = strAddresses.Find (',');
  if (i != -1)
     strAddresses = strAddresses.Left (i);

  // tell them our chat name, IP address, incoming port
  if (m_iChatConnectionType == eChatZMud)
    SendData (CFormat ("ZCHAT:%s\t\n%s%05u",
              (LPCTSTR) m_pDoc->m_strOurChatName, (LPCTSTR) strAddresses, m_pDoc->m_IncomingChatPort));
  else
    SendData (CFormat ("CHAT:%s\n%s%-5u",
              (LPCTSTR) m_pDoc->m_strOurChatName, (LPCTSTR) strAddresses, m_pDoc->m_IncomingChatPort));

  m_iChatStatus = eChatAwaitingConnectConfirm;

  } // end of OnConnect

// The resolver's answer comes back through this: the one event a chat lookup
// wants from the transport it borrows the resolver of.
class CChatResolverEvents : public pal::ITransportEvents
  {
  public:
    explicit CChatResolverEvents (CChatSocket * pSocket) : m_pSocket (pSocket) {}
    void onResolved (pal::ResolveToken, const pal::NetAddress * addresses,
                     size_t count, bool ok) override
      { m_pSocket->HostNameResolved (addresses, count, ok); }
    void onConnect (int) override {}
    void onBytes (const char *, size_t) override {}
    void onWritable () override {}
    void onClosed (int) override {}
    void onTlsEstablished (const char *, const char *, const char *) override {}
    void onTlsFailed (pal::TlsFailure, const char *) override {}
  private:
    CChatSocket * m_pSocket;
  };

// Start looking up the name of an outgoing call; false if it cannot even start.
bool CChatSocket::LookUp (const char * sName)
  {
  m_pResolverEvents = new CChatResolverEvents (this);
#ifdef _WIN32
  m_pResolver = pal::CreateWinTransport (*m_pResolverEvents);
#else
  m_pResolver = pal::CreatePosixTransport (*m_pResolverEvents);
#endif
  if (!m_pResolver)
    return false;
  m_pResolver->resolve (sName, 0);
  return true;
  } // end of LookUp

// A socket of the address's family.  MFC's Create makes only IPv4 ones (and
// binds), so IPv6 is Socket with the family -- the world connection's split.
bool CChatSocket::CreateFor (const pal::NetAddress & address)
  {
  const long lEvents = FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE;
  if (address.isIPv6 ())
    return Socket (SOCK_STREAM, lEvents, 0, AF_INET6) != FALSE;
  return Create (0, SOCK_STREAM, lEvents, NULL) != FALSE;
  } // end of CreateFor

void CChatSocket::HostNameResolved (const pal::NetAddress * addresses, size_t count, bool ok)
  {
  // An IPv4 address when the name has one: a chat peer listening on IPv4
  // alone is the usual case -- zMUD, MudMaster, MUSHclient 5 -- and an
  // IPv6 call to a dual-stack host would be refused.
  if (ok && count > 0)
    {
    m_ServerAddr = addresses [0];
    for (size_t i = 0; i < count; i++)
      if (addresses [i].isIPv4 ())
        {
        m_ServerAddr = addresses [i];
        break;
        }
    }

  // Done with the resolver -- and with `addresses`, which are its.
  // (Destroying it inside its own event is within the ITransport contract.)
  delete m_pResolver;
  m_pResolver = NULL;

  if (!ok || count == 0)
    {
    m_pDoc->ChatNote (eChatConnection,
                TFormat ("Chat session cannot resolve host name: %s.",
                (LPCTSTR) m_strServerName));
    m_bDeleteMe = true;
    m_iChatStatus = eChatClosed;
    return;
    }   // end of error in host name lookup

  // we know the address - get on with it

  MakeCall ();

  } // end of HostNameResolved

void CChatSocket::MakeCall (void)
  {

  // the alleged address and port are what we actually used :)
  m_strAllegedAddress = pal::FormatAddress (m_ServerAddr).c_str ();
  m_iAllegedPort = m_iServerPort;


// first check if we are already connected
  for (POSITION chatpos = m_pDoc->m_ChatList.GetHeadPosition (); chatpos; )
    {
    CChatSocket * pSocket = m_pDoc->m_ChatList.GetNext (chatpos);
    if (pSocket == this || pSocket->m_iChatStatus != eChatConnected)
      continue;

    if (pSocket->m_ServerAddr == m_ServerAddr  &&
        pSocket->m_iServerPort == m_iAllegedPort)
      {
      m_pDoc->ChatNote (eChatConnection,
                  TFormat ("You are already connected to %s port %d",
                        (LPCTSTR) m_strAllegedAddress, 
                        m_iAllegedPort));

      OnClose (0);
      return;
      }       // end of found it
    } // end of checking them

  m_pDoc->ChatNote (eChatConnection,
              TFormat ("Calling chat server at %s port %d",
                    (LPCTSTR) m_strAllegedAddress,
                    m_iAllegedPort));

  m_iChatStatus = eChatConnecting;


  // a call to a name gets its socket now, when the family is known
  if (m_hSocket == INVALID_SOCKET)
    {
    if (!CreateFor (m_ServerAddr))
      {
      OnConnect (GetLastError ());   // says why, and closes the session
      return;
      }
    AsyncSelect ();
    }

  struct sockaddr_storage sa;
  const int saLen = pal::ToSockaddr (m_ServerAddr, m_iServerPort, sa);

BOOL connected = Connect((SOCKADDR*)&sa, saLen);

  if (connected)
    {
    OnConnect (0);    // we have connected already! Do logon of character etc.
    return;
    }

// if error code is "would block" then it will finish later

  int iStatus = GetLastError ();

  if (iStatus == WSAEWOULDBLOCK)
    return;

  // this will display the error message
  OnConnect (iStatus);

  } // end of MakeCall


void CChatSocket::SendData (const CString & strText)
  {

  // send nothing if shutting down
  if (m_bDeleteMe)
    return;

  m_tLastOutgoing = CTime::GetCurrentTime();

  ChatAppendBytes (m_outstanding_output, strText.GetString (), strText.GetLength ());

  // A peer that never reads, while asking for replies (pings, unknown
  // commands), grew this without limit.  Well past any file transfer's
  // working set: give up on them.
  if (m_outstanding_output.GetLength () > 16 * 1024 * 1024)
    {
    m_outstanding_output.Empty ();
    m_pDoc->ChatNote (eChatConnection,
                TFormat ("Chat session to %s closed: they are not reading what we send.",
                          (LPCTSTR) m_strRemoteUserName));
    OnClose (0);
    return;
    }

  OnSend (0);   // in case FD_WRITE message got lost, try to send again
  
  return;
  } // end of SendData

void CChatSocket::ProcessChatMessage (const int iMessage, const CString strMessage)
  {
  if (m_bDeleteMe || m_iChatStatus != eChatConnected)
    return;

#if CHAT_DEBUG
  m_pDoc->Note (TFormat ("Received chat message %i on %i, data: \"%s\"",
                        iMessage, m_iChatID,
                        (LPCTSTR) strMessage));

#endif

  if (!m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_CHAT_MESSAGE, 
                                        m_iChatID,    // who we are
                                        iMessage,     // message number
                                        string (strMessage),    // message text
                                        false,
                                        true))  // stop on false response
    return;   // false means plugin handled it                       


  switch (iMessage)
    {
    case CHAT_TEXT_EVERYBODY:      Process_Text_everybody       (strMessage); break;     
    case CHAT_TEXT_PERSONAL:       Process_Text_personal        (strMessage); break;     
    case CHAT_MESSAGE:             Process_Message              (strMessage); break;     
    case CHAT_TEXT_GROUP:          Process_Text_group           (strMessage); break;     
    case CHAT_PING_REQUEST:        Process_Ping_request         (strMessage); break;     
    case CHAT_PING_RESPONSE:       Process_Ping_response        (strMessage); break;    
    case CHAT_VERSION:             Process_Version              (strMessage); break;    
    case CHAT_REQUEST_CONNECTIONS: Process_Request_connections  (strMessage); break;  
    case CHAT_CONNECTION_LIST:     Process_Connection_list      (strMessage); break;    
    case CHAT_PEEK_CONNECTIONS:    Process_Peek_connections     (strMessage); break;   
    case CHAT_PEEK_LIST:           Process_Peek_list            (strMessage); break;          
    case CHAT_SNOOP:               Process_Snoop                (strMessage); break;              
    case CHAT_SNOOP_DATA:          Process_Snoop_data           (strMessage); break;        
    case CHAT_NAME_CHANGE:         Process_Name_change          (strMessage); break;       
    case CHAT_SEND_COMMAND:        Process_Send_command         (strMessage); break; 
    case CHAT_FILE_START:          Process_File_start				    (strMessage); break;
    case CHAT_FILE_DENY:           Process_File_deny				    (strMessage); break;
    case CHAT_FILE_BLOCK_REQUEST:  Process_File_block_request   (strMessage); break;
    case CHAT_FILE_BLOCK:          Process_File_block           (strMessage); break;
    case CHAT_FILE_END:            Process_File_end				      (strMessage); break;
    case CHAT_FILE_CANCEL:         Process_File_cancel				  (strMessage); break;

    case CHAT_ICON:                Process_Icon                 (strMessage); break;
    case CHAT_STATUS:              Process_Status               (strMessage); break;
    case CHAT_EMAIL_ADDRESS:       Process_EmailAddress				  (strMessage); break;
    case CHAT_STAMP:               Process_Stamp				        (strMessage); break;
    case CHAT_REQUEST_PGP_KEY:     Process_RequestPGPkey				(strMessage); break;
    case CHAT_PGP_KEY:             Process_PGPkey				        (strMessage); break;

    default:

      // tell them we don't support that
      SendChatMessage (CHAT_MESSAGE, 
          TFormat ("\n%s does not support the chat command %i.\n",
                    (LPCTSTR) m_pDoc->m_strOurChatName, iMessage));
      // tell us we got it
      m_pDoc->ChatNote (eChatInformation,
                  TFormat ("Received unsupported chat command %i from %s",
                           iMessage,
                           (LPCTSTR) m_strRemoteUserName));
      break;

    } // end of switch


  } // end of ProcessChatMessage

// sends a chat message to the other end
// A zChat frame: 2 bytes of message number, 2 bytes of length (both little-
// endian), then the data.  The length field cannot say more than 65535, but a
// message can be longer -- a MudMaster peer's text, which has no limit, relayed
// on to zChat peers -- and it used to go out with the length's low 16 bits and
// ALL of its bytes, so the receiver read the excess as further frames and the
// session desynchronised.  The data is cut to what the field can describe.
CString CChatSocket::ZChatFrame (const int iMessage, const CString & strData)
  {
  const int iLength = strData.GetLength () > 0xFFFF ? 0xFFFF : strData.GetLength ();
  CString strFrame;
  strFrame =  (char) (unsigned char) (iMessage & 0xFF);
  strFrame += (char) (unsigned char) ((iMessage >> 8) & 0xFF);
  strFrame += (char) (unsigned char) (iLength & 0xFF);
  strFrame += (char) (unsigned char) ((iLength >> 8) & 0xFF);
  ChatAppendBytes (strFrame, strData.GetString (), iLength);
  return strFrame;
  } // end of CChatSocket::ZChatFrame

void CChatSocket::SendChatMessage (const int iMessage,
                                   const CString strMessage,
                                   const long iStamp)
  {
  if (m_bDeleteMe || m_iChatStatus != eChatConnected)
    return;

#if CHAT_DEBUG
  m_pDoc->Note (TFormat ("Sending chat message %i on %i, data: \"%s\"",
                        iMessage, m_iChatID,
                        (LPCTSTR) strMessage));

#endif

  if (!m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_CHAT_MESSAGE_OUT, 
                                        m_iChatID,    // which chat ID
                                        iMessage,     // message number
                                        string (strMessage),    // message text
                                        false,
                                        true)) // stop on false response
      return;   // false means plugin discarded it 

  if (iMessage == CHAT_SNOOP && m_bYouAreSnooping)
    m_bYouAreSnooping = false;
  
  CString strData;

  switch (m_iChatConnectionType)
    {

    case eChatMudMaster:
      {
      // messages start with a message number and end with 0xFF
      strData = (char) (unsigned char) iMessage;
      // ChatAppendBytes, not +=: a file block is raw file bytes and will
      // contain NULs (see the note on the helper).
      ChatAppendBytes (strData, strMessage.GetString (), strMessage.GetLength ());
      // can't have 0xFF imbedded in messages, except file blocks :)
      // 0xFF is y with 2 dots on it so y will look reasonable 
      if (iMessage != CHAT_FILE_BLOCK)
        strData.Replace ((unsigned char) CHAT_END_OF_COMMAND, 'y');
      strData += (unsigned char) CHAT_END_OF_COMMAND;
      }
      break;

    case eChatZMud:
      {
      CString strStampedMessage;

      // now add message stamp if message requires
      switch (iMessage)
        {
        // stamp it if necessary
        case CHAT_TEXT_EVERYBODY:
        case CHAT_TEXT_PERSONAL:
        case CHAT_TEXT_GROUP:
          // zero means use this connection's stamp
          strStampedMessage = MakeStamp (iStamp ? iStamp : m_zChatStamp) +
                              strMessage;
          break;

        default:
          strStampedMessage = strMessage;
          break;

        }

      strData = ZChatFrame (iMessage, strStampedMessage);
      }
      break;

    } // end of switch on chat connection type

  SendData (strData);

  } // end of ProcessChatMessage


void CChatSocket::Process_Name_change				  (const CString strMessage)
  {
  CString strOldName = m_strRemoteUserName;
  m_strRemoteUserName = strMessage;

  // Kept, so ChatGetID and the chat list know them by it, but not announced
  // for someone being ignored: the name is their text, as much as a message.
  if (m_bIgnore)
    return;

  m_pDoc->ChatNote (eChatNameChange,
              TFormat ("%s has changed his/her name to %s.",
                            (LPCTSTR) strOldName,
                            (LPCTSTR) m_strRemoteUserName));

  } // end of CChatSocket::Process_Name_change	

void CChatSocket::Process_Request_connections (const CString strMessage)
  {
  CString strResult;

  for (POSITION chatpos = m_pDoc->m_ChatList.GetHeadPosition (); chatpos; )
    {
    CChatSocket * pSocket = m_pDoc->m_ChatList.GetNext (chatpos);
    if (pSocket->m_iChatStatus == eChatConnected && !pSocket->m_bPrivate)
      {
      // omit self
      if (pSocket == this)
        continue;

      if (!strResult.IsEmpty ())
        strResult += ",";

      strResult += pSocket->m_strAllegedAddress;
      strResult += ",";
      strResult += CFormat ("%d", pSocket->m_iAllegedPort);

      }   // end of this one wanted
    }   // end of doing all

  m_pDoc->ChatNote (eChatInformation,
              TFormat ("%s has requested your public connections", 
                    (LPCTSTR) m_strRemoteUserName));
  SendChatMessage (CHAT_CONNECTION_LIST, strResult);
  } // end of CChatSocket::Process_Request_connections

void CChatSocket::Process_Connection_list	 		(const CString strMessage)
  {
  // Only the list we asked for (world.ChatRequestConnections sends the
  // request).  Any peer could otherwise send one unasked and have us
  // dial every host in it -- thousands of them, each told our chat name,
  // address and port.
  if (!m_bAskedForConnections)
    {
    m_pDoc->ChatNote (eChatConnectionList,
                  TFormat ("Ignored a connection list from %s that was not asked for.",
                      (LPCTSTR) m_strRemoteUserName));
    return;
    }
  m_bAskedForConnections = false;

  CStringList strList;
  StringToList (strMessage, ",", strList);
  int iCount = strList.GetCount () / 2;   // number of connections
  m_pDoc->ChatNote (eChatConnectionList,
                TFormat ("Found %i connection%s to %s",
                    PLURAL (iCount),
                    (LPCTSTR) m_strRemoteUserName));

  // for each one, connect to it -- within reason, and each address once
  const int kMaxCalls = 100;
  set <string> called;
  for (int i = 0; i < iCount; i++)
    {
    CString strIP;
    long iPort;

    strIP = strList.RemoveHead ();
    iPort = atol (strList.RemoveHead ());

    if (!called.insert (string (CFormat ("%s:%ld", (LPCTSTR) strIP, iPort))).second)
      continue;   // the same one again
    if ((int) called.size () > kMaxCalls)
      {
      m_pDoc->ChatNote (eChatConnectionList,
                    TFormat ("Only the first %i connections were called.", kMaxCalls));
      break;
      }

    m_pDoc->ChatCall(strIP, iPort);
    }   // end of doing each connection
  } // end of CChatSocket::Process_Connection_list	

void CChatSocket::Process_Text_everybody			(const CString strMessage)
  {

  CString strFixedMessage = strMessage;
  long iStamp = GetStamp (strFixedMessage);

  if (!m_bIgnore)
    {
    // anti-message loop provision
    if (strFixedMessage == m_pDoc->m_strLastMessageSent)
      {
      CTimeSpan timediff = CTime::GetCurrentTime() -  m_pDoc->m_tLastMessageTime;
      if (timediff.GetTotalSeconds () < LOOP_DISCARD_SAME_MESSAGE_SECONDS)
        return;
      } // end of same message that we just sent

    // zChat anti-loop provision
    if (m_iChatConnectionType == eChatZMud &&
        m_zChatStamp == iStamp)
        return;

    m_iCountIncomingAll++;
    m_pDoc->ChatNote (eChatIncomingEverybody, strFixedMessage);
    if (m_bIncoming)
      m_pDoc->SendChatMessageToAll (CHAT_TEXT_EVERYBODY,
                              strFixedMessage,
                              true,      // unless ignoring them
                              false,     // to everyone
                              false,     
                              m_iChatID, // except us
                              "",        // no particular group
                              iStamp);   // use this message stamp
    else
      m_pDoc->SendChatMessageToAll (CHAT_TEXT_EVERYBODY,
                              strFixedMessage,
                              true,      // unless ignoring them
                              true,      // to incoming only (ones we serve)
                              false,
                              m_iChatID, // except us
                              "",        // no particular group
                              iStamp);   // use this message stamp

    } // end of not ignoring him
  }  // end of CChatSocket::Process_Text_everybody

void CChatSocket::Process_Text_personal			  (const CString strMessage)
  {
  if (!m_bIgnore)
    {

    CString strFixedMessage = strMessage;
    long iStamp = GetStamp (strFixedMessage);

    // zChat anti-loop provision
    if (m_iChatConnectionType == eChatZMud &&
        m_zChatStamp == iStamp)
        return;

    m_iCountIncomingPersonal++;
    m_pDoc->ChatNote (eChatIncomingPersonal, strFixedMessage);
    }
  } // end of CChatSocket::Process_Text_personal 

void CChatSocket::Process_Text_group				  (const CString strMessage)
  {
  if (!m_bIgnore)
    {
    // anti-message loop provision
    if (strMessage == m_pDoc->m_strLastGroupMessageSent)
      {
      CTimeSpan timediff = CTime::GetCurrentTime() -  m_pDoc->m_tLastGroupMessageTime;
      if (timediff.GetTotalSeconds () < LOOP_DISCARD_SAME_MESSAGE_SECONDS)
        return;
      } // end of same message that we just sent

    CString strFixedMessage = strMessage;
    long iStamp = GetStamp (strFixedMessage);

    // zChat anti-loop provision
    if (m_iChatConnectionType == eChatZMud &&
        m_zChatStamp == iStamp)
        return;

    if (strFixedMessage.GetLength () > 15)
      {
      m_iCountIncomingGroup++;
      CString strGroup = strFixedMessage.Left (15);
      strGroup.TrimLeft ();
      strGroup.TrimRight ();
      m_pDoc->ChatNote (eChatIncomingGroup, strFixedMessage.Mid (15));   // strip group
      if (!m_bIncoming)   // if from one we called
        m_pDoc->SendChatMessageToAll (CHAT_TEXT_GROUP,
                                strFixedMessage,
                                true,      // unless ignoring them
                                true,      // to incoming only (ones we serve)
                                false,
                                m_iChatID,  // except us
                                strGroup, // onsend to same group
                                iStamp);    // use this message stamp
      }   // end of message not ridiculously small
    } // end of not ignoring

  } // end of CChatSocket::Process_Text_group

void CChatSocket::Process_Message					    (const CString strMessage)
  {
  if (!m_bIgnore)
    {
    m_iCountMessages++;
    m_pDoc->ChatNote (eChatMessage, strMessage);
    }

  } // end of CChatSocket::Process_Message	

void CChatSocket::Process_Version					    (const CString strMessage)
  {
  m_strRemoteVersion = strMessage;    // remember it for later
  } // end of CChatSocket::Process_Version	

void CChatSocket::Process_File_start				  (const CString strMessage)
  {
  if (!m_bCanSendFiles)
    {
    SendChatMessage (CHAT_FILE_DENY,
                    TFormat ("%s is not allowing file transfers from you.",
                    (LPCTSTR) m_pDoc->m_strOurChatName));
    return;
    }

  // One transfer at a time.  Without this a second CHAT_FILE_START arriving
  // mid-transfer walked straight over m_pFile with a new CFile *, leaking the
  // open one and orphaning the half-received file it was writing -- and if the
  // new open then FAILED, the catch below left m_pFile dangling while
  // m_bDoingFileTransfer was still true, so the next CHAT_FILE_BLOCK wrote
  // through a freed pointer.  docs/UPSTREAM-BUGS.md 98.
  // ...and that includes one still being asked about: the question below is a
  // modal box, inside whose message loop OnReceive runs again, and a second
  // CHAT_FILE_START there found m_bDoingFileTransfer still false and asked
  // again -- accept both and the first CFile leaked, its file orphaned.
  if (m_bDoingFileTransfer || m_bFileOfferPending)
    {
    SendChatMessage (CHAT_FILE_DENY,
                    TFormat ("%s is already transferring a file.",
                    (LPCTSTR) m_pDoc->m_strOurChatName));
    return;
    }
  struct PendingOffer
    {
    explicit PendingOffer (bool & b) : m_b (b) { m_b = true; }
    ~PendingOffer () { m_b = false; }
    bool & m_b;
    } pending (m_bFileOfferPending);

  CStringList strList;
  StringToList (strMessage, ",", strList);
  
  if (strList.GetCount () != 2)
    {
    SendChatMessage (CHAT_FILE_DENY, 
                    "Expected \"filename,filesize\" but did not get that.");
    return;
    }

  m_strSenderFileName = strList.RemoveHead ();

  if (m_strSenderFileName.Find (_T("/")) != -1 ||
      m_strSenderFileName.Find (_T("\\")) != -1)
    {
    SendChatMessage (CHAT_FILE_DENY,
                    TFormat ("Supplied file name of \"%s\" may not contain slashes.",
                    (LPCTSTR) m_strSenderFileName));
    return;
    }

  // Nor be a name Windows gives a device (NUL, CON, COM1... with or without
  // an extension), which "creates" the device -- NUL then could not be
  // deleted on an abort, which threw -- nor name a stream (":"), nor be
  // empty or dots only.
  if (!IsSafeChatFileName (m_strSenderFileName))
    {
    SendChatMessage (CHAT_FILE_DENY,
                    TFormat ("Supplied file name of \"%s\" is not one that can be saved.",
                    (LPCTSTR) m_strSenderFileName));
    return;
    }

  CString strSize = strList.RemoveHead ();

  if (!IsNumber (strSize))
    {
    SendChatMessage (CHAT_FILE_DENY, 
                    "File size was not numeric.");
    return;
    }
  
  m_iFileSize = atol (strSize);

  bool bWanted = true;

  double K = m_iFileSize / 1024.0;

  // Saved unasked only within reason.  "Auto-allow files" with a save folder
  // took any number of files of any size (up to the protocol's 2 GB each) from
  // anyone allowed to send them, so a peer could fill the disk while nobody was
  // watching.  Past these limits the offer is asked about, as it is without
  // auto-allow: a big file someone really meant to send still gets through.
  const bool bAutoAccept = m_pDoc->m_bAutoAllowFiles &&
                           m_iFileSize <= kMaxAutoFileSize &&
                           m_iAutoFilesReceived < kMaxAutoFilesPerSession &&
                           m_fAutoBytesReceived + m_iFileSize <= kMaxAutoBytesPerSession;

  if (m_pDoc->m_bAutoAllowFiles && !bAutoAccept)
    m_pDoc->ChatNote (eChatFile,
            TFormat ("%s offers the file \"%s\" (%ld bytes): more than is saved "
                     "without asking, so you are asked.",
                     (LPCTSTR) m_strRemoteUserName,
                     (LPCTSTR) m_strSenderFileName,
                     m_iFileSize));

  if (!bAutoAccept)
    {
     m_pDoc->Activate ();
     ModalGuard guard (this);   // the socket is kept while we ask (see chatsock.h)
     if (UMessageBox (
        TFormat ("%s wishes to send you the file \"%s\", size %ld bytes (%1.1f Kb).\n\nAccept it?",
                 (LPCTSTR) m_strRemoteUserName,
                 (LPCTSTR) m_strSenderFileName,
                 m_iFileSize,
                 K),
      MB_YESNO) != IDYES)
      bWanted = false;
    }

  if (m_bDeleteMe)
    return;     // they hung up while we were asking

  // A transfer started while we were asking (ChatSendFile refuses to now, but
  // this is what would have put a second CFile over its open one).
  if (m_bDoingFileTransfer)
    {
    SendChatMessage (CHAT_FILE_DENY,
                    TFormat ("%s is already transferring a file.",
                    (LPCTSTR) m_pDoc->m_strOurChatName));
    return;
    }

  if (bWanted)
    {
    if (m_pDoc->m_strChatSaveDirectory.IsEmpty () ||
         !bAutoAccept)  // no auto-save
      {
      CFileDialog filedlg (FALSE,   // saving the file
                           _T(""),    // default extension
                           m_strSenderFileName,  // suggested name
                           OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                           _T("All files (*.*)|*.*|")
                           _T("|"),    // filter 
                           &Frame);  // parent window

      CString strTitle =  TFormat ("Chat: Save file from %s as ...",
                                          (LPCTSTR) m_strRemoteUserName);
      filedlg.m_ofn.lpstrTitle = strTitle;

      // use file save directory if defined  
      if (!m_pDoc->m_strChatSaveDirectory.IsEmpty ())
       filedlg.m_ofn.lpstrInitialDir = m_pDoc->m_strChatSaveDirectory;

      ChangeToFileBrowsingDirectory ();
        {
        ModalGuard guard (this);
        if (filedlg.DoModal () != IDOK)
          bWanted = false;    // cancelled dialog
        }
      ChangeToStartupDirectory ();

      if (m_bDeleteMe)
        return;     // they hung up while we were choosing

      m_strOurFileName = filedlg.GetPathName ();
      }
    else
      {
#ifdef _WIN32
      // want backslashes not forwards slashes
      m_pDoc->m_strChatSaveDirectory.Replace (_T("/"), _T("\\"));
      // ensure trailing backslash
      if (m_pDoc->m_strChatSaveDirectory.Right (1) != TEXT ('\\'))
        m_pDoc->m_strChatSaveDirectory += "\\";
#else
      // normalize to forward slashes on POSIX
      m_pDoc->m_strChatSaveDirectory.Replace (_T("\\"), _T("/"));
      // ensure trailing slash
      if (m_pDoc->m_strChatSaveDirectory.Right (1) != TEXT ('/'))
        m_pDoc->m_strChatSaveDirectory += "/";
#endif
  
      m_strOurFileName = m_pDoc->m_strChatSaveDirectory + m_strSenderFileName;

      // Saved unasked, so never over a file already there: a peer offering
      // "report.pdf" truncated the user's own, and a cancel then deleted it.
      // The next free "name (2).ext" instead.
      CString strBase = m_strSenderFileName, strExt;
      const int iDot = strBase.ReverseFind ('.');
      if (iDot > 0)
        {
        strExt  = strBase.Mid (iDot);
        strBase = strBase.Left (iDot);
        }
      for (int i = 2; i < 1000 && FileExists (m_strOurFileName); i++)
        m_strOurFileName = m_pDoc->m_strChatSaveDirectory +
                           CFormat ("%s (%i)%s", (LPCTSTR) strBase, i, (LPCTSTR) strExt);

      // ...and when "name (2)" to "name (999)" are all taken too, refuse: the
      // loop used to stop there and the file went over "name (999)".
      if (FileExists (m_strOurFileName))
        {
        SendChatMessage (CHAT_FILE_DENY,
                        TFormat ("%s already has too many files called \"%s\".",
                        (LPCTSTR) m_pDoc->m_strOurChatName,
                        (LPCTSTR) m_strSenderFileName));
        m_pDoc->ChatNote (eChatFile,
                TFormat ("Refused the file \"%s\" from %s: no free name for it in %s.",
                         (LPCTSTR) m_strSenderFileName,
                         (LPCTSTR) m_strRemoteUserName,
                         (LPCTSTR) m_pDoc->m_strChatSaveDirectory));
        m_strOurFileName.Empty ();
        m_iFileSize = 0;
        return;
        }

      m_iAutoFilesReceived++;
      m_fAutoBytesReceived += m_iFileSize;
      }
    }   // end of if (bWanted)

  if (!bWanted)
    {
    // tell them our rejection
    SendChatMessage (CHAT_FILE_DENY, 
                    TFormat ("%s does not want that particular file.",
                    (LPCTSTR) m_pDoc->m_strOurChatName));
    return;
    }   // end of this file not wanted right now

  try
    {
    m_pFile = new CFile (m_strOurFileName, CFile::modeCreate | CFile::modeReadWrite); // new file
    } // end of try block

  catch (CFileException * e)
    {
    SendChatMessage (CHAT_FILE_DENY, 
                    TFormat ("%s can not open that file.",
                    (LPCTSTR) m_pDoc->m_strOurChatName));
    m_pDoc->ChatNote (eChatFile, TFormat ("File %s cannot be opened.", 
                      (LPCTSTR) m_strOurFileName));
    e->Delete ();
    // reset the two fields we changed so far
    m_strOurFileName.Empty ();
    m_iFileSize = 0;

    delete m_pFile;    // in case it was set up
    m_pFile = NULL;    // ...and say so: everything else that deletes it nulls
                       // it, and leaving a freed pointer here is what turned a
                       // failed open into a write through freed memory
                       // (docs/UPSTREAM-BUGS.md 98)

    return;
    } // end of catching a file exception

  // tell them our acceptance - get first block
  SendChatMessage (CHAT_FILE_BLOCK_REQUEST, "");

  m_pDoc->ChatNote (eChatFile,
              TFormat (
        "Receiving a file from %s -- Filename: %s, Length: %ld bytes (%1.1f Kb).",
                            (LPCTSTR) m_strRemoteUserName,
                            (LPCTSTR) m_strOurFileName,
                            m_iFileSize,
                            K));

  // get ready for transfer
  m_tStartedFileTransfer = CTime::GetCurrentTime();  // when started
  m_bSendFile = false;   // we are receiving
  m_bDoingFileTransfer = true;           
  m_iBlocksTransferred = 0;   // no blocks yet
  // add block-size minus one to allow for final partial block
  m_iFileBlocks = (m_iFileSize + m_iFileBlockSize - 1L) /
                  m_iFileBlockSize;

  shsInit  (&m_shsInfo);

  } // end of CChatSocket::Process_File_start

void CChatSocket::Process_File_deny				    (const CString strMessage)
  {
  // Their reason is their text: not shown for someone being ignored, though
  // the refusal itself still stops the transfer (and says so).
  if (!m_bIgnore)
    m_pDoc->ChatNote (eChatFile, strMessage);
  StopFileTransfer (true);    // close file
  } // end of CChatSocket::Process_File_deny

void CChatSocket::Process_File_block_request	(const CString strMessage)
  {
  // MudMaster seems to send an extra request - just ignore it
  if (!m_bDoingFileTransfer)
    return;

  if (!m_bSendFile)
    {
    SendChatMessage (CHAT_MESSAGE, "We are supposed to be receiving a file.");
    return;
    }


  ZeroMemory (m_pFileBuffer, m_iFileBlockSize);
  UINT iBytesRead;

  try
    { 
    iBytesRead = m_pFile->Read (m_pFileBuffer, m_iFileBlockSize);
    }
  catch (CFileException * e)
    {
    e->Delete ();
    iBytesRead = 0;
    } // end of catching a file exception

  // which block we are up to
  m_iBlocksTransferred++;

  UINT expected = m_iFileBlockSize;
  if (m_iBlocksTransferred == m_iFileBlocks)
    expected = m_iFileSize % m_iFileBlockSize;

  if (expected == 0)
     expected = m_iFileBlockSize; // must be an exact multiple of block size

  if (iBytesRead != expected)
    {    
    SendChatMessage (CHAT_MESSAGE, "File transfer aborted due to read error by sender.");
    m_pDoc->ChatNote (eChatFile, 
                      TFormat ("Send of file \"%s\" aborted due to read error.",
                    (LPCTSTR) m_strOurFileName ));
    StopFileTransfer (true);    // close file
    return;
    }

  // update sumcheck
  shsUpdate (&m_shsInfo, m_pFileBuffer, iBytesRead);

  SendChatMessage (CHAT_FILE_BLOCK, CString ((char *) m_pFileBuffer, m_iFileBlockSize));
  m_iCountFileBytesOut += iBytesRead;

  if (m_iBlocksTransferred >= m_iFileBlocks)
    {
    SendChatMessage (CHAT_FILE_END, "");
    m_pDoc->ChatNote (eChatFile, TFormat ("Send of file \"%s\" complete.",
                    (LPCTSTR) m_strOurFileName ));
    shsFinal  (&m_shsInfo);
    SendChatMessage (CHAT_MESSAGE,
            TFormat ("Sumcheck from sender was: %08X %08X %08X %08X %08X", 
            m_shsInfo.digest [0],
            m_shsInfo.digest [1],
            m_shsInfo.digest [2],
            m_shsInfo.digest [3],
            m_shsInfo.digest [4]
            ));
    // tell sender sumcheck
    m_pDoc->ChatNote (eChatFile,
            TFormat ("Sumcheck we calculated:   %08X %08X %08X %08X %08X", 
            m_shsInfo.digest [0],
            m_shsInfo.digest [1],
            m_shsInfo.digest [2],
            m_shsInfo.digest [3],
            m_shsInfo.digest [4]
            ));
    StopFileTransfer (false);    // close file
    return;
    }

  } // end of CChatSocket::Process_File_block_request

void CChatSocket::Process_File_block			    (const CString strMessage)
  {
  // ignore if not doing file transfer
  if (!m_bDoingFileTransfer)
    return;

  if (m_bSendFile)
    {
    SendChatMessage (CHAT_MESSAGE, "We are supposed to be sending a file.");
    return;
    }

  // which block we are up to
  m_iBlocksTransferred++;

  UINT expected = m_iFileBlockSize;
  if (m_iBlocksTransferred == m_iFileBlocks)
    expected = m_iFileSize % m_iFileBlockSize;   // smaller final block

  if (expected == 0)
     expected = m_iFileBlockSize; // must be an exact multiple of block size

  // `expected` is what the PEER promised: m_iFileBlockSize comes from the
  // handshake and m_iFileSize from its file-start message.  The bytes actually
  // in front of us are strMessage, and on the zChat transport the two need not
  // agree — that path takes the block length from two bytes of the peer's own
  // message header and hands over exactly that many, with no comparison against
  // the block size (chatsock.cpp, the zChat arm of OnReceive).  The MudMaster
  // arm cannot produce a short block because it waits for
  // `m_iFileBlockSize + 2` bytes before slicing, which is the check this one
  // never had.
  //
  // Writing `expected` bytes out of a shorter buffer read past the end of the
  // heap allocation and put whatever followed it into the user's file, and into
  // the SHA sumcheck.  A peer that sends a one-byte CHAT_FILE_BLOCK got 1023
  // bytes of this process's heap written to disk.  docs/UPSTREAM-BUGS.md 96.
  //
  // Refuse rather than clamp: a block shorter than promised means the transfer
  // is already wrong, and silently writing a short block would produce a
  // corrupt file whose sumcheck failure is the only clue.
  if ((UINT) strMessage.GetLength () < expected)
    {
    SendChatMessage (CHAT_MESSAGE, "File transfer aborted: short file block.");
    m_pDoc->ChatNote (eChatFile,
                      TFormat ("Receive of file \"%s\" aborted: block %ld arrived "
                               "with %i bytes, not the %u agreed.",
                               (LPCTSTR) m_strOurFileName,
                               m_iBlocksTransferred,
                               strMessage.GetLength (),
                               expected));
    StopFileTransfer (true);    // close file
    return;
    }

  CString strBuffer = strMessage;
  char * pBuff = strBuffer.GetBuffer (strMessage.GetLength ());

  try
    { 
    m_pFile->Write (pBuff, expected);
    }
  catch (CFileException * e)
    {
    e->Delete ();

    strBuffer.ReleaseBuffer ();

    SendChatMessage (CHAT_MESSAGE, "File transfer aborted due to write error by receiver.");
    m_pDoc->ChatNote (eChatFile, 
                      TFormat ("Receive of file \"%s\" aborted due to write error.",
                    (LPCTSTR) m_strOurFileName ));
    StopFileTransfer (true);    // close file
    return;

    } // end of catching a file exception

  // update sumcheck
  shsUpdate (&m_shsInfo, (unsigned char *) pBuff, expected);

  strBuffer.ReleaseBuffer ();
  m_iCountFileBytesIn += expected;

  // get next block - even if at end - see MudMaster
  SendChatMessage (CHAT_FILE_BLOCK_REQUEST, "");

  if (m_iBlocksTransferred >= m_iFileBlocks)
    {
    // Flush + close with error checking BEFORE declaring the transfer complete.
    // StopFileTransfer's `delete m_pFile` swallows the fclose error, and off
    // Windows CFile is buffered stdio, so a final block that fit in one buffer
    // could be lost on a full disk while the receiver printed "complete" with a
    // matching checksum (the sum is over the bytes handed to Write, not the
    // file).  On a failed flush, treat it as an abort so the truncated file is
    // deleted rather than left behind as a "complete, checksum-verified" lie.
    try
      {
      m_pFile->Close ();
      }
    catch (CFileException * e)
      {
      e->Delete ();
      SendChatMessage (CHAT_MESSAGE,
                       "File transfer aborted due to write error by receiver.");
      m_pDoc->ChatNote (eChatFile,
              TFormat ("Receive of file \"%s\" aborted due to write error.",
                       (LPCTSTR) m_strOurFileName ));
      StopFileTransfer (true);    // deletes the truncated file
      return;
      }
    m_pDoc->ChatNote (eChatFile,TFormat ("Receive of file \"%s\" complete.",
                    (LPCTSTR) m_strOurFileName ));
    shsFinal  (&m_shsInfo);
    m_pDoc->ChatNote (eChatFile,
            TFormat ("Sumcheck as written was:  %08X %08X %08X %08X %08X", 
            m_shsInfo.digest [0],
            m_shsInfo.digest [1],
            m_shsInfo.digest [2],
            m_shsInfo.digest [3],
            m_shsInfo.digest [4]
            ));
    // notify sender
    SendChatMessage (CHAT_MESSAGE,
            TFormat ("Sumcheck as received was: %08X %08X %08X %08X %08X", 
            m_shsInfo.digest [0],
            m_shsInfo.digest [1],
            m_shsInfo.digest [2],
            m_shsInfo.digest [3],
            m_shsInfo.digest [4]
            ));
    StopFileTransfer (false);    // close file
    return;
    }

  } // end of CChatSocket::Process_File_block

void CChatSocket::Process_File_end				    (const CString strMessage)
  {
  // no transfer? ignore it
  if (!m_bDoingFileTransfer)
    return;

  // we probably shouldn't get here because we detect the end of file
  // at the moment it occurs, without waiting for the FILE_END block

  if (m_iFileBlocks != m_iBlocksTransferred)
    {
    m_pDoc->ChatNote (eChatFile, 
                      TFormat ("Transfer of file \"%s\" stopped prematurely.",
                    (LPCTSTR) m_strOurFileName ));

    StopFileTransfer (true);    // too soon - abort it
    }
  else
    StopFileTransfer (false);   // close file
 

  } // end of CChatSocket::Process_File_end

void CChatSocket::Process_File_cancel				  (const CString strMessage)
  {
  StopFileTransfer (true);    // too soon - abort it
  } // end of CChatSocket::Process_File_cancel	

void CChatSocket::Process_Ping_request				(const CString strMessage)
  {
  SendChatMessage (CHAT_PING_RESPONSE, strMessage);
  } // end of CChatSocket::Process_Ping_request

void CChatSocket::Process_Ping_response				(const CString strMessage)
  {
  if (App.m_iCounterFrequency && m_iPingStartTime.QuadPart)
    {
    LARGE_INTEGER finish;
    LONGLONG iTimeTaken;
    QueryPerformanceCounter (&finish);
    iTimeTaken = finish.QuadPart - m_iPingStartTime.QuadPart;
    m_iPingStartTime.QuadPart = 0;    // ready for next time
    CString strMessage;
    m_fLastPingTime = ((double) iTimeTaken) / 
                    ((double) App.m_iCounterFrequency);

    CString strPingTime;
    strPingTime.Format ("%9.3f seconds", m_fLastPingTime);
    m_pDoc->ChatNote (eChatPing,
                TFormat ("Ping time to %s: %s", 
                        (LPCTSTR) m_strRemoteUserName,
                        (LPCTSTR) strPingTime));
    }
  else if (!m_bIgnore)    // the bare response is their text, and was unasked
    m_pDoc->ChatNote (eChatPing,
                TFormat ("Ping response: %s", (LPCTSTR) strMessage));
  } // end of CChatSocket::Process_Ping_response

void CChatSocket::Process_Peek_connections		(const CString strMessage)
  {
  CString strResult;

  for (POSITION chatpos = m_pDoc->m_ChatList.GetHeadPosition (); chatpos; )
    {
    CChatSocket * pSocket = m_pDoc->m_ChatList.GetNext (chatpos);
    if (pSocket->m_iChatStatus == eChatConnected && !pSocket->m_bPrivate)
      {
      // omit self
      if (pSocket == this)
        continue;

      if (m_iChatConnectionType == eChatZMud)
        {
        // different format here
        strResult += pSocket->m_strAllegedAddress;
        strResult += ",";
        strResult += CFormat ("%d", pSocket->m_iAllegedPort);
        strResult += ",";
        }
      else
        {
        strResult += pSocket->m_strAllegedAddress;
        strResult += "~";
        strResult += CFormat ("%d", pSocket->m_iAllegedPort);
        strResult += "~";
        strResult += pSocket->m_strRemoteUserName;
        strResult += "~";
        }

      }   // end of this one wanted
    }   // end of doing all

  SendChatMessage (CHAT_PEEK_LIST, strResult);
  m_pDoc->ChatNote (eChatInformation,
                TFormat ("%s is peeking at your connections", 
                    (LPCTSTR) m_strRemoteUserName));

  } // end of CChatSocket::Process_Peek_connections

void CChatSocket::Process_Peek_list			    	(const CString strMessage)
  {
  CStringList strList;
  int iCount;

  // A peek list is one chat message, shown a connection to a line, so the
  // world's limit on a message's lines never applied to it: it is the limit on
  // the connections listed.  And nothing of theirs is shown while ignoring them.
  if (m_bIgnore)
    return;
  const int iMaxShown = m_pDoc->m_iMaxChatLinesPerMessage > 0 ?
                        (int) m_pDoc->m_iMaxChatLinesPerMessage : INT_MAX;

  if (m_iChatConnectionType == eChatZMud)
    {
    StringToList (strMessage, ",", strList);
    iCount = strList.GetCount () / 2;   // number of connections

    m_pDoc->ChatNote (eChatPeekList,
                TFormat ("Peek found %i connection%s to %s", 
                      PLURAL (iCount),
                      (LPCTSTR) m_strRemoteUserName));

    if (iCount)
      {
      m_pDoc->ChatNote (eChatPeekList,
                  "Address                    Port");
      m_pDoc->ChatNote (eChatPeekList,
                  "=========================  =====");
      }
  
    for (int i = 0; i < iCount && i < iMaxShown; i++)
      {
      CString strIP,
              strPort;

      strIP = strList.RemoveHead ();
      strPort = strList.RemoveHead ();

      m_pDoc->ChatNote (eChatPeekList,
                    CFormat ("%-26s %-5s", 
                        (LPCTSTR) strIP,
                        (LPCTSTR) strPort
                        ));
      }   // end of doing each connection

    }  // end of zChat
  else
    {
    StringToList (strMessage, "~", strList);
    iCount = strList.GetCount () / 3;   // number of connections

    m_pDoc->ChatNote (eChatPeekList,
                TFormat ("Peek found %i connection%s to %s", 
                      PLURAL (iCount),
                      (LPCTSTR) m_strRemoteUserName));

    if (iCount)
      {
      m_pDoc->ChatNote (eChatPeekList,
                  "Name                    Address                    Port");
      m_pDoc->ChatNote (eChatPeekList,
                  "======================  =========================  =====");
      }
  
    for (int i = 0; i < iCount && i < iMaxShown; i++)
      {
      CString strIP,
              strPort,
              strName;

      strIP = strList.RemoveHead ();
      strPort = strList.RemoveHead ();
      strName = strList.RemoveHead ();

      m_pDoc->ChatNote (eChatPeekList,
                    CFormat ("%-23s %-26s %-5s", 
                        (LPCTSTR) strName,
                        (LPCTSTR) strIP,
                        (LPCTSTR) strPort
                        ));


      }   // end of doing each connection

    }  // end of MudMaster

  if (iCount > iMaxShown)
    m_pDoc->ChatNote (eChatPeekList,
                TFormat ("[Peek list truncated, exceeds %i lines]", iMaxShown));

  } // end of CChatSocket::Process_Peek_list	

void CChatSocket::Process_Snoop					      (const CString strMessage)
  {
  // if he is snooping us, then stop!
  if (m_bHeIsSnooping)
    {
    SendChatMessage (CHAT_MESSAGE, 
      TFormat ("\nYou are no longer snooping %s.\n",
                (LPCTSTR) m_pDoc->m_strOurChatName));
    m_pDoc->ChatNote (eChatInformation,
                TFormat ("%s has stopped snooping you.",
                              (LPCTSTR) m_strRemoteUserName));
    m_bHeIsSnooping = false;
    return;    // all done
    }

  if (m_bCanSnoop)
    {
    // One question at a time: OnReceive runs inside the box's message loop,
    // and a second CHAT_SNOOP there asked again, nested.
    if (m_bSnoopPending)
      return;

    bool bPermitted = true;
    if (!m_pDoc->m_bAutoAllowSnooping)  // don't bother if they don't mind
      {
      // The socket is kept while we ask, and checked when the box closes: the
      // box had no guard, so a peer that hung up while it was up was closed by
      // FD_CLOSE and deleted by the reaper (WM_TIMER is delivered inside the
      // box), and this went on with a freed socket and returned into
      // OnReceive's loop with one.  The other prompts were guarded in
      // de8e8c72; this one was missed.
      struct PendingSnoop
        {
        explicit PendingSnoop (bool & b) : m_b (b) { m_b = true; }
        ~PendingSnoop () { m_b = false; }
        bool & m_b;
        } pending (m_bSnoopPending);
      ModalGuard guard (this);
      bPermitted = ::UMessageBox (
          TFormat ("%s wishes to start snooping you.\n\nPermit it?",
                   (LPCTSTR) m_strRemoteUserName),
        MB_YESNO) == IDYES;
      }

    if (m_bDeleteMe)
      return;     // they hung up while we were asking

    if (!bPermitted)
      {
      // tell them our rejection
      SendChatMessage (CHAT_MESSAGE, 
        TFormat ("\n%s does not want you to snoop just now.\n",
                  (LPCTSTR) m_pDoc->m_strOurChatName));
      }   // end of snoop not allowed right now
    else
      {
      // tell them our acceptance
      SendChatMessage (CHAT_MESSAGE, 
        TFormat ("\nYou are now snooping %s.\n",
                  (LPCTSTR) m_pDoc->m_strOurChatName));
      m_pDoc->ChatNote (eChatInformation,
                  TFormat ("%s is now snooping you.",
                                (LPCTSTR) m_strRemoteUserName));
      m_bHeIsSnooping = true;
      }

    }   // end of allowed to snoop
  else
    {   // not allowed to snoop
    SendChatMessage (CHAT_MESSAGE, 
        TFormat ("\n%s has not given you permission to snoop.\n",
                  (LPCTSTR) m_pDoc->m_strOurChatName));
    m_pDoc->ChatNote (eChatInformation,
                TFormat ("%s attempted to snoop you.",
                              (LPCTSTR) m_strRemoteUserName));
    }   // end of not allowed to snoop

  } // end of CChatSocket::Process_Snoop

void CChatSocket::Process_Snoop_data				  (const CString strMessage)
  {
  // Snoop data is the peer's text like any other, and was the one kind shown
  // past every control the user has over that: straight to AnsiNote, so it
  // ignored "ignore", the world's byte and line limits on a chat message,
  // "ignore chat colours" and ON_PLUGIN_CHAT_DISPLAY (whose documented
  // message type 15, eChatSnoopData, was never sent).
  if (m_bIgnore)
    return;

  // undocumented feature - first 4 digits are foreground/background colours
  const bool bColours = strMessage.GetLength () >= 4 && IsNumber (strMessage.Left (4));
  CString strText = bColours ? strMessage.Mid (4) : strMessage;

  if (!m_pDoc->ChatDisplayFilter (eChatSnoopData, strText))
    return;   // a plugin took it

  if (m_pDoc->m_bIgnoreChatColours)
    strText = ::StripAnsi (strText);

  // save old colours - because we switch to the chat colour below
  bool bOldNotesInRGB = m_pDoc->m_bNotesInRGB;
  COLORREF iOldNoteColourFore = m_pDoc->m_iNoteColourFore;
  COLORREF iOldNoteColourBack = m_pDoc->m_iNoteColourBack;

  m_pDoc->ColourTell ("springgreen", "black", ">");

  if (bColours && !m_pDoc->m_bIgnoreChatColours)
    {
    int iFore = atoi (strMessage.Left (2));
    int iBack = atoi (strMessage.Mid (2, 2));
    // foreground colour should be 0 to 15 (0 to 7 is normal, 8 to 15 is bold)
    if (iFore >= 0 && iFore <= 7)
      m_pDoc->SetNoteColourFore (m_pDoc->m_normalcolour [iFore]);
    else
      if (iFore >= 8 && iFore <= 15)
        m_pDoc->SetNoteColourFore (m_pDoc->m_boldcolour [iFore - 8]);
    // background colour should be 0 to 7
    if (iBack >= 0 && iBack <= 7)
      m_pDoc->SetNoteColourBack (m_pDoc->m_normalcolour [iBack]);
    }

  m_pDoc->AnsiNote (strText);

  // put the colours back
  if (bOldNotesInRGB)
    {
    m_pDoc->m_iNoteColourFore = iOldNoteColourFore;
    m_pDoc->m_iNoteColourBack = iOldNoteColourBack;
    }
  else  
    m_pDoc->m_bNotesInRGB = false;

  m_bYouAreSnooping = true;   // we seem to be snooping them :)

  } // end of CChatSocket::Process_Snoop_data


void CChatSocket::Process_Send_command				  (const CString strMessage)
  {
  if (m_bCanSendCommands)
    {   
      // tell them our acceptance
      SendChatMessage (CHAT_MESSAGE, 
        TFormat ("\nYou command %s to '%s'.\n",
                  (LPCTSTR) m_pDoc->m_strOurChatName ,
                  (LPCTSTR) strMessage));
      m_pDoc->ChatNote (eChatCommand,
                  TFormat ("%s commands you to '%s'.",
                                (LPCTSTR) m_strRemoteUserName,
                                (LPCTSTR) strMessage));
    m_pDoc->m_iExecutionDepth = 0;
    m_pDoc->Execute (strMessage);
    }   // end of allowed to send commands
  else
    {   // not allowed to send commands
    SendChatMessage (CHAT_MESSAGE, 
        TFormat ("\n%s has not given you permission to send commands.\n",
                  (LPCTSTR) m_pDoc->m_strOurChatName));
    m_pDoc->ChatNote (eChatCommand,
                TFormat ("%s attempted to send you a command.",
                              (LPCTSTR) m_strRemoteUserName));
    }   // end of not allowed to end commands

  } // end of CChatSocket::Process_Send_command


void CChatSocket::Process_Icon               (const CString strMessage)
  {
  // ignore it
  } // end of CChatSocket::Process_Icon 

void CChatSocket::Process_Status             (const CString strMessage)
  {
  if (!strMessage.IsEmpty ())
    m_zChatStatus = (unsigned char) strMessage [0];
  } // end of CChatSocket::Process_Status

void CChatSocket::Process_EmailAddress			 (const CString strMessage)
  {
  m_strEmailAddress = strMessage;
  } // end of CChatSocket::Process_EmailAddress

void CChatSocket::Process_Stamp				       (const CString strMessage)
  {

  if (m_iChatConnectionType != eChatZMud)
    return;

  if (strMessage.GetLength () != 4)
    return;   // should be 4-byte stamp

  CString strFixedMessage = strMessage;
  long iStamp = GetStamp (strFixedMessage);

  if (iStamp == m_zChatStamp)    // if same as ours, make a new one
    {
    // make new stamp
    SHS_INFO shsInfo;
    MakeRandomNumber (m_pDoc, shsInfo);

    m_zChatStamp = shsInfo.digest [0];

    // broadcast new stamp so they know we have changed it
    for (POSITION chatpos = m_pDoc->m_ChatList.GetHeadPosition (); chatpos; )
      {
      CChatSocket * pSocket = m_pDoc->m_ChatList.GetNext (chatpos);
      if (pSocket->m_iChatStatus == eChatConnected)
        {
        // the PEER's protocol: testing this socket's type (always zChat here)
        // sent zChat stamp messages to MudMaster peers as well
        if (pSocket->m_iChatConnectionType != eChatZMud)
          continue;

        // the 4-byte wire form, as at the handshake -- it went out as decimal
        // text, which a zChat peer reads as a garbage stamp (and a length-6+
        // one, which GetStamp's callers refuse)
        pSocket->SendChatMessage (CHAT_STAMP, MakeStamp (m_zChatStamp));  // send it
        }   // end of chat session not being deleted
      } // end of all chat sessions

    }   // end of new stamp wanted

  } // end of CChatSocket::Process_Stamp	

void CChatSocket::Process_RequestPGPkey			 (const CString strMessage)
  {
  SendChatMessage (CHAT_MESSAGE, "Cannot send PGP key.");
  } // end of CChatSocket::Process_RequestPGPkey

void CChatSocket::Process_PGPkey				     (const CString strMessage)  
  {
  m_strPGPkey = strMessage;
  } // end of CChatSocket::Process_PGPkey


long CChatSocket::GetStamp (CString & strMessage)    // extracts message stamp
  {
  if (m_iChatConnectionType != eChatZMud)
    return 0;

  if (strMessage.GetLength () < 4)
    return 0;   // should be 4-byte stamp

  unsigned int iStamp = (unsigned int) (unsigned char) strMessage [0] |
              (((unsigned int) (unsigned char) strMessage [1]) << 8) |
              (((unsigned int) (unsigned char) strMessage [2]) << 16) |
              (((unsigned int) (unsigned char) strMessage [3]) << 24);

  strMessage = strMessage.Mid (4);  // get rid of stamp part

  return (long) iStamp;   // zero-extend, matching how m_zChatStamp is assigned from an unsigned SHS digest word

  }  // end of CChatSocket::GetStamp


CString CChatSocket::MakeStamp (const long iStamp)
  {
  if (m_iChatConnectionType != eChatZMud)
    return "";

  CString strStamp;

  unsigned char * p = (unsigned char *) strStamp.GetBuffer (4);

  p [0] = (unsigned char) iStamp & 0xFF;
  p [1] = (unsigned char) (iStamp >> 8) & 0xFF;
  p [2] = (unsigned char) (iStamp >> 16) & 0xFF;
  p [3] = (unsigned char) (iStamp >> 24) & 0xFF;

  strStamp.ReleaseBuffer (4);

  return strStamp;

}  // end of CChatSocket::MakeStamp
