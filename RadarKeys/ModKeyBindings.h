#pragma once
#include <string>

namespace RadarKeys {
	namespace ModKeyBindings {
		void Load();
		std::string GetOverride(const std::string& scriptName, const std::string& functionName);
		void SetOverride(const std::string& scriptName, const std::string& functionName, const std::string& keyName);
	}
}
