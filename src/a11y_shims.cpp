// a11y_shims.cpp -- plugins the client now does itself.
//
// Blind players' soundpacks are built on two plugins from 2014 that need a DLL each:
// MushReader (speech) and LuaAudio (sound).  Those DLLs are 32-bit, and do not exist for
// macOS or Linux.  So a plugin file with one of those IDs is not run: what loads is the
// small Lua plugin here, with the same ID and the same functions, on top of world.Speak
// and world.PlaySound.  Being a real plugin it answers to CallPlugin, PluginSupports,
// IsPluginInstalled, GetPluginInfo and ppi.Load exactly as the original does, and the file
// on disk is left alone.
//
// These were written from the plugin interfaces as other plugins use them: not from the
// originals, and nothing here comes from QMud's implementation, which is GPL.

#include "a11y.h"

#include <cctype>
#include <cstring>
#include <string>

const char * const kA11yMushReaderID = "925cdd0331023d9f0b8f05a7";
const char * const kA11yLuaAudioID   = "aedf0cb0be5bf045860d54b7";

namespace {

const char kMushReaderShim [] = R"SHIM(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="MushReader" author="MUSHclient" id="925cdd0331023d9f0b8f05a7" language="Lua"
  purpose="Speech through the client's own support, for plugins that ask for MushReader"
  date_written="2026-11-01" requires="4.00" version="11.0">
<description trim="y"><![CDATA[
MUSHclient reads aloud by itself now, so this stands in for the MushReader plugin and its DLL,
which were not loaded.  Other plugins can still call:

  CallPlugin ("925cdd0331023d9f0b8f05a7", "say", text)
  CallPlugin ("925cdd0331023d9f0b8f05a7", "interrupt", text)
  CallPlugin ("925cdd0331023d9f0b8f05a7", "stop")

Commands: tts, tts_note, tts_interrupt, tts_stop, subst.
]]></description>
</plugin>
<script><![CDATA[
function say (text)
  Speak (tostring (text or ""), "mushreader", 1, false)
end
function interrupt (text)
  Speak (tostring (text or ""), "mushreader", 2, true)
end
function stop ()
  StopSpeech ()
end
function plugin_update_url ()
  return ""
end
-- The original read everything out, with a screen reader or without.
function OnPluginInstall ()
  SetOption ("speech_read_all", 1)
end
function OnPluginEnable ()
  SetOption ("speech_read_all", 1)
end
function OnPluginDisable ()
  SetOption ("speech_read_all", 0)
end
function OnPluginClose ()
  SetOption ("speech_read_all", 0)
end
]]></script>
</muclient>
)SHIM";

const char kLuaAudioShim [] = R"SHIM(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE muclient>
<muclient>
<plugin name="LuaAudio" author="MUSHclient" id="aedf0cb0be5bf045860d54b7" language="Lua"
  purpose="Sound through the client's own engine, for plugins that ask for LuaAudio"
  date_written="2026-11-01" requires="4.00" version="11.0">
<description trim="y"><![CDATA[
The client plays sound by itself on every platform now, so this stands in for the LuaAudio plugin
and its audio.dll, which were not loaded.  WAV, MP3 and FLAC play.  An Ogg file plays only if a file
of the same name in one of those kinds is beside it.  Pitch is accepted and has no effect.

Commands: sound_toggle, volume_up, volume_down, vol, sound_close.
]]></description>
</plugin>
<aliases>
  <alias match="^sound_toggle$" regexp="y" enabled="y" script="cmd_toggle" sequence="100"></alias>
  <alias match="^volume_up$" regexp="y" enabled="y" script="cmd_volume_up" sequence="100"></alias>
  <alias match="^volume_down$" regexp="y" enabled="y" script="cmd_volume_down" sequence="100"></alias>
  <alias match="^vol$" regexp="y" enabled="y" script="cmd_vol" sequence="100"></alias>
  <alias match="^sound_close$" regexp="y" enabled="y" script="free" sequence="100"></alias>
