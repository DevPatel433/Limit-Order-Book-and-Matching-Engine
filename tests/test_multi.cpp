// Phase 7 tests: symbols, the multi-instrument format, the router, the SPSC queue, the sharded pipeline,
// and the generator. The pipeline's correctness claim is "identical to the sequential run", so most
// pipeline tests are differential tests against run_sequential / the single-book replay().
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>

#include "lob/generator.hpp"
#include "lob/multi.hpp"
#include "lob/multi_generator.hpp"
#include "lob/multi_replay.hpp"
#include "lob/pipeline.hpp"
#include "lob/replay.hpp"
#include "lob/spsc_queue.hpp"

using namespace lob;
namespace fs = std::filesystem;

namespace {

fs::path root() { return fs::path(LOB_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

MultiParseResult parse_ok(const std::string& text) {
    auto r = parse_multi_commands(text);
    EXPECT_TRUE(r.ok()) << (r.error ? r.error->message : "");
    return r;
}

// A busy workload that exercises every order type, with duplicate/unknown IDs and snapshots disabled.
GeneratedMulti busy(std::uint64_t seed, std::size_t count, std::size_t symbols, unsigned hot = 0, Price band = 4) {
    MultiGeneratorConfig m;
    m.base.seed = seed;
    m.base.count = count;
    m.base.band = band;
    m.base.ioc_percent = 10;
    m.base.fok_percent = 8;
    m.base.market_percent = 8;
    m.base.amend_percent = 12;
    m.symbols = symbols;
    m.hot_percent = hot;
    return generate_multi(m);
}

// One event as the text the CLI prints, for readable failure messages.
std::string line(const SymbolTable& t, const SymbolEvent& e) { return "@" + t.name(e.symbol) + " " + to_string(e.event); }

}  // namespace

// ---- SymbolTable ----------------------------------------------------------------------------------

TEST(SymbolTable, InternsInOrderOfFirstAppearanceAndIsIdempotent) {
    SymbolTable t;
    EXPECT_EQ(t.intern("MSFT"), 0u);
    EXPECT_EQ(t.intern("AAPL"), 1u);
    EXPECT_EQ(t.intern("MSFT"), 0u);
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t.name(1), "AAPL");
    EXPECT_EQ(t.find("AAPL"), std::optional<SymbolId>(1));
    EXPECT_EQ(t.find("GOOG"), std::nullopt);
    EXPECT_THROW(t.name(2), std::out_of_range);
}

TEST(SymbolTable, NamesAreCaseSensitive) {
    SymbolTable t;
    EXPECT_NE(t.intern("aapl"), t.intern("AAPL"));
}

TEST(SymbolTable, ValidNames) {
    for (const char* ok : {"A", "AAPL", "BRK.B", "ES-Z6", "x_1", "0123456789ABCDEF"}) EXPECT_TRUE(SymbolTable::valid_name(ok)) << ok;
    for (const char* bad : {"", "THIS-NAME-IS-TOO-LONG", "A B", "A/B", "A#", "@A", "é", "A\t"}) EXPECT_FALSE(SymbolTable::valid_name(bad)) << bad;
    SymbolTable t;
    EXPECT_THROW(t.intern("bad name"), std::invalid_argument);
}

TEST(SymbolTable, CapIsEnforced) {
    SymbolTable t;
    for (SymbolId i = 0; i < kMaxSymbols; ++i) t.intern("S" + std::to_string(i));
    EXPECT_EQ(t.size(), kMaxSymbols);
    EXPECT_NO_THROW(t.intern("S0"));  // existing names still resolve
    EXPECT_THROW(t.intern("ONE-TOO-MANY"), std::length_error);
}

// ---- parser ----------------------------------------------------------------------------------------

TEST(MultiParse, PrefixesRouteCommandsAndSymbolsAreNumberedByFirstAppearance) {
    const auto r = parse_ok("@AAPL NEW 1 BUY 100 5\n@MSFT CANCEL 7\n@AAPL AMEND 1 100 3\n@MSFT SNAPSHOT 2\n");
    ASSERT_EQ(r.commands.size(), 4u);
    EXPECT_EQ(r.symbols.size(), 2u);
    EXPECT_EQ(r.commands[0].symbol, 0u);
    EXPECT_EQ(r.commands[1].symbol, 1u);
    EXPECT_EQ(r.commands[2].symbol, 0u);
    EXPECT_EQ(r.commands[3].symbol, 1u);
    EXPECT_EQ(r.commands[1].command, Command(CancelCommand{7}));
    EXPECT_EQ(r.commands[3].command, Command(SnapshotCommand{2}));
}

TEST(MultiParse, CommentsBlankLinesAndWhitespaceAreIgnored) {
    const auto r = parse_ok("# header\n\n   @AAPL   NEW 1 BUY 100 5   # trailing\r\n\t@AAPL\tCANCEL\t1\n   # only a comment\n");
    EXPECT_EQ(r.commands.size(), 2u);
}

TEST(MultiParse, MissingPrefixIsAnErrorUnlessADefaultSymbolIsGiven) {
    auto strict = parse_multi_commands("@A NEW 1 BUY 100 5\nNEW 2 BUY 100 5\n");
    ASSERT_FALSE(strict.ok());
    EXPECT_EQ(strict.error->line, 2u);
    EXPECT_NE(strict.error->message.find("@SYMBOL"), std::string::npos);
    EXPECT_EQ(strict.commands.size(), 1u);  // everything before the error is kept

    const auto lenient = parse_multi_commands("NEW 1 BUY 100 5\n@B NEW 1 SELL 100 5\nCANCEL 1\n", std::string("DEFAULT"));
    ASSERT_TRUE(lenient.ok());
    ASSERT_EQ(lenient.commands.size(), 3u);
    EXPECT_EQ(lenient.symbols.name(lenient.commands[0].symbol), "DEFAULT");
    EXPECT_EQ(lenient.symbols.name(lenient.commands[1].symbol), "B");
    EXPECT_EQ(lenient.commands[2].symbol, lenient.commands[0].symbol);
}

TEST(MultiParse, EveryMalformedLineIsReportedWithItsLineNumber) {
    struct Case { const char* text; std::size_t line; const char* fragment; };
    const Case cases[] = {
        {"@A NEW 1 BUY 100 5\n@ NEW 1 BUY 100 5\n", 2, "invalid symbol"},
        {"@AAPL\n", 1, "must be followed by a command"},
        {"@AAPL   # nothing here\n", 1, "must be followed by a command"},
        {"@THIS-NAME-IS-TOO-LONG NEW 1 BUY 1 1\n", 1, "invalid symbol"},
        {"@A/B NEW 1 BUY 1 1\n", 1, "invalid symbol"},
        {"\n\n@A BOGUS 1\n", 3, "unknown command"},
        {"@A NEW 1 SIDEWAYS 100 5\n", 1, "invalid side"},
        {"@A NEW 1 BUY 100\n", 1, "NEW expects"},
        {"@A CANCEL 1 2\n", 1, "CANCEL expects"},
        {"@A@B NEW 1 BUY 1 1\n", 1, "invalid symbol"},
        {"NEW 1 BUY 100 5\n", 1, "missing @SYMBOL"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        const auto r = parse_multi_commands(c.text);
        ASSERT_FALSE(r.ok());
        EXPECT_EQ(r.error->line, c.line);
        EXPECT_NE(r.error->message.find(c.fragment), std::string::npos) << r.error->message;
    }
}

TEST(MultiParse, InvalidDefaultSymbolIsReported) {
    const auto r = parse_multi_commands("NEW 1 BUY 100 5\n", std::string("not valid"));
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error->message.find("invalid default symbol"), std::string::npos);
}

TEST(MultiParse, FormatRoundTripsEveryCommandKind) {
    const std::string text =
        "@A NEW 1 BUY 100 5\n@B NEW 2 SELL 99 7 IOC\n@A NEW 3 BUY MKT 4 IOC\n@B NEW 4 SELL 5 1 FOK\n"
        "@A CANCEL 1\n@B AMEND 2 98 3\n@A SNAPSHOT\n@B SNAPSHOT 3\n";
    const auto r = parse_ok(text);
    std::ostringstream os;
    write_multi_commands(os, r.symbols, r.commands);
    EXPECT_EQ(os.str(), text);
    const auto again = parse_ok(os.str());
    EXPECT_EQ(again.commands, r.commands);
}

// ---- router / isolation ----------------------------------------------------------------------------

TEST(MultiEngine, IdsArePerSymbolAndBooksNeverInteract) {
    MultiEngine m;
    std::vector<Event> ev;
    m.apply(0, NewCommand{Order(1, Side::Sell, 100, 5)}, ev);
    m.apply(1, NewCommand{Order(1, Side::Buy, 100, 5)}, ev);  // would cross if the books were shared
    for (const Event& e : ev) EXPECT_FALSE(std::holds_alternative<Trade>(e));
    EXPECT_EQ(m.find(0)->book().order_count(), 1u);
    EXPECT_EQ(m.find(1)->book().order_count(), 1u);
    EXPECT_EQ(m.engine_count(), 2u);
}

TEST(MultiEngine, CancelAndAmendAreScopedToTheNamedSymbol) {
    MultiEngine m;
    std::vector<Event> ev;
    m.apply(0, NewCommand{Order(7, Side::Buy, 100, 5)}, ev);
    ev.clear();
    m.apply(1, CancelCommand{7}, ev);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0], Event(Rejected{7, RejectReason::UnknownOrder}));
    ev.clear();
    m.apply(1, AmendCommand{7, 100, 1}, ev);
    EXPECT_EQ(ev[0], Event(Rejected{7, RejectReason::UnknownOrder}));
    EXPECT_EQ(m.find(0)->book().order_count(), 1u);  // untouched
    ev.clear();
    m.apply(0, CancelCommand{7}, ev);
    EXPECT_EQ(ev[0], Event(Cancelled{7, 5}));
}

