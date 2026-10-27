// Differential tests: the pooled engine is only acceptable if it is
// indistinguishable from the baseline engine in everything but memory behaviour.
#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "lob/apply.hpp"
#include "lob/generator.hpp"
#include "lob/matching_engine.hpp"
#ifdef LOB_HAVE_BENCH
#include "workloads.hpp"
#endif

using namespace lob;
using Events = std::vector<Event>;

namespace {

// Runs both engines in lock-step; every event batch and every book must match.
void expect_identical(const std::vector<Command>& cmds, const std::string& what, std::size_t compare_every) {
    MatchingEngine base;
    PooledMatchingEngine pool;
    Events eb, ep;
    std::size_t i = 0;
    for (const Command& c : cmds) {
        eb.clear();
        ep.clear();
        if (std::holds_alternative<SnapshotCommand>(c)) continue;
        apply_command(base, c, eb);
        apply_command(pool, c, ep);
        ASSERT_EQ(eb, ep) << what << ": events differ at command " << i;
        if (++i % compare_every == 0) {
            ASSERT_EQ(base.book().snapshot(), pool.book().snapshot()) << what << ": books differ at command " << i;
            ASSERT_EQ(base.book().order_count(), pool.book().order_count());
        }
    }
    EXPECT_EQ(base.book().snapshot(), pool.book().snapshot()) << what;
    std::string why;
    EXPECT_TRUE(pool.book().validate(&why)) << why;
}

}  // namespace

TEST(Pooled, MatchesBaselineOnRandomSequences) {
    for (std::uint64_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
        GeneratorConfig cfg;
        cfg.seed = seed;
        cfg.count = 3000;
        cfg.band = static_cast<Price>(seed * 2);  // from tight (heavy crossing) to wide
        cfg.bad_percent = 8;
        cfg.sweep_percent = 10;
        cfg.snapshot_every = 0;
        expect_identical(generate(cfg), "random seed " + std::to_string(seed), 25);
    }
}

TEST(Pooled, MatchesBaselineWithIocFokMarketAndAmend) {
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        GeneratorConfig cfg;
        cfg.seed = seed;
        cfg.count = 3000;
        cfg.band = static_cast<Price>(3 + seed * 5);
        cfg.ioc_percent = 15;
        cfg.fok_percent = 10;
        cfg.market_percent = 10;
        cfg.amend_percent = 18;
        expect_identical(generate(cfg), "extensions seed " + std::to_string(seed), 25);
    }
}

#ifdef LOB_HAVE_BENCH
TEST(Pooled, MatchesBaselineOnEveryBenchmarkWorkload) {
    lob::bench::WorkloadParams p;
    p.seed = 3;
    p.measured = 2500;
    p.warmup = 200;
    p.prefill_scale = 0.02;
    for (const std::string& name : lob::bench::workload_names())
        expect_identical(lob::bench::to_commands(lob::bench::make_workload(name, p)), name, 50);
}
#endif

TEST(Pooled, SurvivesLongAddCancelChurnWithAValidBook) {
    // Heavy node recycling through the free lists (add/cancel pairs on a ~1,000-order book) must leave the
    // book valid and the right size. Memory reuse itself is asserted at arena level
    // (Arena.SteadyChurnDoesNotGrowMemory); the engine does not expose arena statistics.
    PooledMatchingEngine e;
    Events ev;
    std::mt19937_64 rng(5);
    std::vector<OrderId> live;
    OrderId next = 1;
    auto add = [&] {
        const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
        const Price d = 1 + static_cast<Price>(rng() % 50);
        e.submit(Order{next, s, s == Side::Buy ? 1000 - d : 1000 + d, 1 + rng() % 9}, ev);
        live.push_back(next++);
    };
    for (int i = 0; i < 1000; ++i) add();
    ev.clear();
    for (int i = 0; i < 8000; ++i) {
        const std::size_t k = rng() % live.size();
        e.cancel(live[k], ev);
        live[k] = live.back();
        live.pop_back();
        add();
        ev.clear();
    }
    EXPECT_EQ(e.book().order_count(), 1000u);
    std::string why;
    EXPECT_TRUE(e.book().validate(&why)) << why;
}
