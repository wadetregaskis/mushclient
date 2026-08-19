// tests/test_sound.cpp
// The sound engine's two device-free decisions (platform/ma_sound_rules.h):
// which buffer a play request names, and how a volume percent becomes a linear
// amplitude.  Both are Windows PARITY rules, so getting one wrong is silent.
//
// platform/ma_sound.cpp itself compiles miniaudio and needs a device, so it is
// linked into mushgui only and is exercised by --sound-selftest; these two rules
// are pure, which is exactly why they were split out to here.

#include "mc_test.h"
#include "platform/ma_sound_rules.h"

#include <cstring>

namespace {

// Helper: a loaded[] where the named 1-based buffers are in use.
struct Loaded {
    bool s[MA_MAX_SOUND_BUFFERS];
    Loaded() { memset(s, 0, sizeof(s)); }
    Loaded& busy(int buffer) { s[buffer - 1] = true; return *this; }
};

} // namespace

// ── which slot a request names ────────────────────────────────────────────

TEST(Sound, ExplicitBufferIsHonoured)
{
    // 1..10 map straight to 0..9 regardless of what is already loaded — the
    // reserved half included, since that is what "explicit allocation" means.
    Loaded all;
    for (int b = 1; b <= MA_MAX_SOUND_BUFFERS; ++b) all.busy(b);
    for (int b = 1; b <= MA_MAX_SOUND_BUFFERS; ++b) {
        EXPECT_EQ(b - 1, ma_sound_choose_slot(Loaded().s, b));
        EXPECT_EQ(b - 1, ma_sound_choose_slot(all.s, b));
    }
}

TEST(Sound, AutoSelectTakesTheFirstFree)
{
    // Buffer 0 = "first free".  Nothing loaded -> slot 0 (buffer 1); then each
    // added occupant pushes the answer along by one.
    EXPECT_EQ(0, ma_sound_choose_slot(Loaded().s, 0));
    EXPECT_EQ(1, ma_sound_choose_slot(Loaded().busy(1).s, 0));
    EXPECT_EQ(2, ma_sound_choose_slot(Loaded().busy(1).busy(2).s, 0));
    EXPECT_EQ(4, ma_sound_choose_slot(Loaded().busy(1).busy(2)
                                              .busy(3).busy(4).s, 0));
    // A gap lower down wins over a free slot higher up.
    EXPECT_EQ(1, ma_sound_choose_slot(Loaded().busy(1).busy(3).busy(4).s, 0));
}

TEST(Sound, AutoSelectNeverReachesTheReservedHalf)
{
    // Windows scans only MAX_SOUND_BUFFERS / 2 (methods_sounds.cpp), reserving
    // buffers 6-10 for explicit allocation: with 1-5 busy and 6-10 free the
    // answer is still buffer 1, NOT buffer 6.
    Loaded firstHalf;
    for (int b = 1; b <= MA_MAX_SOUND_BUFFERS / 2; ++b) firstHalf.busy(b);
    EXPECT_EQ(0, ma_sound_choose_slot(firstHalf.s, 0));

    // ...and it stays buffer 1 when every buffer is busy.
    Loaded all;
    for (int b = 1; b <= MA_MAX_SOUND_BUFFERS; ++b) all.busy(b);
    EXPECT_EQ(0, ma_sound_choose_slot(all.s, 0));
}

TEST(Sound, OutOfRangeBufferAutoSelects)
{
    // Negative / too-large buffer numbers are not explicit requests, so they
    // fall through to the auto-select scan rather than indexing off the end.
    EXPECT_EQ(0, ma_sound_choose_slot(Loaded().s, -1));
    EXPECT_EQ(0, ma_sound_choose_slot(Loaded().s, MA_MAX_SOUND_BUFFERS + 1));
    EXPECT_EQ(1, ma_sound_choose_slot(Loaded().busy(1).s, 99));
}

// ── volume percent -> amplitude ───────────────────────────────────────────

TEST(Sound, GainIsTheDirectSoundDecibelScale)
{
    // Windows' DSBVOLUME is hundredths of a dB and the doc layer passes
    // iVolume = Volume * 100, so the volume percent IS the attenuation in dB:
    // gain = 10^(dB/20).  A linear reading (gain = 1 + volume/100) agrees on
    // the first row and on nothing else: it gives 0.8, 0.94 and 0.0 below.
    EXPECT_NEAR(1.0,    ma_sound_gain(0.0),    1e-9);
    EXPECT_NEAR(0.1,    ma_sound_gain(-20.0),  1e-9);
    EXPECT_NEAR(0.5012, ma_sound_gain(-6.0),   1e-4);
    EXPECT_NEAR(1e-5,   ma_sound_gain(-100.0), 1e-12);
}

TEST(Sound, GainClampsToTheDocumentedRange)
{
    // Above 0 is full volume, below -100 is the -100 floor: no gain over 1.0
    // (which would clip) and none below 1e-5.
    EXPECT_NEAR(1.0,  ma_sound_gain(50.0),    1e-9);
    EXPECT_NEAR(1e-5, ma_sound_gain(-1000.0), 1e-12);
}

MC_TEST_MAIN()
