# Benchmarking and profiling

Goal of Phase 4: **measure before optimising**. This establishes a baseline for the
`std::map` + `std::list` + `std::unordered_map` design so Phase 5 can be judged
against evidence rather than intuition.

## Run it
```bash
scripts/run_benchmarks.sh bench_out --pin-cpu 2      # build Release, run all, plot
# or by hand
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel --target lob_bench
./build-rel/benchmarks/lob_bench --out bench_out --measured 200000 --warmup 20000 --reps 5
./build-rel/benchmarks/lob_bench --workload bursty --measured 1000000 --reps 10
./build-rel/benchmarks/lob_bench --engine pooled            # one implementation only (default: both)
python3 scripts/compare_engines.py bench_out                # paired baseline-vs-pooled analysis
python3 scripts/plot_results.py bench_out bench_out baseline   # plots for one engine
```
`lob_bench` **refuses to run** a build with assertions (`NDEBUG` unset) or sanitizers
unless you pass `--allow-debug`; such results are tagged `valid_for_comparison=0` and the
plots are stamped "NOT VALID FOR COMPARISON". This is how "don't compare Debug to Release"
is enforced instead of merely documented.

## Workloads
All are generated deterministically from `--seed` by running the real engine (untimed) so
every cancel targets a genuinely live order.

| Workload | Shape | What it stresses |
|---|---|---|
| `non_crossing` | add-only, 100 levels/side, every order rests; book grows to ~220k orders | pure insertion cost, allocation, ID-index growth |
| `cancel_heavy` | steady ~20k-order book; 50% adds / 50% cancels of random live orders | cancel path, deallocation |
| `crossing_heavy` | tight price band; ~0.75 trades per command; 5% cancels | the matching loop, `execute_front`, event output |
| `many_levels` | 20,000 levels/side, ~100k resting orders, 50/50 add/cancel | ordered-map depth, cache behaviour |
| `bursty` | 900 quiet quote updates, then 100 sweeps of several levels each, repeating | latency tail under bursts |

## Methodology
* **Three passes, fresh engine each**, so no measurement contaminates another:
  1. *throughput* — the whole measured segment is timed once; no per-command clock reads;
  2. *latency* — every measured command timed individually with `std::chrono::steady_clock`;
  3. *allocation* — once per workload, untimed, with counting `operator new/delete`.
* **Warm-up** (prefill + `--warmup` commands) runs first and is never recorded. "Events per
  second" means input commands (submits and cancels) per second.
* **Clock overhead** is measured and written to every output; a timed value includes about two
  clock reads. Latencies near that floor are not meaningful.
* **Percentiles** are nearest-rank on sorted samples and reported only when at least 10
  samples lie above them: p99 needs 1,000 samples, p99.9 needs 10,000. Otherwise the CSV cell
  is empty. (Unit-tested in `tests/test_bench.cpp`.)
* **Engines** (`--engine baseline|pooled|both`, default both): with `both`, repetitions are **interleaved** and the
  first engine alternates every rep, so machine drift cannot favour one. `compare_engines.py` compares them with
  per-repetition (paired) ratios and a sign test, and only calls a difference real if p < 0.05/#workloads.
* **Repetitions** replay the identical command list; `results.csv` has one row per rep so the
  spread (run-to-run noise) is visible, and `summary.csv` pools latency samples across reps.
* **Allocation metrics** count calls to global `operator new` during the measured segment.
  `peak_live_bytes` uses `malloc_usable_size` (glibc; it hides malloc headers/rounding, so it flatters the
  baseline). Prefer `heap_in_use_bytes` (mallinfo2) for memory comparisons.
* **Environment** (CPU model, logical CPUs, OS/kernel, compiler, build type, exact flags,
  governor, pinned CPU, timestamp) is written into every row of `results.csv` and into
  `environment.txt`.

### What is *not* controlled
Turbo/frequency scaling, SMT siblings, other tenants on the host, and interrupts are not
controlled by the tool. For credible numbers: use bare metal or a dedicated machine, set the
performance governor, pin with `--pin-cpu`, close other work, use `--reps 10` or more and look
at the min..max spread. Do not use `-march=native` results as a portable claim.

## Output files
| File | Contents |
|---|---|
| `results.csv` | one row per workload x engine x repetition: throughput, p50/p95/p99/p99.9, min/mean/max, environment |
| `summary.csv` | one row per workload: median/min/max throughput, pooled percentiles, allocs/op, bytes/op, peak live bytes, final book shape, RSS, `heap_in_use_bytes` and `heap_after_drain_bytes` (mallinfo2, includes malloc overhead; the latter after cancelling every resting order) |
| `latency_histogram.csv` | pooled log-spaced histogram (4 bins/octave) for plotting |
| `environment.txt` | key=value environment + the methodology text |

## Finding individual latency spikes
`lob_spikes [workload] [threshold_us] [reps] [measured] [baseline|pooled]` lists the commands above a threshold in each
repetition. Spikes at the *same* command index every rep come from the workload and data structures
(e.g. hash-table rehash); spikes at random indices are OS/VM noise.

## Profiling
```bash
scripts/profile.sh bursty 200000 [baseline|pooled]
```
* With **`perf`** available it records with call graphs (`perf record -g`) and prints a report.
* Otherwise it uses **callgrind** (`valgrind --tool=callgrind`) restricted to the measured segment
  (`--toggle-collect='*lob_measured_segment*'`), excluding workload generation, prefill and warm-up.
  Callgrind counts *instructions executed*; it is deterministic but models no cache misses or
  branch mispredictions. Read it as "where the work is", then confirm with `perf` on real hardware.
* `scripts/summarize_callgrind.py` buckets self cost into allocator / hash index / ordered map /
  list / engine / book. Add `--cache-sim=yes --D1=... --LL=...` to callgrind for simulated misses.

Both need symbols: the script builds `build-prof/` as `-O3 -g -DNDEBUG -fno-omit-frame-pointer`.

## Phase 7: scaling benchmark (`lob_scale`)
Same principles, plus three rules specific to concurrency ([results_phase7.md](results_phase7.md)):
1. **Correctness gate on every timed run.** Aggregate statistics must equal the sequential reference's or the benchmark aborts; a fast wrong answer is never reported.
2. **Oversubscription is flagged, not hidden.** A row where `shards + 1 > logical CPUs` is marked `oversubscribed=1`; such rows measure scheduling overhead and must not be used for scaling claims.
3. **Skew is reported next to speedup.** The *partition bound* (`1 / largest shard share`, computed from the workload alone) is printed with every row, so a hot symbol is not mistaken for poor parallel efficiency.
Modes are interleaved per repetition; timing covers thread start through merge and excludes workload generation.
