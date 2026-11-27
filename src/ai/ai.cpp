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
		// Landlocked nation shouldn't keep fleet (never delete a navy currently carrying embarked armies!)
		if(n.get_is_at_war() == false && nations::is_landlocked(state, n)) {
			for(auto v : n.get_navy_control()) {
				auto nv = v.get_navy();
				if(!nv.get_battle_from_navy_battle_participation()
					&& unit_on_ai_control(state, nv)
					&& nv.get_army_transport().begin() == nv.get_army_transport().end()) {
					to_delete.push_back(nv.id);
				}
			}
		} else if(n.get_is_at_war() == false) {
			dcon::unit_type_id best_transport = military::get_best_transport(state, n);
			dcon::unit_type_id best_light = military::get_best_light_ship(state, n);
			dcon::unit_type_id best_big = military::get_best_big_ship(state, n);
			
			for(auto v : n.get_navy_control()) {
				auto nv = v.get_navy();
				auto nv_act = fleet_activity(nv.get_ai_activity());
				if(nv_act == fleet_activity::boarding
					|| nv_act == fleet_activity::transporting
					|| nv_act == fleet_activity::unloading
					|| nv.get_army_transport().begin() != nv.get_army_transport().end()) {
					continue; // Never disrupt an active peacetime evacuation/ferry fleet mid-voyage!
				}
				for(auto shp : nv.get_navy_membership()) {
					auto type = shp.get_ship().get_type();
					if(!type)
						continue;

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
		if(n.get_is_player_controlled() || n.get_owned_province_count() == 0)
			continue;
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
		int32_t ongoing_naval_builds = 0;

		static std::vector<dcon::province_id> busy_ports;
		busy_ports.clear();

		for(auto v : n.get_navy_control()) {
			if(!unit_on_ai_control(state, v.get_navy())) {
				continue;
			}
			for(auto s : v.get_navy().get_navy_membership()) {
				auto type = s.get_ship().get_type();
				if(!type)
					continue;
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

		// Count ongoing naval constructions AND track busy shipyards so new ships spread across all free naval bases (engine only builds 1 ship at a time per port!)
		state.world.nation_for_each_province_naval_construction(n, [&](dcon::province_naval_construction_id pncid) {
			auto type = state.world.province_naval_construction_get_type(pncid);
			auto bprov = state.world.province_naval_construction_get_province(pncid);
			if(bprov) {
				busy_ports.push_back(bprov);
			}
			++ongoing_naval_builds;
			if(!type)
				return;
			if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::transport) {
				++num_transports;
				fleet_cap_in_transports += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
			} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::big_ship) {
				fleet_cap_in_big += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
			} else if(state.military_definitions.unit_base_definitions[type].type == military::unit_type::light_ship) {
				fleet_cap_in_small += state.military_definitions.unit_base_definitions[type].supply_consumption_score;
			}
		});

		auto is_port_busy = [&](dcon::province_id pid) -> bool {
			return std::find(busy_ports.begin(), busy_ports.end(), pid) != busy_ports.end();
		};

		static std::vector<dcon::province_id> owned_ports;
		owned_ports.clear();
		for(auto p : n.get_province_ownership()) {
			auto prov = p.get_province();
			if(prov.get_is_coast()
				&& prov.get_nation_from_province_control() == n
				&& prov.get_land_battle_location().begin() == prov.get_land_battle_location().end()
				&& !is_port_busy(prov.id)) {
				owned_ports.push_back(prov.id);
			}
		}
		if(owned_ports.empty())
			continue;
		int32_t max_concurrent_naval = std::clamp(int32_t(owned_ports.size() + busy_ports.size()), 4, 24);
		if(ongoing_naval_builds >= max_concurrent_naval)
			continue;

		auto cap = n.get_capital().id;
		std::sort(owned_ports.begin(), owned_ports.end(), [&](dcon::province_id a, dcon::province_id b) {
			if(cap) {
				auto a_dist = province::sorting_distance(state, a, cap);
				auto b_dist = province::sorting_distance(state, b, cap);
				if(a_dist != b_dist)
					return a_dist < b_dist;
			}
			return a.index() < b.index();
		});

		// Depending on the strategy, the AI will prioritize different fleet size
		auto target_naval_supply_points = calculate_desired_navy_size(state, n);
		// Scale amphibious lift to standing army (10..80 transports = 30K..240K single-wave lift for global maritime powers like UK/USA/Japan!)
		// Always respect naval supply cap for small coastal minors, and cap at 1/3 of naval supply cap above 10 transports so the Battle Fleet keeps 67-90% of capacity!
		int32_t desired_transports = std::clamp(std::max(10, int32_t(n.get_active_regiments()) / 2), 10, 80);

		int32_t constructing_fleet_cap = 0;
		if(best_transport) {
			auto overseas_allowed = state.military_definitions.unit_base_definitions[best_transport].can_build_overseas;
			auto level_req = state.military_definitions.unit_base_definitions[best_transport].min_port_level;
			int32_t supply_pts = std::max(1, int32_t(state.military_definitions.unit_base_definitions[best_transport].supply_consumption_score));
			int32_t max_base_transport_pts = std::max(supply_pts * 2, int32_t(target_naval_supply_points * 0.5f));

			for(uint32_t j = 0; j < owned_ports.size() && ongoing_naval_builds < max_concurrent_naval; ++j) {
				if(is_port_busy(owned_ports[j]))
					continue;
				int32_t next_t_pts = fleet_cap_in_transports + constructing_fleet_cap + supply_pts;
				bool need_transports = (num_transports < desired_transports)
					&& ((num_transports < 10 && next_t_pts <= max_base_transport_pts)
						|| float(next_t_pts * 3) <= target_naval_supply_points);
				if(!need_transports)
					break;
				if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
					&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req
					&& command::can_start_naval_unit_construction(state, n, owned_ports[j], best_transport)) {
					command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_transport);
					busy_ports.push_back(owned_ports[j]);
					++num_transports;
					++ongoing_naval_builds;
					constructing_fleet_cap += supply_pts;
				}
			}
		}

		auto rem_free = target_naval_supply_points - (fleet_cap_in_transports + fleet_cap_in_small + fleet_cap_in_big + constructing_fleet_cap);
		fleet_cap_in_small = std::max(fleet_cap_in_small, 1);
		fleet_cap_in_big = std::max(fleet_cap_in_big, 1);

		auto free_big_points = best_light ? rem_free * fleet_cap_in_small / (fleet_cap_in_small + fleet_cap_in_big) : rem_free;
		auto free_small_points = best_big ? rem_free * fleet_cap_in_big / (fleet_cap_in_small + fleet_cap_in_big) : rem_free;

		if(best_light) {
			auto overseas_allowed = state.military_definitions.unit_base_definitions[best_light].can_build_overseas;
			auto level_req = state.military_definitions.unit_base_definitions[best_light].min_port_level;
			auto supply_pts = state.military_definitions.unit_base_definitions[best_light].supply_consumption_score;

			for(uint32_t j = 0; j < owned_ports.size() && supply_pts <= free_small_points && ongoing_naval_builds < max_concurrent_naval; ++j) {
				if(is_port_busy(owned_ports[j]))
					continue;
				if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
					&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req
					&& command::can_start_naval_unit_construction(state, n, owned_ports[j], best_light)) {
					command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_light);
					busy_ports.push_back(owned_ports[j]);
					++ongoing_naval_builds;
					free_small_points -= supply_pts;
				}
			}
		}
		if(best_big) {
			auto overseas_allowed = state.military_definitions.unit_base_definitions[best_big].can_build_overseas;
			auto level_req = state.military_definitions.unit_base_definitions[best_big].min_port_level;
			auto supply_pts = state.military_definitions.unit_base_definitions[best_big].supply_consumption_score;

			for(uint32_t j = 0; j < owned_ports.size() && supply_pts <= free_big_points && ongoing_naval_builds < max_concurrent_naval; ++j) {
				if(is_port_busy(owned_ports[j]))
					continue;
				if((overseas_allowed || !province::is_overseas(state, owned_ports[j]))
					&& state.world.province_get_building_level(owned_ports[j], uint8_t(economy::province_building_type::naval_base)) >= level_req
					&& command::can_start_naval_unit_construction(state, n, owned_ports[j], best_big)) {
					command::execute_start_naval_unit_construction(state, n, owned_ports[j], best_big);
					busy_ports.push_back(owned_ports[j]);
					++ongoing_naval_builds;
					free_big_points -= supply_pts;
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
			int32_t lvl = int32_t(p.get_province().get_building_level(uint8_t(economy::province_building_type::naval_base)));
			float pdist = cap ? province::sorting_distance(state, cap, p.get_province()) : 0.0f;
			if(lvl > max_level) {
				max_level = lvl;
				result = p.get_province();
				current_distance = pdist;
			} else if(result && lvl == max_level && (pdist < current_distance || (pdist == current_distance && p.get_province().id.index() < result.index()))) {
				current_distance = pdist;
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



dcon::province_id find_best_friendly_disembark_port(sys::state& state, dcon::nation_id owner, dcon::province_id dest_prov, dcon::province_id naval_start, float max_ref_km = 1e9f);

// Immediately disembark any embarked armies when a transport navy is already docked in a friendly land port,
// preventing armies from being trapped aboard idle/failed_transport/home-port fleets forever!
bool emergency_unload_at_friendly_port(sys::state& state, dcon::navy_id n, dcon::nation_id owner) {
	auto transported = state.world.navy_get_army_transport(n);
	if(transported.begin() == transported.end())
		return true;
	auto loc = state.world.navy_get_location_from_navy_location(n);
	if(!loc || province::is_sea(state, loc) || !province::has_naval_access_to_province(state, owner, loc))
		return false;
	while(transported.begin() != transported.end()) {
		auto ar = (*transported.begin()).get_army();
		ar.set_navy_from_army_transport(dcon::navy_id{});
		ar.get_path().clear();
		ar.set_arrival_time(sys::date{});
		ar.set_unused_travel_days(0.0f);
		ar.set_ai_activity(uint8_t(army_activity::on_guard));
		ar.set_ai_province(dcon::province_id{});
	}
	return true;
}

void send_fleet_home(sys::state& state, dcon::navy_id n, fleet_activity moving_status = fleet_activity::returning_to_base, fleet_activity at_base = fleet_activity::idle) {
	auto v = fatten(state.world, n);
	auto owner = v.get_controller_from_navy_control().id;
	auto loc = v.get_location_from_navy_location().id;
	dcon::province_id home_port = owner ? state.world.nation_get_ai_home_port(owner).id : dcon::province_id{};
	bool carrying = (v.get_army_transport().begin() != v.get_army_transport().end());

	if(loc == home_port && home_port) {
		if(carrying) {
			emergency_unload_at_friendly_port(state, n, owner);
		}
		v.set_ai_activity(uint8_t(at_base));
	} else if(home_port && military::move_navy_ai(state, n, home_port)) {
		v.set_ai_activity(uint8_t(moving_status));
	} else {
		if(carrying) {
			if(emergency_unload_at_friendly_port(state, n, owner)) {
				v.set_ai_activity(uint8_t(fleet_activity::unspecified));
				return;
			}
			// Home port is unreachable (e.g. closed strait or lost): route to ANY sea-reachable friendly port to rescue embarked armies!
			dcon::province_id rescue_port;
			if(owner) {
				for(auto po : state.world.nation_get_province_ownership(owner)) {
					auto p = po.get_province();
					if(p.get_is_coast() && p.get_nation_from_province_control() == owner && p.id != loc) {
						if(military::move_navy_ai(state, n, p.id)) {
							rescue_port = p.id;
							break;
						}
					}
				}
			}
			if(rescue_port) {
				v.set_ai_activity(uint8_t(fleet_activity::failed_transport));
				return;
			}
		}
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
	auto owner = state.world.navy_get_controller_from_navy_control(n);

	// Friendly port arrival: military.cpp:9238-9268 ALREADY instantaneously disembarked every army with 0 sea-landing penalty!
	// Immediately release the transport fleet back to returning_to_base (available for Wave 2 right away instead of idling 16-24 days in port!)
	if(transported_armies.begin() == transported_armies.end()) {
		send_fleet_home(state, n);
		return;
	}

	// Edge case: transport fleet was already docked inside a friendly port without sailing — unload immediately on land with 0 penalty
	if(location && !province::is_sea(state, location) && province::has_naval_access_to_province(state, owner, location)) {
		while(transported_armies.begin() != transported_armies.end()) {
			auto ar = (*transported_armies.begin()).get_army();
			ar.set_navy_from_army_transport(dcon::navy_id{});
			ar.get_path().clear();
			ar.set_arrival_time(sys::date{});
			ar.set_unused_travel_days(0.0f);
			auto dest = ar.get_ai_province();
			auto activity = army_activity(ar.get_ai_activity());
			if(dest && dest != location && military::move_army_ai(state, ar, dest, ar.get_controller_from_army_control())) {
				if(activity == army_activity::transport_attack) {
					ar.set_ai_activity(uint8_t(army_activity::attack_gathered));
				} else {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
				}
			} else {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
			}
		}
		send_fleet_home(state, n);
		return;
	}

	// Mid-voyage port loss protection (military.cpp:9270-9274 stops navies offshore in sea tile if destination port fell mid-voyage!):
	// If another safe friendly port exists on the same landmass (e.g. Quebec after Halifax fell!), redirect to that safe port instead of suiciding onto the occupied beach!
	if(location && province::is_sea(state, location)) {
		auto first_army = (*transported_armies.begin()).get_army();
		auto first_dest = first_army.get_ai_province().id;
		auto first_act = army_activity(first_army.get_ai_activity());
		bool is_guard_ferry = (first_act == army_activity::transport_guard);
		if(first_dest) {
			float alt_max_km = is_guard_ferry ? 1e9f : 2200.0f;
			dcon::province_id alt_port = find_best_friendly_disembark_port(state, owner, first_dest, location, alt_max_km);
			if(alt_port && military::move_navy_ai(state, n, alt_port)) {
				state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::transporting));
				return;
			}
		}
		// Never silently suicide a guard reinforcement wave onto an enemy-occupied beach when all friendly ports fell!
		if(is_guard_ferry) {
			send_fleet_home(state, n, fleet_activity::failed_transport);
			return;
		}
	}

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

void advance_transport_navy_state(sys::state& state, dcon::navy_fat_id n, dcon::nation_fat_id owner, dcon::province_id location) {
	auto activity = fleet_activity(state.world.navy_get_ai_activity(n));
	if(activity == fleet_activity::boarding) {
		// Wait for ALL assigned embarkation stacks for THIS operation (transport_guard, transport_attack, and matching-target attack_transport!)
		// while NEVER stalling on an unrelated attack_transport on another ocean or when the fleet is already 100% full!
		bool all_loaded = true;
		int32_t free_tcap_rem = military::free_transport_capacity(state, n.id);
		if(free_tcap_rem > 0) {
			dcon::province_id embarked_dest;
			auto tr_rng = n.get_army_transport();
			if(tr_rng.begin() != tr_rng.end()) {
				embarked_dest = (*tr_rng.begin()).get_army().get_ai_province().id;
			}
			for(auto ar : state.world.nation_get_army_control(owner)) {
				auto army = ar.get_army();
				auto act = army_activity(army.get_ai_activity());
				if(act == army_activity::transport_guard || act == army_activity::transport_attack || act == army_activity::attack_transport) {
					if(army.get_battle_from_army_battle_participation() || army.get_is_retreating() || army.get_navy_from_army_transport())
						continue;
					auto mregs = army.get_army_membership();
					int32_t rcount = int32_t(mregs.end() - mregs.begin());
					if(rcount <= 0 || rcount > free_tcap_rem)
						continue;
					if(act == army_activity::attack_transport && embarked_dest && army.get_ai_province().id != embarked_dest)
						continue;
					all_loaded = false;
					break;
				}
			}
		}

		if(all_loaded) {
			auto transporting_range = n.get_army_transport();
			if(transporting_range.begin() == transporting_range.end()) { // failed to pick up troops
				send_fleet_home(state, n);
			} else {
				auto first_army = (*(transporting_range.begin())).get_army();
				auto transported_dest = first_army.get_ai_province();
				auto first_act = army_activity(first_army.get_ai_activity());
				if(!transported_dest) {
					send_fleet_home(state, n);
					return;
				}

				// Whenever heading to a friendly landmass (transport_guard, friendly port, safe destination, or attack with a friendly port <= 2200 km),
				// sail DIRECTLY into the best reachable friendly dockable port (0-day instant unload, 0 sea-crossing penalty, 0 arrival attrition!)
				bool is_guard_ferry = (first_act == army_activity::transport_guard);
				bool dest_is_safe = province::has_safe_access_to_province(state, owner, transported_dest);
				float port_search_km = (is_guard_ferry || dest_is_safe) ? 1e9f : 2200.0f;
				dcon::province_id dock_port = find_best_friendly_disembark_port(state, owner.id, transported_dest.id, location, port_search_km);
				if(dock_port) {
					if(dock_port == location) {
						unload_units_from_transport(state, n.id);
					} else if(military::move_navy_ai(state, n, dock_port)) {
						n.set_ai_activity(uint8_t(fleet_activity::transporting));
					} else {
						military::stop_navy_movement(state, n);
						send_fleet_home(state, n);
					}
					return;
				}
				if(is_guard_ferry) {
					// Destination port fell during boarding and no safe friendly port remains -> return safely home instead of hostile beach suicide!
					military::stop_navy_movement(state, n);
					send_fleet_home(state, n, fleet_activity::failed_transport);
					return;
				}

				if(transported_dest.get_is_coast()) { // move to closest friendly port or offshore landing tile
					auto target_prov = transported_dest.id;
					if(!province::has_naval_access_to_province(state, owner, target_prov)) {
						target_prov = state.world.province_get_port_to(target_prov).id;
					}
					if(target_prov == location) {
						unload_units_from_transport(state, n.id);
					} else if(target_prov && military::move_navy_ai(state, n, target_prov)) {
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
						target_prov = state.world.province_get_port_to(target_prov).id;
					}
					if(target_prov == location) {
						unload_units_from_transport(state, n.id);
					} else if(target_prov && military::move_navy_ai(state, n, target_prov)) {
						n.set_ai_activity(uint8_t(fleet_activity::transporting));
					} else {
						military::stop_navy_movement(state, n);
						send_fleet_home(state, n);
					}
				}
			}
		}
	} else if(activity == fleet_activity::transporting) {
		unload_units_from_transport(state, n);
	} else if(activity == fleet_activity::unloading) {
		bool failed_transport = true;
		auto transporting = state.world.navy_get_army_transport(n);
		for(auto ar : transporting) {
			if(ar.get_army().get_path().size() != 0) {
				failed_transport = false;
			}
		}

		if(transporting.begin() == transporting.end()) {
			// All unloaded -> immediately return to base (available for next ferry wave right away!)
			send_fleet_home(state, n);
		} else if(failed_transport) {
			// An army is stuck on the boats
			state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::failed_transport));
		}
	}
}

void advance_active_transport_fleets(sys::state& state) {
	for(auto n : state.world.in_navy) {
		if(n.get_battle_from_navy_battle_participation() || n.get_arrival_time())
			continue;
		auto activity = fleet_activity(n.get_ai_activity());
		if(activity != fleet_activity::boarding
			&& activity != fleet_activity::transporting
			&& activity != fleet_activity::unloading) {
			continue;
		}
		auto owner = n.get_controller_from_navy_control();
		if(!owner || owner.get_is_player_controlled() || owner.get_owned_province_count() == 0 || !owner.get_is_at_war() || !unit_on_ai_control(state, n))
			continue;
		advance_transport_navy_state(state, n, owner, n.get_location_from_navy_location().id);
	}
}

bool merge_fleet(sys::state& state, dcon::navy_id n, dcon::province_id p, dcon::nation_id owner) {
	// Keep the Battle/Guard Fleet (0 transports) and Transport/Invasion Fleet (transports + light escort) STRICTLY SEPARATE!
	// Transports merge ONLY with transports (consolidating 100% of lift into one massive Invasion Fleet);
	// pure warships merge ONLY with pure warships (forming one unified Battle Fleet that never drags transports into combat!).
	bool self_has_transports = (military::transport_capacity(state, n) > 0);
	auto merge_target = [&]() {
		dcon::navy_id largest;
		int32_t largest_size = 0;
		for(auto on : state.world.province_get_navy_location(p)) {
			auto other = on.get_navy();
			if(other == n
				|| other.get_controller_from_navy_control() != owner
				|| !unit_on_ai_control(state, other)
				|| other.get_battle_from_navy_battle_participation()
				|| other.get_arrival_time())
				continue;
			auto oact = fleet_activity(other.get_ai_activity());
			if(oact == fleet_activity::boarding || oact == fleet_activity::transporting || oact == fleet_activity::unloading)
				continue;
			bool other_has_transports = (military::transport_capacity(state, other.id) > 0);
			if(other_has_transports != self_has_transports)
				continue;
			auto other_mem = other.get_navy_membership();
			if(auto sz = int32_t(other_mem.end() - other_mem.begin()); sz > largest_size) {
				largest = other.id;
				largest_size = sz;
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
	struct navy_split_req {
		dcon::navy_id navy;
		dcon::nation_id owner;
		std::vector<dcon::ship_id> warships_to_split;
	};
	std::vector<navy_split_req> fleet_splits;

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

		// Rescue any embarked armies stuck aboard an idle/returning/failed_transport fleet!
		if(activity != fleet_activity::boarding
			&& activity != fleet_activity::transporting
			&& activity != fleet_activity::unloading
			&& n.get_army_transport().begin() != n.get_army_transport().end()) {
			if(!emergency_unload_at_friendly_port(state, n.id, owner.id)) {
				send_fleet_home(state, n.id, fleet_activity::failed_transport);
				continue;
			}
		}

		// Separate mixed fleets at home_port into dedicated Battle/Guard Fleet (heavy warships, 0 transports)
		// + dedicated Transport/Invasion Fleet (100% transports + light close escort)!
		bool queued_split = false;
		if(location == home_port
			&& (activity == fleet_activity::unspecified || activity == fleet_activity::idle || activity == fleet_activity::merging || activity == fleet_activity::returning_to_base || activity == fleet_activity::failed_transport)
			&& n.get_army_transport().begin() == n.get_army_transport().end()
			&& !n.get_is_retreating()) {
			int32_t t_count = 0;
			std::vector<dcon::ship_id> big_warships;
			std::vector<dcon::ship_id> light_warships;
			for(auto s : n.get_navy_membership()) {
				auto shp = s.get_ship();
				auto shp_type = shp.get_type();
				if(!shp_type)
					continue;
				auto stype = state.military_definitions.unit_base_definitions[shp_type].type;
				if(stype == military::unit_type::transport) {
					++t_count;
				} else if(stype == military::unit_type::big_ship) {
					big_warships.push_back(shp.id);
				} else {
					light_warships.push_back(shp.id);
				}
			}
			int32_t total_warships = int32_t(big_warships.size() + light_warships.size());
			if(t_count > 0 && total_warships >= 4) {
				// Keep a modest close escort (preferring fast light ships, up to min(t_count/3, 10)) with the Transport Fleet,
				// and split the main Battle/Guard Fleet (all heavy big ships + surplus light ships) out permanently!
				int32_t keep_escorts = (total_warships >= 6) ? std::min(t_count / 3, 10) : 0;
				if(total_warships > keep_escorts + 2) {
					int32_t split_target = total_warships - keep_escorts;
					std::vector<dcon::ship_id> to_split;
					to_split.reserve(size_t(split_target));
					for(auto sid : big_warships) {
						if(int32_t(to_split.size()) < split_target) {
							to_split.push_back(sid);
						}
					}
					for(auto sid : light_warships) {
						if(int32_t(to_split.size()) < split_target) {
							to_split.push_back(sid);
						}
					}
					if(!to_split.empty()) {
						fleet_splits.push_back(navy_split_req{ n.id, owner.id, std::move(to_split) });
						queued_split = true;
					}
				}
			}
		}

		// If this mixed fleet is about to split its Battle Fleet out right after this loop,
		// do NOT merge it or send it sailing into battle as attacking BEFORE the split runs!
		if(queued_split) {
			state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
			continue;
		}

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
		case fleet_activity::transporting:
			advance_transport_navy_state(state, n, owner, location);
			break;
		case fleet_activity::failed_transport:
			if(location == home_port) {
				emergency_unload_at_friendly_port(state, n.id, owner.id);
				if(!merge_fleet(state, n, location, owner))
					state.world.navy_set_ai_activity(n, uint8_t(fleet_activity::idle));
			} else {
				send_fleet_home(state, n, fleet_activity::failed_transport);
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
					// Keep the dedicated Transport/Invasion Fleet (tcap > 0) free at port ready for landings!
					// Only send pure Battle/Guard Fleets (tcap == 0) out to hunt enemy fleets (fallback only if no pure battle fleet exists).
					int32_t self_tcap = military::transport_capacity(state, n.id);
					auto self_ships = state.world.navy_get_navy_membership(n);
					int32_t self_sz = int32_t(self_ships.end() - self_ships.begin());
					int32_t self_warships = self_sz - self_tcap;

					bool has_dedicated_battle_fleet = false;
					bool valid_attacker = (self_warships > 0);
					for(auto o : owner.get_navy_control()) {
						auto onv = o.get_navy();
						if(military::transport_capacity(state, onv.id) == 0 && onv.get_navy_membership().begin() != onv.get_navy_membership().end()) {
							has_dedicated_battle_fleet = true;
						}
						if(onv != n) {
							if(onv.get_ai_activity() == uint8_t(fleet_activity::attacking)) {
								valid_attacker = false;
								break;
							}
							if(self_tcap == 0 && military::transport_capacity(state, onv.id) == 0) {
								auto orange = onv.get_navy_membership();
								if(int32_t(orange.end() - orange.begin()) > self_sz
									|| (int32_t(orange.end() - orange.begin()) == self_sz && onv.id.index() < n.id.index())) {
									valid_attacker = false;
									break;
								}
							}
						}
					}
					if(self_tcap > 0 && (has_dedicated_battle_fleet || self_warships <= self_tcap * 2)) {
						valid_attacker = false;
					}
					if(valid_attacker && naval_advantage(state, owner)) {
						set_fleet_target(state, owner, state.world.navy_get_location_from_navy_location(n), n);
					}
				}
			}
			break;
		case fleet_activity::unloading:
			advance_transport_navy_state(state, n, owner, location);
			break;
		}
	}

	// Execute Battle Fleet / Transport Fleet splits safely outside in_navy iteration!
	for(auto const& req : fleet_splits) {
		if(!state.world.navy_is_valid(req.navy) || req.warships_to_split.empty())
			continue;
		if(state.world.navy_get_controller_from_navy_control(req.navy) != req.owner)
			continue;
		std::vector<dcon::ship_id> valid_ships;
		valid_ships.reserve(req.warships_to_split.size());
		for(auto sid : req.warships_to_split) {
			if(state.world.ship_is_valid(sid) && state.world.ship_get_navy_from_navy_membership(sid) == req.navy) {
				valid_ships.push_back(sid);
			}
		}
		auto cur_mem = state.world.navy_get_navy_membership(req.navy);
		int32_t cur_cnt = int32_t(cur_mem.end() - cur_mem.begin());
		if(!valid_ships.empty() && int32_t(valid_ships.size()) < cur_cnt
			&& military::can_split_navy<command::actor::ai>(state, req.owner, req.navy, valid_ships)) {
			military::split_navy<command::actor::ai>(state, req.owner, req.navy, valid_ships, 0);
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
		if(!uid)
			continue;
		auto const& bdef = state.military_definitions.unit_base_definitions[uid];
		auto stats = n ? state.world.nation_get_unit_stats(n, uid) : sys::unit_variable_stats{};
		float sup = (n && stats.discipline_or_evasion > 0.0f) ? stats.support : bdef.support;
		if(sup > 0.0f)
			return true;
	}
	return false;
}

inline bool army_has_offensive_utility(sys::state const& state, dcon::nation_id n, dcon::army_id a) {
	for(auto r : state.world.army_get_army_membership(a)) {
		auto uid = r.get_regiment().get_type();
		if(!uid)
			continue;
		auto const& bdef = state.military_definitions.unit_base_definitions[uid];
		auto stats = n ? state.world.nation_get_unit_stats(n, uid) : sys::unit_variable_stats{};
		bool use_stats = (n && stats.discipline_or_evasion > 0.0f);
		float rec = use_stats ? stats.reconnaissance_or_fire_range : bdef.reconnaissance_or_fire_range;
		float sg = use_stats ? stats.siege_or_torpedo_attack : bdef.siege_or_torpedo_attack;
		if(rec > 0.0f || sg > 0.0f)
			return true;
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

// Verifies that a land path respects Project Alice's 1-province front logistics rule (logistics.cpp:800-832):
// - Every intermediate province (path[1 .. size-1]) MUST be safe friendly/allied controlled land!
// - If allow_hostile_final_step == false, path[0] (destination) must ALSO be safe friendly/allied controlled land.
// Note: province::make_*_path stores provinces in REVERSE order (path[0] = final destination, path.back() = next step from start).
inline bool path_respects_front_supply(sys::state& state, dcon::nation_id n, std::vector<dcon::province_id> const& path, bool allow_hostile_final_step) {
	if(path.empty())
		return false;
	size_t const start_idx = allow_hostile_final_step ? 1 : 0;
	for(size_t i = start_idx; i < path.size(); ++i) {
		auto p = path[i];
		if(!p || province::is_sea(state, p) || !province::has_safe_access_to_province(state, n, p)) {
			return false;
		}
	}
	return true;
}

// Finds a land path from start -> end that NEVER marches through enemy- or rebel-occupied territory!
// - allow_hostile_final_step == false (guards / staging approach): stays 100% on friendly/allied controlled land
//   (allows friendly tiles with siege_progress > 0 via fallback check, but ZERO enemy/rebel-controlled tiles).
// - allow_hostile_final_step == true (frontline attacks / battle reinforcements):
//   if directly adjacent via a passable border, crosses straight in 1 hop; otherwise approaches strictly over
//   safe friendly/allied land and takes at most 1 final step into `end`.
inline std::vector<dcon::province_id> make_front_safe_land_path(sys::state& state, dcon::province_id start, dcon::province_id end, dcon::nation_id n, dcon::army_id army, bool allow_hostile_final_step) {
	if(!start || !end || start == end || province::is_sea(state, end))
		return {};

	if(!allow_hostile_final_step) {
		auto spath = province::make_safe_land_path(state, start, end, n);
		if(!spath.empty())
			return spath;
		auto lpath = province::make_land_unit_path(state, start, end, n, army);
		if(path_respects_front_supply(state, n, lpath, false))
			return lpath;
		return {};
	}

	// Direct 1-hop border step (never detour through hostile rear when directly adjacent!)
	if(!province::is_sea(state, start)) {
		if(auto adj = state.world.get_province_adjacency_by_province_pair(start, end); adj && !province::is_adjacency_impassable(state, n, adj)) {
			if(province::has_access_to_province(state, n, end)) {
				return std::vector<dcon::province_id>{ end };
			}
		}
	}

	auto lpath = province::make_land_unit_path(state, start, end, n, army);
	if(path_respects_front_supply(state, n, lpath, true)) {
		return lpath;
	}

	// If make_land_unit_path tried to cut through an enemy-occupied tile, route cleanly over safe friendly land
	// to the best adjacent supplied/safe staging neighbor of `end`, then take 1 final step into `end`!
	if(!province::has_access_to_province(state, n, end))
		return {};

	struct safe_nb_cand {
		dcon::province_id p;
		bool has_tp;
		float km;
	};
	std::vector<safe_nb_cand> safe_nbs;
	for(auto padj : state.world.province_get_province_adjacency(end)) {
		if(province::is_adjacency_impassable(state, n, padj.id))
			continue;
		dcon::province_id nb = padj.get_connected_provinces(0) == end ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
		if(!nb || nb == start || province::is_sea(state, nb) || !province::has_safe_access_to_province(state, n, nb))
			continue;
		bool has_tp = (logistics::calculate_supply_throughput_in_province(state, nb, n) > 0.0f);
		float km = province::direct_distance_km(state, start, nb);
		safe_nbs.push_back(safe_nb_cand{ nb, has_tp, km });
	}
	std::sort(safe_nbs.begin(), safe_nbs.end(), [](safe_nb_cand const& a, safe_nb_cand const& b) {
		if(a.has_tp != b.has_tp)
			return a.has_tp > b.has_tp;
		if(a.km != b.km)
			return a.km < b.km;
		return a.p.index() < b.p.index();
	});
	for(auto const& cand_nb : safe_nbs) {
		auto approach = province::make_safe_land_path(state, start, cand_nb.p, n);
		if(approach.empty()) {
			auto alt = province::make_land_unit_path(state, start, cand_nb.p, n, army);
			if(path_respects_front_supply(state, n, alt, false)) {
				approach = std::move(alt);
			}
		}
		if(!approach.empty() && std::find(approach.begin(), approach.end(), end) == approach.end()) {
			approach.insert(approach.begin(), end);
			return approach;
		}
	}
	return {};
}

struct walkable_theater_map {
	int32_t land_count = 0;
	std::vector<uint16_t> theater_id;      // Passable across own + allies + war enemies (bridges national borders for attacks/targets)
	std::vector<uint16_t> safe_theater_id; // Strictly SAFE friendly land (own + allies + mil-access, NEVER crossing war-enemy land!)
	std::vector<uint8_t> has_safe_port;
	std::vector<std::vector<dcon::province_id>> safe_ports;
	std::vector<uint8_t> safe_has_port;
	std::vector<std::vector<dcon::province_id>> safe_theater_ports;

	void build(sys::state& state, dcon::nation_id n, dcon::province_id extra_seed = dcon::province_id{}) {
		uint32_t const pcount = state.world.province_size();
		uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
		land_count = int32_t(std::min(pcount, first_sea));
		theater_id.assign(size_t(std::max(0, land_count)), 0);
		safe_theater_id.assign(size_t(std::max(0, land_count)), 0);
		has_safe_port.assign(1, 0);
		safe_ports.assign(1, {});
		safe_has_port.assign(1, 0);
		safe_theater_ports.assign(1, {});
		if(land_count <= 0 || !n)
			return;

		// Deterministic seed list: our armies, capital, controlled provinces, war-ally controlled provinces (so UK/USA see France in WWI & vassal dominions!), and optional seed
		std::vector<dcon::province_id> seeds;
		for(auto ar : state.world.nation_get_army_control(n)) {
			auto loc = ar.get_army().get_location_from_army_location().id;
			if(loc && loc.index() < land_count) {
				seeds.push_back(loc);
			}
		}
		if(dcon::province_id cap = state.world.nation_get_capital(n).id; cap && cap.index() < land_count) {
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
		if(state.world.nation_get_is_at_war(n)) {
			for(auto wp : state.world.nation_get_war_participant(n)) {
				auto war = wp.get_war();
				bool we_att = wp.get_is_attacker();
				for(auto other : war.get_war_participant()) {
					if(other.get_is_attacker() == we_att && other.get_nation() != n) {
						for(auto pc : other.get_nation().get_province_control()) {
							auto p = pc.get_province().id;
							if(p && p.index() < land_count) {
								seeds.push_back(p);
							}
						}
					}
				}
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
			safe_ports.emplace_back();

			theater_id[seed.index()] = next_tid;
			queue.clear();
			queue.push_back(seed);

			for(size_t qi = 0; qi < queue.size(); ++qi) {
				auto u = queue[qi];
				if(state.world.province_get_is_coast(u)
					&& bool(state.world.province_get_port_to(u))
					&& province::has_naval_access_to_province(state, n, u)
					&& !province_has_enemy_army(state, n, u)) {
					has_safe_port[next_tid] = 1;
					safe_ports[next_tid].push_back(u);
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

		// Pass 2: Build SAFE connected theater IDs strictly over safe friendly land (own + allied + mil-access, NEVER crossing hostile Germany between France & Russia!)
		uint16_t next_stid = 0;
		for(auto seed : seeds) {
			if(safe_theater_id[seed.index()] != 0)
				continue;
			if(!province::has_safe_access_to_province(state, n, seed))
				continue;
			if(next_stid == uint16_t(0xFFFF))
				break;
			++next_stid;
			safe_has_port.push_back(0);
			safe_theater_ports.emplace_back();

			safe_theater_id[seed.index()] = next_stid;
			queue.clear();
			queue.push_back(seed);

			for(size_t qi = 0; qi < queue.size(); ++qi) {
				auto u = queue[qi];
				if(state.world.province_get_is_coast(u)
					&& bool(state.world.province_get_port_to(u))
					&& province::has_naval_access_to_province(state, n, u)
					&& !province_has_enemy_army(state, n, u)) {
					safe_has_port[next_stid] = 1;
					safe_theater_ports[next_stid].push_back(u);
				}
				for(auto padj : state.world.province_get_province_adjacency(u)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id v = padj.get_connected_provinces(0) == u ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(!v || v.index() >= land_count || safe_theater_id[v.index()] != 0)
						continue;
					if(!state.world.province_get_nation_from_province_ownership(v))
						continue;
					if(!province::has_safe_access_to_province(state, n, v))
						continue;
					safe_theater_id[v.index()] = next_stid;
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

	uint16_t safe_theater_of(dcon::province_id p) const {
		if(!p || p.index() >= land_count)
			return 0;
		return safe_theater_id[p.index()];
	}

	bool theater_has_safe_port(uint16_t tid) const {
		return tid > 0 && size_t(tid) < has_safe_port.size() && has_safe_port[tid] != 0;
	}

	bool safe_theater_has_port(uint16_t stid) const {
		return stid > 0 && size_t(stid) < safe_has_port.size() && safe_has_port[stid] != 0;
	}

	dcon::province_id pick_port_from_list(sys::state& state, dcon::nation_id n, std::vector<dcon::province_id> const& plist, dcon::province_id ref_prov, dcon::province_id naval_start, float max_ref_km) const {
		if(plist.empty())
			return dcon::province_id{};

		struct port_cand {
			dcon::province_id p;
			float cost;
		};
		std::vector<port_cand> cands;
		cands.reserve(plist.size());
		for(auto p : plist) {
			float ref_km = ref_prov ? province::direct_distance_km(state, p, ref_prov) : 0.0f;
			if(ref_km > max_ref_km)
				continue;
			float sea_km = naval_start ? province::direct_distance_km(state, naval_start, p) : 0.0f;
			float nb_lvl = float(state.world.province_get_building_level(p, uint8_t(economy::province_building_type::naval_base)));
			float sl = float(std::min(60, military::supply_limit_in_province(state, n, p)));
			float tp = logistics::calculate_supply_throughput_in_province(state, p, n);
			float port_cap = logistics::port_supply_capacity_in_province(state, p, n);
			float cost = ref_km + sea_km * 0.25f - nb_lvl * 140.0f - sl * 6.0f
				+ (tp <= 0.0f ? 2500.0f : 0.0f)
				+ (port_cap <= 0.0f ? 4000.0f : 0.0f);
			if(p == ref_prov) {
				cost -= 2000.0f;
			}
			cands.push_back(port_cand{ p, cost });
		}
		if(cands.empty())
			return dcon::province_id{};

		std::sort(cands.begin(), cands.end(), [](port_cand const& a, port_cand const& b) {
			if(a.cost != b.cost)
				return a.cost < b.cost;
			return a.p.index() < b.p.index();
		});

		if(!naval_start) {
			return cands.front().p;
		}
		// Deduplicate sea reachability checks by offshore sea tile (up to 16 distinct seas) so 8 Black Sea/Caspian ports can NEVER hide reachable Atlantic/Baltic ports!
		std::vector<dcon::province_id> tested_seas;
		tested_seas.reserve(16);
		for(size_t ci = 0; ci < cands.size() && tested_seas.size() < 16; ++ci) {
			auto p = cands[ci].p;
			if(p == naval_start)
				return p;
			auto sea_tile = state.world.province_get_port_to(p).id;
			if(sea_tile && std::find(tested_seas.begin(), tested_seas.end(), sea_tile) != tested_seas.end())
				continue;
			if(sea_tile) {
				tested_seas.push_back(sea_tile);
			}
			if(!province::make_naval_unit_path(state, naval_start, p, n).empty()) {
				return p;
			}
		}
		return dcon::province_id{};
	}

	// Pick the best sea-reachable friendly dockable port in walkable theater `tid` near `ref_prov`
	dcon::province_id best_reachable_safe_port(sys::state& state, dcon::nation_id n, uint16_t tid, dcon::province_id ref_prov, dcon::province_id naval_start = dcon::province_id{}, float max_ref_km = 1e9f) const {
		if(tid == 0 || size_t(tid) >= safe_ports.size())
			return dcon::province_id{};
		return pick_port_from_list(state, n, safe_ports[tid], ref_prov, naval_start, max_ref_km);
	}

	// Pick the best sea-reachable friendly dockable port strictly within SAFE friendly theater `stid` (never across enemy territory!)
	dcon::province_id best_reachable_safe_port_in_safe_theater(sys::state& state, dcon::nation_id n, uint16_t stid, dcon::province_id ref_prov, dcon::province_id naval_start = dcon::province_id{}, float max_ref_km = 1e9f) const {
		if(stid == 0 || size_t(stid) >= safe_theater_ports.size())
			return dcon::province_id{};
		return pick_port_from_list(state, n, safe_theater_ports[stid], ref_prov, naval_start, max_ref_km);
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
		dcon::province_id def_cap = state.world.nation_get_capital(def_nat).id;
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

struct front_sector_summary {
	uint16_t sector_id = 0;
	uint16_t theater_id = 0;
	uint16_t safe_theater_id = 0;
	int32_t front_prov_count = 0;
	int32_t front_cw_sum = 0;
	int32_t own_front_provs = 0;
	int32_t own_front_cap_sum = 0;
	int32_t coalition_front_provs = 0;
	int32_t coalition_front_cap_sum = 0;
	int32_t our_regs = 0;
	int32_t our_stacks = 0;
	int32_t coalition_regs = 0;
	int32_t coalition_stacks = 0;
	float our_eff_regs = 0.0f;
	float enemy_eff_regs = 0.0f;
	bool continuous_front = false;
	war_stance stance = war_stance::defensive;
	std::vector<dcon::province_id> front_provs;
};

// Splits each walkable continent (theater_id) & safe landmass (safe_theater_id) into SEPARATE connected active frontlines
// (e.g. Canada vs Mexico for USA, Galicia vs Italian Alps for Austria, France Western Front vs Russia Eastern Front in WWI!)
// and recomputes each frontline's current width, war mode (continuous vs maneuver), and local stance dynamically.
struct frontline_sector_map {
	int32_t land_count = 0;
	std::vector<uint16_t> prov_to_sector; // 0 = not on active 1st line; 1..K = connected frontline sector ID
	std::vector<front_sector_summary> sectors; // 1-indexed; sectors[0] is default fallback

	uint16_t sector_for_province(sys::state const& state, dcon::province_id p, uint16_t tid, uint16_t safe_tid = 0) const {
		if(!p || tid == 0 || sectors.size() <= 1)
			return 0;
		auto matches_sec = [&](uint16_t sid, bool strict_safe) -> bool {
			if(sid == 0 || size_t(sid) >= sectors.size())
				return false;
			if(sectors[sid].theater_id != tid)
				return false;
			if(strict_safe && safe_tid != 0 && sectors[sid].safe_theater_id != 0 && sectors[sid].safe_theater_id != safe_tid)
				return false;
			return true;
		};

		int32_t pidx = p.index();
		if(pidx >= 0 && pidx < land_count) {
			uint16_t direct = prov_to_sector[size_t(pidx)];
			if(matches_sec(direct, true)) {
				return direct;
			}
		}
		for(int pass = 0; pass < 2; ++pass) {
			bool strict_safe = (pass == 0 && safe_tid != 0);
			if(pass == 1 && safe_tid == 0)
				break;
			uint16_t adj_best = 0;
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				int32_t oidx = other ? other.index() : -1;
				if(oidx >= 0 && oidx < land_count) {
					uint16_t osid = prov_to_sector[size_t(oidx)];
					if(matches_sec(osid, strict_safe)) {
						if(adj_best == 0 || osid < adj_best) {
							adj_best = osid;
						}
					}
				}
			}
			if(adj_best > 0)
				return adj_best;

			uint16_t best_sid = 0;
			float best_km = 1e9f;
			for(size_t si = 1; si < sectors.size(); ++si) {
				auto const& sec = sectors[si];
				if(!matches_sec(uint16_t(si), strict_safe) || sec.front_provs.empty())
					continue;
				for(auto fp : sec.front_provs) {
					float d = province::direct_distance_km(state, p, fp);
					if(d < best_km || (d == best_km && (best_sid == 0 || uint16_t(si) < best_sid))) {
						best_km = d;
						best_sid = uint16_t(si);
					}
				}
			}
			if(best_sid > 0)
				return best_sid;
		}
		return 0;
	}

	front_sector_summary const& summary_for_province(sys::state const& state, dcon::province_id p, uint16_t tid, uint16_t safe_tid = 0) const {
		uint16_t sid = sector_for_province(state, p, tid, safe_tid);
		if(sid > 0 && size_t(sid) < sectors.size())
			return sectors[sid];
		return sectors[0];
	}

	void build(
		sys::state& state,
		dcon::nation_id n,
		walkable_theater_map const& tmap,
		std::vector<dcon::nation_id> const& war_allies,
		std::vector<dcon::nation_id> const& war_enemies,
		bool time_against_us
	) {
		land_count = int32_t(std::min(state.world.province_size(), uint32_t(state.province_definitions.first_sea_province.index())));
		prov_to_sector.assign(size_t(std::max(0, land_count)), uint16_t(0));
		sectors.clear();
		sectors.push_back(front_sector_summary{});
		sectors[0].stance = time_against_us ? war_stance::cautious_offense : war_stance::defensive;

		if(land_count <= 0)
			return;

		int32_t min_enemy_cw = nation_base_cw(state, n);
		for(auto e : war_enemies) {
			min_enemy_cw = std::min(min_enemy_cw, nation_base_cw(state, e));
		}
		float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
		size_t const num_theaters = std::max<size_t>(2, tmap.has_safe_port.size());
		size_t const num_safe_theaters = std::max<size_t>(2, tmap.safe_has_port.size());

		// 1. Scan real enemy armies per walkable theater FIRST so empty 0-army side borders or a lone 12K puppet
		// never inflate an active frontline or break continuous front detection.
		struct enemy_pos {
			dcon::province_id loc;
			uint16_t tid;
			float eff;
		};
		std::vector<enemy_pos> enemy_armies;
		std::vector<float> theater_enemy_eff(num_theaters, 0.0f);
		float total_enemy_eff_all = 0.0f;
		for(auto e_nat : war_enemies) {
			for(auto ar : state.world.nation_get_army_control(e_nat)) {
				auto army = ar.get_army();
				if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
					continue;
				auto loc = army.get_location_from_army_location().id;
				if(!loc)
					continue;
				uint16_t etid = tmap.theater_of(loc);
				if(etid == 0 || size_t(etid) >= num_theaters)
					continue;
				float eff = 0.0f;
				for(auto r : army.get_army_membership()) {
					eff += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
				}
				if(eff > 0.05f) {
					theater_enemy_eff[etid] += eff;
					total_enemy_eff_all += eff;
					if(eff >= 0.5f) {
						bool merged = false;
						for(auto& ep : enemy_armies) {
							if(ep.loc == loc) {
								ep.eff += eff;
								merged = true;
								break;
							}
						}
						if(!merged) {
							enemy_armies.push_back(enemy_pos{ loc, etid, eff });
						}
					}
				}
			}
		}

		auto has_nearby_real_enemy = [&](dcon::province_id p, uint16_t ptid) -> bool {
			if(ptid == 0 || size_t(ptid) >= num_theaters || theater_enemy_eff[ptid] < 1.0f)
				return true; // 0-army theater: fall back to current control border
			float const min_sector_eff = std::max(0.5f, std::min(6.0f, theater_enemy_eff[ptid] * 0.12f));
			float near_eff = 0.0f;
			for(auto const& ep : enemy_armies) {
				if(ep.tid == ptid && province::direct_distance_km(state, p, ep.loc) <= 240.0f) {
					near_eff += ep.eff;
					if(near_eff >= min_sector_eff)
						return true;
				}
			}
			return false;
		};

		// 2. Collect all active 1st-line provinces across war_allies' current control
		std::vector<uint8_t> front_flag(size_t(land_count), uint8_t(0)); // 1 = coalition, 2 = own
		std::vector<dcon::province_id> all_front_provs;
		for(auto a_nat : war_allies) {
			for(auto pc : state.world.nation_get_province_control(a_nat)) {
				auto p = pc.get_province().id;
				int32_t pidx = p ? p.index() : -1;
				if(pidx < 0 || pidx >= land_count)
					continue;
				uint16_t ptid = tmap.theater_of(p);
				if(ptid == 0 || size_t(ptid) >= num_theaters)
					continue;
				if(front_flag[size_t(pidx)] != 0) {
					if(a_nat == n) {
						front_flag[size_t(pidx)] = 2;
					}
					continue;
				}
				bool is_front = province_has_enemy_army(state, n, p);
				if(!is_front) {
					for(auto padj : state.world.province_get_province_adjacency(p)) {
						if(province::is_adjacency_impassable(state, n, padj.id))
							continue;
						dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
						if(province::is_sea(state, other))
							continue;
						if(province_has_enemy_army(state, n, other)) {
							is_front = true;
							break;
						}
						auto octrl = state.world.province_get_nation_from_province_control(other);
						if(octrl && military::are_at_war(state, n, octrl) && has_nearby_real_enemy(p, ptid)) {
							is_front = true;
							break;
						}
					}
				}
				if(is_front) {
					front_flag[size_t(pidx)] = (a_nat == n) ? uint8_t(2) : uint8_t(1);
					all_front_provs.push_back(p);
				}
			}
		}

		if(all_front_provs.empty()) {
			// Overseas beachhead with 0 controlled tiles yet: compute fallback stance from actual force ratio!
			float total_our_eff_all = 0.0f;
			for(auto a_nat : war_allies) {
				for(auto ar : state.world.nation_get_army_control(a_nat)) {
					auto army = ar.get_army();
					if(army.get_black_flag() || army.get_navy_from_army_transport())
						continue;
					for(auto r : army.get_army_membership()) {
						total_our_eff_all += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
					}
				}
			}
			float ratio0 = total_our_eff_all / std::max(1.0f, total_enemy_eff_all);
			if(ratio0 < 0.85f) {
				sectors[0].stance = war_stance::defensive;
			} else if(!time_against_us && ratio0 < 1.20f) {
				sectors[0].stance = war_stance::defensive;
			} else if(time_against_us && ratio0 < 1.20f) {
				sectors[0].stance = war_stance::cautious_offense;
			} else {
				sectors[0].stance = war_stance::active_offense;
			}
			return;
		}

		std::sort(all_front_provs.begin(), all_front_provs.end(), [](dcon::province_id a, dcon::province_id b) {
			return a.index() < b.index();
		});

		// 3. Group active frontline tiles into separate connected frontline sectors per (theater_id, safe_theater_id)
		// (links directly adjacent front tiles OR front tiles bridged by 1 shared passable friendly/accessible neighbor — NEVER crossing neutral buffers!)
		std::vector<dcon::province_id> bfs_q;
		for(auto start_p : all_front_provs) {
			int32_t sidx = start_p.index();
			if(prov_to_sector[size_t(sidx)] != 0)
				continue;
			uint16_t stid = tmap.theater_of(start_p);
			uint16_t s_safe_tid = tmap.safe_theater_of(start_p);
			uint16_t sid = uint16_t(sectors.size());
			sectors.push_back(front_sector_summary{});
			sectors.back().sector_id = sid;
			sectors.back().theater_id = stid;
			sectors.back().safe_theater_id = s_safe_tid;

			prov_to_sector[size_t(sidx)] = sid;
			bfs_q.clear();
			bfs_q.push_back(start_p);
			for(size_t qi = 0; qi < bfs_q.size(); ++qi) {
				auto u = bfs_q[qi];
				auto& sec = sectors[sid];
				sec.front_provs.push_back(u);
				int32_t sl_regs = std::max(4, int32_t(float(military::supply_limit_in_province(state, n, u)) / reg_weight));
				int32_t line_cap = std::max(4, std::min(prov_cw(state, n, u, min_enemy_cw), sl_regs));
				++sec.coalition_front_provs;
				sec.coalition_front_cap_sum += line_cap;
				if(front_flag[size_t(u.index())] == 2) {
					++sec.own_front_provs;
					sec.own_front_cap_sum += line_cap;
				}
				for(auto padj : state.world.province_get_province_adjacency(u)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id v = padj.get_connected_provinces(0) == u ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					int32_t vidx = v ? v.index() : -1;
					if(vidx < 0 || vidx >= land_count || province::is_sea(state, v))
						continue;
					if(front_flag[size_t(vidx)] != 0 && prov_to_sector[size_t(vidx)] == 0
						&& tmap.theater_of(v) == stid && tmap.safe_theater_of(v) == s_safe_tid) {
						prov_to_sector[size_t(vidx)] = sid;
						bfs_q.push_back(v);
					} else if(tmap.theater_of(v) == stid && (s_safe_tid == 0 || tmap.safe_theater_of(v) == s_safe_tid)) {
						// Bridge 1-tile jagged indentation ONLY through passable accessible land in the same safe theater (NEVER jump across neutral Switzerland/Belgium!)
						for(auto padj2 : state.world.province_get_province_adjacency(v)) {
							if(province::is_adjacency_impassable(state, n, padj2.id))
								continue;
							dcon::province_id w = padj2.get_connected_provinces(0) == v ? padj2.get_connected_provinces(1).id : padj2.get_connected_provinces(0).id;
							int32_t widx = w ? w.index() : -1;
							if(widx >= 0 && widx < land_count && front_flag[size_t(widx)] != 0 && prov_to_sector[size_t(widx)] == 0
								&& tmap.theater_of(w) == stid && tmap.safe_theater_of(w) == s_safe_tid) {
								prov_to_sector[size_t(widx)] = sid;
								bfs_q.push_back(w);
							}
						}
					}
				}
			}
			auto& sec = sectors[sid];
			sec.front_prov_count = (sec.own_front_provs > 0) ? sec.own_front_provs : sec.coalition_front_provs;
			sec.front_cw_sum = (sec.own_front_provs > 0) ? sec.own_front_cap_sum : sec.coalition_front_cap_sum;
		}

		// 4. Assign enemy & friendly armies to their nearest frontline sector in the same safe/walkable theater
		std::vector<int32_t> safe_our_regs(num_safe_theaters, 0);
		std::vector<int32_t> safe_our_stacks(num_safe_theaters, 0);
		std::vector<int32_t> safe_coal_regs(num_safe_theaters, 0);
		std::vector<int32_t> safe_coal_stacks(num_safe_theaters, 0);
		std::vector<float> safe_sector_enemy_sum(num_safe_theaters, 0.0f);
		std::vector<int32_t> safe_sector_cap_sum(num_safe_theaters, 0);

		for(size_t si = 1; si < sectors.size(); ++si) {
			uint16_t sft = sectors[si].safe_theater_id;
			if(sft > 0 && size_t(sft) < num_safe_theaters) {
				safe_sector_cap_sum[sft] += sectors[si].front_cw_sum;
			}
		}

		for(auto const& ep : enemy_armies) {
			uint16_t sid = sector_for_province(state, ep.loc, ep.tid, 0);
			if(sid > 0 && size_t(sid) < sectors.size()) {
				sectors[sid].enemy_eff_regs += ep.eff;
				uint16_t sft = sectors[sid].safe_theater_id;
				if(sft > 0 && size_t(sft) < num_safe_theaters) {
					safe_sector_enemy_sum[sft] += ep.eff;
				}
			}
		}

		for(auto a_nat : war_allies) {
			for(auto ar : state.world.nation_get_army_control(a_nat)) {
				auto army = ar.get_army();
				if(army.get_black_flag() || army.get_navy_from_army_transport())
					continue;
				auto loc = army.get_location_from_army_location().id;
				if(!loc)
					continue;
				uint16_t atid = tmap.theater_of(loc);
				uint16_t asft = tmap.safe_theater_of(loc);
				if(atid == 0 || size_t(atid) >= num_theaters)
					continue;
				int32_t rcount = 0;
				float eff = 0.0f;
				for(auto r : army.get_army_membership()) {
					++rcount;
					eff += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
				}
				if(rcount <= 0)
					continue;
				if(asft > 0 && size_t(asft) < num_safe_theaters) {
					if(a_nat == n) {
						safe_our_regs[asft] += rcount;
						safe_our_stacks[asft] += 1;
					}
					safe_coal_regs[asft] += rcount;
					safe_coal_stacks[asft] += 1;
				}

				// OOS / thread-safety: NEVER read another nation's mutable ai_province — only read own ai_province!
				dcon::province_id ref_p = loc;
				if(a_nat == n) {
					auto ai_p = army.get_ai_province().id;
					if(ai_p && tmap.theater_of(ai_p) == atid && (asft == 0 || tmap.safe_theater_of(ai_p) == asft)) {
						ref_p = ai_p;
					}
				}
				uint16_t sid = sector_for_province(state, ref_p, atid, asft);
				if(sid > 0 && size_t(sid) < sectors.size()) {
					auto& sec = sectors[sid];
					if(a_nat == n) {
						sec.our_regs += rcount;
						sec.our_stacks += 1;
					}
					sec.coalition_regs += rcount;
					sec.coalition_stacks += 1;
					sec.our_eff_regs += eff;
				}
			}
		}

		// 5. Evaluate continuous_front & local war_stance independently for EACH active frontline sector!
		for(size_t si = 1; si < sectors.size(); ++si) {
			auto& sec = sectors[si];
			uint16_t sft = sec.safe_theater_id;
			float share = 0.0f;
			if(sft > 0 && size_t(sft) < num_safe_theaters) {
				share = (safe_sector_enemy_sum[sft] > 0.1f)
					? (sec.enemy_eff_regs / safe_sector_enemy_sum[sft])
					: (float(sec.front_cw_sum) / std::max(1.0f, float(safe_sector_cap_sum[sft])));
			}
			share = std::clamp(share, 0.0f, 1.0f);

			int32_t s_our_r = (sft > 0 && size_t(sft) < num_safe_theaters) ? safe_our_regs[sft] : sec.our_regs;
			int32_t s_our_s = (sft > 0 && size_t(sft) < num_safe_theaters) ? safe_our_stacks[sft] : sec.our_stacks;
			int32_t s_coal_r = (sft > 0 && size_t(sft) < num_safe_theaters) ? safe_coal_regs[sft] : sec.coalition_regs;
			int32_t s_coal_s = (sft > 0 && size_t(sft) < num_safe_theaters) ? safe_coal_stacks[sft] : sec.coalition_stacks;

			int32_t eff_own_regs = std::max(sec.our_regs, int32_t(std::round(float(s_our_r) * share)));
			int32_t eff_own_stacks = std::max(sec.our_stacks, int32_t(std::round(float(s_our_s) * share)));
			int32_t eff_coal_regs = std::max(sec.coalition_regs, int32_t(std::round(float(s_coal_r) * share)));
			int32_t eff_coal_stacks = std::max(sec.coalition_stacks, int32_t(std::round(float(s_coal_s) * share)));

			bool own_continuous = (sec.own_front_provs > 0)
				&& (float(eff_own_regs) >= float(sec.own_front_cap_sum) * 0.65f)
				&& (eff_own_stacks >= std::max(2, int32_t(std::ceil(float(sec.own_front_provs) * 0.70f))));
			bool coalition_continuous = (sec.coalition_front_provs > 0)
				&& (float(eff_coal_regs) >= float(sec.coalition_front_cap_sum) * 0.65f)
				&& (eff_coal_stacks >= std::max(2, int32_t(std::ceil(float(sec.coalition_front_provs) * 0.70f))));
			sec.continuous_front = own_continuous || coalition_continuous;

			float ratio = sec.our_eff_regs / std::max(1.0f, sec.enemy_eff_regs);
			if(ratio < 0.85f) {
				sec.stance = war_stance::defensive;
			} else if(!time_against_us && ratio < 1.20f) {
				sec.stance = war_stance::defensive;
			} else if(time_against_us && ratio < 1.20f) {
				sec.stance = war_stance::cautious_offense;
			} else {
				sec.stance = war_stance::active_offense;
			}
		}
	}
};

struct front_pin_tracker {
	struct entry {
		dcon::province_id p;
		int32_t rem_regs = 0;
		int32_t rem_stacks = 0;
		int32_t rem_cores = 0;
	};
	std::vector<entry> entries;

	void init_for_nation(sys::state const& state, dcon::nation_id n) {
		entries.clear();
		for(auto ar : state.world.nation_get_army_control(n)) {
			auto army = ar.get_army();
			if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
				continue;
			// Departing, attacking, merging, or embarking stacks must NOT count as stationary holding garrisons!
			if(army.get_arrival_time())
				continue;
			auto act = army_activity(army.get_ai_activity());
			if(act == army_activity::attacking
				|| act == army_activity::attack_gathered
				|| act == army_activity::merging
				|| act == army_activity::transport_guard
				|| act == army_activity::transport_attack
				|| act == army_activity::attack_transport) {
				continue;
			}
			auto loc = army.get_location_from_army_location().id;
			if(!loc || province::is_sea(state, loc))
				continue;
			auto regs = army.get_army_membership();
			int32_t rcount = 0;
			float sum_org = 0.0f;
			float sum_str = 0.0f;
			for(auto r : regs) {
				sum_org += r.get_regiment().get_org();
				sum_str += r.get_regiment().get_strength();
				++rcount;
			}
			if(rcount <= 0 || (sum_org / float(rcount)) < 0.30f || (sum_str / float(rcount)) < 0.20f)
				continue; // Broken/hollow stacks cannot hold a line!
			bool is_core = army_has_backline_core(state, n, army.id);
			bool found = false;
			for(auto& e : entries) {
				if(e.p == loc) {
					e.rem_regs += rcount;
					e.rem_stacks += 1;
					if(is_core) e.rem_cores += 1;
					found = true;
					break;
				}
			}
			if(!found) {
				entries.push_back(entry{ loc, rcount, 1, is_core ? 1 : 0 });
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
		bool allow_screen_breakthrough = false,
		int32_t extra_pulled_cores = 0
	) const {
		if(!continuous_front)
			return true; // Austerlitz maneuver war: never pin frontline!
		float enemy_elsewhere = adjacent_enemy_regs_excluding(state, n, loc, excluded_target_prov);
		if(enemy_elsewhere <= 0.1f)
			return true; // 2nd-line reserve or no other enemy facing loc
		// Never let a tiny <8k screening detachment freeze a full corps from advancing directly into an adjacent wargoal/empty tile!
		if(allow_screen_breakthrough && enemy_elsewhere <= 2.5f && army_regs >= 6)
			return true;
		auto const* e = find_entry(loc);
		if(!e)
			return false;
		// WWI continuous front: never strip the LAST dug-in artillery core off a threatened tile (allow pulling a surplus 2nd core if one stays!)
		if(army_is_core && army_dig_in > 0 && (e->rem_cores - extra_pulled_cores <= 1))
			return false;
		int32_t rem_stacks_after = e->rem_stacks - extra_pulled_stacks - 1;
		int32_t rem_regs_after = e->rem_regs - extra_pulled_regs - army_regs;
		if(rem_stacks_after < 1)
			return false;
		int32_t pin_regs = std::max(4, std::min(prov_cw(state, n, loc), int32_t(std::ceil(enemy_elsewhere * 0.75f))));
		return rem_regs_after >= pin_regs;
	}

	void commit_pull(dcon::province_id loc, int32_t army_regs, bool army_is_core = false) {
		for(auto& e : entries) {
			if(e.p == loc) {
				e.rem_regs = std::max(0, e.rem_regs - army_regs);
				e.rem_stacks = std::max(0, e.rem_stacks - 1);
				if(army_is_core) {
					e.rem_cores = std::max(0, e.rem_cores - 1);
				}
				return;
			}
		}
	}
};

} // namespace ai_front_helpers

dcon::province_id find_best_friendly_disembark_port(sys::state& state, dcon::nation_id owner, dcon::province_id dest_prov, dcon::province_id naval_start, float max_ref_km) {
	if(!owner || !dest_prov || province::is_sea(state, dest_prov))
		return dcon::province_id{};
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, owner, dest_prov);
	// If dest_prov is safe friendly land, strictly pick a port in the SAME safe friendly theater (never land in Calais to walk to Warsaw across Germany!)
	uint16_t dstid = tmap.safe_theater_of(dest_prov);
	if(dstid != 0) {
		if(!tmap.safe_theater_has_port(dstid))
			return dcon::province_id{};
		return tmap.best_reachable_safe_port_in_safe_theater(state, owner, dstid, dest_prov, naval_start, max_ref_km);
	}
	uint16_t dtid = tmap.theater_of(dest_prov);
	if(dtid == 0 || !tmap.theater_has_safe_port(dtid))
		return dcon::province_id{};
	return tmap.best_reachable_safe_port(state, owner, dtid, dest_prov, naval_start, max_ref_km);
}

dcon::navy_id find_transport_fleet(sys::state& state, dcon::nation_id controller);
void distribute_guards_peacetime(sys::state& state, dcon::nation_id n);

void distribute_guards_wartime(sys::state& state, dcon::nation_id n) {
	uint32_t const pcount = state.world.province_size();
	uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
	int32_t const land_count = int32_t(std::min(pcount, first_sea));
	if(land_count <= 0)
		return;

	// Fast early-out BEFORE allocating theater maps if this nation has zero eligible AI on_guard armies!
	bool has_any_guard = false;
	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(unit_on_ai_control(state, ar)
			&& army_activity(ar.get_ai_activity()) == army_activity::on_guard
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_is_retreating()
			&& ar.get_army_membership().begin() != ar.get_army_membership().end()) {
			has_any_guard = true;
			break;
		}
	}
	if(!has_any_guard)
		return;

	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	dcon::province_id cap = state.world.nation_get_capital(n);
	float cap_throughput = cap ? std::max(1.0f, logistics::calculate_supply_throughput_in_province(state, cap, n)) : 1.0f;

	// Build passable cross-border theater map + strictly SAFE friendly theater map (never crosses hostile enemy territory!)
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n);

	// Collect all our guard armies (on_guard, AI-controlled, not in battle/transport/retreat, not marching into an active battle)
	// AND count regiments already en route by sea toward each safe theater!
	struct guard_unit {
		dcon::army_id id;
		dcon::province_id cur_loc;
		dcon::province_id cur_target;
		uint16_t theater_id = 0;
		uint16_t safe_theater_id = 0;
		uint16_t sector_id = 0;
		int32_t regs = 0;
		int32_t dig_in = 0;
		bool is_core = false;
		bool is_offensive_core = false;
		bool is_moving = false;
		bool assigned = false;
	};
	std::vector<guard_unit> guards;
	std::vector<int32_t> en_route_regs(std::max<size_t>(2, tmap.safe_has_port.size()), 0);

	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(!unit_on_ai_control(state, ar))
			continue;
		auto regs_range = ar.get_army_membership();
		int32_t rcount = int32_t(regs_range.end() - regs_range.begin());
		if(rcount == 0)
			continue;

		auto act = army_activity(ar.get_ai_activity());
		if(act == army_activity::transport_guard || act == army_activity::transport_attack || act == army_activity::attack_transport) {
			auto dest_p = ar.get_ai_province().id;
			uint16_t dstid = tmap.safe_theater_of(dest_p);
			if(dstid > 0 && size_t(dstid) < en_route_regs.size()) {
				en_route_regs[dstid] += rcount;
			}
			continue;
		}

		auto loc = ar.get_location_from_army_location().id;
		if(!loc || province::is_sea(state, loc))
			continue;

		uint16_t tid = tmap.theater_of(loc);
		uint16_t stid = tmap.safe_theater_of(loc);

		if(act == army_activity::on_guard
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_is_retreating()) {
			// Do NOT hijack an on_guard reinforcement currently marching directly into an active battle!
			bool is_moving = bool(ar.get_arrival_time());
			if(is_moving) {
				auto apath = ar.get_path();
				if(apath.size() > 0) {
					auto mdest = apath.at(0);
					if(mdest && state.world.province_get_land_battle_location(mdest).begin() != state.world.province_get_land_battle_location(mdest).end()) {
						continue;
					}
				}
			}
			bool core = ai_front_helpers::army_has_backline_core(state, n, ar.id);
			bool off_core = core && ai_front_helpers::army_has_offensive_utility(state, n, ar.id);
			guards.push_back(guard_unit{
				ar.id,
				loc,
				ar.get_ai_province().id,
				tid,
				stid,
				uint16_t(0),
				rcount,
				int32_t(ar.get_dig_in()),
				core,
				off_core,
				is_moving,
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
	std::vector<std::pair<dcon::province_id, uint16_t>> real_enemy_army_locs;
	std::vector<float> theater_real_enemy_regs(std::max<size_t>(2, tmap.has_safe_port.size()), 0.0f);

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

		uint16_t etid = tmap.theater_of(loc);
		if(etid > 0 && size_t(etid) < theater_real_enemy_regs.size()) {
			theater_real_enemy_regs[etid] += eff_regs;
		}
		if(enemy_source_weight[loc.index()] == 0.0f) {
			threat_sources.push_back(loc);
			real_enemy_army_locs.push_back({ loc, etid });
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

	auto has_real_enemy_near_border = [&](dcon::province_id bp, uint16_t btid) -> bool {
		if(btid == 0 || size_t(btid) >= theater_real_enemy_regs.size())
			return false;
		float const min_sector_eff = std::max(0.5f, std::min(6.0f, theater_real_enemy_regs[btid] * 0.12f));
		float near_eff = 0.0f;
		for(auto const& [eloc, etid] : real_enemy_army_locs) {
			if(etid == btid && province::direct_distance_km(state, bp, eloc) <= 240.0f) {
				near_eff += enemy_source_weight[eloc.index()];
				if(near_eff >= min_sector_eff)
					return true;
			}
		}
		return false;
	};

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
			uint16_t ptid = tmap.theater_of(p);
			bool theater_has_real_enemy = (ptid > 0 && size_t(ptid) < theater_real_enemy_regs.size() && theater_real_enemy_regs[ptid] >= 1.0f);
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				dcon::province_id other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				if(other && other.index() < land_count && (check_enemy_ctrl(other) || enemy_source_weight[other.index()] > 0.0f)) {
					if(enemy_source_weight[other.index()] > 0.0f) {
						front = true;
						break;
					}
					// NEVER spawn phantom enemy threat along an empty 0-army secondary border (e.g. 14 empty Turkish tiles = 84K ghost Turks!)
					// when real enemy armies exist elsewhere on the continent (e.g. 250K Russians in Galicia)!
					if(!theater_has_real_enemy) {
						front = true;
						enemy_source_weight[other.index()] = 0.35f;
						threat_sources.push_back(other);
						break;
					} else if(has_real_enemy_near_border(p, ptid)) {
						front = true;
						enemy_source_weight[other.index()] = 0.50f;
						threat_sources.push_back(other);
						break;
					}
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

	// Step 3: Evaluate EACH separate connected active frontline sector independently ("Austerlitz" mobile fist vs "WWI" continuous front)
	// while indexing Step 6B overseas reinforcement strictly by SAFE friendly theater (so France vs Russia across hostile Germany NEVER merge!).
	ai_front_helpers::frontline_sector_map fmap;
	fmap.build(state, n, tmap, war_allies, war_enemies, time_against_us);

	struct safe_theater_info {
		int32_t front_prov_count = 0;
		int32_t total_guards = 0;
	};
	std::vector<safe_theater_info> safe_theaters(std::max<size_t>(2, tmap.safe_has_port.size()));

	auto ensure_safe_theater = [&](uint16_t stid) -> safe_theater_info& {
		if(size_t(stid) >= safe_theaters.size()) {
			safe_theaters.resize(size_t(stid) + 1);
		}
		return safe_theaters[stid];
	};

	struct sector_deploy_info {
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
	std::vector<sector_deploy_info> sector_deploy(std::max<size_t>(1, fmap.sectors.size()));
	for(size_t si = 0; si < fmap.sectors.size(); ++si) {
		sector_deploy[si].continuous_front = fmap.sectors[si].continuous_front;
		sector_deploy[si].stance = fmap.sectors[si].stance;
	}

	for(auto& g : guards) {
		if(g.safe_theater_id > 0) {
			++ensure_safe_theater(g.safe_theater_id).total_guards;
		}
		if(g.theater_id > 0) {
			dcon::province_id ref_p = (g.cur_target && tmap.theater_of(g.cur_target) == g.theater_id
				&& (g.safe_theater_id == 0 || tmap.safe_theater_of(g.cur_target) == g.safe_theater_id))
				? g.cur_target : g.cur_loc;
			g.sector_id = fmap.sector_for_province(state, ref_p, g.theater_id, g.safe_theater_id);
			if(size_t(g.sector_id) < sector_deploy.size()) {
				++sector_deploy[g.sector_id].total_guards;
				if(g.is_core) {
					++sector_deploy[g.sector_id].core_guards;
				}
			}
		}
	}

	std::vector<int32_t> prov_cap_cache(size_t(land_count), -1);
	auto prov_reg_cap = [&](dcon::province_id p) -> int32_t {
		int32_t idx = p ? p.index() : -1;
		if(idx < 0 || idx >= land_count)
			return 0;
		if(prov_cap_cache[size_t(idx)] >= 0)
			return prov_cap_cache[size_t(idx)];
		int32_t sl = military::supply_limit_in_province(state, n, p);
		int32_t cap_by_sl = std::max(0, int32_t(float(sl) / reg_weight));
		if(cap_by_sl <= 0) {
			prov_cap_cache[size_t(idx)] = 0;
			return 0;
		}
		float tp = logistics::calculate_supply_throughput_in_province(state, p, n);
		if(tp <= 0.0f) {
			// Note: calculate_supply_throughput_in_province returns 0 whenever ANY hostile army sits in p (including active sieges/battles!).
			// Keep contested 1st-line tiles and 1-deep supplied bridge tiles valid so defenders NEVER abandon besieged border provinces!
			bool contested_or_supplied_1deep = (enemy_source_weight[idx] > 0.0f || incoming_enemy_weight[idx] > 0.0f);
			if(!contested_or_supplied_1deep) {
				for(auto padj : state.world.province_get_province_adjacency(p)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id nb = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(nb && nb.index() < land_count && check_safe(nb) && logistics::calculate_supply_throughput_in_province(state, nb, n) > 0.0f) {
						contested_or_supplied_1deep = true;
						break;
					}
				}
			}
			if(!contested_or_supplied_1deep) {
				prov_cap_cache[size_t(idx)] = 0;
				return 0;
			}
			cap_by_sl = std::min(cap_by_sl, std::max(4, cap_by_sl / 2));
		} else if(tp < cap_throughput * 0.25f) {
			cap_by_sl = std::min(cap_by_sl, std::max(4, cap_by_sl / 2));
		}
		prov_cap_cache[size_t(idx)] = cap_by_sl;
		return cap_by_sl;
	};

	// Count STATIONARY regiments already occupying p that are NOT in our reassignable guards list
	// (skip passing-through armies with arrival_time so transiting allies/attackers never evict dug-in 1st-line defenders!)
	auto count_preoccupied_regs = [&](dcon::province_id p) -> int32_t {
		int32_t used = 0;
		for(auto oa : state.world.province_get_army_location(p)) {
			auto army = oa.get_army();
			if(army.get_black_flag() || army.get_is_retreating() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation() || army.get_arrival_time())
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
		uint16_t stid = tmap.safe_theater_of(p);
		if(tid == 0)
			continue;
		if(stid > 0) {
			++ensure_safe_theater(stid).front_prov_count;
		}
		uint16_t sid = fmap.sector_for_province(state, p, tid, stid);
		auto& sd = sector_deploy[size_t(sid) < sector_deploy.size() ? size_t(sid) : 0];

		// Compute Austerlitz mobile fist anchor per frontline sector: prioritize confronting the MAIN enemy army concentration on THIS front!
		int32_t rcap = prov_reg_cap(p);
		if(rcap > 0) {
			float score = threat[idx] * 3.0f + incoming_enemy_weight[idx] * 2.5f;
			if(p == cap) score *= 1.25f;
			if(is_wargoal_prov[idx]) score *= 1.35f;
			score += ai_front_helpers::province_stockpile_importance(state, p) * 6.0f;
			score *= (0.85f + 0.015f * float(std::min(rcap, 30)));
			if(score > sd.fist_anchor_score || (score == sd.fist_anchor_score && (!sd.fist_anchor || p.index() < sd.fist_anchor.index()))) {
				sd.fist_anchor_score = score;
				sd.fist_anchor = p;
			}
		}
	}

	// Step 4: Build deployment slots (desired regiments, capacity, priority, echelon) for every candidate province
	struct prov_slot {
		dcon::province_id id;
		uint16_t theater_id = 0;
		uint16_t safe_theater_id = 0;
		uint16_t sector_id = 0;
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
		uint16_t stid = tmap.safe_theater_of(p);
		if(tid == 0)
			return -1;
		int32_t rcap = prov_reg_cap(p);
		if(rcap <= 0)
			return -1;
		int32_t pre_used = count_preoccupied_regs(p);
		uint16_t sid = fmap.sector_for_province(state, p, tid, stid);
		int32_t sidx = int32_t(slots.size());
		prov_to_slot[idx] = sidx;
		slots.push_back(prov_slot{ p, tid, stid, sid, line, rcap, 0, pre_used, 0, 0.0f, false });
		if(stid > 0) {
			ensure_safe_theater(stid);
		}
		auto& sd = sector_deploy[size_t(sid) < sector_deploy.size() ? size_t(sid) : 0];
		if(line == 1) {
			++sd.line1_slot_count;
		} else if(line == 2) {
			++sd.line2_slot_count;
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
		int32_t sidx = add_or_get_slot(p1, 1);
		if(sidx < 0)
			continue;
		uint16_t sid = slots[size_t(sidx)].sector_id;
		auto const& sd = sector_deploy[size_t(sid) < sector_deploy.size() ? size_t(sid) : 0];

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

		if(sd.continuous_front) {
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

			// Always stage 2nd-line reserve & offensive artillery strike hubs behind active continuous fronts (only boost wargoal strike hubs when NOT on strategic defense!)
			float excess_threat = std::max(0.0f, threat[idx] * 1.15f - float(want1));
			bool active_wg_push = adj_our_wg && (sd.stance != ai_front_helpers::war_stance::defensive);
			bool want_l2_hub = (excess_threat > 2.0f) || (threat[idx] >= 3.0f) || active_wg_push || (sd.stance != ai_front_helpers::war_stance::defensive);
			if(want_l2_hub) {
				float spill = std::max({ excess_threat, threat[idx] * 0.50f, active_wg_push ? (threat[idx] >= 2.0f ? 12.0f : 4.0f) : 5.0f });
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
							if(active_wg_push) {
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
			if(p1 == sd.fist_anchor) {
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
			} else if(threat[idx] >= 0.75f || incoming_enemy_weight[idx] > 0.0f || p1 == cap || is_wargoal_prov[idx] || (adj_our_wg && sd.stance != ai_front_helpers::war_stance::defensive)) {
				int32_t prop_want = std::min(slots[size_t(sidx)].max_regs, std::max(3, int32_t(std::ceil(std::max(threat[idx], incoming_enemy_weight[idx]) * 1.15f))));
				slots[size_t(sidx)].desired_regs = std::max(slots[size_t(sidx)].desired_regs, prop_want);
				slots[size_t(sidx)].priority = std::max(slots[size_t(sidx)].priority, (threat[idx] + incoming_enemy_weight[idx] * 2.5f + ((adj_our_wg && sd.stance != ai_front_helpers::war_stance::defensive) ? 4.0f : 0.0f)) * strategic_mult);
			}
		}
	}

	// Compute 2nd-line Artillery Strike Corps allocation independently per continuous frontline sector!
	// In DEFENSIVE stance, NEVER strip a core off a threatened Line-1 tile unless we genuinely have surplus_cores > 0!
	for(size_t si = 1; si < sector_deploy.size(); ++si) {
		auto& sd = sector_deploy[si];
		if(sd.continuous_front && sd.line2_slot_count > 0 && sd.total_guards > sd.line1_slot_count && sd.core_guards >= 3) {
			int32_t surplus_cores = std::max(0, sd.core_guards - sd.line1_slot_count);
			int32_t want_l2 = surplus_cores;
			if(sd.stance == ai_front_helpers::war_stance::active_offense) {
				want_l2 = std::max(want_l2, std::max(1, sd.core_guards / 3));
			} else if(sd.stance == ai_front_helpers::war_stance::cautious_offense) {
				want_l2 = std::max(want_l2, std::max(1, sd.core_guards / 4));
			}
			sd.desired_l2_cores = std::min(want_l2, sd.total_guards - sd.line1_slot_count);
			sd.max_l1_cores = std::max(1, sd.core_guards - sd.desired_l2_cores);
		} else {
			sd.desired_l2_cores = 0;
			sd.max_l1_cores = sd.core_guards;
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
		// Pure defensive holding cores lock onto Line 1 before offensive-utility cores (recon/siege do nothing in defense!)
		bool a_def_core = guards[a].is_core && !guards[a].is_offensive_core;
		bool b_def_core = guards[b].is_core && !guards[b].is_offensive_core;
		if(a_def_core != b_def_core)
			return a_def_core > b_def_core;
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
		auto& sd = sector_deploy[size_t(sl.sector_id) < sector_deploy.size() ? size_t(sl.sector_id) : 0];
		if(g.is_core && sd.desired_l2_cores > 0 && sd.l1_cores_assigned >= sd.max_l1_cores) {
			continue; // Leave offensive-utility / lowest-priority core stacks free to stage as 2nd-line Strike Corps!
		}
		if(sl.assigned_regs + g.regs <= sl.max_regs) {
			g.assigned = true;
			sl.assigned_regs += g.regs;
			if(g.is_core) {
				sl.assigned_cores += 1;
				++sd.l1_cores_assigned;
			}
			state.world.army_set_ai_province(g.id, g.cur_loc);
		}
	}

	// Step 6: Assign remaining guards — Pure defensive holding cores claim Line 1 first;
	// offensive-utility strike cores (carrying recon/siege, useless on defense) naturally fill Line 2 Strike Corps behind wargoals;
	// pure frontline stacks seal any remaining empty Line 1 tile + back up Line 2/3!
	std::sort(guards.begin(), guards.end(), [](guard_unit const& a, guard_unit const& b) {
		if(a.is_core != b.is_core) return a.is_core > b.is_core;
		if(a.is_core && a.is_offensive_core != b.is_offensive_core) return a.is_offensive_core < b.is_offensive_core;
		if(a.regs != b.regs) return a.regs > b.regs;
		return a.id.index() < b.id.index();
	});

	for(auto& g : guards) {
		if(g.assigned)
			continue;

		bool const is_bf = state.world.army_get_black_flag(g.id);
		bool assign_as_l2_strike = false;
		if(g.is_core && g.sector_id > 0 && size_t(g.sector_id) < sector_deploy.size()) {
			auto const& gsd = sector_deploy[g.sector_id];
			if(gsd.desired_l2_cores > 0 && gsd.l1_cores_assigned >= gsd.max_l1_cores && gsd.l2_cores_assigned < gsd.desired_l2_cores) {
				assign_as_l2_strike = true;
			}
		}

		int32_t best_slot = -1;
		float best_score = -1e9f;
		float const g_adj_enemy = (g.cur_loc && !province::is_sea(state, g.cur_loc))
			? ai_front_helpers::adjacent_enemy_regs_excluding(state, n, g.cur_loc, dcon::province_id{})
			: 0.0f;

		for(size_t si = 0; si < slots.size(); ++si) {
			auto const& sl = slots[si];
			if(sl.desired_regs <= 0 && sl.priority <= 0.0f)
				continue;
			// Black-flagged armies ONLY clear black_flag upon entering an OWNED province (military.cpp:9120-9126)!
			if(is_bf) {
				if(state.world.province_get_nation_from_province_ownership(sl.id) != n)
					continue;
			} else {
				// Strictly stay within our SAFE friendly connected landmass (NEVER try to march guards across hostile Germany from France to Warsaw!)
				bool same_safe = (g.safe_theater_id != 0)
					? (sl.safe_theater_id == g.safe_theater_id)
					: (sl.theater_id == g.theater_id && g.theater_id != 0);
				if(!same_safe) {
					continue;
				}
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
			// - Designated 2nd-line Strike Corps -> Line 2 hubs in THIS sector closest to our offensive wargoal
			// - Pure infantry stacks -> immediately seal any empty Line 1 tile (2.40x), then reinforce Line 2/3!
			float role_mult = 1.0f;
			if(assign_as_l2_strike && sl.sector_id == g.sector_id) {
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

			// Anti-ping-pong hysteresis (+30%..110% bonus for holding a hot front / current valid destination)
			if(sl.id == g.cur_loc) {
				score *= (g_adj_enemy >= 3.0f ? (g.dig_in > 0 ? 2.10f : 1.55f) : (g.dig_in > 0 ? 1.65f : 1.30f));
			} else if(sl.id == g.cur_target) {
				score *= 1.25f;
			}
			// Never voluntarily march a guard currently facing a real adjacent enemy army across the empire to a colder distant sector!
			if(g_adj_enemy >= 3.0f && dist_km > 250.0f && threat[sl.id.index()] < g_adj_enemy * 0.75f) {
				score *= 0.20f;
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
				auto& csd = sector_deploy[size_t(chosen_sl.sector_id) < sector_deploy.size() ? size_t(chosen_sl.sector_id) : 0];
				if(chosen_sl.line == 1) {
					++csd.l1_cores_assigned;
				} else if(chosen_sl.line == 2) {
					++csd.l2_cores_assigned;
				}
			}
			state.world.army_set_ai_province(g.id, chosen_sl.id);
		}
	}

	// Step 6B: Overseas Friendly-Port Front Reinforcement!
	// Strictly indexed by SAFE friendly theater (so UK can ferry into Calais for France OR St. Petersburg for Russia without mixing them!).
	dcon::navy_id free_tf = naval_advantage(state, n) ? find_transport_fleet(state, n) : dcon::navy_id{};
	int32_t free_tcap_now = free_tf ? military::transport_capacity(state, free_tf) : 0;
	if(free_tcap_now > 0) {
		auto fleet_loc = state.world.navy_get_location_from_navy_location(free_tf);
		uint16_t const cap_stid = cap ? tmap.safe_theater_of(cap) : uint16_t(0);

		std::vector<int32_t> slot_deficit(safe_theaters.size(), 0);
		std::vector<float> theater_prio_sum(safe_theaters.size(), 0.0f);
		std::vector<dcon::province_id> theater_ref_slot(safe_theaters.size());
		std::vector<float> theater_ref_prio(safe_theaters.size(), -1.0f);
		for(auto const& sl : slots) {
			if(sl.safe_theater_id == 0 || size_t(sl.safe_theater_id) >= safe_theaters.size())
				continue;
			int32_t def = std::max(0, sl.desired_regs - sl.assigned_regs);
			if(def > 0) {
				slot_deficit[sl.safe_theater_id] += def;
				theater_prio_sum[sl.safe_theater_id] += sl.priority;
				if(sl.priority > theater_ref_prio[sl.safe_theater_id]) {
					theater_ref_prio[sl.safe_theater_id] = sl.priority;
					theater_ref_slot[sl.safe_theater_id] = sl.id;
				}
			}
		}

		// Pick highest-need sea-reachable overseas active front safe theater with a safe friendly dockable port
		uint16_t best_recip_stid = 0;
		dcon::province_id best_recip_port;
		float best_recip_score = -1.0f;
		int32_t best_recip_need = 0;
		for(size_t stid = 1; stid < safe_theaters.size(); ++stid) {
			if(safe_theaters[stid].front_prov_count <= 0 || !tmap.safe_theater_has_port(uint16_t(stid)))
				continue;
			int32_t enr = (stid < en_route_regs.size()) ? en_route_regs[stid] : 0;
			int32_t need = slot_deficit[stid] - enr;
			if(need <= 0)
				continue;
			dcon::province_id rport = tmap.best_reachable_safe_port_in_safe_theater(state, n, uint16_t(stid), theater_ref_slot[stid], fleet_loc);
			if(!rport)
				continue;
			float rscore = theater_prio_sum[stid] * (1.0f + float(need) * 0.05f);
			if(rscore > best_recip_score || (rscore == best_recip_score && best_recip_stid != 0 && uint16_t(stid) < best_recip_stid)) {
				best_recip_score = rscore;
				best_recip_stid = uint16_t(stid);
				best_recip_port = rport;
				best_recip_need = need;
			}
		}

		if(best_recip_stid != 0 && best_recip_need > 0 && best_recip_port) {
			std::vector<uint8_t> reserved_home(guards.size(), 0);
			std::vector<int32_t> donor_avail_regs(safe_theaters.size(), 0);
			std::vector<dcon::province_id> donor_sample_loc(safe_theaters.size());

			for(size_t gstid = 1; gstid < safe_theaters.size(); ++gstid) {
				if(uint16_t(gstid) == best_recip_stid || safe_theaters[gstid].front_prov_count > 0 || !tmap.safe_theater_has_port(uint16_t(gstid)))
					continue;
				std::vector<size_t> t_unassigned;
				for(size_t gi = 0; gi < guards.size(); ++gi) {
					if(!guards[gi].assigned && !guards[gi].is_moving && !state.world.army_get_black_flag(guards[gi].id) && guards[gi].safe_theater_id == uint16_t(gstid)) {
						t_unassigned.push_back(gi);
					}
				}
				if(t_unassigned.empty())
					continue;

				int32_t total_g = safe_theaters[gstid].total_guards;
				int32_t keep_garrison = (uint16_t(gstid) == cap_stid)
					? (total_g >= 6 ? 2 : (total_g >= 2 ? 1 : 0))
					: (total_g >= 3 ? 1 : 0);

				std::sort(t_unassigned.begin(), t_unassigned.end(), [&](size_t a, size_t b) {
					if(guards[a].is_core != guards[b].is_core) return guards[a].is_core < guards[b].is_core;
					if(guards[a].regs != guards[b].regs) return guards[a].regs < guards[b].regs;
					return guards[a].id.index() < guards[b].id.index();
				});
				int32_t kept = 0;
				for(size_t gi : t_unassigned) {
					if(kept < keep_garrison) {
						reserved_home[gi] = 1;
						++kept;
					} else if(guards[gi].regs <= free_tcap_now) {
						donor_avail_regs[gstid] += guards[gi].regs;
						if(!donor_sample_loc[gstid]) {
							donor_sample_loc[gstid] = guards[gi].cur_loc;
						}
					}
				}
			}

			uint16_t best_donor_stid = 0;
			float best_donor_score = -1.0f;
			for(size_t gstid = 1; gstid < safe_theaters.size(); ++gstid) {
				if(donor_avail_regs[gstid] <= 0)
					continue;
				dcon::province_id dport = tmap.best_reachable_safe_port_in_safe_theater(state, n, uint16_t(gstid), donor_sample_loc[gstid], fleet_loc);
				if(!dport)
					continue;
				int32_t wave_regs = std::min({ donor_avail_regs[gstid], free_tcap_now, best_recip_need });
				float sea_km = province::direct_distance_km(state, fleet_loc, dport) * 0.6f
					+ province::direct_distance_km(state, dport, best_recip_port);
				float dscore = float(wave_regs) / (1.0f + sea_km / 6500.0f);
				if(dscore > best_donor_score || (dscore == best_donor_score && best_donor_stid != 0 && uint16_t(gstid) < best_donor_stid)) {
					best_donor_score = dscore;
					best_donor_stid = uint16_t(gstid);
				}
			}

			if(best_donor_stid != 0) {
				int32_t wave_budget = free_tcap_now;
				int32_t rem_need = best_recip_need;
				bool recip_port_used_as_fallback = false;
				for(size_t gi = 0; gi < guards.size() && wave_budget > 0 && rem_need > 0; ++gi) {
					auto& g = guards[gi];
					if(g.assigned || g.is_moving || reserved_home[gi] || g.safe_theater_id != best_donor_stid || state.world.army_get_black_flag(g.id))
						continue;
					if(g.regs > wave_budget)
						continue;

					int32_t ov_best_slot = -1;
					float ov_best_score = -1e9f;
					for(size_t si = 0; si < slots.size(); ++si) {
						auto const& sl = slots[si];
						if(sl.safe_theater_id != best_recip_stid)
							continue;
						if(sl.assigned_regs >= sl.desired_regs)
							continue;
						if(sl.assigned_regs + g.regs > sl.max_regs)
							continue;
						int32_t deficit = std::max(0, sl.desired_regs - sl.assigned_regs);
						float need_factor = 1.0f + float(std::min(deficit, g.regs)) / float(std::max(1, g.regs));
						float role_mult = 1.0f;
						if(sl.line == 1) {
							role_mult = g.is_core ? (sl.assigned_cores == 0 ? 1.85f : 1.05f) : (sl.assigned_regs == 0 ? 2.20f : 0.80f);
						} else if(sl.line == 2) {
							role_mult = g.is_offensive_core ? 1.50f : 1.05f;
						}
						float port_dist = province::direct_distance_km(state, best_recip_port, sl.id);
						float sscore = (sl.priority * need_factor * role_mult) / (1.0f + port_dist / 900.0f);
						if(sscore > ov_best_score || (sscore == ov_best_score && ov_best_slot >= 0 && sl.id.index() < slots[size_t(ov_best_slot)].id.index())) {
							ov_best_score = sscore;
							ov_best_slot = int32_t(si);
						}
					}

					if(ov_best_slot >= 0) {
						g.assigned = true;
						auto& chosen_sl = slots[size_t(ov_best_slot)];
						chosen_sl.assigned_regs += g.regs;
						if(g.is_core) {
							chosen_sl.assigned_cores += 1;
						}
						state.world.army_set_ai_province(g.id, chosen_sl.id);
						wave_budget -= g.regs;
						rem_need -= g.regs;
					} else {
						int32_t empty_ov_slot = -1;
						int32_t best_ov_cap = -1;
						for(size_t si = 0; si < slots.size(); ++si) {
							auto const& sl = slots[si];
							if(sl.safe_theater_id == best_recip_stid && sl.assigned_regs == 0 && sl.max_regs > best_ov_cap) {
								best_ov_cap = sl.max_regs;
								empty_ov_slot = int32_t(si);
							}
						}
						dcon::province_id ov_fallback = (empty_ov_slot >= 0)
							? slots[size_t(empty_ov_slot)].id
							: (!recip_port_used_as_fallback ? best_recip_port : dcon::province_id{});
						if(ov_fallback) {
							g.assigned = true;
							if(empty_ov_slot >= 0) {
								slots[size_t(empty_ov_slot)].assigned_regs += g.regs;
							} else {
								recip_port_used_as_fallback = true;
							}
							state.world.army_set_ai_province(g.id, ov_fallback);
							wave_budget -= g.regs;
							rem_need -= g.regs;
						}
					}
				}
			}
		}
	}

	// Step 6C: Safe supply-respecting local rear fallback for all remaining unassigned guards (home garrisons, black-flagged evacuations, Wave 2+ — 0% attrition!)
	for(auto& g : guards) {
		if(g.assigned)
			continue;
		bool const is_bf = state.world.army_get_black_flag(g.id);
		{
			dcon::province_id safe_rear_prov;
			float best_rear_score = -1e9f;
			dcon::province_id empty_max_prov;
			int32_t empty_max_cap = -1;

			for(auto pc : state.world.nation_get_province_control(n)) {
				auto rp = pc.get_province().id;
				if(!rp || rp.index() >= land_count)
					continue;
				if(is_bf) {
					if(state.world.province_get_nation_from_province_ownership(rp) != n)
						continue; // Black flag ONLY clears in owned provinces!
				} else {
					bool same_safe = (g.safe_theater_id != 0)
						? (tmap.safe_theater_of(rp) == g.safe_theater_id)
						: (tmap.theater_of(rp) == g.theater_id);
					if(!same_safe)
						continue;
				}
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
				int32_t s_rear = add_or_get_slot(empty_max_prov, 3);
				if(s_rear >= 0) {
					g.assigned = true;
					slots[size_t(s_rear)].assigned_regs += g.regs;
					state.world.army_set_ai_province(g.id, empty_max_prov);
				}
			} else if(is_bf && cap) {
				// Never leave a black-flagged army stranded abroad in neutral territory during war!
				g.assigned = true;
				state.world.army_set_ai_province(g.id, cap);
			} else {
				// Last-resort fallback: ONLY pick a slot in the same safe theater (or stay at cur_loc) — NEVER assign an unreachable cross-ocean slot without a transport wave!
				int32_t fallback_slot = -1;
				float fb_score = -1.0f;
				for(size_t si = 0; si < slots.size(); ++si) {
					bool same_safe = (g.safe_theater_id != 0)
						? (slots[si].safe_theater_id == g.safe_theater_id)
						: (slots[si].theater_id == g.theater_id);
					if(!same_safe)
						continue;
					if(slots[si].assigned_regs + g.regs <= slots[si].max_regs && slots[si].priority > fb_score) {
						fb_score = slots[si].priority;
						fallback_slot = int32_t(si);
					}
				}
				if(fallback_slot >= 0) {
					g.assigned = true;
					slots[size_t(fallback_slot)].assigned_regs += g.regs;
					state.world.army_set_ai_province(g.id, slots[size_t(fallback_slot)].id);
				} else if(g.cur_loc) {
					g.assigned = true;
					state.world.army_set_ai_province(g.id, g.cur_loc);
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
		if(cap) {
			auto adist = province::sorting_distance(state, a.id, cap);
			auto bdist = province::sorting_distance(state, b.id, cap);
			if(adist != bdist) {
				return adist < bdist;
			}
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
		} else if(g.black_flag && cap) {
			// Never abandon a black-flagged army stranded abroad just because home provinces already hold troops!
			state.world.army_set_ai_province(g.id, cap);
		} else {
			state.world.army_set_ai_province(g.id, dcon::province_id{});
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
	int32_t best_tcap = 0;
	int32_t best_total_ships = 0;
	dcon::navy_id transport_fleet;

	for(auto v : state.world.nation_get_navy_control(controller)) {
		auto nv = v.get_navy();
		if(nv.get_battle_from_navy_battle_participation()
			|| nv.get_is_retreating()
			|| navy_needs_repair(state, nv.id)
			|| !unit_on_ai_control(state, nv))
			continue;
		fleet_activity activity = fleet_activity(nv.get_ai_activity());
		if(activity != fleet_activity::attacking && activity != fleet_activity::idle && activity != fleet_activity::returning_to_base)
			continue;
		auto in_transport = nv.get_army_transport();
		if(in_transport.begin() != in_transport.end())
			continue;

		// Select strictly by REAL transport lift capacity (NOT total warships, which picked a 60-warship + 4-clipper fleet over a 25-clipper fleet!)
		int32_t tcap = military::transport_capacity(state, nv.id);
		if(tcap <= 0)
			continue;
		auto members = nv.get_navy_membership();
		int32_t total_ships = int32_t(members.end() - members.begin());

		if(tcap > best_tcap
			|| (tcap == best_tcap && total_ships > best_total_ships)
			|| (tcap == best_tcap && total_ships == best_total_ships && transport_fleet && nv.id.index() < transport_fleet.index())) {
			best_tcap = tcap;
			best_total_ships = total_ships;
			transport_fleet = nv.id;
		}
	}

	return transport_fleet;
}

void move_idle_guards_impl(sys::state& state, bool only_at_war) {
	std::vector<dcon::army_id> require_transport;

	auto process_army = [&](dcon::army_fat_id ar) {
		if(ar.get_ai_activity() != uint8_t(army_activity::on_guard)
			|| !ar.get_controller_from_army_control()
			|| !unit_on_ai_control(state, ar)
			|| ar.get_battle_from_army_battle_participation()
			|| ar.get_navy_from_army_transport()
			|| ar.get_is_retreating()) {
			return;
		}

		auto ctrl = ar.get_controller_from_army_control();
		auto loc = ar.get_location_from_army_location().id;

		// If a moving wartime on_guard army's path now crosses a newly enemy-occupied tile, halt it immediately
		// so it NEVER cuts through enemy territory (still allows final-step battle reinforcements into active battles!).
		if(ar.get_arrival_time()) {
			if(ctrl.get_is_at_war() && !ar.get_black_flag() && loc && !province::is_sea(state, loc)
				&& province::has_safe_access_to_province(state, ctrl, loc)) {
				auto cur_path = ar.get_path();
				if(cur_path.size() > 0) {
					auto path_dest = cur_path.at(0);
					bool dest_is_battle = path_dest
						&& (state.world.province_get_land_battle_location(path_dest).begin() != state.world.province_get_land_battle_location(path_dest).end());
					uint32_t const start_k = dest_is_battle ? 1u : 0u;
					bool crosses_hostile = false;
					for(uint32_t k = start_k; k < cur_path.size(); ++k) {
						auto step_p = cur_path.at(k);
						if(!step_p || province::is_sea(state, step_p) || !province::has_safe_access_to_province(state, ctrl, step_p)) {
							crosses_hostile = true;
							break;
						}
					}
					if(crosses_hostile) {
						military::stop_army_movement(state, ar.id);
					} else {
						return;
					}
				} else {
					return;
				}
			} else {
				return;
			}
		}

		auto dest = ar.get_ai_province().id;
		if(dest && dest != loc) {
			bool valid_path = false;
			if(ctrl.get_is_at_war() && !ar.get_black_flag() && province::has_safe_access_to_province(state, ctrl, dest)) {
				if(!ctrl.get_is_player_controlled()) {
					state.world.army_set_special_order(ar.id, uint8_t(military::special_army_order::none));
				}
				auto spath = ai_front_helpers::make_front_safe_land_path(state, loc, dest, ctrl, ar.id, false);
				if(!spath.empty()) {
					valid_path = military::set_army_path(state, ar.id, spath, ctrl);
				} else if(loc && !province::is_sea(state, loc) && !province::has_safe_access_to_province(state, ctrl, loc)) {
					// Army is itself stranded on hostile/occupied ground trying to withdraw back to safe territory
					valid_path = military::move_army_ai(state, ar.id, dest, ctrl);
				}
			} else {
				valid_path = military::move_army_ai(state, ar.id, dest, ctrl);
			}

			if(!valid_path) {
				// Units delegated to the AI won't transport themselves on their own
				if(!ctrl.get_is_player_controlled())
					require_transport.push_back(ar.id);
			} else if(!ctrl.get_is_player_controlled()
				&& !ar.get_black_flag()
				&& ar.get_path().size() >= 6
				&& state.world.province_get_building_level(ar.get_location_from_army_location(), uint8_t(economy::province_building_type::railroad)) > 0
				&& !ai_front_helpers::is_province_near_hostiles(state, ctrl, ar.get_location_from_army_location(), 2)
				&& !ai_front_helpers::is_province_near_hostiles(state, ctrl, dest, 2)) {
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

	// Cache walkable_theater_map ONLY for the few controllers that actually have armies waiting for sea ferrying
	std::vector<std::pair<dcon::nation_id, ai_front_helpers::walkable_theater_map>> guard_tmaps;
	auto get_guard_tmap = [&](dcon::nation_id ctrl) -> ai_front_helpers::walkable_theater_map const& {
		for(auto const& kv : guard_tmaps) {
			if(kv.first == ctrl)
				return kv.second;
		}
		guard_tmaps.emplace_back(ctrl, ai_front_helpers::walkable_theater_map{});
		guard_tmaps.back().second.build(state, ctrl);
		return guard_tmaps.back().second;
	};

	for(uint32_t i = 0; i < require_transport.size(); ++i) {
		auto army_i = require_transport[i];
		auto coastal_target_prov = state.world.army_get_location_from_army_location(army_i);
		auto controller = state.world.army_get_controller_from_army_control(army_i);
		dcon::province_id dest_prov = state.world.army_get_ai_province(army_i).id;
		auto regs = state.world.army_get_army_membership(army_i);
		int32_t iregs = int32_t(regs.end() - regs.begin());
		bool const is_bf = state.world.army_get_black_flag(army_i);
		auto const& gtmap = get_guard_tmap(controller);
		uint16_t dest_tid = gtmap.theater_of(dest_prov);
		uint16_t dest_stid = gtmap.safe_theater_of(dest_prov);

		// First: check if controller ALREADY has ANY fleet in `boarding` mode (even if briefly in battle, do NOT start a 2nd competing boarding fleet!)
		dcon::navy_id existing_boarding;
		bool boarding_in_battle = false;
		for(auto v : state.world.nation_get_navy_control(controller)) {
			auto nv = v.get_navy();
			if(unit_on_ai_control(state, nv)
				&& nv.get_ai_activity() == uint8_t(fleet_activity::boarding)) {
				existing_boarding = nv.id;
				boarding_in_battle = bool(nv.get_battle_from_navy_battle_participation());
				break;
			}
		}
		if(existing_boarding) {
			if(boarding_in_battle) {
				state.world.army_set_ai_province(army_i, dcon::province_id{});
				continue;
			}
			int32_t reserved_unboarded = 0;
			dcon::province_id boarding_dest;
			for(auto a : state.world.nation_get_army_control(controller)) {
				auto oa = a.get_army();
				if(oa.get_navy_from_army_transport() == existing_boarding) {
					if(!boarding_dest) boarding_dest = oa.get_ai_province().id;
				} else if(oa.get_ai_activity() == uint8_t(army_activity::transport_guard) || oa.get_ai_activity() == uint8_t(army_activity::transport_attack)) {
					auto oregs = oa.get_army_membership();
					reserved_unboarded += int32_t(oregs.end() - oregs.begin());
					if(!boarding_dest) boarding_dest = oa.get_ai_province().id;
				}
			}
			int32_t rem_tcap = military::free_transport_capacity(state, existing_boarding) - reserved_unboarded;
			uint16_t boarding_tid = gtmap.theater_of(boarding_dest);
			uint16_t boarding_stid = gtmap.safe_theater_of(boarding_dest);
			bool same_theater_dest = !boarding_dest || boarding_dest == dest_prov
				|| ((dest_stid != 0 && boarding_stid != 0) ? (boarding_stid == dest_stid) : (dest_tid != 0 && boarding_tid == dest_tid));
			if(same_theater_dest && rem_tcap >= iregs) {
				auto bpath = state.world.navy_get_path(existing_boarding);
				dcon::province_id bport = bpath.size() > 0 ? bpath[0] : state.world.navy_get_location_from_navy_location(existing_boarding);
				if(bport && province::is_sea(state, bport)) {
					// Fleet is waiting offshore in a sea tile (e.g. evacuating black-flagged armies from neutral France!).
					// Pick the closest reachable coastal province fronting that sea tile (black-flagged armies do NOT need safe access to neutral French coast!).
					dcon::province_id best_coast_p;
					float best_coast_km = 1e9f;
					for(auto padj : state.world.province_get_province_adjacency(bport)) {
						auto other = padj.get_connected_provinces(0) == bport ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
						if(other && !province::is_sea(state, other)
							&& state.world.province_get_port_to(other) == bport
							&& (is_bf || province::has_safe_access_to_province(state, controller, other))) {
							float ckm = province::direct_distance_km(state, coastal_target_prov, other);
							if((other == coastal_target_prov || !province::make_land_unit_path(state, coastal_target_prov, other, controller, army_i).empty())
								&& (ckm < best_coast_km || (ckm == best_coast_km && best_coast_p && other.index() < best_coast_p.index()))) {
								best_coast_km = ckm;
								best_coast_p = other;
							}
						}
					}
					bport = best_coast_p;
				}
				if(bport && (coastal_target_prov == bport || military::move_army_ai(state, army_i, bport, controller))) {
					state.world.army_set_ai_activity(army_i, uint8_t(army_activity::transport_guard));
					continue;
				}
			}
			state.world.army_set_ai_province(army_i, dcon::province_id{});
			continue;
		}

		dcon::navy_id transport_fleet = find_transport_fleet(state, controller);
		int32_t raw_tcap = transport_fleet ? military::transport_capacity(state, transport_fleet) : 0;

		// No transport fleet or no naval supremacy at war: clear waiting guards for this controller
		if(!transport_fleet || raw_tcap <= 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
			for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					state.world.army_set_ai_province(require_transport[j], dcon::province_id{}); // stop rechecking these units
					require_transport[j] = require_transport.back();
					require_transport.pop_back();
				}
			}
			state.world.army_set_ai_province(army_i, dcon::province_id{}); // stop rechecking unit
			continue;
		}
		// Oversized stack for our transport fleet:
		// If black-flagged (e.g. 36K Brazilian army stranded in France after WWI with only 10 transports = 30K lift!),
		// split off the excess regiments cleanly so Wave 1 evacuates immediately and Wave 2 returns for the rest!
		if(iregs > raw_tcap) {
			if(is_bf && raw_tcap >= 1) {
				std::vector<dcon::regiment_id> excess_regs;
				excess_regs.reserve(size_t(iregs - raw_tcap));
				int32_t keep_cnt = 0;
				for(auto rm : state.world.army_get_army_membership(army_i)) {
					if(keep_cnt < raw_tcap) {
						++keep_cnt;
					} else {
						excess_regs.push_back(rm.get_regiment().id);
					}
				}
				if(!excess_regs.empty() && military::can_split_army<command::actor::ai>(state, controller, army_i, excess_regs)) {
					military::split_army<command::actor::ai>(state, controller, army_i, excess_regs, 0);
					dcon::army_id child_army = state.world.regiment_get_army_from_army_membership(excess_regs[0]);
					if(child_army && child_army != army_i) {
						state.world.army_set_ai_activity(child_army, uint8_t(army_activity::on_guard));
						state.world.army_set_ai_province(child_army, dest_prov);
					}
					iregs = raw_tcap;
				} else {
					state.world.army_set_ai_province(army_i, dcon::province_id{});
					continue;
				}
			} else {
				state.world.army_set_ai_province(army_i, dcon::province_id{});
				continue;
			}
		}
		int32_t tcap = raw_tcap - iregs;

		// Prefer a coastal embarkation point where one of the assigned stacks for this wave is ALREADY standing!
		if(!state.world.province_get_is_coast(coastal_target_prov) || !state.world.province_get_port_to(coastal_target_prov)) {
			dcon::province_id better_coast;
			for(uint32_t j = i + 1; j < uint32_t(require_transport.size()); ++j) {
				auto j_army = require_transport[j];
				if(state.world.army_get_controller_from_army_control(j_army) == controller) {
					dcon::province_id j_dest = state.world.army_get_ai_province(j_army).id;
					uint16_t j_tid = gtmap.theater_of(j_dest);
					if(j_dest == dest_prov || (dest_tid != 0 && j_tid == dest_tid)) {
						auto jloc = state.world.army_get_location_from_army_location(j_army);
						bool coast_access_ok = is_bf
							? true
							: (province::has_naval_access_to_province(state, controller, jloc) && !ai_front_helpers::province_has_enemy_army(state, controller, jloc));
						if(jloc && state.world.province_get_is_coast(jloc) && state.world.province_get_port_to(jloc)
							&& coast_access_ok
							&& military::move_army_ai(state, army_i, jloc, controller)) {
							better_coast = jloc;
							break;
						}
					}
				}
			}
			if(better_coast) {
				coastal_target_prov = better_coast;
			} else {
				auto path = is_bf
					? province::make_unowned_path_to_nearest_coast(state, coastal_target_prov)
					: province::make_path_to_nearest_coast(state, controller, coastal_target_prov);
				bool valid_path = military::set_army_path(state, army_i, path, controller);
				if(!valid_path) {
					state.world.army_set_ai_province(army_i, dcon::province_id{}); // stop rechecking unit
					continue; // army could not reach coast
				} else {
					coastal_target_prov = path.front();
				}
			}
		}

		{
			dcon::province_id fleet_destination = province::has_naval_access_to_province(state, controller, coastal_target_prov) ? coastal_target_prov : state.world.province_get_port_to(coastal_target_prov).id;
			bool fleet_routed = false;
			if(fleet_destination && fleet_destination == state.world.navy_get_location_from_navy_location(transport_fleet)) {
				military::stop_navy_movement(state, transport_fleet);
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
				fleet_routed = true;
			} else if(fleet_destination && military::move_navy_ai(state, transport_fleet, fleet_destination)) {
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
				fleet_routed = true;
			}

			// Fallback for black-flagged armies stranded near an unreachable inland sea (e.g. Caspian Sea or closed strait!):
			// BFS across passable land to find the nearest coast whose offshore sea tile IS reachable by transport_fleet!
			if(!fleet_routed && is_bf) {
				auto start_loc = state.world.army_get_location_from_army_location(army_i);
				auto fleet_loc = state.world.navy_get_location_from_navy_location(transport_fleet);
				uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
				int32_t const lcount = int32_t(std::min(state.world.province_size(), first_sea));
				if(start_loc && start_loc.index() < lcount && fleet_loc) {
					std::vector<uint8_t> vis(size_t(lcount), 0);
					std::vector<dcon::province_id> bfs_q;
					bfs_q.reserve(256);
					vis[start_loc.index()] = 1;
					bfs_q.push_back(start_loc);
					int32_t sea_checks = 0;
					std::vector<dcon::province_id> tested_seas;
					dcon::province_id reachable_coast;
					dcon::province_id reachable_fdest;

					for(size_t qi = 0; qi < bfs_q.size() && !reachable_coast && sea_checks < 24; ++qi) {
						auto u = bfs_q[qi];
						if(u != coastal_target_prov && state.world.province_get_is_coast(u)) {
							dcon::province_id s_tile = state.world.province_get_port_to(u).id;
							if(s_tile && std::find(tested_seas.begin(), tested_seas.end(), s_tile) == tested_seas.end()) {
								tested_seas.push_back(s_tile);
								dcon::province_id fdest = province::has_naval_access_to_province(state, controller, u) ? u : s_tile;
								++sea_checks;
								if(fdest == fleet_loc || !province::make_naval_unit_path(state, fleet_loc, fdest, controller).empty()) {
									reachable_coast = u;
									reachable_fdest = fdest;
									break;
								}
							}
						}
						for(auto padj : state.world.province_get_province_adjacency(u)) {
							auto bits = state.world.province_adjacency_get_type(padj.id);
							if((bits & province::border::impassible_bit) != 0)
								continue;
							auto v = padj.get_connected_provinces(0) == u ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
							if(v && v.index() < lcount && !vis[v.index()]) {
								vis[v.index()] = 1;
								bfs_q.push_back(v);
							}
						}
					}

					if(reachable_coast && reachable_fdest
						&& (start_loc == reachable_coast || military::move_army_ai(state, army_i, reachable_coast, controller))) {
						coastal_target_prov = reachable_coast;
						if(reachable_fdest == fleet_loc) {
							military::stop_navy_movement(state, transport_fleet);
							state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
							fleet_routed = true;
						} else if(military::move_navy_ai(state, transport_fleet, reachable_fdest)) {
							state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
							fleet_routed = true;
						}
					}
				}
			}

			if(!fleet_routed) {
				military::stop_army_movement(state, army_i);
				state.world.army_set_ai_province(army_i, dcon::province_id{});
				continue;
			}
		}

		state.world.army_set_ai_activity(army_i, uint8_t(army_activity::transport_guard));

		auto destination_region = state.world.province_get_connected_region_id(dest_prov);

		// Scoop up ALL other stacks heading to the SAME safe destination landmass!
		for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
			auto j_army = require_transport[j];
			if(state.world.army_get_controller_from_army_control(j_army) == controller) {
				dcon::province_id j_dest = state.world.army_get_ai_province(j_army).id;
				uint16_t j_tid = gtmap.theater_of(j_dest);
				uint16_t j_stid = gtmap.safe_theater_of(j_dest);
				bool same_dest_landmass = (j_dest == dest_prov)
					|| ((dest_stid != 0 && j_stid != 0) ? (j_stid == dest_stid) : (dest_tid != 0 && j_tid == dest_tid))
					|| (state.world.province_get_connected_region_id(j_dest) == destination_region);
				auto jregs = state.world.army_get_army_membership(j_army);
				int32_t jcount = int32_t(jregs.end() - jregs.begin());
				bool boarded = false;
				if(same_dest_landmass && tcap >= jcount) {
					if(state.world.army_get_location_from_army_location(j_army) == coastal_target_prov) {
						state.world.army_set_ai_activity(j_army, uint8_t(army_activity::transport_guard));
						tcap -= jcount;
						boarded = true;
					} else if(military::move_army_ai(state, j_army, coastal_target_prov, controller)) {
						state.world.army_set_ai_activity(j_army, uint8_t(army_activity::transport_guard));
						tcap -= jcount;
						boarded = true;
					}
				}
				if(!boarded) {
					state.world.army_set_ai_province(j_army, dcon::province_id{});
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
		if(!ar.get_controller_from_army_control() || !unit_on_ai_control(state, ar))
			continue;
		auto act = army_activity(ar.get_ai_activity());
		if((ar.get_battle_from_army_battle_participation() || ar.get_is_retreating())
			&& !ar.get_navy_from_army_transport()
			&& (act == army_activity::transport_guard || act == army_activity::transport_attack || act == army_activity::attack_transport)) {
			// Army got intercepted or routed before boarding -> release cleanly so waiting transport fleet never deadlocks!
			ar.set_ai_activity(uint8_t(army_activity::on_guard));
			ar.set_ai_province(dcon::province_id{});
			continue;
		}
		if(ar.get_battle_from_army_battle_participation())
			continue;
		if(ar.get_navy_from_army_transport())
			continue;
		if(ar.get_arrival_time())
			continue;

		if(act == army_activity::transport_guard || act == army_activity::transport_attack) {
			auto controller = ar.get_controller_from_army_control();
			dcon::navy_id transports;
			for(auto v : controller.get_navy_control()) {
				if(v.get_navy().get_ai_activity() == uint8_t(fleet_activity::boarding)) {
					transports = v.get_navy();
					break;
				}
			}
			if(!transports) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
				continue;
			}
			if(state.world.navy_get_arrival_time(transports) || state.world.navy_get_battle_from_navy_battle_participation(transports))
				continue; // still moving

			auto regs = ar.get_army_membership();
			int32_t rcount = int32_t(regs.end() - regs.begin());
			if(rcount <= 0 || military::free_transport_capacity(state, transports) < rcount) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
				ar.set_ai_province(dcon::province_id{});
				continue;
			}

			auto army_location = ar.get_location_from_army_location();
			auto transport_location = state.world.navy_get_location_from_navy_location(transports);
			if(transport_location == army_location) {
				ar.set_navy_from_army_transport(transports);
				ar.set_black_flag(false);
			} else if(transport_location && army_location.get_port_to() == transport_location) {
				if(!military::move_army_ai(state, ar, transport_location, controller)) {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
				}
			} else if(transport_location && !province::is_sea(state, transport_location)) {
				// Fleet is docked at a land port on the same landmass (e.g. Portsmouth while Stack 2/3 is in London):
				// march overland to the embarkation port instead of aborting Stacks 2 & 3!
				if(!military::move_army_ai(state, ar, transport_location, controller)) {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
				}
			} else if(transport_location && province::is_sea(state, transport_location)) {
				// Fleet is waiting offshore (e.g. evacuating black-flagged armies from a neutral coast!):
				// walk to the CLOSEST reachable coastal province fronting that sea tile (then next tick step directly into the sea tile to embark!)
				dcon::province_id best_coast;
				float best_coast_km = 1e9f;
				for(auto padj : state.world.province_get_province_adjacency(transport_location)) {
					auto other = padj.get_connected_provinces(0) == transport_location ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(other && !province::is_sea(state, other) && state.world.province_get_port_to(other) == transport_location) {
						float ckm = province::direct_distance_km(state, army_location, other);
						if(!province::make_land_unit_path(state, army_location, other, controller, ar).empty()
							&& (ckm < best_coast_km || (ckm == best_coast_km && best_coast && other.index() < best_coast.index()))) {
							best_coast_km = ckm;
							best_coast = other;
						}
					}
				}
				if(!best_coast || !military::move_army_ai(state, ar, best_coast, controller)) {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
				}
			} else {
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

	// Size battle commitment to the ACTUAL enemy size in this battle (never send 180k against a 12k landing just because 1836 CW == 30!)
	int32_t needed_front = std::min(cw, std::max(4, enemy_regs_here + 4));
	int32_t target_regs = std::min(cw * 2, std::max(needed_front + 4, int32_t(std::ceil(float(enemy_regs_here) * 1.65f)) + 3));
	if(friendly_regs_here >= target_regs)
		return;

	// Fast early-out BEFORE building theater/frontline maps if n has zero ready AI armies within 600 km!
	bool any_near_ready = false;
	for(auto ar : state.world.nation_get_army_control(n)) {
		auto army = ar.get_army();
		if(!unit_on_ai_control(state, army)
			|| army.get_battle_from_army_battle_participation()
			|| army.get_navy_from_army_transport()
			|| army.get_black_flag()
			|| army.get_is_retreating())
			continue;
		army_activity activity = army_activity(army.get_ai_activity());
		if(activity != army_activity::on_guard && activity != army_activity::attacking && activity != army_activity::attack_gathered)
			continue;
		auto loc = army.get_location_from_army_location().id;
		if(!loc || loc == p || province::is_sea(state, loc))
			continue;
		if(province::direct_distance_km(state, loc, p) <= 600.0f && army_ready_for_battle(state, n, army)) {
			any_near_ready = true;
			break;
		}
	}
	if(!any_near_ready)
		return;

	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n, p);
	uint16_t p_theater = tmap.theater_of(p);
	if(p_theater == 0)
		return;
	uint16_t p_safe_theater = tmap.safe_theater_of(p);
	bool continuous_front = false;
	if(at_war) {
		std::vector<dcon::nation_id> war_allies;
		std::vector<dcon::nation_id> war_enemies;
		ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
		ai_front_helpers::frontline_sector_map fmap;
		fmap.build(state, n, tmap, war_allies, war_enemies, false);
		continuous_front = fmap.summary_for_province(state, p, p_theater, p_safe_theater).continuous_front;
	}

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
		auto bpath = ai_front_helpers::make_front_safe_land_path(state, c.loc, p, n, c.id, true);
		if(bpath.empty())
			continue;
		state.world.army_set_special_order(c.id, uint8_t(military::special_army_order::none));
		if(military::set_army_path(state, c.id, bpath, n, true)) {
			auto prev_act = army_activity(state.world.army_get_ai_activity(c.id));
			if(prev_act == army_activity::attacking || prev_act == army_activity::attack_gathered) {
				state.world.army_set_ai_activity(c.id, uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(c.id, p);
			}
			pin_tracker.commit_pull(c.loc, c.regs, c.is_core);
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
		if(!type)
			continue;
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
			if(!type)
				continue;
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
			if(!type)
				continue;
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
	float front_max_org_weighted = 0.0f;
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
	float const deploy_org = std::max(0.05f, state.defines.alice_reg_deploy_from_reserve_org);
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
			if(!uid)
				continue;
			auto& bdef = state.military_definitions.unit_base_definitions[uid];
			auto stats = tech_n ? state.world.nation_get_unit_stats(tech_n, uid) : military::unit_definition{};
			auto& d = (tech_n && stats.discipline_or_evasion > 0.0f) ? stats : bdef;

			// Pure engine rule (military.cpp:7656): ANY land unit with support > 0 shoots from back_line (100% mod-agnostic!)
			bool back = (d.support > 0.0f);
			// In military.cpp:7267, a regiment with org >= deploy_org fights at full `str` damage (NO org multiplier on DPS!),
			// while a regiment below deploy_org rests in reserve (0 current DPS until org recovers).
			// Use identical weight `w_str` in numerator AND denominator so avg stat NEVER explodes by 1/org!
			float active_w = (org >= deploy_org) ? 1.0f : 0.15f;
			float w_str = str * active_w;
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

			float atk_term = w_str * (d.attack_or_gun_power * 0.1f + 1.0f);
			float def_term = w_str * (d.defence_or_hull * 0.1f + 1.0f);
			if(back) {
				s.back_eff += w_str;
				s.back_atk_sup_sum += atk_term * std::max(0.1f, d.support);
				s.back_def_sup_sum += def_term * std::max(0.1f, d.support);
			} else {
				s.front_eff += w_str;
				s.front_org_pool += str * org * max_org;
				s.front_max_org_weighted += max_org * str;
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

// Analytical battle outcome evaluator directly reflecting Project Alice formulas in military.cpp:170-172, 7212-7530
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

	// Active frontline & backline slots capped by combat_width; backline is ONLY exposed to front_line when back > front!
	auto compute_firepower = [&](side_summary const& s, bool is_att) -> std::pair<float, float> {
		float active_front = std::min(fcw, s.front_eff);
		float active_back_total = std::min(fcw, s.back_eff);
		float exposed_back = std::max(0.0f, active_back_total - active_front);
		float safe_back = active_back_total - exposed_back;

		float avg_front_stat = s.front_eff > 0.01f ? ((is_att ? s.front_atk_sum : s.front_def_sum) / s.front_eff) : 1.0f;
		float avg_back_stat = s.back_eff > 0.01f ? ((is_att ? s.back_atk_sup_sum : s.back_def_sup_sum) / s.back_eff) : 1.0f;

		// Exposed artillery in front line loses support multiplier & takes full frontline losses
		float firepower = active_front * avg_front_stat + safe_back * avg_back_stat + exposed_back * (avg_back_stat * 0.45f);
		float expose_penalty = (exposed_back > 0.5f) ? (1.0f / (1.0f + exposed_back / std::max(1.0f, active_front + exposed_back))) : 1.0f;
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

	// Exact daily org & str damage scalers matching military.cpp:170-172, 7267-7305!
	// Both are in matching units: fraction of regiment org lost/day vs fraction of regiment strength lost/day.
	float const org_scaler = 6.0f;
	float const str_scaler = 600.0f / std::max(1.0f, state.defines.pop_size_per_regiment);
	float att_org_dps_vs_def = (att_fp * org_scaler) / std::max(0.1f, fort_mod * def_disc * (1.0f + def_exp) * def_max_org);
	float def_org_dps_vs_att = (def_fp * org_scaler) / std::max(0.1f, att_disc * (1.0f + att_exp) * att_max_org);

	float att_str_dps_vs_def = (att_fp * str_scaler) / std::max(0.1f, fort_mod * def_tactics * (1.0f + def_exp));
	float def_str_dps_vs_att = (def_fp * str_scaler) / std::max(0.1f, att_tactics * (1.0f + att_exp));

	// Staying power accounts for frontline reserve depth & FRONTLINE org (never masking broken infantry behind 100%-org artillery!)
	auto staying_power = [&](side_summary const& s, float expose_pen) -> float {
		float front_depth = std::max(0.1f, s.front_str_pool + s.back_eff * 0.20f);
		float capped_depth = front_depth <= fcw * 4.0f
			? front_depth
			: (fcw * 4.0f + std::sqrt(front_depth - fcw * 4.0f) * std::sqrt(fcw * 2.0f));
		float avg_org_frac = (s.front_max_org_weighted > 0.1f)
			? std::clamp(s.front_org_pool / s.front_max_org_weighted, 0.05f, 1.0f)
			: (s.total_str > 0.01f ? std::clamp(s.total_org_pool / std::max(1.0f, s.max_org_weighted), 0.05f, 1.0f) : 0.5f);
		return capped_depth * (0.10f + 0.90f * avg_org_frac) * expose_pen;
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

	// Fast early-out BEFORE building theater maps if n has zero ready stationary AI on_guard armies!
	bool has_any_ready = false;
	for(auto ar : state.world.nation_get_army_control(n)) {
		auto army = ar.get_army();
		if(unit_on_ai_control(state, army)
			&& army_activity(army.get_ai_activity()) == army_activity::on_guard
			&& !army.get_battle_from_army_battle_participation()
			&& !army.get_navy_from_army_transport()
			&& !army.get_black_flag()
			&& !army.get_arrival_time()
			&& !army.get_is_retreating()
			&& army_ready_for_battle(state, n, army)) {
			has_any_ready = true;
			break;
		}
	}
	if(!has_any_ready)
		return;

	// Build passable cross-border theater map + strictly safe friendly theater map
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n);

	struct ready_stack {
		dcon::army_id id;
		dcon::province_id loc;
		uint16_t theater_id = 0;
		uint16_t safe_theater_id = 0;
		uint16_t sector_id = 0;
		int32_t regs = 0;
		int32_t dig_in = 0;
		bool is_core = false;
		bool is_offensive_core = false;
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
		if(activity == army_activity::attack_transport
			|| activity == army_activity::transport_attack
			|| activity == army_activity::transport_guard) {
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
		uint16_t loc_tid = tmap.theater_of(loc);
		uint16_t loc_stid = tmap.safe_theater_of(loc);
		// Respect overseas guard ferry waves already queued by distribute_guards_wartime (e.g. UK->Canada, UK->France!)
		// so assign_targets NEVER steals those armies for a competing beach assault before move_idle_guards boards them!
		auto cur_ai_prov = army.get_ai_province().id;
		if(cur_ai_prov && cur_ai_prov.index() < land_count) {
			uint16_t cur_dest_tid = tmap.theater_of(cur_ai_prov);
			uint16_t cur_dest_stid = tmap.safe_theater_of(cur_ai_prov);
			bool queued_sea_ferry = (loc_stid != 0 && cur_dest_stid != 0)
				? (cur_dest_stid != loc_stid)
				: (cur_dest_tid != 0 && loc_tid != 0 && cur_dest_tid != loc_tid);
			if(queued_sea_ferry) {
				active_naval_invasion = true;
				continue;
			}
		}
		auto regs = army.get_army_membership();
		int32_t rcount = int32_t(regs.end() - regs.begin());
		if(rcount == 0)
			continue;
		bool core = ai_front_helpers::army_has_backline_core(state, n, army.id);
		bool off_core = core && ai_front_helpers::army_has_offensive_utility(state, n, army.id);
		ready_stacks.push_back(ready_stack{
			army.id,
			loc,
			loc_tid,
			loc_stid,
			uint16_t(0),
			rcount,
			int32_t(army.get_dig_in()),
			core,
			off_core,
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

	// Per-frontline sector map: evaluates each separate active frontline (e.g. Canada vs Mexico, Galicia vs Alps, France vs Russia) independently!
	ai_front_helpers::frontline_sector_map fmap;
	ai_front_helpers::front_sector_summary peace_default_sec{};
	peace_default_sec.continuous_front = false;
	peace_default_sec.stance = ai_front_helpers::war_stance::active_offense;
	if(is_at_war) {
		fmap.build(state, n, tmap, war_allies, war_enemies, time_against_us);
		for(auto& rs : ready_stacks) {
			rs.sector_id = fmap.sector_for_province(state, rs.loc, rs.theater_id, rs.safe_theater_id);
		}
	}
	auto get_front_sector = [&](dcon::province_id p, uint16_t tid, uint16_t stid = 0) -> ai_front_helpers::front_sector_summary const& {
		if(!is_at_war || tid == 0)
			return peace_default_sec;
		return fmap.summary_for_province(state, p, tid, stid);
	};
	auto get_stack_sector = [&](ready_stack const& rs) -> ai_front_helpers::front_sector_summary const& {
		if(!is_at_war || rs.sector_id == 0 || size_t(rs.sector_id) >= fmap.sectors.size())
			return peace_default_sec;
		return fmap.sectors[rs.sector_id];
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
	std::vector<dcon::province_id> wargoal_goals;
	std::vector<dcon::province_id> strategic_goals;

	auto add_wargoal_anchor = [&](dcon::province_id gp) {
		if(!gp || gp.index() >= land_count)
			return;
		if(std::find(wargoal_goals.begin(), wargoal_goals.end(), gp) == wargoal_goals.end()) {
			wargoal_goals.push_back(gp);
		}
	};

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
							add_wargoal_anchor(pid);
						} else {
							auto pctrl = m.get_province().get_nation_from_province_control();
							if((pctrl && military::are_at_war(state, n, pctrl)) || ai_front_helpers::province_has_enemy_army(state, n, pid)) {
								is_enemy_held_wargoal[pid.index()] = 1;
								add_wargoal_anchor(pid);
							}
						}
					}
				} else if(auto tn = wg.get_target_nation(); tn) {
					auto tcap = tn.get_capital().id;
					if(tcap && tcap.index() < land_count) {
						if(wg_ours) {
							is_our_wargoal[tcap.index()] = 1;
							add_wargoal_anchor(tcap);
						} else {
							auto pctrl = state.world.province_get_nation_from_province_control(tcap);
							if((pctrl && military::are_at_war(state, n, pctrl)) || ai_front_helpers::province_has_enemy_army(state, n, tcap)) {
								is_enemy_held_wargoal[tcap.index()] = 1;
								add_wargoal_anchor(tcap);
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
		bool stack_already_here = false;
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
		bool stack_here = false;
		for(auto const& rs : ready_stacks) {
			if(rs.loc == p) {
				stack_here = true;
				break;
			}
		}

		// Respect Project Alice's 1-province logistics rule (logistics.cpp:800-832):
		// Applies EQUALLY to enemy land AND enemy/rebel-occupied own/allied land!
		// NEVER march across deep occupied rear to a distant wargoal (e.g. marching through occupied Macedonia to Thessaly)!
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
		// Land advance MUST advance the front tile-by-tile from supplied friendly territory (`adj_friendly`),
		// or liberate a province a ready stack is already standing on (`stack_here`).
		// Overseas naval invasion requires coastal province when no land corridor exists.
		bool land_viable = same_theater && (adj_friendly || stack_here);
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
			same_theater,
			stack_here
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

		// Directional pull along depth-1 land corridor toward wargoals (stronger) and enemy capitals in the same walkable theater
		if(pt.adjacent_to_friendly && pt.theater_id != 0) {
			if(!wargoal_goals.empty()) {
				float best_wg_km = 1e9f;
				for(auto gp : wargoal_goals) {
					if(tmap.theater_of(gp) == pt.theater_id) {
						float gd = province::direct_distance_km(state, p, gp);
						if(gd < best_wg_km)
							best_wg_km = gd;
					}
				}
				if(best_wg_km < 1e8f) {
					value += 24.0f / (1.0f + best_wg_km / 220.0f);
				}
			}
			if(!strategic_goals.empty()) {
				float best_gdist_km = 1e9f;
				for(auto gp : strategic_goals) {
					if(tmap.theater_of(gp) == pt.theater_id) {
						float gd = province::direct_distance_km(state, p, gp);
						if(gd < best_gdist_km)
							best_gdist_km = gd;
					}
				}
				if(best_gdist_km < 1e8f) {
					value += 12.0f / (1.0f + best_gdist_km / 250.0f);
				}
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
		float est_days = (pt.has_same_theater_stack && (pt.adjacent_to_friendly || pt.stack_already_here))
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

	// Helper: returns peak enemy regiment count directly facing p (1st line) or any safe neighbor of p (2nd line reserve)
	auto front_backing_threat = [&](dcon::province_id p) -> float {
		if(!is_at_war || !p || province::is_sea(state, p))
			return 0.0f;
		float peak = ai_front_helpers::adjacent_enemy_regs_excluding(state, n, p, dcon::province_id{});
		for(auto padj : state.world.province_get_province_adjacency(p)) {
			if(province::is_adjacency_impassable(state, n, padj.id))
				continue;
			dcon::province_id mid = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
			if(mid && !province::is_sea(state, mid) && province::has_safe_access_to_province(state, n, mid)) {
				float nb_e = ai_front_helpers::adjacent_enemy_regs_excluding(state, n, mid, dcon::province_id{});
				if(nb_e > peak) {
					peak = nb_e;
				}
			}
		}
		return peak;
	};

	// Limit target evaluations per tick for fast CPU in massive world wars
	uint32_t evaluated_targets = 0;
	int32_t defensive_empty_grabs = 0;
	for(uint32_t i = 0; i < psize && evaluated_targets < 48; ++i) {
		auto const& pt = potential_targets[i];
		auto target_p = pt.location;
		if(!target_p)
			continue;
		++evaluated_targets;

		bool use_land = pt.has_same_theater_stack && (pt.adjacent_to_friendly || pt.stack_already_here);
		bool use_naval = !use_land && pt.is_coastal && is_at_war && has_naval_adv && free_tcap > 0 && !active_naval_invasion;
		if(!use_land && !use_naval)
			continue;

		// CRITICAL: If we ALREADY control or have allied naval access to a safe friendly port on the EXACT SAME walkable landmass
		// within 2200 km (e.g. Canada's Halifax/Quebec for UK vs USA, French ports for UK/USA vs Germany, Freetown for Liberia, Hong Kong for China):
		// NEVER launch a slow, sea-penalized hostile beach assault onto enemy beaches!
		// - If we already have armies on that continent -> fight overland (extra reinforcements come port-to-port via distribute_guards_wartime).
		// - If we have 0 armies there yet -> ferry our expeditionary corps DIRECTLY into `friendly_ferry_port` (0-day instant unload, 0 penalty, full supply)!
		dcon::province_id friendly_ferry_port;
		if(use_naval && pt.theater_id != 0 && free_transport) {
			auto fleet_loc = state.world.navy_get_location_from_navy_location(free_transport);
			uint16_t t_stid = tmap.safe_theater_of(target_p);
			if(t_stid != 0 && tmap.safe_theater_has_port(t_stid)) {
				friendly_ferry_port = tmap.best_reachable_safe_port_in_safe_theater(state, n, t_stid, target_p, fleet_loc, 2200.0f);
			} else if(tmap.theater_has_safe_port(pt.theater_id)) {
				friendly_ferry_port = tmap.best_reachable_safe_port(state, n, pt.theater_id, target_p, fleet_loc, 2200.0f);
			}
			if(friendly_ferry_port && pt.has_same_theater_stack) {
				continue;
			}
		}
		bool const is_friendly_port_ferry = use_naval && bool(friendly_ferry_port);

		auto const& target_sec = get_front_sector(target_p, pt.theater_id, tmap.safe_theater_of(target_p));
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

		// Enforce Strategic War Stance + Frontline Sector Mode rules
		ai_front_helpers::war_stance stance = target_sec.stance;
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
				if(!is_wg && !pt.is_rebel_or_liberation && !is_friendly_port_ferry)
					continue;
			} else {
				// Strategic defense (both WWI & Austerlitz):
				// ALWAYS allow liberating own/allied tiles, seizing UNDEFENDED enemy border tiles (capped so a 0-army side front doesn't drain our main front!), or smashing unentrenched overextensions!
				bool free_border_grab = !has_direct_defenders && pt.adjacent_to_friendly;
				bool unentrenched_counter = has_direct_defenders && pt.enemy_avg_dig < 1.0f;
				if(!pt.is_rebel_or_liberation && !free_border_grab && !unentrenched_counter)
					continue;
				if(free_border_grab && !is_wg && !pt.is_rebel_or_liberation && defensive_empty_grabs >= 2)
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
		// Quick early reject for suicidal HOSTILE beach landings when transport fleet cannot even carry enough troops to survive nearby enemy reaction armies!
		// (Friendly-port ferries dock safely in port with 0 penalty and can safely build up across multiple waves!)
		if(use_naval && !is_friendly_port_ferry && float(free_tcap) < (direct_def_regs + naval_threat_regs) * 0.75f && (direct_def_regs + naval_threat_regs) >= 4.0f) {
			continue;
		}

		// Find candidate unassigned stacks passing dynamic pinning (free 2nd-line strike reserves first!)
		struct cand {
			size_t idx;
			float dist_km;
			bool is_core;
			bool is_offensive_core;
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
				if(!pt.adjacent_to_friendly && pt.stack_already_here && rs.loc != target_p)
					continue; // Never march distant stacks through hostile rear to join a stack already standing on target_p!
			} else {
				// Naval invasion requires a safe coastal port in the stack's SAFE friendly theater (never embark across hostile land!) and stack fitting in available transports
				bool has_embark_port = (rs.safe_theater_id != 0)
					? tmap.safe_theater_has_port(rs.safe_theater_id)
					: tmap.theater_has_safe_port(rs.theater_id);
				if(!has_embark_port)
					continue;
				if(rs.regs > free_tcap)
					continue;
			}

			auto const& stack_sec = get_stack_sector(rs);
			bool allow_screen_adv = use_land
				&& province::provinces_are_adjacent(state, rs.loc, target_p)
				&& (is_wg || !has_direct_defenders || stance == ai_front_helpers::war_stance::active_offense);
			if(is_at_war && !pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, stack_sec.continuous_front, 0, 0, allow_screen_adv))
				continue;

			float d_km = province::direct_distance_km(state, rs.loc, target_p);
			// Avoid pulling distant armies from across the continent for minor land skirmishes
			if(is_at_war && use_land && target_sec.continuous_front && d_km > 700.0f && cands.size() >= 8)
				continue;

			float other_enemy = is_at_war
				? ai_front_helpers::adjacent_enemy_regs_excluding(state, n, rs.loc, target_p)
				: 0.0f;
			bool adj_to_target = use_land && province::provinces_are_adjacent(state, rs.loc, target_p);
			// CRITICAL (both WWI AND Austerlitz!): NEVER pull a frontline stack facing a real adjacent enemy army (>= 2 regs = 6K)
			// off its border to march away to a distant or empty/weaker secondary front (e.g. abandoning Przemysl vs 68K Russians to walk into empty Turkey!)
			if(is_at_war && use_land && other_enemy >= 2.0f) {
				bool leaving_for_weaker_or_distant = (!adj_to_target)
					|| (!has_direct_defenders && other_enemy >= 3.0f)
					|| (other_enemy > std::max(3.0f, direct_def_regs + nearby_enemy_regs * 0.5f) * 1.25f);
				if(leaving_for_weaker_or_distant
					&& !pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, true, 0, 0, false)) {
					continue;
				}
			}

			// Do NOT drain reserves backing an active major front (e.g. Galicia vs 250K Russians) 500+ km across the empire
			// just to carpet-siege an empty 0-army secondary front (e.g. empty Turkey)!
			if(is_at_war && use_land && !has_direct_defenders && !pt.is_rebel_or_liberation) {
				float max_empty_km = (stance == ai_front_helpers::war_stance::defensive)
					? (is_wg ? 450.0f : 350.0f)
					: (is_wg ? 800.0f : 550.0f);
				if(d_km > max_empty_km)
					continue;
				float backing_threat = front_backing_threat(rs.loc);
				if(stance == ai_front_helpers::war_stance::defensive && d_km > 200.0f && backing_threat >= 4.0f)
					continue;
				if(d_km > 350.0f && backing_threat >= 6.0f)
					continue;
			}

			cands.push_back(cand{ s, d_km, rs.is_core, rs.is_offensive_core, other_enemy <= 0.1f });
		}
		if(cands.empty())
			continue;

		bool const need_artillery_punch = has_direct_defenders || (use_naval && naval_threat_regs >= 4.0f);
		std::sort(cands.begin(), cands.end(), [&](cand const& a, cand const& b) {
			if(a.is_free_reserve != b.is_free_reserve)
				return a.is_free_reserve > b.is_free_reserve;
			if(use_naval && need_artillery_punch && a.is_core != b.is_core)
				return a.is_core > b.is_core;
			// For defended battles, strongly lead with Offensive Strike Corps (backline + recon/siege, -400 km),
			// then regular backline cores (-260 km); for empty grabs prefer frontline stacks so strike corps stay free!
			auto core_bias = [&](cand const& c) -> float {
				if(need_artillery_punch) {
					return c.is_offensive_core ? -400.0f : (c.is_core ? -260.0f : 0.0f);
				}
				return c.is_offensive_core ? 180.0f : (c.is_core ? 120.0f : 0.0f);
			};
			float a_key = a.dist_km + core_bias(a);
			float b_key = b.dist_km + core_bias(b);
			if(a_key != b_key)
				return a_key < b_key;
			return ready_stacks[a.idx].id.index() < ready_stacks[b.idx].id.index();
		});

		// For land attacks, pick best safe supplied staging province adjacent to target_p (avoiding river crossings!)
		dcon::province_id staging_prov;
		uint16_t staging_safe_tid = 0;
		military::crossing_type best_crossing = military::crossing_type::sea;
		if(use_land) {
			if(pt.adjacent_to_friendly) {
				float best_stage_score = -1e9f;
				dcon::province_id lead_loc = ready_stacks[cands[0].idx].loc;
				for(auto padj : state.world.province_get_province_adjacency(target_p)) {
					if(province::is_adjacency_impassable(state, n, padj.id))
						continue;
					dcon::province_id other = padj.get_connected_provinces(0) == target_p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
					if(other && other.index() < land_count && province::has_safe_access_to_province(state, n, other)) {
						uint16_t o_stid = tmap.safe_theater_of(other);
						if(o_stid != 0) {
							// Require at least one candidate stack in the SAME safe friendly landmass so armies never walk across hostile rear to stage!
							bool any_in_ostid = false;
							for(auto const& c : cands) {
								if(ready_stacks[c.idx].safe_theater_id == o_stid) {
									any_in_ostid = true;
									break;
								}
							}
							if(!any_in_ostid)
								continue;
						}
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
							staging_safe_tid = o_stid;
							best_crossing = ctype;
						}
					}
				}
			} else {
				best_crossing = military::crossing_type::none;
			}
			if(!staging_prov && !pt.stack_already_here)
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
			} else if(target_sec.continuous_front) {
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

		// For amphibious invasions & friendly-port ferries, NEVER lock to the closest tiny island garrison (e.g. 12K in Bermuda/Jamaica)!
		// Group embarkation origins by SAFE friendly theater (so waves never mix armies separated by enemy territory!).
		auto naval_origin_key = [](ready_stack const& rs) -> uint32_t {
			return rs.safe_theater_id != 0 ? (0x10000u | uint32_t(rs.safe_theater_id)) : uint32_t(rs.theater_id);
		};
		std::vector<uint32_t> naval_origin_theaters;
		if(use_naval) {
			for(auto const& c : cands) {
				uint32_t okey = naval_origin_key(ready_stacks[c.idx]);
				if(okey != 0 && std::find(naval_origin_theaters.begin(), naval_origin_theaters.end(), okey) == naval_origin_theaters.end()) {
					naval_origin_theaters.push_back(okey);
				}
			}
			if(is_friendly_port_ferry && naval_origin_theaters.size() > 1) {
				auto score_origin = [&](uint32_t okey) -> float {
					int32_t sum_r = 0;
					float min_d = 1e9f;
					for(auto const& c : cands) {
						auto const& rs = ready_stacks[c.idx];
						if(naval_origin_key(rs) == okey) {
							sum_r += rs.regs;
							if(c.dist_km < min_d) min_d = c.dist_km;
						}
					}
					return float(std::min(sum_r, free_tcap)) / (1.0f + min_d / 6500.0f);
				};
				std::sort(naval_origin_theaters.begin(), naval_origin_theaters.end(), [&](uint32_t a, uint32_t b) {
					float sa = score_origin(a);
					float sb = score_origin(b);
					if(sa != sb) return sa > sb;
					return a < b;
				});
			}
		} else {
			naval_origin_theaters.push_back(0);
		}

		auto target_ctrl_n = state.world.province_get_nation_from_province_control(target_p);
		auto target_owner_n = state.world.province_get_nation_from_province_ownership(target_p);
		dcon::nation_id target_def_nat = target_ctrl_n ? target_ctrl_n : target_owner_n;
		int32_t target_nat_total_regs = target_def_nat ? int32_t(state.world.nation_get_active_regiments(target_def_nat)) : 0;

		for(uint32_t orig_key : naval_origin_theaters) {
			chosen_attackers.clear();
			chosen_indices.clear();
			chosen_regs_sum = 0;
			chosen_has_core = false;
			odds_sufficient = false;

			if(!has_direct_defenders || is_friendly_port_ferry) {
				// Empty tile (0 direct defenders) OR safe friendly-port ferry (0-day dock unload, 0 sea-landing penalty!):
				float const reg_w = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
				int32_t stage_sl_regs = staging_prov
					? std::max(8, int32_t(float(military::supply_limit_in_province(state, n, staging_prov)) / reg_w))
					: 12;
				float needed_cover = 0.0f;
				float ideal_naval_lift = 0.0f;
				if(is_friendly_port_ferry) {
					float desired_wave = std::max(10.0f, float(std::min(36, std::max(target_nat_total_regs, int32_t(std::ceil(direct_def_regs + naval_threat_regs))))));
					needed_cover = std::min(float(free_tcap), 2.0f);
					ideal_naval_lift = std::min(float(free_tcap), desired_wave);
				} else if(use_naval) {
					if(naval_threat_regs >= 4.0f) {
						needed_cover = std::max(10.0f, naval_threat_regs * 1.05f);
						ideal_naval_lift = std::min(float(free_tcap), std::max(needed_cover * 1.35f, float(std::min(36, target_nat_total_regs))));
					} else if(target_nat_total_regs >= 8) {
						needed_cover = std::min(float(free_tcap), std::min(12.0f, float(target_nat_total_regs) * 0.50f));
						ideal_naval_lift = std::min(float(free_tcap), std::max(needed_cover * 1.35f, float(std::min(34, target_nat_total_regs))));
					} else {
						needed_cover = std::max(2.0f, naval_threat_regs * 1.25f);
						ideal_naval_lift = needed_cover;
					}
				} else {
					needed_cover = std::min(nearby_enemy_regs * (stance == ai_front_helpers::war_stance::defensive ? 0.65f : 0.45f), float(stage_sl_regs));
					ideal_naval_lift = needed_cover;
				}
				size_t max_empty_stacks = use_naval ? 12 : 3;

				if(!use_naval && needed_cover <= 1.0f) {
					for(auto const& c : cands) {
						auto const& rs = ready_stacks[c.idx];
						if(staging_safe_tid != 0 && rs.safe_theater_id != 0 && rs.safe_theater_id != staging_safe_tid)
							continue;
						chosen_attackers.push_back(rs.id);
						chosen_indices.push_back(c.idx);
						odds_sufficient = true;
						break;
					}
				} else {
					for(auto const& c : cands) {
						if(chosen_attackers.size() >= max_empty_stacks)
							break;
						auto const& rs = ready_stacks[c.idx];
						if(use_naval) {
							if(naval_origin_key(rs) != orig_key || chosen_regs_sum + rs.regs > free_tcap)
								continue;
						} else if(staging_safe_tid != 0 && rs.safe_theater_id != 0 && rs.safe_theater_id != staging_safe_tid) {
							continue;
						}
						if(is_at_war) {
							auto const& stack_sec = get_stack_sector(rs);
							if(stack_sec.continuous_front) {
								int32_t extra_regs = 0;
								int32_t extra_stacks = 0;
								int32_t extra_cores = 0;
								for(auto prev_idx : chosen_indices) {
									if(ready_stacks[prev_idx].loc == rs.loc) {
										extra_regs += ready_stacks[prev_idx].regs;
										extra_stacks += 1;
										if(ready_stacks[prev_idx].is_core) extra_cores += 1;
									}
								}
								bool allow_screen_adv = use_land && province::provinces_are_adjacent(state, rs.loc, target_p);
								if(!pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, true, extra_regs, extra_stacks, allow_screen_adv, extra_cores))
									continue;
							}
						}
						chosen_attackers.push_back(rs.id);
						chosen_indices.push_back(c.idx);
						chosen_regs_sum += rs.regs;
						if(rs.is_core) {
							chosen_has_core = true;
						}
						bool naval_quality_ok = !use_naval || is_friendly_port_ferry || needed_cover < 8.0f || chosen_has_core || float(chosen_regs_sum) >= needed_cover * 1.20f;
						if(float(chosen_regs_sum) >= needed_cover && naval_quality_ok) {
							odds_sufficient = true;
							if(!use_naval || float(chosen_regs_sum) >= ideal_naval_lift) {
								break;
							}
						}
					}
				}
			} else {
				// Directly defended province: evaluate analytically against DIRECT defenders, with realistic discount for neighboring/reaction enemy reinforcements
				float reinf_divisor = 1.0f + (use_naval ? 0.55f : 0.35f) * (nearby_enemy_regs / std::max(4.0f, direct_def_regs));
				size_t max_def_stacks = use_naval ? 12 : 8;
				for(auto const& c : cands) {
					if(chosen_attackers.size() >= max_def_stacks)
						break;
					auto const& rs = ready_stacks[c.idx];
					if(use_naval) {
						if(naval_origin_key(rs) != orig_key)
							continue;
						if(chosen_regs_sum + rs.regs > free_tcap)
							continue;
					} else if(staging_safe_tid != 0 && rs.safe_theater_id != 0 && rs.safe_theater_id != staging_safe_tid) {
						continue; // Never count stacks separated by enemy territory in land battle odds!
					}
					// Dynamic check against stacks already tentatively picked from the same province
					if(is_at_war) {
						auto const& stack_sec = get_stack_sector(rs);
						if(stack_sec.continuous_front) {
							int32_t extra_regs = 0;
							int32_t extra_stacks = 0;
							int32_t extra_cores = 0;
							for(auto prev_idx : chosen_indices) {
								if(ready_stacks[prev_idx].loc == rs.loc) {
									extra_regs += ready_stacks[prev_idx].regs;
									extra_stacks += 1;
									if(ready_stacks[prev_idx].is_core) extra_cores += 1;
								}
							}
							bool allow_screen_adv = use_land
								&& province::provinces_are_adjacent(state, rs.loc, target_p)
								&& (is_wg || stance == ai_front_helpers::war_stance::active_offense);
							if(!pin_tracker.can_pull(state, n, rs.loc, rs.regs, rs.dig_in, rs.is_core, target_p, true, extra_regs, extra_stacks, allow_screen_adv, extra_cores))
								continue;
						}
					}

					chosen_attackers.push_back(rs.id);
					chosen_indices.push_back(c.idx);
					chosen_regs_sum += rs.regs;

					auto eval = ai_battle_eval::evaluate_battle_outcome(state, chosen_attackers, direct_defenders, target_p, best_crossing);
					float eff_win = eval.win_ratio / reinf_divisor;
					bool decisive_overmatch = (stance != ai_front_helpers::war_stance::defensive) && (eff_win >= 2.0f);
					bool naval_survives_reaction = !use_naval || naval_threat_regs < 4.0f || float(chosen_regs_sum) >= (direct_def_regs + naval_threat_regs) * 0.90f;
					if(eff_win >= req_win && (eval.casualty_ratio <= max_cas || decisive_overmatch) && naval_survives_reaction) {
						odds_sufficient = true;
						break;
					}
				}
			}

			if(odds_sufficient)
				break;
		}

		// If THIS sector is too strong, skip to the next sector (NEVER return!)
		if(!odds_sufficient)
			continue;

		bool issued_any = false;
		if(use_naval) {
			dcon::province_id naval_dest = is_friendly_port_ferry ? friendly_ferry_port : target_p;
			for(auto idx : chosen_indices) {
				auto& st = ready_stacks[idx];
				state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
				state.world.army_set_ai_province(st.id, naval_dest);
				state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attack_transport));
				st.assigned = true;
				pin_tracker.commit_pull(st.loc, st.regs, st.is_core);
				issued_any = true;
			}
			if(issued_any) {
				active_naval_invasion = true;
			}
		} else {
			// Strict tile-by-tile frontline advance: NEVER route land stacks across enemy/rebel-occupied rear!
			bool all_already_in_target = !chosen_indices.empty();
			bool all_already_adj = !chosen_indices.empty();
			for(auto idx : chosen_indices) {
				auto loc_i = ready_stacks[idx].loc;
				if(loc_i != target_p) {
					all_already_in_target = false;
				}
				auto adj_i = state.world.get_province_adjacency_by_province_pair(loc_i, target_p);
				if(!adj_i || province::is_adjacency_impassable(state, n, adj_i)) {
					all_already_adj = false;
				}
			}
			if(all_already_in_target) {
				for(auto idx : chosen_indices) {
					auto& st = ready_stacks[idx];
					state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
					state.world.army_set_ai_province(st.id, target_p);
					state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attack_gathered));
					st.assigned = true;
					pin_tracker.commit_pull(st.loc, st.regs, st.is_core);
					issued_any = true;
				}
			} else if(all_already_adj) {
				for(auto idx : chosen_indices) {
					auto& st = ready_stacks[idx];
					if(auto direct_path = ai_front_helpers::make_front_safe_land_path(state, st.loc, target_p, n, st.id, true); !direct_path.empty()) {
						state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
						military::set_army_path(state, st.id, direct_path, n);
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attack_gathered));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs, st.is_core);
						issued_any = true;
					}
				}
			} else if(staging_prov) {
				for(auto idx : chosen_indices) {
					auto& st = ready_stacks[idx];
					if(st.loc == staging_prov) {
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attacking));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs, st.is_core);
						issued_any = true;
					} else if(auto stage_path = ai_front_helpers::make_front_safe_land_path(state, st.loc, staging_prov, n, st.id, false); !stage_path.empty()) {
						state.world.army_set_special_order(st.id, uint8_t(military::special_army_order::none));
						military::set_army_path(state, st.id, stage_path, n);
						state.world.army_set_ai_province(st.id, target_p);
						state.world.army_set_ai_activity(st.id, uint8_t(army_activity::attacking));
						st.assigned = true;
						pin_tracker.commit_pull(st.loc, st.regs, st.is_core);
						issued_any = true;
					}
				}
			}
		}

		if(issued_any && is_at_war && use_land && !has_direct_defenders && !is_wg && !pt.is_rebel_or_liberation && stance == ai_front_helpers::war_stance::defensive) {
			++defensive_empty_grabs;
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

// Sequential deterministic execution (100% MP OOS-safe — zero cross-nation data races!)
void make_defense(sys::state& state) {
	for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
		dcon::nation_id n{ dcon::nation_id::value_base_t(i) };
		if(state.world.nation_is_valid(n) && state.world.nation_get_owned_province_count(n) > 0) {
			distribute_guards(state, n);
		}
	}
}

void move_gathered_attackers_impl(sys::state& state, bool only_at_war) {
	std::vector<dcon::army_id> require_transport;

	auto target_still_hostile = [&](dcon::province_id target_p, dcon::nation_id ctrl) -> bool {
		if(!target_p || province::is_sea(state, target_p))
			return false;
		auto pctrl = state.world.province_get_nation_from_province_control(target_p);
		if(pctrl && military::are_at_war(state, ctrl, pctrl))
			return true;
		if(!pctrl && state.world.province_get_rebel_faction_from_province_rebel_control(target_p))
			return true;
		return ai_front_helpers::province_has_enemy_army(state, ctrl, target_p);
	};

	auto process_army = [&](dcon::army_fat_id ar) {
		if(!ar.get_controller_from_army_control() || !unit_on_ai_control(state, ar))
			return;

		if(ar.get_ai_activity() == uint8_t(army_activity::attack_transport)) {
			if(!ar.get_arrival_time()
				&& !ar.get_battle_from_army_battle_participation()
				&& !ar.get_navy_from_army_transport()
				&& std::find(require_transport.begin(), require_transport.end(), ar.id) == require_transport.end()) {

				auto ctrl_n = ar.get_controller_from_army_control();
				auto dest_p = ar.get_ai_province().id;
				// Release leftover attack_transport stacks cleanly if war ended or destination became unreachable!
				if(ctrl_n.get_is_at_war() && dest_p && province::has_access_to_province(state, ctrl_n, dest_p)) {
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
					if(target_p && province::has_access_to_province(state, ctrl, target_p) && target_still_hostile(target_p, ctrl)) {
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
								auto oact = army_activity(oa.get_ai_activity());
								if(oact != army_activity::attacking && oact != army_activity::attack_gathered)
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
							bool valid_path = false;
							if(loc && province::is_sea(state, loc)) {
								valid_path = military::move_army_ai(state, ar, target_p, ctrl);
							} else if(auto fpath = ai_front_helpers::make_front_safe_land_path(state, loc, target_p, ctrl, ar.id, true); !fpath.empty()) {
								state.world.army_set_special_order(ar.id, uint8_t(military::special_army_order::none));
								valid_path = military::set_army_path(state, ar.id, fpath, ctrl);
							}
							if(!valid_path) {
								ar.set_ai_activity(uint8_t(army_activity::on_guard));
								ar.set_ai_province(dcon::province_id{});
							}
						}
					} else {
						ar.set_ai_activity(uint8_t(army_activity::on_guard));
						ar.set_ai_province(dcon::province_id{});
					}
				} else if(target_p && ar.get_path().size() >= 2 && !ar.get_black_flag()
					&& loc && !province::is_sea(state, loc) && province::has_safe_access_to_province(state, ctrl, loc)) {
					// If an intermediate tile along our march fell to the enemy, halt before marching deep into hostile rear!
					auto cur_path = ar.get_path();
					bool crosses_hostile = false;
					for(uint32_t k = 1; k < cur_path.size(); ++k) {
						auto step_p = cur_path.at(k);
						if(!step_p || province::is_sea(state, step_p) || !province::has_safe_access_to_province(state, ctrl, step_p)) {
							crosses_hostile = true;
							break;
						}
					}
					if(crosses_hostile) {
						military::stop_army_movement(state, ar);
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
							auto oact = army_activity(oa.get_ai_activity());
							if(oact != army_activity::attacking && oact != army_activity::attack_gathered)
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
			&& ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_is_retreating()
			&& !ar.get_black_flag()) {
			// While marching to staging_prov, abort immediately if a tile along the staging path fell to enemy occupation!
			auto ctrl = ar.get_controller_from_army_control().id;
			auto loc = ar.get_location_from_army_location().id;
			if(loc && !province::is_sea(state, loc) && province::has_safe_access_to_province(state, ctrl, loc)) {
				auto cur_path = ar.get_path();
				bool crosses_hostile = false;
				for(uint32_t k = 0; k < cur_path.size(); ++k) {
					auto step_p = cur_path.at(k);
					if(!step_p || province::is_sea(state, step_p) || !province::has_safe_access_to_province(state, ctrl, step_p)) {
						crosses_hostile = true;
						break;
					}
				}
				if(crosses_hostile) {
					military::stop_army_movement(state, ar);
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					ar.set_ai_province(dcon::province_id{});
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
				if(target_p && province::has_access_to_province(state, ctrl, target_p) && target_still_hostile(target_p, ctrl.id)) {
					// Pre-attack re-verification: check whether enemy reinforced target_p while we gathered!
					std::vector<dcon::army_id> gathered_armies;
					for(auto o : ar.get_location_from_army_location().get_army_location()) {
						if(o.get_army().get_controller_from_army_control() == ctrl
							&& o.get_army().get_ai_activity() == uint8_t(army_activity::attacking)
							&& o.get_army().get_ai_province() == target_p
							&& o.get_army().get_path().size() == 0) {
							gathered_armies.push_back(o.get_army().id);
						}
					}
					std::vector<dcon::army_id> def_armies;
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
						for(auto ga : gathered_armies) {
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::on_guard));
							state.world.army_set_ai_province(ga, dcon::province_id{});
						}
					} else if(target_p == loc) {
						for(auto ga : gathered_armies) {
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::attack_gathered));
						}
					} else if(auto path = ai_front_helpers::make_front_safe_land_path(state, loc, target_p, ctrl.id, ar.id, true); path.size() > 0) {
						for(auto ga : gathered_armies) {
							state.world.army_set_special_order(ga, uint8_t(military::special_army_order::none));
							military::set_army_path(state, ga, path, ctrl);
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::attack_gathered));
						}
					} else if(ctrl.get_is_at_war()
						&& state.world.province_get_is_coast(target_p)
						&& province::make_land_unit_path(state, loc, target_p, ctrl, ar).empty()) {
						for(auto ga : gathered_armies) {
							require_transport.push_back(ga);
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::attack_transport));
						}
					} else {
						// Front shifted while gathering (target now lies behind enemy-occupied tiles): cancel cleanly so AI re-targets the current 1-tile frontline!
						for(auto ga : gathered_armies) {
							state.world.army_set_ai_activity(ga, uint8_t(army_activity::on_guard));
							state.world.army_set_ai_province(ga, dcon::province_id{});
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
		auto army_i = require_transport[i];
		auto coastal_target_prov = state.world.army_get_location_from_army_location(army_i);
		auto controller = state.world.army_get_controller_from_army_control(army_i);
		dcon::province_id dest_prov = state.world.army_get_ai_province(army_i).id;
		auto regs = state.world.army_get_army_membership(army_i);
		int32_t iregs = int32_t(regs.end() - regs.begin());

		// First: check if controller ALREADY has ANY fleet in `boarding` mode (even if briefly in battle, never launch a 2nd competing boarding fleet!)
		dcon::navy_id existing_boarding;
		bool boarding_in_battle = false;
		for(auto v : state.world.nation_get_navy_control(controller)) {
			auto nv = v.get_navy();
			if(unit_on_ai_control(state, nv)
				&& nv.get_ai_activity() == uint8_t(fleet_activity::boarding)) {
				existing_boarding = nv.id;
				boarding_in_battle = bool(nv.get_battle_from_navy_battle_participation());
				break;
			}
		}
		if(existing_boarding) {
			if(boarding_in_battle) {
				state.world.army_set_ai_activity(army_i, uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(army_i, dcon::province_id{});
				continue;
			}
			int32_t reserved_unboarded = 0;
			dcon::province_id boarding_dest;
			for(auto a : state.world.nation_get_army_control(controller)) {
				auto oa = a.get_army();
				if(oa.get_navy_from_army_transport() == existing_boarding) {
					if(!boarding_dest) boarding_dest = oa.get_ai_province().id;
				} else if(oa.get_ai_activity() == uint8_t(army_activity::transport_attack) || oa.get_ai_activity() == uint8_t(army_activity::transport_guard)) {
					auto oregs = oa.get_army_membership();
					reserved_unboarded += int32_t(oregs.end() - oregs.begin());
					if(!boarding_dest) boarding_dest = oa.get_ai_province().id;
				}
			}
			int32_t rem_tcap = military::free_transport_capacity(state, existing_boarding) - reserved_unboarded;
			bool same_expedition = !boarding_dest || boarding_dest == dest_prov
				|| (state.world.province_get_connected_region_id(boarding_dest) == state.world.province_get_connected_region_id(dest_prov))
				|| province::direct_distance_km(state, boarding_dest, dest_prov) <= 1500.0f;
			if(same_expedition && rem_tcap >= iregs) {
				auto bpath = state.world.navy_get_path(existing_boarding);
				dcon::province_id bport = bpath.size() > 0 ? bpath[0] : state.world.navy_get_location_from_navy_location(existing_boarding);
				if(bport && province::is_sea(state, bport)) {
					dcon::province_id coast_p;
					for(auto padj : state.world.province_get_province_adjacency(bport)) {
						auto other = padj.get_connected_provinces(0) == bport ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
						if(other && !province::is_sea(state, other) && state.world.province_get_port_to(other) == bport && province::has_safe_access_to_province(state, controller, other)) {
							coast_p = other;
							break;
						}
					}
					bport = coast_p;
				}
				if(bport && (coastal_target_prov == bport || military::move_army_ai(state, army_i, bport, controller))) {
					state.world.army_set_ai_activity(army_i, uint8_t(army_activity::transport_attack));
					continue;
				}
			}
			// Cannot join existing boarding fleet (wrong island or full): cleanly release back to on_guard so fleet doesn't wait forever
			state.world.army_set_ai_activity(army_i, uint8_t(army_activity::on_guard));
			state.world.army_set_ai_province(army_i, dcon::province_id{});
			continue;
		}

		dcon::navy_id transport_fleet = find_transport_fleet(state, controller);
		int32_t raw_tcap = transport_fleet ? military::transport_capacity(state, transport_fleet) : 0;

		// No transport fleet or no naval supremacy at war: release waiting invasion stacks for this controller
		if(!transport_fleet || raw_tcap <= 0 || (state.world.nation_get_is_at_war(controller) && !naval_advantage(state, controller))) {
			for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
					state.world.army_set_ai_activity(require_transport[j], uint8_t(army_activity::on_guard));
					state.world.army_set_ai_province(require_transport[j], dcon::province_id{}); // stop rechecking these units
					require_transport[j] = require_transport.back();
					require_transport.pop_back();
				}
			}
			state.world.army_set_ai_activity(army_i, uint8_t(army_activity::on_guard));
			state.world.army_set_ai_province(army_i, dcon::province_id{}); // stop rechecking these units
			continue;
		}
		if(iregs > raw_tcap) {
			state.world.army_set_ai_activity(army_i, uint8_t(army_activity::on_guard));
			state.world.army_set_ai_province(army_i, dcon::province_id{});
			continue;
		}
		int32_t tcap = raw_tcap - iregs;

		// Prefer a coastal embarkation port where one of the assigned invasion stacks is ALREADY standing!
		if(!state.world.province_get_is_coast(coastal_target_prov)) {
			dcon::province_id better_coast;
			for(uint32_t j = i + 1; j < uint32_t(require_transport.size()); ++j) {
				if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller
					&& state.world.army_get_ai_province(require_transport[j]) == dest_prov) {
					auto jloc = state.world.army_get_location_from_army_location(require_transport[j]);
					if(jloc && state.world.province_get_is_coast(jloc) && military::move_army_ai(state, army_i, jloc, controller)) {
						better_coast = jloc;
						break;
					}
				}
			}
			if(better_coast) {
				coastal_target_prov = better_coast;
			} else {
				auto path = province::make_path_to_nearest_coast(state, controller, coastal_target_prov);
				if(path.empty()) {
					state.world.army_set_ai_activity(army_i, uint8_t(army_activity::on_guard));
					state.world.army_set_ai_province(army_i, dcon::province_id{});
					continue; // army could not reach coast
				} else {
					coastal_target_prov = path.front();
					military::set_army_path(state, army_i, path, controller);
				}
			}
		}

		{
			dcon::province_id fleet_destination = province::has_naval_access_to_province(state, controller, coastal_target_prov) ? coastal_target_prov : state.world.province_get_port_to(coastal_target_prov).id;
			if(fleet_destination && fleet_destination == state.world.navy_get_location_from_navy_location(transport_fleet)) {
				military::stop_navy_movement(state, transport_fleet);
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
			} else if(!fleet_destination || !military::move_navy_ai(state, transport_fleet, fleet_destination)) {
				military::stop_army_movement(state, army_i);
				state.world.army_set_ai_activity(army_i, uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(army_i, dcon::province_id{});
				continue;
			} else {
				state.world.navy_set_ai_activity(transport_fleet, uint8_t(fleet_activity::boarding));
			}
		}

		state.world.army_set_ai_activity(army_i, uint8_t(army_activity::transport_attack));

		auto destination_region = state.world.province_get_connected_region_id(dest_prov);

		// Scoop up ALL other assigned stacks for this expedition onto the SAME boarding fleet!
		for(uint32_t j = uint32_t(require_transport.size()); j-- > i + 1;) {
			if(state.world.army_get_controller_from_army_control(require_transport[j]) == controller) {
				auto j_army = require_transport[j];
				dcon::province_id j_dest = state.world.army_get_ai_province(j_army).id;
				bool same_dest = (j_dest == dest_prov)
					|| (state.world.province_get_connected_region_id(j_dest) == destination_region)
					|| (j_dest && dest_prov && province::direct_distance_km(state, j_dest, dest_prov) <= 1500.0f);
				auto jregs = state.world.army_get_army_membership(j_army);
				int32_t jcount = int32_t(jregs.end() - jregs.begin());
				bool boarded = false;
				if(same_dest && tcap >= jcount) {
					if(state.world.army_get_location_from_army_location(j_army) == coastal_target_prov) {
						state.world.army_set_ai_activity(j_army, uint8_t(army_activity::transport_attack));
						tcap -= jcount;
						boarded = true;
					} else if(military::move_army_ai(state, j_army, coastal_target_prov, controller)) {
						state.world.army_set_ai_activity(j_army, uint8_t(army_activity::transport_attack));
						tcap -= jcount;
						boarded = true;
					}
				}
				if(!boarded) {
					// Cleanly release overflow/unreachable stacks back to on_guard so the boarding fleet doesn't stall waiting for them
					state.world.army_set_ai_activity(j_army, uint8_t(army_activity::on_guard));
					state.world.army_set_ai_province(j_army, dcon::province_id{});
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

struct nation_market_totals {
	std::vector<float> stockpile;
	std::vector<float> supply;
	std::vector<float> demand;
	bool has_market = false;

	void build(sys::state const& state, dcon::nation_id n) {
		size_t const csize = size_t(state.world.commodity_size());
		stockpile.assign(csize, 0.0f);
		supply.assign(csize, 0.0f);
		demand.assign(csize, 0.0f);
		has_market = false;

		auto states_ctrl = state.world.nation_get_state_control(n);
		if(states_ctrl.begin() != states_ctrl.end()) {
			for(auto sc : states_ctrl) {
				auto m = state.world.state_instance_get_market_from_local_market(sc.get_state());
				if(!m)
					continue;
				has_market = true;
				state.world.for_each_commodity([&](dcon::commodity_id cid) {
					size_t ci = size_t(cid.index());
					if(ci < csize) {
						stockpile[ci] += state.world.market_get_government_stockpile(m, cid);
						supply[ci] += state.world.market_get_aggregated_supply_history(m, cid);
						demand[ci] += state.world.market_get_aggregated_demand_history(m, cid);
					}
				});
			}
		} else {
			dcon::province_id cap = state.world.nation_get_capital(n).id;
			if(cap) {
				auto fm = state.world.state_instance_get_market_from_local_market(state.world.province_get_state_membership(cap));
				if(fm) {
					has_market = true;
					state.world.for_each_commodity([&](dcon::commodity_id cid) {
						size_t ci = size_t(cid.index());
						if(ci < csize) {
							stockpile[ci] = state.world.market_get_government_stockpile(fm, cid);
							supply[ci] = state.world.market_get_aggregated_supply_history(fm, cid);
							demand[ci] = state.world.market_get_aggregated_demand_history(fm, cid);
						}
					});
				}
			}
		}
	}
};

inline bool unit_type_has_build_shortage(sys::state& state, dcon::nation_id n, dcon::unit_type_id type, nation_market_totals const& mkt) {
	if(!type) {
		return true;
	}
	if(!mkt.has_market) {
		return false;
	}
	auto& def = state.military_definitions.unit_base_definitions[type];
	float build_time = std::max(1.0f, float(def.build_time));
	bool is_civ = state.world.nation_get_is_civilized(n);

	for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
		if(def.build_cost.commodity_type[i]) {
			auto cid = def.build_cost.commodity_type[i];
			auto amount = def.build_cost.commodity_amounts[i];
			if(amount <= 0.0f) {
				continue;
			}
			size_t ci = size_t(cid.index());
			if(ci >= mkt.stockpile.size()) {
				continue;
			}
			float total_stockpile = mkt.stockpile[ci];
			float total_supply = mkt.supply[ci];
			float total_demand = mkt.demand[ci];

			if(total_stockpile >= amount * 0.25f) {
				continue;
			}
			float target_demand = total_demand + (amount / build_time);
			if(total_supply > 0.0f && target_demand > 0.0f && (total_supply / target_demand) >= 0.05f) {
				continue;
			}
			// For start-of-game goods (artillery, small arms, liquor, wine, explosives, etc.), trade routes only begin importing
			// once local demand appears: do not falsely block unit types when a civilized nation simply has ~0 prior demand.
			if(is_civ && state.world.commodity_get_is_available_from_start(cid) && total_demand <= 0.05f) {
				continue;
			}
			return true;
		} else {
			break;
		}
	}
	return false;
}

inline sys::unit_variable_stats get_effective_unit_stats(sys::state const& state, dcon::nation_id n, dcon::unit_type_id uid) {
	if(!uid)
		return sys::unit_variable_stats{};
	auto const& bdef = state.military_definitions.unit_base_definitions[uid];
	if(n) {
		auto stats = state.world.nation_get_unit_stats(n, uid);
		if(stats.discipline_or_evasion > 0.0f)
			return stats;
	}
	return static_cast<sys::unit_variable_stats const&>(bdef);
}

inline bool is_backline_unit(sys::state const& state, dcon::nation_id n, dcon::unit_type_id uid) {
	if(!uid)
		return false;
	// Pure battle-engine rule (military.cpp:7656): ANY land unit with support > 0 deals damage from back_line!
	// Zero hardcoding of unit_type::support/special so it works seamlessly in Medieval/Modern/Fantasy mods too.
	return get_effective_unit_stats(state, n, uid).support > 0.0f;
}

struct unit_cost_profile {
	float daily_upkeep = 0.0f;
	float build_money = 0.0f;
};

inline unit_cost_profile estimate_unit_cost_profile(sys::state const& state, dcon::nation_id n, dcon::unit_type_id uid) {
	if(!uid)
		return unit_cost_profile{ 1e6f, 1e9f };
	auto const& bdef = state.military_definitions.unit_base_definitions[uid];
	auto set_cost = [&](economy::commodity_set const& cs) -> float {
		float total = 0.0f;
		for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
			auto cid = cs.commodity_type[i];
			if(!cid)
				break;
			float price = state.world.commodity_get_median_price(cid);
			if(price <= 0.01f) {
				price = state.world.commodity_get_cost(cid);
			}
			total += cs.commodity_amounts[i] * std::max(0.1f, price);
		}
		return total;
	};
	float nat_sup_mod = n ? state.world.nation_get_modifier_values(n, sys::national_mod_offsets::supply_consumption) : 0.0f;
	float unit_sup_mod = get_effective_unit_stats(state, n, uid).supply_consumption;
	float supply_mult = std::max(0.01f, nat_sup_mod + unit_sup_mod);
	return unit_cost_profile{ set_cost(bdef.supply_cost) * supply_mult, set_cost(bdef.build_cost) };
}

inline float estimate_unit_economic_cost(sys::state const& state, dcon::nation_id n, dcon::unit_type_id uid) {
	auto cp = estimate_unit_cost_profile(state, n, uid);
	// 1x build cost + 180 days of daily supply upkeep (naturally avoids overpriced luxury elites like Guards for mass frontline!)
	return std::max(1.0f, cp.build_money + cp.daily_upkeep * 180.0f);
}

// Computes how many regiments of unit type `uid` nation `n` can ACTUALLY afford & supply on its real economy + national markets!
// Prevents both failure modes:
//   1. Never chokes mass army on unaffordable/scarce super-units (e.g. expensive Tanks with high support/siege),
//   2. Lets rich industrial powers actually field expensive high-stat units up to their real budget & factory supply cap!
inline int32_t estimate_sustainable_regiment_cap(
	sys::state& state,
	dcon::nation_id n,
	dcon::unit_type_id uid,
	float effective_daily_income,
	float treasury,
	float budget_share,
	nation_market_totals const& mkt
) {
	if(!uid)
		return 0;
	auto const& bdef = state.military_definitions.unit_base_definitions[uid];
	auto cp = estimate_unit_cost_profile(state, n, uid);
	// Daily budget allocated to this role (plus amortized treasury cushion)
	float daily_budget = std::max(0.5f, effective_daily_income * budget_share + std::max(0.0f, treasury) / 365.0f * 0.20f);
	// Sustainment cost: daily upkeep + build cost amortized over 2 years (730 days)
	float amortized_daily_cost = std::max(0.05f, cp.daily_upkeep + cp.build_money / 730.0f);
	int32_t budget_cap = std::clamp(int32_t(daily_budget / amortized_daily_cost), 0, 10000);
	if(budget_cap <= 0 || !mkt.has_market)
		return budget_cap;

	// Market goods throughput limit across pre-aggregated controlled state markets of nation `n` (O(1) per commodity!)
	float nat_sup_mod = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::supply_consumption);
	float unit_sup_mod = get_effective_unit_stats(state, n, uid).supply_consumption;
	float supply_mult = std::max(0.01f, nat_sup_mod + unit_sup_mod);
	float build_amort_days = std::max(120.0f, float(bdef.build_time) * 3.0f);
	bool is_civ = state.world.nation_get_is_civilized(n);

	auto check_commodity_cap = [&](dcon::commodity_id cid, float build_amt, float supply_amt) -> int32_t {
		if(!cid || (build_amt <= 0.0f && supply_amt <= 0.0f))
			return 10000;
		size_t ci = size_t(cid.index());
		if(ci >= mkt.stockpile.size())
			return 10000;
		float total_stockpile = mkt.stockpile[ci];
		float total_supply = mkt.supply[ci];
		float total_demand = mkt.demand[ci];

		// Start-of-game goods with ~0 prior demand in a civilized nation: trade routes import once demand begins
		if(is_civ && state.world.commodity_get_is_available_from_start(cid) && total_demand <= 0.05f) {
			return 10000;
		}
		float daily_need = supply_amt * supply_mult + build_amt / build_amort_days;
		if(daily_need <= 0.0001f)
			return 10000;
		float avail_daily = total_supply * 0.70f + total_stockpile / 90.0f;
		if(avail_daily <= 0.0f)
			return 0;
		return std::clamp(int32_t(avail_daily / daily_need), 0, 10000);
	};

	int32_t goods_cap = 10000;
	for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
		auto cid = bdef.build_cost.commodity_type[i];
		if(!cid)
			break;
		float sup_amt = 0.0f;
		for(uint32_t j = 0; j < economy::commodity_set::set_size; ++j) {
			if(bdef.supply_cost.commodity_type[j] == cid) {
				sup_amt = bdef.supply_cost.commodity_amounts[j];
				break;
			}
		}
		goods_cap = std::min(goods_cap, check_commodity_cap(cid, bdef.build_cost.commodity_amounts[i], sup_amt));
		if(goods_cap <= 0)
			return 0;
	}
	for(uint32_t j = 0; j < economy::commodity_set::set_size; ++j) {
		auto cid = bdef.supply_cost.commodity_type[j];
		if(!cid)
			break;
		bool already_checked = false;
		for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
			if(bdef.build_cost.commodity_type[i] == cid) {
				already_checked = true;
				break;
			}
		}
		if(!already_checked) {
			goods_cap = std::min(goods_cap, check_commodity_cap(cid, 0.0f, bdef.supply_cost.commodity_amounts[j]));
			if(goods_cap <= 0)
				return 0;
		}
	}
	return std::min(budget_cap, goods_cap);
}

inline float evaluate_frontline_power(sys::unit_variable_stats const& d) {
	// Directly mirrors military.cpp:7267,7302: frontline holds the line (defense & org durability weighted above raw attack)
	float off_dmg = std::max(0.0f, d.attack_or_gun_power) * 0.1f + 1.0f;
	float def_dmg = std::max(0.0f, d.defence_or_hull) * 0.1f + 1.0f;
	float durability = std::sqrt(std::max(0.1f, d.discipline_or_evasion) * std::max(5.0f, float(d.default_organisation)) / 30.0f);
	return (0.40f * off_dmg + 0.60f * def_dmg) * durability;
}

inline float evaluate_backline_power(sys::unit_variable_stats const& d) {
	if(d.support <= 0.0f)
		return 0.0f;
	// Directly mirrors military.cpp:7660: backline takes 0 damage while covered, pure (dmg * 0.1 + 1) * support
	float off_dmg = std::max(0.0f, d.attack_or_gun_power) * 0.1f + 1.0f;
	float def_dmg = std::max(0.0f, d.defence_or_hull) * 0.1f + 1.0f;
	return (0.50f * off_dmg + 0.50f * def_dmg) * d.support;
}

struct role_unit_pair {
	dcon::unit_type_id primary;  // Best combat unit affordable for at least 1 regiment (e.g. Tanks/Airplanes when affordable!)
	dcon::unit_type_id fallback; // Most affordable reliable backup once `primary` hits its real budget/supply cap (e.g. Engineers/Hussars!)
};

inline dcon::unit_type_id get_best_frontline_unit(sys::state& state, dcon::nation_id n, bool primary_culture, int32_t min_sustainable, std::vector<int32_t> const& cap_by_type, nation_market_totals const& mkt) {
	// Stat + cost-efficiency + real affordability driven across ALL unlocked land units (zero unit_type:: hardcoding!)
	auto pick_pass = [&](bool require_zero_support, int32_t min_cap, bool check_shortage) -> dcon::unit_type_id {
		dcon::unit_type_id best;
		float best_score = -1.0f;
		for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
			dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
			auto const& bdef = state.military_definitions.unit_base_definitions[uid];
			if(!bdef.is_land)
				continue;
			if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
				continue;
			if(bdef.primary_culture && !primary_culture)
				continue;
			auto d = get_effective_unit_stats(state, n, uid);
			if(require_zero_support && d.support > 0.0f)
				continue;
			if(min_cap > 0 && (i >= cap_by_type.size() || cap_by_type[i] < min_cap))
				continue;
			if(check_shortage && unit_type_has_build_shortage(state, n, uid, mkt))
				continue;
			float power = evaluate_frontline_power(d);
			float cost = estimate_unit_economic_cost(state, n, uid);
			float score = power / std::pow(cost, 0.12f);
			if(!best || score > best_score) {
				best = uid;
				best_score = score;
			}
		}
		return best;
	};
	if(auto u = pick_pass(true, min_sustainable, true)) return u;
	if(auto u = pick_pass(true, 1, true)) return u;
	if(auto u = pick_pass(true, 0, false)) return u;
	if(auto u = pick_pass(false, 1, true)) return u;
	return pick_pass(false, 0, false);
}

inline dcon::unit_type_id get_best_backline_unit(sys::state& state, dcon::nation_id n, bool primary_culture, int32_t min_sustainable, std::vector<int32_t> const& cap_by_type, nation_market_totals const& mkt) {
	// Mass backline unit: must be sustainable for the main line (so expensive Tanks don't paralyze mass Artillery!)
	auto pick_pass = [&](int32_t min_cap) -> dcon::unit_type_id {
		dcon::unit_type_id best;
		float best_score = -1.0f;
		for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
			dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
			auto const& bdef = state.military_definitions.unit_base_definitions[uid];
			if(!bdef.is_land)
				continue;
			if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
				continue;
			if(bdef.primary_culture && !primary_culture)
				continue;
			auto d = get_effective_unit_stats(state, n, uid);
			if(d.support <= 0.0f)
				continue;
			if(min_cap > 0 && (i >= cap_by_type.size() || cap_by_type[i] < min_cap))
				continue;
			if(unit_type_has_build_shortage(state, n, uid, mkt))
				continue;
			float power = evaluate_backline_power(d);
			float cost = estimate_unit_economic_cost(state, n, uid);
			float score = power / std::pow(cost, 0.08f);
			if(!best || score > best_score) {
				best = uid;
				best_score = score;
			}
		}
		return best;
	};
	if(auto u = pick_pass(min_sustainable)) return u;
	return pick_pass(1);
}

inline dcon::unit_type_id get_best_elite_backline_unit(sys::state& state, dcon::nation_id n, bool primary_culture, dcon::unit_type_id mass_back_uid, std::vector<int32_t> const& cap_by_type, nation_market_totals const& mkt) {
	// Highest raw backline firepower unit (e.g. expensive Tanks/Heavy Siege with highest support in a mod!)
	// Built strictly up to its real sustainable budget/supply cap inside Strike Corps — NEVER blocking mass Artillery!
	float mass_power = mass_back_uid ? evaluate_backline_power(get_effective_unit_stats(state, n, mass_back_uid)) : 0.0f;
	dcon::unit_type_id best;
	float best_power = mass_power * 1.15f;
	for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
		dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
		if(uid == mass_back_uid)
			continue;
		auto const& bdef = state.military_definitions.unit_base_definitions[uid];
		if(!bdef.is_land)
			continue;
		if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
			continue;
		if(bdef.primary_culture && !primary_culture)
			continue;
		if(i >= cap_by_type.size() || cap_by_type[i] < 1)
			continue;
		auto d = get_effective_unit_stats(state, n, uid);
		if(d.support <= 0.0f)
			continue;
		if(unit_type_has_build_shortage(state, n, uid, mkt))
			continue;
		float power = evaluate_backline_power(d);
		if(power > best_power) {
			best = uid;
			best_power = power;
		}
	}
	return best;
}

inline role_unit_pair get_best_recon_units(sys::state& state, dcon::nation_id n, bool primary_culture, int32_t target_count, std::vector<int32_t> const& cap_by_type, nation_market_totals const& mkt) {
	// Primary: highest raw recon+combat unit sustainable for >= 1 reg (e.g. Airplanes);
	// Fallback: best recon unit sustainable for the full target_count (e.g. Hussars/Dragoons)!
	role_unit_pair res{};
	float best_pri = -1.0f;
	float best_fb = -1.0f;
	int32_t fb_need = std::max(1, target_count);

	for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
		dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
		auto const& bdef = state.military_definitions.unit_base_definitions[uid];
		if(!bdef.is_land)
			continue;
		if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
			continue;
		if(bdef.primary_culture && !primary_culture)
			continue;
		auto d = get_effective_unit_stats(state, n, uid);
		if(d.reconnaissance_or_fire_range <= 0.0f)
			continue;
		int32_t cap = (i < cap_by_type.size()) ? cap_by_type[i] : 0;
		if(cap < 1)
			continue;
		if(unit_type_has_build_shortage(state, n, uid, mkt))
			continue;
		float combat = (d.support > 0.0f) ? evaluate_backline_power(d) : evaluate_frontline_power(d);
		float raw_score = d.reconnaissance_or_fire_range * 10.0f + combat * 2.0f + d.maneuver * 0.4f;
		if(!res.primary || raw_score > best_pri) {
			res.primary = uid;
			best_pri = raw_score;
		}
		float cost = estimate_unit_economic_cost(state, n, uid);
		float fb_score = (raw_score / std::pow(cost, 0.12f)) * (cap >= fb_need ? 2.0f : 1.0f);
		if(!res.fallback || fb_score > best_fb) {
			res.fallback = uid;
			best_fb = fb_score;
		}
	}
	return res;
}

inline role_unit_pair get_best_siege_units(sys::state& state, dcon::nation_id n, bool primary_culture, int32_t target_count, std::vector<int32_t> const& cap_by_type, nation_market_totals const& mkt) {
	// Primary: highest raw siege+combat unit sustainable for >= 1 reg (e.g. Tanks!);
	// Fallback: best siege unit sustainable for the full target_count (e.g. Engineers!) when Tanks hit budget/barrel cap!
	role_unit_pair res{};
	float best_pri = -1.0f;
	float best_fb = -1.0f;
	int32_t fb_need = std::max(1, target_count);

	for(uint32_t i = 2; i < state.military_definitions.unit_base_definitions.size(); ++i) {
		dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
		auto const& bdef = state.military_definitions.unit_base_definitions[uid];
		if(!bdef.is_land)
			continue;
		if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
			continue;
		if(bdef.primary_culture && !primary_culture)
			continue;
		auto d = get_effective_unit_stats(state, n, uid);
		if(d.siege_or_torpedo_attack <= 0.0f)
			continue;
		int32_t cap = (i < cap_by_type.size()) ? cap_by_type[i] : 0;
		if(cap < 1)
			continue;
		if(unit_type_has_build_shortage(state, n, uid, mkt))
			continue;
		float combat = (d.support > 0.0f) ? evaluate_backline_power(d) : evaluate_frontline_power(d);
		float raw_score = d.siege_or_torpedo_attack * 10.0f + combat * 2.0f;
		if(!res.primary || raw_score > best_pri) {
			res.primary = uid;
			best_pri = raw_score;
		}
		float cost = estimate_unit_economic_cost(state, n, uid);
		float fb_score = (raw_score / std::pow(cost, 0.12f)) * (cap >= fb_need ? 2.0f : 1.0f);
		if(!res.fallback || fb_score > best_fb) {
			res.fallback = uid;
			best_fb = fb_score;
		}
	}
	return res;
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
		// Tie wartime artillery cap strictly to ACTIVE frontlines (not empty 0-army side borders like 14 empty Turkish tiles!)
		std::vector<dcon::nation_id> war_allies;
		std::vector<dcon::nation_id> war_enemies;
		ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
		std::vector<std::pair<dcon::province_id, float>> enemy_locs;
		float total_enemy_eff = 0.0f;
		for(auto e_nat : war_enemies) {
			min_enemy_cw = std::min(min_enemy_cw, estimate_nation_base_combat_width(state, e_nat));
			for(auto ar : state.world.nation_get_army_control(e_nat)) {
				auto army = ar.get_army();
				if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport())
					continue;
				auto loc = army.get_location_from_army_location().id;
				if(!loc || province::is_sea(state, loc))
					continue;
				float eff = 0.0f;
				for(auto r : army.get_army_membership()) {
					eff += r.get_regiment().get_strength() * (0.5f + 0.5f * r.get_regiment().get_org());
				}
				if(eff >= 0.5f) {
					total_enemy_eff += eff;
					bool merged = false;
					for(auto& kv : enemy_locs) {
						if(kv.first == loc) {
							kv.second += eff;
							merged = true;
							break;
						}
					}
					if(!merged) {
						enemy_locs.push_back({ loc, eff });
					}
				}
			}
		}
		float const min_sector_eff = std::max(0.5f, std::min(6.0f, total_enemy_eff * 0.12f));
		auto has_active_enemy_near = [&](dcon::province_id bp) -> bool {
			if(enemy_locs.empty())
				return true;
			float near_eff = 0.0f;
			for(auto const& [eloc, eeff] : enemy_locs) {
				if(province::direct_distance_km(state, bp, eloc) <= 240.0f) {
					near_eff += eeff;
					if(near_eff >= min_sector_eff)
						return true;
				}
			}
			return false;
		};

		std::vector<dcon::province_id> fallback_border_provs;
		for(auto pc : state.world.nation_get_province_control(n)) {
			auto p = pc.get_province().id;
			bool direct_enemy = ai_front_helpers::province_has_enemy_army(state, n, p);
			bool borders_enemy = direct_enemy;
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
				if(province::is_sea(state, other))
					continue;
				if(ai_front_helpers::province_has_enemy_army(state, n, other)) {
					direct_enemy = true;
					borders_enemy = true;
				}
				auto octrl = state.world.province_get_nation_from_province_control(other);
				if(octrl && military::are_at_war(state, n, octrl)) {
					borders_enemy = true;
					min_enemy_cw = std::min(min_enemy_cw, estimate_nation_base_combat_width(state, octrl));
				}
			}
			if(borders_enemy) {
				fallback_border_provs.push_back(p);
				if(direct_enemy || has_active_enemy_near(p)) {
					front_provs.push_back(p);
				}
			}
		}
		if(front_provs.empty()) {
			front_provs = std::move(fallback_border_provs);
		}
		// Expeditionary war ally fallback (e.g. UK fighting in allied France/Canada with 0 own controlled frontline provinces!)
		if(front_provs.empty()) {
			for(auto a_nat : war_allies) {
				if(a_nat == n)
					continue;
				for(auto pc : state.world.nation_get_province_control(a_nat)) {
					auto p = pc.get_province().id;
					bool direct_enemy = ai_front_helpers::province_has_enemy_army(state, n, p);
					bool borders_enemy = direct_enemy;
					for(auto padj : state.world.province_get_province_adjacency(p)) {
						if(province::is_adjacency_impassable(state, n, padj.id))
							continue;
						auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1).id : padj.get_connected_provinces(0).id;
						if(province::is_sea(state, other))
							continue;
						if(ai_front_helpers::province_has_enemy_army(state, n, other)) {
							direct_enemy = true;
							borders_enemy = true;
						}
						auto octrl = state.world.province_get_nation_from_province_control(other);
						if(octrl && military::are_at_war(state, n, octrl)) {
							borders_enemy = true;
						}
					}
					if(borders_enemy && (direct_enemy || has_active_enemy_near(p))) {
						front_provs.push_back(p);
					}
				}
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

	// Budget artillery for BOTH full Line 1 defense across the active front (1.0x front_cw_sum)
	// AND dedicated 2nd-line offensive breakthrough strike corps (+max(2*nation_cw, 0.50*front_cw_sum + nation_cw)).
	// Still capped at 45% of standing regular army size so early-game armies field full balanced corps on L1 + L2,
	// while late-game 1-3M armies on narrow CW=10 / mountain=3 fronts put 75-85%+ manpower into infantry!
	int32_t offensive_strike_reserve = std::max(nation_cw * 2, int32_t(std::ceil(float(front_cw_sum) * 0.50f)) + nation_cw);
	int32_t raw_cap = front_cw_sum + offensive_strike_reserve;
	int32_t max_fraction_cap = int32_t(float(desired_army) * 0.45f);
	int32_t min_era_floor = std::min(nation_cw * 2, max_fraction_cap);
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
		if(!uid)
			continue;
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
		int32_t total_regular_existing = 0;
		int32_t unfull_regular = 0;
		int32_t total_building = 0;

		size_t const unit_type_count = state.military_definitions.unit_base_definitions.size();
		std::vector<int32_t> count_by_type(unit_type_count, 0);

		auto adjust_unit_counts = [&](dcon::unit_type_id uid, int32_t delta) {
			if(!uid)
				return;
			size_t idx = size_t(uid.index());
			if(idx < count_by_type.size()) {
				count_by_type[idx] += delta;
			}
			auto d = ai_detail::get_effective_unit_stats(state, n, uid);
			if(d.support > 0.0f) {
				num_backline += delta;
			} else {
				num_frontline += delta;
			}
			if(d.reconnaissance_or_fire_range > 0.0f) {
				num_recon += delta;
			}
			if(d.siege_or_torpedo_attack > 0.0f) {
				num_siege += delta;
			}
		};

		// Pass 1: Count ONLY permanent regular regiments & ongoing constructions for army composition and recruitment caps
		// (temporary mobilized brigades disappear after war and must NOT distort permanent artillery/infantry ratios or block recruiting regulars!)
		for(auto ar : state.world.nation_get_army_control(n)) {
			for(auto r : ar.get_army().get_army_membership()) {
				auto reg = r.get_regiment();
				auto pop = reg.get_pop_from_regiment_source();
				bool is_regular = pop && (pop.get_poptype() == state.culture_definitions.soldiers);
				if(is_regular) {
					++total_regular_existing;
					adjust_unit_counts(reg.get_type(), 1);
					if(reg.get_strength() < 0.80f) {
						++unfull_regular;
					}
				}
			}
		}

		state.world.nation_for_each_province_land_construction(n, [&](dcon::province_land_construction_id plcid) {
			auto uid = state.world.province_land_construction_get_type(plcid);
			++total_building;
			adjust_unit_counts(uid, 1);
		});

		// Compute composition targets & real economic + market-supply affordability caps per unit type!
		int32_t desired_army = int32_t(calculate_desired_army_size(state, n));
		int32_t composition_army_basis = std::max(desired_army, total_regular_existing);
		int32_t nation_cw = ai_detail::estimate_nation_base_combat_width(state, n);
		int32_t supply_cap_regs = ai_detail::typical_supply_regiment_cap(state, n);

		// O(1) cached daily tax income (NEVER call economy::estimate_daily_income_ai here — it scans ALL world trade routes x ALL commodities per nation!)
		float approx_tax_eff = std::clamp(0.45f * (1.0f + nations::tax_efficiency(state, n.id)), 0.20f, 1.0f);
		float daily_income = (
			n.get_total_poor_income() * (float(n.get_poor_tax()) / 100.0f)
			+ n.get_total_middle_income() * (float(n.get_middle_tax()) / 100.0f)
			+ n.get_total_rich_income() * (float(n.get_rich_tax()) / 100.0f)
		) * approx_tax_eff;
		float treasury = n.get_treasury();
		// Floor against existing army baseline so temporary wartime income dips don't zero-out standard infantry/artillery
		float effective_daily_income = std::max({ daily_income, treasury / 180.0f, float(std::max(1, total_regular_existing)) * 1.5f });

		// Pre-aggregate controlled state markets ONCE per nation (O(1) per commodity across all unit checks!)
		ai_detail::nation_market_totals mkt;
		mkt.build(state, n.id);

		std::vector<int32_t> cap_mass_front(unit_type_count, 0);
		std::vector<int32_t> cap_mass_back(unit_type_count, 0);
		std::vector<int32_t> cap_strike_special(unit_type_count, 0);
		for(uint32_t i = 2; i < unit_type_count; ++i) {
			dcon::unit_type_id uid{ dcon::unit_type_id::value_base_t(i) };
			auto const& bdef = state.military_definitions.unit_base_definitions[uid];
			if(!bdef.is_land)
				continue;
			if(!state.world.nation_get_active_unit(n, uid) && !bdef.active)
				continue;
			cap_mass_front[i] = ai_detail::estimate_sustainable_regiment_cap(state, n.id, uid, effective_daily_income, treasury, 0.65f, mkt);
			cap_mass_back[i] = ai_detail::estimate_sustainable_regiment_cap(state, n.id, uid, effective_daily_income, treasury, 0.50f, mkt);
			cap_strike_special[i] = ai_detail::estimate_sustainable_regiment_cap(state, n.id, uid, effective_daily_income, treasury, 0.25f, mkt);
		}

		// Estimate tentative backline & offensive strike needs before picking capacity-matched units
		int32_t raw_art_cap = ai_detail::calculate_nation_artillery_cap(state, n, composition_army_basis);
		int32_t min_front_cap = std::max(4, composition_army_basis / 2);
		int32_t min_back_cap = std::max(3, raw_art_cap / 2);

		// Pure stat + cost + affordability role discovery across ALL unlocked land units (100% mod-agnostic!)
		auto inf_type = ai_detail::get_best_frontline_unit(state, n, false, min_front_cap, cap_mass_front, mkt);
		auto pc_inf_type = ai_detail::get_best_frontline_unit(state, n, true, min_front_cap, cap_mass_front, mkt);
		auto art_type = ai_detail::get_best_backline_unit(state, n, false, min_back_cap, cap_mass_back, mkt);
		auto pc_art_type = ai_detail::get_best_backline_unit(state, n, true, min_back_cap, cap_mass_back, mkt);
		auto elite_back_type = ai_detail::get_best_elite_backline_unit(state, n, false, art_type, cap_strike_special, mkt);
		auto pc_elite_back_type = ai_detail::get_best_elite_backline_unit(state, n, true, pc_art_type ? pc_art_type : art_type, cap_strike_special, mkt);

		bool has_backline_units = bool(art_type || pc_art_type || elite_back_type || pc_elite_back_type);
		int32_t art_cap = has_backline_units ? raw_art_cap : 0;
		int32_t core_backline_target = has_backline_units ? std::clamp(std::min(nation_cw <= 14 ? nation_cw : 5, std::max(6, supply_cap_regs) / 2), 3, 10) : 0;
		int32_t estimated_core_stacks = (has_backline_units && core_backline_target > 0)
			? std::max(1, (art_cap + core_backline_target - 1) / core_backline_target)
			: std::max(1, composition_army_basis / 10);

		// In Project Alice (military.cpp:7216,7256,7342), recon & siege ONLY work when ATTACKING (0 benefit on defense!).
		// Therefore size recon & siege strictly for dedicated OFFENSIVE strike corps (~1/3 in narrow-CW WWI, ~1/2 in maneuver era),
		// leaving defensive Line 1 holding cores as pure cost-effective Frontline + Backline!
		int32_t target_offensive_cores = (nation_cw <= 14)
			? std::max(1, (estimated_core_stacks + 1) / 3)
			: std::max(1, (estimated_core_stacks + 1) / 2);
		int32_t strike_regs_per_stack = std::clamp(core_backline_target > 0 ? (core_backline_target * 2 + 2) : 10, 8, std::max(8, supply_cap_regs));
		int32_t offensive_reg_basis = std::min(composition_army_basis, target_offensive_cores * strike_regs_per_stack);
		int32_t est_recon_need = std::clamp(int32_t(std::round(float(offensive_reg_basis) * state.defines.recon_unit_ratio)), composition_army_basis >= 6 ? 1 : 0, std::max(1, target_offensive_cores * 2));
		int32_t est_siege_need = (composition_army_basis >= 8)
			? std::clamp(int32_t(std::round(float(offensive_reg_basis) * state.defines.engineer_unit_ratio)), 1, std::max(1, target_offensive_cores * 2))
			: 0;

		auto recon_pair = ai_detail::get_best_recon_units(state, n, false, est_recon_need, cap_strike_special, mkt);
		auto pc_recon_pair = ai_detail::get_best_recon_units(state, n, true, est_recon_need, cap_strike_special, mkt);
		auto siege_pair = ai_detail::get_best_siege_units(state, n, false, est_siege_need, cap_strike_special, mkt);
		auto pc_siege_pair = ai_detail::get_best_siege_units(state, n, true, est_siege_need, cap_strike_special, mkt);

		int32_t target_recon = (recon_pair.primary || pc_recon_pair.primary || recon_pair.fallback || pc_recon_pair.fallback) ? est_recon_need : 0;
		int32_t target_siege = (siege_pair.primary || pc_siege_pair.primary || siege_pair.fallback || pc_siege_pair.fallback) ? est_siege_need : 0;

		// Capacity-aware role pickers: pick the strongest unit (e.g. Tanks/Airplanes/Elite Support) ONLY while below its
		// real sustainable budget & goods cap, then seamlessly fall back to the affordable backup (Engineers/Hussars/Artillery)!
		auto pick_affordable_from_pair = [&](ai_detail::role_unit_pair const& pair) -> dcon::unit_type_id {
			if(pair.primary) {
				size_t idx = size_t(pair.primary.index());
				if(idx < unit_type_count && count_by_type[idx] < cap_strike_special[idx]) {
					return pair.primary;
				}
			}
			if(pair.fallback) {
				size_t idx = size_t(pair.fallback.index());
				if(idx < unit_type_count && count_by_type[idx] < std::max(cap_strike_special[idx], cap_mass_back[idx])) {
					return pair.fallback;
				}
			}
			return dcon::unit_type_id{};
		};

		auto pick_affordable_elite_back = [&](bool pc, bool is_strike_stack) -> dcon::unit_type_id {
			if(!is_strike_stack)
				return dcon::unit_type_id{};
			auto uid = (pc && pc_elite_back_type) ? pc_elite_back_type : elite_back_type;
			if(!uid)
				return dcon::unit_type_id{};
			size_t idx = size_t(uid.index());
			int32_t elite_quota = std::min(idx < unit_type_count ? cap_strike_special[idx] : 0, target_offensive_cores * std::max(1, core_backline_target));
			if(idx < unit_type_count && count_by_type[idx] < elite_quota) {
				return uid;
			}
			return dcon::unit_type_id{};
		};

		// Pass 2: Safe, paced, stat-driven cross-type regiment conversion via military::upgrade_regiment!
		// Zero hardcoding of unit names or unit_type:: buckets — works in Vanilla, Medieval, Modern, or any custom mod.
		bool at_war = n.get_is_at_war();
		auto econ_mode = evaluate_war_economy_mode(state, n.id);
		if(total_regular_existing > 0 && econ_mode != war_economy_mode::prewar_ready) {
			int32_t max_unfull = at_war
				? std::max(1, int32_t(float(total_regular_existing) * 0.08f))
				: std::max(2, int32_t(std::ceil(float(total_regular_existing) * 0.15f)));
			int32_t max_conversions_this_tick = at_war
				? std::clamp(total_regular_existing / 15, 1, 4)
				: std::clamp(total_regular_existing / 8, 2, 12);

			if(unfull_regular < max_unfull) {
				struct conv_army {
					dcon::army_id id;
					ai_detail::stack_composition sc;
					int32_t priority_group = 3;
				};
				std::vector<conv_army> conv_armies;
				int32_t existing_offensive_cores = 0;

				for(auto a : state.world.nation_get_army_control(n)) {
					auto ar = a.get_army();
					auto sc_all = ai_detail::classify_army_composition(state, n.id, ar.id);
					if((sc_all.backline > 0 || !has_backline_units) && (sc_all.recon > 0 || sc_all.siege > 0)) {
						++existing_offensive_cores;
					}
					if(!unit_on_ai_control(state, ar)
						|| ar.get_battle_from_army_battle_participation()
						|| ar.get_navy_from_army_transport()
						|| ar.get_is_retreating()
						|| ar.get_black_flag()
						|| ar.get_arrival_time()
						|| army_activity(ar.get_ai_activity()) != army_activity::on_guard) {
						continue;
					}
					auto loc = ar.get_location_from_army_location().id;
					if(!loc || province::is_sea(state, loc) || state.world.province_get_nation_from_province_control(loc) != n.id) {
						continue;
					}
					// In war: strictly rear-only (>= 2 hops from hostiles). In peace: skip armies next to active rebels.
					if(ai_front_helpers::is_province_near_hostiles(state, n.id, loc, at_war ? 2 : 1)) {
						continue;
					}
					if(sc_all.total <= 0) {
						continue;
					}
					int32_t pgroup = 3;
					if(has_backline_units && sc_all.backline > 0 && sc_all.backline < core_backline_target) {
						pgroup = 0; // First top up existing partial backline cores
					} else if(sc_all.backline == 0 && sc_all.total >= 6) {
						pgroup = 1; // Next turn substantial frontline stacks into balanced cores in-place
					} else if(sc_all.backline > 0) {
						pgroup = 2; // Already at core_backline_target (still eligible for obsolete/offensive-role cleanup)
					}
					conv_armies.push_back(conv_army{ ar.id, sc_all, pgroup });
				}

				std::sort(conv_armies.begin(), conv_armies.end(), [](conv_army const& a, conv_army const& b) {
					if(a.priority_group != b.priority_group)
						return a.priority_group < b.priority_group;
					if(a.sc.total != b.sc.total)
						return a.sc.total > b.sc.total;
					return a.id.index() < b.id.index();
				});

				auto can_convert_to = [&](dcon::regiment_id reg_id, dcon::unit_type_id target_uid) -> bool {
					if(!target_uid)
						return false;
					return military::can_change_land_unit_type<command::actor::ai>(state, n.id, reg_id, target_uid);
				};

				auto adjust_stack_comp = [&](ai_detail::stack_composition& comp, dcon::unit_type_id uid, int32_t delta) {
					if(!uid)
						return;
					auto d = ai_detail::get_effective_unit_stats(state, n, uid);
					if(d.support > 0.0f) {
						comp.backline += delta;
					} else {
						comp.frontline += delta;
					}
					if(d.reconnaissance_or_fire_range > 0.0f) {
						comp.recon += delta;
					}
					if(d.siege_or_torpedo_attack > 0.0f) {
						comp.siege += delta;
					}
				};

				int32_t conversions_done = 0;
				for(auto& ca : conv_armies) {
					if(conversions_done >= max_conversions_this_tick || unfull_regular >= max_unfull)
						break;
					int32_t stack_conv_cap = at_war ? 2 : std::max(2, ca.sc.total / 2);
					int32_t stack_convs = 0;

					for(auto r : state.world.army_get_army_membership(ca.id)) {
						if(conversions_done >= max_conversions_this_tick || unfull_regular >= max_unfull || stack_convs >= stack_conv_cap)
							break;
						auto reg = r.get_regiment();
						auto pop = reg.get_pop_from_regiment_source();
						// NEVER convert temporary mobilized regiments or regiments still re-equipping
						if(!pop || pop.get_poptype() != state.culture_definitions.soldiers)
							continue;
						if(reg.get_strength() < 0.80f)
							continue;

						auto cur_type = reg.get_type();
						if(!cur_type)
							continue;
						bool pc = pop.get_is_primary_or_accepted_culture();
						bool stack_already_offensive = (ca.sc.recon > 0 || ca.sc.siege > 0);
						bool stack_wants_offensive_role = stack_already_offensive || (existing_offensive_cores < target_offensive_cores && ca.sc.total >= 6);

						auto pick_front = (pc && pc_inf_type) ? pc_inf_type : inf_type;
						auto pick_elite_back = pick_affordable_elite_back(pc, stack_already_offensive);
						auto pick_back = pick_elite_back ? pick_elite_back : ((pc && pc_art_type) ? pc_art_type : art_type);
						auto pick_rec = pick_affordable_from_pair(pc ? pc_recon_pair : recon_pair);
						auto pick_sg = pick_affordable_from_pair(pc ? pc_siege_pair : siege_pair);

						auto cur_d = ai_detail::get_effective_unit_stats(state, n, cur_type);
						bool cur_is_back = (cur_d.support > 0.0f);
						bool cur_has_recon = (cur_d.reconnaissance_or_fire_range > 0.0f);
						bool cur_has_siege = (cur_d.siege_or_torpedo_attack > 0.0f);
						float cur_front_pwr = ai_detail::evaluate_frontline_power(cur_d);
						float cur_back_pwr = ai_detail::evaluate_backline_power(cur_d);
						float best_front_pwr = pick_front ? ai_detail::evaluate_frontline_power(ai_detail::get_effective_unit_stats(state, n, pick_front)) : 0.0f;
						float best_back_pwr = pick_back ? ai_detail::evaluate_backline_power(ai_detail::get_effective_unit_stats(state, n, pick_back)) : 0.0f;

						// Converting 1 frontline regiment -> 1 backline regiment ALWAYS preserves >= +1 frontline buffer in-stack
						// (e.g. 10 regs -> up to 6 front / 4 back; 12 regs -> up to 7 front / 5 back) AND nation-wide!
						bool can_swap_front_to_back = (!cur_is_back)
							&& has_backline_units
							&& (ca.sc.frontline - 1 >= ca.sc.backline + 2)
							&& (ca.sc.backline < core_backline_target)
							&& (num_frontline - 1 >= num_backline + 2);

						// Purely stat-driven obsolescence check (catches Irregulars, Medieval Peasant Levies, obsolete militia,
						// while NEVER deleting strong shock/flanking units like Medieval Heavy Knights or early Cuirassiers!)
						float obsolete_ratio = (nation_cw >= 18 && cur_d.maneuver >= 2.0f) ? 0.70f : 0.85f;
						bool is_obsolete_front = (!cur_is_back) && (!cur_has_recon) && (!cur_has_siege)
							&& pick_front && (pick_front != cur_type)
							&& (cur_front_pwr < best_front_pwr * obsolete_ratio);

						// Excess offensive-only utility beyond Strike Corps needs (never blindly overwriting working Dragoons!)
						bool is_excess_recon = cur_has_recon && !cur_has_siege && (cur_type != pick_front) && (cur_type != pick_back)
							&& ((ca.sc.recon > 1 && num_recon > target_recon) || (num_recon > target_recon + 3));
						bool is_excess_siege = cur_has_siege && !cur_has_recon && (cur_type != pick_front) && (cur_type != pick_back)
							&& ((ca.sc.siege > 1 && num_siege > target_siege) || (num_siege > target_siege + 3));

						dcon::unit_type_id new_type;

						if(is_obsolete_front) {
							if(can_swap_front_to_back && stack_wants_offensive_role && num_siege < target_siege && ca.sc.siege == 0 && pick_sg && ai_detail::is_backline_unit(state, n, pick_sg) && num_backline < art_cap && can_convert_to(reg.id, pick_sg)) {
								new_type = pick_sg;
							} else if(can_swap_front_to_back && num_backline < art_cap && pick_back && can_convert_to(reg.id, pick_back)) {
								new_type = pick_back;
							} else if(stack_wants_offensive_role && num_recon < target_recon && ca.sc.recon == 0 && pick_rec && !ai_detail::is_backline_unit(state, n, pick_rec) && can_convert_to(reg.id, pick_rec)) {
								new_type = pick_rec;
							} else if(can_convert_to(reg.id, pick_front)) {
								new_type = pick_front;
							}
						} else if(is_excess_recon || is_excess_siege) {
							if(can_swap_front_to_back && stack_wants_offensive_role && num_siege < target_siege && ca.sc.siege == 0 && pick_sg && ai_detail::is_backline_unit(state, n, pick_sg) && num_backline < art_cap && can_convert_to(reg.id, pick_sg)) {
								new_type = pick_sg;
							} else if(can_swap_front_to_back && num_backline < art_cap && pick_back && can_convert_to(reg.id, pick_back)) {
								new_type = pick_back;
							} else if(cur_is_back && pick_back && pick_back != cur_type && can_convert_to(reg.id, pick_back)) {
								new_type = pick_back;
							} else if(!cur_is_back && pick_front && pick_front != cur_type && can_convert_to(reg.id, pick_front)) {
								new_type = pick_front;
							}
						} else if(!cur_is_back && !cur_has_recon && !cur_has_siege) {
							// Standard frontline regiment: convert surplus in-place into Strike Corps utility or Backline firepower
							if(stack_wants_offensive_role && num_recon < target_recon && ca.sc.recon == 0 && ca.sc.total >= 4 && pick_rec && !ai_detail::is_backline_unit(state, n, pick_rec) && can_convert_to(reg.id, pick_rec)) {
								new_type = pick_rec;
							} else if(stack_wants_offensive_role && num_recon < target_recon && ca.sc.recon == 0 && can_swap_front_to_back && num_backline < art_cap && pick_rec && ai_detail::is_backline_unit(state, n, pick_rec) && can_convert_to(reg.id, pick_rec)) {
								new_type = pick_rec;
							} else if(stack_wants_offensive_role && num_siege < target_siege && ca.sc.siege == 0 && pick_sg && can_convert_to(reg.id, pick_sg)
								&& (ai_detail::is_backline_unit(state, n, pick_sg) ? (can_swap_front_to_back && num_backline < art_cap) : (ca.sc.total >= 4))) {
								new_type = pick_sg;
							} else if(can_swap_front_to_back && num_backline < art_cap && pick_back && can_convert_to(reg.id, pick_back)) {
								new_type = pick_back;
							}
						} else if(cur_is_back) {
							// Backline unit: upgrade into affordable elite/stronger backline or strictly better siege/recon ONLY within sustainable cap!
							if(!cur_has_siege && !cur_has_recon && pick_back && pick_back != cur_type && best_back_pwr > cur_back_pwr * 1.25f && can_convert_to(reg.id, pick_back)) {
								new_type = pick_back;
							} else if(cur_has_siege && pick_sg && pick_sg != cur_type && can_convert_to(reg.id, pick_sg)) {
								auto new_sg_d = ai_detail::get_effective_unit_stats(state, n, pick_sg);
								if(new_sg_d.siege_or_torpedo_attack > cur_d.siege_or_torpedo_attack) {
									new_type = pick_sg;
								}
							} else if(cur_has_recon && pick_rec && pick_rec != cur_type && can_convert_to(reg.id, pick_rec)) {
								auto new_rec_d = ai_detail::get_effective_unit_stats(state, n, pick_rec);
								if(new_rec_d.reconnaissance_or_fire_range > cur_d.reconnaissance_or_fire_range) {
									new_type = pick_rec;
								}
							}
						} else if(cur_has_recon || cur_has_siege) {
							// Frontline recon/siege unit (e.g. Hussars -> Airplanes, or frontline Siege -> Tanks when unlocked & affordable!):
							// Preserve >= +2 frontline buffer, stack core_backline_target, and national art_cap whenever upgrading from frontline into a backline unit!
							bool safe_front_to_back_upgrade = (ca.sc.frontline - 1 >= ca.sc.backline + 2)
								&& (ca.sc.backline < core_backline_target)
								&& (num_frontline - 1 >= num_backline + 2)
								&& (num_backline < art_cap);
							if(cur_has_siege && pick_sg && pick_sg != cur_type && can_convert_to(reg.id, pick_sg)) {
								auto new_sg_d = ai_detail::get_effective_unit_stats(state, n, pick_sg);
								bool new_is_back = (new_sg_d.support > 0.0f);
								if(new_sg_d.siege_or_torpedo_attack > cur_d.siege_or_torpedo_attack
									&& (!new_is_back || safe_front_to_back_upgrade)) {
									new_type = pick_sg;
								}
							} else if(cur_has_recon && pick_rec && pick_rec != cur_type && can_convert_to(reg.id, pick_rec)) {
								auto new_rec_d = ai_detail::get_effective_unit_stats(state, n, pick_rec);
								bool new_is_back = (new_rec_d.support > 0.0f);
								if(new_rec_d.reconnaissance_or_fire_range > cur_d.reconnaissance_or_fire_range
									&& (!new_is_back || safe_front_to_back_upgrade)) {
									new_type = pick_rec;
								}
							}
						}

						if(new_type && new_type != cur_type) {
							bool was_off_core = (ca.sc.backline > 0 || !has_backline_units) && (ca.sc.recon > 0 || ca.sc.siege > 0);
							military::upgrade_regiment(state, reg.id, new_type);
							adjust_unit_counts(cur_type, -1);
							adjust_unit_counts(new_type, 1);
							adjust_stack_comp(ca.sc, cur_type, -1);
							adjust_stack_comp(ca.sc, new_type, 1);
							bool now_off_core = (ca.sc.backline > 0 || !has_backline_units) && (ca.sc.recon > 0 || ca.sc.siege > 0);
							if(!was_off_core && now_off_core) {
								++existing_offensive_cores;
							} else if(was_off_core && !now_off_core && existing_offensive_cores > 0) {
								--existing_offensive_cores;
							}
							++conversions_done;
							++stack_convs;
							++unfull_regular;
						}
					}
				}
			}
		}

		// Pass 3: Recruit NEW regular regiments if below desired_army (ignoring temporary mobilized brigades!)
		int32_t num_to_build_nation = desired_army - (total_regular_existing + total_building);
		if(num_to_build_nation <= 0)
			continue;

		// Cap concurrent queue size so the nation doesn't starve itself of goods in one giant spike
		int32_t max_concurrent_builds = std::max(8, desired_army / 3);
		int32_t allowed_new_orders = max_concurrent_builds - total_building;
		if(allowed_new_orders <= 0)
			continue;
		num_to_build_nation = std::min(num_to_build_nation, allowed_new_orders);

		auto try_build_from_pop = [&](dcon::province_id prov, dcon::pop_id pop_id, bool pc) -> bool {
			auto cult = state.world.pop_get_culture(pop_id);
			auto pick_inf = pc && pc_inf_type ? pc_inf_type : inf_type;
			auto pick_elite = pick_affordable_elite_back(pc, true);
			auto pick_art = pick_elite ? pick_elite : (pc && pc_art_type ? pc_art_type : art_type);
			auto pick_rec = pick_affordable_from_pair(pc ? pc_recon_pair : recon_pair);
			auto pick_sg = pick_affordable_from_pair(pc ? pc_siege_pair : siege_pair);

			auto attempt = [&](dcon::unit_type_id uid) -> bool {
				if(!uid)
					return false;
				if(!command::can_start_land_unit_construction<false>(state, n, prov, cult, uid))
					return false;
				command::execute_start_land_unit_construction(state, n, prov, cult, uid);
				adjust_unit_counts(uid, 1);
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

	// Cache per-nation base CW and typical front supply cap ONCE per nation (not once per army!)
	uint32_t const nsize = state.world.nation_size();
	std::vector<int32_t> cached_nation_cw(nsize, -1);
	std::vector<int32_t> cached_supply_cap(nsize, -1);
	auto get_nation_cw_and_cap = [&](dcon::nation_id nid) -> std::pair<int32_t, int32_t> {
		uint32_t idx = uint32_t(nid.index());
		if(idx < nsize) {
			if(cached_nation_cw[idx] < 0) {
				cached_nation_cw[idx] = ai_detail::estimate_nation_base_combat_width(state, nid);
				cached_supply_cap[idx] = ai_detail::typical_supply_regiment_cap(state, nid);
			}
			return { cached_nation_cw[idx], cached_supply_cap[idx] };
		}
		return { ai_detail::estimate_nation_base_combat_width(state, nid), ai_detail::typical_supply_regiment_cap(state, nid) };
	};

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

		auto [nation_cw, supply_cap_regs] = get_nation_cw_and_cap(controller.id);
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
				|| state.world.army_get_arrival_time(host)
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
				// NEVER push backline above frontline cover (prevents exposed artillery stacks!)
				if(hsc.backline + joining_count > hsc.frontline)
					return 0;
				if(host_is_core && hsc.backline + joining_count <= core_backline_target) {
					if(joining_is_siege && hsc.siege == 0) {
						// Cluster siege into Strike Corps (stacks that already have recon!) rather than defensive holding cores
						return (hsc.recon > 0) ? 5 : 4;
					}
					return 3;
				}
				if(!host_is_core && hsc.frontline >= 4 && joining_count <= core_backline_target) {
					return 2;
				}
				return 0;
			}
			if(joining_is_recon) {
				// Cluster recon into Strike Corps (core stacks that already have siege!) rather than diluting defensive holding cores
				if(host_is_core && hsc.recon < 2 && hsc.frontline + joining_count <= core_backline_target + 4) {
					if(hsc.recon == 0) {
						return (hsc.siege > 0) ? 5 : 4;
					}
					return 2;
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

		auto ar_act = army_activity(ar.get_ai_activity());
		if(ar.get_black_flag()) {
			// NEVER overwrite transport_guard on a black-flagged army waiting at a neutral coast for its evacuation fleet!
			// Also never try to land-merge a black-flagged army abroad — ensure it is on_guard so distribute_guards + move_idle_guards evacuate it home!
			if(ar_act != army_activity::transport_guard && ar_act != army_activity::on_guard) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
			}
		} else if(ar_act == army_activity::unspecified) {
			if(reg_count > 1) {
				ar.set_ai_activity(uint8_t(army_activity::on_guard));
			} else {
				auto reg = (*regs.begin()).get_regiment();
				auto type = reg.get_type();
				if(!type) {
					ar.set_ai_activity(uint8_t(army_activity::on_guard));
					continue;
				}
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
					} else {
						bool moved = false;
						if(controller.get_is_at_war()) {
							auto spath = province::make_safe_land_path(state, location.id, target_location, controller);
							if(!spath.empty()) {
								moved = military::set_army_path(state, ar.id, spath, controller);
							}
						} else {
							moved = military::move_army_ai(state, ar, target_location, controller);
						}
						if(moved) {
							ar.set_ai_province(target_location);
							ar.set_ai_activity(uint8_t(army_activity::merging));
						} else {
							ar.set_ai_activity(uint8_t(army_activity::on_guard));
						}
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
					if(!type)
						continue;
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

			// Split oversized stacks: either actively taking attrition above local supply limit, OR oversized hub blobs
			// (e.g. 36K-60K parked in London/Paris/New York) that are too big to fit onto realistic border front provinces!
			bool alone_over_local_sl = (reg_count > local_sl_regs);
			bool giant_hub_blob = (reg_count >= std::max(10, supply_cap_regs + 2));
			if(reg_count >= 8
				&& ((alone_over_local_sl && military::relative_attrition_amount(state, ar.id, location.id) >= 0.01f)
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
					// Balanced half split ensuring NEITHER parent NOR child ever ends up with backline > frontline!
					int32_t total_front = int32_t(front_regs.size());
					int32_t total_back = int32_t(back_regs.size());
					int32_t take_back = total_back / 2;
					int32_t rem_back = total_back - take_back;
					int32_t min_take_front = std::min(total_front, take_back);
					int32_t max_take_front = std::max(0, total_front - rem_back);
					int32_t take_front = std::max(1, split_count - take_back);
					if(min_take_front <= max_take_front) {
						take_front = std::clamp(take_front, min_take_front, max_take_front);
					} else {
						take_front = std::max(1, total_front / 2);
					}
					for(int32_t k = 0; k < take_back && k < total_back; ++k) {
						to_split.push_back(back_regs[size_t(k)]);
					}
					for(int32_t k = 0; k < take_front && k < total_front; ++k) {
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
			// Immediately mark the newly split child army as on_guard so distribute_guards spreads it out of attrition on the next tick (not 8 days later!)
			dcon::army_id new_child = state.world.regiment_get_army_from_army_membership(req.to_split[0]);
			if(new_child && new_child != req.army) {
				state.world.army_set_ai_activity(new_child, uint8_t(army_activity::on_guard));
				state.world.army_set_ai_province(new_child, dcon::province_id{});
			}
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
	bool prewar = (!at_war && evaluate_war_economy_mode(state, n) == war_economy_mode::prewar_ready);

	uint32_t const first_sea = uint32_t(state.province_definitions.first_sea_province.index());
	int32_t const land_count = int32_t(std::min(state.world.province_size(), first_sea));
	std::vector<uint8_t> front_cache(size_t(std::max(0, land_count)), uint8_t(0)); // 0=unknown, 1=false, 2=true

	auto is_province_frontline = [&](dcon::province_id p) -> bool {
		if(!p || province::is_sea(state, p))
			return false;
		int32_t pidx = p.index();
		if(pidx >= 0 && pidx < land_count && front_cache[size_t(pidx)] != 0) {
			return front_cache[size_t(pidx)] == 2;
		}
		bool res = false;
		auto ctrl = state.world.province_get_nation_from_province_control(p);
		if((ctrl && military::are_at_war(state, n, ctrl))
			|| (!ctrl && state.world.province_get_rebel_faction_from_province_rebel_control(p))) {
			res = true;
		}
		if(!res) {
			for(auto ar : state.world.province_get_army_location(p)) {
				auto army = ar.get_army();
				if(army.get_black_flag() || army.get_is_retreating() || army.get_navy_from_army_transport())
					continue;
				auto ac = army.get_controller_from_army_control();
				if(!ac || military::are_at_war(state, n, ac)) {
					res = true;
					break;
				}
			}
		}
		if(!res) {
			for(auto padj : state.world.province_get_province_adjacency(p)) {
				if(province::is_adjacency_impassable(state, n, padj.id))
					continue;
				auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
				if(province::is_sea(state, other))
					continue;
				auto octrl = state.world.province_get_nation_from_province_control(other);
				if((octrl && military::are_at_war(state, n, octrl))
					|| (!octrl && state.world.province_get_rebel_faction_from_province_rebel_control(other))) {
					res = true;
					break;
				}
				for(auto ar : state.world.province_get_army_location(other)) {
					auto army = ar.get_army();
					if(army.get_black_flag() || army.get_is_retreating() || army.get_navy_from_army_transport())
						continue;
					auto ac = army.get_controller_from_army_control();
					if(!ac || military::are_at_war(state, n, ac)) {
						res = true;
						break;
					}
				}
				if(res)
					break;
			}
		}
		if(pidx >= 0 && pidx < land_count) {
			front_cache[size_t(pidx)] = res ? uint8_t(2) : uint8_t(1);
		}
		return res;
	};

	auto is_province_near_front = [&](dcon::province_id p) -> bool {
		if(!p || province::is_sea(state, p))
			return false;
		for(auto padj : state.world.province_get_province_adjacency(p)) {
			if(province::is_adjacency_impassable(state, n, padj.id))
				continue;
			auto other = padj.get_connected_provinces(0) == p ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			if(!province::is_sea(state, other) && is_province_frontline(other))
				return true;
		}
		return false;
	};

	auto is_active_front_zone = [&](dcon::province_id p) -> bool {
		return is_province_frontline(p) || is_province_near_front(p);
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
		dcon::province_id ai_p = ar.get_ai_province().id;
		auto path = ar.get_path();
		dcon::province_id move_dest = (path.size() > 0) ? path.at(0) : dcon::province_id{};
		bool attacking_order = (act == army_activity::attacking
			|| act == army_activity::attack_gathered
			|| act == army_activity::transport_attack
			|| act == army_activity::attack_transport
			|| (at_war && (act == army_activity::transport_guard || bool(ar.get_navy_from_army_transport()))));

		military::unit_priority prio = military::unit_priority::normal_priority;
		if(ar.get_battle_from_army_battle_participation()) {
			prio = military::unit_priority::high_priority;
		} else if(at_war) {
			// Active front zone covers BOTH 1st line AND 2nd-line strike/reserve corps, plus armies marching/retreating on the front!
			if(attacking_order
				|| ar.get_is_retreating()
				|| is_active_front_zone(loc)
				|| (ai_p && is_active_front_zone(ai_p))
				|| (move_dest && is_active_front_zone(move_dest))) {
				prio = military::unit_priority::high_priority;
			} else if(avg_str < 0.95f || avg_org < 0.90f || move_dest || act == army_activity::merging) {
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
	float no_atk_score = no_atk * 3.0f + no_def * 0.35f + no_org * 2.0f + no_mor * 1.5f;
	float no_def_score = no_def * 3.0f + no_atk * 0.35f + no_org * 2.0f + no_mor * 1.5f;
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
			if(ai_detail::is_backline_unit(state, n, utid)) {
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
		|| state.world.nation_get_modifier_values(nid, sys::national_mod_offsets::mobilization_impact) <= 0.0f
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

	std::vector<std::pair<dcon::nation_id, ai_front_helpers::frontline_sector_map>> nation_front_maps;
	auto is_continuous_front = [&](dcon::nation_id n, ai_front_helpers::walkable_theater_map const& tmap, dcon::province_id battle_loc, uint16_t tid, uint16_t stid) -> bool {
		if(tid == 0 || !state.world.nation_get_is_at_war(n))
			return false;
		for(auto const& kv : nation_front_maps) {
			if(kv.first == n)
				return kv.second.summary_for_province(state, battle_loc, tid, stid).continuous_front;
		}
		nation_front_maps.emplace_back(n, ai_front_helpers::frontline_sector_map{});
		std::vector<dcon::nation_id> war_allies;
		std::vector<dcon::nation_id> war_enemies;
		ai_front_helpers::collect_war_sides(state, n, war_allies, war_enemies);
		nation_front_maps.back().second.build(state, n, tmap, war_allies, war_enemies, false);
		return nation_front_maps.back().second.summary_for_province(state, battle_loc, tid, stid).continuous_front;
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
			if(!army)
				return;
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

		// Also include ALL armies (both 1-hop AND 2+-hop reinforcements!) currently marching with final destination == loc!
		// (Previously only 1-hop neighbors were visible, causing day-2 to miss yesterday's 2-hop reinforcements and over-commit/retreat!)
		auto lead_att = military::get_land_battle_lead_attacker(state, b);
		auto lead_def = military::get_land_battle_lead_defender(state, b);
		std::vector<dcon::army_id> att_with_enroute = att_armies;
		std::vector<dcon::army_id> def_with_enroute = def_armies;
		int32_t att_enroute_front = 0, def_enroute_front = 0;
		int32_t att_enroute_back = 0, def_enroute_back = 0;

		auto record_enroute_army = [&](dcon::army_fat_id army, bool is_att_side, dcon::nation_id tech_ctrl) {
			if(is_att_side) {
				if(std::find(att_with_enroute.begin(), att_with_enroute.end(), army.id) != att_with_enroute.end())
					return;
				auto sc = ai_detail::classify_army_composition(state, tech_ctrl, army.id);
				att_with_enroute.push_back(army.id);
				att_enroute_front += sc.frontline;
				att_enroute_back += sc.backline;
			} else {
				if(std::find(def_with_enroute.begin(), def_with_enroute.end(), army.id) != def_with_enroute.end())
					return;
				auto sc = ai_detail::classify_army_composition(state, tech_ctrl, army.id);
				def_with_enroute.push_back(army.id);
				def_enroute_front += sc.frontline;
				def_enroute_back += sc.backline;
			}
		};

		std::vector<dcon::nation_id> scan_att_nations;
		std::vector<dcon::nation_id> scan_def_nations;
		if(lead_att) {
			std::vector<dcon::nation_id> tmp_e;
			ai_front_helpers::collect_war_sides(state, lead_att, scan_att_nations, tmp_e);
		}
		if(lead_def) {
			std::vector<dcon::nation_id> tmp_e;
			ai_front_helpers::collect_war_sides(state, lead_def, scan_def_nations, tmp_e);
		}
		for(auto an : att_ai_nations) {
			if(std::find(scan_att_nations.begin(), scan_att_nations.end(), an) == scan_att_nations.end())
				scan_att_nations.push_back(an);
		}
		for(auto dn : def_ai_nations) {
			if(std::find(scan_def_nations.begin(), scan_def_nations.end(), dn) == scan_def_nations.end())
				scan_def_nations.push_back(dn);
		}

		for(auto sn : scan_att_nations) {
			for(auto ar : state.world.nation_get_army_control(sn)) {
				auto army = ar.get_army();
				if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation())
					continue;
				auto path = army.get_path();
				if(path.size() > 0 && path.at(0) == loc) {
					record_enroute_army(army, true, sn);
				}
			}
		}
		for(auto sn : scan_def_nations) {
			for(auto ar : state.world.nation_get_army_control(sn)) {
				auto army = ar.get_army();
				if(army.get_is_retreating() || army.get_black_flag() || army.get_navy_from_army_transport() || army.get_battle_from_army_battle_participation())
					continue;
				auto path = army.get_path();
				if(path.size() > 0 && path.at(0) == loc) {
					record_enroute_army(army, false, sn);
				}
			}
		}
		// Also catch 1-hop rebel armies marching into this battle (rebel armies have null controller!)
		for(auto padj : state.world.province_get_province_adjacency(loc)) {
			auto nb = padj.get_connected_provinces(0) == loc ? padj.get_connected_provinces(1) : padj.get_connected_provinces(0);
			if(province::is_sea(state, nb))
				continue;
			for(auto ar : state.world.province_get_army_location(nb)) {
				auto army = ar.get_army();
				if(army.get_controller_from_army_control() || army.get_is_retreating() || army.get_battle_from_army_battle_participation())
					continue;
				auto path = army.get_path();
				if(path.size() > 0 && path.at(0) == loc) {
					bool rebels_att = (!lead_att);
					record_enroute_army(army, rebels_att, dcon::nation_id{});
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
				uint16_t loc_safe_theater = tmap.safe_theater_of(loc);
				if(loc_theater == 0)
					continue;
				bool cont_front = is_continuous_front(n, tmap, loc, loc_theater, loc_safe_theater);
				auto const& tracker = get_pin_tracker(n);
				float max_gather_km = cont_front ? 450.0f : 650.0f;
				float max_days = cont_front ? 22.0f : 26.0f;

				for(auto ar : state.world.nation_get_army_control(n)) {
					auto army = ar.get_army();
					if(!unit_on_ai_control(state, army))
						continue;
					auto a_act = army_activity(army.get_ai_activity());
					if(a_act == army_activity::transport_guard
						|| a_act == army_activity::transport_attack
						|| a_act == army_activity::attack_transport
						|| a_act == army_activity::merging) {
						continue; // NEVER yank an army boarding transports (deadlocks the fleet!) or a 1-reg merging fragment!
					}
					if(army.get_battle_from_army_battle_participation()
						|| army.get_navy_from_army_transport()
						|| army.get_black_flag()
						|| army.get_is_retreating()
						|| !army_ready_for_battle(state, n, army))
						continue;
					auto aloc = army.get_location_from_army_location().id;
					if(!aloc || aloc == loc || tmap.theater_of(aloc) != loc_theater)
						continue;
					if(loc_safe_theater != 0 && tmap.safe_theater_of(aloc) != 0 && tmap.safe_theater_of(aloc) != loc_safe_theater)
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
						int32_t extra_cores = 0;
						for(auto const& prev : to_send) {
							if(prev.ctrl == rc.ctrl && prev.loc == rc.loc) {
								extra_regs += prev.total_regs;
								extra_stacks += 1;
								if(prev.is_core) extra_cores += 1;
							}
						}
						if(!tracker.can_pull(state, rc.ctrl, rc.loc, rc.total_regs, rc.dig_in, rc.is_core, loc, true, extra_regs, extra_stacks, false, extra_cores))
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
					bool sent_any = false;
					for(auto const& rc : to_send) {
						auto bpath = ai_front_helpers::make_front_safe_land_path(state, rc.loc, loc, rc.ctrl, rc.id, true);
						if(bpath.empty())
							continue;
						state.world.army_set_special_order(rc.id, uint8_t(military::special_army_order::none));
						if(military::set_army_path(state, rc.id, bpath, rc.ctrl, true)) {
							auto prev_act = army_activity(state.world.army_get_ai_activity(rc.id));
							if(prev_act == army_activity::attacking || prev_act == army_activity::attack_gathered) {
								state.world.army_set_ai_activity(rc.id, uint8_t(army_activity::on_guard));
								state.world.army_set_ai_province(rc.id, loc);
							}
							get_pin_tracker(rc.ctrl).commit_pull(rc.loc, rc.total_regs, rc.is_core);
							if(side_is_att) att_with_enroute.push_back(rc.id);
							else def_with_enroute.push_back(rc.id);
							sent_any = true;
						}
					}
					if(sent_any) {
						return; // Reinforced — stay and fight!
					}
				}
			}

			// 2. Retreat before wipe if battle is retreatable and decisively lost with no viable relief!
			// Use effective_cw (scaled to actual battle size, NOT full 1836 cw=30!) so small skirmishes don't falsely trigger frontline_collapsed!
			if(retreatable && our_ratio < 0.45f) {
				bool frontline_collapsed = (our_front < std::max(1, effective_cw / 3)) || (our_exposed >= 2);
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
	int32_t core_backline_target = std::clamp(std::min(nation_cw <= 14 ? nation_cw : 5, std::max(6, supply_cap_regs) / 2), 3, 10);

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
		int32_t local_cap = std::min(local_sl_regs, std::max(6, supply_cap_regs));

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
			int32_t cap_for_host = (oact == army_activity::attacking) ? local_sl_regs : local_cap;
			if(hsc.total == 0
				|| hsc.total + my_sc.total > cap_for_host
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
		} else {
			// Stopped with no compatible host (arrived OR path interrupted mid-march) -> release cleanly to on_guard!
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

	// Fast early-out BEFORE building theater map if n has no stationary >=6-regiment on_guard donor stacks!
	bool any_potential_donor = false;
	for(auto a : state.world.nation_get_army_control(n)) {
		auto ar = a.get_army();
		if(unit_on_ai_control(state, ar)
			&& army_activity(ar.get_ai_activity()) == army_activity::on_guard
			&& !ar.get_arrival_time()
			&& !ar.get_battle_from_army_battle_participation()
			&& !ar.get_navy_from_army_transport()
			&& !ar.get_black_flag()
			&& !ar.get_is_retreating()) {
			auto r = ar.get_army_membership();
			if(int32_t(r.end() - r.begin()) >= 6) {
				any_potential_donor = true;
				break;
			}
		}
	}
	if(!any_potential_donor)
		return;

	float const reg_weight = std::max(0.1f, state.defines.pop_size_per_regiment / 1000.0f);
	ai_front_helpers::walkable_theater_map tmap;
	tmap.build(state, n);

	struct prov_state {
		dcon::province_id p;
		uint16_t safe_theater_id = 0;
		int32_t present_regs = 0;
		int32_t own_present_regs = 0;
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
		// Account for stationary allied/non-n armies occupying p for supply-cap room (skip passing-through armies!)
		int32_t other_regs = 0;
		for(auto oa : state.world.province_get_army_location(p)) {
			auto oarmy = oa.get_army();
			if(oarmy.get_black_flag() || oarmy.get_is_retreating() || oarmy.get_navy_from_army_transport() || oarmy.get_battle_from_army_battle_participation() || oarmy.get_arrival_time())
				continue;
			if(oarmy.get_controller_from_army_control() == n && unit_on_ai_control(state, oarmy))
				continue;
			auto orng = oarmy.get_army_membership();
			other_regs += int32_t(orng.end() - orng.begin());
		}
		uint16_t stid = tmap.safe_theater_of(p);
		provs.push_back(prov_state{
			p,
			stid != 0 ? stid : tmap.theater_of(p),
			other_regs,
			0,
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
		uint16_t safe_theater_id = 0;
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
		provs[pidx].own_present_regs += sc.total;
		provs[pidx].present_front += sc.frontline;
		provs[pidx].present_back += sc.backline;
		if(act == army_activity::attacking) {
			provs[pidx].has_staging_attack = true;
		}

		// Eligible donor: stationary on_guard stack staying at loc (not queued to depart or board transports!) with >= 6 regiments
		auto ai_p = ar.get_ai_province().id;
		bool queued_to_depart = (ai_p && ai_p != loc);
		if(act == army_activity::on_guard
			&& !queued_to_depart
			&& !ar.get_battle_from_army_battle_participation()
			&& sc.total >= 6
			&& army_ready_for_battle(state, n, ar.id)) {
			std::vector<inf_reg_entry> pure_inf;
			for(auto r : ar.get_army_membership()) {
				auto reg = r.get_regiment();
				auto uid = reg.get_type();
				if(!uid || ai_detail::is_backline_unit(state, n, uid))
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
					provs[pidx].safe_theater_id,
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
			if(donors[d].safe_theater_id == 0 || donors[d].safe_theater_id != tprov.safe_theater_id || donors[d].loc == tprov.p || donors[d].pure_inf.empty())
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

			// Ensure donating NEVER weakens our OWN local defense (never rely on allied stacks that may walk away next tick!) or exposes donor's artillery!
			int32_t min_loc_regs = (dprov.adj_enemy_regs > 0.1f)
				? std::max(4, std::min(dprov.cw, int32_t(std::ceil(dprov.adj_enemy_regs * 0.85f))) + 2)
				: 0;
			int32_t prov_surplus = dprov.own_present_regs - min_loc_regs;
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
			dprov.own_present_regs -= take;
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

	// 1. Collect warring AI nations once (skip global transport scans when nobody is at war!)
	//    and cancel strategic_redeployment immediately if hostiles approach within 2 hops!
	std::vector<dcon::nation_id> warring_nations;
	for(uint32_t i = 0; i < state.world.nation_size(); ++i) {
		dcon::nation_id nid{ dcon::nation_id::value_base_t(i) };
		if(!state.world.nation_is_valid(nid)
			|| state.world.nation_get_owned_province_count(nid) == 0
			|| !state.world.nation_get_is_at_war(nid)) {
			continue;
		}
		warring_nations.push_back(nid);
		if(!state.world.nation_get_is_player_controlled(nid)) {
			for(auto a : state.world.nation_get_army_control(nid)) {
				auto ar = a.get_army();
				if(ar.get_special_order() == uint8_t(military::special_army_order::strategic_redeployment)
					&& unit_on_ai_control(state, ar)) {
					auto loc = ar.get_location_from_army_location().id;
					bool danger = ai_front_helpers::is_province_near_hostiles(state, nid, loc, 2);
					if(!danger && ar.get_path().size() > 0) {
						auto next_p = ar.get_path().at(ar.get_path().size() - 1);
						danger = ai_front_helpers::is_province_near_hostiles(state, nid, next_p, 1);
					}
					if(danger) {
						ar.set_special_order(uint8_t(military::special_army_order::none));
					}
				}
			}
		}
	}

	// 2. Daily live battle control (reserve reinforcement + retreat before wipe — also covers rebel battles in peacetime!)
	manage_active_battles(state);

	if(warring_nations.empty())
		return;

	// 3. Fast 2-3 day war reaction cadence for nations currently at war (100% sequential & OOS-safe!)
	auto day = state.current_date.value;
	bool const everyday_mode = bool(state.defines.alice_eval_ai_mil_everyday);
	if(day % 3 == 0) {
		if(!everyday_mode) {
			for(auto n : warring_nations) {
				distribute_guards_wartime(state, n);
			}
		}
		for(auto n : warring_nations) {
			ai_split_surplus_regiments(state, n);
		}
	} else if(day % 3 == 1 && !everyday_mode) {
		for(auto n : warring_nations) {
			assign_targets(state, n);
		}
	}

	if(day % 2 == 0) {
		for(auto n : warring_nations) {
			merge_arrived_reinforcements(state, n);
		}
		if(!everyday_mode) {
			auto r8 = day % 8;
			move_idle_guards_impl(state, true);
			move_gathered_attackers_impl(state, true);
			if(r8 != 4) {
				update_naval_transport(state);
			}
			if(r8 != 0) {
				advance_active_transport_fleets(state);
			}
		}
	}

	// 4. Keep wartime supply priorities synced daily (after guard/target assignment, splits, attacks & retreats — warring AI nations only)
	for(auto n : warring_nations) {
		if(!state.world.nation_get_is_player_controlled(n)) {
			update_unit_supply_priorities_for_nation(state, n);
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
