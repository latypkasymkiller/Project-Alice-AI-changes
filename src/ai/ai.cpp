#include "ai.hpp"
#include "ai_types.hpp"
#include "ai_campaign_values.hpp"
#include "ai_economy.hpp"
#include "system_state.hpp"
#include "demographics.hpp"
#include "economy.hpp"
#include "economy_stats.hpp"
#include "economy_production.hpp"
#include "economy_government.hpp"
#include "construction.hpp"
#include "effects.hpp"
#include "gui_effect_tooltips.hpp"
#include "math_fns.hpp"
#include "military.hpp"
#include "logistics.hpp"
#include "politics.hpp"
#include "prng.hpp"
#include "province_templates.hpp"
#include "triggers.hpp"
#include "province.hpp"
#include "commands.hpp"
#include "nations_templates.hpp"
#include "battle_prediction.hpp"
#include <algorithm>
#include <queue>
#include <limits>

namespace ai {

void take_ai_decisions(sys::state& state) {
	using decision_nation_pair = std::pair<dcon::decision_id, dcon::nation_id>;
	concurrency::combinable<std::vector<decision_nation_pair, dcon::cache_aligned_allocator<decision_nation_pair>>> decisions_taken;

	// execute in staggered blocks
	uint32_t d_block_size = state.world.decision_size() / 32;
	uint32_t block_index = 0;
	auto d_block_end = state.world.decision_size();
	//uint32_t block_index = (state.current_date.value & 31);
	//auto d_block_end = block_index == 31 ? state.world.decision_size() : d_block_size * (block_index + 1);
	concurrency::parallel_for(d_block_size * block_index, d_block_end, [&](uint32_t i) {
		auto d = dcon::decision_id{ dcon::decision_id::value_base_t(i) };
		auto e = state.world.decision_get_effect(d);
		if(e) {
			auto potential = state.world.decision_get_potential(d);
			auto allow = state.world.decision_get_allow(d);
			auto ai_will_do = state.world.decision_get_ai_will_do(d);
			ve::execute_serial_fast<dcon::nation_id>(state.world.nation_size(), [&](auto ids) {
				// AI-only, not dead nations
				ve::mask_vector filter_a = !state.world.nation_get_is_player_controlled(ids) && nations::exists_or_is_utility_tag(state, ids);
				if(ve::compress_mask(filter_a).v != 0) {
					// empty allow assumed to be an "always = yes"
					ve::mask_vector filter_b = potential
						? filter_a && (trigger::evaluate(state, potential, trigger::to_generic(ids), trigger::to_generic(ids), 0))
						: filter_a;
					if(ve::compress_mask(filter_b).v != 0) {
						ve::mask_vector filter_c = allow
							? filter_b && (trigger::evaluate(state, allow, trigger::to_generic(ids), trigger::to_generic(ids), 0))
							: filter_b;
						if(ve::compress_mask(filter_c).v != 0) {
							ve::mask_vector filter_d = ai_will_do
								? filter_c && (trigger::evaluate_multiplicative_modifier(state, ai_will_do, trigger::to_generic(ids), trigger::to_generic(ids), 0) > 0.0f)
								: filter_c;
							ve::apply([&](dcon::nation_id n, bool passed_filter) {
								if(passed_filter) {
									decisions_taken.local().push_back(decision_nation_pair(d, n));
								}
							}, ids, filter_d);
						}
					}
				}
			});
		}
	});
	// combination and final execution
	auto total_vector = decisions_taken.combine([](auto& a, auto& b) {
		std::vector<decision_nation_pair, dcon::cache_aligned_allocator<decision_nation_pair>> result(a.begin(), a.end());
		result.insert(result.end(), b.begin(), b.end());
		return result;
	});
	// ensure total deterministic ordering
	std::sort(total_vector.begin(), total_vector.end(), [&](auto a, auto b) {
		auto na = a.second;
		auto nb = b.second;
		if(na != nb)
			return na.index() < nb.index();
		return a.first.index() < b.first.index();
	});
	// assumption 1: no duplicate pair of <n, d>
	for(const auto& v : total_vector) {
		auto n = v.second;
		auto d = v.first;
		auto e = state.world.decision_get_effect(d);
		if(command::can_take_decision(state, n, d)) {
			nations::take_decision(state, n, d);
		}
	}
}

float estimate_pop_party_support(sys::state& state, dcon::nation_id n, dcon::political_party_id pid) {
	auto iid = state.world.political_party_get_ideology(pid);
	/*float v = 0.f;
	for(const auto poid : state.world.nation_get_province_ownership_as_nation(n)) {
		for(auto plid : state.world.province_get_pop_location_as_province(poid.get_province())) {
			float weigth = plid.get_pop().get_size() * 0.001f;
			v += state.world.pop_get_demographics(plid.get_pop(), pop_demographics::to_key(state, iid)) * weigth;
		}
	}*/
	return state.world.nation_get_demographics(n, demographics::to_key(state, iid));
}

bool ai_can_appoint_political_party(sys::state& state, dcon::nation_id n) {
	if(!politics::can_appoint_ruling_party(state, n))
		return false;
	auto last_change = state.world.nation_get_ruling_party_last_appointed(n);
	if(last_change && state.current_date < last_change + 365)
		return false;
	if(politics::is_election_ongoing(state, n))
		return false;
	// Do not appoint if we are a democracy!
	if(politics::has_elections(state, n))
		return false;
	return true;
}

void update_ai_ruling_party(sys::state& state) {
	for(auto n : state.world.in_nation) {
		// skip over: non ais, dead nations
		if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
			continue;

		if(ai_can_appoint_political_party(state, n)) {
			auto gov = n.get_government_type();
			auto identity = n.get_identity_from_identity_holder();
			auto start = state.world.national_identity_get_political_party_first(identity).id.index();
			auto end = start + state.world.national_identity_get_political_party_count(identity);

			dcon::political_party_id target;
			float max_support = estimate_pop_party_support(state, n, state.world.nation_get_ruling_party(n));
			for(int32_t i = start; i < end; i++) {
				auto pid = dcon::political_party_id(uint16_t(i));
				if(pid != state.world.nation_get_ruling_party(n) && politics::political_party_is_active(state, n, pid) && (gov.get_ideologies_allowed() & ::culture::to_bits(state.world.political_party_get_ideology(pid))) != 0) {
					auto support = estimate_pop_party_support(state, n, pid);
					if(support > max_support) {
						target = pid;
						max_support = support;
					}
				}
			}

			assert(target != state.world.nation_get_ruling_party(n)); // Fires if some nation has no available parties
			if(target) {
				command::execute_appoint_ruling_party(state, n, target);
			}
		}
	}
}

void update_ai_colonial_investment(sys::state& state) {
	static std::vector<dcon::state_definition_id> investments;
	static std::vector<int32_t> free_points;

	investments.clear();
	investments.resize(uint32_t(state.defines.colonial_rank));

	free_points.clear();
	free_points.resize(uint32_t(state.defines.colonial_rank), -1);



	for(auto col : state.world.in_colonization) {
		auto n = col.get_colonizer();
		if(n.get_is_player_controlled() == false
			&& n.get_rank() <= uint16_t(state.defines.colonial_rank)
			&& !investments[n.get_rank() - 1]
			&& col.get_state().get_colonization_stage() <= uint8_t(2)
			// Perhaps wrong logic
			&& col.get_state() != state.world.state_instance_get_definition(state.crisis_state_instance)
			&& (!state.crisis_war || n.get_is_at_war() == false)
			 ) {

			if(state.crisis_attacker_wargoals.size() > 0) {
				auto first_wg = state.crisis_attacker_wargoals.at(0);
				if(first_wg.state == col.get_state()) {
					continue;
				}
			}

			auto crange = col.get_state().get_colonization();
			if(crange.end() - crange.begin() > 1) {
				if(col.get_last_investment() + int32_t(state.defines.colonization_days_between_investment) <= state.current_date) {

					if(free_points[n.get_rank() - 1] < 0) {
						free_points[n.get_rank() - 1] = nations::free_colonial_points(state, n);
					}

					int32_t cost = 0;;
					if(col.get_state().get_colonization_stage() == 1) {
						cost = int32_t(state.defines.colonization_interest_cost);
					} else if(col.get_level() <= 4) {
						cost = int32_t(state.defines.colonization_influence_cost);
					} else {
						cost =
							int32_t(state.defines.colonization_extra_guard_cost * (col.get_level() - 4) + state.defines.colonization_influence_cost);
					}
					if(free_points[n.get_rank() - 1] >= cost) {
						investments[n.get_rank() - 1] = col.get_state().id;
					}
				}
			}
		}
	}
	for(uint32_t i = 0; i < investments.size(); ++i) {
		if(investments[i])
			province::increase_colonial_investment(state, state.nations_by_rank[i], investments[i]);
	}
}
void update_ai_colony_starting(sys::state& state) {
	static std::vector<int32_t> free_points;
	free_points.clear();
	free_points.resize(uint32_t(state.defines.colonial_rank), -1);
	uint64_t seed = 0; // Seed for random shuffle below
	for(int32_t i = 0; i < int32_t(state.defines.colonial_rank); ++i) {
		if(state.world.nation_get_is_player_controlled(state.nations_by_rank[i])) {
			free_points[i] = 0;
		} else {
			if(military::get_role(state, state.crisis_war, state.nations_by_rank[i]) != military::war_role::none) {
				free_points[i] = 0;
			} else {
				free_points[i] = nations::free_colonial_points(state, state.nations_by_rank[i]);
				seed += (uint64_t) state.nations_by_rank[i].index();
			}
		}
	}
	// Randomize colonization target to avoid colonization along map patterns
	std::vector<dcon::state_definition_id> states;
	for(auto sd : state.world.in_state_definition) {
		states.push_back(sd);
	}
	// Fisher-Yates shuffle implementation
	const int size = static_cast<int>(states.size());
	for(int i = size - 1; i > 0; i--) {
		auto rnd = rng::get_random(state, static_cast<uint32_t>(seed));
		seed = rnd;
		int j = rnd % (i + 1);
		std::swap(states[i], states[j]);
	}

	// Iterate every colonizeable state and find colonizer
	for(auto sdid : states) {
		auto sd = dcon::fatten(state.world, sdid);

		if(sd.get_colonization_stage() > 1) {
			continue;
		}
		bool has_unowned_land = false;

		dcon::province_id coastal_target;
		for(auto p : state.world.state_definition_get_abstract_state_membership(sd)) {
			if(!p.get_province().get_nation_from_province_ownership()) {
				if(p.get_province().get_is_coast() && !coastal_target) {
					coastal_target = p.get_province();
				}
				if(p.get_province().id.index() < state.province_definitions.first_sea_province.index())
					has_unowned_land = true;
			}
		}
		if(!has_unowned_land) {
			continue;
		}
		for(int32_t i = 0; i < int32_t(state.defines.colonial_rank); ++i) {
			if(free_points[i] > 0) {
				bool adjacent = false;
				if(province::fast_can_start_colony(state, state.nations_by_rank[i], sd, free_points[i], coastal_target, adjacent)) {
					free_points[i] -= int32_t(state.defines.colonization_interest_cost_initial + (adjacent ? state.defines.colonization_interest_cost_neighbor_modifier : 0.0f));

					auto new_rel = fatten(state.world, state.world.force_create_colonization(sd, state.nations_by_rank[i]));
					new_rel.set_level(uint8_t(1));
					new_rel.set_last_investment(state.current_date);
					new_rel.set_points_invested(uint16_t(state.defines.colonization_interest_cost_initial + (adjacent ? state.defines.colonization_interest_cost_neighbor_modifier : 0.0f)));

					state.world.state_definition_set_colonization_stage(sd, uint8_t(1));
				}
			}
		}
	}
}

void upgrade_colonies(sys::state& state) {
	for(auto si : state.world.in_state_instance) {
		if(si.get_capital().get_is_colonial() && si.get_nation_from_state_ownership().get_is_player_controlled() == false) {
			if(province::can_integrate_colony(state, si)) {
				province::upgrade_colonial_state(state, si.get_nation_from_state_ownership(), si);
			}
		}
	}
}

void civilize(sys::state& state) {
	for(auto n : state.world.in_nation) {
		if(!n.get_is_player_controlled() && command::can_civilize_nation(state, n.id)) {
			command::execute_civilize_nation(state, n);
		}
	}
}

void take_reforms(sys::state& state) {
	for(auto n : state.world.in_nation) {
		if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
			continue;

		if(n.get_is_civilized()) { // political & social
			// Enact social policies to deter Jacobin rebels from overruning the country
			// Reactionaries will popup in effect but they are MORE weak that Jacobins
			dcon::issue_option_id iss;
			float max_support = 0.0f;

			for(auto m : state.world.nation_get_movement_within(n)) {
				if(m.get_movement().get_associated_issue_option() && m.get_movement().get_pop_support() > max_support) {
					iss = m.get_movement().get_associated_issue_option();
					max_support = m.get_movement().get_pop_support();
				}
			}
			if(!iss || !command::can_enact_issue(state, n, iss)) {
				max_support = 0.0f;
				iss = dcon::issue_option_id{};
				state.world.for_each_issue_option([&](dcon::issue_option_id io) {
					if(command::can_enact_issue(state, n, io)) {
						float support = 0.f;
						for(const auto poid : state.world.nation_get_province_ownership_as_nation(n)) {
							for(auto plid : state.world.province_get_pop_location_as_province(poid.get_province())) {
								float weigth = plid.get_pop().get_size() * 0.001f;
								support += pop_demographics::get_demo(state, plid.get_pop(), pop_demographics::to_key(state, io)) * weigth;
							}
						}
						if(support > max_support) {
							iss = io;
							max_support = support;
						}
					}
				});
			}
			if(iss) {
				nations::enact_issue(state, n, iss);
			}
		} else { // military and economic
			dcon::reform_option_id cheap_r;
			float cheap_cost = 0.0f;

			auto e_mul = politics::get_economic_reform_multiplier(state, n);
			auto m_mul = politics::get_military_reform_multiplier(state, n);

			for(auto r : state.world.in_reform_option) {
				bool is_military = state.world.reform_get_reform_type(state.world.reform_option_get_parent_reform(r)) == uint8_t(culture::issue_category::military);

				auto reform = state.world.reform_option_get_parent_reform(r);
				auto current = state.world.nation_get_reforms(n, reform.id).id;
				auto allow = state.world.reform_option_get_allow(r);

				if(r.id.index() > current.index() && (!state.world.reform_get_is_next_step_only(reform.id) || current.index() + 1 == r.id.index()) && (!allow || trigger::evaluate(state, allow, trigger::to_generic(n.id), trigger::to_generic(n.id), 0))) {

					float base_cost = float(state.world.reform_option_get_technology_cost(r));
					float reform_factor = is_military ? m_mul : e_mul;

					if(!cheap_r || base_cost * reform_factor < cheap_cost) {
						cheap_cost = base_cost * reform_factor;
						cheap_r = r.id;
					}
				}
			}

			if(cheap_r && cheap_cost <= n.get_research_points()) {
				nations::enact_reform(state, n, cheap_r);
			}
		}
	}
}

void remove_ai_data(sys::state& state, dcon::nation_id n) {
	for(auto ar : state.world.nation_get_army_control(n)) {
		ar.get_army().set_ai_activity(0);
		ar.get_army().set_ai_province(dcon::province_id{});
	}
	for(auto v : state.world.nation_get_navy_control(n)) {
		v.get_navy().set_ai_activity(0);
	}
}

bool unit_on_ai_control(const sys::state& state, dcon::army_id a) {
	if(!a || !state.world.army_is_valid(a)) {
		return false;
	}
	auto fat_id = dcon::fatten(state.world, a);
	auto nation = fat_id.get_controller_from_army_control();
	if(!nation) {
		return false;
	}
	if(nations::is_units_commanded_by_overlord(state, nation)) {
		return false;
	}
	return nation.get_is_player_controlled()
		? fat_id.get_is_ai_controlled()
		: true;
}
bool unit_on_ai_control(const sys::state& state, dcon::navy_id a) {
	if(!a || !state.world.navy_is_valid(a)) {
		return false;
	}
	auto fat_id = dcon::fatten(state.world, a);
	auto nation = fat_id.get_controller_from_navy_control();
	if(!nation) {
		return false;
	}
	if(nations::is_units_commanded_by_overlord(state, nation)) {
		return false;
	}
	return !nation.get_is_player_controlled();
}

bool will_upgrade_ships(sys::state& state, dcon::nation_id n) {
	auto fid = dcon::fatten(state.world, n);

	auto total = 0;
	auto unfull = 0;

	for(auto v : state.world.nation_get_navy_control(n)) {
		if(!v.get_navy().get_battle_from_navy_battle_participation()) {
			for(auto shp : v.get_navy().get_navy_membership()) {
				total++;
				if(shp.get_ship().get_strength() < 1.f)
					unfull++;

			}
		}
	}

	return unfull <= total * 0.1f;
}

void update_ships(sys::state& state) {
	static std::vector<dcon::navy_id> to_delete;
	to_delete.clear();

	for(auto n : state.world.in_nation) {
		if(n.get_is_player_controlled() || !will_upgrade_ships(state, n))
			continue;
		// Landlocked nation shouldn't keep fleet
		if(n.get_is_at_war() == false && nations::is_landlocked(state, n)) {
			for(auto v : n.get_navy_control()) {
				if(!v.get_navy().get_battle_from_navy_battle_participation() && unit_on_ai_control(state, v.get_navy())) {
					to_delete.push_back(v.get_navy());
				}
			}
		} else if(n.get_is_at_war() == false) {
			dcon::unit_type_id best_transport = military::get_best_transport(state, n);
			dcon::unit_type_id best_light = military::get_best_light_ship(state, n);
			dcon::unit_type_id best_big = military::get_best_big_ship(state, n);
			
			for(auto v : n.get_navy_control()) {


				for(auto shp : v.get_navy().get_navy_membership()) {
					auto type = shp.get_ship().get_type();

					// Upgrade ships, don't delete them
					if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::transport) {
						if(military::can_change_naval_unit_type<command::actor::ai>(state, n, shp.get_ship(), best_transport)) {
							military::upgrade_ship(state, shp.get_ship().id, best_transport);
						}
					} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::light_ship) {
						if(military::can_change_naval_unit_type<command::actor::ai>(state, n, shp.get_ship(), best_light)) {
							military::upgrade_ship(state, shp.get_ship().id, best_light);
						}
					} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::big_ship) {
						if(military::can_change_naval_unit_type<command::actor::ai>(state, n, shp.get_ship(), best_big)) {
							military::upgrade_ship(state, shp.get_ship().id, best_big);
						}
					}
				}
				
			}
		}
	}

	for(auto s : to_delete) {
		military::cleanup_navy(state, s);
	}
}

void build_ships(sys::state& state) {
	for(auto n : state.world.in_nation) {
		if(!n.get_is_player_controlled() && n.get_province_naval_construction().begin() == n.get_province_naval_construction().end()) {
			auto disarm = n.get_disarmed_until();
			if(disarm && state.current_date < disarm)
				continue;

			dcon::unit_type_id best_transport;
			dcon::unit_type_id best_light;
			dcon::unit_type_id best_big;

			for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
				dcon::unit_type_id j{ dcon::unit_type_id::value_base_t(i) };
				if(!n.get_active_unit(j) && !state.military_definitions.unit_base_definitions[j].active)
					continue;

				if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::transport) {
					if(!best_transport || state.military_definitions.unit_base_definitions[best_transport].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
						best_transport = j;
					}
				} else if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::light_ship) {
					if(!best_light || state.military_definitions.unit_base_definitions[best_light].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
						best_light = j;
					}
				} else if(state.military_definitions.unit_base_definitions[j].type == military::unit_type::big_ship) {
					if(!best_big || state.military_definitions.unit_base_definitions[best_big].defence_or_hull < state.military_definitions.unit_base_definitions[j].defence_or_hull) {
						best_big = j;
					}
				}
			}

			int32_t num_transports = 0;
			int32_t fleet_cap_in_transports = 0;
			int32_t fleet_cap_in_small = 0;
			int32_t fleet_cap_in_big = 0;

			for(auto v : n.get_navy_control()) {
				if(!unit_on_ai_control(state, v.get_navy())) {
					continue;
				}
				for(auto s : v.get_navy().get_navy_membership()) {
					auto type = s.get_ship().get_type();
					if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::transport) {
						++num_transports;
						fleet_cap_in_transports += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
					} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::big_ship) {
						fleet_cap_in_big += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
					} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::light_ship) {
						fleet_cap_in_small += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
					}
				}
			}

			static std::vector<dcon::province_id> owned_ports;
			owned_ports.clear();
			for(auto p : n.get_province_ownership()) {
				if(p.get_province().get_is_coast() && p.get_province().get_nation_from_province_control() == n) {
					owned_ports.push_back(p.get_province().id);
				}
			}
			auto cap = n.get_capital().id;
			std::sort(owned_ports.begin(), owned_ports.end(), [&](dcon::province_id a, dcon::province_id b) {
				auto a_dist = province::sorting_distance(state, a, cap);
				auto b_dist = province::sorting_distance(state, b, cap);
				if(a_dist != b_dist)
					return a_dist < b_dist;
				else
					return a.index() < b.index();
			});

			// Depending on the strategy, the AI will prioritize different fleet size
			auto target_naval_supply_points = calculate_desired_navy_size(state, n);

			int32_t constructing_fleet_cap = 0;
			if(best_transport) {
				if(fleet_cap_in_transports * 3 < target_naval_supply_points) {
					auto overseas_allowed = state.military_definitions.unit_base_definitions[best_transport].can_build_overseas;
					auto level_req = state.military_definitions.unit_base_definitions[best_transport].min_port_level;
					auto supply_pts = state.military_definitions.unit_base_definitions[best_transport].supply_consumption_score;

					for(uint32_t j = 0; j < owned_ports.size() && (fleet_cap_in_transports + constructing_fleet_cap) * 3 < target_naval_supply_points; ++j) {
						if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
							&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req) {
							assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_transport));
							command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_transport);
							constructing_fleet_cap += supply_pts;
						}
					}
				} else if(num_transports < 10) {
					auto overseas_allowed = state.military_definitions.unit_base_definitions[best_transport].can_build_overseas;
					auto level_req = state.military_definitions.unit_base_definitions[best_transport].min_port_level;
					auto supply_pts = state.military_definitions.unit_base_definitions[best_transport].supply_consumption_score;

					for(uint32_t j = 0; j < owned_ports.size() && num_transports < 10; ++j) {
						if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
							&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req) {
							assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_transport));
							command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_transport);
							++num_transports;
							constructing_fleet_cap += supply_pts;
						}
					}
				}
			}

			int32_t used_points = n.get_used_naval_supply_points();
			auto rem_free = target_naval_supply_points - (fleet_cap_in_transports + fleet_cap_in_small + fleet_cap_in_big + constructing_fleet_cap);
			fleet_cap_in_small = std::max(fleet_cap_in_small, 1);
			fleet_cap_in_big = std::max(fleet_cap_in_big, 1);

			auto free_big_points = best_light ? rem_free * fleet_cap_in_small / (fleet_cap_in_small + fleet_cap_in_big) : rem_free;
			auto free_small_points = best_big ? rem_free * fleet_cap_in_big / (fleet_cap_in_small + fleet_cap_in_big) : rem_free;

			if(best_light) {
				auto overseas_allowed = state.military_definitions.unit_base_definitions[best_light].can_build_overseas;
				auto level_req = state.military_definitions.unit_base_definitions[best_light].min_port_level;
				auto supply_pts = state.military_definitions.unit_base_definitions[best_light].supply_consumption_score;

				for(uint32_t j = 0; j < owned_ports.size() && supply_pts <= free_small_points; ++j) {
					if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
						&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req) {
						assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_light));
						command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_light);
						free_small_points -= supply_pts;
					}
				}
			}
			if(best_big) {
				auto overseas_allowed = state.military_definitions.unit_base_definitions[best_big].can_build_overseas;
				auto level_req = state.military_definitions.unit_base_definitions[best_big].min_port_level;
				auto supply_pts = state.military_definitions.unit_base_definitions[best_big].supply_consumption_score;

				for(uint32_t j = 0; j < owned_ports.size() && supply_pts <= free_big_points; ++j) {
					if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
						&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req) {
						assert(command::can_start_naval_unit_construction(state, n, owned_ports[j], best_big));
						command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_big);
						free_big_points -= supply_pts;
					}
				}
			}
		}
	}
}

dcon::province_id get_home_port(sys::state& state, dcon::nation_id n) {
	auto cap = state.world.nation_get_capital(n);
	int32_t max_level = -1;
	dcon::province_id result;
	float current_distance = 1.0f;
	for(auto p : state.world.nation_get_province_ownership(n)) {
		if(p.get_province().get_is_coast() && p.get_province().get_nation_from_province_control() == n) {
			if(p.get_province().get_building_level(uint8_t(economy::province_building_type::naval_base)) > max_level) {
				max_level = p.get_province().get_building_level(uint8_t(economy::province_building_type::naval_base));
				result = p.get_province();
				current_distance = province::sorting_distance(state, cap, p.get_province());
			} else if(result && p.get_province().get_building_level(uint8_t(economy::province_building_type::naval_base)) == max_level && province::sorting_distance(state, cap, p.get_province()) < current_distance) {
				current_distance = province::sorting_distance(state, cap, p.get_province());
				result = p.get_province();
			}
		}
	}
	return result;
}

void refresh_home_ports(sys::state& state) {
	for(auto n : state.world.in_nation) {
		if(!n.get_is_player_controlled() && n.get_owned_province_count() > 0) {
			n.set_ai_home_port(get_home_port(state, n));
		}
	}
}

void daily_cleanup(sys::state& state) {

}


bool navy_needs_repair(sys::state& state, dcon::navy_id n) {
	/*
	auto in_nation = fatten(state.world, state.world.navy_get_controller_from_navy_control(n));
	auto base_spending_level = in_nation.get_effective_naval_spending();
	float oversize_amount =
		in_nation.get_naval_supply_points() > 0
		? std::min(float(in_nation.get_used_naval_supply_points()) / float(in_nation.get_naval_supply_points()), 1.75f)
		: 1.75f;
	float over_size_penalty = oversize_amount > 1.0f ? 2.0f - oversize_amount : 1.0f;
	auto spending_level = base_spending_level * over_size_penalty;
	auto max_org = 0.25f + 0.75f * spending_level;
	*/

	for(auto shp : state.world.navy_get_navy_membership(n)) {
		if(shp.get_ship().get_strength() < 0.5f)
			return true;
		//if(shp.get_ship().get_org() < 0.75f * max_org)
		//	return true;
	}
	return false;
}

bool naval_advantage(sys::state& state, dcon::nation_id n) {
	for(auto par : state.world.nation_get_war_participant(n)) {
		for(auto other : par.get_war().get_war_participant()) {
			if(other.get_is_attacker() != par.get_is_attacker()) {
				if(other.get_nation().get_used_naval_supply_points() > state.world.nation_get_used_naval_supply_points(n))
					return false;
			}
		}
	}
	return true;
}



void send_fleet_home(sys::state& state, dcon::navy_id n, fleet_activity moving_status = fleet_activity::returning_to_base, fleet_activity at_base = fleet_activity::idle) {
	auto v = fatten(state.world, n);
	auto home_port = v.get_controller_from_navy_control().get_ai_home_port();
	if(v.get_location_from_navy_location() == home_port) {
		v.set_ai_activity(uint8_t(at_base));
	} else if(!home_port) {
		v.set_ai_activity(uint8_t(fleet_activity::unspecified));
	} else if(military::move_navy_ai(state, n, home_port)) {
		v.set_ai_activity(uint8_t(moving_status));
	} else {
		v.set_ai_activity(uint8_t(fleet_activity::unspecified));
	}
}

bool set_fleet_target(sys::state& state, dcon::nation_id n, dcon::province_id start, dcon::navy_id for_navy) {
	dcon::province_id result;
	float closest = 0.0f;
	for(auto par : state.world.nation_get_war_participant(n)) {
		for(auto other : par.get_war().get_war_participant()) {
			if(other.get_is_attacker() != par.get_is_attacker()) {
				for(auto nv : other.get_nation().get_navy_control()) {
					auto loc = nv.get_navy().get_location_from_navy_location();
					auto dist = province::sorting_distance(state, start, loc);
					if(!result || dist < closest) {
						if(loc.id.index() < state.province_definitions.first_sea_province.index()) {
							result = loc.get_port_to();
						} else {
							result = loc;
						}
						closest = dist;
					}
				}
			}
		}
	}

	if(result == start)
		return true;

	if(result) {
		auto valid_path = military::move_navy_ai<military::ai_path_length{ 4 }>(state, for_navy, result);
		if(valid_path) {
			state.world.navy_set_ai_activity(for_navy, uint8_t(fleet_activity::attacking));
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
}

void unload_units_from_transport(sys::state& state, dcon::navy_id n) {
	auto transported_armies = state.world.navy_get_army_transport(n);
	auto location = state.world.navy_get_location_from_navy_location(n);


	for(auto ar : transported_armies) {
		auto valid_path = military::move_army_ai(state, ar.get_army(), ar.get_army().get_ai_province(), ar.get_army().get_controller_from_army_control());
		if(valid_path) {
			auto activity = army_activity(ar.get_army().get_ai_activity());
			if(activity == army_activity::transport_guard) {
				ar.get_army().set_ai_activity(uint8_t(army_activity::on_guard));
			} else if(activity == army_activity::transport_attack) {
				ar.get_army().set_ai_activity(uint8_t(army_activity::attack_gathered));
			}
		}
	}

	state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::unloading));
}

bool merge_fleet(sys::state& state, dcon::navy_id n, dcon::province_id p, dcon::nation_id owner) {
	auto merge_target = [&]() {
		dcon::navy_id largest;
		int32_t largest_size = 0;
		for(auto on : state.world.province_get_navy_location(p)) {
			if(on.get_navy() != n && on.get_navy().get_controller_from_navy_control() == owner) {
				auto other_mem = on.get_navy().get_navy_membership();
				if(auto sz = int32_t(other_mem.end() - other_mem.begin()); sz > largest_size) {
					largest =  on.get_navy().id;
					largest_size = sz;
				}
			}
		}
		return largest;
	}();

	if(!merge_target) {
		return false;
	}
	military::merge_navies_impl(state, merge_target, n);
	return true;
}

void pickup_idle_ships(sys::state& state) {
	for(auto n : state.world.in_navy) {
		if(n.get_battle_from_navy_battle_participation())
			continue;
		if(n.get_arrival_time())
			continue;

		auto owner = n.get_controller_from_navy_control();

		if(!owner || owner.get_is_player_controlled() || owner.get_owned_province_count() == 0 || !unit_on_ai_control(state, n))
			continue;

		auto home_port = state.world.nation_get_ai_home_port(owner);
		if(!home_port)
			continue;

		auto location = state.world.navy_get_location_from_navy_location(n);
		auto activity = fleet_activity(state.world.navy_get_ai_activity(n));

		switch(activity) {
		case fleet_activity::unspecified:
			if(location == home_port) {
				if(!merge_fleet(state, n, location, owner))
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
			} else if(!home_port) {

			} else {
				// move to home port to merge
				send_fleet_home(state, n, fleet_activity::merging);
			}
			break;
		case fleet_activity::boarding:
		{
			bool all_loaded = true;
			for(auto ar : state.world.nation_get_army_control(owner)) {
				if(ar.get_army().get_ai_activity() == uint8_t(army_activity::transport_guard) || ar.get_army().get_ai_activity() == uint8_t(army_activity::transport_attack)) {
					if(ar.get_army().get_navy_from_army_transport() != n)
						all_loaded = false;
				}
			}

			if(all_loaded) {
				auto transporting_range = n.get_army_transport();
				if(transporting_range.begin() == transporting_range.end()) { // failed to pick up troops
					send_fleet_home(state, n);
				} else {
					auto transported_dest = (*(transporting_range.begin())).get_army().get_ai_province();
					if(!transported_dest) {
						send_fleet_home(state, n);
					} else if(transported_dest.get_is_coast()) { // move to closest port or closest off_shore

						auto target_prov = transported_dest.id;
						if(!province::has_naval_access_to_province(state, owner, target_prov)) {
							target_prov = state.world.province_get_port_to(target_prov);
						}
						auto valid_path = military::move_navy_ai(state, n, target_prov);

						if(valid_path) {
							n.set_ai_activity(uint8_t(fleet_activity::transporting));
						} else {
							military::stop_navy_movement(state, n);
							send_fleet_home(state, n);
						}
					} else if(auto path = province::make_path_to_nearest_coast(state, owner, transported_dest); path.empty()) {
						send_fleet_home(state, n);
					} else {
						auto target_prov = path.front();
						if(!province::has_naval_access_to_province(state, owner, target_prov)) {
							target_prov = state.world.province_get_port_to(target_prov);
						}
						auto path_valid = military::move_navy_ai(state, n, target_prov);
						if(path_valid) {
							n.set_ai_activity(uint8_t(fleet_activity::transporting));
						} else {
							military::stop_navy_movement(state, n);
							send_fleet_home(state, n);
						}
					}
				}
			}
		}
		break;
		case fleet_activity::transporting:
			unload_units_from_transport(state, n);
			break;
		case fleet_activity::failed_transport:
			if(location == home_port) {
				if(!merge_fleet(state, n, location, owner))
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
			} else if(home_port) {
				military::move_navy_ai(state, n, home_port);
			}
			break;
		case fleet_activity::returning_to_base:
			if(location == home_port) {
				if(!merge_fleet(state, n, location, owner))
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
			} else {
				send_fleet_home(state, n);
			}
			break;
		case fleet_activity::attacking:
			if(state.world.nation_get_is_at_war(owner) == false) {
				send_fleet_home(state, n);
			} else if(navy_needs_repair(state, n)) {
				send_fleet_home(state, n);
			} else {
				if(naval_advantage(state, owner) && set_fleet_target(state, owner, state.world.navy_get_location_from_navy_location(n), n)) {
					// do nothing -- target set successfully
				} else {
					send_fleet_home(state, n);
				}
			}
			break;
		case fleet_activity::merging:
			if(location == home_port) {
				if(!merge_fleet(state, n, location, owner))
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
			} else {
				send_fleet_home(state, n);
			}
			break;
		case fleet_activity::idle:
			if(location != home_port) {
				state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::unspecified));
			} else if(owner.get_is_at_war()) {
				if(!navy_needs_repair(state, n)) {
					bool valid_attacker = true;
					auto self_ships = state.world.navy_get_navy_membership(n);
					int32_t self_sz = int32_t(self_ships.end() - self_ships.begin());
					for(auto o : owner.get_navy_control()) {
						if(o.get_navy() != n) {
							if(o.get_navy().get_ai_activity() == uint8_t(fleet_activity::attacking)) {
								valid_attacker = false;
								break;
							}
							auto orange = o.get_navy().get_navy_membership();
							if(int32_t(orange.end() - orange.begin()) >= self_sz) {
								valid_attacker = false;
								break;
							}
						}
					}
					if(valid_attacker && naval_advantage(state, owner)) {
						set_fleet_target(state, owner, state.world.navy_get_location_from_navy_location(n), n);
					}
				}
			}
			break;
		case fleet_activity::unloading:
		{
			bool failed_transport = true;

			auto transporting = state.world.navy_get_army_transport(n);
			for(auto ar : transporting) {
				if(ar.get_army().get_path().size() != 0) {
					failed_transport = false;
				}
			}

			if(transporting.begin() == transporting.end()) {
				// all unloaded -> set to unspecified to send home later in this routine
				state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::unspecified));
			} else if(failed_transport) {
				// an army is stuck on the boats
				state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::failed_transport));
			} else {
				// do nothing, still unloading
			}
		}
		break;
		}
	}
}


enum class province_class : uint8_t {
	interior = 0,
	coast = 1,
	low_priority_border = 2,
	border = 3,
	threat_border = 4,
	hostile_border = 5,
	count = 6
};

struct classified_province {
	dcon::province_id id;
	province_class c;
};

