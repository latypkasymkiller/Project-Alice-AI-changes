#pragma once

#include <string_view>
#include <string>
#include <fstream>
#include <mutex>
#include <cstdint>
#include <cstdlib>

#include "system_state.hpp"

namespace sys {
struct state;
}

namespace ai {

// Parsed from ALICE_AI_LOG once per process. "on"/"true"/"1" -> level 5; otherwise an integer 0..5.
// Zero (or unset) disables logging, and the AI_LOG macro then evaluates nothing.
inline int ai_log_max_level() {
	static bool opened = false;
	static int level = 0;
	if(!opened) {
		opened = true;
		char const* env = std::getenv("ALICE_AI_LOG");
		if(env && *env) {
			std::string s(env);
			if(s == "on" || s == "true" || s == "1") {
				level = 5;
			} else {
				try {
					level = std::stoi(s);
				} catch(...) {
					level = 0;
				}
			}
			if(level < 0)
				level = 0;
			if(level > 5)
				level = 5;
		}
	}
	return level;
}

inline bool ai_log_enabled(int level) {
	return level <= ai_log_max_level();
}

// Static locals inside an inline function are a single shared instance across all TUs, so this
// opens exactly one file handle and serializes every write behind one mutex.
inline void ai_log_message(sys::state const& state, int level, std::string_view category, std::string_view message) {
	static std::mutex mtx;
	static std::ofstream out;
	static bool opened = false;
	std::lock_guard<std::mutex> lock(mtx);
	if(!opened) {
		opened = true;
		out.open("alice_ai.log", std::ios::out | std::ios::app);
	}
	if(!out.is_open())
		return;
	auto d = state.current_date.to_ymd(state.start_date);
	out << d.year << '.' << int(d.month) << '.' << int(d.day) << " [" << level << "] " << category << ": " << message << "\n";
	out.flush();
}

#ifndef AI_LOG
#define AI_LOG(state, level, category, message)                               \
	do {                                                                     \
		if(::ai::ai_log_enabled(level)) {                                     \
			::ai::ai_log_message((state), (level), (category), (message));     \
		}                                                                     \
	} while(false)
#endif

} // namespace ai
