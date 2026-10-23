#include "stats.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>

namespace lob::bench {

std::uint64_t percentile_sorted(const std::vector<std::uint32_t>& s, double p) {
    assert(!s.empty() && p > 0.0 && p <= 1.0);
    const std::size_t n = s.size();
    auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n) - 1e-9));
    rank = std::clamp<std::size_t>(rank, 1, n);
    return s[rank - 1];
}

bool supports_percentile(std::size_t n, double p, std::size_t min_tail) {
    return static_cast<double>(n) * (1.0 - p) >= static_cast<double>(min_tail) - 1e-9;
}

LatencySummary summarize(std::vector<std::uint32_t> v) {
    LatencySummary s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    if ((s.has_p50 = supports_percentile(s.n, 0.50))) s.p50 = percentile_sorted(v, 0.50);
    if ((s.has_p95 = supports_percentile(s.n, 0.95))) s.p95 = percentile_sorted(v, 0.95);
    if ((s.has_p99 = supports_percentile(s.n, 0.99))) s.p99 = percentile_sorted(v, 0.99);
    if ((s.has_p999 = supports_percentile(s.n, 0.999))) s.p999 = percentile_sorted(v, 0.999);
    return s;
}

void LogHistogram::add(std::uint32_t ns) {
    const double x = std::max<double>(1.0, ns);
    const auto idx = static_cast<std::size_t>(std::floor(kBinsPerOctave * std::log2(x)));
    if (idx >= counts_.size()) counts_.resize(idx + 1, 0);
    ++counts_[idx];
    ++total_;
}

std::vector<LogHistogram::Bin> LogHistogram::bins() const {
    std::vector<Bin> out;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        if (counts_[i] == 0) continue;
        out.push_back({std::exp2(static_cast<double>(i) / kBinsPerOctave),
                       std::exp2(static_cast<double>(i + 1) / kBinsPerOctave), counts_[i]});
    }
    return out;
}

}  // namespace lob::bench
