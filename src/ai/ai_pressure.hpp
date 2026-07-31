#pragma once
#include "dcon_generated_ids.hpp"
#include "container_types.hpp"
#include <cstdint>
#include <vector>

namespace sys {
struct state;
}

namespace ai {

/*
The AI's "internal map" of a war.

Instead of asking the binary question "is there an enemy next door", which a single token
regiment can answer in the affirmative and thereby pin an entire army group, the AI sums
the weight of every army that could plausibly reach a province, discounted by how far away
it is. A 3k garrison two provinces away barely registers; a 200k stack four provinces back
in the enemy rear still does. Deciding by that number rather than by presence is what lets
the AI hold a line without being trivially fixed in place.

Weights are in thousands of men, matching military::sum_army_weight, so they are directly
comparable to supply limits and to the rest of the AI's strength arithmetic.
*/

enum class pressure_horizon : uint8_t {
	// Used when redistributing garrisons. Armies locked in a battle still count, at a
	// discount: the battle will end and the mass will still be sitting there.
	strategic,
	// Used when deciding whether a garrison may leave its post today. An army pinned in a
	// battle contributes nothing, because it cannot intervene anywhere else.
	tactical
};

struct pressure_field {
	// Both indexed by province_id::index(); sea provinces stay zero.
	std::vector<float> hostile;
	std::vector<float> friendly;

	bool valid() const {
		return !hostile.empty();
	}

	/*
	province_id::index() is int32_t(value) - 1, so a null id yields -1. An unguarded read
	would take a heap byte that is not gamestate, differs between allocators, and would
	flow into a guard assignment; hence the explicit test rather than a bare subscript.
	*/
	float hostile_at(dcon::province_id p) const {
		return (p && !hostile.empty()) ? hostile[uint32_t(p.index())] : 0.0f;
	}
	float friendly_at(dcon::province_id p) const {
		return (p && !friendly.empty()) ? friendly[uint32_t(p.index())] : 0.0f;
	}
};

/*
One army's contribution before it is spread: where to start, the nation whose right of passage
gates how far it travels, and how much to deposit.

This and coalesce_seeds below are declared here rather than kept private because they are the
only part of this module that is a pure function of its arguments, and so the only part a test
can reach without standing up a whole game state. They are also where a divergence between two
clients would hide, which is the thing most worth pinning down with a test.
*/
struct pressure_seed {
	dcon::province_id where;
	dcon::nation_id mover; // whose right of passage gates the spread
	float weight = 0.0f;
	bool hostile = false;
};

/*
Merges seeds sharing a province and an owner so each costs a single traversal, leaving the
result ordered by mover and then by province.

The sort is stable on purpose. Seeds from several armies of one nation in one province compare
equal, and float addition is not associative, so an unspecified order among them would make the
merged weight depend on the standard library implementation: a Windows and a Linux client would
disagree, and the desync would surface days later at an unrelated field.
*/
void coalesce_seeds(std::vector<pressure_seed>& seeds);

// True when the pressure model is enabled; when false every consumer keeps its old path.
bool pressure_enabled(sys::state& state);

// Weight of one army in field units. Zero for armies that cannot fight where they stand.
float army_pressure_weight(sys::state& state, dcon::army_id a);

/*
Coarse magnitude of a pressure reading, as a power of two: 0 for [1,2), 1 for [2,4), -1 for
[0.5,1), and -128 for "nothing here". Comparing buckets rather than raw floats is what stops
a rounding-level difference in pressure from reshuffling every garrison assignment on the
next pass. Deliberately built from exact halving rather than std::log2, which is a libm call
and therefore not guaranteed identical across platforms.
*/
int8_t pressure_bucket(float v);

// Builds the field as seen by n. Costs one traversal per province holding armies, so
// callers should build once and reuse it for a whole decision pass.
void build_pressure_field(sys::state& state, dcon::nation_id n, pressure_horizon horizon, pressure_field& out);

/*
n's tactical field for the current day, built on first use and then reused. Only safe from
single-threaded parts of the tick; distribute_guards runs under parallel_for and builds its
own. Debits applied during the day persist, which is the point: an army committed to one
battle must not still read as cover for the next.
*/
pressure_field& cached_tactical_field(sys::state& state, dcon::nation_id n);

// Drops every cached field. Fields expire on their own when the day changes; this is for the
// one case that cannot detect: a save loaded at a date the cache has already seen.
void clear_pressure_cache();

/*
Removes a departing army's contribution from the friendly side, so that armies deciding one
after another do not each count the others as cover they are about to lose.

The province and weight are passed in rather than re-derived from the army because the caller
issues movement orders around this call, and re-derivation would then depend on whether those
orders took: an army whose path had been rewritten would have its weight subtracted partly
from wherever it was headed instead of wholly from the province it is vacating.
*/
void debit_friendly_at(sys::state& state, pressure_field& field, dcon::province_id where, float weight, dcon::nation_id mover);

// Weight engaged on each side of the land battle in p, from n's point of view.
// False when there is no battle there.
bool battle_side_weights(sys::state& state, dcon::nation_id n, dcon::province_id p, float& out_hostile, float& out_friendly);

// Weight of friendly armies already marching to p, so reinforcements in transit are not
// ordered a second time on the following day.
float inbound_friendly_weight(sys::state& state, dcon::nation_id n, dcon::province_id p);

} // namespace ai
