--[[

gmcp.lua -- GMCP for a Lua plugin, decoded into tables.

  require "gmcp"

  gmcp.subscribe ("Char.Vitals", function (data, package)
    Note ("HP is ", data.hp)
  end)

  gmcp.subscribe ("room.info", "1", show_room)   -- with a version, if the server wants one
  gmcp.send ("Char.Skills.Get", { group = "fighting" })
  print (gmcp.get ("Char.Vitals").hp)            -- the last one, whenever you ask

It wraps the world functions GMCP_Subscribe, GMCP_Send, GMCP_Get and the callback
OnPluginGMCP, which do not depend on it and which any language can use.  docs/PROTOCOLS.md
describes those.  This adds only the JSON, using lua/json.lua.

Packages match without regard to case, and a subscription to "Char" gets "Char.Vitals"
as well.  The name given to a handler is as the server spelled it.

Require it when the plugin loads, because that is when the client looks for OnPluginGMCP.
A plugin that already defines OnPluginGMCP has it called as well, after the handlers.
One that defines it after the require should call gmcp.dispatch (package, rawText) from it.

--]]

local json = require "json"

gmcp = gmcp or {}

local handlers = {}      -- lower-case package -> list of functions
local earlier = nil      -- the plugin's own OnPluginGMCP, if it had one

local function decode (text)
  if text == nil or text == "" then
    return nil
  end -- if nothing
  local ok, value = pcall (json.decode, text)
  if ok then
    return value
  end -- if good JSON
  return nil, value
end -- decode

-- Is a message for package something a subscription to name is meant to get?
local function covers (name, package)
  return package == name or package:sub (1, #name + 1) == name .. "."
end -- covers

local function dispatch (package, text)
  local lower = package:lower ()
  local data, decode_error
  local decoded = false
  for name, list in pairs (handlers) do
    if covers (name, lower) then
      if not decoded then
        data, decode_error = decode (text)
        decoded = true
        if decode_error then
          ColourNote ("white", "red", "GMCP " .. package .. " is not valid JSON: " .. tostring (decode_error))
        end -- if bad
      end -- if not yet decoded
      for _, handler in ipairs (list) do
        handler (data, package, text)
      end -- for each handler
    end -- if subscribed
  end -- for each subscription
  if earlier then
    earlier (package, text)
  end -- if the plugin has its own
end -- dispatch

-- Put ourselves where the client looks, once, keeping whatever was there.
local function install ()
  if _G.OnPluginGMCP ~= dispatch then
    earlier = _G.OnPluginGMCP
    _G.OnPluginGMCP = dispatch
  end -- if not yet
end -- install

gmcp.dispatch = dispatch
install ()

-- gmcp.subscribe (package, handler) or gmcp.subscribe (package, version, handler)
function gmcp.subscribe (package, version, handler)
  if type (version) == "function" then
    version, handler = nil, version
  end -- if no version
  assert (type (package) == "string", "gmcp.subscribe: the package must be a string")
  local result = GMCP_Subscribe (package, version and tostring (version) or "")
  if result == eOK and handler then
    local key = package:lower ()
    handlers [key] = handlers [key] or {}
    table.insert (handlers [key], handler)
  end -- if subscribed
  return result
end -- gmcp.subscribe

function gmcp.unsubscribe (package)
  handlers [package:lower ()] = nil
  return GMCP_Unsubscribe (package)
end -- gmcp.unsubscribe

-- gmcp.send (package [, table or JSON text])
function gmcp.send (package, data)
  local text = ""
  if type (data) == "string" then
    text = data
  elseif data ~= nil then
    text = json.encode (data)
  end -- if something to encode
  return GMCP_Send (package, text)
end -- gmcp.send

-- The last message for a package, decoded.  Nil if there has not been one.
function gmcp.get (package)
  local text = GMCP_Get (package)
  if text == nil then
    return nil
  end -- if none
  return (decode (text))
end -- gmcp.get

return gmcp
