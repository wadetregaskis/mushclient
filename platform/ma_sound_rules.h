// platform/ma_sound_rules.h
// The two decisions the sound engine makes that are pure — no device, no
// miniaudio — split out of platform/ma_sound.cpp so they can be tested.
//
// ma_sound.cpp compiles miniaudio's implementation, so it is a mushgui-only TU
// that no headless test executable can link.  Both rules below are Windows
// PARITY rules rather than port inventions (the first-half auto-select scan is
// scripting/methods/methods_sounds.cpp's, the dB scale is DSBVOLUME's), so
// getting one wrong is silent and permanent.  Neither needs an audio device, so
// they live here and tests/test_sound.cpp pins them; what genuinely needs a
// device (per-owner buffer isolation) stays in --sound-selftest.

#pragma once
#ifndef _WIN32

#include <cmath>

#include "platform/ma_sound.h"    // MA_MAX_SOUND_BUFFERS

// Which slot (0-based) a request for `buffer` names, given which of this
// owner's slots are currently loaded.  Buffer 0 means "first free", and — as on
// Windows — only the first HALF of the range is scanned, reserving 6-10 for
// explicit allocation.  With the first half full it re-uses buffer 1.
inline int ma_sound_choose_slot(const bool loaded[MA_MAX_SOUND_BUFFERS], int buffer)
{
    if (buffer >= 1 && buffer <= MA_MAX_SOUND_BUFFERS)
        return buffer - 1;                    // explicit buffer
    for (int i = 0; i < (MA_MAX_SOUND_BUFFERS / 2); ++i)
        if (!loaded[i]) return i;
    return 0;                                 // all busy: slot 1
}

// Volume percent [-100, 0] (0 = full) → linear amplitude gain.  This matches
// Windows' DSBVOLUME, which is hundredths of a dB: iVolume = Volume * 100,
// i.e. the attenuation in dB equals Volume, and gain = 10^(dB/20).
inline double ma_sound_gain(double volume)
{
    if (volume > 0.0)    volume = 0.0;
    if (volume < -100.0) volume = -100.0;
    return std::pow(10.0, volume / 20.0);
}


#endif // !_WIN32
