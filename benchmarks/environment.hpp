#pragma once

#include <string>

namespace lob::bench {

struct Environment {
    std::string cpu_model;
    unsigned logical_cpus{};
    std::string os;            // e.g. "Linux 6.8.0 x86_64"
    std::string compiler;      // compiler version string
    std::string build_type;    // CMake build type
    std::string cxx_flags;     // flags the benchmark library was compiled with
    std::string cpu_governor;  // "unknown" if not exposed (VMs, containers, non-Linux)
    std::string timestamp_utc;
    bool asserts_enabled{};    // true when NDEBUG is NOT defined
    bool sanitizers{};
    int pinned_cpu{-1};

    // Results from a build with assertions or sanitizers are not comparable with
    // optimised builds, so they are flagged rather than silently mixed.
    bool valid_for_comparison() const noexcept { return !asserts_enabled && !sanitizers; }
};

Environment collect_environment();
bool pin_to_cpu(int cpu);  // Linux only; returns false if unsupported or it failed
long read_rss_kb();        // current resident set size, or -1 if unavailable (Linux /proc)

}  // namespace lob::bench
