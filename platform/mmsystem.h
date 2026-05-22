// platform/mmsystem.h — stub for Windows <mmsystem.h>
// Provides minimal multimedia types and constants needed by MUSHclient
// dialog code that hasn't been fully ported yet.
#pragma once
#ifndef _WIN32

#include <stdint.h>

typedef uint32_t  FOURCC;
typedef uint16_t  WORD;

// MMIO types
typedef void*     HMMIO;
typedef struct { int unused; } MMCKINFO;
typedef struct { int unused; } MMIOINFO;
typedef struct { int unused; } WAVEFORMATEX;

// MMIO constants
#define MMIO_READ         0x0000
#define MMIO_WRITE        0x0001
#define MMIO_READWRITE    0x0002
#define MMIO_FINDRIFF     0x0020
#define MMIO_FINDCHUNK    0x0010
#define FOURCC_MEM        0

#define mmioFOURCC(c0,c1,c2,c3) \
    ((FOURCC)(uint8_t)(c0) | ((FOURCC)(uint8_t)(c1)<<8) | \
     ((FOURCC)(uint8_t)(c2)<<16) | ((FOURCC)(uint8_t)(c3)<<24))

inline HMMIO  mmioOpen(char*, MMIOINFO*, uint32_t) { return nullptr; }
inline int    mmioClose(HMMIO, uint16_t) { return 0; }
inline int    mmioDescend(HMMIO, MMCKINFO*, const MMCKINFO*, uint16_t) { return -1; }
inline int    mmioAscend(HMMIO, MMCKINFO*, uint16_t) { return 0; }
inline long   mmioRead(HMMIO, char*, long) { return -1; }

#endif // !_WIN32
