// lob_spikes: list the individual commands whose latency exceeds a threshold, per repetition.
//
// A spike that lands on the SAME command index in every repetition is caused by the workload and
// the data structures (e.g. a hash-table rehash at a particular size); a spike at a random
// index is OS/VM noise. This is how the rehash stalls in docs/results_baseline.md were found.
//
//   lob_spikes [workload=non_crossing] [threshold_us=100] [reps=5] [measured=200000] [engine=baseline|pooled]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lob/matching_engine.hpp"
#include "workloads.hpp"

using namespace lob;
using namespace lob::bench;

template <class Engine>
static void run(const Workload& w, unsigned threshold_ns, int reps) {
    for (int rep = 0; rep < reps; ++rep) {
        Engine eng;
        std::vector<Event> ev;
        ev.reserve(4096);
        std::vector<std::pair<unsigned, std::size_t>> spikes;
        for (std::size_t i = 0; i < w.ops.size(); ++i) {
            const BenchOp& op = w.ops[i];
            ev.clear();
            const auto t0 = std::chrono::steady_clock::now();
            if (op.is_cancel) eng.cancel(op.cancel_id, ev);
            else eng.submit(op.order, ev);
            const auto t1 = std::chrono::steady_clock::now();
            const auto ns = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (ns > threshold_ns) spikes.emplace_back(ns, i);
        }
        std::sort(spikes.rbegin(), spikes.rend());
        std::printf("rep %d: %zu spikes; worst:", rep, spikes.size());
        for (std::size_t k = 0; k < std::min<std::size_t>(6, spikes.size()); ++k)
            std::printf(" [cmd %zu: %u us]", spikes[k].second, spikes[k].first / 1000);
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    const std::string workload = argc > 1 ? argv[1] : "non_crossing";
    const unsigned threshold_ns = static_cast<unsigned>((argc > 2 ? std::atof(argv[2]) : 100.0) * 1000);
    const int reps = argc > 3 ? std::atoi(argv[3]) : 5;
    WorkloadParams p;
    p.measured = argc > 4 ? static_cast<std::size_t>(std::atoll(argv[4])) : 200000;
    const Workload w = make_workload(workload, p);

    const std::string engine = argc > 5 ? argv[5] : "baseline";
    std::printf("%s [%s]: %zu commands (prefill %zu, warmup %zu), threshold %u us\n", workload.c_str(), engine.c_str(),
                w.ops.size(), w.prefill, w.warmup, threshold_ns / 1000);
    if (engine == "pooled") run<PooledMatchingEngine>(w, threshold_ns, reps);
    else run<MatchingEngine>(w, threshold_ns, reps);
    return 0;
}
