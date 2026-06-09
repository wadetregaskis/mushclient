// platform/ma_sound.h
// Portable sound engine (miniaudio) for the macOS/Linux port.
//
// Backs the canonical world.PlaySound / Sound / StopSound / GetSoundStatus API
// (scripting/methods/methods_sounds.cpp, _NOSOUND branch) and the MSP / trigger
// sound hooks (g_wxhook_play_sound).  miniaudio's high-level ma_engine mixes up
// to MA_MAX_SOUND_BUFFERS concurrent sounds and gives per-sound volume, pan,
// looping and playing/looping state — matching what DirectSound provided on
// Windows.
//
// Buffers are 1..MA_MAX_SOUND_BUFFERS; buffer 0 means "pick a free slot".
// Volume is a percent in [-100, 0] (0 = full, -100 ≈ silent — a dB scale, like
// Windows' DSBVOLUME).  Pan is a percent in [-100, +100] (-100 = full left,
// +100 = full right, 0 = centred).
//
// All functions are no-ops / safe when the audio device can't be opened.

#pragma once
#ifndef _WIN32

#define MA_MAX_SOUND_BUFFERS 10   // mirrors MAX_SOUND_BUFFERS (MUSHclient.h)

// Play `path` in `buffer` (0 = auto-select a free slot).  loop != 0 loops.
// Returns the 1-based buffer actually used, or 0 on failure.
int  ma_audio_play(int buffer, const char* path, int loop, double volume, double pan);

// Adjust an already-playing buffer's loop / volume / pan in place (the canonical
// "empty filename + buffer" case).  Returns 1 on success, 0 if that buffer is
// not currently playing.
int  ma_audio_adjust(int buffer, int loop, double volume, double pan);

// Stop + free a buffer (1..N), or every buffer when `buffer` == 0.
void ma_audio_stop(int buffer);

// Status of buffer (1..N): -2 = free/never used, 0 = not playing,
// 1 = playing (not looping), 2 = playing and looping.
int  ma_audio_status(int buffer);

#endif // !_WIN32
