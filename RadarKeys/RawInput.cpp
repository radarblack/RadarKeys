#include "RawInput.h"
#include "DirectInputHook.h"
#include "spdlog/spdlog.h"
#include <MinHook.h>
#include <Xinput.h>
#include <tlhelp32.h>
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
#include <string>
#include <cctype>
#include <delayimp.h>

namespace RadarKeys {
	namespace RawInput {
		const USHORT vKeyMax = kMaxVKey;
		USHORT currFlags[vKeyMax];
		namespace { struct CurrFlagsFiller { CurrFlagsFiller() { std::fill_n(currFlags, vKeyMax, static_cast<USHORT>(RI_KEY_BREAK)); } }; }
		static CurrFlagsFiller g_currFlagsFiller;
		bool ignore[vKeyMax] = { false };
		std::atomic<unsigned char> blockGameKeys[vKeyMax]{};
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
		static constexpr int kMaxXInputModules = 8;
		static XInputGetStateFunc g_origXInputGetState[kMaxXInputModules] = {};
		static XInputGetStateFunc g_origXInputGetStateEx[kMaxXInputModules] = {};
		static int g_xinputModuleCount = 0;
		static int g_xinputExModuleCount = 0;
		static const char* g_xinputSlotNames[kMaxXInputModules] = {};
		static const char* g_xinputExSlotNames[kMaxXInputModules] = {};
		static std::unordered_set<void*> g_xinputHookedTargets;
		static std::unordered_set<void*> g_xinputExHookedTargets;
		static XInputGetStateFunc g_iatOrigXInputGetState = nullptr;
		static XInputGetStateFunc g_iatOrigXInputGetStateEx = nullptr;

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

		DWORD WINAPI HookedXInputGetState_IAT(DWORD dwUserIndex, XINPUT_STATE* pState) {
			static std::atomic<unsigned long long> callCount{ 0 };
			static std::atomic<ULONGLONG> lastLogTick{ 0 };
			DWORD result = ERROR_DEVICE_NOT_CONNECTED;
			if (g_iatOrigXInputGetState) {
				result = g_iatOrigXInputGetState(dwUserIndex, pState);
			} else {
				for (int i = 0; i < g_xinputModuleCount; ++i) {
					if (g_origXInputGetState[i]) {
						result = g_origXInputGetState[i](dwUserIndex, pState);
						if (result == ERROR_SUCCESS) break;
					}
				}
			}
			const bool blocked = g_gamepadBlockedToGame.load() != false;
			if (result == ERROR_SUCCESS && pState && blocked) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			const unsigned long long calls = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
			const ULONGLONG now = GetTickCount64();
			ULONGLONG last = lastLogTick.load(std::memory_order_relaxed);
			if (calls == 1 || (now - last >= 2000 && lastLogTick.compare_exchange_strong(last, now))) {
				spdlog::info("RawInput: XInputGetState via IAT: {} call(s) so far, last userIndex {} result {} blocked {}",
					calls, dwUserIndex, result, blocked);
				spdlog::default_logger()->flush();
			}
			return result;
		}

		DWORD WINAPI HookedXInputGetStateEx_IAT(DWORD dwUserIndex, XINPUT_STATE* pState) {
			static std::atomic<unsigned long long> callCount{ 0 };
			static std::atomic<ULONGLONG> lastLogTick{ 0 };
			DWORD result = ERROR_DEVICE_NOT_CONNECTED;
			if (g_iatOrigXInputGetStateEx) {
				result = g_iatOrigXInputGetStateEx(dwUserIndex, pState);
			} else {
				for (int i = 0; i < g_xinputExModuleCount; ++i) {
					if (g_origXInputGetStateEx[i]) {
						result = g_origXInputGetStateEx[i](dwUserIndex, pState);
						if (result == ERROR_SUCCESS) break;
					}
				}
			}
			const bool blocked = g_gamepadBlockedToGame.load() != false;
			if (result == ERROR_SUCCESS && pState && blocked) {
				ZeroMemory(&pState->Gamepad, sizeof(XINPUT_GAMEPAD));
			}
			const unsigned long long calls = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
			const ULONGLONG now = GetTickCount64();
			ULONGLONG last = lastLogTick.load(std::memory_order_relaxed);
			if (calls == 1 || (now - last >= 2000 && lastLogTick.compare_exchange_strong(last, now))) {
				spdlog::info("RawInput: XInputGetStateEx via IAT: {} call(s) so far, last userIndex {} result {} blocked {}",
					calls, dwUserIndex, result, blocked);
				spdlog::default_logger()->flush();
			}
			return result;
		}

