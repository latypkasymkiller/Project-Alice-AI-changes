#include "ai.hpp"
#include "ai_pressure.hpp"
#include "ai_types.hpp"
#include "ai_campaign_values.hpp"
#include "system_state.hpp"
#include "demographics.hpp"
#include "economy_stats.hpp"
#include "economy_production.hpp"
#include "economy_government.hpp"
#include "construction.hpp"
#include "effects.hpp"
#include "gui_effect_tooltips.hpp"
#include "math_fns.hpp"
#include "military.hpp"
#include "politics.hpp"
#include "prng.hpp"
#include "province_templates.hpp"
#include "triggers.hpp"
#include "province.hpp"
#include "commands.hpp"
#include "battle_prediction.hpp"

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
	auto fat_id = dcon::fatten(state.world, a);
	if(fat_id.get_controller_from_army_control().get_overlord_commanding_units()) {
		return false;
	}
	return fat_id.get_controller_from_army_control().get_is_player_controlled()
		? fat_id.get_is_ai_controlled()
		: true;
}
bool unit_on_ai_control(const sys::state& state, dcon::navy_id a) {
	auto fat_id = dcon::fatten(state.world, a);
	if(fat_id.get_controller_from_navy_control().get_overlord_commanding_units()) {
		return false;
	}
	return !fat_id.get_controller_from_navy_control().get_is_player_controlled();
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

/*
Orders outlive the situation that justified them, and nothing else reconsiders an army once
it is moving: every other pass skips armies that have an arrival time. Four ways an AI army
gets stuck, all repaired in one sweep.

Everything here is derived from synchronized gamestate rather than remembered. A side table
of "which battle was this army sent to" would not survive a save, a load, or a player joining
mid-game, and the host would then cancel an order that the joiner kept.
*/
void validate_ai_orders(sys::state& state) {
	float const sufficiency = std::max(1.0f, state.defines.alice_ai_reinforce_sufficiency);

	for(auto ar : state.world.in_army) {
		if(ar.get_battle_from_army_battle_participation() || ar.get_navy_from_army_transport())
			continue;

		auto controller = ar.get_controller_from_army_control();
		if(!controller || !unit_on_ai_control(state, ar))
			continue;

		auto const activity = army_activity(ar.get_ai_activity());
		auto const location = ar.get_location_from_army_location().id;

		if(ar.get_arrival_time() && !ar.get_black_flag() && !ar.get_is_retreating()) {
			auto path = ar.get_path();

			/*
			A garrison sent to reinforce a battle keeps marching long after that battle has
			been decided, then arrives alone in a province the enemy now holds. Feeding a
			defender to an attacker one stack at a time is the cheapest trick there is
			against this AI, and it needs two separate guards to close.

			The first is the detour's own signature: gather_to_battle appends a return leg,
			so the far end of the path is the province the army set out from. A path that
			ends where it began is a round trip, and if nowhere along it still has a battle
			there is nothing left to arrive for. This is the cheap catch, but it only holds
			while the army is still on its first province: after that its location has moved
			on while the return destination has not, and the signature no longer matches.
			*/
			bool cancelled = false;
			if(path.size() > 0 && path.at(0) == location) {
				bool any_live_battle = false;
				for(uint32_t i = 0; i < path.size(); ++i) {
					auto battles = state.world.province_get_land_battle_location(path.at(i));
					if(battles.begin() != battles.end()) {
						any_live_battle = true;
						break;
					}
				}
				if(!any_live_battle) {
					// The station is left alone, so move_idle_guards walks the army back to
					// its post rather than leaving it standing in the open.
					military::stop_army_movement(state, ar);
					cancelled = true;
				}
			}

			/*
			The second guard covers the rest of the march, and every other reason an army
			might be walking somewhere unwise: look at the province about to be entered. If
			the fighting there is already over and what remains outweighs what would arrive,
			stop. Attack orders are exempt, because assign_targets weighed those when it
			issued them; this rescues guards only.
			*/
			if(!cancelled && activity == army_activity::on_guard && path.size() > 0) {
				auto next = path.at(path.size() - 1); // the path is consumed from the back

				auto battles = state.world.province_get_land_battle_location(next);
				if(battles.begin() == battles.end()) {
					float hostile_there = 0.0f;
					float friendly_there = ai::army_pressure_weight(state, ar.id);

					for(auto other : state.world.province_get_army_location(next)) {
						auto other_army = other.get_army();
						auto other_controller = other_army.get_controller_from_army_control();
						float const w = ai::army_pressure_weight(state, other_army.id);
						if(w <= 0.0f)
							continue;

						// A null controller is a rebel army, and rebels fight everyone;
						// military::are_enemies gets that case wrong (military.cpp:884).
						if(!other_controller || military::are_at_war(state, controller, other_controller))
							hostile_there += w;
						else if(other_controller == controller
							|| military::are_allied_in_war(state, controller, other_controller))
							friendly_there += w;
					}

					if(hostile_there > 0.0f && hostile_there > sufficiency * friendly_there) {
						military::stop_army_movement(state, ar);

						// The station goes with the march. Cancelling the path alone left the
						// army pointed at the same place, and move_idle_guards re-issued the
						// identical order on its next pass for this check to cancel again,
						// forever. Releasing the station instead sends the army back through
						// distribute_guards, which can weigh the province against the rest of
						// the line and hand it a different one.
						ar.set_ai_province(dcon::province_id{});
					}
				}
			}
		}

		/*
		A guard whose station stopped being ours, or stopped being reachable. An AI nation
		eventually gives up on its own through the transport bail-outs in move_idle_guards,
		but for a player nation with AI-delegated armies those are unreachable, so nothing
		clears the station and the army re-runs a failing search every eight days forever.
		*/
		if(activity == army_activity::on_guard) {
			if(auto station = ar.get_ai_province().id; station) {
				auto station_controller = state.world.province_get_nation_from_province_control(station);
				bool const valid = station.index() < state.province_definitions.first_sea_province.index()
					&& province::has_access_to_province(state, controller, station)
					// Guards are legitimately stationed on an ally's soil, so belonging to us
					// is not the test.
					&& (station_controller == controller
						|| military::are_allied_in_war(state, controller, station_controller));

				if(!valid) {
					military::stop_army_movement(state, ar.id);
					ar.set_ai_province(dcon::province_id{});
				}
			}
		}

		/*
		An attack that arrived and found nothing left to do. move_gathered_attackers only
		releases an army when the province has a controller it is not at war with, so a
		rebel-held or uncontrolled target keeps the army forever; and its attacking branch
		requires the target to differ from the current location, which an attack ordered
		against the army's own province never satisfies.
		*/
		if((activity == army_activity::attacking || activity == army_activity::attack_gathered)
			&& ar.get_ai_province() == location
			&& !ar.get_arrival_time()) {

			auto occupier = state.world.province_get_nation_from_province_control(location);
			bool const nothing_left =
				// Being mid-siege is the normal, correct form of this state.
				state.world.province_get_siege_progress(location) == 0.0f
				&& !state.world.province_get_rebel_faction_from_province_rebel_control(location)
				&& !(occupier && military::are_at_war(state, controller, occupier));

			if(nothing_left) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
			}
		}

		// A merge that failed or finished resets the activity but leaves the old station
		// behind, and move_idle_guards then dutifully marches the army to it.
		if(activity == army_activity::unspecified && ar.get_ai_province())
			ar.set_ai_province(dcon::province_id{});
	}
}

/*
Battles are only ever evaluated for reinforcement at the moment they are created. That was
survivable while the rule for sending help was a yes-or-no question about adjacent enemies,
but it is not survivable alongside a cap on how much gets sent: the AI would measure the
shortfall once, commit against that, and then watch a doomstack walk into the same province
the following day without ever reconsidering. Re-examining live battles daily is what makes
the cap a budget rather than a ceiling.
*/
void reinforce_live_battles(sys::state& state) {
	if(!ai::pressure_enabled(state))
		return;

	std::vector<dcon::nation_id> participants;

	for(auto b : state.world.in_land_battle) {
		auto location = state.world.land_battle_get_location_from_land_battle_location(b);
		if(!location)
			continue;

		participants.clear();

		/*
		Every belligerent of the war, not only those with an army already in the fight. The
		creation-time call in military.cpp is narrowed to the engaged, which is affordable
		there because it fires once per battle; this daily pass is what actually reaches the
		ally standing two provinces away. Narrowing here as well would have meant a
		co-belligerent whose armies never happened to join was never asked at all, since
		nothing else would ever put one of its armies in the battle to qualify it.
		*/
		if(auto w = state.world.land_battle_get_war_from_land_battle_in_war(b); w) {
			for(auto par : state.world.war_get_war_participant(w)) {
				auto controller = par.get_nation().id;
				if(!controller || state.world.nation_get_is_player_controlled(controller))
					continue;

				participants.push_back(controller);
			}
		} else {
			// A battle outside any war is a rebel suppression; only the armies present have
			// a stake in it.
			for(auto par : state.world.land_battle_get_army_battle_participation(b)) {
				auto controller = par.get_army().get_controller_from_army_control();
				if(!controller || state.world.nation_get_is_player_controlled(controller))
					continue;
				if(std::find(participants.begin(), participants.end(), controller) != participants.end())
					continue;

				participants.push_back(controller);
			}
		}

		// Collected first: gathering issues movement orders, and doing that while walking
		// the participant list would mean mutating around an active iterator.
		for(auto controller : participants)
			gather_to_battle(state, controller, location);
	}
}

