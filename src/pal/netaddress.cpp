// pal/netaddress.cpp — see pal/netaddress.h.

#include "pal/netaddress.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <netdb.h>
#endif

#include <cstdio>
#include <cstring>

namespace pal {

namespace {

const unsigned char kMappedPrefix[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Exactly four decimal parts, each 0..255 -- the only IPv4 spelling RFC 4291
// allows at the end of an IPv6 address (inet_addr's "127.1" and hex are not).
bool ParseDottedQuad(const char* s, size_t n, unsigned char out[4])
{
    int part = 0;
    size_t i = 0;
    while (part < 4) {
        if (i >= n || s[i] < '0' || s[i] > '9')
            return false;
        unsigned v = 0;
        size_t digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (unsigned) (s[i] - '0');
            if (++digits > 3 || v > 255)
                return false;
            i++;
        }
        out[part++] = (unsigned char) v;
        if (part < 4) {
            if (i >= n || s[i] != '.')
                return false;
            i++;
        }
    }
    return i == n;
}

// RFC 4291 section 2.2 text: up to eight groups of one to four hex digits, at
// most one "::", optionally ending in a dotted quad.
bool ParseIPv6(const char* s, size_t n, unsigned char out[16])
{
    unsigned short head[8], tail[8];
    int nHead = 0, nTail = 0;
    bool compressed = false;
    size_t i = 0;

    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        compressed = true;
        i = 2;
    }
    else if (n >= 1 && s[0] == ':')
        return false;

    while (i < n) {
        size_t j = i;
        while (j < n && HexDigit(s[j]) >= 0)
            j++;

        if (j < n && s[j] == '.') {
            // the embedded IPv4 part: it must be last, and it is two groups
            unsigned char quad[4];
            if (!ParseDottedQuad(s + i, n - i, quad) || nHead + nTail + 2 > 8)
                return false;
            unsigned short* groups = compressed ? tail : head;
            int& count = compressed ? nTail : nHead;
            groups[count++] = (unsigned short) ((quad[0] << 8) | quad[1]);
            groups[count++] = (unsigned short) ((quad[2] << 8) | quad[3]);
            i = n;
            break;
        }

        if (j == i || j - i > 4 || nHead + nTail >= 8)
            return false;
        unsigned v = 0;
        for (size_t k = i; k < j; k++)
            v = (v << 4) | (unsigned) HexDigit(s[k]);
        if (compressed)
            tail[nTail++] = (unsigned short) v;
        else
            head[nHead++] = (unsigned short) v;

        i = j;
        if (i == n)
            break;
        if (s[i] != ':')
            return false;
        i++;
        if (i < n && s[i] == ':') {
            if (compressed)
                return false;      // a second "::"
            compressed = true;
            i++;
            continue;              // "::" at the very end is fine
        }
        if (i == n)
            return false;          // a single trailing colon
    }

    const int total = nHead + nTail;
    if (compressed ? total > 7 : total != 8)
        return false;

    memset(out, 0, 16);
    for (int g = 0; g < nHead; g++) {
        out[2 * g]     = (unsigned char) (head[g] >> 8);
        out[2 * g + 1] = (unsigned char) (head[g] & 0xFF);
    }
    for (int g = 0; g < nTail; g++) {
        const int at = 8 - nTail + g;
        out[2 * at]     = (unsigned char) (tail[g] >> 8);
        out[2 * at + 1] = (unsigned char) (tail[g] & 0xFF);
    }
    return true;
}

// "%3", or off Windows an interface name ("%en0").  Windows names zones by
// number only: if_nametoindex is Vista and later, and is what "%12" means
// there anyway.
bool ParseZone(const char* s, size_t n, unsigned int& zone)
{
    if (n == 0)
        return false;
    unsigned long v = 0;
    size_t i = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (unsigned long) (s[i] - '0');
        if (v > 0xFFFFFFFFul)
            return false;
        i++;
    }
    if (i == n) {
        zone = (unsigned int) v;
        return true;
    }
#ifndef _WIN32
    std::string name(s, n);
    zone = ::if_nametoindex(name.c_str());
    return zone != 0;
#else
    return false;
#endif
}

