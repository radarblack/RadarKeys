#pragma once
#include "windowsapi.h"
#include "RawInput.h"
#include <string>
#include <vector>

namespace RadarKeys {
	namespace KeyBindMenu {

		enum class BindMode {
			STANDARD, // normal press
			TOGGLE    // on/off switch
		};
		// Binding: key(+mods)→Lua file; shared dispatcher per vKey enables fallback (e.g., Shift+X→plain X).
		struct KeyBind {
			USHORT vKey;
			bool needCtrl;
			bool needShift;
			bool needAlt;
			std::string keyName;     // base key display name, e.g. "F6", "A", "," - see vkNameTable

			BindMode mode = BindMode::STANDARD;
			std::string scriptPath;  // fire script for standard mode
			float holdSeconds = 0.0f; // 0 = Instant tap, >0 = Long press trigger

			std::string scriptPath;  // passed to dofile() via the DoScript IPC command (see RegisterBindingAction/OnBoundKeyPressed)
			float holdSeconds = 0.0f; // 0 = fires instantly on press (default/original behavior); >0 = fires once after being held this long, see Update()
			bool currentToggleState = false; // runtime tracker
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
