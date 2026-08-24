#include "windowsapi.h"
#include "Render.h"
#include "LuaBridge.h"
#include "KeyBindMenu.h"
#include "DebuggerMenu.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <MinHook.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <thread>
#include <optional>
#include <string>

namespace RadarKeys {

	// Hook SetCursorPos like IHHook to swallow game's calls and keep cursor free.
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

	void SetupLog() {
		// simplified from IHHook's SetupLog - no config file in this build (no debugMode/openConsole toggles), just a straightforward always-on file logger.
		auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("radarkeys_log.txt", true);
		auto logger = std::make_shared<spdlog::logger>("radarkeys", fileSink);
		spdlog::set_default_logger(logger);
		spdlog::set_level(spdlog::level::debug);
		spdlog::flush_on(spdlog::level::warn);
		spdlog::info("RadarKeys log started");
	}

	// defer heavy DLL initialization to a background thread from DLL_PROCESS_ATTACH to avoid loader lock issues, following IHHook/V_Framework patterns.
	void InitThread() {
		SetupLog();
		spdlog::info("RadarKeys InitThread starting");

		if (MH_Initialize() != MH_OK) {
			spdlog::error("RadarKeys InitThread: MH_Initialize failed");
			return;
		}

		Render::CreateD3DHook();
		InitCursorHook();

		spdlog::info("RadarKeys InitThread done");
	}

	//--- Lua bindings ---
	static int l_MenuMessage(lua_State* L) {
		const char* cmd = lua_tostring(L, 1);
		const char* message = lua_tostring(L, 2);
		spdlog::trace("l_MenuMessage cmd:{},<> message:{}", cmd ? cmd : "", message ? message : "");
		LuaBridge::QueueMessageOut(message ? message : "");
		return 1;
	}

	static int l_GetMenuMessages(lua_State* L) {
		std::optional<std::string> messageOpt = LuaBridge::messagesIn.pop();//tex waits if empty
		if (!messageOpt) {
			lua_pushnil(L);
			return 1;
		}
		int index = 0;
		lua_createtable(L, 0, 0);
		while (messageOpt) {
			std::string message = *messageOpt;

			index++;
			lua_pushstring(L, message.c_str());
			lua_rawseti(L, -2, index);

			messageOpt = LuaBridge::messagesIn.pop();
		}
		assert(lua_gettop(L) == 1);
		return 1;
	}

}

// To export a Lua C module on Windows, use extern "C" __declspec(dllexport) and name the function luaopen_RadarKeys so Lua's require can load it.
extern "C" __declspec(dllexport) int __cdecl luaopen_RadarKeys(lua_State* L) {
	spdlog::debug("luaopen_RadarKeys");

	luaL_Reg radarkeys_funcs[] = {
		{ "MenuMessage", RadarKeys::l_MenuMessage },
		{ "GetMenuMessages", RadarKeys::l_GetMenuMessages },
		{ NULL, NULL }
	};
	luaI_openlib(L, "RadarKeys", radarkeys_funcs, 0);
	return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);// we don't need this so disabling it
		break;
	case DLL_PROCESS_DETACH:
		spdlog::shutdown();
		break;
	}
	return TRUE;
}
