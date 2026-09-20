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

		// binding the PS keys the way it VKs are recognized by the RawInput. please work. LMAO
		constexpr USHORT VK_PS_CROSS      = 0x0100;
		constexpr USHORT VK_PS_CIRCLE     = 0x0101;
		constexpr USHORT VK_PS_SQUARE     = 0x0102;
		constexpr USHORT VK_PS_TRIANGLE   = 0x0103;
		constexpr USHORT VK_PS_L1         = 0x0104;
		constexpr USHORT VK_PS_R1         = 0x0105;
		constexpr USHORT VK_PS_L2         = 0x0106;
		constexpr USHORT VK_PS_R2         = 0x0107;
		constexpr USHORT VK_PS_SHARE      = 0x0108;
		constexpr USHORT VK_PS_OPTIONS    = 0x0109;
		constexpr USHORT VK_PS_L3         = 0x010A;
		constexpr USHORT VK_PS_R3         = 0x010B;
		constexpr USHORT VK_PS_DPAD_UP    = 0x010C;
		constexpr USHORT VK_PS_DPAD_DOWN  = 0x010D;
		constexpr USHORT VK_PS_DPAD_LEFT  = 0x010E;
		constexpr USHORT VK_PS_DPAD_RIGHT = 0x010F;
		constexpr USHORT VK_PS_LS_UP      = 0x0110;
		constexpr USHORT VK_PS_LS_DOWN    = 0x0111;
		constexpr USHORT VK_PS_LS_LEFT    = 0x0112;
		constexpr USHORT VK_PS_LS_RIGHT   = 0x0113;
		constexpr USHORT VK_PS_RS_UP      = 0x0114;
		constexpr USHORT VK_PS_RS_DOWN    = 0x0115;
		constexpr USHORT VK_PS_RS_LEFT    = 0x0116;
		constexpr USHORT VK_PS_RS_RIGHT   = 0x0117;

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
		void PollPlaystation();
		bool IsAnyGamepadConnected();
		bool HasXInputGamepad();
		const std::vector<USHORT>& GamepadVKeys();
		const std::vector<USHORT>& PlaystationVKeys();
		bool IsKeyboardBlockedToGame();
		bool IsMouseBlockedToGame();
		void OnFocusLost();
	}
}
