#!/usr/bin/env bash
# Build, test, and demo the project in one go (about a minute on a laptop).
#
#   scripts/quickstart.sh            # Release build in ./build
#   BUILD_DIR=/tmp/lob scripts/quickstart.sh
#
# Needs: a C++20 compiler, CMake >= 3.21, and network access the first time (GoogleTest is fetched
# unless it is installed). The benchmark plots additionally need python3 + matplotlib + pandas (optional).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${BUILD_DIR:-$root/build}"

step() { printf '\n==> %s\n' "$*"; }

step "configure + build (Release)"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j >/dev/null

step "run the test suite"
ctest --test-dir "$build" --output-on-failure | tail -3

step "replay a hand-traced sample (every command verified by the independent checker)"
"$build/lob_replay" "$root/data/sample/orders.txt" --check

step "same file as CSV (first rows)"
"$build/lob_replay" "$root/data/sample/orders.txt" --csv 2>/dev/null | head -6

step "a reproducible 20,000-command random workload with every order type, on both engines"
"$build/lob_gen" --seed 42 --count 20000 --band 5 --ioc-pct 15 --fok-pct 10 --market-pct 10 --amend-pct 15 > "$build/demo_workload.txt"
"$build/lob_replay" "$build/demo_workload.txt" --check --quiet --no-final | tail -1
"$build/lob_replay" "$build/demo_workload.txt" --check --quiet --no-final --engine pooled | tail -1

step "multi-instrument replay: sequential (checked) vs 4 worker threads (verified identical)"
"$build/lob_mreplay" "$root/data/sample/multi.txt" --check --no-final | tail -4
"$build/lob_mgen" --seed 42 --count 50000 --symbols 8 --band 5 --ioc-pct 10 --fok-pct 8 --market-pct 8 --amend-pct 10 > "$build/demo_multi.txt"
"$build/lob_mreplay" "$build/demo_multi.txt" --threads 4 --verify --quiet --no-final | tail -1

step "write-ahead log: run, then recover the books from the log alone, then recover from a log cut mid-record"
rm -f "$build/demo.wal" "$build/demo_torn.wal"
"$build/lob_mreplay" "$build/demo_multi.txt" --quiet --wal "$build/demo.wal" --no-final | tail -1
"$build/lob_recover" "$build/demo.wal" --quiet
size=$(wc -c < "$build/demo.wal")
head -c $((size - 7)) "$build/demo.wal" > "$build/demo_torn.wal"
"$build/lob_recover" "$build/demo_torn.wal" --quiet || [ $? -eq 3 ]   # exit 3 = recovered after discarding a torn tail

step "benchmark smoke run (tiny sizes; use scripts/run_benchmarks.sh for real numbers)"
"$build/benchmarks/lob_bench" --smoke --out "$build/bench_smoke" | sed -n 1,3p
"$build/benchmarks/lob_scale" --smoke --allow-debug | sed -n 1,3p
"$build/benchmarks/lob_wal" --smoke --allow-debug | sed -n 1,3p
echo "..."

printf '\nAll good. Next: try the interactive view:  %s/lob_shell\n' "$build"
