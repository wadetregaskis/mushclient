-- tools/bench.lua -- MUSHclient throughput benchmarks, run in the client from
-- Game > Immediate (the world may be disconnected):
--   bench_n = 10000; bench_rep = 100; dofile [[<repository>/tools/bench.lua]]
-- Results go to the output window and to bench-results.txt in the data folder
-- (GetInfo (64)).  docs/BUGHUNT-2026-10.md has the numbers it first produced.

local timer = utils.timer
local results = {}

local function report (fmt, ...)
  local s = string.format (fmt, ...)
  table.insert (results, s)
  ColourNote ("yellow", "", s)
  local f = io.open (GetInfo (64) .. "bench-results.txt", "a")
  if f then f:write (s, "\n"); f:close () end
end

local function bench (name, n, f)
  collectgarbage ()
  local s = timer ()
  f (n)
  local e = timer () - s
  report ("%-34s %7d  %8.3f s  %9.0f /s", name, n, e, n / e)
  return e
end

local N = tonumber (bench_n) or 20000
local plain = "The quick brown fox jumps over the lazy dog, again and again: "
local ansi  = "\27[1;31mRed \27[32mgreen \27[0;34mblue \27[33myellow\27[0m and plain text "

-- start from a known state
DeleteTriggerGroup ("bench")
SetOption ("enable_triggers", 1)

report ("MUSHclient %s, %s, N = %d", Version (), GetInfo (63) or "?", N)

bench ("Simulate plain", N, function (n)
  for i = 1, n do Simulate (plain .. i .. "\r\n") end end)

bench ("Simulate ANSI colour", N, function (n)
  for i = 1, n do Simulate (ansi .. i .. "\r\n") end end)

-- 200 regular-expression triggers that never match
for i = 1, 200 do
  AddTriggerEx ("bench_nm" .. i, "^zzz" .. i .. " (.*) yyy$", "", 1 + 32, -1, 0, "", "", 0, 100)
  SetTriggerOption ("bench_nm" .. i, "group", "bench")
end
bench ("+200 non-matching regex triggers", N, function (n)
  for i = 1, n do Simulate (plain .. i .. "\r\n") end end)

-- 200 wildcard (non-regex) triggers that never match
for i = 1, 200 do
  AddTriggerEx ("bench_nw" .. i, "zzz" .. i .. " * yyy", "", 1, -1, 0, "", "", 0, 100)
  SetTriggerOption ("bench_nw" .. i, "group", "bench")
end
bench ("+200 non-matching wildcard trig", N, function (n)
  for i = 1, n do Simulate (plain .. i .. "\r\n") end end)

-- plus one matching trigger that calls a Lua function
bench_count = 0
function bench_fn (name, line, wildcards) bench_count = bench_count + 1 end
AddTriggerEx ("bench_m", "^The quick (\\w+) fox", "", 1 + 32 + 8, -1, 0, "", "bench_fn", 0, 50)
SetTriggerOption ("bench_m", "group", "bench")
bench ("+1 matching trigger -> Lua fn", N, function (n)
  for i = 1, n do Simulate (plain .. i .. "\r\n") end end)
report ("   (callback ran %d times)", bench_count)

-- a matching trigger that recolours the line
AddTriggerEx ("bench_c", "jumps", "", 1 + 8, 7, 0, "", "", 0, 60)
SetTriggerOption ("bench_c", "group", "bench")
bench ("+1 colouring trigger", N, function (n)
  for i = 1, n do Simulate (plain .. i .. "\r\n") end end)

DeleteTriggerGroup ("bench")

-- script output paths
bench ("Note", N, function (n)
  for i = 1, n do Note ("a note line ", i) end end)
bench ("ColourNote (3 runs)", N, function (n)
  for i = 1, n do ColourNote ("red", "", "red ", "lime", "", "green ", "white", "blue", tostring (i)) end end)

-- variables
bench ("SetVariable+GetVariable", N * 5, function (n)
  for i = 1, n do SetVariable ("bench_v", i); local x = GetVariable ("bench_v") end end)
DeleteVariable ("bench_v")

-- painting: a screenful of coloured text, repainted synchronously
local REP = tonumber (bench_rep) or 200
bench ("Repaint (full window)", REP, function (n)
  for i = 1, n do Repaint () end end)

-- scrolling back through the buffer and painting each step
local lines = GetLinesInBufferCount ()
bench ("scroll+repaint through buffer", REP, function (n)
  for i = 1, n do
    SetScroll (math.floor (lines * i / n) * 12, false)
    Repaint ()
  end
  SetScroll (-1, true)
  end)

-- miniwindow drawing
WindowCreate ("bench_w", 0, 0, 400, 300, 6, 0, ColourNameToRGB ("black"))
WindowFont ("bench_w", "f", "Courier New", 9)
WindowShow ("bench_w", true)
bench ("WindowText x40 + RectOp (a frame)", REP, function (n)
  for i = 1, n do
    WindowRectOp ("bench_w", 2, 0, 0, 0, 0, ColourNameToRGB ("black"))
    for j = 0, 39 do
      WindowText ("bench_w", "f", "Line " .. j .. " of some status text " .. i, 2, j * 7, 0, 0, 0xFFFFFF)
    end
    WindowRectOp ("bench_w", 1, 0, 0, 0, 0, 0xFFFFFF)
  end end)
bench ("... and Repaint each", REP, function (n)
  for i = 1, n do
    WindowRectOp ("bench_w", 2, 0, 0, 0, 0, ColourNameToRGB ("black"))
    for j = 0, 39 do
      WindowText ("bench_w", "f", "Line " .. j .. " of some status text " .. i, 2, j * 7, 0, 0, 0xFFFFFF)
    end
    Repaint ()
  end end)
WindowDelete ("bench_w")

report ("lines in buffer: %d", GetLinesInBufferCount ())

