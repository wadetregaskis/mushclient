// methods_sounds.cpp

// Relating to playing sounds

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../errors.h"

// Implements:

//    GetSoundStatus
//    PlaySound
//    Sound
//    StopSound

#ifndef _NOSOUND

// world.Sound - plays a sound

long CMUSHclientDoc::Sound(LPCTSTR SoundFileName) 
{
  if (SoundFileName [0] == 0)
    return eNoNameSpecified;

  if (!PlaySoundFile (SoundFileName))
  	return eCannotPlaySound;

  return eOK;
}   // end of CMUSHclientDoc::Sound


// start playing sound FileName in buffer Buffer (or in memory in MemoryBuffer for length MemLen)

// If not a fully-qualified path, prepend mushclient.exe directory followed by "sounds\"
// Loop when finished if Loop is true
// Play at volume 'Volume' (percent) - 0 is full volume, -100 is -100 db  
//   (3 db each time halves the volume, so -3 would be half volume, and -6 would be quarter volume)
// Pan at Pan -100 to +100 percent where -100 is fully left and +100 is fully right (and zero is centered)
// If Buffer is nonzero, and FileName is "" then adjust the volume, pan and loop flag for an existing, playing, sound, if possible
// If Buffer zero then try to take an unused buffer. If that fails, take a non-playing buffer. If that fails, take buffer 1
// Buffer should be zero (for any free buffer) or 1 to MAX_SOUND_BUFFERS (currently 10)

// Sound files must be .WAV files, 16-bit, 22.05KHz, PCM, Mono or stereo

// test: /print (PlaySound (9, "/Program Files/Mushclient/sounds/battle/battle01.wav", true, 100))

