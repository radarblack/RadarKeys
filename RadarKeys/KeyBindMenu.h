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
			std::string scriptPathOn;
			std::string scriptPathOff;
			mutable bool toggleState = false;
			float holdSeconds = 0.0f;
			bool isInstant = false;
			int instantTriggerType = 0;
			float repeatAccelMult = 1.0f;
			double runtimeRepeatSpeedMult = 1.0;
			bool disabled = false;

			// lua script pass
			std::string functionOn;
			std::string functionOff;
			std::string functionTap;

			std::vector<USHORT> comboKeys;
			bool comboActive = false;
			bool comboHoldFired = false;
			bool comboTapFired = false;
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

		std::vector<USHORT> ParseComboKeyNames(const std::string& raw);

		extern std::vector<KeyBind> bindings;
		extern bool menuOpen;
	}
}
