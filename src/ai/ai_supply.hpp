#pragma once
#include "system_state_forward.hpp"

namespace ai {

/*
Aggregate 0..1 measure of how well an army is being supplied: the best active
supply route's throughput x survival, floored by yesterday's average regiment
satisfaction. Zero for black-flagged armies and for armies no route reaches --
those recover no organization (calculate_regiment_org_regain scales with
satisfaction) and only enlarge the pile that starts bleeding from attrition the
moment the battle they are stacked into resolves.
*/
float army_supply_score(sys::state& state, dcon::army_id army);

/*
Supply quality of a province for one nation, from the per-nation caches that
update_nations_supply_cache refreshes daily. Zero where no delivery is possible
for this nation; otherwise graded down by the loss cache. Cheap enough to read
per candidate province inside a decision pass.
*/
float province_supply_quality(sys::state const& state, dcon::nation_id n, dcon::province_id prov);

/*
True where a supply-quality reading is meaningful to judge for this nation: the
province is controlled by n, or held by an ally. Hostile ground is exempt from
march-floor vetoes, because its cache reads zero even when the advance itself is
sound -- the province is not ours to supply until it is taken.
*/
bool is_friendly_supply_zone(sys::state const& state, dcon::nation_id n, dcon::province_id prov);

// Tunables, clamped per the define_f discipline in ai_pressure.cpp.

// Armies below this score are held out of offensives and battle gathering. 0 disables.
float army_supply_floor(sys::state const& state);

// Land marches into one of our or an ally's provinces below this quality are
// refused. 0 disables.
float march_supply_floor(sys::state const& state);

// Relative distance penalty for gathering an assault in a province our supply
// does not reach: 1.0 doubles the effective distance of a zero-quality candidate.
float assembly_supply_weight(sys::state const& state);

} // namespace ai
