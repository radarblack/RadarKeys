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
			if (!logBindUnbind) {
				return;
			}
			AddLogEntry("[bind] " + message);
		}

		void LogButtonPress(const std::string& message) {
			if (!logButtonPress) {
				return;
			}
			AddLogEntry("[button] " + message);
		}

		bool LogScriptAttempt(const std::string& scriptPath) {
			// always actually checked (this gates whether the caller queues DoScript at all) -
			// only the LOGGING of it is conditional on the toggle.
			bool exists = std::filesystem::exists(scriptPath);
			if (!exists) {
				if (logScriptResult) {
					AddLogEntry("[script][dll] NOT FOUND, skipping: " + scriptPath);
				}
				return false;
			}
			if (logScriptResult) {
				AddLogEntry("[script][dll] attempting: " + scriptPath);
			}
			return true;
		}

		// registered as the "DoScriptResult" command (see Init) - our companion
		// RadarKeys_Core.lua calls RadarKeys.MenuMessage("DoScriptResult", success, errorMsg) after
		// actually attempting to run a script. args follows LuaBridge's usual layout: args[0]=seq,
		// args[1]=cmd, args[2]=success ("1"/"0"), args[3]=error message (empty string on success).

		// by the time this fires, LogScriptAttempt has already confirmed the file exists
		// (see above) - so any failure reported here is necessarily a lua-side problem (syntax or
		// runtime error inside the script itself), never a missing/bad path.
		void OnDoScriptResult(std::vector<std::string> args) {
			if (args.size() < 1 + 3) {
				spdlog::warn("DebuggerMenu::OnDoScriptResult: malformed args (size {})", args.size());
				return;
			}
			if (!logScriptResult) {
				return;
			}

			bool success = args[2] == "1";
			if (success) {
				AddLogEntry("[script][lua] success");
			}
			else {
				std::string errorMsg = args[3];
				AddLogEntry("[script][lua] FAILED: " + errorMsg);
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
