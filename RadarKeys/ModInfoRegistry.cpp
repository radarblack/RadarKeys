#include "ModInfoRegistry.h"
#include <algorithm>

namespace RadarKeys {
	namespace ModInfoRegistry {
		namespace {
			struct Entry {
				ModInfo info;
				bool touchedSinceSweep = false;
			};

			std::vector<Entry> entries;

			Entry* FindByScript(const std::string& scriptName) {
				for (Entry& e : entries) {
					if (e.info.scriptName == scriptName) {
						return &e;
					}
				}
				return nullptr;
			}
		}

		void DescribeMod(const std::string& scriptName, const std::string& modName, const std::string& modDescription, const std::string& modCreator, const std::string& modVersion, const std::string& modLink) {
			if (scriptName.empty()) {
				return;
			}

			Entry* existing = FindByScript(scriptName);
			if (existing) {
				existing->info.modName = modName;
				existing->info.modDescription = modDescription;
				existing->info.modCreator = modCreator;
				existing->info.modVersion = modVersion;
				existing->info.modLink = modLink;
				existing->touchedSinceSweep = true;
				return;
			}

			Entry e;
			e.info.scriptName = scriptName;
			e.info.modName = modName;
			e.info.modDescription = modDescription;
			e.info.modCreator = modCreator;
			e.info.modVersion = modVersion;
			e.info.modLink = modLink;
			e.touchedSinceSweep = true;
			entries.push_back(std::move(e));
		}

		void SweepStale() {
			entries.erase(
				std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return !e.touchedSinceSweep; }),
				entries.end()
			);
			for (Entry& e : entries) {
				e.touchedSinceSweep = false;
			}
		}

		std::vector<ModInfo> GetTrackedModInfo() {
			std::vector<ModInfo> result;
			result.reserve(entries.size());
			for (const Entry& e : entries) {
				result.push_back(e.info);
			}
			return result;
		}
	}
}
