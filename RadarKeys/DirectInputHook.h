#pragma once
#include "windowsapi.h"
#include <cstdint>

namespace RadarKeys {
	namespace DirectInputHook {
		void Install();
		void InstallEarly();
		void Poll(HWND hwnd);
		bool IsPlaystationControlHeld(USHORT vKey);
		bool HasJoystickDevice();
		bool HasPlaystationDevice();
		bool IsSonyGamepadAttachedToSystem();
		void NotifyDeviceListChanged();
		uint32_t GetDeviceListSweepCount();
	}
}
