-- MOHAVR: load the Cheat Engine MCP bridge.
--
-- In Cheat Engine: press Ctrl+Alt+L for the Lua Engine (or Table -> Show Cheat Table Lua
-- Script), paste this, and Execute. Stock CE 7.7 has no File -> Execute Script.
--
-- Look for: [MCP v12.0.0] MCP Server Listening on: CE_MCP_Bridge_v99
--
-- The project root comes from the MOHAVR_ROOT environment variable when it is set, so this
-- needs no editing after moving machines:
--     setx MOHAVR_ROOT "D:\path\to\MOHAVR"
-- Otherwise the fallback below is used -- change that one line instead.
--
-- MOHA.exe is 32-bit: attach with the normal CE (it handles 32-bit targets from the 64-bit
-- build), not a separate 32-bit CE.

local root = os.getenv("MOHAVR_ROOT") or [[C:\Users\j_tom\Projects\MOHAVR]]
local script = root .. [[\tools\cheatengine-mcp-bridge\MCP_Server\ce_mcp_bridge.lua]]

local f = io.open(script, "r")
if f == nil then
  print("[MOHAVR] bridge script not found: " .. script)
  print("[MOHAVR] set MOHAVR_ROOT to the project folder, or edit the fallback in this file.")
  return
end
f:close()

print("[MOHAVR] loading " .. script)
dofile(script)
