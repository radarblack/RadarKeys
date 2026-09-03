#include "windowsapi.h"
#include "Render.h"
#include "LuaBridge.h"
#include "LuaApi.h"
#include "HookUtils.h"
#include "KeyBindMenu.h"
#include "DebuggerMenu.h"
#include "LuaKeyState.h"
#include <MinHook.h>

#include "spdlog/spdlog.h"

#include <thread>
#include <optional>
#include <string>
#include <cassert>

namespace RadarKeys {
	typedef BOOL(WINAPI* SetCursorPosFunc)(int, int);
	SetCursorPosFunc SetCursorPos_Orig = NULL;

	BOOL WINAPI SetCursorPos_Hook(int X, int Y) {
		if (Render::IsUnlockCursor()) {
			return FALSE;
		}
		return SetCursorPos_Orig(X, Y);
	}

	void InitCursorHook() {
		if (MH_CreateHook(&SetCursorPos, &SetCursorPos_Hook, reinterpret_cast<LPVOID*>(&SetCursorPos_Orig)) != MH_OK) {
			spdlog::error("InitCursorHook: MH_CreateHook failed for SetCursorPos");
			return;
		}
		if (MH_EnableHook(&SetCursorPos) != MH_OK) {
			spdlog::error("InitCursorHook: MH_EnableHook failed for SetCursorPos");
		}
	}

	// DLL_PROCESS_ATTACH runs under the loader lock - heavy initialization
	void InitThread() {
		spdlog::info("RadarKeys InitThread starting");

		if (MH_Initialize() != MH_OK) {
			spdlog::error("RadarKeys InitThread: MH_Initialize failed");
			return;
		}

		if (!ResolveLuaApi()) {
			spdlog::error("RadarKeys InitThread: ResolveLuaApi failed - Lua bindings will not work");
		}

		Render::CreateD3DHook();
		InitCursorHook();

		spdlog::info("RadarKeys frame initialized");
	}

	//--- Lua bindings ---
	static int l_MenuMessage(lua_State* L) {
		const char* cmd = LuaToString(L, 1);
		const char* message = LuaToString(L, 2);
		spdlog::trace("l_MenuMessage cmd:{},<> message:{}", cmd ? cmd : "", message ? message : "");
		LuaBridge::QueueMessageOut(message ? message : "");
		return 1;
	}

	static int l_GetMenuMessages(lua_State* L) {
		std::optional<std::string> messageOpt = LuaBridge::messagesIn.pop(); // waits if empty
		if (!messageOpt) {
			LuaPushNil(L); // no messages
			return 1;
		}
		LuaCreateTable(L, 0, 0);
		int tableAbsIdx = LuaGetTop(L); // captured once, stays valid regardless of later pushes

		int index = 0;
		while (messageOpt) {
			std::string message = *messageOpt;
			index++;
			LuaRawSetIndexed(L, tableAbsIdx, index, message.c_str());
			messageOpt = LuaBridge::messagesIn.pop();
		}
		assert(LuaGetTop(L) == 1); //  table still on stack
		return 1;
	}

	//--- Lua bindings: key polling (InfButton-shaped, but keyed by name instead of a bitmask) ---
	// Resolves arg 1 (a key name from the same table the keybind UI dropdown uses, e.g. "Numpad7")
	// to a vKey. Returns -1 (and logs) if the name isn't recognized, so callers below can just
	// treat "-1" as "nothing to query" and fall through to their not-found return.
	static int ResolveKeyNameArg(lua_State* L) {
		const char* name = LuaToString(L, 1);
		if (!name) {
			return -1;
		}
		int vKey = KeyBindMenu::VKeyForName(name);
		if (vKey < 0) {
			spdlog::warn("RadarKeys key query: unrecognized key name '{}'", name);
		}
		return vKey;
	}

	static int l_ButtonDown(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushBool(L, vKey >= 0 && LuaKeyState::ButtonDown((USHORT)vKey));
		return 1;
	}
	static int l_OnButtonDown(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushBool(L, vKey >= 0 && LuaKeyState::OnButtonDown((USHORT)vKey));
		return 1;
	}
	static int l_OnButtonUp(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushBool(L, vKey >= 0 && LuaKeyState::OnButtonUp((USHORT)vKey));
		return 1;
	}
	static int l_ButtonHeld(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushBool(L, vKey >= 0 && LuaKeyState::ButtonHeld((USHORT)vKey));
		return 1;
	}
	static int l_OnButtonHoldTime(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushBool(L, vKey >= 0 && LuaKeyState::OnButtonHoldTime((USHORT)vKey));
		return 1;
	}
	static int l_OnButtonRepeat(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushBool(L, vKey >= 0 && LuaKeyState::OnButtonRepeat((USHORT)vKey));
		return 1;
	}
	static int l_GetRepeatMult(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		LuaPushNumber(L, vKey >= 0 ? LuaKeyState::GetRepeatMult((USHORT)vKey) : 1.0);
		return 1;
	}
	static int l_ResetRepeat(lua_State* L) {
		int vKey = RadarKeys::ResolveKeyNameArg(L);
		if (vKey >= 0) {
			LuaKeyState::ResetRepeat((USHORT)vKey);
		}
		return 0;
	}

	// RadarKeys.DebugLog(message) - lets a Lua script push its own free-form line into the
	// RadarKeys debugger window (gated behind DebuggerMenu's "Log script debug messages"
	// checkbox, same as the DLL's own [BTN]/[BND]/[SCR] log lines).
	static int l_DebugLog(lua_State* L) {
		const char* message = LuaToString(L, 1);
		if (message) {
			DebuggerMenu::LogLuaDebug(message);
		}
		return 0;
	}
}

extern "C" __declspec(dllexport) int __cdecl luaopen_RadarKeys(lua_State* L) {
	spdlog::debug("luaopen_RadarKeys");

	luaL_Reg radarkeys_funcs[] = {
		{ "MenuMessage", RadarKeys::l_MenuMessage },
		{ "GetMenuMessages", RadarKeys::l_GetMenuMessages },
		{ "ButtonDown", RadarKeys::l_ButtonDown },
		{ "OnButtonDown", RadarKeys::l_OnButtonDown },
		{ "OnButtonUp", RadarKeys::l_OnButtonUp },
		{ "ButtonHeld", RadarKeys::l_ButtonHeld },
		{ "OnButtonHoldTime", RadarKeys::l_OnButtonHoldTime },
		{ "OnButtonRepeat", RadarKeys::l_OnButtonRepeat },
		{ "GetRepeatMult", RadarKeys::l_GetRepeatMult },
		{ "ResetRepeat", RadarKeys::l_ResetRepeat },
		{ "DebugLog", RadarKeys::l_DebugLog },
		{ NULL, NULL }
	};

	if (!RadarKeys::RegisterLuaLibrary(L, "RadarKeys", radarkeys_funcs)) {
		spdlog::error("luaopen_RadarKeys: RegisterLuaLibrary failed - Lua API addresses may not have resolved yet");
		return 0;
	}
	return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		std::thread(RadarKeys::InitThread).detach();
		break;
	case DLL_PROCESS_DETACH:
		RadarKeys::KeyBindMenu::LogCleanShutdown();
		break;
	}
	return TRUE;
}
