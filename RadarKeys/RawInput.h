#pragma once
#include "windowsapi.h"
#include <functional>
#include <vector>

namespace RadarKeys {
	namespace RawInput {
		enum BUTTONEVENT {
			UP,
			ONDOWN,
			ONUP,
			HELD
		};

		constexpr USHORT kMaxVKey = 0x180;

		bool IsKeyHeldReal(USHORT vKey);
		bool ProcessMouseButtons(PRAWINPUT pRaw);
		typedef std::function<void(BUTTONEVENT buttonEvent)> ButtonAction;
		typedef unsigned long long ActionHandle;
		void InitializeInput();
		ActionHandle RegisterAction(USHORT vKey, ButtonAction action);
		void UnRegisterAction(USHORT vKey);
		void UnRegisterAction(USHORT vKey, ActionHandle handle);
		bool IsKeyDown(USHORT vKey);
		bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
		void BlockMouseClick(); 
		void UnBlockMouseClick();
		void BlockKeyboard();
		void UnBlockKeyboard();
		void PollGamepad();
		bool IsAnyGamepadConnected();
		bool HasXInputGamepad();
		const std::vector<USHORT>& GamepadVKeys();
		void SetGamepadBlockedToGame(bool blocked);
		bool IsGamepadBlockedToGame();
		bool IsKeyboardBlockedToGame();
		bool IsMouseBlockedToGame();
		void OnFocusLost();
	}
}
