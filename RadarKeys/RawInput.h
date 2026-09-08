#pragma once
#include "windowsapi.h"
#include <functional>

namespace RadarKeys {
	namespace RawInput {
		enum BUTTONEVENT {
			UP,
			ONDOWN,
			ONUP,
			HELD
		};

		bool IsKeyHeldReal(USHORT vKey);
		bool ProcessMouseButtons(PRAWINPUT pRaw);
		typedef std::function<void(BUTTONEVENT buttonEvent)> ButtonAction;
		typedef unsigned long long ActionHandle;
		void InitializeInput();
		void HookWndProc(HWND hWnd);
		ActionHandle RegisterAction(USHORT vKey, ButtonAction action);
		void UnRegisterAction(USHORT vKey);
		void UnRegisterAction(USHORT vKey, ActionHandle handle);
		bool IsKeyDown(USHORT vKey);
		bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
		void BlockAll();
		void UnBlockAll();
		void BlockMouseClick(); 
		void UnBlockMouseClick();
		void BlockKeyboard();
		void UnBlockKeyboard();
	}
}
