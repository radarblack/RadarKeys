#include "LuaApi.h"
#include "HookUtils.h"
#include "spdlog/spdlog.h"

namespace RadarKeys {

	FoxLuaRegisterLibrary_t g_FoxLuaRegisterLibrary = nullptr;
	lua_tolstring_t g_lua_tolstring = nullptr;
	lua_gettop_t g_lua_gettop = nullptr;
	lua_pushnumber_t g_lua_pushnumber = nullptr;
	lua_pushstring_t g_lua_pushstring = nullptr;
	lua_createtable_t g_lua_createtable = nullptr;
	lua_rawset_t g_lua_rawset = nullptr;
	lua_pushnil_t g_lua_pushnil = nullptr;
	lua_getfield_t g_lua_getfield = nullptr;
	lua_pcall_t g_lua_pcall = nullptr;
	lua_settop_t g_lua_settop = nullptr;
	lua_type_t g_lua_type = nullptr;

	constexpr uintptr_t ADDR_FoxLuaRegisterLibrary = 0x14006b8c0ull;
	constexpr uintptr_t ADDR_lua_gettop = 0x141A112E0ull;
	constexpr uintptr_t ADDR_lua_pushnumber = 0x141A11950ull;
	constexpr uintptr_t ADDR_lua_tolstring = 0x141A12150ull;
	constexpr uintptr_t ADDR_lua_pushstring = 0x141A11970ull;
	constexpr uintptr_t ADDR_lua_createtable = 0x141A10E80ull;
	constexpr uintptr_t ADDR_lua_rawset = 0x141A11B20ull;
	constexpr uintptr_t ADDR_lua_pushnil = 0x141A11930ull;
	constexpr uintptr_t ADDR_lua_getfield = 0x141a111e0ull;
	constexpr uintptr_t ADDR_lua_pcall = 0x141a116c0ull;
	constexpr uintptr_t ADDR_lua_settop = 0x141a11f70ull;
	constexpr uintptr_t ADDR_lua_type = 0x141a12300ull;

	bool ResolveLuaApi() {
		if (!g_FoxLuaRegisterLibrary) {
			g_FoxLuaRegisterLibrary = reinterpret_cast<FoxLuaRegisterLibrary_t>(ResolveGameAddress(ADDR_FoxLuaRegisterLibrary));
		}
		if (!g_lua_gettop) {
			g_lua_gettop = reinterpret_cast<lua_gettop_t>(ResolveGameAddress(ADDR_lua_gettop));
		}
		if (!g_lua_pushnumber) {
			g_lua_pushnumber = reinterpret_cast<lua_pushnumber_t>(ResolveGameAddress(ADDR_lua_pushnumber));
		}
		if (!g_lua_tolstring) {
			g_lua_tolstring = reinterpret_cast<lua_tolstring_t>(ResolveGameAddress(ADDR_lua_tolstring));
		}
		if (!g_lua_pushstring) {
			g_lua_pushstring = reinterpret_cast<lua_pushstring_t>(ResolveGameAddress(ADDR_lua_pushstring));
		}
		if (!g_lua_createtable) {
			g_lua_createtable = reinterpret_cast<lua_createtable_t>(ResolveGameAddress(ADDR_lua_createtable));
		}
		if (!g_lua_rawset) {
			g_lua_rawset = reinterpret_cast<lua_rawset_t>(ResolveGameAddress(ADDR_lua_rawset));
		}
		if (!g_lua_pushnil) {
			g_lua_pushnil = reinterpret_cast<lua_pushnil_t>(ResolveGameAddress(ADDR_lua_pushnil));
		}

		bool ok = g_FoxLuaRegisterLibrary && g_lua_gettop && g_lua_pushnumber && g_lua_tolstring &&
			g_lua_pushstring && g_lua_createtable && g_lua_rawset && g_lua_pushnil;
		if (!ok) {
			spdlog::error("ResolveLuaApi: one or more Lua function addresses failed to resolve - game version may not match the day3900-en address table this build uses");
		}

		if (!g_lua_getfield) {
			g_lua_getfield = reinterpret_cast<lua_getfield_t>(ResolveGameAddress(ADDR_lua_getfield));
		}
		if (!g_lua_pcall) {
			g_lua_pcall = reinterpret_cast<lua_pcall_t>(ResolveGameAddress(ADDR_lua_pcall));
		}
		if (!g_lua_settop) {
			g_lua_settop = reinterpret_cast<lua_settop_t>(ResolveGameAddress(ADDR_lua_settop));
		}
		if (!g_lua_type) {
			g_lua_type = reinterpret_cast<lua_type_t>(ResolveGameAddress(ADDR_lua_type));
		}

		static bool loggedDirectCallAvailability = false;
		if (!loggedDirectCallAvailability) {
			loggedDirectCallAvailability = true;
			bool directCallReady = g_lua_getfield && g_lua_pcall && g_lua_settop && g_lua_type;
			if (directCallReady) {
				spdlog::info("ResolveLuaApi: direct (non-queued) Lua call path is available");
			} else {
				spdlog::info("ResolveLuaApi: direct Lua call path unavailable (ADDR_lua_getfield/pcall/settop/type not set) - manual key binds will use the queued path");
			}
		}

		return ok;
	}

