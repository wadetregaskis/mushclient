// pal/netaddress.h — an IPv4 or IPv6 address, as the engine and the transports
// hand it to each other.
//
// Upstream kept every address in a SOCKADDR_IN and parsed and printed it with
// inet_addr / inet_ntoa, so the client could neither connect to an IPv6 MUD nor
// say where it was connected.  This is the one value type that replaces those:
// plain bytes, no socket headers, so doc.h and the transport seam can carry it
// without dragging Winsock or <netinet/in.h> into everything that includes them.
//
// Parsing and printing are done here by hand rather than with inet_pton /
// inet_ntop: those are Vista-and-later on Windows (the build still declares
// XP), getaddrinfo's AI_NUMERICHOST needs WSAStartup, and a pure function is the
// same on every platform and testable without a network stack.  IPv4 is still
// read by inet_addr, so every IPv4 spelling the client accepted it still does.
//
// No C++17 here: the Windows build compiles at /std:c++14 (see itransport.h).

#pragma once

#include <string>
#include <vector>

struct sockaddr;
struct sockaddr_storage;

namespace pal {

struct NetAddress {
    enum Family { None = 0, IPv4 = 4, IPv6 = 6 };

    Family        family = None;
    unsigned char bytes[16] = {};   // network order; IPv4 uses the first four
    unsigned int  scopeId = 0;      // IPv6 zone (a link-local address's interface), else 0

    bool valid() const { return family != None; }
    bool isIPv4() const { return family == IPv4; }
    bool isIPv6() const { return family == IPv6; }

    // An IPv4 address in network byte order (an in_addr's s_addr), and back.
    // ipv4() is 0 for anything that is not IPv4.
    static NetAddress FromIPv4(unsigned int netOrder);
    unsigned int ipv4() const;

    bool operator==(const NetAddress& o) const;
    bool operator!=(const NetAddress& o) const { return !(*this == o); }
};

// A numeric address: "192.0.2.1" (anything inet_addr takes), "2001:db8::1",
// "[2001:db8::1]", "::ffff:192.0.2.1", "fe80::1%3", and off Windows
// "fe80::1%en0".  false for a host name, and for "255.255.255.255" as before
// (inet_addr cannot tell it from its own failure).  An IPv4-mapped IPv6
// address comes back as the IPv4 address it maps: that is what it reaches.
bool ParseNumericAddress(const char* text, NetAddress& out);

// "192.0.2.1", or RFC 5952's canonical IPv6 text ("2001:db8::1", "fe80::1%3").
// "" for an address that is not valid.  IPv6 without brackets -- callers that
// put a port after it add them (FormatAddressAndPort).
std::string FormatAddress(const NetAddress& a);

// "192.0.2.1 port 23" is how the client has always printed a destination; this
// is the bracketed form for places that join them: "192.0.2.1:23",
// "[2001:db8::1]:23".
std::string FormatAddressAndPort(const NetAddress& a, unsigned short port);

// For socket calls.  ToSockaddr fills `out` with a sockaddr_in or sockaddr_in6
// for this address and port (host order) and returns its length, or 0 for an
// address that is not valid.  FromSockaddr reads either back (IPv4-mapped ->
// IPv4); false for any other family.
int  ToSockaddr(const NetAddress& a, unsigned short port, sockaddr_storage& out);
bool FromSockaddr(const sockaddr* sa, NetAddress& out, unsigned short* port = nullptr);

// The address family constant (AF_INET / AF_INET6) for socket(); 0 if none.
int  SocketFamily(const NetAddress& a);

// The order to try a host name's addresses in (RFC 8305 section 4): duplicates
// dropped, then the families alternated, starting with whichever the resolver
// put first -- so a host whose IPv6 route is broken costs one failed attempt
// before an IPv4 one, not one per IPv6 address.
void OrderForConnecting(std::vector<NetAddress>& addresses);

// Synchronous lookups, for the callers that always were (world.GetHostAddress,
// world.GetHostName, the chat handshake's own address).  LookUpNow gives every
// address the name has, IPv4 ones first -- what gethostbyname gave, then what
// it could not -- and NameOfAddress the name an address has, or "".
std::vector<NetAddress> LookUpNow(const char* host);
std::string NameOfAddress(const NetAddress& a);

} // namespace pal
