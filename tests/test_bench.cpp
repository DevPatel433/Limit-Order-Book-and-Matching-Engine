// Tests for the benchmark support code: statistics must be right, and every
// workload must be valid, deterministic, and shaped as documented. Workloads are
// also pushed through the independent ConsistencyChecker.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#include "lob/matching_engine.hpp"
#include "lob/replay.hpp"
#include "stats.hpp"
#include "workloads.hpp"

using namespace lob;
using namespace lob::bench;

// ----------------------------------------------------------------- statistics --

TEST(BenchStats, NearestRankPercentiles) {
    std::vector<std::uint32_t> v(100);
    std::iota(v.begin(), v.end(), 1u);  // 1..100
    EXPECT_EQ(percentile_sorted(v, 0.50), 50u);
    EXPECT_EQ(percentile_sorted(v, 0.95), 95u);
    EXPECT_EQ(percentile_sorted(v, 0.99), 99u);
    EXPECT_EQ(percentile_sorted(v, 1.00), 100u);
    EXPECT_EQ(percentile_sorted(v, 0.001), 1u);
}

TEST(BenchStats, PercentileAtExactBoundaryIsNotOffByOne) {
    std::vector<std::uint32_t> v(1000);
    std::iota(v.begin(), v.end(), 1u);
    EXPECT_EQ(percentile_sorted(v, 0.999), 999u);  // 0.999 * 1000 is not exactly 999 in floating point
    EXPECT_EQ(percentile_sorted(v, 0.99), 990u);
}

TEST(BenchStats, SampleSizeGatesTailPercentiles) {
    EXPECT_TRUE(supports_percentile(1000, 0.99));
    EXPECT_FALSE(supports_percentile(999, 0.99));
    EXPECT_TRUE(supports_percentile(10000, 0.999));
    EXPECT_FALSE(supports_percentile(9999, 0.999));
    EXPECT_FALSE(supports_percentile(0, 0.5));
    EXPECT_TRUE(supports_percentile(20, 0.5));
}

TEST(BenchStats, SummarizeReportsOnlySupportedPercentiles) {
    std::vector<std::uint32_t> small(500);
    std::iota(small.begin(), small.end(), 1u);
    auto s = summarize(small);
    EXPECT_TRUE(s.has_p95);
    EXPECT_FALSE(s.has_p99);
    EXPECT_FALSE(s.has_p999);
    EXPECT_EQ(s.min, 1u);
    EXPECT_EQ(s.max, 500u);
    EXPECT_DOUBLE_EQ(s.mean, 250.5);

    std::vector<std::uint32_t> big(20000);
    std::iota(big.begin(), big.end(), 1u);
    s = summarize(big);
    EXPECT_TRUE(s.has_p999);
    EXPECT_EQ(s.p999, 19980u);
    EXPECT_EQ(s.p50, 10000u);
}

TEST(BenchStats, SummarizeDoesNotRequireSortedInputAndHandlesEmpty) {
    EXPECT_EQ(summarize({}).n, 0u);
    std::vector<std::uint32_t> v{5, 1, 4, 2, 3};
    const auto s = summarize(v);
    EXPECT_EQ(s.min, 1u);
    EXPECT_EQ(s.max, 5u);
}

TEST(BenchStats, HistogramBinsAreLogSpacedAndCountEverything) {
    LogHistogram h;
    for (std::uint32_t ns : {0u, 1u, 100u, 100u, 105u, 5000u, 4'000'000u}) h.add(ns);
    EXPECT_EQ(h.total(), 7u);
    std::uint64_t sum = 0;
    double prev_hi = 0;
    for (const auto& b : h.bins()) {
        EXPECT_LT(b.lo_ns, b.hi_ns);
        EXPECT_GE(b.lo_ns, prev_hi - 1e-9);  // ascending, non-overlapping
        EXPECT_NEAR(b.hi_ns / b.lo_ns, std::exp2(0.25), 1e-6);
        prev_hi = b.hi_ns;
        sum += b.count;
    }
    EXPECT_EQ(sum, 7u);
}

// ------------------------------------------------------------------ workloads --