long CMUSHclientDoc::PlaySoundHelper(short Buffer, LPCTSTR FileName, BOOL Loop, double Volume, double Pan, LPCTSTR MemoryBuffer, size_t MemLen) 
{
int i;

  // no sound object? can't do it
  if (!App.m_pDirectSoundObject || !App.m_pDirectSoundPrimaryBuffer)
    return eCannotPlaySound ;

  // first release sound buffers for sounds that are not playing or are lost
  for (i = 0; i < MAX_SOUND_BUFFERS; i++)
    if (m_pDirectSoundSecondaryBuffer [i])
      {
      DWORD iStatus;
      if (SUCCEEDED (m_pDirectSoundSecondaryBuffer [i]->GetStatus (&iStatus)) &&
          (((iStatus & (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING)) == 0) ||
          (iStatus & DSBSTATUS_BUFFERLOST ) ) )
        {  
        m_pDirectSoundSecondaryBuffer [i]->Release ();
        m_pDirectSoundSecondaryBuffer [i] = NULL;
        }
      }


  // -------------------------- convert volume from percent into db * 100 ----------------------------

  // Force volume into range
  if (Volume > 0 || Volume < (-100.0))
    Volume = 0.0;

  long iVolume = - (Volume / 100) * DSBVOLUME_MIN;


  // -------------------------- convert pan from percent into +/- db * 100 ----------------------------

  // Force Pan into range
  if (Pan > 100.0 || Pan < (-100))
    Pan = 0;

  long iPan = (Pan / 100) * DSBPAN_RIGHT;


  // ---------------------- if no filename, attempt to adjust existing sound ---------------------

  if (MemoryBuffer == NULL)
    {
    if (Buffer >= 1 && Buffer <= MAX_SOUND_BUFFERS && strlen (FileName) == 0)
      {
      Buffer--;   // make zero-relative

      // can't if not there
      if (m_pDirectSoundSecondaryBuffer [Buffer] == NULL)
        return eBadParameter;
      
      // don't if not playing
      DWORD iStatus;
      if (FAILED (m_pDirectSoundSecondaryBuffer [Buffer]->GetStatus (&iStatus)) ||
          (iStatus & DSBSTATUS_PLAYING) == 0)
        return eCannotPlaySound;

        // set requested panning
      m_pDirectSoundSecondaryBuffer [Buffer]->SetPan (iPan);
      // set requested volume
      m_pDirectSoundSecondaryBuffer [Buffer]->SetVolume (iVolume);
      // set requested looping
      m_pDirectSoundSecondaryBuffer [Buffer]->Play( 0, 0, Loop ? DSBPLAY_LOOPING : 0);

      return eOK;

      }

    // no file name AND no buffer number? can't do much
    if  (strlen (FileName) < 2)  // later I test the 1st and 2nd characters
      return eBadParameter;

    } // have a file name (not memory file)

  // ------------------- we have filename, get on with it -----------------------------

  // ---------------- if Buffer is zero, first try to find a free one -----------------

  if (Buffer == 0)
    {
    for (i = 0; i < (MAX_SOUND_BUFFERS / 2); i++)
      if (m_pDirectSoundSecondaryBuffer [i] == NULL)
        {
        Buffer = i + 1;
        break;
        } // end of found an unused slot
    }  // end of no buffer specifed

  // no free buffers? re-use buffer #1
  if (Buffer == 0)
    Buffer = 1;  

  // Make buffer zero-relative
  Buffer--;
  
  // Buffer must be in range
  if (Buffer < 0 || Buffer >= MAX_SOUND_BUFFERS)
    return eBadParameter;


  // ------------------- we have a buffer now -------------------------------

  CString strName;

  if (MemoryBuffer == NULL)
    {

    // ---------- if not absolute path, use MUSHclient.exe / sounds / filename 

    strName = FileName;

    strName.Replace ("/", "\\");  // convert forward to backslash

    // without a full pathname, assume in sounds directory under MUSHclient.exe
    if (!(
        isalpha ((unsigned char) strName [0]) &&
        strName [1] == ':'               // eg. c: blah
        ) &&      
        strName [0] != '\\')          // eg. \mydir\blah or \\server\blah
      strName = ExtractDirectory (App.m_strMUSHclientFileName) + "sounds\\" + strName;

    if (strName.GetLength () > 127)
      return eBadParameter;

    } // have a file name (not memory file)



  // ---------- open multimedia file ----------------------------

  HMMIO          mmioWave ;
  MMCKINFO       mmckinfoParent, mmckinfoSubchunk ;
  WAVEFORMATEX   wfPCM ;
  MMIOINFO mmioInfo;
  memset(&mmioInfo, 0, sizeof(MMIOINFO));

  // Open the wave data using the mm functions
  if (MemoryBuffer == NULL)
    {
    char sFilename [128];
    strcpy (sFilename, strName);  // make not const

    mmioWave = mmioOpen (sFilename, NULL, MMIO_READ);   // open from disk
    }   // have a file name (not memory file)
  else
    {  // open from memory
    mmioInfo.pchBuffer = (char *) MemoryBuffer;
    mmioInfo.cchBuffer = MemLen;
    mmioInfo.fccIOProc = FOURCC_MEM;
    mmioWave = mmioOpen (NULL, &mmioInfo, MMIO_READ) ;
    }

  if ( !mmioWave )
   return eFileNotFound;

  // now that we know we have a file there, release the sound buffer if possible

  // buffer currently in use? release it
  if (m_pDirectSoundSecondaryBuffer [Buffer])
    {
    DWORD iStatus;
    if (SUCCEEDED (m_pDirectSoundSecondaryBuffer [Buffer]->GetStatus (&iStatus)) &&
        (iStatus & DSBSTATUS_PLAYING))
      m_pDirectSoundSecondaryBuffer [Buffer]->Stop ();

    m_pDirectSoundSecondaryBuffer [Buffer]->Release ();
    m_pDirectSoundSecondaryBuffer [Buffer] = NULL;
    }

  // Descend to find a "WAVE" block, if this fails then the data is not
  // WAV data.
  mmckinfoParent.fccType = mmioFOURCC( 'W', 'A', 'V', 'E' ) ;
  if ( mmioDescend( mmioWave, &mmckinfoParent, NULL, MMIO_FINDRIFF ) ) 
   {
    mmioClose( mmioWave, 0 ) ;
    return eCannotPlaySound ;
   }

  // Descend again to the "fmt " block and retrieve the format information
  mmckinfoSubchunk.ckid = mmioFOURCC( 'f', 'm', 't', ' ' ) ;
  if ( mmioDescend( mmioWave, &mmckinfoSubchunk, &mmckinfoParent, 
                   MMIO_FINDCHUNK ) )
   {
    mmioClose( mmioWave, 0 ) ;
    return eCannotPlaySound ;
   }

  // This line actually reads the data from the "fmt " chunk, this data
  // should be in the form of a WAVEFORMATEX structure
  if ( mmioRead( mmioWave, (char *) &wfPCM, mmckinfoSubchunk.cksize ) == -1 )
   {
    mmioClose( mmioWave, 0 ) ;
    return eCannotPlaySound ;
   }

  // Step out a layer... think of the mm functions as step in and out of
  // hierarchies of "chunks" of information
  mmioAscend( mmioWave, &mmckinfoSubchunk, 0 ) ;

  // Find the "data" subchunk
  mmckinfoSubchunk.ckid = mmioFOURCC( 'd', 'a', 't', 'a' ) ;
  if ( mmioDescend( mmioWave, &mmckinfoSubchunk, &mmckinfoParent, 
                    MMIO_FINDCHUNK ) ) 
   {
    mmioClose( mmioWave, 0 ) ;
    return eCannotPlaySound ;
   }

 HRESULT        hr ;
 LPBYTE         lpvAudio1 ;
 LPBYTE         lpvAudio2 ;
 DWORD          dwWriteBytes1 ;
 DWORD          dwWriteBytes2 ;
 DSBUFFERDESC   bd ;   
 
 // At this point we have succeeded in finding the data for the WAV file so
 // we need to create a DirectSoundBuffer 
  // Set up bd structure for a static secondary buffer.       
  bd.dwSize = sizeof( bd ) ;       
  bd.dwFlags = (DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY) | DSBCAPS_STATIC;  

  // want to hear in background?
  if (m_bPlaySoundsInBackground)
    bd.dwFlags |= DSBCAPS_GLOBALFOCUS;

  // Buffer size retrieved from the mmckinfo structure for the data 
  // portion of the wav
  bd.dwBufferBytes = mmckinfoSubchunk.cksize ; 
  bd.dwReserved = 0 ;
  bd.lpwfxFormat = &wfPCM ; 

  // Create buffer.    
  if ( FAILED( App.m_pDirectSoundObject->CreateSoundBuffer( &bd, 
               &m_pDirectSoundSecondaryBuffer [Buffer], NULL ) ) )
   {
    mmioClose( mmioWave, 0 ) ;
    return eCannotPlaySound ;
   }
 
#define WRAP_UP_FAILED_SOUND \
    m_pDirectSoundSecondaryBuffer [Buffer]->Release ();  \
    m_pDirectSoundSecondaryBuffer [Buffer] = NULL;  \
    mmioClose( mmioWave, 0 ) ;  \
    return eCannotPlaySound;

 // make copy to save a bit of typing
 LPDIRECTSOUNDBUFFER lpdsBuffer = m_pDirectSoundSecondaryBuffer [Buffer];

 // Lock the buffer for the DirectSoundBuffer object
 hr = lpdsBuffer->Lock( 0, 0, (void **) &lpvAudio1, &dwWriteBytes1, 
               (void **) &lpvAudio2, &dwWriteBytes2, DSBLOCK_ENTIREBUFFER ) ;
 if ( hr == DSERR_BUFFERLOST )
   {
    // If the buffer was lost try restoring it and lock again
    lpdsBuffer->Restore() ;
    hr = lpdsBuffer->Lock( 0, 0, (void **) &lpvAudio1, &dwWriteBytes1, 
               (void **) &lpvAudio2, &dwWriteBytes2, DSBLOCK_ENTIREBUFFER ) ;
   }

  if ( FAILED( hr ) )
    {
    WRAP_UP_FAILED_SOUND;  // lock failed
    }
    
  if ( dwWriteBytes1 != mmckinfoSubchunk.cksize )
    {
    lpdsBuffer->Unlock( (void *) lpvAudio1, dwWriteBytes1, 
                        (void *) lpvAudio2, dwWriteBytes2 );
    WRAP_UP_FAILED_SOUND;  // wrong number of bytes
    }

  // Read the data directly into the locked buffer
  if ( mmioRead( mmioWave, ( char* ) lpvAudio1, mmckinfoSubchunk.cksize ) 
      == -1 )
    {
    lpdsBuffer->Unlock( (void *) lpvAudio1, dwWriteBytes1, 
                        (void *) lpvAudio2, dwWriteBytes2 );
    WRAP_UP_FAILED_SOUND;  // couldn't read
    }

  // Unlock the buffer
  if ( FAILED( lpdsBuffer->Unlock( (void *) lpvAudio1, dwWriteBytes1, 
                                  (void *) lpvAudio2, dwWriteBytes2 ) ) )
    {
    WRAP_UP_FAILED_SOUND;   // couldn't unlock
    }
 
  // Close the multimedia object
  mmioClose( mmioWave, 0 ) ;

  // set requested panning
  lpdsBuffer->SetPan (iPan);
  // set requested volume
  lpdsBuffer->SetVolume (iVolume);

 if (FAILED (lpdsBuffer->Play( 0, 0, Loop ? DSBPLAY_LOOPING : 0) ))
   {
    m_pDirectSoundSecondaryBuffer [Buffer]->Release ();
    m_pDirectSoundSecondaryBuffer [Buffer] = NULL;
    return eCannotPlaySound;
   }

  return eOK;
} // end of CMUSHclientDoc::PlaySoundHelper

