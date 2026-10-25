# Phase 7 results: what the concurrent pipeline costs, and what could not be measured

**Headline: this phase demonstrates correctness and measures overhead; it does not demonstrate speedup.**
The only machine available during development had **one logical CPU**, so the producer and its worker threads
time-share a single core. Parallel speedup is physically impossible there, and no speedup is claimed.
The benchmark harness (`lob_scale`) is built and ready; the table that matters needs a multi-core machine
(instructions at the bottom).

## Environment
Recorded in [phase7_run/environment.txt](results/phase7_run/environment.txt) and in every CSV: Intel Xeon @ 2.10 GHz
virtual machine, **1 logical CPU**, Linux 6.18 x86_64, gcc 13.3.0, Release (`-O3 -DNDEBUG`), `valid_for_comparison=1`, baseline engine,
queue capacity 4096, no CPU pinning, noisy shared VM (see [limitations.md](limitations.md)).

## Method
[benchmarking.md](benchmarking.md) rules still apply: workload generated outside the timed region; modes **interleaved** within each
repetition; median of 5-7 repetitions; and, new for this phase, a **correctness gate on every timed run**:
the run's statistics (every counter and quantity) must equal the sequential reference's or the benchmark aborts.
Timing covers thread start, dispatch, matching, join and (for `merged`) the ordered merge. "none" counts events
without storing them (matching + queueing cost); "merged" stores and merges them as `lob_mreplay` does.

Workload: 1,000,000 commands over 64 symbols, band 5, 20% cancels, 5% IOC, 3% market, 5% amend; about 748k orders
accepted and 612k trades. Raw rows: [scale_uniform.csv](results/phase7_run/scale_uniform.csv),
[scale_hot80.csv](results/phase7_run/scale_hot80.csv).

## Results (1 CPU, so every pipeline row is OVERSUBSCRIBED)

Uniform load over 64 symbols (7 reps):

| mode | output | median s | commands/s | vs sequential | partition bound |
|---|---|---:|---:|---:|---:|
| sequential | none | 0.192 | 5.22 M | 1.00x | 1.00x |
| 1 shard | none | 0.235 | 4.26 M | 0.82x | 1.00x |
| 2 shards | none | 0.208 | 4.80 M | 0.92x | 2.00x |
| 4 shards | none | 0.211 | 4.73 M | 0.91x | 3.98x |
| sequential | merged | 0.332 | 3.01 M | 1.00x | 1.00x |
| 1 shard | merged | 0.363 | 2.76 M | 0.91x | 1.00x |
| 2 shards | merged | 0.447 | 2.24 M | 0.74x | 2.00x |
| 4 shards | merged | 0.406 | 2.46 M | 0.82x | 3.98x |

Hot-symbol skew, 80% of commands on one symbol (5 reps, "none"): 2/4/8 shards ran at 0.85x / 0.85x / 0.80x of
sequential, and the partition bound was only **1.11x / 1.18x / 1.22x**.

## What these numbers do and do not say
* **Overhead, measured:** with no parallelism available, the pipeline machinery (queue hand-off, thread
  scheduling, per-event tallying) costs roughly **8-18%** of sequential throughput without output, and
  **9-26%** with the ordered merge, on this machine. That is the price that parallelism has to beat.
  Single-run differences of a few percent are within this VM's noise (the Phase 5 A/A control showed that scale), so
  the ordering between 2 and 4 shards is not meaningful.
* **Partition bound, computed from the workload alone (no timing involved):** uniform load over 64 symbols
  splits almost perfectly (`max shard 25.1%` at 4 shards, bound 3.98x), but with one symbol carrying 80% of the flow,
  **no number of shards can give more than about 1.2x**. That is a structural property of "one book = one unit
  of parallelism", and it holds on any hardware. It is the most transferable finding of this phase.
* **Not shown:** any speedup, scaling curve, queue-contention behaviour, cache effects of cross-core
  hand-off, or the cost of the serial producer and merge stages on real cores. Expect the achievable speedup to sit
  below the partition bound, and the `merged` mode to saturate early because the merge is single-threaded.
* **Correctness is the evidence that *does* transfer:** every one of these runs matched the sequential statistics;
  the test suite additionally shows byte-identical output and a clean ThreadSanitizer run
  ([phase7_design.md](phase7_design.md)). Correctness does not depend on core count.

## Reproduce, and get the real answer on a multi-core machine
```bash
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/lob_scale --symbols 64 --count 2000000 --shards 1,2,4,8 --reps 9 --engine pooled \
    --out scale.csv          # run on an otherwise idle machine with at least shards+1 cores
taskset -c 0-4 ./build/release/benchmarks/lob_scale ...   # (Linux) restrict to a known set of cores
```
Rows are flagged `oversubscribed=1` automatically when `shards + 1 > logical CPUs`; discard those for scaling claims.
If you publish a table, include the `#` header lines of the CSV (CPU, OS, compiler, flags), and keep the
partition-bound column next to the speedup so skew is not mistaken for poor scaling.
