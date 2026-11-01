// platform/ma_decode.h -- see ma_decode.cpp.

#pragma once

#ifdef _WIN32

#include <string>

// A sound file as a WAV image of 16-bit PCM, for PlaySoundHelper's memory path.
// False if it is not something miniaudio can read.
bool MC_DecodeAudioFileToWav (const wchar_t * path, std::string * wav);

#endif
