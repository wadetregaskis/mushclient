// UDPsocket.cpp : implementation file
//

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "UDPsocket.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// UDPsocket

UDPsocket::UDPsocket(CMUSHclientDoc * pDoc)
{
m_pDoc = pDoc;

}

UDPsocket::~UDPsocket()
{
}


// Do not edit the following lines, which are needed by ClassWizard.
#if 0
BEGIN_MESSAGE_MAP(UDPsocket, CAsyncSocket)
	//{{AFX_MSG_MAP(UDPsocket)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()
#endif	// 0

/////////////////////////////////////////////////////////////////////////////
// UDPsocket member functions

void UDPsocket::OnReceive(int nErrorCode)
  {
  // Room for the largest UDP datagram (65,507 bytes of payload).  The buffer
  // was 1,000 bytes, and a datagram that does not fit fails the whole receive
  // with WSAEMSGSIZE -- so anything longer was dropped without a word.
  static char buff [65536];
  int count = Receive (buff, sizeof (buff) - 1);

  if (count == SOCKET_ERROR)
    return;   // strange

  buff [count] = 0;   // null terminate it

  // Copies, not the members: the handler may call world.UdpListen on this
  // port, which deletes THIS socket while CallPlugin is still using the
  // routine and plugin names it was given.
  const CString strPluginID = m_strPluginID;
  const CString strScript   = m_strScript;
  CMUSHclientDoc * pDoc     = m_pDoc;

  // call script
  pDoc->CallPlugin (strPluginID, strScript, buff);

  }
