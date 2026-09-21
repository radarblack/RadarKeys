#include "LuaBridge.h"
#include "Util.h"
#include "spdlog/spdlog.h"

#include <map>

namespace RadarKeys {
	namespace LuaBridge {
		SafeQueue<std::string> messagesIn;
		SafeQueue<std::string> messagesOut;
		std::map<std::string, MenuCommandFunc> menuCommands;

		static const char* LOG_LUABRIDGE_DISPATCHMESSAGE_MALFORMED_MESSAGE_TOO_FEW = "LuaBridge::DispatchMessage: malformed message (too few fields): {}";
		static const char* LOG_LUABRIDGE_DISPATCHMESSAGE_NO_HANDLER_REGISTERED_CMD = "LuaBridge::DispatchMessage: no handler registered for cmd '{}'";
		static const char* LOG_LUABRIDGE_DISPATCHMESSAGE_HANDLER_FMT_THREW_FMT = "LuaBridge::DispatchMessage: handler for '{}' threw: {}";
		static const char* LOG_LUABRIDGE_DISPATCHMESSAGE_HANDLER_FMT_THREW_UNKNOWN = "LuaBridge::DispatchMessage: handler for '{}' threw an unknown exception";

		void AddMenuCommand(const std::string& cmd, MenuCommandFunc func) {
			menuCommands[cmd] = func;
		}

		void QueueMessageOut(std::string message) {
			messagesOut.push(message);
		}

		void QueueMessageIn(std::string message) {
			spdlog::trace("LuaBridge QueueMessageIn: " + message);
			messagesIn.push(message);
		}

		void DispatchMessage(const std::string& message) {
			std::vector<std::string> args = split(message, "|");
			if (args.size() < 2) {
				spdlog::warn(LOG_LUABRIDGE_DISPATCHMESSAGE_MALFORMED_MESSAGE_TOO_FEW, message);
				return;
			}
			std::string cmd = args[1];
			auto it = menuCommands.find(cmd);
			if (it == menuCommands.end()) {
				spdlog::warn(LOG_LUABRIDGE_DISPATCHMESSAGE_NO_HANDLER_REGISTERED_CMD, cmd);
				return;
			}
			MenuCommandFunc MenuCommand = it->second;
			try {
				MenuCommand(args);
			} catch (const std::exception& e) {
				spdlog::error(LOG_LUABRIDGE_DISPATCHMESSAGE_HANDLER_FMT_THREW_FMT, cmd, e.what());
			} catch (...) {
				spdlog::error(LOG_LUABRIDGE_DISPATCHMESSAGE_HANDLER_FMT_THREW_UNKNOWN, cmd);
			}
		}

		void ProcessMessages() {
			std::optional<std::string> messageOpt = messagesOut.pop();
			while (messageOpt) {
				DispatchMessage(*messageOpt);
				messageOpt = messagesOut.pop();
			}
		}
	}
}
