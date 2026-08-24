--RadarKeys_Core.lua
--tex: loads RadarKeys.dll via Lua's own require() (no IHHook/dinput8.dll needed at all - see
--dllmain.cpp's luaopen_RadarKeys), then polls it every frame for messages it queued from its own
--render/input thread (key presses -> "DoScript|<lua expression>" requests), running them here on
--Lua's own thread and reporting success/failure back via RadarKeys.MenuMessage("DoScriptResult",...)
--so DebuggerMenu's script-attempt logging can show dll-side vs lua-side failures correctly.
--
--This is a normal InfModule (this.Update() called every frame by InfMain's module loader) -
--Infinite Heaven itself must be installed for this to load, but IHHook is NOT required at all.

local this = {}

if rawget(_G, "RadarKeys_Core") then
  return _G.RadarKeys_Core
end

local ok, RadarKeysOrErr = pcall(require, "RadarKeys")
if not ok then
  InfCore.Log("RadarKeys_Core: failed to require RadarKeys: "..tostring(RadarKeysOrErr),true,true)
  return this
end

local RK = RadarKeysOrErr
_G.RadarKeys = RK
_G.RadarKeys_Core = this

--tex: runs a "DoScript|<lua expression>" request queued from RadarKeys.dll's render/input thread
--(see KeyBindMenu.cpp/DebuggerMenu.cpp's LuaBridge::QueueMessageIn calls, which send exactly
--"DoScript|dofile([[path]])" - luaExpr here is that dofile(...) call, not a raw path). Reports
--success/failure back so DebuggerMenu can distinguish dll-side vs lua-side failures, mirroring
--what RadarKeysDoScriptHook.lua did for the old IHHook-based build - but this is now the ONLY
--implementation, since this build doesn't touch InfExtToMgsv.lua/IHHook's DoScript command at all.
local function RunDoScript(luaExpr)
  InfCore.Log("RadarKeys DoScript:"..luaExpr)
  local chunk,err=loadstring(luaExpr)
  if not chunk then
    InfCore.Log(tostring(err))
    RK.MenuMessage("DoScriptResult","0|DoScriptResult|0|"..tostring(err))
    return
  end

  local runOk,runErr=pcall(chunk)
  if runOk then
    RK.MenuMessage("DoScriptResult","0|DoScriptResult|1|")
  else
    InfCore.Log(tostring(runErr))
    RK.MenuMessage("DoScriptResult","0|DoScriptResult|0|"..tostring(runErr))
  end
end--RunDoScript

function this.Update()
  local messages=RK.GetMenuMessages()
  if not messages then
    return
  end
  for _,message in ipairs(messages) do
    --tex message format: "DoScript|dofile([[path]])" - exactly 2 fields, no leading sequence
    --number (that's only used on the OUTGOING DoScriptResult side above, to match LuaBridge's
    --DispatchMessage convention on the C++ side - GetMenuMessages here just returns whatever raw
    --strings QueueMessageIn pushed, unparsed).
    local parts={}
    for part in string.gmatch(message,"[^|]+") do
      table.insert(parts,part)
    end
    local cmd=parts[1]
    if cmd=="DoScript" then
      RunDoScript(parts[2])
    else
      InfCore.Log("RadarKeys_Core: unknown command '"..tostring(cmd).."'")
    end
  end
end--Update

return this