void UnmapIPv4(NetAddress& a)
{
    if (a.family == NetAddress::IPv6 && memcmp(a.bytes, kMappedPrefix, 12) == 0) {
        unsigned char v4[4];
        memcpy(v4, a.bytes + 12, 4);
        memset(a.bytes, 0, sizeof a.bytes);
        memcpy(a.bytes, v4, 4);
        a.family = NetAddress::IPv4;
        a.scopeId = 0;
    }
}

} // namespace


NetAddress NetAddress::FromIPv4(unsigned int netOrder)
{
    NetAddress a;
    a.family = IPv4;
    memcpy(a.bytes, &netOrder, 4);
    return a;
}

unsigned int NetAddress::ipv4() const
{
    unsigned int v = 0;
    if (family == IPv4)
        memcpy(&v, bytes, 4);
    return v;
}

bool NetAddress::operator==(const NetAddress& o) const
{
    if (family != o.family)
        return false;
    if (family == None)
        return true;
    return memcmp(bytes, o.bytes, family == IPv4 ? 4 : 16) == 0 &&
           scopeId == o.scopeId;
}


bool ParseNumericAddress(const char* text, NetAddress& out)
{
    out = NetAddress();
    if (!text || !*text)
        return false;

    size_t n = strlen(text);

    // IPv4 first, and exactly as before: inet_addr, so "127.1" and the rest of
    // its spellings still mean what they did, and its INADDR_NONE still sends
    // "255.255.255.255" to the resolver.
    if (!strchr(text, ':')) {
        const unsigned long v = inet_addr(text);
        if (v == INADDR_NONE)
            return false;
        out = NetAddress::FromIPv4((unsigned int) v);
        return true;
    }

    // "[...]" -- how an IPv6 address is written next to a port, so people
    // paste it that way into the host box too
    if (text[0] == '[') {
        if (n < 3 || text[n - 1] != ']')
            return false;
        text++;
        n -= 2;
    }

    const char* pct = (const char*) memchr(text, '%', n);
    const size_t addrLen = pct ? (size_t) (pct - text) : n;

    NetAddress a;
    a.family = NetAddress::IPv6;
    if (!ParseIPv6(text, addrLen, a.bytes))
        return false;
    if (pct && !ParseZone(pct + 1, n - addrLen - 1, a.scopeId))
        return false;

    UnmapIPv4(a);
    out = a;
    return true;
}


std::string FormatAddress(const NetAddress& a)
{
    char buf[64];
    if (a.family == NetAddress::IPv4) {
        snprintf(buf, sizeof buf, "%u.%u.%u.%u",
                 a.bytes[0], a.bytes[1], a.bytes[2], a.bytes[3]);
        return buf;
    }
    if (a.family != NetAddress::IPv6)
        return std::string();

    unsigned short g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (unsigned short) ((a.bytes[2 * i] << 8) | a.bytes[2 * i + 1]);

    // RFC 5952 section 4.2: the longest run of two or more zero groups
    // becomes "::" -- the first such run when two are equally long
    int bestAt = -1, bestLen = 1;
    for (int i = 0; i < 8; ) {
        if (g[i] != 0) { i++; continue; }
        int j = i;
        while (j < 8 && g[j] == 0)
            j++;
        if (j - i > bestLen) {
            bestAt = i;
            bestLen = j - i;
        }
        i = j;
    }

    std::string s;
    for (int i = 0; i < 8; i++) {
        if (i == bestAt) {
            s += "::";
            i += bestLen - 1;
            continue;
        }
        if (!s.empty() && s[s.size() - 1] != ':')
            s += ':';
        snprintf(buf, sizeof buf, "%x", g[i]);
        s += buf;
    }

    if (a.scopeId != 0) {
        snprintf(buf, sizeof buf, "%%%u", a.scopeId);
        s += buf;
    }
    return s;
}


std::string FormatAddressAndPort(const NetAddress& a, unsigned short port)
{
    char buf[16];
    snprintf(buf, sizeof buf, ":%u", (unsigned) port);
    if (a.family == NetAddress::IPv6)
        return "[" + FormatAddress(a) + "]" + buf;
    return FormatAddress(a) + buf;
}