namespace {
WorkloadParams small_params() {
    WorkloadParams p;
    p.seed = 7;
    p.measured = 2000;  // small: Debug+ASan validate() is O(n) per operation
    p.warmup = 200;
    p.prefill_scale = 0.02;
    return p;
}

WorkloadParams tiny_params() {
    WorkloadParams p = small_params();
    p.measured = 800;
    p.warmup = 100;
    p.prefill_scale = 0.005;
    return p;
}

struct Shape {
    ReplayStats stats;
    std::size_t max_trades_in_one_op = 0;
    std::size_t bid_levels = 0, ask_levels = 0;
};

Shape run_shape(const Workload& w) {
    MatchingEngine eng;
    std::vector<Event> ev;
    Shape s;
    for (const BenchOp& op : w.ops) {
        ev.clear();
        if (op.is_cancel) eng.cancel(op.cancel_id, ev);
        else eng.submit(op.order, ev);
        std::size_t trades = 0;
        for (const Event& e : ev) {
            if (std::holds_alternative<Trade>(e)) ++trades, ++s.stats.trades;
            else if (std::holds_alternative<Rejected>(e)) ++s.stats.rejected;
            else if (std::holds_alternative<Cancelled>(e)) ++s.stats.cancelled;
        }
        s.max_trades_in_one_op = std::max(s.max_trades_in_one_op, trades);
    }
    s.bid_levels = eng.book().level_count(Side::Buy);
    s.ask_levels = eng.book().level_count(Side::Sell);
    return s;
}
}  // namespace

TEST(BenchWorkloads, AllFiveExistAndUnknownNamesThrow) {
    const auto& n = workload_names();
    EXPECT_EQ(n.size(), 5u);
    for (const char* want : {"non_crossing", "cancel_heavy", "crossing_heavy", "many_levels", "bursty"})
        EXPECT_NE(std::find(n.begin(), n.end(), want), n.end()) << want;
    EXPECT_THROW(make_workload("nope", small_params()), std::invalid_argument);
}

TEST(BenchWorkloads, SizesAddUp_AndAreDeterministic) {
    for (const std::string& name : workload_names()) {
        SCOPED_TRACE(name);
        const WorkloadParams p = tiny_params();
        const Workload a = make_workload(name, p), b = make_workload(name, p);
        EXPECT_EQ(a.measured(), p.measured);
        EXPECT_EQ(a.warmup, p.warmup);
        EXPECT_EQ(a.ops.size(), a.prefill + a.warmup + p.measured);
        EXPECT_EQ(to_commands(a), to_commands(b));
        WorkloadParams other = p;
        other.seed = 8;
        EXPECT_NE(to_commands(a), to_commands(make_workload(name, other)));
    }
}

TEST(BenchWorkloads, EveryWorkloadPassesTheIndependentChecker) {
    for (const std::string& name : workload_names()) {
        SCOPED_TRACE(name);
        const Workload w = make_workload(name, tiny_params());
        std::ostringstream sink;
        const ReplayResult r = replay(to_commands(w), sink, ReplayOptions{.print_events = false, .final_snapshot = false, .check = true});
        EXPECT_FALSE(r.violation) << (r.violation ? r.violation->message : "");
    }
}

TEST(BenchWorkloads, NoRejectsAnywhere_AllCancelsHitLiveOrders) {
    for (const std::string& name : workload_names()) {
        SCOPED_TRACE(name);
        EXPECT_EQ(run_shape(make_workload(name, small_params())).stats.rejected, 0u);
    }
}

TEST(BenchWorkloads, NonCrossingNeverTradesOrCancels) {
    const Shape s = run_shape(make_workload("non_crossing", small_params()));
    EXPECT_EQ(s.stats.trades, 0u);
    EXPECT_EQ(s.stats.cancelled, 0u);
    EXPECT_GT(s.bid_levels, 50u);
}

TEST(BenchWorkloads, CancelHeavyIsAboutHalfCancels) {
    const Workload w = make_workload("cancel_heavy", small_params());
    const Shape s = run_shape(w);
    EXPECT_EQ(s.stats.trades, 0u);
    const double share = static_cast<double>(s.stats.cancelled) / static_cast<double>(w.ops.size() - w.prefill);
    EXPECT_GT(share, 0.4);
    EXPECT_LT(share, 0.6);
}

TEST(BenchWorkloads, CrossingHeavyTradesOnMostOrders) {
    const Workload w = make_workload("crossing_heavy", small_params());
    EXPECT_GT(run_shape(w).stats.trades, w.measured() / 3);
}

TEST(BenchWorkloads, ManyLevelsIsWide) {
    const Shape s = run_shape(make_workload("many_levels", small_params()));
    EXPECT_EQ(s.stats.trades, 0u);
    EXPECT_GT(s.bid_levels, 500u);
    EXPECT_GT(s.ask_levels, 500u);
}

TEST(BenchWorkloads, BurstyHasMultiLevelSweeps) {
    const Shape s = run_shape(make_workload("bursty", small_params()));
    EXPECT_GT(s.stats.trades, 100u);
    EXPECT_GE(s.max_trades_in_one_op, 3u);
}
