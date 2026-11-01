// tests/win/test_https_get.cpp -- pal/https_get.cpp, without the network: what it
// refuses before connecting, and how it decodes a response.  Real TLS against a real
// site, including a certificate it must refuse, was checked by hand in the client.

#include "mc_test.h"
#include "stdafx.h"
#include "pal/https_get.h"

#include <string>

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
