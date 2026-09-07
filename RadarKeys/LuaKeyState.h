#pragma once
#include "windowsapi.h"
#include <vector>
#include <string>

namespace RadarKeys {
	namespace LuaKeyState {
		bool ButtonDown(USHORT vKey);
		bool ButtonHeld(USHORT vKey, double holdSecondsOverride = -1.0);
		bool OnButtonDown(USHORT vKey);
		bool OnButtonUp(USHORT vKey);
		bool OnButtonRepeat(USHORT vKey);
		bool OnButtonHoldTime(USHORT vKey, double holdSecondsOverride = -1.0);
		void ResetRepeat(USHORT vKey);
		double GetRepeatMult(USHORT vKey);
		bool PhysicalOnButtonDown(USHORT vKey);
		bool PhysicalOnButtonUp(USHORT vKey);
		bool PhysicalOnButtonHoldTime(USHORT vKey, double holdSecondsOverride = -1.0);
		void SweepStaleDescriptions();
		void RetireIfUndescribed(USHORT vKey);
		void SetSuppressedVKeys(const std::vector<USHORT>& vKeys);
		void ReassignBinding(USHORT oldVKey, USHORT newVKey, const std::string& scriptName, const std::string& functionName);
		void DescribeKey(USHORT vKey, const std::string& scriptName, const std::string& functionName, const std::string& toggleState);

		struct TrackedKeyInfo {
			USHORT vKey = 0;
			bool isPressed = false;
			bool hasDescription = false;
			std::string scriptName;
			std::string functionName;
			bool hasToggleState = false;
			bool toggleEnabled = false;
			bool isConflicted = false;
			bool usesOnPress = false;
			bool usesHoldTime = false;
			double lastHoldSeconds = 0.0;
			bool usesRepeat = false;
			bool usesOnRelease = false;
		};
		std::vector<TrackedKeyInfo> GetTrackedKeyInfo();
	}
}