</aliases>
<script><![CDATA[
local have_ppi = pcall (require, "ppi")

-- Volume here is 0 to 100, and PlaySound's is a dB-like -100 to 0.
local master_volume = 100
local master_pan = 0
local enabled = true
local loops = {}          -- file -> buffer, for OnPluginPlaySound's loop= and stop=
local volumes = {}        -- buffer -> volume
local pans = {}           -- buffer -> pan
local generation = 0      -- what delayed things check, so that stop means stop

local function num (x, default)
  return tonumber (x) or default
end

local function db (volume)
  if volume < 0 then volume = 0 elseif volume > 100 then volume = 100 end
  return volume * master_volume / 100 - 100
end

local function exists (name)
  local f = io.open (name, "rb")
  if f then
    f:close ()
    return true
  end
  return false
end

-- Ogg has no decoder in the client.  Something of the same name that plays, if any.
local function playable (file)
  local stem = file:match ("^(.*)%.[Oo][Gg][Gg]$")
  if not stem then
    return file
  end
  for _, extension in ipairs { ".wav", ".mp3", ".flac" } do
    if exists (stem .. extension) or exists (GetInfo (74) .. stem .. extension) then
      return stem .. extension
    end
  end
  return file
end

local function free_buffer ()
  for buffer = 10, 1, -1 do
    if GetSoundStatus (buffer) <= 0 then
      return buffer
    end
  end
  return 1
end

function play (file, loop, pan, volume)
  if not enabled then
    return 0
  end
  local buffer = free_buffer ()
  pan = num (pan, master_pan)
  volume = num (volume, 100)
  local looped = loop ~= nil and loop ~= false and loop ~= 0 and loop ~= "0"
  if PlaySound (buffer, playable (tostring (file)), looped, db (volume), pan) ~= 0 then
    return 0
  end
  volumes [buffer] = volume
  pans [buffer] = pan
  return buffer
end

function playlooped (file)
  return play (file, true)
end

function playdelay (file, ms, pan, volume)
  _G._luaaudio_delayed = _G._luaaudio_delayed or {}
  local key = #_G._luaaudio_delayed + 1
  _G._luaaudio_delayed [key] = { file = file, pan = pan, volume = volume, generation = generation }
  DoAfterSpecial (math.max (num (ms, 0) / 1000, 0.1), "_luaaudio_fire (" .. key .. ")", sendto.script)
end

function playdelaylooped (file, ms, pan, volume)
  _G._luaaudio_delayed = _G._luaaudio_delayed or {}
  local key = #_G._luaaudio_delayed + 1
  _G._luaaudio_delayed [key] = { file = file, pan = pan, volume = volume, loop = true, generation = generation }
  DoAfterSpecial (math.max (num (ms, 250) / 1000, 0.1), "_luaaudio_fire (" .. key .. ")", sendto.script)
end

function _luaaudio_fire (key)
  local t = _G._luaaudio_delayed [key]
  _G._luaaudio_delayed [key] = nil
  if t and t.generation == generation then
    play (t.file, t.loop, t.pan, t.volume)
  end
end

function stop (buffer)
  buffer = num (buffer, 0)
  if buffer == 0 then
    generation = generation + 1
    loops = {}
  end
  StopSound (buffer)
end

function isplaying (buffer)
  return GetSoundStatus (num (buffer, 0)) > 0
end

function getvolume (buffer)
  return volumes [num (buffer, 0)] or master_volume
end

local function apply (buffer)
  local looping = GetSoundStatus (buffer) == 2
  PlaySound (buffer, "", looping, db (volumes [buffer] or 100), pans [buffer] or 0)
end

local function each_buffer (buffer, f)
  buffer = num (buffer, 0)
  if buffer ~= 0 then
    f (buffer)
    return
  end
  for b = 1, 10 do
    if GetSoundStatus (b) > 0 then
      f (b)
    end
  end
end

function setvol (volume, buffer)
  volume = num (volume, 100)
  if num (buffer, 0) == 0 then
    master_volume = math.max (0, math.min (100, volume))
  else
    volumes [num (buffer, 0)] = volume
  end
  each_buffer (buffer, apply)
end

function setpan (pan, buffer)
  pan = num (pan, 0)
  if num (buffer, 0) == 0 then
    master_pan = pan
  end
  each_buffer (buffer, function (b) pans [b] = pan; apply (b) end)
end

function setpitch (pitch, buffer)
end
function slidepitch (pitch, buffer, ms)
end
function freq (value)
end

-- Five steps is enough for the ear, and timers are what there is.
local function slide (buffer, ms, get, set, target, after)
  ms = num (ms, 1000)
  local b = num (buffer, 0)
  local from = get (b)
  _G._luaaudio_slides = _G._luaaudio_slides or {}
  local key = #_G._luaaudio_slides + 1
  _G._luaaudio_slides [key] = { b = b, from = from, to = num (target, 0), set = set, step = 0, after = after,
                                 generation = generation, each = math.max (ms / 5000, 0.1) }
  _luaaudio_slide_step (key)
end

function _luaaudio_slide_step (key)
  local s = _G._luaaudio_slides [key]
  if not s or s.generation ~= generation then
    return
  end
  s.step = s.step + 1
  s.set (s.from + (s.to - s.from) * s.step / 5, s.b)
  if s.step >= 5 then
    _G._luaaudio_slides [key] = nil
    if s.after then s.after (s.b) end
    return
  end
  DoAfterSpecial (s.each, "_luaaudio_slide_step (" .. key .. ")", sendto.script)
end

function slidevol (volume, buffer, ms)
  slide (buffer, ms, getvolume, setvol, volume)
end

function slidepan (pan, buffer, ms)
  slide (buffer, ms, function (b) return pans [b] or 0 end, setpan, pan)
end

function fadeout (buffer, ms)
  slide (buffer, ms, getvolume, setvol, 0, stop)
end

function init ()
  return true
end

function free ()
  stop (0)
end

unload = free

if have_ppi then
  for _, name in ipairs { "init", "unload", "play", "stop", "fadeout" } do
    ppi.Expose (name, _G [name])
  end
  ppi.Expose ("isPlaying", isplaying)
  ppi.Expose ("getVolume", getvolume)
  ppi.Expose ("playLooped", playlooped)
  ppi.Expose ("playDelay", playdelay)
  ppi.Expose ("playDelayLooped", playdelaylooped)
  ppi.Expose ("setVol", setvol)
  ppi.Expose ("setPan", setpan)
  ppi.Expose ("setPitch", setpitch)
  ppi.Expose ("slideVol", slidevol)
  ppi.Expose ("slidePan", slidepan)
  ppi.Expose ("slidePitch", slidepitch)
end
-- The names CallPlugin uses are whatever case the caller wrote.
volume = setvol
pan = setpan
pitch = setpitch

function OnPluginPlaySound (sound)
  sound = tostring (sound or "")
  if sound == "" then
    stop (0)
    return
  end
  local key, value = sound:match ("^%s*(%a+)%s*=%s*(.-)%s*$")
  key = key and key:lower ()
  if key == "pan" then
    setpan (value, 0)
  elseif key == "volume" then
    setvol (value, 0)
  elseif key == "freq" then
    freq (value)
  elseif key == "loop" then
    local buffer = play (value, true)
    if buffer ~= 0 then
      loops [value] = buffer
    end
  elseif key == "stop" then
    if value:lower () == "all" then
      for _, buffer in pairs (loops) do
        StopSound (buffer)
      end
      loops = {}
    elseif loops [value] then
      StopSound (loops [value])
      loops [value] = nil
    end
  elseif not key then
    play (sound)
  end
  return true
end

local saved_volume = 100

function cmd_toggle ()
  enabled = not enabled
  if enabled then
    setvol (saved_volume, 0)
    Note ("Sounds on.")
  else
    saved_volume = master_volume
    setvol (0, 0)
    Note ("Sounds off.")
  end
end

function cmd_volume_up ()
  setvol (master_volume + 5, 0)
  Note ("Volume " .. master_volume)
end

function cmd_volume_down ()
  setvol (master_volume - 5, 0)
  Note ("Volume " .. master_volume)
end

function cmd_vol ()
  Note ("Volume " .. master_volume .. (enabled and "" or ", sounds off"))
end
]]></script>
</muclient>
)SHIM";

