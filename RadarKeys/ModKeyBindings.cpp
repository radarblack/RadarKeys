#include "ModKeyBindings.h"
#include "KeyBindMenu.h"
#include "HookUtils.h"
#include "Util.h"
#include "spdlog/spdlog.h"

#include <filesystem>
#include <fstream>
#include <map>

namespace RadarKeys {
	namespace ModKeyBindings {
		static std::map<std::string, std::map<std::string, std::string>> overrides;
		static bool loaded = false;
		static bool TryMigrateLegacyFile() {
			std::filesystem::path legacyPath = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys" / "radar_keybinds_mod.conf";
			std::ifstream inFile(legacyPath);
			if (!inFile) {
				return false;
			}

			std::string currentScript;
			std::string line;
			bool foundAny = false;
			while (std::getline(inFile, line)) {
				line = trim(line);
				if (line.empty()) {
					continue;
				}
				if (line.front() == '[' && line.back() == ']') {
					currentScript = line.substr(1, line.size() - 2);
					continue;
				}
				if (currentScript.empty()) {
					continue;
				}

				size_t eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				std::string functionName = line.substr(0, eq);
				std::string keyName = line.substr(eq + 1);
				trim(functionName);
				trim(keyName);
				if (functionName.empty() || keyName.empty()) {
					continue;
				}

				overrides[currentScript][functionName] = keyName;
				foundAny = true;
			}

			if (foundAny) {
				spdlog::info("ModKeyBindings: migrated {} script(s) worth of overrides from legacy {}", overrides.size(), legacyPath.string());
			}
			return foundAny;
		}

		void Load() {
			if (loaded) {
				return;
			}
			loaded = true;
		}

		std::vector<OverrideEntry> GetAllOverrides() {
			std::vector<OverrideEntry> result;
			for (const auto& scriptEntry : overrides) {
				for (const auto& funcEntry : scriptEntry.second) {
					result.push_back(OverrideEntry{ scriptEntry.first, funcEntry.first, funcEntry.second });
				}
			}
			return result;
		}

		void LoadFromEntries(const std::vector<OverrideEntry>& entries) {
			overrides.clear();
			for (const auto& e : entries) {
				if (e.scriptName.empty() || e.functionName.empty() || e.keyName.empty()) {
					continue;
				}
				overrides[e.scriptName][e.functionName] = e.keyName;
			}

			bool migrated = false;
			if (overrides.empty()) {
				migrated = TryMigrateLegacyFile();
			}

			loaded = true;
			spdlog::debug("ModKeyBindings::LoadFromEntries: loaded overrides for {} script(s){}",
				overrides.size(), migrated ? " (migrated from legacy file)" : "");

			if (migrated) {
				KeyBindMenu::SaveBindings();
			}
		}

		std::string GetOverride(const std::string& scriptName, const std::string& functionName) {
			if (!loaded) {
				Load();
			}
			auto scriptIt = overrides.find(scriptName);
			if (scriptIt == overrides.end()) {
				return "";
			}
			auto funcIt = scriptIt->second.find(functionName);
			if (funcIt == scriptIt->second.end()) {
				return "";
			}
			return funcIt->second;
		}

		void SetOverride(const std::string& scriptName, const std::string& functionName, const std::string& keyName) {
			if (!loaded) {
				Load();
			}
			overrides[scriptName][functionName] = keyName;
			KeyBindMenu::SaveBindings();
		}
	}
}
