#pragma once
#include "windowsapi.h"

namespace RadarKeys {
	namespace Render {
    // CreateD3DHook: early init thread creates dummy-device D3D11 Present/ResizeBuffers hook; real device untouched; other systems init on first Present.
		void CreateD3DHook();

    // Cursor visible when menus open (computed per-frame); read by SetCursorPos hook to block game's lock/re-center attempts.
		bool IsUnlockCursor();
	}
}
