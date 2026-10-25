| workload | ops/s (median) | min..max ops/s | p50 ns | p95 ns | p99 ns | p99.9 ns | max ns | allocs/op | trades/op | resting orders at end |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| non_crossing | 9,374,274 | 8.05M..9.60M | 101 | 166 | 289 | 2,510 | 12,128,866 | 2.00 | 0.00 | 220,000 |
| cancel_heavy | 6,707,230 | 6.43M..7.17M | 154 | 319 | 409 | 2,301 | 441,564 | 1.00 | 0.00 | 20,096 |
| crossing_heavy | 8,032,087 | 7.36M..8.40M | 127 | 322 | 464 | 2,749 | 1,035,881 | 1.39 | 0.75 | 31,830 |
| many_levels | 1,902,577 | 1.77M..2.16M | 548 | 866 | 1,071 | 8,613 | 921,229 | 1.04 | 0.00 | 99,682 |
| bursty | 8,387,976 | 6.68M..8.76M | 123 | 388 | 740 | 3,066 | 288,731 | 1.27 | 0.49 | 13,702 |

Intel(R) Xeon(R) Processor @ 2.10GHz (1 logical CPU) | Linux 6.18.44-fc-v64 x86_64 | gcc 13.3.0 | Release: -O3 -DNDEBUG | pinned_cpu=-1 | 5 reps x 200,000 ops
