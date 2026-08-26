#include "windowsapi.h"
#include "Render.h"
#include "LuaBridge.h"
#include "LuaApi.h"
#include "HookUtils.h"
#include "KeyBindMenu.h"
#include "DebuggerMenu.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <MinHook.h>
#include <thread>
#include <optional>
#include <string>
#include <cassert>
#include <filesystem>

namespace RadarKeys {

	// matches IHHook's proven SetCursorPos hook pattern - intercepts the game's OWN calls to the real Win32 SetCursorPos, swallowing them while our menu wants the cursor free
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
		// simplified from IHHook. stays with the .conf file in mod/radarKeys folder
		std::filesystem::path logDir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
		std::error_code ec;
		std::filesystem::create_directories(logDir, ec);//tex: spdlog's file sink won't create missing directories itself

		std::filesystem::path logPath = logDir / "radarkeys_log.txt";
		auto logger = spdlog::basic_logger_mt("radarkeys", logPath.wstring());
		spdlog::set_default_logger(logger);
		spdlog::set_level(spdlog::level::debug);
		spdlog::flush_on(spdlog::level::warn);
		spdlog::info("RadarKeys log started");
	}

	// DLL_PROCESS_ATTACH runs under the loader lock - heavy initialization
	void InitThread() {
		SetupLog();
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

		spdlog::info("RadarKeys InitThread done");
	}

	static int l_MenuMessage(lua_State* L) {
		const char* cmd = LuaToString(L, 1);
		const char* message = LuaToString(L, 2);
		spdlog::trace("l_MenuMessage cmd:{},<> message:{}", cmd ? cmd : "", message ? message : "");
		LuaBridge::QueueMessageOut(message ? message : "");
		return 1;
	}

	static int l_GetMenuMessages(lua_State* L) {
		std::optional<std::string> messageOpt = LuaBridge::messagesIn.pop();//tex waits if empty
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
		assert(LuaGetTop(L) == 1); // table still on stack
		return 1;
	}

}

extern "C" __declspec(dllexport) int __cdecl luaopen_RadarKeys(lua_State* L) {
	spdlog::debug("luaopen_RadarKeys");

	luaL_Reg radarkeys_funcs[] = {
		{ "MenuMessage", RadarKeys::l_MenuMessage },
		{ "GetMenuMessages", RadarKeys::l_GetMenuMessages },
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
		DisableThreadLibraryCalls(hModule); // we don't need DLL_THREAD_ATTACH/DETACH notifications
		std::thread(RadarKeys::InitThread).detach();
		break;
	case DLL_PROCESS_DETACH:
		spdlog::shutdown();
		break;
	}
	return TRUE;
}
