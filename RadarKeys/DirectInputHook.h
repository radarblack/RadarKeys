#pragma once
#include "windowsapi.h"

namespace RadarKeys {
	namespace DirectInputHook {
		void Install();
		void InstallEarly();
		void Poll(HWND hwnd);
		void Shutdown();
		bool IsGamepadButtonHeld(USHORT vKey);
		bool IsPlaystationControlHeld(USHORT vKey);
		bool HasJoystickDevice();
		bool HasPlaystationDevice();
		bool IsSonyGamepadAttachedToSystem();
	}
}