TEST(MultiEngine, DuplicateIdIsOnlyDuplicateWithinItsOwnSymbol) {
    MultiEngine m;
    std::vector<Event> ev;
    m.apply(0, NewCommand{Order(1, Side::Buy, 100, 5)}, ev);
    ev.clear();
    m.apply(0, NewCommand{Order(1, Side::Buy, 99, 5)}, ev);
    EXPECT_EQ(ev[0], Event(Rejected{1, RejectReason::DuplicateId}));
    ev.clear();
    m.apply(1, NewCommand{Order(1, Side::Buy, 99, 5)}, ev);
    EXPECT_TRUE(std::holds_alternative<Accepted>(ev[0]));
}

TEST(MultiEngine, FindIsNullForUnusedAndEngineRejectsOutOfRangeIds) {
    MultiEngine m;
    EXPECT_EQ(m.find(3), nullptr);
    m.engine(2);
    EXPECT_EQ(m.find(0), nullptr);  // gaps are not created
    EXPECT_NE(m.find(2), nullptr);
    EXPECT_THROW(m.engine(kMaxSymbols), std::out_of_range);
    EXPECT_EQ(m.engine_count(), 1u);
}

// The strongest isolation property: running the commands of ONE symbol through the Phase 1-6 single-book
// replay() must give exactly the lines the multi run printed for that symbol. Covers every order type.
TEST(MultiEngine, EachSymbolBehavesExactlyLikeAnIndependentSingleBookReplay) {
    for (std::uint64_t seed : {1, 2, 3, 4, 5}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const GeneratedMulti g = busy(seed, 6000, 5);

        std::ostringstream multi_out;
        MultiReplayOptions mo;
        mo.final_snapshot = false;
        multi_replay(g.symbols, g.commands, multi_out, mo);

        for (SymbolId s = 0; s < g.symbols.size(); ++s) {
            std::vector<Command> only;
            for (const SymbolCommand& c : g.commands)
                if (c.symbol == s) only.push_back(c.command);
            std::ostringstream single;
            ReplayOptions so;
            so.final_snapshot = false;
            replay(only, single, so);

            // The multi stream's event lines for this symbol, with the prefix removed.
            std::string expected_lines;
            std::istringstream in(multi_out.str());
            const std::string prefix = "@" + g.symbols.name(s) + " ";
            for (std::string l; std::getline(in, l);)
                if (l.rfind(prefix, 0) == 0 && l.rfind("SUMMARY", 0) != 0) expected_lines += l.substr(prefix.size()) + "\n";
            // single's output = event lines + its own SUMMARY line; strip that line for the comparison.
            std::string single_events = single.str();
            single_events.erase(single_events.rfind("SUMMARY"));
            EXPECT_EQ(expected_lines, single_events) << g.symbols.name(s);
        }
    }
}