// The value of the id attribute of the <plugin> tag, if there is one.
std::string PluginIdOf (const char * sXml)
  {
  // Fast out: nearly every XML file through here is a world or an ordinary plugin.
  if (!strstr (sXml, kA11yMushReaderID) && !strstr (sXml, kA11yLuaAudioID))
    return "";

  const char * pTag = sXml;
  for (;;)
    {
    pTag = strstr (pTag, "<plugin");
    if (!pTag)
      return "";
    if (isspace ((unsigned char) pTag [7]))
      break;
    pTag += 7;
    }

  const char * pEnd = strchr (pTag, '>');
  if (!pEnd)
    return "";

  // id, on its own: not part of a longer attribute name
  for (const char * p = pTag + 7; p + 2 < pEnd; p++)
    if (p [0] == 'i' && p [1] == 'd' && isspace ((unsigned char) p [-1]))
      {
      const char * q = p + 2;
      while (q < pEnd && isspace ((unsigned char) *q))
        q++;
      if (*q != '=')
        continue;
      q++;
      while (q < pEnd && isspace ((unsigned char) *q))
        q++;
      if (*q != '"' && *q != '\'')
        continue;
      const char cQuote = *q++;
      const char * pClose = strchr (q, cQuote);
      if (!pClose || pClose > pEnd)
        return "";
      return std::string (q, pClose - q);
      }
  return "";
  }

}  // namespace

