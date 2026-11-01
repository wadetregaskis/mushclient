// platform/ma_decode.cpp
// Windows' half of sound files other than the WAVs DirectSound's loader can open:
// miniaudio's decoders, the ones the port plays with, turning a file into a WAV in
// memory for the existing DirectSound path.  No device code: this is decoding only.
//
// What it reads: WAV, MP3 and FLAC, which miniaudio has built in.  Not Ogg Vorbis, whose
// decoder is a separate file that is not in this tree.

#include "stdafx.h"
#include "platform/ma_decode.h"

#ifdef _WIN32

#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include "platform/miniaudio/miniaudio.h"

#include <cstring>

namespace {

// DirectSound's static buffers top out below 256 MB, and a file this long is music
// someone can wait to hear the first eleven minutes of (at 44.1 kHz, 16 bits, stereo).
const size_t kMaxPcmBytes = 128 * 1024 * 1024;

void Append32 (std::string & s, const unsigned long value)
  {
  for (int i = 0; i < 4; i++)
    s += (char) ((value >> (8 * i)) & 0xFF);
  }

void Append16 (std::string & s, const unsigned int value)
  {
  s += (char) (value & 0xFF);
  s += (char) ((value >> 8) & 0xFF);
  }

} // anonymous namespace

bool MC_DecodeAudioFileToWav (const wchar_t * path, std::string * wav)
  {
  // always 16-bit: whatever the file is, DirectSound takes this
  ma_decoder_config config = ma_decoder_config_init (ma_format_s16, 0, 0);

  ma_decoder decoder;
  if (ma_decoder_init_file_w (path, &config, &decoder) != MA_SUCCESS)
    return false;

  const unsigned int channels = decoder.outputChannels;
  const unsigned long rate = decoder.outputSampleRate;

  bool bOK = channels >= 1 && channels <= 2 && rate > 0;

  // anything with more channels than this goes down to stereo
  if (!bOK && channels > 2 && rate > 0)
    {
    ma_decoder_uninit (&decoder);
    config = ma_decoder_config_init (ma_format_s16, 2, 0);
    if (ma_decoder_init_file_w (path, &config, &decoder) != MA_SUCCESS)
      return false;
    bOK = true;
    }

  if (!bOK)
    {
    ma_decoder_uninit (&decoder);
    return false;
    }

  const unsigned int outChannels = decoder.outputChannels;
  const unsigned int frameBytes = outChannels * 2;

  std::string pcm;
  char buffer [65536];
  const ma_uint64 framesPerRead = sizeof buffer / frameBytes;

  for (;;)
    {
    ma_uint64 framesRead = 0;
    const ma_result result = ma_decoder_read_pcm_frames (&decoder, buffer, framesPerRead, &framesRead);
    if (framesRead)
      pcm.append (buffer, (size_t) framesRead * frameBytes);
    if (result != MA_SUCCESS || framesRead < framesPerRead || pcm.size () >= kMaxPcmBytes)
      break;
    }

  ma_decoder_uninit (&decoder);

  if (pcm.empty ())
    return false;

  wav->clear ();
  wav->reserve (pcm.size () + 44);
  wav->append ("RIFF");
  Append32 (*wav, (unsigned long) (pcm.size () + 36));
  wav->append ("WAVEfmt ");
  Append32 (*wav, 16);
  Append16 (*wav, 1);                       // PCM
  Append16 (*wav, outChannels);
  Append32 (*wav, rate);
  Append32 (*wav, rate * frameBytes);
  Append16 (*wav, frameBytes);
  Append16 (*wav, 16);
  wav->append ("data");
  Append32 (*wav, (unsigned long) pcm.size ());
  wav->append (pcm);
  return true;
  }

#endif // _WIN32