void daily_cleanup(sys::state& state) {
	/*
	No cache clear here. cached_tactical_field already stamps each nation's field with the day
	it was built and rebuilds on the first read of a new one, and the load path clears outright
	in state::fill_unsaved_data. Clearing again from here was worse than redundant: this runs
	near the end of the tick, well after update_movement has built fields and recorded debits
	against them, so it threw away the record of which armies had already been committed today
	and forced a second build of every field a few lines later in reinforce_live_battles. The
	debits are meant to survive the whole day -- an army sent to one battle must not still read
	as cover when the next one is weighed.
	*/
	validate_ai_orders(state);
	reinforce_live_battles(state);
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
	allied_hostile_border = 5,
	hostile_rear_2 = 6,
	hostile_rear_1 = 7,
	hostile_border = 8,
	count = 9
};

struct classified_province {
	dcon::province_id id;
	province_class c;
	// Coarse magnitude of the threat facing this province, as a power of two; -128 means
	// no pressure data, which is also what every province gets in peacetime.
	int8_t pressure = -128;
};

dcon::army_id split_army_for_weight(sys::state& state, dcon::nation_id n, dcon::army_id a, float desired_weight) {
	if(!a || !(desired_weight > 0.0f))
		return a;

	float const reg_weight = state.defines.pop_size_per_regiment / 1000.0f;
	if(!(reg_weight > 0.0f))
		return a;

	auto regs = state.world.army_get_army_membership(a);
	uint32_t total_regs = 0;
	float total_weight = 0.0f;
	std::vector<dcon::regiment_id> frontline;
	std::vector<dcon::regiment_id> support;

	for(auto r : regs) {
		auto reg = r.get_regiment();
		float w = reg_weight * reg.get_strength();
		total_weight += w;
		++total_regs;

		auto type = reg.get_type();
		auto etype = state.military_definitions.unit_base_definitions[type].type;
		if(etype == military::unit_type::support || etype == military::unit_type::special) {
			support.push_back(reg.id);
		} else {
			frontline.push_back(reg.id);
		}
	}

	// Если в армии всего 1 полк или запрашиваемый вес покрывает почти всю армию — не делим
	if(total_regs <= 1 || desired_weight >= (total_weight - reg_weight * 0.5f))
		return a;

	// Считаем сколько полков нужно отделить (+0.5f для округления без #include <cmath>)
	uint32_t target_reg_count = std::clamp(uint32_t(desired_weight / reg_weight + 0.5f), 1u, total_regs - 1u);

	std::vector<dcon::regiment_id> to_split;
	to_split.reserve(target_reg_count);

	// Соблюдаем баланс: ~50% фронт, ~50% поддержка
	uint32_t target_front = (target_reg_count + 1) / 2;
	uint32_t target_supp = target_reg_count / 2;

	uint32_t taken_front = 0;
	for(size_t i = 0; i < frontline.size() && taken_front < target_front; ++i) {
		to_split.push_back(frontline[i]);
		++taken_front;
	}

	uint32_t taken_supp = 0;
	for(size_t i = 0; i < support.size() && taken_supp < target_supp; ++i) {
		to_split.push_back(support[i]);
		++taken_supp;
	}

	// Если не хватило одного типа, добираем из другого
	if(to_split.size() < target_reg_count) {
		for(size_t i = taken_front; i < frontline.size() && to_split.size() < target_reg_count; ++i) {
			to_split.push_back(frontline[i]);
		}
		for(size_t i = taken_supp; i < support.size() && to_split.size() < target_reg_count; ++i) {
			to_split.push_back(support[i]);
		}
	}

	if(to_split.empty() || to_split.size() >= total_regs)
		return a;

	if(!military::can_split_army<command::actor::ai>(state, n, a, to_split))
		return a;

	military::split_army<command::actor::ai>(state, n, a, to_split);

	// ОШИБКА ИСПРАВЛЕНА: получаем army_id напрямую, без .id
	dcon::army_id new_army = state.world.regiment_get_army_from_army_membership(to_split[0]);

	// ОШИБКА ИСПРАВЛЕНА: копируем AI-статус, чтобы армия не "зависла" в логике
	if(new_army && new_army != a) {
		state.world.army_set_ai_activity(new_army, state.world.army_get_ai_activity(a));
		state.world.army_set_ai_province(new_army, state.world.army_get_ai_province(a));
		state.world.army_set_is_ai_controlled(new_army, state.world.army_get_is_ai_controlled(a));
	}

	return new_army ? new_army : a;
}

