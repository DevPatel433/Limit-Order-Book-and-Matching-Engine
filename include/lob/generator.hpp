#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/commands.hpp"

namespace lob {

// Seeded workload generator. Fully deterministic and portable: it uses
// std::mt19937_64 (bit-exact per the C++ standard) and plain modulo, never
// std::*_distribution, whose output differs between standard libraries.
struct GeneratorConfig {
    std::uint64_t seed{1};
    std::size_t count{1000};         // number of commands
    Price mid{1000};                 // centre of the price band (ticks)
    Price band{10};                  // prices are in [mid-band, mid+band]; must be < mid
    Quantity max_quantity{50};       // typical order size is uniform in [1, max_quantity]
    unsigned cancel_percent{30};     // chance a command is a cancel
    unsigned bad_percent{2};         // chance a new order is malformed or reuses a live-looking ID
    unsigned sweep_percent{3};       // chance a new order is 10x larger (multi-level sweeps)
    std::size_t snapshot_every{0};   // insert a SNAPSHOT every N commands (0 = never)
    // Opt-in extensions, all 0 by default. A zero percentage draws no random numbers, so
    // configurations that predate these fields generate byte-identical files.
    unsigned ioc_percent{0};     // chance a new order is IOC
    unsigned fok_percent{0};     // chance a (non-IOC) new order is FOK
    unsigned market_percent{0};  // chance a new order is a market order (IOC unless FOK was drawn)
    unsigned amend_percent{0};   // chance a command is an AMEND of a past ID (live, dead or never-seen)
};

// Narrow `band` => many crossings; wide `band` => mostly resting orders.
// Cancels target past IDs, so they hit live, already-filled and never-seen orders.
std::vector<Command> generate(const GeneratorConfig& config);

}  // namespace lob