long CMUSHclientDoc::PlaySound(short Buffer, LPCTSTR FileName, BOOL Loop, double Volume, double Pan) 
  {
  return PlaySoundHelper (Buffer, FileName, Loop, Volume, Pan, NULL, 0);
  }  // end of  CMUSHclientDoc::PlaySound


// stop the sound in the numbered Buffer from playing
// if Buffer is zero, stop all sounds
long CMUSHclientDoc::StopSound(short Buffer) 
{
  // buffer of zero means all
  if (Buffer == 0)
    {
    // stop sounds playing, release sound buffers
    for (int i = 0; i < MAX_SOUND_BUFFERS; i++)
      if (m_pDirectSoundSecondaryBuffer [i])
        {
        DWORD iStatus;
        if (SUCCEEDED (m_pDirectSoundSecondaryBuffer [i]->GetStatus (&iStatus)) &&
            (iStatus & DSBSTATUS_PLAYING))
          m_pDirectSoundSecondaryBuffer [i]->Stop ();

        m_pDirectSoundSecondaryBuffer [i]->Release ();
        m_pDirectSoundSecondaryBuffer [i] = NULL;

        }
      return eOK;
    }

  // Make buffer zero-relative
  Buffer--;
  
  // Buffer must be in range
  if (Buffer < 0 || Buffer >= MAX_SOUND_BUFFERS)
    return eBadParameter;

  // buffer currently in use? release it
  if (m_pDirectSoundSecondaryBuffer [Buffer])
    {
    DWORD iStatus;
    if (SUCCEEDED (m_pDirectSoundSecondaryBuffer [Buffer]->GetStatus (&iStatus)) &&
        (iStatus & DSBSTATUS_PLAYING))
      m_pDirectSoundSecondaryBuffer [Buffer]->Stop ();

    m_pDirectSoundSecondaryBuffer [Buffer]->Release ();
    m_pDirectSoundSecondaryBuffer [Buffer] = NULL;
    }

	return eOK;
}  // end of CMUSHclientDoc::StopSound

