// tests/test_hashing.cpp
// Unit tests for the vendored hash functions: MD5, SHA-256, SHA-1 (SHS).
// Test vectors from IETF RFCs and NIST FIPS publications.

#include "mc_test.h"

extern "C" {
#include "scripting/md5.h"
#include "scripting/sha256.h"
#include "scripting/shs.h"
}

#include <cstring>
#include <cstdio>

// ── Hex helpers ───────────────────────────────────────────────────────────

static std::string to_hex(const unsigned char* buf, int len)
{
    std::string out;
    char tmp[3];
    for (int i = 0; i < len; i++) {
        snprintf(tmp, sizeof(tmp), "%02x", buf[i]);
        out += tmp;
    }
    return out;
}

static std::string md5_hex(const char* msg)
{
    md5_context ctx;
    md5_starts(&ctx);
    md5_update(&ctx, (uint8*)msg, (uint32)strlen(msg));
    uint8 digest[16];
    md5_finish(&ctx, digest);
    return to_hex(digest, 16);
}

static std::string sha256_hex(const char* msg)
{
    sha256_context ctx;
    sha256_starts(&ctx);
    sha256_update(&ctx, (uint8*)msg, (uint32)strlen(msg));
    uint8 digest[32];
    sha256_finish(&ctx, digest);
    return to_hex(digest, 32);
}

static std::string sha1_hex(const char* msg)
{
    SHS_INFO ctx;
    shsInit(&ctx);
    shsUpdate(&ctx, (BYTE*)msg, (int)strlen(msg));
    shsFinal(&ctx);
    // digest is five 32-bit words in big-endian order
    unsigned char digest[20];
    for (int i = 0; i < 5; i++) {
        digest[i*4+0] = (ctx.digest[i] >> 24) & 0xFF;
        digest[i*4+1] = (ctx.digest[i] >> 16) & 0xFF;
        digest[i*4+2] = (ctx.digest[i] >>  8) & 0xFF;
        digest[i*4+3] = (ctx.digest[i]      ) & 0xFF;
    }
    return to_hex(digest, 20);
}

// ── MD5 tests (RFC 1321 test vectors) ─────────────────────────────────────

TEST(MD5Test, EmptyString) {
    EXPECT_STREQ("d41d8cd98f00b204e9800998ecf8427e", md5_hex("").c_str());
}

TEST(MD5Test, ABC) {
    EXPECT_STREQ("900150983cd24fb0d6963f7d28e17f72", md5_hex("abc").c_str());
}

TEST(MD5Test, AlphaNumeric) {
    EXPECT_STREQ("c3fcd3d76192e4007dfb496cca67e13b",
                 md5_hex("abcdefghijklmnopqrstuvwxyz").c_str());
}

TEST(MD5Test, QuickBrownFox) {
    EXPECT_STREQ("9e107d9d372bb6826bd81d3542a419d6",
                 md5_hex("The quick brown fox jumps over the lazy dog").c_str());
}

TEST(MD5Test, TwoBlockInput) {
    // "message digest" — RFC 1321 vector
    EXPECT_STREQ("f96b697d7cb7938d525a2f31aaf161d0",
                 md5_hex("message digest").c_str());
}

// ── SHA-256 tests (FIPS 180-2 test vectors) ───────────────────────────────

TEST(SHA256Test, EmptyString) {
    EXPECT_STREQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 sha256_hex("").c_str());
}

TEST(SHA256Test, ABC) {
    EXPECT_STREQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 sha256_hex("abc").c_str());
}

TEST(SHA256Test, QuickBrownFox) {
    EXPECT_STREQ("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
                 sha256_hex("The quick brown fox jumps over the lazy dog").c_str());
}

TEST(SHA256Test, TwoBlockInput) {
    // "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    EXPECT_STREQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                 sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
}

// ── SHS (SHA-0) tests ─────────────────────────────────────────────────────
// shs.c implements FIPS 180 (SHA-0), not FIPS 180-1 (SHA-1).  The two differ
// only in that SHA-1 adds a ROTL(1,...) to the message-schedule expansion step.
// MUSHclient uses SHA-0 for chat-socket file-transfer verification; both ends
// must agree on the same algorithm, so we deliberately keep SHA-0.
// Test vectors below were computed with an independent Python SHA-0 implementation.

TEST(SHSTest, ABC) {
    EXPECT_STREQ("0164b8a914cd2a5e74c4f7ff082c4d97f1edf880",
                 sha1_hex("abc").c_str());
}

TEST(SHSTest, TwoBlockInput) {
    EXPECT_STREQ("d2516ee1acfa5baf33dfc1c471e438449ef134c8",
                 sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
}

TEST(SHSTest, QuickBrownFox) {
    EXPECT_STREQ("b03b401ba92d77666221e843feebf8c561cea5f7",
                 sha1_hex("The quick brown fox jumps over the lazy dog").c_str());
}

MC_TEST_MAIN()