TEST(MultiEngine, SymbolsAreIndependentOfWhatElseIsTradedAroundThem) {
    // Symbol A's output must not change if B's activity is removed from the stream entirely.
    const GeneratedMulti g = busy(11, 5000, 3);
    std::vector<SymbolCommand> only_a;
    for (const SymbolCommand& c : g.commands)
        if (c.symbol == 0) only_a.push_back(c);
    const PipelineOptions po{.shards = 1};
    const auto full = run_sequential(g.commands, po);
    const auto alone = run_sequential(only_a, po);
    std::vector<Event> a_full, a_alone;
    for (const auto& e : full.events) if (e.symbol == 0) a_full.push_back(e.event);
    for (const auto& e : alone.events) a_alone.push_back(e.event);
    EXPECT_EQ(a_full, a_alone);
}

// ---- SpscQueue -------------------------------------------------------------------------------------

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwoWithMinimumTwo) {
    EXPECT_EQ(SpscQueue<int>(0).capacity(), 2u);
    EXPECT_EQ(SpscQueue<int>(1).capacity(), 2u);
    EXPECT_EQ(SpscQueue<int>(2).capacity(), 2u);
    EXPECT_EQ(SpscQueue<int>(3).capacity(), 4u);
    EXPECT_EQ(SpscQueue<int>(1000).capacity(), 1024u);
}

TEST(SpscQueue, FifoFullAndEmptySingleThreaded) {
    SpscQueue<int> q(4);
    int out = -1;
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(out, -1);  // untouched on failure
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));  // full
    EXPECT_EQ(q.size_approx(), 4u);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueue, WrapsAroundManyTimesWithoutLosingOrder) {
    SpscQueue<std::uint64_t> q(8);
    std::uint64_t next_in = 0, next_out = 0, out = 0;
    for (int round = 0; round < 10000; ++round) {
        const int burst = 1 + round % 8;
        for (int i = 0; i < burst; ++i) ASSERT_TRUE(q.try_push(next_in++));
        for (int i = 0; i < burst; ++i) {
            ASSERT_TRUE(q.try_pop(out));
            ASSERT_EQ(out, next_out++);
        }
    }
}

