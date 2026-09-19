//DEBUGNOW this only really gets you OnKeyDown, OnKeyUp reliably as Held will be limited by key repeat rate
//the solution there would be to have another state array and have the input events set up,down and querry that with the assumption that down is held

#include "RawInput.h"
#include "DirectInputHook.h"
#include "spdlog/spdlog.h"
#include <MinHook.h>
#include <Xinput.h>
#include <cstdlib>
#include <algorithm>
#include <utility>
#include <mutex>
#include <vector>
#include <atomic>
#include <list>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>

namespace RadarKeys {
	namespace RawInput {
		const USHORT vKeyMax = kMaxVKey;
		USHORT currFlags[vKeyMax]; // indexed by Virtual Keycode
		namespace { struct CurrFlagsFiller { CurrFlagsFiller() { std::fill_n(currFlags, vKeyMax, static_cast<USHORT>(RI_KEY_BREAK)); } }; }
		static CurrFlagsFiller g_currFlagsFiller;
		bool ignore[vKeyMax] = { false }; // don't process key, set up in InitIgnoreKeys (written once, before input starts)
		std::atomic<unsigned char> blockGameKeys[vKeyMax]{}; // block game from recieving message
		std::atomic<unsigned char> realStateHeld[vKeyMax]{};
		std::atomic<unsigned char> g_keyboardBlockedToGame{ false };
		std::atomic<unsigned char> g_mouseBlockedToGame{ false };
		std::atomic<unsigned char> g_gamepadBlockedToGame{ false };

		bool IsKeyboardBlockedToGame() { return g_keyboardBlockedToGame.load() != false; }
		bool IsMouseBlockedToGame() { return g_mouseBlockedToGame.load() != false; }
		bool IsGamepadBlockedToGame() { return g_gamepadBlockedToGame.load() != false; }

		void SetGamepadBlockedToGame(bool blocked) { g_gamepadBlockedToGame.store(blocked ? 1 : 0); }

		std::list<std::pair<ActionHandle, ButtonAction>>* buttonActions[vKeyMax] = { nullptr };
		ActionHandle nextActionHandle = 1;
		std::recursive_mutex g_actionMutex;

		void BlockMouseClick() {
			blockGameKeys[VK_LBUTTON] = true;
			blockGameKeys[VK_RBUTTON] = true;
			blockGameKeys[VK_MBUTTON] = true;
			blockGameKeys[VK_XBUTTON1] = true;
			blockGameKeys[VK_XBUTTON2] = true;
			g_mouseBlockedToGame.store(1);
		}

		void UnBlockMouseClick() {
			blockGameKeys[VK_LBUTTON] = false;
			blockGameKeys[VK_RBUTTON] = false;
			blockGameKeys[VK_MBUTTON] = false;
			blockGameKeys[VK_XBUTTON1] = false;
			blockGameKeys[VK_XBUTTON2] = false;
			g_mouseBlockedToGame.store(0);
		}

		void BlockKeyboard() {
			for (USHORT i = VK_BACK; i < 256; i++) {
				blockGameKeys[i] = true;
			}
			blockGameKeys[VK_ESCAPE] = false;
			g_keyboardBlockedToGame.store(1);
		}

		void UnBlockKeyboard() {
			for (USHORT i = VK_BACK; i < 256; i++) {
				blockGameKeys[i] = false;
			}
			g_keyboardBlockedToGame.store(0);
		}

		const std::vector<USHORT>& GamepadVKeys() {
			static const std::vector<USHORT> keys = {
				VK_GAMEPAD_A, VK_GAMEPAD_B, VK_GAMEPAD_X, VK_GAMEPAD_Y,
				VK_GAMEPAD_LEFT_SHOULDER, VK_GAMEPAD_RIGHT_SHOULDER,
				VK_GAMEPAD_LEFT_TRIGGER, VK_GAMEPAD_RIGHT_TRIGGER,
				VK_GAMEPAD_DPAD_UP, VK_GAMEPAD_DPAD_DOWN, VK_GAMEPAD_DPAD_LEFT, VK_GAMEPAD_DPAD_RIGHT,
				VK_GAMEPAD_MENU, VK_GAMEPAD_VIEW,
				VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON, VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON,
				VK_GAMEPAD_LEFT_THUMBSTICK_UP, VK_GAMEPAD_LEFT_THUMBSTICK_DOWN,
				VK_GAMEPAD_LEFT_THUMBSTICK_LEFT, VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT,
				VK_GAMEPAD_RIGHT_THUMBSTICK_UP, VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN,
				VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT, VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT,
			};
			return keys;
		}

		const std::vector<USHORT>& PlaystationVKeys() {
			static const std::vector<USHORT> keys = {
				VK_PS_CROSS, VK_PS_CIRCLE, VK_PS_SQUARE, VK_PS_TRIANGLE,
				VK_PS_L1, VK_PS_R1, VK_PS_L2, VK_PS_R2,
				VK_PS_SHARE, VK_PS_OPTIONS, VK_PS_L3, VK_PS_R3,
				VK_PS_DPAD_UP, VK_PS_DPAD_DOWN, VK_PS_DPAD_LEFT, VK_PS_DPAD_RIGHT,
				VK_PS_LS_UP, VK_PS_LS_DOWN, VK_PS_LS_LEFT, VK_PS_LS_RIGHT,
				VK_PS_RS_UP, VK_PS_RS_DOWN, VK_PS_RS_LEFT, VK_PS_RS_RIGHT,
			};
			return keys;
		}

