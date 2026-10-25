// platform/sound_selftest.cpp — in-app conformance suite for the miniaudio
// sound engine (--sound-selftest).
//
// WHY IT LIVES HERE.  platform/ma_sound.cpp compiles miniaudio's implementation,
// so it is linked into mushgui and nowhere else: the 40 headless executables
// cannot see it.  The purely arithmetic half of what it decides now lives in
// platform/ma_sound_rules.h and is pinned by tests/test_sound.cpp; what is left
// needs a real device, and that is this file.
//
// WHAT IT ASSERTS.  The two things the file exists to get right and one thing it
// used to get wrong:
//
//   * per-owner buffer isolation.  On Windows the ten LPDIRECTSOUNDBUFFERs are
//     members of CMUSHclientDoc, so two worlds can both be using buffer 1 and
//     one world's StopSound(0) leaves the other's alone.  A single process-wide
//     array of ten -- the obvious implementation -- gets all of that wrong and
//     nothing else in the suite would notice.
//   * the auto-select rule reaching the real engine.  ma_sound_rules.h pins the
//     rule; this pins that ma_sound.cpp still asks it, with real loaded slots.
//   * ma_audio_shutdown() actually tearing the engine down.  Before it existed,
//     static destruction of the owner map handed every voice back to the
//     allocator while the device thread was still mixing it -- a use-after-free
//     on every quit that had ever played a sound.  A shutdown that did nothing
//     would still answer 1 / -2 here, so the assertion is that every buffer
//     reads -3 (no device) afterwards.
//
// Exits 77 (ctest SKIPPED) when no audio device can be opened, following the
// --engines-selftest convention: sound is expected, not required.

#ifndef _WIN32

#include "platform/ma_sound.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

// Little-endian appenders for the RIFF header below.
void put16(std::vector<unsigned char>& v, unsigned int x)
{
    v.push_back((unsigned char) (x & 0xFF));
    v.push_back((unsigned char) ((x >> 8) & 0xFF));
}

void put32(std::vector<unsigned char>& v, unsigned int x)
{
    put16(v, x & 0xFFFF);
    put16(v, (x >> 16) & 0xFFFF);
}

void puttag(std::vector<unsigned char>& v, const char* tag)
{
    for (int i = 0; i < 4; ++i) v.push_back((unsigned char) tag[i]);
}

// A real (decodable) WAV of `seconds` of silence: 8 kHz, mono, 16-bit PCM.
// Silence so a test run makes no noise, and long enough that every voice is
// still playing when the assertions read its status back.
std::vector<unsigned char> makeWav(int seconds)
{
    const unsigned int rate  = 8000;
    const unsigned int bytes = rate * 2 * (unsigned int) seconds;
    std::vector<unsigned char> v;
    puttag(v, "RIFF");  put32(v, 36 + bytes);  puttag(v, "WAVE");
    puttag(v, "fmt ");  put32(v, 16);
    put16(v, 1);                     // PCM
    put16(v, 1);                     // mono
    put32(v, rate);
    put32(v, rate * 2);              // byte rate
    put16(v, 2);                     // block align
    put16(v, 16);                    // bits per sample
    puttag(v, "data");  put32(v, bytes);
    v.resize(v.size() + bytes, 0);   // the silence
    return v;
}

} // namespace

int RunSoundSelfTest()
{
    printf("Sound engine selftest (miniaudio)\n");

    // Two distinct owner addresses stand in for two open worlds (the real
    // callers pass CMUSHclientDoc*); a third for the failure cases.
    const char worldA = 'A', worldB = 'B', worldC = 'C';
    const void* A = &worldA;
    const void* B = &worldB;
    const void* C = &worldC;

    // Device gate.  ma_audio_status opens the device the same way the play
    // paths do, so -3 here means there is none to test against.
    if (ma_audio_status(A, 1) == -3) {
        printf("  [SKIP] no audio device -- nothing to test\n");
        return 77;
    }

    const std::vector<unsigned char> wav = makeWav(5);

    // ── per-owner buffers ────────────────────────────────────────────────
    check(ma_audio_play_memory(A, 0, wav.data(), wav.size(), 0, 0.0, 0.0) == 1,
          "auto-select gives world A buffer 1");
    check(ma_audio_status(A, 1) == 1, "A buffer 1 is playing, not looping");
    check(ma_audio_status(A, 2) == -2, "A buffer 2 is still free");

    check(ma_audio_play_memory(B, 0, wav.data(), wav.size(), 0, 0.0, 0.0) == 1,
          "world B gets its OWN buffer 1, not A's slot 2");
    check(ma_audio_status(B, 1) == 1, "B buffer 1 is playing");
    check(ma_audio_status(A, 1) == 1, "A buffer 1 unaffected by B's play");

    // ── auto-select never reaches the reserved half ──────────────────────
    // Fill A's 2..5 explicitly; with 1..5 all busy the next auto-select must
    // re-use buffer 1, never allocate 6 (the Windows rule reserves 6-10).
    bool filled = true;
    for (int b = 2; b <= 5; ++b)
        if (ma_audio_play_memory(A, b, wav.data(), wav.size(), 1, 0.0, 0.0) != b)
            filled = false;
    check(filled, "explicit buffers 2-5 play in the buffer asked for");
    check(ma_audio_play_memory(A, 0, wav.data(), wav.size(), 0, 0.0, 0.0) == 1,
          "first half full -> auto-select re-uses buffer 1");
    check(ma_audio_status(A, 6) == -2, "auto-select never touched buffer 6");

    // ── adjust reaches the live voice ────────────────────────────────────
    check(ma_audio_adjust(A, 1, 1 /*loop*/, -20.0, 0.0) == 1,
          "adjust succeeds on a playing buffer");
    check(ma_audio_status(A, 1) == 2, "adjusted buffer 1 now reports looping");
    check(ma_audio_adjust(A, 7, 0, 0.0, 0.0) == 0,
          "adjust fails on a buffer that is not playing");

    // ── one world's stop leaves the other's alone ────────────────────────
    ma_audio_stop(B, 0);
    check(ma_audio_status(B, 1) == -2, "B's StopSound(0) freed B's buffer 1");
    check(ma_audio_status(A, 1) == 2,  "B's StopSound(0) left A's buffer 1 playing");

    ma_audio_release_owner(B);
    check(ma_audio_status(A, 1) == 2, "closing world B left A's buffer 1 playing");

    // ── a corrupt image fails cleanly ────────────────────────────────────
    const unsigned char junk[16] = { 0 };
    check(ma_audio_play_memory(C, 3, junk, sizeof(junk), 0, 0.0, 0.0) == 0,
          "undecodable bytes refuse to play");
    check(ma_audio_status(C, 3) == -2, "the refused buffer is left free");

    // ── teardown ─────────────────────────────────────────────────────────
    // With voices still playing -- which is the state every real quit is in.
    ma_audio_shutdown();
    bool allGone = true;
    for (int b = 1; b <= MA_MAX_SOUND_BUFFERS; ++b)
        if (ma_audio_status(A, b) != -3 || ma_audio_status(B, b) != -3)
            allGone = false;
    check(allGone, "after shutdown every buffer reports -3 (no device)");
    check(ma_audio_play_memory(A, 0, wav.data(), wav.size(), 0, 0.0, 0.0) == 0,
          "shutdown does not re-open the device for a late play");
    ma_audio_shutdown();                 // idempotent: must not crash
    check(ma_audio_status(A, 1) == -3, "a second shutdown is a no-op");

    printf("Sound engine selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}


#endif // !_WIN32
