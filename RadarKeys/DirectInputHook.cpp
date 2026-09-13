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

		typedef HRESULT(STDMETHODCALLTYPE* GetCapabilities_t)(IDirectInputDevice8*, LPDIDEVCAPS);
		typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirectInputDevice8*, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN);
		typedef HRESULT(STDMETHODCALLTYPE* SetProperty_t)(IDirectInputDevice8*, REFGUID, LPCDIPROPHEADER);
		typedef HRESULT(STDMETHODCALLTYPE* Acquire_t)(IDirectInputDevice8*);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceData_t)(IDirectInputDevice8*, DWORD, DIDEVICEOBJECTDATA*, LPDWORD, DWORD);
		typedef HRESULT(STDMETHODCALLTYPE* Poll_t)(IDirectInputDevice8*);

		static GetCapabilities_t g_origGetCapabilities = nullptr;
		static CreateDevice_t g_origCreateDevice = nullptr;
		static SetProperty_t g_origSetProperty = nullptr;
		static Acquire_t g_origAcquire = nullptr;
		static GetDeviceState_t g_origGetDeviceState = nullptr;
		static GetDeviceData_t g_origGetDeviceData = nullptr;
		static Poll_t g_origPoll = nullptr;

		static constexpr size_t kDirectInput8VTableSize = 11;
		static constexpr size_t kDeviceVTableSize = 29;
		static constexpr size_t kSlotGetCapabilities = 3;
		static constexpr size_t kSlotCreateDevice = 3;
		static constexpr size_t kSlotSetProperty = 6;
		static constexpr size_t kSlotAcquire = 7;
		static constexpr size_t kSlotGetDeviceState = 9;
		static constexpr size_t kSlotGetDeviceData = 10;
		static constexpr size_t kSlotPoll = 25;

		enum class DeviceKind { Unknown, Keyboard, Mouse, Joystick };

		struct AxisRange { bool known = false; LONG minV = 0; LONG maxV = 0; };
		struct DeviceInfo {
			DeviceKind kind = DeviceKind::Unknown;
			bool kindFromCapabilities = false;
			AxisRange deviceRange;
			AxisRange perOffset[32];
			BYTE realState[sizeof(DIJOYSTATE2)] = {};
			DWORD realStateSize = 0;
			bool hasRealState = false;
		};

		static std::mutex g_mutex;
		static std::unordered_set<void*> g_wrappedObjects;
		static std::unordered_map<IDirectInputDevice8*, DeviceInfo> g_deviceInfo;

		// GUIDs
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
				return false; // already wrapped
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

		static void ClassifyFromCapabilities(IDirectInputDevice8* self) {
			if (!g_origGetCapabilities) {
				return;
			}
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(DIDEVCAPS);
			HRESULT hr = g_origGetCapabilities(self, &caps);
			if (FAILED(hr)) {
				return;
			}
			DeviceKind kind = DeviceKind::Unknown;
			switch (caps.dwDevType & 0xFF) {
			case DI8DEVTYPE_KEYBOARD: kind = DeviceKind::Keyboard; break;
			case DI8DEVTYPE_MOUSE:    kind = DeviceKind::Mouse; break;
			case DI8DEVTYPE_JOYSTICK:
			case DI8DEVTYPE_GAMEPAD:
			case DI8DEVTYPE_1STPERSON:
			case DI8DEVTYPE_DRIVING:
			case DI8DEVTYPE_FLIGHT:
			case DI8DEVTYPE_SUPPLEMENTAL:
			case 4: // legacy DIDEVTYPE_JOYSTICK
				kind = DeviceKind::Joystick;
				break;
			default:
				kind = DeviceKind::Joystick;
				break;
			}
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_deviceInfo.find(self);
			if (it != g_deviceInfo.end()) {
				it->second.kind = kind;
				it->second.kindFromCapabilities = true;
			}
		}

		static HRESULT STDMETHODCALLTYPE Hooked_Acquire(IDirectInputDevice8* self) {
			HRESULT hr = g_origAcquire(self);
			if (SUCCEEDED(hr)) {
				ClassifyFromCapabilities(self);
			}
			return hr;
		}

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
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					kind = it->second.kind;
				}
			}

			if (kind == DeviceKind::Joystick) {
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					DWORD copyBytes = cbData < sizeof(it->second.realState)
						? cbData : (DWORD)sizeof(it->second.realState);
					std::memcpy(it->second.realState, lpvData, copyBytes);
					it->second.realStateSize = copyBytes;
					it->second.hasRealState = true;
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
			case DeviceKind::Joystick: {
				DeviceInfo info;
				{
					std::lock_guard<std::mutex> lock(g_mutex);
					auto it = g_deviceInfo.find(self);
					if (it != g_deviceInfo.end()) {
						info = it->second;
					}
				}
				NeutralizeJoystick(lpvData, cbData, info);
				break;
			}
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

		static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirectInputDevice8* self,
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
					g_origGetCapabilities = reinterpret_cast<GetCapabilities_t>(vtbl[kSlotGetCapabilities]);
					g_origSetProperty = reinterpret_cast<SetProperty_t>(vtbl[kSlotSetProperty]);
					g_origAcquire = reinterpret_cast<Acquire_t>(vtbl[kSlotAcquire]);
					g_origGetDeviceState = reinterpret_cast<GetDeviceState_t>(vtbl[kSlotGetDeviceState]);
					g_origGetDeviceData = reinterpret_cast<GetDeviceData_t>(vtbl[kSlotGetDeviceData]);
					g_origPoll = reinterpret_cast<Poll_t>(vtbl[kSlotPoll]);
				}
			}

			static const std::pair<size_t, void*> kDeviceOverrides[] = {
				{ kSlotSetProperty,      reinterpret_cast<void*>(&Hooked_SetProperty) },
				{ kSlotAcquire,          reinterpret_cast<void*>(&Hooked_Acquire) },
				{ kSlotGetDeviceState,   reinterpret_cast<void*>(&Hooked_GetDeviceState) },
				{ kSlotGetDeviceData,    reinterpret_cast<void*>(&Hooked_GetDeviceData) },
				{ kSlotPoll,             reinterpret_cast<void*>(&Hooked_Poll) },
			};
			bool installed = OverrideObjectVTable(device, kDeviceVTableSize,
				kDeviceOverrides, sizeof(kDeviceOverrides) / sizeof(kDeviceOverrides[0]));

			spdlog::debug("DirectInputHook: wrapped device {:p} initial kind:{} ({})",
				static_cast<void*>(device), static_cast<int>(kind), installed ? "vtable wrapped" : "already wrapped");
			return hr;
		}

		static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
			REFGUID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
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

		static bool ButtonHeld(const DIJOYSTATE* js, int index) {
			size_t count = 128;
			if (index < 0 || index >= (int)count) {
				return false;
			}
			return (js->rgbButtons[index] & 0x80) != 0;
		}

		static bool PovHeld(const DIJOYSTATE* js, int povIndex, int direction) {
			if (povIndex < 0 || povIndex >= 4) {
				return false;
			}
			DWORD pov = js->rgdwPOV[povIndex];
			if (pov == 0xFFFFFFFF || pov > 35999) {
				return false;
			}
			switch (direction) {
			case 0: return pov <= 4500  || pov >= 31500; // up
			case 1: return pov >= 13500 && pov <= 22500; // down
			case 2: return pov >= 22500 && pov <= 31500; // left
			case 3: return pov >= 4500  && pov <= 13500; // right
			}
			return false;
		}

		static bool AxisPast(const DeviceInfo& info, const DIJOYSTATE* js,
			int axisIndex, size_t offsetBytes, int direction) {
			const LONG* axes = reinterpret_cast<const LONG*>(js);
			LONG value = axes[axisIndex];

			size_t idx = offsetBytes / 4;
			LONG low = 0, high = 0;
			const AxisRange* r = (idx < 32 && info.perOffset[idx].known)
				? &info.perOffset[idx] : (info.deviceRange.known ? &info.deviceRange : nullptr);
			if (r && r->known) {
				low = r->minV;
				high = r->maxV;
				LONG threshold = (high - low) / 2;
				if (direction < 0) return value <= low + threshold / 2;
				return value >= low + threshold + (high - low) / 4;
			}
			const LONG kStickThreshold = 8000;
			return direction < 0 ? value <= -kStickThreshold : value >= kStickThreshold;
		}

		static bool TriggerHeld(const DeviceInfo& info, const DIJOYSTATE* js, int which) {
			LONG value = (which == 0) ? js->rglSlider[0] : js->rglSlider[1];
			size_t offsetBytes = DIJOFS_SLIDER(which);
			size_t idx = offsetBytes / 4;
			const AxisRange* r = (idx < 32 && info.perOffset[idx].known)
				? &info.perOffset[idx] : (info.deviceRange.known ? &info.deviceRange : nullptr);
			if (r && r->known) {
				return value >= r->minV + (r->maxV - r->minV) * 4 / 5;
			}
			if (which == 0 && js->lZ > 20000)  return true;
			if (which == 1 && js->lRz > 20000) return true;
			return false;
		}

		static bool IsGamepadButtonHeldLocked(USHORT vKey, const DeviceInfo& info) {
			if (!info.hasRealState || info.realStateSize < sizeof(DIJOYSTATE) ||
				info.kind != DeviceKind::Joystick) {
				return false;
			}
			const DIJOYSTATE* js = reinterpret_cast<const DIJOYSTATE*>(info.realState);

			switch (vKey) {
			case VK_GAMEPAD_A:                       return ButtonHeld(js, 0);
			case VK_GAMEPAD_B:                       return ButtonHeld(js, 1);
			case VK_GAMEPAD_X:                       return ButtonHeld(js, 2);
			case VK_GAMEPAD_Y:                       return ButtonHeld(js, 3);
			case VK_GAMEPAD_RIGHT_SHOULDER:          return ButtonHeld(js, 5);
			case VK_GAMEPAD_LEFT_SHOULDER:           return ButtonHeld(js, 4);
			case VK_GAMEPAD_LEFT_TRIGGER:            return TriggerHeld(info, js, 0);
			case VK_GAMEPAD_RIGHT_TRIGGER:           return TriggerHeld(info, js, 1);
			case VK_GAMEPAD_DPAD_UP:                 return PovHeld(js, 0, 0);
			case VK_GAMEPAD_DPAD_DOWN:               return PovHeld(js, 0, 1);
			case VK_GAMEPAD_DPAD_LEFT:               return PovHeld(js, 0, 2);
			case VK_GAMEPAD_DPAD_RIGHT:              return PovHeld(js, 0, 3);
			case VK_GAMEPAD_MENU:                    return ButtonHeld(js, 7);
			case VK_GAMEPAD_VIEW:                    return ButtonHeld(js, 6);
			case VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON:  return ButtonHeld(js, 8);
			case VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON: return ButtonHeld(js, 9);
			case VK_GAMEPAD_LEFT_THUMBSTICK_UP:      return AxisPast(info, js, 1, DIJOFS_Y, -1);
			case VK_GAMEPAD_LEFT_THUMBSTICK_DOWN:    return AxisPast(info, js, 1, DIJOFS_Y, +1);
			case VK_GAMEPAD_LEFT_THUMBSTICK_LEFT:    return AxisPast(info, js, 0, DIJOFS_X, -1);
			case VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT:   return AxisPast(info, js, 0, DIJOFS_X, +1);
			case VK_GAMEPAD_RIGHT_THUMBSTICK_UP:     return AxisPast(info, js, 4, DIJOFS_RY, -1);
			case VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN:   return AxisPast(info, js, 4, DIJOFS_RY, +1);
			case VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT:   return AxisPast(info, js, 3, DIJOFS_RX, -1);
			case VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT:  return AxisPast(info, js, 3, DIJOFS_RX, +1);
			default: return false;
			}
		}

		bool IsGamepadButtonHeld(USHORT vKey) {
			std::lock_guard<std::mutex> lock(g_mutex);
			for (const auto& entry : g_deviceInfo) {
				if (IsGamepadButtonHeldLocked(vKey, entry.second)) {
					return true;
				}
			}
			return false;
		}

		bool HasJoystickDevice() {
			std::lock_guard<std::mutex> lock(g_mutex);
			for (const auto& entry : g_deviceInfo) {
				if (entry.second.kind == DeviceKind::Joystick) {
					return true;
				}
			}
			return false;
		}

		void Install() {
			static bool attempted = false;
			if (attempted) {
				return;
			}

			HMODULE module = GetModuleHandleW(L"dinput8.dll");
			if (!module) {
				module = LoadLibraryW(L"dinput8.dll");
			}
			if (!module) {
				return;
			}
			attempted = true;

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