	const char* LuaToString(lua_State* L, int idx) {
		if (!g_lua_tolstring) {
			return nullptr;
		}
		return g_lua_tolstring(L, idx, nullptr);
	}

	void LuaPushString(lua_State* L, const char* s) {
		if (!g_lua_pushstring) {
			return;
		}
		g_lua_pushstring(L, s);
	}
	int LuaGetTop(lua_State* L) {
		if (!g_lua_gettop) {
			return 0;
		}
		return g_lua_gettop(L);
	}

	void LuaCreateTable(lua_State* L, int narr, int nrec) {
		if (!g_lua_createtable) {
			return;
		}
		g_lua_createtable(L, narr, nrec);
	}

	void LuaPushNil(lua_State* L) {
		if (!g_lua_pushnil) {
			return;
		}
		g_lua_pushnil(L);
	}

	void LuaPushNumber(lua_State* L, double n) {
		if (!g_lua_pushnumber) {
			return;
		}
		g_lua_pushnumber(L, (lua_Number)n);
	}

	void LuaPushBool(lua_State* L, bool b) {
		if (b) {
			LuaPushNumber(L, 1);
		}
		else {
			LuaPushNil(L);
		}
	}

	void LuaRawSetIndexed(lua_State* L, int tableAbsIdx, int n, const char* value) {
		if (!g_lua_pushnumber || !g_lua_pushstring || !g_lua_rawset) {
			return;
		}
    
		g_lua_pushnumber(L, (lua_Number)n);
		g_lua_pushstring(L, value);
		g_lua_rawset(L, tableAbsIdx);
	}

	bool RegisterLuaLibrary(lua_State* L, const char* libName, luaL_Reg* funcs) {
		if (!ResolveLuaApi() || !L || !libName || !funcs) {
			return false;
		}
		g_FoxLuaRegisterLibrary(L, libName, funcs);
		spdlog::debug("RegisterLuaLibrary: registered {}", libName);
		return true;
	}

	static lua_State* g_CapturedLuaState = nullptr;

	void LuaApiCaptureState(lua_State* L) {
		if (!L) {
			return;
		}
		if (!g_CapturedLuaState) {
			spdlog::info("LuaApiCaptureState: captured main lua_State for direct calls");
		}
		g_CapturedLuaState = L;
	}

	lua_State* LuaApiGetCapturedState() {
		return g_CapturedLuaState;
	}

	LuaDirectCallResult LuaCallGlobalFunction(const std::string& functionName) {
		if (functionName.empty()) {
			return LuaDirectCallResult::NotFound;
		}

		lua_State* L = g_CapturedLuaState;
		if (!L || !g_lua_getfield || !g_lua_pcall || !g_lua_settop || !g_lua_type || !g_lua_gettop) {
			return LuaDirectCallResult::NotAvailable;
		}

		int top = g_lua_gettop(L);
		g_lua_getfield(L, LUA_GLOBALSINDEX, functionName.c_str());
		if (g_lua_type(L, -1) != LUA_TFUNCTION) {
			g_lua_settop(L, top);
			return LuaDirectCallResult::NotFound;
		}

		if (g_lua_pcall(L, 0, 0, 0) != 0) {
			const char* err = LuaToString(L, -1);
			spdlog::error("LuaCallGlobalFunction: '{}' raised an error: {}", functionName, err ? err : "<no message>");
			g_lua_settop(L, top);
			return LuaDirectCallResult::RuntimeError;
		}

		g_lua_settop(L, top);
		return LuaDirectCallResult::Success;
	}
}
