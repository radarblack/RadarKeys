#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	namespace ModKeyBindings {
		enum class BindSlot {
			Kbm = 0,
			Pad = 1
		};

		struct OverrideEntry {
			std::string scriptName;
			std::string functionName;
			std::string keyName;
			std::string padKeyName;
			bool disabled = false;
			int triggerType = 0;
			float holdSeconds = 0.0f;
			float repeatAccelMult = 1.0f;
			std::string nativeKeyName;
		};

		void Load();
		std::string GetNativeKey(const std::string& scriptName, const std::string& functionName);
		void SetNativeKeyWithoutSave(const std::string& scriptName, const std::string& functionName, const std::string& keyName);

		std::string GetOverride(const std::string& scriptName, const std::string& functionName);
		std::string GetSlotOverride(const std::string& scriptName, const std::string& functionName, BindSlot slot);
		void SetOverride(const std::string& scriptName, const std::string& functionName, const std::string& keyName);
		void SetOverrideWithoutSave(const std::string& scriptName, const std::string& functionName, const std::string& keyName);
		void SetSlotOverride(const std::string& scriptName, const std::string& functionName, BindSlot slot, const std::string& keyName);
		void SetSlotOverrideWithoutSave(const std::string& scriptName, const std::string& functionName, BindSlot slot, const std::string& keyName);
		void SetDisabledWithoutSave(const std::string& scriptName, const std::string& functionName, bool disabled);

		bool IsDisabled(const std::string& scriptName, const std::string& functionName);
		void SetDisabled(const std::string& scriptName, const std::string& functionName, bool disabled);

		struct TriggerConfig {
			int triggerType = 0;
			float holdSeconds = 0.0f;
			float repeatAccelMult = 1.0f;
		};

		TriggerConfig GetTriggerConfig(const std::string& scriptName, const std::string& functionName);
		void SetTriggerConfigWithoutSave(const std::string& scriptName, const std::string& functionName, int triggerType, float holdSeconds, float repeatAccelMult);
		bool HasTriggerConfig(const std::string& scriptName, const std::string& functionName);

		std::vector<OverrideEntry> GetAllOverrides();
		void LoadFromEntries(const std::vector<OverrideEntry>& entries);
	}
}
