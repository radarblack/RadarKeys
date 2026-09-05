#pragma once
#include "windowsapi.h"
#include <vector>
#include <string>

namespace RadarKeys {
	namespace LuaKeyState {
		bool ButtonDown(USHORT vKey);
		bool OnButtonDown(USHORT vKey);
		bool OnButtonUp(USHORT vKey);
		bool ButtonHeld(USHORT vKey, double holdSecondsOverride = -1.0);
		bool OnButtonHoldTime(USHORT vKey, double holdSecondsOverride = -1.0);
		bool OnButtonRepeat(USHORT vKey);
		double GetRepeatMult(USHORT vKey);
		void ResetRepeat(USHORT vKey);
		void DescribeKey(USHORT vKey, const std::string& scriptName, const std::string& functionName, const std::string& toggleState);
		void SweepStaleDescriptions();

		struct TrackedKeyInfo {
			USHORT vKey = 0;
			bool isPressed = false;
			bool hasDescription = false;
			std::string scriptName;
			std::string functionName;
			bool hasToggleState = false;
			bool toggleEnabled = false;
			bool isConflicted = false;
		};
		std::vector<TrackedKeyInfo> GetTrackedKeyInfo();
	}
}
