# Complexity and known limitations

## Complexity
`n` = live orders, `L` = price levels on the touched side, `m` = maker orders touched, `l` = levels
emptied by one command, `d` = snapshot depth.

| Operation | Cost | Why |
|---|---|---|
| submit, does not cross (GTC) | `O(log L)` + `O(1)` average | ordered-map level lookup; hash-index insert; list append |
| submit, crosses | `O(m + l log L)` | best level is `begin()`; each fill is `O(1)`; removing an emptied level is `O(log L)` |
| FOK availability check | `O(levels scanned)` | stops as soon as enough quantity is found |
| cancel | `O(log L)` + `O(1)` average | hash index gives the list node; level found by price |
| amend, priority kept | `O(log L)` + `O(1)` | in-place reduce |
| amend, priority lost | cancel + submit | re-entered as a fresh order |
| best bid / ask, spread | `O(1)` | `begin()` of the ordered maps |
| snapshot | `O(d)` | walk `d` levels |
| `validate()` / checker | `O(n)` per call | debug and verification only |

Memory: about **110-125 bytes of heap per resting order** (one 48 B list node, one 40 B hash node, a
share of the bucket array and level nodes); measured in [results_phase5.md](results_phase5.md). An
order is 32 bytes (`static_assert`ed so node size cannot silently grow).

## Measured costs of the design (details in the Phase 4/5/6 docs)
* Allocation was the biggest single cost (30-51% of instructions); the pooled engine removes it,
  gaining about 1.2-1.3x on four of five workloads, at the price of never returning memory.
* Wide books (`many_levels`) are cache-miss bound: pooling does not help them.
* The ID index (`std::unordered_map`) causes rehash latency spikes (~0.1-3 ms) as the book grows.
* The Phase 6 order types cost +1.5% to +3.8% instructions per command (deterministic count).

## Not implemented (deliberately)
| Feature | What it would take |
|---|---|
| ~~Multiple instruments~~ | **Done in Phase 7** ([phase7_design.md](phase7_design.md)): router, `@SYMBOL` format, per-symbol IDs. Still missing: venue-wide order IDs, symbol admission control (symbols are created on first use), cross-symbol anything. |
| **Self-trade prevention** | Needs a participant/owner field on `Order` (the struct is 32 bytes and node size matters) plus a policy (cancel newest/oldest/both, decrement). |
| **Post-only / stop / iceberg / pegged orders** | Post-only is a new TIF-like flag plus a reject; the others need trigger/hidden-quantity state. |
| **Concurrency (partly)** | **Done in Phase 7:** books are sharded across worker threads behind a sequencer and SPSC queues; the engine itself stays single-threaded. Still missing: a *streaming* output path (output is merged after join), load-aware sharding (one hot symbol caps speedup, see the partition bound), accepting input from several producers, and any **measured** speedup (needs a multi-core machine). |
| **Persistence / recovery (partly)** | **Done in Phase 8** ([phase8_design.md](phase8_design.md)): checksummed write-ahead log of the sequenced input, write-ahead ordering, torn-tail recovery, resume. Still missing: checkpoints and log rotation (recovery replays from the start), group commit, directory `fsync`, replication/failover. |
| **Market data / network** | Out of scope: no feed handlers, protocols, or gateways. |
| **Auctions, trade busting, circuit breakers** | Out of scope. |

## Phase 7 limitations (multi-instrument and concurrency)
* **No speedup demonstrated.** Developed on a one-CPU machine; see [results_phase7.md](results_phase7.md). Measured there: pipeline overhead only.
* **One book is the unit of parallelism.** A hot symbol bounds speedup whatever the shard count.
* **Threaded mode skips `SNAPSHOT` commands** and has no per-command checker (use `--verify`, which compares with a sequential run).
* **Output is merged after join**, so memory grows with the event count and nothing is available until the end.
* **Static shard map** (`symbol_id % shards`, ids by first appearance); no rebalancing.
* **Idle waiting is not latency-tuned** (yield, then 50 microsecond sleeps).
* **Queue verified on x86 + ThreadSanitizer only**, not on weakly ordered hardware.
* The failure path (a worker exception) is only exercised through an out-of-range symbol id.

## Phase 8 limitations (write-ahead log)
* **Recovery replays the whole log**: time grows with log length (about 2.2 M commands/s measured; no checkpoints).
* **`fsync` cost is brutal if per-record** (about 190 microseconds on the dev VM's disk, so about 5,000 commands/s); there is no group commit.
  The absolute figure is storage-specific and must not be generalised ([results_phase8.md](results_phase8.md)).
* **Damage in the final ~44 bytes is treated as a torn write** and discarded; this is inherent, not a bug ([wal_format.md](wal_format.md)).
* **Simulated, not real, failures**: truncation, bit flips and `/dev/full`; never a real power loss or failing disk.
* **No directory `fsync`** after creating a log; one writer only; inputs are logged, not events (output already delivered before a crash is regenerated by replay).
* Logging overhead (about 200 ns/command) is **not profiled** and not optimised.

## Known limitations and sharp edges
* **Synthetic workloads only.** Benchmarks use uniform-random flow; real flow clusters at the touch
  and is bursty with heavy-tailed sizes. Conclusions about `many_levels` in particular may not carry over.
* **Noisy benchmark machine.** All reported numbers came from a shared single-vCPU VM without
  pinning; relative comparisons use interleaved, paired runs with an A/A control, but absolute numbers
  are not portable. Re-run on your hardware.
* **No hardware counters.** `perf` was unavailable; cache findings come from callgrind's simulated
  cache and are labelled as such.
* **Pooled engine memory is sticky.** The arena keeps its high-water mark until the book is destroyed.
* **Pooled nodes are invisible to ASan/valgrind** as individual allocations; the baseline engine and
  its tests exist for that reason.
* **Rehash stalls** in the ID index are unfixed (a candidate for future work).
* **`Order` price for market orders is ignored** but stored; the parser canonicalises it to 0.
* **Quantity overflow rejection** is per price level (`uint64` sum); it only matters at absurd sizes but is handled and tested.
* **Not copyable/movable books.** Deliberate (see phase5_design.md); engines are used in place.
