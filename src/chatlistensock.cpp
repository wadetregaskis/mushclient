// chatlistensock.cpp : implementation file
//

// But does it get goat's blood out?



#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include <stddef.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

IMPLEMENT_DYNAMIC(CChatListenSocket, CAsyncSocket)

CChatListenSocket::CChatListenSocket(CMUSHclientDoc* pDoc)
{
	m_pDoc = pDoc;

}   // end of constructor

CChatListenSocket::~CChatListenSocket()
{

// cancel callbacks for closed sockets

  Close ();

}     // end of destructor

// How many chat sessions a world will hold, and how many incoming calls may sit
// unidentified at once.  Nothing limited either, so anyone able to reach the
// port could open connections until the process ran out of sockets.
static const int kMaxChatSessions         = 100;
static const int kMaxUnidentifiedIncoming = 10;

void CChatListenSocket::OnAccept(int nErrorCode)
  {

// Every ChatNote below can run a plugin's ON_PLUGIN_CHAT_DISPLAY, and that
// plugin may call ChatStopAcceptingCalls -- which deletes THIS socket.  So the
// call is accepted before the first note, and nothing reads a member after one.
CMUSHclientDoc * pDoc = m_pDoc;
CChatSocket * pSocket = new CChatSocket (pDoc);
struct sockaddr_storage peer;     // an IPv4 or an IPv6 caller
int SockAddrLen = sizeof peer;

  memset (&peer, 0, sizeof peer);
  const BOOL bAccepted = Accept(*pSocket,
                                (SOCKADDR*) &peer,
                                &SockAddrLen);
  if (bAccepted)
    pal::FromSockaddr ((const struct sockaddr *) &peer,
                       pSocket->m_ServerAddr, &pSocket->m_iServerPort);

  pDoc->ChatNote (eChatConnection, "Incoming chat call");

  if (!bAccepted)
    {
    pDoc->ChatNote (eChatConnection, "Cannot accept call.");
    delete pSocket;
    return;
    }

  pSocket->m_bIncoming = true;
  pSocket->m_strServerName = pal::FormatAddress (pSocket->m_ServerAddr).c_str ();

  int iSessions = 0, iUnidentified = 0;
  for (POSITION pos = pDoc->m_ChatList.GetHeadPosition (); pos; )
    {
    CChatSocket * pOther = pDoc->m_ChatList.GetNext (pos);
    if (pOther->m_bDeleteMe)
      continue;
    iSessions++;
    if (pOther->m_bIncoming && pOther->m_iChatStatus == eChatAwaitingConnectionRequest)
      iUnidentified++;
    }

  if (iSessions >= kMaxChatSessions || iUnidentified >= kMaxUnidentifiedIncoming)
    {
    pDoc->ChatNote (eChatConnection,
                    TFormat ("Refused call from %s: too many chat connections.",
                             (LPCTSTR) pSocket->m_strServerName));
    delete pSocket;     // closes the accepted socket
    return;
    }

  pSocket->AsyncSelect ();

  pDoc->ChatNote (eChatConnection,
                  TFormat (
                    "Accepted call from %s port %d",
                            (LPCTSTR) pSocket->m_strServerName,
                            pSocket->m_iServerPort));

  pDoc->m_ChatList.AddTail (pSocket);

  pSocket->m_iChatStatus = eChatAwaitingConnectionRequest;
  } // end of OnAccept


