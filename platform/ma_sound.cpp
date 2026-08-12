// platform/ma_sound.cpp
// miniaudio-backed implementation of the portable sound engine (ma_sound.h).
//
// One ma_engine drives the audio device and mixes the voices.  The engine is
// created lazily on first use and kept alive for the process.
//
// The BUFFERS are per world, not per process: on Windows the ten
// LPDIRECTSOUNDBUFFERs are a member of CMUSHclientDoc (doc.h), so world A's
// buffer 1 and world B's buffer 1 are two different sounds that play at the
// same time, world B's StopSound(0) leaves A's alone, and closing a world
// releases exactly its own.  A single process-wide array of ten got all three
// wrong, so voices are keyed by owner (the doc) here.  miniaudio mixes as many
// voices as we hand it, so ten per world costs nothing until they are used.
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
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {

ma_engine  g_engine;
bool       g_engineReady  = false;
bool       g_engineFailed = false;          // don't retry a failed device open
std::mutex g_mutex;

// One voice.  For memory-sourced voices (ma_audio_play_memory) the decoder reads
// from `mem` and the sound reads from the decoder, so all three are torn down
// together, in that order.  File voices leave decoder/mem unused.
struct Slot {
    ma_sound                   sound;
    ma_decoder                 decoder;
    std::vector<unsigned char> mem;
    bool                       loaded     = false;
    bool                       hasDecoder = false;
};

// One world's ten buffers.  Held behind a unique_ptr and never moved: ma_sound
// and ma_decoder are C structs the engine keeps raw pointers into, so their
// addresses have to stay put for the life of the voice.
struct Voices { Slot s[MA_MAX_SOUND_BUFFERS]; };

std::map<const void*, std::unique_ptr<Voices>> g_owners;

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

// This owner's voices, creating the set on first use.  Caller holds g_mutex.
Voices& voicesFor(const void* owner)
{
    std::unique_ptr<Voices>& v = g_owners[owner];
    if (!v) v.reset(new Voices());
    return *v;
}

// This owner's voices, or NULL if it has never played anything.  Caller holds
// g_mutex.  Used by the read-only/stop paths so they do not allocate a set for
// a world that has no sounds.
Voices* findVoices(const void* owner)
{
    std::map<const void*, std::unique_ptr<Voices>>::iterator it = g_owners.find(owner);
    return (it == g_owners.end()) ? NULL : it->second.get();
}

// Free a slot's voice.  Caller holds g_mutex.  Order matters: the sound reads
// from the decoder, which reads from mem, so tear down sound → decoder → bytes.
void unloadSlot(Slot& sl)
{
    if (sl.loaded) {
        ma_sound_uninit(&sl.sound);
        sl.loaded = false;
    }
    if (sl.hasDecoder) {
        ma_decoder_uninit(&sl.decoder);
        sl.hasDecoder = false;
        std::vector<unsigned char>().swap(sl.mem);   // release the byte copy
    }
}

// True if the slot is loaded and currently playing.  Caller holds g_mutex.
bool slotPlaying(Slot& sl)
{
    return sl.loaded && ma_sound_is_playing(&sl.sound) == MA_TRUE;
}

// Reap finished one-shot voices so auto-select (and re-use) can reclaim them —
// mirrors the canonical "release non-playing buffers first" pass.  Caller holds
// g_mutex.
void reap(Voices& v)
{
    for (int i = 0; i < MA_MAX_SOUND_BUFFERS; ++i)
        if (v.s[i].loaded && ma_sound_is_playing(&v.s[i].sound) != MA_TRUE)
            unloadSlot(v.s[i]);
}

// Which slot a request names.  Buffer 0 means "first free", and — as on Windows
// — only the first HALF of the range is scanned, reserving 6-10 for explicit
// allocation.  Caller holds g_mutex.
int chooseSlot(Voices& v, int buffer)
{
    if (buffer >= 1 && buffer <= MA_MAX_SOUND_BUFFERS)
        return buffer - 1;                    // explicit buffer
    for (int i = 0; i < (MA_MAX_SOUND_BUFFERS / 2); ++i)
        if (!v.s[i].loaded) return i;
    return 0;                                 // all busy: slot 1
}

// Apply the three per-voice settings.  Caller holds g_mutex.
void applySettings(Slot& sl, int loop, double volume, double pan)
{
    // Volume percent [-100, 0] (0 = full) → linear amplitude gain.  This matches
    // Windows' DSBVOLUME, which is hundredths of a dB: iVolume = Volume * 100,
    // i.e. the attenuation in dB equals Volume, and gain = 10^(dB/20).
    if (volume > 0.0)    volume = 0.0;
    if (volume < -100.0) volume = -100.0;
    // Pan percent [-100, +100] → miniaudio pan [-1, +1].
    if (pan >  100.0) pan =  100.0;
    if (pan < -100.0) pan = -100.0;

    ma_sound_set_looping(&sl.sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume (&sl.sound, (float) std::pow(10.0, volume / 20.0));
    ma_sound_set_pan    (&sl.sound, (float) (pan / 100.0));
}

} // namespace

int ma_audio_play(const void* owner, int buffer, const char* path, int loop,
                  double volume, double pan)
{
    if (!path || !*path) return 0;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!ensureEngine()) return 0;

    Voices& v = voicesFor(owner);
    reap(v);
    const int idx = chooseSlot(v, buffer);
    Slot& sl = v.s[idx];
    unloadSlot(sl);

    // Decode the whole file up front (short sounds, low latency).  Disable
    // spatialization so pan is a plain stereo pan with no 3D attenuation.
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(&g_engine, path, flags, NULL, NULL, &sl.sound)
            != MA_SUCCESS)
        return 0;
    sl.loaded = true;

    applySettings(sl, loop, volume, pan);

    if (ma_sound_start(&sl.sound) != MA_SUCCESS) {
        unloadSlot(sl);
        return 0;
    }
    return idx + 1;
}

