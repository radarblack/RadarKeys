#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	namespace ModKeyBindings {
		struct OverrideEntry {
			std::string scriptName;
			std::string functionName;
			std::string keyName;
		};

		void Load();
		std::string GetOverride(const std::string& scriptName, const std::string& functionName);
		void SetOverride(const std::string& scriptName, const std::string& functionName, const std::string& keyName);

		std::vector<OverrideEntry> GetAllOverrides();
		void LoadFromEntries(const std::vector<OverrideEntry>& entries);
	}
}