namespace ai_front_helpers {

inline int32_t nation_base_cw(sys::state const& state, dcon::nation_id n) {
	if(!n) {
		return int32_t(state.defines.base_combat_width);
	}
	return std::clamp(
		int32_t(state.world.nation_get_modifier_values(n, sys::national_mod_offsets::combat_width) + state.defines.base_combat_width),
		int32_t(military::min_combat_width),
		int32_t(military::max_combat_width)
	);
}

inline int32_t prov_cw(sys::state const& state, dcon::nation_id n, dcon::province_id p, int32_t enemy_base_cw = 0) {
	int32_t base_cw = nation_base_cw(state, n);
	if(enemy_base_cw > 0) {
		base_cw = std::min(base_cw, enemy_base_cw);
	}
	float prov_mod = state.world.province_get_modifier_values(p, sys::provincial_mod_offsets::combat_width) + 1.0f;
	return std::clamp(int32_t(float(base_cw) * prov_mod), int32_t(military::min_combat_width), int32_t(military::max_combat_width));
}

inline bool army_has_backline_core(sys::state const& state, dcon::nation_id n, dcon::army_id a) {
	for(auto r : state.world.army_get_army_membership(a)) {
		auto uid = r.get_regiment().get_type();
		auto& bdef = state.military_definitions.unit_base_definitions[uid];
		if(bdef.type == military::unit_type::support)
			return true;
		if(bdef.type == military::unit_type::special) {
			auto stats = n ? state.world.nation_get_unit_stats(n, uid) : military::unit_definition{};
			float sup = (n && stats.discipline_or_evasion > 0.0f) ? stats.support : bdef.support;
			if(sup > 0.0f)
				return true;
		}
	}
	return false;
}

inline float province_stockpile_importance(sys::state const& state, dcon::province_id p) {
	auto si = state.world.province_get_state_membership(p);
	if(!si || state.world.state_instance_get_capital(si) != p)
		return 0.0f;
	auto m = state.world.state_instance_get_market_from_local_market(si);
	if(!m)
		return 0.0f;
	float total_stock = 0.0f;
	state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
		auto base_com = economy::unit_commodity_get_base_commodity(state, com_id);
		total_stock += state.world.market_get_government_stockpile(m, base_com);
	});
	// Scale into a bounded priority bonus (0..2.5)
	return std::min(2.5f, total_stock * 0.002f);
}

inline bool is_province_near_hostiles(sys::state const& state, dcon::nation_id n, dcon::province_id p, int32_t max_hops) {
	if(!p || province::is_sea(state, p))
		return false;
	auto has_hostile_at = [&](dcon::province_id q) -> bool {
		auto ctrl = state.world.province_get_nation_from_province_control(q);
		if(ctrl && military::are_at_war(state, n, ctrl))
			return true;
		if(!ctrl && state.world.province_get_rebel_faction_from_province_rebel_control(q))
			return true;
		for(auto ar : state.world.province_get_army_location(q)) {
			auto ac = ar.get_army().get_controller_from_army_control();
			if(!ac || military::are_at_war(state, n, ac))
				return true;
		}
		return false;
	};
	if(has_hostile_at(p))
		return true;
	if(max_hops <= 0)
		return false;
	for(auto padj : state.world.province_get_province_adjacency(p)) {
		if(province::is_adjacency_impassable(state, n, padj.id))
			continue;
		auto o1 = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
		if(province::is_sea(state, o1))
			continue;
		if(has_hostile_at(o1))
			return true;
		if(max_hops >= 2) {
			for(auto padj2 : state.world.province_get_province_adjacency(o1)) {
				if(province::is_adjacency_impassable(state, n, padj2.id))
					continue;
				auto o2 = padj2.get_connected_provinces(0) == o1 ? padj2.get_connected_provinces(1) : padj2.get_connected_provinces(0);
				if(!province::is_sea(state, o2) && has_hostile_at(o2))
					return true;
			}
		}
	}
	return false;
}

inline void collect_war_sides(
	sys::state const& state,
	dcon::nation_id n,
	std::vector<dcon::nation_id>& allies_out,
	std::vector<dcon::nation_id>& enemies_out
) {
	allies_out.clear();
	enemies_out.clear();
	allies_out.push_back(n);
	for(auto wp : state.world.nation_get_war_participant(n)) {
		bool we_attack = wp.get_is_attacker();
		for(auto par : wp.get_war().get_war_participant()) {
			auto other = par.get_nation().id;
			if(par.get_is_attacker() == we_attack) {
				if(std::find(allies_out.begin(), allies_out.end(), other) == allies_out.end()) {
					allies_out.push_back(other);
				}
			} else {
				if(std::find(enemies_out.begin(), enemies_out.end(), other) == enemies_out.end()) {
					enemies_out.push_back(other);
				}
			}
		}
	}
}

enum class war_stance : uint8_t {
	defensive = 0,
	cautious_offense = 1,
	active_offense = 2
};

inline bool is_time_working_against_us(sys::state& state, dcon::nation_id n) {
	for(auto wp : state.world.nation_get_war_participant(n)) {
		auto war = wp.get_war();
		bool we_attack = wp.get_is_attacker();
		for(auto wg_att : war.get_wargoals_attached()) {
			auto wg = wg_att.get_wargoal();
			bool wg_ours = (military::is_attacker(state, war, wg.get_added_by()) == we_attack);
			auto bits = wg.get_type().get_type_bits();
			bool territorial_wg = (bits & (military::cb_flag::po_demand_state
				| military::cb_flag::po_annex
				| military::cb_flag::po_transfer_provinces
				| military::cb_flag::po_make_puppet
				| military::cb_flag::po_make_substate)) != 0;

			if(wg_ours) {
				// Our side added a wargoal (as formal attacker OR as defender adding e.g. Yucatan): if not yet ticking positive, time is against us!
				if(wg.get_ticking_war_score() <= 0.5f) {
					if(territorial_wg || wg.get_type().get_tws_battle_factor() > 0) {
						return true;
					}
				}
			} else {
				// Enemy wargoal: if enemy already holds it (ticking positive for them, e.g. Japan took Philippines), time is against us!
				if(wg.get_ticking_war_score() > 0.5f) {
					return true;
				}
				if(auto sd = wg.get_associated_state(); sd) {
					int32_t total_p = 0;
					int32_t enemy_held_p = 0;
					for(auto m : sd.get_abstract_state_membership()) {
						auto p = m.get_province();
						if(p.get_nation_from_province_ownership() == wg.get_target_nation()) {
							++total_p;
							auto ctrl = p.get_nation_from_province_control();
							if(ctrl && military::are_at_war(state, n, ctrl)) {
								++enemy_held_p;
							}
						}
					}
					if(total_p > 0 && enemy_held_p * 2 >= total_p) {
						return true;
					}
				} else if((bits & (military::cb_flag::po_make_puppet | military::cb_flag::po_make_substate)) != 0) {
					if(auto tn = wg.get_target_nation(); tn) {
						auto tcap = tn.get_capital();
						if(tcap) {
							auto ctrl = tcap.get_nation_from_province_control();
							if(ctrl && military::are_at_war(state, n, ctrl)) {
								return true;
							}
						}
					}
				}
			}
		}
	}
	return false;
}

inline bool province_has_enemy_army(sys::state const& state, dcon::nation_id n, dcon::province_id p) {
	if(!p || province::is_sea(state, p))
		return false;
	bool at_war = state.world.nation_get_is_at_war(n);
	for(auto ar : state.world.province_get_army_location(p)) {
		auto army = ar.get_army();
		if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
			continue;
		auto ctrl = army.get_controller_from_army_control();
		if(!ctrl) {
			return true;
		}
		if(at_war && military::are_at_war(state, n, ctrl)) {
			return true;
		}
	}
	return false;
}

inline float adjacent_enemy_regs_excluding(sys::state const& state, dcon::nation_id n, dcon::province_id p, dcon::province_id excluded_prov) {
	if(!p || province::is_sea(state, p))
		return 0.0f;
	bool at_war = state.world.nation_get_is_at_war(n);
	float total = 0.0f;
	for(auto padj : state.world.province_get_province_adjacency(p)) {
		if(province::is_adjacency_impassable(state, n, padj.id))
			continue;
		dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
		if(other == excluded_prov || province::is_sea(state, other))
			continue;
		for(auto ea : state.world.province_get_army_location(other)) {
			auto army = ea.get_army();
			if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			auto ec = army.get_controller_from_army_control();
			if(!ec || (at_war && military::are_at_war(state, n, ec))) {
				for(auto r : army.get_army_membership()) {
					total += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
				}
			}
		}
	}
	return total;
}

struct walkable_theater_map {
	int32_t land_count = 0;
	std::vector<uint16_t> theater_id;
	std::vector<uint8_t> has_safe_port;

	void build(sys::state& state, dcon::nation_id n, dcon::province_id extra_seed = dcon::province_id{}) {
		uint32_t const pcount = state.world.province_size();
		uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
		land_count = int32_t(std::min(pcount, first_sea));
		theater_id.assign(size_t(std::max(0, land_count)), 0);
		has_safe_port.assign(1, 0);
		if(land_count <= 0 || !n)
			return;

		// Deterministic seed list: our armies, capital, controlled provinces, and optional battle/target seed
		std::vector<dcon::province_id> seeds;
		for(auto ar : state.world.nation_get_army_control(n)) {
			auto loc = ar.get_army().get_location_from_army_location().id;
			if(loc && loc.index() < land_count) {
				seeds.push_back(loc);
			}
		}
		if(auto cap = state.world.nation_get_capital(n); cap && cap.index() < land_count) {
			seeds.push_back(cap);
		}
		if(extra_seed && extra_seed.index() < land_count) {
			seeds.push_back(extra_seed);
		}
		for(auto pc : state.world.nation_get_province_control(n)) {
			auto p = pc.get_province().id;
			if(p && p.index() < land_count) {
				seeds.push_back(p);
			}
		}
		std::sort(seeds.begin(), seeds.end(), [](dcon::province_id a, dcon::province_id b) {
			return a.index() < b.index();
		});
		seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

		std::vector<dcon::province_id> queue;
		queue.reserve(256);
		uint16_t next_tid = 0;

		for(auto seed : seeds) {
			if(theater_id[seed.index()] != 0)
				continue;
			if(next_tid == uint16_t(0xFFFF))
				break;
			++next_tid;
			has_safe_port.push_back(0);

			theater_id[seed.index()] = next_tid;
			queue.clear();
			queue.push_back(seed);

			for(size_t qi = 0; qi < queue.size(); ++qi) {
				auto u = queue[qi];
				if(state.world.province_get_is_coast(u) && province::has_safe_access_to_province(state, n, u)) {
					has_safe_port[next_tid] = 1;
				}
				for(auto padj : state.world.province_get_province_adjacency(u)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id v = padj.get_connected_provinces(0) == u ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(!v || v.index() >= land_count || theater_id[v.index()] != 0)
						continue;
					// Walk across all owned passable land where n has military access (own, allies, vassals, occupied, war enemies),
					// without leaking across thousands of km of uncolonized wasteland!
					if(!state.world.province_get_nation_from_province_ownership(v))
						continue;
					if(!province::has_access_to_province(state, n, v))
						continue;
					theater_id[v.index()] = next_tid;
					queue.push_back(v);
				}
			}
		}
	}

	uint16_t theater_of(dcon::province_id p) const {
		if(!p || p.index() >= land_count)
			return 0;
		return theater_id[p.index()];
	}

	bool theater_has_safe_port(uint16_t tid) const {
		return tid > 0 && size_t(tid) < has_safe_port.size() && has_safe_port[tid] != 0;
	}
};

// Estimate enemy land reaction force able to march to a coastal naval landing beach (e.g. USA's 60K in New York reacting to a landing in Washington!)
inline float estimate_naval_landing_threat(sys::state& state, dcon::nation_id n, dcon::province_id target_p) {
	if(!target_p || province::is_sea(state, target_p))
		return 0.0f;
	uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
	int32_t const land_count = int32_t(std::min(state.world.province_size(), first_sea));
	if(target_p.index() >= land_count)
		return 0.0f;

	auto target_ctrl = state.world.province_get_nation_from_province_control(target_p);
	auto target_owner = state.world.province_get_nation_from_province_ownership(target_p);
	dcon::nation_id def_nat = target_ctrl ? target_ctrl : target_owner;

	// Bounded BFS across enemy-accessible owned land up to 800 km from target_p
	std::vector<uint8_t> visited(size_t(land_count), 0);
	std::vector<dcon::province_id> queue;
	queue.reserve(128);
	visited[target_p.index()] = 1;
	queue.push_back(target_p);

	float reaction_regs = 0.0f;
	for(size_t qi = 0; qi < queue.size(); ++qi) {
		auto u = queue[qi];
		if(u != target_p) {
			float d_km = province::direct_distance_km(state, target_p, u);
			float w = (d_km <= 450.0f) ? 1.00f : 0.60f;
			for(auto ea : state.world.province_get_army_location(u)) {
				auto army = ea.get_army();
				if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation())
					continue;
				auto ec = army.get_controller_from_army_control();
				if(!ec || military::are_at_war(state, n, ec)) {
					float eff = 0.0f;
					for(auto r : army.get_army_membership()) {
						eff += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
					}
					reaction_regs += eff * w;
				}
			}
		}

		for(auto padj : state.world.province_get_province_adjacency(u)) {
			if(def_nat && province::is_adjacency_impassable(state, def_nat, padj.id))
				continue;
			dcon::province_id v = padj.get_connected_provinces(0) == u ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
			if(!v || v.index() >= land_count || visited[v.index()])
				continue;
			if(!state.world.province_get_nation_from_province_ownership(v))
				continue;
			if(province::direct_distance_km(state, target_p, v) > 800.0f)
				continue;
			if(def_nat && !province::has_access_to_province(state, def_nat, v))
				continue;
			visited[v.index()] = 1;
			queue.push_back(v);
		}
	}

	// If landing directly onto an enemy's homeland capital landmass, also account for unengaged homeland armies beyond 800 km (capped)
	if(def_nat && military::are_at_war(state, n, def_nat)) {
		auto def_cap = state.world.nation_get_capital(def_nat);
		if(def_cap && state.world.province_get_connected_region_id(def_cap) == state.world.province_get_connected_region_id(target_p)) {
			uint16_t cap_crid = state.world.province_get_connected_region_id(def_cap);
			float far_homeland_regs = 0.0f;
			for(auto ar : state.world.nation_get_army_control(def_nat)) {
				auto army = ar.get_army();
				if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation())
					continue;
				auto aloc = army.get_location_from_army_location().id;
				if(!aloc || aloc.index() >= land_count || visited[aloc.index()])
					continue;
				if(state.world.province_get_connected_region_id(aloc) == cap_crid) {
					for(auto r : army.get_army_membership()) {
						far_homeland_regs += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
					}
				}
			}
			reaction_regs += std::min(20.0f, far_homeland_regs * 0.30f);
		}
	}

	return reaction_regs;
}

struct theater_summary {
	int32_t front_prov_count = 0;
	int32_t front_cw_sum = 0;
	int32_t our_regs = 0;
	int32_t our_stacks = 0;
	float our_eff_regs = 0.0f;
	float enemy_eff_regs = 0.0f;
	bool continuous_front = false;
	war_stance stance = war_stance::defensive;
};

inline theater_summary evaluate_theater_summary(
	sys::state& state,
	dcon::nation_id n,
	walkable_theater_map const& tmap,
	uint16_t theater_id,
	std::vector<dcon::nation_id> const& war_allies,
	std::vector<dcon::nation_id> const& war_enemies,
	bool time_against_us
) {
	theater_summary ts{};
	if(theater_id == 0)
		return ts;

	int32_t min_enemy_cw = nation_base_cw(state, n);
	for(auto e : war_enemies) {
		min_enemy_cw = std::min(min_enemy_cw, nation_base_cw(state, e));
	}
	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);

	int32_t own_front_provs = 0;
	int32_t own_front_cap_sum = 0;
	int32_t coalition_regs = 0;
	int32_t coalition_stacks = 0;
	int32_t coalition_front_provs = 0;
	int32_t coalition_front_cap_sum = 0;

	for(auto a_nat : war_allies) {
		for(auto ar : state.world.nation_get_army_control(a_nat)) {
			auto army = ar.get_army();
			if(army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			auto loc = army.get_location_from_army_location().id;
			if(!loc || tmap.theater_of(loc) != theater_id)
				continue;
			int32_t rcount = 0;
			float eff = 0.0f;
			for(auto r : army.get_army_membership()) {
				++rcount;
				eff += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
			}
			if(rcount > 0) {
				if(a_nat == n) {
					ts.our_regs += rcount;
					ts.our_stacks += 1;
				}
				coalition_regs += rcount;
				coalition_stacks += 1;
				ts.our_eff_regs += eff;
			}
		}
		for(auto pc : state.world.nation_get_province_control(a_nat)) {
			auto p = pc.get_province().id;
			if(!p || tmap.theater_of(p) != theater_id)
				continue;
			bool front = province_has_enemy_army(state, n, p);
			if(!front) {
				for(auto padj : state.world.province_get_province_adjacency(p)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(province::is_sea(state, other))
						continue;
					auto octrl = state.world.province_get_nation_from_province_control(other);
					if((octrl && military::are_at_war(state, n, octrl)) || province_has_enemy_army(state, n, other)) {
						front = true;
						break;
					}
				}
			}
			if(front) {
				// Real holding capacity of 1 front tile in ANY era is min(combat_width, province_supply_cap_in_regs)!
				int32_t sl_regs = std::max(4, int32_t(float(military::supply_limit_in_province(state, n, p)) / reg_weight));
				int32_t line_cap = std::max(4, std::min(prov_cw(state, n, p, min_enemy_cw), sl_regs));
				++coalition_front_provs;
				coalition_front_cap_sum += line_cap;
				if(a_nat == n) {
					++own_front_provs;
					own_front_cap_sum += line_cap;
				}
			}
		}
	}

	ts.front_prov_count = (own_front_provs > 0) ? own_front_provs : coalition_front_provs;
	ts.front_cw_sum = (own_front_provs > 0) ? own_front_cap_sum : coalition_front_cap_sum;

	for(auto e_nat : war_enemies) {
		for(auto ar : state.world.nation_get_army_control(e_nat)) {
			auto army = ar.get_army();
			if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			auto loc = army.get_location_from_army_location().id;
			if(!loc || tmap.theater_of(loc) != theater_id)
				continue;
			for(auto r : army.get_army_membership()) {
				ts.enemy_eff_regs += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
			}
		}
	}

	// Compare own forces against own border (AND coalition forces against coalition border)
	bool own_continuous = (own_front_provs > 0)
		&& (float(ts.our_regs) >= float(own_front_cap_sum) * 0.65f)
		&& (ts.our_stacks >= std::max(2, int32_t(std::ceil(float(own_front_provs) * 0.70f))));
	bool coalition_continuous = (coalition_front_provs > 0)
		&& (float(coalition_regs) >= float(coalition_front_cap_sum) * 0.65f)
		&& (coalition_stacks >= std::max(2, int32_t(std::ceil(float(coalition_front_provs) * 0.70f))));
	ts.continuous_front = own_continuous || coalition_continuous;

	float ratio = ts.our_eff_regs / std::max(1.0f, ts.enemy_eff_regs);
	if(ratio < 0.85f) {
		ts.stance = war_stance::defensive;
	} else if(!time_against_us && ratio < 1.20f) {
		ts.stance = war_stance::defensive;
	} else if(time_against_us && ratio < 1.20f) {
		ts.stance = war_stance::cautious_offense;
	} else {
		ts.stance = war_stance::active_offense;
	}

	return ts;
}

struct front_pin_tracker {
	struct entry {
		dcon::province_id p;
		int32_t rem_regs = 0;
		int32_t rem_stacks = 0;
	};
	std::vector<entry> entries;

	void init_for_nation(sys::state const& state, dcon::nation_id n) {
		entries.clear();
		for(auto ar : state.world.nation_get_army_control(n)) {
			auto army = ar.get_army();
			if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			auto loc = army.get_location_from_army_location().id;
			if(!loc || province::is_sea(state, loc))
				continue;
			auto regs = army.get_army_membership();
			int32_t rcount = int32_t(regs.end() - regs.begin());
			if(rcount <= 0)
				continue;
			bool found = false;
			for(auto& e : entries) {
				if(e.p == loc) {
					e.rem_regs += rcount;
					e.rem_stacks += 1;
					found = true;
					break;
				}
			}
			if(!found) {
				entries.push_back(entry{ loc, rcount, 1 });
			}
		}
	}

	entry const* find_entry(dcon::province_id p) const {
		for(auto const& e : entries) {
			if(e.p == p)
				return &e;
		}
		return nullptr;
	}

	bool can_pull(
		sys::state const& state,
		dcon::nation_id n,
		dcon::province_id loc,
		int32_t army_regs,
		int32_t army_dig_in,
		bool army_is_core,
		dcon::province_id excluded_target_prov,
		bool continuous_front,
		int32_t extra_pulled_regs = 0,
		int32_t extra_pulled_stacks = 0,
		bool allow_screen_breakthrough = false
	) const {
		if(!continuous_front)
			return true; // Austerlitz maneuver war: never pin frontline!
		float enemy_elsewhere = adjacent_enemy_regs_excluding(state, n, loc, excluded_target_prov);
		if(enemy_elsewhere <= 0.1f)
			return true; // 2nd-line reserve or no other enemy facing loc
		// Never let a tiny <8k screening detachment freeze a full corps from advancing directly into an adjacent wargoal/empty tile!
		if(allow_screen_breakthrough && enemy_elsewhere <= 2.5f && army_regs >= 6)
			return true;
		// WWI continuous front: never pull a dug-in artillery-core stack sideways off a threatened sector
		if(army_is_core && army_dig_in > 0)
			return false;
		auto const* e = find_entry(loc);
		if(!e)
			return false;
		int32_t rem_stacks_after = e->rem_stacks - extra_pulled_stacks - 1;
		int32_t rem_regs_after = e->rem_regs - extra_pulled_regs - army_regs;
		if(rem_stacks_after < 1)
			return false;
		int32_t pin_regs = std::max(4, std::min(prov_cw(state, n, loc), int32_t(std::ceil(enemy_elsewhere * 0.75f))));
		return rem_regs_after >= pin_regs;
	}

	void commit_pull(dcon::province_id loc, int32_t army_regs) {
		for(auto& e : entries) {
			if(e.p == loc) {
				e.rem_regs = std::max(0, e.rem_regs - army_regs);
				e.rem_stacks = std::max(0, e.rem_stacks - 1);
				return;
			}
		}
	}
};

} // namespace ai_front_helpers

void distribute_guards_peacetime(sys::state& state, dcon::nation_id n);

