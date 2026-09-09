local this = {}

	if rawget(_G, "RadarKeys_Core") then
		return _G.RadarKeys_Core
	end

	local ok, RadarKeysOrErr = pcall(require, "RadarKeys")
	if not ok then
		InfCore.Log("RadarKeys_Core: failed to require RadarKeys: " .. tostring(RadarKeysOrErr), true, true)
		return this
	end

	local RK = RadarKeysOrErr
	_G.RadarKeys = RK
	_G.RadarKeys_Core = this

	--[[ Key Polling API:
		RK.ButtonDown(name)       -- raw physical state, no edge/hold tracking
		RK.OnButtonDown(name)     -- true once, on the press
		RK.OnButtonUp(name)       -- true once, on the release
		RK.ButtonHeld(name)       -- true continuously once held past ~0.9s
		RK.OnButtonHoldTime(name) -- true once, when that ~0.9s threshold is crossed
		RK.OnButtonRepeat(name)   -- true on each accelerating repeat tick while held
		RK.GetRepeatMult(name)    -- current repeat acceleration multiplier
		RK.ResetRepeat(name)      -- clears hold/repeat timers for that key

	  Example - prone directly on Numpad7 instead of requiring a TOGGLE_LIGHT button-mask entry using the Shortcut.lua mod:
		if RK.OnButtonDown("Numpad 7") then prone() end
	]]
	local function ResolveFunction(functionExpr)
		if type(functionExpr) ~= "string" or functionExpr == "" then
			return nil
		end

		local simpleName = string.match(functionExpr, "^[%a_][%w_]*$")
		if simpleName then
			local fn = rawget(_G, simpleName)
			if type(fn) == "function" then
				return fn
			end
			return nil
		end

		this._functionResolvers = this._functionResolvers or {}
		local resolver = this._functionResolvers[functionExpr]
		if resolver == nil then
			local chunk, err = loadstring("return " .. functionExpr)
			if not chunk then
				InfCore.Log(tostring(err))
				this._functionResolvers[functionExpr] = false
				return nil
			end
			resolver = chunk
			this._functionResolvers[functionExpr] = resolver
		elseif resolver == false then
			return nil
		end

		local ok, fn = pcall(resolver)
		if ok and type(fn) == "function" then
			return fn
		end
		return nil
	end

	local function RunDoScript(luaExpr)
		InfCore.Log("RadarKeys DoScript:" .. luaExpr)
		local chunk, err = loadstring(luaExpr)
		if not chunk then
			InfCore.Log(tostring(err))
			RK.MenuMessage("DoScriptResult", "0|DoScriptResult|0|" .. tostring(err))
			return
		end

		local runOk, runErr = pcall(chunk)
		if runOk then
			RK.MenuMessage("DoScriptResult", "0|DoScriptResult|1|")
		else
			InfCore.Log(tostring(runErr))
			RK.MenuMessage("DoScriptResult", "0|DoScriptResult|0|" .. tostring(runErr))
		end
	end

	function this.Update()
		local messages = RK.GetMenuMessages()
		if not messages then
			return
		end
		for _, message in ipairs(messages) do
			local parts = {}
			for part in string.gmatch(message, "[^|]+") do
				table.insert(parts, part)
			end
			local cmd = parts[1]
			if cmd == "DoScript" then
				RunDoScript(parts[2])
			elseif cmd == "CallFunction" then
				local functionExpr = parts[2]
				local scriptPath = parts[3]
				local fn = ResolveFunction(functionExpr)
				if fn then
					local okCall, callErr = pcall(fn)
					if not okCall then
						InfCore.Log(tostring(callErr))
					end
				else
					if scriptPath and scriptPath ~= "" then
						RunDoScript(
							"local f=loadfile([[" ..
								scriptPath ..
									"]]); if f then f(); local fn=" ..
										tostring(functionExpr) .. '; if type(fn)=="function" then fn(); end end'
						)
					else
						InfCore.Log("RadarKeys_Core: function not found: " .. tostring(functionExpr))
					end
				end
			else
				InfCore.Log("RadarKeys_Core: unknown command '" .. tostring(cmd) .. "'")
			end
		end
	end

return this
