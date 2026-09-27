#include "ai_supply.hpp"
#include "military.hpp"
#include "supply_route.hpp"

#include <algorithm>

namespace ai {

namespace {

// Defines come from a modifiable lua file, so anything used to size a loop or
// index a buffer is clamped, and NaN is scrubbed first: NaN survives std::clamp
// untouched, because both of its comparisons are false (ai_pressure.cpp).
// Distinct name from ai_pressure's define_f: the unity build folds both files
// into one translation unit, where two anonymous-namespace define_f collide.
float supply_define_f(float v, float lo, float hi) {
	if(!(v == v))
		return lo;
	return std::clamp(v, lo, hi);
}

} // namespace

float army_supply_score(sys::state& state, dcon::army_id army) {
	if(state.world.army_get_black_flag(army))
		return 0.0f;

	// What the routes would deliver from today onward: an active route's
	// throughput already folds in everything along the path (adjacency
	// efficiency, port capacity, hostile troops); the loss multiplier is the
	// share of the goods that survive the trip.
	float best_route = 0.0f;
	for(auto route : military::unit_get_supply_routes(state, army)) {
		if(!supply_routes::supply_route_is_active(state, route.id))
			continue;
		float const t = supply_routes::supply_route_get_throughput(state, route.id);
		float const l = supply_routes::supply_route_get_supply_loss(state, route.id);
		best_route = std::max(best_route, std::clamp(t * l, 0.0f, 1.0f));
	}

	// Yesterday's per-regiment satisfaction as the floor: it covers armies whose
	// routes were only just (re)created by the daily pass, and armies standing
	// where delivery is possible but nothing is in transit yet.
	float satisfaction_sum = 0.0f;
	uint32_t regiments = 0;
	for(auto m : state.world.army_get_army_membership(army)) {
		satisfaction_sum += state.world.regiment_get_supply_satisfaction(m.get_regiment().id);
		++regiments;
	}
	float const avg_satisfaction = regiments == 0 ? 0.0f : satisfaction_sum / float(regiments);

	return std::clamp(std::max(best_route, avg_satisfaction), 0.0f, 1.0f);
}

float province_supply_quality(sys::state const& state, dcon::nation_id n, dcon::province_id prov) {
	/*
	The throughput cache is an absolute goods volume, and early in the game the
	base modifiers are zero for everyone -- the whole map then reads as zero and
	an absolute floor vetoes every march in the empire (638 cancelled guard
	marches in the first logged campaign). Normalized instead against the
	capital: a nation's capital is the one province that is always supplied, so
	"how much of the capital's throughput does this province get" is meaningful
	in every era. When even the capital reads zero there is no network to
	measure, and filtering would be pure noise -- the quality reads as 1.0 so
	that every floor passes and nothing is vetoed.
	*/
	auto cap = state.world.nation_get_capital(n);
	float const reference = cap ? state.world.nation_get_prov_supply_throughput_cache(n, cap) : 0.0f;
	if(reference <= 0.0f)
		return 1.0f;

	float const throughput = state.world.nation_get_prov_supply_throughput_cache(n, prov);
	if(throughput <= 0.0f)
		return 0.0f;
	// The loss cache is a per-distance fraction; treated here as an indicator --
	// graded quality, not a prediction of what one specific route would deliver.
	float const loss = state.world.nation_get_prov_supply_loss_cache(n, prov);
	return std::clamp((throughput / reference) * (1.0f - loss), 0.0f, 1.0f);
}

bool is_friendly_supply_zone(sys::state const& state, dcon::nation_id n, dcon::province_id prov) {
	auto const controller = state.world.province_get_nation_from_province_control(prov);
	if(controller == n)
		return true;
	if(!controller)
		return false;
	if(auto dip_rel = state.world.get_diplomatic_relation_by_diplomatic_pair(controller, n);
		state.world.diplomatic_relation_get_are_allied(dip_rel)) {
		return true;
	}
	return military::are_allied_in_war(state, n, controller);
}

float army_supply_floor(sys::state const& state) {
	return supply_define_f(state.defines.alice_ai_army_supply_floor, 0.0f, 1.0f);
}

float march_supply_floor(sys::state const& state) {
	return supply_define_f(state.defines.alice_ai_march_supply_floor, 0.0f, 1.0f);
}

float assembly_supply_weight(sys::state const& state) {
	// NaN scrub without a range: any non-negative weight is meaningful.
	float const v = state.defines.alice_ai_assembly_supply_weight;
	return !(v == v) ? 0.0f : std::max(v, 0.0f);
}

} // namespace ai
