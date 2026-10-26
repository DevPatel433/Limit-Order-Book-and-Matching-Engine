#!/usr/bin/env python3
"""A/B two lob_bench binaries (e.g. before/after a change) on the same machine.

usage: compare_builds.py <old_lob_bench> <new_lob_bench> [rounds=6] [reps=3] [measured=200000]

Each round runs three arms back to back, rotating the order every round so no arm is always first:
    old, new, and old AGAIN (the A/A control).
The A/A arm shows how large a "difference" is produced by noise alone on this machine; only a
new/old ratio clearly outside that spread means anything. Ratios are paired per round, summarised
by median and IQR, with an exact two-sided sign test.
"""
import csv
import math
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

old_bin, new_bin = sys.argv[1], sys.argv[2]
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 6
reps = int(sys.argv[4]) if len(sys.argv) > 4 else 3
measured = int(sys.argv[5]) if len(sys.argv) > 5 else 200000
arms = {"old": old_bin, "new": new_bin, "old_again": old_bin}


def run(binary):
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([binary, "--out", d, "--measured", str(measured), "--warmup", "20000", "--reps", str(reps)],
                       check=True, capture_output=True)
        out = {}
        with open(Path(d) / "summary.csv") as f:
            for row in csv.DictReader(f):
                out[(row["workload"], row.get("engine", "baseline"))] = float(row["throughput_median_ops_per_sec"])
        return out


def sign_p(wins, n):
    k = max(wins, n - wins)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k, n + 1)) / 2**n)


data = {a: [] for a in arms}
names = list(arms)
for r in range(rounds):
    order = names[r % 3:] + names[:r % 3]
    for a in order:
        data[a].append(run(arms[a]))
    print(f"round {r + 1}/{rounds} done", file=sys.stderr)

keys = list(data["old"][0])
print(f"| workload | engine | new/old median | IQR | new faster in | A/A (old again / old) median | A/A IQR |")
print("|---|---|---:|---:|---:|---:|---:|")
for k in keys:
    def ratios(a):
        return [data[a][i][k] / data["old"][i][k] for i in range(rounds)]
    rn, ra = ratios("new"), ratios("old_again")
    q = lambda v: statistics.quantiles(v, n=4) if len(v) >= 4 else [min(v), statistics.median(v), max(v)]
    wins = sum(x > 1 for x in rn)
    qn, qa = q(rn), q(ra)
    print(f"| {k[0]} | {k[1]} | **{statistics.median(rn):.3f}** | {qn[0]:.2f}..{qn[-1]:.2f} | {wins}/{rounds} (p={sign_p(wins, rounds):.2f}) | "
          f"{statistics.median(ra):.3f} | {qa[0]:.2f}..{qa[-1]:.2f} |")
