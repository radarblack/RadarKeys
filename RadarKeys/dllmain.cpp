#include "windowsapi.h"
#include "Render.h"
#include "LuaBridge.h"
#include "LuaApi.h"
#include "HookUtils.h"
#include "KeyBindMenu.h"
#include "DebuggerMenu.h"
#include <MinHook.h>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/basic_file_sink.h"

#include <thread>
#include <optional>
#include <string>
#include <cassert>
#include <filesystem>
#include <queue>
#include <fstream>

namespace RadarKeys {
	static bool is_initialization_active = true;
	class StartupFileSink : public spdlog::sinks::base_sink<std::mutex> {
	private:
		std::wstring file_path_;
	public:
		explicit StartupFileSink(std::wstring path) : file_path_(std::move(path)) {}
	protected:
		void sink_it_(const spdlog::details::log_msg& msg) override {
			if (!is_initialization_active) return;

			spdlog::memory_buf_t formatted;
			base_sink<std::mutex>::formatter_->format(msg, formatted);
			
			FILE* file_handle = nullptr;
			if (_wfopen_s(&file_handle, file_path_.c_str(), L"ab") == 0 && file_handle) {
				fwrite(formatted.data(), sizeof(char), formatted.size(), file_handle);
				fclose(file_handle);
			}
		}
		void flush_() override {}
	};

	class MemoryCappedSink : public spdlog::sinks::base_sink<std::mutex> {
	private:
		std::wstring file_path_;
		std::queue<std::string> log_lines_;
		size_t current_bytes_ = 0;
		size_t max_runtime_bytes_ = 8192; // fallback: set to 8KB 

	public:
		explicit MemoryCappedSink(std::wstring path) : file_path_(std::move(path)) {}

		void ActivateMemoryBufferTrack() {
			std::error_code ec;
			size_t startup_disk_bytes = std::filesystem::file_size(file_path_, ec);
			if (!ec && startup_disk_bytes < 10240) {
				max_runtime_bytes_ = 10240 - startup_disk_bytes; // tracks remainder
			}
		}

	protected:
		void sink_it_(const spdlog::details::log_msg& msg) override {
			if (is_initialization_active) return;

			spdlog::memory_buf_t formatted;
			base_sink<std::mutex>::formatter_->format(msg, formatted);
			std::string line_str(formatted.data(), formatted.size());

			log_lines_.push(line_str);
			current_bytes_ += line_str.size();

			while (current_bytes_ > max_runtime_bytes_ && !log_lines_.empty()) {
				current_bytes_ -= log_lines_.front().size();
				log_lines_.pop();
			}
		}

		void flush_() override {
			FILE* file_handle = nullptr;
			if (_wfopen_s(&file_handle, file_path_.c_str(), L"ab") != 0 || !file_handle) {
				return;
			}
			while (!log_lines_.empty()) {
				const std::string& line = log_lines_.front();
				fwrite(line.c_str(), sizeof(char), line.size(), file_handle);
				log_lines_.pop();
			}
			fflush(file_handle);
			fclose(file_handle);
		}
	};

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

	std::shared_ptr<spdlog::sinks::sink> startup_sink = nullptr;
	std::shared_ptr<MemoryCappedSink> sliding_sink = nullptr;

	void SetupLog() {
		std::filesystem::path logDir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
		std::error_code ec;
		std::filesystem::create_directories(logDir, ec);

		std::filesystem::path logPath = logDir / "radarkeys_log.txt";
		std::filesystem::path logPathPrev = logDir / "radarkeys_log_prev.txt";

		DeleteFileW(logPathPrev.c_str());
		CopyFileW(logPath.c_str(), logPathPrev.c_str(), FALSE);
		DeleteFileW(logPath.c_str());

		startup_sink = std::make_shared<StartupFileSink>(logPath.wstring());
		sliding_sink = std::make_shared<MemoryCappedSink>(logPath.wstring());

		auto logger = std::make_shared<spdlog::logger>("radarkeys", spdlog::sinks_init_list{ startup_sink, sliding_sink });
		spdlog::set_default_logger(logger);
		spdlog::set_level(spdlog::level::debug);
		
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

		spdlog::info("RadarKeys frame initialized");

		// remove the sinks to the disk
		if (auto logger = spdlog::get("radarkeys")) {
			logger->flush();
		}
			
		is_initialization_active = false;
		if (sliding_sink) {
			sliding_sink->ActivateMemoryBufferTrack();
		}
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
		DisableThreadLibraryCalls(hModule);
		std::thread(RadarKeys::InitThread).detach();
		break;
	case DLL_PROCESS_DETACH:
		// custom ring buffer. flushes out activities past the success init log
		if (auto logger = spdlog::get("radarkeys")) {
			logger->flush();
		}
		spdlog::shutdown();
		break;
	}
	return TRUE;
}
