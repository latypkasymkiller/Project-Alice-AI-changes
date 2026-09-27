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
update_nations_supply_cache refreshes daily -- normalized against the nation's
capital, since the absolute throughput is zero for everyone early in the game
and at war fronts. Used as a soft tie-break when picking assault assembly
points; deliberately NOT used to veto marches, because a zero cache at the
front says nothing about whether the march makes sense.
*/
float province_supply_quality(sys::state const& state, dcon::nation_id n, dcon::province_id prov);

// Tunables, clamped per the define_f discipline in ai_pressure.cpp.

// Armies below this score are held out of offensives and battle gathering. 0 disables.
float army_supply_floor(sys::state const& state);

// Relative distance penalty for gathering an assault in a province our supply
// does not reach: 1.0 doubles the effective distance of a zero-quality candidate.
float assembly_supply_weight(sys::state const& state);

} // namespace ai