bool A11y_ShadowPlugin (const char * sXml, std::string & strReplacement)
  {
  const std::string id = PluginIdOf (sXml);
  if (id == kA11yMushReaderID)
    strReplacement = kMushReaderShim;
  else if (id == kA11yLuaAudioID)
    strReplacement = kLuaAudioShim;
  else
    return false;
  return true;
  }

// What require "luacom" gets where there is no COM: enough of SAPI.SpVoice for a plugin
// such as the bundled Text_To_Speech.  On Windows it is the real luacom, and real SAPI.
const char * const kA11yLuacomShim = R"SHIM(
local M = {}

local function voices ()
  local list = {}
  for name in tostring (SpeechInfo (12) or ""):gmatch ("[^|]+") do
    list [#list + 1] = name
  end
  return list
end

local function token (name)
  return { ID = name, GetDescription = function () return name end }
end

local function voices_object ()
  local list = voices ()
  return { Count = #list, Item = function (self, i) return token (list [i + 1] or "") end }
end

local SpVoice = {}
SpVoice.__index = SpVoice

function SpVoice:Speak (text, flags)
  flags = tonumber (flags) or 0
  -- bit 2 is SVSFPurgeBeforeSpeak; bit 1 is async, and everything here is
  local purge = math.floor (flags / 2) % 2 == 1
  Speak (tostring (text or ""), "sapi", 1, purge)
  return 0
end
function SpVoice:Skip () StopSpeech () return 0 end
function SpVoice:GetVoices () return voices_object () end
function SpVoice:setVoice (voice) self.Voice = voice end
function SpVoice:Pause () end
function SpVoice:Resume () end

M.CreateObject = function (name)
  if tostring (name):lower () ~= "sapi.spvoice" then
    return nil
  end
  local list = voices ()
  return setmetatable ({ Rate = 0, Volume = 100, Voice = token (list [1] or "") }, SpVoice)
end

M.GetEnumerator = function (collection)
  local i = -1
  return { Next = function ()
    i = i + 1
    if i < (collection.Count or 0) then
      return collection:Item (i)
    end
  end }
end

M.GetTypeInfo = function ()
  return { GetTypeLib = function ()
    return { ExportEnumerations = function ()
      return { SpeechVoiceSpeakFlags = { SVSFDefault = 0, SVSFlagsAsync = 1, SVSFPurgeBeforeSpeak = 2,
                                         SVSFIsFilename = 4, SVSFIsXML = 8, SVSFIsNotXML = 16 } }
    end }
  end }
end

luacom = M
return M
)SHIM";
