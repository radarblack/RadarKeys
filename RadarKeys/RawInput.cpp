//DEBUGNOW this only really gets you OnKeyDown, OnKeyUp reliably as Held will be limited by key repeat rate
//the solution there would be to have another state array and have the input events set up,down and querry that with the assumption that down is held

#include "RawInput.h"
#include "DirectInputHook.h"
#include "spdlog/spdlog.h"
#include <MinHook.h>
#include <Xinput.h>
#include <tlhelp32.h>
#include <cstdlib>
#include <cstring>
#include <mmsystem.h>
#include <algorithm>
#include <utility>
#include <mutex>
#include <thread>
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
		static std::unordered_map<void*, ULONGLONG> g_xinputFailedTargets;
		static std::unordered_set<void*> g_xinputExHookedTargets;
		static std::unordered_map<void*, ULONGLONG> g_xinputExFailedTargets;
		static constexpr ULONGLONG kXInputRetryDelayMs = 2000;

		static XInputGetStateFunc g_iatOrigGetState = nullptr;
		static XInputGetStateFunc g_iatOrigGetStateEx = nullptr;
		static bool g_iatPatchedGetState = false;
		static bool g_iatPatchedGetStateEx = false;
		static void* g_iatThunkGetState = nullptr;
		static void* g_iatThunkGetStateEx = nullptr;
		static ULONG_PTR g_iatOriginalGetState = 0;
		static ULONG_PTR g_iatOriginalGetStateEx = 0;

		static DWORD WINAPI HookedIatXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
			DWORD result = g_iatOrigGetState ? g_iatOrigGetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
			if (result == ERROR_SUCCESS && pState && g_gamepadBlockedToGame.load() != false) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			return result;
		}

		static DWORD WINAPI HookedIatXInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE* pState) {
			DWORD result = g_iatOrigGetStateEx ? g_iatOrigGetStateEx(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
			if (result == ERROR_SUCCESS && pState && g_gamepadBlockedToGame.load() != false) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			return result;
		}

		static bool EqualsIgnoreCaseAscii(const char* a, const char* b) {
			while (*a && *b) {
				char ca = *a;
				char cb = *b;
				if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
				if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
				if (ca != cb) return false;
				++a;
				++b;
			}
			return *a == '\0' && *b == '\0';
		}

		static bool PatchXInputIatEntry(HMODULE module, const char* functionName, WORD ordinal, void* replacement,
			void** savedThunk, ULONG_PTR* savedOriginal) {
			BYTE* base = reinterpret_cast<BYTE*>(module);
			IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return false;
			}
			IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return false;
			}
			IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (importDir.VirtualAddress == 0) {
				return false;
			}
			IMAGE_IMPORT_DESCRIPTOR* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
			for (; desc->Name != 0; ++desc) {
				const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
				if (!EqualsIgnoreCaseAscii(dllName, "xinput1_3.dll")) {
					continue;
				}
				IMAGE_THUNK_DATA* nameThunks = desc->OriginalFirstThunk != 0
					? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
					: nullptr;
				IMAGE_THUNK_DATA* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
				for (DWORD i = 0; iat[i].u1.Function != 0; ++i) {
					bool match = false;
					if (nameThunks != nullptr) {
						if ((nameThunks[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) != 0) {
							match = ordinal != 0 && IMAGE_ORDINAL64(nameThunks[i].u1.Ordinal) == ordinal;
						} else {
							IMAGE_IMPORT_BY_NAME* importName =
								reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunks[i].u1.AddressOfData);
							match = functionName != nullptr && std::strcmp(importName->Name, functionName) == 0;
						}
					}
					if (!match) {
						continue;
					}
					void* thunk = &iat[i].u1.Function;
					if (*savedThunk == thunk) {
						return true;
					}
					DWORD oldProtect = 0;
					if (!VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
						return false;
					}
					*savedOriginal = iat[i].u1.Function;
					*reinterpret_cast<void**>(thunk) = replacement;
					VirtualProtect(thunk, sizeof(void*), oldProtect, &oldProtect);
					*savedThunk = thunk;
					return true;
				}
			}
			return false;
		}

		static void RestoreXInputIatEntry(void** savedThunk, ULONG_PTR savedOriginal) {
			DWORD oldProtect = 0;
			if (VirtualProtect(*savedThunk, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				*reinterpret_cast<void**>(*savedThunk) = reinterpret_cast<void*>(savedOriginal);
				VirtualProtect(*savedThunk, sizeof(void*), oldProtect, &oldProtect);
			}
			*savedThunk = nullptr;
		}

		static void EnsureXInput13IatFallback() {
			HMODULE x13 = GetModuleHandleW(L"xinput1_3.dll");
			if (!x13) {
				return;
			}
			void* getStateTarget = reinterpret_cast<void*>(GetProcAddress(x13, "XInputGetState"));
			void* getStateExTarget = reinterpret_cast<void*>(GetProcAddress(x13, reinterpret_cast<LPCSTR>(100)));
			bool minHookedGetState = getStateTarget != nullptr && g_xinputHookedTargets.count(getStateTarget) != 0;
			bool minHookedGetStateEx = getStateExTarget != nullptr && g_xinputExHookedTargets.count(getStateExTarget) != 0;
			if (g_iatPatchedGetState && minHookedGetState && g_iatThunkGetState != nullptr) {
				RestoreXInputIatEntry(&g_iatThunkGetState, g_iatOriginalGetState);
				g_iatPatchedGetState = false;
				spdlog::info("RawInput: xinput1_3 MinHook hook acquired - XInputGetState IAT fallback restored");
			}
			if (g_iatPatchedGetStateEx && minHookedGetStateEx && g_iatThunkGetStateEx != nullptr) {
				RestoreXInputIatEntry(&g_iatThunkGetStateEx, g_iatOriginalGetStateEx);
				g_iatPatchedGetStateEx = false;
				spdlog::info("RawInput: xinput1_3 MinHook hook acquired - XInputGetStateEx IAT fallback restored");
			}
			if (!g_iatPatchedGetState && !minHookedGetState && getStateTarget != nullptr) {
				if (!g_iatOrigGetState) {
					g_iatOrigGetState = reinterpret_cast<XInputGetStateFunc>(getStateTarget);
				}
				if (PatchXInputIatEntry(GetModuleHandleW(nullptr), "XInputGetState", 0,
					reinterpret_cast<void*>(&HookedIatXInputGetState),
					&g_iatThunkGetState, &g_iatOriginalGetState)) {
					g_iatPatchedGetState = true;
					spdlog::info("RawInput: xinput1_3 XInputGetState IAT fallback active (MinHook trampoline unavailable near module)");
				} else {
					static bool warnedNoImport = false;
					if (!warnedNoImport) {
						warnedNoImport = true;
						spdlog::warn("RawInput: xinput1_3 XInputGetState import not found in exe IAT - fallback unavailable");
					}
				}
			}
			if (!g_iatPatchedGetStateEx && !minHookedGetStateEx && getStateExTarget != nullptr) {
				if (!g_iatOrigGetStateEx) {
					g_iatOrigGetStateEx = reinterpret_cast<XInputGetStateFunc>(getStateExTarget);
				}
				if (PatchXInputIatEntry(GetModuleHandleW(nullptr), "XInputGetStateEx", 100,
					reinterpret_cast<void*>(&HookedIatXInputGetStateEx),
					&g_iatThunkGetStateEx, &g_iatOriginalGetStateEx)) {
					g_iatPatchedGetStateEx = true;
					spdlog::info("RawInput: xinput1_3 XInputGetStateEx IAT fallback active (MinHook trampoline unavailable near module)");
				}
			}
		}

		template <int N>
		DWORD WINAPI HookedXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
			static std::atomic<unsigned long long> callCount{ 0 };
			static std::atomic<ULONGLONG> lastLogTick{ 0 };
			DWORD result = ERROR_DEVICE_NOT_CONNECTED;
			if (g_origXInputGetState[N]) {
				result = g_origXInputGetState[N](dwUserIndex, pState);
			}
			const bool blocked = g_gamepadBlockedToGame.load() != false;
			if (result == ERROR_SUCCESS && pState && blocked) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			const unsigned long long calls = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
			const ULONGLONG now = GetTickCount64();
			ULONGLONG last = lastLogTick.load(std::memory_order_relaxed);
			if (calls == 1 || (now - last >= 2000 && lastLogTick.compare_exchange_strong(last, now))) {
				spdlog::info("RawInput: XInputGetState via {} slot {}: {} call(s) so far, last userIndex {} result {} blocked {}",
					g_xinputSlotNames[N] ? g_xinputSlotNames[N] : "?", N, calls, dwUserIndex, result, blocked);
				spdlog::default_logger()->flush();
			}
			return result;
		}

		template <int N>
		DWORD WINAPI HookedXInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE* pState) {
			static std::atomic<unsigned long long> callCount{ 0 };
			static std::atomic<ULONGLONG> lastLogTick{ 0 };
			DWORD result = ERROR_DEVICE_NOT_CONNECTED;
			if (g_origXInputGetStateEx[N]) {
				result = g_origXInputGetStateEx[N](dwUserIndex, pState);
			}
			const bool blocked = g_gamepadBlockedToGame.load() != false;
			if (result == ERROR_SUCCESS && pState && blocked) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			const unsigned long long calls = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
			const ULONGLONG now = GetTickCount64();
			ULONGLONG last = lastLogTick.load(std::memory_order_relaxed);
			if (calls == 1 || (now - last >= 2000 && lastLogTick.compare_exchange_strong(last, now))) {
				spdlog::info("RawInput: XInputGetStateEx via {} slot {}: {} call(s) so far, last userIndex {} result {} blocked {}",
					g_xinputExSlotNames[N] ? g_xinputExSlotNames[N] : "?", N, calls, dwUserIndex, result, blocked);
				spdlog::default_logger()->flush();
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

		struct WGIGamepadReading {
			ULONGLONG timestamp;
			ULONGLONG buttons;
			double leftThumbstickX;
			double leftThumbstickY;
			double rightThumbstickX;
			double rightThumbstickY;
			double leftTrigger;
			double rightTrigger;
		};
		typedef HRESULT(__stdcall* WIGetCurrentReading_t)(void*, WGIGamepadReading*);
		static WIGetCurrentReading_t g_origWIGetCurrentReading = nullptr;
		static std::atomic<bool> g_wiGetCurrentReadingHooked{ false };
		static std::atomic<ULONGLONG> g_lastWGISuppressLog{ 0 };

		static HRESULT __stdcall HookedWIGetCurrentReading(void* self, WGIGamepadReading* reading) {
			HRESULT hr = g_origWIGetCurrentReading ? g_origWIGetCurrentReading(self, reading) : E_POINTER;
			if (SUCCEEDED(hr) && reading && g_gamepadBlockedToGame.load() != false) {
				if (reading->buttons != 0 || reading->leftThumbstickX != 0 || reading->leftThumbstickY != 0 ||
					reading->rightThumbstickX != 0 || reading->rightThumbstickY != 0 ||
					reading->leftTrigger != 0 || reading->rightTrigger != 0) {
					const ULONGLONG now = GetTickCount64();
					ULONGLONG last = g_lastWGISuppressLog.load(std::memory_order_relaxed);
					if (now - last >= 1000 && g_lastWGISuppressLog.compare_exchange_strong(last, now)) {
						spdlog::info("RawInput: WGI gamepad reading intercepted (buttons {:016X}, lx {:.2f} ly {:.2f} rx {:.2f} ry {:.2f}) - suppressed",
							reading->buttons, reading->leftThumbstickX, reading->leftThumbstickY,
							reading->rightThumbstickX, reading->rightThumbstickY);
						spdlog::default_logger()->flush();
					}
				}
				reading->buttons = 0;
				reading->leftThumbstickX = 0;
				reading->leftThumbstickY = 0;
				reading->rightThumbstickX = 0;
				reading->rightThumbstickY = 0;
				reading->leftTrigger = 0;
				reading->rightTrigger = 0;
			}
			return hr;
		}

		static void EnsureWinmmHook();
		static void EnsureHidReadHook();

		typedef HRESULT(__stdcall* RoGetActivationFactory_t)(void*, const IID*, void**);
		typedef LONG(__stdcall* WindowsCreateString_t)(LPCWSTR, UINT32, void**);
		typedef LONG(__stdcall* WindowsDeleteString_t)(void*);
		typedef HRESULT(__stdcall* WIGetGamepads_t)(void*, void**);
		typedef HRESULT(__stdcall* WIGetAt_t)(void*, UINT32, void**);
		typedef ULONG(__stdcall* WIRelease_t)(void*);

		struct WGIWalkResult {
			HRESULT factoryHr;
			HRESULT gamepadsHr;
			HRESULT getAtHr;
			void* factory;
			void* vectorView;
			void* gamepad;
			void* getCurrentReadingTarget;
		};

		static WGIWalkResult WGIClassWalk(RoGetActivationFactory_t roGetActivationFactory,
			WindowsCreateString_t windowsCreateString, WindowsDeleteString_t windowsDeleteString) {
			WGIWalkResult result{};
			void* className = nullptr;
			if (FAILED(windowsCreateString(L"Windows.Gaming.Input.Gamepad", 28, &className))) {
				result.factoryHr = E_FAIL;
				return result;
			}
			static const IID kIID_IGamepadStatics = { 0x8BBCE529, 0xD49C, 0x39E9, { 0x95, 0x60, 0xE4, 0x7D, 0xDE, 0x96, 0xB7, 0xC8 } };
			result.factoryHr = roGetActivationFactory(className, &kIID_IGamepadStatics, &result.factory);
			windowsDeleteString(className);
			if (FAILED(result.factoryHr) || !result.factory) {
				return result;
			}
			__try {
				void** factoryVtbl = *reinterpret_cast<void***>(result.factory);
				WIGetGamepads_t getGamepads = reinterpret_cast<WIGetGamepads_t>(factoryVtbl[6]);
				if (!getGamepads) {
					result.gamepadsHr = E_POINTER;
					return result;
				}
				result.gamepadsHr = getGamepads(result.factory, &result.vectorView);
				if (FAILED(result.gamepadsHr) || !result.vectorView) {
					return result;
				}
				void** viewVtbl = *reinterpret_cast<void***>(result.vectorView);
				WIGetAt_t getAt = reinterpret_cast<WIGetAt_t>(viewVtbl[6]);
				if (!getAt) {
					result.getAtHr = E_POINTER;
					return result;
				}
				result.getAtHr = getAt(result.vectorView, 0, &result.gamepad);
				if (SUCCEEDED(result.getAtHr) && result.gamepad) {
					void** padVtbl = *reinterpret_cast<void***>(result.gamepad);
					result.getCurrentReadingTarget = padVtbl[6];
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				result.factoryHr = E_FAIL;
			}
			return result;
		}

		static void WGIClassHookWorker() {
			EnsureWinmmHook();
			EnsureHidReadHook();
			{
				HMODULE combase = GetModuleHandleW(L"combase.dll");
				if (!combase) {
					combase = LoadLibraryW(L"combase.dll");
				}
				if (combase) {
					typedef HRESULT(__stdcall* RoInitialize_t)(UINT);
					auto roInitialize = reinterpret_cast<RoInitialize_t>(GetProcAddress(combase, "RoInitialize"));
					if (roInitialize) {
						roInitialize(1);
					}
				}
			}
			for (int attempt = 1; attempt <= 150; ++attempt) {
				if (g_wiGetCurrentReadingHooked.load(std::memory_order_acquire)) {
					return;
				}
				HMODULE combase = GetModuleHandleW(L"combase.dll");
				if (!combase) {
					combase = LoadLibraryW(L"combase.dll");
				}
				if (!combase) {
					return;
				}
				RoGetActivationFactory_t roGetActivationFactory =
					reinterpret_cast<RoGetActivationFactory_t>(GetProcAddress(combase, "RoGetActivationFactory"));
				WindowsCreateString_t windowsCreateString =
					reinterpret_cast<WindowsCreateString_t>(GetProcAddress(combase, "WindowsCreateString"));
				WindowsDeleteString_t windowsDeleteString =
					reinterpret_cast<WindowsDeleteString_t>(GetProcAddress(combase, "WindowsDeleteString"));
				if (!roGetActivationFactory || !windowsCreateString || !windowsDeleteString) {
					return;
				}
				WGIWalkResult walk = WGIClassWalk(roGetActivationFactory, windowsCreateString, windowsDeleteString);
				if (SUCCEEDED(walk.factoryHr) && walk.factory && walk.getCurrentReadingTarget) {
					void* trampoline = nullptr;
					if (MH_CreateHook(walk.getCurrentReadingTarget,
						reinterpret_cast<LPVOID>(&HookedWIGetCurrentReading),
						reinterpret_cast<LPVOID*>(&g_origWIGetCurrentReading)) == MH_OK &&
						MH_EnableHook(walk.getCurrentReadingTarget) == MH_OK) {
						g_wiGetCurrentReadingHooked.store(true, std::memory_order_release);
						spdlog::info("RawInput: WGI GetCurrentReading class hook active at {:p} (covers every Windows.Gaming.Input gamepad in the process)",
							walk.getCurrentReadingTarget);
					} else {
						MH_RemoveHook(walk.getCurrentReadingTarget);
						spdlog::warn("RawInput: WGI GetCurrentReading hook failed - will retry");
					}
				} else if (attempt == 1) {
					spdlog::info("RawInput: WGI gamepad factory probe (factoryHr={:08X}, gamepadsHr={:08X}, getAtHr={:08X})",
						static_cast<unsigned>(walk.factoryHr), static_cast<unsigned>(walk.gamepadsHr),
						static_cast<unsigned>(walk.getAtHr));
				}
				if (walk.factory) {
					void** factoryVtbl = *reinterpret_cast<void***>(walk.factory);
					reinterpret_cast<WIRelease_t>(factoryVtbl[2])(walk.factory);
				}
				if (walk.vectorView) {
					void** viewVtbl = *reinterpret_cast<void***>(walk.vectorView);
					reinterpret_cast<WIRelease_t>(viewVtbl[2])(walk.vectorView);
				}
				if (walk.gamepad) {
					void** padVtbl = *reinterpret_cast<void***>(walk.gamepad);
					reinterpret_cast<WIRelease_t>(padVtbl[2])(walk.gamepad);
				}
				if (g_wiGetCurrentReadingHooked.load(std::memory_order_acquire)) {
					return;
				}
				Sleep(2000);
			}
		}

		void StartWGIClassHookWorker() {
			std::thread(WGIClassHookWorker).detach();
		}
		typedef DWORD(__stdcall* joyGetPosEx_t)(UINT, JOYINFOEX*);
		typedef DWORD(__stdcall* joyGetPos_t)(UINT, JOYINFO*);
		static joyGetPosEx_t g_origJoyGetPosEx = nullptr;
		static joyGetPos_t g_origJoyGetPos = nullptr;

		static DWORD __stdcall HookedJoyGetPosEx(UINT uJoyID, JOYINFOEX* pji) {
			DWORD result = g_origJoyGetPosEx ? g_origJoyGetPosEx(uJoyID, pji) : JOYERR_NOCANDO;
			if (result == JOYERR_NOERROR && pji && g_gamepadBlockedToGame.load() != false) {
				pji->dwXpos = 32767;
				pji->dwYpos = 32767;
				pji->dwZpos = 32767;
				pji->dwRpos = 32767;
				pji->dwUpos = 32767;
				pji->dwVpos = 32767;
				pji->dwButtons = 0;
				pji->dwButtonNumber = 0;
			}
			return result;
		}

		static DWORD __stdcall HookedJoyGetPos(UINT uJoyID, JOYINFO* pji) {
			DWORD result = g_origJoyGetPos ? g_origJoyGetPos(uJoyID, pji) : JOYERR_NOCANDO;
			if (result == JOYERR_NOERROR && pji && g_gamepadBlockedToGame.load() != false) {
				pji->wXpos = 32767;
				pji->wYpos = 32767;
				pji->wZpos = 32767;
				pji->wButtons = 0;
			}
			return result;
		}

		static void EnsureWinmmHook() {
			static bool attempted = false;
			if (attempted) {
				return;
			}
			attempted = true;
			HMODULE module = GetModuleHandleW(L"winmm.dll");
			if (!module) {
				module = LoadLibraryW(L"winmm.dll");
			}
			if (!module) {
				return;
			}
			void* target = reinterpret_cast<void*>(GetProcAddress(module, "joyGetPosEx"));
			if (target && !g_origJoyGetPosEx &&
				MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedJoyGetPosEx),
					reinterpret_cast<LPVOID*>(&g_origJoyGetPosEx)) == MH_OK) {
				MH_EnableHook(target);
				spdlog::info("RawInput: hooked joyGetPosEx (winmm) for gamepad suppression");
			}
			target = reinterpret_cast<void*>(GetProcAddress(module, "joyGetPos"));
			if (target && !g_origJoyGetPos &&
				MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedJoyGetPos),
					reinterpret_cast<LPVOID*>(&g_origJoyGetPos)) == MH_OK) {
				MH_EnableHook(target);
				spdlog::info("RawInput: hooked joyGetPos (winmm) for gamepad suppression");
			}
			spdlog::default_logger()->flush();
		}


		static std::unordered_map<HANDLE, unsigned char> g_hidHandleTags;
		struct HidOverlappedRead {
			HANDLE handle;
			LPVOID buffer;
			DWORD size;
			HANDLE event;
			LPOVERLAPPED_COMPLETION_ROUTINE exRoutine;
		};
		static std::unordered_map<LPOVERLAPPED, HidOverlappedRead> g_hidOverlappedReads;
		static std::unordered_map<HANDLE, HidOverlappedRead> g_hidEventReads;
		typedef LONG(NTAPI* NtReadFile_t)(HANDLE, HANDLE, void*, void*, void*, void*, ULONG, void*, void*);
		static NtReadFile_t g_origNtReadFile = nullptr;
		typedef BOOL(WINAPI* ReadFileEx_t)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
		static ReadFileEx_t g_origReadFileEx = nullptr;
		typedef DWORD(WINAPI* WaitForSingleObject_t)(HANDLE, DWORD);
		typedef DWORD(WINAPI* WaitForSingleObjectEx_t)(HANDLE, DWORD, BOOL);
		typedef DWORD(WINAPI* WaitForMultipleObjects_t)(DWORD, const HANDLE*, BOOL, DWORD);
		typedef DWORD(WINAPI* WaitForMultipleObjectsEx_t)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
		static WaitForSingleObject_t g_origWaitForSingleObject = nullptr;
		static WaitForSingleObjectEx_t g_origWaitForSingleObjectEx = nullptr;
		static WaitForMultipleObjects_t g_origWaitForMultipleObjects = nullptr;
		static WaitForMultipleObjectsEx_t g_origWaitForMultipleObjectsEx = nullptr;
		struct NtIoStatusBlock {
			LONG status;
			ULONG_PTR information;
		};
		typedef BOOL(WINAPI* GetOverlappedResult_t)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
		typedef BOOL(WINAPI* GetOverlappedResultEx_t)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
		static GetOverlappedResult_t g_origGetOverlappedResult = nullptr;
		static GetOverlappedResultEx_t g_origGetOverlappedResultEx = nullptr;
		typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		typedef BOOL(WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
		static CreateFileW_t g_origCreateFileW = nullptr;
		static CreateFileA_t g_origCreateFileA = nullptr;
		static ReadFile_t g_origReadFile = nullptr;
		static std::atomic<bool> g_hidFileHooksInstalled{ false };
		static std::atomic<ULONGLONG> g_lastHidSanitizeLog{ 0 };

		static bool LowerContainsGamepadHidNeedle(const std::wstring& lowerPath) {
			return lowerPath.find(L"vid_054c") != std::wstring::npos ||
				lowerPath.find(L"pid_05c4") != std::wstring::npos ||
				lowerPath.find(L"00001124") != std::wstring::npos ||
				lowerPath.find(L"vid_1234") != std::wstring::npos ||
				lowerPath.find(L"vigem") != std::wstring::npos;
		}

		static bool HandleIsGamepadHid(HANDLE handle) {
			auto it = g_hidHandleTags.find(handle);
			if (it != g_hidHandleTags.end()) {
				return it->second == 1;
			}
			wchar_t path[512] = L"";
			UINT n = GetFinalPathNameByHandleW(handle, path, 512, FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
			bool gamepad = false;
			if (n > 0 && n < 512) {
				std::wstring lower;
				for (wchar_t* p = path; *p; ++p) {
					lower.push_back((*p >= L'A' && *p <= L'Z') ? static_cast<wchar_t>((*p + 32)) : *p);
				}
				gamepad = lower.find(L"hid") != std::wstring::npos && LowerContainsGamepadHidNeedle(lower);
			}
			if (g_hidHandleTags.size() > 8192) {
				g_hidHandleTags.clear();
			}
			g_hidHandleTags[handle] = gamepad ? 1 : 2;
			if (gamepad) {
				spdlog::info("RawInput: tagged pre-opened gamepad HID handle {:p} ({})", static_cast<void*>(handle),
					std::filesystem::path(path).string());
				spdlog::default_logger()->flush();
			}
			return gamepad;
		}

		static void SanitizeGamepadHidBuffer(LPVOID buf, DWORD bytesRead) {
			std::memset(buf, 0, bytesRead);
			unsigned char* bytes = static_cast<unsigned char*>(buf);
			if (bytesRead > 5) {
				bytes[1] = 0x80;
				bytes[2] = 0x80;
				bytes[3] = 0x80;
				bytes[4] = 0x80;
				bytes[5] = 0x80;
			}
		}

		static void SanitizeCompletedHidRead(const HidOverlappedRead& rec, DWORD transferred) {
			if (transferred == 0 || !rec.buffer) {
				return;
			}
			if (g_gamepadBlockedToGame.load() != false && HandleIsGamepadHid(rec.handle)) {
				DWORD toSanitize = transferred < rec.size ? transferred : rec.size;
				SanitizeGamepadHidBuffer(rec.buffer, toSanitize);
				const ULONGLONG now = GetTickCount64();
				ULONGLONG last = g_lastHidSanitizeLog.load(std::memory_order_relaxed);
				if (now - last >= 1000 && g_lastHidSanitizeLog.compare_exchange_strong(last, now)) {
					spdlog::info("RawInput: sanitized {}-byte HID read on gamepad handle {:p} (suppression active)",
						toSanitize, static_cast<void*>(rec.handle));
					spdlog::default_logger()->flush();
				}
			}
		}

		static void SanitizeHidEventReadsForHandle(HANDLE hHandle, DWORD waitResult) {
			if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
				return;
			}
			auto it = g_hidEventReads.find(hHandle);
			if (it == g_hidEventReads.end()) {
				return;
			}
			SanitizeCompletedHidRead(it->second, it->second.size);
			g_hidEventReads.erase(it);
		}

		static void SanitizeHidOverlappedCompletion(HANDLE hFile, LPOVERLAPPED lpOverlapped, DWORD transferred) {
			auto it = g_hidOverlappedReads.find(lpOverlapped);
			if (it == g_hidOverlappedReads.end()) {
				return;
			}
			if (it->second.handle != hFile || transferred == 0 || !it->second.buffer) {
				g_hidOverlappedReads.erase(it);
				return;
			}
			if (g_gamepadBlockedToGame.load() != false && HandleIsGamepadHid(hFile)) {
				DWORD toSanitize = transferred < it->second.size ? transferred : it->second.size;
				SanitizeGamepadHidBuffer(it->second.buffer, toSanitize);
				const ULONGLONG now = GetTickCount64();
				ULONGLONG last = g_lastHidSanitizeLog.load(std::memory_order_relaxed);
				if (now - last >= 1000 && g_lastHidSanitizeLog.compare_exchange_strong(last, now)) {
					spdlog::info("RawInput: sanitized {}-byte overlapped HID read on gamepad handle {:p} (suppression active)",
						toSanitize, static_cast<void*>(hFile));
					spdlog::default_logger()->flush();
				}
			}
			g_hidOverlappedReads.erase(it);
		}

		static BOOL WINAPI HookedGetOverlappedResultGamepad(HANDLE hFile, LPOVERLAPPED lpOverlapped,
			LPDWORD lpNumberOfBytesTransferred, BOOL bWait) {
			BOOL ok = g_origGetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait);
			if (ok && lpNumberOfBytesTransferred) {
				SanitizeHidOverlappedCompletion(hFile, lpOverlapped, *lpNumberOfBytesTransferred);
			}
			return ok;
		}

		static BOOL WINAPI HookedGetOverlappedResultExGamepad(HANDLE hFile, LPOVERLAPPED lpOverlapped,
			LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable) {
			BOOL ok = g_origGetOverlappedResultEx(hFile, lpOverlapped, lpNumberOfBytesTransferred, dwMilliseconds, bAlertable);
			if (ok && lpNumberOfBytesTransferred) {
				SanitizeHidOverlappedCompletion(hFile, lpOverlapped, *lpNumberOfBytesTransferred);
			}
			return ok;
		}

		static BOOL WINAPI HookedReadFileGamepad(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
			LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
			BOOL ok = g_origReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
			if (lpOverlapped && lpBuffer && nNumberOfBytesToRead > 0 && HandleIsGamepadHid(hFile)) {
				if (g_hidOverlappedReads.size() > 4096) {
					g_hidOverlappedReads.clear();
				}
				HidOverlappedRead record{ hFile, lpBuffer, nNumberOfBytesToRead, lpOverlapped->hEvent, nullptr };
				g_hidOverlappedReads[lpOverlapped] = record;
				if (record.event) {
					if (g_hidEventReads.size() > 4096) {
						g_hidEventReads.clear();
					}
					g_hidEventReads[record.event] = record;
				}
			}
			if (ok && !lpOverlapped && lpNumberOfBytesRead && *lpNumberOfBytesRead > 0 && lpBuffer &&
				g_gamepadBlockedToGame.load() != false && HandleIsGamepadHid(hFile)) {
				SanitizeGamepadHidBuffer(lpBuffer, *lpNumberOfBytesRead);
				const ULONGLONG now = GetTickCount64();
				ULONGLONG last = g_lastHidSanitizeLog.load(std::memory_order_relaxed);
				if (now - last >= 1000 && g_lastHidSanitizeLog.compare_exchange_strong(last, now)) {
					spdlog::info("RawInput: sanitized {}-byte HID read on gamepad handle {:p} (suppression active)",
						*lpNumberOfBytesRead, static_cast<void*>(hFile));
					spdlog::default_logger()->flush();
				}
			}
			return ok;
		}

		static VOID CALLBACK WrappedExCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);
		struct HidExWrap {
			LPOVERLAPPED_COMPLETION_ROUTINE routine;
			HidOverlappedRead rec;
		};
		static std::unordered_map<LPOVERLAPPED, HidExWrap> g_hidExRoutines;

		static VOID CALLBACK WrappedExCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
			auto it = g_hidExRoutines.find(lpOverlapped);
			if (it != g_hidExRoutines.end()) {
				if (dwErrorCode == 0) {
					SanitizeCompletedHidRead(it->second.rec, dwNumberOfBytesTransfered);
				}
				LPOVERLAPPED_COMPLETION_ROUTINE routine = it->second.routine;
				g_hidExRoutines.erase(it);
				if (routine) {
					routine(dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped);
				}
				return;
			}
		}

		static LONG NTAPI HookedNtReadFileGamepad(HANDLE fileHandle, HANDLE hEvent, void* apcRoutine, void* apcContext,
			void* ioStatusBlock, void* buffer, ULONG length, void* byteOffset, void* key) {
			LONG status = g_origNtReadFile(fileHandle, hEvent, apcRoutine, apcContext, ioStatusBlock, buffer, length, byteOffset, key);
			constexpr LONG kStatusPending = 0x00000103;
			if ((status >= 0 || status == kStatusPending) && buffer && length > 0 && HandleIsGamepadHid(fileHandle)) {
				if (status == kStatusPending) {
					if (hEvent) {
						if (g_hidEventReads.size() > 4096) {
							g_hidEventReads.clear();
						}
						HidOverlappedRead record{ fileHandle, buffer, length, hEvent, nullptr };
						g_hidEventReads[hEvent] = record;
					}
				} else if (ioStatusBlock) {
					NtIoStatusBlock* iosb = static_cast<NtIoStatusBlock*>(ioStatusBlock);
					DWORD transferred = static_cast<DWORD>(iosb->information);
					if (transferred > 0 && transferred <= length && g_gamepadBlockedToGame.load() != false) {
						SanitizeGamepadHidBuffer(buffer, transferred);
						const ULONGLONG now = GetTickCount64();
						ULONGLONG last = g_lastHidSanitizeLog.load(std::memory_order_relaxed);
						if (now - last >= 1000 && g_lastHidSanitizeLog.compare_exchange_strong(last, now)) {
							spdlog::info("RawInput: sanitized {}-byte HID read on gamepad handle {:p} (ntdll path, suppression active)",
								transferred, static_cast<void*>(fileHandle));
							spdlog::default_logger()->flush();
						}
					}
				}
			}
			return status;
		}

		static BOOL WINAPI HookedReadFileExGamepad(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
			LPOVERLAPPED lpOverlapped, LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
			LPOVERLAPPED_COMPLETION_ROUTINE routine = lpCompletionRoutine;
			if (lpOverlapped && lpBuffer && nNumberOfBytesToRead > 0 && HandleIsGamepadHid(hFile)) {
				if (g_hidExRoutines.size() > 4096) {
					g_hidExRoutines.clear();
				}
				HidOverlappedRead record{ hFile, lpBuffer, nNumberOfBytesToRead, lpOverlapped->hEvent, nullptr };
				g_hidExRoutines[lpOverlapped] = { lpCompletionRoutine, record };
				routine = &WrappedExCompletion;
			}
			return g_origReadFileEx(hFile, lpBuffer, nNumberOfBytesToRead, lpOverlapped, routine);
		}

		static DWORD WINAPI HookedWaitForSingleObjectGamepad(HANDLE hHandle, DWORD dwMilliseconds) {
			DWORD result = g_origWaitForSingleObject(hHandle, dwMilliseconds);
			SanitizeHidEventReadsForHandle(hHandle, result);
			return result;
		}

		static DWORD WINAPI HookedWaitForSingleObjectExGamepad(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable) {
			DWORD result = g_origWaitForSingleObjectEx(hHandle, dwMilliseconds, bAlertable);
			SanitizeHidEventReadsForHandle(hHandle, result);
			return result;
		}

		static DWORD WINAPI HookedWaitForMultipleObjectsGamepad(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
			DWORD result = g_origWaitForMultipleObjects(nCount, lpHandles, bWaitAll, dwMilliseconds);
			if (nCount > 0 && nCount <= MAXIMUM_WAIT_OBJECTS && result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + nCount) {
				for (DWORD i = 0; i < nCount; ++i) {
					SanitizeHidEventReadsForHandle(lpHandles[i], WAIT_OBJECT_0);
				}
			}
			return result;
		}

		static DWORD WINAPI HookedWaitForMultipleObjectsExGamepad(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable) {
			DWORD result = g_origWaitForMultipleObjectsEx(nCount, lpHandles, bWaitAll, dwMilliseconds, bAlertable);
			if (nCount > 0 && nCount <= MAXIMUM_WAIT_OBJECTS && result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + nCount) {
				for (DWORD i = 0; i < nCount; ++i) {
					SanitizeHidEventReadsForHandle(lpHandles[i], WAIT_OBJECT_0);
				}
			}
			return result;
		}

		static HANDLE WINAPI HookedCreateFileWGamepad(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
			LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
			if (g_gamepadBlockedToGame.load() != false && lpFileName) {
				std::wstring lowerCheck;
				for (LPCWSTR p = lpFileName; *p; ++p) {
					lowerCheck.push_back((*p >= L'A' && *p <= L'Z') ? static_cast<wchar_t>((*p + 32)) : *p);
				}
				if (lowerCheck.find(L"hid") != std::wstring::npos && LowerContainsGamepadHidNeedle(lowerCheck)) {
					static ULONGLONG lastRedirectLog = 0;
					const ULONGLONG now = GetTickCount64();
					if (now - lastRedirectLog >= 2000) {
						lastRedirectLog = now;
						spdlog::info("RawInput: blocked gamepad HID device open while suppression active ({})",
							std::filesystem::path(lpFileName).string());
						spdlog::default_logger()->flush();
					}
					SetLastError(ERROR_FILE_NOT_FOUND);
					return INVALID_HANDLE_VALUE;
				}
			}
			HANDLE handle = g_origCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
				dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
			if (handle != INVALID_HANDLE_VALUE && lpFileName) {
				std::wstring lower;
				for (LPCWSTR p = lpFileName; *p; ++p) {
					lower.push_back((*p >= L'A' && *p <= L'Z') ? static_cast<wchar_t>((*p + 32)) : *p);
				}
				if (lower.find(L"hid") != std::wstring::npos && LowerContainsGamepadHidNeedle(lower)) {
					if (g_hidHandleTags.size() > 8192) {
						g_hidHandleTags.clear();
					}
					g_hidHandleTags[handle] = 1;
					spdlog::info("RawInput: game opened a gamepad HID device ({})", std::filesystem::path(lpFileName).string());
					spdlog::default_logger()->flush();
				}
			}
			return handle;
		}

		static HANDLE WINAPI HookedCreateFileAGamepad(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
			LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
			if (g_gamepadBlockedToGame.load() != false && lpFileName) {
				std::string lowerCheck;
				for (LPCSTR p = lpFileName; *p; ++p) {
					lowerCheck.push_back((*p >= 'A' && *p <= 'Z') ? static_cast<char>((*p + 32)) : *p);
				}
				if (lowerCheck.find("hid") != std::string::npos &&
					(lowerCheck.find("vid_054c") != std::string::npos || lowerCheck.find("pid_05c4") != std::string::npos ||
						lowerCheck.find("00001124") != std::string::npos || lowerCheck.find("vid_1234") != std::string::npos ||
						lowerCheck.find("vigem") != std::string::npos)) {
					static ULONGLONG lastRedirectLogA = 0;
					const ULONGLONG now = GetTickCount64();
					if (now - lastRedirectLogA >= 2000) {
						lastRedirectLogA = now;
						spdlog::info("RawInput: blocked gamepad HID device open while suppression active ({})", lowerCheck);
						spdlog::default_logger()->flush();
					}
					SetLastError(ERROR_FILE_NOT_FOUND);
					return INVALID_HANDLE_VALUE;
				}
			}
			HANDLE handle = g_origCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
				dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
			if (handle != INVALID_HANDLE_VALUE && lpFileName) {
				std::string lower;
				for (LPCSTR p = lpFileName; *p; ++p) {
					lower.push_back((*p >= 'A' && *p <= 'Z') ? static_cast<char>((*p + 32)) : *p);
				}
				if (lower.find("hid") != std::string::npos &&
					(lower.find("vid_054c") != std::string::npos || lower.find("pid_05c4") != std::string::npos ||
						lower.find("00001124") != std::string::npos || lower.find("vid_1234") != std::string::npos ||
						lower.find("vigem") != std::string::npos)) {
					if (g_hidHandleTags.size() > 8192) {
						g_hidHandleTags.clear();
					}
					g_hidHandleTags[handle] = 1;
					spdlog::info("RawInput: game opened a gamepad HID device ({})", lower);
					spdlog::default_logger()->flush();
				}
			}
			return handle;
		}

		static void EnsureHidReadHook() {
			static bool attempted = false;
			if (attempted || g_hidFileHooksInstalled.load(std::memory_order_acquire)) {
				return;
			}
			attempted = true;
			HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
			if (!kernel32) {
				return;
				}
			void* readFile = reinterpret_cast<void*>(GetProcAddress(kernel32, "ReadFile"));
			void* createFileW = reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW"));
			void* createFileA = reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA"));
			bool allOk = true;
			if (readFile && g_origReadFile == nullptr &&
				(MH_CreateHook(readFile, reinterpret_cast<LPVOID>(&HookedReadFileGamepad),
					reinterpret_cast<LPVOID*>(&g_origReadFile)) != MH_OK ||
					MH_EnableHook(readFile) != MH_OK)) {
				MH_RemoveHook(readFile);
				allOk = false;
			}
			if (createFileW && g_origCreateFileW == nullptr &&
				(MH_CreateHook(createFileW, reinterpret_cast<LPVOID>(&HookedCreateFileWGamepad),
					reinterpret_cast<LPVOID*>(&g_origCreateFileW)) != MH_OK ||
					MH_EnableHook(createFileW) != MH_OK)) {
				MH_RemoveHook(createFileW);
				allOk = false;
			}
			if (createFileA && g_origCreateFileA == nullptr &&
				(MH_CreateHook(createFileA, reinterpret_cast<LPVOID>(&HookedCreateFileAGamepad),
					reinterpret_cast<LPVOID*>(&g_origCreateFileA)) != MH_OK ||
					MH_EnableHook(createFileA) != MH_OK)) {
				MH_RemoveHook(createFileA);
				allOk = false;
			}
			void* overlappedResult = reinterpret_cast<void*>(GetProcAddress(kernel32, "GetOverlappedResult"));
			if (overlappedResult && g_origGetOverlappedResult == nullptr &&
				(MH_CreateHook(overlappedResult, reinterpret_cast<LPVOID>(&HookedGetOverlappedResultGamepad),
					reinterpret_cast<LPVOID*>(&g_origGetOverlappedResult)) != MH_OK ||
					MH_EnableHook(overlappedResult) != MH_OK)) {
				MH_RemoveHook(overlappedResult);
				allOk = false;
			}
			void* overlappedResultEx = reinterpret_cast<void*>(GetProcAddress(kernel32, "GetOverlappedResultEx"));
			if (overlappedResultEx && g_origGetOverlappedResultEx == nullptr &&
				(MH_CreateHook(overlappedResultEx, reinterpret_cast<LPVOID>(&HookedGetOverlappedResultExGamepad),
					reinterpret_cast<LPVOID*>(&g_origGetOverlappedResultEx)) != MH_OK ||
					MH_EnableHook(overlappedResultEx) != MH_OK)) {
				MH_RemoveHook(overlappedResultEx);
				allOk = false;
			}
			HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			if (ntdll) {
				void* ntReadFile = reinterpret_cast<void*>(GetProcAddress(ntdll, "NtReadFile"));
				if (ntReadFile && g_origNtReadFile == nullptr) {
					if (MH_CreateHook(ntReadFile, reinterpret_cast<LPVOID>(&HookedNtReadFileGamepad),
						reinterpret_cast<LPVOID*>(&g_origNtReadFile)) == MH_OK &&
						MH_EnableHook(ntReadFile) == MH_OK) {
						spdlog::info("RawInput: hooked NtReadFile (ntdll) - direct syscall-path HID reads now sanitized");
					} else {
						MH_RemoveHook(ntReadFile);
						spdlog::warn("RawInput: NtReadFile hook failed - direct ntdll reads will bypass sanitization");
					}
				}
			}
			void* readFileEx = reinterpret_cast<void*>(GetProcAddress(kernel32, "ReadFileEx"));
			if (readFileEx && g_origReadFileEx == nullptr &&
				(MH_CreateHook(readFileEx, reinterpret_cast<LPVOID>(&HookedReadFileExGamepad),
					reinterpret_cast<LPVOID*>(&g_origReadFileEx)) != MH_OK ||
					MH_EnableHook(readFileEx) != MH_OK)) {
				MH_RemoveHook(readFileEx);
				spdlog::warn("RawInput: ReadFileEx hook failed - alertable HID reads will bypass sanitization");
			}
			struct WaitTarget { const char* name; void** origSlot; void* detour; };
			const WaitTarget waitTargets[] = {
				{ "WaitForSingleObject", reinterpret_cast<void**>(&g_origWaitForSingleObject), reinterpret_cast<void*>(&HookedWaitForSingleObjectGamepad) },
				{ "WaitForSingleObjectEx", reinterpret_cast<void**>(&g_origWaitForSingleObjectEx), reinterpret_cast<void*>(&HookedWaitForSingleObjectExGamepad) },
				{ "WaitForMultipleObjects", reinterpret_cast<void**>(&g_origWaitForMultipleObjects), reinterpret_cast<void*>(&HookedWaitForMultipleObjectsGamepad) },
				{ "WaitForMultipleObjectsEx", reinterpret_cast<void**>(&g_origWaitForMultipleObjectsEx), reinterpret_cast<void*>(&HookedWaitForMultipleObjectsExGamepad) },
			};
			for (const WaitTarget& wait : waitTargets) {
				void* waitTarget = reinterpret_cast<void*>(GetProcAddress(kernel32, wait.name));
				if (waitTarget && *wait.origSlot == nullptr &&
					MH_CreateHook(waitTarget, wait.detour, wait.origSlot) == MH_OK &&
					MH_EnableHook(waitTarget) == MH_OK) {
					spdlog::info("RawInput: hooked {} - event-driven HID read completions now sanitized", wait.name);
				}
			}
			g_hidFileHooksInstalled.store(allOk, std::memory_order_release);
			spdlog::info("RawInput: HID handle/read suppression {} (gamepad device opens are tagged, reads sanitized while Gamepad suppression is active)",
				allOk ? "ACTIVE" : "PARTIAL/FAILED");
			spdlog::default_logger()->flush();
		}

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
				if (target) {
					auto failedIt = g_xinputFailedTargets.find(target);
					if (failedIt != g_xinputFailedTargets.end()) {
						if (GetTickCount64() - failedIt->second < kXInputRetryDelayMs) {
							target = nullptr;
						} else {
							g_xinputFailedTargets.erase(failedIt);
						}
					}
				}
				if (target && g_xinputHookedTargets.count(target) == 0 &&
					g_xinputModuleCount < kMaxXInputModules) {
					int slot = g_xinputModuleCount;
					XInputGetStateFunc* origSlot = &g_origXInputGetState[slot];
					MH_STATUS hookStatus = MH_CreateHook(target, reinterpret_cast<LPVOID>(g_xinputDetours[slot]),
						reinterpret_cast<LPVOID*>(origSlot));
					if (hookStatus == MH_OK) {
						hookStatus = MH_EnableHook(target) == MH_OK ? MH_OK : MH_ERROR_ENABLED;
					}
					if (hookStatus == MH_OK) {
						g_xinputHookedTargets.insert(target);
						g_xinputSlotNames[slot] = moduleNameNarrow;
						++g_xinputModuleCount;
						spdlog::info("RawInput: hooked XInputGetState in {} (loaded from {}) for gamepad suppression ({} module(s))",
							moduleNameNarrow, modulePath, g_xinputModuleCount);
					} else {
						MH_RemoveHook(target);
						g_xinputFailedTargets[target] = GetTickCount64();
						spdlog::warn("RawInput: failed to hook XInputGetState in {} (loaded from {}, status {}) - will retry",
							moduleNameNarrow, modulePath, MH_StatusToString(hookStatus));
					}
				}

				void* targetEx = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetStateEx"));
				if (!targetEx) {
					targetEx = reinterpret_cast<void*>(GetProcAddress(module, reinterpret_cast<LPCSTR>(100)));
				}
				if (targetEx) {
					auto failedExIt = g_xinputExFailedTargets.find(targetEx);
					if (failedExIt != g_xinputExFailedTargets.end()) {
						if (GetTickCount64() - failedExIt->second < kXInputRetryDelayMs) {
							targetEx = nullptr;
						} else {
							g_xinputExFailedTargets.erase(failedExIt);
						}
					}
				}
				if (targetEx && g_xinputExHookedTargets.count(targetEx) == 0 &&
					g_xinputExModuleCount < kMaxXInputModules) {
					int slotEx = g_xinputExModuleCount;
					XInputGetStateFunc* origSlotEx = &g_origXInputGetStateEx[slotEx];
					MH_STATUS hookStatusEx = MH_CreateHook(targetEx, reinterpret_cast<LPVOID>(g_xinputExDetours[slotEx]),
						reinterpret_cast<LPVOID*>(origSlotEx));
					if (hookStatusEx == MH_OK) {
						hookStatusEx = MH_EnableHook(targetEx) == MH_OK ? MH_OK : MH_ERROR_ENABLED;
					}
					if (hookStatusEx == MH_OK) {
						g_xinputExHookedTargets.insert(targetEx);
						g_xinputExSlotNames[slotEx] = moduleNameNarrow;
						++g_xinputExModuleCount;
						spdlog::info("RawInput: hooked XInputGetStateEx in {} (loaded from {}) for gamepad suppression ({} module(s))",
							moduleNameNarrow, modulePath, g_xinputExModuleCount);
					} else {
						MH_RemoveHook(targetEx);
						g_xinputExFailedTargets[targetEx] = GetTickCount64();
						spdlog::warn("RawInput: failed to hook XInputGetStateEx in {} (loaded from {}, status {}) - will retry",
							moduleNameNarrow, modulePath, MH_StatusToString(hookStatusEx));
					}
				}
			}

			EnsureXInput13IatFallback();

			static bool inputModuleScanDone = false;
			if (!inputModuleScanDone) {
				inputModuleScanDone = true;
				HANDLE snapshot = CreateToolhelp32Snapshot(
					TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
				if (snapshot != INVALID_HANDLE_VALUE) {
					MODULEENTRY32W entry{};
					entry.dwSize = sizeof(entry);
					static const wchar_t* kInputRelated[] = {
						L"xinput", L"dinput", L"gameinput", L"gaming.input", L"hid.dll",
						L"steam_api", L"steamclient", L"gameoverlayrenderer", L"vigem",
						L"sdl", L"xusb", L"inputhost"
					};
					if (Module32FirstW(snapshot, &entry)) {
						do {
							std::wstring lowerName(entry.szModule);
							std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
							for (const wchar_t* needle : kInputRelated) {
								if (lowerName.find(needle) != std::wstring::npos) {
									spdlog::info("RawInput: input-related module in process: {} ({})",
										std::filesystem::path(entry.szModule).string(),
										std::filesystem::path(entry.szExePath).string());
									break;
								}
							}
						} while (Module32NextW(snapshot, &entry));
					}
					CloseHandle(snapshot);
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
				if (g_gamepadBlockedToGame.load() != false) {
					spdlog::info("RawInput: GP key vKey={} {} (suppression active)", vKey, isDown ? "DOWN" : "UP");
					spdlog::default_logger()->flush();
				}
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
				if (g_gamepadBlockedToGame.load() != false) {
					spdlog::info("RawInput: PS key vKey={} {} (suppression active)", psKey, isDown ? "DOWN" : "UP");
					spdlog::default_logger()->flush();
				}
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
					static std::unordered_set<HANDLE> seenHidDevices;
					static std::unordered_map<HANDLE, bool> hidIsGamepad;
					HANDLE hidDevice = pRaw->header.hDevice;
					bool isGamepadClass = false;
					auto cachedIsGamepad = hidIsGamepad.find(hidDevice);
					if (cachedIsGamepad != hidIsGamepad.end()) {
						isGamepadClass = cachedIsGamepad->second;
					} else {
						RID_DEVICE_INFO hidInfo{};
						hidInfo.cbSize = sizeof(RID_DEVICE_INFO);
						UINT hidInfoSize = sizeof(RID_DEVICE_INFO);
						if (GetRawInputDeviceInfoW(hidDevice, RIDI_DEVICEINFO, &hidInfo, &hidInfoSize) != static_cast<UINT>(-1)) {
							isGamepadClass = hidInfo.hid.usUsagePage == 0x01 &&
								(hidInfo.hid.usUsage == 0x04 || hidInfo.hid.usUsage == 0x05);
							if (seenHidDevices.find(hidDevice) == seenHidDevices.end()) {
								seenHidDevices.insert(hidDevice);
								spdlog::info("RawInput: WM_INPUT HID device seen (usagePage={:04X}, usage={:04X}{})",
									hidInfo.hid.usUsagePage, hidInfo.hid.usUsage,
									isGamepadClass ? " GAMEPAD/JOYSTICK" : "");
								spdlog::default_logger()->flush();
							}
						}
						hidIsGamepad.emplace(hidDevice, isGamepadClass);
					}
					if (isGamepadClass && g_gamepadBlockedToGame.load() != false) {
						static std::unordered_set<HANDLE> blockedHidLogged;
						if (blockedHidLogged.find(hidDevice) == blockedHidLogged.end()) {
							blockedHidLogged.insert(hidDevice);
							spdlog::info("RawInput: blocking raw-input HID gamepad {:p} to game (suppression active)",
								static_cast<void*>(hidDevice));
							spdlog::default_logger()->flush();
						}
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
