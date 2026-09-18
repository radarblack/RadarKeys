#pragma once
#include "windowsapi.h"

namespace RadarKeys {
	namespace DirectInputHook {
		void Install();
		void Poll(HWND hwnd);
		void Shutdown();
		bool IsGamepadButtonHeld(USHORT vKey);
		bool IsPlaystationKeyHeld(USHORT vKey);
		bool HasJoystickDevice();
		bool HasPlaystationDevice();
	}
}
