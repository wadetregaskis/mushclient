// platform/ma_sound.h
// Portable sound engine (miniaudio) for the macOS/Linux port.
//
// Backs the canonical world.PlaySound / Sound / StopSound / GetSoundStatus API
// (scripting/methods/methods_sounds.cpp, _NOSOUND branch) and the MSP / trigger
// sound hooks (g_wxhook_play_sound).  miniaudio's high-level ma_engine mixes the
// voices and gives per-sound volume, pan, looping and playing/looping state —
// matching what DirectSound provided on Windows.
//
// `owner` scopes the buffers.  On Windows the ten LPDIRECTSOUNDBUFFERs live in
// CMUSHclientDoc, so each world has ten of its own: two worlds can both be using
// buffer 1, one world's StopSound(0) does not touch another's, and closing a
// world releases exactly its own.  Pass the CMUSHclientDoc* to get that here.
//
// Buffers are 1..MA_MAX_SOUND_BUFFERS; buffer 0 means "pick a free slot".
// Volume is a percent in [-100, 0] (0 = full, -100 ≈ silent — a dB scale, like
// Windows' DSBVOLUME).  Pan is a percent in [-100, +100] (-100 = full left,
// +100 = full right, 0 = centred).
//
// All functions are no-ops / safe when the audio device can't be opened.

#pragma once
#ifndef _WIN32

#include <cstddef>                // size_t

#define MA_MAX_SOUND_BUFFERS 10   // mirrors MAX_SOUND_BUFFERS (MUSHclient.h)

// Play `path` in `owner`'s `buffer` (0 = auto-select a free slot).  loop != 0
// loops.  Returns the 1-based buffer actually used, or 0 on failure.
int  ma_audio_play(const void* owner, int buffer, const char* path, int loop,
                   double volume, double pan);

// Play an in-memory encoded-audio image (the canonical world.PlaySoundMemory):
// `data`/`len` is e.g. a WAV/MP3/FLAC blob.  The bytes are copied internally, so
// the caller's buffer need not outlive the call.  Same buffer/return semantics as
// ma_audio_play.
int  ma_audio_play_memory(const void* owner, int buffer, const void* data, size_t len,
                          int loop, double volume, double pan);

// Adjust an already-playing buffer's loop / volume / pan in place (the canonical
// "empty filename + buffer" case).  Returns 1 on success, 0 if that buffer is
// not currently playing for this owner.
int  ma_audio_adjust(const void* owner, int buffer, int loop, double volume, double pan);

// Stop + free one of `owner`'s buffers (1..N), or every one of them when
// `buffer` == 0.  Other owners' sounds keep playing.
void ma_audio_stop(const void* owner, int buffer);

// Status of `owner`'s buffer (1..N): -2 = free/never used, 0 = not playing,
// 1 = playing (not looping), 2 = playing and looping, -3 = no audio device.
int  ma_audio_status(const void* owner, int buffer);

// Stop everything this owner has playing and forget it — the doc destructor's
// counterpart to Windows releasing its DirectSound buffers.  Safe for an owner
// that never played anything.
void ma_audio_release_owner(const void* owner);


#endif // !_WIN32
