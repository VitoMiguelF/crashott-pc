-- Lists what the game's Lua state offers (globals, the C++ classes and their methods, enums).
-- Run inside the game with CRASH_DEV_LUA_FILE=tools/dev_lua/dump_globals.lua (or the dev_api
-- profile); each line goes to the log as "[lua] API <path> <type>".
-- Only the base, string, table and math libraries exist in the retail game (no debug/io/os).

local seen = {}
local count = 0
local print = devprint

local function sortedKeys(t)
  local keys = {}
  for k in pairs(t) do table.insert(keys, k) end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  return keys
end

local function describe(v)
  local kind = type(v)
  if kind == "number" or kind == "boolean" or kind == "string" then
    return kind .. " = " .. tostring(v)
  end
  return kind
end

local function walk(path, value, depth)
  local kind = type(value)
  print("API " .. path .. " " .. describe(value))
  count = count + 1
  if (kind ~= "table" and kind ~= "userdata") or seen[value] or depth > 3 then return end
  seen[value] = true
  if kind == "table" then
    for _, k in ipairs(sortedKeys(value)) do
      walk(path .. "." .. tostring(k), rawget(value, k), depth + 1)
    end
  end
  local meta = getmetatable(value)
  if type(meta) == "table" and not seen[meta] then
    walk(path .. ".<meta>", meta, depth + 1)
  end
end

for _, k in ipairs(sortedKeys(_G)) do
  if k ~= "_G" and k ~= "string" and k ~= "table" and k ~= "math" and k ~= "coroutine" then
    walk(tostring(k), rawget(_G, k), 0)
  end
end
print("API done: " .. count .. " entries")
