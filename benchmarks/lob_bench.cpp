// lob_bench: measure the matching engine on reproducible workloads.
//
// Three separate passes per repetition, each on a fresh engine, so no
// measurement contaminates another:
//   1. throughput  - whole measured segment timed once; no per-operation clock reads
//   2. latency     - every measured operation timed individually (2 clock reads each)
//   3. allocation  - once per workload, untimed; counts allocations and live memory
// Each pass first runs the prefill + warm-up operations untimed.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <string>
#include <vector>

#include "alloc_counter.hpp"
#include "environment.hpp"
#include "lob/matching_engine.hpp"
#include "stats.hpp"
#include "workloads.hpp"

namespace fs = std::filesystem;
using namespace lob;
using namespace lob::bench;
using Clock = std::chrono::steady_clock;
static_assert(Clock::is_steady, "benchmarks need a monotonic clock");

namespace {

struct Options {
    std::string workload{"all"};
    WorkloadParams params;
    int reps{5};
    std::string out_dir{"bench_out"};
    int pin_cpu{-1};
    bool allow_debug{false};
    bool profile_only{false};
    std::vector<EngineKind> engines{EngineKind::Baseline, EngineKind::Pooled};
};

template <class T>
bool parse_num(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc() && p == end;
}

void usage() {
    std::cerr << "usage: lob_bench [--workload NAME|all] [--measured N] [--warmup N] [--reps R] [--seed S]\n"
                 "                 [--out DIR] [--pin-cpu N] [--allow-debug] [--smoke] [--profile]\n"
                 "                 [--engine baseline|pooled|both]   (default both, reps interleaved)\n"
                 "workloads: non_crossing cancel_heavy crossing_heavy many_levels bursty\n"
                 "  --smoke   tiny run for CI; implies nothing about performance\n"
                 "  --profile run only the throughput pass (for perf/callgrind), write no files\n";
}

template <class Engine>
inline void apply(Engine& e, const BenchOp& op, std::vector<Event>& ev) {
    if (op.is_cancel) e.cancel(op.cancel_id, ev);
    else e.submit(op.order, ev);
}

inline std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// ---- passes ---------------------------------------------------------------------

struct Throughput {
    double seconds{};
    std::uint64_t sink{};  // consumed so the work cannot be optimised away
};

// The measured segment of the throughput pass lives in its own non-inlined function so
// profilers can target exactly the timed code (callgrind: --toggle-collect=*lob_measured_segment*;
// perf: look at this symbol's children). It excludes workload generation, prefill and warm-up.
template <class Engine>
[[gnu::noinline]] std::uint64_t lob_measured_segment(Engine& eng, const Workload& w, std::vector<Event>& ev,
                                                    std::size_t from) {
    std::uint64_t sink = 0;
    for (std::size_t i = from; i < w.ops.size(); ++i) {
        ev.clear();
        apply(eng, w.ops[i], ev);
        sink += ev.size();
    }
    return sink;
}

template <class Engine>
Throughput run_throughput_t(const Workload& w) {
    Engine eng;
    std::vector<Event> ev;
    ev.reserve(4096);
    const std::size_t warm = w.unmeasured();
    for (std::size_t i = 0; i < warm; ++i) {
        ev.clear();
        apply(eng, w.ops[i], ev);
    }
    const auto t0 = Clock::now();
    const std::uint64_t sink = lob_measured_segment(eng, w, ev, warm);
    const auto t1 = Clock::now();
    return {std::chrono::duration<double>(t1 - t0).count(), sink + eng.book().order_count()};
}

template <class Engine>
std::vector<std::uint32_t> run_latency_t(const Workload& w) {
    Engine eng;
    std::vector<Event> ev;
    ev.reserve(4096);
    const std::size_t warm = w.unmeasured();
    for (std::size_t i = 0; i < warm; ++i) {
        ev.clear();
        apply(eng, w.ops[i], ev);
    }
    std::vector<std::uint32_t> samples(w.measured());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const BenchOp& op = w.ops[warm + i];
        ev.clear();
        const auto t0 = Clock::now();
        apply(eng, op, ev);
        const auto t1 = Clock::now();
        samples[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(ns_between(t0, t1), UINT32_MAX));
    }
    return samples;
}