// set the status of the sound buffer
// -1 means buffer number out of range (1 to MAX_SOUND_BUFFERS)
// -2 means that the buffer is free
// -3 means sound system is not initialized (eg. Windows NT)
//  0 means the sound is NOT playing
//  1 means the sound is playing but not looping
//  2 means the sound is playing AND looping

long CMUSHclientDoc::GetSoundStatus(short Buffer) 
{

  // no sound object? can't do it
  if (!App.m_pDirectSoundObject || !App.m_pDirectSoundPrimaryBuffer)
    return -3;

  // Make buffer zero-relative
  Buffer--;
  
  // Buffer must be in range
  if (Buffer < 0 || Buffer >= MAX_SOUND_BUFFERS)
    return -1;

  if (!m_pDirectSoundSecondaryBuffer [Buffer])
    return -2;

  DWORD iStatus = 0;

  if (FAILED (m_pDirectSoundSecondaryBuffer [Buffer]->GetStatus (&iStatus)))
    return 0;  // can't get status, presume no sound

  if (iStatus & DSBSTATUS_PLAYING)
    return 1;

  if (iStatus & DSBSTATUS_LOOPING)
    return 2;

	return 0;  // not playing
}  // end of CMUSHclientDoc::GetSoundStatus

#else // _NOSOUND — portable audio.
//
// DirectSound isn't available off-Windows, but the engine already has a working
// audio path: CMUSHclientDoc::PlaySoundFile / CancelSound route to the wxSound
// hook (g_wxhook_play_sound — see doc_portable.cpp + platform/wx_mfc_impl.cpp),
// which MSP and trigger sounds already use.  These methods hand the Lua sound API
// (world.Sound / PlaySound / StopSound) to that same path.
//
// The port's audio backend is miniaudio (platform/ma_sound.*), reached through
// the g_wxhook_sound_* hooks.  It mixes up to MAX_SOUND_BUFFERS concurrent
// voices, each with its own Volume, Pan and Loop — the same capabilities
// DirectSound gave the Windows build — so PlaySound / StopSound / GetSoundStatus
// now behave like Windows (buffers, volume, pan, the "adjust playing buffer"
// case, and real per-buffer status).  Memory-buffer playback is the one gap
// (rejected, as before).  In the tests the hooks are null → playback no-ops.

#include "../errors.h"
#include <cctype>
#include "platform/wx_mfc_hooks.h"   // g_wxhook_sound_* (miniaudio engine)

// world.Sound — play a sound file (the common one-shot API).  Mirrors Windows:
// empty name → error; otherwise hand off to the working PlaySoundFile path.
long CMUSHclientDoc::Sound(LPCTSTR SoundFileName)
{
  if (!SoundFileName || SoundFileName [0] == 0)
    return eNoNameSpecified;
  if (!PlaySoundFile (SoundFileName))
    return eCannotPlaySound;
  return eOK;
}

