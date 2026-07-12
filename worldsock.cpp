// worldsock.cpp : implementation file
//

// But does it get goat's blood out?



#include "stdafx.h"
#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
  #include "MUSHclient.h"
  #include "mainfrm.h"
  #include "doc.h"
  #include <openssl/ssl.h>
  #include <openssl/err.h>
#else
  #include "platform/doc_stub.h"
  #ifdef OPENSSL_FOUND
    #include <openssl/ssl.h>
    #include <openssl/err.h>
  #endif
#endif

#include "worldsock.h"
#include <stddef.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

IMPLEMENT_DYNAMIC(CWorldSocket, CAsyncSocket)

CWorldSocket::CWorldSocket(CMUSHclientDoc* pDoc)
{
	m_pDoc = pDoc;
}

void CWorldSocket::OnReceive(int nErrorCode)
{
  // save m_pDoc locally — if the handshake fails, 'this' (the socket) gets
  // deleted inside ReceiveMsg, so we must not touch 'this' afterwards
  CMUSHclientDoc * pDoc = m_pDoc;

  pDoc->ReceiveMsg();

  // if the socket was destroyed (e.g. SSL handshake failure), don't touch anything
  if (pDoc->m_pSocket == NULL)
    return;

  // SSL may have buffered more decrypted data than one SSL_read consumed.
  // Since we won't get another FD_READ for already-buffered data, drain it now.
  if (pDoc->m_pSSL && pDoc->m_bSSL_Connected)
    {
    // Re-check m_pSSL/m_bSSL_Connected every iteration, not just SSL_pending:
    // the in-loop ReceiveMsg() can take the clean-shutdown path (SSL_read ->
    // SSL_ERROR_ZERO_RETURN -> OnClose -> SSLCleanup), which frees m_pSSL and
    // sets it NULL *without* destroying m_pSocket. The m_pSocket==NULL guard
    // below therefore would not fire, and re-evaluating SSL_pending(m_pSSL)
    // with m_pSSL==NULL dereferences NULL (OpenSSL's SSL_pending does not
    // NULL-check its argument). Short-circuit on the same condition that
    // gated entry.
    while (pDoc->m_pSSL && pDoc->m_bSSL_Connected && SSL_pending (pDoc->m_pSSL) > 0)
      {
      pDoc->ReceiveMsg();
      if (pDoc->m_pSocket == NULL)
        return;
      }
    }

  // On non-Windows, the MFC view/invalidation mechanism is absent; push any
  // new lines from m_LineList to the GUI output pane explicitly.
  // On Windows this is a no-op extra UpdateAllViews call (harmless).
  pDoc->UpdateAllViews (NULL);

  CAsyncSocket::OnReceive(nErrorCode);
}

void CWorldSocket::OnSend(int nErrorCode)
{

  // save m_pDoc locally — if the TLS handshake fails, 'this' (the socket) gets
  // deleted inside ContinueSSLHandshake, so we must not touch 'this' afterwards
  CMUSHclientDoc * pDoc = m_pDoc;

int count;

   if (nErrorCode)    // had an error, give up!
      return;

  // if we are in the middle of a TLS handshake, continue it
  if (pDoc->m_iConnectPhase == eConnectAwaitingSSLHandshake)
    {
    pDoc->ContinueSSLHandshake ();
    return;  // always return - if handshake failed, socket may be destroyed
    }

// if we have outstanding data to send, do it
  if (m_outstanding_data.empty ())
    return;

  // SSL-aware send
  if (pDoc->m_pSSL && pDoc->m_bSSL_Connected)
    {
    count = SSL_write (pDoc->m_pSSL, m_outstanding_data.data (),
                       m_outstanding_data.length ());
    if (count > 0)
      {
      pDoc->m_nBytesOut += count;
      m_outstanding_data.erase (0, count);
      }
    else
      {
      int ssl_err = SSL_get_error (pDoc->m_pSSL, count);
      if (ssl_err != SSL_ERROR_WANT_WRITE && ssl_err != SSL_ERROR_WANT_READ)
        {
        ShutDownSocket (*this);
        m_outstanding_data.erase ();
        }
      }
    }
  else
    {
    count = Send (m_outstanding_data.data (), m_outstanding_data.length ());

    if (count != SOCKET_ERROR)
      pDoc->m_nBytesOut += count; // count bytes out

    if (count > 0)    // good send - do rest later
      m_outstanding_data.erase (0, count);
    else
      {
      int nError = GetLastError ();
      if (count == SOCKET_ERROR && nError != WSAEWOULDBLOCK)
        {
        ShutDownSocket (*this);
  //       m_pSocket->OnClose (nError);      // ????
        m_outstanding_data.erase ();

        }   // end of an error other than "would block"
      } // end of an error
    }

}

void CWorldSocket::OnClose(int nErrorCode)
  {
  // The disconnect bookkeeping (scripts, plugin callbacks, stats Notes, UX)
  // is engine logic; it lives in CMUSHclientDoc::ConnectionClosed
  // (doc_shared.cpp) — ITransport migration step T2a.
  m_pDoc->ConnectionClosed (nErrorCode);
  } // end of OnClose

void CWorldSocket::OnConnect(int nErrorCode)
  {

  m_pDoc->OnConnect (nErrorCode);
  } // end of OnConnect