void distribute_guards(sys::state& state, dcon::nation_id n) {
	std::vector<classified_province> provinces;
	provinces.reserve(state.world.province_size());

	/*
	Where each province sits in the list above, so the echelon passes can ask what a
	neighbour was classified as in constant time. They used to scan the whole list once per
	adjacency, which costs a large empire several million comparisons per rebuild; that was
	tolerable while defenses were redistributed twice a month and is not now that a nation
	at war rebuilds them every few days. Only valid until the list is sorted below.
	*/
	std::vector<int32_t> slot_of(state.world.province_size(), -1);

	auto cap = state.world.nation_get_capital(n);

	/*
	Built before anything is classified, because a nation with nothing to station has no use
	for the rest of this function, and most nations on most passes are in that position.
	*/
	std::vector<dcon::army_id> guards_list;
	bool const use_pressure = ai::pressure_enabled(state) && state.world.nation_get_is_at_war(n);

	{
		auto controlled = state.world.nation_get_army_control(n);
		guards_list.reserve(size_t(controlled.end() - controlled.begin()));
		for(auto a : controlled) {
			if(a.get_army().get_ai_activity() != uint8_t(army_activity::on_guard))
				continue;

			/*
			Every on_guard army is listed, including those already marching to a station.
			Excluding them looks like it prevents mid-march churn, but this pass matches
			provinces to guards rather than guards to provinces: an in-flight army that is
			left out of the list does not take its destination province out of the running,
			so that province simply collects a second guard, and then a third. Marches
			routinely outlast the four-day wartime cadence, so most of the garrison can be
			invisible on any given pass. Listing everyone keeps each pass a complete
			re-matching, which is what makes an existing claim on a province visible.
			*/
			guards_list.push_back(a.get_army().id);
		}
	}

	if(guards_list.empty())
		return;

	// Strategic horizon: an army tied up in a battle still counts, at a discount, because
	// the battle will end and the mass will still be there.
	static thread_local ai::pressure_field guard_field;
	if(use_pressure)
		ai::build_pressure_field(state, n, ai::pressure_horizon::strategic, guard_field);

	// 1. Primary province classification
	for(auto c : state.world.nation_get_province_control(n)) {
		province_class cls = c.get_province().get_is_coast() ? province_class::coast : province_class::interior;
		if(c.get_province() == cap)
			cls = province_class::border;

		for(auto padj : c.get_province().get_province_adjacency()) {
			auto other = padj.get_connected_provinces(0) == c.get_province() ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			auto n_controller = other.get_nation_from_province_control();
			auto ovr = n_controller.get_overlord_as_subject().get_ruler();

			if(n_controller == n) {
				// own province
			} else if(!n_controller && !other.get_rebel_faction_from_province_rebel_control()) {
				// uncolonized or sea
			} else if(other.get_rebel_faction_from_province_rebel_control()) {
				auto other_owner = other.get_nation_from_province_ownership();
				if(other_owner == n || (other_owner && military::are_at_war(state, n, other_owner))) {
					cls = province_class::hostile_border;
					break;
				}
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
		slot_of[c.get_province().id.index()] = int32_t(provinces.size());
		provinces.push_back(classified_province{ c.get_province().id, cls,
			use_pressure ? ai::pressure_bucket(guard_field.hostile_at(c.get_province().id)) : int8_t(-128) });
	}

	// 1.5 Allied frontline (Allied provinces adjacent to the enemy)
	for(auto par : state.world.nation_get_war_participant(n)) {
		for(auto other : par.get_war().get_war_participant()) {
			if(other.get_is_attacker() == par.get_is_attacker() && other.get_nation() != n) {
				auto ally = other.get_nation();
				for(auto c : state.world.nation_get_province_control(ally)) {
					bool is_hostile = false;
					for(auto padj : c.get_province().get_province_adjacency()) {
						auto other_prov = padj.get_connected_provinces(0) == c.get_province() ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
						auto n_controller = other_prov.get_nation_from_province_control();

						if(other_prov.get_rebel_faction_from_province_rebel_control()) {
							is_hostile = true;
							break;
						} else if(n_controller && military::are_at_war(state, n, n_controller)) {
							is_hostile = true;
							break;
						}
					}

					if(is_hostile) {
						auto prov_id = c.get_province().id;
						if(slot_of[prov_id.index()] < 0) {
							slot_of[prov_id.index()] = int32_t(provinces.size());
							provinces.push_back(classified_province{ prov_id, province_class::allied_hostile_border,
								use_pressure ? ai::pressure_bucket(guard_field.hostile_at(prov_id)) : int8_t(-128) });
						}
					}
				}
			}
		}
	}

	// 2. Secondary defensive line (hostile_rear_1: friendly provinces adjacent to the hostile border)
	for(auto& cp : provinces) {
		if(cp.c != province_class::hostile_border) {
			auto fat_p = dcon::fatten(state.world, cp.id);
			for(auto padj : fat_p.get_province_adjacency()) {
				auto other = padj.get_connected_provinces(0) == fat_p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				auto slot = slot_of[other.id.index()];
				if(slot >= 0 && provinces[slot].c == province_class::hostile_border) {
					cp.c = province_class::hostile_rear_1;
					break;
				}
			}
		}
	}

	// 3. Tertiary defensive line (hostile_rear_2: friendly provinces adjacent to the secondary line)
	for(auto& cp : provinces) {
		if(cp.c != province_class::hostile_border && cp.c != province_class::hostile_rear_1) {
			auto fat_p = dcon::fatten(state.world, cp.id);
			for(auto padj : fat_p.get_province_adjacency()) {
				auto other = padj.get_connected_provinces(0) == fat_p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				auto slot = slot_of[other.id.index()];
				if(slot >= 0 && provinces[slot].c == province_class::hostile_rear_1) {
					cp.c = province_class::hostile_rear_2;
					break;
				}
			}
		}
	}

	std::sort(provinces.begin(), provinces.end(), [&](classified_province& a, classified_province& b) {
		if(a.c != b.c) {
			return uint8_t(a.c) > uint8_t(b.c);
		}
		/*
		Within one tier, garrison the heavier threat first. This reorders the border ring so
		that the axis actually under weight is manned before a quiet stretch of the same
		class; it deliberately does not reorder across tiers, because the class ordering is
		the depth-in-depth scheme and the stage loop below counts down through it.

		In peacetime, and whenever the model is switched off, every bucket is -128 and this
		clause is never reached, so the ordering is exactly what it was before.
		*/
		if(a.pressure != b.pressure) {
			return a.pressure > b.pressure;
		}
		auto adist = province::sorting_distance(state, a.id, cap);
		auto bdist = province::sorting_distance(state, b.id, cap);
		if(adist != bdist) {
			return adist < bdist;
		}
		return a.id.index() < b.id.index();
	});

	// distribute target provinces
	uint32_t end_of_stage = 0;

	for(uint8_t stage = uint8_t(province_class::count); stage-- > 0 && !guards_list.empty(); ) {
		uint32_t start_of_stage = end_of_stage;

		for(; end_of_stage < provinces.size(); ++end_of_stage) {
			if(uint8_t(provinces[end_of_stage].c) != stage)
				break;
		}

		uint32_t full_loops_through = 0;
		bool guard_assigned = false;
		do {
			guard_assigned = false;
			for(uint32_t j = start_of_stage; j < end_of_stage && !guards_list.empty(); ++j) {
				auto p = provinces[j].id;
				auto p_region = state.world.province_get_connected_region_id(provinces[j].id);
				assert(p_region > 0);
				bool p_region_is_coastal = state.province_definitions.connected_region_is_coastal[p_region - 1];

				if(10.0f * (1 + full_loops_through) <= military::peacetime_attrition_limit(state, n, p)) {
					uint32_t nearest_index = 0;
					dcon::army_id nearest;
					float nearest_distance = 1.0f;
					for(uint32_t k = uint32_t(guards_list.size()); k-- > 0;) {
						auto guard_loc = state.world.army_get_location_from_army_location(guards_list[k]);

						/*
						A "too heavy for this province" test used to sit here. It could never
						fire: relative_attrition_amount ends in std::min(1.f, value * 0.01f)
						(military.cpp:6656), so it is bounded by 1.0 and was compared against
						2.0. Deleted rather than repaired, because every repair considered is
						worse than the gap.

						Lowering the threshold turns it into a cliff rather than a gradient:
						the value is capped by the province's max_attrition modifier, which is
						0 on ordinary terrain, so the test stays a no-op almost everywhere and
						becomes "no guards at all" on exactly the harsh terrain worth holding.
						That is the regression commit 1bd387271 reverted. Comparing
						local_army_weight against the supply limit instead has the same cliff
						plus a griefing vector: that count includes every army in the province
						whoever owns it, so an ally parking a doomstack in a mountain pass
						would make this AI refuse to garrison its own pass.

						Capacity is therefore bounded only by the peacetime_attrition_limit
						ladder above, which already gates how many stacks a poor province
						accumulates.
						*/

						/*
						// this wont work because a unit could end up in, for example, a subject's region at the end of a war
						// this region could be landlocked, resulting in this thinking that the unit can only be stationed in that
						// same region, even though it could walk out to another region

						auto g_region = state.world.province_get_connected_region_id(guard_loc);
						assert(g_region > 0);

						if(p_region != g_region && !(state.world.army_get_black_flag(guards_list[k]))  && (!p_region_is_coastal || !state.province_definitions.connected_region_is_coastal[g_region - 1]))
							continue;
						*/

						if(auto d = province::sorting_distance(state, guard_loc, p); !nearest || d < nearest_distance) {

							nearest_index = k;
							nearest_distance = d;
							nearest = guards_list[k];
						}
					}

					// assign nearest guard
					if(nearest) {
						state.world.army_set_ai_province(nearest, p);
						guards_list[nearest_index] = guards_list.back();
						guards_list.pop_back();

						guard_assigned = true;
					}
				}
			}

			++full_loops_through;
		} while(guard_assigned);

	}
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

void move_idle_guards(sys::state& state) {
	std::vector<dcon::army_id> require_transport;
	require_transport.reserve(state.world.army_size());

	// 1. Evacuate exiled (black-flagged) idle armies from foreign land to owned home territory
	for(auto ar : state.world.in_army) {
		if(ar.get_black_flag()
			&& ar.get_controller_from_army_control()
			&& unit_on_ai_control(state, ar)
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {

			auto controller = ar.get_controller_from_army_control();
			auto army_loc = ar.get_location_from_army_location();

			// If the army is currently exiled in foreign land
			if(army_loc.get_nation_from_province_ownership() != controller) {
				dcon::province_id home_target;
				float min_dist = 100000.0f;

				// Find the nearest owned province to march towards
				for(auto p : controller.get_province_ownership()) {
					auto dist = province::sorting_distance(state, army_loc.id, p.get_province().id);
					if(!home_target || dist < min_dist) {
						min_dist = dist;
						home_target = p.get_province().id;
					}
				}

				if(home_target) {
					auto path = province::make_land_unit_path(state, army_loc.id, home_target, controller, ar);
					if(!path.empty()) {
						military::set_army_path(state, ar.id, path, controller);
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
						ar.set_ai_province(home_target);
						continue;
					} else {
						ar.set_ai_province(home_target);
						require_transport.push_back(ar.id);
					}
				}
			}
		}
	}

	for(auto ar : state.world.in_army) {
		if(ar.get_ai_activity() == uint8_t(army_activity::on_guard)
			&& ar.get_ai_province()
			&& ar.get_ai_province() != ar.get_location_from_army_location()
			&& ar.get_controller_from_army_control()
			&& unit_on_ai_control(state, ar)
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {

			auto path = province::make_safe_land_path(state, ar.get_location_from_army_location().id, ar.get_ai_province(), ar.get_controller_from_army_control());
			bool valid_path = !path.empty() && military::set_army_path(state, ar.id, path, ar.get_controller_from_army_control());

			if(!valid_path) {
				//Units delegated to the AI won't transport themselves on their own
				if(!ar.get_controller_from_army_control().get_is_player_controlled())
					require_transport.push_back(ar.id);
			}
		}
	}

	for(uint32_t i = 0; i < require_transport.size(); ++i) {
		auto coastal_target_prov = state.world.army_get_location_from_army_location(require_transport[i]);
		auto controller = state.world.army_get_controller_from_army_control(require_transport[i]);

		dcon::navy_id transport_fleet = find_transport_fleet(state, controller);

		auto regs = state.world.army_get_army_membership(require_transport[i]);

		auto tcap = military::transport_capacity(state, transport_fleet);
		tcap -= int32_t(regs.end() - regs.begin());

		if(tcap < 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
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
	dcon::regiment_id sample_reg;
	auto regs = state.world.army_get_army_membership(a);
	if(regs.begin() != regs.end()) {
		sample_reg = (*regs.begin()).get_regiment().id;
	} else {
		return false;
	}

	return state.world.regiment_get_org(sample_reg) >= 0.5f;
}

/*
The original gathering rule, kept intact and still used whenever the pressure model is off
and for every nation at peace. Rebel suppression runs through here, and a change aimed at
wars has no business altering it.
*/
// MP compliant
void gather_to_battle_legacy(sys::state& state, dcon::nation_id n, dcon::province_id p) {
	for(auto ar : state.world.nation_get_army_control(n)) {
		army_activity activity = army_activity(ar.get_army().get_ai_activity());
		if(ar.get_army().get_battle_from_army_battle_participation()
			|| ar.get_army().get_navy_from_army_transport()
			|| ar.get_army().get_black_flag()
			|| ar.get_army().get_arrival_time()
			|| (activity != army_activity::on_guard && activity != army_activity::attacking && activity != army_activity::attack_gathered && activity != army_activity::attack_transport)
			|| !army_ready_for_battle(state, n, ar.get_army())) {

			continue;
		}

		auto location = ar.get_army().get_location_from_army_location();
		if(location == p)
			continue;

		// Do not leave the frontline open if there are unengaged enemy forces right in front of us
		bool has_unengaged_enemy_in_front = false;
		for(auto padj : location.get_province_adjacency()) {
			auto other = padj.get_connected_provinces(0) == location ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);

			// Ignore the combat province we are actively trying to reinforce
			if(other.id == p)
				continue;

			for(auto enemy_ar : state.world.province_get_army_location(other.id)) {
				auto e_army = enemy_ar.get_army();
				auto e_controller = e_army.get_controller_from_army_control();

				// If there's a hostile army in front of us which is NOT in battle, NOT retreating, and NOT exiled -> it's an active threat
				if(military::are_enemies(state, n, e_controller)
					&& !e_army.get_battle_from_army_battle_participation()
					&& !e_army.get_is_retreating()
					&& !e_army.get_black_flag()) {

					has_unengaged_enemy_in_front = true;
					break;
				}
			}

			if(has_unengaged_enemy_in_front)
				break;
		}

		if(has_unengaged_enemy_in_front)
			continue; // Hold frontline to prevent hostile breakthroughs

		auto sdist = province::sorting_distance(state, location, p);
		if(sdist > state.defines.alice_ai_gather_radius)
			continue;

		// move back and fourth between the battle and original location
		military::move_army_ai(state, ar.get_army().id, p, n);
		military::move_army_ai(state, ar.get_army().id, ar.get_army().get_location_from_army_location(), n, false);
	}
}

/*
Deciding who goes to a battle.

What this replaces asked one question per candidate: is there any enemy army in a province
next to me that is not already fighting. A single regiment answered yes, so one scout could
freeze an entire army group in place indefinitely, and it counted enemies across impassable
borders and in adjacent sea zones too, which meant a fleet parked offshore permanently
immobilised every coastal garrison.

The replacement asks two questions with arithmetic in them. Is what faces me worth holding
against at all, and if I leave, is this sector still covered? Neither can be answered by a
token, and both scale with the actual mass involved. A third test caps how much goes to any
one battle, so the AI stops emptying a theatre into a fight it has already won.
*/
// MP compliant
void gather_to_battle(sys::state& state, dcon::nation_id n, dcon::province_id p) {
	if(!ai::pressure_enabled(state) || !state.world.nation_get_is_at_war(n)) {
		gather_to_battle_legacy(state, n, p);
		return;
	}

	float hostile_engaged = 0.0f;
	float friendly_engaged = 0.0f;
	if(!ai::battle_side_weights(state, n, p, hostile_engaged, friendly_engaged))
		return; // the fighting is already over

	struct candidate {
		dcon::army_id a;
		dcon::province_id loc;
		float distance = 0.0f;
		float weight = 0.0f;
	};

	// Candidates are gathered before the field is touched: a nation with nothing within
	// reach must not pay for building one.
	std::vector<candidate> candidates;
	for(auto ar : state.world.nation_get_army_control(n)) {
		army_activity activity = army_activity(ar.get_army().get_ai_activity());
		if(ar.get_army().get_battle_from_army_battle_participation()
			|| ar.get_army().get_navy_from_army_transport()
			|| ar.get_army().get_black_flag()
			|| ar.get_army().get_arrival_time()
			|| (activity != army_activity::on_guard && activity != army_activity::attacking && activity != army_activity::attack_gathered && activity != army_activity::attack_transport)
			|| !army_ready_for_battle(state, n, ar.get_army())) {

			continue;
		}

		auto location = ar.get_army().get_location_from_army_location();
		if(location.id == p)
			continue;

		auto sdist = province::sorting_distance(state, location, p);
		if(sdist > state.defines.alice_ai_gather_radius)
			continue;

		float const w = ai::army_pressure_weight(state, ar.get_army().id);
		if(w <= 0.0f)
			continue;

		candidates.push_back(candidate{ ar.get_army().id, location.id, sdist, w });
	}

	if(candidates.empty())
		return;

	/*
	What still has to be sent, discounting help already on the road. Without that discount the
	same shortfall would be filled again on every re-examination and the theatre would drain a
	day at a time.

	Computed here rather than up front because inbound_friendly_weight walks every army of the
	nation and every province of its path. reinforce_live_battles now asks every belligerent of
	the war about every live battle daily, so most calls reach this function with nothing in
	range; those must fall out at the candidate scan above, which touches only armies, and not
	pay for a path walk first.
	*/
	float const inbound = ai::inbound_friendly_weight(state, n, p);

	float const need = std::max(1.0f, state.defines.alice_ai_reinforce_sufficiency) * hostile_engaged
		- friendly_engaged
		- inbound;

	if(need <= 0.0f)
		return;


	// Nearest first: sorting_distance is the negated cosine of the arc, so smaller is
	// closer. The tie-break on id keeps the order total, which every client depends on.
	std::sort(candidates.begin(), candidates.end(), [](candidate const& a, candidate const& b) {
		if(a.distance != b.distance)
			return a.distance < b.distance;
		return a.a.index() < b.a.index();
	});

	auto& field = ai::cached_tactical_field(state, n);

	float const hold_ratio = std::max(0.0f, state.defines.alice_ai_hold_ratio);
	float const token_pressure = std::max(0.0f, state.defines.alice_ai_token_pressure);
	int32_t const max_commits = int32_t(std::clamp(state.defines.alice_ai_gather_max_commits, 1.0f, 64.0f));

	float committed = 0.0f;
	int32_t commits = 0;

	for(auto const& c : candidates) {
		if(committed >= need || commits >= max_commits)
			break;

		// The field was built earlier in the day. Anyone who has moved since is skipped,
		// because the debit below would otherwise be subtracted from the wrong place.
		if(state.world.army_get_location_from_army_location(c.a) != c.loc)
			continue;

		bool is_frontline = false;
		auto fat_c_loc = dcon::fatten(state.world, c.loc);
		for(auto padj : fat_c_loc.get_province_adjacency()) {
			auto other = padj.get_connected_provinces(0) == fat_c_loc ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			auto n_controller = other.get_nation_from_province_control();

			if(other.get_rebel_faction_from_province_rebel_control() || (n_controller && military::are_at_war(state, n, n_controller))) {
				is_frontline = true;
				break;
			}
		}

		float const facing = field.hostile_at(c.loc);
		float const cover = field.friendly_at(c.loc);

		float const rem_need = need - committed;
		float const send_w = std::min(c.weight, rem_need);

		/*
		Either what faces this army is beneath notice, or the sector survives its departure.
		Frontline armies hold the line unless cover remains or threat is trivial;
		rear reserve armies are exempt from holding the line.
		*/
		bool const overwhelm = facing <= token_pressure;
		bool const cover_remains = std::max(0.0f, cover - send_w) >= hold_ratio * facing;

		if(is_frontline && !overwhelm && !cover_remains)
			continue; // hold the line

		// Напрямую проверяем путь. Если пути нет — сразу переходим к следующему кандидату, не трогая армию
		auto path = province::make_land_unit_path(state, c.loc, p, n, c.a);
		if(path.empty())
			continue;

		// Путь есть: отделяем требуемый вес
		dcon::army_id army_to_send = split_army_for_weight(state, n, c.a, send_w);
		float const actual_w = ai::army_pressure_weight(state, army_to_send);

		// Назначаем построенный путь новой армии и добавляем обратный путь
		military::set_army_path(state, army_to_send, path, n);
		military::move_army_ai(state, army_to_send, c.loc, n, false);

		// Each departure lowers the cover the next candidate sees, so a sector cannot be
		// emptied by several armies each believing the others stayed.
		ai::debit_friendly_at(state, field, c.loc, actual_w, n);

		committed += actual_w;
		++commits;
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
	// average army quality
	float total_str = 0.f;
	auto owner = state.world.army_control_get_controller(state.world.army_get_army_control(a));
	for(const auto reg : regs) {
		auto type = reg.get_regiment().get_type();
		auto stats = state.world.nation_get_unit_stats(owner, type);
		auto& atk = (stats.discipline_or_evasion > 0.0f) ? stats.attack_or_gun_power : state.military_definitions.unit_base_definitions[type].attack_or_gun_power;
		auto& def = (stats.discipline_or_evasion > 0.0f) ? stats.defence_or_hull : state.military_definitions.unit_base_definitions[type].defence_or_hull;
		auto& sup = (stats.discipline_or_evasion > 0.0f) ? stats.support : state.military_definitions.unit_base_definitions[type].support;

		total_str += (10 + atk + 10 + def + sup) / 2 * reg.get_regiment().get_strength() * (1 + reg.get_regiment().get_experience());
	}
	assert(std::isfinite(total_str));

	return total_str;
}

float estimate_army_defensive_strength(sys::state& state, dcon::army_id a) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	float scale = state.world.army_get_controller_from_army_control(a) ? 1.f : 0.9f; // Since army quality is evaluated, no need to devalue rebels so much
	// account general
	if(auto gen = state.world.army_get_general_from_army_leadership(a); gen) {
		auto n = state.world.army_get_controller_from_army_control(a);
		if(!n)
			n = state.world.national_identity_get_nation_from_identity_holder(state.national_definitions.rebel_id);
		auto back = military::get_leader_background_wrapper(state, gen);
		auto pers = military::get_leader_personality_wrapper(state, gen);
		float morale = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::org_regain)
			+ state.world.leader_trait_get_morale(back)
			+ state.world.leader_trait_get_morale(pers) + 1.0f;
		float org = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_organisation)
			+ state.world.leader_trait_get_organisation(back)
			+ state.world.leader_trait_get_organisation(pers) + 1.0f;
		float def = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_defense_modifier)
			+ state.world.leader_trait_get_defense(back)
			+ state.world.leader_trait_get_defense(pers) + 1.0f;
		scale += def * morale * org;
		scale += state.world.nation_get_has_gas_defense(n) ? 10.f : 0.f;
	}
	// terrain defensive bonus
	float terrain_bonus = state.world.province_get_modifier_values(state.world.army_get_location_from_army_location(a), sys::provincial_mod_offsets::defense);
	scale += terrain_bonus;
	float defender_fort = 1.0f + 0.1f * state.world.province_get_building_level(state.world.army_get_location_from_army_location(a), uint8_t(economy::province_building_type::fort));
	scale += defender_fort;
	// composition bonus and average unit quality
	float strength = estimate_balanced_composition_factor(state, a) * estimate_army_quality(state, a);
	return std::max(0.1f, strength * scale);
}

float estimate_army_offensive_strength(sys::state& state, dcon::army_id a) {
	if(state.cheat_data.disable_ai) {
		return 0.0f;
	}
	float scale = state.world.army_get_controller_from_army_control(a) ? 1.f : 0.9f; // Since army quality is evaluated, no need to devalue rebels so much
	// account general
	if(auto gen = state.world.army_get_general_from_army_leadership(a); gen) {
		auto n = state.world.army_get_controller_from_army_control(a);
		if(!n)
			n = state.world.national_identity_get_nation_from_identity_holder(state.national_definitions.rebel_id);
		auto back = military::get_leader_background_wrapper(state, gen);
		auto pers = military::get_leader_personality_wrapper(state, gen);
		float morale = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::org_regain)
			+ state.world.leader_trait_get_morale(back)
			+ state.world.leader_trait_get_morale(pers) + 1.0f;
		float org = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_organisation)
			+ state.world.leader_trait_get_organisation(back)
			+ state.world.leader_trait_get_organisation(pers) + 1.0f;
		float atk = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::land_attack_modifier)
			+ state.world.leader_trait_get_attack(back)
			+ state.world.leader_trait_get_attack(pers) + 1.0f;
		scale += atk * morale * org;
		scale += state.world.nation_get_has_gas_attack(n) ? 10.f : 0.f;
	}
	// composition bonus and average unit quality
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
	for (auto a : attacker) {
		auto nation = state.world.army_control_get_controller(state.world.army_get_army_control(a));
		float a_str = 0.f;
		for(const auto reg : state.world.army_get_army_membership(a)) {
			auto type = reg.get_regiment().get_type();
			auto stats = state.world.nation_get_unit_stats(nation, type);
			auto& atk = (stats.discipline_or_evasion > 0.0f) ? stats.attack_or_gun_power : state.military_definitions.unit_base_definitions[type].attack_or_gun_power;
			auto& def = (stats.discipline_or_evasion > 0.0f) ? stats.defence_or_hull : state.military_definitions.unit_base_definitions[type].defence_or_hull;
			auto& sup = (stats.discipline_or_evasion > 0.0f) ? stats.support : state.military_definitions.unit_base_definitions[type].support;
			a_str += (atk + def) * reg.get_regiment().get_strength() * reg.get_regiment().get_org();
		}
		attacker_str += a_str;
		attacker_tactic += a_str * state.world.nation_get_modifier_values(nation, sys::national_mod_offsets::military_tactics);
	}

	if(attacker_str > 0.f) {
		attacker_tactic = attacker_tactic / attacker_str;
	} else {
		attacker_tactic = 0.f;
	}

	float dig_in = 0.f;
	float defender_str = 0.f;
	float defender_tactic = 0.f;
	for (auto a : defender) {
		auto nation = state.world.army_control_get_controller(state.world.army_get_army_control(a));
		float a_str = 0.f;
		for(const auto reg : state.world.army_get_army_membership(a)) {
			auto type = reg.get_regiment().get_type();
			auto stats = state.world.nation_get_unit_stats(nation, type);
			auto& atk = (stats.discipline_or_evasion > 0.0f) ? stats.attack_or_gun_power : state.military_definitions.unit_base_definitions[type].attack_or_gun_power;
			auto& def = (stats.discipline_or_evasion > 0.0f) ? stats.defence_or_hull : state.military_definitions.unit_base_definitions[type].defence_or_hull;
			auto& sup = (stats.discipline_or_evasion > 0.0f) ? stats.support : state.military_definitions.unit_base_definitions[type].support;
			a_str += (atk + def) * reg.get_regiment().get_strength() * reg.get_regiment().get_org();
		}
		defender_str += a_str;
		defender_tactic += a_str * state.world.nation_get_modifier_values(nation, sys::national_mod_offsets::military_tactics);
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
		// if its no leader, skip
		if(!candidate) {
			continue;
		}
		auto score = military::get_leader_select_score(state, candidate, true);
		if(score > a_score) {
			a_lid = candidate;
			a_score = score;
		}
	}

	dcon::leader_id d_lid;
	float d_score = -999.f;
	for(const auto a : attacker) {
		auto candidate = state.world.army_get_general_from_army_leadership(a);
		// if its no leader, skip
		if(!candidate) {
			continue;
		}
		auto score = military::get_leader_select_score(state, candidate, false);
		if(score > d_score) {
			d_lid = candidate;
			d_score = score;
		}
	}

	auto attacker_leader_str = 0.f;
	auto attacker_general = a_lid;
	if (attacker_general) {
		auto back = military::get_leader_background_wrapper(state, attacker_general);
		auto pers = military::get_leader_personality_wrapper(state, attacker_general);
		attacker_leader_str = state.world.leader_trait_get_attack(back) + state.world.leader_trait_get_attack(pers);
	} else {
		attacker_leader_str = -2;
	}

	auto defender_leader_str = 0.f;
	auto defender_general = d_lid;
	if(defender_general) {
		auto back = military::get_leader_background_wrapper(state, defender_general);
		auto pers = military::get_leader_personality_wrapper(state, defender_general);
		defender_leader_str = state.world.leader_trait_get_defense(back) + state.world.leader_trait_get_defense(pers);
	} else {
		defender_leader_str = -1;
	}

	float probability = predictions::battle_win_probability(
		attacker_str, defender_str,
		dig_in,
		(float)(crossing),
		state.world.province_get_modifier_values(attack_toward, sys::provincial_mod_offsets::defense),
		attacker_tactic,
		defender_tactic,
		attacker_leader_str,
		defender_leader_str,
		state.world.leader_get_prestige(attacker_general),
		state.world.leader_get_prestige(defender_general)
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
	return state.defines.alice_ai_offensive_strength_overestimate * strength_total;
}

void get_enemy_defensive_force(sys::state& state, dcon::province_id target, dcon::nation_id by, std::vector<dcon::army_id>& result) {
	if(state.cheat_data.disable_ai) {
		return;
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

void assign_targets(sys::state& state, dcon::nation_id n) {
	struct a_str {
		dcon::province_id p;
		float str = 0.0f;
	};
	std::vector<a_str> ready_armies;
	bool const use_pressure = ai::pressure_enabled(state);
	ai::pressure_field* attack_field = nullptr;
	if(use_pressure) {
		attack_field = &ai::cached_tactical_field(state, n);
	}
	ready_armies.reserve(state.world.province_size());

	int32_t ready_count = 0;
	for(auto ar : state.world.nation_get_army_control(n)) {
		army_activity activity = army_activity(ar.get_army().get_ai_activity());
		if(ar.get_army().get_battle_from_army_battle_participation()
			|| ar.get_army().get_navy_from_army_transport()
			|| ar.get_army().get_black_flag()
			|| ar.get_army().get_arrival_time()
			|| activity != army_activity::on_guard
			|| !army_ready_for_battle(state, n, ar.get_army())) {

			continue;
		}

		++ready_count;
		auto loc = ar.get_army().get_location_from_army_location().id;
		if(std::find_if(ready_armies.begin(), ready_armies.end(), [loc](a_str const& v) { return loc == v.p; }) == ready_armies.end()) {
			ready_armies.push_back(a_str{ loc, 0.0f });
		}
	}

	if(ready_armies.empty())
		return; // nothing to attack with

	struct army_target {
		float minimal_distance;
		dcon::province_id location;
		float strength_estimate = 0.0f;
	};

	/* Ourselves */
	std::vector<army_target> potential_targets;
	potential_targets.reserve(state.world.province_size());
	for(auto o : state.world.nation_get_province_ownership(n)) {
		if(!o.get_province().get_nation_from_province_control()
			|| military::rebel_army_in_province(state, o.get_province())
			) {
			potential_targets.push_back(
				army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
			);
		}
	}
	/* Nations we're at war with OR hostile to */
	std::vector<dcon::nation_id> at_war_with;
	at_war_with.reserve(state.world.nation_size());
	for(auto w : state.world.nation_get_war_participant(n)) {
		auto attacker = w.get_is_attacker();
		for(auto p : w.get_war().get_war_participant()) {
			if(p.get_is_attacker() != attacker) {
				if(std::find(at_war_with.begin(), at_war_with.end(), p.get_nation().id) == at_war_with.end()) {
					at_war_with.push_back(p.get_nation().id);
				}
			}
		}
	}
	for(auto w : at_war_with) {
		for(auto o : state.world.nation_get_province_control(w)) {
			potential_targets.push_back(
				army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
			);
		}
		for(auto o : state.world.nation_get_province_ownership(w)) {
			if(!o.get_province().get_nation_from_province_control()) {
				potential_targets.push_back(
					army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id,0.0f }
				);
			}
		}
	}
	/* Our allies (mainly our substates, vassals) - we need to care of them! */
	for(const auto ovr : state.world.nation_get_overlord_as_ruler(n)) {
		auto w = ovr.get_subject();
		for(auto o : state.world.nation_get_province_ownership(w)) {
			if(!o.get_province().get_nation_from_province_control()
				|| military::rebel_army_in_province(state, o.get_province())
				) {
				potential_targets.push_back(
					army_target{ province::sorting_distance(state, o.get_province(), ready_armies[0].p), o.get_province().id, 0.0f }
				);
			}
		}
	}

	bool has_any_frontline_target = false;

	for(auto& pt : potential_targets) {
		for(uint32_t i = uint32_t(ready_armies.size()); i-- > 1;) {
			auto sdist = province::sorting_distance(state, ready_armies[i].p, pt.location);
			if(sdist < pt.minimal_distance) {
				pt.minimal_distance = sdist;
			}
		}

		// Frontline defense: heavily penalize non-frontline hostile provinces to avoid detours ("pawn-eating")
		auto fat_prov = dcon::fatten(state.world, pt.location);
		auto ctrl = fat_prov.get_nation_from_province_control();

		if(ctrl && ctrl != n && !military::are_allied_in_war(state, n, ctrl)) {
			bool borders_friendly = false;
			for(auto padj : fat_prov.get_province_adjacency()) {
				auto other = padj.get_connected_provinces(0) == fat_prov ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				auto other_ctrl = other.get_nation_from_province_control();

				auto other_ovr = other_ctrl.get_overlord_as_subject().get_ruler();
				if(other_ctrl && (other_ctrl == n
					|| military::are_allied_in_war(state, n, other_ctrl)
					|| (state.world.nation_get_in_sphere_of(other_ctrl) == n && !military::are_at_war(state, n, other_ctrl))
					|| (other_ovr && other_ovr == n && !military::are_at_war(state, n, other_ctrl)))) {
					borders_friendly = true;
					break;
				}
			}
			// Apply a massive penalty distance score to provinces deep in the enemy's rear
			if(!borders_friendly) {
				pt.minimal_distance += 100000.0f;
			} else {
				has_any_frontline_target = true;
			}
		} else {
			// Liberating owned/allied territory is always considered a frontline target
			has_any_frontline_target = true;
		}
	}

	std::sort(potential_targets.begin(), potential_targets.end(), [&](army_target& a, army_target& b) {
		if(a.minimal_distance != b.minimal_distance)
			return a.minimal_distance < b.minimal_distance;
		else
			return a.location.index() < b.location.index();
	});

	// organize attack stacks
	bool is_at_war = state.world.nation_get_is_at_war(n);
	int32_t min_ready_count = std::min(ready_count, 3); //Atleast 3 attacks
	int32_t max_attacks_to_make = is_at_war ? std::max(min_ready_count, (ready_count + 1) / 3) : ready_count; // not at war -- allow all stacks to attack rebels
	auto const psize = potential_targets.size();

	for(uint32_t i = 0; i < psize && max_attacks_to_make > 0; ++i) {
		if(!potential_targets[i].location)
			continue; // target has been removed as too close by some earlier iteration

		// If frontline objectives exist, strictly forbid attacks deep in the rear
		if(has_any_frontline_target && potential_targets[i].minimal_distance > 50000.0f)
			break;

		if(potential_targets[i].strength_estimate == 0.0f)
			potential_targets[i].strength_estimate = estimate_enemy_defensive_force(state, potential_targets[i].location, n) + 0.00001f;

		auto target_attack_force = potential_targets[i].strength_estimate;
		std::sort(ready_armies.begin(), ready_armies.end(), [&](a_str const& a, a_str const& b) {
			auto d_a = province::sorting_distance(state, a.p, potential_targets[i].location);
			auto d_b = province::sorting_distance(state, b.p, potential_targets[i].location);
			if(d_a != d_b)
				return d_a > d_b;
			else
				return a.p.index() < b.p.index();
		});

		// make list of attackers
		float a_force_str = 0.f;
		int32_t k = int32_t(ready_armies.size());
		for(; k-- > 0 && a_force_str <= target_attack_force;) {
			if(ready_armies[k].str == 0.0f) {
				float extracted_weight = 0.0f;
				for(auto ar : state.world.province_get_army_location(ready_armies[k].p)) {
					if(ar.get_army().get_battle_from_army_battle_participation()
						|| n != ar.get_army().get_controller_from_army_control()
						|| ar.get_army().get_navy_from_army_transport()
						|| ar.get_army().get_black_flag()
						|| ar.get_army().get_arrival_time()
						|| army_activity(ar.get_army().get_ai_activity()) != army_activity::on_guard
						|| !army_ready_for_battle(state, n, ar.get_army())) {

						continue;
					}

					// Do not strip the frontline on other sectors if there is an active threat in front of us
					auto loc_fat = ar.get_army().get_location_from_army_location();
					bool should_hold_frontline = false;
					float const army_w = use_pressure ? ai::army_pressure_weight(state, ar.get_army().id) : 0.0f;

					if(use_pressure) {
						bool is_frontline = false;
						for(auto padj : loc_fat.get_province_adjacency()) {
							auto other = padj.get_connected_provinces(0) == loc_fat ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
							if(other.id != potential_targets[i].location) {
								auto n_controller = other.get_nation_from_province_control();
								if(other.get_rebel_faction_from_province_rebel_control() || (n_controller && military::are_at_war(state, n, n_controller))) {
									is_frontline = true;
									break;
								}
							}
						}

						if(is_frontline) {
							float const facing = attack_field->hostile_at(loc_fat.id);
							float const cover = attack_field->friendly_at(loc_fat.id);

							float const hold_ratio = std::max(0.0f, state.defines.alice_ai_hold_ratio);
							float const token_pressure = std::max(0.0f, state.defines.alice_ai_token_pressure);

							bool const overwhelm = facing <= token_pressure;
							// Deduct extracted_weight so multiple armies in the same province don't overestimate available cover
							bool const cover_remains = std::max(0.0f, cover - extracted_weight - army_w) >= hold_ratio * facing;

							if(!overwhelm && !cover_remains) {
								should_hold_frontline = true;
							}
						}
					} else {
						for(auto padj : loc_fat.get_province_adjacency()) {
							auto other = padj.get_connected_provinces(0) == loc_fat ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
							if(other.id != potential_targets[i].location && military::province_has_enemy_army(state, other.id, n)) {
								should_hold_frontline = true;
								break;
							}
						}
					}

					if(should_hold_frontline)
						continue; // Hold frontline sector instead of marching away on other attacks

					extracted_weight += army_w;
					ready_armies[k].str += estimate_army_offensive_strength(state, ar.get_army());
				}
				ready_armies[k].str += 0.00001f;
			}
			a_force_str += ready_armies[k].str;
		}

		if(a_force_str < target_attack_force) {
			continue; // Target is too strong for remaining available forces, skip and check others
		}

		// Find closest safe frontline assembly province
		dcon::province_id central_province;
		float minimal_distance = 100000.0f;

		province::for_each_land_province(state, [&](dcon::province_id p) {
			if(!province::has_safe_access_to_province(state, n, p))
				return;

			auto dist = province::sorting_distance(state, p, potential_targets[i].location);
			if(!central_province || dist < minimal_distance) {
				minimal_distance = dist;
				central_province = p;
			}
		});

		if(!central_province)
			continue;

		// issue safe-move gather command
		for(int32_t m = int32_t(ready_armies.size()); m-- > k + 1; ) {
			assert(m >= 0 && m < int32_t(ready_armies.size()));

			// СОЗДАЕМ БУФЕР, ЧТОБЫ НЕ СЛОМАТЬ ИТЕРАТОРЫ ПРИ СОЗДАНИИ НОВЫХ АРМИЙ
			std::vector<dcon::army_id> armies_to_process;
			for(auto ar : state.world.province_get_army_location(ready_armies[m].p)) {
				armies_to_process.push_back(ar.get_army().id);
			}

			// ТЕПЕРЬ БЕЗОПАСНО ПЕРЕБИРАЕМ АРМИИ ИЗ БУФЕРА
			for(auto arid : armies_to_process) {
				if(!state.world.army_is_valid(arid)) continue;
				auto ar_army = dcon::fatten(state.world, arid);

				if(ar_army.get_battle_from_army_battle_participation()
					|| n != ar_army.get_controller_from_army_control()
					|| ar_army.get_navy_from_army_transport()
					|| ar_army.get_black_flag()
					|| ar_army.get_arrival_time()
					|| army_activity(ar_army.get_ai_activity()) != army_activity::on_guard
					|| !army_ready_for_battle(state, n, ar_army)) {

					continue;
				}

				// DOUBLE-CHECK: Do not strip frontline on command issuance if threat was detected
				auto loc_fat = ar_army.get_location_from_army_location();
				bool should_hold_frontline = false;
				float const army_w = use_pressure ? ai::army_pressure_weight(state, arid) : 0.0f;

				if(use_pressure) {
					bool is_frontline = false;
					for(auto padj : loc_fat.get_province_adjacency()) {
						auto other = padj.get_connected_provinces(0) == loc_fat ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
						if(other.id != potential_targets[i].location) {
							auto n_controller = other.get_nation_from_province_control();
							if(other.get_rebel_faction_from_province_rebel_control() || (n_controller && military::are_at_war(state, n, n_controller))) {
								is_frontline = true;
								break;
							}
						}
					}

					if(is_frontline) {
						float const facing = attack_field->hostile_at(loc_fat.id);
						float const cover = attack_field->friendly_at(loc_fat.id);

						float const hold_ratio = std::max(0.0f, state.defines.alice_ai_hold_ratio);
						float const token_pressure = std::max(0.0f, state.defines.alice_ai_token_pressure);

						bool const overwhelm = facing <= token_pressure;
						bool const cover_remains = std::max(0.0f, cover - army_w) >= hold_ratio * facing;

						if(!overwhelm && !cover_remains) {
							should_hold_frontline = true;
						}
					}
				} else {
					for(auto padj : loc_fat.get_province_adjacency()) {
						auto other = padj.get_connected_provinces(0) == loc_fat ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
						if(other.id != potential_targets[i].location && military::province_has_enemy_army(state, other.id, n)) {
							should_hold_frontline = true;
							break;
						}
					}
				}

				if(should_hold_frontline) {
					continue; // Hold frontline sector
				}

				dcon::army_id army_to_send = split_army_for_weight(state, n, arid, target_attack_force);
				float const actual_w = use_pressure ? ai::army_pressure_weight(state, army_to_send) : 0.0f;

				if(use_pressure && actual_w > 0.0f) {
					ai::debit_friendly_at(state, *attack_field, loc_fat.id, actual_w, n);
				}

				if(ready_armies[m].p == central_province) {
					state.world.army_set_ai_province(army_to_send, potential_targets[i].location);
					state.world.army_set_ai_activity(army_to_send, uint8_t(army_activity::attacking));
				} else if(auto path = province::make_safe_land_path(state, ready_armies[m].p, central_province, n); !path.empty()) {
					military::set_army_path(state, army_to_send, path, n);
					state.world.army_set_ai_province(army_to_send, potential_targets[i].location);
					state.world.army_set_ai_activity(army_to_send, uint8_t(army_activity::attacking));
				}
			}
		}

		ready_armies.resize(k + 1);
		--max_attacks_to_make;

		// remove subsequent targets that are too close
		if(is_at_war) {
			for(uint32_t j = i + 1; j < psize; ++j) {
				if(province::sorting_distance(state, potential_targets[j].location, potential_targets[i].location) < state.defines.alice_ai_attack_target_radius)
					potential_targets[j].location = dcon::province_id{};
			}
		}
	}
}

void make_attacks(sys::state& state) {
	for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
		dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
		if(state.world.nation_is_valid(n)) {
			assign_targets(state, n);
		}
	}
}

void make_defense(sys::state& state, bool at_war_only) {
	concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
		dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
		if(!state.world.nation_is_valid(n))
			return;
		// A nation at peace has nothing to react to, so it keeps the cheap cadence and
		// only the belligerents pay for the frequent passes.
		if(at_war_only && !state.world.nation_get_is_at_war(n))
			return;

		distribute_guards(state, n);
	});
}

