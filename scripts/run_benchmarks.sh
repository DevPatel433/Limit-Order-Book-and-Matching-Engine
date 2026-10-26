#!/usr/bin/env bash
# Build Release, run the full benchmark suite, and plot. Usage:
#   scripts/run_benchmarks.sh [out_dir] [extra lob_bench args...]
# Example (recommended on a real machine):  scripts/run_benchmarks.sh bench_out --pin-cpu 2
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/bench_out}"
shift || true
cmake -S "$root" -B "$root/build-rel" -DCMAKE_BUILD_TYPE=Release -DLOB_BUILD_TESTS=OFF >/dev/null
cmake --build "$root/build-rel" --target lob_bench -j >/dev/null
"$root/build-rel/benchmarks/lob_bench" --out "$out" "$@"
python3 "$root/scripts/plot_results.py" "$out" "$out"
