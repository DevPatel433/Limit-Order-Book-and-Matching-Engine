// lob_scale: does sharding books across threads scale, and what limits it?
//
// Methodology (see docs/benchmarking.md and docs/results_phase7.md):
//   * One pre-generated multi-instrument workload; generation is never timed.
//   * Modes are INTERLEAVED within each repetition (seq, 1 shard, 2 shards, ... then repeat), so slow drift
//     on a noisy machine hits all modes alike instead of whichever ran last.
//   * Every timed run is gated on correctness: its statistics must equal the sequential reference's, or the
//     benchmark aborts. A fast wrong answer is never reported.
//   * Two output modes: "none" (events are only counted; pure matching + queueing cost) and "merged"
//     (events are stored and merged into one ordered stream, as the replay tool does).
//   * Timing covers thread start, dispatch, matching, join and merge. It excludes workload generation.
//   * The environment is recorded, and a row is flagged `oversubscribed=1` when producer + workers
//     outnumber the logical CPUs; no speedup should be expected (or claimed) from those rows.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "environment.hpp"
#include "lob/multi_generator.hpp"
#include "lob/pipeline.hpp"

using namespace lob;

namespace {

struct Options {
    std::size_t symbols{64};
    std::size_t count{1000000};
    unsigned hot_pct{0};
    Price band{5};
    std::vector<std::size_t> shards{1, 2, 4};
    int reps{7};
    EngineKind engine{EngineKind::Baseline};
    std::string out;       // CSV path; empty = stdout
    bool allow_debug{false};
    bool smoke{false};
    std::size_t queue_capacity{4096};
};

template <class T>
bool parse_num(const std::string& s, T& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [--symbols N] [--count N] [--hot-pct P] [--band B] [--shards 1,2,4]\n"
              << "          [--reps R] [--engine baseline|pooled] [--queue-capacity C] [--out FILE.csv]\n"
              << "          [--smoke] [--allow-debug]\n"
              << "Compares run_sequential against the sharded pipeline on one generated workload. Refuses\n"
              << "Debug/sanitizer builds unless --allow-debug (rows are then marked valid_for_comparison=0).\n";
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// The correctness gate: the aggregate statistics are integers, so any divergence is exact.
bool same_stats(const ReplayStats& a, const ReplayStats& b) {
    return a.commands == b.commands && a.accepted == b.accepted && a.rejected == b.rejected && a.trades == b.trades &&
           a.cancelled == b.cancelled && a.expired == b.expired && a.amended == b.amended &&
           a.accepted_quantity == b.accepted_quantity && a.traded_quantity == b.traded_quantity &&
           a.cancelled_quantity == b.cancelled_quantity && a.expired_quantity == b.expired_quantity &&
           a.amend_added_quantity == b.amend_added_quantity && a.amend_removed_quantity == b.amend_removed_quantity;
}

struct Mode {
    std::string name;     // "sequential" or "pipeline"
    std::size_t shards;   // 0 for sequential
    bool collect;
    std::string label() const {
        return (shards == 0 ? std::string("seq") : std::to_string(shards) + "-shard") + (collect ? "/merged" : "/none");
    }
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--smoke") { o.smoke = true; continue; }
        if (a == "--allow-debug") { o.allow_debug = true; continue; }
        if (i + 1 >= argc) { usage(argv[0]); return 1; }
        const std::string v = argv[++i];
        bool ok = true;
        if (a == "--symbols") ok = parse_num(v, o.symbols);
        else if (a == "--count") ok = parse_num(v, o.count);
        else if (a == "--hot-pct") ok = parse_num(v, o.hot_pct);
        else if (a == "--band") ok = parse_num(v, o.band);
        else if (a == "--reps") ok = parse_num(v, o.reps);
        else if (a == "--queue-capacity") ok = parse_num(v, o.queue_capacity);
        else if (a == "--out") o.out = v;
        else if (a == "--engine") {
            if (v == "baseline") o.engine = EngineKind::Baseline;
            else if (v == "pooled") o.engine = EngineKind::Pooled;
            else ok = false;
        } else if (a == "--shards") {
            o.shards.clear();
            std::stringstream ss(v);
            for (std::string tok; std::getline(ss, tok, ',');) {
                std::size_t n = 0;
                if (!parse_num(tok, n) || n == 0) { ok = false; break; }
                o.shards.push_back(n);
            }
            if (o.shards.empty()) ok = false;
        } else ok = false;
        if (!ok) { usage(argv[0]); return 1; }
    }
    if (o.smoke) { o.count = 20000; o.symbols = 8; o.reps = 2; o.shards = {1, 2}; }
    if (o.reps < 1) { usage(argv[0]); return 1; }

    bench::Environment env = bench::collect_environment();
    if (!env.valid_for_comparison() && !o.allow_debug) {
        std::cerr << "Refusing to run: this build has assertions or sanitizers enabled, so timings are meaningless.\n"
                     "Build with -DCMAKE_BUILD_TYPE=Release, or pass --allow-debug (rows are marked valid_for_comparison=0).\n";
        return 1;
    }

    MultiGeneratorConfig mg;
    mg.base.seed = 12345;
    mg.base.count = o.count;
    mg.base.band = o.band;
    mg.base.cancel_percent = 20;
    mg.base.ioc_percent = 5;
    mg.base.market_percent = 3;
    mg.base.amend_percent = 5;
    mg.symbols = o.symbols;
    mg.hot_percent = o.hot_pct;
    const GeneratedMulti g = generate_multi(mg);

    // Reference statistics and the load split each shard count implies (from the workload alone).
    PipelineOptions ref_opt;
    ref_opt.engine = o.engine;
    ref_opt.collect_events = false;
    const PipelineResult ref = run_sequential(g.commands, ref_opt);

    std::map<std::size_t, double> max_share;  // shards -> largest shard's fraction of commands
    for (std::size_t sh : o.shards) {
        std::vector<std::size_t> n(sh, 0);
        for (const SymbolCommand& c : g.commands) ++n[c.symbol % sh];
        max_share[sh] = static_cast<double>(*std::max_element(n.begin(), n.end())) / static_cast<double>(g.commands.size());
    }

    std::vector<Mode> modes;
    for (bool collect : {false, true}) {
        modes.push_back({"sequential", 0, collect});
        for (std::size_t sh : o.shards) modes.push_back({"pipeline", sh, collect});
    }

    std::map<std::string, std::vector<double>> times;  // mode label -> seconds per rep
    std::ostringstream csv;
    csv << "# lob_scale: symbols=" << o.symbols << " commands=" << o.count << " hot_pct=" << o.hot_pct << " band=" << o.band
        << " engine=" << to_string(o.engine) << " queue_capacity=" << o.queue_capacity << " reps=" << o.reps << '\n'
        << "# cpu=" << env.cpu_model << " logical_cpus=" << env.logical_cpus << " os=" << env.os << " compiler=" << env.compiler << '\n'
        << "# build_type=" << env.build_type << " flags=" << env.cxx_flags << " valid_for_comparison=" << (env.valid_for_comparison() ? 1 : 0)
        << " timestamp_utc=" << env.timestamp_utc << '\n'
        << "mode,output,shards,rep,seconds,commands,commands_per_sec,max_shard_share,oversubscribed,valid_for_comparison\n";

    for (int rep = 0; rep < o.reps; ++rep) {
        for (const Mode& m : modes) {
            PipelineOptions po;
            po.engine = o.engine;
            po.collect_events = m.collect;
            po.shards = m.shards == 0 ? 1 : m.shards;
            po.queue_capacity = o.queue_capacity;

            const double t0 = now_seconds();
            const PipelineResult r = m.shards == 0 ? run_sequential(g.commands, po) : run_pipeline(g.commands, po);
            const double secs = now_seconds() - t0;

            if (!same_stats(r.stats, ref.stats) || (m.collect && r.events.empty())) {
                std::cerr << "CORRECTNESS GATE FAILED for " << m.label() << " (rep " << rep << "): statistics differ from the sequential reference\n";
                return 2;
            }
            times[m.label()].push_back(secs);
            const double share = m.shards == 0 ? 1.0 : max_share[m.shards];
            const bool oversub = m.shards != 0 && m.shards + 1 > env.logical_cpus;
            csv << m.name << ',' << (m.collect ? "merged" : "none") << ',' << m.shards << ',' << rep << ',' << std::fixed << std::setprecision(6)
                << secs << ',' << r.stats.commands << ',' << std::setprecision(0) << static_cast<double>(r.stats.commands) / secs << ','
                << std::setprecision(4) << share << ',' << (oversub ? 1 : 0) << ',' << (env.valid_for_comparison() ? 1 : 0) << '\n';
        }
    }

    if (o.out.empty()) std::cout << csv.str();
    else {
        std::ofstream f(o.out);
        f << csv.str();
        std::cout << "wrote " << o.out << '\n';
    }

    // Human-readable summary: medians and speedup versus the sequential run with the SAME output mode.
    std::cout << "\nlob_scale: " << o.count << " commands, " << o.symbols << " symbols, hot_pct=" << o.hot_pct << ", engine=" << to_string(o.engine)
              << ", " << o.reps << " reps, logical CPUs=" << env.logical_cpus << "\n"
              << "workload checked against the sequential reference on every run: " << ref.stats.trades << " trades, "
              << ref.stats.accepted << " accepted\n\n"
              << std::left << std::setw(16) << "mode" << std::right << std::setw(13) << "median s" << std::setw(16) << "commands/s"
              << std::setw(10) << "speedup" << std::setw(15) << "max shard %" << std::setw(18) << "partition bound" << "  note\n";
    for (bool collect : {false, true}) {
        const double base = median(times[Mode{"sequential", 0, collect}.label()]);
        for (const Mode& m : modes) {
            if (m.collect != collect) continue;
            const double med = median(times[m.label()]);
            const double share = m.shards == 0 ? 1.0 : max_share[m.shards];
            std::cout << std::left << std::setw(16) << m.label() << std::right << std::fixed << std::setw(13) << std::setprecision(4) << med << std::setw(16)
                      << std::setprecision(0) << static_cast<double>(o.count) / med << std::setw(9) << std::setprecision(2) << base / med << 'x'
                      << std::setw(14) << std::setprecision(1) << 100.0 * share << '%' << std::setw(17) << std::setprecision(2) << 1.0 / share << 'x' << "  "
                      << (m.shards != 0 && m.shards + 1 > env.logical_cpus ? "OVERSUBSCRIBED: more threads than CPUs, no speedup expected" : "") << '\n';
        }
        std::cout << '\n';
    }
    std::cout << "'partition bound' = 1 / (largest shard's share of commands): the best speedup the symbol->shard split allows even with\n"
                 "free dispatch and merge. Real speedup is lower: the single producer thread and (for 'merged') the single-threaded merge\n"
                 "are serial. Rows marked OVERSUBSCRIBED measure scheduling overhead, not parallel scaling.\n";
    return 0;
}
