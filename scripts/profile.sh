#!/usr/bin/env bash
# Profile the measured segment of one benchmark workload.
#
#   scripts/profile.sh <workload> [measured_ops] [baseline|pooled]
#
# Uses `perf` (sampling, real timing, needs perf_event access) if available,
# otherwise `valgrind --tool=callgrind` (instruction counts: deterministic, but they
# model no cache misses or branch mispredictions, so treat them as "where the work is",
# not "where the time is"). Builds an optimised build WITH symbols in build-prof/.
set -euo pipefail
workload="${1:?usage: profile.sh <workload> [measured_ops]}"
measured="${2:-200000}"
engine="${3:-baseline}"
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-prof"
out="$root/profile_out"
mkdir -p "$out"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O3 -g -DNDEBUG -fno-omit-frame-pointer" \
      -DLOB_BUILD_TESTS=OFF >/dev/null
cmake --build "$build" --target lob_bench -j >/dev/null
bench="$build/benchmarks/lob_bench"
args=(--workload "$workload" --measured "$measured" --warmup 20000 --reps 1 --profile --engine "$engine")
tag="${workload}_${engine}"

if command -v perf >/dev/null && perf stat -e task-clock true >/dev/null 2>&1; then
  echo "== perf record ($workload, $engine) =="
  perf record -F 5000 -g --call-graph fp -o "$out/perf_$tag.data" -- "$bench" "${args[@]}"
  perf report -i "$out/perf_$tag.data" --no-children --stdio --percent-limit 1 \
      --sort symbol 2>/dev/null | head -60 | tee "$out/perf_$tag.txt"
elif command -v valgrind >/dev/null; then
  echo "== callgrind ($workload, $engine) =="
  valgrind --tool=callgrind --callgrind-out-file="$out/callgrind_$tag.out" \
           --collect-atstart=no --toggle-collect='*lob_measured_segment*' \
           "$bench" "${args[@]}" 2>&1 | tail -4
  callgrind_annotate --threshold=97 "$out/callgrind_$tag.out" \
      | sed -n '1,45p' | tee "$out/callgrind_$tag.txt"
else
  echo "neither perf nor valgrind found; install one (e.g. apt install linux-tools-generic or valgrind)" >&2
  exit 1
fi
