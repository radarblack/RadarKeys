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
		static const char* LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_MALFORMED_ARGS_SIZE_FM = "DebuggerMenu::OnDoScriptResult: malformed args (size {})";
		static const char* UI_DEBUGGER_LOG_SCOPE_HINT = "These control both the live view below and what gets written to radarkeys_log.txt - tick a box to record its category, untick to silence it.";
		static const char* LOG_SCR_LUA_SUCCESS = "[SCR][LUA] success";
		static const char* LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_SCRIPT_FAILED_BUT_NO = "DebuggerMenu::OnDoScriptResult: script failed but no error message provided (size {})";

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
			if (!logBindUnbind) return;
			KeyBindMenu::LogActivity("[BND] " + message);
			AddLogEntry("[BND] " + message);
		}

		void LogButtonPress(const std::string& message) {
			if (!logButtonPress) return;
			KeyBindMenu::LogActivity("[BTN] " + message);
			AddLogEntry("[BTN] " + message);
		}

		void LogLuaDebug(const std::string& message) {
			if (!logLuaDebug) return;
			KeyBindMenu::LogActivity("[LUA] " + message);
			AddLogEntry("[LUA] " + message);
		}

		bool LogScriptAttempt(const std::string& scriptPath) {
			bool exists = std::filesystem::exists(scriptPath);
			if (!logScriptResult) {
				return exists;
			}
			if (!exists) {
				KeyBindMenu::LogActivity("[SCR][DLL] missing - SKIP: " + scriptPath, false);
				AddLogEntry("[SCR][DLL] missing - SKIP: " + scriptPath);
				return false;
			}
			KeyBindMenu::LogActivity("[SCR][DLL] attempt: " + scriptPath);
			AddLogEntry("[SCR][DLL] attempt: " + scriptPath);
			return true;
		}

		void OnDoScriptResult(std::vector<std::string> args) {
			if (args.size() < 3) {
				spdlog::warn(LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_MALFORMED_ARGS_SIZE_FM, args.size());
				return;
			}

			if (!logScriptResult) {
				return;
			}
			bool success = args[2] == "1";
			if (success) {
				KeyBindMenu::LogActivity(LOG_SCR_LUA_SUCCESS);
				AddLogEntry("[SCR][LUA] success");
			}
			else {
				if (args.size() < 4) {
					spdlog::warn(LOG_DEBUGGERMENU_ONDOSCRIPTRESULT_SCRIPT_FAILED_BUT_NO, args.size());
					return;
				}

				std::string errorMsg = args[3];
				KeyBindMenu::LogActivity("[SCR][LUA] fail: " + errorMsg, false);
				AddLogEntry("[SCR][LUA] fail: " + errorMsg);
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
			ImGui::TextDisabled(UI_DEBUGGER_LOG_SCOPE_HINT);

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