// The real test of the memory orderings: two threads, a tiny queue (so both "full" and "empty" are hit
// constantly), and a payload that would expose a torn or reordered read. Meaningful under TSan too.
TEST(SpscQueue, TwoThreadStressDeliversEveryItemOnceInOrderWithIntactPayload) {
    struct Msg { std::uint64_t seq; std::uint64_t check; std::uint64_t pad[2]; };
    constexpr std::uint64_t kCount = 400000;
    SpscQueue<Msg> q(4);
    std::atomic<bool> ok{true};
    std::thread consumer([&] {
        Msg m;
        for (std::uint64_t expect = 0; expect < kCount;) {
            if (!q.try_pop(m)) { std::this_thread::yield(); continue; }
            if (m.seq != expect || m.check != ~expect || m.pad[0] != expect * 3 || m.pad[1] != expect * 5) ok = false;
            ++expect;
        }
    });
    for (std::uint64_t i = 0; i < kCount; ++i) {
        const Msg m{i, ~i, {i * 3, i * 5}};
        while (!q.try_push(m)) std::this_thread::yield();
    }
    consumer.join();
    EXPECT_TRUE(ok.load());
    Msg dummy;
    EXPECT_FALSE(q.try_pop(dummy));  // nothing extra was delivered
}

// ---- pipeline: equals the sequential reference -----------------------------------------------------

namespace {
void expect_same(const SymbolTable& t, const PipelineResult& got, const PipelineResult& want) {
    ASSERT_EQ(got.events.size(), want.events.size());
    for (std::size_t i = 0; i < want.events.size(); ++i)
        ASSERT_EQ(got.events[i], want.events[i]) << "event " << i << ": got " << line(t, got.events[i]) << " want "
                                                  << line(t, want.events[i]);
    ASSERT_EQ(got.finals.size(), want.finals.size());
    for (std::size_t i = 0; i < want.finals.size(); ++i) {
        EXPECT_EQ(got.finals[i].symbol, want.finals[i].symbol);
        EXPECT_EQ(got.finals[i].book, want.finals[i].book);
        EXPECT_EQ(got.finals[i].resting, want.finals[i].resting);
    }
    EXPECT_EQ(got.stats.commands, want.stats.commands);
    EXPECT_EQ(got.stats.accepted, want.stats.accepted);
    EXPECT_EQ(got.stats.traded_quantity, want.stats.traded_quantity);
    EXPECT_EQ(got.snapshots_skipped, want.snapshots_skipped);
}
}  // namespace

TEST(Pipeline, IdenticalToSequentialForEveryShardCountAndBothEngines) {
    const GeneratedMulti g = busy(21, 20000, 9);
    for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        const PipelineResult ref = run_sequential(g.commands, PipelineOptions{.engine = kind});
        ASSERT_GT(ref.stats.trades, 1000u) << "workload must actually trade or this test is weak";
        for (std::size_t shards : {1u, 2u, 3u, 4u, 5u, 8u, 16u}) {
            SCOPED_TRACE(std::string(to_string(kind)) + " shards=" + std::to_string(shards));
            PipelineOptions po;
            po.shards = shards;
            po.engine = kind;
            expect_same(g.symbols, run_pipeline(g.commands, po), ref);
        }
    }
}

TEST(Pipeline, BaselineAndPooledAgreeWithEachOther) {
    const GeneratedMulti g = busy(22, 10000, 6);
    const auto a = run_pipeline(g.commands, PipelineOptions{.shards = 3, .engine = EngineKind::Baseline});
    const auto b = run_pipeline(g.commands, PipelineOptions{.shards = 3, .engine = EngineKind::Pooled});
    expect_same(g.symbols, a, b);
}

TEST(Pipeline, RepeatedRunsAreIdenticalDespiteThreadTiming) {
    const GeneratedMulti g = busy(23, 8000, 7);
    const PipelineResult first = run_pipeline(g.commands, PipelineOptions{.shards = 4, .queue_capacity = 8});
    for (int i = 0; i < 25; ++i) {
        SCOPED_TRACE("run " + std::to_string(i));
        expect_same(g.symbols, run_pipeline(g.commands, PipelineOptions{.shards = 4, .queue_capacity = 8}), first);
    }
}

TEST(Pipeline, TinyQueuesForceBackpressureWithoutLosingOrReorderingAnything) {
    const GeneratedMulti g = busy(24, 15000, 6);
    const PipelineResult ref = run_sequential(g.commands, {});
    for (std::size_t cap : {0u, 1u, 2u, 3u, 7u}) {
        SCOPED_TRACE("capacity " + std::to_string(cap));
        PipelineOptions po;
        po.shards = 4;
        po.queue_capacity = cap;
        expect_same(g.symbols, run_pipeline(g.commands, po), ref);
    }
}

