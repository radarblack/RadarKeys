#include "DebuggerMenu.h"
#include "LuaBridge.h"
#include "KeyBindMenu.h"
#include "spdlog/spdlog.h"
#include "imgui/imgui.h"

#include <filesystem>
#include <deque>
#include <mutex>
#include <vector>
#include <ctime>

namespace RadarKeys {
	namespace DebuggerMenu {
		static const char* LOG_BND_FMT = "[BND] {}";
		static const char* LOG_BTN_FMT = "[BTN] {}";
		static const char* LOG_LUA_FMT = "[LUA] {}";
		static const char* LOG_SCR_DLL_MISSING_SKIP_FMT = "[SCR][DLL] missing - SKIP: {}";
		static const char* LOG_SCR_DLL_ATTEMPT_FMT = "[SCR][DLL] attempt: {}";
		static const char* LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_MALFORMED_ARGS_SIZE_FM = "DebuggerMenu::OnDoScriptResult: malformed args (size {})";
		static const char* LOG_SCR_LUA_SUCCESS = "[SCR][LUA] success";
		static const char* LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_SCRIPT_FAILED_BUT_NO = "DebuggerMenu::OnDoScriptResult: script failed but no error message provided (size {})";
		static const char* LOG_SCR_LUA_FAIL_FMT = "[SCR][LUA] fail: {}";

		bool logBindUnbind = false;
		bool logButtonPress = false;
		bool logScriptResult = false;
		bool logLuaDebug = false;
		bool menuOpen = false;

		struct LogEntry {
			std::string timestamp;
			std::string text;
		};
		const size_t maxLogEntries = 300;
		std::deque<LogEntry> logEntries;
		std::mutex g_logMutex;

		std::string CurrentTimestamp() {
			using clock = std::chrono::steady_clock;
			static const auto bootTime = clock::now();
			auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - bootTime);
			auto hours = std::chrono::duration_cast<std::chrono::hours>(durationMs);
			durationMs -= hours;
			auto minutes = std::chrono::duration_cast<std::chrono::minutes>(durationMs);
			durationMs -= minutes;
			auto seconds = std::chrono::duration_cast<std::chrono::seconds>(durationMs);
			durationMs -= seconds;

			char timestr[32];
			snprintf(timestr, sizeof(timestr), "%02d:%02d:%02d.%03d", static_cast<int>(hours.count()), static_cast<int>(minutes.count()), static_cast<int>(seconds.count()), static_cast<int>(durationMs.count())); // L3: %d needs int
			return std::string(timestr);
		}

		void AddLogEntry(std::string text) {
			std::lock_guard<std::mutex> lock(g_logMutex);
			logEntries.emplace_back(LogEntry{ CurrentTimestamp(), std::move(text) });
			
			while (logEntries.size() > maxLogEntries) {
				logEntries.pop_front();
			}
		}

		void LogBindEvent(const std::string& message) {
			spdlog::info(LOG_BND_FMT, message);
			KeyBindMenu::LogActivity("[BND] " + message);
			if (!logBindUnbind) return;
			AddLogEntry("[BND] " + message);
		}

		void LogButtonPress(const std::string& message) {
			spdlog::debug(LOG_BTN_FMT, message);
			KeyBindMenu::LogActivity("[BTN] " + message);
			if (!logButtonPress) return;
			AddLogEntry("[BTN] " + message);
		}

		void LogLuaDebug(const std::string& message) {
			spdlog::debug(LOG_LUA_FMT, message);
			KeyBindMenu::LogActivity("[LUA] " + message);
			if (!logLuaDebug) return;
			AddLogEntry("[LUA] " + message);
		}

		bool LogScriptAttempt(const std::string& scriptPath) {
			bool exists = std::filesystem::exists(scriptPath);
			if (!exists) {
				spdlog::error(LOG_SCR_DLL_MISSING_SKIP_FMT, scriptPath);
				KeyBindMenu::LogActivity("[SCR][DLL] missing - SKIP: " + scriptPath, false);
				if (logScriptResult) {
					AddLogEntry("[SCR][DLL] missing - SKIP: " + scriptPath);
				}
				return false;
			}
			spdlog::debug(LOG_SCR_DLL_ATTEMPT_FMT, scriptPath);
			KeyBindMenu::LogActivity("[SCR][DLL] attempt: " + scriptPath);
			if (logScriptResult) {
				AddLogEntry("[SCR][DLL] attempt: " + scriptPath);
			}
			return true;
		}

		void OnDoScriptResult(std::vector<std::string> args) {
			if (args.size() < 3) {
				spdlog::warn(LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_MALFORMED_ARGS_SIZE_FM, args.size());
				return;
			}

			bool success = args[2] == "1";
			if (success) {
				spdlog::debug(LOG_SCR_LUA_SUCCESS);
				KeyBindMenu::LogActivity(LOG_SCR_LUA_SUCCESS);
				if (logScriptResult) {
					AddLogEntry("[SCR][LUA] success");
				}
			}
			else {
				if (args.size() < 4) {
					spdlog::warn(LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_SCRIPT_FAILED_BUT_NO, args.size());
					return;
				}

				std::string errorMsg = args[3];
				spdlog::error(LOG_SCR_LUA_FAIL_FMT, errorMsg);
				KeyBindMenu::LogActivity("[SCR][LUA] fail: " + errorMsg, false);
				if (logScriptResult) {
					AddLogEntry("[SCR][LUA] fail: " + errorMsg);
				}
			}
		}

		void Init() {
			LuaBridge::AddMenuCommand("DoScriptResult", OnDoScriptResult);
		}

		void Draw(bool* p_open) {
			ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_::ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("RadarKeys - Debugger", p_open)) {
				ImGui::End();
				return;
			}

			bool allOn = logBindUnbind && logButtonPress && logScriptResult && logLuaDebug;
			if (ImGui::Checkbox("Log All", &allOn)) {
				logBindUnbind = allOn;
				logButtonPress = allOn;
				logScriptResult = allOn;
				logLuaDebug = allOn;
			}
			ImGui::Separator();
			ImGui::Checkbox("Log key bind / unbind", &logBindUnbind);
			ImGui::Checkbox("Log button presses", &logButtonPress);
			ImGui::Checkbox("Log script run attempts (success/fail, dll-side vs lua-side)", &logScriptResult);
			ImGui::Checkbox("Log script debug messages (RadarKeys.DebugLog)", &logLuaDebug);
			ImGui::TextDisabled("These only control what's shown live below - everything is always written to radarkeys_log.txt regardless.");

			ImGui::Separator();
			if (ImGui::Button("Clear Log")) {
				std::lock_guard<std::mutex> lock(g_logMutex);
				logEntries.clear();
			}

			std::vector<LogEntry> logSnapshot;
			{
				std::lock_guard<std::mutex> lock(g_logMutex);
				logSnapshot.assign(logEntries.begin(), logEntries.end());
			}
			ImGui::BeginChild("DebuggerLog", ImVec2(0, 0), true);
			for (const LogEntry& entry : logSnapshot) {
				ImGui::TextWrapped("[%s] %s", entry.timestamp.c_str(), entry.text.c_str());
			}

			if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
				ImGui::SetScrollHereY(1.0f);
			}
			ImGui::EndChild(); ImGui::End();
		}
	}
}
