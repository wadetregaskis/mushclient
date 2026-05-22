/* -------------- SHS.H --------------------------- */
#ifndef SCRIPTING_SHS_H_INCLUDED
#define SCRIPTING_SHS_H_INCLUDED

/* BYTE may already be defined by platform/windows_types.h on non-Windows.
 * Guard against the redefinition. */
#ifndef BYTE
typedef unsigned char BYTE;
#endif

/* Use 'unsigned int' (always 32-bit) rather than 'unsigned long'
 * which is 64-bit on LP64 platforms (Linux/macOS 64-bit). */
typedef unsigned int SHSLONG;

#define SHS_BLOCKSIZE 64

#define SHS_DIGESTSIZE 20

typedef struct
  {
  SHSLONG digest [5];  // message digest
  SHSLONG countLo, countHi;  // 64-bit bit count
  SHSLONG data [16];   // SHS data buffer
  } SHS_INFO;

// include the define below if running on a little-endian machine, such as Intel

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN
#endif

#ifdef __cplusplus
extern "C" {
#endif

  void shsInit (SHS_INFO * shsInfo);
  void shsUpdate (SHS_INFO * shsInfo, BYTE * buffer, int count);
  void shsFinal (SHS_INFO * shsInfo);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPTING_SHS_H_INCLUDED */