void distribute_guards_wartime(sys::state& state, dcon::nation_id n) {
	uint32_t const pcount = state.world.province_size();
	uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
	int32_t const land_count = int32_t(std::min(pcount, first_sea));
	if(land_count <= 0)
		return;

	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	dcon::province_id cap = state.world.nation_get_capital(n);
	float cap_throughput = cap ? std::max(1.0f, logistics::calculate_supply_throughput_in_province(state, cap, n)) : 1.0f;

	// Build passable cross-border theater map (own + allied + military-access + war-enemy land, NEVER split by national ownership!)
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n);

	// Collect all our guard armies (on_guard, AI-controlled, not in battle/transport/retreat)
	struct guard_unit {
		dcon::army_id id;
		dcon::province_id cur_loc;
		dcon::province_id cur_target;
		uint16_t theater_id = 0;
		int32_t regs = 0;
		int32_t dig_in = 0;
		bool is_core = false;
		bool assigned = false;
	};
	std::vector<guard_unit> guards;

	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!unit_on_ai_control(state, ar))
			continue;
		auto loc = ar.get_location_from_army_location().id;
		if(!loc || province::is_sea(state, loc))
			continue;
		auto regs_range = ar.get_army_membership();
		int32_t rcount = int32_t(regs_range.end() - regs_range.begin());
		if(rcount == 0)
			continue;

		uint16_t tid = tmap.theater_of(loc);

		if(ar.get_ai_activity() == uint8_t(army_activity::on_guard)
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_is_retreating()) {
			guards.push_back(guard_unit{
				ar.id,
				loc,
				ar.get_ai_province().id,
				tid,
				rcount,
				int32_t(ar.get_dig_in()),
				ai_front_helpers::army_has_backline_core(state, n, ar.id),
				false
			});
		}
	}

	if(guards.empty())
		return;

	// Local war participants only (ZERO global in_army / 0..land_count scans!)
	std::vector<dcon::nation_id> war_allies;
	std::vector<dcon::nation_id> war_enemies;
	ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
	bool const time_against_us = ai_front_helpers::is_time_working_against_us(state, n);

	// Lazy province status caches: 0 = unknown, 1 = false, 2 = true (evaluated ONLY on touched provinces!)
	std::vector<uint8_t> safe_cache(land_count, 0);
	std::vector<uint8_t> enemy_ctrl_cache(land_count, 0);
	std::vector<uint8_t> echelon(land_count, 0); // 1 = 1st line, 2 = 2nd line, 3 = 3rd line
	std::vector<float> enemy_source_weight(land_count, 0.0f);
	std::vector<float> incoming_enemy_weight(land_count, 0.0f);
	std::vector<float> threat(land_count, 0.0f);
	std::vector<float> nearest_enemy_days(land_count, 999.0f);
	std::vector<uint8_t> is_wargoal_prov(land_count, 0);
	std::vector<uint8_t> is_our_wargoal_prov(land_count, 0);

	auto check_enemy_ctrl = [&](dcon::province_id p) -> bool {
		int32_t idx = p.index();
		if(idx < 0 || idx >= land_count)
			return false;
		if(enemy_ctrl_cache[idx] != 0)
			return enemy_ctrl_cache[idx] == 2;
		auto ctrl = state.world.province_get_nation_from_province_control(p);
		bool res = false;
		if(ctrl && military::are_at_war(state, n, ctrl)) {
			res = true;
		} else if(!ctrl && state.world.province_get_rebel_faction_from_province_rebel_control(p)) {
			auto owner = state.world.province_get_nation_from_province_ownership(p);
			if(owner == n || (owner && nations::are_allied(state, n, owner))) {
				res = true;
			}
		}
		enemy_ctrl_cache[idx] = res ? 2 : 1;
		return res;
	};

	auto check_safe = [&](dcon::province_id p) -> bool {
		int32_t idx = p.index();
		if(idx < 0 || idx >= land_count)
			return false;
		if(safe_cache[idx] != 0)
			return safe_cache[idx] == 2;
		bool res = !check_enemy_ctrl(p) && province::has_safe_access_to_province(state, n, p);
		safe_cache[idx] = res ? 2 : 1;
		return res;
	};

	// Mark enemy wargoals (to defend) AND our offensive wargoals (to stage 2nd-line strike corps behind them!)
	for(auto wp : state.world.nation_get_war_participant(n)) {
		auto war = wp.get_war();
		bool we_attack = wp.get_is_attacker();
		for(auto wg_att : war.get_wargoals_attached()) {
			auto wg = wg_att.get_wargoal();
			bool wg_by_attacker = military::is_attacker(state, war, wg.get_added_by());
			bool wg_ours = (wg_by_attacker == we_attack);
			auto& target_vec = wg_ours ? is_our_wargoal_prov : is_wargoal_prov;
			if(auto sd = wg.get_associated_state(); sd) {
				for(auto m : sd.get_abstract_state_membership()) {
					auto pid = m.get_province().id;
					if(pid && pid.index() < land_count && m.get_province().get_nation_from_province_ownership() == wg.get_target_nation()) {
						target_vec[pid.index()] = 1;
					}
				}
			} else if(auto tn = wg.get_target_nation(); tn) {
				auto tcap = tn.get_capital().id;
				if(tcap && tcap.index() < land_count) {
					target_vec[tcap.index()] = 1;
				}
			}
		}
	}

	int32_t min_enemy_cw = ai_front_helpers::nation_base_cw(state, n);
	std::vector<dcon::province_id> threat_sources;

	auto add_hostile_army = [&](dcon::army_id aid) {
		auto ar = fatten(state.world, aid);
		if(ar.get_is_retreating() || ar.get_black_flag() || ar.get_navy_from_army_transport())
			return;
		auto loc = ar.get_location_from_army_location().id;
		if(!loc || loc.index() >= land_count)
			return;
		float eff_regs = 0.0f;
		for(auto r : ar.get_army_membership()) {
			eff_regs += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
		}
		if(eff_regs <= 0.05f)
			return;

		if(enemy_source_weight[loc.index()] == 0.0f) {
			threat_sources.push_back(loc);
		}
		enemy_source_weight[loc.index()] += eff_regs;

		// If enemy is visibly marching into one of our adjacent safe provinces, flag it for interception!
		auto path = ar.get_path();
		if(path.size() > 0) {
			auto next_p = path.at(path.size() - 1);
			if(next_p && next_p.index() < land_count && check_safe(next_p)) {
				incoming_enemy_weight[next_p.index()] += eff_regs;
			}
		}
	};

	// Gather enemy army weights ONLY from nations we are at war with (+ local rebels on our own provinces)
	for(auto e_nat : war_enemies) {
		min_enemy_cw = std::min(min_enemy_cw, ai_front_helpers::nation_base_cw(state, e_nat));
		for(auto ar : state.world.nation_get_army_control(e_nat)) {
			add_hostile_army(ar.get_army().id);
		}
	}
	for(auto po : state.world.nation_get_province_ownership(n)) {
		auto p = po.get_province();
		for(auto ar : p.get_army_location()) {
			if(!ar.get_army().get_controller_from_army_control()) {
				add_hostile_army(ar.get_army().id);
			}
		}
	}

	// Classify 1st-line safe provinces locally from war_allies' controlled provinces (zero scan of unrelated world provinces!)
	std::vector<dcon::province_id> line1_provs;
	std::vector<dcon::province_id> line2_provs;
	std::vector<dcon::province_id> line3_provs;

	auto consider_friendly_prov = [&](dcon::province_id p) {
		int32_t idx = p.index();
		if(idx < 0 || idx >= land_count || echelon[idx] == 1)
			return;
		if(!check_safe(p))
			return;
		bool front = (enemy_source_weight[idx] > 0.0f || incoming_enemy_weight[idx] > 0.0f);
		if(!front) {
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				if(other && other.index() < land_count && (check_enemy_ctrl(other) || enemy_source_weight[other.index()] > 0.0f)) {
					front = true;
					// Seed baseline border threat even if enemy army is slightly behind the border
					if(enemy_source_weight[other.index()] == 0.0f) {
						enemy_source_weight[other.index()] = 2.0f;
						threat_sources.push_back(other);
					}
					break;
				}
			}
		}
		if(front) {
			echelon[idx] = 1;
			line1_provs.push_back(p);
		}
	};

	for(auto a_nat : war_allies) {
		for(auto pc : state.world.nation_get_province_control(a_nat)) {
			consider_friendly_prov(pc.get_province().id);
		}
	}
	// Also inspect provinces around our current guards in case they are stationed in a military-access neutral ally
	for(auto const& g : guards) {
		consider_friendly_prov(g.cur_loc);
	}

	// Classify 2nd-line and 3rd-line safe provinces behind 1st line
	for(auto p1 : line1_provs) {
		for(auto padj : state.world.province_get_province_adjacency(p1)) {
			if(province::is_adjacency_impassable(state, n, padj.id))
				continue;
			dcon::province_id other = padj.get_connected_provinces(0) == p1 ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
			if(other && other.index() < land_count && check_safe(other) && echelon[other.index()] == 0) {
				echelon[other.index()] = 2;
				line2_provs.push_back(other);
			}
		}
	}
	for(auto p2 : line2_provs) {
		for(auto padj : state.world.province_get_province_adjacency(p2)) {
			if(province::is_adjacency_impassable(state, n, padj.id))
				continue;
			dcon::province_id other = padj.get_connected_provinces(0) == p2 ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
			if(other && other.index() < land_count && check_safe(other) && echelon[other.index()] == 0) {
				echelon[other.index()] = 3;
				line3_provs.push_back(other);
			}
		}
	}

	// Step 2: Bounded multi-source Dijkstra wave in REAL march days over passable land graph (blocked by neutrals & seas!)
	// Note: effective_military_distance returns km * movement_cost; infantry marches at ~4.0 * alice_army_marching_hours_per_day km/day!
	float const km_per_day = std::max(5.0f, 4.0f * state.defines.alice_army_marching_hours_per_day);
	constexpr float max_wave_days = 28.0f;
	std::vector<float> local_dist(land_count, 999.0f);
	std::vector<uint32_t> stamp(land_count, 0);
	uint32_t cur_stamp = 0;

	using pq_node = std::pair<float, int32_t>;
	std::priority_queue<pq_node, std::vector<pq_node>, std::greater<pq_node>> pq;

	for(auto src : threat_sources) {
		int32_t sidx = src.index();
		float weight = enemy_source_weight[sidx];
		if(weight <= 0.0f)
			continue;

		++cur_stamp;
		while(!pq.empty()) pq.pop();

		local_dist[sidx] = 0.0f;
		stamp[sidx] = cur_stamp;
		pq.push(pq_node{ 0.0f, sidx });

		while(!pq.empty()) {
			auto [d, uidx] = pq.top();
			pq.pop();
			if(d > local_dist[uidx])
				continue;

			dcon::province_id u{ dcon::province_id::value_base_t(uidx) };
			bool u_is_safe = check_safe(u);
			if(u_is_safe) {
				threat[uidx] += weight * std::exp(-d / 12.0f);
				if(d < nearest_enemy_days[uidx]) {
					nearest_enemy_days[uidx] = d;
				}
			}

			if(d >= max_wave_days)
				continue;

			// Wave can step out of enemy territory (up to 12 real march days deep, ~2 provinces) into our safe territory, then ONLY through safe land
			for(auto padj : state.world.province_get_province_adjacency(u)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				dcon::province_id v = padj.get_connected_provinces(0) == u ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				int32_t vidx = v.index();
				if(vidx < 0 || vidx >= land_count)
					continue;
				bool v_is_safe = check_safe(v);
				bool v_is_enemy = !v_is_safe && check_enemy_ctrl(v);
				if(!v_is_safe && !(v_is_enemy && !u_is_safe && d <= 12.0f))
					continue;

				float step_days = std::max(1.5f, military::effective_military_distance(state, n, u, v) / km_per_day);
				float nd = d + step_days;
				if(nd <= max_wave_days && (stamp[vidx] != cur_stamp || nd < local_dist[vidx])) {
					stamp[vidx] = cur_stamp;
					local_dist[vidx] = nd;
					pq.push(pq_node{ nd, vidx });
				}
			}
		}
	}

	// If no active front exists on any land border (e.g., overseas war like France vs USA), distribute across capital/ports/borders strictly respecting province supply limits!
	if(line1_provs.empty()) {
		distribute_guards_peacetime(state, n);
		return;
	}

	// Step 3: Evaluate theater density per walkable theater_id ("Austerlitz" mobile fist vs "WWI" continuous front)
	struct theater_info {
		int32_t front_prov_count = 0;
		int32_t total_guards = 0;
		int32_t core_guards = 0;
		int32_t line1_slot_count = 0;
		int32_t line2_slot_count = 0;
		int32_t desired_l2_cores = 0;
		int32_t max_l1_cores = 0;
		int32_t l1_cores_assigned = 0;
		int32_t l2_cores_assigned = 0;
		bool continuous_front = false;
		ai_front_helpers::war_stance stance = ai_front_helpers::war_stance::defensive;
		dcon::province_id fist_anchor;
		float fist_anchor_score = -1.0f;
	};
	std::vector<theater_info> theaters(std::max<size_t>(2, tmap.has_safe_port.size()));

	auto ensure_theater = [&](uint16_t tid) -> theater_info& {
		if(tid >= theaters.size()) {
			theaters.resize(size_t(tid) + 1);
		}
		return theaters[tid];
	};

	for(auto const& g : guards) {
		if(g.theater_id > 0) {
			auto& th = ensure_theater(g.theater_id);
			++th.total_guards;
			if(g.is_core) {
				++th.core_guards;
			}
		}
	}

	auto prov_reg_cap = [&](dcon::province_id p) -> int32_t {
		int32_t sl = military::supply_limit_in_province(state, n, p);
		int32_t cap_by_sl = std::max(0, int32_t(float(sl) / reg_weight));
		if(cap_by_sl <= 0)
			return 0;
		float tp = logistics::calculate_supply_throughput_in_province(state, p, n);
		if(tp <= 0.0f) {
			// Avoid parking guards in zero-throughput pockets (>1 deep in enemy land or cut off)
			return 0;
		}
		if(tp < cap_throughput * 0.25f) {
			cap_by_sl = std::min(cap_by_sl, std::max(4, cap_by_sl / 2));
		}
		return cap_by_sl;
	};

	// Count regiments already occupying p that are NOT in our reassignable guards list (allied armies + our attacking/merging armies count toward local_army_weight!)
	auto count_preoccupied_regs = [&](dcon::province_id p) -> int32_t {
		int32_t used = 0;
		for(auto oa : state.world.province_get_army_location(p)) {
			auto army = oa.get_army();
			if(army.get_black_flag() || army.get_is_retreating() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation())
				continue;
			if(army.get_controller_from_army_control() == n
				&& unit_on_ai_control(state, army)
				&& army.get_ai_activity() == uint8_t(army_activity::on_guard)) {
				continue; // will be assigned by this routine
			}
			auto r = army.get_army_membership();
			used += int32_t(r.end() - r.begin());
		}
		return used;
	};

	for(auto p : line1_provs) {
		uint32_t idx = uint32_t(p.index());
		if(threat[idx] < 0.25f)
			continue;
		uint16_t tid = tmap.theater_of(p);
		if(tid == 0)
			continue;
		auto& th = ensure_theater(tid);
		++th.front_prov_count;

		// Compute Austerlitz mobile fist anchor: prioritize confronting the MAIN enemy army concentration
		int32_t rcap = prov_reg_cap(p);
		if(rcap > 0) {
			float score = threat[idx] * 3.0f + incoming_enemy_weight[idx] * 2.5f;
			if(p == cap) score *= 1.25f;
			if(is_wargoal_prov[idx]) score *= 1.35f;
			score += ai_front_helpers::province_stockpile_importance(state, p) * 6.0f;
			score *= (0.85f + 0.015f * float(std::min(rcap, 30)));
			if(score > th.fist_anchor_score || (score == th.fist_anchor_score && (!th.fist_anchor || p.index() < th.fist_anchor.index()))) {
				th.fist_anchor_score = score;
				th.fist_anchor = p;
			}
		}
	}

	for(size_t tid = 1; tid < theaters.size(); ++tid) {
		if(theaters[tid].front_prov_count > 0) {
			// Single source of truth with assign_targets / gather_to_battle / manage_active_battles!
			auto ts = ai_front_helpers::evaluate_theater_summary(state, n, tmap, uint16_t(tid), war_allies, war_enemies, time_against_us);
			theaters[tid].continuous_front = ts.continuous_front;
			theaters[tid].stance = ts.stance;
		}
	}

	// Step 4: Build deployment slots (desired regiments, capacity, priority, echelon) for every candidate province
	struct prov_slot {
		dcon::province_id id;
		uint16_t theater_id = 0;
		uint8_t line = 0; // 1, 2, 3
		int32_t max_regs = 0;
		int32_t desired_regs = 0;
		int32_t assigned_regs = 0;
		int32_t assigned_cores = 0;
		float priority = 0.0f;
		bool near_our_wargoal = false;
	};
	std::vector<prov_slot> slots;
	std::vector<int32_t> prov_to_slot(land_count, -1);

	auto add_or_get_slot = [&](dcon::province_id p, uint8_t line) -> int32_t {
		uint32_t idx = uint32_t(p.index());
		if(prov_to_slot[idx] >= 0)
			return prov_to_slot[idx];
		uint16_t tid = tmap.theater_of(p);
		if(tid == 0)
			return -1;
		int32_t rcap = prov_reg_cap(p);
		if(rcap <= 0)
			return -1;
		int32_t pre_used = count_preoccupied_regs(p);
		int32_t sidx = int32_t(slots.size());
		prov_to_slot[idx] = sidx;
		slots.push_back(prov_slot{ p, tid, line, rcap, 0, pre_used, 0, 0.0f, false });
		auto& th = ensure_theater(tid);
		if(line == 1) {
			++th.line1_slot_count;
		} else if(line == 2) {
			++th.line2_slot_count;
		}
		return sidx;
	};

	// Populate 1st-line slots + stage 2nd-line reserve & offensive strike hubs behind the front!
	for(auto p1 : line1_provs) {
		uint32_t idx = uint32_t(p1.index());
		if(threat[idx] < 0.20f)
			continue;
		uint16_t tid = tmap.theater_of(p1);
		if(tid == 0)
			continue;
		auto const& th = ensure_theater(tid);
		int32_t sidx = add_or_get_slot(p1, 1);
		if(sidx < 0)
			continue;

		bool adj_our_wg = false;
		for(auto padj : state.world.province_get_province_adjacency(p1)) {
			if(province::is_adjacency_impassable(state, n, padj.id))
				continue;
			dcon::province_id other = padj.get_connected_provinces(0) == p1 ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
			if(other && other.index() < land_count && is_our_wargoal_prov[other.index()]) {
				adj_our_wg = true;
				break;
			}
		}
		slots[size_t(sidx)].near_our_wargoal = adj_our_wg;

		int32_t cw = ai_front_helpers::prov_cw(state, n, p1, min_enemy_cw);
		float stock_bonus = ai_front_helpers::province_stockpile_importance(state, p1);
		float strategic_mult = 1.0f + stock_bonus * 0.4f;
		if(p1 == cap) strategic_mult += 1.2f;
		if(is_wargoal_prov[idx]) strategic_mult += 0.8f;
		if(state.world.province_get_nation_from_province_ownership(p1) == n) strategic_mult += 0.25f;

		if(th.continuous_front) {
			// WWI continuous front: terrain & forts ARE strong because the enemy cannot walk around a solid line
			float terrain_def = std::max(0.0f, state.world.province_get_modifier_values(p1, sys::provincial_mod_offsets::defense));
			float fort_lvl = float(state.world.province_get_building_level(p1, uint8_t(economy::province_building_type::fort)));
			float hold_mult = strategic_mult * (1.0f + terrain_def * 0.12f + fort_lvl * 0.10f);

			int32_t want1 = std::min(slots[size_t(sidx)].max_regs, std::max(std::min(cw, 4), int32_t(std::ceil(threat[idx] * 0.85f))));
			if(incoming_enemy_weight[idx] > 0.0f) {
				want1 = std::min(slots[size_t(sidx)].max_regs, std::max(want1, int32_t(std::ceil(incoming_enemy_weight[idx] * 1.1f))));
			}
			slots[size_t(sidx)].desired_regs = want1;
			slots[size_t(sidx)].priority = (threat[idx] + incoming_enemy_weight[idx] * 2.5f + 2.0f) * hold_mult;

			// Always stage 2nd-line reserve & offensive artillery strike hubs behind active continuous fronts (especially behind our wargoal!)
			float excess_threat = std::max(0.0f, threat[idx] * 1.15f - float(want1));
			bool want_l2_hub = (excess_threat > 2.0f) || (threat[idx] >= 3.0f) || adj_our_wg || (th.stance != ai_front_helpers::war_stance::defensive);
			if(want_l2_hub) {
				float spill = std::max({ excess_threat, threat[idx] * 0.50f, adj_our_wg ? 12.0f : 5.0f });
				std::vector<dcon::province_id> adj_l2;
				for(auto padj : state.world.province_get_province_adjacency(p1)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id other = padj.get_connected_provinces(0) == p1 ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(other && other.index() < land_count && echelon[other.index()] == 2) {
						adj_l2.push_back(other);
					}
				}
				if(!adj_l2.empty()) {
					float per_l2 = spill / float(adj_l2.size());
					for(auto p2 : adj_l2) {
						int32_t s2 = add_or_get_slot(p2, 2);
						if(s2 >= 0) {
							float rr = float(state.world.province_get_building_level(p2, uint8_t(economy::province_building_type::railroad)));
							float tp = logistics::calculate_supply_throughput_in_province(state, p2, n);
							float hub_bonus = 1.0f + rr * 0.10f + std::min(0.5f, tp / std::max(1.0f, cap_throughput) * 0.25f);
							if(adj_our_wg) {
								slots[size_t(s2)].near_our_wargoal = true;
								hub_bonus *= 1.45f;
							}
							slots[size_t(s2)].desired_regs = std::min(slots[size_t(s2)].max_regs, slots[size_t(s2)].desired_regs + std::max(4, int32_t(std::ceil(per_l2))));
							slots[size_t(s2)].priority += per_l2 * 0.70f * hub_bonus;
						}
					}
				}
			}
		} else {
			// Austerlitz low-density maneuver war (NO ZoC!):
			if(p1 == th.fist_anchor) {
				slots[size_t(sidx)].desired_regs = slots[size_t(sidx)].max_regs;
				slots[size_t(sidx)].priority = std::max(slots[size_t(sidx)].priority, (threat[idx] + incoming_enemy_weight[idx] * 2.5f + 8.0f) * strategic_mult * 1.8f);
				for(auto padj : state.world.province_get_province_adjacency(p1)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id other = padj.get_connected_provinces(0) == p1 ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(other && other.index() < land_count && check_safe(other)) {
						int32_t s_nb = add_or_get_slot(other, echelon[other.index()] == 1 ? 1 : 2);
						if(s_nb >= 0) {
							slots[size_t(s_nb)].desired_regs = std::max(slots[size_t(s_nb)].desired_regs, std::min(slots[size_t(s_nb)].max_regs, int32_t(std::ceil(threat[idx] * 0.75f))));
							slots[size_t(s_nb)].priority = std::max(slots[size_t(s_nb)].priority, (threat[idx] + 4.0f) * 1.25f);
						}
					}
				}
			} else if(threat[idx] >= 0.75f || incoming_enemy_weight[idx] > 0.0f || p1 == cap || is_wargoal_prov[idx] || adj_our_wg) {
				int32_t prop_want = std::min(slots[size_t(sidx)].max_regs, std::max(3, int32_t(std::ceil(std::max(threat[idx], incoming_enemy_weight[idx]) * 1.15f))));
				slots[size_t(sidx)].desired_regs = std::max(slots[size_t(sidx)].desired_regs, prop_want);
				slots[size_t(sidx)].priority = std::max(slots[size_t(sidx)].priority, (threat[idx] + incoming_enemy_weight[idx] * 2.5f + (adj_our_wg ? 4.0f : 0.0f)) * strategic_mult);
			}
		}
	}

	// Compute 2nd-line Artillery Strike Corps allocation per continuous-front theater!
	// Never starve Line 1 of stacks (requires total_guards > line1_slot_count), while keeping 25-33% of artillery-core stacks on Line 2
	// so offensives can launch with full artillery + recon + engineers WITHOUT pulling artillery off Line 1 and exposing the front!
	for(size_t tid = 1; tid < theaters.size(); ++tid) {
		auto& th = theaters[tid];
		if(th.continuous_front && th.line2_slot_count > 0 && th.total_guards > th.line1_slot_count && th.core_guards >= 3) {
			int32_t want_l2 = 0;
			if(th.stance == ai_front_helpers::war_stance::active_offense) {
				want_l2 = std::max(1, th.core_guards / 3);
			} else if(th.stance == ai_front_helpers::war_stance::cautious_offense) {
				want_l2 = std::max(1, th.core_guards / 4);
			} else {
				want_l2 = (th.core_guards > th.line1_slot_count) ? (th.core_guards - th.line1_slot_count) : (th.core_guards >= 5 ? 1 : 0);
			}
			th.desired_l2_cores = std::min(want_l2, th.total_guards - th.line1_slot_count);
			th.max_l1_cores = std::max(1, th.core_guards - th.desired_l2_cores);
		} else {
			th.desired_l2_cores = 0;
			th.max_l1_cores = th.core_guards;
		}
	}

	// Spill overflow from heavy 2nd-line slots into 3rd-line rear reserve hubs
	size_t const pre_l3_count = slots.size();
	for(size_t i = 0; i < pre_l3_count; ++i) {
		if(slots[i].line == 2 && slots[i].desired_regs >= slots[i].max_regs && slots[i].priority > 15.0f) {
			auto p2 = slots[i].id;
			for(auto padj : state.world.province_get_province_adjacency(p2)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				dcon::province_id other = padj.get_connected_provinces(0) == p2 ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				if(other && other.index() < land_count && echelon[other.index()] == 3) {
					int32_t s3 = add_or_get_slot(other, 3);
					if(s3 >= 0) {
						slots[size_t(s3)].desired_regs = std::min(slots[size_t(s3)].max_regs, slots[size_t(s3)].desired_regs + 10);
						slots[size_t(s3)].priority += slots[i].priority * 0.35f;
					}
				}
			}
		}
	}

	if(slots.empty())
		return;

	// Step 5: Anti-ping-pong pass 1 — lock dug-in 1st-line guards onto highest-priority threatened provinces first,
	// leaving up to desired_l2_cores lowest-priority core stacks free to form 2nd-line Strike Corps while infantry seals their tiles!
	std::vector<size_t> lock_order;
	lock_order.reserve(guards.size());
	for(size_t gi = 0; gi < guards.size(); ++gi) {
		auto const& g = guards[gi];
		if(g.dig_in > 0 && g.cur_loc && g.cur_loc.index() < land_count) {
			int32_t sidx = prov_to_slot[g.cur_loc.index()];
			if(sidx >= 0 && slots[size_t(sidx)].line == 1 && slots[size_t(sidx)].priority > 1.0f) {
				lock_order.push_back(gi);
			}
		}
	}
	std::sort(lock_order.begin(), lock_order.end(), [&](size_t a, size_t b) {
		float pa = slots[size_t(prov_to_slot[guards[a].cur_loc.index()])].priority;
		float pb = slots[size_t(prov_to_slot[guards[b].cur_loc.index()])].priority;
		if(pa != pb)
			return pa > pb;
		return guards[a].id.index() < guards[b].id.index();
	});
	for(size_t gi : lock_order) {
		auto& g = guards[gi];
		int32_t sidx = prov_to_slot[g.cur_loc.index()];
		auto& sl = slots[size_t(sidx)];
		auto& th = ensure_theater(sl.theater_id);
		if(g.is_core && th.desired_l2_cores > 0 && th.l1_cores_assigned >= th.max_l1_cores) {
			continue; // Leave lowest-priority core stack free to stage as a 2nd-line Strike Corps!
		}
		if(sl.assigned_regs + g.regs <= sl.max_regs) {
			g.assigned = true;
			sl.assigned_regs += g.regs;
			if(g.is_core) {
				sl.assigned_cores += 1;
				++th.l1_cores_assigned;
			}
			state.world.army_set_ai_province(g.id, g.cur_loc);
		}
	}

	// Step 6: Assign remaining guards — Artillery-core stacks split cleanly between hottest Line 1 tiles AND Line 2 Offensive Strike Corps,
	// then pure-infantry stacks seal every remaining empty Line 1 tile + back up Line 2/3!
	std::sort(guards.begin(), guards.end(), [](guard_unit const& a, guard_unit const& b) {
		if(a.is_core != b.is_core) return a.is_core > b.is_core;
		if(a.regs != b.regs) return a.regs > b.regs;
		return a.id.index() < b.id.index();
	});

	for(auto& g : guards) {
		if(g.assigned)
			continue;

		bool assign_as_l2_strike = false;
		if(g.is_core && g.theater_id > 0 && size_t(g.theater_id) < theaters.size()) {
			auto const& gth = theaters[g.theater_id];
			if(gth.desired_l2_cores > 0 && gth.l1_cores_assigned >= gth.max_l1_cores && gth.l2_cores_assigned < gth.desired_l2_cores) {
				assign_as_l2_strike = true;
			}
		}

		int32_t best_slot = -1;
		float best_score = -1e9f;

		for(size_t si = 0; si < slots.size(); ++si) {
			auto const& sl = slots[si];
			if(sl.desired_regs <= 0 && sl.priority <= 0.0f)
				continue;
			// Strongly prefer same walkable theater (seamlessly bridges owned + allied + occupied land!)
			bool same_theater = (sl.theater_id == g.theater_id) && (g.theater_id != 0);
			if(!same_theater && !state.world.army_get_black_flag(g.id)) {
				continue;
			}
			// Strictly respect province supply limit outside battle (0% attrition — never stack over max_regs!)
			if(sl.assigned_regs + g.regs > sl.max_regs) {
				continue;
			}

			int32_t deficit = std::max(0, sl.desired_regs - sl.assigned_regs);
			float need_factor = deficit > 0
				? (1.0f + float(std::min(deficit, g.regs)) / float(std::max(1, g.regs)))
				: 0.25f;

			// Role fit:
			// - Frontline core stacks -> hottest Line 1 sectors without a core stack yet
			// - Designated 2nd-line Strike Corps -> Line 2 hubs closest to our offensive wargoal (ready to attack with artillery without exposing Line 1!)
			// - Pure infantry stacks -> immediately seal any empty Line 1 tile (2.40x), then reinforce Line 2/3!
			float role_mult = 1.0f;
			if(assign_as_l2_strike) {
				if(sl.line == 2) {
					role_mult = (sl.assigned_cores == 0 ? 2.80f : 1.70f) * (sl.near_our_wargoal ? 1.40f : 1.0f);
				} else if(sl.line == 3) {
					role_mult = 0.85f;
				} else {
					role_mult = 0.45f;
				}
			} else if(sl.line == 1) {
				if(g.is_core) {
					role_mult = (sl.assigned_cores == 0) ? 1.85f : 1.05f;
				} else {
					role_mult = (sl.assigned_regs == 0) ? 2.40f : 0.75f;
				}
			} else if(sl.line == 2) {
				role_mult = g.is_core ? 0.85f : 1.35f;
			} else {
				role_mult = g.is_core ? 0.55f : 1.05f;
			}

			float dist_km = province::direct_distance_km(state, g.cur_loc, sl.id);
			float dist_penalty = 1.0f + dist_km / 250.0f;
			float score = (sl.priority * need_factor * role_mult) / dist_penalty;

			// Anti-ping-pong hysteresis (+30% bonus for staying on current province or keeping current valid destination)
			if(sl.id == g.cur_loc) {
				score *= (g.dig_in > 0 ? 1.65f : 1.30f);
			} else if(sl.id == g.cur_target) {
				score *= 1.25f;
			}

			if(score > best_score || (score == best_score && best_slot >= 0 && sl.id.index() < slots[size_t(best_slot)].id.index())) {
				best_score = score;
				best_slot = int32_t(si);
			}
		}

		if(best_slot >= 0) {
			g.assigned = true;
			auto& chosen_sl = slots[size_t(best_slot)];
			chosen_sl.assigned_regs += g.regs;
			if(g.is_core) {
				chosen_sl.assigned_cores += 1;
				auto& cth = ensure_theater(chosen_sl.theater_id);
				if(chosen_sl.line == 1) {
					++cth.l1_cores_assigned;
				} else if(chosen_sl.line == 2) {
					++cth.l2_cores_assigned;
				}
			}
			state.world.army_set_ai_province(g.id, chosen_sl.id);
		} else {
			// Safe supply-respecting fallback inside the guard's own walkable theater
			dcon::province_id safe_rear_prov;
			float best_rear_score = -1e9f;
			dcon::province_id empty_max_prov;
			int32_t empty_max_cap = -1;

			for(auto pc : state.world.nation_get_province_control(n)) {
				auto rp = pc.get_province().id;
				if(!rp || rp.index() >= land_count)
					continue;
				if(tmap.theater_of(rp) != g.theater_id)
					continue;
				if(!check_safe(rp))
					continue;
				int32_t rcap = prov_reg_cap(rp);
				if(rcap <= 0)
					continue;
				int32_t existing_idx = prov_to_slot[rp.index()];
				int32_t cur_used = (existing_idx >= 0) ? slots[size_t(existing_idx)].assigned_regs : count_preoccupied_regs(rp);
				if(cur_used == 0 && rcap > empty_max_cap) {
					empty_max_cap = rcap;
					empty_max_prov = rp;
				}
				if(cur_used + g.regs > rcap)
					continue;

				float rscore = 10.0f;
				if(rp == cap) rscore += 20.0f;
				if(state.world.province_get_is_coast(rp)) {
					rscore += 6.0f + float(state.world.province_get_building_level(rp, uint8_t(economy::province_building_type::naval_base))) * 4.0f;
				}
				if(rp == g.cur_loc) rscore *= 1.35f;
				float rear_km = province::direct_distance_km(state, g.cur_loc, rp);
				float dist_pen = 1.0f + rear_km / 250.0f;
				rscore /= dist_pen;
				if(rscore > best_rear_score || (rscore == best_rear_score && safe_rear_prov && rp.index() < safe_rear_prov.index())) {
					best_rear_score = rscore;
					safe_rear_prov = rp;
				}
			}

			if(safe_rear_prov) {
				int32_t s_rear = add_or_get_slot(safe_rear_prov, 3);
				if(s_rear >= 0) {
					g.assigned = true;
					slots[size_t(s_rear)].assigned_regs += g.regs;
					state.world.army_set_ai_province(g.id, safe_rear_prov);
				}
			} else if(empty_max_prov) {
				// Oversized starting stack (before new_units_and_merging splits it): park it ALONE in the highest-supply empty province
				int32_t s_rear = add_or_get_slot(empty_max_prov, 3);
				if(s_rear >= 0) {
					g.assigned = true;
					slots[size_t(s_rear)].assigned_regs += g.regs;
					state.world.army_set_ai_province(g.id, empty_max_prov);
				}
			} else {
				// Last-resort cross-region fallback only if a transportable front slot has supply room
				int32_t fallback_slot = -1;
				float fb_score = -1.0f;
				for(size_t si = 0; si < slots.size(); ++si) {
					if(slots[si].assigned_regs + g.regs <= slots[si].max_regs && slots[si].priority > fb_score) {
						fb_score = slots[si].priority;
						fallback_slot = int32_t(si);
					}
				}
				if(fallback_slot >= 0) {
					g.assigned = true;
					slots[size_t(fallback_slot)].assigned_regs += g.regs;
					state.world.army_set_ai_province(g.id, slots[size_t(fallback_slot)].id);
				}
			}
		}
	}
}

void distribute_guards_peacetime(sys::state& state, dcon::nation_id n) {
	std::vector<classified_province> provinces;
	auto ctrl_range = state.world.nation_get_province_control(n);
	provinces.reserve(size_t(ctrl_range.end() - ctrl_range.begin()));

	auto cap = state.world.nation_get_capital(n);
	bool at_war = state.world.nation_get_is_at_war(n);

	for(auto c : ctrl_range) {
		auto prov = c.get_province();
		province_class cls = prov.get_is_coast() ? province_class::coast : province_class::interior;
		if(prov == cap) {
			cls = province_class::border;
		} else if(at_war && prov.get_is_coast() && prov.get_building_level(uint8_t(economy::province_building_type::naval_base)) > 0) {
			// During overseas war, defend main naval ports alongside capital
			cls = province_class::border;
		}

		for(auto padj : prov.get_province_adjacency()) {
			auto other = padj.get_connected_provinces(0) == prov ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			auto n_controller = other.get_nation_from_province_control();
			auto ovr = n_controller.get_overlord_as_subject().get_ruler();

			if(n_controller == n) {
				// own province
			} else if(!n_controller && !other.get_rebel_faction_from_province_rebel_control()) {
				// uncolonized or sea
			} else if(other.get_rebel_faction_from_province_rebel_control()) {
				cls = province_class::hostile_border;
				break;
			} else if(military::are_at_war(state, n, n_controller)) {
				cls = province_class::hostile_border;
				break;
			} else if(nations::are_allied(state, n, n_controller) || (ovr && ovr == n) || (ovr && nations::are_allied(state, n, ovr))) {
				if(uint8_t(cls) < uint8_t(province_class::low_priority_border)) {
					cls = province_class::low_priority_border;
				}
			} else {
				bool is_threat = false;
				if(n_controller) {
					is_threat |= n_controller.get_ai_rival() == n;
					is_threat |= state.world.nation_get_ai_rival(n) == n_controller.id;
					if(ovr) {
						is_threat |= ovr.get_ai_rival() == n;
						is_threat |= state.world.nation_get_ai_rival(n) == ovr.id;
						is_threat |= ovr.get_constructing_cb_target() == n;
						for(auto cb : ovr.get_available_cbs())
							is_threat |= cb.target == n;
					} else {
						is_threat |= n_controller.get_constructing_cb_target() == n;
						for(auto cb : n_controller.get_available_cbs())
							is_threat |= cb.target == n;
					}
				}
				if(is_threat) {
					if(uint8_t(cls) < uint8_t(province_class::threat_border)) {
						cls = province_class::threat_border;
					}
				} else {
					if(uint8_t(cls) < uint8_t(province_class::border)) {
						cls = province_class::border;
					}
				}
			}
		}
		provinces.push_back(classified_province{ prov.id, cls });
	}

	if(provinces.empty())
		return;

	std::sort(provinces.begin(), provinces.end(), [&](classified_province const& a, classified_province const& b) {
		if(a.c != b.c) {
			return uint8_t(a.c) > uint8_t(b.c);
		}
		auto adist = province::sorting_distance(state, a.id, cap);
		auto bdist = province::sorting_distance(state, b.id, cap);
		if(adist != bdist) {
			return adist < bdist;
		}
		return a.id.index() < b.id.index();
	});

	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	struct peace_prov_cap {
		int32_t cap_regs = 0;
		int32_t used_regs = 0;
		uint16_t region_id = 0;
	};
	std::vector<peace_prov_cap> pcaps(provinces.size());
	for(size_t j = 0; j < provinces.size(); ++j) {
		auto p = provinces[j].id;
		int32_t sl = military::supply_limit_in_province(state, n, p);
		int32_t cap_regs = std::max(0, int32_t(float(sl) / reg_weight));
		int32_t pre_used = 0;
		for(auto oa : state.world.province_get_army_location(p)) {
			auto army = oa.get_army();
			if(army.get_black_flag() || army.get_is_retreating() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation())
				continue;
			if(army.get_controller_from_army_control() == n
				&& unit_on_ai_control(state, army)
				&& army.get_ai_activity() == uint8_t(army_activity::on_guard)) {
				continue;
			}
			auto r = army.get_army_membership();
			pre_used += int32_t(r.end() - r.begin());
		}
		pcaps[j] = peace_prov_cap{ cap_regs, pre_used, state.world.province_get_connected_region_id(p) };
	}

	struct peace_guard {
		dcon::army_id id;
		dcon::province_id loc;
		uint16_t region_id = 0;
		int32_t regs = 0;
		bool black_flag = false;
	};
	std::vector<peace_guard> guards_list;
	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!unit_on_ai_control(state, ar)
			|| ar.get_ai_activity() != uint8_t(army_activity::on_guard)
			|| ar.get_battle_from_army_battle_participation()
			|| ar.get_navy_from_army_transport()
			|| ar.get_is_retreating()) {
			continue;
		}
		auto loc = ar.get_location_from_army_location().id;
		if(!loc || province::is_sea(state, loc))
			continue;
		auto regs = ar.get_army_membership();
		int32_t rcount = int32_t(regs.end() - regs.begin());
		if(rcount <= 0)
			continue;
		guards_list.push_back(peace_guard{ ar.id, loc, state.world.province_get_connected_region_id(loc), rcount, ar.get_black_flag() });
	}
	if(guards_list.empty())
		return;

	// Distribute across priority stages strictly within each province's real supply_limit (0% attrition!)
	uint32_t end_of_stage = 0;
	for(uint8_t stage = uint8_t(province_class::count); stage-- > 0 && !guards_list.empty(); ) {
		uint32_t start_of_stage = end_of_stage;
		for(; end_of_stage < provinces.size(); ++end_of_stage) {
			if(uint8_t(provinces[end_of_stage].c) != stage)
				break;
		}

		bool guard_assigned = false;
		do {
			guard_assigned = false;
			for(uint32_t j = start_of_stage; j < end_of_stage && !guards_list.empty(); ++j) {
				auto p = provinces[j].id;
				auto& pc = pcaps[j];
				if(pc.used_regs >= pc.cap_regs)
					continue;

				int32_t best_k = -1;
				float best_dist = 1e9f;
				for(size_t k = 0; k < guards_list.size(); ++k) {
					auto const& g = guards_list[k];
					if(pc.used_regs + g.regs > pc.cap_regs)
						continue; // STRICT supply limit check (accounting for already assigned & preoccupied regiments!)
					if(g.region_id != pc.region_id && !g.black_flag)
						continue;
					float d = province::sorting_distance(state, g.loc, p);
					if(g.loc == p) d -= 0.01f; // mild hysteresis to prevent pointless shuffling
					if(best_k < 0 || d < best_dist || (d == best_dist && g.id.index() < guards_list[size_t(best_k)].id.index())) {
						best_k = int32_t(k);
						best_dist = d;
					}
				}

				if(best_k >= 0) {
					state.world.army_set_ai_province(guards_list[size_t(best_k)].id, p);
					pc.used_regs += guards_list[size_t(best_k)].regs;
					guards_list[size_t(best_k)] = guards_list.back();
					guards_list.pop_back();
					guard_assigned = true;
				}
			}
		} while(guard_assigned);
	}

	// Any remaining oversized or cross-region guards: park ALONE in the highest-supply empty province of their region (never stack on top of another army!)
	for(auto const& g : guards_list) {
		int32_t best_j = -1;
		int32_t best_cap = -1;
		for(size_t j = 0; j < provinces.size(); ++j) {
			if(pcaps[j].region_id != g.region_id && !g.black_flag)
				continue;
			if(pcaps[j].used_regs == 0 && pcaps[j].cap_regs > best_cap) {
				best_cap = pcaps[j].cap_regs;
				best_j = int32_t(j);
			}
		}
		if(best_j >= 0) {
			state.world.army_set_ai_province(g.id, provinces[size_t(best_j)].id);
			pcaps[size_t(best_j)].used_regs += g.regs;
		}
	}
}

void distribute_guards(sys::state& state, dcon::nation_id n) {
	if(state.world.nation_get_is_at_war(n)) {
		distribute_guards_wartime(state, n);
		return;
	}
	distribute_guards_peacetime(state, n);
}

dcon::navy_id find_transport_fleet(sys::state& state, dcon::nation_id controller) {
	int32_t n_size = 0;
	dcon::navy_id transport_fleet;

	for(auto v : state.world.nation_get_navy_control(controller)) {
		if(v.get_navy().get_battle_from_navy_battle_participation() || !unit_on_ai_control(state, v.get_navy()))
			continue;
		auto members = v.get_navy().get_navy_membership();

		auto tsize = int32_t(members.end() - members.begin());
		if(tsize <= n_size || tsize <= 1)
			continue;

		n_size = tsize;
		transport_fleet = dcon::navy_id{};

		fleet_activity activity = fleet_activity(v.get_navy().get_ai_activity());
		if(activity == fleet_activity::attacking || activity == fleet_activity::idle || activity == fleet_activity::returning_to_base) {
			auto in_transport = v.get_navy().get_army_transport();
			if(in_transport.begin() == in_transport.end()) {
				transport_fleet = v.get_navy();
			}
		}
	}

	return transport_fleet;
}

void move_idle_guards_impl(sys::state& state, bool only_at_war) {
	std::vector<dcon::army_id> require_transport;

	auto process_army = [&](dcon::army_fat_id ar) {
		if(ar.get_ai_activity() == uint8_t(army_activity::on_guard)
			&& ar.get_ai_province()
			&& ar.get_ai_province() != ar.get_location_from_army_location()
			&& ar.get_controller_from_army_control()
			&& unit_on_ai_control(state, ar)
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {

			auto ctrl = ar.get_controller_from_army_control();
			auto valid_path = military::move_army_ai(state, ar.id, ar.get_ai_province(), ctrl);

			if(!valid_path) {
				// Units delegated to the AI won't transport themselves on their own
				if(!ctrl.get_is_player_controlled())
					require_transport.push_back(ar.id);
			} else if(!ctrl.get_is_player_controlled()
				&& !ar.get_black_flag()
				&& ar.get_path().size() >= 6
				&& state.world.province_get_building_level(ar.get_location_from_army_location(), uint8_t(economy::province_building_type::railroad)) > 0
				&& !ai_front_helpers::is_province_near_hostiles(state, ctrl, ar.get_location_from_army_location(), 2)
				&& !ai_front_helpers::is_province_near_hostiles(state, ctrl, ar.get_ai_province(), 2)) {
				// Safe deep-rear strategic redeployment on railroads (drops org to 10% while moving, so strictly forbidden near hostiles!)
				ar.set_special_order(uint8_t(military::special_army_order::strategic_redeployment));
			}
		}
	};

	if(only_at_war) {
		for(auto n : state.world.in_nation) {
			if(n.get_owned_province_count() == 0 || !n.get_is_at_war())
				continue;
			for(auto a : n.get_army_control()) {
				process_army(a.get_army());
			}
		}
	} else {
		require_transport.reserve(state.world.army_size());
		for(auto ar : state.world.in_army) {
			process_army(ar);
		}
	}

	for(uint32_t i = 0; i < require_transport.size(); ++i) {
		auto coastal_target_prov = state.world.army_get_location_from_army_location(require_transport[i]);
		auto controller = state.world.army_get_controller_from_army_control(require_transport[i]);

		dcon::navy_id transport_fleet = find_transport_fleet(state, controller);

		auto regs = state.world.army_get_army_membership(require_transport[i]);

		auto tcap = transport_fleet ? military::transport_capacity(state, transport_fleet) : 0;
		tcap -= int32_t(regs.end() - regs.begin());

		// no transport fleet at all: the army cannot be shipped (and an empty army, tcap == 0, must not path a null navy)
		if(!transport_fleet || tcap < 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
			for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					state.world.army_set_ai_province(require_transport[j], dcon::province_id{}); // stop rechecking these units
					require_transport[j] = require_transport.back();
					require_transport.pop_back();
				}
			}
			state.world.army_set_ai_province(require_transport[i], dcon::province_id{}); // stop rechecking unit
			continue;
		}

		if(!state.world.province_get_is_coast(coastal_target_prov)) {
			auto path = state.world.army_get_black_flag(require_transport[i])
				? province::make_unowned_path_to_nearest_coast(state, coastal_target_prov)
				: province::make_path_to_nearest_coast(state, controller, coastal_target_prov);
			bool valid_path = military::set_army_path(state, require_transport[i], path, controller);
			if(!valid_path) {
				state.world.army_set_ai_province(require_transport[i], dcon::province_id{}); // stop rechecking unit
				continue; // army could not reach coast
			} else {
				coastal_target_prov = path.front();

			}
		}

		{
			auto fleet_destination = province::has_naval_access_to_province(state, controller, coastal_target_prov) ? coastal_target_prov : state.world.province_get_port_to(coastal_target_prov);
			if(fleet_destination == state.world.navy_get_location_from_navy_location(transport_fleet)) {
				military::stop_navy_movement(state, transport_fleet);
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
			} else if(auto valid_path = military::move_navy_ai(state, transport_fleet, fleet_destination); !valid_path) { // this essentially should be impossible ...
				continue;
			} else {
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
			}
		}

		state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_guard));

		auto destination_region = state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[i]));

		// scoop up other armies to transport
		for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
			if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
				auto jregs = state.world.army_get_army_membership(require_transport[j]);
				if(tcap >= (jregs.end() - jregs.begin())) { // check if it will fit
					if(state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[j])) != destination_region)
						continue;

					if(state.world.army_get_location_from_army_location(require_transport[j]) == coastal_target_prov) {
						state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::transport_guard));
						tcap -= int32_t(jregs.end() - jregs.begin());
					} else {
						auto valid_path = military::move_army_ai(state, require_transport[j], coastal_target_prov, controller);
						if(valid_path) {
							state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::transport_guard));
							tcap -= int32_t(jregs.end() - jregs.begin());
						}
					}
				}

				require_transport[j] = require_transport.back();
				require_transport.pop_back();
			}
		}
	}
}

void move_idle_guards(sys::state& state) {
	move_idle_guards_impl(state, false);
}

void update_naval_transport(sys::state& state) {

	// set armies to move into transports
	for(auto ar : state.world.in_army) {
		if(ar.get_battle_from_army_battle_participation())
			continue;
		if(ar.get_navy_from_army_transport())
			continue;
		if(ar.get_arrival_time())
			continue;

		if(ar.get_ai_activity() == uint8_t(army_activity::transport_guard) || ar.get_ai_activity() == uint8_t(army_activity::transport_attack)) {
			auto controller = ar.get_controller_from_army_control();
			dcon::navy_id transports;
			for(auto v : controller.get_navy_control()) {
				if(v.get_navy().get_ai_activity() == uint8_t(fleet_activity::boarding)) {
					transports = v.get_navy();
				}
			}
			if(!transports) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
				continue;
			}
			if(state.world.navy_get_arrival_time(transports) || state.world.navy_get_battle_from_navy_battle_participation(transports))
				continue; // still moving

			auto army_location = ar.get_location_from_army_location();
			auto transport_location = state.world.navy_get_location_from_navy_location(transports);
			if(transport_location == army_location) {
				ar.set_navy_from_army_transport(transports);
				ar.set_black_flag(false);
			} else if(army_location.get_port_to() == transport_location) {
				military::move_army_ai(state, ar, transport_location, controller);
				assert(transport_location);
			} else { // transport arrived in inaccessible location
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
			}
		}
	}
}