// Bytes the C library currently has handed out, INCLUDING per-chunk headers and rounding
// (unlike malloc_usable_size, which hides them). 0 if unsupported.
std::int64_t heap_in_use_bytes() {
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
    const struct mallinfo2 m = mallinfo2();
    return static_cast<std::int64_t>(m.uordblks + m.hblkhd);
#else
    return 0;
#endif
}

struct AllocPass {
    std::uint64_t allocs_measured{}, bytes_measured{};  // during measured ops only
    std::int64_t peak_live_bytes{};                     // whole run (book memory persists)
    std::uint64_t events{}, trades{}, rejected{};
    std::size_t resting{}, bid_levels{}, ask_levels{};
    long rss_kb{-1};
    std::int64_t heap_in_use{0};       // engine heap at end of run (malloc view, incl. overhead)
    std::int64_t heap_after_drain{0};  // ... after cancelling every resting order
};

template <class Engine>
AllocPass run_alloc_t(const Workload& w) {
    AllocPass r;
    alloc::reset();
    alloc::set_enabled(true);
    {
        const std::int64_t heap_base = heap_in_use_bytes();
        Engine eng;
        std::vector<Event> ev;
        ev.reserve(4096);
        const std::size_t warm = w.unmeasured();
        alloc::Stats at_start{};
        for (std::size_t i = 0; i < w.ops.size(); ++i) {
            if (i == warm) at_start = alloc::stats();
            ev.clear();
            apply(eng, w.ops[i], ev);
            if (i >= warm) {
                r.events += ev.size();
                for (const Event& e : ev) {
                    if (std::holds_alternative<Trade>(e)) ++r.trades;
                    else if (std::holds_alternative<Rejected>(e)) ++r.rejected;
                }
            }
        }
        const alloc::Stats end = alloc::stats();
        r.allocs_measured = end.allocations - at_start.allocations;
        r.bytes_measured = end.bytes_requested - at_start.bytes_requested;
        r.peak_live_bytes = end.peak_live_bytes;
        r.resting = eng.book().order_count();
        r.bid_levels = eng.book().level_count(Side::Buy);
        r.ask_levels = eng.book().level_count(Side::Sell);
        r.rss_kb = read_rss_kb();
        r.heap_in_use = heap_in_use_bytes() - heap_base;
        for (const OrderId id : w.live_at_end) {
            ev.clear();
            eng.cancel(id, ev);
        }
        r.heap_after_drain = heap_in_use_bytes() - heap_base;
    }
    alloc::set_enabled(false);
    return r;
}

Throughput run_throughput(EngineKind k, const Workload& w) {
    return k == EngineKind::Pooled ? run_throughput_t<PooledMatchingEngine>(w) : run_throughput_t<MatchingEngine>(w);
}
std::vector<std::uint32_t> run_latency(EngineKind k, const Workload& w) {
    return k == EngineKind::Pooled ? run_latency_t<PooledMatchingEngine>(w) : run_latency_t<MatchingEngine>(w);
}
AllocPass run_alloc(EngineKind k, const Workload& w) {
    return k == EngineKind::Pooled ? run_alloc_t<PooledMatchingEngine>(w) : run_alloc_t<MatchingEngine>(w);
}

