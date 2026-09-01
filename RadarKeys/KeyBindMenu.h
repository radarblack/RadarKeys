#pragma once
#include "windowsapi.h"
#include "RawInput.h"
#include <string>
#include <vector>

namespace RadarKeys {
	namespace KeyBindMenu {
		struct KeyBind {
			USHORT vKey;
			bool needCtrl;
			bool needShift;
			bool needAlt;
			std::string keyName;     // base key display name, e.g. "F6", "A", "," - see vkNameTable
			
			bool isToggle = false;
			std::string scriptPathOn;  // for toggle On
			std::string scriptPathOff; // for toggle Off
			mutable bool toggleState = false; // checks toggle state
			
			float holdSeconds = 0.0f; // 0 = fires instantly on press (default/original behavior); >0 = fires once after being held this long, see Update()

			// lua script pass
			std::string functionOn;   // toggle ON slot
			std::string functionOff;  // toggle OFF slot
			std::string functionTap;  // instant tap / long press slot
		};

		// loads bindings and menu key at startup from mod/radarKeys/radar_keybinds.conf; uses hardcoded F7 as fallback if nopersisted menu key.
		void Init(const std::string& defaultMenuKeyName);

		// Draws key-bindings window; call only when menu open (ImGui p_open pattern).
		void Draw(bool* p_open);

		// call once per frame when the key is held (?). Doubt if this works. Hopefully. lol
		void Update();

		extern std::vector<KeyBind> bindings;
		extern bool menuOpen;
	}
}