bool army_ready_for_battle(sys::state& state, dcon::nation_id n, dcon::army_id a) {
	auto regs = state.world.army_get_army_membership(a);
	if(regs.begin() == regs.end()) {
		return false;
	}
	float total_org = 0.0f;
	float total_str = 0.0f;
	int32_t count = 0;
	for(auto r : regs) {
		total_org += r.get_regiment().get_org();
		total_str += r.get_regiment().get_strength();
		++count;
	}
	if(count == 0) {
		return false;
	}
	// Require reasonable average organization AND combat manpower across the entire stack
	return (total_org / float(count)) > 0.45f && (total_str / float(count)) > 0.25f;
}

// MP compliant: proportional day-1 battle reinforcement (ongoing reinforcement & retreat handled daily by manage_active_battles)
void gather_to_battle(sys::state& state, dcon::nation_id n, dcon::province_id p) {
	if(!state.world.nation_is_valid(n) || state.world.nation_get_is_player_controlled(n) || !p || province::is_sea(state, p))
		return;

	int32_t cw = ai_front_helpers::prov_cw(state, n, p);
	int32_t friendly_regs_here = 0;
	int32_t enemy_regs_here = 0;
	bool at_war = state.world.nation_get_is_at_war(n);
	for(auto ar : state.world.province_get_army_location(p)) {
		auto army = ar.get_army();
		if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
			continue;
		auto ctrl = army.get_controller_from_army_control();
		auto regs = army.get_army_membership();
		int32_t rcount = int32_t(regs.end() - regs.begin());
		if(ctrl == n || (ctrl && military::are_on_same_side_in_common_war(state, n, ctrl))) {
			friendly_regs_here += rcount;
		} else if(!ctrl || (at_war && military::are_at_war(state, n, ctrl))) {
			enemy_regs_here += rcount;
		}
	}

	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n, p);
	uint16_t p_theater = tmap.theater_of(p);
	bool continuous_front = false;
	if(at_war && p_theater != 0) {
		std::vector<dcon::nation_id> war_allies;
		std::vector<dcon::nation_id> war_enemies;
		ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
		auto ts = ai_front_helpers::evaluate_theater_summary(state, n, tmap, p_theater, war_allies, war_enemies, false);
		continuous_front = ts.continuous_front;
	}

	// Size battle commitment to the ACTUAL enemy size in this battle (never send 180k against a 12k landing just because 1836 CW == 30!)
	int32_t needed_front = std::min(cw, std::max(4, enemy_regs_here + 4));
	int32_t target_regs = std::min(cw * 2, std::max(needed_front + 4, int32_t(std::ceil(float(enemy_regs_here) * 1.65f)) + 3));
	if(friendly_regs_here >= target_regs || p_theater == 0)
		return;

	ai_front_helpers::front_pin_tracker pin_tracker;
	if(continuous_front) {
		pin_tracker.init_for_nation(state, n);
	}

	struct cand {
		dcon::army_id id;
		dcon::province_id loc;
		float dist;
		int32_t regs;
		int32_t dig_in;
		bool is_core;
		bool is_second_line;
	};
	std::vector<cand> cands;
	for(auto ar : state.world.nation_get_army_control(n)) {
		auto army = ar.get_army();
		if(!unit_on_ai_control(state, army))
			continue;
		army_activity activity = army_activity(army.get_ai_activity());
		if(army.get_battle_from_army_battle_participation()
			|| army.get_navy_from_army_transport()
			|| army.get_black_flag()
			|| army.get_is_retreating()
			|| (activity != army_activity::on_guard && activity != army_activity::attacking && activity != army_activity::attack_gathered)
			|| !army_ready_for_battle(state, n, army)) {
			continue;
		}
		auto location = army.get_location_from_army_location().id;
		if(!location || location == p || province::is_sea(state, location))
			continue;
		if(tmap.theater_of(location) != p_theater)
			continue;
		float dist_km = province::direct_distance_km(state, location, p);
		float max_gather_km = continuous_front ? 400.0f : 600.0f;
		if(dist_km > max_gather_km)
			continue;

		auto regs = army.get_army_membership();
		int32_t rcount = int32_t(regs.end() - regs.begin());
		if(rcount == 0)
			continue;

		bool is_core = ai_front_helpers::army_has_backline_core(state, n, army.id);
		int32_t dig_in = int32_t(army.get_dig_in());
		float enemy_elsewhere = ai_front_helpers::adjacent_enemy_regs_excluding(state, n, location, p);
		// Even in Austerlitz mode, NEVER abandon a larger main enemy army on the front to pile onto a tiny rear raid!
		if(!continuous_front && enemy_elsewhere > float(std::max(4, enemy_regs_here)) * 1.25f)
			continue;
		bool second_line = (enemy_elsewhere <= 0.1f);

		cands.push_back(cand{ army.id, location, dist_km, rcount, dig_in, is_core, second_line });
	}

	// Prefer free 2nd-line reserves first (zero frontline disruption!), then closest stacks
	std::sort(cands.begin(), cands.end(), [](cand const& a, cand const& b) {
		if(a.is_second_line != b.is_second_line)
			return a.is_second_line > b.is_second_line;
		if(a.dist != b.dist)
			return a.dist < b.dist;
		return a.id.index() < b.id.index();
	});

	for(auto const& c : cands) {
		if(friendly_regs_here >= target_regs)
			break;
		if(!pin_tracker.can_pull(state, n, c.loc, c.regs, c.dig_in, c.is_core, p, continuous_front))
			continue;
		state.world.army_set_special_order(c.id, uint8_t(military::special_army_order::none));
		if(military::move_army_ai(state, c.id, p, n, true)) {
			pin_tracker.commit_pull(c.loc, c.regs);
			friendly_regs_here += c.regs;
		}
	}
}

float estimate_balanced_composition_factor(sys::state& state, dcon::army_id a) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	auto regs = state.world.army_get_army_membership(a);
	if(regs.begin() == regs.end())
		return 0.0f;
	// account composition
	// Ideal composition: 4/1/4 (1 cavalry for each 4 infantry and 1 infantry for each arty)
	float total_str = 0.f;
	float str_art = 0.f;
	float str_inf = 0.f;
	float str_cav = 0.f;
	for(const auto reg : regs) {
		float str = reg.get_regiment().get_strength() * reg.get_regiment().get_org();
		assert(std::isfinite(reg.get_regiment().get_strength()));
		assert(std::isfinite(reg.get_regiment().get_org()));
		if(auto utid = reg.get_regiment().get_type(); utid) {
			switch(state.military_definitions.unit_base_definitions[utid].type) {
			case military::unit_type::infantry:
				str_inf += str;
				break;
			case military::unit_type::cavalry:
				str_cav += str;
				break;
			case military::unit_type::support:
			case military::unit_type::special:
				str_art += str;
				break;
			default:
				break;
			}
		}
		total_str += str;
	}
	if(total_str == 0.f)
		return 0.f;
	// provide continous function for each military unit composition
	// such that 4x times the infantry (we min with arty for equality reasons) and 1/4th of cavalry
	
	assert(std::isfinite(total_str));
	assert(std::isfinite(str_inf));
	assert(std::isfinite(str_art));
	assert(std::isfinite(str_cav));

	float min_cav = std::min(str_cav, str_inf * (1.f / 4.f)); // more cavalry isn't bad (if the rest of the composition is 4x/y/4x), just don't underestimate it!
	assert(std::isfinite(min_cav));

	float scale = 1.f - math::sin(std::abs(std::min(str_art / total_str, str_inf / total_str) - (4.f * min_cav / total_str)));
	assert(std::isfinite(scale));

	return total_str * scale;
}

float estimate_army_quality(sys::state& state, dcon::army_id a) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	auto regs = state.world.army_get_army_membership(a);
	if(regs.begin() == regs.end())
		return 0.0f;
	// average army quality per regiment (linear, not N^2 when multiplied by composition factor)
	float total_str = 0.f;
	int32_t reg_count = 0;
	auto owner = military::tech_nation_for_army(state, a);
	for(const auto reg : regs) {
		auto type = reg.get_regiment().get_type();
		auto stats = owner ? state.world.nation_get_unit_stats(owner, type) : military::unit_definition{};
		auto atk = (owner && stats.discipline_or_evasion > 0.0f) ? stats.attack_or_gun_power : state.military_definitions.unit_base_definitions[type].attack_or_gun_power;
		auto def = (owner && stats.discipline_or_evasion > 0.0f) ? stats.defence_or_hull : state.military_definitions.unit_base_definitions[type].defence_or_hull;
		auto sup = (owner && stats.discipline_or_evasion > 0.0f) ? stats.support : state.military_definitions.unit_base_definitions[type].support;

		total_str += (10.f + atk + 10.f + def + sup * 2.f) * 0.5f * (1.f + reg.get_regiment().get_experience());
		++reg_count;
	}
	if(reg_count == 0)
		return 0.0f;
	total_str /= float(reg_count);
	assert(std::isfinite(total_str));

	return total_str;
}

float estimate_army_defensive_strength(sys::state& state, dcon::army_id a) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	float scale = state.world.army_get_controller_from_army_control(a) ? 1.f : 0.9f;
	auto n = military::tech_nation_for_army(state, a);
	auto gen = state.world.army_get_general_from_army_leadership(a);
	auto back = military::get_leader_background_wrapper(state, gen);
	auto pers = military::get_leader_personality_wrapper(state, gen);
	float morale = (n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::org_regain) : 0.0f)
		+ state.world.leader_trait_get_morale(back)
		+ state.world.leader_trait_get_morale(pers) + 1.0f;
	float org = (n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_organisation) : 0.0f)
		+ state.world.leader_trait_get_organisation(back)
		+ state.world.leader_trait_get_organisation(pers) + 1.0f;
	float def = (n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_defense_modifier) : 0.0f)
		+ state.world.leader_trait_get_defense(back)
		+ state.world.leader_trait_get_defense(pers);
	float leader_mult = std::max(0.25f, (1.0f + def * 0.15f) * std::max(0.25f, morale) * std::max(0.25f, org));
	scale *= leader_mult;
	if(n && state.world.nation_get_has_gas_defense(n)) {
		scale *= 1.15f;
	}
	// terrain & fort defensive bonus
	auto loc = state.world.army_get_location_from_army_location(a);
	float terrain_bonus = loc ? std::max(0.0f, state.world.province_get_modifier_values(loc, sys::provincial_mod_offsets::defense)) : 0.0f;
	float fort_level = loc ? float(state.world.province_get_building_level(loc, uint8_t(economy::province_building_type::fort))) : 0.0f;
	float dig_in = float(state.world.army_get_dig_in(a));
	scale *= (1.0f + terrain_bonus * 0.15f + dig_in * 0.12f) * (1.0f + state.defines.alice_fort_mil_tactics_discipline_per_level * fort_level);
	if(n) {
		scale *= std::max(0.25f, state.defines.base_military_tactics + state.world.nation_get_modifier_values(n, sys::national_mod_offsets::military_tactics));
	}
	float strength = estimate_balanced_composition_factor(state, a) * estimate_army_quality(state, a);
	return std::max(0.1f, strength * scale);
}

float estimate_army_offensive_strength(sys::state& state, dcon::army_id a) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	float scale = state.world.army_get_controller_from_army_control(a) ? 1.f : 0.9f;
	auto n = military::tech_nation_for_army(state, a);
	auto gen = state.world.army_get_general_from_army_leadership(a);
	auto back = military::get_leader_background_wrapper(state, gen);
	auto pers = military::get_leader_personality_wrapper(state, gen);
	float morale = (n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::org_regain) : 0.0f)
		+ state.world.leader_trait_get_morale(back)
		+ state.world.leader_trait_get_morale(pers) + 1.0f;
	float org = (n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_organisation) : 0.0f)
		+ state.world.leader_trait_get_organisation(back)
		+ state.world.leader_trait_get_organisation(pers) + 1.0f;
	float atk = (n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_attack_modifier) : 0.0f)
		+ state.world.leader_trait_get_attack(back)
		+ state.world.leader_trait_get_attack(pers);
	float leader_mult = std::max(0.25f, (1.0f + atk * 0.15f) * std::max(0.25f, morale) * std::max(0.25f, org));
	scale *= leader_mult;
	if(n && state.world.nation_get_has_gas_attack(n)) {
		scale *= 1.35f;
	}
	if(n) {
		scale *= std::max(0.25f, state.defines.base_military_tactics + state.world.nation_get_modifier_values(n, sys::national_mod_offsets::military_tactics));
	}
	float strength = estimate_balanced_composition_factor(state, a) * estimate_army_quality(state, a);
	return std::max(0.1f, strength * scale);
}

float estimate_win_probability(sys::state& state, std::vector<dcon::army_id> const& attacker, std::vector<dcon::army_id> const& defender) {
	if(attacker.size() == 0) {
		return 0.f;
	}
	if(defender.size() == 0) {
		return 1.f;
	}
	float attacker_str = 0.f;
	float attacker_tactic = 0.f;
	for(auto a : attacker) {
		auto nation = military::tech_nation_for_army(state, a);
		float a_str = 0.f;
		for(const auto reg : state.world.army_get_army_membership(a)) {
			auto type = reg.get_regiment().get_type();
			auto stats = nation ? state.world.nation_get_unit_stats(nation, type) : military::unit_definition{};
			auto atk = (nation && stats.discipline_or_evasion > 0.0f) ? stats.attack_or_gun_power : state.military_definitions.unit_base_definitions[type].attack_or_gun_power;
			auto def = (nation && stats.discipline_or_evasion > 0.0f) ? stats.defence_or_hull : state.military_definitions.unit_base_definitions[type].defence_or_hull;
			auto sup = (nation && stats.discipline_or_evasion > 0.0f) ? stats.support : state.military_definitions.unit_base_definitions[type].support;
			auto utype = state.military_definitions.unit_base_definitions[type].type;
			float sup_mult = (utype == military::unit_type::support || (utype == military::unit_type::special && sup > 0.0f)) ? std::max(1.0f, sup) : 1.0f;
			a_str += (atk + def) * sup_mult * reg.get_regiment().get_strength() * reg.get_regiment().get_org();
		}
		attacker_str += a_str;
		if(nation) {
			attacker_tactic += a_str * state.world.nation_get_modifier_values(nation, sys::national_mod_offsets::military_tactics);
		}
	}

	if(attacker_str > 0.f) {
		attacker_tactic = attacker_tactic / attacker_str;
	} else {
		attacker_tactic = 0.f;
	}

	float dig_in = 0.f;
	float defender_str = 0.f;
	float defender_tactic = 0.f;
	for(auto a : defender) {
		auto nation = military::tech_nation_for_army(state, a);
		float a_str = 0.f;
		for(const auto reg : state.world.army_get_army_membership(a)) {
			auto type = reg.get_regiment().get_type();
			auto stats = nation ? state.world.nation_get_unit_stats(nation, type) : military::unit_definition{};
			auto atk = (nation && stats.discipline_or_evasion > 0.0f) ? stats.attack_or_gun_power : state.military_definitions.unit_base_definitions[type].attack_or_gun_power;
			auto def = (nation && stats.discipline_or_evasion > 0.0f) ? stats.defence_or_hull : state.military_definitions.unit_base_definitions[type].defence_or_hull;
			auto sup = (nation && stats.discipline_or_evasion > 0.0f) ? stats.support : state.military_definitions.unit_base_definitions[type].support;
			auto utype = state.military_definitions.unit_base_definitions[type].type;
			float sup_mult = (utype == military::unit_type::support || (utype == military::unit_type::special && sup > 0.0f)) ? std::max(1.0f, sup) : 1.0f;
			a_str += (atk + def) * sup_mult * reg.get_regiment().get_strength() * reg.get_regiment().get_org();
		}
		defender_str += a_str;
		if(nation) {
			defender_tactic += a_str * state.world.nation_get_modifier_values(nation, sys::national_mod_offsets::military_tactics);
		}
		dig_in += state.world.army_get_dig_in(a) * a_str;
	}

	if(defender_str > 0.f) {
		defender_tactic = defender_tactic / defender_str;
		dig_in = dig_in / defender_str;
	} else {
		defender_tactic = 0.f;
		dig_in = 0.f;
	}


	auto attack_from = state.world.army_get_location_from_army_location(attacker[0]);
	auto attack_toward = state.world.army_get_location_from_army_location(defender[0]);
	auto adj = state.world.get_province_adjacency_by_province_pair(attack_toward, attack_from);
	auto crossing = military::crossing_type::none;
	if(adj) {
		crossing = military::get_crossing_type(state, adj);
	}

	dcon::leader_id a_lid;
	float a_score = -999.f;
	for(const auto a : attacker) {
		auto candidate = state.world.army_get_general_from_army_leadership(a);
		if(!candidate) {
			continue;
		}
		auto score = military::get_leader_battle_select_score(state, candidate, true);
		if(score > a_score) {
			a_lid = candidate;
			a_score = score;
		}
	}

	dcon::leader_id d_lid;
	float d_score = -999.f;
	for(const auto a : defender) {
		auto candidate = state.world.army_get_general_from_army_leadership(a);
		if(!candidate) {
			continue;
		}
		auto score = military::get_leader_battle_select_score(state, candidate, false);
		if(score > d_score) {
			d_lid = candidate;
			d_score = score;
		}
	}

	auto a_back = military::get_leader_background_wrapper(state, a_lid);
	auto a_pers = military::get_leader_personality_wrapper(state, a_lid);
	float attacker_leader_str = state.world.leader_trait_get_attack(a_back) + state.world.leader_trait_get_attack(a_pers);

	auto d_back = military::get_leader_background_wrapper(state, d_lid);
	auto d_pers = military::get_leader_personality_wrapper(state, d_lid);
	float defender_leader_str = state.world.leader_trait_get_defense(d_back) + state.world.leader_trait_get_defense(d_pers);

	float attacker_prestige = a_lid ? state.world.leader_get_prestige(a_lid) : 0.0f;
	float defender_prestige = d_lid ? state.world.leader_get_prestige(d_lid) : 0.0f;

	float probability = predictions::battle_win_probability(
		attacker_str, defender_str,
		dig_in,
		(float)(crossing),
		state.world.province_get_modifier_values(attack_toward, sys::provincial_mod_offsets::defense),
		attacker_tactic,
		defender_tactic,
		attacker_leader_str,
		defender_leader_str,
		attacker_prestige,
		defender_prestige
	);

	return probability;
}

float estimate_enemy_defensive_force(sys::state& state, dcon::province_id target, dcon::nation_id by) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	float strength_total = 0.f;
	if(state.world.nation_get_is_at_war(by)) {
		for(auto ar : state.world.in_army) {
			if(ar.get_is_retreating()
			|| ar.get_battle_from_army_battle_participation()
			|| ar.get_controller_from_army_control() == by)
				continue;
			auto loc = ar.get_location_from_army_location();
			auto sdist = province::sorting_distance(state, loc, target);
			if(sdist < state.defines.alice_ai_threat_radius) {
				auto other_nation = ar.get_controller_from_army_control();
				if(!other_nation || military::are_at_war(state, other_nation, by)) {
					strength_total += estimate_army_defensive_strength(state, ar);
				}
			}
		}
	} else { // not at war -- rebel fighting
		for(auto ar : state.world.province_get_army_location(target)) {
			auto other_nation = ar.get_army().get_controller_from_army_control();
			if(!other_nation) {
				strength_total += estimate_army_defensive_strength(state, ar.get_army());
			}
		}
	}
	return state.defines.alice_ai_threat_overestimate * strength_total;
}

void get_enemy_defensive_force(sys::state& state, dcon::province_id target, dcon::nation_id by, std::vector<dcon::army_id>& result) {
	if(state.cheat_data.disable_ai) {
		return;
	}
	if(state.world.nation_get_is_at_war(by)) {
		for(auto ar : state.world.in_army) {
			if(ar.get_is_retreating()
			|| ar.get_battle_from_army_battle_participation()
			|| ar.get_controller_from_army_control() == by)
				continue;
			auto loc = ar.get_location_from_army_location();
			auto sdist = province::sorting_distance(state, loc, target);
			if(sdist < state.defines.alice_ai_threat_radius) {
				auto other_nation = ar.get_controller_from_army_control();
				if(!other_nation || military::are_at_war(state, other_nation, by)) {
					result.push_back(ar);
				}
			}
		}
	} else { // not at war -- rebel fighting
		for(auto ar : state.world.province_get_army_location(target)) {
			auto other_nation = ar.get_army().get_controller_from_army_control();
			if(!other_nation) {
				result.push_back(ar.get_army());
			}
		}
	}
	return;
}

namespace ai_battle_eval {

inline constexpr float combat_mod_table[19] = {
	0.0f, 0.02f, 0.04f, 0.06f, 0.08f, 0.10f, 0.12f, 0.16f, 0.20f, 0.25f, 0.30f,
	0.35f, 0.40f, 0.45f, 0.50f, 0.60f, 0.70f, 0.80f, 0.90f
};

inline float expected_roll_modifier(int32_t net_mod) {
	float sum = 0.0f;
	for(int32_t d = 0; d <= 9; ++d) {
		sum += combat_mod_table[std::clamp(d + net_mod + 3, 0, 18)];
	}
	return sum * 0.1f;
}

struct side_summary {
	float front_eff = 0.0f;
	float back_eff = 0.0f;
	float total_str = 0.0f;
	float total_org_pool = 0.0f;
	float front_org_pool = 0.0f;
	float front_str_pool = 0.0f;
	float front_atk_sum = 0.0f;
	float front_def_sum = 0.0f;
	float back_atk_sup_sum = 0.0f;
	float back_def_sup_sum = 0.0f;
	float recon_str = 0.0f;
	float max_recon = 0.0f;
	float siege_str = 0.0f;
	float max_siege = 0.0f;
	float dig_in_weighted = 0.0f;
	float tactics_weighted = 0.0f;
	float disc_weighted = 0.0f;
	float exp_weighted = 0.0f;
	float max_org_weighted = 0.0f;
	int32_t min_base_cw = int32_t(military::max_combat_width);
	dcon::nation_id lead_nation;
	float lead_nation_str = -1.0f;
	dcon::leader_id best_leader;
	float best_leader_score = -999.0f;
	bool has_gas_attack = false;
	bool has_gas_defense = false;
};

struct eval_result {
	float win_ratio = 1.0f;           // > 1.0 attacker favored, < 1.0 defender favored
	float casualty_ratio = 1.0f;      // attacker daily str loss / defender daily str loss (lower is better for attacker)
	int32_t combat_width = 10;
	float attacker_front_eff = 0.0f;
	float defender_front_eff = 0.0f;
	float attacker_back_eff = 0.0f;
	float defender_back_eff = 0.0f;
};

inline side_summary summarize_side(sys::state& state, std::vector<dcon::army_id> const& armies, bool is_attacker) {
	side_summary s{};
	for(auto a : armies) {
		if(!state.world.army_is_valid(a))
			continue;
		auto ctrl = state.world.army_get_controller_from_army_control(a);
		auto tech_n = military::tech_nation_for_army(state, a);
		int32_t army_cw = ai_front_helpers::nation_base_cw(state, ctrl ? ctrl : tech_n);
		s.min_base_cw = std::min(s.min_base_cw, army_cw);

		if(auto gen = state.world.army_get_general_from_army_leadership(a); gen) {
			float lscore = military::get_leader_battle_select_score(state, gen, is_attacker);
			if(lscore > s.best_leader_score) {
				s.best_leader_score = lscore;
				s.best_leader = gen;
			}
		}

		float army_dig = float(state.world.army_get_dig_in(a));
		float nat_tactics = tech_n ? (state.defines.base_military_tactics + state.world.nation_get_modifier_values(tech_n, sys::national_mod_offsets::military_tactics)) : state.defines.base_military_tactics;
		float nat_org_mod = tech_n ? (1.0f + state.world.nation_get_modifier_values(tech_n, sys::national_mod_offsets::land_organisation)) : 1.0f;

		float army_str = 0.0f;
		for(auto r : state.world.army_get_army_membership(a)) {
			auto reg = r.get_regiment();
			float str = reg.get_strength();
			float org = reg.get_org();
			if(str <= 0.01f)
				continue;
			auto uid = reg.get_type();
			auto& bdef = state.military_definitions.unit_base_definitions[uid];
			auto stats = tech_n ? state.world.nation_get_unit_stats(tech_n, uid) : military::unit_definition{};
			auto& d = (tech_n && stats.discipline_or_evasion > 0.0f) ? stats : bdef;

			bool back = (bdef.type == military::unit_type::support) || (bdef.type == military::unit_type::special && d.support > 0.0f);
			float eff = str * std::clamp(org, 0.05f, 1.0f);
			float max_org = std::max(1.0f, d.default_organisation * nat_org_mod);
			float disc = std::max(0.1f, d.discipline_or_evasion);
			float exp_val = reg.get_experience();

			army_str += str;
			s.total_str += str;
			s.total_org_pool += str * org * max_org;
			s.dig_in_weighted += army_dig * str;
			s.tactics_weighted += std::max(0.1f, nat_tactics) * str;
			s.disc_weighted += disc * str;
			s.exp_weighted += exp_val * str;
			s.max_org_weighted += max_org * str;

			if(d.reconnaissance_or_fire_range > 0.0f) {
				s.recon_str += str;
				s.max_recon = std::max(s.max_recon, d.reconnaissance_or_fire_range);
			}
			if(d.siege_or_torpedo_attack > 0.0f) {
				s.siege_str += str;
				s.max_siege = std::max(s.max_siege, d.siege_or_torpedo_attack);
			}

			float atk_term = str * (d.attack_or_gun_power * 0.1f + 1.0f);
			float def_term = str * (d.defence_or_hull * 0.1f + 1.0f);
			if(back) {
				s.back_eff += eff;
				s.back_atk_sup_sum += atk_term * std::max(0.1f, d.support);
				s.back_def_sup_sum += def_term * std::max(0.1f, d.support);
			} else {
				s.front_eff += eff;
				s.front_org_pool += str * org * max_org;
				s.front_str_pool += str;
				s.front_atk_sum += atk_term;
				s.front_def_sum += def_term;
			}
		}

		if(ctrl && army_str > s.lead_nation_str) {
			s.lead_nation_str = army_str;
			s.lead_nation = ctrl;
		}
		if(tech_n) {
			if(state.world.nation_get_has_gas_attack(tech_n)) s.has_gas_attack = true;
			if(state.world.nation_get_has_gas_defense(tech_n)) s.has_gas_defense = true;
		}
	}
	return s;
}

// Analytical battle outcome evaluator directly reflecting Project Alice formulas in military.cpp:7212-7530
inline eval_result evaluate_battle_outcome(
	sys::state& state,
	std::vector<dcon::army_id> const& attackers,
	std::vector<dcon::army_id> const& defenders,
	dcon::province_id battle_prov,
	military::crossing_type crossing
) {
	eval_result res{};
	auto att = summarize_side(state, attackers, true);
	auto def = summarize_side(state, defenders, false);

	res.attacker_front_eff = att.front_eff;
	res.defender_front_eff = def.front_eff;
	res.attacker_back_eff = att.back_eff;
	res.defender_back_eff = def.back_eff;

	if(def.total_str <= 0.01f) {
		res.win_ratio = att.total_str > 0.01f ? 100.0f : 1.0f;
		res.casualty_ratio = 0.01f;
		return res;
	}
	if(att.total_str <= 0.01f) {
		res.win_ratio = 0.0f;
		res.casualty_ratio = 100.0f;
		return res;
	}

	int32_t base_cw = std::min(att.min_base_cw, def.min_base_cw);
	float prov_cw_mod = battle_prov ? (state.world.province_get_modifier_values(battle_prov, sys::provincial_mod_offsets::combat_width) + 1.0f) : 1.0f;
	int32_t cw = std::clamp(int32_t(float(base_cw) * prov_cw_mod), int32_t(military::min_combat_width), int32_t(military::max_combat_width));
	res.combat_width = cw;
	float fcw = float(cw);

	// Leader attack/defense bonuses (automatically includes no-leader penalty via wrappers)
	auto a_bg = military::get_leader_background_wrapper(state, att.best_leader);
	auto a_per = military::get_leader_personality_wrapper(state, att.best_leader);
	int32_t att_gen = int32_t(state.world.leader_trait_get_attack(a_bg) + state.world.leader_trait_get_attack(a_per));

	auto d_bg = military::get_leader_background_wrapper(state, def.best_leader);
	auto d_per = military::get_leader_personality_wrapper(state, def.best_leader);
	int32_t def_gen = int32_t(state.world.leader_trait_get_defense(d_bg) + state.world.leader_trait_get_defense(d_per));

	bool att_gas = att.has_gas_attack && !def.has_gas_defense;
	bool def_gas = def.has_gas_attack && !att.has_gas_defense;

	int32_t terrain_def = battle_prov ? int32_t(state.world.province_get_modifier_values(battle_prov, sys::provincial_mod_offsets::defense)) : 0;
	int32_t cross_pen = (crossing == military::crossing_type::sea) ? military::strait_crossing_modifier
		: (crossing == military::crossing_type::river ? military::river_crossing_modifier : 0);

	// Effective dig-in reduced by attacker recon (military.cpp:7174-7223)
	float avg_dig = def.dig_in_weighted / def.total_str;
	float eff_recon = att.max_recon * std::clamp((att.recon_str / att.total_str) / std::max(0.01f, state.defines.recon_unit_ratio), 0.0f, 1.0f);
	int32_t eff_dig = int32_t(std::floor(avg_dig / (eff_recon + 1.0f)));

	// Effective fort level reduced by attacker siege units (military.cpp:7314-7318, 7488-7490)
	float fort_mod = 1.0f;
	if(battle_prov) {
		auto loc_ctrl = state.world.province_get_nation_from_province_control(battle_prov);
		bool fort_helps_def = (!att.lead_nation && loc_ctrl)
			|| (att.lead_nation && (!loc_ctrl || military::are_at_war(state, att.lead_nation, loc_ctrl)));
		if(fort_helps_def) {
			float raw_fort = state.world.province_get_modifier_values(battle_prov, sys::provincial_mod_offsets::fort_level);
			float siege_red = att.max_siege * std::min(att.siege_str / att.total_str, state.defines.engineer_unit_ratio) / std::max(0.01f, state.defines.engineer_unit_ratio);
			int32_t eff_fort = std::clamp(int32_t(raw_fort) - int32_t(siege_red), 0, 10);
			fort_mod = 1.0f + state.defines.alice_fort_mil_tactics_discipline_per_level * float(eff_fort);
		}
	}

	int32_t att_net_mod = att_gen + (att_gas ? int32_t(state.defines.gas_attack_modifier) : 0) - terrain_def - eff_dig + cross_pen;
	int32_t def_net_mod = def_gen + (def_gas ? int32_t(state.defines.gas_attack_modifier) : 0);
	float att_roll_mult = std::max(0.01f, expected_roll_modifier(att_net_mod));
	float def_roll_mult = std::max(0.01f, expected_roll_modifier(def_net_mod));

	// Active frontline & backline slots capped by combat_width; penalize exposed backline when frontline < cw!
	auto compute_firepower = [&](side_summary const& s, bool is_att) -> std::pair<float, float> {
		float active_front = std::min(fcw, s.front_eff);
		float exposed_back = std::min(std::max(0.0f, fcw - active_front), s.back_eff);
		float safe_back = std::min(fcw, std::max(0.0f, s.back_eff - exposed_back));

		float avg_front_stat = s.front_eff > 0.01f ? ((is_att ? s.front_atk_sum : s.front_def_sum) / s.front_eff) : 1.0f;
		float avg_back_stat = s.back_eff > 0.01f ? ((is_att ? s.back_atk_sup_sum : s.back_def_sup_sum) / s.back_eff) : 1.0f;

		// Exposed artillery in front line loses support multiplier & takes full frontline losses
		float firepower = active_front * avg_front_stat + safe_back * avg_back_stat + exposed_back * (avg_back_stat * 0.45f);
		float expose_penalty = (exposed_back > 0.5f) ? (1.0f / (1.0f + exposed_back / std::max(1.0f, fcw))) : 1.0f;
		return { firepower, expose_penalty };
	};

	auto [att_fp_raw, att_exp_pen] = compute_firepower(att, true);
	auto [def_fp_raw, def_exp_pen] = compute_firepower(def, false);

	float att_fp = att_fp_raw * att_roll_mult;
	float def_fp = def_fp_raw * def_roll_mult;

	float att_tactics = att.tactics_weighted / att.total_str;
	float def_tactics = def.tactics_weighted / def.total_str;
	float att_disc = att.disc_weighted / att.total_str;
	float def_disc = def.disc_weighted / def.total_str;
	float att_exp = att.exp_weighted / att.total_str;
	float def_exp = def.exp_weighted / def.total_str;
	float att_max_org = att.max_org_weighted / att.total_str;
	float def_max_org = def.max_org_weighted / def.total_str;

	// Daily org & str damage rates matching military.cpp:7256, 7291
	float att_org_dps_vs_def = att_fp / std::max(0.1f, fort_mod * def_disc * (1.0f + def_exp) * def_max_org);
	float def_org_dps_vs_att = def_fp / std::max(0.1f, att_disc * (1.0f + att_exp) * att_max_org);

	float att_str_dps_vs_def = att_fp / std::max(0.1f, fort_mod * def_tactics * (1.0f + def_exp));
	float def_str_dps_vs_att = def_fp / std::max(0.1f, att_tactics * (1.0f + att_exp));

	// Staying power accounts for frontline reserve depth (reserves recover org daily in Alice!) with soft diminishing returns beyond 4x CW
	auto staying_power = [&](side_summary const& s, float expose_pen) -> float {
		float front_depth = std::max(0.1f, s.front_str_pool + s.back_eff * 0.20f);
		float capped_depth = front_depth <= fcw * 4.0f
			? front_depth
			: (fcw * 4.0f + std::sqrt(front_depth - fcw * 4.0f) * std::sqrt(fcw * 2.0f));
		float avg_org_frac = s.total_str > 0.01f ? std::clamp(s.total_org_pool / std::max(1.0f, s.max_org_weighted), 0.1f, 1.0f) : 0.5f;
		return capped_depth * (0.4f + 0.6f * avg_org_frac) * expose_pen;
	};

	float att_stay = staying_power(att, att_exp_pen);
	float def_stay = staying_power(def, def_exp_pen);

	// Blend org break rate (65%) and manpower attrition rate (35%) since reserves cycle back into combat
	float att_break_rate = def_org_dps_vs_att * 0.65f + def_str_dps_vs_att * 0.35f;
	float def_break_rate = att_org_dps_vs_def * 0.65f + att_str_dps_vs_def * 0.35f;

	float att_days = att_stay / std::max(1e-4f, att_break_rate);
	float def_days = def_stay / std::max(1e-4f, def_break_rate);

	res.win_ratio = att_days / std::max(1e-4f, def_days);
	res.casualty_ratio = def_str_dps_vs_att / std::max(1e-4f, att_str_dps_vs_def);
	return res;
}

inline void collect_enemy_defenders_at(sys::state& state, dcon::province_id target, dcon::nation_id by, std::vector<dcon::army_id>& out, bool include_adjacent_1hop) {
	out.clear();
	bool at_war = state.world.nation_get_is_at_war(by);
	auto add_from_prov = [&](dcon::province_id p) {
		for(auto ar : state.world.province_get_army_location(p)) {
			auto army = ar.get_army();
			if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			if(army.get_battle_from_army_battle_participation())
				continue;
			auto ctrl = army.get_controller_from_army_control();
			if(ctrl == by)
				continue;
			if(!ctrl) {
				out.push_back(army.id);
			} else if(at_war && military::are_at_war(state, by, ctrl)) {
				out.push_back(army.id);
			}
		}
	};

	add_from_prov(target);
	if(include_adjacent_1hop && at_war) {
		for(auto padj : state.world.province_get_province_adjacency(target)) {
			if(province::is_adjacency_impassable(state, by, padj.id))
				continue;
			auto other = padj.get_connected_provinces(0) == target ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			if(!province::is_sea(state, other)) {
				add_from_prov(other);
			}
		}
	}
}

} // namespace ai_battle_eval

