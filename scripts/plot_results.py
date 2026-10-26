#!/usr/bin/env python3
"""Plot lob_bench output.

usage: plot_results.py <bench_out_dir> [<png_out_dir>] [engine]

`engine` (baseline|pooled, default baseline) selects which engine to plot when the run contains
both; older single-engine CSVs without an engine column are plotted as is. To compare engines use
compare_engines.py instead.

Reads summary.csv, results.csv and latency_histogram.csv written by lob_bench and writes:
  throughput.png, latency_percentiles.png, latency_distribution.png, allocations.png, summary_table.md
Every figure is stamped with the CPU / compiler / flags it was measured on, because a
number without its machine is not a result.
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

src = Path(sys.argv[1])
dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src
engine = sys.argv[3] if len(sys.argv) > 3 else "baseline"
dst.mkdir(parents=True, exist_ok=True)

summary = pd.read_csv(src / "summary.csv")
results = pd.read_csv(src / "results.csv")
hist = pd.read_csv(src / "latency_histogram.csv")
if "engine" in summary.columns:
    summary, results, hist = (d[d.engine == engine].reset_index(drop=True) for d in (summary, results, hist))
first = results.iloc[0]
valid = bool(first.valid_for_comparison)
stamp = (f"{first.cpu_model} ({first.logical_cpus} logical CPU) | {first.os} | {first.compiler} | "
         f"{first.build_type}: {first.cxx_flags} | pinned_cpu={first.pinned_cpu} | "
         f"{int(summary.reps.iloc[0])} reps x {int(summary.measured_ops.iloc[0]):,} ops")
if not valid:
    stamp = "NOT VALID FOR COMPARISON (assertions/sanitizers on)  |  " + stamp
timer = float(summary.timer_overhead_ns.iloc[0])
order = list(summary.workload)


def finish(fig, name, title):
    fig.suptitle(title, fontsize=12, fontweight="bold")
    fig.text(0.5, 0.005, stamp, ha="center", va="bottom", fontsize=6.5, color="#444")
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))
    fig.savefig(dst / name, dpi=140)
    plt.close(fig)


# 1. throughput ------------------------------------------------------------------
fig, ax = plt.subplots(figsize=(8, 4.2))
med = summary.throughput_median_ops_per_sec / 1e6
lo = med - summary.throughput_min_ops_per_sec / 1e6
hi = summary.throughput_max_ops_per_sec / 1e6 - med
bars = ax.bar(order, med, yerr=[lo, hi], capsize=5, color="#4C72B0")
for b, v in zip(bars, med):
    ax.text(b.get_x() + b.get_width() / 2, 0.02, f"{v:.2f}M", ha="center", va="bottom", color="white", fontsize=9)
ax.set_ylabel("million commands / second")
ax.set_title("median of reps; whiskers = min..max across reps (run-to-run noise)", fontsize=8)
ax.tick_params(axis="x", labelrotation=15)
finish(fig, "throughput.png", "Throughput by workload")

# 2. latency percentiles ---------------------------------------------------------
pcols = [("p50_ns", "p50"), ("p95_ns", "p95"), ("p99_ns", "p99"), ("p999_ns", "p99.9")]
fig, ax = plt.subplots(figsize=(9, 4.4))
w = 0.2
x = np.arange(len(order))
for i, (col, label) in enumerate(pcols):
    vals = summary[col].astype(float)
    ax.bar(x + (i - 1.5) * w, vals, w, label=label)
ax.axhline(timer, color="red", ls="--", lw=1, label=f"clock read overhead ({timer:.0f} ns)")
ax.set_yscale("log")
ax.set_xticks(x, order)
ax.tick_params(axis="x", labelrotation=15)
ax.set_ylabel("per-command latency (ns, log scale)")
ax.set_title("pooled over all reps; each timed value includes ~2 clock reads; missing bar = too few samples", fontsize=8)
ax.legend(ncol=5, fontsize=8)
finish(fig, "latency_percentiles.png", "Latency percentiles by workload")

# 3. latency distribution --------------------------------------------------------
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.4))
for name in order:
    h = hist[hist.workload == name]
    mid = np.sqrt(h.bin_lo_ns * h.bin_hi_ns)
    frac = h["count"] / h["count"].sum()
    ax1.plot(mid, frac, marker=".", label=name)
    ccdf = 1.0 - np.cumsum(h["count"]) / h["count"].sum()
    ax2.plot(h.bin_hi_ns[:-1], ccdf[:-1].clip(lower=1e-7), label=name)
ax1.set_xscale("log")
ax1.set_xlabel("latency (ns)")
ax1.set_ylabel("fraction of commands per bin")
ax1.set_title("distribution (log-spaced bins)", fontsize=9)
ax2.set_xscale("log")
ax2.set_yscale("log")
ax2.set_xlabel("latency (ns)")
ax2.set_ylabel("P(latency > x)")
ax2.set_title("tail: complementary CDF", fontsize=9)
for a in (ax1, ax2):
    a.axvline(timer, color="red", ls="--", lw=0.8)
ax1.legend(fontsize=8)
finish(fig, "latency_distribution.png", "Latency distribution by workload (red dashed = clock read overhead)")

# 4. allocation behaviour --------------------------------------------------------
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(9, 4))
ax1.bar(order, summary.allocs_per_op, color="#DD8452")
ax1.set_ylabel("heap allocations per command")
ax1.tick_params(axis="x", labelrotation=25)
ax2.bar(order, summary.peak_live_bytes / 2**20, color="#55A868")
ax2.set_ylabel("peak live heap (MiB), whole run")
ax2.tick_params(axis="x", labelrotation=25)
finish(fig, "allocations.png", "Allocation behaviour (measured segment / whole run)")

# 5. markdown table --------------------------------------------------------------
def cell(v, fmt="{:,.0f}"):
    return "-" if pd.isna(v) else fmt.format(v)

lines = ["| workload | ops/s (median) | min..max ops/s | p50 ns | p95 ns | p99 ns | p99.9 ns | max ns | allocs/op | trades/op | resting orders at end |",
         "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
for _, r in summary.iterrows():
    lines.append(
        f"| {r.workload} | {r.throughput_median_ops_per_sec:,.0f} | {r.throughput_min_ops_per_sec/1e6:.2f}M..{r.throughput_max_ops_per_sec/1e6:.2f}M | "
        f"{cell(r.p50_ns)} | {cell(r.p95_ns)} | {cell(r.p99_ns)} | {cell(r.p999_ns)} | {cell(r.max_ns)} | "
        f"{r.allocs_per_op:.2f} | {r.trades_per_op:.2f} | {int(r.final_resting_orders):,} |")
(dst / "summary_table.md").write_text("\n".join(lines) + "\n\n" + stamp + "\n")
print("wrote", ", ".join(p.name for p in sorted(dst.glob("*.png"))), "and summary_table.md ->", dst)
