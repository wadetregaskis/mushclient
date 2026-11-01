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

#include <cstdint>
#include <functional>
#include <string>

namespace pal {

const int kHttpsGetDefaultTimeoutSeconds = 10;

// True if a response came back, of any status: *status has it (404 too) and *body
// the content.  False with *error saying why if none did.  Follows up to three
// redirects, to https only.
bool HttpsGet (const std::string & url, int timeoutSeconds,
               const std::string & userAgent,
               int * status, std::string * body, std::string * error);

// The same, for something too big to hold in memory: a 200's content goes to `path`,
// a piece at a time.  It is the wait for each piece that is limited to timeoutSeconds,
// not the whole download.  `progress`, if any, is told how far it has got (total is 0
// if the server didn't say), and returning false from it stops the download.  True
// with *status if a response came back: only 200 will have written anything.
bool HttpsDownload (const std::string & url, int timeoutSeconds,
                    const std::string & userAgent, const std::string & path,
                    const std::function<bool (uint64_t done, uint64_t total)> & progress,
                    int * status, std::string * error);

// For tests: a response's content, given its headers, with any chunked encoding
// undone.  False if it is incomplete (a chunked body with no last chunk, or fewer
// bytes than Content-Length).
bool HttpsGetDecodeBody (const std::string & headers, const std::string & raw,
                         std::string * body);

} // namespace pal
