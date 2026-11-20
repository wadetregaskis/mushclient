// pal/isound.h -- the sound seam of the Platform Abstraction Layer, as far as it has been
// needed: playing a short cue.  The rest of what docs/PAL.md lists under ISound (load, status,
// and so on) still goes straight to world.PlaySound's engines, DirectSound on Windows and
// miniaudio elsewhere.  Those are what the implementation in a11y.cpp plays through.

#pragma once

namespace pal {

class ISound
  {
  public:
    virtual ~ISound () {}

    // Fire and forget.  Volume is as world.PlaySound: 0 is full and -100 silent.  Pan is
    // -100 left to +100 right.  False if there was nothing to play it with.
    virtual bool playCue (const char * file, double volume, double pan) = 0;
  };

}  // namespace pal
