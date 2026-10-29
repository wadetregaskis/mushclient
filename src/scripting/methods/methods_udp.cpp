// methods_udp.cpp

// Related to the UDP send/receive stuff


#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../errors.h"
#include "../../UDPsocket.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#endif

// Implements:

//    GetUdpPort
//    UdpListen
//    UdpPortList
//    UdpSend


// /UdpSend ("10.0.0.3", 4111, "m,textcolour,5555")

long CMUSHclientDoc::UdpSend(LPCTSTR IP, short Port, LPCTSTR Text) 
{
// timer t ("UdpSend");

SOCKET s;  // socket
struct sockaddr_storage sa;

  // work out address to send to: IPv4 (anything inet_addr takes, as before)
  // or IPv6
  pal::NetAddress address;
  if (!pal::ParseNumericAddress (IP, address))
    return -1;     // address no good
  const int saLen = pal::ToSockaddr (address, (unsigned short) Port, sa);   // 32768..65535 arrive negative

  // make socket
  s = socket(pal::SocketFamily (address), SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET)
    return WSAGetLastError ();    // cannot create socket

  // allow broadcasts (IPv6 has none: multicast needs no option to send)
  int x = 1;
  if (address.isIPv4 ())
    setsockopt( s, SOL_SOCKET, SO_BROADCAST, (const char *) &x, sizeof x);

  // send it
  long result = sendto (s, 
                        Text, strlen (Text), 
                        0,     // flags
                        (struct sockaddr *) &sa, saLen);

  // close it regardless
  closesocket (s);

  if (result == SOCKET_ERROR)
	  return WSAGetLastError ();    // return error reason

  return 0;
}  // end of  CMUSHclientDoc::UdpSend


// /world.UdpListen "0.0.0.0", 4222, "OnUDP"

long CMUSHclientDoc::UdpListen(LPCTSTR IP, short Port, LPCTSTR Script) 
{

  // I think we'll insist plugins do this
  if (m_CurrentPlugin == NULL)
    return eNotAPlugin;

  // The port arrives as a (signed) short, so 32768..65535 came in negative:
  // keyed and listed as a negative number, and handed to Create sign-extended.
  // A port is 16 unsigned bits whatever the parameter's type.
  const unsigned short uPort = (unsigned short) Port;

  // see if we already have one for this port
  map<int, UDPsocket *>::iterator it = m_UDPsocketMap.find (uPort);

  // delete existing, so we can use an empty script to achieve this
  if (it != m_UDPsocketMap.end ())
    {
    // don't re-use another plugin's port
    if (it->second->m_strPluginID != m_CurrentPlugin->m_strID)
      return eBadParameter;

    delete it->second;  // delete existing listener
    m_UDPsocketMap.erase (it);
    }

  // no script, cannot do it 
  if (strlen (Script) == 0)
    return eNoNameSpecified;

  // make a new listener
  UDPsocket * pSocket = new UDPsocket (this);

  // An IPv6 address to listen on ("::" for all of them) needs a socket of
  // that family, which Create cannot make; everything else is the IPv4
  // Create it always was, "" and "0.0.0.0" included.
  pal::NetAddress address;
  BOOL bCreated;
  if (strchr (IP, ':') && pal::ParseNumericAddress (IP, address) && address.isIPv6 ())
    {
    struct sockaddr_storage sa;
    const int saLen = pal::ToSockaddr (address, uPort, sa);
    bCreated = pSocket->Socket (SOCK_DGRAM, FD_READ, 0, AF_INET6) &&
               pSocket->Bind ((SOCKADDR *) &sa, saLen);
    }
  else
    bCreated = pSocket->Create (uPort, SOCK_DGRAM, FD_READ, IP);

	if (!bCreated)
	  {
		delete pSocket;
		return eBadParameter;
	  }     // end of can't create socket

  // remember script to call, and which plugin it is (should be?) in
  pSocket->m_strScript = Script;
  pSocket->m_strPluginID =  m_CurrentPlugin->m_strID;

  // keep a map of them
  m_UDPsocketMap [uPort] = pSocket;
	return eOK;
}   // end of CMUSHclientDoc::UdpListen


VARIANT CMUSHclientDoc::UdpPortList() 
{
  COleSafeArray sa;   // for wildcard list

  long iCount = m_UDPsocketMap.size ();

  if (iCount) // cannot create empty array dimension
    {
    sa.CreateOneDim (VT_VARIANT, iCount);
  
    map<int, UDPsocket *>::const_iterator it;

    for (iCount = 0, it = m_UDPsocketMap.begin (); 
         it != m_UDPsocketMap.end (); it++, iCount++)
      {
      // the array must be a bloody array of variants, or VBscript kicks up
      COleVariant v ((long) it->first);
      sa.PutElement (&iCount, &v);
      }      // end of looping through each colour
    } // end of having at least one

	return sa.Detach ();
}     // end of CMUSHclientDoc::UdpPortList


long CMUSHclientDoc::GetUdpPort(long First, long Last) 
{
  CMUSHclientDoc * pDoc;
  POSITION pos;

  if (First > Last || 
      First < 1 ||
      Last > 65535)
    return 0;   // bad range

  set<long> UsedPortsSet;
  set<long>::const_iterator it;

  map<int, UDPsocket *>::const_iterator i;

  // get all worlds
  for (pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos != NULL; )
    {
    pDoc = (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc(pos);

    // remember all UDP ports used by this world
    for (i = pDoc->m_UDPsocketMap.begin ();
         i != pDoc->m_UDPsocketMap.end ();
         i++)
      UsedPortsSet.insert (i->first);

    }      // end of looping through each world

  // now UsedPortsSet contains an entry for every world - find the first free one

  long result;

  for (result = First; result <= Last; result++)
    {
    it = UsedPortsSet.find (result);
    if (it == UsedPortsSet.end ())
      return result;    // this one wasn't used
    }

  return 0;   // couldn't find one
}  // end of CMUSHclientDoc::GetUdpPort
