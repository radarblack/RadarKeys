#pragma once
#include "windowsapi.h"

namespace RadarKeys {
	namespace LuaKeyState {
		bool ButtonDown(USHORT vKey);
		bool OnButtonDown(USHORT vKey);
		bool OnButtonUp(USHORT vKey);
		bool ButtonHeld(USHORT vKey, double holdSecondsOverride = -1.0);
		bool OnButtonHoldTime(USHORT vKey, double holdSecondsOverride = -1.0);
		bool OnButtonRepeat(USHORT vKey);
		double GetRepeatMult(USHORT vKey);
		void ResetRepeat(USHORT vKey);
	}
}