		void DoActions(USHORT vKey, RawInput::BUTTONEVENT buttonEvent);

		typedef DWORD(WINAPI* XInputGetStateFunc)(DWORD, XINPUT_STATE*);
		static constexpr int kMaxXInputModules = 5;
		static XInputGetStateFunc g_origXInputGetState[kMaxXInputModules] = {};
		static XInputGetStateFunc g_origXInputGetStateEx[kMaxXInputModules] = {};
		static int g_xinputModuleCount = 0;
		static int g_xinputExModuleCount = 0;
		static const char* g_xinputSlotNames[kMaxXInputModules] = {};
		static const char* g_xinputExSlotNames[kMaxXInputModules] = {};
		static std::unordered_set<void*> g_xinputHookedTargets;
		static std::unordered_set<void*> g_xinputFailedTargets;
		static std::unordered_set<void*> g_xinputExHookedTargets;
		static std::unordered_set<void*> g_xinputExFailedTargets;

		template <int N>
		DWORD WINAPI HookedXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
			static std::atomic<bool> firstCallLogged{ false };
			DWORD result = ERROR_DEVICE_NOT_CONNECTED;
			if (g_origXInputGetState[N]) {
				result = g_origXInputGetState[N](dwUserIndex, pState);
			}
			if (!firstCallLogged.exchange(true)) {
				spdlog::info("RawInput: game called XInputGetState (slot {}, module {}, userIndex {}, result {})",
					N, g_xinputSlotNames[N] ? g_xinputSlotNames[N] : "?", dwUserIndex, result);
				spdlog::default_logger()->flush();
			}
			if (result == ERROR_SUCCESS && pState && g_gamepadBlockedToGame.load()) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			return result;
		}