// world.PlaySound — the buffered API.  Drives the miniaudio engine via the
// g_wxhook_sound_* hooks: Buffer (0 = auto), Volume %, Pan % and Loop all apply,
// and "empty filename + buffer" retunes an already-playing voice.  Mirrors the
// Windows DirectSound helper's parameter handling, including memory-image
// playback.  Null hooks (tests) → no audio backend.
long CMUSHclientDoc::PlaySoundHelper(short Buffer, LPCTSTR FileName, BOOL Loop,
                                     double Volume, double Pan,
                                     LPCTSTR MemoryBuffer, size_t MemLen)
{
  if (!g_wxhook_sound_play)
    return eCannotPlaySound;                 // no audio backend (CLI / tests)

  // Force Volume / Pan into range exactly like the Windows helper.
  if (Volume > 0 || Volume < -100.0) Volume = 0.0;
  if (Pan > 100.0 || Pan < -100.0)   Pan    = 0.0;

  // ---- memory image (world.PlaySoundMemory): hand the encoded bytes to the
  //      backend, which copies + decodes them.  Buffer 0 (auto) or 1..MAX. ----
  if (MemoryBuffer != NULL)
    {
    if (Buffer < 0 || Buffer > MAX_SOUND_BUFFERS)
      return eBadParameter;
    if (!g_wxhook_sound_play_memory)
      return eCannotPlaySound;
    return (g_wxhook_sound_play_memory (Buffer, MemoryBuffer, MemLen,
                                        Loop != 0, Volume, Pan) != 0)
           ? eOK : eCannotPlaySound;
    }

  // ---- empty filename + valid buffer → adjust the playing voice in place ----
  if (Buffer >= 1 && Buffer <= MAX_SOUND_BUFFERS &&
      (FileName == NULL || strlen (FileName) == 0))
    {
    int status = g_wxhook_sound_status ? g_wxhook_sound_status (Buffer) : -2;
    if (status == -2)                        // buffer is free → nothing to adjust
      return eBadParameter;
    if (status <= 0)                         // loaded but not playing
      return eCannotPlaySound;
    return (g_wxhook_sound_adjust &&
            g_wxhook_sound_adjust (Buffer, Loop != 0, Volume, Pan))
           ? eOK : eCannotPlaySound;
    }

  // Need a real filename from here on.
  if (FileName == NULL || strlen (FileName) < 2)
    return eBadParameter;

  // Buffer must be 0 (auto) or 1..MAX.
  if (Buffer < 0 || Buffer > MAX_SOUND_BUFFERS)
    return eBadParameter;

  // Resolve a non-absolute path against <exe-dir>/sounds/ (mirrors Windows).
  CString strName = FileName;
  if (strName [0] != '/' &&
      !(isalpha ((unsigned char) strName [0]) && strName [1] == ':'))
    strName = ExtractDirectory (App.m_strMUSHclientFileName) + "sounds/" + strName;

  // play returns the buffer used (1..N), or 0 on failure.
  if (g_wxhook_sound_play (Buffer, (LPCTSTR) strName, Loop != 0, Volume, Pan) == 0)
    return eCannotPlaySound;
  return eOK;
}

long CMUSHclientDoc::PlaySound(short Buffer, LPCTSTR FileName, BOOL Loop, double Volume, double Pan)
{
  return PlaySoundHelper (Buffer, FileName, Loop, Volume, Pan, NULL, 0);
}

// world.StopSound — stop one buffer (1..N) or all (0) via the engine.
// Do NOT call CancelSound(): canonical CancelSound() ends with StopSound(0), so
// routing StopSound back through it would recurse infinitely on the port.
long CMUSHclientDoc::StopSound(short Buffer)
{
  if (g_wxhook_sound_stop_buffer)
    g_wxhook_sound_stop_buffer (Buffer);
  else if (Buffer == 0 && g_wxhook_stop_sound)
    g_wxhook_stop_sound();
  return eOK;
}

// world.GetSoundStatus — real per-buffer state from the engine.
//   -3 sound system unavailable, -1 buffer out of range, -2 free,
//    0 not playing, 1 playing, 2 playing + looping.
long CMUSHclientDoc::GetSoundStatus(short Buffer)
{
  if (!g_wxhook_sound_status)
    return -3;                               // no audio backend
  if (Buffer < 1 || Buffer > MAX_SOUND_BUFFERS)
    return -1;
  return g_wxhook_sound_status (Buffer);
}

#endif // _NOSOUND
