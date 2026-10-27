#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lob/events.hpp"
#include "lob/generator.hpp"
#include "lob/replay.hpp"

using namespace lob;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

struct ReplayRun {
    ReplayResult result;
    std::string output;
};

ReplayRun run_text(const std::string& text, ReplayOptions opt = {}) {
    const auto parsed = parse_commands(text);
    EXPECT_TRUE(parsed.ok()) << (parsed.error ? parsed.error->message : "");
    std::ostringstream os;
    ReplayRun r;
    r.result = replay(parsed.commands, os, opt);
    r.output = os.str();
    return r;
}

fs::path root() { return fs::path(LOB_SOURCE_DIR); }

}  // namespace

TEST(Replay, HandVerifiedBasicWalkthrough) {
    // Expected output traced by hand from the rules in docs/matching_rules.md.
    const std::string expected =
        "ACCEPTED id=1 Sell 101 x 5\n"
        "ACCEPTED id=2 Sell 100 x 4\n"
        "ACCEPTED id=3 Buy 99 x 10\n"
        "BOOK depth=3\n"
        "  ASK 101 x 5 (1)\n"
        "  ASK 100 x 4 (1)\n"
        "  ----\n"
        "  BID 99 x 10 (1)\n"
        "END\n"
        "ACCEPTED id=4 Buy 101 x 6\n"
        "TRADE maker=2 taker=4 Buy 100 x 4\n"
        "TRADE maker=1 taker=4 Buy 101 x 2\n"
        "CANCELLED id=3 remaining=10\n"
        "REJECTED id=3 UnknownOrder\n"
        "REJECTED id=2 InvalidQuantity\n"
        "BOOK depth=5\n"
        "  ASK 101 x 3 (1)\n"
        "  ----\n"
        "END\n"
        "BOOK depth=5\n"
        "  ASK 101 x 3 (1)\n"
        "  ----\n"
        "END\n"
        "SUMMARY commands=9 accepted=4 rejected=2 trades=2 cancelled=1 traded_qty=6 resting=1\n";
    for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        SCOPED_TRACE(to_string(kind));
        const ReplayRun r = run_text(slurp(root() / "data/sample/basic.txt"), ReplayOptions{.check = true, .engine = kind});
        EXPECT_EQ(r.output, expected);
        EXPECT_FALSE(r.result.violation);
    }
}

TEST(Replay, SweepSampleMatchesHandTrace) {
    const std::string expected =
        "ACCEPTED id=10 Sell 102 x 5\n"
        "ACCEPTED id=11 Sell 100 x 5\n"
        "ACCEPTED id=12 Sell 100 x 5\n"
        "ACCEPTED id=13 Sell 101 x 5\n"
        "ACCEPTED id=14 Buy 98 x 7\n"
        "BOOK depth=5\n"
        "  ASK 102 x 5 (1)\n"
        "  ASK 101 x 5 (1)\n"
        "  ASK 100 x 10 (2)\n"
        "  ----\n"
        "  BID 98 x 7 (1)\n"
        "END\n"
        "ACCEPTED id=15 Buy 101 x 13\n"
        "TRADE maker=11 taker=15 Buy 100 x 5\n"   // FIFO: 11 before 12
        "TRADE maker=12 taker=15 Buy 100 x 5\n"
        "TRADE maker=13 taker=15 Buy 101 x 3\n"   // then next price; partial
        "BOOK depth=5\n"
        "  ASK 102 x 5 (1)\n"
        "  ASK 101 x 2 (1)\n"
        "  ----\n"
        "  BID 98 x 7 (1)\n"
        "END\n"
        "ACCEPTED id=16 Sell 97 x 20\n"
        "TRADE maker=14 taker=16 Sell 98 x 7\n"
        "BOOK depth=5\n"
        "  ASK 102 x 5 (1)\n"
        "  ASK 101 x 2 (1)\n"
        "  ASK 97 x 13 (1)\n"
        "  ----\n"
        "END\n"
        "BOOK depth=5\n"
        "  ASK 102 x 5 (1)\n"
        "  ASK 101 x 2 (1)\n"
        "  ASK 97 x 13 (1)\n"
        "  ----\n"
        "END\n"
        "SUMMARY commands=10 accepted=7 rejected=0 trades=4 cancelled=0 traded_qty=20 resting=3\n";
    for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        SCOPED_TRACE(to_string(kind));
        const ReplayRun r = run_text(slurp(root() / "data/sample/sweep.txt"), ReplayOptions{.check = true, .engine = kind});
        EXPECT_EQ(r.output, expected);
    }
}

