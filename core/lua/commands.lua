-- kmod console commands: a registry with built-in commands. Mods add their own with Console.register (see docs/console.md).
-- Entry point for the host: kmod.console_exec(line) -> true when the line was a command (otherwise the host runs it as Lua).
-- Re-running this file (dofile) is safe: it keeps registered commands.

local cmds = kmod._cmds or {}              -- name -> { fn, help, usage, owner }
kmod._cmds = cmds

local NAME_PAT = "^[a-z][a-z0-9_%-%.]*$"

-- split a command line into tokens; "double" and 'single' quotes group words, backslash escapes inside quotes
local function tokenize(line)
  local out, i, n = {}, 1, #line
  while i <= n do
    local c = line:sub(i, i)
    if c:match("%s") then i = i + 1
    elseif c == '"' or c == "'" then
      local q, buf = c, {}
      i = i + 1
      while i <= n and line:sub(i, i) ~= q do
        local ch = line:sub(i, i)
        if ch == "\\" and i < n then i = i + 1; ch = line:sub(i, i) end
        buf[#buf + 1] = ch
        i = i + 1
      end
      i = i + 1
      out[#out + 1] = table.concat(buf)
    else
      local j = i
      while j <= n and not line:sub(j, j):match("%s") do j = j + 1 end
      out[#out + 1] = line:sub(i, j - 1)
      i = j
    end
  end
  return out
end
kmod.console_tokenize = tokenize

local function register(name, fn, info, owner)
  if type(name) ~= "string" or not name:match(NAME_PAT) then return false, "invalid command name (use a-z 0-9 _ - .)" end
  if type(fn) ~= "function" then return false, "handler must be a function" end
  local old = cmds[name]
  if old and old.owner ~= owner then return false, "command '" .. name .. "' is already registered" .. (old.owner ~= "" and (" by " .. old.owner) or " (built-in)") end
  local help, usage = "", nil
  if type(info) == "string" then help = info
  elseif type(info) == "table" then help, usage = info.help or "", info.usage end
  cmds[name] = { fn = fn, help = help, usage = usage, owner = owner }
  return true
end

local function run(name, args, line)
  local c = cmds[name]
  local ctx = { line = line, print = print }
  local ok, res = pcall(c.fn, args, ctx)
  if not ok then
    print("[error] " .. name .. ": " .. tostring(res):match("^[^\n]*"))
    kmod.log("warn", c.owner ~= "" and c.owner or "console", "command '" .. name .. "' failed: " .. tostring(res))
  elseif res ~= nil then
    print(tostring(res))
  end
end

-- explicit: the line started with "/" (always a command); otherwise only a registered first word that is not followed by
-- Lua syntax ("mods = 1", "reload(...)") counts as a command.
function kmod.console_exec(line)
  line = tostring(line or ""):match("^%s*(.-)%s*$")
  local explicit = line:sub(1, 1) == "/"
  if explicit then line = line:sub(2) end
  local name = line:match("^(%S+)")
  if not name then return explicit end
  if not cmds[name] then
    if explicit then print("unknown command '" .. name .. "' (try: help)") end
    return explicit
  end
  local rest = line:sub(#name + 1)
  if not explicit and rest:match("^%s*[=%.%(:%[]") then return false end
  local args = tokenize(rest)
  run(name, args, line)
  return true
end

-- API handed to each mod as `Console`; commands are dropped when the mods reload
function kmod.console_api(rec)
  return {
    register = function(name, fn, info)
      local ok, err = register(name, fn, info, rec.id)
      if not ok then kmod.log("warn", rec.id, "Console.register('" .. tostring(name) .. "'): " .. tostring(err)) end
      return ok, err
    end,
    unregister = function(name)
      local c = cmds[name]
      if c and c.owner == rec.id then cmds[name] = nil; return true end
      return false
    end,
    exec = function(line) return kmod.console_exec(line) end,
    print = function(...) print(...) end,
  }
end

function kmod.console_clear_mods()
  for name, c in pairs(cmds) do if c.owner ~= "" then cmds[name] = nil end end
end

local function builtin(name, fn, help, usage) register(name, fn, { help = help, usage = usage }, "") end

local function findMod(id)
  for _, rec in ipairs(kmod.mods or {}) do if rec.id == id then return rec end end
end

-- ------------------------------------------------------------------ built-in commands
builtin("help", function(args)
  if args[1] then
    local c = cmds[args[1]]
    if not c then return "unknown command '" .. args[1] .. "'" end
    local s = args[1] .. (c.owner ~= "" and ("  [" .. c.owner .. "]") or "") .. "\n  " .. (c.help ~= "" and c.help or "(no description)")
    if c.usage then s = s .. "\n  usage: " .. c.usage end
    return s
  end
  local names = {}
  for name in pairs(cmds) do names[#names + 1] = name end
  table.sort(names, function(a, b)
    local oa, ob = cmds[a].owner ~= "", cmds[b].owner ~= ""
    if oa ~= ob then return not oa end
    return a < b
  end)
  local lines = { "Commands (anything else is run as Lua; prefix a line with / to force a command):" }
  for _, name in ipairs(names) do
    local c = cmds[name]
    lines[#lines + 1] = string.format("  %-14s %s%s", name, c.help, c.owner ~= "" and ("  [" .. c.owner .. "]") or "")
  end
  return table.concat(lines, "\n")
end, "list commands, or describe one", "help [command]")

builtin("version", function()
  return string.format("kmod %s (%s), api %d, game %s", kmod.version, kmod.flavor, kmod.api, tostring(kmod.game_version or kmod.game or "unknown"))
end, "show loader and game versions")

builtin("mods", function()
  local list = kmod.refresh_mods()
  if #list == 0 then return "no mods installed" end
  local lines = {}
  for _, rec in ipairs(list) do
    lines[#lines + 1] = string.format("  %-20s %-10s %s", rec.id, rec.version ~= "" and rec.version or "-", rec.status or "")
  end
  return table.concat(lines, "\n")
end, "list installed mods and their status")

local function setEnabled(id, on)
  local rec = id and findMod(id)
  if not rec then return "unknown mod '" .. tostring(id) .. "' (see: mods)" end
  kmod.set_enabled(id, on)
  kmod.reload_mods()
  return id .. (on and " enabled" or " disabled")
end
builtin("enable", function(args) return setEnabled(args[1], true) end, "enable a mod and reload", "enable <modid>")
builtin("disable", function(args) return setEnabled(args[1], false) end, "disable a mod and reload", "disable <modid>")
builtin("reload", function() kmod.reload_mods(); return "mods reloaded" end, "reload all mods")

builtin("lang", function(args)
  if not args[1] then return "language: " .. kmod.lang_current() .. " (setting: " .. kmod.state.language .. ")" end
  local tag = args[1]
  local ok = tag == "auto"
  for _, t in ipairs(kmod.language_tags) do if t == tag then ok = true end end
  if not ok then return "unknown language '" .. tag .. "' (auto, " .. table.concat(kmod.language_tags, ", ") .. ")" end
  kmod.set_language(tag)
  return "language set to " .. tag
end, "show or set the mod UI language", "lang [auto|en|zh-CN|...]")

builtin("settings", function(args)
  local rec = args[1] and findMod(args[1])
  if not rec then return "unknown mod '" .. tostring(args[1]) .. "' (see: mods)" end
  if not args[2] then
    local lines = {}
    for _, s in ipairs(rec.settings) do
      lines[#lines + 1] = string.format("  %-16s %-8s %s", s.id, s.type, tostring(kmod.settings_get(rec, s.id)))
    end
    return #lines > 0 and table.concat(lines, "\n") or (rec.id .. " declares no settings")
  end
  local s = rec.settingById and rec.settingById[args[2]]
  if not s then return "no setting '" .. args[2] .. "' in " .. rec.id end
  if args[3] == nil then return tostring(kmod.settings_get(rec, s.id)) end
  local raw, v = args[3], nil
  if s.type == "bool" then
    if raw == "true" or raw == "1" or raw == "on" then v = true elseif raw == "false" or raw == "0" or raw == "off" then v = false end
    if v == nil then return "expected true/false" end
  elseif s.type == "number" then
    v = tonumber(raw)
    if not v then return "expected a number" end
    if s.min and v < s.min then v = s.min end
    if s.max and v > s.max then v = s.max end
  elseif s.type == "choice" then
    for _, o in ipairs(s.options or {}) do if o == raw then v = o end end
    if v == nil then return "expected one of: " .. table.concat(s.options or {}, ", ") end
  else
    v = raw
    if s.maxlen and #v > s.maxlen then v = v:sub(1, s.maxlen) end
  end
  kmod.settings_set(rec, s.id, v)
  return rec.id .. "." .. s.id .. " = " .. tostring(v)
end, "list, read or change a mod's settings", "settings <modid> [key [value]]")

builtin("log", function(args)
  local n = math.max(1, math.min(tonumber(args[1]) or 20, 200))
  local f = io.open(kmod.paths.logs .. "/kmod.log", "rb")
  if not f then return "no log file" end
  local size = f:seek("end")
  f:seek("set", math.max(0, size - 65536))
  local data = f:read("a") or ""
  f:close()
  local lines = {}
  for l in data:gmatch("[^\r\n]+") do lines[#lines + 1] = l end
  local from = math.max(1, #lines - n + 1)
  return table.concat(lines, "\n", from, #lines)
end, "show the last lines of kmod.log", "log [count]")

builtin("find", function(args)
  if not args[1] then return "usage: find <class> [limit]" end
  local list = FindAllOf(args[1]) or {}
  local limit = tonumber(args[2]) or 20
  local lines = { #list .. " object(s)" }
  for i = 1, math.min(#list, limit) do lines[#lines + 1] = "  " .. list[i]:GetPathName() end
  if #list > limit then lines[#lines + 1] = "  ... (" .. (#list - limit) .. " more)" end
  return table.concat(lines, "\n")
end, "list live objects of a class", "find <class> [limit]")

local function findClass(name)
  local c = FindObject("Class", name)
  if not c:IsValid() then return nil end
  return c
end

builtin("props", function(args)
  if not args[1] then return "usage: props <class>" end
  local c = findClass(args[1])
  if not c then return "class '" .. args[1] .. "' not found (use the name without U/A prefix; try: search " .. args[1] .. ")" end
  local lines = {}
  for _, v in pairs(c:GetProperties()) do
    lines[#lines + 1] = "  " .. tostring(v.name) .. " : " .. tostring(v.type) .. "  (" .. tostring(v.owner) .. ")"
  end
  table.sort(lines)
  return #lines .. " props\n" .. table.concat(lines, "\n")
end, "list the properties of a class", "props <class>")

builtin("funcs", function(args)
  if not args[1] then return "usage: funcs <class> [filter]" end
  local c = findClass(args[1])
  if not c then return "class '" .. args[1] .. "' not found (use the name without U/A prefix; try: search " .. args[1] .. ")" end
  local filter = args[2] and args[2]:lower()
  local lines = {}
  for _, f in ipairs(kmod.signatures(c)) do
    if not filter or f.name:lower():find(filter, 1, true) then
      lines[#lines + 1] = "  " .. f.owner .. "." .. f.name .. f.sig
    end
  end
  table.sort(lines)
  return #lines .. " funcs\n" .. table.concat(lines, "\n")
end, "list a class's functions with their parameters", "funcs <class> [filter]")

-- search <words...> [class|object|function|all] [limit]
builtin("search", function(args)
  local kinds = { class = "class", c = "class", object = "object", o = "object", ["function"] = "function", func = "function", f = "function", all = "all", a = "all" }
  local limit, kind = 20, "all"
  local words = {}
  for _, a in ipairs(args) do
    if kinds[a:lower()] and #words > 0 then kind = kinds[a:lower()]
    elseif a:match("^%d+$") and #words > 0 then limit = tonumber(a)
    else words[#words + 1] = a end
  end
  if #words == 0 then return "usage: search <words...> [class|object|function|all] [limit]" end
  local pattern = table.concat(words, " ")
  local out = {}
  local function section(k, title)
    local res, total = kmod.search(pattern, k, limit)
    table.sort(res, function(x, y) if #x.path ~= #y.path then return #x.path < #y.path end return x.path < y.path end)
    out[#out + 1] = title .. " (" .. total .. (total > #res and (", showing " .. #res) or "") .. ")"
    for _, r in ipairs(res) do out[#out + 1] = "  " .. r.path .. (k == "object" and ("  [" .. r.class .. "]") or "") end
  end
  if kind == "all" or kind == "class" then section("class", "classes") end
  if kind == "all" or kind == "object" then section("object", "objects") end
  if kind == "all" or kind == "function" then section("function", "functions") end
  return table.concat(out, "\n")
end, "search classes, objects and functions by path (all words must match, any case)", "search <words...> [class|object|function|all] [limit]")

-- ------------------------------------------------------------------ hook: print the arguments of every call
local hooks = kmod._cmd_hooks or {}          -- path -> { id, calls, max, params }
kmod._cmd_hooks = hooks
local DEFAULT_CALLS = 20

local function fmt(v, depth)
  depth = depth or 0
  local t = type(v)
  if t == "string" then
    local s = #v > 60 and (v:sub(1, 60) .. "...") or v
    return '"' .. s .. '"'
  elseif t == "number" or t == "boolean" or t == "nil" then return tostring(v)
  elseif t == "userdata" then
    local ok, s = pcall(function() if v:IsValid() then return v:GetPathName() end return "invalid" end)
    if ok then return "<" .. s .. ">" end
    return tostring(v)
  elseif t == "table" then
    if depth >= 1 then return "{...}" end
    local parts, n = {}, 0
    for k, x in pairs(v) do
      n = n + 1
      if n > 4 then parts[#parts + 1] = "..."; break end
      parts[#parts + 1] = tostring(k) .. "=" .. fmt(x, depth + 1)
    end
    return "{" .. table.concat(parts, ", ") .. "}"
  end
  return tostring(v)
end

local function selfName(self)
  local ok, s = pcall(function() return self:GetPathName() end)
  return ok and s or "?"
end

local function describe(params, p, outputs)
  local parts = {}
  for _, pi in ipairs(params) do
    local isOut = pi.out or pi.ret
    if isOut == outputs then
      local ok, v = pcall(function() return p[pi.name] end)
      parts[#parts + 1] = pi.name .. "=" .. (ok and fmt(v) or "?")
    end
  end
  return table.concat(parts, ", ")
end

local function unhook(path)
  local h = hooks[path]
  if not h then return false end
  UnregisterHook(h.id)
  hooks[path] = nil
  return true
end

local function SigOfParams(params)
  local parts = {}
  for _, pi in ipairs(params) do
    if not pi.ret then parts[#parts + 1] = (pi.out and "out " or "") .. pi.name .. ": " .. pi.type end
  end
  return "(" .. table.concat(parts, ", ") .. ")"
end

builtin("hook", function(args)
  local path = args[1]
  if not path then return "usage: hook <function path> [max calls (default " .. DEFAULT_CALLS .. ", 0 = unlimited)]" end
  if hooks[path] then return "already hooked (unhook " .. path .. " first)" end
  local max = args[2] and tonumber(args[2]) or DEFAULT_CALLS
  local params = kmod.func_params(path)
  if not params then return "function not found: " .. path .. " (try: search <word> function)" end
  local hasOut = false
  for _, pi in ipairs(params) do if pi.out or pi.ret then hasOut = true end end
  local h = { calls = 0, max = max, params = params }
  local function pre(self, p)
    h.calls = h.calls + 1
    print(string.format("[hook %d] %s  self=%s  (%s)", h.calls, path, selfName(self), describe(params, p, false)))
    if max > 0 and h.calls >= max and not hasOut then
      unhook(path); print("[hook] " .. path .. ": reached " .. max .. " calls, removed")
    end
  end
  local function post(self, p)
    if hasOut then
      print(string.format("[hook %d]   -> %s", h.calls, describe(params, p, true)))
      if max > 0 and h.calls >= max and hooks[path] then
        unhook(path); print("[hook] " .. path .. ": reached " .. max .. " calls, removed")
      end
    end
  end
  local ok, id = pcall(RegisterHook, path, pre, post)
  if not ok then return "cannot hook " .. path .. ": " .. tostring(id):match("^[^\n]*") end
  h.id = id
  hooks[path] = h
  return "hooked " .. path .. " " .. SigOfParams(params) .. (max > 0 and (" for " .. max .. " calls") or " (unlimited)")
end, "print the arguments of every call to a function", "hook <function path> [max calls]")

builtin("unhook", function(args)
  if args[1] == "all" or args[1] == nil then
    local n = 0
    for path in pairs(hooks) do unhook(path); n = n + 1 end
    return n .. " hook(s) removed"
  end
  return unhook(args[1]) and ("removed " .. args[1]) or ("not hooked: " .. args[1])
end, "remove one console hook, or all", "unhook [function path|all]")

builtin("hooks", function()
  local lines = {}
  for path, h in pairs(hooks) do lines[#lines + 1] = string.format("  %-60s %d call(s)%s", path, h.calls, h.max > 0 and (" / " .. h.max) or "") end
  table.sort(lines)
  return #lines > 0 and table.concat(lines, "\n") or "no console hooks"
end, "list active console hooks")