void move_gathered_attackers(sys::state& state) {
	static std::vector<dcon::army_id> require_transport;
	require_transport.clear();

	for(auto ar : state.world.in_army) {
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
			if(!ar.get_arrival_time()
				&& !ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()) {

				if(ar.get_location_from_army_location() == ar.get_ai_province()) { // attack finished ?
					if(ar.get_location_from_army_location().get_nation_from_province_control() && !military::are_at_war(state, ar.get_location_from_army_location().get_nation_from_province_control(), ar.get_controller_from_army_control())) {

						ar.set_ai_activity(uint8_t(army_activity::on_guard));
						ar.set_ai_province(dcon::province_id{});
					}
				} else {
					if(province::has_access_to_province(state, ar.get_controller_from_army_control(), ar.get_ai_province())) {
						auto valid_path = military::move_army_ai(state, ar, ar.get_ai_province(), ar.get_controller_from_army_control());
						if(!valid_path) {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						}
					} else {
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
						ar.set_ai_province(dcon::province_id{});
					}
				}
			}
		} else if(ar.get_ai_activity() == uint8_t(army_activity::attacking)
			&& ar.get_ai_province() != ar.get_location_from_army_location()
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()) {

			bool all_gathered = true;
			for(auto o : ar.get_controller_from_army_control().get_army_control()) {
				if(o.get_army().get_ai_province() == ar.get_ai_province()) {
					if(ar.get_location_from_army_location() != o.get_army().get_location_from_army_location()) {
						// an army with the same target on a different location
						if(o.get_army().get_path().size() > 0 && o.get_army().get_path()[0] == ar.get_location_from_army_location()) {
							all_gathered = false;
							break;
						}
					} else {
						// on same location
						if(o.get_army().get_battle_from_army_battle_participation()) { // is in a battle
							all_gathered = false;
							break;
						}
					}
				}
			}

			if(all_gathered) {
				if(province::has_access_to_province(state, ar.get_controller_from_army_control(), ar.get_ai_province())) {
					auto army_loc = ar.get_location_from_army_location();
					auto target_prov = ar.get_ai_province();

					if(target_prov == army_loc) {
						for(auto o : army_loc.get_army_location()) {
							if(o.get_army().get_ai_province() == target_prov
								&& o.get_army().get_path().size() == 0) {

								o.get_army().set_ai_activity(uint8_t(army_activity::attack_gathered));
							}
						}
					} else {
						// Land march is only allowed directly to adjacent provinces or via safe paths (owned/occupied)
						bool is_adjacent = province::provinces_are_adjacent(state, army_loc.id, target_prov);
						bool is_safe_path = false;
						if(!is_adjacent) {
							auto safe_path = province::make_safe_land_path(state, army_loc.id, target_prov, ar.get_controller_from_army_control());
							if(!safe_path.empty()) {
								is_safe_path = true;
							}
						}

						// Land march is permitted ONLY to adjacent sectors or via safe territory
						if(is_adjacent || is_safe_path) {
							if(auto path = province::make_land_unit_path(state, army_loc.id, target_prov, ar.get_controller_from_army_control(), ar); path.size() > 0) {
								for(auto o : army_loc.get_army_location()) {
									if(o.get_army().get_ai_province() == target_prov
										&& o.get_army().get_path().size() == 0) {

										military::set_army_path(state, o.get_army(), path, o.get_army().get_controller_from_army_control());
										o.get_army().set_ai_activity(uint8_t(army_activity::attack_gathered));
									}
								}
							} else {
								ar.set_ai_activity(uint8_t(army_activity::on_guard));
								ar.set_ai_province(dcon::province_id{});
							}
						} else if(state.world.province_get_is_coast(target_prov)) {
							// Non-adjacent coastal targets MUST trigger naval transport instead of walking across continents
							for(auto o : army_loc.get_army_location()) {
								if(o.get_army().get_ai_province() == target_prov
									&& o.get_army().get_path().size() == 0) {

									require_transport.push_back(o.get_army().id);
									ar.set_ai_activity(uint8_t(army_activity::attack_transport));
								}
							}
						} else {
							// Distant continental targets with no direct safe access -> cancel march to avoid attrition deaths
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
							ar.set_ai_province(dcon::province_id{});
						}
					}
				} else {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
				}
			}
		}
	}

	for(uint32_t i = 0; i < require_transport.size(); ++i) {
		auto coastal_target_prov = state.world.army_get_location_from_army_location(require_transport[i]);
		auto controller = state.world.army_get_controller_from_army_control(require_transport[i]);

		dcon::navy_id transport_fleet = find_transport_fleet(state, controller);

		auto regs = state.world.army_get_army_membership(require_transport[i]);

		auto tcap = military::transport_capacity(state, transport_fleet);
		tcap -= int32_t(regs.end() - regs.begin());

		if(tcap < 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
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
						state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_attack));
						tcap -= int32_t(jregs.end() - jregs.begin());
					} else {
						auto valid_path = military::move_army_ai(state, require_transport[j], coastal_target_prov, controller);
						if(valid_path) {
							state.world.army_set_ai_activity(require_transport[i], uint8_t(army_activity::transport_attack));
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

void update_land_constructions(sys::state& state) {
	for(auto n : state.world.in_nation) {
		if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
			continue;
		auto disarm = n.get_disarmed_until();
		if(disarm && state.current_date < disarm)
			continue;

		static std::vector<dcon::province_land_construction_id> hopeless_construction;
		hopeless_construction.clear();

		state.world.nation_for_each_province_land_construction(n, [&](dcon::province_land_construction_id plcid) {
			auto fat_plc = dcon::fatten(state.world, plcid);
			auto prov = fat_plc.get_pop().get_province_from_pop_location();
			if(prov.get_nation_from_province_control() != n)
				hopeless_construction.push_back(plcid);
		});

		for(auto item : hopeless_construction) {
			state.world.delete_province_land_construction(item);
		}			

		auto constructions = state.world.nation_get_province_land_construction(n);
		if(constructions.begin() != constructions.end())
			continue;

		int32_t num_frontline = 0;
		int32_t num_support = 0;

		// Nation-wide best unit types
		auto inf_type = military::get_best_infantry(state, n);
		auto art_type = military::get_best_artillery(state, n);
		military::unit_definition art_def;
		if (art_type)
			art_def = state.military_definitions.unit_base_definitions[art_type];
		bool art_req_pc = art_def.primary_culture;
		auto cav_type = military::get_best_cavalry(state, n);
		if(will_upgrade_regiments(state, n)) {
			for(auto ar : state.world.nation_get_army_control(n)) {
				for(auto r : ar.get_army().get_army_membership()) {
					auto type = r.get_regiment().get_type();
					auto etype = state.military_definitions.unit_base_definitions[type].type;
					if(etype == military::unit_type::support || etype == military::unit_type::special) {
						++num_support;
					} else {
						++num_frontline;
					}

					/* AI units upgrade
					* AI upgrades units only if less than 10% of the army is currently under 80% strength (requiring supplies for reinforcement)
					*/

					auto primary_culture = r.get_regiment().get_pop_from_regiment_source().get_culture() == n.get_primary_culture();

					// AI can upgrade into primary-culture-specific units such as guards
					if(primary_culture) {
						auto pc_adj_inf_type = military::get_best_infantry(state, n, primary_culture);
						auto pc_adj_art_type = military::get_best_artillery(state, n, primary_culture);
						auto pc_adj_cav_type = military::get_best_cavalry(state, n, primary_culture);

						switch(etype) {
						case military::unit_type::infantry:
						{
							if(military::can_change_land_unit_type<command::actor::ai>(state, n, r.get_regiment(), pc_adj_inf_type) && military::is_infantry_better(state, n, type, pc_adj_inf_type)) {
								military::upgrade_regiment(state, r.get_regiment(), pc_adj_inf_type);
							}
							break;
						}
						case military::unit_type::support:
						{
							if(military::can_change_land_unit_type<command::actor::ai>(state, n, r.get_regiment(), pc_adj_art_type) && military::is_artillery_better(state, n, type, pc_adj_art_type)) {
								military::upgrade_regiment(state, r.get_regiment(), pc_adj_art_type);
							}
							break;
						}
						// cavalry
						default:
						{
							if(military::can_change_land_unit_type<command::actor::ai>(state, n, r.get_regiment(), pc_adj_cav_type) && military::is_cavalry_better(state, n, type, pc_adj_cav_type)) {
								military::upgrade_regiment(state, r.get_regiment(), pc_adj_cav_type);
							}
							break;
						}
						}
					}
					// Keep non-primary-culture units as nation-wide best units
					else {
						switch(etype) {
						case military::unit_type::infantry:
						{
							if(military::can_change_land_unit_type<command::actor::ai>(state, n, r.get_regiment(), inf_type) && military::is_infantry_better(state, n, type, inf_type)) {
								military::upgrade_regiment(state, r.get_regiment(), inf_type);
							}
							break;
						}
						case military::unit_type::support:
						{
							if(military::can_change_land_unit_type<command::actor::ai>(state, n, r.get_regiment(), art_type) && military::is_artillery_better(state, n, type, art_type)) {
								military::upgrade_regiment(state, r.get_regiment(), art_type);
							}
							break;
						}
						// cavalry
						default:
						{
							if(military::can_change_land_unit_type<command::actor::ai>(state, n, r.get_regiment(), cav_type) && military::is_cavalry_better(state, n, type, cav_type)) {
								military::upgrade_regiment(state, r.get_regiment(), cav_type);
							}
							break;
						}
						}
					}

				}
			}
		}
		

		const auto decide_type = [&](bool pc) {
			if(art_type && (!art_req_pc || (art_req_pc && pc))) {
				if(num_frontline > num_support) {
					++num_support;
					return art_type;
				} else {
					++num_frontline;
					return inf_type;
				}
			} else {
				return inf_type;
			}
		};

		auto num_to_build_nation = calculate_desired_army_size(state, n) - num_frontline - num_support;

		for(auto p : state.world.nation_get_province_ownership(n)) {
			if(p.get_province().get_nation_from_province_control() != n)
				continue;

			if(p.get_province().get_is_colonial()) {
				float divisor = state.defines.pop_size_per_regiment * state.defines.pop_min_size_for_regiment_colony_multiplier;
				float minimum = state.defines.pop_min_size_for_regiment;

				for(auto pop : p.get_province().get_pop_location()) {
					if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
						if(pop.get_pop().get_size() >= minimum) {
							auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
							auto regs = pop.get_pop().get_regiment_source();
							auto building = pop.get_pop().get_province_land_construction();
							auto num_to_make_local = amount - ((regs.end() - regs.begin()) + (building.end() - building.begin()));
							while(num_to_make_local > 0 && num_to_build_nation > 0) {
								auto t = decide_type(pop.get_pop().get_is_primary_or_accepted_culture());
								assert(command::can_start_land_unit_construction<true>(state, n, pop.get_province(), pop.get_pop().get_culture(), t));
								command::execute_start_land_unit_construction(state, n, pop.get_province(), pop.get_pop().get_culture(), t);
								--num_to_make_local;
								--num_to_build_nation;
							}
						}
					}
				}
			} else if(!p.get_province().get_is_owner_core()) {
				float divisor = state.defines.pop_size_per_regiment * state.defines.pop_min_size_for_regiment_noncore_multiplier;
				float minimum = state.defines.pop_min_size_for_regiment;

				dcon::pop_id non_preferred;
				for(auto pop : p.get_province().get_pop_location()) {
					if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
						if(pop.get_pop().get_size() >= minimum) {
							auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
							auto regs = pop.get_pop().get_regiment_source();
							auto building = pop.get_pop().get_province_land_construction();
							auto num_to_make_local = amount - ((regs.end() - regs.begin()) + (building.end() - building.begin()));
							while(num_to_make_local > 0 && num_to_build_nation > 0) {
								auto t = decide_type(pop.get_pop().get_is_primary_or_accepted_culture());
								assert(command::can_start_land_unit_construction<true>(state, n, pop.get_province(), pop.get_pop().get_culture(), t));
								command::execute_start_land_unit_construction(state, n, pop.get_province(), pop.get_pop().get_culture(), t);
								--num_to_make_local;
								--num_to_build_nation;
							}
						}
					}
				}
			} else {
				float divisor = state.defines.pop_size_per_regiment;
				float minimum = state.defines.pop_min_size_for_regiment;

				dcon::pop_id non_preferred;
				for(auto pop : p.get_province().get_pop_location()) {
					if(pop.get_pop().get_poptype() == state.culture_definitions.soldiers) {
						if(pop.get_pop().get_size() >= minimum) {
							auto amount = int32_t((pop.get_pop().get_size() / divisor) + 1);
							auto regs = pop.get_pop().get_regiment_source();
							auto building = pop.get_pop().get_province_land_construction();
							auto num_to_make_local = amount - ((regs.end() - regs.begin()) + (building.end() - building.begin()));
							while(num_to_make_local > 0 && num_to_build_nation > 0) {
								auto t = decide_type(pop.get_pop().get_is_primary_or_accepted_culture());
								assert(command::can_start_land_unit_construction<true>(state, n, pop.get_province(), pop.get_pop().get_culture(), t));
								command::execute_start_land_unit_construction(state, n, pop.get_province(), pop.get_pop().get_culture(), t);
								--num_to_make_local;
								--num_to_build_nation;
							}
						}
					}
				}
			}
		}
	}
}

