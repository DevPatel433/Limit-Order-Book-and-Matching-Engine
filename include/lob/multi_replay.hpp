#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "lob/multi.hpp"
#include "lob/policy.hpp"
#include "lob/replay.hpp"

namespace lob {

struct MultiReplayOptions {
    bool print_events{true};      // one line per event, prefixed with @SYMBOL
    bool final_snapshot{true};    // print every symbol's book after the last command
    bool check{false};            // sequential only: run a ConsistencyChecker per symbol after every command
    std::size_t default_depth{5};
    EngineKind engine{EngineKind::Baseline};
    bool csv{false};              // stdout becomes a pure CSV event log with a leading `symbol` column
    std::size_t threads{0};       // 0 = run in the calling thread; N >= 1 = N shard worker threads
    std::size_t queue_capacity{4096};
    bool verify{false};           // threads >= 1 only: also run sequentially and require identical results
    // Write-ahead hook (Phase 8): called for every non-SNAPSHOT command, in input order, before it is applied
    // (sequential mode) or dispatched (threaded mode, on the sequencer thread). May throw; the exception
    // propagates out of multi_replay. The `verify` reference run does not call it.
    std::function<void(const SymbolCommand&)> journal{};
};

struct SymbolReplayResult {
    SymbolId symbol{};
    ReplayStats stats;            // this symbol's events and commands (SNAPSHOT lines count as commands)
    BookSnapshot final_book;      // full depth
    std::size_t resting{};
};

struct MultiReplayResult {
    std::vector<SymbolReplayResult> symbols;  // one per symbol in the table, ascending id
    ReplayStats total;                        // sum over symbols
    std::size_t resting_orders{};
    std::size_t snapshots_skipped{};          // threaded mode only: SNAPSHOT commands are not dispatched
    std::optional<Violation> violation;       // checker failure (message names the symbol)
    std::optional<std::string> verify_mismatch;  // set only if `verify` found a difference
};

// Runs `commands` on fresh engines, writing deterministic text (or CSV) to `out`.
//
// Sequential (threads == 0): events and SNAPSHOT output interleave in input order; stops at the first
// checker violation. Threaded (threads >= 1): runs the sharded pipeline; the event stream is
// byte-identical to the sequential one, but SNAPSHOT commands are skipped (see pipeline.hpp) and the
// per-command checker is unavailable (use `verify`, which compares against a sequential run).
// Throws std::invalid_argument for `check` with threads, `verify` without threads, and a command whose
// symbol is not in `symbols`.
MultiReplayResult multi_replay(const SymbolTable& symbols, const std::vector<SymbolCommand>& commands,
                               std::ostream& out, const MultiReplayOptions& options = {});

// "SUMMARY @SYM commands=..." for one symbol and "SUMMARY symbols=N commands=..." for the total.
std::string format_symbol_summary(const SymbolTable& symbols, const SymbolReplayResult& r);
std::string format_total_summary(const SymbolTable& symbols, const MultiReplayResult& r);

}  // namespace lob
