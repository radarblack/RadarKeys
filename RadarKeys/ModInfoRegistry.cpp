#include "ModInfoRegistry.h"
#include <algorithm>
#include <chrono>
#include <mutex>

namespace RadarKeys {
	namespace ModInfoRegistry {
		namespace {
			constexpr double kModInfoStaleSeconds = 30.0;

			struct Entry {
				ModInfo info;
				std::chrono::steady_clock::time_point lastTouched = std::chrono::steady_clock::now();
			};

			std::vector<Entry> entries;
			std::mutex g_entriesMutex;

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

			std::lock_guard<std::mutex> lock(g_entriesMutex);
			Entry* existing = FindByScript(scriptName);
			if (existing) {
				existing->info.modName = modName;
				existing->info.modDescription = modDescription;
				existing->info.modCreator = modCreator;
				existing->info.modVersion = modVersion;
				existing->info.modLink = modLink;
				existing->lastTouched = std::chrono::steady_clock::now();
				return;
			}

			Entry e;
			e.info.scriptName = scriptName;
			e.info.modName = modName;
			e.info.modDescription = modDescription;
			e.info.modCreator = modCreator;
			e.info.modVersion = modVersion;
			e.info.modLink = modLink;
			e.lastTouched = std::chrono::steady_clock::now();
			entries.push_back(std::move(e));
		}

		void SweepStale() {
			std::lock_guard<std::mutex> lock(g_entriesMutex);
			const auto now = std::chrono::steady_clock::now();
			entries.erase(
				std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) {
					return std::chrono::duration<double>(now - e.lastTouched).count() > kModInfoStaleSeconds;
				}),
				entries.end()
			);
		}

		std::vector<ModInfo> GetTrackedModInfo() {
			std::lock_guard<std::mutex> lock(g_entriesMutex);
			std::vector<ModInfo> result;
			result.reserve(entries.size());
			for (const Entry& e : entries) {
				result.push_back(e.info);
			}
			return result;
		}
	}
}
