#include "ai_pressure.hpp"
#include "system_state.hpp"
#include "military.hpp"
#include "province.hpp"

#include <algorithm>
#include <cmath>

namespace ai {

namespace {

// Defines come from a modifiable lua file, so anything used to size a loop or index a
// buffer is clamped, and NaN is scrubbed first: NaN survives std::clamp untouched, because
// both of its comparisons are false, and would then turn a bound into garbage.
float define_f(float v, float lo, float hi) {
	if(!(v == v))
		return lo;
	return std::clamp(v, lo, hi);
}

/*
An army with no controlling nation is a rebel army. military::are_enemies cannot classify
those: its first clause reads (!a && a), which is never true, so a real nation asking about
a rebel army falls through to are_at_war and is told they are not enemies (military.cpp:884).
Repairing that helper outright would change what a null argument means at its other call
sites, where it stands for an uncolonized province rather than a rebel army, so the
classification is made here instead.
*/
bool army_is_hostile_to(sys::state& state, dcon::nation_id n, dcon::nation_id controller) {
	if(!controller)
		return true; // rebels fight everyone
	if(controller == n)
		return false;
	return military::are_at_war(state, n, controller);
}

bool army_is_friendly_to(sys::state& state, dcon::nation_id n, dcon::nation_id controller) {
	if(!controller)
		return false;
	if(controller == n)
		return true;
	return military::are_allied_in_war(state, n, controller);
}

bool is_land_province(sys::state& state, dcon::province_id p) {
	return p && p.index() < state.province_definitions.first_sea_province.index();
}

/*
Whether `mover` can cross an edge. Rebels are handled separately because
province::is_adjacency_impassable reaches military::are_at_war with a null nation, which
ends up testing a bit vector at index -1; rebels are gated by terrain alone anyway.
*/
bool edge_passable(sys::state& state, dcon::nation_id mover, dcon::province_adjacency_id adj) {
	if(!mover)
		return (state.world.province_adjacency_get_type(adj) & province::border::impassible_bit) == 0;

	return !province::is_adjacency_impassable(state, mover, adj);
}

/*
Scratch shared by every traversal in one field build. Visited marks are stamped with a
generation counter instead of being cleared, and the answers of has_access_to_province are
memoised per mover, because that call is the dominant cost of the whole build: it ends in
military::are_in_common_war, which unlike are_at_war has no is-at-war fast path and walks
every war and participant.

thread_local, not static: distribute_guards builds a field inside make_defense's
parallel_for. The memo is also invalidated at every public entry point, so it can never
serve an answer computed on a previous day or let the result depend on which worker thread
happened to run a given nation.
*/
struct traversal_scratch {
	std::vector<uint32_t> stamp;
	std::vector<uint8_t> access; // 0 unknown, 1 reachable, 2 denied
	std::vector<dcon::province_id> frontier;
	std::vector<dcon::province_id> next;
	dcon::nation_id access_for;
	bool access_valid = false;
	uint32_t generation = 0;
};

thread_local traversal_scratch scratch;
thread_local std::vector<pressure_seed> seed_buffer;

void ensure_scratch(sys::state& state) {
	auto const province_count = state.world.province_size();
	// Resized rather than assumed: returning to the menu and loading a different scenario
	// leaves this buffer alive at the old size, and the traversal would then stamp past
	// its end.
	if(scratch.stamp.size() != province_count) {
		scratch.stamp.assign(province_count, 0u);
		scratch.access.assign(province_count, uint8_t(0));
		scratch.generation = 0;
	}
	scratch.frontier.reserve(256);
	scratch.next.reserve(256);
}

// Invalidates the access memo. Called whenever a build or a debit begins, so a memo never
// outlives the traversal group it was computed for.
void begin_epoch() {
	scratch.access_valid = false;
}

uint32_t next_generation() {
	// Rewound rather than allowed to wrap onto 0, which is the "never visited" value.
	if(scratch.generation == UINT32_MAX) {
		std::fill(scratch.stamp.begin(), scratch.stamp.end(), 0u);
		scratch.generation = 0;
	}
	return ++scratch.generation;
}

bool cached_access(sys::state& state, dcon::nation_id mover, dcon::province_id p) {
	if(!scratch.access_valid || mover != scratch.access_for) {
		std::fill(scratch.access.begin(), scratch.access.end(), uint8_t(0));
		scratch.access_for = mover;
		scratch.access_valid = true;
	}

	auto& slot = scratch.access[uint32_t(p.index())];
	if(slot == 0)
		slot = province::has_access_to_province(state, mover, p) ? uint8_t(1) : uint8_t(2);

	return slot == 1;
}

/*
Adds `weight` at `origin` and a geometrically decaying share of it to everything within
reach, so that distance dilutes a threat instead of switching it off. Expansion refuses
provinces `mover` may not enter, which is what keeps an army with no way through from
haunting a sector it can never attack: such a province receives exactly zero. Negative
weights subtract, which is how an army that has committed elsewhere is removed.
*/
void spread_pressure(sys::state& state, dcon::province_id origin, float weight, dcon::nation_id mover,
		std::vector<float>& out) {

	if(!is_land_province(state, origin) || weight == 0.0f)
		return;

	float const falloff = define_f(state.defines.alice_ai_pressure_falloff, 0.05f, 0.95f);
	float const min_contribution = define_f(state.defines.alice_ai_pressure_min_contribution, 0.01f, 1000.0f);
	int32_t const max_hops = int32_t(define_f(state.defines.alice_ai_pressure_max_hops, 0.0f, 64.0f));

	auto const generation = next_generation();

	// The origin always receives its full weight, even for an army too small to propagate
	// anywhere: it is standing right there.
	scratch.stamp[uint32_t(origin.index())] = generation;
	out[uint32_t(origin.index())] += weight;

	scratch.frontier.clear();
	scratch.frontier.push_back(origin);

	float contribution = weight;
	for(int32_t hop = 0; hop < max_hops && !scratch.frontier.empty(); ++hop) {
		contribution *= falloff;
		if(std::abs(contribution) < min_contribution)
			break;

		scratch.next.clear();
		for(auto cur : scratch.frontier) {
			auto fat_cur = dcon::fatten(state.world, cur);
			for(auto adj : fat_cur.get_province_adjacency()) {
				auto other = adj.get_connected_provinces(0) == fat_cur
					? adj.get_connected_provinces(1)
					: adj.get_connected_provinces(0);

				if(!is_land_province(state, other.id))
					continue;
				if(scratch.stamp[uint32_t(other.id.index())] == generation)
					continue;
				if(!edge_passable(state, mover, adj.id))
					continue;
				if(!cached_access(state, mover, other.id))
					continue;

				scratch.stamp[uint32_t(other.id.index())] = generation;
				out[uint32_t(other.id.index())] += contribution;
				scratch.next.push_back(other.id);
			}
		}
		std::swap(scratch.frontier, scratch.next);
	}
}

// Raw army weight, without the zeroing army_pressure_weight applies to armies that cannot
// fight. Used for battle tallies, where a black-flagged army is still absorbing blows.
float raw_army_weight(sys::state& state, dcon::army_id a) {
	float total = 0.0f;
	for(auto rg : state.world.army_get_army_membership(a)) {
		total += (state.defines.pop_size_per_regiment / 1000.0f) * rg.get_regiment().get_strength();
	}
	return total;
}

/*
Works out where an army's weight belongs. The AI is deliberately omniscient, so an army
already on the march is counted partly where it stands and partly where it is going: that
is what lets a defender start shifting reserves while the blow is still in transit rather
than after it lands.
*/
void collect_army_seeds(sys::state& state, dcon::army_id a, dcon::nation_id controller, bool hostile,
		pressure_horizon horizon, std::vector<pressure_seed>& into) {

	float w = army_pressure_weight(state, a);
	if(w <= 0.0f)
		return;

	if(state.world.army_get_battle_from_army_battle_participation(a)) {
		if(horizon == pressure_horizon::tactical)
			return; // pinned in a fight; it cannot intervene anywhere else today
		w *= define_f(state.defines.alice_ai_pressure_in_battle_weight, 0.0f, 1.0f);
	}

	/*
	An embarked army projects onto every shore its fleet could put it on. The weight is not
	divided between them, which overstates the total: the fleet really can land at any one
	of them, and the alternative, contributing nothing at all, reads as an empty coast and
	releases the garrison outright. Overstating a threat is the safer error here.
	*/
	if(auto transport = state.world.army_get_navy_from_army_transport(a); transport) {
		auto sea = state.world.navy_get_location_from_navy_location(transport);
		if(!sea || is_land_province(state, sea))
			return;

		float const share = define_f(state.defines.alice_ai_pressure_amphibious_weight, 0.0f, 1.0f) * w;
		if(share <= 0.0f)
			return;

		auto fat_sea = dcon::fatten(state.world, sea);
		for(auto adj : fat_sea.get_province_adjacency()) {
			auto other = adj.get_connected_provinces(0) == fat_sea
				? adj.get_connected_provinces(1)
				: adj.get_connected_provinces(0);
			if(is_land_province(state, other.id))
				into.push_back(pressure_seed{ other.id, controller, share, hostile });
		}
		return;
	}

	auto loc = state.world.army_get_location_from_army_location(a);
	if(!is_land_province(state, loc))
		return;

	dcon::province_id destination;
	auto path = state.world.army_get_path(a);
	if(path.size() > 0)
		destination = path.at(0); // paths are stored destination first

	if(is_land_province(state, destination) && destination != loc) {
		float const share = define_f(state.defines.alice_ai_pressure_destination_weight, 0.0f, 1.0f);
		into.push_back(pressure_seed{ loc, controller, w * (1.0f - share), hostile });
		into.push_back(pressure_seed{ destination, controller, w * share, hostile });
	} else {
		into.push_back(pressure_seed{ loc, controller, w, hostile });
	}
}

// Tactical fields for the current day, indexed by nation. Serial paths only. Sized once per
// day, so a reference handed out stays valid for the rest of the day.
struct tactical_cache {
	std::vector<pressure_field> fields;
	std::vector<int32_t> day;
	uint32_t province_count = 0;
};

tactical_cache serial_cache;

} // namespace

// Defined out here, not in the anonymous namespace above, so that the declaration in the
// header names this function rather than silently leaving an internal copy beside it.
void coalesce_seeds(std::vector<pressure_seed>& seeds) {
	std::stable_sort(seeds.begin(), seeds.end(), [](pressure_seed const& a, pressure_seed const& b) {
		// Grouped by mover first so the access memo is reset once per nation, not per seed.
		if(a.mover != b.mover)
			return a.mover.index() < b.mover.index();
		return a.where.index() < b.where.index();
	});

	uint32_t write = 0;
	for(uint32_t read = 0; read < seeds.size(); ++read) {
		if(write > 0 && seeds[write - 1].where == seeds[read].where && seeds[write - 1].mover == seeds[read].mover) {
			// Which side a seed lands on is a function of its mover alone, so a merged run
			// is always uniform and the flag never has to be part of the key.
			assert(seeds[write - 1].hostile == seeds[read].hostile);
			seeds[write - 1].weight += seeds[read].weight;
		} else {
			seeds[write] = seeds[read];
			++write;
		}
	}
	seeds.resize(write);
}

bool pressure_enabled(sys::state& state) {
	return state.defines.alice_ai_use_pressure != 0.0f;
}

int8_t pressure_bucket(float v) {
	// Also catches NaN, for which every comparison is false.
	if(!(v > 0.0f))
		return -128;

	int8_t b = 0;
	while(v >= 2.0f && b < 64) {
		v *= 0.5f;
		++b;
	}
	while(v < 1.0f && b > -64) {
		v *= 2.0f;
		--b;
	}
	return b;
}

float army_pressure_weight(sys::state& state, dcon::army_id a) {
	// Matches military::sum_army_weight so these numbers stay comparable with the supply
	// limits the rest of the AI reasons about.
	if(state.world.army_get_black_flag(a) || state.world.army_get_is_retreating(a))
		return 0.0f;

	return raw_army_weight(state, a);
}

void build_pressure_field(sys::state& state, dcon::nation_id n, pressure_horizon horizon, pressure_field& out) {
	auto const province_count = state.world.province_size();

	if(out.hostile.size() == province_count) {
		std::fill(out.hostile.begin(), out.hostile.end(), 0.0f);
		std::fill(out.friendly.begin(), out.friendly.end(), 0.0f);
	} else {
		out.hostile.assign(province_count, 0.0f);
		out.friendly.assign(province_count, 0.0f);
	}

	if(!pressure_enabled(state) || !n)
		return;

	ensure_scratch(state);
	begin_epoch();

	auto& seeds = seed_buffer;
	seeds.clear();

	// in_army iterates in id order, so the seed list, and with it the order the float sums
	// are accumulated in, is identical on every client.
	for(auto ar : state.world.in_army) {
		auto controller = ar.get_controller_from_army_control();
		bool const hostile = army_is_hostile_to(state, n, controller);
		if(!hostile && !army_is_friendly_to(state, n, controller))
			continue;

		collect_army_seeds(state, ar.id, controller, hostile, horizon, seeds);
	}

	if(seeds.empty())
		return;

	coalesce_seeds(seeds);

	for(auto const& s : seeds) {
		spread_pressure(state, s.where, s.weight, s.mover, s.hostile ? out.hostile : out.friendly);
	}
}

void debit_friendly_at(sys::state& state, pressure_field& field, dcon::province_id where, float weight, dcon::nation_id mover) {
	if(!field.valid() || !pressure_enabled(state) || !where || !(weight > 0.0f))
		return;

	ensure_scratch(state);
	begin_epoch();

	// The mirror image of how a stationary army was deposited: one seed, full weight, spread
	// with the same decay, so the subtraction lands exactly where the deposit did.
	spread_pressure(state, where, -weight, mover, field.friendly);
}

pressure_field& cached_tactical_field(sys::state& state, dcon::nation_id n) {
	auto const nation_count = state.world.nation_size();
	auto const province_count = state.world.province_size();
	auto const today = state.current_date.value;

	if(serial_cache.fields.size() != nation_count || serial_cache.province_count != province_count) {
		serial_cache.fields.assign(nation_count, pressure_field{});
		serial_cache.day.assign(nation_count, -1);
		serial_cache.province_count = province_count;
	}

	auto const slot = uint32_t(n.index());
	if(serial_cache.day[slot] != today) {
		serial_cache.day[slot] = today;
		build_pressure_field(state, n, pressure_horizon::tactical, serial_cache.fields[slot]);
	}

	return serial_cache.fields[slot];
}

void clear_pressure_cache() {
	serial_cache.fields.clear();
	serial_cache.day.clear();
	serial_cache.province_count = 0;
}

bool battle_side_weights(sys::state& state, dcon::nation_id n, dcon::province_id p, float& out_hostile, float& out_friendly) {
	auto battles = state.world.province_get_land_battle_location(p);
	if(battles.begin() == battles.end())
		return false;

	out_hostile = 0.0f;
	out_friendly = 0.0f;

	auto battle = (*battles.begin()).get_battle();
	for(auto par : state.world.land_battle_get_army_battle_participation(battle)) {
		auto a = par.get_army();
		auto controller = a.get_controller_from_army_control();
		// Raw weight here: an army that is black-flagged or retreating is still part of the
		// fight it is standing in.
		float const w = raw_army_weight(state, a.id);

		if(army_is_hostile_to(state, n, controller))
			out_hostile += w;
		else if(army_is_friendly_to(state, n, controller))
			out_friendly += w;
	}
	return true;
}

float inbound_friendly_weight(sys::state& state, dcon::nation_id n, dcon::province_id p) {
	if(!p)
		return 0.0f;

	float total = 0.0f;
	for(auto ar : state.world.in_army) {
		if(!ar.get_arrival_time())
			continue;

		auto controller = ar.get_controller_from_army_control();
		if(!army_is_friendly_to(state, n, controller))
			continue;

		auto path = ar.get_path();
		for(uint32_t i = 0; i < path.size(); ++i) {
			if(path.at(i) == p) {
				total += army_pressure_weight(state, ar.id);
				break;
			}
		}
	}
	return total;
}

} // namespace ai
