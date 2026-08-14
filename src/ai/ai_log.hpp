#pragma once

#include <string_view>

// Forward declaration only -- the logger never needs the full definition of
// sys::state, it just threads it through so each line can be stamped with the
// in-game date.
namespace sys {
struct state;
}

namespace ai {

// Returns true when AI logging is active at the requested verbosity level.
//
// Activate by setting the ALICE_AI_LOG environment variable before launching the
// game:
//   ALICE_AI_LOG=1   -> log only the highest-level orchestration events
//   ALICE_AI_LOG=3   -> log decision-making detail (recommended for debugging AI)
//   ALICE_AI_LOG=5   -> log everything (very verbose)
//   ALICE_AI_LOG=on  -> same as 5
// When the variable is unset or "0", logging is completely disabled and none of
// the AI_LOG arguments are ever evaluated, so game behaviour is unchanged.
int ai_log_max_level();

inline bool ai_log_enabled(int level) {
	return level <= ai_log_max_level();
}

// Append a single log line (stamped with the in-game date) to the AI log file.
// `category` is a short string identifying the subsystem (e.g. "attack",
// "defense", "war_dec"). Callers should prefer the AI_LOG macro so the message
// expression is only built when logging is actually enabled.
void ai_log_message(sys::state const& state, int level, std::string_view category, std::string_view message);

} // namespace ai

// AI_LOG(state, level, category, message)
//
// `message` may be any expression convertible to std::string (e.g. a string
// concatenation). It is evaluated ONLY when ai_log_enabled(level) is true, so
// enabling logging has zero effect on control flow or on the values the AI code
// reads when logging is disabled.
//
// IMPORTANT: this macro only *observes* the game state -- it must never appear
// in a position that would change a variable's value or a branch's outcome.
#define AI_LOG(state, level, category, message)                                                       \
	do {                                                                                               \
		if (::ai::ai_log_enabled(level)) {                                                             \
			::ai::ai_log_message((state), (level), (category), (message));                            \
		}                                                                                              \
	} while (false)