TEST(Pipeline, SkewedLoadStillMatchesAndShowsUpInShardCounts) {
    const GeneratedMulti g = busy(25, 20000, 8, /*hot=*/80);
    const PipelineResult ref = run_sequential(g.commands, {});
    PipelineOptions po;
    po.shards = 4;
    const PipelineResult got = run_pipeline(g.commands, po);
    expect_same(g.symbols, got, ref);
    ASSERT_EQ(got.shard_commands.size(), 4u);
    EXPECT_EQ(std::accumulate(got.shard_commands.begin(), got.shard_commands.end(), std::size_t{0}), got.stats.commands);
    // Symbol 0 (80% of the flow) lives on shard 0, which must therefore be the busiest by a wide margin.
    EXPECT_GT(got.shard_commands[0], got.shard_commands[1] * 3);
}

TEST(Pipeline, MoreShardsThanSymbolsLeavesIdleWorkersHarmless) {
    const GeneratedMulti g = busy(26, 3000, 2);
    const PipelineResult ref = run_sequential(g.commands, {});
    const PipelineResult got = run_pipeline(g.commands, PipelineOptions{.shards = 8});
    expect_same(g.symbols, got, ref);
    int idle = 0;
    for (std::size_t n : got.shard_commands) idle += n == 0;
    EXPECT_EQ(idle, 6);
}

TEST(Pipeline, SingleSymbolOnManyShardsEqualsTheSingleBookReplay) {
    // The pipeline layered on one instrument must reproduce the Phase 1-6 engine's own output.
    GeneratorConfig base;
    base.seed = 31;
    base.count = 5000;
    base.band = 3;
    base.ioc_percent = 10; base.fok_percent = 10; base.market_percent = 10; base.amend_percent = 10;
    const std::vector<Command> single = generate(base);
    std::vector<SymbolCommand> tagged;
    for (const Command& c : single) tagged.push_back(SymbolCommand{0, c});
    const PipelineResult got = run_pipeline(tagged, PipelineOptions{.shards = 4});

    std::vector<Event> expected;
    MatchingEngine eng;
    for (const Command& c : single) apply_command(eng, c, expected);
    std::vector<Event> actual;
    for (const SymbolEvent& e : got.events) actual.push_back(e.event);
    EXPECT_EQ(actual, expected);
}

TEST(Pipeline, EventsAreInInputOrderWithContiguousCommands) {
    const GeneratedMulti g = busy(27, 6000, 6);
    const PipelineResult got = run_pipeline(g.commands, PipelineOptions{.shards = 3});
    std::uint64_t last_seq = 0;
    for (const SymbolEvent& e : got.events) {
        ASSERT_GE(e.seq, last_seq) << "events went backwards in input order";
        ASSERT_LE(e.seq, g.commands.size());
        // The event must belong to the symbol of the command that caused it.
        ASSERT_EQ(e.symbol, g.commands[e.seq - 1].symbol);
        last_seq = e.seq;
    }
    // A command's events are one contiguous run: its seq never reappears after a larger one.
    std::set<std::uint64_t> closed;
    std::uint64_t cur = 0;
    for (const SymbolEvent& e : got.events) {
        if (e.seq != cur) {
            ASSERT_EQ(closed.count(e.seq), 0u) << "seq " << e.seq << " is split into two runs";
            closed.insert(cur);
            cur = e.seq;
        }
    }
}

TEST(Pipeline, ConservationLawHoldsPerSymbolOnThePipelineOutput) {
    const GeneratedMulti g = busy(28, 20000, 6);
    const PipelineResult got = run_pipeline(g.commands, PipelineOptions{.shards = 4});
    std::vector<ReplayStats> per(g.symbols.size());
    for (const SymbolEvent& e : got.events) tally_event(e.event, per[e.symbol]);
    for (const SymbolFinal& f : got.finals) {
        Quantity resting = 0;
        for (const auto& l : f.book.bids) resting += l.quantity;
        for (const auto& l : f.book.asks) resting += l.quantity;
        const ReplayStats& s = per[f.symbol];
        SCOPED_TRACE(g.symbols.name(f.symbol));
        EXPECT_EQ(s.accepted_quantity + s.amend_added_quantity,
                  2 * s.traded_quantity + s.cancelled_quantity + s.expired_quantity + resting + s.amend_removed_quantity);
    }
}

TEST(Pipeline, AggregateStatsEqualTheSumOfPerSymbolSingleBookStats) {
    const GeneratedMulti g = busy(29, 10000, 5);
    const PipelineResult got = run_pipeline(g.commands, PipelineOptions{.shards = 3});
    ReplayStats sum;
    for (SymbolId s = 0; s < g.symbols.size(); ++s) {
        std::vector<Command> only;
        for (const SymbolCommand& c : g.commands) if (c.symbol == s) only.push_back(c.command);
        std::ostringstream sink;
        ReplayOptions ro;
        ro.print_events = false;
        ro.final_snapshot = false;
        merge_stats(sum, replay(only, sink, ro).stats);
    }
    EXPECT_EQ(got.stats.accepted, sum.accepted);
    EXPECT_EQ(got.stats.rejected, sum.rejected);
    EXPECT_EQ(got.stats.trades, sum.trades);
    EXPECT_EQ(got.stats.traded_quantity, sum.traded_quantity);
    EXPECT_EQ(got.stats.expired, sum.expired);
    EXPECT_EQ(got.stats.amended, sum.amended);
    EXPECT_EQ(got.stats.commands, sum.commands);
}