void assign_targets(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_is_valid(n) || state.world.nation_get_owned_province_count(n) == 0)
		return;

	bool is_at_war = state.world.nation_get_is_at_war(n);
	uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
	int32_t const land_count = int32_t(std::min(state.world.province_size(), first_sea));
	if(land_count <= 0)
		return;

	// Build passable cross-border theater map (own + allied + occupied + war-enemy land — NEVER split at national borders!)
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n);

	struct ready_stack {
		dcon::army_id id;
		dcon::province_id loc;
		uint16_t theater_id = 0;
		int32_t regs = 0;
		int32_t dig_in = 0;
		bool is_core = false;
		bool assigned = false;
	};
	std::vector<ready_stack> ready_stacks;
	std::vector<uint8_t> already_targeted(size_t(land_count), 0);
	bool active_naval_invasion = false;

	for(auto ar : state.world.nation_get_army_control(n)) {
		auto army = ar.get_army();
		if(!unit_on_ai_control(state, army))
			continue;
		army_activity activity = army_activity(army.get_ai_activity());
		if(activity == army_activity::attack_transport || activity == army_activity::transport_attack) {
			active_naval_invasion = true;
		}
		if(activity == army_activity::attacking
			|| activity == army_activity::attack_gathered
			|| activity == army_activity::attack_transport
			|| activity == army_activity::transport_attack) {
			auto tp = army.get_ai_province().id;
			if(tp && tp.index() < land_count) {
				already_targeted[tp.index()] = 1;
			}
		}
		if(army.get_battle_from_army_battle_participation()
			|| army.get_navy_from_army_transport()
			|| army.get_black_flag()
			|| army.get_arrival_time()
			|| army.get_is_retreating()
			|| activity != army_activity::on_guard
			|| !army_ready_for_battle(state, n, army)) {
			continue;
		}
		auto loc = army.get_location_from_army_location().id;
		if(!loc || province::is_sea(state, loc))
			continue;
		auto regs = army.get_army_membership();
		int32_t rcount = int32_t(regs.end() - regs.begin());
		if(rcount == 0)
			continue;
		ready_stacks.push_back(ready_stack{
			army.id,
			loc,
			tmap.theater_of(loc),
			rcount,
			int32_t(army.get_dig_in()),
			ai_front_helpers::army_has_backline_core(state, n, army.id),
			false
		});
	}

	if(ready_stacks.empty())
		return;

	std::vector<dcon::nation_id> war_allies;
	std::vector<dcon::nation_id> war_enemies;
	if(is_at_war) {
		ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
	} else {
		war_allies.push_back(n);
	}

	bool const time_against_us = is_at_war ? ai_front_helpers::is_time_working_against_us(state, n) : true;
	bool const has_naval_adv = is_at_war && naval_advantage(state, n);
	dcon::navy_id const free_transport = has_naval_adv ? find_transport_fleet(state, n) : dcon::navy_id{};
	int32_t const free_tcap = free_transport ? military::transport_capacity(state, free_transport) : 0;

	// Dynamic per-province pinning tracker (prevents stripping WWI 1st-line sectors; zero pinning in Austerlitz!)
	ai_front_helpers::front_pin_tracker pin_tracker;
	if(is_at_war) {
		pin_tracker.init_for_nation(state, n);
	}

	// Cache theater mode (Austerlitz vs WWI) and war stance per walkable theater_id
	std::vector<std::pair<uint16_t, ai_front_helpers::theater_summary>> theater_cache;
	auto get_theater = [&](uint16_t tid) -> ai_front_helpers::theater_summary const& {
		for(auto const& kv : theater_cache) {
			if(kv.first == tid)
				return kv.second;
		}
		ai_front_helpers::theater_summary ts{};
		if(is_at_war && tid != 0) {
			ts = ai_front_helpers::evaluate_theater_summary(state, n, tmap, tid, war_allies, war_enemies, time_against_us);
		} else {
			ts.continuous_front = false;
			ts.stance = ai_front_helpers::war_stance::active_offense;
		}
		theater_cache.push_back({ tid, ts });
		return theater_cache.back().second;
	};

	// Precompute global force ratio for overseas expeditions where target_p is unreachable by land
	float global_friendly_eff = 0.0f;
	float global_enemy_eff = 0.0f;
	if(is_at_war) {
		for(auto a_nat : war_allies) {
			global_friendly_eff += estimate_strength(state, a_nat);
		}
		for(auto e_nat : war_enemies) {
			global_enemy_eff += estimate_strength(state, e_nat);
		}
	}
	float const global_ratio = global_friendly_eff / std::max(1.0f, global_enemy_eff);

	// Mark our wargoals, enemy-held enemy wargoals (to stop enemy ticking WS), and strategic objective anchors
	std::vector<uint8_t> is_our_wargoal(size_t(land_count), 0);
	std::vector<uint8_t> is_enemy_held_wargoal(size_t(land_count), 0);
	std::vector<uint8_t> seen_target(size_t(land_count), 0);
	std::vector<dcon::province_id> strategic_goals;

	auto add_strategic_goal = [&](dcon::province_id gp) {
		if(!gp || gp.index() >= land_count)
			return;
		if(std::find(strategic_goals.begin(), strategic_goals.end(), gp) == strategic_goals.end()) {
			strategic_goals.push_back(gp);
		}
	};

	if(is_at_war) {
		for(auto wp : state.world.nation_get_war_participant(n)) {
			auto war = wp.get_war();
			bool we_attack = wp.get_is_attacker();
			for(auto wg_att : war.get_wargoals_attached()) {
				auto wg = wg_att.get_wargoal();
				bool wg_ours = (military::is_attacker(state, war, wg.get_added_by()) == we_attack);
				if(auto sd = wg.get_associated_state(); sd) {
					for(auto m : sd.get_abstract_state_membership()) {
						auto pid = m.get_province().id;
						if(!pid || pid.index() >= land_count || m.get_province().get_nation_from_province_ownership() != wg.get_target_nation())
							continue;
						if(wg_ours) {
							is_our_wargoal[pid.index()] = 1;
							add_strategic_goal(pid);
						} else {
							auto pctrl = m.get_province().get_nation_from_province_control();
							if((pctrl && military::are_at_war(state, n, pctrl)) || ai_front_helpers::province_has_enemy_army(state, n, pid)) {
								is_enemy_held_wargoal[pid.index()] = 1;
								add_strategic_goal(pid);
							}
						}
					}
				} else if(auto tn = wg.get_target_nation(); tn) {
					auto tcap = tn.get_capital().id;
					if(tcap && tcap.index() < land_count) {
						if(wg_ours) {
							is_our_wargoal[tcap.index()] = 1;
							add_strategic_goal(tcap);
						} else {
							auto pctrl = state.world.province_get_nation_from_province_control(tcap);
							if((pctrl && military::are_at_war(state, n, pctrl)) || ai_front_helpers::province_has_enemy_army(state, n, tcap)) {
								is_enemy_held_wargoal[tcap.index()] = 1;
								add_strategic_goal(tcap);
							}
						}
					}
				}
			}
		}
		for(auto e_nat : war_enemies) {
			add_strategic_goal(state.world.nation_get_capital(e_nat));
		}
	}

	auto has_stack_in_theater = [&](uint16_t tid) -> bool {
		if(tid == 0)
			return false;
		for(auto const& rs : ready_stacks) {
			if(rs.theater_id == tid)
				return true;
		}
		return false;
	};

	struct army_target {
		dcon::province_id location;
		uint16_t theater_id = 0;
		float score = 0.0f;
		float enemy_avg_dig = 0.0f;
		bool adjacent_to_friendly = false;
		bool is_rebel_or_liberation = false;
		bool is_besieged_home = false;
		bool is_coastal = false;
		bool has_same_theater_stack = false;
	};
	std::vector<army_target> potential_targets;

	auto add_target = [&](dcon::province_id p, bool liberation, bool besieged_home) {
		if(!p || p.index() >= land_count || seen_target[p.index()] || already_targeted[p.index()])
			return;
		seen_target[p.index()] = 1;

		// Active battles are managed daily by manage_active_battles (do not treat an active battle tile as an empty target!)
		auto local_battles = state.world.province_get_land_battle_location(p);
		if(local_battles.begin() != local_battles.end())
			return;

		uint16_t tid = tmap.theater_of(p);
		bool same_theater = has_stack_in_theater(tid);
		bool coastal = state.world.province_get_is_coast(p);

		// Respect Project Alice's 1-province logistics rule: check if p borders supplied friendly/allied territory
		bool adj_friendly = false;
		for(auto padj : state.world.province_get_province_adjacency(p)) {
			if(province::is_adjacency_impassable(state, n, padj.id))
				continue;
			dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
			if(other && other.index() < land_count && province::has_safe_access_to_province(state, n, other)) {
				if(logistics::calculate_supply_throughput_in_province(state, other, n) > 0.0f) {
					adj_friendly = true;
					break;
				}
			}
		}

		// Filter unreachable targets early:
		// Land advance requires same walkable theater (USA<->Canada, RUS<->Krakow/Austria!) + depth-1 supply corridor (or liberation).
		// Overseas naval invasion requires coastal province when no land route exists.
		bool land_viable = same_theater && (adj_friendly || liberation);
		bool naval_viable = coastal && is_at_war && has_naval_adv && free_tcap > 0 && !active_naval_invasion && (!same_theater || !adj_friendly);
		if(!land_viable && !naval_viable)
			return;

		potential_targets.push_back(army_target{
			p,
			tid,
			0.0f,
			0.0f,
			adj_friendly,
			liberation,
			besieged_home,
			coastal,
			same_theater
		});
	};

	// 1. Own provinces occupied by rebels/enemies OR actively besieged by a hostile army
	for(auto o : state.world.nation_get_province_ownership(n)) {
		auto p = o.get_province();
		auto ctrl = p.get_nation_from_province_control();
		bool occupied = !ctrl || (ctrl != n && military::are_at_war(state, n, ctrl));
		bool has_hostile = ai_front_helpers::province_has_enemy_army(state, n, p.id);
		if(occupied || has_hostile) {
			add_target(p.id, true, !occupied && has_hostile);
		}
	}

	// 2. War-ally occupied or actively besieged provinces (in theaters where we have armies or naval reach)
	if(is_at_war) {
		for(auto a_nat : war_allies) {
			if(a_nat == n)
				continue;
			for(auto o : state.world.nation_get_province_ownership(a_nat)) {
				auto p = o.get_province();
				auto ctrl = p.get_nation_from_province_control();
				bool occupied = !ctrl || military::are_at_war(state, n, ctrl);
				bool has_hostile = ai_front_helpers::province_has_enemy_army(state, n, p.id);
				if(occupied || has_hostile) {
					add_target(p.id, true, false);
				}
			}
		}
		// 3. Enemy-controlled provinces (and rebel-occupied enemy provinces)
		for(auto e_nat : war_enemies) {
			for(auto o : state.world.nation_get_province_control(e_nat)) {
				add_target(o.get_province().id, false, false);
			}
			for(auto o : state.world.nation_get_province_ownership(e_nat)) {
				if(!o.get_province().get_nation_from_province_control()) {
					add_target(o.get_province().id, false, false);
				}
			}
		}
	}

	// 4. Subject rebel provinces
	for(const auto ovr : state.world.nation_get_overlord_as_ruler(n)) {
		auto w = ovr.get_subject();
		for(auto o : state.world.nation_get_province_ownership(w)) {
			if(!o.get_province().get_nation_from_province_control() || military::rebel_army_in_province(state, o.get_province())) {
				add_target(o.get_province().id, true, false);
			}
		}
	}

	if(potential_targets.empty())
		return;

	// Unified Value / Cost target scoring (real warscore VP, wargoal, stockpile, dig_in==0 window / real km march+boarding days)
	auto our_cap = state.world.nation_get_capital(n);
	float const land_km_per_day = std::max(5.0f, 4.0f * state.defines.alice_army_marching_hours_per_day);
	float const sea_km_per_day = std::max(40.0f, 10.0f * state.defines.alice_navy_sailing_hours_per_day);
	for(auto& pt : potential_targets) {
		auto p = pt.location;
		auto owner = state.world.province_get_nation_from_province_ownership(p);
		int32_t vp = owner ? military::province_point_cost(state, p, owner) : 1;

		float value = 2.0f + float(vp) * 4.0f;
		if(is_our_wargoal[p.index()] || is_enemy_held_wargoal[p.index()]) {
			value += 62.0f + float(vp) * 2.5f;
		}
		if(p == our_cap) {
			value += 55.0f;
		} else if(owner && owner != n && state.world.nation_get_capital(owner) == p) {
			value += 28.0f;
		}
		// Capturing or defending a state capital government stockpile
		value += ai_front_helpers::province_stockpile_importance(state, p) * 16.0f;

		if(pt.is_rebel_or_liberation) {
			if(owner == n) {
				value += (pt.is_besieged_home ? 16.0f : 12.0f) + float(vp) * 2.5f;
			} else {
				value += 8.0f + float(vp) * 1.5f;
			}
		}

		// Directional pull along depth-1 land corridor toward strategic goals in the same walkable theater
		if(pt.adjacent_to_friendly && pt.theater_id != 0 && !strategic_goals.empty()) {
			float best_gdist_km = 1e9f;
			for(auto gp : strategic_goals) {
				if(tmap.theater_of(gp) == pt.theater_id) {
					float gd = province::direct_distance_km(state, p, gp);
					if(gd < best_gdist_km)
						best_gdist_km = gd;
				}
			}
			if(best_gdist_km < 1e8f) {
				value += 14.0f / (1.0f + best_gdist_km / 250.0f);
			}
		}

		// Check direct enemy army dig-in on target_p: catching an unentrenched besieging/moving army is a prime window!
		float dir_enemy_str = 0.0f;
		float dir_dig_sum = 0.0f;
		for(auto ar : state.world.province_get_army_location(p)) {
			auto army = ar.get_army();
			if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			auto ec = army.get_controller_from_army_control();
			if(!ec || (is_at_war && military::are_at_war(state, n, ec))) {
				float astr = float(army.get_army_membership().end() - army.get_army_membership().begin());
				dir_enemy_str += astr;
				dir_dig_sum += astr * float(army.get_dig_in());
			}
		}
		if(dir_enemy_str > 0.0f) {
			pt.enemy_avg_dig = dir_dig_sum / dir_enemy_str;
			if(pt.enemy_avg_dig < 0.5f) {
				value *= 1.35f;
			}
		}

		// Real time cost: march days in km (same walkable theater) vs boarding + sea transit overhead (overseas)
		float min_km = 1e9f;
		for(auto const& rs : ready_stacks) {
			if(pt.has_same_theater_stack && rs.theater_id != pt.theater_id)
				continue;
			float km = province::direct_distance_km(state, rs.loc, p);
			if(km < min_km)
				min_km = km;
		}
		if(min_km > 1e8f)
			min_km = 2000.0f;
		float est_days = (pt.has_same_theater_stack && (pt.adjacent_to_friendly || pt.is_rebel_or_liberation))
			? std::max(3.0f, min_km / land_km_per_day)
			: (28.0f + min_km / sea_km_per_day);

		float terrain_def = std::max(0.0f, state.world.province_get_modifier_values(p, sys::provincial_mod_offsets::defense));
		float fort_lvl = float(state.world.province_get_building_level(p, uint8_t(economy::province_building_type::fort)));
		float def_cost = 1.0f + terrain_def * 0.20f + fort_lvl * 0.14f;

		pt.score = value / (est_days * def_cost);
	}

	std::sort(potential_targets.begin(), potential_targets.end(), [](army_target const& a, army_target const& b) {
		if(a.score != b.score)
			return a.score > b.score;
		return a.location.index() < b.location.index();
	});

	auto const psize = potential_targets.size();
	std::vector<dcon::army_id> direct_defenders;
	std::vector<dcon::army_id> chosen_attackers;
	std::vector<size_t> chosen_indices;
	dcon::province_id const home_port = state.world.nation_get_ai_home_port(n);

	// Limit target evaluations per tick for fast CPU in massive world wars
	uint32_t evaluated_targets = 0;
	for(uint32_t i = 0; i < psize && evaluated_targets < 48; ++i) {
		auto const& pt = potential_targets[i];
		auto target_p = pt.location;
		if(!target_p)
			continue;
		++evaluated_targets;

		bool use_land = pt.has_same_theater_stack && (pt.adjacent_to_friendly || pt.is_rebel_or_liberation);
		bool use_naval = !use_land && pt.is_coastal && is_at_war && has_naval_adv && free_tcap > 0 && !active_naval_invasion;
		if(!use_land && !use_naval)
			continue;

		auto const target_theater = get_theater(pt.theater_id);
		bool is_wg = (is_our_wargoal[target_p.index()] != 0) || (is_enemy_held_wargoal[target_p.index()] != 0);
		auto target_owner = state.world.province_get_nation_from_province_ownership(target_p);
		bool is_enemy_cap = target_owner && target_owner != n && state.world.nation_get_capital(target_owner) == target_p;

		// Separate DIRECT defenders sitting on target_p from neighboring enemies (who arrive later & WITHOUT target_p's dig-in!)
		ai_battle_eval::collect_enemy_defenders_at(state, target_p, n, direct_defenders, false);
		bool has_direct_defenders = !direct_defenders.empty();
		float direct_def_regs = 0.0f;
		for(auto da : direct_defenders) {
			for(auto r : state.world.army_get_army_membership(da)) {
				direct_def_regs += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
			}
		}
		float nearby_enemy_regs = is_at_war
			? ai_front_helpers::adjacent_enemy_regs_excluding(state, n, target_p, dcon::province_id{})
			: 0.0f;
		// For amphibious invasions, check enemy land reaction forces within 800 km (e.g. USA's 60K in NY reacting to a landing on Washington!)
		float naval_threat_regs = use_naval
			? ai_front_helpers::estimate_naval_landing_threat(state, n, target_p)
			: 0.0f;
		if(use_naval && naval_threat_regs > nearby_enemy_regs) {
			nearby_enemy_regs = naval_threat_regs;
		}

		// Enforce Strategic War Stance + Theater Mode rules
		ai_front_helpers::war_stance stance = target_theater.stance;
		if(use_naval && !pt.has_same_theater_stack) {
			// Overseas expedition (e.g. UK vs Liberia, USA vs Japan/Philippines): driven by ticking WS pressure & global strength
			if(time_against_us || is_wg || pt.is_rebel_or_liberation) {
				stance = (global_ratio >= 1.15f) ? ai_front_helpers::war_stance::active_offense : ai_front_helpers::war_stance::cautious_offense;
			} else if(global_ratio >= 1.25f) {
				stance = ai_front_helpers::war_stance::active_offense;
			} else {
				stance = ai_front_helpers::war_stance::defensive;
			}
		}

		if(is_at_war && stance == ai_front_helpers::war_stance::defensive) {
			if(use_naval) {
				// Defensive nations only mount overseas landings for urgent wargoals or home liberation
				if(!is_wg && !pt.is_rebel_or_liberation)
					continue;
			} else {
				// Strategic defense (both WWI & Austerlitz):
				// ALWAYS allow liberating own/allied tiles, seizing UNDEFENDED enemy border tiles, or smashing unentrenched overextensions!
				bool free_border_grab = !has_direct_defenders && pt.adjacent_to_friendly;
				bool unentrenched_counter = has_direct_defenders && pt.enemy_avg_dig < 1.0f;
				if(!pt.is_rebel_or_liberation && !free_border_grab && !unentrenched_counter)
					continue;
			}
		}
		if(use_naval && pt.has_same_theater_stack) {
			// When we ALREADY have a land front in this walkable theater, only allow short-range coastal flanks on wargoal/capital, NEVER suicide cross-continent raids!
			if(stance != ai_front_helpers::war_stance::active_offense)
				continue;
			if(!is_wg && !is_enemy_cap)
				continue;
			if(home_port && province::direct_distance_km(state, home_port, target_p) > 1000.0f)
				continue;
		}
		// Quick early reject for suicidal overseas landings when transport fleet cannot even carry enough troops to survive nearby enemy reaction armies!
		if(use_naval && float(free_tcap) < (direct_def_regs + naval_threat_regs) * 0.75f && (direct_def_regs + naval_threat_regs) >= 4.0f) {
			continue;
		}

		// Find candidate unassigned stacks passing dynamic pinning (free 2nd-line reserves first!)
		struct cand {
			size_t idx;
			float dist_km;
			bool is_core;
			bool is_free_reserve;
		};
		std::vector<cand> cands;
		cands.reserve(ready_stacks.size());

		for(size_t s = 0; s < ready_stacks.size(); ++s) {
			auto const& rs = ready_stacks[s];
			if(rs.assigned)
				continue;
			if(use_land) {
				if(rs.theater_id != pt.theater_id || pt.theater_id == 0)
					continue;
			} else {
				// Naval invasion requires a safe coastal port in the stack's walkable theater and stack fitting in available transports
				if(!tmap.theater_has_safe_port(rs.theater_id))
					continue;
				if(rs.regs > free_tcap)
					continue;
			}

			auto const stack_th = get_theater(rs.theater_id);
			bool allow_screen_adv = use_land
				&& province::provinces_are_adjacent(state, rs.loc, target_p)
				&& (is_wg || !has_direct_defenders || stance == ai_front_helpers::war_stance::active_offense);
			if(is_at_war && !pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, stack_th.continuous_front, 0, 0, allow_screen_adv))
				continue;

			float d_km = province::direct_distance_km(state, rs.loc, target_p);
			// Avoid pulling distant armies from across the continent for minor land skirmishes
			if(is_at_war && use_land && target_theater.continuous_front && d_km > 700.0f && cands.size() >= 8)
				continue;

			float other_enemy = is_at_war
				? ai_front_helpers::adjacent_enemy_regs_excluding(state, n, rs.loc, target_p)
				: 0.0f;
			// Even in Austerlitz mode, never pull a stack facing a much larger enemy army to chase a small side raid!
			if(is_at_war && use_land && !stack_th.continuous_front && has_direct_defenders
				&& other_enemy > std::max(4.0f, direct_def_regs) * 1.35f) {
				continue;
			}

			cands.push_back(cand{ s, d_km, rs.is_core, other_enemy <= 0.1f });
		}
		if(cands.empty())
			continue;

		bool const need_artillery_punch = has_direct_defenders || (use_naval && naval_threat_regs >= 4.0f);
		std::sort(cands.begin(), cands.end(), [&](cand const& a, cand const& b) {
			if(a.is_free_reserve != b.is_free_reserve)
				return a.is_free_reserve > b.is_free_reserve;
			if(use_naval && need_artillery_punch && a.is_core != b.is_core)
				return a.is_core > b.is_core;
			// For defended battles, strongly lead with a 2nd-line artillery-core strike corps (~260 km preference);
			// for empty border grabs, prefer pure infantry so artillery strike corps stay free for real battles!
			float a_key = a.dist_km + (need_artillery_punch ? (a.is_core ? -260.0f : 0.0f) : (a.is_core ? 120.0f : 0.0f));
			float b_key = b.dist_km + (need_artillery_punch ? (b.is_core ? -260.0f : 0.0f) : (b.is_core ? 120.0f : 0.0f));
			if(a_key != b_key)
				return a_key < b_key;
			return ready_stacks[a.idx].id.index() < ready_stacks[b.idx].id.index();
		});

		// For land attacks, pick best safe supplied staging province adjacent to target_p (avoiding river crossings!)
		dcon::province_id staging_prov;
		military::crossing_type best_crossing = military::crossing_type::sea;
		if(use_land) {
			float best_stage_score = -1e9f;
			dcon::province_id lead_loc = ready_stacks[cands[0].idx].loc;
			for(auto padj : state.world.province_get_province_adjacency(target_p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				dcon::province_id other = padj.get_connected_provinces(0) == target_p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				if(other && other.index() < land_count && province::has_safe_access_to_province(state, n, other)) {
					float tp = logistics::calculate_supply_throughput_in_province(state, other, n);
					auto ctype = military::get_crossing_type(state, padj.id);
					float sc = (ctype == military::crossing_type::none ? 30.0f : (ctype == military::crossing_type::river ? 10.0f : -20.0f));
					sc += (tp > 0.0f ? 25.0f : -40.0f);
					sc += float(military::supply_limit_in_province(state, n, other)) * 0.45f;
					if(other == lead_loc) {
						sc += 18.0f;
					} else {
						sc -= province::direct_distance_km(state, lead_loc, other) * 0.08f;
					}
					if(sc > best_stage_score || (sc == best_stage_score && staging_prov && other.index() < staging_prov.index())) {
						best_stage_score = sc;
						staging_prov = other;
						best_crossing = ctype;
					}
				}
			}
			if(!staging_prov && !pt.is_rebel_or_liberation)
				continue;
		} else {
			best_crossing = military::crossing_type::sea;
		}

		// Determine stance-appropriate win & casualty thresholds (realistic for hill/river/dig_in breakthroughs so AI doesn't freeze at -50% WS!)
		float req_win = 1.20f;
		float max_cas = 2.60f;
		if(!is_at_war) {
			req_win = 1.15f;
			max_cas = 3.00f;
		} else if(stance == ai_front_helpers::war_stance::defensive) {
			if(pt.is_rebel_or_liberation) {
				req_win = 1.20f;
				max_cas = 2.80f;
			} else if(target_theater.continuous_front) {
				req_win = 1.50f;
				max_cas = 1.35f;
			} else {
				req_win = 1.35f;
				max_cas = 1.50f;
			}
		} else if(stance == ai_front_helpers::war_stance::cautious_offense) {
			req_win = pt.is_rebel_or_liberation ? 1.15f : 1.30f;
			max_cas = (is_wg || pt.is_rebel_or_liberation) ? 3.20f : 2.40f;
		} else {
			req_win = pt.is_rebel_or_liberation ? 1.12f : 1.22f;
			max_cas = (is_wg || time_against_us || pt.is_rebel_or_liberation) ? 3.50f : 2.60f;
		}

		chosen_attackers.clear();
		chosen_indices.clear();
		bool odds_sufficient = false;
		int32_t chosen_regs_sum = 0;
		bool chosen_has_core = false;
		uint16_t naval_origin_theater = ready_stacks[cands[0].idx].theater_id;

		if(!has_direct_defenders) {
			// Empty tile (0 direct defenders):
			// - On land: walk in with enough force not to get wiped by 1-hop enemies, WITHOUT over-stacking beyond supply!
			// - By sea: size strictly against nearby enemy land reaction forces (e.g. 60K in New York -> NEVER drop 12K on Washington!)
			float const reg_w = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
			int32_t stage_sl_regs = staging_prov
				? std::max(8, int32_t(float(military::supply_limit_in_province(state, n, staging_prov)) / reg_w))
				: 12;
			float needed_cover = use_naval
				? (naval_threat_regs * 0.90f)
				: std::min(nearby_enemy_regs * (stance == ai_front_helpers::war_stance::defensive ? 0.65f : 0.45f), float(stage_sl_regs));
			size_t max_empty_stacks = use_naval ? 6 : 3;

			if(needed_cover <= 1.0f) {
				chosen_attackers.push_back(ready_stacks[cands[0].idx].id);
				chosen_indices.push_back(cands[0].idx);
				odds_sufficient = true;
			} else {
				for(auto const& c : cands) {
					if(chosen_attackers.size() >= max_empty_stacks)
						break;
					auto const& rs = ready_stacks[c.idx];
					if(use_naval) {
						if(rs.theater_id != naval_origin_theater || chosen_regs_sum + rs.regs > free_tcap)
							continue;
					}
					if(is_at_war) {
						auto const stack_th = get_theater(rs.theater_id);
						if(stack_th.continuous_front) {
							int32_t extra_regs = 0;
							int32_t extra_stacks = 0;
							for(auto prev_idx : chosen_indices) {
								if(ready_stacks[prev_idx].loc == rs.loc) {
									extra_regs += ready_stacks[prev_idx].regs;
									extra_stacks += 1;
								}
							}
							bool allow_screen_adv = use_land && province::provinces_are_adjacent(state, rs.loc, target_p);
							if(!pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, true, extra_regs, extra_stacks, allow_screen_adv))
								continue;
						}
					}
					chosen_attackers.push_back(rs.id);
					chosen_indices.push_back(c.idx);
					chosen_regs_sum += rs.regs;
					if(rs.is_core) {
						chosen_has_core = true;
					}
					bool naval_quality_ok = !use_naval || naval_threat_regs < 4.0f || chosen_has_core || float(chosen_regs_sum) >= naval_threat_regs * 1.15f;
					if(float(chosen_regs_sum) >= needed_cover && naval_quality_ok) {
						odds_sufficient = true;
						break;
					}
				}
			}
		} else {
			// Directly defended province: evaluate analytically against DIRECT defenders, with realistic discount for neighboring/reaction enemy reinforcements
			float reinf_divisor = 1.0f + (use_naval ? 0.55f : 0.35f) * (nearby_enemy_regs / std::max(4.0f, direct_def_regs));
			for(auto const& c : cands) {
				if(chosen_attackers.size() >= 8)
					break;
				auto const& rs = ready_stacks[c.idx];
				if(use_naval) {
					if(rs.theater_id != naval_origin_theater)
						continue;
					if(chosen_regs_sum + rs.regs > free_tcap)
						continue;
				}
				// Dynamic check against stacks already tentatively picked from the same province
				if(is_at_war) {
					auto const stack_th = get_theater(rs.theater_id);
					if(stack_th.continuous_front) {
						int32_t extra_regs = 0;
						int32_t extra_stacks = 0;
						for(auto prev_idx : chosen_indices) {
							if(ready_stacks[prev_idx].loc == rs.loc) {
								extra_regs += ready_stacks[prev_idx].regs;
								extra_stacks += 1;
							}
						}
						bool allow_screen_adv = use_land
							&& province::provinces_are_adjacent(state, rs.loc, target_p)
							&& (is_wg || stance == ai_front_helpers::war_stance::active_offense);
						if(!pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, true, extra_regs, extra_stacks, allow_screen_adv))
							continue;
					}
				}

				chosen_attackers.push_back(rs.id);
				chosen_indices.push_back(c.idx);
				chosen_regs_sum += rs.regs;

				auto eval = ai_battle_eval::evaluate_battle_outcome(state, chosen_attackers, direct_defenders, target_p, best_crossing);
				float eff_win = eval.win_ratio / reinf_divisor;
				bool decisive_overmatch = (stance != ai_front_helpers::war_stance::defensive) && (eff_win >= 2.0f);
				bool naval_survives_reaction = !use_naval || naval_threat_regs < 4.0f || float(chosen_regs_sum) >= (direct_def_regs + naval_threat_regs) * 0.85f;
				if(eff_win >= req_win && (eval.casualty_ratio <= max_cas || decisive_overmatch) && naval_survives_reaction) {
					odds_sufficient = true;
					break;
				}
			}
		}

		// If THIS sector is too strong, skip to the next sector (NEVER return!)
		if(!odds_sufficient)
			continue;

		bool issued_any = false;
		if(use_naval) {
			for(auto idx : chosen_indices) {
				auto& st = ready_stacks[idx];
				state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
				state.world.army_set_ai_province(st.id, target_p);
				state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attack_transport));
				st.assigned = true;
				pin_tracker.commit_pull(st.loc, st.regs);
				issued_any = true;
			}
			if(issued_any) {
				active_naval_invasion = true;
			}
		} else {
			if(!staging_prov) {
				staging_prov = ready_stacks[chosen_indices[0]].loc;
			}
			// If ALL chosen stacks are ALREADY adjacent to target_p, strike simultaneously from their current tiles (zero attrition & zero 10-day staging delay!)
			bool all_already_adj = true;
			for(auto idx : chosen_indices) {
				auto loc_i = ready_stacks[idx].loc;
				if(loc_i != staging_prov && !province::provinces_are_adjacent(state, loc_i, target_p)) {
					all_already_adj = false;
					break;
				}
			}
			if(all_already_adj) {
				for(auto idx : chosen_indices) {
					auto& st = ready_stacks[idx];
					if(auto direct_path = province::make_land_unit_path(state, st.loc, target_p, n, st.id); !direct_path.empty()) {
						state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
						military::set_army_path(state, st.id, direct_path, n);
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attack_gathered));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs);
						issued_any = true;
					}
				}
			} else if(chosen_indices.size() == 1) {
				auto& st = ready_stacks[chosen_indices[0]];
				if(auto safe_path = province::make_safe_land_path(state, st.loc, staging_prov, n); !safe_path.empty()) {
					// Approach strictly along safe friendly supply lines to staging_prov first!
					state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
					military::set_army_path(state, st.id, safe_path, n);
					state.world.army_set_ai_province(st.id, target_p);
					state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attacking));
					st.assigned = true;
					pin_tracker.commit_pull(st.loc, st.regs);
					issued_any = true;
				} else if(pt.is_rebel_or_liberation) {
					if(auto lib_path = province::make_land_unit_path(state, st.loc, target_p, n, st.id); !lib_path.empty()) {
						state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
						military::set_army_path(state, st.id, lib_path, n);
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attack_gathered));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs);
						issued_any = true;
					}
				}
			} else {
				for(auto idx : chosen_indices) {
					auto& st = ready_stacks[idx];
					if(st.loc == staging_prov) {
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attacking));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs);
						issued_any = true;
					} else if(auto stage_path = province::make_safe_land_path(state, st.loc, staging_prov, n); !stage_path.empty()) {
						state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
						military::set_army_path(state, st.id, stage_path, n);
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attacking));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs);
						issued_any = true;
					}
				}
			}
		}

		if(issued_any && is_at_war && has_direct_defenders) {
			// Only avoid launching duplicate defended assaults on immediately adjacent defended tiles (~120 km),
			// NEVER wipe out the whole 570 km theater or skip empty undefended border tiles (like empty Bohemia)!
			for(uint32_t j = i + 1; j < psize; ++j) {
				auto jloc = potential_targets[j].location;
				if(jloc
					&& province::direct_distance_km(state, jloc, target_p) < 120.0f
					&& ai_front_helpers::province_has_enemy_army(state, n, jloc)) {
					potential_targets[j].location = dcon::province_id{};
				}
			}
		}
	}
}

// Sequential deterministic execution (eliminates latent dcon_vv path races during set_army_path)
void make_attacks(sys::state& state) {
	for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
		dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
		if(state.world.nation_is_valid(n) && state.world.nation_get_owned_province_count(n) > 0) {
			assign_targets(state, n);
		}
	}
}

void make_defense(sys::state& state) {
	concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
		dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
		if(state.world.nation_is_valid(n) && state.world.nation_get_owned_province_count(n) > 0) {
			distribute_guards(state, n);
		}
	});
}