int ToSockaddr(const NetAddress& a, unsigned short port, sockaddr_storage& out)
{
    memset(&out, 0, sizeof out);
    if (a.family == NetAddress::IPv4) {
        sockaddr_in* sin = (sockaddr_in*) &out;
        sin->sin_family = AF_INET;
        sin->sin_port   = htons(port);
        memcpy(&sin->sin_addr, a.bytes, 4);
        return (int) sizeof *sin;
    }
    if (a.family == NetAddress::IPv6) {
        sockaddr_in6* sin6 = (sockaddr_in6*) &out;
        sin6->sin6_family   = AF_INET6;
        sin6->sin6_port     = htons(port);
        memcpy(&sin6->sin6_addr, a.bytes, 16);
        sin6->sin6_scope_id = a.scopeId;
        return (int) sizeof *sin6;
    }
    return 0;
}


bool FromSockaddr(const sockaddr* sa, NetAddress& out, unsigned short* port)
{
    out = NetAddress();
    if (!sa)
        return false;
    if (sa->sa_family == AF_INET) {
        const sockaddr_in* sin = (const sockaddr_in*) sa;
        out.family = NetAddress::IPv4;
        memcpy(out.bytes, &sin->sin_addr, 4);
        if (port)
            *port = ntohs(sin->sin_port);
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const sockaddr_in6* sin6 = (const sockaddr_in6*) sa;
        out.family = NetAddress::IPv6;
        memcpy(out.bytes, &sin6->sin6_addr, 16);
        out.scopeId = sin6->sin6_scope_id;
        if (port)
            *port = ntohs(sin6->sin6_port);
        UnmapIPv4(out);
        return true;
    }
    return false;
}


int SocketFamily(const NetAddress& a)
{
    if (a.family == NetAddress::IPv4) return AF_INET;
    if (a.family == NetAddress::IPv6) return AF_INET6;
    return 0;
}


void OrderForConnecting(std::vector<NetAddress>& addresses)
{
    std::vector<NetAddress> unique;
    for (size_t i = 0; i < addresses.size(); i++) {
        if (!addresses[i].valid())
            continue;
        bool seen = false;
        for (size_t j = 0; j < unique.size() && !seen; j++)
            seen = unique[j] == addresses[i];
        if (!seen)
            unique.push_back(addresses[i]);
    }

    if (unique.empty()) {
        addresses.clear();
        return;
    }

    std::vector<NetAddress> first, other;
    const NetAddress::Family lead = unique[0].family;
    for (size_t i = 0; i < unique.size(); i++)
        (unique[i].family == lead ? first : other).push_back(unique[i]);

    addresses.clear();
    for (size_t i = 0; i < first.size() || i < other.size(); i++) {
        if (i < first.size()) addresses.push_back(first[i]);
        if (i < other.size()) addresses.push_back(other[i]);
    }
}


std::vector<NetAddress> LookUpNow(const char* host)
{
    std::vector<NetAddress> v4, v6;
    if (!host || !*host)
        return v4;

    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;   // one entry per address, not per socket type
    addrinfo* res = NULL;
    if (::getaddrinfo(host, NULL, &hints, &res) == 0) {
        for (addrinfo* p = res; p; p = p->ai_next) {
            NetAddress a;
            if (!FromSockaddr(p->ai_addr, a))
                continue;
            std::vector<NetAddress>& v = a.isIPv4() ? v4 : v6;
            bool seen = false;
            for (size_t i = 0; i < v.size() && !seen; i++)
                seen = v[i] == a;
            if (!seen)
                v.push_back(a);
        }
    }
    if (res)
        ::freeaddrinfo(res);

    v4.insert(v4.end(), v6.begin(), v6.end());
    return v4;
}


std::string NameOfAddress(const NetAddress& a)
{
    sockaddr_storage sa;
    const int len = ToSockaddr(a, 0, sa);
    if (len == 0)
        return std::string();
    char host[1025];   // NI_MAXHOST
    if (::getnameinfo((const sockaddr*) &sa, (socklen_t) len, host, sizeof host,
                      NULL, 0, NI_NAMEREQD) != 0)
        return std::string();
    return host;
}

} // namespace pal