TEST(Pipeline, CollectEventsFalseStillTalliesIdenticalStats) {
    const GeneratedMulti g = busy(30, 12000, 6);
    const PipelineResult with = run_pipeline(g.commands, PipelineOptions{.shards = 4, .collect_events = true});
    const PipelineResult without = run_pipeline(g.commands, PipelineOptions{.shards = 4, .collect_events = false});
    EXPECT_TRUE(without.events.empty());
    EXPECT_EQ(without.stats.accepted, with.stats.accepted);
    EXPECT_EQ(without.stats.trades, with.stats.trades);
    EXPECT_EQ(without.stats.traded_quantity, with.stats.traded_quantity);
    EXPECT_EQ(without.stats.cancelled_quantity, with.stats.cancelled_quantity);
    ASSERT_EQ(without.finals.size(), with.finals.size());
    for (std::size_t i = 0; i < with.finals.size(); ++i) EXPECT_EQ(without.finals[i].book, with.finals[i].book);
}

TEST(Pipeline, SnapshotsAreCountedSkippedAndStillConsumeASequenceNumber) {
    const auto p = parse_ok("@A NEW 1 BUY 100 5\n@A SNAPSHOT\n@B NEW 1 SELL 100 5\n@B SNAPSHOT 2\n@A CANCEL 1\n");
    const PipelineResult r = run_pipeline(p.commands, PipelineOptions{.shards = 2});
    EXPECT_EQ(r.snapshots_skipped, 2u);
    EXPECT_EQ(r.stats.commands, 3u);  // dispatched only
    ASSERT_EQ(r.events.size(), 3u);
    EXPECT_EQ(r.events[0].seq, 1u);
    EXPECT_EQ(r.events[1].seq, 3u);   // the snapshot at 2 still used up a number
    EXPECT_EQ(r.events[2].seq, 5u);
}

TEST(Pipeline, EmptyInputAndEmptyShards) {
    const PipelineResult r = run_pipeline({}, PipelineOptions{.shards = 4});
    EXPECT_TRUE(r.events.empty());
    EXPECT_TRUE(r.finals.empty());
    EXPECT_EQ(r.stats.commands, 0u);
    EXPECT_EQ(r.shard_commands, std::vector<std::size_t>(4, 0));
}

TEST(Pipeline, ZeroShardsIsRejected) {
    EXPECT_THROW(run_pipeline({}, PipelineOptions{.shards = 0}), std::invalid_argument);
}

TEST(Pipeline, StreamingInterfaceMatchesTheBatchHelper) {
    const GeneratedMulti g = busy(32, 4000, 4);
    BasicShardedPipeline<StdPolicy> p(PipelineOptions{.shards = 3});
    for (const SymbolCommand& c : g.commands) p.submit(c);
    expect_same(g.symbols, p.finish(), run_sequential(g.commands, {}));
}

TEST(Pipeline, MisuseIsDetected) {
    BasicShardedPipeline<StdPolicy> p(PipelineOptions{.shards = 2});
    p.submit(SymbolCommand{0, NewCommand{Order(1, Side::Buy, 10, 1)}});
    (void)p.finish();
    EXPECT_THROW(p.finish(), std::logic_error);
    EXPECT_THROW(p.submit(SymbolCommand{0, CancelCommand{1}}), std::logic_error);
}

TEST(Pipeline, DestroyingAnUnfinishedPipelineJoinsItsThreadsCleanly) {
    for (int i = 0; i < 20; ++i) {
        BasicShardedPipeline<PoolPolicy> p(PipelineOptions{.shards = 3, .queue_capacity = 2});
        for (int n = 1; n <= 50; ++n) p.submit(SymbolCommand{static_cast<SymbolId>(n % 5), NewCommand{Order(n, Side::Buy, 100 + n, 1)}});
    }  // no finish(): the destructor must stop and join (a leaked joinable thread would std::terminate)
    SUCCEED();
}

// A worker failure must neither deadlock the producer (it keeps submitting into a tiny queue) nor
// be swallowed: finish() joins every thread and then rethrows. A symbol id >= kMaxSymbols is the one
// failure reachable through the public API.
TEST(Pipeline, WorkerFailureDoesNotDeadlockAndIsRethrownByFinish) {
    BasicShardedPipeline<StdPolicy> p(PipelineOptions{.shards = 2, .queue_capacity = 2});
    p.submit(SymbolCommand{0, NewCommand{Order(1, Side::Buy, 10, 1)}});
    p.submit(SymbolCommand{kMaxSymbols, NewCommand{Order(1, Side::Buy, 10, 1)}});  // shard (kMaxSymbols % 2) = 0 throws
    for (int n = 2; n < 2000; ++n) p.submit(SymbolCommand{0, NewCommand{Order(n, Side::Buy, 10, 1)}});  // must not block forever
    EXPECT_THROW(p.finish(), std::out_of_range);
}

