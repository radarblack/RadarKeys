#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	namespace DebuggerMenu {
		// independent toggles - see Draw()'s "Log All" checkbox, which just sets/clears all three together as a convenience, not a separate fourth mode.
		extern bool logBindUnbind;
		extern bool logButtonPress;
		extern bool logScriptResult;

		extern bool menuOpen; // opened via a button inside KeyBindMenu::Draw(), not its own hotkey

		// call once at startup (see dllmain.cpp init sequence) - registers the "DoScriptResult" command that our companion RadarKeys_Core.lua reports back through after actually attempting to run a script.
		void Init();
		void Draw(bool* p_open);

		// each is a no-op unless its matching logXxx toggle is enabled, so callers (KeyBindMenu) don't need to check the toggle themselves.
		void LogBindEvent(const std::string& message);
		void LogButtonPress(const std::string& message);

		// does the dll-side file-existence check itself, always (not just when logScriptResult
		// is on) - this doubles as validation, not just logging: false means the script file wasn't
		// found on disk, so the caller should NOT queue the DoScript IPC message at all. Returns
		// true if the file exists and the caller should proceed. The eventual success/failure of
		// actually running it arrives later, asynchronously, via the registered DoScriptResult command.
		bool LogScriptAttempt(const std::string& scriptPath);
	}
}
