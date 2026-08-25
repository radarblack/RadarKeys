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

	constexpr uintptr_t ADDR_FoxLuaRegisterLibrary = 0x14006b8c0ull;
	constexpr uintptr_t ADDR_lua_gettop = 0x141A112E0ull;
	constexpr uintptr_t ADDR_lua_pushnumber = 0x141A11950ull;
	constexpr uintptr_t ADDR_lua_tolstring = 0x141A12150ull;
	constexpr uintptr_t ADDR_lua_pushstring = 0x141A11970ull;
	constexpr uintptr_t ADDR_lua_createtable = 0x141A10E80ull;
	constexpr uintptr_t ADDR_lua_rawset = 0x141A11B20ull;
	constexpr uintptr_t ADDR_lua_pushnil = 0x141A11930ull;

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
}
