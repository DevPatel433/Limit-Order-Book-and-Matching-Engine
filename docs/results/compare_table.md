### Throughput (paired by repetition)

| workload | baseline ops/s | pooled ops/s | median pooled/baseline | IQR | min..max | pooled faster in | sign-test p | verdict (alpha=0.01) |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| non_crossing | 10,303,701 | 13,000,575 | **1.23x** | 1.12..1.30 | 0.87..1.77 | 18/20 | <0.001 | faster |
| cancel_heavy | 6,648,338 | 8,225,416 | **1.23x** | 1.20..1.27 | 0.98..1.39 | 19/20 | <0.001 | faster |
| crossing_heavy | 8,106,524 | 10,399,669 | **1.27x** | 1.23..1.35 | 0.80..1.72 | 18/20 | <0.001 | faster |
| many_levels | 2,082,337 | 2,215,593 | **1.08x** | 0.95..1.16 | 0.82..1.53 | 11/20 | 0.824 | inconclusive |
| bursty | 7,792,735 | 9,694,984 | **1.22x** | 1.19..1.35 | 0.82..1.90 | 19/20 | <0.001 | faster |

### Latency (ns, pooled over all reps; `baseline -> pooled`)

| workload | p50 | p95 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| non_crossing | 104 -> 89 | 153 -> 136 | 264 -> 200 | 1,473 -> 401 | 4,282,717 -> 1,610,116 |
| cancel_heavy | 160 -> 140 | 330 -> 293 | 416 -> 373 | 626 -> 561 | 1,139,090 -> 1,452,338 |
| crossing_heavy | 129 -> 106 | 324 -> 258 | 464 -> 372 | 826 -> 639 | 1,713,454 -> 1,955,293 |
| many_levels | 570 -> 526 | 963 -> 848 | 1,208 -> 1,052 | 11,746 -> 9,316 | 2,690,319 -> 3,078,309 |
| bursty | 121 -> 100 | 413 -> 342 | 816 -> 612 | 1,574 -> 1,035 | 2,219,456 -> 2,129,339 |

### Allocation and memory (`baseline -> pooled`)

| workload | allocs/op | heap in use at end (MiB) | heap bytes per resting order | heap in use after draining the book (MiB) |
|---|---:|---:|---:|---:|
| non_crossing | 2.00 -> 0.001 | 26.4 -> 23.1 | 126 -> 110 | 2.87 -> 23.06 |
| cancel_heavy | 1.00 -> 0.000 | 2.5 -> 2.3 | 132 -> 119 | 0.35 -> 2.28 |
| crossing_heavy | 1.39 -> 0.000 | 3.9 -> 3.4 | 129 -> 114 | 0.51 -> 3.45 |
| many_levels | 1.04 -> 0.000 | 15.5 -> 14.1 | 163 -> 148 | 1.51 -> 14.07 |
| bursty | 1.27 -> 0.000 | 1.8 -> 2.3 | 139 -> 175 | 0.35 -> 2.28 |

Intel(R) Xeon(R) Processor @ 2.10GHz (1 logical CPU) | Linux 6.18.44-fc-v64 x86_64 | gcc 13.3.0 | Release: -O3 -DNDEBUG | 20 interleaved reps x 200,000 ops
