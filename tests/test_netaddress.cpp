// tests/test_netaddress.cpp — pal::NetAddress: reading, printing and ordering
// IPv4 and IPv6 addresses (pal/netaddress.h).
//
// The parser and printer are hand-written (inet_pton / inet_ntop are Vista and
// later, and AI_NUMERICHOST needs WSAStartup), so they are pinned here against
// RFC 4291's text forms and RFC 5952's canonical output -- the cases a
// hand-written one gets wrong being the ones listed.

#include "mc_test.h"
#include "stdafx.h"
#include "pal/netaddress.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace {

pal::NetAddress Parse(const char* s)
{
    pal::NetAddress a;
    pal::ParseNumericAddress(s, a);
    return a;
}

std::string RoundTrip(const char* s)
{
    pal::NetAddress a;
    if (!pal::ParseNumericAddress(s, a))
        return "(refused)";
    return pal::FormatAddress(a);
}

} // namespace

// ── IPv4: exactly what inet_addr took ──────────────────────────────────────

TEST(NetAddress, IPv4IsReadAsInetAddrReadsIt)
{
    pal::NetAddress a;
    ASSERT_TRUE(pal::ParseNumericAddress("192.0.2.1", a));
    EXPECT_TRUE(a.isIPv4());
    EXPECT_EQ((unsigned int) inet_addr("192.0.2.1"), a.ipv4());
    EXPECT_STREQ("192.0.2.1", pal::FormatAddress(a).c_str());

    // inet_addr's own spellings still mean what they did
    EXPECT_STREQ("127.0.0.1", RoundTrip("127.1").c_str());

    // and its one blind spot is still sent to the resolver
    EXPECT_FALSE(pal::ParseNumericAddress("255.255.255.255", a));
}

TEST(NetAddress, ANameIsNotAnAddress)
{
    pal::NetAddress a;
    EXPECT_FALSE(pal::ParseNumericAddress("mud.example.com", a));
    EXPECT_FALSE(pal::ParseNumericAddress("localhost", a));
    EXPECT_FALSE(pal::ParseNumericAddress("", a));
    EXPECT_FALSE(pal::ParseNumericAddress(NULL, a));
    EXPECT_FALSE(a.valid());
}

// ── IPv6 text (RFC 4291 section 2.2) ───────────────────────────────────────

TEST(NetAddress, IPv6ForAllItsSpellings)
{
    EXPECT_STREQ("::1",            RoundTrip("::1").c_str());
    EXPECT_STREQ("::",             RoundTrip("::").c_str());
    EXPECT_STREQ("2001:db8::1",    RoundTrip("2001:db8::1").c_str());
    EXPECT_STREQ("2001:db8::1",    RoundTrip("[2001:db8::1]").c_str());     // as typed beside a port
    EXPECT_STREQ("2001:db8::1",    RoundTrip("2001:0DB8:0000::0001").c_str()); // case, leading zeros
    EXPECT_STREQ("1:2:3:4:5:6:7:8", RoundTrip("1:2:3:4:5:6:7:8").c_str());
    EXPECT_STREQ("1::",            RoundTrip("1::").c_str());
    EXPECT_STREQ("::2:3:4:5:6:7",  RoundTrip("0:0:2:3:4:5:6:7").c_str());
    // the embedded-IPv4 form (RFC 6052's well-known prefix here)
    EXPECT_STREQ("64:ff9b::c000:221", RoundTrip("64:ff9b::192.0.2.33").c_str());
}

TEST(NetAddress, IPv6TextThatIsNotAnAddressIsRefused)
{
    const char* bad[] = {
        ":::", "1::2::3", "2001:db8::1:", ":1::", "12345::", "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7::8:9", "::1.2.3", "::1.2.3.256", "1.2.3.4::", "g::1",
        "[::1", "::1]", "[]", "fe80::1%", "2001:db8:::1", "::01234",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pal::NetAddress a;
        EXPECT_FALSE(pal::ParseNumericAddress(bad[i], a));
        if (pal::ParseNumericAddress(bad[i], a))
            printf("    accepted: \"%s\" as %s\n", bad[i], pal::FormatAddress(a).c_str());
    }
}

TEST(NetAddress, AnIPv4MappedAddressIsTheIPv4AddressItMaps)
{
    pal::NetAddress a;
    ASSERT_TRUE(pal::ParseNumericAddress("::ffff:192.0.2.1", a));
    EXPECT_TRUE(a.isIPv4());
    EXPECT_STREQ("192.0.2.1", pal::FormatAddress(a).c_str());
    ASSERT_TRUE(pal::ParseNumericAddress("::ffff:c000:201", a));
    EXPECT_TRUE(a.isIPv4());
}

