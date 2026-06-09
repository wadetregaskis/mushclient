// platform/ma_sound.cpp
// miniaudio-backed implementation of the portable sound engine (ma_sound.h).
//
// One ma_engine drives the audio device and mixes up to MA_MAX_SOUND_BUFFERS
// concurrent ma_sound voices — one per "buffer" in the canonical PlaySound API.
// The engine is created lazily on first use and kept alive for the process.
//
// Compiled into mushgui only (the GUI links the real methods_sounds.cpp); the
// engine has no MFC/wx dependency.  miniaudio is a single public-domain/MIT-0
// header (platform/miniaudio/miniaudio.h).

#ifndef _WIN32

// Trim parts we don't use: no WAV/etc. encoding (we only play), no waveform/
// noise generators.  Keep decoding (WAV/MP3/FLAC) + device playback + engine.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "platform/miniaudio/miniaudio.h"

#include "platform/ma_sound.h"

#include <cmath>
#include <mutex>

namespace {

ma_engine  g_engine;
bool       g_engineReady  = false;
bool       g_engineFailed = false;          // don't retry a failed device open
ma_sound   g_sounds[MA_MAX_SOUND_BUFFERS];
bool       g_loaded[MA_MAX_SOUND_BUFFERS] = { false };
std::mutex g_mutex;

// Open the playback device + engine once.  Returns false if no device.
bool ensureEngine()
{
    if (g_engineReady)  return true;
    if (g_engineFailed) return false;
    if (ma_engine_init(NULL, &g_engine) != MA_SUCCESS) {
        g_engineFailed = true;
        return false;
    }
    g_engineReady = true;
    return true;
}

// Volume percent [-100, 0] (0 = full) → linear amplitude gain.  This matches
// Windows' DSBVOLUME, which is hundredths of a dB: iVolume = Volume * 100, i.e.
// the attenuation in dB equals Volume, and amplitude gain = 10^(dB/20).
float volumeToGain(double volume)
{
    if (volume > 0.0)      volume = 0.0;
    if (volume < -100.0)   volume = -100.0;
    return (float) std::pow(10.0, volume / 20.0);
}

// Pan percent [-100, +100] → miniaudio pan [-1, +1].
float panToPan(double pan)
{
    if (pan >  100.0) pan =  100.0;
    if (pan < -100.0) pan = -100.0;
    return (float) (pan / 100.0);
}

// Free a slot's voice.  Caller holds g_mutex.
void unloadSlot(int i)
{
    if (g_loaded[i]) {
        ma_sound_uninit(&g_sounds[i]);
        g_loaded[i] = false;
    }
}

// True if the slot is loaded and currently playing.  Caller holds g_mutex.
bool slotPlaying(int i)
{
    return g_loaded[i] && ma_sound_is_playing(&g_sounds[i]) == MA_TRUE;
}

} // namespace

int ma_audio_play(int buffer, const char* path, int loop, double volume, double pan)
{
    if (!path || !*path) return 0;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!ensureEngine()) return 0;

    // Reap finished one-shot voices so auto-select (and re-use) can reclaim them
    // — mirrors the canonical "release non-playing buffers first" pass.
    for (int i = 0; i < MA_MAX_SOUND_BUFFERS; ++i)
        if (g_loaded[i] && ma_sound_is_playing(&g_sounds[i]) != MA_TRUE)
            unloadSlot(i);

    int idx;
    if (buffer >= 1 && buffer <= MA_MAX_SOUND_BUFFERS) {
        idx = buffer - 1;                     // explicit buffer
    } else {
        idx = -1;                             // buffer 0 → first free slot…
        for (int i = 0; i < MA_MAX_SOUND_BUFFERS; ++i)
            if (!g_loaded[i]) { idx = i; break; }
        if (idx < 0) idx = 0;                 // …else re-use slot 1 (canonical)
    }

    unloadSlot(idx);

    // Decode the whole file up front (short sounds, low latency).  Disable
    // spatialization so pan is a plain stereo pan with no 3D attenuation.
    ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(&g_engine, path, flags, NULL, NULL, &g_sounds[idx])
            != MA_SUCCESS)
        return 0;
    g_loaded[idx] = true;

    ma_sound_set_looping(&g_sounds[idx], loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume (&g_sounds[idx], volumeToGain(volume));
    ma_sound_set_pan    (&g_sounds[idx], panToPan(pan));

    if (ma_sound_start(&g_sounds[idx]) != MA_SUCCESS) {
        unloadSlot(idx);
        return 0;
    }
    return idx + 1;
}

int ma_audio_adjust(int buffer, int loop, double volume, double pan)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (buffer < 1 || buffer > MA_MAX_SOUND_BUFFERS) return 0;
    int i = buffer - 1;
    if (!slotPlaying(i)) return 0;

    ma_sound_set_looping(&g_sounds[i], loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume (&g_sounds[i], volumeToGain(volume));
    ma_sound_set_pan    (&g_sounds[i], panToPan(pan));
    return 1;
}

void ma_audio_stop(int buffer)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_engineReady) return;
    if (buffer == 0) {                        // stop everything
        for (int i = 0; i < MA_MAX_SOUND_BUFFERS; ++i) unloadSlot(i);
        return;
    }
    if (buffer < 1 || buffer > MA_MAX_SOUND_BUFFERS) return;
    unloadSlot(buffer - 1);
}

int ma_audio_status(int buffer)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (buffer < 1 || buffer > MA_MAX_SOUND_BUFFERS) return -2;
    int i = buffer - 1;
    if (!g_loaded[i]) return -2;                          // free
    if (ma_sound_is_playing(&g_sounds[i]) != MA_TRUE) return 0;  // not playing
    return ma_sound_is_looping(&g_sounds[i]) == MA_TRUE ? 2 : 1;
}

#endif // !_WIN32
