#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	namespace ModInfoRegistry {
		struct ModInfo {
			std::string scriptName;
			std::string modName;
			std::string modDescription;
			std::string modCreator;
			std::string modVersion;
			std::string modLink;
		};

		void DescribeMod(const std::string& scriptName, const std::string& modName, const std::string& modDescription, const std::string& modCreator, const std::string& modVersion, const std::string& modLink);
		void SweepStale();
		std::vector<ModInfo> GetTrackedModInfo();
	}
}
