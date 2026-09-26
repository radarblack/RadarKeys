#include "ModKeyBindings.h"
#include "KeyBindMenu.h"
#include "HookUtils.h"
#include "Util.h"
#include "spdlog/spdlog.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace RadarKeys {
	namespace ModKeyBindings {
		static const char* LOG_MODKEYBINDINGS_MIGRATED_FMT_SCRIPT_S_WORTH = "ModKeyBindings: migrated {} script(s) worth of overrides from legacy {}";
		static const char* LOG_MODKEYBINDINGS_LOADFROMENTRIES_LOADED_OVERRIDES_FMT_ = "ModKeyBindings::LoadFromEntries: loaded overrides for {} script(s){}";

		struct SlotOverride {
			std::string kbm;
			std::string pad;
			int triggerType = 0;
			float holdSeconds = 0.0f;
			float repeatAccelMult = 1.0f;
			bool toggleMode = false;
			std::string nativeKeyName;
		};

		static std::map<std::string, std::map<std::string, SlotOverride>> overrides;
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

				overrides[currentScript][functionName].kbm = keyName;
				overrides[currentScript][functionName].pad = keyName;
				foundAny = true;
			}

			if (foundAny) {
				spdlog::info(LOG_MODKEYBINDINGS_MIGRATED_FMT_SCRIPT_S_WORTH, overrides.size(), FileNameOnly(legacyPath.string()));
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
					OverrideEntry entry;
					entry.scriptName = scriptEntry.first;
					entry.functionName = funcEntry.first;
					entry.keyName = funcEntry.second.kbm;
					entry.padKeyName = funcEntry.second.pad;
					entry.disabled = disabled;
					entry.triggerType = funcEntry.second.triggerType;
					entry.holdSeconds = funcEntry.second.holdSeconds;
					entry.repeatAccelMult = funcEntry.second.repeatAccelMult;
					entry.toggleMode = funcEntry.second.toggleMode;
					entry.nativeKeyName = funcEntry.second.nativeKeyName;
					result.push_back(entry);
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
					result.push_back(OverrideEntry{ scriptEntry.first, funcEntry.first, "", "", true });
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
					overrides[e.scriptName][e.functionName].kbm = e.keyName;
				}
				if (!e.padKeyName.empty()) {
					overrides[e.scriptName][e.functionName].pad = e.padKeyName;
				}
				if (!e.nativeKeyName.empty()) {
					overrides[e.scriptName][e.functionName].nativeKeyName = e.nativeKeyName;
				}
				if (e.triggerType != 0 || e.holdSeconds > 0.0f || e.repeatAccelMult != 1.0f || e.toggleMode) {
					overrides[e.scriptName][e.functionName].triggerType = e.triggerType;
					overrides[e.scriptName][e.functionName].holdSeconds = e.holdSeconds;
					overrides[e.scriptName][e.functionName].repeatAccelMult = e.repeatAccelMult;
					overrides[e.scriptName][e.functionName].toggleMode = e.toggleMode;
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

		TriggerConfig GetTriggerConfig(const std::string& scriptName, const std::string& functionName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			TriggerConfig config;
			auto scriptIt = overrides.find(scriptName);
			if (scriptIt == overrides.end()) {
				return config;
			}
			auto funcIt = scriptIt->second.find(functionName);
			if (funcIt == scriptIt->second.end()) {
				return config;
			}
			config.triggerType = funcIt->second.triggerType;
			config.holdSeconds = funcIt->second.holdSeconds;
			config.repeatAccelMult = funcIt->second.repeatAccelMult;
			config.toggleMode = funcIt->second.toggleMode;
			return config;
		}

		void SetTriggerConfigWithoutSave(const std::string& scriptName, const std::string& functionName, int triggerType, float holdSeconds, float repeatAccelMult, bool toggleMode) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			if (triggerType < 0 || triggerType > 3) {
				triggerType = 0;
			}
			if (!std::isfinite(holdSeconds) || holdSeconds < 0.0f) {
				holdSeconds = 0.0f;
			}
			if (!std::isfinite(repeatAccelMult) || repeatAccelMult < 0.1f) {
				repeatAccelMult = 0.1f;
			} else if (repeatAccelMult > 20.0f) {
				repeatAccelMult = 20.0f;
			}
			overrides[scriptName][functionName].triggerType = triggerType;
			overrides[scriptName][functionName].holdSeconds = holdSeconds;
			overrides[scriptName][functionName].repeatAccelMult = repeatAccelMult;
			overrides[scriptName][functionName].toggleMode = toggleMode;
		}

		bool HasTriggerConfig(const std::string& scriptName, const std::string& functionName) {
			TriggerConfig config = GetTriggerConfig(scriptName, functionName);
			return config.triggerType != 0 || config.holdSeconds > 0.0f || config.repeatAccelMult != 1.0f || config.toggleMode;
		}

		std::string GetNativeKey(const std::string& scriptName, const std::string& functionName) {
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
			return funcIt->second.nativeKeyName;
		}

		void SetNativeKeyWithoutSave(const std::string& scriptName, const std::string& functionName, const std::string& keyName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			if (keyName.empty()) {
				return;
			}
			auto& native = overrides[scriptName][functionName].nativeKeyName;
			if (native.empty()) {
				native = keyName;
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
			return funcIt->second.kbm.empty() ? funcIt->second.pad : funcIt->second.kbm;
		}

		std::string GetSlotOverride(const std::string& scriptName, const std::string& functionName, BindSlot slot) {
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
			return (slot == BindSlot::Pad) ? funcIt->second.pad : funcIt->second.kbm;
		}

		void SetOverride(const std::string& scriptName, const std::string& functionName, const std::string& keyName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			if (keyName.empty()) {
				auto scriptIt = overrides.find(scriptName);
				if (scriptIt != overrides.end()) {
					auto funcIt = scriptIt->second.find(functionName);
					if (funcIt != scriptIt->second.end()) {
						funcIt->second.kbm.clear();
						funcIt->second.pad.clear();
						funcIt->second.triggerType = 0;
						funcIt->second.holdSeconds = 0.0f;
						funcIt->second.repeatAccelMult = 1.0f;
						funcIt->second.toggleMode = false;
						if (funcIt->second.nativeKeyName.empty()) {
							scriptIt->second.erase(funcIt);
							if (scriptIt->second.empty()) overrides.erase(scriptIt);
						}
					}
				}
			} else {
				overrides[scriptName][functionName].kbm = keyName;
				overrides[scriptName][functionName].pad = keyName;
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
				overrides[scriptName][functionName].kbm = keyName;
				overrides[scriptName][functionName].pad = keyName;
			}
		}

		void SetSlotOverrideWithoutSave(const std::string& scriptName, const std::string& functionName, BindSlot slot, const std::string& keyName) {
			std::lock_guard<std::recursive_mutex> lock(g_overridesMutex);
			if (!loaded) {
				Load();
			}
			if (keyName.empty()) {
				auto scriptIt = overrides.find(scriptName);
				if (scriptIt != overrides.end()) {
					auto funcIt = scriptIt->second.find(functionName);
					if (funcIt != scriptIt->second.end()) {
						if (slot == BindSlot::Pad) funcIt->second.pad.clear();
						else funcIt->second.kbm.clear();
						if (funcIt->second.kbm.empty() && funcIt->second.pad.empty() && funcIt->second.triggerType == 0 && funcIt->second.holdSeconds == 0.0f && funcIt->second.repeatAccelMult == 1.0f && !funcIt->second.toggleMode && funcIt->second.nativeKeyName.empty()) {
							scriptIt->second.erase(funcIt);
							if (scriptIt->second.empty()) overrides.erase(scriptIt);
						}
					}
				}
			} else {
				if (slot == BindSlot::Pad) overrides[scriptName][functionName].pad = keyName;
				else overrides[scriptName][functionName].kbm = keyName;
			}
		}

		void SetSlotOverride(const std::string& scriptName, const std::string& functionName, BindSlot slot, const std::string& keyName) {
			SetSlotOverrideWithoutSave(scriptName, functionName, slot, keyName);
			KeyBindMenu::SaveBindings();
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