TEST(NetAddress, AZoneIsKeptAndPrinted)
{
    pal::NetAddress a;
    ASSERT_TRUE(pal::ParseNumericAddress("fe80::1%3", a));
    EXPECT_EQ(3u, a.scopeId);
    EXPECT_STREQ("fe80::1%3", pal::FormatAddress(a).c_str());
    // the same address on another interface is another address
    EXPECT_TRUE(Parse("fe80::1%3") != Parse("fe80::1%4"));
}

// ── RFC 5952 output ────────────────────────────────────────────────────────

TEST(NetAddress, TheLongestRunOfZerosIsTheOneCompressed)
{
    EXPECT_STREQ("1:0:0:2::3",       RoundTrip("1:0:0:2:0:0:0:3").c_str());
    // equally long: the first
    EXPECT_STREQ("2001:db8::1:0:0:1", RoundTrip("2001:db8:0:0:1:0:0:1").c_str());
    // one zero group alone is not "::"
    EXPECT_STREQ("2001:db8:0:1:1:1:1:1", RoundTrip("2001:db8:0:1:1:1:1:1").c_str());
    // the longer run wins wherever it is
    EXPECT_STREQ("0:0:1::",          RoundTrip("0:0:1:0:0:0:0:0").c_str());
}

TEST(NetAddress, WithAPortIPv6IsBracketed)
{
    EXPECT_STREQ("[2001:db8::1]:4000", pal::FormatAddressAndPort(Parse("2001:db8::1"), 4000).c_str());
    EXPECT_STREQ("192.0.2.1:23",       pal::FormatAddressAndPort(Parse("192.0.2.1"), 23).c_str());
    EXPECT_STREQ("",                   pal::FormatAddress(pal::NetAddress()).c_str());
}

// ── to and from sockets ────────────────────────────────────────────────────

TEST(NetAddress, SockaddrRoundTripsBothFamiliesAndThePort)
{
    const char* each[] = { "192.0.2.1", "2001:db8::17", "fe80::1%5" };
    for (size_t i = 0; i < 3; i++) {
        const pal::NetAddress a = Parse(each[i]);
        sockaddr_storage ss;
        const int len = pal::ToSockaddr(a, 4000, ss);
        EXPECT_EQ(a.isIPv4() ? (int) sizeof(sockaddr_in) : (int) sizeof(sockaddr_in6), len);
        EXPECT_EQ(pal::SocketFamily(a), (int) ((sockaddr*) &ss)->sa_family);

        pal::NetAddress back;
        unsigned short port = 0;
        ASSERT_TRUE(pal::FromSockaddr((sockaddr*) &ss, back, &port));
        EXPECT_TRUE(back == a);
        EXPECT_EQ(4000, (int) port);
    }

    sockaddr_storage ss;
    EXPECT_EQ(0, pal::ToSockaddr(pal::NetAddress(), 1, ss));   // nothing to make
}

// ── the order to try a name's addresses in ─────────────────────────────────

TEST(NetAddress, FamiliesAlternateStartingWithTheResolversFirst)
{
    std::vector<pal::NetAddress> v;
    v.push_back(Parse("2001:db8::1"));
    v.push_back(Parse("2001:db8::2"));
    v.push_back(Parse("192.0.2.1"));
    v.push_back(Parse("192.0.2.2"));
    v.push_back(Parse("2001:db8::1"));          // a duplicate
    pal::OrderForConnecting(v);
    ASSERT_EQ(4u, v.size());
    EXPECT_STREQ("2001:db8::1", pal::FormatAddress(v[0]).c_str());
    EXPECT_STREQ("192.0.2.1",   pal::FormatAddress(v[1]).c_str());
    EXPECT_STREQ("2001:db8::2", pal::FormatAddress(v[2]).c_str());
    EXPECT_STREQ("192.0.2.2",   pal::FormatAddress(v[3]).c_str());

    std::vector<pal::NetAddress> w;
    w.push_back(Parse("192.0.2.1"));
    w.push_back(Parse("2001:db8::1"));
    w.push_back(Parse("2001:db8::2"));
    pal::OrderForConnecting(w);
    ASSERT_EQ(3u, w.size());
    EXPECT_TRUE(w[0].isIPv4());
    EXPECT_TRUE(w[1].isIPv6());
    EXPECT_TRUE(w[2].isIPv6());
}

// ── the synchronous lookups ────────────────────────────────────────────────

TEST(NetAddress, LookUpNowGivesIPv4First)
{
    const std::vector<pal::NetAddress> v = pal::LookUpNow("localhost");
    ASSERT_FALSE(v.empty());
    EXPECT_TRUE(std::find(v.begin(), v.end(), Parse("127.0.0.1")) != v.end());
    // every IPv4 address before any IPv6 one
    bool seenIPv6 = false;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].isIPv6()) seenIPv6 = true;
        else EXPECT_FALSE(seenIPv6);
    }
    EXPECT_TRUE(pal::LookUpNow("").empty());
    EXPECT_TRUE(pal::LookUpNow("mushclient-no-such-host.invalid").empty());
}
