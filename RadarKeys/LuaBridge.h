#pragma once
#include "SafeQueue.h"
#include <vector>
#include <string>

namespace RadarKeys {
	namespace LuaBridge {
		typedef void(*MenuCommandFunc) (std::vector<std::string> args);
		void AddMenuCommand(const std::string& cmd, MenuCommandFunc func);
		void ProcessMessages();
		void QueueMessageOut(std::string message);
		void QueueMessageIn(std::string message);

		extern SafeQueue<std::string> messagesOut;
		extern SafeQueue<std::string> messagesIn;
	}
}
