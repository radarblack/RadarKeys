#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	namespace DebuggerMenu {
		extern bool logBindUnbind;
		extern bool logButtonPress;
		extern bool logScriptResult;
		extern bool logLuaDebug;
		extern bool menuOpen;

		void Init();
		void Draw(bool* p_open);
		void LogBindEvent(const std::string& message);
		void LogButtonPress(const std::string& message);
		void LogLuaDebug(const std::string& message);
		bool LogScriptAttempt(const std::string& scriptPath);
	}
}