void move_gathered_attackers_impl(sys::state& state, bool only_at_war) {
	std::vector<dcon::army_id> require_transport;

	auto process_army = [&](dcon::army_fat_id ar) {
		if(!ar.get_controller_from_army_control() || !unit_on_ai_control(state, ar))
			return;

		if(ar.get_ai_activity() == uint8_t(army_activity::attack_transport)) {
			if(!ar.get_arrival_time()
				&& !ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()
				&& std::find(require_transport.begin(), require_transport.end(), ar.id) == require_transport.end()) {

				// try to transport
				if(province::has_access_to_province(state, ar.get_controller_from_army_control(), ar.get_ai_province())) {
					require_transport.push_back(ar.id);
				} else {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
				}
			}
		} else if(ar.get_ai_activity() == uint8_t(army_activity::attack_gathered)) {
			if(!ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()
				&& !ar.get_is_retreating()) {

				auto ctrl = ar.get_controller_from_army_control().id;
				auto target_p = ar.get_ai_province().id;
				auto loc = ar.get_location_from_army_location().id;

				if(loc == target_p) { // arrived at target: check if siege/occupation completed
					if(!ar.get_arrival_time()) {
						auto pctrl = ar.get_location_from_army_location().get_nation_from_province_control();
						if(pctrl && !military::are_at_war(state, pctrl, ctrl)) {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						}
					}
				} else if(!ar.get_arrival_time()) {
					if(target_p && province::has_access_to_province(state, ctrl, target_p)) {
						// Re-verify battle odds right before stepping in!
						std::vector<dcon::army_id> def_armies;
						ai_battle_eval::collect_enemy_defenders_at(state, target_p, ctrl, def_armies, false);
						bool abort_attack = false;
						if(!def_armies.empty()) {
							std::vector<dcon::army_id> att_armies;
							for(auto o : state.world.nation_get_army_control(ctrl)) {
								auto oa = o.get_army();
								if(oa.get_is_retreating() || oa.get_black_flag() || oa.get_navy_from_army_transport())
									continue;
								auto oloc = oa.get_location_from_army_location().id;
								if(oloc && oa.get_ai_province() == target_p
									&& (oloc == loc || oloc == target_p || province::provinces_are_adjacent(state, oloc, target_p))) {
									att_armies.push_back(oa.id);
								}
							}
							if(att_armies.empty()) att_armies.push_back(ar.id);
							auto adj = state.world.get_province_adjacency_by_province_pair(loc, target_p);
							auto cross = adj ? military::get_crossing_type(state, adj) : military::crossing_type::none;
							auto ev = ai_battle_eval::evaluate_battle_outcome(state, att_armies, def_armies, target_p, cross);
							if(ev.win_ratio < 0.85f) {
								abort_attack = true;
							}
						}
						if(abort_attack) {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						} else {
							auto valid_path = military::move_army_ai(state, ar, target_p, ctrl);
							if(!valid_path) {
								ar.set_ai_activity(uint8_t(army_activity::on_guard));
								ar.set_ai_province(dcon::province_id{});
							}
						}
					} else {
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
						ar.set_ai_province(dcon::province_id{});
					}
				} else if(target_p && ar.get_path().size() == 1 && ar.get_path().at(0) == target_p) {
					// Already marching on the final step into target_p: abort if enemy reinforced into an unwinnable trap!
					std::vector<dcon::army_id> def_armies;
					ai_battle_eval::collect_enemy_defenders_at(state, target_p, ctrl, def_armies, false);
					if(!def_armies.empty()) {
						std::vector<dcon::army_id> att_armies;
						for(auto o : state.world.nation_get_army_control(ctrl)) {
							auto oa = o.get_army();
							if(oa.get_is_retreating() || oa.get_black_flag() || oa.get_navy_from_army_transport())
								continue;
							auto oloc = oa.get_location_from_army_location().id;
							if(oloc && oa.get_ai_province() == target_p
								&& (oloc == loc || oloc == target_p || province::provinces_are_adjacent(state, oloc, target_p))) {
								att_armies.push_back(oa.id);
							}
						}
						if(att_armies.empty()) att_armies.push_back(ar.id);
						auto adj = state.world.get_province_adjacency_by_province_pair(loc, target_p);
						auto cross = adj ? military::get_crossing_type(state, adj) : military::crossing_type::none;
						auto ev = ai_battle_eval::evaluate_battle_outcome(state, att_armies, def_armies, target_p, cross);
						if(ev.win_ratio < 0.65f) {
							military::stop_army_movement(state, ar);
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						}
					}
				}
			}
		} else if(ar.get_ai_activity() == uint8_t(army_activity::attacking)
			&& ar.get_ai_province() == ar.get_location_from_army_location()
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {
			ar.set_ai_activity(uint8_t(army_activity::attack_gathered));
		} else if(ar.get_ai_activity() == uint8_t(army_activity::attacking)
			&& ar.get_ai_province() != ar.get_location_from_army_location()
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {

			auto ctrl = ar.get_controller_from_army_control();
			auto target_p = ar.get_ai_province().id;
			auto loc = ar.get_location_from_army_location().id;

			bool all_gathered = true;
			for(auto o : ctrl.get_army_control()) {
				if(o.get_army().get_ai_province() == target_p) {
					if(loc != o.get_army().get_location_from_army_location()) {
						if(o.get_army().get_path().size() > 0 && o.get_army().get_path()[0] == loc) {
							all_gathered = false;
							break;
						}
					} else {
						if(o.get_army().get_battle_from_army_battle_participation()) {
							all_gathered = false;
							break;
						}
					}
				}
			}

			if(all_gathered) {
				if(target_p && province::has_access_to_province(state, ctrl, target_p)) {
					// Pre-attack re-verification: check whether enemy reinforced target_p while we gathered!
					std::vector<dcon::army_id> gathered_armies;
					for(auto o : ar.get_location_from_army_location().get_army_location()) {
						if(o.get_army().get_controller_from_army_control() == ctrl
							&& o.get_army().get_ai_province() == target_p
							&& o.get_army().get_path().size() == 0) {
							gathered_armies.push_back(o.get_army().id);
						}
					}
					std::vector<dcon::army_id> def_armies;
					// Check DIRECT defenders sitting on target_p (planning in assign_targets already accounted for 1-hop neighbors!)
					ai_battle_eval::collect_enemy_defenders_at(state, target_p, ctrl, def_armies, false);

					bool odds_ok = true;
					if(!def_armies.empty() && !gathered_armies.empty()) {
						auto adj = state.world.get_province_adjacency_by_province_pair(loc, target_p);
						auto cross = adj ? military::get_crossing_type(state, adj) : military::crossing_type::none;
						auto ev = ai_battle_eval::evaluate_battle_outcome(state, gathered_armies, def_armies, target_p, cross);
						if(ev.win_ratio < 0.85f) {
							odds_ok = false;
						}
					}

					if(!odds_ok) {
						// Enemy reinforced into an unwinnable meatgrinder -> abort cleanly back to on_guard!
						for(auto ga : gathered_armies) {
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::on_guard));
							state.world.army_set_ai_province(ga, dcon::province_id{});
						}
					} else if(target_p == loc) {
						for(auto ga : gathered_armies) {
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::attack_gathered));
						}
					} else if(auto path = province::make_land_unit_path(state, loc, target_p, ctrl, ar); path.size() > 0) {
						for(auto ga : gathered_armies) {
							state.world.army_set_special_order(ga, uint8_t(military::special_army_order::none));
							military::set_army_path(state, ga, path, ctrl);
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::attack_gathered));
						}
					} else {
						for(auto ga : gathered_armies) {
							require_transport.push_back(ga);
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::attack_transport));
						}
					}
				} else {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
				}
			}
		}
	};

	if(only_at_war) {
		for(auto n : state.world.in_nation) {
			if(n.get_owned_province_count() == 0 || !n.get_is_at_war())
				continue;
			for(auto a : n.get_army_control()) {
				process_army(a.get_army());
			}
		}
	} else {
		for(auto ar : state.world.in_army) {
			process_army(ar);
		}
	}

	for(uint32_t i = 0; i < require_transport.size(); ++i) {
		auto coastal_target_prov = state.world.army_get_location_from_army_location(require_transport[i]);
		auto controller = state.world.army_get_controller_from_army_control(require_transport[i]);

		dcon::navy_id transport_fleet = find_transport_fleet(state, controller);

		auto regs = state.world.army_get_army_membership(require_transport[i]);

		auto tcap = transport_fleet ? military::transport_capacity(state, transport_fleet) : 0;
		tcap -= int32_t(regs.end() - regs.begin());

		// no transport fleet at all: the army cannot be shipped (and an empty army, tcap == 0, must not path a null navy)
		if(!transport_fleet || tcap < 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
			for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::on_guard));
					state.world.army_set_ai_province(require_transport[j], dcon::province_id{}); // stop rechecking these units
					require_transport[j] = require_transport.back();
					require_transport.pop_back();
				}
			}
			state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::on_guard));
			state.world.army_set_ai_province(require_transport[i], dcon::province_id{}); // stop rechecking these units
			continue;
		}

		if(!state.world.province_get_is_coast(coastal_target_prov)) {
			auto path = province::make_path_to_nearest_coast(state, controller, coastal_target_prov);
			if(path.empty()) {
				state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(require_transport[i], dcon::province_id{});
				continue; // army could not reach coast
			} else {
				coastal_target_prov = path.front();

				military::set_army_path(state, require_transport[i], path, controller);
			}
		}

		{
			auto fleet_destination = province::has_naval_access_to_province(state, controller, coastal_target_prov) ? coastal_target_prov : state.world.province_get_port_to(coastal_target_prov);
			if(fleet_destination == state.world.navy_get_location_from_navy_location(transport_fleet)) {
				military::stop_navy_movement(state, transport_fleet);
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
			} else if(auto valid_path = military::move_navy_ai(state, transport_fleet, fleet_destination); !valid_path) {
				continue;
			} else {
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
			}
		}

		state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_attack));

		auto destination_region = state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[i]));

		// scoop up other armies to transport
		for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
			if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
				auto jregs = state.world.army_get_army_membership(require_transport[j]);
				if(tcap >= (jregs.end() - jregs.begin())) { // check if it will fit
					if(state.world.province_get_connected_region_id(state.world.army_get_ai_province(require_transport[j])) != destination_region)
						continue;

					if(state.world.army_get_location_from_army_location(require_transport[j]) == coastal_target_prov) {
						state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::transport_attack));
						tcap -= int32_t(jregs.end() - jregs.begin());
					} else {
						auto valid_path = military::move_army_ai(state, require_transport[j], coastal_target_prov, controller);
						if(valid_path) {
							state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::transport_attack));
							tcap -= int32_t(jregs.end() - jregs.begin());
						}
					}
				}

				require_transport[j] = require_transport.back();
				require_transport.pop_back();
			}
		}
	}
}

void move_gathered_attackers(sys::state& state) {
	move_gathered_attackers_impl(state, false);
}

bool will_upgrade_regiments(sys::state& state, dcon::nation_id n) {
	auto fid = dcon::fatten(state.world, n);

	auto total = fid.get_active_regiments();
	auto unfull = 0;

	for(auto ar : state.world.nation_get_army_control(n)) {
		for(auto r : ar.get_army().get_army_membership()) {
			if(r.get_regiment().get_strength() < 0.8f) {
				unfull++;

				if(unfull > total * 0.1f) {
					return false;
				}
			}
		}
	}

	return true;
}

namespace ai_detail {

inline int32_t estimate_nation_base_combat_width(sys::state const& state, dcon::nation_id n) {
	if(!n) {
		return int32_t(state.defines.base_combat_width);
	}
	return std::clamp(
		int32_t(state.world.nation_get_modifier_values(n, sys::national_mod_offsets::combat_width) + state.defines.base_combat_width),
		int32_t(military::min_combat_width),
		int32_t(military::max_combat_width)
	);
}

inline int32_t estimate_province_combat_width(sys::state const& state, dcon::nation_id n, dcon::province_id p, int32_t enemy_base_cw = 0) {
	int32_t base_cw = estimate_nation_base_combat_width(state, n);
	if(enemy_base_cw > 0) {
		base_cw = std::min(base_cw, enemy_base_cw);
	}
	float prov_mod = state.world.province_get_modifier_values(p, sys::provincial_mod_offsets::combat_width) + 1.0f;
	return std::clamp(int32_t(float(base_cw) * prov_mod), int32_t(military::min_combat_width), int32_t(military::max_combat_width));
}

inline bool unit_type_has_build_shortage(sys::state& state, dcon::nation_id n, dcon::unit_type_id type) {
	auto& def = state.military_definitions.unit_base_definitions[type];
	auto cap = state.world.nation_get_capital(n);
	if(!cap) {
		return false;
	}
	auto m = state.world.state_instance_get_market_from_local_market(state.world.province_get_state_membership(cap));
	if(!m) {
		return false;
	}
	float build_time = std::max(1.0f, float(def.build_time));
	for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
		if(def.build_cost.commodity_type[i]) {
			auto cid = def.build_cost.commodity_type[i];
			auto amount = def.build_cost.commodity_amounts[i];
			if(economy::estimate_probability_to_buy_after_demand_increase(state, m, cid, amount / build_time) < 0.1f) {
				return true;
			}
		} else {
			break;
		}
	}
	return false;
}

inline bool is_backline_unit(sys::state const& state, dcon::nation_id n, dcon::unit_type_id uid) {
	if(!uid)
		return false;
	auto& bdef = state.military_definitions.unit_base_definitions[uid];
	if(bdef.type == military::unit_type::support)
		return true;
	if(bdef.type == military::unit_type::special) {
		auto stats = n ? state.world.nation_get_unit_stats(n, uid) : military::unit_definition{};
		float sup = (n && stats.discipline_or_evasion > 0.0f) ? stats.support : bdef.support;
		return sup > 0.0f;
	}
	return false;
}

inline dcon::unit_type_id get_best_recon_unit(sys::state& state, dcon::nation_id n, bool primary_culture) {
	dcon::unit_type_id best = military::get_best_cavalry(state, n, primary_culture, true);
	auto get_recon = [&](dcon::unit_type_id uid) -> float {
		if(!uid) return 0.0f;
		auto stats = state.world.nation_get_unit_stats(n, uid);
		auto& bdef = state.military_definitions.unit_base_definitions[uid];
		return (stats.discipline_or_evasion > 0.0f) ? stats.reconnaissance_or_fire_range : bdef.reconnaissance_or_fire_range;
	};
	auto get_combat = [&](dcon::unit_type_id uid) -> float {
		if(!uid) return 0.0f;
		auto stats = state.world.nation_get_unit_stats(n, uid);
		auto& bdef = state.military_definitions.unit_base_definitions[uid];
		auto& d = (stats.discipline_or_evasion > 0.0f) ? stats : bdef;
		return d.defence_or_hull + d.attack_or_gun_power + d.support * 2.0f;
	};

	if(best && get_recon(best) <= 0.0f) {
		best = dcon::unit_type_id{};
	}

	for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
		dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
		auto& bdef = state.military_definitions.unit_base_definitions[uid];
		if(bdef.type != military::unit_type::cavalry && bdef.type != military::unit_type::special)
			continue;
		if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
			continue;
		if(bdef.primary_culture && !primary_culture)
			continue;
		float rec = get_recon(uid);
		if(rec <= 0.0f)
			continue;
		if(unit_type_has_build_shortage(state, n, uid))
			continue;
		float best_rec = get_recon(best);
		if(!best || rec > best_rec || (rec == best_rec && get_combat(uid) > get_combat(best))) {
			best = uid;
		}
	}
	return best;
}

inline dcon::unit_type_id get_best_siege_unit(sys::state& state, dcon::nation_id n, bool primary_culture) {
	dcon::unit_type_id best;
	float best_siege = 0.0f;
	float best_score = -1.0f;

	for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
		dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
		auto& bdef = state.military_definitions.unit_base_definitions[uid];
		if(bdef.type != military::unit_type::special)
			continue;
		if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
			continue;
		if(bdef.primary_culture && !primary_culture)
			continue;
		auto stats = state.world.nation_get_unit_stats(n, uid);
		auto& d = (stats.discipline_or_evasion > 0.0f) ? stats : bdef;
		if(d.siege_or_torpedo_attack <= 0.0f)
			continue;
		if(unit_type_has_build_shortage(state, n, uid))
			continue;
		float score = d.siege_or_torpedo_attack * 10.0f + (d.support > 0.0f ? 4.0f : 0.0f) + d.defence_or_hull + d.attack_or_gun_power;
		if(!best || d.siege_or_torpedo_attack > best_siege || (d.siege_or_torpedo_attack == best_siege && score > best_score)) {
			best = uid;
			best_siege = d.siege_or_torpedo_attack;
			best_score = score;
		}
	}
	return best;
}

inline int32_t calculate_nation_artillery_cap(sys::state& state, dcon::nation_id n, int32_t desired_army) {
	if(desired_army <= 2) {
		return 0;
	}
	int32_t nation_cw = estimate_nation_base_combat_width(state, n);
	bool at_war = state.world.nation_get_is_at_war(n);
	auto cap = state.world.nation_get_capital(n);

	std::vector<dcon::province_id> front_provs;
	int32_t min_enemy_cw = nation_cw;

	if(at_war) {
		for(auto pc : state.world.nation_get_province_control(n)) {
			auto p = pc.get_province().id;
			bool borders_enemy = false;
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				if(province::is_sea(state, other))
					continue;
				auto octrl = state.world.province_get_nation_from_province_control(other);
				if(octrl && military::are_at_war(state, n, octrl)) {
					borders_enemy = true;
					min_enemy_cw = std::min(min_enemy_cw, estimate_nation_base_combat_width(state, octrl));
				}
			}
			if(borders_enemy) {
				front_provs.push_back(p);
			}
		}
	} else {
		// Pick up to 2 strongest non-allied/rival land neighbors to size peacetime artillery corps realistically
		struct neighbor_entry {
			dcon::nation_id id;
			float threat;
		};
		std::vector<neighbor_entry> neighbors;
		auto rival = state.world.nation_get_ai_rival(n);
		auto in_sphere = state.world.nation_get_in_sphere_of(n);
		for(auto adj : state.world.nation_get_nation_adjacency(n)) {
			auto other = adj.get_connected_nations(0) != n ? adj.get_connected_nations(0) : adj.get_connected_nations(1);
			if(!other || other.get_owned_province_count() == 0)
				continue;
			auto ovr = other.get_overlord_as_subject().get_ruler();
			if(ovr == n || nations::are_allied(state, n, other) || (ovr && nations::are_allied(state, n, ovr)))
				continue;
			if(in_sphere && in_sphere == other.get_in_sphere_of())
				continue;
			float thr = estimate_strength(state, other.id);
			if(other.id == rival || other.get_ai_rival() == n) {
				thr *= 1.5f;
			}
			neighbors.push_back(neighbor_entry{ other.id, thr });
		}
		std::sort(neighbors.begin(), neighbors.end(), [](neighbor_entry const& a, neighbor_entry const& b) {
			if(a.threat != b.threat)
				return a.threat > b.threat;
			return a.id.index() < b.id.index();
		});
		if(neighbors.size() > 2) {
			neighbors.resize(2);
		}
		for(auto nb : neighbors) {
			min_enemy_cw = std::min(min_enemy_cw, estimate_nation_base_combat_width(state, nb.id));
		}

		for(auto po : state.world.nation_get_province_ownership(n)) {
			auto p = po.get_province().id;
			bool borders_target = false;
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				if(province::is_sea(state, other))
					continue;
				auto oown = state.world.province_get_nation_from_province_ownership(other);
				if(!oown || oown == n)
					continue;
				if(!neighbors.empty()) {
					for(auto nb : neighbors) {
						if(oown == nb.id) {
							borders_target = true;
							break;
						}
					}
				} else {
					borders_target = true;
				}
				if(borders_target)
					break;
			}
			if(borders_target) {
				front_provs.push_back(p);
			}
		}
	}

	// Cap sampled border provinces at 20 closest to capital so giant empty frontiers don't bloat artillery
	if(front_provs.size() > 20) {
		std::sort(front_provs.begin(), front_provs.end(), [&](dcon::province_id a, dcon::province_id b) {
			float da = cap ? province::sorting_distance(state, a, cap) : 0.0f;
			float db = cap ? province::sorting_distance(state, b, cap) : 0.0f;
			if(da != db)
				return da < db;
			return a.index() < b.index();
		});
		front_provs.resize(20);
	}

	int32_t front_cw_sum = 0;
	for(auto p : front_provs) {
		front_cw_sum += estimate_province_combat_width(state, n, p, min_enemy_cw);
	}
	if(front_cw_sum == 0) {
		// Island / expeditionary nation: enough artillery to cover ~2 full-width expeditionary fronts
		front_cw_sum = nation_cw * 2;
	}

	// 20% reserve margin over sum of terrain-adjusted front combat widths, capped at 45% of total desired army
	int32_t raw_cap = int32_t(std::ceil(float(front_cw_sum) * 1.20f));
	int32_t max_fraction_cap = int32_t(float(desired_army) * 0.45f);
	int32_t min_era_floor = std::min(nation_cw, max_fraction_cap);
	return std::clamp(raw_cap, min_era_floor, max_fraction_cap);
}

inline int32_t typical_supply_regiment_cap(sys::state& state, dcon::nation_id n) {
	float reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	std::vector<int32_t> border_caps;
	std::vector<int32_t> owned_caps;
	border_caps.reserve(12);
	owned_caps.reserve(12);

	for(auto po : state.world.nation_get_province_ownership(n)) {
		auto p = po.get_province().id;
		if(province::is_sea(state, p))
			continue;
		bool is_border = false;
		if(border_caps.size() < 12) {
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				if(!province::is_sea(state, other)) {
					auto oown = state.world.province_get_nation_from_province_ownership(other);
					if(oown && oown != n) {
						is_border = true;
						break;
					}
				}
			}
		}
		if(is_border && border_caps.size() < 12) {
			int32_t sl = military::supply_limit_in_province(state, n, p);
			border_caps.push_back(std::max(1, int32_t(float(sl) / reg_weight)));
		} else if(owned_caps.size() < 12) {
			int32_t sl = military::supply_limit_in_province(state, n, p);
			owned_caps.push_back(std::max(1, int32_t(float(sl) / reg_weight)));
		}
		if(border_caps.size() >= 12 && owned_caps.size() >= 12)
			break;
	}

	auto& samples = !border_caps.empty() ? border_caps : owned_caps;
	if(samples.empty()) {
		auto cap = state.world.nation_get_capital(n);
		int32_t sl = cap ? military::supply_limit_in_province(state, n, cap) : 30;
		return std::clamp(int32_t(float(sl) / reg_weight), 6, 30);
	}
	std::sort(samples.begin(), samples.end());
	// Use 40th percentile across border/owned provinces so stacks fit realistic front supply limits (not just a high-SL capital!)
	size_t idx = (samples.size() * 2) / 5;
	return std::clamp(samples[idx], 6, 30);
}

struct stack_composition {
	int32_t total = 0;
	int32_t frontline = 0;
	int32_t backline = 0;
	int32_t recon = 0;
	int32_t siege = 0;
};

inline stack_composition classify_army_composition(sys::state const& state, dcon::nation_id n, dcon::army_id a) {
	stack_composition sc{};
	for(auto r : state.world.army_get_army_membership(a)) {
		auto uid = r.get_regiment().get_type();
		auto const& bdef = state.military_definitions.unit_base_definitions[uid];
		auto stats = n ? state.world.nation_get_unit_stats(n, uid) : military::unit_definition{};
		bool use_stats = (n && stats.discipline_or_evasion > 0.0f);
		float recon_val = use_stats ? stats.reconnaissance_or_fire_range : bdef.reconnaissance_or_fire_range;
		float siege_val = use_stats ? stats.siege_or_torpedo_attack : bdef.siege_or_torpedo_attack;
		++sc.total;
		if(is_backline_unit(state, n, uid)) {
			++sc.backline;
		} else {
			++sc.frontline;
		}
		if(recon_val > 0.0f) {
			++sc.recon;
		}
		if(siege_val > 0.0f) {
			++sc.siege;
		}
	}
	return sc;
}

} // namespace ai_detail

void update_land_constructions(sys::state& state) {
	for(auto n : state.world.in_nation) {
		if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
			continue;
		auto disarm = n.get_disarmed_until();
		if(disarm && state.current_date < disarm)
			continue;

		std::vector<dcon::province_land_construction_id> hopeless_construction;

		state.world.nation_for_each_province_land_construction(n, [&](dcon::province_land_construction_id plcid) {
			auto fat_plc = dcon::fatten(state.world, plcid);
			auto prov = fat_plc.get_pop().get_province_from_pop_location();
			if(prov.get_nation_from_province_control() != n) {
				hopeless_construction.push_back(plcid);
				return;
			}
			auto date = fat_plc.get_start_date();
			auto today = state.current_date;
			auto days_passed = float(1 + today.value - date.value);
			auto progress = economy::construction_progress(state, plcid);
			uint32_t build_time = economy::construction_get_actual_construction_time(state, plcid);
			auto estimated_progress = days_passed / float(std::max<uint32_t>(1, build_time));
			if(estimated_progress > 5.f * progress && estimated_progress >= 1.f) {
				hopeless_construction.push_back(plcid);
				return;
			}
		});

		for(auto item : hopeless_construction) {
			state.world.delete_province_land_construction(item);
		}

		int32_t num_frontline = 0;
		int32_t num_backline = 0;
		int32_t num_recon = 0;
		int32_t num_siege = 0;
		int32_t total_existing = 0;
		int32_t total_building = 0;

		// Nation-wide best unit types (non-primary and primary culture)
		auto inf_type = military::get_best_infantry(state, n, false);
		auto pc_inf_type = military::get_best_infantry(state, n, true);
		auto art_type = military::get_best_artillery(state, n, false);
		auto pc_art_type = military::get_best_artillery(state, n, true);
		auto cav_type = military::get_best_cavalry(state, n, false);
		auto pc_cav_type = military::get_best_cavalry(state, n, true);
		auto recon_type = ai_detail::get_best_recon_unit(state, n, false);
		auto pc_recon_type = ai_detail::get_best_recon_unit(state, n, true);
		auto siege_type = ai_detail::get_best_siege_unit(state, n, false);
		auto pc_siege_type = ai_detail::get_best_siege_unit(state, n, true);

		auto count_unit = [&](dcon::unit_type_id uid) {
			if(!uid)
				return;
			auto& bdef = state.military_definitions.unit_base_definitions[uid];
			auto stats = state.world.nation_get_unit_stats(n, uid);
			auto& d = (stats.discipline_or_evasion > 0.0f) ? stats : bdef;
			if(ai_detail::is_backline_unit(state, n, uid)) {
				++num_backline;
			} else {
				++num_frontline;
			}
			if(d.reconnaissance_or_fire_range > 0.0f) {
				++num_recon;
			}
			if(d.siege_or_torpedo_attack > 0.0f) {
				++num_siege;
			}
		};

		bool can_upgrade = will_upgrade_regiments(state, n);
		for(auto ar : state.world.nation_get_army_control(n)) {
			for(auto r : ar.get_army().get_army_membership()) {
				auto reg = r.get_regiment();
				auto type = reg.get_type();
				++total_existing;
				count_unit(type);

				if(!can_upgrade)
					continue;

				auto etype = state.military_definitions.unit_base_definitions[type].type;
				auto pop = reg.get_pop_from_regiment_source();
				bool primary_culture = pop && (pop.get_culture() == n.get_primary_culture());

				auto target_inf = primary_culture ? pc_inf_type : inf_type;
				auto target_art = primary_culture ? pc_art_type : art_type;
				auto target_cav = primary_culture ? pc_cav_type : cav_type;
				auto target_siege = primary_culture ? pc_siege_type : siege_type;

				switch(etype) {
				case military::unit_type::infantry:
					if(target_inf && military::can_change_land_unit_type<command::actor::ai>(state, n, reg, target_inf)
						&& military::is_infantry_better(state, n, type, target_inf)) {
						military::upgrade_regiment(state, reg, target_inf);
					}
					break;
				case military::unit_type::support:
					if(target_art && military::can_change_land_unit_type<command::actor::ai>(state, n, reg, target_art)
						&& military::is_artillery_better(state, n, type, target_art)) {
						military::upgrade_regiment(state, reg, target_art);
					}
					break;
				case military::unit_type::cavalry:
					if(target_cav && military::can_change_land_unit_type<command::actor::ai>(state, n, reg, target_cav)
						&& military::is_cavalry_better(state, n, type, target_cav)) {
						military::upgrade_regiment(state, reg, target_cav);
					}
					break;
				case military::unit_type::special:
					// Never downgrade special units (engineers/tanks/planes) into cavalry!
					if(target_siege && target_siege != type
						&& state.military_definitions.unit_base_definitions[type].siege_or_torpedo_attack > 0.0f
						&& military::can_change_land_unit_type<command::actor::ai>(state, n, reg, target_siege)) {
						auto cur_stats = state.world.nation_get_unit_stats(n, type);
						auto new_stats = state.world.nation_get_unit_stats(n, target_siege);
						if(new_stats.siege_or_torpedo_attack > cur_stats.siege_or_torpedo_attack) {
							military::upgrade_regiment(state, reg, target_siege);
						}
					}
					break;
				default:
					break;
				}
			}
		}

		// Count ongoing land constructions as well so we don't block all recruitment while 1 regiment builds
		state.world.nation_for_each_province_land_construction(n, [&](dcon::province_land_construction_id plcid) {
			auto uid = state.world.province_land_construction_get_type(plcid);
			++total_building;
			count_unit(uid);
		});

		int32_t desired_army = int32_t(calculate_desired_army_size(state, n));
		int32_t num_to_build_nation = desired_army - (total_existing + total_building);
		if(num_to_build_nation <= 0)
			continue;

		// Cap concurrent queue size so the nation doesn't starve itself of goods in one giant spike
		int32_t max_concurrent_builds = std::max(8, desired_army / 3);
		int32_t allowed_new_orders = max_concurrent_builds - total_building;
		if(allowed_new_orders <= 0)
			continue;
		num_to_build_nation = std::min(num_to_build_nation, allowed_new_orders);

		int32_t art_cap = ai_detail::calculate_nation_artillery_cap(state, n, desired_army);
		int32_t nation_cw = ai_detail::estimate_nation_base_combat_width(state, n);
		int32_t core_art_per_stack = std::clamp(nation_cw <= 14 ? std::min(nation_cw, 10) : 5, 4, 10);
		int32_t estimated_core_stacks = std::max(1, (art_cap + core_art_per_stack - 1) / core_art_per_stack);

		// Target recon & siege proportional to core fighting stacks (matching recon_unit_ratio / engineer_unit_ratio)
		int32_t recon_basis = std::min(desired_army, std::max(art_cap * 2 + 4, 10));
		int32_t target_recon = (recon_type || pc_recon_type)
			? std::clamp(int32_t(std::round(float(recon_basis) * state.defines.recon_unit_ratio)), desired_army >= 6 ? 1 : 0, std::max(1, estimated_core_stacks * 2))
			: 0;
		int32_t target_siege = ((siege_type || pc_siege_type) && desired_army >= 8)
			? std::clamp(estimated_core_stacks, 1, std::max(1, int32_t(float(desired_army) * state.defines.engineer_unit_ratio)))
			: 0;

		auto try_build_from_pop = [&](dcon::province_id prov, dcon::pop_id pop_id, bool pc) -> bool {
			auto cult = state.world.pop_get_culture(pop_id);
			auto pick_inf = pc && pc_inf_type ? pc_inf_type : inf_type;
			auto pick_art = pc && pc_art_type ? pc_art_type : art_type;
			auto pick_rec = pc && pc_recon_type ? pc_recon_type : recon_type;
			auto pick_sg = pc && pc_siege_type ? pc_siege_type : siege_type;

			auto attempt = [&](dcon::unit_type_id uid) -> bool {
				if(!uid)
					return false;
				if(!command::can_start_land_unit_construction<false>(state, n, prov, cult, uid))
					return false;
				command::execute_start_land_unit_construction(state, n, prov, cult, uid);
				count_unit(uid);
				return true;
			};

			// 1. Always keep frontline cover ahead of backline
			if(num_frontline < num_backline + 2) {
				if(attempt(pick_inf)) return true;
				if(pick_inf != inf_type && attempt(inf_type)) return true;
			}
			// 2. Recon (hussars / airplanes) up to target_recon
			if(num_recon < target_recon && pick_rec) {
				bool rec_backline = ai_detail::is_backline_unit(state, n, pick_rec);
				if(!rec_backline || num_backline < art_cap) {
					if(attempt(pick_rec)) return true;
				}
			}
			// 3. Siege (engineers / tanks) up to target_siege
			if(num_siege < target_siege && pick_sg) {
				bool sg_backline = ai_detail::is_backline_unit(state, n, pick_sg);
				if(!sg_backline || num_backline < art_cap) {
					if(attempt(pick_sg)) return true;
				}
			}
			// 4. Artillery ONLY up to front-width-derived art_cap (smooth transition: excess old art is never disbanded)
			if(num_backline < art_cap && num_frontline >= num_backline && pick_art) {
				if(attempt(pick_art)) return true;
			}
			// 5. All remaining soldier capacity goes into frontline infantry!
			if(attempt(pick_inf)) return true;
			if(pick_inf != inf_type && attempt(inf_type)) return true;
			return false;
		};

		for(auto p : state.world.nation_get_province_ownership(n)) {
			if(num_to_build_nation <= 0)
				break;
			auto prov = p.get_province();
			if(prov.get_nation_from_province_control() != n)
				continue;

			float divisor = state.defines.pop_size_per_regiment;
			if(prov.get_is_colonial()) {
				divisor *= state.defines.pop_min_size_for_regiment_colony_multiplier;
			} else if(!prov.get_is_owner_core()) {
				divisor *= state.defines.pop_min_size_for_regiment_noncore_multiplier;
			}
			float minimum = state.defines.pop_min_size_for_regiment;

			for(auto pop : prov.get_pop_location()) {
				if(num_to_build_nation <= 0)
					break;
				if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
					if(pop.get_pop().get_size() >= minimum) {
						auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
						auto regs = pop.get_pop().get_regiment_source();
						auto building = pop.get_pop().get_province_land_construction();
						auto num_to_make_local = amount - int32_t((regs.end() - regs.begin()) + (building.end() - building.begin()));
						bool pc = pop.get_pop().get_is_primary_or_accepted_culture();
						while(num_to_make_local > 0 && num_to_build_nation > 0) {
							if(!try_build_from_pop(prov.id, pop.get_pop().id, pc)) {
								break;
							}
							--num_to_make_local;
							--num_to_build_nation;
						}
					}
				}
			}
		}
	}
}

