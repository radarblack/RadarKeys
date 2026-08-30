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

		// checks if the long press is valid.
		bool IsKeyHeldReal(USHORT vKey);

		// changed to std::function to allow capturing lambdas for dynamic key bindings while keeping plain function pointers compatible.
		typedef std::function<void(BUTTONEVENT buttonEvent)> ButtonAction;

		// opaque handle from RegisterAction lets UnRegisterAction(vKey, handle) remove just that action when multiple share a vKey.
		typedef unsigned long long ActionHandle;

		void InitializeInput();
		void HookWndProc(HWND hWnd);

		// handle return value ignorable for actions never needing individual removal (e.g., ToggleMenu, MenuOff).
		ActionHandle RegisterAction(USHORT vKey, ButtonAction action);

		void UnRegisterAction(USHORT vKey); //  old behaviour - removes EVERY action on this vKey
		void UnRegisterAction(USHORT vKey, ActionHandle handle); // removes just the one matching action

		// True if vKey held; lets actions check modifiers (e.g., distinguish Z from Shift+Z)
		bool IsKeyDown(USHORT vKey);
		bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

		//		
		void BlockAll();
		void UnBlockAll();
		void BlockMouseClick(); 
		void UnBlockMouseClick();
		void BlockKeyboard();
		void UnBlockKeyboard();
	}
}
