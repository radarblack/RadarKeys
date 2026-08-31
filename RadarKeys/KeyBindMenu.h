#pragma once
#include "windowsapi.h"
#include "RawInput.h"
#include <string>
#include <vector>

namespace RadarKeys {
	namespace KeyBindMenu {

		enum class BindMode {
			STANDARD = 0, // normal press
			TOGGLE = 1    // on/off switch
		};
		
		struct KeyBind {
			USHORT vKey;
			bool needCtrl;
			bool needShift;
			bool needAlt;
			std::string keyName;     // base key display name, e.g. "F6", "A", "," - see vkNameTable

			BindMode mode = BindMode::STANDARD;

			std::string scriptPath;  // fire script for standard mode
			float holdSeconds = 0.0f; // 0 = Instant tap, >0 = Long press trigger

			std::string toggleOnScriptPath;
			std::string toggleOffScriptPath;
			bool currentToggleState = false;  // runtime tracker
		};

		// loads bindings and menu key at startup from mod/radarKeys/radar_keybinds.conf; uses hardcoded F7 as fallback if nopersisted menu key.
		void Init(const std::string& defaultMenuKeyName);

		// Draws key-bindings window; call only when menu open (ImGui p_open pattern).
		void Draw(bool* p_open);

		// call once per frame when the key is held (?). Doubt if this works. Hopefully. lol
		void Update();

		extern std::vector<KeyBind> bindings;
		extern bool menuOpen; // Render.cpp's OnFrame checks this directly and passes &menuOpen into Draw()
	}
}
