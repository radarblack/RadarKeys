#include "DebuggerMenu.h"
#include "LuaBridge.h"
#include "spdlog/spdlog.h"
#include "imgui/imgui.h"

#include <filesystem>
#include <deque>
#include <ctime>

namespace RadarKeys {
	namespace DebuggerMenu {
		bool logBindUnbind = false;
		bool logButtonPress = false;
		bool logScriptResult = false;
		bool menuOpen = false;

		struct LogEntry {
			std::string timestamp;
			std::string text;
		};
		const size_t maxLogEntries = 300;
		std::deque<LogEntry> logEntries;

		std::string CurrentTimestamp() {
			auto duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::duration<double>(ImGui::GetTime()));
			auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
			duration -= hours;
			auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
			duration -= minutes;

			char timestr[32];
			snprintf(timestr, sizeof(timestr), "%02lld:%02lld:%02lld", hours.count(), minutes.count(), duration.count());
			return std::string(timestr);
		}

		void AddLogEntry(std::string text) {
			logEntries.emplace_back(LogEntry{ CurrentTimestamp(), std::move(text) });
			
			while (logEntries.size() > maxLogEntries) {
				logEntries.pop_front();
			}
		}

		void LogBindEvent(const std::string& message) {
			spdlog::info("[BND] {}", message);
			if (!logBindUnbind) return;
			AddLogEntry("[BND] " + message);
		}

		void LogButtonPress(const std::string& message) {
			if (!logButtonPress) return;
			spdlog::debug("[BTN] {}", message);
			AddLogEntry("[BTN] " + message);
		}

		bool LogScriptAttempt(const std::string& scriptPath) {
			bool exists = std::filesystem::exists(scriptPath);
			if (!exists) {
				spdlog::error("[SCR][DLL] missing - SKIP: {}", scriptPath);
				if (logScriptResult) {
					AddLogEntry("[SCR][DLL] missing - SKIP: " + scriptPath);
				}
				return false;
			}
			spdlog::debug("[SCR][DLL] attempt: {}", scriptPath);
			if (logScriptResult) {
				AddLogEntry("[SCR][DLL] attempt: " + scriptPath);
			}
			return true;
		}

		void OnDoScriptResult(std::vector<std::string> args) {
			if (args.size() < 3) {
				spdlog::warn("DebuggerMenu::OnDoScriptResult: malformed args (size {})", args.size());
				return;
			}

			bool success = args[2] == "1";
			if (success) {
				spdlog::debug("[SCR][LUA] success");
				if (logScriptResult) {
					AddLogEntry("[SCR][LUA] success");
				}
			}
			else {
				if (args.size() < 4) {
					spdlog::warn("DebuggerMenu::OnDoScriptResult: script failed but no error message provided (size {})", args.size());
					return;
				}

				std::string errorMsg = args[3];
				spdlog::error("[SCR][LUA] fail: {}", errorMsg);
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

			bool allOn = logBindUnbind && logButtonPress && logScriptResult;
			if (ImGui::Checkbox("Log All", &allOn)) {
				logBindUnbind = allOn;
				logButtonPress = allOn;
				logScriptResult = allOn;
			}
			ImGui::Separator();
			ImGui::Checkbox("Log key bind / unbind", &logBindUnbind);
			ImGui::Checkbox("Log button presses", &logButtonPress);
			ImGui::Checkbox("Log script run attempts (success/fail, dll-side vs lua-side)", &logScriptResult);

			ImGui::Separator();
			if (ImGui::Button("Clear Log")) {
				logEntries.clear();
			}

			ImGui::BeginChild("DebuggerLog", ImVec2(0, 0), true);
			for (const LogEntry& entry : logEntries) {
				ImGui::TextWrapped("[%s] %s", entry.timestamp.c_str(), entry.text.c_str());
			}

			if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
				ImGui::SetScrollHereY(1.0f);
			}
			ImGui::EndChild(); ImGui::End();
		}
	}
}
