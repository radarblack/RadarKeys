#include "ModKeyBindings.h"
#include "KeyBindMenu.h"
#include "HookUtils.h"
#include "Util.h"
#include "spdlog/spdlog.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace RadarKeys {
	namespace ModKeyBindings {
		static const char* LOG_MODKEYBINDINGS_MIGRATED_FMT_SCRIPT_S_WORTH = "ModKeyBindings: migrated {} script(s) worth of overrides from legacy {}";
		static const char* LOG_MODKEYBINDINGS_LOADFROMENTRIES_LOADED_OVERRIDES_FMT_ = "ModKeyBindings::LoadFromEntries: loaded overrides for {} script(s){}";

		static std::map<std::string, std::map<std::string, std::string>> overrides;
		static std::map<std::string, std::map<std::string, bool>> disabledMap;
		static bool loaded = false;
		static std::recursive_mutex g_overridesMutex;

		static bool TryMigrateLegacyFile() {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
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
				spdlog::info(LOG_MODKEYBINDINGS_MIGRATED_FMT_SCRIPT_S_WORTH, overrides.size(), legacyPath.string());
			}
			return foundAny;
		}

		void Load() {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (loaded) {
				return;
			}
			loaded = true;
		}

		std::vector<OverrideEntry> GetAllOverrides() {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			std::vector<OverrideEntry> result;
			std::map<std::string, std::map<std::string, bool>> seen;
			for (const auto& scriptEntry : overrides) {
				for (const auto& funcEntry : scriptEntry.second) {
					bool disabled = IsDisabled(scriptEntry.first, funcEntry.first);
					result.push_back(OverrideEntry{ scriptEntry.first, funcEntry.first, funcEntry.second, disabled });
					seen[scriptEntry.first][funcEntry.first] = true;
				}
			}
			for (const auto& scriptEntry : disabledMap) {
				for (const auto& funcEntry : scriptEntry.second) {
					if (!funcEntry.second) {
						continue;
					}
					if (seen[scriptEntry.first].count(funcEntry.first)) {
						continue;
					}
					result.push_back(OverrideEntry{ scriptEntry.first, funcEntry.first, "", true });
				}
			}
			return result;
		}

		void LoadFromEntries(const std::vector<OverrideEntry>& entries) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			overrides.clear();
			disabledMap.clear();
			for (const auto& e : entries) {
				if (e.scriptName.empty() || e.functionName.empty()) {
					continue;
				}
				if (!e.keyName.empty()) {
					overrides[e.scriptName][e.functionName] = e.keyName;
				}
				if (e.disabled) {
					disabledMap[e.scriptName][e.functionName] = true;
				}
			}

			bool migrated = false;
			if (overrides.empty()) {
				migrated = TryMigrateLegacyFile();
			}

			loaded = true;
			spdlog::debug(LOG_MODKEYBINDINGS_LOADFROMENTRIES_LOADED_OVERRIDES_FMT_,
				overrides.size(), migrated ? " (migrated from legacy file)" : "");

			if (migrated) {
				KeyBindMenu::SaveBindings();
			}
		}

		std::string GetOverride(const std::string& scriptName, const std::string& functionName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
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
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			if (keyName.empty()) {
				auto scriptIt = overrides.find(scriptName);
				if (scriptIt != overrides.end()) {
					scriptIt->second.erase(functionName);
					if (scriptIt->second.empty()) overrides.erase(scriptIt);
				}
			} else {
				overrides[scriptName][functionName] = keyName;
			}
			KeyBindMenu::SaveBindings();
		}

		void SetOverrideWithoutSave(const std::string& scriptName, const std::string& functionName, const std::string& keyName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			if (keyName.empty()) {
				auto scriptIt = overrides.find(scriptName);
				if (scriptIt != overrides.end()) {
					scriptIt->second.erase(functionName);
					if (scriptIt->second.empty()) overrides.erase(scriptIt);
				}
			} else {
				overrides[scriptName][functionName] = keyName;
			}
		}

		void SetDisabledWithoutSave(const std::string& scriptName, const std::string& functionName, bool disabled) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			disabledMap[scriptName][functionName] = disabled;
		}

		bool IsDisabled(const std::string& scriptName, const std::string& functionName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			auto scriptIt = disabledMap.find(scriptName);
			if (scriptIt == disabledMap.end()) {
				return false;
			}
			auto funcIt = scriptIt->second.find(functionName);
			if (funcIt == scriptIt->second.end()) {
				return false;
			}
			return funcIt->second;
		}

		void SetDisabled(const std::string& scriptName, const std::string& functionName, bool disabled) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			disabledMap[scriptName][functionName] = disabled;
			KeyBindMenu::SaveBindings();
		}
	}
}
