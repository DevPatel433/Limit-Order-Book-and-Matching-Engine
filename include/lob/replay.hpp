#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "lob/commands.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/policy.hpp"

namespace lob {

struct ReplayOptions {
    bool print_events{true};      // one line per event
    bool final_snapshot{true};    // print the book after the last command
    bool check{false};            // run ConsistencyChecker after every command
    std::size_t default_depth{5}; // for SNAPSHOT without a depth, and the final snapshot
    EngineKind engine{EngineKind::Baseline};  // which implementation to run (same results, different memory)
    bool csv{false};  // stdout becomes a pure CSV event log: no snapshots, no SUMMARY (see docs/replay_format.md)
};

struct ReplayStats {
    std::size_t commands{};  // includes SNAPSHOT commands
    std::size_t accepted{};
    std::size_t rejected{};
    std::size_t trades{};
    std::size_t cancelled{};
    std::size_t expired{};
    std::size_t amended{};
    Quantity accepted_quantity{};   // sum of Accepted quantities (original sizes)
    Quantity traded_quantity{};     // sum of Trade quantities
    Quantity cancelled_quantity{};  // sum of Cancelled remaining quantities
    Quantity expired_quantity{};    // sum of Expired remaining quantities (IOC/FOK/market leftovers)
    Quantity amend_added_quantity{};    // sum of (new - old) over amends that increased quantity
    Quantity amend_removed_quantity{};  // sum of (old - new) over amends that decreased quantity
    // Conservation law, checked by the property tests:
    //   accepted + amend_added == 2 * traded + cancelled + expired + resting + amend_removed
    // (every traded unit leaves both a maker and a taker; everything else is cancelled, expired,
    //  still resting, or removed by shrinking an order).
};

// Adds one event's effect to `stats` (every field except `commands`). Used by the replay runners and
// the multi-instrument pipeline, so all of them count identically.
void tally_event(const Event& e, ReplayStats& stats);
// Field-wise sum (quantities saturate rather than wrap).
void merge_stats(ReplayStats& into, const ReplayStats& from);

struct Violation {
    std::size_t command_index{};  // 1-based index into the command list
    std::string message;
};

struct ReplayResult {
    ReplayStats stats;
    BookSnapshot final_book;        // full depth
    std::size_t resting_orders{};
    std::optional<Violation> violation;  // set only if options.check and a check failed
};

// Runs `commands` on a fresh MatchingEngine, writing deterministic text to
// `out` (events, snapshots, SUMMARY line). Stops at the first checker violation.
ReplayResult replay(const std::vector<Command>& commands, std::ostream& out,
                    const ReplayOptions& options = {});

// The one-line SUMMARY printed at the end of a text replay.
std::string format_summary(const ReplayResult& result);

// Ladder view: asks worst-to-best, a separator, then bids best-to-worst.
std::string format_snapshot(const BookSnapshot& snapshot, std::size_t depth);

}  // namespace lob