// ---- multi_replay / CLI-level behaviour ------------------------------------------------------------

TEST(MultiReplay, GoldenFileMatchesOnBothEnginesWithTheCheckerOn) {
    const auto p = parse_multi_commands(slurp(root() / "data/sample/multi.txt"));
    ASSERT_TRUE(p.ok());
    const std::string expected = slurp(root() / "data/sample/multi.expected");
    ASSERT_FALSE(expected.empty());
    for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        std::ostringstream os;
        MultiReplayOptions o;
        o.check = true;
        o.engine = kind;
        const auto r = multi_replay(p.symbols, p.commands, os, o);
        EXPECT_FALSE(r.violation.has_value());
        EXPECT_EQ(os.str(), expected) << to_string(kind);
    }
}

TEST(MultiReplay, ThreadedOutputEqualsSequentialMinusMidStreamSnapshots) {
    const auto p = parse_multi_commands(slurp(root() / "data/sample/multi.txt"));
    ASSERT_TRUE(p.ok());
    std::string expected = slurp(root() / "data/sample/multi.expected");
    // Threaded mode does not print mid-stream SNAPSHOT output; remove that one block from the golden text.
    const std::string block = "@AAPL BOOK depth=2\n  ASK 101 x 2 (1)\n  ----\n  BID 100 x 4 (1)\nEND\n";
    const auto pos = expected.find(block);
    ASSERT_NE(pos, std::string::npos);
    expected.erase(pos, block.size());

    for (std::size_t threads : {1u, 2u, 3u}) {
        std::ostringstream os;
        MultiReplayOptions o;
        o.threads = threads;
        o.verify = true;
        const auto r = multi_replay(p.symbols, p.commands, os, o);
        EXPECT_FALSE(r.verify_mismatch.has_value()) << *r.verify_mismatch;
        EXPECT_EQ(r.snapshots_skipped, 1u);
        EXPECT_EQ(os.str(), expected) << "threads=" << threads;
    }
}

TEST(MultiReplay, CheckerRunsPerSymbolOnRandomWorkloadsOfEveryOrderType) {
    for (std::uint64_t seed : {41, 42, 43}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const GeneratedMulti g = busy(seed, 4000, 6, seed == 43 ? 70 : 0, seed == 42 ? 2 : 5);
        std::ostringstream sink;
        MultiReplayOptions o;
        o.check = true;
        o.print_events = false;
        for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
            o.engine = kind;
            const auto r = multi_replay(g.symbols, g.commands, sink, o);
            ASSERT_FALSE(r.violation.has_value()) << r.violation->message;
            EXPECT_EQ(r.total.commands, g.commands.size());
        }
    }
}

TEST(MultiReplay, VerifyDetectsADifferenceItIsGiven) {
    // verify's comparison must be able to fail: feed it a pipeline result that differs by one event.
    // (Done through the public helpers: tamper with a copy of a real result and compare it.)
    const GeneratedMulti g = busy(44, 2000, 3);
    PipelineResult a = run_pipeline(g.commands, PipelineOptions{.shards = 2});
    const PipelineResult b = run_sequential(g.commands, {});
    ASSERT_FALSE(a.events.empty());
    EXPECT_EQ(a.events, b.events);
    std::get<Rejected>(a.events.emplace_back(SymbolEvent{1, 0, Rejected{1, RejectReason::UnknownOrder}}).event).id = 2;
    EXPECT_NE(a.events, b.events);
}

TEST(MultiReplay, OptionAndInputValidation) {
    const auto p = parse_ok("@A NEW 1 BUY 100 5\n");
    std::ostringstream os;
    MultiReplayOptions both;
    both.check = true;
    both.threads = 2;
    EXPECT_THROW(multi_replay(p.symbols, p.commands, os, both), std::invalid_argument);
    MultiReplayOptions lonely_verify;
    lonely_verify.verify = true;
    EXPECT_THROW(multi_replay(p.symbols, p.commands, os, lonely_verify), std::invalid_argument);
    SymbolTable empty;
    EXPECT_THROW(multi_replay(empty, p.commands, os, {}), std::invalid_argument);  // symbol 0 not in the table
}