void new_units_and_merging(sys::state& state) {
	for(auto ar : state.world.in_army) {
		auto controller = ar.get_controller_from_army_control();
		if(controller
			&& unit_on_ai_control(state, ar)
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_arrival_time()) {

			auto location = ar.get_location_from_army_location();

			if(ar.get_black_flag() || army_activity(ar.get_ai_activity()) == army_activity::unspecified || army_activity(ar.get_ai_activity()) == army_activity::on_guard) {
				auto regs = ar.get_army_membership();
				if(regs.begin() == regs.end()) {
					// empty army -- cleanup will get it
				} else if(regs.end() - regs.begin() > 1) {
					// existing multi-unit formation
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
				} else {
					auto type = (*regs.begin()).get_regiment().get_type();
					auto etype = state.military_definitions.unit_base_definitions[type].type;
					auto is_art = etype == military::unit_type::support;
					dcon::province_id target_location;
					float nearest_distance = 1.0f;

					// find army to merge with
					for(auto o : controller.get_army_control()) {
						auto other_location = o.get_army().get_location_from_army_location();
						auto sdist = province::sorting_distance(state, other_location, location);
						if(army_activity(o.get_army().get_ai_activity()) == army_activity::on_guard
							&& other_location.get_connected_region_id() == location.get_connected_region_id()
							&& (!target_location || sdist < nearest_distance)) {

							int32_t num_support = 0;
							int32_t num_frontline = 0;
							for(auto r : o.get_army().get_army_membership()) {
								auto stype = r.get_regiment().get_type();
								auto setype = state.military_definitions.unit_base_definitions[stype].type;
								if(setype == military::unit_type::support || setype == military::unit_type::special) {
									++num_support;
								} else {
									++num_frontline;
								}
							}

							if((is_art && num_support < 5) || (!is_art && num_frontline < 5)) {
								target_location = other_location;
								nearest_distance = sdist;
							}
						}
					}

					if(target_location) {
						if(target_location == location) {
							ar.set_ai_province(target_location);
							ar.set_ai_activity(uint8_t(army_activity::merging));
						} else if(bool valid_path = military::move_army_ai(state, ar, target_location, controller);  valid_path) {
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
				auto regs = ar.get_army_membership();
				if(regs.begin() == regs.end()) {
					// empty army -- cleanup will get it
					continue;
				}
				auto type = (*regs.begin()).get_regiment().get_type();
				auto etype = state.military_definitions.unit_base_definitions[type].type;
				auto is_art = etype == military::unit_type::support;
				for(auto o : location.get_army_location()) {
					if(o.get_army().get_ai_activity() == uint8_t(army_activity::on_guard)
						&& o.get_army().get_controller_from_army_control() == controller) {

						int32_t num_support = 0;
						int32_t num_frontline = 0;
						for(auto r : o.get_army().get_army_membership()) {
							auto stype = r.get_regiment().get_type();
							auto setype = state.military_definitions.unit_base_definitions[stype].type;
							if(setype == military::unit_type::support || setype == military::unit_type::special) {
								++num_support;
							} else {
								++num_frontline;
							}
						}

						if((is_art && num_support < 5) || (!is_art && num_frontline < 5)) {
							(*regs.begin()).get_regiment().set_army_from_army_membership(o.get_army());
							break;
						}
					}
				}
				ar.set_ai_activity(uint8_t(army_activity::unspecified)); // if merging fails, this will try to find a merge target again
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
