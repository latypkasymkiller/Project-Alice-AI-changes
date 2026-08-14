#include "ai_log.hpp"

#include "system_state.hpp"
#include "text.hpp"
#include "nations.hpp"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace ai {

namespace {

// All logger state is private to this translation unit. The logger is strictly
// additive: it reads the game state through the const reference it is handed and
// writes a file. It never mutates sys::state, so instrumenting the AI with it
// cannot change AI behaviour.
struct ai_log_globals {
	std::mutex write_mutex;
	std::ofstream file;
	int max_level = 0;
	bool opened = false;
};

ai_log_globals& globals() {
	static ai_log_globals g;
	return g;
}

int parse_level(char const* value) {
	if (value == nullptr)
		return 0;
	std::string s(value);
	if (s == "on" || s == "yes" || s == "true")
		return 5;
	if (s.empty())
		return 0;
	// Tolerate trailing whitespace / surrounding quotes that a launcher might add.
	size_t start = 0;
	while (start < s.size() && (s[start] == ' ' || s[start] == '"' || s[start] == '\''))
		++start;
	size_t end = s.size();
	while (end > start && (s[end - 1] == ' ' || s[end - 1] == '"' || s[end - 1] == '\'' || s[end - 1] == '\n' || s[end - 1] == '\r'))
		--end;
	if (start >= end)
		return 0;
	std::string trimmed = s.substr(start, end - start);
	try {
		size_t pos = 0;
		int lvl = std::stoi(trimmed, &pos);
		if (pos == trimmed.size() && lvl > 0)
			return lvl > 5 ? 5 : lvl;
	} catch (...) {
		// fall through
	}
	if (trimmed == "on" || trimmed == "yes" || trimmed == "true")
		return 5;
	return 0;
}

void ensure_open() {
	ai_log_globals& g = globals();
	if (g.opened)
		return;
	g.opened = true;

	g.max_level = parse_level(std::getenv("ALICE_AI_LOG"));
	if (g.max_level <= 0)
		return;

	char const* path = std::getenv("ALICE_AI_LOG_FILE");
	char const* fname = (path && *path) ? path : "alice_ai.log";

	g.file.open(fname, std::ios::out | std::ios::app);
	if (g.file.is_open()) {
		g.file << "=== Alice AI log session started (max level " << g.max_level << ") ===" << std::endl;
		g.file.flush();
	}
}

std::string format_date(sys::state const& state) {
	auto const ymd = state.current_date.to_ymd(state.start_date);
	std::ostringstream oss;
	oss << ymd.year << '.';
	if (ymd.month < 10)
		oss << '0';
	oss << ymd.month << '.';
	if (ymd.day < 10)
		oss << '0';
	oss << ymd.day;
	return oss.str();
}

} // namespace

int ai_log_max_level() {
	ensure_open();
	return globals().max_level;
}

void ai_log_message(sys::state const& state, int level, std::string_view category, std::string_view message) {
	ensure_open();
	ai_log_globals& g = globals();
	if (level > g.max_level || !g.file.is_open())
		return;

	std::lock_guard<std::mutex> lock(g.write_mutex);

	g.file << '[' << format_date(state) << "] [" << level << "] [";
	g.file.write(category.data(), static_cast<std::streamsize>(category.size()));
	g.file << "] ";
	g.file.write(message.data(), static_cast<std::streamsize>(message.size()));
	g.file << std::endl;
}

} // namespace ai
