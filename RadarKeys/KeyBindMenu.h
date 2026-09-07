#pragma once
#include "windowsapi.h"
#include "RawInput.h"
#include <string>
#include <vector>
#include <chrono>

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
			bool isInstant = false;
			int instantTriggerType = 0;

			// lua script pass
			std::string functionOn;   // toggle ON slot
			std::string functionOff;  // toggle OFF slot
			std::string functionTap;  // instant tap / long press slot

			std::vector<USHORT> comboKeys;
			bool comboActive = false;    // true while every key in comboKeys is currently held
			bool comboHoldFired = false; // Long Press threshold already fired for this hold
			bool comboTapFired = false;  // tap/press-time fire already happened for this hold
			std::chrono::steady_clock::time_point comboPressTime;
			std::chrono::steady_clock::time_point comboLastRepeatTime;

			bool IsCombo() const { return comboKeys.size() >= 2; }
		};
		void Init(const std::string& defaultMenuKeyName);
		void Draw(bool* p_open);
		void Update();
		void LogCleanShutdown();
		void LogActivity(const std::string& message, bool success = true);
		void SaveBindings();

		std::string NameForVKey(USHORT vKey);
		int VKeyForName(const std::string& name);

		extern std::vector<KeyBind> bindings;
		extern bool menuOpen;
	}
}
