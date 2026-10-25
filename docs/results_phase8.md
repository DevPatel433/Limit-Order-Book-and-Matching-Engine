# Phase 8 results: what the write-ahead log costs, and how fast recovery is

Unlike Phase 7, this question **can** be answered on the development machine: durability cost is mostly about
syscalls and the storage device, not core count. The caveat is the storage: these numbers are for one virtual disk, and
the `fsync` rows especially must not be generalised.

## Environment
[phase8_run/environment.txt](results/phase8_run/environment.txt) and every CSV row: Intel Xeon @ 2.10 GHz VM, 1 logical CPU,
Linux 6.18 x86_64, gcc 13.3.0, Release (`-O3 -DNDEBUG`), `valid_for_comparison=1`. Log directory `/tmp` on **ext4 over a virtio
block device (`/dev/vda`)**, a shared VM, not local NVMe and not a battery-backed controller. Noisy machine: see
[limitations.md](limitations.md).

## Method
Same rules as before ([benchmarking.md](benchmarking.md)): workload generated outside the timed region (500,000 commands over
16 symbols: 20% cancels, 5% IOC, 3% market, 5% amend); modes **interleaved** inside each of 5 repetitions; medians reported;
**correctness gates** on every run (a journalled run must reach the same trades, traded quantity and accepted count as the
unjournalled one, and recovery must reproduce the run exactly or the benchmark aborts). Timing covers journalling, matching
and the final flush and close; events are discarded. The `fsync`-every-record mode runs 20,000 commands (it is
about a thousand times slower) and is compared with a no-log run of the same 20,000. Raw rows: [wal.csv](results/phase8_run/wal.csv).

## Results
| mode | commands | median s | commands/s | slowdown vs no log | log bytes/command |
|---|---:|---:|---:|---:|---:|
| no log | 500,000 | 0.094 | 5.34 M | 1.00x | - |
| log, never `fsync` | 500,000 | 0.197 | 2.54 M | 2.11x | 40.2 |
| log, `fsync` every 1000 records | 500,000 | 0.445 | 1.12 M | 4.76x | 40.2 |
| log, `fsync` every 100 records | 500,000 | 1.918 | 0.26 M | 20.5x | 40.2 |
| no log (small) | 20,000 | 0.0035 | 5.69 M | 1.00x | - |
| log, `fsync` every record | 20,000 | 3.850 | 5.2 k | 1096x | 40.3 |

**Recovery** (scan + CRC + decode + replay) of the 500,000-command, 20 MB log: median **0.228 s, 2.19 million commands/s**,
about 40% of the speed of matching alone, so recovery time is roughly "log length divided by two million per second" on this machine.

## What these numbers do and do not say
* **Writing the log at all costs about 2x** (never `fsync`): (0.197 - 0.094) s over 500,000 commands is roughly
  **200 ns per command** on top of matching. I did **not** profile where it goes. The candidates are all in my own code
  rather than the disk: a byte-at-a-time table CRC-32 over about 40 bytes, one `fwrite` per record, a string-keyed
  `std::map` lookup of the symbol on every append, and the page-cache writes. I cannot say how the 200 ns splits, or how much a
  faster CRC or batched writes would recover; none of that was attempted.
* **`fsync` dominates everything once it is frequent.** On this virtual disk one `fsync` costs about 190 microseconds
  (3.85 s / 20,000), so "durable before acknowledging every command" caps throughput near **5,000 commands/s**, a 1,000x
  slowdown. Syncing every 1000 records costs 4.8x in total and bounds the loss on a power failure to 999 commands. The
  real-world answer is **group commit** (many commands per `fsync`, acknowledging only after it), which is not built.
* **Not transferable:** the absolute `fsync` latency. NVMe with a power-loss-protected cache, a battery-backed RAID
  controller, a network disk, and a laptop SSD differ by orders of magnitude, and some storage acknowledges `fsync` before
  data is durable. Re-run `lob_wal --dir <your storage>` before drawing a conclusion for any other system. The *shape* (a
  fixed per-`fsync` cost amortised over the sync interval) is general; the constants are not.
* **Recovery speed is not startup latency for a large book of business.** There are no checkpoints, so a day-long log would
  replay in full. At 2 million commands per second, 100 million logged commands is under a minute: tolerable, but it grows
  without bound, which is why checkpointing is the first next step.
* **Correctness evidence is the stronger result** ([phase8_design.md](phase8_design.md)): every timed run was gated on the
  journalled results equalling the unjournalled ones, and every recovery on reproducing its run.

## Reproduce
```bash
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/lob_wal --count 500000 --sync1-count 20000 --symbols 16 --reps 5 --dir /path/on/your/storage --out wal.csv
```
`--smoke` runs a tiny version for CI. Include the `#` header lines of the CSV (CPU, OS, compiler, flags, directory) when quoting
numbers, and keep the `fsync` rows next to a description of the storage.