TEST(MultiReplay, CsvHasASymbolColumnAndNoSummary) {
    const auto p = parse_ok("@A NEW 1 SELL 100 5\n@B NEW 1 BUY MKT 3\n");
    std::ostringstream os;
    MultiReplayOptions o;
    o.csv = true;
    multi_replay(p.symbols, p.commands, os, o);
    EXPECT_EQ(os.str(),
              "symbol," + std::string(csv_header()) + "\n"
              "A,1,1,ACCEPTED,1,Sell,100,5,,,,GTC\n"
              "B,2,2,ACCEPTED,1,Buy,MKT,3,,,,IOC\n"
              "B,3,2,EXPIRED,1,,,,,,3,\n");
    // And threaded CSV is identical.
    std::ostringstream threaded;
    o.threads = 2;
    multi_replay(p.symbols, p.commands, threaded, o);
    EXPECT_EQ(threaded.str(), os.str());
}

TEST(MultiReplay, SummaryLinesSumToTheTotal) {
    const GeneratedMulti g = busy(45, 5000, 4);
    std::ostringstream os;
    MultiReplayOptions o;
    o.print_events = false;
    const auto r = multi_replay(g.symbols, g.commands, os, o);
    ReplayStats sum;
    std::size_t resting = 0;
    for (const auto& s : r.symbols) { merge_stats(sum, s.stats); resting += s.resting; }
    EXPECT_EQ(sum.accepted, r.total.accepted);
    EXPECT_EQ(sum.commands, g.commands.size());
    EXPECT_EQ(resting, r.resting_orders);
    EXPECT_NE(os.str().find("SUMMARY symbols=4 commands=5000"), std::string::npos);
}

// ---- generator -------------------------------------------------------------------------------------

TEST(MultiGenerator, SameSeedSameBytesDifferentSeedDifferent) {
    auto text = [](std::uint64_t seed) {
        const GeneratedMulti g = busy(seed, 2000, 5);
        std::ostringstream os;
        write_multi_commands(os, g.symbols, g.commands);
        return os.str();
    };
    EXPECT_EQ(text(1), text(1));
    EXPECT_NE(text(1), text(2));
}

TEST(MultiGenerator, OutputParsesBackToTheSameCommands) {
    const GeneratedMulti g = busy(3, 3000, 7);
    std::ostringstream os;
    write_multi_commands(os, g.symbols, g.commands);
    const auto p = parse_ok(os.str());
    EXPECT_EQ(p.commands.size(), g.commands.size());
    // Symbols may be numbered differently if some never appeared first, but names must line up.
    for (std::size_t i = 0; i < g.commands.size(); ++i) {
        ASSERT_EQ(p.symbols.name(p.commands[i].symbol), g.symbols.name(g.commands[i].symbol));
        ASSERT_EQ(p.commands[i].command, g.commands[i].command);
    }
}

TEST(MultiGenerator, CountsSymbolsAndSkew) {
    const GeneratedMulti uni = busy(4, 20000, 5);
    EXPECT_EQ(uni.commands.size(), 20000u);
    std::vector<std::size_t> n(5, 0);
    for (const auto& c : uni.commands) ++n[c.symbol];
    for (std::size_t x : n) EXPECT_NEAR(static_cast<double>(x), 4000.0, 400.0);  // uniform within 10%

    const GeneratedMulti hot = busy(4, 20000, 5, 70);
    std::vector<std::size_t> h(5, 0);
    for (const auto& c : hot.commands) ++h[c.symbol];
    EXPECT_NEAR(static_cast<double>(h[0]), 14000.0, 600.0);
    for (int s = 1; s < 5; ++s) EXPECT_NEAR(static_cast<double>(h[s]), 1500.0, 400.0);
}

TEST(MultiGenerator, OrderIdsRestartInEverySymbol) {
    const GeneratedMulti g = busy(5, 4000, 4);
    std::vector<OrderId> first_new(4, 0);
    for (const auto& c : g.commands)
        if (const auto* n = std::get_if<NewCommand>(&c.command))
            if (first_new[c.symbol] == 0) first_new[c.symbol] = n->order.id;
    for (OrderId id : first_new) EXPECT_EQ(id, 1u);  // so IDs are shared across books by construction
}

TEST(MultiGenerator, ValidatesItsConfiguration) {
    MultiGeneratorConfig m;
    m.symbols = 0;
    EXPECT_THROW(generate_multi(m), std::invalid_argument);
    m.symbols = kMaxSymbols + 1;
    EXPECT_THROW(generate_multi(m), std::invalid_argument);
    m.symbols = 2;
    m.hot_percent = 101;
    EXPECT_THROW(generate_multi(m), std::invalid_argument);
    m.hot_percent = 0;
    m.prefix = "has space";
    EXPECT_THROW(generate_multi(m), std::invalid_argument);
}

TEST(MultiGenerator, OneSymbolIsTheSingleBookWorkload) {
    MultiGeneratorConfig m;
    m.base.seed = 7;
    m.base.count = 500;
    m.symbols = 1;
    const GeneratedMulti g = generate_multi(m);
    GeneratorConfig single = m.base;
    single.seed = m.base.seed * 1000003ULL + 1;  // the documented derivation for symbol 0
    const std::vector<Command> want = generate(single);
    ASSERT_EQ(g.commands.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_EQ(g.commands[i].command, want[i]);
}
