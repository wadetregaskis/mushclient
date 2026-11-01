// pal/https_get.h — fetch one URL over HTTPS, for utils.https_get.
//
// LuaSocket has no TLS, and the sites a plugin wants (GitHub's release API, for
// one) refuse plain HTTP.  This is the client's own OpenSSL, with the same trust
// anchors as a world's TLS connection (the system's roots), verifying the chain
// and the host name.  There is no way to turn that off.
//
// It BLOCKS for up to timeoutSeconds, in all, so that is how long a caller on the
// UI thread freezes the window if the far end is slow.

#pragma once

#include <string>

namespace pal {

const int kHttpsGetDefaultTimeoutSeconds = 10;

// True if a response came back, of any status: *status has it (404 too) and *body
// the content.  False with *error saying why if none did.  Follows up to three
// redirects, to https only.
bool HttpsGet (const std::string & url, int timeoutSeconds,
               const std::string & userAgent,
               int * status, std::string * body, std::string * error);

// A fetch on behalf of a server, rather than of the player's own script.  The server
// chose the URL, so it might point anywhere, and this one will not go just anywhere:
//
//   Every address the name resolves to must be a public one (see IsPublicAddress), and
//   the connection is made to the address that was checked, so a name cannot be made to
//   change its mind between the check and the connection.  http only if bAllowHttp.
//   Redirects stay on the same host, and never from https to http.  Nothing past maxBytes.
struct SafeGetOptions
  {
  bool bAllowHttp;
  size_t maxBytes;
  int timeoutSeconds;
  std::string userAgent;
  SafeGetOptions () : bAllowHttp (false), maxBytes (16 * 1024 * 1024), timeoutSeconds (60) { }
  };

bool SafeGet (const std::string & url, const SafeGetOptions & options,
              int * status, std::string * body, std::string * error);

// Is this an address on the internet proper?  Not loopback, private, link-local, CGNAT,
// multicast, reserved or unspecified, in IPv4, in IPv6, or in IPv4 in disguise in IPv6.
// The length is 4 or 16.
bool IsPublicAddress (const unsigned char * address, size_t length);

// For tests: a response's content, given its headers, with any chunked encoding
// undone.  False if it is incomplete (a chunked body with no last chunk, or fewer
// bytes than Content-Length).
bool HttpsGetDecodeBody (const std::string & headers, const std::string & raw,
                         std::string * body);

} // namespace pal
