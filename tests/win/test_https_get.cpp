// tests/win/test_https_get.cpp -- pal/https_get.cpp, without the network: what it
// refuses before connecting, how it decodes a response, and (HttpsLoopback,
// SafeGetLoopback) what it makes of a TLS server of the test's own on the loopback
// address, which says what the test wants it to.  Real TLS against a real site,
// including a certificate it must refuse, was checked by hand in the client.

#include "mc_test.h"
#include "stdafx.h"
#include "pal/https_get.h"

#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Result
  {
  bool ok = false;
  int status = 0;
  std::string body, error;
  };

Result get (const char * url)
  {
  Result r;
  r.ok = pal::HttpsGet (url, 2, "MUSHclient-test", &r.status, &r.body, &r.error);
  return r;
  }

std::string decode (const char * headers, const std::string & raw, bool * ok = nullptr)
  {
  std::string body;
  const bool decoded = pal::HttpsGetDecodeBody (headers, raw, &body);
  if (ok)
    *ok = decoded;
  return body;
  }

} // namespace

TEST(HttpsGet, RefusesAnythingButHttps)
{
    for (const char * url : { "http://example.com/", "ftp://example.com/", "example.com", "" })
    {
        const Result r = get (url);
        EXPECT_FALSE(r.ok);
        EXPECT_TRUE(r.error.find ("only https") != std::string::npos);
    }
}

TEST(HttpsGet, RefusesAUrlThatWouldWriteItsOwnHeaders)
{
    const Result r = get ("https://example.com/a\r\nX-Evil: yes");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.find ("control character") != std::string::npos);
}

TEST(HttpsGet, RefusesAHostItCannotUse)
{
    EXPECT_FALSE(get ("https://").ok);
    EXPECT_FALSE(get ("https:///path").ok);
    EXPECT_FALSE(get ("https://user@example.com/").ok);
    EXPECT_FALSE(get ("https://[::1/").ok);
}

TEST(HttpsGet, AConnectionRefusedIsAnError)
{
    // Port 1 on the loopback address: nothing listens there.
    const Result r = get ("https://127.0.0.1:1/");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty ());
}

TEST(HttpsGet, APlainBodyIsReturnedAsIs)
{
    bool ok = false;
    EXPECT_EQ(std::string ("hello"), decode ("HTTP/1.1 200 OK\r\nContent-Length: 5", "hello", &ok));
    EXPECT_TRUE(ok);
    EXPECT_EQ(std::string ("no length"), decode ("HTTP/1.1 200 OK", "no length", &ok));
    EXPECT_TRUE(ok);
}

TEST(HttpsGet, FewerBytesThanContentLengthIsCutShort)
{
    bool ok = true;
    decode ("HTTP/1.1 200 OK\r\nContent-Length: 6", "hello", &ok);
    EXPECT_FALSE(ok);
}

TEST(HttpsGet, AChunkedBodyIsDecoded)
{
    bool ok = false;
    const std::string body = decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked",
                                     "5\r\nhello\r\n1;ext=1\r\n \r\nA\r\n0123456789\r\n0\r\n\r\n", &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(std::string ("hello 0123456789"), body);
}

TEST(HttpsGet, HeaderNamesAreNotCaseSensitive)
{
    bool ok = false;
    EXPECT_EQ(std::string ("hi"), decode ("HTTP/1.1 200 OK\r\ntransfer-ENCODING: Chunked", "2\r\nhi\r\n0\r\n\r\n", &ok));
    EXPECT_TRUE(ok);
}

TEST(HttpsGet, AChunkedBodyWithNoLastChunkIsCutShort)
{
    bool ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "5\r\nhello\r\n", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "FF\r\nhello", &ok);
    EXPECT_FALSE(ok);
}

TEST(HttpsGet, AChunkSizeThatIsNotHexIsMalformed)
{
    // strtoull made each of these 0, the last chunk: an empty body, and "all here".
    bool ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "zz\r\nhello\r\n0\r\n\r\n", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "\r\nhello\r\n0\r\n\r\n", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "5 junk\r\nhello\r\n0\r\n\r\n", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "5\r\nhelloXX0\r\n\r\n", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked", "ffffffffffffffffff\r\nhello\r\n0\r\n\r\n", &ok);
    EXPECT_FALSE(ok);

    // Spaces after the size are allowed, and so is an extension.
    ok = false;
    EXPECT_EQ(std::string ("hello"), decode ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked",
                                             "5 \r\nhello\r\n0\r\n\r\n", &ok));
    EXPECT_TRUE(ok);
}

