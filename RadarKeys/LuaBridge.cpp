#include "LuaBridge.h"
#include "Util.h"
#include "spdlog/spdlog.h"

#include <map>

namespace RadarKeys {
	namespace LuaBridge {
		SafeQueue<std::string> messagesIn;
		SafeQueue<std::string> messagesOut;
		std::map<std::string, MenuCommandFunc> menuCommands;

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

		// IHHook-style IPC: pipe-separated "seq|cmd|payload..."
        // args[0]=seq (unused), args[1]=cmd (dispatch key), args[2+]=payload
		void DispatchMessage(const std::string& message) {
			std::vector<std::string> args = split(message, "|");
			if (args.size() < 2) {
				spdlog::warn("LuaBridge::DispatchMessage: malformed message (too few fields): {}", message);
				return;
			}
			std::string cmd = args[1];
			auto it = menuCommands.find(cmd);
			if (it == menuCommands.end()) {
				spdlog::warn("LuaBridge::DispatchMessage: no handler registered for cmd '{}'", cmd);
				return;
			}
			MenuCommandFunc MenuCommand = it->second;
			MenuCommand(args);
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
