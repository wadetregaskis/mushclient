// pal/https_get.cpp — see pal/https_get.h.

#include "stdafx.h"
#include "pal/https_get.h"

#if defined(_WIN32) || defined(OPENSSL_FOUND)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#ifdef _WIN32
#include <wincrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include "platform/posix_transport.h"    // ConfigureTrustAnchors
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

namespace pal {

namespace {

const int    kMaxRedirects    = 3;
const size_t kMaxResponseSize = 32 * 1024 * 1024;

void AddTrustAnchors (SSL_CTX * ctx)
  {
#ifdef _WIN32
  // As WinTransport::startTls does.
  HCERTSTORE hStore = CertOpenSystemStore (0, "ROOT");
  if (!hStore)
    return;
  X509_STORE * store = SSL_CTX_get_cert_store (ctx);
  PCCERT_CONTEXT pCertCtx = NULL;
  while ((pCertCtx = CertEnumCertificatesInStore (hStore, pCertCtx)) != NULL)
    {
    const unsigned char * pDer = pCertCtx->pbCertEncoded;
    X509 * x509 = d2i_X509 (NULL, &pDer, pCertCtx->cbCertEncoded);
    if (x509)
      {
      X509_STORE_add_cert (store, x509);
      X509_free (x509);
      }
    }
  CertCloseStore (hStore, 0);
#else
  ConfigureTrustAnchors (ctx);
#endif
  }

std::string Lower (std::string s)
  {
  std::transform (s.begin (), s.end (), s.begin (),
                  [] (unsigned char c) { return (char) std::tolower (c); });
  return s;
  }

// The value of a header, from a block of them, or "" if it is not there.
std::string HeaderValue (const std::string & headers, const std::string & name)
  {
  const std::string lower = Lower (headers);
  const std::string wanted = "\r\n" + Lower (name) + ":";
  const size_t at = lower.find (wanted);
  if (at == std::string::npos)
    return "";
  size_t start = at + wanted.size ();
  size_t end = headers.find ("\r\n", start);
  if (end == std::string::npos)
    end = headers.size ();
  while (start < end && headers [start] == ' ')
    start++;
  while (end > start && headers [end - 1] == ' ')
    end--;
  return headers.substr (start, end - start);
  }

struct Url
  {
  std::string host, port, path;
  };

bool ParseUrl (const std::string & url, Url * out, std::string * error)
  {
  static const std::string kScheme = "https://";
  if (Lower (url.substr (0, kScheme.size ())) != kScheme)
    {
    *error = "only https:// URLs are supported";
    return false;
    }
  const size_t hostStart = kScheme.size ();
  const size_t pathStart = url.find_first_of ("/?#", hostStart);
  std::string authority = url.substr (hostStart, pathStart == std::string::npos
                                                 ? std::string::npos : pathStart - hostStart);
  out->path = pathStart == std::string::npos ? "/" : url.substr (pathStart);
  if (out->path [0] != '/')
    out->path = "/" + out->path;
  const size_t fragment = out->path.find ('#');
  if (fragment != std::string::npos)
    out->path.resize (fragment);

  // A control character or a space would let the URL write its own request headers.
  for (const unsigned char c : url)
    if (c <= ' ' || c == 0x7F)
      {
      *error = "the URL contains a space or a control character";
      return false;
      }

  // [v6]:port, host:port or host
  out->port = "443";
  if (!authority.empty () && authority [0] == '[')
    {
    const size_t close = authority.find (']');
    if (close == std::string::npos)
      {
      *error = "the URL's address is malformed";
      return false;
      }
    if (close + 1 < authority.size () && authority [close + 1] == ':')
      out->port = authority.substr (close + 2);
    out->host = authority.substr (1, close - 1);
    }
  else
    {
    const size_t colon = authority.rfind (':');
    if (colon != std::string::npos)
      {
      out->port = authority.substr (colon + 1);
      authority.resize (colon);
      }
    out->host = authority;
    }
  if (out->host.empty () || out->host.find ('@') != std::string::npos)
    {
    *error = "the URL has no usable host name";
    return false;
    }
  return true;
  }

std::string SslError (const char * what)
  {
  const unsigned long code = ERR_peek_last_error ();
  std::string message = what;
  if (code)
    {
    char text [256] = "";
    ERR_error_string_n (code, text, sizeof text);
    message += ": ";
    message += text;
    }
  ERR_clear_error ();
  return message;
  }

// Where a download's content goes.  Null for "into *body".
struct Sink
  {
  FILE * file;
  int    stallSeconds;
  const std::function<bool (uint64_t, uint64_t)> * progress;
  };

// One request.  True if a response came back, whatever its status.
bool FetchOnce (SSL_CTX * ctx, const Url & url, const std::string & userAgent,
                time_t deadline, int * status, std::string * headers,
                std::string * body, std::string * error, const Sink * sink = nullptr)
  {
  std::unique_ptr<BIO, decltype (&BIO_free_all)> bio (BIO_new_ssl_connect (ctx), BIO_free_all);
  if (!bio)
    {
    *error = SslError ("could not start a TLS connection");
    return false;
    }

  SSL * ssl = NULL;
  BIO_get_ssl (bio.get (), &ssl);
  if (!ssl)
    {
    *error = SslError ("could not start a TLS connection");
    return false;
    }
  const bool numeric = url.host.find (':') != std::string::npos;     // an IPv6 address
  if (numeric)
    X509_VERIFY_PARAM_set1_ip_asc (SSL_get0_param (ssl), url.host.c_str ());
  else
    {
    SSL_set_tlsext_host_name (ssl, url.host.c_str ());
    SSL_set1_host (ssl, url.host.c_str ());
    }
  BIO_set_conn_hostname (bio.get (), url.host.c_str ());
  BIO_set_conn_port (bio.get (), url.port.c_str ());
  BIO_set_nbio (bio.get (), 1);

  const time_t now = time (NULL);
  const int remaining = deadline > now ? (int) (deadline - now) : 0;
  const int connected = remaining > 0 ? BIO_do_connect_retry (bio.get (), remaining, 100) : 0;
  if (connected != 1)
    {
    const long verify = SSL_get_verify_result (ssl);
    if (connected == 0)
      {
      ERR_clear_error ();
      *error = "timed out connecting to " + url.host;
      }
    else if (verify != X509_V_OK)
      {
      ERR_clear_error ();
      *error = "the certificate of " + url.host + " was refused: " +
               X509_verify_cert_error_string (verify);
      }
    else
      *error = SslError (("could not connect to " + url.host).c_str ());
    return false;
    }

  const std::string request =
      "GET " + url.path + " HTTP/1.1\r\n"
      "Host: " + (numeric ? "[" + url.host + "]" : url.host) + "\r\n"
      "User-Agent: " + userAgent + "\r\n"
      "Accept: */*\r\n"
      "Connection: close\r\n\r\n";

  size_t sent = 0;
  while (sent < request.size ())
    {
    const int n = BIO_write (bio.get (), request.data () + sent, (int) (request.size () - sent));
    if (n > 0)
      {
      sent += (size_t) n;
      continue;
      }
    if (!BIO_should_retry (bio.get ()) || BIO_wait (bio.get (), deadline, 100) != 1)
      {
      *error = time (NULL) >= deadline ? "timed out sending the request"
                                       : SslError ("could not send the request");
      return false;
      }
    }

  std::string raw;
  char buffer [65536];
  bool cleanEnd = false;
  bool streaming = false;     // a download's 200, going to the file rather than to raw
  uint64_t streamed = 0;
  uint64_t expected = 0;
  for (;;)
    {
    const int n = BIO_read (bio.get (), buffer, sizeof buffer);
    if (n > 0)
      {
      if (sink)
        deadline = time (NULL) + sink->stallSeconds;

      if (!streaming)
        raw.append (buffer, (size_t) n);

      // Once a download's headers are in, its content can go straight to the file:
      // unless it is chunked, which needs the whole thing, and which GitHub's
      // file servers don't do.
      const char * toWrite = nullptr;
      size_t writeSize = 0;
      if (streaming)
        {
        toWrite = buffer;
        writeSize = (size_t) n;
        }
      else if (sink)
        {
        const size_t end = raw.find ("\r\n\r\n");
        if (end != std::string::npos)
          {
          const std::string first = raw.substr (0, end);
          const size_t space = first.find (' ');
          const int code = space == std::string::npos ? 0 : atoi (first.c_str () + space + 1);
          if (code == 200 && Lower (HeaderValue (first, "Transfer-Encoding")).find ("chunked") == std::string::npos)
            {
            *status = code;
            *headers = first;
            expected = strtoull (HeaderValue (first, "Content-Length").c_str (), NULL, 10);
            streaming = true;
            toWrite = raw.data () + end + 4;
            writeSize = raw.size () - end - 4;
            }
          }
        }

      if (streaming)
        {
        if (writeSize && fwrite (toWrite, 1, writeSize, sink->file) != writeSize)
          {
          *error = "could not write the downloaded file (is the disk full?)";
          return false;
          }
        streamed += writeSize;
        if (raw.size () > 0)
          raw.clear ();
        if (*sink->progress && !(*sink->progress) (streamed, expected))
          {
          *error = "cancelled";
          return false;
          }
        continue;
        }

      if (raw.size () > kMaxResponseSize)
        {
        *error = "the response is too large";
        return false;
        }
      continue;
      }
    if (n == 0 && !BIO_should_retry (bio.get ()))
      {
      cleanEnd = true;
      break;
      }
    if (!BIO_should_retry (bio.get ()))
      break;         // judged below: a whole response that lacked a close_notify is fine
    if (BIO_wait (bio.get (), deadline, 100) != 1)
      {
      *error = "timed out waiting for the response";
      return false;
      }
    }
  ERR_clear_error ();

  if (streaming)
    {
    if (expected ? streamed < expected : !cleanEnd)
      {
      *error = "the download was cut short";
      return false;
      }
    return true;
    }

  const size_t headersEnd = raw.find ("\r\n\r\n");
  if (headersEnd == std::string::npos || raw.compare (0, 5, "HTTP/") != 0)
    {
    *error = "the server's response could not be understood";
    return false;
    }
  const size_t space = raw.find (' ');
  *status = space == std::string::npos ? 0 : atoi (raw.c_str () + space + 1);
  *headers = raw.substr (0, headersEnd);

  // Without a clean TLS close, only a length or a last chunk says it is all here.
  const std::string framed = Lower (HeaderValue (*headers, "Transfer-Encoding")) +
                             HeaderValue (*headers, "Content-Length");
  if (!HttpsGetDecodeBody (*headers, raw.substr (headersEnd + 4), body) ||
      (!cleanEnd && framed.empty ()))
    {
    *error = "the response was cut short";
    return false;
    }
  return true;
  }

// --- SafeGet's own connection, so that it is to the address that was checked.

#ifdef _WIN32
typedef SOCKET SafeSocket;
const SafeSocket kNoSocket = INVALID_SOCKET;
void CloseSafeSocket (SafeSocket s) { closesocket (s); }
bool SetNonBlocking (SafeSocket s, bool bOn) { u_long mode = bOn ? 1 : 0; return ioctlsocket (s, FIONBIO, &mode) == 0; }
#else
typedef int SafeSocket;
const SafeSocket kNoSocket = -1;
void CloseSafeSocket (SafeSocket s) { close (s); }
bool SetNonBlocking (SafeSocket s, bool bOn)
  {
  const int flags = fcntl (s, F_GETFL, 0);
  return flags >= 0 && fcntl (s, F_SETFL, bOn ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
  }
#endif

// Wait for a socket to be ready.  False at the deadline.
bool WaitSocket (const SafeSocket s, const bool bWrite, const time_t deadline)
  {
  const time_t now = time (NULL);
  if (now >= deadline)
    return false;
  fd_set set;
  FD_ZERO (&set);
  FD_SET (s, &set);
  timeval timeout;
  timeout.tv_sec = (long) (deadline - now);
  timeout.tv_usec = 0;
  return select ((int) s + 1, bWrite ? NULL : &set, bWrite ? &set : NULL, NULL, &timeout) > 0;
  }

// What is left after the URL has been judged.
struct SafeUrl
  {
  bool bHttps;
  Url url;
  };

bool ParseSafeUrl (const std::string & text, const bool bAllowHttp, SafeUrl * out, std::string * error)
  {
  const std::string lower = Lower (text.substr (0, 8));
  out->bHttps = lower.compare (0, 8, "https://") == 0;
  const bool bHttp = lower.compare (0, 7, "http://") == 0;
  if (bHttp && !bAllowHttp)
    {
    *error = "plain http is not allowed";
    return false;
    }
  if (!out->bHttps && !bHttp)
    {
    *error = "only http and https URLs are supported";
    return false;
    }
  // ParseUrl knows https, and everything after the scheme is the same
  if (!ParseUrl (out->bHttps ? text : "https://" + text.substr (7), &out->url, error))
    return false;
  if (!out->bHttps && out->url.port == "443")
    {
    const size_t authorityEnd = text.find_first_of ("/?#", 7);
    const std::string authority = text.substr (7, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - 7);
    if (authority.rfind (':') == std::string::npos || authority.back () == ']')
      out->url.port = "80";
    }
  return true;
  }

// Resolve, judge all the answers, and connect to one of them.
SafeSocket ConnectToPublicAddress (const Url & url, const time_t deadline, std::string * error)
  {
  addrinfo hints;
  memset (&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;

  addrinfo * answers = NULL;
  if (getaddrinfo (url.host.c_str (), url.port.c_str (), &hints, &answers) != 0 || !answers)
    {
    *error = "could not find the address of " + url.host;
    return kNoSocket;
    }

  // All of them.  A name with one public address and one private one is a name to distrust.
  for (const addrinfo * a = answers; a; a = a->ai_next)
    {
    bool bPublic = false;
    if (a->ai_family == AF_INET)
      bPublic = IsPublicAddress ((const unsigned char *) &((const sockaddr_in *) a->ai_addr)->sin_addr, 4);
    else if (a->ai_family == AF_INET6)
      bPublic = IsPublicAddress ((const unsigned char *) &((const sockaddr_in6 *) a->ai_addr)->sin6_addr, 16);
    if (!bPublic)
      {
      freeaddrinfo (answers);
      *error = url.host + " has an address on a private network, which is not allowed";
      return kNoSocket;
      }
    }

  SafeSocket connected = kNoSocket;
  for (const addrinfo * a = answers; a && connected == kNoSocket; a = a->ai_next)
    {
    const SafeSocket s = socket (a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kNoSocket)
      continue;
    SetNonBlocking (s, true);
    const int result = connect (s, a->ai_addr, (int) a->ai_addrlen);
    bool bOK = result == 0;
    if (!bOK && WaitSocket (s, true, deadline))
      {
      int socketError = 0;
      socklen_t size = sizeof socketError;
      bOK = getsockopt (s, SOL_SOCKET, SO_ERROR, (char *) &socketError, &size) == 0 && socketError == 0;
      }
    if (bOK)
      connected = s;
    else
      CloseSafeSocket (s);
    }

  freeaddrinfo (answers);
  if (connected == kNoSocket)
    *error = "could not connect to " + url.host;
  return connected;
  }

// Everything below is the same for both but for how bytes move.
class SafeConnection
  {
  public:
    SafeConnection (SafeSocket s, SSL * ssl, time_t deadline) : m_socket (s), m_ssl (ssl), m_deadline (deadline) { }
    ~SafeConnection ()
      {
      if (m_ssl)
        SSL_free (m_ssl);
      CloseSafeSocket (m_socket);
      }

    bool Handshake (std::string * error, const std::string & host)
      {
      if (!m_ssl)
        return true;
      for (;;)
        {
        ERR_clear_error ();
        const int result = SSL_connect (m_ssl);
        if (result == 1)
          return true;
        const int reason = SSL_get_error (m_ssl, result);
        if (reason == SSL_ERROR_WANT_READ && WaitSocket (m_socket, false, m_deadline))
          continue;
        if (reason == SSL_ERROR_WANT_WRITE && WaitSocket (m_socket, true, m_deadline))
          continue;
        const long verify = SSL_get_verify_result (m_ssl);
        *error = verify != X509_V_OK ? "the certificate of " + host + " was refused: " + X509_verify_cert_error_string (verify)
                                     : SslError (("could not connect to " + host).c_str ());
        return false;
        }
      }

    bool Write (const std::string & data)
      {
      size_t sent = 0;
      while (sent < data.size ())
        {
        int n;
        if (m_ssl)
          {
          ERR_clear_error ();
          n = SSL_write (m_ssl, data.data () + sent, (int) (data.size () - sent));
          if (n <= 0)
            {
            const int reason = SSL_get_error (m_ssl, n);
            if ((reason == SSL_ERROR_WANT_READ && WaitSocket (m_socket, false, m_deadline)) ||
                (reason == SSL_ERROR_WANT_WRITE && WaitSocket (m_socket, true, m_deadline)))
              continue;
            return false;
            }
          }
        else
          {
          if (!WaitSocket (m_socket, true, m_deadline))
            return false;
          n = (int) send (m_socket, data.data () + sent, (int) (data.size () - sent), 0);
          if (n <= 0)
            return false;
          }
        sent += (size_t) n;
        }
      return true;
      }

    // >0 bytes, 0 at the end, <0 if it went wrong or took too long
    int Read (char * buffer, const int size)
      {
      for (;;)
        {
        if (m_ssl)
          {
          ERR_clear_error ();
          const int n = SSL_read (m_ssl, buffer, size);
          if (n > 0)
            return n;
          const int reason = SSL_get_error (m_ssl, n);
          if (reason == SSL_ERROR_ZERO_RETURN)
            return 0;
          if ((reason == SSL_ERROR_WANT_READ && WaitSocket (m_socket, false, m_deadline)) ||
              (reason == SSL_ERROR_WANT_WRITE && WaitSocket (m_socket, true, m_deadline)))
            continue;
          // A connection closed with no close_notify: the framing will judge what we got.
          return 0;
          }
        if (!WaitSocket (m_socket, false, m_deadline))
          return -1;
        const int n = (int) recv (m_socket, buffer, size, 0);
        return n < 0 ? -1 : n;
        }
      }

  private:
    SafeSocket m_socket;
    SSL * m_ssl;
    time_t m_deadline;
  };

bool SafeFetchOnce (SSL_CTX * ctx, const SafeUrl & target, const SafeGetOptions & options,
                    const time_t deadline, int * status, std::string * headers,
                    std::string * body, std::string * error)
  {
  const Url & url = target.url;

  const SafeSocket s = ConnectToPublicAddress (url, deadline, error);
  if (s == kNoSocket)
    return false;

  SSL * ssl = NULL;
  if (target.bHttps)
    {
    ssl = SSL_new (ctx);
    if (!ssl)
      {
      CloseSafeSocket (s);
      *error = SslError ("could not start a TLS connection");
      return false;
      }
    SSL_set_fd (ssl, (int) s);
    SSL_set_connect_state (ssl);
    const bool numeric = url.host.find (':') != std::string::npos;
    if (numeric)
      X509_VERIFY_PARAM_set1_ip_asc (SSL_get0_param (ssl), url.host.c_str ());
    else
      {
      SSL_set_tlsext_host_name (ssl, url.host.c_str ());
      SSL_set1_host (ssl, url.host.c_str ());
      }
    }

  SafeConnection connection (s, ssl, deadline);

  if (!connection.Handshake (error, url.host))
    return false;

  const bool numeric = url.host.find (':') != std::string::npos;
  const std::string request =
      "GET " + url.path + " HTTP/1.1\r\n"
      "Host: " + (numeric ? "[" + url.host + "]" : url.host) + "\r\n"
      "User-Agent: " + options.userAgent + "\r\n"
      "Accept: */*\r\n"
      "Connection: close\r\n\r\n";

  if (!connection.Write (request))
    {
    *error = "could not send the request";
    return false;
    }

  std::string raw;
  char buffer [16384];
  bool bEnded = false;
  const size_t kHeaderAllowance = 64 * 1024;
  while (!bEnded)
    {
    const int n = connection.Read (buffer, sizeof buffer);
    if (n < 0)
      {
      *error = "timed out waiting for the response";
      return false;
      }
    if (n == 0)
      bEnded = true;
    else
      raw.append (buffer, (size_t) n);
    if (raw.size () > options.maxBytes + kHeaderAllowance)
      {
      *error = "the file is too large";
      return false;
      }
    }

  const size_t headersEnd = raw.find ("\r\n\r\n");
  if (headersEnd == std::string::npos || raw.compare (0, 5, "HTTP/") != 0)
    {
    *error = "the server's response could not be understood";
    return false;
    }
  const size_t space = raw.find (' ');
  *status = space == std::string::npos ? 0 : atoi (raw.c_str () + space + 1);
  *headers = raw.substr (0, headersEnd);

  if (!HttpsGetDecodeBody (*headers, raw.substr (headersEnd + 4), body))
    {
    *error = "the response was cut short";
    return false;
    }
  if (body->size () > options.maxBytes)
    {
    *error = "the file is too large";
    return false;
    }
  return true;
  }

} // anonymous namespace

bool IsPublicAddress (const unsigned char * a, const size_t length)
  {
  if (length == 16)
    {
    static const unsigned char kZero [16] = { 0 };

    if (memcmp (a, kZero, 15) == 0 && a [15] <= 1)     // :: and ::1
      return false;
    if ((a [0] & 0xFE) == 0xFC)                        // fc00::/7, unique local
      return false;
    if (a [0] == 0xFE && (a [1] & 0xC0) == 0x80)       // fe80::/10, link local
      return false;
    if (a [0] == 0xFE && (a [1] & 0xC0) == 0xC0)       // fec0::/10, old site local
      return false;
    if (a [0] == 0xFF)                                 // multicast
      return false;
    if (a [0] == 0x20 && a [1] == 0x01 && a [2] == 0x0D && a [3] == 0xB8)     // 2001:db8::/32
      return false;
    if (a [0] == 0x20 && a [1] == 0x01 && a [2] == 0 && a [3] == 0)           // 2001::/32, Teredo
      return false;

    // IPv4 inside IPv6, each of which reaches what its IPv4 address does.
    static const unsigned char kMapped [12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
    static const unsigned char kNat64 [12]  = { 0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0 };
    if (memcmp (a, kMapped, 12) == 0 || memcmp (a, kZero, 12) == 0 || memcmp (a, kNat64, 12) == 0)
      return IsPublicAddress (a + 12, 4);
    if (a [0] == 0x20 && a [1] == 0x02)                // 2002::/16, 6to4
      return IsPublicAddress (a + 2, 4);

    return true;
    }

  if (length != 4)
    return false;

  const unsigned int a0 = a [0], a1 = a [1], a2 = a [2];

  if (a0 == 0 || a0 == 10 || a0 == 127)                    // this network, private, loopback
    return false;
  if (a0 == 100 && a1 >= 64 && a1 <= 127)                  // CGNAT
    return false;
  if (a0 == 169 && a1 == 254)                              // link local
    return false;
  if (a0 == 172 && a1 >= 16 && a1 <= 31)                   // private
    return false;
  if (a0 == 192 && a1 == 168)                              // private
    return false;
  if (a0 == 192 && a1 == 0 && (a2 == 0 || a2 == 2))        // IETF, TEST-NET-1
    return false;
  if (a0 == 192 && a1 == 88 && a2 == 99)                   // 6to4 relay
    return false;
  if (a0 == 198 && (a1 == 18 || a1 == 19))                 // benchmarking
    return false;
  if (a0 == 198 && a1 == 51 && a2 == 100)                  // TEST-NET-2
    return false;
  if (a0 == 203 && a1 == 0 && a2 == 113)                   // TEST-NET-3
    return false;
  if (a0 >= 224)                                           // multicast, reserved, broadcast
    return false;
  return true;
  }

bool SafeGet (const std::string & url, const SafeGetOptions & options,
              int * status, std::string * body, std::string * error)
  {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
    {
    *error = "could not start networking";
    return false;
    }
  struct WsaCleanup { ~WsaCleanup () { WSACleanup (); } } wsaCleanup;
#endif

  std::unique_ptr<SSL_CTX, decltype (&SSL_CTX_free)> ctx (SSL_CTX_new (TLS_client_method ()), SSL_CTX_free);
  if (!ctx)
    {
    *error = SslError ("could not start TLS");
    return false;
    }
  SSL_CTX_set_min_proto_version (ctx.get (), TLS1_2_VERSION);
  SSL_CTX_set_verify (ctx.get (), SSL_VERIFY_PEER, NULL);
  AddTrustAnchors (ctx.get ());

  const time_t deadline = time (NULL) + (options.timeoutSeconds > 0 ? options.timeoutSeconds
                                                                    : kHttpsGetDefaultTimeoutSeconds);

  SafeUrl first;
  if (!ParseSafeUrl (url, options.bAllowHttp, &first, error))
    return false;

  SafeUrl current = first;
  for (int redirects = 0; ; redirects++)
    {
    std::string headers;
    if (!SafeFetchOnce (ctx.get (), current, options, deadline, status, &headers, body, error))
      return false;

    const std::string location = HeaderValue ("\r\n" + headers, "Location");
    if (*status < 300 || *status >= 400 || location.empty ())
      return true;
    if (redirects >= kMaxRedirects)
      {
      *error = "too many redirects";
      return false;
      }

    SafeUrl next = current;
    if (location [0] == '/')
      next.url.path = location;
    else if (!ParseSafeUrl (location, options.bAllowHttp, &next, error))
      return false;

    if (Lower (next.url.host) != Lower (first.url.host))
      {
      *error = "it redirected to another host, " + next.url.host;
      return false;
      }
    if (current.bHttps && !next.bHttps)
      {
      *error = "it redirected from https to http";
      return false;
      }
    current = next;
    }
  }

bool HttpsGetDecodeBody (const std::string & headers, const std::string & raw, std::string * body)
  {
  if (Lower (HeaderValue (headers, "Transfer-Encoding")).find ("chunked") == std::string::npos)
    {
    const std::string length = HeaderValue (headers, "Content-Length");
    if (!length.empty () && raw.size () < (size_t) strtoull (length.c_str (), NULL, 10))
      return false;
    *body = raw;
    return true;
    }

  body->clear ();
  size_t at = 0;
  for (;;)
    {
    const size_t lineEnd = raw.find ("\r\n", at);
    if (lineEnd == std::string::npos)
      return false;
    const size_t size = (size_t) strtoull (raw.c_str () + at, NULL, 16);
    at = lineEnd + 2;
    if (size == 0)
      return true;
    if (size > raw.size () - at)
      return false;
    body->append (raw, at, size);
    at += size + 2;      // and its CRLF
    if (at > raw.size ())
      return false;
    }
  }

namespace {

bool FetchFollowingRedirects (const std::string & url, const int timeoutSeconds, const std::string & userAgent,
                              int * status, std::string * body, std::string * error, const Sink * sink)
  {
  std::unique_ptr<SSL_CTX, decltype (&SSL_CTX_free)> ctx (SSL_CTX_new (TLS_client_method ()), SSL_CTX_free);
  if (!ctx)
    {
    *error = SslError ("could not start TLS");
    return false;
    }
  SSL_CTX_set_min_proto_version (ctx.get (), TLS1_2_VERSION);
  SSL_CTX_set_verify (ctx.get (), SSL_VERIFY_PEER, NULL);
  AddTrustAnchors (ctx.get ());

  const int seconds = timeoutSeconds > 0 ? timeoutSeconds : kHttpsGetDefaultTimeoutSeconds;
  time_t deadline = time (NULL) + seconds;
  std::string current = url;
  for (int redirects = 0; ; redirects++)
    {
    Url parsed;
    if (!ParseUrl (current, &parsed, error))
      return false;

    // Each hop of a download gets a fresh start: the file server is a different host.
    if (sink)
      deadline = time (NULL) + seconds;

    std::string headers;
    if (!FetchOnce (ctx.get (), parsed, userAgent, deadline, status, &headers, body, error, sink))
      return false;

    const std::string location = HeaderValue (headers, "Location");
    if (*status < 300 || *status >= 400 || location.empty ())
      return true;
    if (redirects >= kMaxRedirects)
      {
      *error = "too many redirects";
      return false;
      }
    current = location [0] == '/'
        ? "https://" + (parsed.host.find (':') != std::string::npos ? "[" + parsed.host + "]" : parsed.host) +
          (parsed.port == "443" ? "" : ":" + parsed.port) + location
        : location;
    }
  }

} // anonymous namespace

bool HttpsGet (const std::string & url, const int timeoutSeconds, const std::string & userAgent,
               int * status, std::string * body, std::string * error)
  {
  return FetchFollowingRedirects (url, timeoutSeconds, userAgent, status, body, error, nullptr);
  }

bool HttpsDownload (const std::string & url, const int timeoutSeconds, const std::string & userAgent,
                    const std::string & path,
                    const std::function<bool (uint64_t, uint64_t)> & progress,
                    int * status, std::string * error)
  {
  FILE * file = fopen (path.c_str (), "wb");
  if (!file)
    {
    *error = "could not create " + path;
    return false;
    }
  const Sink sink = { file, timeoutSeconds > 0 ? timeoutSeconds : kHttpsGetDefaultTimeoutSeconds, &progress };
  std::string body;
  bool bOK = FetchFollowingRedirects (url, timeoutSeconds, userAgent, status, &body, error, &sink);
  // A chunked answer came back whole, rather than going straight to the file.
  if (bOK && *status == 200 && !body.empty () && fwrite (body.data (), 1, body.size (), file) != body.size ())
    {
    *error = "could not write the downloaded file (is the disk full?)";
    bOK = false;
    }
  if (fclose (file) != 0 && bOK)
    {
    *error = "could not write the downloaded file (is the disk full?)";
    bOK = false;
    }
  return bOK;
  }

} // namespace pal

#else // no OpenSSL in this build

namespace pal {

bool HttpsGet (const std::string &, int, const std::string &, int *, std::string *, std::string * error)
  {
  *error = "this build has no TLS";
  return false;
  }

bool SafeGet (const std::string &, const SafeGetOptions &, int *, std::string *, std::string * error)
  {
  *error = "this build has no TLS";
  return false;
  }

bool HttpsDownload (const std::string &, int, const std::string &, const std::string &,
                    const std::function<bool (uint64_t, uint64_t)> &, int *, std::string * error)
  {
  *error = "this build has no TLS";
  return false;
  }

bool IsPublicAddress (const unsigned char *, size_t)
  {
  return false;
  }

bool HttpsGetDecodeBody (const std::string &, const std::string & raw, std::string * body)
  {
  *body = raw;
  return true;
  }

} // namespace pal

#endif
