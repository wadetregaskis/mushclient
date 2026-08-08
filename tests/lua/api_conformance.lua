--[[ tests/lua/api_conformance.lua — a deterministic, side-effect-light probe of
     the world API surface that the PAL refactor moved, written so the SAME
     script can be run against a stock MUSHclient and against this build and the
     two outputs diffed.

     Every probe prints one `key = value` line.  A line that differs between the
     two clients is either a known intentional delta (docs/UPSTREAM-DIVERGENCES.md,
     docs/WINDOWS-VALIDATION.md §6) or a regression — there is no third case,
     which is the point of writing it this way rather than as pass/fail.

     Covers, in order: the command bar and its history, the output-window
     selection getters, bookmarks, the scroll position, option validation,
     GetInfo's connection/window family, accelerators, printf-format handling,
     and trigger wildcard shape + case folding.

     Deliberately NOT covered because they need a human or a MUD: hotspot
     mouse callbacks, the spell checker, modal dialogs, and anything that
     depends on a live connection.
]]

local out = {}
local function p (k, v) out [#out + 1] = tostring (k) .. " = " .. tostring (v) end

-- A stable rendering for values that would otherwise print as table addresses
-- or with float noise.
local function fmt (v)
  if type (v) == "table" then
    local keys = {}
    for k in pairs (v) do keys [#keys + 1] = k end
    table.sort (keys, function (a, b) return tostring (a) < tostring (b) end)
    local parts = {}
    for _, k in ipairs (keys) do
      parts [#parts + 1] = tostring (k) .. "=" .. tostring (v [k])
    end
    return "{" .. table.concat (parts, ",") .. "}"
  end
  return tostring (v)
end

-- Call a world function that may not exist in an older client.
local function try (name, ...)
  local f = _G [name]
  if type (f) ~= "function" then return "<absent>" end
  local ok, a, b, c, d = pcall (f, ...)
  if not ok then return "<error: " .. tostring (a) .. ">" end
  if d ~= nil then return fmt (a) .. "," .. fmt (b) .. "," .. fmt (c) .. "," .. fmt (d) end
  if c ~= nil then return fmt (a) .. "," .. fmt (b) .. "," .. fmt (c) end
  if b ~= nil then return fmt (a) .. "," .. fmt (b) end
  return fmt (a)
end

-- An older client will not have every method this probes.  Stub the missing
-- ones to a no-op returning nil so the run continues and the absence shows up
-- in the diff as a value, rather than aborting the whole script.
for _, n in ipairs {"SetSelection", "GetMainWindowPosition", "SetMainTitle",
                    "SetTitle", "AcceleratorList", "Simulate", "PushCommand",
                    "DeleteCommandHistory", "SetCommand", "GetCommandList",
                    "GetLineInfo", "GetLinesInBufferCount", "DeleteTrigger",
                    "TraceOut", "Note", "GetInfo", "GetOption", "SetOption"} do
  if type (_G [n]) ~= "function" then
    _G [n] = function () return nil end
  end
end

p ("version", GetInfo (72))

-- ── command bar + history (§4c "Command bar") ──────────────────────────────
DeleteCommandHistory ()
p ("cmd.empty.get",            try ("GetCommand"))
p ("cmd.set.when.empty",       try ("SetCommand", "hello there"))
p ("cmd.get.after.set",        try ("GetCommand"))
p ("cmd.set.when.not.empty",   try ("SetCommand", "second attempt"))
p ("cmd.get.unchanged",        try ("GetCommand"))
p ("cmd.selectcommand",        try ("SelectCommand"))
p ("cmd.sel.start.all",        try ("GetInfo", 236))
p ("cmd.sel.end.all",          try ("GetInfo", 237))
p ("cmd.setselection.3.8",     try ("SetCommandSelection", 3, 8))
p ("cmd.sel.start.after",      try ("GetInfo", 236))
p ("cmd.sel.end.after",        try ("GetInfo", 237))
p ("cmd.setselection.past.end",try ("SetCommandSelection", 1, 9999))
p ("cmd.sel.start.clamped",    try ("GetInfo", 236))
p ("cmd.sel.end.clamped",      try ("GetInfo", 237))

-- PushCommand takes no arguments: it moves the command bar's CURRENT text into
-- the history and returns it.  That is the only script route into the history,
-- so it is how this populates one.  GetCommandList is documented newest-first.
DeleteCommandHistory ()
SetCommand ("")
p ("hist.empty",               fmt (GetCommandList (0)))
for _, c in ipairs {"first cmd", "second cmd", "third cmd"} do
  SetCommand ("")
  SetCommand (c)
  PushCommand ()
end
do
  local l = GetCommandList (0)
  p ("hist.count",             type (l) == "table" and #l or fmt (l))
  p ("hist.newest.first",      type (l) == "table" and tostring (l [1]) or "<nil>")
  p ("hist.oldest.last",       type (l) == "table" and tostring (l [#l]) or "<nil>")
  p ("hist.limit.2",           fmt (GetCommandList (2)))
end
p ("cmd.after.pushes",         try ("GetCommand"))
SetCommand ("")
DeleteCommandHistory ()
p ("hist.after.delete",        fmt (GetCommandList (0)))

-- ── output-window selection getters (§4c "Output", 4c-bis) ────────────────
-- The buffer already holds the startup banner, so there are lines to select.
p ("buf.lines",                try ("GetLinesInBufferCount"))
p ("sel.none.startline",       try ("GetSelectionStartLine"))
p ("sel.none.endline",         try ("GetSelectionEndLine"))
p ("sel.none.startcol",        try ("GetSelectionStartColumn"))
p ("sel.none.endcol",          try ("GetSelectionEndColumn"))
SetSelection (2, 2, 1, 6)
p ("sel.one.startline",        try ("GetSelectionStartLine"))
p ("sel.one.endline",          try ("GetSelectionEndLine"))
p ("sel.one.startcol",         try ("GetSelectionStartColumn"))
p ("sel.one.endcol",           try ("GetSelectionEndColumn"))
-- degenerate (start == end) must read back as "no selection"
SetSelection (2, 2, 4, 4)
p ("sel.degenerate.startline", try ("GetSelectionStartLine"))
p ("sel.degenerate.endcol",    try ("GetSelectionEndColumn"))
-- a column past the line's length is clamped to it
SetSelection (2, 2, 1, 9999)
p ("sel.clamped.endcol",       try ("GetSelectionEndColumn"))
SetSelection (0, 0, 0, 0)

-- ── bookmarks (§4c-vicies) ────────────────────────────────────────────────
p ("bookmark.before",          try ("GetLineInfo", 3, 7))
p ("bookmark.set",             try ("Bookmark", 3, 1))
p ("bookmark.after.set",       try ("GetLineInfo", 3, 7))
p ("bookmark.clear",           try ("Bookmark", 3, 0))
p ("bookmark.after.clear",     try ("GetLineInfo", 3, 7))
p ("bookmark.line.zero",       try ("Bookmark", 0, 1))
p ("bookmark.line.huge",       try ("Bookmark", 999999, 1))
p ("bookmark.untouched",       try ("GetLineInfo", 3, 7))

-- ── scroll position (§4c-duodecies: 296 and SetScroll share a unit) ───────
-- The buffer must be taller than the window or `highest` (lastline * fontheight
-- - windowheight) is NEGATIVE and every scroll clamps to it, which tells us
-- nothing about the unit.  Fill it first.
do
  local before = GetInfo (296)
  p ("scroll.before",          fmt (before))
  p ("scroll.setscroll.same",  try ("SetScroll", before, true))
  p ("scroll.after.roundtrip", fmt (GetInfo (296)))
  p ("scroll.visible.flag",    fmt (GetInfo (120)))

  for i = 1, 300 do Note ("scroll filler line " .. i) end
  p ("scroll.filled.lines",    try ("GetLinesInBufferCount"))
  SetScroll (-1, true)                      -- to the bottom
  local bottom = GetInfo (296)
  p ("scroll.bottom",          fmt (bottom))
  SetScroll (150, true)                     -- an explicit pixel Y
  p ("scroll.at.150",          fmt (GetInfo (296)))
  SetScroll (0, true)
  p ("scroll.at.0",            fmt (GetInfo (296)))
  -- the §4c-duodecies claim: reading 296 and feeding it back is a no-op
  SetScroll (bottom, true)
  p ("scroll.roundtrip.bottom",fmt (GetInfo (296)) .. " (wanted " .. fmt (bottom) .. ")")
  p ("scroll.nochange.-2",     try ("SetScroll", -2, true))
  p ("scroll.after.-2",        fmt (GetInfo (296)))
end

-- ── option validation (§4c-quatertricies, §4c-quinvicies) ────────────────
p ("opt.note_colour.get",      try ("GetOption", "note_text_colour"))
p ("opt.note_colour.set.3",    try ("SetOption", "note_text_colour", 3))
p ("opt.note_colour.after.3",  try ("GetOption", "note_text_colour"))
p ("opt.note_colour.set.300",  try ("SetOption", "note_text_colour", 300))
p ("opt.note_colour.after.300",try ("GetOption", "note_text_colour"))
p ("opt.echo_colour.set.16",   try ("SetOption", "echo_colour", 16))
p ("opt.hyperlink.set.white",  try ("SetOption", "hyperlink_colour", 0xFFFFFF))
p ("opt.maxlines.get",         try ("GetOption", "max_output_lines"))
p ("opt.maxlines.set.0",       try ("SetOption", "max_output_lines", 0))
p ("opt.maxlines.after.0",     try ("GetOption", "max_output_lines"))
p ("opt.maxlines.set.5",       try ("SetOption", "max_output_lines", 5))
p ("opt.maxlines.restore",     try ("SetOption", "max_output_lines", 10000))

-- ── GetInfo: connection + window family (§4c "Window management") ─────────
p ("info.106.connected",       fmt (GetInfo (106)))
p ("info.125.fullscreen",      fmt (GetInfo (125)))
p ("info.272.connect.phase",   fmt (GetInfo (272)))
p ("info.289.line.with.GA",    fmt (GetInfo (289)))
p ("mainwindowposition",       fmt (GetMainWindowPosition ()))
p ("win.activateclient",       try ("ActivateClient"))
p ("win.status.normal",        try ("SetWorldWindowStatus", 4))
p ("pause.on",                 try ("Pause", 1))
p ("pause.off",                try ("Pause", 0))

-- ── titles (§4c "Title") ──────────────────────────────────────────────────
p ("title.set",                try ("SetTitle", "conformance title"))
p ("title.mainset",            try ("SetMainTitle", "conformance main"))
p ("title.getinfo.89",         fmt (GetInfo (89)))
SetMainTitle ("")
SetTitle ("")

-- ── info bar (§4c "Info bar") ────────────────────────────────────────────
p ("infobar.show",             try ("ShowInfoBar", 1))
p ("infobar.text",             try ("Info", "conformance"))
p ("infobar.colour",           try ("InfoColour", "red"))
p ("infobar.background",       try ("InfoBackground", "yellow"))
p ("infobar.font",             try ("InfoFont", "Courier New", 12, 1))
p ("infobar.clear",            try ("InfoClear"))
p ("infobar.hide",             try ("ShowInfoBar", 0))

-- ── accelerators (§4c-quater) ────────────────────────────────────────────
-- AcceleratorList returns a 1-relative array of the bound key names; the SEND
-- text is not in it, so a bind shows up as a new entry.
p ("accel.bind",               try ("Accelerator", "Ctrl+F8", "hello"))
p ("accel.list.after.bind",    fmt (AcceleratorList ()))
p ("accel.rebind.empty",       try ("AcceleratorTo", "Ctrl+F8", "", 0))
p ("accel.list.after.unbind",  fmt (AcceleratorList ()))

-- ── printf-format handling (§4c-ter) ─────────────────────────────────────
-- A runtime string used as a printf FORMAT reads a vararg that was never
-- pushed, so a literal % in it prints garbage.  These must come out verbatim.
do
  if type (TraceOut) == "function" then pcall (TraceOut, "100% done: %s and %d") end
  Note ("literal percent: 100% done %s")
  -- Note terminates the line, so the text is the line BEFORE the new empty one.
  local n = GetLinesInBufferCount ()
  p ("printf.note.roundtrip",  fmt (GetLineInfo (n - 1, 1)))
end

-- ── trigger wildcards: shape and case folding (§4c-sexies) ───────────────
-- A regexp trigger with "convert wildcards to lower case" ticked plus a script
-- callback.  On Windows AND on the port, Lua reads the regexp captures RAW, so
-- the callback's wildcards keep their original case while GetTriggerInfo(101)
-- returns the folded one.  That asymmetry is upstream's, and is what this pins.
_G.conformance_wild = nil
function ConformanceTriggerCallback (name, line, wildcards)
  local parts = {}
  for i = 0, 12 do
    if wildcards [i] ~= nil then
      parts [#parts + 1] = i .. ":" .. tostring (wildcards [i])
    end
  end
  _G.conformance_wild = table.concat (parts, "|") .. "  #=" .. tostring (#wildcards)
end

try ("DeleteTrigger", "conformance_wild")
-- flags.h: eEnabled 0x01 | eTriggerRegularExpression 0x20 | eReplace 0x400
--          | eLowercaseWildcard 0x800
p ("trig.add", try ("AddTriggerEx", "conformance_wild",
                    "^GREET (\\w+) AND (\\w+)$", "",
                    0x01 + 0x20 + 0x400 + 0x800,
                    -1, 0, "", "ConformanceTriggerCallback", 12 --[[sendto script]], 100))
p ("trig.lowercase.flag", try ("GetTriggerInfo", "conformance_wild", 26))
Simulate ("GREET Alice AND Bob\n")
p ("trig.callback.wildcards",  fmt (_G.conformance_wild))
p ("trig.info.101",            try ("GetTriggerInfo", "conformance_wild", 101))
p ("trig.info.102",            try ("GetTriggerInfo", "conformance_wild", 102))
p ("trig.info.110",            try ("GetTriggerInfo", "conformance_wild", 110))
try ("DeleteTrigger", "conformance_wild")

-- ── clear output + the IAC GA line number (§4c-septemvicies) ─────────────
-- ClearOutput's script name is DeleteOutput.  m_last_line_with_IAC_GA is now
-- zeroed alongside m_total_lines (UPSTREAM-BUGS #11), so 289 must not report a
-- phantom line afterwards.
p ("info.289.before.clear",    fmt (GetInfo (289)))
p ("clear.output",             try ("DeleteOutput"))
p ("info.289.after.clear",     fmt (GetInfo (289)))
p ("buf.lines.after.clear",    try ("GetLinesInBufferCount"))

return table.concat (out, "\n")