		static XInputGetStateFunc g_xinputDetours[kMaxXInputModules] = {
			&HookedXInputGetState<0>, &HookedXInputGetState<1>,
			&HookedXInputGetState<2>, &HookedXInputGetState<3>,
			&HookedXInputGetState<4>, &HookedXInputGetState<5>,
			&HookedXInputGetState<6>, &HookedXInputGetState<7>,
		};

		static XInputGetStateFunc g_xinputExDetours[kMaxXInputModules] = {
			&HookedXInputGetStateEx<0>, &HookedXInputGetStateEx<1>,
			&HookedXInputGetStateEx<2>, &HookedXInputGetStateEx<3>,
			&HookedXInputGetStateEx<4>, &HookedXInputGetStateEx<5>,
			&HookedXInputGetStateEx<6>, &HookedXInputGetStateEx<7>,
		};

		static std::string ToLowerAsciiStr(const std::string& s) {
			std::string r;
			r.reserve(s.size());
			for (unsigned char c : s) r.push_back(static_cast<char>(::tolower(c)));
			return r;
		}

		static int PatchIATInModule(HMODULE hMod, const char* funcName, WORD ordinal, void* detour, void** origOut) {
			if (!hMod || !detour) return 0;
			PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
			PIMAGE_NT_HEADERS nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(hMod) + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
			DWORD importRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
			if (importRVA == 0) return 0;
			PIMAGE_IMPORT_DESCRIPTOR impDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<BYTE*>(hMod) + importRVA);
			int patched = 0;
			for (; impDesc->Name != 0; ++impDesc) {
				const char* dllName = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(hMod) + impDesc->Name);
				std::string dllLower = ToLowerAsciiStr(dllName);
				if (dllLower.find("xinput") == std::string::npos) continue;
				PIMAGE_THUNK_DATA origThunk = nullptr;
				if (impDesc->OriginalFirstThunk != 0) {
					origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hMod) + impDesc->OriginalFirstThunk);
				}
				PIMAGE_THUNK_DATA iatThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hMod) + impDesc->FirstThunk);
				for (size_t idx = 0; ; ++idx) {
					ULONGLONG origAddr = origThunk ? origThunk[idx].u1.AddressOfData : 0;
					ULONGLONG iatFunc = iatThunk[idx].u1.Function;
					if (origThunk) {
						if (origAddr == 0) break;
					} else {
						if (iatFunc == 0) break;
					}
					bool isOrdinal = false;
					WORD curOrd = 0;
					const char* curName = nullptr;
					if (origThunk) {
						if (IMAGE_SNAP_BY_ORDINAL(origThunk[idx].u1.Ordinal)) {
							isOrdinal = true;
							curOrd = IMAGE_ORDINAL(origThunk[idx].u1.Ordinal);
						} else {
							PIMAGE_IMPORT_BY_NAME byName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<BYTE*>(hMod) + origAddr);
							curName = reinterpret_cast<const char*>(byName->Name);
						}
					}
					bool match = false;
					if (funcName && curName && _stricmp(curName, funcName) == 0) match = true;
					if (ordinal != 0 && isOrdinal && curOrd == ordinal) match = true;
					if (ordinal == 100 && funcName && _stricmp(funcName, "XInputGetStateEx") == 0) {
						if (isOrdinal && curOrd == 100) match = true;
					}
					if (!match) continue;
					void* currentFunc = reinterpret_cast<void*>(iatThunk[idx].u1.Function);
					if (currentFunc == detour) continue;
					if (origOut && *origOut == nullptr) {
						*origOut = currentFunc;
					}
					DWORD oldProtect = 0;
					if (VirtualProtect(&iatThunk[idx].u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
						iatThunk[idx].u1.Function = reinterpret_cast<ULONGLONG>(detour);
						VirtualProtect(&iatThunk[idx].u1.Function, sizeof(void*), oldProtect, &oldProtect);
						++patched;
					}
				}
			}
			return patched;
		}

		static int PatchDelayIATInModule(HMODULE hMod, const char* funcName, WORD ordinal, void* detour, void** origOut) {
			if (!hMod || !detour) return 0;
			PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
			PIMAGE_NT_HEADERS nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(hMod) + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
			DWORD delayRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
			if (delayRVA == 0) return 0;
			PIMAGE_DELAYLOAD_DESCRIPTOR delayDesc = reinterpret_cast<PIMAGE_DELAYLOAD_DESCRIPTOR>(reinterpret_cast<BYTE*>(hMod) + delayRVA);
			int patched = 0;
			for (; delayDesc->DllNameRVA != 0; ++delayDesc) {
				const char* dllName = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(hMod) + delayDesc->DllNameRVA);
				std::string dllLower = ToLowerAsciiStr(dllName);
				if (dllLower.find("xinput") == std::string::npos) continue;
				PIMAGE_THUNK_DATA nameThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hMod) + delayDesc->ImportNameTableRVA);
				PIMAGE_THUNK_DATA iatThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hMod) + delayDesc->ImportAddressTableRVA);
				for (size_t idx = 0; nameThunk[idx].u1.AddressOfData != 0; ++idx) {
					bool isOrdinal = IMAGE_SNAP_BY_ORDINAL(nameThunk[idx].u1.Ordinal) != 0;
					WORD curOrd = 0;
					const char* curName = nullptr;
					if (isOrdinal) {
						curOrd = IMAGE_ORDINAL(nameThunk[idx].u1.Ordinal);
					} else {
						PIMAGE_IMPORT_BY_NAME byName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<BYTE*>(hMod) + nameThunk[idx].u1.AddressOfData);
						curName = reinterpret_cast<const char*>(byName->Name);
					}
					bool match = false;
					if (funcName && curName && _stricmp(curName, funcName) == 0) match = true;
					if (ordinal != 0 && isOrdinal && curOrd == ordinal) match = true;
					if (!match) continue;
					void* currentFunc = reinterpret_cast<void*>(iatThunk[idx].u1.Function);
					if (currentFunc == detour) continue;
					if (origOut && *origOut == nullptr) {
						*origOut = currentFunc;
					}
					DWORD oldProtect = 0;
					if (VirtualProtect(&iatThunk[idx].u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
						iatThunk[idx].u1.Function = reinterpret_cast<ULONGLONG>(detour);
						VirtualProtect(&iatThunk[idx].u1.Function, sizeof(void*), oldProtect, &oldProtect);
						++patched;
					}
				}
			}
			return patched;
		}

		static int PatchAllModulesIAT() {
			int totalPatched = 0;
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE) return 0;
			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (!Module32FirstW(snapshot, &entry)) {
				CloseHandle(snapshot);
				return 0;
			}
			do {
				HMODULE hMod = reinterpret_cast<HMODULE>(entry.modBaseAddr);
				void* origStateTmp = nullptr;
				void* origExTmp = nullptr;
				int p1 = PatchIATInModule(hMod, "XInputGetState", 0, reinterpret_cast<void*>(&HookedXInputGetState_IAT), g_iatOrigXInputGetState ? nullptr : &origStateTmp);
				int p2 = PatchIATInModule(hMod, "XInputGetStateEx", 100, reinterpret_cast<void*>(&HookedXInputGetStateEx_IAT), g_iatOrigXInputGetStateEx ? nullptr : &origExTmp);
				int p3 = PatchDelayIATInModule(hMod, "XInputGetState", 0, reinterpret_cast<void*>(&HookedXInputGetState_IAT), g_iatOrigXInputGetState ? nullptr : &origStateTmp);
				int p4 = PatchDelayIATInModule(hMod, "XInputGetStateEx", 100, reinterpret_cast<void*>(&HookedXInputGetStateEx_IAT), g_iatOrigXInputGetStateEx ? nullptr : &origExTmp);
				if (p1 || p2 || p3 || p4) {
					if (origStateTmp && !g_iatOrigXInputGetState) g_iatOrigXInputGetState = reinterpret_cast<XInputGetStateFunc>(origStateTmp);
					if (origExTmp && !g_iatOrigXInputGetStateEx) g_iatOrigXInputGetStateEx = reinterpret_cast<XInputGetStateFunc>(origExTmp);
				}
				totalPatched += p1 + p2 + p3 + p4;
			} while (Module32NextW(snapshot, &entry));
			CloseHandle(snapshot);
			return totalPatched;
		}

		void EnsureXInputHook() {
			static const wchar_t* kModuleNamesW[] = {
				L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll"
			};
			static const char* kModuleNamesA[] = {
				"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"
			};
			static bool loadAttemptedFor[kMaxXInputModules] = {};

			if (!g_iatOrigXInputGetState) {
				HMODULE mod13 = GetModuleHandleW(L"xinput1_3.dll");
				if (!mod13) mod13 = GetModuleHandleW(L"XINPUT1_3.dll");
				if (mod13) {
					void* p = reinterpret_cast<void*>(GetProcAddress(mod13, "XInputGetState"));
					if (p) g_iatOrigXInputGetState = reinterpret_cast<XInputGetStateFunc>(p);
				}
				if (!g_iatOrigXInputGetState) {
					HMODULE mod14 = GetModuleHandleW(L"xinput1_4.dll");
					if (mod14) {
						void* p = reinterpret_cast<void*>(GetProcAddress(mod14, "XInputGetState"));
						if (p) g_iatOrigXInputGetState = reinterpret_cast<XInputGetStateFunc>(p);
					}
				}
			}
			if (!g_iatOrigXInputGetStateEx) {
				HMODULE mod13 = GetModuleHandleW(L"xinput1_3.dll");
				if (!mod13) mod13 = GetModuleHandleW(L"XINPUT1_3.dll");
				if (mod13) {
					void* p = reinterpret_cast<void*>(GetProcAddress(mod13, "XInputGetStateEx"));
					if (!p) p = reinterpret_cast<void*>(GetProcAddress(mod13, reinterpret_cast<LPCSTR>(100)));
					if (p) g_iatOrigXInputGetStateEx = reinterpret_cast<XInputGetStateFunc>(p);
				}
				if (!g_iatOrigXInputGetStateEx) {
					HMODULE mod14 = GetModuleHandleW(L"xinput1_4.dll");
					if (mod14) {
						void* p = reinterpret_cast<void*>(GetProcAddress(mod14, "XInputGetStateEx"));
						if (!p) p = reinterpret_cast<void*>(GetProcAddress(mod14, reinterpret_cast<LPCSTR>(100)));
						if (p) g_iatOrigXInputGetStateEx = reinterpret_cast<XInputGetStateFunc>(p);
					}
				}
			}

			for (int nameIndex = 0; nameIndex < 5; ++nameIndex) {
				const wchar_t* moduleNameW = kModuleNamesW[nameIndex];
				const char* moduleNameA = kModuleNamesA[nameIndex];
				HMODULE module = GetModuleHandleW(moduleNameW);
				if (!module && !loadAttemptedFor[nameIndex]) {
					loadAttemptedFor[nameIndex] = true;
					module = LoadLibraryW(moduleNameW);
				}
				if (!module) continue;
				wchar_t modulePathW[MAX_PATH] = L"";
				GetModuleFileNameW(module, modulePathW, MAX_PATH);
				std::string modulePath = std::filesystem::path(modulePathW).string();
				void* target = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
				if (target && g_xinputHookedTargets.count(target) == 0 && g_xinputModuleCount < kMaxXInputModules) {
					int slot = g_xinputModuleCount;
					XInputGetStateFunc* origSlot = &g_origXInputGetState[slot];
					if (!*origSlot) *origSlot = reinterpret_cast<XInputGetStateFunc>(target);
					MH_STATUS stCreate = MH_CreateHook(target, reinterpret_cast<LPVOID>(g_xinputDetours[slot]), reinterpret_cast<LPVOID*>(origSlot));
					MH_STATUS stEnable = MH_OK;
					if (stCreate == MH_OK) stEnable = MH_EnableHook(target);
					if (stCreate == MH_OK && stEnable == MH_OK) {
						g_xinputHookedTargets.insert(target);
						g_xinputSlotNames[slot] = moduleNameA;
						++g_xinputModuleCount;
						spdlog::info("RawInput: hooked XInputGetState in {} (loaded from {}) for gamepad suppression ({} module(s))",
							moduleNameA, modulePath, g_xinputModuleCount);
					} else {
						if (stCreate != MH_OK) {
							spdlog::warn("RawInput: failed to hook XInputGetState in {} (loaded from {}) status {} {}",
								moduleNameA, modulePath, static_cast<int>(stCreate), MH_StatusToString(stCreate));
						} else {
							spdlog::warn("RawInput: failed to enable hook XInputGetState in {} (loaded from {}) status {} {}",
								moduleNameA, modulePath, static_cast<int>(stEnable), MH_StatusToString(stEnable));
							MH_RemoveHook(target);
						}
					}
				}
				void* targetEx = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetStateEx"));
				if (!targetEx) targetEx = reinterpret_cast<void*>(GetProcAddress(module, reinterpret_cast<LPCSTR>(100)));
				if (targetEx && g_xinputExHookedTargets.count(targetEx) == 0 && g_xinputExModuleCount < kMaxXInputModules) {
					int slotEx = g_xinputExModuleCount;
					XInputGetStateFunc* origSlotEx = &g_origXInputGetStateEx[slotEx];
					if (!*origSlotEx) *origSlotEx = reinterpret_cast<XInputGetStateFunc>(targetEx);
					MH_STATUS stCreate = MH_CreateHook(targetEx, reinterpret_cast<LPVOID>(g_xinputExDetours[slotEx]), reinterpret_cast<LPVOID*>(origSlotEx));
					MH_STATUS stEnable = MH_OK;
					if (stCreate == MH_OK) stEnable = MH_EnableHook(targetEx);
					if (stCreate == MH_OK && stEnable == MH_OK) {
						g_xinputExHookedTargets.insert(targetEx);
						g_xinputExSlotNames[slotEx] = moduleNameA;
						++g_xinputExModuleCount;
						spdlog::info("RawInput: hooked XInputGetStateEx in {} (loaded from {}) for gamepad suppression ({} module(s))",
							moduleNameA, modulePath, g_xinputExModuleCount);
					} else {
						if (stCreate != MH_OK) {
							spdlog::warn("RawInput: failed to hook XInputGetStateEx in {} (loaded from {}) status {} {}",
								moduleNameA, modulePath, static_cast<int>(stCreate), MH_StatusToString(stCreate));
						} else {
							spdlog::warn("RawInput: failed to enable hook XInputGetStateEx in {} (loaded from {}) status {} {}",
								moduleNameA, modulePath, static_cast<int>(stEnable), MH_StatusToString(stEnable));
							MH_RemoveHook(targetEx);
						}
					}
				}
			}

			static ULONGLONG lastIatPatchTick = 0;
			ULONGLONG now = GetTickCount64();
			if (now - lastIatPatchTick >= 1000) {
				lastIatPatchTick = now;
				int patched = PatchAllModulesIAT();
				if (patched > 0) {
					spdlog::info("RawInput: IAT patched {} import(s) for XInput suppression", patched);
					spdlog::default_logger()->flush();
				}
			}

			static bool inputModuleScanDone = false;
			if (!inputModuleScanDone) {
				inputModuleScanDone = true;
				HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
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

			if (g_iatOrigXInputGetState) {
				for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
					XINPUT_STATE s{};
					if (g_iatOrigXInputGetState(i, &s) != ERROR_SUCCESS) continue;
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
			if (g_iatOrigXInputGetState) {
				for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
					XINPUT_STATE s{};
					if (g_iatOrigXInputGetState(i, &s) != ERROR_SUCCESS) continue;
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
			if (!isBreak && wasBreak) {
				buttonEvent = BUTTONEVENT::ONDOWN;
				realStateHeld[vKey] = true;
			}
			else if (isBreak && !wasBreak) {
				buttonEvent = BUTTONEVENT::ONUP;
				realStateHeld[vKey] = false;
			}
			else if (!isBreak && !wasBreak) {
				buttonEvent = BUTTONEVENT::HELD;
			}

			currFlags[vKey] = flags;

			DoActions(vKey, buttonEvent);

		}

		struct {
			USHORT vk;		UINT downflag;						UINT upflag;
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

				if (!ignore[vKey] && vKey != VK_LBUTTON && vKey != VK_RBUTTON) {
					DoActions(vKey, buttonEvent);
				}
			}

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
		}


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
		}

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
		}

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
		}

		bool IsKeyDown(USHORT vKey) {
			return vKey < vKeyMax && !((currFlags[vKey] & RI_KEY_BREAK) != 0);
		}

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
		}

		void InitializeInput() {
			spdlog::debug("Rawinput InitializeInput");

			std::fill_n(currFlags, vKeyMax, static_cast<USHORT>(RI_KEY_BREAK));

			InitIgnoreKeys();
		}

		bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam){
			switch (uMsg) {
			case WM_INPUT:
			{
				UINT dwSize;

				if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER)) == -1) {
					break;
				}

				LPBYTE lpb = new BYTE[dwSize];
				if (lpb == NULL) {
					break;
				}
				ZeroMemory(lpb, dwSize);

				if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
					delete[] lpb;
					break;
				}

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
					static std::unordered_map<HANDLE, bool> hidIsGamepad;
					static std::unordered_set<HANDLE> seenHidDevices;
					static std::atomic<ULONGLONG> lastHidBlockLog{ 0 };
					HANDLE hidDevice = pRaw->header.hDevice;
					bool isGamepad = false;
					auto it = hidIsGamepad.find(hidDevice);
					if (it != hidIsGamepad.end()) {
						isGamepad = it->second;
					} else {
						RID_DEVICE_INFO hidInfo{};
						hidInfo.cbSize = sizeof(RID_DEVICE_INFO);
						UINT hidInfoSize = sizeof(RID_DEVICE_INFO);
						if (GetRawInputDeviceInfoW(hidDevice, RIDI_DEVICEINFO, &hidInfo, &hidInfoSize) != static_cast<UINT>(-1)) {
							isGamepad = (hidInfo.hid.usUsagePage == 0x01 &&
								(hidInfo.hid.usUsage == 0x04 || hidInfo.hid.usUsage == 0x05));
							hidIsGamepad[hidDevice] = isGamepad;
							if (seenHidDevices.find(hidDevice) == seenHidDevices.end()) {
								seenHidDevices.insert(hidDevice);
								spdlog::info("RawInput: WM_INPUT HID device seen (usagePage={:04X}, usage={:04X}{})",
									hidInfo.hid.usUsagePage, hidInfo.hid.usUsage,
									isGamepad ? " GAMEPAD/JOYSTICK" : "");
								spdlog::default_logger()->flush();
							}
						}
					}
					if (isGamepad && g_gamepadBlockedToGame.load() != false) {
						ULONGLONG now = GetTickCount64();
						ULONGLONG last = lastHidBlockLog.load(std::memory_order_relaxed);
						if (now - last >= 2000 && lastHidBlockLog.compare_exchange_strong(last, now)) {
							spdlog::info("RawInput: HID GAMEPAD/JOYSTICK blocked to game (device {:p})", hidDevice);
							spdlog::default_logger()->flush();
						}
						delete[] lpb;
						return false;
					}
				}

				delete[] lpb;
				break;
			}
			}

			return true;
		}

		bool IsKeyHeldReal(USHORT vKey) {
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