		template <int N>
		DWORD WINAPI HookedXInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE* pState) {
			static std::atomic<bool> firstCallLogged{ false };
			DWORD result = ERROR_DEVICE_NOT_CONNECTED;
			if (g_origXInputGetStateEx[N]) {
				result = g_origXInputGetStateEx[N](dwUserIndex, pState);
			}
			if (!firstCallLogged.exchange(true)) {
				spdlog::info("RawInput: game called XInputGetStateEx (slot {}, module {}, userIndex {}, result {})",
					N, g_xinputExSlotNames[N] ? g_xinputExSlotNames[N] : "?", dwUserIndex, result);
				spdlog::default_logger()->flush();
			}
			if (result == ERROR_SUCCESS && pState && g_gamepadBlockedToGame.load()) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			return result;
		}

		static XInputGetStateFunc g_xinputDetours[kMaxXInputModules] = {
			&HookedXInputGetState<0>, &HookedXInputGetState<1>,
			&HookedXInputGetState<2>, &HookedXInputGetState<3>,
			&HookedXInputGetState<4>,
		};

		static XInputGetStateFunc g_xinputExDetours[kMaxXInputModules] = {
			&HookedXInputGetStateEx<0>, &HookedXInputGetStateEx<1>,
			&HookedXInputGetStateEx<2>, &HookedXInputGetStateEx<3>,
			&HookedXInputGetStateEx<4>,
		};

		void EnsureXInputHook() {
			static const wchar_t* kModuleNames[] = {
				L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll"
			};
			static const char* kModuleNamesNarrow[] = {
				"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"
			};
			static bool loadAttemptedFor[kMaxXInputModules] = {};

			for (int nameIndex = 0; nameIndex < kMaxXInputModules; ++nameIndex) {
				const wchar_t* moduleName = kModuleNames[nameIndex];
				const char* moduleNameNarrow = kModuleNamesNarrow[nameIndex];
				HMODULE module = GetModuleHandleW(moduleName);
				if (!module && !loadAttemptedFor[nameIndex]) {
					loadAttemptedFor[nameIndex] = true;
					module = LoadLibraryW(moduleName);
				}
				if (!module) {
					continue;
				}
				wchar_t modulePathW[MAX_PATH] = L"";
				GetModuleFileNameW(module, modulePathW, MAX_PATH);
				std::string modulePath = std::filesystem::path(modulePathW).string();
				void* target = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
				if (target && g_xinputHookedTargets.count(target) == 0 &&
					g_xinputFailedTargets.count(target) == 0 &&
					g_xinputModuleCount < kMaxXInputModules) {
					int slot = g_xinputModuleCount;
					XInputGetStateFunc* origSlot = &g_origXInputGetState[slot];
					if (MH_CreateHook(target, reinterpret_cast<LPVOID>(g_xinputDetours[slot]),
						reinterpret_cast<LPVOID*>(origSlot)) == MH_OK &&
						MH_EnableHook(target) == MH_OK) {
						g_xinputHookedTargets.insert(target);
						g_xinputSlotNames[slot] = moduleNameNarrow;
						++g_xinputModuleCount;
						spdlog::info("RawInput: hooked XInputGetState in {} (loaded from {}) for gamepad suppression ({} module(s))",
							moduleNameNarrow, modulePath, g_xinputModuleCount);
					} else {
						MH_RemoveHook(target);
						g_xinputFailedTargets.insert(target);
						spdlog::warn("RawInput: failed to hook XInputGetState in {} (loaded from {})",
							moduleNameNarrow, modulePath);
					}
				}

				void* targetEx = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetStateEx"));
				if (!targetEx) {
					targetEx = reinterpret_cast<void*>(GetProcAddress(module, reinterpret_cast<LPCSTR>(100)));
				}
				if (targetEx && g_xinputExHookedTargets.count(targetEx) == 0 &&
					g_xinputExFailedTargets.count(targetEx) == 0 &&
					g_xinputExModuleCount < kMaxXInputModules) {
					int slotEx = g_xinputExModuleCount;
					XInputGetStateFunc* origSlotEx = &g_origXInputGetStateEx[slotEx];
					if (MH_CreateHook(targetEx, reinterpret_cast<LPVOID>(g_xinputExDetours[slotEx]),
						reinterpret_cast<LPVOID*>(origSlotEx)) == MH_OK &&
						MH_EnableHook(targetEx) == MH_OK) {
						g_xinputExHookedTargets.insert(targetEx);
						g_xinputExSlotNames[slotEx] = moduleNameNarrow;
						++g_xinputExModuleCount;
						spdlog::info("RawInput: hooked XInputGetStateEx in {} (loaded from {}) for gamepad suppression ({} module(s))",
							moduleNameNarrow, modulePath, g_xinputExModuleCount);
					} else {
						MH_RemoveHook(targetEx);
						g_xinputExFailedTargets.insert(targetEx);
						spdlog::warn("RawInput: failed to hook XInputGetStateEx in {} (loaded from {})",
							moduleNameNarrow, modulePath);
					}
				}
			}
		}

		std::atomic<bool> g_anyGamepadConnected{ false };
		std::atomic<bool> g_xinputGamepadConnected{ false };
		std::atomic<bool> g_psBridgeActive{ false };
		void PollGamepad() {
			EnsureXInputHook();

			WORD buttons = 0;
			BYTE leftTrigger = 0, rightTrigger = 0;
			SHORT lx = 0, ly = 0, rx = 0, ry = 0;
			bool anyConnected = false;
			bool xinputConnected = false;

			const bool bridgeActive = g_psBridgeActive.load(std::memory_order_relaxed);
			for (int m = 0; !bridgeActive && m < g_xinputModuleCount; ++m) {
				for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
					XINPUT_STATE s{};
					XInputGetStateFunc orig = g_origXInputGetState[m];
					if (!orig || orig(i, &s) != ERROR_SUCCESS) {
						continue;
					}
				anyConnected = true;
				xinputConnected = true;
				buttons |= s.Gamepad.wButtons;
				leftTrigger = (std::max)(leftTrigger, s.Gamepad.bLeftTrigger);
				rightTrigger = (std::max)(rightTrigger, s.Gamepad.bRightTrigger);
				if (abs((int)s.Gamepad.sThumbLX) > abs((int)lx)) lx = s.Gamepad.sThumbLX;
				if (abs((int)s.Gamepad.sThumbLY) > abs((int)ly)) ly = s.Gamepad.sThumbLY;
				if (abs((int)s.Gamepad.sThumbRX) > abs((int)rx)) rx = s.Gamepad.sThumbRX;
				if (abs((int)s.Gamepad.sThumbRY) > abs((int)ry)) ry = s.Gamepad.sThumbRY;
				}
			}

			anyConnected = anyConnected || DirectInputHook::HasJoystickDevice();
			for (USHORT gpKey : GamepadVKeys()) {
				if (!DirectInputHook::IsGamepadButtonHeld(gpKey)) {
					continue;
				}
				switch (gpKey) {
				case VK_GAMEPAD_A:                       buttons |= XINPUT_GAMEPAD_A; break;
				case VK_GAMEPAD_B:                       buttons |= XINPUT_GAMEPAD_B; break;
				case VK_GAMEPAD_X:                       buttons |= XINPUT_GAMEPAD_X; break;
				case VK_GAMEPAD_Y:                       buttons |= XINPUT_GAMEPAD_Y; break;
				case VK_GAMEPAD_LEFT_SHOULDER:           buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
				case VK_GAMEPAD_RIGHT_SHOULDER:          buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
				case VK_GAMEPAD_DPAD_UP:                 buttons |= XINPUT_GAMEPAD_DPAD_UP; break;
				case VK_GAMEPAD_DPAD_DOWN:               buttons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
				case VK_GAMEPAD_DPAD_LEFT:               buttons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
				case VK_GAMEPAD_DPAD_RIGHT:              buttons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
				case VK_GAMEPAD_MENU:                    buttons |= XINPUT_GAMEPAD_START; break;
				case VK_GAMEPAD_VIEW:                    buttons |= XINPUT_GAMEPAD_BACK; break;
				case VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON:  buttons |= XINPUT_GAMEPAD_LEFT_THUMB; break;
				case VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON: buttons |= XINPUT_GAMEPAD_RIGHT_THUMB; break;
				case VK_GAMEPAD_LEFT_TRIGGER:            leftTrigger = (std::max)(leftTrigger, (BYTE)255); break;
				case VK_GAMEPAD_RIGHT_TRIGGER:           rightTrigger = (std::max)(rightTrigger, (BYTE)255); break;
				case VK_GAMEPAD_LEFT_THUMBSTICK_UP:      ly = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 1; break;
				case VK_GAMEPAD_LEFT_THUMBSTICK_DOWN:    ly = -(SHORT)(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 1); break;
				case VK_GAMEPAD_LEFT_THUMBSTICK_LEFT:    lx = -(SHORT)(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 1); break;
				case VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT:   lx = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 1; break;
				case VK_GAMEPAD_RIGHT_THUMBSTICK_UP:     ry = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE + 1; break;
				case VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN:   ry = -(SHORT)(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE + 1); break;
				case VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT:   rx = -(SHORT)(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE + 1); break;
				case VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT:  rx = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE + 1; break;
				}
			}

			const std::pair<USHORT, bool> nowState[] = {
				{ VK_GAMEPAD_A,                       (buttons & XINPUT_GAMEPAD_A) != 0 },
				{ VK_GAMEPAD_B,                       (buttons & XINPUT_GAMEPAD_B) != 0 },
				{ VK_GAMEPAD_X,                       (buttons & XINPUT_GAMEPAD_X) != 0 },
				{ VK_GAMEPAD_Y,                       (buttons & XINPUT_GAMEPAD_Y) != 0 },
				{ VK_GAMEPAD_RIGHT_SHOULDER,          (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0 },
				{ VK_GAMEPAD_LEFT_SHOULDER,           (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0 },
				{ VK_GAMEPAD_LEFT_TRIGGER,            leftTrigger  > XINPUT_GAMEPAD_TRIGGER_THRESHOLD },
				{ VK_GAMEPAD_RIGHT_TRIGGER,           rightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD },
				{ VK_GAMEPAD_DPAD_UP,                 (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0 },
				{ VK_GAMEPAD_DPAD_DOWN,               (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 },
				{ VK_GAMEPAD_DPAD_LEFT,               (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 },
				{ VK_GAMEPAD_DPAD_RIGHT,              (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 },
				{ VK_GAMEPAD_MENU,                    (buttons & XINPUT_GAMEPAD_START) != 0 },
				{ VK_GAMEPAD_VIEW,                    (buttons & XINPUT_GAMEPAD_BACK) != 0 },
				{ VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON,  (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 },
				{ VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON, (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0 },
				{ VK_GAMEPAD_LEFT_THUMBSTICK_UP,      ly >  XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE },
				{ VK_GAMEPAD_LEFT_THUMBSTICK_DOWN,    ly < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE },
				{ VK_GAMEPAD_LEFT_THUMBSTICK_LEFT,    lx < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE },
				{ VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT,   lx >  XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE },
				{ VK_GAMEPAD_RIGHT_THUMBSTICK_UP,     ry >  XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE },
				{ VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN,   ry < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE },
				{ VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT,   rx < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE },
				{ VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT,  rx >  XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE },
			};

			for (const auto& entry : nowState) {
				USHORT vKey = entry.first;
				bool isDown = entry.second;
				bool wasDown = realStateHeld[vKey];
				if (isDown == wasDown) {
					continue;
				}
				realStateHeld[vKey] = isDown;
				currFlags[vKey] = isDown ? RI_KEY_MAKE : RI_KEY_BREAK;
				DoActions(vKey, isDown ? BUTTONEVENT::ONDOWN : BUTTONEVENT::ONUP);
			}

			g_anyGamepadConnected = anyConnected;
			g_xinputGamepadConnected = xinputConnected;
		}

		void PollPlaystation() {
			DirectInputHook::Poll(nullptr);

			WORD buttons = 0;
			BYTE leftTrigger = 0, rightTrigger = 0;
			SHORT lx = 0, ly = 0, rx = 0, ry = 0;
			int connectedSlots = 0;
			bool slotSeen[XUSER_MAX_COUNT] = {};

			EnsureXInputHook();
			for (int m = 0; m < g_xinputModuleCount; ++m) {
				for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
					XINPUT_STATE s{};
					XInputGetStateFunc orig = g_origXInputGetState[m];
					if (!orig || orig(i, &s) != ERROR_SUCCESS) {
						continue;
					}
					if (!slotSeen[i]) {
						slotSeen[i] = true;
						++connectedSlots;
					}
					buttons |= s.Gamepad.wButtons;
					leftTrigger = (std::max)(leftTrigger, s.Gamepad.bLeftTrigger);
					rightTrigger = (std::max)(rightTrigger, s.Gamepad.bRightTrigger);
					if (abs((int)s.Gamepad.sThumbLX) > abs((int)lx)) lx = s.Gamepad.sThumbLX;
					if (abs((int)s.Gamepad.sThumbLY) > abs((int)ly)) ly = s.Gamepad.sThumbLY;
					if (abs((int)s.Gamepad.sThumbRX) > abs((int)rx)) rx = s.Gamepad.sThumbRX;
					if (abs((int)s.Gamepad.sThumbRY) > abs((int)ry)) ry = s.Gamepad.sThumbRY;
				}
			}

			static bool bridgeActive = false;
			static ULONGLONG lastBridgeCheck = 0;
			ULONGLONG bridgeNow = GetTickCount64();
			if (bridgeNow - lastBridgeCheck >= 2000) {
				lastBridgeCheck = bridgeNow;
				bool wantBridge = connectedSlots == 1 &&
					!DirectInputHook::HasPlaystationDevice() &&
					DirectInputHook::IsSonyGamepadAttachedToSystem();
				if (wantBridge != bridgeActive) {
					bridgeActive = wantBridge;
					g_psBridgeActive.store(bridgeActive, std::memory_order_relaxed);
					spdlog::info("RawInput: PlayStation XInput bridge {} (xinputSlots={}, directInputPlaystationDevice={}, sonyGamepadInSystem={})",
						bridgeActive ? "ENGAGED" : "released", connectedSlots,
						DirectInputHook::HasPlaystationDevice(),
						DirectInputHook::IsSonyGamepadAttachedToSystem());
				}
			}

			auto bridgeHeld = [&](USHORT psKey) -> bool {
				switch (psKey) {
				case VK_PS_CROSS:      return (buttons & XINPUT_GAMEPAD_A) != 0;
				case VK_PS_CIRCLE:     return (buttons & XINPUT_GAMEPAD_B) != 0;
				case VK_PS_SQUARE:     return (buttons & XINPUT_GAMEPAD_X) != 0;
				case VK_PS_TRIANGLE:   return (buttons & XINPUT_GAMEPAD_Y) != 0;
				case VK_PS_L1:         return (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
				case VK_PS_R1:         return (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
				case VK_PS_L2:         return leftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
				case VK_PS_R2:         return rightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
				case VK_PS_SHARE:      return (buttons & XINPUT_GAMEPAD_BACK) != 0;
				case VK_PS_OPTIONS:    return (buttons & XINPUT_GAMEPAD_START) != 0;
				case VK_PS_L3:         return (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
				case VK_PS_R3:         return (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
				case VK_PS_DPAD_UP:    return (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
				case VK_PS_DPAD_DOWN:  return (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
				case VK_PS_DPAD_LEFT:  return (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
				case VK_PS_DPAD_RIGHT: return (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
				case VK_PS_LS_UP:      return ly > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
				case VK_PS_LS_DOWN:    return ly < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
				case VK_PS_LS_LEFT:    return lx < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
				case VK_PS_LS_RIGHT:   return lx > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
				case VK_PS_RS_UP:      return ry > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
				case VK_PS_RS_DOWN:    return ry < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
				case VK_PS_RS_LEFT:    return rx < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
				case VK_PS_RS_RIGHT:   return rx > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
				default:               return false;
				}
			};

			for (USHORT psKey : PlaystationVKeys()) {
				bool isDown = bridgeActive ? bridgeHeld(psKey) : DirectInputHook::IsPlaystationControlHeld(psKey);
				bool wasDown = realStateHeld[psKey];
				if (isDown == wasDown) {
					continue;
				}
				realStateHeld[psKey] = isDown;
				currFlags[psKey] = isDown ? RI_KEY_MAKE : RI_KEY_BREAK;
				DoActions(psKey, isDown ? BUTTONEVENT::ONDOWN : BUTTONEVENT::ONUP);
			}
		}

		bool IsAnyGamepadConnected() {
			return g_anyGamepadConnected.load();
		}

		bool HasXInputGamepad() {
			return g_xinputGamepadConnected.load();
		}

		void DoActions(USHORT vKey, RawInput::BUTTONEVENT buttonEvent);

		void ProcessKey(PRAWINPUT pRaw) {
			//spdlog::trace("ProcessKey");//DEBUG
			USHORT vKey = pRaw->data.keyboard.VKey;
			if (vKey >= vKeyMax) {
				spdlog::warn("RawInput::ProcessKey: ignoring out-of-range VKey {}", vKey);
				return;
			}

			USHORT flags = pRaw->data.keyboard.Flags;
			USHORT oldFlags = currFlags[vKey];
			const bool isBreak = (flags & RI_KEY_BREAK) != 0;
			const bool wasBreak = (oldFlags & RI_KEY_BREAK) != 0;

			BUTTONEVENT buttonEvent = BUTTONEVENT::UP;
			if (!isBreak && wasBreak) {//OnKeyDown
				buttonEvent = BUTTONEVENT::ONDOWN;
				realStateHeld[vKey] = true; // Update tracking table
			}
			else if (isBreak && !wasBreak) {//OnKeyUp
				buttonEvent = BUTTONEVENT::ONUP;
				realStateHeld[vKey] = false; // Update tracking table
			}
			else if (!isBreak && !wasBreak) {//Held
				buttonEvent = BUTTONEVENT::HELD;
			}
			//else up, which you shouldnt hit

			currFlags[vKey] = flags;

			DoActions(vKey, buttonEvent);

#ifdef _DEBUG
			//WCHAR wcTextBuffer[512];
			//UINT keyChar = MapVirtualKey(pRaw->data.keyboard.VKey, MAPVK_VK_TO_CHAR);

			//wsprintf(wcTextBuffer,
			//	TEXT("Type=%d\nDevice=0x%x\nMakeCode=0x%x\nFlags=0x%x\nReserved=0x%x\nExtraInformation=0x%x\nMessage=0x%x\nVKey=0x%x\nEvent=0x%x\nkeyChar=0x%x\n\n"),
			//	/// device header
			//	pRaw->header.dwType,
			//	// device handle, pass this to GetRawInputDeviceInfo
			//	pRaw->header.hDevice,

			//	pRaw->data.keyboard.MakeCode,
			//	pRaw->data.keyboard.Flags,
			//	pRaw->data.keyboard.Reserved,
			//	pRaw->data.keyboard.ExtraInformation,
			//	pRaw->data.keyboard.Message,
			//	pRaw->data.keyboard.VKey,
			//	keyChar);

			//wprintf(wcTextBuffer);
#endif // _DEBUG
		}//ProcessRawInput

		// modified to add in mouse keys
		struct {
			USHORT vk;		UINT downflag;					UINT upflag;
		} const k[] = {
			{ VK_LBUTTON,   RI_MOUSE_LEFT_BUTTON_DOWN,		RI_MOUSE_LEFT_BUTTON_UP },
			{ VK_RBUTTON,   RI_MOUSE_RIGHT_BUTTON_DOWN,		RI_MOUSE_RIGHT_BUTTON_UP },
			{ VK_MBUTTON,	RI_MOUSE_MIDDLE_BUTTON_DOWN,	RI_MOUSE_MIDDLE_BUTTON_UP },
			{ VK_XBUTTON1,	RI_MOUSE_BUTTON_4_DOWN,			RI_MOUSE_BUTTON_4_UP },
			{ VK_XBUTTON2,	RI_MOUSE_BUTTON_5_DOWN,			RI_MOUSE_BUTTON_5_UP }
		};

		bool ProcessMouseButtons(PRAWINPUT pRaw) {
			USHORT usButtonFlags = pRaw->data.mouse.usButtonFlags;
			const int numButtons = _countof(k);

			USHORT oldFlagsB[vKeyMax];
			for (UINT i = 0; i < numButtons; ++i) {
				USHORT vKey = k[i].vk;
				oldFlagsB[vKey] = currFlags[vKey];
			}

			for (UINT i = 0; i < numButtons; ++i) {
				USHORT vKey = k[i].vk;
				if (usButtonFlags & k[i].downflag) {
					currFlags[vKey] = RI_KEY_MAKE;
					realStateHeld[vKey] = true;
				}
				if (usButtonFlags & k[i].upflag) {
					currFlags[vKey] = RI_KEY_BREAK;
					realStateHeld[vKey] = false;
				}
			}

			bool allowGameInput = true;

			for (UINT i = 0; i < numButtons; ++i) {
				USHORT vKey = k[i].vk;
				USHORT flags = currFlags[vKey];
				USHORT oldFlags = oldFlagsB[vKey];

				BUTTONEVENT buttonEvent = BUTTONEVENT::UP;
				if (flags == RI_KEY_MAKE && oldFlags == RI_KEY_BREAK) {
					buttonEvent = BUTTONEVENT::ONDOWN;
				}
				else if (flags == RI_KEY_BREAK && oldFlags == RI_KEY_MAKE) {
					buttonEvent = BUTTONEVENT::ONUP;
				}
				else if (flags == RI_KEY_MAKE && oldFlags == RI_KEY_MAKE) {
					buttonEvent = BUTTONEVENT::HELD;
				}

				if (blockGameKeys[vKey]) {
					allowGameInput = false;
				}

				// safety filter
				if (!ignore[vKey] && vKey != VK_LBUTTON && vKey != VK_RBUTTON) {
					DoActions(vKey, buttonEvent);
				}
			}

#ifdef _DEBUG
			/* FIXED: Wrapped the multi-line string text cleanly inside a block comment to stop the syntax crash on line 207 */
			//WCHAR wcTextBuffer[512];

			//wsprintf(wcTextBuffer,
			//	TEXT("Type=%d\nDevice=0x%x\nulButtons=0x%x\nulRawButtons=0x%x\nusButtonData=0x%x\nusButtonFlags=0x%x\nusFlags=0x%x\nlLastX=0x%x\nlLastY=0x%x\n\n"),
			//	pRaw->header.dwType,
			//	pRaw->header.hDevice,

			//	pRaw->data.mouse.ulButtons,
			//	pRaw->data.mouse.ulRawButtons,
			//	pRaw->data.mouse.usButtonData,
			//	pRaw->data.mouse.usButtonFlags,
			//	pRaw->data.mouse.usFlags,
			//	pRaw->data.mouse.lLastX,
			//	pRaw->data.mouse.lLastY);

			//wprintf(wcTextBuffer);
#endif // _DEBUG
			return allowGameInput;
		}

		void DoActions(USHORT vKey, RawInput::BUTTONEVENT buttonEvent) {
			if (vKey >= vKeyMax) {
				return;
			}
			std::vector<ButtonAction> snapshot;
			{
				std::lock_guard<std::recursive_mutex> lock(g_actionMutex);
				std::list<std::pair<ActionHandle, ButtonAction>>* actions = buttonActions[vKey];
				if (actions != nullptr) {
					snapshot.reserve(actions->size());
					for (const auto& entry : *actions) {
						snapshot.push_back(entry.second);
					}
				}
			}
			if (!snapshot.empty()) {
				spdlog::trace("RawInput DoActions for vKey:{}", vKey);
				for (ButtonAction& action : snapshot) {
					action(buttonEvent);
				}
			}
		}//DoActions


		ActionHandle RegisterAction(USHORT vKey, ButtonAction action) {
			if (vKey == 0 || vKey >= vKeyMax || !action) {
				spdlog::warn("RawInput::RegisterAction: invalid VKey {}", vKey);
				return 0;
			}
			spdlog::debug("RawInput RegisterAction for vKey:{}", vKey);
			std::lock_guard<std::recursive_mutex> lock(g_actionMutex);
			if (buttonActions[vKey] == nullptr) {
				buttonActions[vKey] = new std::list<std::pair<ActionHandle, ButtonAction>>();
			}

			ActionHandle handle = nextActionHandle++;
			buttonActions[vKey]->push_back({ handle, action });
			return handle;
		}//RegisterAction

		void UnRegisterAction(USHORT vKey) {
			if (vKey >= vKeyMax) {
				return;
			}
			std::lock_guard<std::recursive_mutex> lock(g_actionMutex);
			if (buttonActions[vKey] == nullptr) {
				spdlog::warn("RawInput UnRegisterAction: No actions for vKey {}", vKey);
				return;
			}
			else {
				buttonActions[vKey]->clear();
				delete buttonActions[vKey];
				buttonActions[vKey] = nullptr;
			}
		}//UnRegisterAction

		void UnRegisterAction(USHORT vKey, ActionHandle handle) {
			if (vKey >= vKeyMax || handle == 0) {
				return;
			}
			std::lock_guard<std::recursive_mutex> lock(g_actionMutex);
			std::list<std::pair<ActionHandle, ButtonAction>>* actions = buttonActions[vKey];
			if (actions == nullptr) {
				spdlog::warn("RawInput UnRegisterAction: No actions for vKey {}", vKey);
				return;
			}
			for (auto it = actions->begin(); it != actions->end(); ++it) {
				if (it->first == handle) {
					actions->erase(it);
					spdlog::debug("RawInput UnRegisterAction: removed handle {} from vKey {}", handle, vKey);
					if (actions->empty()) {
						delete buttonActions[vKey];
						buttonActions[vKey] = nullptr;
					}
					return;
				}
			}
			spdlog::warn("RawInput UnRegisterAction: handle {} not found for vKey {}", handle, vKey);
		}//UnRegisterAction (handle)

		bool IsKeyDown(USHORT vKey) {
			return vKey < vKeyMax && !((currFlags[vKey] & RI_KEY_BREAK) != 0);
		}//IsKeyDown

		//DEBUG
		//tex: don't process key //DEBUGNOW what am I doing here?
		void InitIgnoreKeys() {
			ignore[VK_KANA] = true;
			ignore[VK_HANGEUL] = true;
			ignore[VK_HANGUL] = true;
			ignore[VK_JUNJA] = true;
			ignore[VK_FINAL] = true;
			ignore[VK_HANJA] = true;
			ignore[VK_KANJI] = true;
			ignore[VK_CONVERT] = true;
			ignore[VK_NONCONVERT] = true;
			ignore[VK_ACCEPT] = true;
			ignore[VK_MODECHANGE] = true;

			ignore[VK_SLEEP] = true;
			ignore[VK_NAVIGATION_VIEW] = true;
			ignore[VK_NAVIGATION_MENU] = true;
			ignore[VK_NAVIGATION_UP] = true;
			ignore[VK_NAVIGATION_DOWN] = true;
			ignore[VK_NAVIGATION_LEFT] = true;
			ignore[VK_NAVIGATION_RIGHT] = true;
			ignore[VK_NAVIGATION_ACCEPT] = true;
			ignore[VK_NAVIGATION_CANCEL] = true;

			ignore[VK_OEM_NEC_EQUAL] = true;


			ignore[VK_OEM_FJ_JISHO] = true;
			ignore[VK_OEM_FJ_MASSHOU] = true;
			ignore[VK_OEM_FJ_TOUROKU] = true;
			ignore[VK_OEM_FJ_LOYA] = true;
			ignore[VK_OEM_FJ_ROYA] = true;

			ignore[VK_OEM_AX] = true;
			ignore[VK_OEM_102] = true;
			ignore[VK_ICO_HELP] = true;
			ignore[VK_ICO_00] = true;

			ignore[VK_PROCESSKEY] = true;
			ignore[VK_ICO_CLEAR] = true;
			ignore[VK_PACKET] = true;

			ignore[VK_OEM_RESET] = true;
			ignore[VK_OEM_JUMP] = true;
			ignore[VK_OEM_PA1] = true;
			ignore[VK_OEM_PA2] = true;
			ignore[VK_OEM_PA3] = true;
			ignore[VK_OEM_WSCTRL] = true;
			ignore[VK_OEM_CUSEL] = true;
			ignore[VK_OEM_ATTN] = true;
			ignore[VK_OEM_FINISH] = true;
			ignore[VK_OEM_COPY] = true;
			ignore[VK_OEM_AUTO] = true;
			ignore[VK_OEM_ENLW] = true;
			ignore[VK_OEM_BACKTAB] = true;

			ignore[VK_ATTN] = true;
			ignore[VK_CRSEL] = true;
			ignore[VK_EXSEL] = true;
			ignore[VK_EREOF] = true;
			ignore[VK_PLAY] = true;
			ignore[VK_ZOOM] = true;
			ignore[VK_NONAME] = true;
			ignore[VK_PA1] = true;
			ignore[VK_OEM_CLEAR] = true;
		}//InitIgnoreKeys

		void InitializeInput() {
			spdlog::debug("Rawinput InitializeInput");

			std::fill_n(currFlags, vKeyMax, static_cast<USHORT>(RI_KEY_BREAK));

			InitIgnoreKeys();
		}

		bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam){
			switch (uMsg) {
			case WM_INPUT:
			{
				// wParam is either RIM_INPUT (this app foreground) or RIM_INPUTSINK (this app background)
				// lParam is the RAWINPUT handle

				UINT dwSize;

				// determine size of buffer
				if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER)) == -1) {
					break;
				}

				LPBYTE lpb = new BYTE[dwSize];
				if (lpb == NULL) {
					break;
				}
				ZeroMemory(lpb, dwSize);

				// get actual data
				if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
					delete[] lpb;
					break;
				}

				// process it
				PRAWINPUT pRaw = (PRAWINPUT)lpb;
				if (pRaw->header.dwType == RIM_TYPEKEYBOARD) {
					USHORT vKey = pRaw->data.keyboard.VKey;
					if (vKey >= vKeyMax) {
						delete[] lpb;
						return true;
					}

					if (!ignore[vKey]) {
						ProcessKey(pRaw);
					}
					if (blockGameKeys[vKey]) {
						delete[] lpb;
						return false;
					}
				}
				else if (pRaw->header.dwType == RIM_TYPEMOUSE) {
					if (!ProcessMouseButtons(pRaw)) {
						delete[] lpb;
						return false;
					}
				}
				else if (pRaw->header.dwType == RIM_TYPEHID) {
					static std::unordered_map<HANDLE, bool> hidGamepadDevices;
					HANDLE hidDevice = pRaw->header.hDevice;
					auto seen = hidGamepadDevices.find(hidDevice);
					bool isGamepad;
					if (seen == hidGamepadDevices.end()) {
						RID_DEVICE_INFO hidInfo{};
						hidInfo.cbSize = sizeof(RID_DEVICE_INFO);
						UINT hidInfoSize = sizeof(RID_DEVICE_INFO);
						isGamepad = false;
						if (GetRawInputDeviceInfoW(hidDevice, RIDI_DEVICEINFO, &hidInfo, &hidInfoSize) != static_cast<UINT>(-1)) {
							isGamepad = (hidInfo.hid.usUsagePage == 0x01 && (hidInfo.hid.usUsage == 0x04 || hidInfo.hid.usUsage == 0x05));
							spdlog::info("RawInput: WM_INPUT HID device seen (usagePage={:04X}, usage={:04X}{})",
								hidInfo.hid.usUsagePage, hidInfo.hid.usUsage,
								isGamepad ? " GAMEPAD/JOYSTICK" : "");
							spdlog::default_logger()->flush();
						}
						hidGamepadDevices[hidDevice] = isGamepad;
					} else {
						isGamepad = seen->second;
					}
					if (isGamepad && IsGamepadBlockedToGame()) {
						delete[] lpb;
						return false;
					}
				}

				// not needed
				delete[] lpb;
				break;
			}//case WM_INPUT
			}//switch uMsg

			return true;
		}//OnMessage

		bool IsKeyHeldReal(USHORT vKey) {
			// this is for the hold function.
			if (vKey >= vKeyMax) return false;
			return realStateHeld[vKey];
		}

		void OnFocusLost() {
			for (int i = 0; i < vKeyMax; ++i) {
				if (realStateHeld[i].exchange(0)) {
					currFlags[i] = RI_KEY_BREAK;
					DoActions((USHORT)i, BUTTONEVENT::ONUP);
				} else {
					currFlags[i] = RI_KEY_BREAK;
				}
			}
		}
	}
}
