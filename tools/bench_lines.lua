-- tools/bench_lines.lua -- how fast the trigger path takes server output, with
-- whatever plugins are installed (Quow's, for docs/PERFORMANCE-2026-11.md).
-- Run it in the client from Game > Immediate (the world may be disconnected):
--   bench_n = 20000; dofile [[<repository>/tools/bench_lines.lua]]
-- Each test Simulates bench_n lines, as if from the server.  Results go to the
-- output window and to bench-results.txt in the data folder, as tools/bench.lua's do.
--
--   plain             one sentence that no trigger matches
--   Discworld mix     tools/trigger_bench/discworld_lines.txt in turn: room
--                     descriptions, chat, tells, combat; a few of them match
--                     a plugin trigger, as in a real session
--   matching          tools/trigger_bench/discworld_matching.txt: lines that
--                     each fire a plugin trigger and its Lua callback
--
-- The figure to compare is lines a second; "fired" is how many triggers matched.

local timer = utils.timer
local here = debug.getinfo (1, "S").source:match ("^@(.*[\\/])") or ""

local function report (fmt, ...)
  local s = string.format (fmt, ...)
  ColourNote ("yellow", "", s)
  local f = io.open (GetInfo (64) .. "bench-results.txt", "a")
  if f then f:write (s, "\n"); f:close () end
end

local function read_lines (name)
  local t = {}
  for l in io.lines (here .. "trigger_bench/" .. name) do
    if l ~= "" then table.insert (t, l) end
  end
  return t
end

local N = tonumber (bench_n) or 20000
local mix = read_lines ("discworld_lines.txt")
local matching = read_lines ("discworld_matching.txt")

SetOption ("enable_triggers", 1)
report ("MUSHclient %s, %s, N = %d, plugins %d", Version (), GetInfo (63) or "?", N, #(GetPluginList () or {}))

local function bench (name, n, lines)
  collectgarbage ()
  local fired = GetInfo (245)
  local count = #lines
  local s = timer ()
  for i = 1, n do
    Simulate (lines [(i - 1) % count + 1] .. "\r\n")
  end
  local e = timer () - s
  report ("%-22s %7d lines  %8.3f s  %9.0f lines/s  %7d fired", name, n, e, n / e, GetInfo (245) - fired)
end

bench ("plain", N, { "The quick brown fox jumps over the lazy dog, again and again." })
bench ("Discworld mix", N, mix)
bench ("matching", N, matching)
