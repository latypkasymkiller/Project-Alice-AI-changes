#include "catch.hpp"
#include "ai_pressure.hpp"

/*
The two pure functions behind the AI pressure model.

Everything else in ai_pressure.cpp needs a loaded game to say anything, but these two are
functions of their arguments alone -- and they are also the two places in that module where a
difference between clients would be silent locally and fatal in multiplayer. pressure_bucket
deliberately avoids std::log2, which is a libm call and not guaranteed identical across
platforms; coalesce_seeds sums floats in an order that a sort decides.

Names are namespaced because the test files are #included into one translation unit.
*/
namespace ai_pressure_test {

static dcon::province_id prov(int32_t i) {
	return dcon::province_id(dcon::province_id::value_base_t(i));
}
static dcon::nation_id nat(int32_t i) {
	return dcon::nation_id(dcon::nation_id::value_base_t(i));
}

} // namespace ai_pressure_test

TEST_CASE("ai::pressure_bucket classifies magnitudes", "[ai][pressure]") {
	SECTION("an absent reading is distinct from a small one") {
		// Guard distribution treats -128 as "empty", not "quiet", so these must not collapse
		// into the ordinary bucket range.
		REQUIRE(ai::pressure_bucket(0.0f) == -128);
		REQUIRE(ai::pressure_bucket(-0.0f) == -128);
		REQUIRE(ai::pressure_bucket(-1.0f) == -128);
		REQUIRE(ai::pressure_bucket(std::numeric_limits<float>::quiet_NaN()) == -128);
	}

	SECTION("powers of two are the edges, and each edge belongs to the bucket above") {
		REQUIRE(ai::pressure_bucket(1.0f) == 0);
		REQUIRE(ai::pressure_bucket(1.999f) == 0);
		REQUIRE(ai::pressure_bucket(2.0f) == 1);
		REQUIRE(ai::pressure_bucket(3.999f) == 1);
		REQUIRE(ai::pressure_bucket(4.0f) == 2);
		REQUIRE(ai::pressure_bucket(1024.0f) == 10);

		REQUIRE(ai::pressure_bucket(0.5f) == -1);
		REQUIRE(ai::pressure_bucket(0.999f) == -1);
		REQUIRE(ai::pressure_bucket(0.25f) == -2);
	}

	SECTION("the loop bounds terminate on values no real reading produces") {
		// Reachable only through a corrupted define, but it must not hang.
		REQUIRE(ai::pressure_bucket(std::numeric_limits<float>::infinity()) == 64);
		REQUIRE(ai::pressure_bucket(std::numeric_limits<float>::denorm_min()) == -64);
	}

	SECTION("monotonic, which is what lets the guard sort compare buckets not floats") {
		int8_t previous = ai::pressure_bucket(0.01f);
		for(float v = 0.01f; v < 100000.0f; v *= 1.07f) {
			int8_t const b = ai::pressure_bucket(v);
			REQUIRE(b >= previous);
			previous = b;
		}
	}
}

TEST_CASE("ai::coalesce_seeds merges by province and mover", "[ai][pressure]") {
	using ai_pressure_test::nat;
	using ai_pressure_test::prov;

	SECTION("degenerate inputs") {
		std::vector<ai::pressure_seed> empty;
		ai::coalesce_seeds(empty);
		REQUIRE(empty.empty());

		std::vector<ai::pressure_seed> one{ { prov(3), nat(1), 5.0f, false } };
		ai::coalesce_seeds(one);
		REQUIRE(one.size() == 1);
		REQUIRE(one[0].weight == 5.0f);
	}

	SECTION("a shared province and mover costs one traversal") {
		std::vector<ai::pressure_seed> seeds{
			{ prov(7), nat(2), 1.5f, true },
			{ prov(7), nat(2), 2.5f, true },
		};
		ai::coalesce_seeds(seeds);
		REQUIRE(seeds.size() == 1);
		REQUIRE(seeds[0].weight == 4.0f);
		REQUIRE(seeds[0].hostile);
	}

	SECTION("different movers cannot share a traversal") {
		// Right of passage gates how far a seed spreads, so these are not interchangeable
		// even though they start in the same province.
		std::vector<ai::pressure_seed> seeds{
			{ prov(7), nat(2), 1.0f, false },
			{ prov(7), nat(3), 2.0f, false },
		};
		ai::coalesce_seeds(seeds);
		REQUIRE(seeds.size() == 2);
	}

	SECTION("output is ordered by mover, then province") {
		// build_pressure_field relies on this to reset its access memo once per nation
		// rather than once per seed.
		std::vector<ai::pressure_seed> seeds{
			{ prov(9), nat(5), 1.0f, false },
			{ prov(2), nat(5), 1.0f, false },
			{ prov(4), nat(1), 1.0f, false },
		};
		ai::coalesce_seeds(seeds);
		REQUIRE(seeds.size() == 3);
		REQUIRE(seeds[0].mover == nat(1));
		REQUIRE(seeds[1].where == prov(2));
		REQUIRE(seeds[2].where == prov(9));
	}

	SECTION("equal keys keep their input order, so the sum is reproducible") {
		/*
		This is the case the whole file exists for. Float addition is not associative: summed
		left to right these three give 0, because adding 1 to 1e8 is lost to rounding, but a
		permutation that cancels the large pair first gives 1. std::stable_sort must leave
		them as they came in, pinning the answer to 0. An unstable sort would be free to
		produce either, and two clients could then disagree about a province while each
		believed it agreed.
		*/
		REQUIRE(((1e8f + 1.0f) + -1e8f) != ((1e8f + -1e8f) + 1.0f)); // not a vacuous case

		std::vector<ai::pressure_seed> seeds{
			{ prov(1), nat(1), 1e8f, false },
			{ prov(1), nat(1), 1.0f, false },
			{ prov(1), nat(1), -1e8f, false },
		};
		ai::coalesce_seeds(seeds);
		REQUIRE(seeds.size() == 1);
		REQUIRE(seeds[0].weight == (1e8f + 1.0f) + -1e8f);
	}

	SECTION("input order holds even when other keys are interleaved between members") {
		std::vector<ai::pressure_seed> seeds{
			{ prov(1), nat(1), 1e8f, false },
			{ prov(5), nat(1), 7.0f, false },
			{ prov(1), nat(1), 1.0f, false },
			{ prov(5), nat(1), 3.0f, false },
			{ prov(1), nat(1), -1e8f, false },
		};
		ai::coalesce_seeds(seeds);
		REQUIRE(seeds.size() == 2);
		REQUIRE(seeds[0].weight == (1e8f + 1.0f) + -1e8f);
		REQUIRE(seeds[1].weight == 10.0f);
	}
}
