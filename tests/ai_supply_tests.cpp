#include "catch.hpp"
#include "ai_supply.hpp"
#include "military.hpp"

/*
The test files are #included into one translation unit, so the helpers below are
namespaced (see ai_pressure_tests.cpp). gamestate is the file-static from
pathfinding_tests.cpp.
*/

namespace {

void require_in_unit_range(float v) {
	REQUIRE(v >= 0.0f);
	REQUIRE(v <= 1.0f);
}

}

TEST_CASE("ai supply scores", "[ai][supply]") {

	gamestate = load_testing_scenario_file_with_save(sys::network_mode_type::host);

	// The per-nation caches are refreshed on load and every tick; refreshing
	// here keeps the assertions independent of fill_unsaved_data ordering.
	supply_routes::update_nations_supply_cache(*gamestate);

	SECTION("army supply scores stay within 0..1") {
		int32_t checked = 0;
		for(auto ar : gamestate->world.in_army) {
			if(!ar.get_controller_from_army_control())
				continue;

			require_in_unit_range(ai::army_supply_score(*gamestate, ar.id));
			if(++checked >= 256)
				break;
		}
		// The test scenario is expected to field at least one controlled army.
		REQUIRE(checked > 0);
	}

	SECTION("province quality stays in 0..1 and own ground reads as friendly") {
		int32_t checked = 0;
		for(auto ar : gamestate->world.in_army) {
			auto n = ar.get_controller_from_army_control();
			if(!n)
				continue;

			auto loc = ar.get_location_from_army_location();
			require_in_unit_range(ai::province_supply_quality(*gamestate, n, loc.id));
			if(loc.get_nation_from_province_control() == n) {
				// A province this nation controls must judge as a supply zone
				// for itself; otherwise the march-floor vetoes would misfire.
				REQUIRE(ai::is_friendly_supply_zone(*gamestate, n, loc.id));
			}
			if(++checked >= 256)
				break;
		}
		REQUIRE(checked > 0);
	}
}
