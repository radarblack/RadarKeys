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
end

function this.Update()
  local messages=RK.GetMenuMessages()
  if not messages then
    return
  end
  for _,message in ipairs(messages) do
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
end

return this