TEST(Replay, OptionsControlOutput) {
    const std::string text = "NEW 1 SELL 100 5\nNEW 2 BUY 100 5\n";
    const ReplayRun quiet = run_text(text, ReplayOptions{.print_events = false, .final_snapshot = false});
    EXPECT_EQ(quiet.output, "SUMMARY commands=2 accepted=2 rejected=0 trades=1 cancelled=0 traded_qty=5 resting=0\n");
    const ReplayRun deep = run_text("NEW 1 BUY 1 1\nNEW 2 BUY 2 1\nNEW 3 BUY 3 1\n", ReplayOptions{.print_events = false, .default_depth = 2});
    EXPECT_NE(deep.output.find("BOOK depth=2"), std::string::npos);
    EXPECT_EQ(deep.output.find("BID 1 x"), std::string::npos);  // third level cut off by depth
}

TEST(Replay, SameInputProducesByteIdenticalOutput) {
    GeneratorConfig cfg;
    cfg.seed = 99;
    cfg.count = 1200;
    cfg.snapshot_every = 250;
    const auto cmds = generate(cfg);
    auto once = [&] {
        std::ostringstream os;
        replay(cmds, os, ReplayOptions{.check = true});
        return os.str();
    };
    const std::string a = once();
    EXPECT_EQ(a, once());
    EXPECT_NE(a.find("TRADE"), std::string::npos);
}

TEST(Generator, SameSeedSameCommandsDifferentSeedDiffers) {
    GeneratorConfig a;
    a.seed = 1;
    a.count = 500;
    GeneratorConfig b = a;
    EXPECT_EQ(generate(a), generate(b));
    b.seed = 2;
    EXPECT_NE(generate(a), generate(b));
}

// A hand-traced walk-through of market / IOC / FOK / amend (data/sample/orders.txt). The .expected
// file was checked line by line against docs/matching_rules.md before being committed.
TEST(Replay, OrderTypesSampleMatchesHandTrace) {
    const std::string expected = slurp(root() / "data/sample/orders.expected");
    ASSERT_FALSE(expected.empty());
    for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        SCOPED_TRACE(to_string(kind));
        const ReplayRun r = run_text(slurp(root() / "data/sample/orders.txt"), ReplayOptions{.check = true, .engine = kind});
        EXPECT_EQ(r.output, expected);
        EXPECT_FALSE(r.result.violation);
    }
}

// The README shows the output of data/sample/demo.txt. If the rules or formats change and the README is
// not updated, this fails: documentation that can rot silently is worse than none.
TEST(Docs, ReadmeDemoMatchesTheRealOutput) {
    const std::string readme = slurp(root() / "README.md");
    const std::string expected = slurp(root() / "data/sample/demo.expected");
    ASSERT_FALSE(readme.empty());
    ASSERT_FALSE(expected.empty());
    std::istringstream lines(expected);
    std::string line;
    std::size_t checked = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        EXPECT_NE(readme.find(line), std::string::npos) << "README is missing the demo output line: " << line;
        ++checked;
    }
    EXPECT_GE(checked, 10u);
    for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
        SCOPED_TRACE(to_string(kind));
        EXPECT_EQ(run_text(slurp(root() / "data/sample/demo.txt"), ReplayOptions{.check = true, .engine = kind}).output, expected);
    }
}

TEST(Replay, CsvModeIsAPureEventLog) {
    const std::string text = "NEW 1 SELL 100 5\nNEW 2 BUY MKT 8\nAMEND 9 1 1\nSNAPSHOT\n";
    const ReplayRun r = run_text(text, ReplayOptions{.csv = true});
    EXPECT_EQ(r.output,
              std::string(csv_header()) + "\n"
              "1,1,ACCEPTED,1,Sell,100,5,,,,GTC\n"
              "2,2,ACCEPTED,2,Buy,MKT,8,,,,IOC\n"
              "3,2,TRADE,2,Buy,100,5,1,2,,\n"
              "4,2,EXPIRED,2,,,,,,3,\n"
              "5,3,REJECTED,9,,,,,,,UnknownOrder\n");  // no BOOK, no SUMMARY
}

// Every file in tests/regression is a saved reproduction of a past failure or a
// hand-picked edge case. Each must replay cleanly under the checker, and if a
// matching .expected file exists the output must match it byte for byte.
TEST(Regression, AllSavedCasesReplayCleanly) {
    const fs::path dir = root() / "tests/regression";
    ASSERT_TRUE(fs::is_directory(dir)) << dir;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "no regression cases found; test would be vacuous";

    for (const fs::path& f : files) {
        SCOPED_TRACE(f.filename().string());
        for (EngineKind kind : {EngineKind::Baseline, EngineKind::Pooled}) {
            SCOPED_TRACE(to_string(kind));
            const ReplayRun r = run_text(slurp(f), ReplayOptions{.check = true, .engine = kind});
            if (r.result.violation) {
                ADD_FAILURE() << "violation at command " << r.result.violation->command_index << ": "
                              << r.result.violation->message;
            }
            fs::path expected = f;
            expected.replace_extension(".expected");
            if (fs::exists(expected)) {
                EXPECT_EQ(r.output, slurp(expected));
            }
        }
    }
}
