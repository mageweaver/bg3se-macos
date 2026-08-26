--[[
  ConsoleBridge - external Lua eval channel for Norbyte's Windows BG3SE.

  Norbyte's extender has no IPC socket; its console reads stdin only. This mod
  gives an external program a clean request/response channel over two files in
  the SE user directory (Ext.IO root = %LOCALAPPDATA%\Larian Studios\
  Baldur's Gate 3\Script Extender\):

     bridge_req.json   written by the external client  {"id":N,"code":"..."}
     bridge_resp.json  written by this mod             {"id":N,"ok":bool,"output":"...","error":"..."|nil}

  The mod polls the request file a few times per second on Tick. When it sees an
  id greater than the last one it processed, it evaluates the code in SERVER
  context, capturing both Ext.Print output and the returned value(s), then writes
  the response. The client polls bridge_resp.json until resp.id == its request id.

  This is the Windows analogue of the macOS port's /tmp/bg3se.sock console.
]]--

local REQ  = "bridge_req.json"
local RESP = "bridge_resp.json"
local POLL_EVERY = 10          -- process at most once per this many ticks (~6/sec)

local lastId = 0
local tickCounter = 0

-- Evaluate `code` REPL-style: try it as an expression first (so a bare value is
-- returned), fall back to running it as statements. Capture Ext.Print output and
-- any returned values. Never throws.
local function evalCode(code)
    local buf = {}
    local origPrint = Ext.Print
    Ext.Print = function(...)
        local n = select("#", ...)
        local parts = {}
        for i = 1, n do parts[i] = tostring((select(i, ...))) end
        buf[#buf + 1] = table.concat(parts, " ")
    end

    local ok, err
    local f = load("return " .. code, "=bridge")
    if not f then f = load(code, "=bridge") end

    if not f then
        local _, compileErr = load(code, "=bridge")
        ok, err = false, "compile: " .. tostring(compileErr)
    else
        local r = { pcall(f) }
        ok = r[1]
        if ok then
            for i = 2, #r do buf[#buf + 1] = tostring(r[i]) end
        else
            err = "runtime: " .. tostring(r[2])
        end
    end

    Ext.Print = origPrint
    return ok, table.concat(buf, "\n"), err
end

local function writeResp(tbl)
    local okJson, encoded = pcall(Ext.Json.Stringify, tbl)
    if not okJson then
        encoded = '{"id":' .. tostring(tbl.id) .. ',"ok":false,"error":"json encode failed"}'
    end
    Ext.IO.SaveFile(RESP, encoded)
end

local function poll()
    local raw = Ext.IO.LoadFile(REQ)
    if not raw or raw == "" then return end

    local okParse, req = pcall(Ext.Json.Parse, raw)
    if not okParse or type(req) ~= "table" or type(req.id) ~= "number" then return end
    if req.id <= lastId then return end          -- already handled (or stale)
    lastId = req.id

    local code = tostring(req.code or "")
    local ok, output, err = evalCode(code)
    writeResp({ id = req.id, ok = ok, output = output, error = err })
end

Ext.Events.Tick:Subscribe(function()
    tickCounter = tickCounter + 1
    if tickCounter < POLL_EVERY then return end
    tickCounter = 0
    -- Guard the whole poll so a bad file can never take down the mod.
    local ok, e = pcall(poll)
    if not ok then
        Ext.Utils.PrintWarning("[ConsoleBridge] poll error: " .. tostring(e))
    end
end)

Ext.Utils.Print("[ConsoleBridge] ready - polling " .. REQ .. " in the SE user directory")