// Cost of Clock::now() itself, so readers can judge how much of a small latency is the timer.
double timer_overhead_ns() {
    constexpr int kN = 200'000;
    std::vector<std::uint32_t> d(kN);
    for (int i = 0; i < kN; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        d[i] = static_cast<std::uint32_t>(ns_between(a, b));
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[kN / 2]);
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ---- output ---------------------------------------------------------------------

std::string q(const std::string& s) {  // CSV-quote
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

template <class T>
std::string opt(bool has, T v) {
    return has ? std::to_string(v) : std::string();
}

const char* kMethodology =
    "Methodology\n"
    "-----------\n"
    "* Operations are pre-generated (deterministic from the seed) so generation, parsing and I/O are never timed.\n"
    "* Each pass uses a fresh MatchingEngine. Prefill + warm-up operations run first, untimed; only the\n"
    "  measured segment is recorded. 'Event' in events/sec means one input command (submit or cancel).\n"
    "* Throughput pass: the whole measured segment is timed once with std::chrono::steady_clock; no per-op clock reads.\n"
    "* Latency pass: each operation is timed individually (two steady_clock reads). The cost of the clock\n"
    "  itself is reported as timer_overhead_ns; latencies below a few multiples of it are not meaningful.\n"
    "* Percentiles use nearest-rank on the sorted samples and are reported only when at least 10 samples lie\n"
    "  above them (p99 needs >= 1,000 samples; p99.9 needs >= 10,000). Otherwise the cell is left empty.\n"
    "* Allocation pass: global operator new/delete are counted during the measured segment; peak_live_bytes uses\n"
    "  malloc_usable_size (glibc; excludes malloc headers/rounding). heap_in_use_bytes uses mallinfo2 and includes them;\n"
    "  heap_after_drain_bytes is the same after every resting order has been cancelled.\n"
    "* Repetitions re-run the identical operation list; the spread across reps shows run-to-run noise.\n"
    "* With both engines, repetitions are interleaved and the first engine alternates each rep, so machine drift\n"
    "  does not systematically favour one implementation. Compare engines using the PAIRED ratios per rep.\n"
    "* Builds with assertions enabled or sanitizers are flagged valid_for_comparison=0 and must not be\n"
    "  compared with optimised builds. Frequency scaling / turbo / noisy neighbours are NOT controlled\n"
    "  unless you pin the CPU and fix the governor yourself.\n";

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        bool ok = true;
        if (a == "--allow-debug") o.allow_debug = true;
        else if (a == "--profile") o.profile_only = true;
        else if (a == "--smoke") {
            o.params.measured = 1500;
            o.params.warmup = 150;
            o.params.prefill_scale = 0.01;
            o.reps = 2;
        } else if (a == "--workload" && (v = next())) o.workload = v;
        else if (a == "--out" && (v = next())) o.out_dir = v;
        else if (a == "--measured" && (v = next())) ok = parse_num(v, o.params.measured);
        else if (a == "--warmup" && (v = next())) ok = parse_num(v, o.params.warmup);
        else if (a == "--reps" && (v = next())) ok = parse_num(v, o.reps);
        else if (a == "--engine" && (v = next())) {
            const std::string e = v;
            if (e == "baseline") o.engines = {EngineKind::Baseline};
            else if (e == "pooled") o.engines = {EngineKind::Pooled};
            else if (e == "both") o.engines = {EngineKind::Baseline, EngineKind::Pooled};
            else ok = false;
        }
        else if (a == "--seed" && (v = next())) ok = parse_num(v, o.params.seed);
        else if (a == "--pin-cpu" && (v = next())) ok = parse_num(v, o.pin_cpu);
        else ok = false;
        if (!ok) { usage(); return 1; }
    }
    if (o.reps < 1 || o.params.measured == 0) { usage(); return 1; }

    std::vector<std::string> names;
    if (o.workload == "all") names = workload_names();
    else if (std::find(workload_names().begin(), workload_names().end(), o.workload) != workload_names().end())
        names = {o.workload};
    else { std::cerr << "unknown workload '" << o.workload << "'\n"; usage(); return 1; }

    Environment env = collect_environment();
    if (o.pin_cpu >= 0) {
        if (pin_to_cpu(o.pin_cpu)) env.pinned_cpu = o.pin_cpu;
        else std::cerr << "warning: could not pin to CPU " << o.pin_cpu << '\n';
    }
    if (!env.valid_for_comparison()) {
        std::cerr << "WARNING: this build has assertions " << (env.asserts_enabled ? "ENABLED" : "off")
                  << (env.sanitizers ? " and sanitizers ON" : "")
                  << " (build type '" << env.build_type << "'). Numbers are not representative.\n";
        if (!o.allow_debug) {
            std::cerr << "Refusing to run. Build with -DCMAKE_BUILD_TYPE=Release, or pass --allow-debug "
                         "(results will be marked valid_for_comparison=0).\n";
            return 1;
        }
    }

    // ---- profile mode: just run the throughput pass repeatedly ------------------
    if (o.profile_only) {
        for (const std::string& n : names) {
            const Workload w = make_workload(n, o.params);
            std::uint64_t sink = 0;
            for (int r = 0; r < o.reps; ++r) sink += run_throughput(o.engines.front(), w).sink;
            std::cerr << n << " [" << to_string(o.engines.front()) << "]: profiled " << o.reps << " pass(es) (" << sink << ")\n";
        }
        return 0;
    }

    const double timer_ns = timer_overhead_ns();
    fs::create_directories(o.out_dir);
    std::ofstream results(fs::path(o.out_dir) / "results.csv");
    std::ofstream summary(fs::path(o.out_dir) / "summary.csv");
    std::ofstream hist(fs::path(o.out_dir) / "latency_histogram.csv");
    results << std::fixed;
    summary << std::fixed;
    hist << std::fixed;

    results << "workload,engine,rep,measured_ops,warmup_ops,prefill_ops,seed,throughput_ops_per_sec,seconds,"
               "p50_ns,p95_ns,p99_ns,p999_ns,min_ns,mean_ns,max_ns,timer_overhead_ns,"
               "valid_for_comparison,build_type,cpu_model,logical_cpus,pinned_cpu,os,compiler,cxx_flags,cpu_governor,timestamp_utc\n";
    summary << "workload,engine,description,reps,measured_ops,latency_samples,throughput_median_ops_per_sec,"
               "throughput_min_ops_per_sec,throughput_max_ops_per_sec,p50_ns,p95_ns,p99_ns,p999_ns,min_ns,mean_ns,max_ns,"
               "timer_overhead_ns,events_per_op,trades_per_op,rejects_per_op,allocs_per_op,bytes_per_op,peak_live_bytes,"
               "final_resting_orders,final_bid_levels,final_ask_levels,rss_kb_end,valid_for_comparison,heap_in_use_bytes,heap_after_drain_bytes\n";
    hist << "workload,engine,bin_lo_ns,bin_hi_ns,count\n";

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "CPU: " << env.cpu_model << " (" << env.logical_cpus << " logical)  OS: " << env.os << '\n'
              << "Compiler: " << env.compiler << "\nBuild: " << env.build_type << "  flags: " << env.cxx_flags
              << "\nValid for comparison: " << (env.valid_for_comparison() ? "yes" : "NO") << "  timer overhead: " << timer_ns
              << " ns  pinned cpu: " << env.pinned_cpu << '\n'
              << "measured=" << o.params.measured << " warmup=" << o.params.warmup << " reps=" << o.reps
              << " seed=" << o.params.seed << " engines=" << (o.engines.size() == 2 ? "baseline+pooled (interleaved)" : to_string(o.engines[0]))
              << "\n\n"
              << std::left << std::setw(16) << "workload" << std::setw(10) << "engine" << std::right << std::setw(13)
              << "ops/s (med)" << std::setw(8) << "p50 ns" << std::setw(8) << "p95 ns" << std::setw(8) << "p99 ns"
              << std::setw(10) << "p99.9 ns" << std::setw(10) << "allocs/op" << std::setw(10) << "trades/op" << '\n';

    struct PerEngine {
        std::vector<double> tputs;
        std::vector<std::uint32_t> pooled;
        LogHistogram lh;
        AllocPass ap;
    };

    std::uint64_t global_sink = 0;
    for (const std::string& name : names) {
        const Workload w = make_workload(name, o.params);
        std::vector<PerEngine> per(o.engines.size());
        for (std::size_t e = 0; e < o.engines.size(); ++e) per[e].ap = run_alloc(o.engines[e], w);

        for (int rep = 0; rep < o.reps; ++rep) {
            // Interleave engines and alternate who goes first, so slow drift of the machine
            // (thermal, noisy neighbours) cannot systematically favour one implementation.
            std::vector<std::size_t> order(o.engines.size());
            for (std::size_t e = 0; e < order.size(); ++e) order[e] = (rep % 2 == 0) ? e : order.size() - 1 - e;
            for (const std::size_t e : order) {
                const EngineKind kind = o.engines[e];
                const Throughput tp = run_throughput(kind, w);
                global_sink += tp.sink;
                const double ops_s = static_cast<double>(w.measured()) / tp.seconds;
                per[e].tputs.push_back(ops_s);

                const auto samples = run_latency(kind, w);
                const LatencySummary ls = summarize(samples);
                per[e].pooled.insert(per[e].pooled.end(), samples.begin(), samples.end());
                for (std::uint32_t sm : samples) per[e].lh.add(sm);

                results << q(w.name) << ',' << q(to_string(kind)) << ',' << rep << ',' << w.measured() << ',' << w.warmup << ','
                        << w.prefill << ',' << o.params.seed << ',' << std::setprecision(0) << ops_s << std::setprecision(6) << ','
                        << tp.seconds << ',' << opt(ls.has_p50, ls.p50) << ',' << opt(ls.has_p95, ls.p95) << ','
                        << opt(ls.has_p99, ls.p99) << ',' << opt(ls.has_p999, ls.p999) << ',' << ls.min << ',' << std::setprecision(1)
                        << ls.mean << ',' << ls.max << ',' << timer_ns << ',' << (env.valid_for_comparison() ? 1 : 0) << ','
                        << q(env.build_type) << ',' << q(env.cpu_model) << ',' << env.logical_cpus << ',' << env.pinned_cpu << ','
                        << q(env.os) << ',' << q(env.compiler) << ',' << q(env.cxx_flags) << ',' << q(env.cpu_governor) << ','
                        << q(env.timestamp_utc) << '\n';
            }
        }

        for (std::size_t e = 0; e < o.engines.size(); ++e) {
            const EngineKind kind = o.engines[e];
            PerEngine& pe = per[e];
            const LatencySummary ps = summarize(pe.pooled);
            const AllocPass& ap = pe.ap;
            const double n = static_cast<double>(w.measured());
            const double tput_med = median(pe.tputs);
            summary << q(w.name) << ',' << q(to_string(kind)) << ',' << q(w.description) << ',' << o.reps << ',' << w.measured() << ','
                    << ps.n << ',' << std::setprecision(0) << tput_med << ',' << *std::min_element(pe.tputs.begin(), pe.tputs.end())
                    << ',' << *std::max_element(pe.tputs.begin(), pe.tputs.end()) << std::setprecision(3) << ','
                    << opt(ps.has_p50, ps.p50) << ',' << opt(ps.has_p95, ps.p95) << ',' << opt(ps.has_p99, ps.p99) << ','
                    << opt(ps.has_p999, ps.p999) << ',' << ps.min << ',' << ps.mean << ',' << ps.max << ',' << timer_ns << ','
                    << ap.events / n << ',' << ap.trades / n << ',' << ap.rejected / n << ',' << ap.allocs_measured / n << ','
                    << ap.bytes_measured / n << ',' << ap.peak_live_bytes << ',' << ap.resting << ',' << ap.bid_levels << ','
                    << ap.ask_levels << ',' << ap.rss_kb << ',' << (env.valid_for_comparison() ? 1 : 0) << ',' << ap.heap_in_use << ','
                    << ap.heap_after_drain << '\n';
            for (const auto& b : pe.lh.bins())
                hist << q(w.name) << ',' << q(to_string(kind)) << ',' << std::setprecision(1) << b.lo_ns << ',' << b.hi_ns << ','
                     << b.count << '\n';

            auto cell = [](bool has, std::uint64_t v) { return has ? std::to_string(v) : std::string("-"); };
            std::cout << std::left << std::setw(16) << w.name << std::setw(10) << to_string(kind) << std::right << std::setw(13)
                      << std::setprecision(0) << tput_med << std::setw(8) << cell(ps.has_p50, ps.p50) << std::setw(8)
                      << cell(ps.has_p95, ps.p95) << std::setw(8) << cell(ps.has_p99, ps.p99) << std::setw(10)
                      << cell(ps.has_p999, ps.p999) << std::setprecision(2) << std::setw(10) << ap.allocs_measured / n
                      << std::setw(10) << ap.trades / n << '\n';
        }
    }

    {
        std::ofstream envf(fs::path(o.out_dir) / "environment.txt");
        envf << "timestamp_utc=" << env.timestamp_utc << "\ncpu_model=" << env.cpu_model
             << "\nlogical_cpus=" << env.logical_cpus << "\ncpu_governor=" << env.cpu_governor << "\nos=" << env.os
             << "\ncompiler=" << env.compiler << "\nbuild_type=" << env.build_type << "\ncxx_flags=" << env.cxx_flags
             << "\nasserts_enabled=" << env.asserts_enabled << "\nsanitizers=" << env.sanitizers
             << "\nvalid_for_comparison=" << env.valid_for_comparison() << "\npinned_cpu=" << env.pinned_cpu
             << "\ntimer_overhead_ns=" << timer_ns << "\nmeasured_ops=" << o.params.measured
             << "\nwarmup_ops=" << o.params.warmup << "\nreps=" << o.reps << "\nseed=" << o.params.seed << "\nengines="
             << (o.engines.size() == 2 ? "baseline,pooled" : to_string(o.engines[0])) << "\n\n"
             << kMethodology;
    }
    std::cout << "\nwrote " << o.out_dir << "/{results.csv,summary.csv,latency_histogram.csv,environment.txt}"
              << "  (checksum " << global_sink << ")\n";
    return 0;
}