void new_units_and_merging(sys::state& state) {
	struct split_request {
		dcon::army_id army;
		dcon::nation_id controller;
		std::vector<dcon::regiment_id> to_split;
	};
	std::vector<split_request> splits;
	std::vector<dcon::army_id> to_cleanup;
	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);

	for(auto ar : state.world.in_army) {
		auto controller = ar.get_controller_from_army_control();
		if(!controller
			|| !unit_on_ai_control(state, ar)
			|| ar.get_battle_from_army_battle_participation()
			|| ar.get_navy_from_army_transport()
			|| ar.get_arrival_time()
			|| ar.get_is_retreating()) {
			continue;
		}

		auto location = ar.get_location_from_army_location();
		if(!location || province::is_sea(state, location))
			continue;
		auto regs = ar.get_army_membership();
		int32_t reg_count = int32_t(regs.end() - regs.begin());
		if(reg_count == 0)
			continue;

		int32_t nation_cw = ai_detail::estimate_nation_base_combat_width(state, controller);
		int32_t supply_cap_regs = ai_detail::typical_supply_regiment_cap(state, controller);
		int32_t max_stack_regs = std::max(6, supply_cap_regs);
		// Core artillery stack: 3..10 backline (scales with era CW and real front supply limit) + >=1:1 frontline cover
		int32_t core_backline_target = std::clamp(std::min(nation_cw <= 14 ? nation_cw : 5, max_stack_regs / 2), 3, 10);
		// Pure infantry reserve stack (including mobilized brigades): 6..12 regiments, strictly fitting province supply limits
		int32_t reserve_stack_target = std::clamp((max_stack_regs * 3) / 4, 6, std::min(12, max_stack_regs));

		auto is_compatible_merge_host = [&](dcon::army_id host, bool joining_is_backline, bool joining_is_recon, bool joining_is_siege, int32_t joining_count) -> int32_t {
			if(host == ar.id)
				return 0;
			if(army_activity(state.world.army_get_ai_activity(host)) != army_activity::on_guard)
				return 0;
			if(state.world.army_get_battle_from_army_battle_participation(host)
				|| state.world.army_get_navy_from_army_transport(host)
				|| state.world.army_get_is_retreating(host))
				return 0;
			auto hsc = ai_detail::classify_army_composition(state, controller, host);
			if(hsc.total == 0)
				return 0;
			auto hloc = state.world.army_get_location_from_army_location(host);
			int32_t host_sl_regs = hloc ? std::max(1, int32_t(float(military::supply_limit_in_province(state, controller, hloc)) / reg_weight)) : max_stack_regs;
			int32_t host_cap = std::min(max_stack_regs, host_sl_regs);
			if(hsc.total + joining_count > host_cap)
				return 0;

			bool host_is_core = (hsc.backline > 0);
			if(joining_is_backline) {
				// NEVER push backline above frontline cover (prevents exposed 3/0/5 artillery stacks!)
				if(hsc.backline + joining_count > hsc.frontline)
					return 0;
				if(host_is_core && hsc.backline + joining_count <= core_backline_target) {
					return (joining_is_siege && hsc.siege == 0) ? 4 : 3;
				}
				if(!host_is_core && hsc.frontline >= 4 && joining_count <= core_backline_target) {
					return 2;
				}
				return 0;
			}
			if(joining_is_recon) {
				// Recon prefers core stacks that still lack recon
				if(host_is_core && hsc.recon < 2 && hsc.frontline + joining_count <= core_backline_target + 4) {
					return hsc.recon == 0 ? 4 : 2;
				}
				if(!host_is_core && hsc.total + joining_count <= reserve_stack_target && hsc.recon == 0) {
					return 1;
				}
				return 0;
			}
			// Frontline infantry (including mobilized infantry):
			// Priority 1: reinforce core artillery stacks that need frontline cover (>= backline + 2)
			if(host_is_core && hsc.frontline < std::max(core_backline_target + 1, hsc.backline + 2)) {
				return 4;
			}
			// Priority 2: build pure infantry reserve stacks (6-12 regiments within supply) instead of bloating artillery stacks!
			if(!host_is_core && hsc.total + joining_count <= reserve_stack_target) {
				return 3;
			}
			// Priority 3: top off a core stack slightly if no reserve stack is available
			if(host_is_core && hsc.frontline < core_backline_target + 3 && hsc.total + joining_count <= host_cap) {
				return 1;
			}
			return 0;
		};

		if(ar.get_black_flag() || army_activity(ar.get_ai_activity()) == army_activity::unspecified) {
			if(reg_count > 1) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
			} else {
				auto reg = (*regs.begin()).get_regiment();
				auto type = reg.get_type();
				auto& bdef = state.military_definitions.unit_base_definitions[type];
				auto stats = state.world.nation_get_unit_stats(controller, type);
				auto& d = (stats.discipline_or_evasion > 0.0f) ? stats : bdef;
				bool is_back = ai_detail::is_backline_unit(state, controller, type);
				bool is_rec = !is_back && (d.reconnaissance_or_fire_range > 0.0f);
				bool is_sg = (d.siege_or_torpedo_attack > 0.0f);

				dcon::province_id target_location;
				int32_t best_tier = 0;
				float best_distance = 2.0f;

				for(auto o : controller.get_army_control()) {
					auto other_army = o.get_army();
					auto other_location = other_army.get_location_from_army_location();
					if(!other_location || other_location.get_connected_region_id() != location.get_connected_region_id())
						continue;
					int32_t tier = is_compatible_merge_host(other_army.id, is_back, is_rec, is_sg, 1);
					if(tier <= 0)
						continue;
					float sdist = province::sorting_distance(state, other_location, location);
					// Prefer higher role-fit tier unless a lower-tier stack is vastly closer
					if(!target_location || tier > best_tier || (tier == best_tier && sdist < best_distance)) {
						target_location = other_location.id;
						best_tier = tier;
						best_distance = sdist;
					}
				}

				if(target_location) {
					if(target_location == location.id) {
						ar.set_ai_province(target_location);
						ar.set_ai_activity(uint8_t(army_activity::merging));
					} else if(military::move_army_ai(state, ar, target_location, controller)) {
						ar.set_ai_province(target_location);
						ar.set_ai_activity(uint8_t(army_activity::merging));
					} else {
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
					}
				} else {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
				}
			}
		} else if(army_activity(ar.get_ai_activity()) == army_activity::merging) {
			auto my_sc = ai_detail::classify_army_composition(state, controller, ar.id);
			int32_t local_sl_regs = std::max(1, int32_t(float(military::supply_limit_in_province(state, controller, location.id)) / reg_weight));
			int32_t guard_merge_cap = std::min(max_stack_regs, local_sl_regs);

			dcon::army_id best_host;
			int32_t best_tier = 0;
			for(auto o : location.get_army_location()) {
				auto other_army = o.get_army();
				if(other_army.id == ar.id || other_army.get_controller_from_army_control() != controller)
					continue;
				if(other_army.get_battle_from_army_battle_participation()
					|| other_army.get_navy_from_army_transport()
					|| other_army.get_is_retreating())
					continue;
				auto oact = army_activity(other_army.get_ai_activity());
				if(oact != army_activity::on_guard && oact != army_activity::attacking)
					continue;

				int32_t tier = 0;
				if(reg_count == 1 && oact == army_activity::on_guard) {
					auto reg = (*regs.begin()).get_regiment();
					auto type = reg.get_type();
					auto& bdef = state.military_definitions.unit_base_definitions[type];
					auto stats = state.world.nation_get_unit_stats(controller, type);
					auto& d = (stats.discipline_or_evasion > 0.0f) ? stats : bdef;
					bool is_back = ai_detail::is_backline_unit(state, controller, type);
					bool is_rec = !is_back && (d.reconnaissance_or_fire_range > 0.0f);
					bool is_sg = (d.siege_or_torpedo_attack > 0.0f);
					tier = is_compatible_merge_host(other_army.id, is_back, is_rec, is_sg, 1);
				} else {
					auto hsc = ai_detail::classify_army_composition(state, controller, other_army.id);
					int32_t cap_here = (oact == army_activity::attacking) ? local_sl_regs : guard_merge_cap;
					bool art_covered = (my_sc.backline == 0) || (hsc.backline + my_sc.backline <= hsc.frontline + my_sc.frontline);
					if(hsc.total > 0
						&& hsc.total + my_sc.total <= cap_here
						&& hsc.backline + my_sc.backline <= core_backline_target
						&& art_covered) {
						if(oact == army_activity::attacking) {
							tier = 5;
						} else if(hsc.backline > 0 && hsc.frontline < hsc.backline + 2 && my_sc.frontline > 0) {
							tier = 4;
						} else if(hsc.backline == 0 && my_sc.backline == 0 && hsc.total + my_sc.total <= std::min(reserve_stack_target, local_sl_regs)) {
							tier = 3;
						} else {
							tier = 2;
						}
					}
				}
				if(tier > best_tier || (tier == best_tier && tier > 0 && best_host && other_army.id.index() < best_host.index())) {
					best_tier = tier;
					best_host = other_army.id;
				}
			}
			if(best_host) {
				std::vector<dcon::regiment_id> to_move;
				for(auto r : regs) {
					to_move.push_back(r.get_regiment().id);
				}
				for(auto rid : to_move) {
					state.world.regiment_set_army_from_army_membership(rid, best_host);
				}
				to_cleanup.push_back(ar.id);
			} else {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
			}
		} else if(army_activity(ar.get_ai_activity()) == army_activity::on_guard) {
			int32_t local_sl_regs = std::max(1, int32_t(float(military::supply_limit_in_province(state, controller, location.id)) / reg_weight));
			// Coalesce small idle on-guard fragments (2..4 regiments): first same province, then nearby safe host in same region!
			if(reg_count <= 4) {
				auto my_sc = ai_detail::classify_army_composition(state, controller, ar.id);
				bool has_back = (my_sc.backline > 0);
				dcon::army_id best_host;
				int32_t best_host_regs = -1;
				for(auto o : location.get_army_location()) {
					auto other = o.get_army();
					if(other.id == ar.id
						|| other.get_controller_from_army_control() != controller
						|| army_activity(other.get_ai_activity()) != army_activity::on_guard
						|| other.get_battle_from_army_battle_participation()
						|| other.get_navy_from_army_transport()
						|| other.get_arrival_time()
						|| other.get_is_retreating())
						continue;
					auto osc = ai_detail::classify_army_composition(state, controller, other.id);
					if(osc.total == 0 || osc.total < reg_count)
						continue;
					if(osc.total == reg_count && other.id.index() > ar.id.index())
						continue;
					int32_t max_allowed = std::min(local_sl_regs, (has_back || osc.backline > 0) ? max_stack_regs : reserve_stack_target);
					if(osc.total + reg_count > max_allowed)
						continue;
					if(has_back && (osc.backline + my_sc.backline > core_backline_target || osc.backline + my_sc.backline > osc.frontline + my_sc.frontline))
						continue;
					if(osc.total > best_host_regs) {
						best_host_regs = osc.total;
						best_host = other.id;
					}
				}
				if(best_host) {
					std::vector<dcon::regiment_id> to_move;
					for(auto r : regs) {
						to_move.push_back(r.get_regiment().id);
					}
					for(auto rid : to_move) {
						state.world.regiment_set_army_from_army_membership(rid, best_host);
					}
					to_cleanup.push_back(ar.id);
					continue;
				}

				// Also allow idle 2..4-regiment starting fragments (away from active frontline contact) to march-merge into a nearby compatible stack!
				if(!ai_front_helpers::is_province_near_hostiles(state, controller, location.id, 1)) {
					dcon::province_id merge_dest;
					float best_dist_km = 450.0f;
					for(auto o : controller.get_army_control()) {
						auto other = o.get_army();
						if(other.id == ar.id
							|| army_activity(other.get_ai_activity()) != army_activity::on_guard
							|| other.get_battle_from_army_battle_participation()
							|| other.get_navy_from_army_transport()
							|| other.get_arrival_time()
							|| other.get_is_retreating())
							continue;
						auto oloc = other.get_location_from_army_location();
						if(!oloc || oloc.id == location.id)
							continue;
						if(!controller.get_is_at_war() && oloc.get_connected_region_id() != location.get_connected_region_id())
							continue;
						auto osc = ai_detail::classify_army_composition(state, controller, other.id);
						if(osc.total < reg_count || (osc.total == reg_count && other.id.index() > ar.id.index()))
							continue;
						int32_t oloc_sl_regs = std::max(1, int32_t(float(military::supply_limit_in_province(state, controller, oloc.id)) / reg_weight));
						int32_t max_allowed = std::min(oloc_sl_regs, (has_back || osc.backline > 0) ? max_stack_regs : reserve_stack_target);
						if(osc.total + reg_count > max_allowed)
							continue;
						if(has_back && (osc.backline + my_sc.backline > core_backline_target || osc.backline + my_sc.backline > osc.frontline + my_sc.frontline))
							continue;
						float d_km = province::direct_distance_km(state, location.id, oloc.id);
						if(d_km < best_dist_km || (d_km == best_dist_km && merge_dest && oloc.id.index() < merge_dest.index())) {
							best_dist_km = d_km;
							merge_dest = oloc.id;
						}
					}
					if(merge_dest) {
						bool moved = false;
						if(controller.get_is_at_war()) {
							auto spath = province::make_safe_land_path(state, location.id, merge_dest, controller);
							if(!spath.empty()) {
								moved = military::set_army_path(state, ar.id, spath, controller);
							}
						} else {
							moved = military::move_army_ai(state, ar.id, merge_dest, controller);
						}
						if(moved) {
							ar.set_ai_province(merge_dest);
							ar.set_ai_activity(uint8_t(army_activity::merging));
							continue;
						}
					}
				}
			}

			// Split oversized stacks: either actively taking attrition above local supply limit, OR giant blobs parked in a high-SL hub
			// (e.g. 60K in New York) that are too big to fit onto any realistic front province without burning attrition!
			bool alone_over_local_sl = (reg_count > local_sl_regs);
			bool over_typical_cap = (reg_count >= std::max(10, supply_cap_regs + 2));
			bool giant_hub_blob = (reg_count >= std::max(14, supply_cap_regs + 4));
			if(reg_count >= 8
				&& (((alone_over_local_sl || over_typical_cap) && military::relative_attrition_amount(state, ar.id, location.id) >= 0.01f)
					|| giant_hub_blob)) {
				std::vector<dcon::regiment_id> front_regs;
				std::vector<dcon::regiment_id> back_regs;
				for(auto r : regs) {
					auto rid = r.get_regiment().id;
					if(ai_detail::is_backline_unit(state, controller, state.world.regiment_get_type(rid))) {
						back_regs.push_back(rid);
					} else {
						front_regs.push_back(rid);
					}
				}
				int32_t split_count = reg_count / 2;
				std::vector<dcon::regiment_id> to_split;
				to_split.reserve(size_t(split_count));
				// Prefer peeling excess pure infantry into a reserve stack while keeping artillery+cover intact
				int32_t keep_front_for_core = int32_t(back_regs.size()) > 0 ? std::min(int32_t(front_regs.size()), int32_t(back_regs.size()) + 2) : 0;
				int32_t surplus_front = std::max(0, int32_t(front_regs.size()) - keep_front_for_core);
				if(surplus_front >= 4) {
					int32_t take = std::min(surplus_front, std::max(4, split_count));
					for(int32_t k = 0; k < take; ++k) {
						to_split.push_back(front_regs[size_t(front_regs.size() - 1 - k)]);
					}
				} else {
					// Balanced half split
					int32_t take_back = int32_t(back_regs.size()) / 2;
					int32_t take_front = std::max(1, split_count - take_back);
					for(int32_t k = 0; k < take_back && k < int32_t(back_regs.size()); ++k) {
						to_split.push_back(back_regs[size_t(k)]);
					}
					for(int32_t k = 0; k < take_front && k < int32_t(front_regs.size()); ++k) {
						to_split.push_back(front_regs[size_t(k)]);
					}
				}
				if(int32_t(to_split.size()) >= 4 && reg_count - int32_t(to_split.size()) >= 4) {
					splits.push_back(split_request{ ar.id, controller.id, std::move(to_split) });
				}
			}
		}
	}

	// Execute army splits and empty-army cleanups safely outside in_army iteration
	for(auto const& req : splits) {
		if(!state.world.army_is_valid(req.army)
			|| req.to_split.empty()
			|| !military::can_split_army<command::actor::ai>(state, req.controller, req.army, req.to_split)) {
			continue;
		}
		bool all_regs_valid = true;
		for(auto rid : req.to_split) {
			if(!state.world.regiment_is_valid(rid) || state.world.regiment_get_army_from_army_membership(rid) != req.army) {
				all_regs_valid = false;
				break;
			}
		}
		if(all_regs_valid) {
			military::split_army<command::actor::ai>(state, req.controller, req.army, req.to_split, 0);
		}
	}
	for(auto empty_id : to_cleanup) {
		if(state.world.army_is_valid(empty_id)) {
			auto rem = state.world.army_get_army_membership(empty_id);
			if(rem.begin() == rem.end()) {
				military::cleanup_army(state, empty_id);
			}
		}
	}
}

void update_unit_supply_priorities_for_nation(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_is_valid(n)
		|| state.world.nation_get_is_player_controlled(n)
		|| state.world.nation_get_owned_province_count(n) == 0) {
		return;
	}

	// Always prioritize units actively fighting in battles
	military::set_supply_priority_for_armies_in_battle<command::actor::ai>(state, n, 1);
	military::set_supply_priority_for_navies_in_battle<command::actor::ai>(state, n, 1);

	bool at_war = state.world.nation_get_is_at_war(n);
	auto econ_mode = evaluate_war_economy_mode(state, n);
	bool prewar = (!at_war && econ_mode == war_economy_mode::prewar_ready);

	auto is_province_frontline = [&](dcon::province_id p) -> bool {
		if(!p || province::is_sea(state, p))
			return false;
		auto ctrl = state.world.province_get_nation_from_province_control(p);
		if(ctrl && military::are_at_war(state, n, ctrl))
			return true;
		if(!ctrl && state.world.province_get_rebel_faction_from_province_rebel_control(p))
			return true;
		for(auto ar : state.world.province_get_army_location(p)) {
			auto ac = ar.get_army().get_controller_from_army_control();
			if(!ac || military::are_at_war(state, n, ac))
				return true;
		}
		for(auto padj : state.world.province_get_province_adjacency(p)) {
			auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			if(province::is_sea(state, other))
				continue;
			auto octrl = state.world.province_get_nation_from_province_control(other);
			if(octrl && military::are_at_war(state, n, octrl))
				return true;
			if(!octrl && state.world.province_get_rebel_faction_from_province_rebel_control(other))
				return true;
			for(auto ar : state.world.province_get_army_location(other)) {
				auto ac = ar.get_army().get_controller_from_army_control();
				if(!ac || military::are_at_war(state, n, ac))
					return true;
			}
		}
		return false;
	};

	auto is_province_near_front = [&](dcon::province_id p) -> bool {
		if(!p || province::is_sea(state, p))
			return false;
		for(auto padj : state.world.province_get_province_adjacency(p)) {
			auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			if(!province::is_sea(state, other) && is_province_frontline(other))
				return true;
		}
		return false;
	};

	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!unit_on_ai_control(state, ar))
			continue;

		float total_str = 0.0f;
		float total_org = 0.0f;
		int32_t reg_count = 0;
		for(auto r : ar.get_army_membership()) {
			total_str += r.get_regiment().get_strength();
			total_org += r.get_regiment().get_org();
			++reg_count;
		}
		float avg_str = reg_count > 0 ? total_str / float(reg_count) : 1.0f;
		float avg_org = reg_count > 0 ? total_org / float(reg_count) : 1.0f;

		auto act = army_activity(ar.get_ai_activity());
		auto loc = ar.get_location_from_army_location().id;
		bool attacking_order = (act == army_activity::attacking
			|| act == army_activity::attack_gathered
			|| act == army_activity::transport_attack
			|| act == army_activity::attack_transport);

		military::unit_priority prio = military::unit_priority::normal_priority;
		if(ar.get_battle_from_army_battle_participation()) {
			prio = military::unit_priority::high_priority;
		} else if(at_war) {
			if(attacking_order || is_province_frontline(loc) || (ar.get_ai_province() && is_province_frontline(ar.get_ai_province()))) {
				prio = military::unit_priority::high_priority;
			} else if(ar.get_is_retreating() || avg_str < 0.90f || avg_org < 0.85f || is_province_near_front(loc)) {
				prio = military::unit_priority::normal_priority;
			} else {
				prio = military::unit_priority::low_priority;
			}
		} else {
			if(attacking_order || is_province_frontline(loc)) {
				prio = military::unit_priority::high_priority;
			} else if(prewar || avg_str < 0.95f || avg_org < 0.80f) {
				prio = military::unit_priority::normal_priority;
			} else {
				prio = military::unit_priority::low_priority;
			}
		}

		if(military::can_set_army_supply_priority<command::actor::ai>(state, n, ar.id, prio)) {
			military::set_army_supply_priority<command::actor::ai>(state, n, ar.id, prio);
		}
	}

	for(auto v : state.world.nation_get_navy_control(n)) {
		auto nv = v.get_navy();
		if(!unit_on_ai_control(state, nv))
			continue;

		float total_str = 0.0f;
		int32_t ship_count = 0;
		for(auto s : nv.get_navy_membership()) {
			total_str += s.get_ship().get_strength();
			++ship_count;
		}
		float avg_str = ship_count > 0 ? total_str / float(ship_count) : 1.0f;
		auto act = fleet_activity(nv.get_ai_activity());

		military::unit_priority prio = military::unit_priority::low_priority;
		if(nv.get_battle_from_navy_battle_participation()) {
			prio = military::unit_priority::high_priority;
		} else if(at_war && (act == fleet_activity::attacking || act == fleet_activity::boarding || act == fleet_activity::transporting || act == fleet_activity::unloading)) {
			prio = military::unit_priority::high_priority;
		} else if(at_war || prewar || avg_str < 0.95f) {
			prio = military::unit_priority::normal_priority;
		}

		if(military::can_set_navy_supply_priority<command::actor::ai>(state, n, nv.id, prio)) {
			military::set_navy_supply_priority<command::actor::ai>(state, n, nv.id, prio);
		}
	}
}

void update_unit_supply_priorities(sys::state& state) {
	for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
		dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
		update_unit_supply_priorities_for_nation(state, n);
	}
}

void manage_ai_leaders_for_nation(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_is_valid(n)
		|| state.world.nation_get_is_player_controlled(n)
		|| state.world.nation_get_owned_province_count(n) == 0
		|| nations::is_units_commanded_by_overlord(state, n)) {
		return;
	}

	auto no_bg = military::get_leader_background_wrapper(state, dcon::leader_id{});
	auto no_per = military::get_leader_personality_wrapper(state, dcon::leader_id{});
	float no_atk = state.world.leader_trait_get_attack(no_bg) + state.world.leader_trait_get_attack(no_per);
	float no_def = state.world.leader_trait_get_defense(no_bg) + state.world.leader_trait_get_defense(no_per);
	float no_org = state.world.leader_trait_get_organisation(no_bg) + state.world.leader_trait_get_organisation(no_per);
	float no_mor = state.world.leader_trait_get_morale(no_bg) + state.world.leader_trait_get_morale(no_per);
	float no_atk_score = no_atk * 3.0f + no_org * 2.0f + no_mor * 1.5f;
	float no_def_score = no_def * 3.0f + no_org * 2.0f + no_mor * 1.5f;
	float no_bal_score = (no_atk + no_def) * 1.5f + no_org * 2.0f + no_mor * 1.5f;

	auto score_leader = [&](dcon::leader_id l, bool want_attack, bool balanced) -> float {
		auto bg = military::get_leader_background_wrapper(state, l);
		auto per = military::get_leader_personality_wrapper(state, l);
		float atk = state.world.leader_trait_get_attack(bg) + state.world.leader_trait_get_attack(per);
		float def = state.world.leader_trait_get_defense(bg) + state.world.leader_trait_get_defense(per);
		float org = state.world.leader_trait_get_organisation(bg) + state.world.leader_trait_get_organisation(per);
		float mor = state.world.leader_trait_get_morale(bg) + state.world.leader_trait_get_morale(per);
		float spd = state.world.leader_trait_get_speed(bg) + state.world.leader_trait_get_speed(per);
		float rec = state.world.leader_trait_get_reconnaissance(bg) + state.world.leader_trait_get_reconnaissance(per);
		float pre = state.world.leader_get_prestige(l);
		if(balanced) {
			return (atk + def) * 1.5f + org * 2.0f + mor * 1.5f + spd * 0.3f + pre * 0.5f;
		}
		if(want_attack) {
			return atk * 3.0f + def * 0.35f + org * 2.0f + mor * 1.5f + spd * 0.5f + rec * 0.5f + pre * 0.5f;
		}
		return def * 3.0f + atk * 0.35f + org * 2.0f + mor * 1.5f + pre * 0.5f;
	};

	bool at_war = state.world.nation_get_is_at_war(n);

	struct army_slot {
		dcon::army_id id;
		float priority = 0.0f;
		bool want_attack = false;
		bool balanced = false;
	};
	std::vector<army_slot> swappable_armies;
	std::vector<dcon::leader_id> available_generals;

	auto is_army_swappable = [&](dcon::army_id a) -> bool {
		if(!a || !state.world.army_is_valid(a)
			|| state.world.army_get_controller_from_army_control(a) != n
			|| !unit_on_ai_control(state, a))
			return false;
		if(state.world.army_get_is_retreating(a)
			|| state.world.army_get_battle_from_army_battle_participation(a)
			|| state.world.army_get_navy_from_army_transport(a)) {
			return false;
		}
		auto loc = state.world.army_get_location_from_army_location(a);
		return loc && province::has_naval_access_to_province(state, n, loc);
	};

	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!is_army_swappable(ar.id))
			continue;
		int32_t regs = 0;
		int32_t art_regs = 0;
		for(auto r : ar.get_army_membership()) {
			auto utid = r.get_regiment().get_type();
			if(!utid)
				continue;
			++regs;
			auto utype = state.military_definitions.unit_base_definitions[utid].type;
			if(utype == military::unit_type::support || utype == military::unit_type::special) {
				++art_regs;
			}
		}
		if(regs == 0)
			continue;
		auto act = army_activity(ar.get_ai_activity());
		bool offensive = (act == army_activity::attacking
			|| act == army_activity::attack_gathered
			|| act == army_activity::transport_attack
			|| act == army_activity::attack_transport);

		bool on_front = false;
		auto loc = ar.get_location_from_army_location().id;
		if(at_war && loc) {
			for(auto padj : state.world.province_get_province_adjacency(loc)) {
				auto other = padj.get_connected_provinces(0) == loc ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				auto octrl = state.world.province_get_nation_from_province_control(other);
				if(octrl && military::are_at_war(state, n, octrl)) {
					on_front = true;
					break;
				}
			}
		}

		float prio = float(regs) * (art_regs > 0 ? 1.35f : 1.0f);
		if(offensive) {
			prio *= 1.6f;
		} else if(on_front) {
			prio *= 1.4f;
		}
		if(regs == 1) {
			prio *= 0.25f;
		}
		swappable_armies.push_back(army_slot{ ar.id, prio, offensive, !at_war && !offensive });
	}

	for(auto ll : state.world.nation_get_leader_loyalty(n)) {
		auto l = ll.get_leader();
		if(l.get_is_admiral())
			continue;
		auto assigned_army = l.get_army_from_army_leadership().id;
		if(!assigned_army) {
			available_generals.push_back(l.id);
		} else if(is_army_swappable(assigned_army)) {
			available_generals.push_back(l.id);
		}
	}

	if(!swappable_armies.empty() && !available_generals.empty()) {
		// Detach generals from swappable armies so optimal deterministic reassignment has zero conflicts
		for(auto const& slot : swappable_armies) {
			state.world.army_set_general_from_army_leadership(slot.id, dcon::leader_id{});
		}

		std::sort(swappable_armies.begin(), swappable_armies.end(), [](army_slot const& a, army_slot const& b) {
			if(a.priority != b.priority)
				return a.priority > b.priority;
			return a.id.index() < b.id.index();
		});

		std::sort(available_generals.begin(), available_generals.end(), [](dcon::leader_id a, dcon::leader_id b) {
			return a.index() < b.index();
		});

		std::vector<uint8_t> used(available_generals.size(), 0);
		for(auto const& slot : swappable_armies) {
			float baseline = slot.balanced ? no_bal_score : (slot.want_attack ? no_atk_score : no_def_score);
			int32_t best_idx = -1;
			float best_score = baseline; // strictly avoid generals worse than no-leader for this role
			for(size_t i = 0; i < available_generals.size(); ++i) {
				if(used[i])
					continue;
				float s = score_leader(available_generals[i], slot.want_attack, slot.balanced);
				if(s > best_score) {
					best_score = s;
					best_idx = int32_t(i);
				}
			}
			if(best_idx >= 0) {
				used[size_t(best_idx)] = 1;
				state.world.army_set_general_from_army_leadership(slot.id, available_generals[size_t(best_idx)]);
			}
		}
	} else if(!swappable_armies.empty()) {
		// Even with no spare generals, strip any poison general strictly worse than no-leader
		for(auto const& slot : swappable_armies) {
			auto cur = state.world.army_get_general_from_army_leadership(slot.id);
			if(cur) {
				float baseline = slot.balanced ? no_bal_score : (slot.want_attack ? no_atk_score : no_def_score);
				if(score_leader(cur, slot.want_attack, slot.balanced) < baseline) {
					state.world.army_set_general_from_army_leadership(slot.id, dcon::leader_id{});
				}
			}
		}
	}

	// Same deterministic pool assignment for admirals on swappable combat navies
	struct navy_slot {
		dcon::navy_id id;
		float priority = 0.0f;
	};
	std::vector<navy_slot> swappable_navies;
	std::vector<dcon::leader_id> available_admirals;

	auto is_navy_swappable = [&](dcon::navy_id v) -> bool {
		if(!v || !state.world.navy_is_valid(v)
			|| state.world.navy_get_controller_from_navy_control(v) != n
			|| !unit_on_ai_control(state, v))
			return false;
		if(state.world.navy_get_is_retreating(v) || state.world.navy_get_battle_from_navy_battle_participation(v))
			return false;
		auto loc = state.world.navy_get_location_from_navy_location(v);
		return loc && province::has_naval_access_to_province(state, n, loc);
	};

	for(auto v : state.world.nation_get_navy_control(n)) {
		auto nv = v.get_navy();
		if(!is_navy_swappable(nv.id))
			continue;
		float combat_score = 0.0f;
		int32_t ships = 0;
		for(auto s : nv.get_navy_membership()) {
			auto utid = s.get_ship().get_type();
			if(!utid)
				continue;
			++ships;
			auto utype = state.military_definitions.unit_base_definitions[utid].type;
			combat_score += (utype == military::unit_type::transport) ? 0.25f : 1.0f;
		}
		if(ships > 0) {
			swappable_navies.push_back(navy_slot{ nv.id, combat_score });
		}
	}

	for(auto ll : state.world.nation_get_leader_loyalty(n)) {
		auto l = ll.get_leader();
		if(!l.get_is_admiral())
			continue;
		auto assigned_navy = l.get_navy_from_navy_leadership().id;
		if(!assigned_navy || is_navy_swappable(assigned_navy)) {
			available_admirals.push_back(l.id);
		}
	}

	if(!swappable_navies.empty() && !available_admirals.empty()) {
		for(auto const& slot : swappable_navies) {
			state.world.navy_set_admiral_from_navy_leadership(slot.id, dcon::leader_id{});
		}
		std::sort(swappable_navies.begin(), swappable_navies.end(), [](navy_slot const& a, navy_slot const& b) {
			if(a.priority != b.priority)
				return a.priority > b.priority;
			return a.id.index() < b.id.index();
		});
		std::sort(available_admirals.begin(), available_admirals.end(), [](dcon::leader_id a, dcon::leader_id b) {
			return a.index() < b.index();
		});
		std::vector<uint8_t> used(available_admirals.size(), 0);
		for(auto const& slot : swappable_navies) {
			int32_t best_idx = -1;
			float best_score = no_bal_score;
			for(size_t i = 0; i < available_admirals.size(); ++i) {
				if(used[i])
					continue;
				float s = score_leader(available_admirals[i], false, true);
				if(s > best_score) {
					best_score = s;
					best_idx = int32_t(i);
				}
			}
			if(best_idx >= 0) {
				used[size_t(best_idx)] = 1;
				state.world.navy_set_admiral_from_navy_leadership(slot.id, available_admirals[size_t(best_idx)]);
			}
		}
	}
}

void manage_ai_leaders(sys::state& state) {
	for(auto n : state.world.in_nation) {
		manage_ai_leaders_for_nation(state, n.id);
	}
}

void update_mobilization_for_nation(sys::state& state, dcon::nation_id nid) {
	if(!state.world.nation_is_valid(nid)
		|| state.world.nation_get_is_player_controlled(nid)
		|| state.world.nation_get_owned_province_count(nid) == 0) {
		return;
	}

	auto n = fatten(state.world, nid);
	if(!n.get_is_at_war()) {
		if(n.get_is_mobilized()) {
			// Never demobilize while any army is still fighting an active battle (e.g. rebels or mid-cleanup war)
			for(auto ar : state.world.nation_get_army_control(nid)) {
				if(ar.get_army().get_battle_from_army_battle_participation()) {
					return;
				}
			}
			military::end_mobilization(state, nid);
			std::vector<dcon::army_id> empty_armies;
			for(auto ar : state.world.nation_get_army_control(nid)) {
				auto a = ar.get_army();
				auto regs = a.get_army_membership();
				if(regs.begin() == regs.end() && !a.get_battle_from_army_battle_participation()) {
					empty_armies.push_back(a.id);
				}
			}
			for(auto aid : empty_armies) {
				if(state.world.army_is_valid(aid)) {
					auto rem = state.world.army_get_army_membership(aid);
					if(rem.begin() == rem.end()) {
						military::cleanup_army(state, aid);
					}
				}
			}
		}
		return;
	}

	if(n.get_is_mobilized()) {
		return;
	}

	auto disarm = n.get_disarmed_until();
	if(disarm && state.current_date < disarm) {
		return;
	}

	if(military::mobilization_size(state, nid) <= 0.0f
		|| military::mobilization_impact(state, nid) <= 0.0f
		|| military::mobilized_regiments_pop_limit(state, nid) <= 0) {
		return;
	}

	float own_str = std::max(1.0f, estimate_strength(state, nid));
	float friendly_str = own_str;
	float enemy_str = 0.0f;
	bool serious_enemy_mobilized = false;

	std::vector<dcon::nation_id> counted_allies;
	std::vector<dcon::nation_id> counted_enemies;
	counted_allies.push_back(nid);

	for(auto wp : state.world.nation_get_war_participant(nid)) {
		bool we_attack = wp.get_is_attacker();
		for(auto par : wp.get_war().get_war_participant()) {
			auto other = par.get_nation().id;
			if(par.get_is_attacker() == we_attack) {
				if(std::find(counted_allies.begin(), counted_allies.end(), other) == counted_allies.end()) {
					counted_allies.push_back(other);
					friendly_str += estimate_strength(state, other);
				}
			} else {
				if(std::find(counted_enemies.begin(), counted_enemies.end(), other) == counted_enemies.end()) {
					counted_enemies.push_back(other);
					float estr = estimate_strength(state, other);
					enemy_str += estr;
					if(state.world.nation_get_is_mobilized(other) && estr >= own_str * 0.40f) {
						serious_enemy_mobilized = true;
					}
				}
			}
		}
	}

	bool core_occupied = false;
	for(auto po : state.world.nation_get_province_ownership(nid)) {
		auto p = po.get_province();
		if(!p.get_is_colonial() && p.get_is_owner_core()) {
			auto ctrl = p.get_nation_from_province_control();
			if(ctrl && ctrl != nid && military::are_at_war(state, nid, ctrl)) {
				core_occupied = true;
				break;
			}
		}
	}

	if(enemy_str >= friendly_str * 0.70f
		|| serious_enemy_mobilized
		|| (core_occupied && enemy_str >= friendly_str * 0.35f)) {
		military::start_mobilization(state, nid);
	}
}

void update_mobilization(sys::state& state) {
	for(auto n : state.world.in_nation) {
		update_mobilization_for_nation(state, n.id);
	}
}

void on_war_state_changed(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_is_valid(n)
		|| state.world.nation_get_is_player_controlled(n)
		|| state.world.nation_get_owned_province_count(n) == 0) {
		return;
	}
	update_budget_for_nation(state, n, false);
	update_stockpile_targets_for_nation(state, n);
	update_unit_supply_priorities_for_nation(state, n);
	manage_ai_leaders_for_nation(state, n);
	update_mobilization_for_nation(state, n);
}