TEST(HttpsGet, AContentLengthIsSixtyFourBitsAndDigits)
{
    // 4 GB and 5 bytes: as a 32-bit size_t, 5.
    bool ok = true;
    decode ("HTTP/1.1 200 OK\r\nContent-Length: 4294967301", "hello", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nContent-Length: 5x", "hello", &ok);
    EXPECT_FALSE(ok);
    ok = true;
    decode ("HTTP/1.1 200 OK\r\nContent-Length: -1", "hello", &ok);
    EXPECT_FALSE(ok);

    // And nothing past it.
    ok = false;
    EXPECT_EQ(std::string ("hello"), decode ("HTTP/1.1 200 OK\r\nContent-Length: 5", "hello world", &ok));
    EXPECT_TRUE(ok);
}

// ------------------------------------------------- against a server of our own

namespace {

// A certificate for the loopback names and addresses, made once, and trusted by the
// fetches (HttpsTestTrust) while a test wants it.
struct TestIdentity
  {
  EVP_PKEY * pkey = nullptr;
  X509 * cert = nullptr;
  std::string pem;

  TestIdentity ()
    {
    pkey = EVP_EC_gen ("P-256");
    cert = X509_new ();
    X509_set_version (cert, 2);
    ASN1_INTEGER_set (X509_get_serialNumber (cert), 1);
    X509_gmtime_adj (X509_getm_notBefore (cert), -60);
    X509_gmtime_adj (X509_getm_notAfter (cert), 3600);
    X509_set_pubkey (cert, pkey);
    X509_NAME * name = X509_NAME_new ();
    X509_NAME_add_entry_by_txt (name, "CN", MBSTRING_ASC, (const unsigned char *) "localhost", -1, -1, 0);
    X509_set_subject_name (cert, name);
    X509_set_issuer_name (cert, name);
    X509_NAME_free (name);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb (&v3);
    X509V3_set_ctx (&v3, cert, cert, NULL, NULL, 0);
    if (X509_EXTENSION * san = X509V3_EXT_conf_nid (NULL, &v3, NID_subject_alt_name,
                                                     "DNS:localhost,IP:127.0.0.1,IP:::1"))
      {
      X509_add_ext (cert, san, -1);
      X509_EXTENSION_free (san);
      }
    X509_sign (cert, pkey, EVP_sha256 ());
    BIO * bio = BIO_new (BIO_s_mem ());
    PEM_write_bio_X509 (bio, cert);
    char * data = nullptr;
    const long size = BIO_get_mem_data (bio, &data);
    pem.assign (data, (size_t) size);
    BIO_free (bio);
    }

  ~TestIdentity ()
    {
    X509_free (cert);
    EVP_PKEY_free (pkey);
    }
  };

TestIdentity & Identity ()
  {
  static TestIdentity identity;
  return identity;
  }

// For the length of a test: our certificate trusted, and SafeGet let reach loopback.
struct Trusting
  {
  explicit Trusting (const bool bLoopback = false)
    {
    pal::HttpsTestTrust (Identity ().pem);
    pal::SafeGetTestAllowLoopback (bLoopback);
    }
  ~Trusting ()
    {
    pal::HttpsTestTrust ("");
    pal::SafeGetTestAllowLoopback (false);
    }
  };

// How a connection ends, once the reply is written.
enum class End
  {
  Clean,      // close_notify, then the socket closed
  Cut,        // the socket closed with no close_notify: a connection cut, or a liar
  Hold        // nothing, for holdMs, as a stalled server or one that ignores "Connection: close"
  };

struct Reply
  {
  std::string bytes;
  End end;
  int holdMs;
  };

Reply Response (const std::string & statusLine, const std::string & content, const std::string & headers = "",
                const End end = End::Clean, const int holdMs = 0)
  {
  return Reply { "HTTP/1.1 " + statusLine + "\r\nContent-Length: " + std::to_string (content.size ()) + "\r\n" +
                 headers + "\r\n" + content, end, holdMs };
  }

// Answers one connection per reply, in order, on 127.0.0.1 or ::1.
class LoopbackServer
  {
  public:
    LoopbackServer (const int family, const std::vector<Reply> & replies) : m_bV6 (family == AF_INET6)
      {
      WSADATA wsa;
      WSAStartup (MAKEWORD (2, 2), &wsa);
      m_listener = socket (family, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_storage address;
      memset (&address, 0, sizeof address);
      int length = 0;
      if (m_bV6)
        {
        sockaddr_in6 * in6 = (sockaddr_in6 *) &address;
        in6->sin6_family = AF_INET6;
        in6->sin6_addr = in6addr_loopback;
        length = sizeof (sockaddr_in6);
        }
      else
        {
        sockaddr_in * in = (sockaddr_in *) &address;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        length = sizeof (sockaddr_in);
        }
      ok = m_listener != INVALID_SOCKET && ::bind (m_listener, (sockaddr *) &address, length) == 0 &&
           listen (m_listener, 4) == 0 && getsockname (m_listener, (sockaddr *) &address, &length) == 0;
      if (ok)
        port = ntohs (m_bV6 ? ((sockaddr_in6 *) &address)->sin6_port : ((sockaddr_in *) &address)->sin_port);
      m_thread = std::thread ([this, replies] { Serve (replies); });
      }

    ~LoopbackServer ()
      {
      m_bStop = true;
      m_thread.join ();
      if (m_listener != INVALID_SOCKET)
        closesocket (m_listener);
      WSACleanup ();
      }

    std::string Url (const std::string & path) const
      {
      return std::string ("https://") + (m_bV6 ? "[::1]" : "127.0.0.1") + ":" + std::to_string (port) + path;
      }

    std::vector<std::string> Requests ()
      {
      std::lock_guard<std::mutex> lock (m_mutex);
      return m_requests;
      }

    bool ok = false;
    int port = 0;

  private:
    void Serve (const std::vector<Reply> & replies)
      {
      SSL_CTX * ctx = SSL_CTX_new (TLS_server_method ());
      SSL_CTX_use_certificate (ctx, Identity ().cert);
      SSL_CTX_use_PrivateKey (ctx, Identity ().pkey);
      for (const Reply & reply : replies)
        {
        if (!ok)
          break;
        SOCKET c = INVALID_SOCKET;
        while (!m_bStop && c == INVALID_SOCKET)
          {
          fd_set set;
          FD_ZERO (&set);
          FD_SET (m_listener, &set);
          timeval wait = { 0, 50000 };
          if (select (0, &set, NULL, NULL, &wait) > 0)
            c = accept (m_listener, NULL, NULL);
          }
        if (c == INVALID_SOCKET)
          break;

        SSL * ssl = SSL_new (ctx);
        SSL_set_fd (ssl, (int) c);
        if (SSL_accept (ssl) == 1)
          {
          std::string request;
          char buffer [4096];
          while (request.find ("\r\n\r\n") == std::string::npos)
            {
            const int n = SSL_read (ssl, buffer, sizeof buffer);
            if (n <= 0)
              break;
            request.append (buffer, (size_t) n);
            }
            {
            std::lock_guard<std::mutex> lock (m_mutex);
            m_requests.push_back (request);
            }
          if (!reply.bytes.empty ())
            SSL_write (ssl, reply.bytes.data (), (int) reply.bytes.size ());
          if (reply.end == End::Hold)
            for (int waited = 0; waited < reply.holdMs && !m_bStop; waited += 20)
              std::this_thread::sleep_for (std::chrono::milliseconds (20));
          if (reply.end != End::Cut)
            SSL_shutdown (ssl);
          }
        SSL_free (ssl);
        closesocket (c);
        }
      SSL_CTX_free (ctx);
      }

    bool m_bV6;
    SOCKET m_listener = INVALID_SOCKET;
    std::thread m_thread;
    std::atomic<bool> m_bStop { false };
    std::mutex m_mutex;
    std::vector<std::string> m_requests;
  };

std::string TempFile (const char * name)
  {
  const char * dir = getenv ("TEMP");
  return (std::filesystem::path (dir ? dir : ".") / (std::string ("mc_https_") + name)).u8string ();
  }

std::string ReadFile (const std::string & path)
  {
  std::ifstream f (std::filesystem::u8path (path), std::ios::binary);
  std::stringstream s;
  s << f.rdbuf ();
  return s.str ();
  }

double SecondsSince (const std::chrono::steady_clock::time_point start)
  {
  return std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
  }

pal::SafeGetOptions SafeOptions (const int timeoutSeconds)
  {
  pal::SafeGetOptions options;
  options.timeoutSeconds = timeoutSeconds;
  options.userAgent = "MUSHclient-test";
  return options;
  }

} // namespace

TEST(HttpsLoopback, ARedirectsOwnContentIsNotPartOfTheDownload)
{
    Trusting trusting;
    const std::string path = TempFile ("redirected.bin");

    // GitHub's file links redirect, and a redirect may say something of its own.
    {
    LoopbackServer server (AF_INET, { Response ("302 Found", "You are being redirected.", "Location: /the-file\r\n"),
                                      Response ("200 OK", "the package") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string error;
    uint64_t last = 0;
    EXPECT_TRUE(pal::HttpsDownload (server.Url ("/start"), 5, "MUSHclient-test", path,
                                    [&] (uint64_t done, uint64_t) { last = done; return true; }, &status, &error));
    EXPECT_EQ(std::string (), error);
    EXPECT_EQ(200, status);
    EXPECT_EQ(std::string ("the package"), ReadFile (path));
    EXPECT_EQ((uint64_t) 11, last);
    const std::vector<std::string> requests = server.Requests ();
    ASSERT_EQ((size_t) 2, requests.size ());
    EXPECT_EQ((size_t) 0, requests [1].find ("GET /the-file HTTP/1.1\r\n"));
    }

    // The same with a chunked 200, which comes back whole rather than streamed.
    {
    LoopbackServer server (AF_INET, { Response ("302 Found", "You are being redirected.", "Location: /the-file\r\n"),
                                      Reply { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                              "5\r\nhello\r\n0\r\n\r\n", End::Clean, 0 } });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string error;
    EXPECT_TRUE(pal::HttpsDownload (server.Url ("/start"), 5, "MUSHclient-test", path, nullptr, &status, &error));
    EXPECT_EQ(std::string ("hello"), ReadFile (path));
    }
    std::error_code ec;
    std::filesystem::remove (std::filesystem::u8path (path), ec);
}

TEST(HttpsLoopback, AnIPv6AddressIsBracketedForTheConnection)
{
    Trusting trusting;
    LoopbackServer server (AF_INET6, { Response ("200 OK", "over six") });
    if (!server.ok)
    {
        mc_test::skip_current ("this machine has no IPv6 loopback address");
        return;
    }
    int status = 0;
    std::string body, error;
    EXPECT_TRUE(pal::HttpsGet (server.Url ("/"), 5, "MUSHclient-test", &status, &body, &error));
    EXPECT_EQ(std::string (), error);
    EXPECT_EQ(200, status);
    EXPECT_EQ(std::string ("over six"), body);
    const std::vector<std::string> requests = server.Requests ();
    ASSERT_EQ((size_t) 1, requests.size ());
    EXPECT_TRUE(requests [0].find ("\r\nHost: [::1]:" + std::to_string (server.port) + "\r\n") != std::string::npos);
}

TEST(HttpsLoopback, AnIPv4AddressIsCheckedAgainstTheCertificatesAddresses)
{
    Trusting trusting;
    LoopbackServer server (AF_INET, { Response ("200 OK", "over four") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string body, error;
    EXPECT_TRUE(pal::HttpsGet (server.Url ("/"), 5, "MUSHclient-test", &status, &body, &error));
    EXPECT_EQ(std::string (), error);
    EXPECT_EQ(std::string ("over four"), body);
}

TEST(HttpsLoopback, OurCertificateIsOnlyTrustedWhenATestSaysSo)
{
    LoopbackServer server (AF_INET, { Response ("200 OK", "secret") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string body, error;
    EXPECT_FALSE(pal::HttpsGet (server.Url ("/"), 5, "MUSHclient-test", &status, &body, &error));
    EXPECT_TRUE(error.find ("certificate") != std::string::npos);
}

TEST(HttpsLoopback, AServerThatKeepsTheConnectionOpenIsNotWaitedFor)
{
    Trusting trusting (true);
    {
    LoopbackServer server (AF_INET, { Response ("200 OK", "all of it", "", End::Hold, 8000) });
    ASSERT_TRUE(server.ok);
    const auto start = std::chrono::steady_clock::now ();
    int status = 0;
    std::string body, error;
    EXPECT_TRUE(pal::HttpsGet (server.Url ("/"), 6, "MUSHclient-test", &status, &body, &error));
    EXPECT_EQ(std::string ("all of it"), body);
    EXPECT_LT(SecondsSince (start), 4.0);
    }
    {
    LoopbackServer server (AF_INET, { Response ("200 OK", "all of it", "", End::Hold, 8000) });
    ASSERT_TRUE(server.ok);
    const auto start = std::chrono::steady_clock::now ();
    int status = 0;
    std::string body, error;
    EXPECT_TRUE(pal::SafeGet (server.Url ("/"), SafeOptions (6), &status, &body, &error));
    EXPECT_EQ(std::string ("all of it"), body);
    EXPECT_LT(SecondsSince (start), 4.0);
    }
}

TEST(SafeGetLoopback, ACutOrStalledResponseIsNotTakenAsWhole)
{
    Trusting trusting (true);
    struct Case { Reply reply; bool bWhole; const char * why; };
    const Case cases [] = {
        // No length, and the connection cut: as much as came, so no telling.
        { Reply { "HTTP/1.1 200 OK\r\n\r\nhalf a sou", End::Cut, 0 }, false, "cut short" },
        // Fewer than it said, then cut.
        { Reply { "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nten bytes!", End::Cut, 0 }, false, "cut short" },
        // Fewer than it said, then nothing: a timeout is not the end.
        { Reply { "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nten bytes!", End::Hold, 5000 }, false, "timed out" },
        // A chunked one with no last chunk, then cut.
        { Reply { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", End::Cut, 0 }, false, "cut short" },
        // These are whole.
        { Reply { "HTTP/1.1 200 OK\r\n\r\nno length, but a clean end", End::Clean, 0 }, true, "" },
        { Reply { "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nten bytes!", End::Cut, 0 }, true, "" },
    };
    for (const Case & c : cases)
    {
        LoopbackServer server (AF_INET, { c.reply });
        ASSERT_TRUE(server.ok);
        int status = 0;
        std::string body, error;
        const bool bOK = pal::SafeGet (server.Url ("/x.wav"), SafeOptions (2), &status, &body, &error);
        EXPECT_EQ(c.bWhole, bOK);
        if (!c.bWhole)
            EXPECT_TRUE(error.find (c.why) != std::string::npos);
    }
}

TEST(SafeGetLoopback, ARefusedConnectionFailsAtOnce)
{
    Trusting trusting (true);
    // A port nothing listens on: one that was just given back.
    int port = 0;
    {
    LoopbackServer server (AF_INET, {});
    ASSERT_TRUE(server.ok);
    port = server.port;
    }
    const auto start = std::chrono::steady_clock::now ();
    int status = 0;
    std::string body, error;
    EXPECT_FALSE(pal::SafeGet ("https://127.0.0.1:" + std::to_string (port) + "/x.wav", SafeOptions (20),
                               &status, &body, &error));
    EXPECT_TRUE(error.find ("could not connect") != std::string::npos);
    // Windows tries a refused connection to loopback again for a second or two; it used
    // to wait out all 20.
    EXPECT_LT(SecondsSince (start), 10.0);
}

TEST(SafeGetLoopback, RedirectsStayOnThePortAndCannotWriteHeaders)
{
    Trusting trusting (true);
    {
    LoopbackServer server (AF_INET, { Response ("302 Found", "", "Location: /next.wav\r\n"),
                                      Response ("200 OK", "the sound") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string body, error;
    EXPECT_TRUE(pal::SafeGet (server.Url ("/x.wav"), SafeOptions (5), &status, &body, &error));
    EXPECT_EQ(std::string ("the sound"), body);
    const std::vector<std::string> requests = server.Requests ();
    ASSERT_EQ((size_t) 2, requests.size ());
    EXPECT_EQ((size_t) 0, requests [1].find ("GET /next.wav HTTP/1.1\r\n"));
    }
    {
    LoopbackServer server (AF_INET, { Response ("302 Found", "", "Location: https://127.0.0.1:1/x.wav\r\n") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string body, error;
    EXPECT_FALSE(pal::SafeGet (server.Url ("/x.wav"), SafeOptions (5), &status, &body, &error));
    EXPECT_TRUE(error.find ("another port") != std::string::npos);
    }
    {
    // A bare line feed in the Location would have gone into the next request as it was.
    LoopbackServer server (AF_INET, { Response ("302 Found", "", "Location: /a\nX-Evil: yes\r\n") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string body, error;
    EXPECT_FALSE(pal::SafeGet (server.Url ("/x.wav"), SafeOptions (5), &status, &body, &error));
    EXPECT_TRUE(error.find ("can't go") != std::string::npos);
    EXPECT_EQ((size_t) 1, server.Requests ().size ());
    }
    {
    // Nothing relative to the current path.
    LoopbackServer server (AF_INET, { Response ("302 Found", "", "Location: next.wav\r\n") });
    ASSERT_TRUE(server.ok);
    int status = 0;
    std::string body, error;
    EXPECT_FALSE(pal::SafeGet (server.Url ("/x.wav"), SafeOptions (5), &status, &body, &error));
    }
}
