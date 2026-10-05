local this = {}

	-- Module Loading
		if rawget(_G, "RadarKeys_Dispatcher") then
			return _G.RadarKeys_Dispatcher
		end
		_G.RadarKeys_Dispatcher = this
	-- Module Loading • ends here

	--[[ RadarKeys Trigger Dispatch
		 • this.CreateDispatcher(Script-Name, RadarKeys) = returns the dispatchTrigger for one mod script.
		 • Each script creates its own dispatcher, so each keeps its own toggle memory.
		 • The trigger config comes first, then the actions; the repeat interval is in SECONDS;
		   pass an offAction to alternate on/off per fire; a stored Toggle latches on the on-transition.
		 • Auto-detects Single vs Combo keys.
	]]

	function this.CreateDispatcher(scriptName, RK)

		local toggleStates = {}
		local function dispatchTrigger(key, functionName, defaultType, holdSeconds, repeatInterval, action, offAction)
			local triggerType = defaultType
			local triggerHold = tonumber(holdSeconds) or 0
			local activeRepeatMult = nil
			local repeatIntervalValue = tonumber(repeatInterval)
			if repeatIntervalValue ~= nil and repeatIntervalValue > 0 then
				local repeatBase = 0.85
				if RK.GetRepeatBaseSeconds then
					repeatBase = RK.GetRepeatBaseSeconds()
				end
				if type(repeatBase) == "number" and repeatBase > 0 then
					activeRepeatMult = repeatBase / repeatIntervalValue
				end
			end
			
			local toggleRun = false
			if RK.GetTriggerType then
				local storedType, storedHold, storedMult, storedToggle = RK.GetTriggerType(scriptName, functionName)
				if type(storedType) == "number" and storedType >= 0 then
					triggerType = storedType
					triggerHold = storedHold
					if triggerType == 0 and (triggerHold == nil or triggerHold <= 0) then
						triggerHold = tonumber(holdSeconds) or 0
					end
					if type(storedMult) == "number" and storedMult ~= 1.0 then
						activeRepeatMult = storedMult
					end
					toggleRun = storedToggle == 1
				end
			end
			
			local function fireOnce()
				if offAction then
					toggleStates[functionName] = not (toggleStates[functionName] == true)
					if toggleStates[functionName] then action() else offAction() end
				elseif toggleRun then
					toggleStates[functionName] = not (toggleStates[functionName] == true)
					if toggleStates[functionName] then action() end
				else
					action()
				end
			end
			
			local isCombo = type(key) == "string" and string.find(key, " + ", 1, true) ~= nil
			if triggerType == 2 then
				if activeRepeatMult ~= nil then
					if isCombo then
						if RK.SetComboRepeatMult then
							RK.SetComboRepeatMult(key, activeRepeatMult)
						end
					else
						if RK.SetRepeatMult then
							RK.SetRepeatMult(key, activeRepeatMult)
						end
					end
				end
				if isCombo then
					if RK.OnComboButtonRepeat and RK.OnComboButtonRepeat(key, scriptName, functionName) then
						fireOnce()
					end
				else
					if RK.OnButtonRepeat and RK.OnButtonRepeat(key, scriptName, functionName) then
						fireOnce()
					end
				end
			elseif triggerType == 3 then
				if isCombo then
					if RK.OnComboButtonDown and RK.OnComboButtonDown(key, scriptName, functionName) then
						fireOnce()
					end
				else
					if RK.OnButtonDown and RK.OnButtonDown(key, scriptName, functionName) then
						fireOnce()
					end
				end
			elseif triggerType == 1 then
				if isCombo then
					if RK.OnComboButtonUp and RK.OnComboButtonUp(key, scriptName, functionName) then fireOnce() end
				else
					if RK.OnButtonUp and RK.OnButtonUp(key, scriptName, functionName) then fireOnce() end
				end
			elseif triggerType == 0 and triggerHold > 0 then
				if isCombo then
					if RK.OnComboButtonHoldTime and RK.OnComboButtonHoldTime(key, triggerHold, scriptName, functionName) then fireOnce() end
				else
					if RK.OnButtonHoldTime and RK.OnButtonHoldTime(key, triggerHold, scriptName, functionName) then fireOnce() end
				end
			else
				if isCombo then
					if RK.OnComboButtonDown and RK.OnComboButtonDown(key, scriptName, functionName) then fireOnce() end
				else
					if RK.OnButtonDown and RK.OnButtonDown(key, scriptName, functionName) then fireOnce() end
				end
			end
		end
		return dispatchTrigger
	end

return this
