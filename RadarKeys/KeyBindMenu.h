#pragma once
#include "windowsapi.h"
#include "RawInput.h"
#include <string>
#include <vector>

namespace RadarKeys {
	extern bool showCapturePrompt;

	namespace KeyBindMenu {
		struct KeyBind {
			USHORT vKey;
			bool needCtrl;
			bool needShift;
			bool needAlt;
			std::string keyName;
			bool isToggle = false;
			std::string scriptPathOn;  // for toggle On
			std::string scriptPathOff; // for toggle Off
			mutable bool toggleState = false; // checks toggle state
			float holdSeconds = 0.0f;

			// lua script pass
			std::string functionOn;   // toggle ON slot
			std::string functionOff;  // toggle OFF slot
			std::string functionTap;  // instant tap / long press slot
		};
		void Init(const std::string& defaultMenuKeyName);
		void Draw(bool* p_open);
		void Update();
		void LogCleanShutdown();
		void LogActivity(const std::string& message, bool success = true);

		std::string NameForVKey(USHORT vKey);
		int VKeyForName(const std::string& name);

		extern std::vector<KeyBind> bindings;
		extern bool menuOpen;
	}
}
