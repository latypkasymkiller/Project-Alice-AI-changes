#pragma once

// ============================================================================
//  AI DEBUG INSTRUMENTATION — USA ARMY BEHAVIOUR LOGGER
// ============================================================================
//
//  This file is a *read-only* observer. It does NOT change any decision the AI
//  makes; it only prints, to the console, what the USA AI is doing with its
//  armies and why. The goal is to gather data about the reported behaviour
//  ("huge US army circling an encircled enemy province, shuffling troops to the
//  2nd and 3rd lines, merging stacks into one province and then pulling them
//  away") so a fix can be designed afterwards, with the console in hand.
//
//  Usage / toggles:
//    * Set AI_DEBUG_USA to 0 to compile every log call out entirely
//      (zero runtime cost, nothing is printed).
//    * Set AI_DEBUG_USA to 1 (default) to log USA army decisions to the
//      console (debugger output window / in-game log overlay).
//
//  Every log line is prefixed with:
//      [AI-DEBUG][USA]
//  (state::console_log adds its own date stamp) and is emitted ONLY for the
//  nation whose tag is "USA".
// ============================================================================

#include <cstdint>
#include <string>

#include "system_state.hpp"
#include "text.hpp"

#ifndef AI_DEBUG_USA
#define AI_DEBUG_USA 1
#endif

namespace ai::dbg {

// Compile-time on/off switch.
inline constexpr bool enabled() {
	return AI_DEBUG_USA != 0;
}

// True when `n` holds the USA tag. Uses the same tag extraction the rest of the
// AI already uses (national identity name == tag string).
inline bool is_usa(sys::state& state, dcon::nation_id n) {
	if(!n)
		return false;
	auto ident = state.world.nation_get_identity_from_identity_holder(n);
	if(!ident)
		return false;
	return text::produce_simple_string(state, state.world.national_identity_get_name(ident)) == "USA";
}

// "<ProvinceName>#<index>"; "<none>" when the province id is null.
inline std::string prov_str(sys::state& state, dcon::province_id p) {
	if(!p)
		return std::string("<none>");
	return text::get_name_as_string(state, p) + "#" + std::to_string(p.index());
}

// Human-readable army activity name.
inline const char* activity_name(uint8_t a) {
	switch(a) {
		case uint8_t(army_activity::unspecified): return "unspecified";
		case uint8_t(army_activity::on_guard): return "on_guard";
		case uint8_t(army_activity::attacking): return "attacking";
		case uint8_t(army_activity::merging): return "merging";
		case uint8_t(army_activity::transport_guard): return "transport_guard";
		case uint8_t(army_activity::transport_attack): return "transport_attack";
		case uint8_t(army_activity::attack_gathered): return "attack_gathered";
		case uint8_t(army_activity::attack_transport): return "attack_transport";
		default: return "?";
	}
}

// One-line description of an army: identity, location, activity, AI target and
// whether it is currently on the move. Does NOT call army_pressure_weight
// (callers that already have a weight append it to their message instead).
inline std::string army_str(sys::state& state, dcon::army_id a) {
	if(!a)
		return std::string("<none>");

	auto loc = state.world.army_get_location_from_army_location(a);
	auto tgt = state.world.army_get_ai_province(a);
	auto members = state.world.army_get_army_membership(a);
	int32_t regs = int32_t(members.end() - members.begin());

	std::string s;
	s.reserve(160);
	s += "army#";
	s += std::to_string(a.index());
	s += " loc=";
	s += prov_str(state, loc);
	s += " act=";
	s += activity_name(state.world.army_get_ai_activity(a));
	s += " tgt=";
	s += prov_str(state, tgt);
	s += " regs=";
	s += std::to_string(regs);
	s += " moving=";
	s += state.world.army_get_arrival_time(a) ? "1" : "0";
	return s;
}

// The single sink. Emits one prefixed line, and only for the USA tag.
// Routed through state::console_log so the output lands where the rest of the
// game's logging does: the debugger output window in a debug build, and the
// in-game console/log overlay otherwise. state::console_log is thread-safe
// (it enqueues onto a moodycamel concurrent queue), so it is safe to call from
// the parallel passes in make_defense.
inline void log(sys::state& state, dcon::nation_id n, std::string const& msg) {
#if AI_DEBUG_USA
	if(!is_usa(state, n))
		return;
	state.console_log("[AI-DEBUG][USA] " + msg);
#else
	(void)state;
	(void)n;
	(void)msg;
#endif
}

} // namespace ai::dbg
