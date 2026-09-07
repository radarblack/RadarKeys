#pragma once
#include <string>
#include <vector>

namespace RadarKeys {
	static std::vector<std::string> split(const std::string& str, const std::string& delim) {
		std::vector<std::string> tokens;
		if (delim.empty()) {
			tokens.push_back(str);
			return tokens;
		}

		size_t prev = 0;
		while (true) {
			size_t pos = str.find(delim, prev);
			if (pos == std::string::npos) {
				tokens.push_back(str.substr(prev));
				break;
			}
			tokens.push_back(str.substr(prev, pos - prev));
			prev = pos + delim.length();
		}
		return tokens;
	}

	// trim from left
	inline std::string& ltrim(std::string& s, const char* t = " \t\n\r\f\v") {
		s.erase(0, s.find_first_not_of(t));
		return s;
	}

	// trim from right
	inline std::string& rtrim(std::string& s, const char* t = " \t\n\r\f\v") {
		s.erase(s.find_last_not_of(t) + 1);
		return s;
	}

	// trim from left & right
	inline std::string& trim(std::string& s, const char* t = " \t\n\r\f\v") {
		return ltrim(rtrim(s, t), t);
	}
}
