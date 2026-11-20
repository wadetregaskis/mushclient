-- Dump every trigger (world and plugins) with its options, one per line, tab separated.
local out = io.open (GetInfo (64) .. "triggers-dump.txt", "w")
local function hex (s) return (s:gsub (".", function (c) return string.format ("%02x", c:byte ()) end)) end
local function one (plugin, name, info, opt)
  local function b (v) return v and 1 or 0 end
  out:write (table.concat ({
    plugin or "world", name,
    b (info (name, 8)), b (info (name, 9)), b (info (name, 10)),
    tostring (opt (name, "multi_line")), tostring (opt (name, "lines_to_match")),
    tostring (info (name, 16)), b (info (name, 7)), b (info (name, 13)),
    tostring (info (name, 4)), tostring (info (name, 15)), tostring (info (name, 17)),
    hex (info (name, 1)) }, "\t"), "\n")
end
for _, name in ipairs (GetTriggerList () or {}) do
  one (nil, name, GetTriggerInfo, GetTriggerOption)
end
for _, id in ipairs (GetPluginList () or {}) do
  for _, name in ipairs (GetPluginTriggerList (id) or {}) do
    one (id, name,
         function (n, i) return GetPluginTriggerInfo (id, n, i) end,
         function (n, o) return GetPluginTriggerOption (id, n, o) end)
  end
end
out:close ()
