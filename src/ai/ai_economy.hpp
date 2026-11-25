#pragma once
#include "dcon_generated_ids.hpp"

namespace sys {
struct state;
}

namespace ai {

bool province_has_available_workers(sys::state& state, dcon::province_id p);
bool province_has_workers(sys::state& state, dcon::province_id p);

enum class war_economy_mode : uint8_t {
	peacetime = 0,
	prewar_ready = 1,
	minor_war = 2,
	major_war = 3
};

war_economy_mode evaluate_war_economy_mode(sys::state& state, dcon::nation_id nid);
void update_stockpile_targets_for_nation(sys::state& state, dcon::nation_id nid);
void update_budget_for_nation(sys::state& state, dcon::nation_id nid, bool presim = false);
void update_stockpile_targets(sys::state& state);
void update_budget(sys::state& state, bool presim = false);
void get_craved_factory_types(sys::state& state, dcon::nation_id nid, dcon::market_id mid, dcon::province_id, std::vector<dcon::factory_type_id>& desired_types, bool pop_project);
void get_desired_factory_types(sys::state& state, dcon::nation_id nid, dcon::market_id mid, dcon::province_id, std::vector<dcon::factory_type_id>& desired_types, bool pop_project);
void update_ai_econ_construction(sys::state& state);
void update_factory_types_priority(sys::state& state);
}
