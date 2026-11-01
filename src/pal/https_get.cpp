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
#else
#include "platform/posix_transport.h"    // ConfigureTrustAnchors
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

// One request.  True if a response came back, whatever its status.
bool FetchOnce (SSL_CTX * ctx, const Url & url, const std::string & userAgent,
                const time_t deadline, int * status, std::string * headers,
                std::string * body, std::string * error)
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
  char buffer [16384];
  bool cleanEnd = false;
  for (;;)
    {
    const int n = BIO_read (bio.get (), buffer, sizeof buffer);
    if (n > 0)
      {
      raw.append (buffer, (size_t) n);
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

} // anonymous namespace

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

bool HttpsGet (const std::string & url, const int timeoutSeconds, const std::string & userAgent,
               int * status, std::string * body, std::string * error)
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

  const time_t deadline = time (NULL) + (timeoutSeconds > 0 ? timeoutSeconds : kHttpsGetDefaultTimeoutSeconds);
  std::string current = url;
  for (int redirects = 0; ; redirects++)
    {
    Url parsed;
    if (!ParseUrl (current, &parsed, error))
      return false;

    std::string headers;
    if (!FetchOnce (ctx.get (), parsed, userAgent, deadline, status, &headers, body, error))
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

} // namespace pal

#else // no OpenSSL in this build

namespace pal {

bool HttpsGet (const std::string &, int, const std::string &, int *, std::string *, std::string * error)
  {
  *error = "this build has no TLS";
  return false;
  }

bool HttpsGetDecodeBody (const std::string &, const std::string & raw, std::string * body)
  {
  *body = raw;
  return true;
  }

} // namespace pal

#endif
