# Changelog

Reconstructed from the deliverables of each project phase (this repository was assembled phase by phase,
so there is no fine-grained git history to mine). Newest first.

## 0.8.0 - Phase 8: write-ahead log and crash recovery
- Added a checksummed write-ahead log of the sequenced input (`LOBWAL01`, [docs/wal_format.md](docs/wal_format.md)):
  `WalWriter`, `wal_scan` (classifies Clean / TornTail / Corrupt / BadHeader), `recover_wal`, `DurableMultiEngine`
  (log first, then apply). Design and trade-offs: [docs/phase8_design.md](docs/phase8_design.md).
- A `journal` hook on `PipelineOptions` and `MultiReplayOptions`: the sequencer logs each command before dispatching it,
  so sequential and threaded runs write byte-identical logs.
- New tools: `lob_recover` (rebuild and print books; `--truncate` repairs a torn tail; exit codes 0/3/2/1), `lob_wal`
  (cost of logging and recovery speed); `lob_mreplay --wal FILE [--wal-sync N]`.
- Tests: truncation at every byte offset recovers exactly a prefix of the commands; every bit flip is detected unless it
  is within the final record; a mini-fuzzer under ASan/UBSan; `/dev/full` write-failure test; 12-mutant mutation run.
- Results: logging costs about 2x without `fsync` (about 200 ns/command, not profiled), `fsync` per record about 1000x on the
  development VM's disk, recovery about 2.2 M commands/s ([docs/results_phase8.md](docs/results_phase8.md)).
- Fixed in this phase: a `std::function` member without a default initializer made every existing designated-initializer
  use warn (`-Werror` in CI would have failed).
- Not done: checkpoints/rotation, group commit, directory `fsync`, replication.

## 0.7.0 - Phase 7: multiple instruments and concurrent input
- Added the `@SYMBOL` multi-instrument format, `SymbolTable`, `BasicMultiEngine` (one independent engine per
  symbol; the matching core is unchanged), per-symbol order IDs. See [docs/phase7_design.md](docs/phase7_design.md).
- Added a sharded concurrent pipeline: one sequencer, lock-free SPSC queues, one worker thread per shard,
  deterministic merge by input sequence number. Output is byte-identical to the sequential run for any shard
  count (differential tests, `--verify`).
- New tools: `lob_mreplay` (sequential with per-symbol checker, or `--threads N`, `--verify`, `--csv`),
  `lob_mgen`, `lob_scale` (scaling benchmark with a correctness gate on every run).
- New `tsan` CMake preset and CI job. Mutation tests of the pipeline found one mutant (a weakened memory
  ordering in the queue) that only ThreadSanitizer detects.
- Refactors, no behaviour change: `parse_command_line`, `tally_event`, `merge_stats` exposed for reuse.
- Fixed: the benchmark's "refuse sanitizer builds" logic did not know about TSan.
- Results: correctness and overhead only. The development machine had one CPU, so **no speedup is claimed**
  ([docs/results_phase7.md](docs/results_phase7.md)).

## 0.6.0 - Phase 6: extensions and polish
- Added IOC and FOK time in force, market orders, and amend/replace (priority-aware), each with explicit
  semantics ([docs/matching_rules.md](docs/matching_rules.md)); new events `Expired` and `Amended`.
- Added `lob_replay --csv`, an interactive `lob_shell`, `lob_gen` flags for the new order types.
- Independent checker extended; a mutation test exposed that it shared `effective_limit()` with the engine -
  fixed, and the naive reference model made independent too.
- Fixed: generator could emit a market order with a non-canonical price (broke text round trip).
- Removed a stray `{include` directory shipped since Phase 1.
- Docs: lifecycle, limitations/complexity, interview notes, milestones; quickstart script; CMake presets.
- Cost of the extensions: +1.5% to +3.8% instructions per command (deterministic).

## 0.5.0 - Phase 5: node pooling (one measured optimisation)
- `SlabArena` + policy templates; `PooledMatchingEngine` alongside the baseline `MatchingEngine`.
- ~1.2-1.3x throughput on four of five workloads (paired, interleaved, sign-tested); no reliable gain on
  `many_levels`; memory is never returned until destruction.
- Fixed: `OrderBook` copy aliased the original's nodes (now non-copyable/non-movable).

## 0.4.0 - Phase 4: benchmarks and profiling
- `lob_bench` with five workloads, latency percentiles gated on sample size, allocation tracking, CSV
  output, plots, callgrind/perf scripts, `lob_spikes`. Baseline results and analysis.

## 0.3.0 - Phase 3: replay, property tests
- Replay file format, `lob_replay`, `lob_gen`, independent `ConsistencyChecker`, property tests,
  regression workflow, golden files.

## 0.2.0 - Phase 2: matching
- Price-time priority matching, partial fills, cancellation, event output, naive reference-model tests.

## 0.1.0 - Phases 0 and 1: scaffold and book
- CMake/CI/warnings, tick-price data model, `PriceLevel`, `OrderBook`, snapshots, invariants.