void manage_active_battles(sys::state& state) {
	if(state.cheat_data.disable_ai)
		return;

	// Collect battle IDs first because retreating the last army on one side calls end_battle() and deletes the battle!
	std::vector<dcon::land_battle_id> battles;
	for(auto b : state.world.in_land_battle) {
		battles.push_back(b.id);
	}
	if(battles.empty())
		return;

	// Shared daily caches across all battles so two adjacent battles NEVER strip the same 1st-line province!
	std::vector<std::pair<dcon::nation_id, ai_front_helpers::front_pin_tracker>> nation_pin_trackers;
	auto get_pin_tracker = [&](dcon::nation_id n) -> ai_front_helpers::front_pin_tracker& {
		for(auto& kv : nation_pin_trackers) {
			if(kv.first == n)
				return kv.second;
		}
		nation_pin_trackers.push_back({ n, ai_front_helpers::front_pin_tracker{} });
		nation_pin_trackers.back().second.init_for_nation(state, n);
		return nation_pin_trackers.back().second;
	};

	std::vector<std::pair<dcon::nation_id, ai_front_helpers::walkable_theater_map>> nation_theater_maps;
	auto get_theater_map = [&](dcon::nation_id n) -> ai_front_helpers::walkable_theater_map const& {
		for(auto const& kv : nation_theater_maps) {
			if(kv.first == n)
				return kv.second;
		}
		nation_theater_maps.push_back({ n, ai_front_helpers::walkable_theater_map{} });
		nation_theater_maps.back().second.build(state, n);
		return nation_theater_maps.back().second;
	};

	struct theater_mode_key {
		dcon::nation_id n;
		uint16_t theater_id;
		bool continuous_front;
	};
	std::vector<theater_mode_key> theater_modes;
	auto is_continuous_front = [&](dcon::nation_id n, ai_front_helpers::walkable_theater_map const& tmap, uint16_t tid) -> bool {
		if(tid == 0)
			return false;
		for(auto const& tm : theater_modes) {
			if(tm.n == n && tm.theater_id == tid)
				return tm.continuous_front;
		}
		bool cf = false;
		if(state.world.nation_get_is_at_war(n)) {
			std::vector<dcon::nation_id> war_allies;
			std::vector<dcon::nation_id> war_enemies;
			ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
			auto ts = ai_front_helpers::evaluate_theater_summary(state, n, tmap, tid, war_allies, war_enemies, false);
			cf = ts.continuous_front;
		}
		theater_modes.push_back(theater_mode_key{ n, tid, cf });
		return cf;
	};

	for(auto b : battles) {
		if(!state.world.land_battle_is_valid(b))
			continue;

		auto loc = state.world.land_battle_get_location_from_land_battle_location(b);
		if(!loc)
			continue;

		int32_t cw = int32_t(state.world.land_battle_get_combat_width(b));
		if(cw < int32_t(military::min_combat_width)) {
			cw = int32_t(military::min_combat_width);
		}
		bool retreatable = military::is_battle_retreatable(state, b);

		std::vector<dcon::army_id> att_armies;
		std::vector<dcon::army_id> def_armies;
		std::vector<dcon::nation_id> att_ai_nations;
		std::vector<dcon::nation_id> def_ai_nations;

		for(auto part : state.world.land_battle_get_army_battle_participation(b)) {
			auto ar = part.get_army();
			if(!ar)
				continue;
			bool is_att = military::is_attacker_in_battle(state, ar.id);
			auto ctrl = ar.get_controller_from_army_control().id;
			if(is_att) {
				att_armies.push_back(ar.id);
				if(ctrl && !state.world.nation_get_is_player_controlled(ctrl) && unit_on_ai_control(state, ar.id)) {
					if(std::find(att_ai_nations.begin(), att_ai_nations.end(), ctrl) == att_ai_nations.end()) {
						att_ai_nations.push_back(ctrl);
					}
				}
			} else {
				def_armies.push_back(ar.id);
				if(ctrl && !state.world.nation_get_is_player_controlled(ctrl) && unit_on_ai_control(state, ar.id)) {
					if(std::find(def_ai_nations.begin(), def_ai_nations.end(), ctrl) == def_ai_nations.end()) {
						def_ai_nations.push_back(ctrl);
					}
				}
			}
		}

		if(att_ai_nations.empty() && def_ai_nations.empty())
			continue;

		// Inspect current battle slots & reserves directly from military.cpp state
		auto& att_front_slots = state.world.land_battle_get_attacker_front_line(b);
		auto& def_front_slots = state.world.land_battle_get_defender_front_line(b);
		auto& att_back_slots = state.world.land_battle_get_attacker_back_line(b);
		auto& def_back_slots = state.world.land_battle_get_defender_back_line(b);
		auto reserves = state.world.land_battle_get_reserves(b);

		int32_t att_healthy_front = 0, def_healthy_front = 0;
		int32_t att_healthy_back = 0, def_healthy_back = 0;
		int32_t att_exposed_back = 0, def_exposed_back = 0;

		auto inspect_slot = [&](military::battle_regiment br, bool is_front_slot, bool is_att) {
			if(!br.regiment || !state.world.regiment_is_valid(br.regiment))
				return;
			float str = state.world.regiment_get_strength(br.regiment);
			float org = state.world.regiment_get_org(br.regiment);
			if(str <= 0.05f || org < state.defines.alice_reg_deploy_from_reserve_org)
				return;
			auto army = state.world.regiment_get_army_from_army_membership(br.regiment);
			auto tech_n = military::tech_nation_for_army(state, army);
			bool back_unit = ai_detail::is_backline_unit(state, tech_n, state.world.regiment_get_type(br.regiment));
			if(is_att) {
				if(back_unit) {
					++att_healthy_back;
					if(is_front_slot) ++att_exposed_back;
				} else {
					++att_healthy_front;
				}
			} else {
				if(back_unit) {
					++def_healthy_back;
					if(is_front_slot) ++def_exposed_back;
				} else {
					++def_healthy_front;
				}
			}
		};

		for(int32_t i = 0; i < cw && i < int32_t(military::max_combat_width); ++i) {
			inspect_slot(att_front_slots[size_t(i)], true, true);
			inspect_slot(def_front_slots[size_t(i)], true, false);
			inspect_slot(att_back_slots[size_t(i)], false, true);
			inspect_slot(def_back_slots[size_t(i)], false, false);
		}
		for(uint32_t i = 0; i < reserves.size(); ++i) {
			auto br = reserves.at(i);
			inspect_slot(br, false, br.get_is_attacking());
		}

		// Also include armies already marching directly into this battle province
		auto lead_att = military::get_land_battle_lead_attacker(state, b);
		auto lead_def = military::get_land_battle_lead_defender(state, b);
		std::vector<dcon::army_id> att_with_enroute = att_armies;
		std::vector<dcon::army_id> def_with_enroute = def_armies;
		int32_t att_enroute_front = 0, def_enroute_front = 0;
		int32_t att_enroute_back = 0, def_enroute_back = 0;

		for(auto padj : state.world.province_get_province_adjacency(loc)) {
			auto nb = padj.get_connected_provinces(0) == loc ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			if(province::is_sea(state, nb))
				continue;
			for(auto ar : state.world.province_get_army_location(nb)) {
				auto army = ar.get_army();
				if(army.get_is_retreating() || army.get_battle_from_army_battle_participation())
					continue;
				auto path = army.get_path();
				if(path.size() == 0 || path.at(path.size() - 1) != loc)
					continue;
				auto ctrl = army.get_controller_from_army_control().id;
				if(!ctrl)
					continue;
				auto sc = ai_detail::classify_army_composition(state, ctrl, army.id);
				if(ctrl == lead_att || (lead_att && military::are_on_same_side_in_common_war(state, ctrl, lead_att))) {
					att_with_enroute.push_back(army.id);
					att_enroute_front += sc.frontline;
					att_enroute_back += sc.backline;
				} else if(ctrl == lead_def || (lead_def && military::are_on_same_side_in_common_war(state, ctrl, lead_def))) {
					def_with_enroute.push_back(army.id);
					def_enroute_front += sc.frontline;
					def_enroute_back += sc.backline;
				}
			}
		}

		auto base_eval = ai_battle_eval::evaluate_battle_outcome(state, att_with_enroute, def_with_enroute, loc, military::crossing_type::none);

		auto process_side = [&](bool side_is_att) {
			auto const& ai_nations = side_is_att ? att_ai_nations : def_ai_nations;
			if(ai_nations.empty())
				return;
			auto const& our_armies = side_is_att ? att_armies : def_armies;
			auto const& enemy_armies = side_is_att ? def_armies : att_armies;
			int32_t our_front = side_is_att ? (att_healthy_front + att_enroute_front) : (def_healthy_front + def_enroute_front);
			int32_t our_back = side_is_att ? (att_healthy_back + att_enroute_back) : (def_healthy_back + def_enroute_back);
			int32_t our_exposed = side_is_att ? att_exposed_back : def_exposed_back;
			float our_ratio = side_is_att ? base_eval.win_ratio : (1.0f / std::max(0.01f, base_eval.win_ratio));

			// Compute average org of our armies vs enemy armies in the battle
			auto avg_org_of = [&](std::vector<dcon::army_id> const& list) -> float {
				float o = 0.0f;
				int32_t c = 0;
				for(auto a : list) {
					if(!state.world.army_is_valid(a)) continue;
					for(auto r : state.world.army_get_army_membership(a)) {
						o += r.get_regiment().get_org();
						++c;
					}
				}
				return c > 0 ? o / float(c) : 0.0f;
			};
			float our_avg_org = avg_org_of(our_armies);
			float enemy_avg_org = avg_org_of(enemy_armies);
			int32_t enemy_front = side_is_att ? (def_healthy_front + def_enroute_front) : (att_healthy_front + att_enroute_front);
			int32_t enemy_back = side_is_att ? (def_healthy_back + def_enroute_back) : (att_healthy_back + att_enroute_back);
			int32_t enemy_total_in_battle = enemy_front + enemy_back;
			// Effective needed battle width scales with ACTUAL enemy size (never call 150k against 12k just because 1836 CW == 30!)
			int32_t effective_cw = std::min(cw, std::max(4, enemy_front + 4));

			// 1. Gather nearby reachable AI reserves (1-2 marches away) for potential reinforcement
			struct reinf_cand {
				dcon::army_id id;
				dcon::nation_id ctrl;
				dcon::province_id loc;
				float days;
				int32_t front_regs;
				int32_t back_regs;
				int32_t total_regs;
				int32_t dig_in;
				bool is_core;
				bool continuous_front;
				float priority;
			};
			std::vector<reinf_cand> reserves_pool;

			float const battle_km_per_day = std::max(5.0f, 4.0f * state.defines.alice_army_marching_hours_per_day);
			for(auto n : ai_nations) {
				auto const& tmap = get_theater_map(n);
				uint16_t loc_theater = tmap.theater_of(loc);
				if(loc_theater == 0)
					continue;
				bool cont_front = is_continuous_front(n, tmap, loc_theater);
				auto const& tracker = get_pin_tracker(n);
				float max_gather_km = cont_front ? 450.0f : 650.0f;
				float max_days = cont_front ? 22.0f : 26.0f;

				for(auto ar : state.world.nation_get_army_control(n)) {
					auto army = ar.get_army();
					if(!unit_on_ai_control(state, army))
						continue;
					if(army.get_battle_from_army_battle_participation()
						|| army.get_navy_from_army_transport()
						|| army.get_black_flag()
						|| army.get_is_retreating()
						|| !army_ready_for_battle(state, n, army))
						continue;
					auto aloc = army.get_location_from_army_location().id;
					if(!aloc || aloc == loc || tmap.theater_of(aloc) != loc_theater)
						continue;
					// Skip armies already marching directly to this battle
					auto apath = army.get_path();
					if(apath.size() > 0 && apath.at(0) == loc)
						continue;

					// Quick bounding filter in real kilometers before checking march time
					float dist_km = province::direct_distance_km(state, aloc, loc);
					if(dist_km > max_gather_km)
						continue;

					bool adj_to_battle = province::provinces_are_adjacent(state, aloc, loc);
					float est_days = adj_to_battle
						? std::max(2.0f, military::movement_time_from_to(state, army.id, aloc, loc))
						: std::max(5.0f, dist_km / battle_km_per_day);
					if(est_days > max_days)
						continue;

					auto sc = ai_detail::classify_army_composition(state, n, army.id);
					if(sc.total == 0)
						continue;
					bool is_core = (sc.backline > 0);
					int32_t dig_in = int32_t(army.get_dig_in());

					// WWI continuous front: enforce dynamic pinning (never strip threatened 1st-line sectors or dug-in core stacks!)
					// Austerlitz (!cont_front): converge into decisive battles, but NEVER abandon a larger main enemy army for a small rear raid!
					if(!tracker.can_pull(state, n, aloc, sc.total, dig_in, is_core, loc, cont_front))
						continue;

					float enemy_facing = ai_front_helpers::adjacent_enemy_regs_excluding(state, n, aloc, loc);
					if(!cont_front && enemy_facing > float(std::max(4, enemy_total_in_battle)) * 1.25f)
						continue;

					// Strong 2.2x priority for free 2nd-line reserves over surplus 1st-line stacks!
					float reserve_mult = (enemy_facing <= 0.1f) ? 2.20f : 1.0f;

					// Synergy with Stage 2: if backline is already covered, strongly prefer feeding pure infantry reserves!
					float role_bonus = 1.0f;
					if(our_back >= int32_t(float(effective_cw) * 0.8f)) {
						role_bonus = is_core ? 0.65f : 1.50f;
					} else if(our_back < int32_t(float(effective_cw) * 0.6f)) {
						role_bonus = is_core ? 1.45f : 1.0f;
					}
					float prio = (float(sc.frontline) + float(sc.backline) * 0.5f) * role_bonus * reserve_mult / (est_days + 3.0f);
					reserves_pool.push_back(reinf_cand{ army.id, n, aloc, est_days, sc.frontline, sc.backline, sc.total, dig_in, is_core, cont_front, prio });
				}
			}

			std::sort(reserves_pool.begin(), reserves_pool.end(), [](reinf_cand const& a, reinf_cand const& b) {
				if(a.priority != b.priority)
					return a.priority > b.priority;
				return a.id.index() < b.id.index();
			});

			// Evaluate whether nearby reserves are actually needed against THIS enemy force
			int32_t target_front = effective_cw + std::max(2, effective_cw / 3);
			bool needs_front_cover = (our_front < target_front) || (our_exposed > 0);
			bool contested = (our_ratio >= 0.50f && our_ratio < 1.40f);
			bool winning_comfortably = (our_ratio >= 1.60f && our_front >= effective_cw + 2 && our_exposed == 0)
				|| (our_ratio >= 2.20f && our_front >= enemy_front + 3 && our_exposed == 0);

			if(!winning_comfortably && !reserves_pool.empty() && (needs_front_cover || contested || our_ratio < 0.50f)) {
				std::vector<dcon::army_id> sim_att = att_with_enroute;
				std::vector<dcon::army_id> sim_def = def_with_enroute;
				std::vector<reinf_cand> to_send;

				int32_t sim_front = our_front;
				float sim_ratio = our_ratio;
				for(auto const& rc : reserves_pool) {
					if(to_send.size() >= 4)
						break;
					// Check dynamic pinning accounting for stacks already selected in to_send from the same province!
					if(rc.continuous_front) {
						auto const& tracker = get_pin_tracker(rc.ctrl);
						int32_t extra_regs = 0;
						int32_t extra_stacks = 0;
						for(auto const& prev : to_send) {
							if(prev.ctrl == rc.ctrl && prev.loc == rc.loc) {
								extra_regs += prev.total_regs;
								extra_stacks += 1;
							}
						}
						if(!tracker.can_pull(state, rc.ctrl, rc.loc, rc.total_regs, rc.dig_in, rc.is_core, loc, true, extra_regs, extra_stacks))
							continue;
					}

					if(side_is_att) sim_att.push_back(rc.id);
					else sim_def.push_back(rc.id);
					to_send.push_back(rc);
					sim_front += rc.front_regs;

					auto ev = ai_battle_eval::evaluate_battle_outcome(state, sim_att, sim_def, loc, military::crossing_type::none);
					sim_ratio = side_is_att ? ev.win_ratio : (1.0f / std::max(0.01f, ev.win_ratio));
					if((sim_ratio >= 1.35f && sim_front >= target_front) || (sim_ratio >= 2.00f && sim_front >= effective_cw)) {
						break;
					}
				}

				// Only commit reserves if they actually give us a viable chance (don't trickle-feed into a hopeless slaughter!)
				if(!to_send.empty() && sim_ratio >= 0.72f) {
					for(auto const& rc : to_send) {
						state.world.army_set_special_order(rc.id, uint8_t(military::special_army_order::none));
						if(military::move_army_ai(state, rc.id, loc, rc.ctrl, true)) {
							get_pin_tracker(rc.ctrl).commit_pull(rc.loc, rc.total_regs);
							if(side_is_att) att_with_enroute.push_back(rc.id);
							else def_with_enroute.push_back(rc.id);
						}
					}
					return; // Reinforced — stay and fight!
				}
			}

			// 2. Retreat before wipe if battle is retreatable and decisively lost with no viable relief!
			if(retreatable && our_ratio < 0.45f) {
				bool frontline_collapsed = (our_front < std::max(1, cw / 3)) || (our_exposed >= 2);
				bool org_crushed = (our_avg_org < 0.25f && enemy_avg_org > 0.50f);
				bool hopeless_odds = (our_ratio < 0.30f && our_avg_org < 0.45f);
				if(frontline_collapsed || org_crushed || hopeless_odds) {
					// Collect our AI armies to retreat safely (retreat(..., true) may end & delete the battle!)
					std::vector<std::pair<dcon::army_id, dcon::nation_id>> to_retreat;
					for(auto a : our_armies) {
						if(!state.world.army_is_valid(a))
							continue;
						auto ctrl = state.world.army_get_controller_from_army_control(a);
						if(ctrl && !state.world.nation_get_is_player_controlled(ctrl) && unit_on_ai_control(state, a)) {
							to_retreat.push_back({ a, ctrl });
						}
					}
					for(auto [a, ctrl] : to_retreat) {
						if(!state.world.land_battle_is_valid(b))
							break;
						if(!state.world.army_is_valid(a) || state.world.army_get_battle_from_army_battle_participation(a) != b)
							continue;
						auto rpath = province::make_land_auto_retreat_path(state, ctrl, loc);
						if(!rpath.empty()) {
							military::retreat(state, a, rpath, true);
							if(state.world.army_is_valid(a)) {
								state.world.army_set_moving_to_merge(a, false);
								state.world.army_set_ai_activity(a, uint8_t(army_activity::on_guard));
								state.world.army_set_ai_province(a, dcon::province_id{});
							}
						}
					}
				}
			}
		};

		process_side(true);
		if(state.world.land_battle_is_valid(b)) {
			process_side(false);
		}
	}
}

// Fast wartime merge for detached reinforcement brigades arriving at their target front sector or staging fist
void merge_arrived_reinforcements(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_is_valid(n)
		|| state.world.nation_get_is_player_controlled(n)
		|| state.world.nation_get_owned_province_count(n) == 0) {
		return;
	}

	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	int32_t nation_cw = ai_detail::estimate_nation_base_combat_width(state, n);
	int32_t supply_cap_regs = ai_detail::typical_supply_regiment_cap(state, n);
	int32_t core_backline_target = std::clamp(std::min(nation_cw <= 14 ? nation_cw : 5, supply_cap_regs / 2), 4, 10);

	std::vector<dcon::army_id> to_cleanup;
	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!unit_on_ai_control(state, ar)
			|| army_activity(ar.get_ai_activity()) != army_activity::merging
			|| ar.get_arrival_time()
			|| ar.get_battle_from_army_battle_participation()
			|| ar.get_navy_from_army_transport()
			|| ar.get_is_retreating()) {
			continue;
		}
		auto loc = ar.get_location_from_army_location();
		if(!loc || province::is_sea(state, loc))
			continue;
		auto regs = ar.get_army_membership();
		if(regs.begin() == regs.end()) {
			to_cleanup.push_back(ar.id);
			continue;
		}

		auto my_sc = ai_detail::classify_army_composition(state, n, ar.id);
		int32_t local_sl_regs = std::max(1, int32_t(float(military::supply_limit_in_province(state, n, loc.id)) / reg_weight));
		int32_t local_cap = local_sl_regs;

		dcon::army_id best_host;
		int32_t best_tier = 0;
		for(auto o : loc.get_army_location()) {
			auto other = o.get_army();
			if(other.id == ar.id
				|| other.get_controller_from_army_control() != n
				|| other.get_battle_from_army_battle_participation()
				|| other.get_navy_from_army_transport()
				|| other.get_is_retreating()) {
				continue;
			}
			auto oact = army_activity(other.get_ai_activity());
			if(oact != army_activity::on_guard && oact != army_activity::attacking)
				continue;
			auto hsc = ai_detail::classify_army_composition(state, n, other.id);
			bool art_covered = (my_sc.backline == 0) || (hsc.backline + my_sc.backline <= hsc.frontline + my_sc.frontline);
			if(hsc.total == 0
				|| hsc.total + my_sc.total > local_cap
				|| hsc.backline + my_sc.backline > core_backline_target
				|| !art_covered) {
				continue;
			}
			int32_t tier = (oact == army_activity::attacking) ? 5
				: ((hsc.backline > 0 && hsc.frontline < hsc.backline + 2 && my_sc.frontline > 0) ? 4 : 2);
			if(tier > best_tier || (tier == best_tier && best_host && other.id.index() < best_host.index())) {
				best_tier = tier;
				best_host = other.id;
			}
		}

		if(best_host) {
			std::vector<dcon::regiment_id> to_move;
			for(auto r : regs) {
				to_move.push_back(r.get_regiment().id);
			}
			for(auto rid : to_move) {
				state.world.regiment_set_army_from_army_membership(rid, best_host);
			}
			to_cleanup.push_back(ar.id);
		} else if(ar.get_ai_province() == loc) {
			ar.set_ai_activity(uint8_t(army_activity::on_guard));
			ar.set_ai_province(dcon::province_id{});
		}
	}

	for(auto empty_id : to_cleanup) {
		if(state.world.army_is_valid(empty_id)) {
			auto rem = state.world.army_get_army_membership(empty_id);
			if(rem.begin() == rem.end()) {
				military::cleanup_army(state, empty_id);
			}
		}
	}
}

// Precision wartime detachment via military::split_army<actor::ai> (SEQUENTIAL ONLY — calls create_army()!):
// Detaches 1..K surplus infantry brigades from nearby safe/overstocked stacks (keeping donor dig_in, general, and artillery cover intact!)
// and routes them along safe friendly paths to reinforce deficit front sectors or staging offensive fists.
void ai_split_surplus_regiments(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_is_valid(n)
		|| state.world.nation_get_is_player_controlled(n)
		|| state.world.nation_get_owned_province_count(n) == 0
		|| !state.world.nation_get_is_at_war(n)) {
		return;
	}

	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n);

	struct prov_state {
		dcon::province_id p;
		uint16_t theater_id = 0;
		int32_t present_regs = 0;
		int32_t incoming_regs = 0;
		int32_t present_front = 0;
		int32_t present_back = 0;
		int32_t supply_cap_regs = 0;
		int32_t cw = 10;
		float adj_enemy_regs = 0.0f;
		bool has_staging_attack = false;
	};
	std::vector<prov_state> provs;

	auto get_or_add_prov = [&](dcon::province_id p) -> size_t {
		for(size_t i = 0; i < provs.size(); ++i) {
			if(provs[i].p == p)
				return i;
		}
		int32_t sl = military::supply_limit_in_province(state, n, p);
		int32_t scap = std::max(0, int32_t(float(sl) / reg_weight));
		// Account for allied/non-n armies already occupying p (local_army_weight sums ALL non-blackflag non-retreating armies!)
		int32_t other_regs = 0;
		for(auto oa : state.world.province_get_army_location(p)) {
			auto oarmy = oa.get_army();
			if(oarmy.get_black_flag() || oarmy.get_is_retreating() || oarmy.get_navy_from_army_transport() || oarmy.get_battle_from_army_battle_participation())
				continue;
			if(oarmy.get_controller_from_army_control() == n && unit_on_ai_control(state, oarmy))
				continue; // counted directly in the army_control loop below
			auto orng = oarmy.get_army_membership();
			other_regs += int32_t(orng.end() - orng.begin());
		}
		provs.push_back(prov_state{
			p,
			tmap.theater_of(p),
			other_regs,
			0,
			0,
			0,
			scap,
			ai_front_helpers::prov_cw(state, n, p),
			ai_front_helpers::adjacent_enemy_regs_excluding(state, n, p, dcon::province_id{}),
			false
		});
		return provs.size() - 1;
	};

	struct inf_reg_entry {
		dcon::regiment_id id;
		float readiness;
	};
	struct donor_stack {
		dcon::army_id id;
		dcon::province_id loc;
		uint16_t theater_id = 0;
		size_t loc_prov_idx = 0;
		int32_t total = 0;
		int32_t frontline = 0;
		int32_t backline = 0;
		std::vector<inf_reg_entry> pure_inf;
	};
	std::vector<donor_stack> donors;

	// Pre-collect all province counts & safe donor stacks BEFORE calling split_army (iterator-safe!)
	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!unit_on_ai_control(state, ar)
			|| ar.get_is_retreating()
			|| ar.get_black_flag()
			|| ar.get_navy_from_army_transport()) {
			continue;
		}
		auto loc = ar.get_location_from_army_location().id;
		if(!loc || province::is_sea(state, loc))
			continue;

		auto sc = ai_detail::classify_army_composition(state, n, ar.id);
		if(sc.total == 0)
			continue;

		auto act = army_activity(ar.get_ai_activity());
		auto path = ar.get_path();
		if(ar.get_arrival_time() && path.size() > 0) {
			dcon::province_id dest = path.at(0);
			if(dest && !province::is_sea(state, dest) && province::has_safe_access_to_province(state, n, dest)) {
				size_t didx = get_or_add_prov(dest);
				provs[didx].incoming_regs += sc.total;
			}
			continue;
		}

		size_t pidx = get_or_add_prov(loc);
		provs[pidx].present_regs += sc.total;
		provs[pidx].present_front += sc.frontline;
		provs[pidx].present_back += sc.backline;
		if(act == army_activity::attacking) {
			provs[pidx].has_staging_attack = true;
		}

		// Eligible donor: idle on_guard stack with >= 6 regiments and ready for combat
		if(act == army_activity::on_guard
			&& !ar.get_battle_from_army_battle_participation()
			&& sc.total >= 6
			&& army_ready_for_battle(state, n, ar.id)) {
			std::vector<inf_reg_entry> pure_inf;
			for(auto r : ar.get_army_membership()) {
				auto reg = r.get_regiment();
				auto uid = reg.get_type();
				if(ai_detail::is_backline_unit(state, n, uid))
					continue;
				auto const& bdef = state.military_definitions.unit_base_definitions[uid];
				auto stats = state.world.nation_get_unit_stats(n, uid);
				float rec = (stats.discipline_or_evasion > 0.0f) ? stats.reconnaissance_or_fire_range : bdef.reconnaissance_or_fire_range;
				if(rec > 0.0f)
					continue; // keep recon regiments with their parent core stack
				float str = reg.get_strength();
				float org = reg.get_org();
				if(str > 0.30f && org > 0.45f) {
					pure_inf.push_back(inf_reg_entry{ reg.id, str * org });
				}
			}
			if(!pure_inf.empty()) {
				std::sort(pure_inf.begin(), pure_inf.end(), [](inf_reg_entry const& x, inf_reg_entry const& y) {
					if(x.readiness != y.readiness)
						return x.readiness > y.readiness;
					return x.id.index() < y.id.index();
				});
				donors.push_back(donor_stack{
					ar.id,
					loc,
					provs[pidx].theater_id,
					pidx,
					sc.total,
					sc.frontline,
					sc.backline,
					std::move(pure_inf)
				});
			}
		}
	}

	if(donors.empty() || provs.empty())
		return;

	struct deficit_target {
		size_t prov_idx;
		int32_t deficit;
		float urgency;
	};
	std::vector<deficit_target> targets;

	for(size_t i = 0; i < provs.size(); ++i) {
		auto const& ps = provs[i];
		if(!province::has_safe_access_to_province(state, n, ps.p))
			continue;
		if(logistics::calculate_supply_throughput_in_province(state, ps.p, n) <= 0.0f)
			continue;

		int32_t total_cov = ps.present_regs + ps.incoming_regs;
		int32_t room = ps.supply_cap_regs - total_cov;
		if(room <= 0)
			continue;

		int32_t need = 0;
		float urgency = 0.0f;

		// Priority 1: frontline infantry cover for exposed artillery on a threatened sector or staging offensive fist
		if(ps.present_back > 0 && (ps.adj_enemy_regs > 0.1f || ps.has_staging_attack)) {
			int32_t front_need = (ps.present_back + 2) - (ps.present_front + ps.incoming_regs);
			if(front_need > 0) {
				need = std::max(need, front_need);
				urgency = std::max(urgency, 100.0f + float(front_need) * 10.0f);
			}
		}
		// Priority 2: understrength threatened 1st-line defense sector
		if(ps.adj_enemy_regs > 0.1f) {
			int32_t target_def = std::min(ps.supply_cap_regs, std::max(4, std::min(ps.cw + 2, int32_t(std::ceil(ps.adj_enemy_regs * 0.90f)))));
			int32_t def_need = target_def - total_cov;
			if(def_need > 0) {
				need = std::max(need, def_need);
				urgency = std::max(urgency, 40.0f + ps.adj_enemy_regs * 2.0f + float(def_need) * 3.0f);
			}
		}
		// Priority 3: gathering offensive fist below full combat width
		if(ps.has_staging_attack) {
			int32_t target_fist = std::min(ps.supply_cap_regs, ps.cw + std::max(4, ps.cw / 2));
			int32_t fist_need = std::min(6, target_fist - total_cov);
			if(fist_need > 0) {
				need = std::max(need, fist_need);
				urgency = std::max(urgency, 60.0f + float(fist_need) * 4.0f);
			}
		}

		need = std::min(need, room);
		if(need > 0) {
			targets.push_back(deficit_target{ i, need, urgency });
		}
	}

	if(targets.empty())
		return;

	std::sort(targets.begin(), targets.end(), [&](deficit_target const& a, deficit_target const& b) {
		if(a.urgency != b.urgency)
			return a.urgency > b.urgency;
		return provs[a.prov_idx].p.index() < provs[b.prov_idx].p.index();
	});

	int32_t splits_done = 0;
	constexpr int32_t max_splits_per_tick = 4;

	for(auto& tgt : targets) {
		if(splits_done >= max_splits_per_tick)
			break;
		auto& tprov = provs[tgt.prov_idx];
		int32_t rem_deficit = tgt.deficit;

		// Order donors deterministically: 2nd-line safe donors first, then closest to target_p
		std::vector<size_t> donor_order;
		for(size_t d = 0; d < donors.size(); ++d) {
			if(donors[d].theater_id == 0 || donors[d].theater_id != tprov.theater_id || donors[d].loc == tprov.p || donors[d].pure_inf.empty())
				continue;
			if(province::direct_distance_km(state, donors[d].loc, tprov.p) > 450.0f)
				continue;
			donor_order.push_back(d);
		}
		std::sort(donor_order.begin(), donor_order.end(), [&](size_t a, size_t b) {
			bool a_safe = (provs[donors[a].loc_prov_idx].adj_enemy_regs <= 0.1f);
			bool b_safe = (provs[donors[b].loc_prov_idx].adj_enemy_regs <= 0.1f);
			if(a_safe != b_safe)
				return a_safe > b_safe;
			float da = province::direct_distance_km(state, donors[a].loc, tprov.p);
			float db = province::direct_distance_km(state, donors[b].loc, tprov.p);
			if(da != db)
				return da < db;
			return donors[a].id.index() < donors[b].id.index();
		});

		for(size_t didx : donor_order) {
			if(rem_deficit <= 0 || splits_done >= max_splits_per_tick)
				break;
			auto& donor = donors[didx];
			auto& dprov = provs[donor.loc_prov_idx];

			if(!state.world.army_is_valid(donor.id))
				continue;

			// Ensure donating NEVER weakens donor's local defense or exposes donor's artillery!
			int32_t min_loc_regs = (dprov.adj_enemy_regs > 0.1f)
				? std::max(4, std::min(dprov.cw, int32_t(std::ceil(dprov.adj_enemy_regs * 0.85f))) + 2)
				: 0;
			int32_t prov_surplus = dprov.present_regs - min_loc_regs;
			int32_t keep_front = std::max(std::max(0, 4 - donor.backline), donor.backline > 0 ? (donor.backline + 2) : 4);
			int32_t detachable_front = std::max(0, donor.frontline - keep_front);
			int32_t safe_surplus = std::min({ prov_surplus, donor.total - 4, detachable_front, int32_t(donor.pure_inf.size()) });
			if(safe_surplus < 1)
				continue;

			auto safe_path = province::make_safe_land_path(state, donor.loc, tprov.p, n);
			if(safe_path.empty() || safe_path.size() > 4)
				continue;

			int32_t take = std::min(rem_deficit, safe_surplus);
			std::vector<dcon::regiment_id> detached;
			detached.reserve(size_t(take));
			for(int32_t k = 0; k < take; ++k) {
				detached.push_back(donor.pure_inf[size_t(k)].id);
			}

			if(!military::can_split_army<command::actor::ai>(state, n, donor.id, detached))
				continue;

			// Donor stays in place keeping its general & dig_in intact; detached brigade(s) march along safe path to merge!
			military::split_army<command::actor::ai>(state, n, donor.id, detached, 0);
			dcon::army_id new_army = state.world.regiment_get_army_from_army_membership(detached[0]);
			if(new_army && new_army != donor.id) {
				state.world.army_set_dig_in(new_army, 0);
				if(military::set_army_path(state, new_army, safe_path, n)) {
					state.world.army_set_ai_province(new_army, tprov.p);
					state.world.army_set_ai_activity(new_army, uint8_t(army_activity::merging));
				} else {
					state.world.army_set_ai_activity(new_army, uint8_t(army_activity::on_guard));
				}
			}

			donor.pure_inf.erase(donor.pure_inf.begin(), donor.pure_inf.begin() + take);
			donor.total -= take;
			donor.frontline -= take;
			dprov.present_regs -= take;
			dprov.present_front -= take;
			tprov.incoming_regs += take;
			rem_deficit -= take;
			++splits_done;
		}
	}
}

void war_ai_tick(sys::state& state) {
	if(state.cheat_data.disable_ai)
		return;

	// 1. Safety guard for strategic_redeployment: cancel immediately if hostiles approach within 2 hops (warring nations only — zero global in_army scan!)
	for(auto n : state.world.in_nation) {
		if(n.get_is_player_controlled() || n.get_owned_province_count() == 0 || !n.get_is_at_war())
			continue;
		for(auto a : n.get_army_control()) {
			auto ar = a.get_army();
			if(ar.get_special_order() == uint8_t(military::special_army_order::strategic_redeployment)
				&& unit_on_ai_control(state, ar)) {
				auto loc = ar.get_location_from_army_location().id;
				bool danger = ai_front_helpers::is_province_near_hostiles(state, n.id, loc, 2);
				if(!danger && ar.get_path().size() > 0) {
					auto next_p = ar.get_path().at(ar.get_path().size() - 1);
					danger = ai_front_helpers::is_province_near_hostiles(state, n.id, next_p, 1);
				}
				if(danger) {
					ar.set_special_order(uint8_t(military::special_army_order::none));
				}
			}
		}
	}

	// 2. Daily live battle control (reserve reinforcement + retreat before wipe)
	manage_active_battles(state);

	// 3. Fast 2-3 day war reaction cadence for nations currently at war (peaceful nations stay on cheap twice-monthly schedule)
	if(!bool(state.defines.alice_eval_ai_mil_everyday)) {
		auto day = state.current_date.value;
		if(day % 3 == 0) {
			concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
				dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
				if(state.world.nation_is_valid(n)
					&& state.world.nation_get_owned_province_count(n) > 0
					&& state.world.nation_get_is_at_war(n)) {
					distribute_guards_wartime(state, n);
				}
			});
			// Sequential precision detachment after guard distribution (split_army creates armies!)
			for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
				dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
				if(state.world.nation_is_valid(n)
					&& state.world.nation_get_owned_province_count(n) > 0
					&& state.world.nation_get_is_at_war(n)) {
					ai_split_surplus_regiments(state, n);
				}
			}
		} else if(day % 3 == 1) {
			for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
				dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
				if(state.world.nation_is_valid(n)
					&& state.world.nation_get_owned_province_count(n) > 0
					&& state.world.nation_get_is_at_war(n)) {
					assign_targets(state, n);
				}
			}
		}

		// Execute wartime guard, merge & attacker movement every 2 days (warring nations ONLY, zero global in_army scan!)
		auto r8 = day % 8;
		if(day % 2 == 0) {
			for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
				dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
				if(state.world.nation_is_valid(n)
					&& state.world.nation_get_owned_province_count(n) > 0
					&& state.world.nation_get_is_at_war(n)) {
					merge_arrived_reinforcements(state, n);
				}
			}
			if(r8 != 1 && r8 != 5) {
				move_idle_guards_impl(state, true);
			}
			if(r8 != 3 && r8 != 7) {
				move_gathered_attackers_impl(state, true);
			}
		}
	}
}

void general_ai_unit_tick(sys::state& state) {
	auto v = state.current_date.value;
	auto r = v % 8;

	switch(r) {
	case 0:
		pickup_idle_ships(state);
		break;
	case 1:
		move_idle_guards(state);
		break;
	case 2:
		new_units_and_merging(state);
		break;
	case 3:
		move_gathered_attackers(state);
		break;
	case 4:
		update_naval_transport(state);
		break;
	case 5:
		move_idle_guards(state);
		break;
	case 6:
		update_unit_supply_priorities(state);
		for(auto n : state.world.in_nation) {
			if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
				continue;
			if(n.get_is_at_war()) {
				manage_ai_leaders_for_nation(state, n.id);
				update_mobilization_for_nation(state, n.id);
			} else {
				if(n.get_is_mobilized()) {
					update_mobilization_for_nation(state, n.id);
				}
				// Switch into pre-war readiness early without waiting for the 19th
				if(evaluate_war_economy_mode(state, n.id) == war_economy_mode::prewar_ready
					&& n.get_land_supply_consumption() < 70) {
					update_budget_for_nation(state, n.id, false);
				}
			}
		}
		break;
	case 7:
		move_gathered_attackers(state);
		break;
	}
}

float estimate_rebel_strength(sys::state& state, dcon::province_id p) {
	float v = 0.f;
	for(auto ar : state.world.province_get_army_location(p))
		if(ar.get_army().get_controller_from_army_rebel_control())
			v += estimate_army_defensive_strength(state, ar.get_army());
	return v;
}

bool ai_will_issue_embargo(sys::state& state, dcon::nation_id from, dcon::nation_id to) {
	if(from == to) {
		return false;
	}

	// Embargo countries with high infamy
	if(state.world.nation_get_infamy(to) > state.defines.badboy_limit * 1.2f) {
		return true;
	}

	// If a player has embargoed us - embargo him back
	if (state.world.nation_get_is_player_controlled(to) && economy::has_active_embargo(state, to, from)) {
		return true;
	}

	return false;
}

void update_ai_embargoes(sys::state& state) {
	for(auto from : state.world.in_nation) {
		// Only independent AI countries can issue embargoes
		if(from.get_is_player_controlled() || from.get_overlord_as_subject().get_ruler()) {
			continue;
		}

		for(auto to : state.world.in_nation) {
			// Do not consider subjects as embargo targets
			if(to.get_overlord_as_subject().get_ruler()) {
				continue;
			}

			auto has_embargo = economy::has_active_embargo(state, from, to);
			if(has_embargo != ai_will_issue_embargo(state, from, to) && command::can_switch_embargo_status(state, from, to, true)) {
				command::execute_switch_embargo_status(state, from, to);

				// For gameplay reasons limit to one embargo per month per AI.
				break;
			}
		}
	}
}

}
