#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "nations.hpp"
#include "system_state.hpp"
#include "text.hpp"

namespace ai {

/*
Level-gated debug logging for the military AI, meant for diagnosing "why does
nation X behave like that" sessions. Disabled by default: without ALICE_AI_LOG
set, the AI_LOG / AI_LOG_N macros evaluate to nothing and the game is untouched.

ALICE_AI_LOG       "on"/"true"/"1" -> level 5, otherwise an integer 0..5.
                   1 = scheduler passes, 2 = pass summaries, 3 = decisions and
                   vetoes, 4 = per-candidate detail, 5 = everything.
ALICE_AI_LOG_TAG   three-letter country tag (e.g. "AUS"); only that nation's
                   decision lines are written. Unset = all nations.

Output appends to alice_ai.log next to the project root (when PROJECT_ROOT is
defined) or the process working directory otherwise. A single mutex serializes
writes from the parallel_for AI passes.
*/

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
			level = std::clamp(level, 0, 5);
		}
	}
	return level;
}

inline bool ai_log_enabled(int level) {
	return level <= ai_log_max_level();
}

// The three-letter tag to log, or empty for all nations.
inline std::string const& ai_log_tag_filter() {
	static std::string filter = [] {
		char const* env = std::getenv("ALICE_AI_LOG_TAG");
		return (env && *env) ? std::string(env) : std::string();
	}();
	return filter;
}

// True when n passes the tag filter. The tag lives in
// national_identity::identifying_int (the name field holds the localized
// display name), so compare through nations::int_to_tag, the way save-file
// names are built in serialization.cpp.
inline bool ai_log_nation_enabled(sys::state const& state, dcon::nation_id n) {
	auto const& filter = ai_log_tag_filter();
	if(filter.empty())
		return true;
	if(!n)
		return false;
	auto ident = state.world.nation_get_identity_from_identity_holder(n);
	if(!ident)
		return false;
	return nations::int_to_tag(state.world.national_identity_get_identifying_int(ident)) == filter;
}

// "TAG (Display Name)" for log lines; "tag?" for anything unnamed.
inline std::string ai_log_nation_label(sys::state& state, dcon::nation_id n) {
	if(!n)
		return "none";
	auto ident = state.world.nation_get_identity_from_identity_holder(n);
	std::string tag = ident ? nations::int_to_tag(state.world.national_identity_get_identifying_int(ident)) : "tag?";
	return tag + " (" + text::get_name_as_string(state, n) + ")";
}

// "Name#index" for provinces; "#index" when unnamed.
inline std::string ai_log_prov_label(sys::state& state, dcon::province_id p) {
	if(!p)
		return "none";
	return text::get_name_as_string(state, p) + "#" + std::to_string(p.index());
}

inline void ai_log_message(sys::state const& state, int level, std::string_view category, std::string_view message) {
	static std::mutex mtx;
	static std::ofstream out;
	static bool opened = false;
	std::lock_guard<std::mutex> lock(mtx);
	if(!opened) {
		opened = true;
#ifdef PROJECT_ROOT
		out.open(PROJECT_ROOT "/alice_ai.log", std::ios::out | std::ios::app);
#else
		out.open("alice_ai.log", std::ios::out | std::ios::app);
#endif
	}
	if(!out.is_open())
		return;
	auto d = state.current_date.to_ymd(state.start_date);
	out << d.year << '.' << int(d.month) << '.' << int(d.day) << " [" << level << "] " << category << ": " << message << "\n";
	out.flush();
}

#ifndef AI_LOG
#define AI_LOG(state, level, category, message)                            \
	do {                                                                   \
		if(::ai::ai_log_enabled(level)) {                                   \
			::ai::ai_log_message((state), (level), (category), (message));  \
		}                                                                   \
	} while(false)
#endif

// Same, but the line is dropped unless `nation` matches ALICE_AI_LOG_TAG.
#ifndef AI_LOG_N
#define AI_LOG_N(state, level, category, nation, message)                          \
	do {                                                                           \
		if(::ai::ai_log_enabled(level) && ::ai::ai_log_nation_enabled((state), (nation))) { \
			::ai::ai_log_message((state), (level), (category), (message));          \
		}                                                                           \
	} while(false)
#endif

} // namespace ai
