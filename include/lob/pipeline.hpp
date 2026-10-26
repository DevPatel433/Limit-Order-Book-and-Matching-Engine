#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "lob/multi.hpp"
#include "lob/policy.hpp"
#include "lob/replay.hpp"

namespace lob {

// Concurrent input, Phase 7. Shape (details and trade-offs: docs/phase7_design.md):
//
//   producer (one thread)          one SPSC queue per shard          one worker thread per shard
//   ----------------------         -------------------------         ---------------------------
//   assigns a global sequence  ->  [queue 0] -> worker 0: engines for the symbols of shard 0
//   number, routes by symbol   ->  [queue 1] -> worker 1: engines for the symbols of shard 1
//                              ->  [queue N] -> worker N: ...
//   after join(): k-way merge of the workers' event buffers by sequence number -> one ordered stream
//
//   * Every symbol lives on exactly one shard (symbol_id % shards), so each book still sees its
//     commands one at a time, in input order: the matching core stays single-threaded and unchanged.
//   * Workers share NO mutable state. Each owns its engines (allocated by that worker's own thread, so
//     with PoolPolicy the arenas are thread-local too), its event buffer and its statistics.
//   * Determinism: the engine's output for a symbol depends only on that symbol's command sequence,
//     which the FIFO queue preserves; the merged stream is ordered by input sequence number. So the
//     result is IDENTICAL to run_sequential() for any shard count and any thread timing.
//   * SNAPSHOT commands are not dispatched (they are a view, and a mid-stream view would have to be
//     a second output channel); they are counted in `snapshots_skipped`. Final books are returned.
//   * Backpressure: a full queue makes the producer wait (spin, then yield, then short sleeps).
struct PipelineOptions {
    std::size_t shards{2};            // number of worker threads; must be >= 1
    std::size_t queue_capacity{4096}; // per shard; rounded up to a power of two, minimum 2
    EngineKind engine{EngineKind::Baseline};
    // false: workers only count (events are still tallied into `stats`), nothing is stored. Used to
    // measure pure matching throughput without the cost of materialising and merging the output.
    bool collect_events{true};
    // Write-ahead hook (Phase 8). Called on the SEQUENCER thread for every non-SNAPSHOT command, in input
    // order, BEFORE the command is assigned a sequence number or dispatched to a worker. If it throws, the
    // command was not submitted and submit() propagates the exception. run_sequential never calls it.
    std::function<void(const SymbolCommand&)> journal{};
};

struct SymbolFinal {
    SymbolId symbol{};
    BookSnapshot book;           // full depth
    std::size_t resting{};
};

struct PipelineResult {
    std::vector<SymbolEvent> events;     // merged into input order; empty if !collect_events
    std::vector<SymbolFinal> finals;     // every symbol that had at least one dispatched command, ascending id
    ReplayStats stats;                   // totals over all events; `commands` = dispatched commands
    std::size_t snapshots_skipped{};
    std::vector<std::size_t> shard_commands;  // commands handled per shard (shows load imbalance)
};

// Streaming interface. submit() must be called from ONE thread only (the sequencer); finish() must be
// called exactly once. Destroying an unfinished pipeline stops and joins the workers.
template <class Policy>
class BasicShardedPipeline {
public:
    explicit BasicShardedPipeline(const PipelineOptions& options);
    ~BasicShardedPipeline();
    BasicShardedPipeline(const BasicShardedPipeline&) = delete;
    BasicShardedPipeline& operator=(const BasicShardedPipeline&) = delete;

    // Assigns the next sequence number and routes the command. Blocks while the target queue is full.
    void submit(const SymbolCommand& command);

    // Stops the workers, joins them, merges their output. If a worker failed (for example a symbol id
    // >= kMaxSymbols), the first failure is rethrown here, after every thread has been joined.
    PipelineResult finish();

private:
    struct Shard;
    void shutdown() noexcept;

    PipelineOptions options_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::uint64_t next_seq_{0};
    std::size_t snapshots_skipped_{0};
    bool finished_{false};
};

extern template class BasicShardedPipeline<StdPolicy>;
extern template class BasicShardedPipeline<PoolPolicy>;

// Convenience: submit every command, then finish. Dispatches on options.engine.
PipelineResult run_pipeline(const std::vector<SymbolCommand>& commands, const PipelineOptions& options);

// The single-threaded reference: same inputs and outputs as run_pipeline (ignores shards and
// queue_capacity). The pipeline's correctness claim is "equals this", and the tests check it.
PipelineResult run_sequential(const std::vector<SymbolCommand>& commands, const PipelineOptions& options);

}  // namespace lob
