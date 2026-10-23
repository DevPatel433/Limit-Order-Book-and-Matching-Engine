#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lob/commands.hpp"
#include "lob/order.hpp"

namespace lob::bench {

// Compact pre-built operation so the timed loop does no parsing or allocation.
struct BenchOp {
    bool is_cancel{false};
    Order order{};         // valid when !is_cancel
    OrderId cancel_id{0};  // valid when is_cancel
};

struct WorkloadParams {
    std::uint64_t seed{1};
    std::size_t measured{200'000};  // operations that are timed
    std::size_t warmup{20'000};     // untimed operations run right after the prefill
    double prefill_scale{1.0};      // scales each workload's prefill (tests/smoke use small values)
};

struct Workload {
    std::string name;
    std::string description;
    std::vector<BenchOp> ops;  // prefill + warmup + measured, in execution order
    std::size_t prefill{0};
    std::size_t warmup{0};
    std::vector<OrderId> live_at_end;  // IDs resting after the last op (used to measure memory after draining the book)

    std::size_t unmeasured() const noexcept { return prefill + warmup; }
    std::size_t measured() const noexcept { return ops.size() - unmeasured(); }
};

// non_crossing, cancel_heavy, crossing_heavy, many_levels, bursty
const std::vector<std::string>& workload_names();

// Deterministic for a given (name, params). Throws std::invalid_argument on an unknown name.
// Cancels always target orders that are live at that point: the builder runs the
// engine itself (untimed) to know exactly which IDs exist.
Workload make_workload(const std::string& name, const WorkloadParams& params);

// Same operations as replay-file commands (used by tests to verify with the checker).
std::vector<Command> to_commands(const Workload& w);

}  // namespace lob::bench
