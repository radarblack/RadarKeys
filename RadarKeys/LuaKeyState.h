#pragma once
#include "windowsapi.h"
#include <vector>
#include <string>
#include <map>

namespace RadarKeys {
	namespace LuaKeyState {
		bool ButtonDown(USHORT vKey);
		bool ButtonHeld(USHORT vKey, double holdSecondsOverride = -1.0, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool OnButtonDown(USHORT vKey, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool OnButtonUp(USHORT vKey, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool OnButtonRepeat(USHORT vKey, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool OnButtonHoldTime(USHORT vKey, double holdSecondsOverride = -1.0, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool ComboButtonDown(const std::vector<USHORT>& vKeys);
		bool OnComboButtonDown(const std::vector<USHORT>& vKeys, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool OnComboButtonUp(const std::vector<USHORT>& vKeys, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool ComboButtonHeld(const std::vector<USHORT>& vKeys, double holdSecondsOverride = -1.0);
		bool OnComboButtonHoldTime(const std::vector<USHORT>& vKeys, double holdSecondsOverride = -1.0, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		bool OnComboButtonRepeat(const std::vector<USHORT>& vKeys, const std::string& scriptName = std::string(), const std::string& functionName = std::string());
		double GetComboRepeatMult(const std::vector<USHORT>& vKeys);
		double GetComboRepeatIntervalSeconds(const std::vector<USHORT>& vKeys);
		void ResetComboRepeat(const std::vector<USHORT>& vKeys);
		void ResetRepeat(USHORT vKey);
		double GetRepeatMult(USHORT vKey);
		double GetRepeatIntervalSeconds(USHORT vKey);
		double GetRepeatBaseSeconds();
		void SetComboRepeatMult(const std::vector<USHORT>& vKeys, double mult);
		void SetRepeatMult(USHORT vKey, double mult);
		bool PhysicalOnButtonDown(USHORT vKey);
		bool PhysicalOnButtonUp(USHORT vKey);
		bool PhysicalOnButtonHoldTime(USHORT vKey, double holdSecondsOverride = -1.0);
		void SweepStaleDescriptions();
		void RetireIfUndescribed(USHORT vKey);
		void SetSuppressedVKeys(const std::vector<USHORT>& vKeys);
		void SetDisabledVKeys(const std::vector<USHORT>& vKeys);
		void SetHoldSecondsOverrides(const std::map<USHORT, double>& overrides);
		void SetDisabledCombos(const std::vector<std::vector<USHORT>>& combos);
		void SetDisabledFunctionIdentities(const std::vector<std::string>& identities);
		void ReassignBinding(USHORT oldVKey, USHORT newVKey, const std::string& scriptName, const std::string& functionName);
		void DescribeKey(USHORT vKey, const std::string& scriptName, const std::string& functionName, const std::string& toggleState, int declaredTriggerType = -1, double declaredHoldSeconds = -1.0, double declaredRepeatSeconds = -1.0);
		void DescribeComboKey(const std::vector<USHORT>& vKeys, const std::string& scriptName, const std::string& functionName, const std::string& toggleState, int declaredTriggerType = -1, double declaredHoldSeconds = -1.0, double declaredRepeatSeconds = -1.0);
		void SweepStaleComboDescriptions();
		void RetireCombosForIdentity(const std::string& scriptName, const std::string& functionName, const std::vector<std::vector<USHORT>>& keepActiveKeySets);
		void FlushPendingEdgesForIdentity(const std::string& scriptName, const std::string& functionName);
		double GetStaleSweepSecondsRemaining();
		double GetStaleSweepSecondsTotal();
		void OnFocusLost();
		USHORT FindRedirectSource(USHORT activeVKey);
		void ClearComboRedirect(const std::vector<USHORT>& nativeVKeys);
		std::vector<USHORT> ResolveActiveCombo(const std::vector<USHORT>& vKeys);

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
			int declaredTriggerType = -1;
			double declaredHoldSeconds = -1.0;
			double declaredRepeatSeconds = -1.0;
		};
		std::vector<TrackedKeyInfo> GetTrackedKeyInfo();

		struct TrackedComboKeyInfo {
			std::vector<USHORT> nativeKeys;
			std::vector<USHORT> activeKeys;
			bool isPressed = false;
			std::string scriptName;
			std::string functionName;
			bool hasToggleState = false;
			bool toggleEnabled = false;
			bool usesOnPress = false;
			bool usesHoldTime = false;
			bool usesRepeat = false;
			bool usesOnRelease = false;
			double lastHoldSeconds = 0.0;
			int declaredTriggerType = -1;
			double declaredHoldSeconds = -1.0;
			double declaredRepeatSeconds = -1.0;
		};
		std::vector<TrackedComboKeyInfo> GetTrackedComboKeyInfo();
	}
}