int ma_audio_play_memory(const void* owner, int buffer, const void* data, size_t len,
                         int loop, double volume, double pan)
{
    if (!data || len == 0) return 0;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!ensureEngine()) return 0;

    Voices& v = voicesFor(owner);
    reap(v);
    const int idx = chooseSlot(v, buffer);
    Slot& sl = v.s[idx];
    unloadSlot(sl);

    // Own a copy of the encoded bytes; the decoder (and the voice that reads it)
    // reference this for their whole lifetime — freed together in unloadSlot.
    sl.mem.assign((const unsigned char*) data, (const unsigned char*) data + len);
    if (ma_decoder_init_memory(sl.mem.data(), sl.mem.size(), NULL, &sl.decoder)
            != MA_SUCCESS) {
        std::vector<unsigned char>().swap(sl.mem);
        return 0;
    }
    sl.hasDecoder = true;

    if (ma_sound_init_from_data_source(&g_engine, &sl.decoder,
                                       MA_SOUND_FLAG_NO_SPATIALIZATION, NULL,
                                       &sl.sound) != MA_SUCCESS) {
        unloadSlot(sl);                       // frees the decoder + bytes
        return 0;
    }
    sl.loaded = true;

    applySettings(sl, loop, volume, pan);

    if (ma_sound_start(&sl.sound) != MA_SUCCESS) {
        unloadSlot(sl);
        return 0;
    }
    return idx + 1;
}

int ma_audio_adjust(const void* owner, int buffer, int loop, double volume, double pan)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (buffer < 1 || buffer > MA_MAX_SOUND_BUFFERS) return 0;
    Voices* v = findVoices(owner);
    if (!v) return 0;
    Slot& sl = v->s[buffer - 1];
    if (!slotPlaying(sl)) return 0;

    applySettings(sl, loop, volume, pan);
    return 1;
}

void ma_audio_stop(const void* owner, int buffer)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_engineReady) return;
    Voices* v = findVoices(owner);
    if (!v) return;
    if (buffer == 0) {                        // stop all of THIS world's sounds
        for (int i = 0; i < MA_MAX_SOUND_BUFFERS; ++i) unloadSlot(v->s[i]);
        return;
    }
    if (buffer < 1 || buffer > MA_MAX_SOUND_BUFFERS) return;
    unloadSlot(v->s[buffer - 1]);
}

int ma_audio_status(const void* owner, int buffer)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (buffer < 1 || buffer > MA_MAX_SOUND_BUFFERS) return -2;
    // No usable audio device -> -3, matching Windows' "no DirectSound object".
    // ensureEngine() is the same device-open the play paths use (and takes no lock).
    if (!ensureEngine()) return -3;
    Voices* v = findVoices(owner);
    if (!v) return -2;                                    // never played: all free
    Slot& sl = v->s[buffer - 1];
    if (!sl.loaded) return -2;                            // free
    if (ma_sound_is_playing(&sl.sound) != MA_TRUE) return 0;   // not playing
    return ma_sound_is_looping(&sl.sound) == MA_TRUE ? 2 : 1;
}

void ma_audio_release_owner(const void* owner)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    std::map<const void*, std::unique_ptr<Voices>>::iterator it = g_owners.find(owner);
    if (it == g_owners.end()) return;
    if (it->second)
        for (int i = 0; i < MA_MAX_SOUND_BUFFERS; ++i)
            unloadSlot(it->second->s[i]);
    g_owners.erase(it);
}


#endif // !_WIN32
