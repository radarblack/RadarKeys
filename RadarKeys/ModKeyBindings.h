#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	namespace ModKeyBindings {
		struct OverrideEntry {
			std::string scriptName;
			std::string functionName;
			std::string keyName;
			bool disabled = false;
		};

		void Load();
		std::string GetOverride(const std::string& scriptName, const std::string& functionName);
		void SetOverride(const std::string& scriptName, const std::string& functionName, const std::string& keyName);

		bool IsDisabled(const std::string& scriptName, const std::string& functionName);
		void SetDisabled(const std::string& scriptName, const std::string& functionName, bool disabled);

		std::vector<OverrideEntry> GetAllOverrides();
		void LoadFromEntries(const std::vector<OverrideEntry>& entries);
	}
}
