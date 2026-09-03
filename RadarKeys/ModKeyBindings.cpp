#include "ModKeyBindings.h"
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

		const std::string& GetConfFileName() {
			static std::string cached;
			if (cached.empty()) {
				cached = (std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys" / "radar_keybinds_mod_.conf").string();
			}
			return cached;
		}

		bool EnsureConfDirectory() {
			std::error_code ec;
			std::filesystem::path dir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
			std::filesystem::create_directories(dir, ec);
			if (ec) {
				spdlog::warn("ModKeyBindings: couldn't create {} directory: {}", dir.string(), ec.message());
				return false;
			}
			return true;
		}

		void Load() {
			overrides.clear();
			std::ifstream inFile(GetConfFileName());
			if (!inFile) {
				spdlog::debug("ModKeyBindings::Load: no {} yet (fine on first run)", GetConfFileName());
				loaded = true;
				return;
			}

			std::string currentScript;
			std::string line;
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
					continue; // stray line before any [Section] header
				}

				size_t eq = line.find('=');
				if (eq == std::string::npos) {
					spdlog::warn("ModKeyBindings::Load: skipping malformed line under [{}]: {}", currentScript, line);
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
			}
			loaded = true;
			spdlog::debug("ModKeyBindings::Load: loaded overrides for {} script(s) from {}", overrides.size(), GetConfFileName());
		}

		void Save() {
			EnsureConfDirectory();
			std::ofstream outFile(GetConfFileName());
			if (!outFile) {
				spdlog::warn("ModKeyBindings::Save: couldn't open {} for writing", GetConfFileName());
				return;
			}
			for (const auto& scriptEntry : overrides) {
				outFile << "[" << scriptEntry.first << "]\n";
				for (const auto& funcEntry : scriptEntry.second) {
					outFile << funcEntry.first << "=" << funcEntry.second << "\n";
				}
				outFile << "\n";
			}
			outFile.close();
			spdlog::debug("ModKeyBindings::Save: wrote overrides for {} script(s) to {}", overrides.size(), GetConfFileName());
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
			Save();
		}
	}
}
