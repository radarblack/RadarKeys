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
#include <string>
#include <atomic>
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
		static constexpr size_t kDeviceVTableSize = 32;
		static constexpr size_t kSlotGetCapabilities = 3;
		static constexpr size_t kSlotRelease = 2;
		static constexpr size_t kSlotGetProperty = 5;
		static constexpr size_t kSlotCreateDevice = 3;
		static constexpr size_t kSlotSetProperty = 6;
		static constexpr size_t kSlotAcquire = 7;
		static constexpr size_t kSlotGetDeviceState = 9;
		static constexpr size_t kSlotGetDeviceData = 10;
		static constexpr size_t kSlotGetDeviceInfo = 15;
		static constexpr size_t kSlotEnumObjects = 4;

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
		static std::unordered_set<void*> g_selfDevicePointers;
		static std::vector<std::pair<GUID, IDirectInputDevice8*>> g_ownedDevices;

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
			case DI8DEVTYPE_DRIVING:
			case DI8DEVTYPE_FLIGHT:
			case DI8DEVTYPE_1STPERSON:
			case DI8DEVTYPE_SUPPLEMENTAL:
			case 4:
				kind = DeviceKind::Joystick;
				break;
			default:
				kind = DeviceKind::Unknown;
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

		static std::unordered_map<void*, void*> g_classGetDeviceStateOrigins;
		static std::unordered_map<void*, void*> g_classGetDeviceDataOrigins;
		static std::atomic<bool> g_classGetDeviceStateHooked{ false };
		static std::atomic<bool> g_classGetDeviceDataHooked{ false };
		static std::unordered_map<void*, ULONGLONG> g_lastSuppressedJoystickLog;
		struct GameDeviceFormat {
			bool valid = false;
			bool buildAttempted = false;
			std::vector<std::pair<DWORD, LONG>> axisNeutrals;
			std::vector<DWORD> povOfs;
			std::vector<DWORD> buttonOfs;
		};
		static std::unordered_map<void*, GameDeviceFormat> g_gameDeviceFormats;
		static std::unordered_set<void*> g_classHookTargets;

		static bool IsSelfOpenedDevice(IDirectInputDevice8* self) {
			std::lock_guard<std::mutex> lock(g_mutex);
			return g_selfDevicePointers.find(self) != g_selfDevicePointers.end();
		}

		static DeviceInfo AcquireGameJoystickInfo(IDirectInputDevice8* self, const LPVOID lpvData, DWORD cbData) {
			DeviceInfo info;
			std::lock_guard<std::mutex> lock(g_mutex);
			DeviceInfo& stored = g_deviceInfo[self];
			if (stored.kind == DeviceKind::Unknown) {
				stored.kind = DeviceKind::Joystick;
				const DeviceInfo* firstSelf = nullptr;
				const DeviceInfo* psSelf = nullptr;
				for (const auto& entry : g_deviceInfo) {
					if (entry.first == self || !entry.second.selfOpened) {
						continue;
					}
					if (!firstSelf) {
						firstSelf = &entry.second;
					}
					if (entry.second.isPlaystation) {
						psSelf = &entry.second;
						break;
					}
				}
				const DeviceInfo* seed = psSelf ? psSelf : firstSelf;
				if (seed) {
					for (size_t a = 0; a < 8; ++a) {
						stored.observed[a] = seed->observed[a];
					}
					stored.isPlaystation = seed->isPlaystation;
				}
			}
			if (lpvData && cbData >= 8 * sizeof(LONG)) {
				const LONG* axes = static_cast<const LONG*>(lpvData);
				for (size_t a = 0; a < 8; ++a) {
					AxisRange& obs = stored.observed[a];
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
			info = stored;
			return info;
		}

		static bool JoystickStateActive(const DeviceInfo& info, LPVOID lpvData, DWORD cbData) {
			const unsigned char* bytes = static_cast<const unsigned char*>(lpvData);
			if (cbData >= offsetof(DIJOYSTATE, rgbButtons) + 4) {
				for (size_t i = 0; i < 4; ++i) {
					if (bytes[offsetof(DIJOYSTATE, rgbButtons) + i] & 0x80) {
						return true;
					}
				}
			}
			if (cbData >= 8 * sizeof(LONG)) {
				const LONG* axes = static_cast<const LONG*>(lpvData);
				for (size_t a = 0; a < 8; ++a) {
					DWORD offsetBytes = static_cast<DWORD>(a * sizeof(LONG));
					if (axes[a] != AxisNeutral(info, offsetBytes, IsStickAxisOffset(offsetBytes, info.isPlaystation))) {
						return true;
					}
				}
			}
			if (cbData >= offsetof(DIJOYSTATE, rgdwPOV) + sizeof(DWORD)) {
				const DWORD* pov = reinterpret_cast<const DWORD*>(bytes + offsetof(DIJOYSTATE, rgdwPOV));
				if (pov[0] <= 35999) {
					return true;
				}
			}
			return false;
		}

		static void LogSuppressedJoystick(IDirectInputDevice8* self, LPVOID lpvData, DWORD cbData, const GameDeviceFormat* fmt) {
			const ULONGLONG now = GetTickCount64();
			ULONGLONG& last = g_lastSuppressedJoystickLog[self];
			if (now - last < 1000) {
				return;
			}
			last = now;
			if (fmt && lpvData) {
				std::string axisText;
				char valueText[40];
				size_t printed = 0;
				for (const auto& axis : fmt->axisNeutrals) {
					if (axis.first + sizeof(LONG) <= cbData) {
						if (printed > 0) {
							axisText.push_back(' ');
						}
						sprintf_s(valueText, sizeof(valueText), "@%u=%d",
							static_cast<unsigned>(axis.first),
							static_cast<int>(*reinterpret_cast<const LONG*>(static_cast<const unsigned char*>(lpvData) + axis.first)));
						axisText += valueText;
						++printed;
						if (printed >= 8) {
							break;
						}
					}
				}
				spdlog::info("DirectInputHook: suppressed game joystick {:p} pre-neutral [{}]",
					static_cast<void*>(self), axisText);
				spdlog::default_logger()->flush();
				return;
			}
			if (lpvData && cbData >= 8 * sizeof(LONG)) {
				const LONG* axes = static_cast<const LONG*>(lpvData);
				const unsigned char* bytes = static_cast<const unsigned char*>(lpvData);
				if (cbData >= offsetof(DIJOYSTATE, rgbButtons) + 4) {
					spdlog::info("DirectInputHook: suppressed game joystick {:p} input (buttons {:02X} {:02X} {:02X} {:02X}, x {} y {} z {} rx {} ry {} rz {})",
						static_cast<void*>(self),
						bytes[offsetof(DIJOYSTATE, rgbButtons)], bytes[offsetof(DIJOYSTATE, rgbButtons) + 1],
						bytes[offsetof(DIJOYSTATE, rgbButtons) + 2], bytes[offsetof(DIJOYSTATE, rgbButtons) + 3],
						axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
					spdlog::default_logger()->flush();
					return;
				}
			}
			spdlog::info("DirectInputHook: suppressed game joystick {:p} buffered input", static_cast<void*>(self));
			spdlog::default_logger()->flush();
		}

		typedef HRESULT(STDMETHODCALLTYPE* DiEnumObjects_t)(IDirectInputDevice8*, void*, void*, DWORD);


		struct FormatBuildContext {
			IDirectInputDevice8* device;
			GetProperty_t getProperty;
			GameDeviceFormat* format;
			int axisCount;
		};

		static BOOL CALLBACK GameFormatObjectCallback(const DIDEVICEOBJECTINSTANCEW* pdidoi, void* pContext) {
			FormatBuildContext* ctx = reinterpret_cast<FormatBuildContext*>(pContext);
			if (!pdidoi || (pdidoi->dwType & DIDFT_NODATA) != 0) {
				return DIENUM_CONTINUE;
			}
			if ((pdidoi->dwType & DIDFT_POV) != 0) {
				ctx->format->povOfs.push_back(pdidoi->dwOfs);
				return DIENUM_CONTINUE;
			}
			if ((pdidoi->dwType & DIDFT_BUTTON) != 0) {
				ctx->format->buttonOfs.push_back(pdidoi->dwOfs);
				return DIENUM_CONTINUE;
			}
			if ((pdidoi->dwType & DIDFT_AXIS) != 0) {
				DIPROPRANGE range{};
				range.diph.dwSize = sizeof(DIPROPRANGE);
				range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
				range.diph.dwObj = pdidoi->dwOfs;
				range.diph.dwHow = DIPH_BYOFFSET;
				if (SUCCEEDED(ctx->getProperty(ctx->device, DIPROP_RANGE, &range.diph)) &&
					range.lMax > range.lMin) {
					ctx->format->axisNeutrals.push_back({ pdidoi->dwOfs, range.lMin + (range.lMax - range.lMin) / 2 });
					++ctx->axisCount;
				}
			}
			return DIENUM_CONTINUE;
		}

		static void SuppressGameJoystickRead(IDirectInputDevice8* self, LPVOID lpvData, DWORD cbData) {
			std::lock_guard<std::mutex> lock(g_mutex);
			GameDeviceFormat& fmt = g_gameDeviceFormats[self];
			if (!fmt.valid && !fmt.buildAttempted) {
				fmt.buildAttempted = true;
				void** vtbl = *reinterpret_cast<void***>(self);
				DiEnumObjects_t enumObjects = reinterpret_cast<DiEnumObjects_t>(vtbl[kSlotEnumObjects]);
				GetProperty_t getProperty = reinterpret_cast<GetProperty_t>(vtbl[kSlotGetProperty]);
				if (enumObjects && getProperty) {
					FormatBuildContext ctx{ self, getProperty, &fmt, 0 };
					if (SUCCEEDED(enumObjects(self, reinterpret_cast<void*>(&GameFormatObjectCallback), &ctx,
						DIDFT_AXIS | DIDFT_POV | DIDFT_BUTTON))) {
						fmt.valid = true;
						spdlog::info("DirectInputHook: built read map for game joystick {:p} (axes {}, buttons {}, povs {}, cbData {})",
							static_cast<void*>(self), ctx.axisCount, fmt.buttonOfs.size(), fmt.povOfs.size(), cbData);
						spdlog::default_logger()->flush();
					}
				}
			}
			bool active = false;
			if (fmt.valid) {
				const unsigned char* bytes = static_cast<const unsigned char*>(lpvData);
				for (const auto& axis : fmt.axisNeutrals) {
					if (axis.first + sizeof(LONG) <= cbData &&
						*reinterpret_cast<const LONG*>(bytes + axis.first) != axis.second) {
						active = true;
						break;
					}
				}
				if (!active) {
					for (DWORD ofs : fmt.buttonOfs) {
						if (ofs < cbData && (bytes[ofs] & 0x80) != 0) {
							active = true;
							break;
						}
					}
				}
				if (!active) {
					for (DWORD ofs : fmt.povOfs) {
						if (ofs + sizeof(DWORD) <= cbData &&
							*reinterpret_cast<const DWORD*>(bytes + ofs) <= 35999) {
							active = true;
							break;
						}
					}
				}
				if (active) {
					LogSuppressedJoystick(self, lpvData, cbData, &fmt);
				}
				std::memset(lpvData, 0, cbData);
				for (const auto& axis : fmt.axisNeutrals) {
					if (axis.first + sizeof(LONG) <= cbData) {
						*reinterpret_cast<LONG*>(static_cast<unsigned char*>(lpvData) + axis.first) = axis.second;
					}
				}
				for (DWORD ofs : fmt.povOfs) {
					if (ofs + sizeof(DWORD) <= cbData) {
						*reinterpret_cast<DWORD*>(static_cast<unsigned char*>(lpvData) + ofs) = 0xFFFFFFFF;
					}
				}
			} else {
				DeviceInfo info;
				auto it = g_deviceInfo.find(self);
				if (it != g_deviceInfo.end()) {
					info = it->second;
				}
				active = JoystickStateActive(info, lpvData, cbData);
				if (active) {
					LogSuppressedJoystick(self, lpvData, cbData, nullptr);
				}
				NeutralizeJoystick(lpvData, cbData, info);
			}
		}

		static void SuppressGameJoystickBuffered(IDirectInputDevice8* self, DIDEVICEOBJECTDATA* rgdod, DWORD count) {
			std::lock_guard<std::mutex> lock(g_mutex);
			GameDeviceFormat& fmt = g_gameDeviceFormats[self];
			if (!fmt.valid && !fmt.buildAttempted) {
				return;
			}
			if (!fmt.valid) {
				return;
			}
			bool hadInput = false;
			for (DWORD i = 0; i < count; ++i) {
				bool isPov = false;
				for (DWORD ofs : fmt.povOfs) {
					if (rgdod[i].dwOfs == ofs) { isPov = true; break; }
				}
				if (rgdod[i].dwData != 0) {
					hadInput = true;
				}
				if (isPov) {
					rgdod[i].dwData = 0xFFFFFFFF;
					continue;
				}
				bool isAxis = false;
				for (const auto& axis : fmt.axisNeutrals) {
					if (rgdod[i].dwOfs == axis.first) {
						rgdod[i].dwData = static_cast<DWORD>(axis.second);
						isAxis = true;
						break;
					}
				}
				if (!isAxis) {
					rgdod[i].dwData = 0;
				}
			}
			if (hadInput) {
				LogSuppressedJoystick(self, nullptr, 0, &fmt);
			}
		}

		static void LogClassDeviceDiag(IDirectInputDevice8* self, DWORD cbData) {
			static std::unordered_set<void*> diagLogged;
			std::lock_guard<std::mutex> lock(g_mutex);
			if (diagLogged.find(self) != diagLogged.end()) {
				return;
			}
			diagLogged.insert(self);
			void** vtbl = *reinterpret_cast<void***>(self);
			GetDeviceInfo_t getDeviceInfo = reinterpret_cast<GetDeviceInfo_t>(vtbl[kSlotGetDeviceInfo]);
			DIDEVICEINSTANCE di{};
			di.dwSize = sizeof(DIDEVICEINSTANCE);
			if (getDeviceInfo && SUCCEEDED(getDeviceInfo(self, &di))) {
				std::string name;
				for (size_t i = 0; i < MAX_PATH - 1 && di.tszProductName[i] != L'\0'; ++i) {
					name.push_back(static_cast<char>(di.tszProductName[i]));
				}
				spdlog::info("DirectInputHook: game joystick reads intercepted on {:p} (product \"{}\", devtype {:04X}, cbData {})",
					static_cast<void*>(self), name, static_cast<unsigned>(di.dwDevType & 0xFFFF), cbData);
			} else {
				spdlog::info("DirectInputHook: game joystick reads intercepted on {:p} (devinfo unavailable, cbData {})",
					static_cast<void*>(self), cbData);
			}
			spdlog::default_logger()->flush();
		}

		static HRESULT STDMETHODCALLTYPE HookedClassGetDeviceState(IDirectInputDevice8* self, DWORD cbData, LPVOID lpvData) {
			void** selfVtbl = *reinterpret_cast<void***>(self);
			GetDeviceState_t orig = reinterpret_cast<GetDeviceState_t>(g_classGetDeviceStateOrigins[selfVtbl[kSlotGetDeviceState]]);
			HRESULT hr = orig ? orig(self, cbData, lpvData) : E_FAIL;
			if (g_classGetDeviceStateHooked.load(std::memory_order_acquire) &&
				SUCCEEDED(hr) && lpvData && cbData != 0 &&
				RawInput::IsGamepadBlockedToGame() && !IsSelfOpenedDevice(self)) {
				LogClassDeviceDiag(self, cbData);
				SuppressGameJoystickRead(self, lpvData, cbData);
			}
			return hr;
		}

		static HRESULT STDMETHODCALLTYPE HookedClassGetDeviceData(IDirectInputDevice8* self,
			DWORD cbObjectData, DIDEVICEOBJECTDATA* rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
			void** selfVtbl = *reinterpret_cast<void***>(self);
			GetDeviceData_t orig = reinterpret_cast<GetDeviceData_t>(g_classGetDeviceDataOrigins[selfVtbl[kSlotGetDeviceData]]);
			HRESULT hr = orig ? orig(self, cbObjectData, rgdod, pdwInOut, dwFlags) : E_FAIL;
			if (g_classGetDeviceDataHooked.load(std::memory_order_acquire) &&
				SUCCEEDED(hr) && pdwInOut && rgdod && *pdwInOut != 0 &&
				RawInput::IsGamepadBlockedToGame() && !IsSelfOpenedDevice(self)) {
				LogClassDeviceDiag(self, cbObjectData);
				SuppressGameJoystickBuffered(self, rgdod, *pdwInOut);
			}
			return hr;
		}

		typedef HRESULT(STDMETHODCALLTYPE* DiCreateDeviceGeneric_t)(void*, REFGUID, void**, LPUNKNOWN);
		typedef ULONG(STDMETHODCALLTYPE* DiReleaseGeneric_t)(void*);
		typedef HRESULT(STDMETHODCALLTYPE* DiEnumDevicesGeneric_t)(void*, DWORD, void*, void*, DWORD);

		struct ClassProbeContext {
			DiCreateDeviceGeneric_t createDevice;
			void* di8;
			void* device;
		};

		static bool ClassProbeEnumDevice(REFGUID guidInstance, void* pContext) {
			ClassProbeContext* ctx = reinterpret_cast<ClassProbeContext*>(pContext);
			void* device = nullptr;
			if (ctx->createDevice && SUCCEEDED(ctx->createDevice(ctx->di8, guidInstance, &device, nullptr)) && device) {
				ctx->device = device;
				return DIENUM_STOP;
			}
			return DIENUM_CONTINUE;
		}

		static BOOL CALLBACK ClassProbeEnumCallbackA(const DIDEVICEINSTANCEA* pdidInstance, void* pContext) {
			return pdidInstance ? ClassProbeEnumDevice(pdidInstance->guidInstance, pContext) : DIENUM_CONTINUE;
		}

		static BOOL CALLBACK ClassProbeEnumCallbackW(const DIDEVICEINSTANCEW* pdidInstance, void* pContext) {
			return pdidInstance ? ClassProbeEnumDevice(pdidInstance->guidInstance, pContext) : DIENUM_CONTINUE;
		}

		static bool HookClassFunction(void* target, void* detour, std::unordered_map<void*, void*>& origins, const char* label) {
			if (!target) {
				return false;
			}
			if (g_classHookTargets.find(target) != g_classHookTargets.end()) {
				return true;
			}
			void* trampoline = nullptr;
			if (MH_CreateHook(target, detour, &trampoline) != MH_OK) {
				spdlog::warn("DirectInputHook: class hook create failed for {} (MinHook trampoline unavailable)", label);
				return false;
			}
			if (MH_EnableHook(target) != MH_OK) {
				MH_RemoveHook(target);
				spdlog::warn("DirectInputHook: class hook enable failed for {}", label);
				return false;
			}
			origins[target] = trampoline;
			g_classHookTargets.insert(target);
			spdlog::info("DirectInputHook: class hook active for {} at {:p} (covers every DirectInput joystick device in the process)", label, target);
			spdlog::default_logger()->flush();
			return true;
		}

		static void InstallClassHooks() {
			static bool attempted = false;
			if (attempted || g_ownedDevices.empty()) {
				return;
			}
			attempted = true;
			std::vector<void*> stateTargets;
			std::vector<void*> dataTargets;
			void** ownVTable = *reinterpret_cast<void***>(g_ownedDevices.front().second);
			stateTargets.push_back(ownVTable[kSlotGetDeviceState]);
			dataTargets.push_back(ownVTable[kSlotGetDeviceData]);

			void* di8A = nullptr;
			if (g_origDirectInput8Create &&
				SUCCEEDED(g_origDirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, &di8A, nullptr)) && di8A) {
				void** aVtbl = *reinterpret_cast<void***>(di8A);
				DiCreateDeviceGeneric_t createDeviceA = reinterpret_cast<DiCreateDeviceGeneric_t>(aVtbl[kSlotCreateDevice]);
				DiEnumDevicesGeneric_t enumDevicesA = reinterpret_cast<DiEnumDevicesGeneric_t>(aVtbl[4]);
				DiReleaseGeneric_t releaseA = reinterpret_cast<DiReleaseGeneric_t>(aVtbl[kSlotRelease]);
				ClassProbeContext ctx{ createDeviceA, di8A, nullptr };
				enumDevicesA(di8A, DI8DEVCLASS_GAMECTRL, reinterpret_cast<void*>(&ClassProbeEnumCallbackA), &ctx, DIEDFL_ATTACHEDONLY);
				if (ctx.device) {
					void** deviceVtbl = *reinterpret_cast<void***>(ctx.device);
					stateTargets.push_back(deviceVtbl[kSlotGetDeviceState]);
					dataTargets.push_back(deviceVtbl[kSlotGetDeviceData]);
					void** probeVtbl = *reinterpret_cast<void***>(ctx.device);
					reinterpret_cast<DiReleaseGeneric_t>(probeVtbl[kSlotRelease])(ctx.device);
				}
				releaseA(di8A);
			} else {
				spdlog::warn("DirectInputHook: ANSI interface probe unavailable - class hooks cover the Unicode device class only");
			}

			bool anyState = false;
			bool anyData = false;
			for (void* target : stateTargets) {
				anyState = HookClassFunction(target, reinterpret_cast<void*>(&HookedClassGetDeviceState),
					g_classGetDeviceStateOrigins, "GetDeviceState(joystick)") || anyState;
			}
			for (void* target : dataTargets) {
				anyData = HookClassFunction(target, reinterpret_cast<void*>(&HookedClassGetDeviceData),
					g_classGetDeviceDataOrigins, "GetDeviceData(joystick)") || anyData;
			}
			g_classGetDeviceStateHooked.store(anyState, std::memory_order_release);
			g_classGetDeviceDataHooked.store(anyData, std::memory_order_release);
		}

		static void RemoveClassHooks() {
			g_classGetDeviceStateHooked.store(false, std::memory_order_release);
			g_classGetDeviceDataHooked.store(false, std::memory_order_release);
			for (void* target : g_classHookTargets) {
				MH_RemoveHook(target);
			}
			g_classHookTargets.clear();
			g_classGetDeviceStateOrigins.clear();
			g_classGetDeviceDataOrigins.clear();
		}

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
			size_t count = 32;
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
				if (high - low < 16) {
					return false;
				}
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
				if (entry.second.kind == DeviceKind::Joystick && entry.second.isPlaystation) {
					return false;
				}
			}
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

		static bool g_directInput8CreateHooked = false;
		static bool warnedInstallFailure = false;

		static bool InstallInternal(bool earlyPass) {
			if (g_directInput8CreateHooked) {
				return true;
			}
			HMODULE module = GetModuleHandleW(L"dinput8.dll");
			if (!module && !earlyPass) {
				module = LoadLibraryW(L"dinput8.dll");
			}
			if (!module) {
				return false;
			}

			void* target = reinterpret_cast<void*>(GetProcAddress(module, "DirectInput8Create"));
			if (!target) {
				if (!warnedInstallFailure) {
					warnedInstallFailure = true;
					spdlog::warn("DirectInputHook: DirectInput8Create export not found - suppression disabled");
				}
				return false;
			}

			MH_STATUS createStatus = MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hooked_DirectInput8Create),
				reinterpret_cast<LPVOID*>(&g_origDirectInput8Create));
			bool createUsable = (createStatus == MH_OK) ||
				(createStatus == MH_ERROR_ALREADY_CREATED && g_origDirectInput8Create != nullptr);
			if (!createUsable) {
				if (!warnedInstallFailure) {
					warnedInstallFailure = true;
					spdlog::warn("DirectInputHook: failed to hook DirectInput8Create (mh={}) - will retry", static_cast<int>(createStatus));
				}
				return false;
			}
			MH_STATUS enableStatus = MH_EnableHook(target);
			if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
				if (!warnedInstallFailure) {
					warnedInstallFailure = true;
					spdlog::warn("DirectInputHook: failed to enable DirectInput8Create hook (mh={}) - will retry", static_cast<int>(enableStatus));
				}
				return false;
			}

			g_directInput8CreateHooked = true;
			spdlog::info("DirectInputHook: hooked DirectInput8Create in the loaded dinput8.dll (chains through any proxy such as IHHook's)");
			spdlog::info("DirectInputHook: passive vtable wrapping {} (kill switch: mod/radarKeys/di_vtable_wrap_off.txt)",
				PassiveWrapEnabled() ? "ENABLED" : "DISABLED");
			spdlog::default_logger()->flush();
			return true;
		}

		void InstallEarly() {
			InstallInternal(true);
		}

		void Install() {
			InstallInternal(false);
		}

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
			int sonyPadCount = 0;
			for (UINT i = 0; i < got; ++i) {
				if (list[i].dwType != RIM_TYPEHID) {
					continue;
				}
				RID_DEVICE_INFO info{};
				UINT infoSize = static_cast<UINT>(sizeof(info));
				if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &infoSize) == static_cast<UINT>(-1)) {
					continue;
				}
				if (info.hid.dwVendorId != 0x054C) {
					continue;
				}
				if (info.hid.usUsagePage == 0x01 && (info.hid.usUsage == 0x04 || info.hid.usUsage == 0x05)) {
					++sonyPadCount;
				}
			}
			return sonyPadCount == 1;
		}

		bool IsSonyGamepadAttachedToSystem() {
			return SonyGamepadPresentInSystem();
		}

		struct OwnedRangeContext {
			IDirectInputDevice8* device;
			DeviceInfo* info;
		};

		static BOOL CALLBACK OwnedRangeCallback(const DIDEVICEOBJECTINSTANCEW* pdidoi, void* pContext) {
			OwnedRangeContext* ctx = reinterpret_cast<OwnedRangeContext*>(pContext);
			if (!pdidoi || (pdidoi->dwType & DIDFT_AXIS) == 0 || (pdidoi->dwType & DIDFT_NODATA) != 0) {
				return DIENUM_CONTINUE;
			}
			if ((pdidoi->dwOfs % 4) != 0) {
				return DIENUM_CONTINUE;
			}
			size_t idx = pdidoi->dwOfs / 4;
			if (idx >= 32) {
				return DIENUM_CONTINUE;
			}
			void** vtbl = *reinterpret_cast<void***>(ctx->device);
			GetProperty_t getProperty = reinterpret_cast<GetProperty_t>(vtbl[kSlotGetProperty]);
			if (!getProperty) {
				return DIENUM_CONTINUE;
			}
			DIPROPRANGE range{};
			range.diph.dwSize = sizeof(DIPROPRANGE);
			range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
			range.diph.dwObj = pdidoi->dwOfs;
			range.diph.dwHow = DIPH_BYOFFSET;
			if (SUCCEEDED(getProperty(ctx->device, DIPROP_RANGE, &range.diph)) && range.lMax > range.lMin) {
				ctx->info->perOffset[idx] = AxisRange{ true, range.lMin, range.lMax };
			}
			return DIENUM_CONTINUE;
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
			void** ownedVtbl = *reinterpret_cast<void***>(device);
			DiEnumObjects_t enumObjectsOwned = reinterpret_cast<DiEnumObjects_t>(ownedVtbl[kSlotEnumObjects]);
			if (enumObjectsOwned) {
				OwnedRangeContext rangeCtx{ device, &info };
				enumObjectsOwned(device, reinterpret_cast<void*>(&OwnedRangeCallback), &rangeCtx, DIDFT_AXIS);
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
			g_selfDevicePointers.insert(device);
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

			InstallClassHooks();

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
			RemoveClassHooks();
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
