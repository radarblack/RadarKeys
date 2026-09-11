#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace RadarKeys {

	using FoxLuaRegisterLibrary_t = void(__fastcall*)(lua_State* L, const char* libName, luaL_Reg* funcs);
	using lua_tolstring_t = const char* (__fastcall*)(lua_State* L, int idx, size_t* len);
	using lua_gettop_t = int(__fastcall*)(lua_State* L);
	using lua_pushnumber_t = void(__fastcall*)(lua_State* L, lua_Number n);
	using lua_pushstring_t = void(__fastcall*)(lua_State* L, const char* s);
	using lua_createtable_t = void(__fastcall*)(lua_State* L, int narr, int nrec);
	using lua_rawset_t = void(__fastcall*)(lua_State* L, int idx);
	using lua_pushnil_t = void(__fastcall*)(lua_State* L);
	using lua_getfield_t = void(__fastcall*)(lua_State* L, int idx, const char* k);
	using lua_pcall_t = int(__fastcall*)(lua_State* L, int nargs, int nresults, int errfunc);
	using lua_settop_t = void(__fastcall*)(lua_State* L, int idx);
	using lua_type_t = int(__fastcall*)(lua_State* L, int idx);

	extern FoxLuaRegisterLibrary_t g_FoxLuaRegisterLibrary;
	extern lua_tolstring_t g_lua_tolstring;
	extern lua_gettop_t g_lua_gettop;
	extern lua_pushnumber_t g_lua_pushnumber;
	extern lua_pushstring_t g_lua_pushstring;
	extern lua_createtable_t g_lua_createtable;
	extern lua_rawset_t g_lua_rawset;
	extern lua_pushnil_t g_lua_pushnil;
	extern lua_getfield_t g_lua_getfield;
	extern lua_pcall_t g_lua_pcall;
	extern lua_settop_t g_lua_settop;
	extern lua_type_t g_lua_type;

	bool ResolveLuaApi();
	void LuaApiCaptureState(lua_State* L);
	lua_State* LuaApiGetCapturedState();

	enum class LuaDirectCallResult {
		NotAvailable,
		NotFound,
		RuntimeError,
		Success
	};

	LuaDirectCallResult LuaCallGlobalFunction(const std::string& functionName);

	const char* LuaToString(lua_State* L, int idx);
	void LuaPushString(lua_State* L, const char* s);
	int LuaGetTop(lua_State* L);
	void LuaCreateTable(lua_State* L, int narr, int nrec);
	void LuaPushNil(lua_State* L);
	void LuaPushNumber(lua_State* L, double n);
	void LuaPushBool(lua_State* L, bool b);
	void LuaRawSetIndexed(lua_State* L, int tableAbsIdx, int n, const char* value);
	bool RegisterLuaLibrary(lua_State* L, const char* libName, luaL_Reg* funcs);

}
