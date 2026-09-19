#include "DirectInputHook.h"
#include "windowsapi.h"
#include "RawInput.h"
#include "HookUtils.h"
#include <MinHook.h>
#include "spdlog/spdlog.h"
#include <filesystem>

#define DI8SDK
#define INITGUID
#include <initguid.h>
#include <dinput.h>
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RadarKeys {
	namespace DirectInputHook {
		typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFGUID, LPVOID*, LPUNKNOWN);
		static DirectInput8Create_t g_origDirectInput8Create = nullptr;

		typedef HRESULT(STDMETHODCALLTYPE* GetCapabilities_t)(IDirectInputDevice8*, LPDIDEVCAPS);
		typedef HRESULT(STDMETHODCALLTYPE* GetProperty_t)(IDirectInputDevice8*, REFGUID, LPDIPROPHEADER);
		typedef ULONG(STDMETHODCALLTYPE* Release_t)(IDirectInputDevice8*);
		typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirectInput8*, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN);
		typedef HRESULT(STDMETHODCALLTYPE* SetProperty_t)(IDirectInputDevice8*, REFGUID, LPCDIPROPHEADER);
		typedef HRESULT(STDMETHODCALLTYPE* Acquire_t)(IDirectInputDevice8*);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceData_t)(IDirectInputDevice8*, DWORD, DIDEVICEOBJECTDATA*, LPDWORD, DWORD);
		typedef HRESULT(STDMETHODCALLTYPE* GetDeviceInfo_t)(IDirectInputDevice8*, LPDIDEVICEINSTANCE);

		static GetCapabilities_t g_origGetCapabilities = nullptr;
		static GetProperty_t g_origGetProperty = nullptr;
		static Release_t g_origRelease = nullptr;
		static CreateDevice_t g_origCreateDevice = nullptr;
		static SetProperty_t g_origSetProperty = nullptr;
		static Acquire_t g_origAcquire = nullptr;
		static GetDeviceState_t g_origGetDeviceState = nullptr;
		static GetDeviceData_t g_origGetDeviceData = nullptr;

		static IDirectInput8* g_ownDI8 = nullptr;

		static constexpr size_t kDirectInput8VTableSize = 11;
		static constexpr size_t kDeviceVTableSize = 29;
		static constexpr size_t kSlotGetCapabilities = 3;
		static constexpr size_t kSlotRelease = 2;
		static constexpr size_t kSlotGetProperty = 5;
		static constexpr size_t kSlotCreateDevice = 3;
		static constexpr size_t kSlotSetProperty = 6;
		static constexpr size_t kSlotAcquire = 7;
		static constexpr size_t kSlotGetDeviceState = 9;
		static constexpr size_t kSlotGetDeviceData = 10;
		static constexpr size_t kSlotGetDeviceInfo = 15;

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
			bool rangeQueried = false;
			AxisRange observed[32];
			bool isPlaystation = false;
			bool productQueried = false;
			bool selfOpened = false;
			bool steamVirtual360 = false;
			void** origVTable = nullptr;
		};

		static std::mutex g_mutex;
		static std::unordered_set<void*> g_wrappedObjects;
		static std::unordered_set<void*> g_vtableCopies;
		static std::unordered_map<void*, void**> g_di8OrigVTables;
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
		static bool SameGuid(REFGUID a, REFGUID b) {
			return std::memcmp(&a, &b, sizeof(GUID)) == 0;
		}

		static bool IsDipropRangeGuid(REFGUID rguidProp) {
			return SameGuid(rguidProp, DIPROP_RANGE);
		}

		static bool OverrideObjectVTable(void* object, size_t vtableSize,
			const std::pair<size_t, void*> overrides[], size_t overrideCount,
			void*** outOriginalVTable) {
			if (!object) {
				return false;
			}
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_wrappedObjects.find(object) != g_wrappedObjects.end()) {
				return false;
			}
			void** originalVTable = *reinterpret_cast<void***>(object);
			if (outOriginalVTable) {
				*outOriginalVTable = originalVTable;
			}

			void** copy = new void*[vtableSize];
			std::memcpy(copy, originalVTable, vtableSize * sizeof(void*));
			for (size_t i = 0; i < overrideCount; ++i) {
				copy[overrides[i].first] = overrides[i].second;
			}
			*reinterpret_cast<void***>(object) = copy;
			g_wrappedObjects.insert(object);
			g_vtableCopies.insert(copy);
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

		static bool SonyGamepadPresentInSystem();

		static void** OriginalVTableForDevice(IDirectInputDevice8* self) {
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_deviceInfo.find(self);
			if (it != g_deviceInfo.end() && it->second.origVTable) {
				return it->second.origVTable;
			}
			return nullptr;
		}

		static Acquire_t OrigAcquire(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<Acquire_t>(vtbl[kSlotAcquire]) : g_origAcquire;
		}

		static SetProperty_t OrigSetProperty(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<SetProperty_t>(vtbl[kSlotSetProperty]) : g_origSetProperty;
		}

		static GetProperty_t OrigGetProperty(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<GetProperty_t>(vtbl[kSlotGetProperty]) : g_origGetProperty;
		}

		static GetCapabilities_t OrigGetCapabilities(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<GetCapabilities_t>(vtbl[kSlotGetCapabilities]) : g_origGetCapabilities;
		}

		static GetDeviceState_t OrigGetDeviceState(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<GetDeviceState_t>(vtbl[kSlotGetDeviceState]) : g_origGetDeviceState;
		}

		static GetDeviceData_t OrigGetDeviceData(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<GetDeviceData_t>(vtbl[kSlotGetDeviceData]) : g_origGetDeviceData;
		}

		static Release_t OrigRelease(IDirectInputDevice8* self) {
			void** vtbl = OriginalVTableForDevice(self);
			return vtbl ? reinterpret_cast<Release_t>(vtbl[kSlotRelease]) : g_origRelease;
		}

		static CreateDevice_t OrigCreateDevice(IDirectInput8* self) {
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_di8OrigVTables.find(self);
				if (it != g_di8OrigVTables.end() && it->second) {
					return reinterpret_cast<CreateDevice_t>(it->second[kSlotCreateDevice]);
				}
			}
			return g_origCreateDevice;
		}

		// detour

		static void ClassifyFromCapabilities(IDirectInputDevice8* self) {
			GetCapabilities_t origGetCapabilities = OrigGetCapabilities(self);
			if (!origGetCapabilities) {
				return;
			}
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(DIDEVCAPS);
			HRESULT hr = origGetCapabilities(self, &caps);
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
			case 4:
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

		static void ClassifyPlaystation(IDirectInputDevice8* self) {
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_deviceInfo.find(self);
			if (it == g_deviceInfo.end() || it->second.productQueried) {
				return;
			}

			void** vtbl = *reinterpret_cast<void***>(self);
			auto origGetDeviceInfo = reinterpret_cast<GetDeviceInfo_t>(vtbl[kSlotGetDeviceInfo]);
			if (!origGetDeviceInfo) {
				it->second.productQueried = true;
				return;
			}

			DIDEVICEINSTANCE diInfo{};
			diInfo.dwSize = sizeof(DIDEVICEINSTANCE);
			bool infoIsWide = SUCCEEDED(origGetDeviceInfo(self, &diInfo));
			DIDEVICEINSTANCEA diInfoA{};
			if (!infoIsWide) {
				diInfoA.dwSize = sizeof(DIDEVICEINSTANCEA);
				if (FAILED(origGetDeviceInfo(self, reinterpret_cast<LPDIDEVICEINSTANCE>(&diInfoA)))) {
					return;
				}
			}
			it->second.productQueried = true;

			GUID productGuid{};
			wchar_t productName[MAX_PATH] = L"";
			wchar_t instanceName[MAX_PATH] = L"";
			if (infoIsWide) {
				productGuid = diInfo.guidProduct;
				std::memcpy(productName, diInfo.tszProductName, sizeof(productName) - sizeof(wchar_t));
				std::memcpy(instanceName, diInfo.tszInstanceName, sizeof(instanceName) - sizeof(wchar_t));
			} else {
				productGuid = diInfoA.guidProduct;
				for (size_t i = 0; i < MAX_PATH - 1 && diInfoA.tszProductName[i] != '\0'; ++i) {
					productName[i] = static_cast<wchar_t>(static_cast<unsigned char>(diInfoA.tszProductName[i]));
				}
				for (size_t i = 0; i < MAX_PATH - 1 && diInfoA.tszInstanceName[i] != '\0'; ++i) {
					instanceName[i] = static_cast<wchar_t>(static_cast<unsigned char>(diInfoA.tszInstanceName[i]));
				}
			}

			auto toNarrow = [](const wchar_t* w) -> std::string {
				std::string s;
				while (*w) {
					s.push_back(static_cast<char>(*w));
					++w;
				}
				return s;
			};

			auto toUpperAscii = [](wchar_t c) -> wchar_t {
				return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
			};
			wchar_t upperProduct[MAX_PATH] = L"";
			for (size_t i = 0; i < MAX_PATH - 1 && productName[i] != L'\0'; ++i) {
				upperProduct[i] = toUpperAscii(productName[i]);
			}
			wchar_t upperInstance[MAX_PATH] = L"";
			for (size_t i = 0; i < MAX_PATH - 1 && instanceName[i] != L'\0'; ++i) {
				upperInstance[i] = toUpperAscii(instanceName[i]);
			}

			const bool nameMatch =
				wcsstr(upperProduct, L"DUALSHOCK") != nullptr ||
				wcsstr(upperProduct, L"DUALSENSE") != nullptr ||
				wcsstr(upperProduct, L"PLAYSTATION") != nullptr ||
				wcsstr(upperProduct, L"WIRELESS CONTROLLER") != nullptr;
			const WORD vendorId = static_cast<WORD>(productGuid.Data1 & 0xFFFF);
			const WORD productId = static_cast<WORD>((productGuid.Data1 >> 16) & 0xFFFF);
			const bool vidMatch = (vendorId == 0x054C);
			it->second.steamVirtual360 = (vendorId == 0x28DE && productId == 0x11FF);

			it->second.isPlaystation = nameMatch || vidMatch ||
				(it->second.steamVirtual360 && SonyGamepadPresentInSystem());
			spdlog::info("DirectInputHook: device {:p} PlayStation button mapping {} (product:\"{}\", instance:\"{}\", vid:{:04X}, pid:{:04X})",
				static_cast<void*>(self), it->second.isPlaystation ? "ENABLED" : "disabled",
				toNarrow(productName), toNarrow(instanceName), vendorId, productId);
		}

		static HRESULT STDMETHODCALLTYPE Hooked_Acquire(IDirectInputDevice8* self) {
			Acquire_t origAcquire = OrigAcquire(self);
			HRESULT hr = origAcquire ? origAcquire(self) : E_FAIL;
			if (SUCCEEDED(hr)) {
				ClassifyFromCapabilities(self);
				ClassifyPlaystation(self);
				spdlog::default_logger()->flush();
			}
			return hr;
		}

		static HRESULT STDMETHODCALLTYPE Hooked_SetProperty(IDirectInputDevice8* self,
			REFGUID rguidProp, LPCDIPROPHEADER pdiph) {
			SetProperty_t origSetProperty = OrigSetProperty(self);
			HRESULT hr = origSetProperty ? origSetProperty(self, rguidProp, pdiph) : E_FAIL;

			if (SUCCEEDED(hr) && pdiph && IsDipropRangeGuid(rguidProp) &&
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

		static const AxisRange* ResolveAxisRange(const DeviceInfo& info, size_t idx) {
			if (idx < 32 && info.perOffset[idx].known &&
				info.perOffset[idx].maxV > info.perOffset[idx].minV) {
				return &info.perOffset[idx];
			}
			if (info.deviceRange.known && info.deviceRange.maxV > info.deviceRange.minV) {
				return &info.deviceRange;
			}
			if (idx < 32 && info.observed[idx].known &&
				info.observed[idx].maxV > info.observed[idx].minV) {
				return &info.observed[idx];
			}
			return nullptr;
		}

		static LONG AxisNeutral(const DeviceInfo& info, size_t offsetBytes, bool stickAxis) {
			size_t idx = offsetBytes / 4;
			const AxisRange* r = ResolveAxisRange(info, idx);
			if (r) {
				if (stickAxis) {
					return r->minV + (r->maxV - r->minV) / 2;
				}
				return r->minV;
			}
			if (idx < 32 && info.observed[idx].known) {
				const AxisRange& obs = info.observed[idx];
				return stickAxis ? obs.minV + (obs.maxV - obs.minV) / 2 : obs.minV;
			}
			return 0;
		}

		static bool IsStickAxisOffset(DWORD offsetBytes, bool isPlaystation) {
			if (offsetBytes == DIJOFS_X || offsetBytes == DIJOFS_Y) {
				return true;
			}
			if (offsetBytes == DIJOFS_Z || offsetBytes == DIJOFS_RZ) {
				return isPlaystation;
			}
			if (offsetBytes == DIJOFS_RX || offsetBytes == DIJOFS_RY) {
				return !isPlaystation;
			}
			return false;
		}

		static void NeutralizeJoystick(LPVOID data, DWORD cbData, const DeviceInfo& info) {
			std::memset(data, 0, cbData);

			if (cbData >= offsetof(DIJOYSTATE, rgbButtons)) {
				LONG* axes = static_cast<LONG*>(data);
				for (size_t axisIndex = 0; axisIndex < 8; ++axisIndex) {
					DWORD offsetBytes = static_cast<DWORD>(axisIndex * sizeof(LONG));
					axes[axisIndex] = AxisNeutral(info, offsetBytes, IsStickAxisOffset(offsetBytes, info.isPlaystation));
				}

				DWORD* pov = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(data) + 32);
				for (int i = 0; i < 4; ++i) {
					pov[i] = 0xFFFFFFFF;
				}
			}
		}

		static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceState(IDirectInputDevice8* self,
			DWORD cbData, LPVOID lpvData) {
			GetDeviceState_t origGetDeviceState = OrigGetDeviceState(self);
			HRESULT hr = origGetDeviceState ? origGetDeviceState(self, cbData, lpvData) : E_FAIL;
			if (FAILED(hr) || !lpvData || cbData == 0) {
				return hr;
			}
			GetProperty_t origGetProperty = OrigGetProperty(self);

			DeviceKind kind = DeviceKind::Unknown;
			bool selfOpened = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					kind = it->second.kind;
					selfOpened = it->second.selfOpened;
				}
			}

			if (kind == DeviceKind::Joystick) {
				bool needRangeQuery = false;
				{
					std::lock_guard<std::mutex> lock(g_mutex);
					auto it = g_deviceInfo.find(self);
					if (it != g_deviceInfo.end()) {
						DWORD copyBytes = cbData < sizeof(it->second.realState)
							? cbData : (DWORD)sizeof(it->second.realState);
						std::memcpy(it->second.realState, lpvData, copyBytes);
						it->second.realStateSize = copyBytes;
						it->second.hasRealState = true;
						if (cbData >= 8 * sizeof(LONG)) {
							const LONG* axes = static_cast<const LONG*>(lpvData);
							for (size_t a = 0; a < 8; ++a) {
								AxisRange& obs = it->second.observed[a];
								if (!obs.known) {
									obs.known = true;
									obs.minV = axes[a];
									obs.maxV = axes[a];
								} else {
									if (axes[a] < obs.minV) obs.minV = axes[a];
									if (axes[a] > obs.maxV) obs.maxV = axes[a];
								}
							}
						}
						needRangeQuery = origGetProperty && !it->second.deviceRange.known &&
							!it->second.rangeQueried;
					}
				}
				if (needRangeQuery) {
					DIPROPRANGE queried{};
					queried.diph.dwSize = sizeof(DIPROPRANGE);
					queried.diph.dwHeaderSize = sizeof(DIPROPHEADER);
					queried.diph.dwObj = 0;
					queried.diph.dwHow = DIPH_DEVICE;
					if (SUCCEEDED(origGetProperty(self, DIPROP_RANGE, &queried.diph))) {
						std::lock_guard<std::mutex> lock(g_mutex);
						auto it = g_deviceInfo.find(self);
						if (it != g_deviceInfo.end()) {
							it->second.deviceRange.known = true;
							it->second.deviceRange.minV = queried.lMin;
							it->second.deviceRange.maxV = queried.lMax;
							it->second.rangeQueried = true;
						}
					} else {
						std::lock_guard<std::mutex> lock(g_mutex);
						auto it = g_deviceInfo.find(self);
						if (it != g_deviceInfo.end()) {
							it->second.rangeQueried = true;
						}
					}
				}
			}

			if (selfOpened || !ShouldBlock(kind)) {
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
			GetDeviceData_t origGetDeviceData = OrigGetDeviceData(self);
			HRESULT hr = origGetDeviceData ? origGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags) : E_FAIL;
			if (FAILED(hr) || !pdwInOut || !rgdod || *pdwInOut == 0) {
				return hr;
			}

			DeviceKind kind = DeviceKind::Unknown;
			bool selfOpened = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					kind = it->second.kind;
					selfOpened = it->second.selfOpened;
				}
			}
			if (selfOpened || !ShouldBlock(kind)) {
				return hr;
			}

			const DWORD povOffsets[] = { DIJOFS_POV(0), DIJOFS_POV(1), DIJOFS_POV(2), DIJOFS_POV(3) };
			DeviceInfo info;
			if (kind == DeviceKind::Joystick) {
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					info = it->second;
				}
			}
			for (DWORD i = 0; i < *pdwInOut; ++i) {
			    bool isPov = false;
			    for (DWORD pov : povOffsets) {
			        if (rgdod[i].dwOfs == pov) { isPov = true; break; }
			    }
			    if (isPov) {
			        rgdod[i].dwData = 0xFFFFFFFFu;
			        continue;
			    }
			    if (kind == DeviceKind::Joystick && rgdod[i].dwOfs < 32 && (rgdod[i].dwOfs % 4) == 0) {
			        LONG neutral = AxisNeutral(info, rgdod[i].dwOfs, IsStickAxisOffset(rgdod[i].dwOfs, info.isPlaystation));
			        rgdod[i].dwData = static_cast<DWORD>(neutral);
			        continue;
			    }
			    if (rgdod[i].dwData == 0) {
			        continue;
			    }
			    rgdod[i].dwData = 0;
			}								
			return hr;
		}

		// detour

		static ULONG STDMETHODCALLTYPE Hooked_Release(IDirectInputDevice8* self) {
			void** vtableCopy = nullptr;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (g_wrappedObjects.find(self) != g_wrappedObjects.end()) {
					vtableCopy = *reinterpret_cast<void***>(self);
				}
			}
			Release_t origRelease = OrigRelease(self);
			ULONG refs = origRelease ? origRelease(self) : 0;
			if (refs == 0) {
				std::lock_guard<std::mutex> lock(g_mutex);
				g_deviceInfo.erase(self);
				g_wrappedObjects.erase(self);
				if (vtableCopy && g_vtableCopies.erase(vtableCopy)) {
					delete[] vtableCopy;
				}
			}
			return refs;
		}

		static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirectInput8* self,
			REFGUID rguid, LPDIRECTINPUTDEVICE8* lplpDevice, LPUNKNOWN pUnkOuter) {
			CreateDevice_t origCreateDevice = OrigCreateDevice(self);
			HRESULT hr = origCreateDevice ? origCreateDevice(self, rguid, lplpDevice, pUnkOuter) : E_FAIL;
			if (FAILED(hr) || !lplpDevice || !*lplpDevice) {
				return hr;
			}

			IDirectInputDevice8* device = *lplpDevice;
			if (self == g_ownDI8) {
				return hr;
			}
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
					g_origGetProperty = reinterpret_cast<GetProperty_t>(vtbl[kSlotGetProperty]);
					g_origRelease = reinterpret_cast<Release_t>(vtbl[kSlotRelease]);
					g_origAcquire = reinterpret_cast<Acquire_t>(vtbl[kSlotAcquire]);
					g_origGetDeviceState = reinterpret_cast<GetDeviceState_t>(vtbl[kSlotGetDeviceState]);
					g_origGetDeviceData = reinterpret_cast<GetDeviceData_t>(vtbl[kSlotGetDeviceData]);
				}
			}

			static const std::pair<size_t, void*> kDeviceOverrides[] = {
				{ kSlotSetProperty,      reinterpret_cast<void*>(&Hooked_SetProperty) },
				{ kSlotAcquire,          reinterpret_cast<void*>(&Hooked_Acquire) },
				{ kSlotGetDeviceState,   reinterpret_cast<void*>(&Hooked_GetDeviceState) },
				{ kSlotGetDeviceData,    reinterpret_cast<void*>(&Hooked_GetDeviceData) },
				{ kSlotRelease,          reinterpret_cast<void*>(&Hooked_Release) },
			};
			void** origVtbl = nullptr;
			bool installed = OverrideObjectVTable(device, kDeviceVTableSize,
				kDeviceOverrides, sizeof(kDeviceOverrides) / sizeof(kDeviceOverrides[0]), &origVtbl);
			if (installed && origVtbl) {
				std::lock_guard<std::mutex> lock(g_mutex);
				g_deviceInfo[device].origVTable = origVtbl;
			}

			spdlog::debug("DirectInputHook: wrapped device {:p} initial kind:{} ({})",
				static_cast<void*>(device), static_cast<int>(kind), installed ? "vtable wrapped" : "already wrapped");
			spdlog::default_logger()->flush();
			return hr;
		}

		static bool PassiveWrapEnabled() {
			static const bool enabled = [] {
				std::error_code ec;
				bool offFile = std::filesystem::exists(
					std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys" / "di_vtable_wrap_off.txt", ec);
				return !(offFile && !ec);
			}();
			return enabled;
		}

		static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
			REFGUID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
			HRESULT hr = g_origDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
			spdlog::info("DirectInputHook: DirectInput8Create called (dwVersion={:08X}, riid={:08X}, hr={:08X}, wrapping={})",
				dwVersion, riidltf.Data1, static_cast<unsigned>(hr),
				(SUCCEEDED(hr) && PassiveWrapEnabled()) ? "on" : "off");
			spdlog::default_logger()->flush();
			if (FAILED(hr) || !ppvOut || !*ppvOut) {
				return hr;
			}
			if (!PassiveWrapEnabled()) {
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
			void** origVtbl = nullptr;
			if (OverrideObjectVTable(directInput, kDirectInput8VTableSize,
				kDirectInputOverrides, sizeof(kDirectInputOverrides) / sizeof(kDirectInputOverrides[0]),
				&origVtbl) && origVtbl) {
				std::lock_guard<std::mutex> lock(g_mutex);
				g_di8OrigVTables[directInput] = origVtbl;
			}

			spdlog::debug("DirectInputHook: wrapped IDirectInput8 instance {:p}", static_cast<void*>(directInput));
			spdlog::default_logger()->flush();
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
			case 0: return pov <= 4500  || pov >= 31500;
			case 1: return pov >= 13500 && pov <= 22500;
			case 2: return pov >= 22500 && pov <= 31500;
			case 3: return pov >= 4500  && pov <= 13500;
			}
			return false;
		}

		static bool AxisPast(const DeviceInfo& info, const DIJOYSTATE* js,
			int axisIndex, size_t offsetBytes, int direction) {
			const LONG* axes = reinterpret_cast<const LONG*>(js);
			LONG value = axes[axisIndex];

			size_t idx = offsetBytes / 4;
			const AxisRange* r = ResolveAxisRange(info, idx);
			if (r) {
				LONG low = r->minV;
				LONG high = r->maxV;
				LONG threshold = (high - low) / 2;
				if (direction < 0) return value <= low + threshold / 2;
				return value >= low + threshold + (high - low) / 4;
			}
			LONG center = 0;
			if (idx < 32 && info.observed[idx].known) {
				center = (info.observed[idx].minV + info.observed[idx].maxV) / 2;
			}
			const LONG kStickThreshold = 8000;
			return direction < 0 ? value <= center - kStickThreshold : value >= center + kStickThreshold;
		}

		static bool TriggerHeld(const DeviceInfo& info, const DIJOYSTATE* js, int which) {
			LONG value = (which == 0) ? js->rglSlider[0] : js->rglSlider[1];
			size_t offsetBytes = DIJOFS_SLIDER(which);
			size_t idx = offsetBytes / 4;
			const AxisRange* r = ResolveAxisRange(info, idx);
			if (r) {
				return value >= r->minV + (r->maxV - r->minV) * 4 / 5;
			}
			if (which == 0 && js->lZ > 20000)  return true;
			if (which == 1 && js->lRz > 20000) return true;
			return false;
		}

		static bool PsTriggerHeld(const DeviceInfo& info, const DIJOYSTATE* js, int axisIndex) {
			const LONG* axes = reinterpret_cast<const LONG*>(js);
			LONG value = axes[axisIndex];
			const AxisRange* r = ResolveAxisRange(info, static_cast<size_t>(axisIndex));
			if (r) {
				return value >= r->minV + (r->maxV - r->minV) * 4 / 5;
			}
			return value >= 20000;
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
				const DeviceInfo& info = entry.second;
				if (info.selfOpened || info.isPlaystation) {
					continue;
				}
				if (IsGamepadButtonHeldLocked(vKey, info)) {
					return true;
				}
			}
			return false;
		}

		static bool IsPlaystationControlHeldLocked(USHORT vKey, const DeviceInfo& info) {
			if (!info.hasRealState || info.realStateSize < sizeof(DIJOYSTATE) ||
				info.kind != DeviceKind::Joystick) {
				return false;
			}
			const DIJOYSTATE* js = reinterpret_cast<const DIJOYSTATE*>(info.realState);

			if (info.steamVirtual360) {
				switch (vKey) {
				case RawInput::VK_PS_CROSS:                  return ButtonHeld(js, 0);
				case RawInput::VK_PS_CIRCLE:                 return ButtonHeld(js, 1);
				case RawInput::VK_PS_SQUARE:                 return ButtonHeld(js, 2);
				case RawInput::VK_PS_TRIANGLE:               return ButtonHeld(js, 3);
				case RawInput::VK_PS_L1:                     return ButtonHeld(js, 4);
				case RawInput::VK_PS_R1:                     return ButtonHeld(js, 5);
				case RawInput::VK_PS_SHARE:                  return ButtonHeld(js, 6);
				case RawInput::VK_PS_OPTIONS:                return ButtonHeld(js, 7);
				case RawInput::VK_PS_L3:                     return ButtonHeld(js, 8);
				case RawInput::VK_PS_R3:                     return ButtonHeld(js, 9);
				case RawInput::VK_PS_L2:                     return TriggerHeld(info, js, 0);
				case RawInput::VK_PS_R2:                     return TriggerHeld(info, js, 1);
				case RawInput::VK_PS_DPAD_UP:                return PovHeld(js, 0, 0);
				case RawInput::VK_PS_DPAD_DOWN:              return PovHeld(js, 0, 1);
				case RawInput::VK_PS_DPAD_LEFT:              return PovHeld(js, 0, 2);
				case RawInput::VK_PS_DPAD_RIGHT:             return PovHeld(js, 0, 3);
				case RawInput::VK_PS_LS_UP:                  return AxisPast(info, js, 1, DIJOFS_Y, -1);
				case RawInput::VK_PS_LS_DOWN:                return AxisPast(info, js, 1, DIJOFS_Y, +1);
				case RawInput::VK_PS_LS_LEFT:                return AxisPast(info, js, 0, DIJOFS_X, -1);
				case RawInput::VK_PS_LS_RIGHT:               return AxisPast(info, js, 0, DIJOFS_X, +1);
				case RawInput::VK_PS_RS_UP:                  return AxisPast(info, js, 4, DIJOFS_RY, -1);
				case RawInput::VK_PS_RS_DOWN:                return AxisPast(info, js, 4, DIJOFS_RY, +1);
				case RawInput::VK_PS_RS_LEFT:                return AxisPast(info, js, 3, DIJOFS_RX, -1);
				case RawInput::VK_PS_RS_RIGHT:               return AxisPast(info, js, 3, DIJOFS_RX, +1);
				default: return false;
				}
			}

			switch (vKey) {
			case RawInput::VK_PS_SQUARE:                 return ButtonHeld(js, 0);
			case RawInput::VK_PS_CROSS:                  return ButtonHeld(js, 1);
			case RawInput::VK_PS_CIRCLE:                 return ButtonHeld(js, 2);
			case RawInput::VK_PS_TRIANGLE:               return ButtonHeld(js, 3);
			case RawInput::VK_PS_L1:                     return ButtonHeld(js, 4);
			case RawInput::VK_PS_R1:                     return ButtonHeld(js, 5);
			case RawInput::VK_PS_L2:                     return ButtonHeld(js, 6) || PsTriggerHeld(info, js, 3);
			case RawInput::VK_PS_R2:                     return ButtonHeld(js, 7) || PsTriggerHeld(info, js, 4);
			case RawInput::VK_PS_SHARE:                  return ButtonHeld(js, 8);
			case RawInput::VK_PS_OPTIONS:                return ButtonHeld(js, 9);
			case RawInput::VK_PS_L3:                     return ButtonHeld(js, 10);
			case RawInput::VK_PS_R3:                     return ButtonHeld(js, 11);
			case RawInput::VK_PS_DPAD_UP:                return PovHeld(js, 0, 0);
			case RawInput::VK_PS_DPAD_DOWN:              return PovHeld(js, 0, 1);
			case RawInput::VK_PS_DPAD_LEFT:              return PovHeld(js, 0, 2);
			case RawInput::VK_PS_DPAD_RIGHT:             return PovHeld(js, 0, 3);
			case RawInput::VK_PS_LS_UP:                  return AxisPast(info, js, 1, DIJOFS_Y, -1);
			case RawInput::VK_PS_LS_DOWN:                return AxisPast(info, js, 1, DIJOFS_Y, +1);
			case RawInput::VK_PS_LS_LEFT:                return AxisPast(info, js, 0, DIJOFS_X, -1);
			case RawInput::VK_PS_LS_RIGHT:               return AxisPast(info, js, 0, DIJOFS_X, +1);
			case RawInput::VK_PS_RS_UP:                  return AxisPast(info, js, 5, DIJOFS_RZ, -1);
			case RawInput::VK_PS_RS_DOWN:                return AxisPast(info, js, 5, DIJOFS_RZ, +1);
			case RawInput::VK_PS_RS_LEFT:                return AxisPast(info, js, 2, DIJOFS_Z, -1);
			case RawInput::VK_PS_RS_RIGHT:               return AxisPast(info, js, 2, DIJOFS_Z, +1);
			default: return false;
			}
		}

		bool IsPlaystationControlHeld(USHORT vKey) {
			std::lock_guard<std::mutex> lock(g_mutex);
			for (const auto& entry : g_deviceInfo) {
				const DeviceInfo& info = entry.second;
				if (info.kind != DeviceKind::Joystick || !info.isPlaystation) {
					continue;
				}
				if (IsPlaystationControlHeldLocked(vKey, info)) {
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

		bool HasPlaystationDevice() {
			std::lock_guard<std::mutex> lock(g_mutex);
			for (const auto& entry : g_deviceInfo) {
				if (entry.second.kind == DeviceKind::Joystick && entry.second.isPlaystation) {
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
			spdlog::info("DirectInputHook: passive vtable wrapping {} (kill switch: mod/radarKeys/di_vtable_wrap_off.txt)",
				PassiveWrapEnabled() ? "ENABLED" : "DISABLED");
			spdlog::default_logger()->flush();
		}

		static std::vector<std::pair<GUID, IDirectInputDevice8*>> g_ownedDevices;
		static ULONGLONG g_lastEnumTick = 0;
		static int g_enumCallbackSeen = 0;

		static bool EnumWarnOnceForGuid(int category, REFGUID guid) {
			static std::vector<GUID> warnedCreate;
			static std::vector<GUID> warnedFormat;
			static std::vector<GUID> warnedCoop;
			static std::vector<GUID> warnedAcquire;
			std::vector<GUID>* warned = &warnedCreate;
			if (category == 1) warned = &warnedFormat;
			else if (category == 2) warned = &warnedCoop;
			else if (category == 3) warned = &warnedAcquire;
			for (const auto& entry : *warned) {
				if (SameGuid(entry, guid)) {
					return false;
				}
			}
			warned->push_back(guid);
			return true;
		}

		static LPCDIDATAFORMAT DefaultJoystickFormat() {
			static LPCDIDATAFORMAT cached = []() -> LPCDIDATAFORMAT {
				HMODULE module = GetModuleHandleW(L"dinput8.dll");
				if (!module) {
					module = LoadLibraryW(L"dinput8.dll");
				}
				if (!module) {
					return nullptr;
				}
				typedef const DIDATAFORMAT* (WINAPI* GetJoystickFormatFunc)();
				auto getter = reinterpret_cast<GetJoystickFormatFunc>(GetProcAddress(module, "GetdfDIJoystick"));
				if (getter) {
					return getter();
				}
				return reinterpret_cast<LPCDIDATAFORMAT>(GetProcAddress(module, "c_dfDIJoystick"));
			}();
			return cached;
		}

		static const DIOBJECTDATAFORMAT kJoystickObjectFormat[] = {
			{ const_cast<LPGUID>(&GUID_XAxis),  offsetof(DIJOYSTATE, lX),  DIDFT_OPTIONAL | DIDFT_AXIS   | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_YAxis),  offsetof(DIJOYSTATE, lY),  DIDFT_OPTIONAL | DIDFT_AXIS   | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_ZAxis),  offsetof(DIJOYSTATE, lZ),  DIDFT_OPTIONAL | DIDFT_AXIS   | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_RxAxis), offsetof(DIJOYSTATE, lRx), DIDFT_OPTIONAL | DIDFT_AXIS   | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_RyAxis), offsetof(DIJOYSTATE, lRy), DIDFT_OPTIONAL | DIDFT_AXIS   | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_RzAxis), offsetof(DIJOYSTATE, lRz), DIDFT_OPTIONAL | DIDFT_AXIS   | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_Slider), offsetof(DIJOYSTATE, rglSlider[0]), DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_Slider), offsetof(DIJOYSTATE, rglSlider[1]), DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
			{ const_cast<LPGUID>(&GUID_POV), offsetof(DIJOYSTATE, rgdwPOV[0]), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0 },
			{ const_cast<LPGUID>(&GUID_POV), offsetof(DIJOYSTATE, rgdwPOV[1]), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0 },
			{ const_cast<LPGUID>(&GUID_POV), offsetof(DIJOYSTATE, rgdwPOV[2]), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0 },
			{ const_cast<LPGUID>(&GUID_POV), offsetof(DIJOYSTATE, rgdwPOV[3]), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[0]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[1]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[2]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[3]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[4]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[5]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[6]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[7]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[8]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[9]),  DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[10]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[11]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[12]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[13]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[14]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[15]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[16]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[17]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[18]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[19]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[20]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[21]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[22]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[23]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[24]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[25]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[26]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[27]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[28]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[29]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[30]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
			{ nullptr, offsetof(DIJOYSTATE, rgbButtons[31]), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
		};

		static const DIDATAFORMAT kJoystickFormat = {
			sizeof(DIDATAFORMAT),
			sizeof(DIOBJECTDATAFORMAT),
			DIDF_ABSAXIS,
			sizeof(DIJOYSTATE),
			sizeof(kJoystickObjectFormat) / sizeof(kJoystickObjectFormat[0]),
			const_cast<DIOBJECTDATAFORMAT*>(kJoystickObjectFormat)
		};

		static bool SonyGamepadPresentInSystem() {
			UINT count = 0;
			if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0 || count > 512) {
				return false;
			}
			RAWINPUTDEVICELIST list[512];
			UINT got = GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST));
			if (got == static_cast<UINT>(-1) || got == 0) {
				return false;
			}
			for (UINT i = 0; i < got; ++i) {
				if (list[i].dwType != RIM_TYPEHID) {
					continue;
				}
				wchar_t name[256] = L"";
				UINT size = static_cast<UINT>(sizeof(name));
				if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name, &size) == static_cast<UINT>(-1)) {
					continue;
				}
				for (wchar_t* p = name; *p; ++p) {
					if (*p >= L'a' && *p <= L'z') {
						*p = static_cast<wchar_t>(*p - (L'a' - L'A'));
					}
				}
				if (wcsstr(name, L"VID_054C") != nullptr || wcsstr(name, L"VID&0002054C") != nullptr) {
					return true;
				}
			}
			return false;
		}

		bool IsSonyGamepadAttachedToSystem() {
			return SonyGamepadPresentInSystem();
		}

		static void ClassifyOwnedDevice(IDirectInputDevice8* device, DeviceInfo& info) {
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(DIDEVCAPS);
			if (SUCCEEDED(device->GetCapabilities(&caps))) {
				switch (caps.dwDevType & 0xFF) {
				case DI8DEVTYPE_KEYBOARD: info.kind = DeviceKind::Keyboard; break;
				case DI8DEVTYPE_MOUSE:    info.kind = DeviceKind::Mouse; break;
				default:                  info.kind = DeviceKind::Joystick; break;
				}
				info.kindFromCapabilities = true;
			} else {
				info.kind = DeviceKind::Joystick;
			}

			DIDEVICEINSTANCE diInfo{};
			diInfo.dwSize = sizeof(DIDEVICEINSTANCE);
			if (FAILED(device->GetDeviceInfo(&diInfo))) {
				info.productQueried = true;
				return;
			}
			info.productQueried = true;

			auto toNarrow = [](const wchar_t* w) -> std::string {
				std::string s;
				while (*w) { s.push_back(static_cast<char>(*w)); ++w; }
				return s;
			};
			auto toUpperAscii = [](wchar_t c) -> wchar_t {
				return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
			};
			wchar_t upperProduct[MAX_PATH] = L"";
			for (size_t i = 0; i < MAX_PATH - 1 && diInfo.tszProductName[i] != L'\0'; ++i) {
				upperProduct[i] = toUpperAscii(diInfo.tszProductName[i]);
			}

			const bool nameMatch =
				wcsstr(upperProduct, L"DUALSHOCK") != nullptr ||
				wcsstr(upperProduct, L"DUALSENSE") != nullptr ||
				wcsstr(upperProduct, L"PLAYSTATION") != nullptr ||
				wcsstr(upperProduct, L"WIRELESS CONTROLLER") != nullptr;
			const WORD vendorId = static_cast<WORD>(diInfo.guidProduct.Data1 & 0xFFFF);
			const WORD productId = static_cast<WORD>((diInfo.guidProduct.Data1 >> 16) & 0xFFFF);
			const bool vidMatch = (vendorId == 0x054C);
			info.steamVirtual360 = (vendorId == 0x28DE && productId == 0x11FF);

			info.isPlaystation = nameMatch || vidMatch ||
				(info.steamVirtual360 && SonyGamepadPresentInSystem());

			DIPROPRANGE deviceQuery{};
			deviceQuery.diph.dwSize = sizeof(DIPROPRANGE);
			deviceQuery.diph.dwHeaderSize = sizeof(DIPROPHEADER);
			deviceQuery.diph.dwObj = 0;
			deviceQuery.diph.dwHow = DIPH_DEVICE;
			if (SUCCEEDED(device->GetProperty(DIPROP_RANGE, &deviceQuery.diph))) {
				info.deviceRange.known = true;
				info.deviceRange.minV = deviceQuery.lMin;
				info.deviceRange.maxV = deviceQuery.lMax;
			}
			info.rangeQueried = true;

			spdlog::info("DirectInputHook: self-opened device {:p} PlayStation button mapping {} (product:\"{}\", vid:{:04X}, pid:{:04X})",
				static_cast<void*>(device), info.isPlaystation ? "ENABLED" : "disabled",
				toNarrow(diInfo.tszProductName), vendorId, productId);
		}

		static BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCE* pdidInstance, VOID* pContext) {
			HWND hwnd = *reinterpret_cast<HWND*>(pContext);
			++g_enumCallbackSeen;

			for (const auto& owned : g_ownedDevices) {
				if (SameGuid(owned.first, pdidInstance->guidInstance)) {
					return DIENUM_CONTINUE;
				}
			}

			auto toNarrow = [](const wchar_t* w) -> std::string {
				std::string s;
				while (*w) { s.push_back(static_cast<char>(*w)); ++w; }
				return s;
			};

			IDirectInputDevice8* device = nullptr;
			HRESULT hrCreate = g_ownDI8->CreateDevice(pdidInstance->guidInstance, &device, nullptr);
			if (FAILED(hrCreate) || !device) {
				if (device) {
					device->Release();
				}
				if (EnumWarnOnceForGuid(0, pdidInstance->guidInstance)) {
					spdlog::warn("DirectInputHook: CreateDevice failed for \"{}\" (hr={:08X})",
						toNarrow(pdidInstance->tszProductName), static_cast<unsigned>(hrCreate));
				}
				return DIENUM_CONTINUE;
			}
			HRESULT hrFormat = device->SetDataFormat(&kJoystickFormat);
			if (FAILED(hrFormat)) {
				LPCDIDATAFORMAT fallbackFormat = DefaultJoystickFormat();
				HRESULT hrFallback = fallbackFormat ? device->SetDataFormat(fallbackFormat) : E_POINTER;
				if (FAILED(hrFallback)) {
					device->Release();
					if (EnumWarnOnceForGuid(1, pdidInstance->guidInstance)) {
						spdlog::warn("DirectInputHook: SetDataFormat failed for \"{}\" (custom hr={:08X}, default hr={:08X})",
							toNarrow(pdidInstance->tszProductName), static_cast<unsigned>(hrFormat), static_cast<unsigned>(hrFallback));
					}
					return DIENUM_CONTINUE;
				}
				spdlog::info("DirectInputHook: \"{}\" accepted only the default joystick data format (custom hr={:08X})",
					toNarrow(pdidInstance->tszProductName), static_cast<unsigned>(hrFormat));
			}
			if (hwnd) {
				HRESULT hrCoop = device->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
				if (FAILED(hrCoop) && EnumWarnOnceForGuid(2, pdidInstance->guidInstance)) {
					spdlog::warn("DirectInputHook: SetCooperativeLevel failed for \"{}\" (hr={:08X})",
						toNarrow(pdidInstance->tszProductName), static_cast<unsigned>(hrCoop));
				}
			}
			HRESULT hrAcquire = device->Acquire();
			if (FAILED(hrAcquire) && hrAcquire != S_FALSE && EnumWarnOnceForGuid(3, pdidInstance->guidInstance)) {
				spdlog::warn("DirectInputHook: Acquire failed for \"{}\" (hr={:08X})",
					toNarrow(pdidInstance->tszProductName), static_cast<unsigned>(hrAcquire));
			}

			DeviceInfo info;
			info.selfOpened = true;
			ClassifyOwnedDevice(device, info);
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto existing = g_deviceInfo.find(device);
				if (existing != g_deviceInfo.end()) {
					info.origVTable = existing->second.origVTable;
				}
				g_deviceInfo[device] = info;
			}
			g_ownedDevices.emplace_back(pdidInstance->guidInstance, device);
			spdlog::info("DirectInputHook: self-opened joystick device {:p} ({})",
				static_cast<void*>(device), info.isPlaystation ? "PlayStation" : "generic");

			return DIENUM_CONTINUE;
		}

		static void EnsureOwnDirectInput() {
			if (g_ownDI8) {
				return;
			}
			HMODULE module = GetModuleHandleW(L"dinput8.dll");
			if (!module) {
				module = LoadLibraryW(L"dinput8.dll");
			}
			if (!module) {
				static bool warnedModule = false;
				if (!warnedModule) {
					warnedModule = true;
					spdlog::warn("DirectInputHook: self-polling unavailable - dinput8.dll could not be loaded");
				}
				return;
			}
			auto create = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(module, "DirectInput8Create"));
			if (!create) {
				static bool warnedExport = false;
				if (!warnedExport) {
					warnedExport = true;
					spdlog::warn("DirectInputHook: self-polling unavailable - DirectInput8Create export not found");
				}
				return;
			}
			void* out = nullptr;
			HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, &out, nullptr);
			if (FAILED(hr) || !out) {
				static bool warnedCreate = false;
				if (!warnedCreate) {
					warnedCreate = true;
					spdlog::warn("DirectInputHook: self-polling unavailable - IDirectInput8 creation failed (hr={:08X})", static_cast<unsigned>(hr));
				}
				return;
			}
			g_ownDI8 = static_cast<IDirectInput8*>(out);
			spdlog::info("DirectInputHook: opened an independent IDirectInput8 instance for self-driven controller polling");
		}

		void Poll(HWND hwnd) {
			EnsureOwnDirectInput();
			if (!g_ownDI8) {
				return;
			}
			if (!hwnd) {
				hwnd = GetActiveWindow();
			}

			static bool loggedPollStart = false;
			if (!loggedPollStart) {
				loggedPollStart = true;
				spdlog::info("DirectInputHook: self-polling active (hwnd={:p})", static_cast<void*>(hwnd));
			}

			ULONGLONG now = GetTickCount64();
			if (now - g_lastEnumTick >= 2000) {
				g_lastEnumTick = now;
				HWND ctxHwnd = hwnd;
				g_enumCallbackSeen = 0;
				HRESULT hrEnum = g_ownDI8->EnumDevices(DI8DEVCLASS_GAMECTRL, &EnumJoysticksCallback, &ctxHwnd, DIEDFL_ATTACHEDONLY);
				if (FAILED(hrEnum)) {
					static bool warnedEnum = false;
					if (!warnedEnum) {
						warnedEnum = true;
						spdlog::warn("DirectInputHook: EnumDevices failed (hr={:08X})", static_cast<unsigned>(hrEnum));
					}
				} else if (g_enumCallbackSeen == 0 && g_ownedDevices.empty()) {
					static bool warnedNoDevices = false;
					if (!warnedNoDevices) {
						warnedNoDevices = true;
						spdlog::warn("DirectInputHook: Windows reports zero DirectInput game controllers - connected pads are XInput-only or hidden by another driver");
					}
				}

				std::lock_guard<std::mutex> lock(g_mutex);
				bool sonyChecked = false;
				bool sonyPresent = false;
				for (auto& entry : g_deviceInfo) {
					DeviceInfo& info = entry.second;
					if (!info.selfOpened || !info.steamVirtual360) {
						continue;
					}
					if (!sonyChecked) {
						sonyPresent = SonyGamepadPresentInSystem();
						sonyChecked = true;
					}
					if (info.isPlaystation != sonyPresent) {
						info.isPlaystation = sonyPresent;
						spdlog::info("DirectInputHook: Steam virtual 360 device {:p} PlayStation identity {} (Sony pad {})",
							static_cast<void*>(entry.first), sonyPresent ? "ENABLED" : "disabled",
							sonyPresent ? "present" : "not present");
					}
				}
			}

			for (auto& owned : g_ownedDevices) {
				IDirectInputDevice8* device = owned.second;
				device->Poll();

				DIJOYSTATE state{};
				HRESULT hr = device->GetDeviceState(sizeof(DIJOYSTATE), &state);
				if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
					if (hwnd) {
						device->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
					}
					device->Acquire();
					continue;
				}
				if (FAILED(hr)) {
					continue;
				}

				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_deviceInfo.find(device);
				if (it == g_deviceInfo.end()) {
					continue;
				}
				DWORD copyBytes = sizeof(state) < sizeof(it->second.realState)
					? sizeof(state) : (DWORD)sizeof(it->second.realState);
				std::memcpy(it->second.realState, &state, copyBytes);
				it->second.realStateSize = copyBytes;
				it->second.hasRealState = true;
				if (copyBytes >= 8 * sizeof(LONG)) {
					const LONG* axes = reinterpret_cast<const LONG*>(it->second.realState);
					for (size_t a = 0; a < 8; ++a) {
						AxisRange& obs = it->second.observed[a];
						if (!obs.known) {
							obs.known = true;
							obs.minV = axes[a];
							obs.maxV = axes[a];
						} else {
							if (axes[a] < obs.minV) obs.minV = axes[a];
							if (axes[a] > obs.maxV) obs.maxV = axes[a];
						}
					}
				}
			}
		}

		void Shutdown() {
			for (auto& owned : g_ownedDevices) {
				owned.second->Unacquire();
				owned.second->Release();
			}
			g_ownedDevices.clear();
			if (g_ownDI8) {
				g_ownDI8->Release();
				g_ownDI8 = nullptr;
			}
		}
	}
}
