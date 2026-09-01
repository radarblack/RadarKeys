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
		const size_t maxLogEntries = 300; // capped ring buffer - this is an in-game window, not a file, no need to keep an unbounded session-long history
		std::deque<LogEntry> logEntries;

		// matches the time-formatting pattern already used elsewhere for startup log headers
		std::string CurrentTimestamp() {
			std::time_t currentTime = time(0);
			std::tm now;
			localtime_s(&now, &currentTime);
			char timestr[16];
			std::strftime(timestr, sizeof(timestr), "%H:%M:%S", &now);
			return std::string(timestr);
		}

		void AddLogEntry(const std::string& text) {
			logEntries.push_back(LogEntry{ CurrentTimestamp(), text });
			if (logEntries.size() > maxLogEntries) {
				logEntries.pop_front();
			}
		}

		void LogBindEvent(const std::string& message) {
			spdlog::info("[BND] {}", message);
			if (!logBindUnbind) {
				return;
			}
			AddLogEntry("[BND] " + message);
		}

		void LogButtonPress(const std::string& message) {
			if (!logButtonPress) {
				return;
			}
			spdlog::debug("[BTN] {}", message);
			AddLogEntry("[BTN] " + message);
		}

		bool LogScriptAttempt(const std::string& scriptPath) {
			bool exists = std::filesystem::exists(scriptPath);
			if (!exists) {
				spdlog::error("[SCR][DLL] NOT FOUND, SKIP: {}", scriptPath);
				if (logScriptResult) {
					AddLogEntry("[SCR][DLL] NOT FOUND - SKIP: " + scriptPath);
				}
				return false;
			}
			spdlog::debug("[SCR][DLL] ATTEMPT: {}", scriptPath);
			if (logScriptResult) {
				AddLogEntry("[SCR][DLL] ATTEMPT: " + scriptPath);
			}
			return true;
		}

		void OnDoScriptResult(std::vector<std::string> args) {
			// A valid success return needs at least 3 arguments ("DoScriptResult", identifier, successFlag)
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
				// A failure return requires a 4th argument containing the error text string
				if (args.size() < 4) {
					spdlog::warn("DebuggerMenu::OnDoScriptResult: script failed but no error message provided (size {})", args.size());
					return;
				}

				std::string errorMsg = args[3];
				spdlog::error("[SCR][LUA] FAILED: {}", errorMsg);
				if (logScriptResult) {
					AddLogEntry("[SCR][LUA] FAILED: " + errorMsg);
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

			// "Log All" reads as checked only when all three already are, and just sets/clears
			// all three together when clicked - not an independent fourth state of its own.
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
			// auto-scroll only while already at the bottom, so scrolling up to read history
			// isn't constantly yanked back down by new entries arriving.
			if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
				ImGui::SetScrollHereY(1.0f);
			}
			ImGui::EndChild();

			ImGui::End();
		}
	}
}
