// Property tests: long seeded random sequences, verified command-by-command by
// the independent ConsistencyChecker, plus global conservation laws.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "lob/generator.hpp"
#include "lob/replay.hpp"

using namespace lob;

namespace {

// On failure, write the exact command sequence to a file that can be fed to
// `lob_replay --check` and then committed under tests/regression/.
std::string save_failure(std::uint64_t seed, const std::vector<Command>& cmds) {
    const auto path = std::filesystem::temp_directory_path() / ("lob_failure_seed" + std::to_string(seed) + ".txt");
    std::ofstream out(path);
    out << "# saved by test_property; seed=" << seed << '\n';
    write_commands(out, cmds);
    return path.string();
}

struct Profile {
    const char* name;
    GeneratorConfig cfg;
};

std::vector<Profile> profiles(std::uint64_t seed) {
    std::vector<Profile> v;
    GeneratorConfig base;
    base.seed = seed;
    base.count = 2000;

    GeneratorConfig crossing = base;  // tight band: almost every order trades
    crossing.band = 2;
    v.push_back({"crossing-heavy", crossing});

    GeneratorConfig resting = base;  // wide band: deep book, few trades
    resting.band = 400;
    resting.mid = 1000;
    v.push_back({"many-levels", resting});

    GeneratorConfig cancels = base;  // cancel storm
    cancels.cancel_percent = 60;
    cancels.band = 5;
    v.push_back({"cancel-heavy", cancels});

    GeneratorConfig sweeps = base;  // big orders that clear many levels
    sweeps.sweep_percent = 30;
    sweeps.band = 8;
    sweeps.max_quantity = 20;
    v.push_back({"sweeps", sweeps});

    GeneratorConfig messy = base;  // lots of malformed / duplicate input
    messy.bad_percent = 25;
    messy.band = 4;
    v.push_back({"messy-input", messy});

    GeneratorConfig ext = base;  // IOC / FOK / market orders and amends mixed into crossing flow
    ext.band = 4;
    ext.ioc_percent = 15;
    ext.fok_percent = 10;
    ext.market_percent = 10;
    ext.amend_percent = 15;
    v.push_back({"extensions", ext});

    GeneratorConfig ext_wide = base;  // same, on a deeper book
    ext_wide.band = 40;
    ext_wide.ioc_percent = 10;
    ext_wide.fok_percent = 10;
    ext_wide.market_percent = 5;
    ext_wide.amend_percent = 20;
    v.push_back({"extensions-wide", ext_wide});

    GeneratorConfig tiny = base;  // price band collapsed to a single tick
    tiny.band = 0;
    tiny.max_quantity = 3;
    v.push_back({"single-price", tiny});
    return v;
}

}  // namespace

TEST(Property, RandomSequencesPreserveAllInvariants) {
    for (std::uint64_t seed : {1u, 2u, 3u, 4u}) {
      for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        for (const Profile& p : profiles(seed)) {
            SCOPED_TRACE(std::string(p.name) + " seed=" + std::to_string(seed) + " engine=" + to_string(kind));
            const auto cmds = generate(p.cfg);

            std::ostringstream sink;
            const ReplayResult r = replay(cmds, sink, ReplayOptions{.print_events = false, .final_snapshot = false, .check = true, .engine = kind});
            if (r.violation) {
                FAIL() << "violation at command " << r.violation->command_index << ": " << r.violation->message
                       << "\nreproduce with: lob_replay " << save_failure(seed, cmds) << " --check";
            }

            // Global conservation: every accepted unit ends up traded (counted once
            // for the maker and once for the taker), cancelled, or still resting.
            Quantity resting = 0;
            for (const auto& l : r.final_book.bids) resting += l.quantity;
            for (const auto& l : r.final_book.asks) resting += l.quantity;
            // accepted + amend_added == 2*traded + cancelled + expired + resting + amend_removed
            EXPECT_EQ(r.stats.accepted_quantity + r.stats.amend_added_quantity,
                      2 * r.stats.traded_quantity + r.stats.cancelled_quantity + r.stats.expired_quantity + resting +
                          r.stats.amend_removed_quantity);
            EXPECT_LE(2 * r.stats.traded_quantity, r.stats.accepted_quantity + r.stats.amend_added_quantity);

            const std::string name = p.name;
            if (name == "crossing-heavy" || name == "sweeps" || name == "single-price") {
                EXPECT_GT(r.stats.trades, 100u) << "workload too tame; test would be weak";
            }
            if (name == "messy-input") {
                EXPECT_GT(r.stats.rejected, 100u);
            }
            if (name == "cancel-heavy") {
                EXPECT_GT(r.stats.cancelled, 100u);
            }
            if (name == "extensions" || name == "extensions-wide") {
                EXPECT_GT(r.stats.expired, 100u);
                EXPECT_GT(r.stats.amended, 40u);
                EXPECT_GT(r.stats.trades, 100u);
            }
            if (name == "many-levels") {
                EXPECT_GT(r.final_book.bids.size() + r.final_book.asks.size(), 50u);
            }
        }
      }
    }
}

TEST(Property, ReplayOfSerialisedFileEqualsReplayOfOriginalCommands) {
    GeneratorConfig cfg;
    cfg.seed = 2025;
    cfg.count = 1500;
    cfg.snapshot_every = 400;
    const auto cmds = generate(cfg);

    std::ostringstream file;
    write_commands(file, cmds);
    const auto parsed = parse_commands(file.str());
    ASSERT_TRUE(parsed.ok());

    std::ostringstream a, b;
    replay(cmds, a, ReplayOptions{.check = true});
    replay(parsed.commands, b, ReplayOptions{.check = true});
    EXPECT_EQ(a.str(), b.str());
}
