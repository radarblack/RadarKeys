#include "DirectInputHook.h"
#include "windowsapi.h"
#include "RawInput.h"
#include <MinHook.h>
#include "spdlog/spdlog.h"

#include <dinput.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RadarKeys {
	namespace DirectInputHook {
		typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFGUID, LPVOID*, LPUNKNOWN);
		static DirectInput8Create_t g_origDirectInput8Create = nullptr;

		typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirectInput8*, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN);
		typedef HRESULT(STDMETHODCALLTYPE* SetProperty_t)(IDirectInputDevice8*, REFGUID, LPCDIPROPHEADER);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceData_t)(IDirectInputDevice8*, DWORD, DIDEVICEOBJECTDATA*, LPDWORD, DWORD);
		typedef HRESULT(STDMETHODCALLTYPE* Poll_t)(IDirectInputDevice8*);

		static CreateDevice_t g_origCreateDevice = nullptr;
		static SetProperty_t g_origSetProperty = nullptr;
		static GetDeviceState_t g_origGetDeviceState = nullptr;
		static GetDeviceData_t g_origGetDeviceData = nullptr;
		static Poll_t g_origPoll = nullptr;

		static constexpr size_t kDirectInput8VTableSize = 11;
		static constexpr size_t kDeviceVTableSize = 29;
		static constexpr size_t kSlotCreateDevice = 3;
		static constexpr size_t kSlotSetProperty = 6;
		static constexpr size_t kSlotGetDeviceState = 9;
		static constexpr size_t kSlotGetDeviceData = 10;
		static constexpr size_t kSlotPoll = 25;

		enum class DeviceKind { Unknown, Keyboard, Mouse, Joystick };

		struct AxisRange { bool known = false; LONG minV = 0; LONG maxV = 0; };
		struct DeviceInfo {
			DeviceKind kind = DeviceKind::Unknown;
			AxisRange deviceRange;
			AxisRange perOffset[32];
		};

		static std::mutex g_mutex;
		static std::unordered_set<void*> g_wrappedObjects;
		static std::unordered_map<IDirectInputDevice8*, DeviceInfo> g_deviceInfo;

		// GUID
		static const GUID kGuidSysMouse =
		{ 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54 } };
		static const GUID kGuidSysKeyboard =
		{ 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54 } };
		static const GUID kGuidSysMouseEm =
		{ 0x6F1D2B80, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54 } };
		static const GUID kGuidSysMouseEm2 =
		{ 0x6F1D2B81, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54 } };
		static const GUID kGuidSysKeyboardEm =
		{ 0x6F1D2B82, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54 } };
		static const GUID kGuidSysKeyboardEm2 =
		{ 0x6F1D2B83, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54 } };
		// DIPROP_RANGE
		static const GUID kGuidPropRange =
		{ 0x13517C81, 0x6E81, 0x11CF, { 0x9C, 0x3E, 0x00, 0xAA, 0x00, 0x4A, 0x48, 0xA4 } };

		static bool SameGuid(REFGUID a, REFGUID b) {
			return std::memcmp(&a, &b, sizeof(GUID)) == 0;
		}

		static bool OverrideObjectVTable(void* object, size_t vtableSize,
			const std::pair<size_t, void*> overrides[], size_t overrideCount) {
			if (!object) {
				return false;
			}
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_wrappedObjects.find(object) != g_wrappedObjects.end()) {
				return false;
			}
			void** originalVTable = *reinterpret_cast<void***>(object);

			void** copy = new void*[vtableSize];
			std::memcpy(copy, originalVTable, vtableSize * sizeof(void*));
			for (size_t i = 0; i < overrideCount; ++i) {
				copy[overrides[i].first] = overrides[i].second;
			}
			*reinterpret_cast<void***>(object) = copy;
			g_wrappedObjects.insert(object);
			return true;
		}

		static bool ShouldBlock(DeviceKind kind) {
			switch (kind) {
			case DeviceKind::Keyboard: return RawInput::IsKeyboardBlockedToGame();
			case DeviceKind::Mouse:    return RawInput::IsMouseBlockedToGame();
			case DeviceKind::Joystick: return RawInput::IsGamepadBlockedToGame();
			default:                   return false;
			}
		}

		// detour

		static HRESULT STDMETHODCALLTYPE Hooked_SetProperty(IDirectInputDevice8* self,
			REFGUID rguidProp, LPCDIPROPHEADER pdiph) {
			HRESULT hr = g_origSetProperty(self, rguidProp, pdiph);

			if (SUCCEEDED(hr) && pdiph && SameGuid(rguidProp, kGuidPropRange) &&
				pdiph->dwSize >= sizeof(DIPROPRANGE)) {
				const DIPROPRANGE* range = reinterpret_cast<const DIPROPRANGE*>(pdiph);
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					AxisRange r;
					r.known = true;
					r.minV = range->lMin;
					r.maxV = range->lMax;
					if (pdiph->dwHow == DIPH_DEVICE) {
						it->second.deviceRange = r;
					} else if (pdiph->dwHow == DIPH_BYOFFSET && (pdiph->dwObj % 4) == 0) {
						size_t idx = pdiph->dwObj / 4;
						if (idx < 32) {
							it->second.perOffset[idx] = r;
						}
					}
				}
			}
			return hr;
		}

		static LONG AxisNeutral(const DeviceInfo& info, size_t offsetBytes, bool stickAxis) {
			size_t idx = offsetBytes / 4;
			const AxisRange* r = (idx < 32 && info.perOffset[idx].known)
				? &info.perOffset[idx] : nullptr;
			if (!r && info.deviceRange.known) {
				r = &info.deviceRange;
			}
			if (r && r->known) {
				if (stickAxis) {
					return r->minV + (r->maxV - r->minV) / 2;
				}
				return r->minV;
			}
			return 0;
		}

		static void NeutralizeJoystick(LPVOID data, DWORD cbData, const DeviceInfo& info) {
			std::memset(data, 0, cbData);

			if (cbData >= offsetof(DIJOYSTATE, rgbButtons)) {
				LONG* axes = static_cast<LONG*>(data);
				axes[0] = AxisNeutral(info, DIJOFS_X, true);
				axes[1] = AxisNeutral(info, DIJOFS_Y, true);
				axes[2] = AxisNeutral(info, DIJOFS_Z, false);
				axes[3] = AxisNeutral(info, DIJOFS_RX, true);
				axes[4] = AxisNeutral(info, DIJOFS_RY, true);
				axes[5] = AxisNeutral(info, DIJOFS_RZ, false);
				axes[6] = AxisNeutral(info, DIJOFS_SLIDER(0), false);
				axes[7] = AxisNeutral(info, DIJOFS_SLIDER(1), false);

				DWORD* pov = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(data) + 32);
				for (int i = 0; i < 4; ++i) {
					pov[i] = 0xFFFFFFFF;
				}
			}
		}

		static HRESULT STDMETHODCALLTYPE Hooked_Poll(IDirectInputDevice8* self) {
			return g_origPoll(self);
		}

		static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceState(IDirectInputDevice8* self,
			DWORD cbData, LPVOID lpvData) {
			HRESULT hr = g_origGetDeviceState(self, cbData, lpvData);
			if (FAILED(hr) || !lpvData || cbData == 0) {
				return hr;
			}

			DeviceKind kind = DeviceKind::Unknown;
			DeviceInfo info;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					kind = it->second.kind;
					info = it->second;
				}
			}
			if (!ShouldBlock(kind)) {
				return hr;
			}

			switch (kind) {
			case DeviceKind::Keyboard:
				std::memset(lpvData, 0, cbData);
				break;
			case DeviceKind::Mouse:
				std::memset(lpvData, 0, cbData);
				break;
			case DeviceKind::Joystick:
				NeutralizeJoystick(lpvData, cbData, info);
				break;
			default:
				break;
			}
			return hr;
		}

		static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceData(IDirectInputDevice8* self,
			DWORD cbObjectData, DIDEVICEOBJECTDATA* rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
			HRESULT hr = g_origGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags);
			if (FAILED(hr) || !pdwInOut || !rgdod || *pdwInOut == 0) {
				return hr;
			}

			DeviceKind kind = DeviceKind::Unknown;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					kind = it->second.kind;
				}
			}
			if (!ShouldBlock(kind)) {
				return hr;
			}
      
			const DWORD povOffsets[] = { DIJOFS_POV(0), DIJOFS_POV(1), DIJOFS_POV(2), DIJOFS_POV(3) };
			for (DWORD i = 0; i < *pdwInOut; ++i) {
				bool isPov = false;
				for (DWORD pov : povOffsets) {
					if (rgdod[i].dwOfs == pov) { isPov = true; break; }
				}
				rgdod[i].dwData = isPov ? 0xFFFFFFFFu : 0;
			}
			return hr;
		}

		// detour

		static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirectInput8* self,
			REFGUID rguid, LPDIRECTINPUTDEVICE8* lplpDevice, LPUNKNOWN pUnkOuter) {
			HRESULT hr = g_origCreateDevice(self, rguid, lplpDevice, pUnkOuter);
			if (FAILED(hr) || !lplpDevice || !*lplpDevice) {
				return hr;
			}

			IDirectInputDevice8* device = *lplpDevice;
			DeviceKind kind;
			if (SameGuid(rguid, kGuidSysKeyboard) || SameGuid(rguid, kGuidSysKeyboardEm) ||
				SameGuid(rguid, kGuidSysKeyboardEm2)) {
				kind = DeviceKind::Keyboard;
			} else if (SameGuid(rguid, kGuidSysMouse) || SameGuid(rguid, kGuidSysMouseEm) ||
				SameGuid(rguid, kGuidSysMouseEm2)) {
				kind = DeviceKind::Mouse;
			} else {
				kind = DeviceKind::Joystick;
			}

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_deviceInfo[device].kind = kind;
				if (!g_origSetProperty) {
					void** vtbl = *reinterpret_cast<void***>(device);
					g_origSetProperty = reinterpret_cast<SetProperty_t>(vtbl[kSlotSetProperty]);
					g_origGetDeviceState = reinterpret_cast<GetDeviceState_t>(vtbl[kSlotGetDeviceState]);
					g_origGetDeviceData = reinterpret_cast<GetDeviceData_t>(vtbl[kSlotGetDeviceData]);
					g_origPoll = reinterpret_cast<Poll_t>(vtbl[kSlotPoll]);
				}
			}

			static const std::pair<size_t, void*> kDeviceOverrides[] = {
				{ kSlotSetProperty,   reinterpret_cast<void*>(&Hooked_SetProperty) },
				{ kSlotGetDeviceState,reinterpret_cast<void*>(&Hooked_GetDeviceState) },
				{ kSlotGetDeviceData, reinterpret_cast<void*>(&Hooked_GetDeviceData) },
				{ kSlotPoll,          reinterpret_cast<void*>(&Hooked_Poll) },
			};
			bool installed = OverrideObjectVTable(device, kDeviceVTableSize,
				kDeviceOverrides, sizeof(kDeviceOverrides) / sizeof(kDeviceOverrides[0]));

			spdlog::debug("DirectInputHook: wrapped device {:p} kind:{} ({})",
				static_cast<void*>(device), static_cast<int>(kind), installed ? "vtable wrapped" : "already wrapped");
			return hr;
		}

		static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
			REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
			HRESULT hr = g_origDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
			if (FAILED(hr) || !ppvOut || !*ppvOut) {
				return hr;
			}

			IDirectInput8* directInput = static_cast<IDirectInput8*>(*ppvOut);
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (!g_origCreateDevice) {
					void** vtbl = *reinterpret_cast<void***>(directInput);
					g_origCreateDevice = reinterpret_cast<CreateDevice_t>(vtbl[kSlotCreateDevice]);
				}
			}

			static const std::pair<size_t, void*> kDirectInputOverrides[] = {
				{ kSlotCreateDevice, reinterpret_cast<void*>(&Hooked_CreateDevice) },
			};
			OverrideObjectVTable(directInput, kDirectInput8VTableSize,
				kDirectInputOverrides, sizeof(kDirectInputOverrides) / sizeof(kDirectInputOverrides[0]));

			spdlog::debug("DirectInputHook: wrapped IDirectInput8 instance {:p}", static_cast<void*>(directInput));
			return hr;
		}

		void Install() {
			static bool attempted = false;
			if (attempted) {
				return;
			}
			attempted = true;

			HMODULE module = GetModuleHandleW(L"dinput8.dll");
			if (!module) {
				module = LoadLibraryW(L"dinput8.dll");
			}
			if (!module) {
				spdlog::warn("DirectInputHook: dinput8.dll not available - DirectInput gamepad suppression disabled");
				return;
			}

			void* target = reinterpret_cast<void*>(GetProcAddress(module, "DirectInput8Create"));
			if (!target) {
				spdlog::warn("DirectInputHook: DirectInput8Create export not found - suppression disabled");
				return;
			}

			if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hooked_DirectInput8Create),
				reinterpret_cast<LPVOID*>(&g_origDirectInput8Create)) != MH_OK ||
				MH_EnableHook(target) != MH_OK) {
				spdlog::warn("DirectInputHook: failed to hook DirectInput8Create - suppression disabled");
				return;
			}

			spdlog::info("DirectInputHook: hooked DirectInput8Create in the loaded dinput8.dll (chains through any proxy such as IHHook's)");
		}
	}
}
