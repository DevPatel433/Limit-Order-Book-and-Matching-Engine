#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob::bench {

// Nearest-rank percentile of an already SORTED sample vector. p in (0, 1].
std::uint64_t percentile_sorted(const std::vector<std::uint32_t>& sorted, double p);

// A percentile is only reported when at least `min_tail` samples lie above it,
// otherwise it is just the maximum in disguise. E.g. p99 needs n >= 1,000 and
// p99.9 needs n >= 10,000 with the default min_tail of 10.
bool supports_percentile(std::size_t n, double p, std::size_t min_tail = 10);

struct LatencySummary {
    std::size_t n{};
    std::uint64_t min{}, max{};
    double mean{};
    std::uint64_t p50{}, p95{}, p99{}, p999{};
    bool has_p50{}, has_p95{}, has_p99{}, has_p999{};
};

LatencySummary summarize(std::vector<std::uint32_t> samples);  // sorts its copy

// Log-spaced histogram (4 bins per octave) for plotting latency distributions.
class LogHistogram {
public:
    static constexpr int kBinsPerOctave = 4;
    struct Bin {
        double lo_ns, hi_ns;
        std::uint64_t count;
    };
    void add(std::uint32_t ns);
    std::vector<Bin> bins() const;  // only non-empty bins, ascending
    std::uint64_t total() const noexcept { return total_; }

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t total_{0};
};

}  // namespace lob::bench
