#pragma once
#include "windowsapi.h"

namespace RadarKeys {
	namespace DirectInputHook {
		void Install();
		bool IsGamepadButtonHeld(USHORT vKey);
		bool HasJoystickDevice();
		bool HasPlaystationDevice();
	}
}
