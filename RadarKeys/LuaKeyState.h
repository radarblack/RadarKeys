#pragma once
#include "windowsapi.h"

namespace RadarKeys {
	namespace LuaKeyState {
		bool ButtonDown(USHORT vKey);
		bool OnButtonDown(USHORT vKey);
		bool OnButtonUp(USHORT vKey);
		bool ButtonHeld(USHORT vKey);
		bool OnButtonHoldTime(USHORT vKey);
		bool OnButtonRepeat(USHORT vKey);
		double GetRepeatMult(USHORT vKey);
		void ResetRepeat(USHORT vKey);
	}
}
