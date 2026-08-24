#pragma once
#include "SafeQueue.h"
#include <vector>
#include <string>

namespace RadarKeys {
	namespace LuaBridge {
		// capture-free function pointer forwarding parsed args to target handlers (e.g., DebuggerMenu).
		typedef void(*MenuCommandFunc) (std::vector<std::string> args);

		// KeyBindMenu/DebuggerMenu register handlers without this file knowing specifics (mirrors IHHook::AddMenuCommand)
		void AddMenuCommand(const std::string& cmd, MenuCommandFunc func);

		// called once per frame from the render thread
    // Drains messagesOut and dispatches each to its registered command handler.
		void ProcessMessages();

		// lua -> c++ direction, pushed by l_MenuMessage (see dllmain.cpp), drained by ProcessMessages
		void QueueMessageOut(std::string message);
		// c++ -> lua direction, pushed from KeyBindMenu/DebuggerMenu (the render/input thread),
		// drained by l_GetMenuMessages when the game's Lua side polls RadarKeys.GetMenuMessages()
		void QueueMessageIn(std::string message);

		extern SafeQueue<std::string> messagesOut;
		extern SafeQueue<std::string> messagesIn;
	}
}
