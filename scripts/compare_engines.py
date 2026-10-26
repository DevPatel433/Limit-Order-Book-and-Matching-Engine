#!/usr/bin/env python3
"""Compare the baseline and pooled engines from one lob_bench run (--engine both).

usage: compare_engines.py <bench_out_dir> [<out_dir>]

Writes compare_table.md and compare_{throughput,latency,distribution,memory}.png.

Why paired ratios: lob_bench interleaves the engines rep by rep (alternating who goes first), so
rep i of both engines ran at nearly the same time on the same machine state. The per-rep ratio
pooled/baseline cancels slow drift that comparing two medians would not. A claim of "faster" is
only made when the pooled engine won in (almost) every rep, backed by a two-sided sign test.
"""
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

src = Path(sys.argv[1])
dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src
dst.mkdir(parents=True, exist_ok=True)

res = pd.read_csv(src / "results.csv")
summ = pd.read_csv(src / "summary.csv")
hist = pd.read_csv(src / "latency_histogram.csv")
workloads = list(dict.fromkeys(summ.workload))
first = res.iloc[0]
stamp = (f"{first.cpu_model} ({first.logical_cpus} logical CPU) | {first.os} | {first.compiler} | "
         f"{first.build_type}: {first.cxx_flags} | {res.rep.nunique()} interleaved reps x {int(first.measured_ops):,} ops")
if not bool(first.valid_for_comparison):
    stamp = "NOT VALID FOR COMPARISON | " + stamp


def sign_test_p(wins: int, n: int) -> float:
    """Two-sided exact binomial (p=0.5) probability of a result at least this lopsided."""
    k = max(wins, n - wins)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k, n + 1)) / 2**n)


def S(df, wl, eng):
    return df[(df.workload == wl) & (df.engine == eng)].iloc[0]


def finish(fig, name, title):
    fig.suptitle(title, fontsize=12, fontweight="bold")
    fig.text(0.5, 0.005, stamp, ha="center", va="bottom", fontsize=6.5, color="#444")
    fig.tight_layout(rect=(0, 0.03, 1, 0.94))
    fig.savefig(dst / name, dpi=140)
    plt.close(fig)


rows, ratios = [], {}
for wl in workloads:
    b = res[(res.workload == wl) & (res.engine == "baseline")].set_index("rep").throughput_ops_per_sec
    p = res[(res.workload == wl) & (res.engine == "pooled")].set_index("rep").throughput_ops_per_sec
    r = (p / b).dropna()
    ratios[wl] = r
    wins = int((r > 1).sum())
    pv = sign_test_p(wins, len(r))
    # One rule for every workload: Bonferroni-corrected sign test (alpha = 0.05 / number of workloads).
    alpha = 0.05 / len(workloads)
    if pv < alpha and r.median() > 1.0:
        verdict = "faster" + (" (every rep)" if r.min() > 1.0 else "")
    elif pv < alpha and r.median() < 1.0:
        verdict = "SLOWER"
    else:
        verdict = "inconclusive"
    sb, sp = S(summ, wl, "baseline"), S(summ, wl, "pooled")
    rows.append(dict(
        workload=wl, base=sb.throughput_median_ops_per_sec, pool=sp.throughput_median_ops_per_sec,
        ratio=r.median(), q1=r.quantile(0.25), q3=r.quantile(0.75), rmin=r.min(), rmax=r.max(), wins=wins, n=len(r), p=pv, verdict=verdict,
        p50=(sb.p50_ns, sp.p50_ns), p95=(sb.p95_ns, sp.p95_ns), p99=(sb.p99_ns, sp.p99_ns), p999=(sb.p999_ns, sp.p999_ns),
        mx=(sb.max_ns, sp.max_ns), allocs=(sb.allocs_per_op, sp.allocs_per_op),
        peak=(sb.peak_live_bytes, sp.peak_live_bytes), resting=int(sb.final_resting_orders),
        rss=(sb.rss_kb_end, sp.rss_kb_end)))

# ---- markdown -------------------------------------------------------------------
L = ["### Throughput (paired by repetition)", "",
     "| workload | baseline ops/s | pooled ops/s | median pooled/baseline | IQR | min..max | pooled faster in | sign-test p | verdict (alpha=0.01) |",
     "|---|---:|---:|---:|---:|---:|---:|---:|---|"]
for x in rows:
    L.append(f"| {x['workload']} | {x['base']:,.0f} | {x['pool']:,.0f} | **{x['ratio']:.2f}x** | {x['q1']:.2f}..{x['q3']:.2f} | {x['rmin']:.2f}..{x['rmax']:.2f} | "
             f"{x['wins']}/{x['n']} | {('<0.001' if x['p'] < 0.001 else format(x['p'], '.3f'))} | {x['verdict']} |")
L += ["", "### Latency (ns, pooled over all reps; `baseline -> pooled`)", "",
      "| workload | p50 | p95 | p99 | p99.9 | max |", "|---|---:|---:|---:|---:|---:|"]
f = lambda t: f"{t[0]:,.0f} -> {t[1]:,.0f}" if not (pd.isna(t[0]) or pd.isna(t[1])) else "-"
for x in rows:
    L.append(f"| {x['workload']} | {f(x['p50'])} | {f(x['p95'])} | {f(x['p99'])} | {f(x['p999'])} | {f(x['mx'])} |")
has_heap = "heap_in_use_bytes" in summ.columns
L += ["", "### Allocation and memory (`baseline -> pooled`)", "",
      "| workload | allocs/op | heap in use at end (MiB) | heap bytes per resting order | heap in use after draining the book (MiB) |",
      "|---|---:|---:|---:|---:|"]
for x in rows:
    sb, sp = S(summ, x["workload"], "baseline"), S(summ, x["workload"], "pooled")
    if has_heap:
        hb, hp = sb.heap_in_use_bytes, sp.heap_in_use_bytes
        db, dp = sb.heap_after_drain_bytes, sp.heap_after_drain_bytes
        L.append(f"| {x['workload']} | {x['allocs'][0]:.2f} -> {x['allocs'][1]:.3f} | {hb / 2**20:.1f} -> {hp / 2**20:.1f} | "
                 f"{hb / x['resting']:.0f} -> {hp / x['resting']:.0f} | {db / 2**20:.2f} -> {dp / 2**20:.2f} |")
    else:
        L.append(f"| {x['workload']} | {x['allocs'][0]:.2f} -> {x['allocs'][1]:.3f} | n/a | n/a | n/a |")
L += ["", stamp]
(dst / "compare_table.md").write_text("\n".join(L) + "\n")

# ---- figures --------------------------------------------------------------------
x = np.arange(len(workloads))
fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.3), gridspec_kw={"width_ratios": [3, 2]})
w = 0.38
for i, (eng, color) in enumerate([("baseline", "#8C8C8C"), ("pooled", "#4C72B0")]):
    med = [S(summ, wl, eng).throughput_median_ops_per_sec / 1e6 for wl in workloads]
    lo = [m - S(summ, wl, eng).throughput_min_ops_per_sec / 1e6 for m, wl in zip(med, workloads)]
    hi = [S(summ, wl, eng).throughput_max_ops_per_sec / 1e6 - m for m, wl in zip(med, workloads)]
    a1.bar(x + (i - 0.5) * w, med, w, yerr=[lo, hi], capsize=3, label=eng, color=color)
a1.set_xticks(x, workloads)
a1.tick_params(axis="x", labelrotation=15)
a1.set_ylabel("million commands / second")
a1.set_title("median; whiskers = min..max over reps", fontsize=8)
a1.legend()
for i, wl in enumerate(workloads):
    r = ratios[wl]
    a2.scatter([i] * len(r), r, color="#4C72B0", alpha=0.6, s=18)
    a2.plot([i - 0.25, i + 0.25], [r.median()] * 2, color="black", lw=2)
a2.axhline(1.0, color="red", ls="--", lw=1)
a2.set_xticks(x, workloads)
a2.tick_params(axis="x", labelrotation=25)
a2.set_ylabel("pooled / baseline throughput")
a2.set_title("paired ratio per rep (black = median; red = no change)", fontsize=8)
finish(fig, "compare_throughput.png", "Throughput: baseline vs pooled")

fig, ax = plt.subplots(figsize=(10, 4.4))
cols = [("p50_ns", "p50"), ("p99_ns", "p99"), ("p999_ns", "p99.9")]
ww = 0.13
for j, (c, lab) in enumerate(cols):
    for i, (eng, hatch) in enumerate([("baseline", ""), ("pooled", "//")]):
        vals = [S(summ, wl, eng)[c] for wl in workloads]
        ax.bar(x + (j * 2 + i - 2.5) * ww, vals, ww, label=f"{lab} {eng}", hatch=hatch, color=f"C{j}",
               alpha=0.55 if eng == "baseline" else 1.0, edgecolor="white")
ax.set_yscale("log")
ax.set_xticks(x, workloads)
ax.set_ylabel("latency (ns, log)")
ax.legend(ncol=3, fontsize=7)
finish(fig, "compare_latency.png", "Latency percentiles: baseline (faded) vs pooled (hatched)")

fig, axes = plt.subplots(1, len(workloads), figsize=(3.2 * len(workloads), 3.4), sharey=True)
for ax, wl in zip(np.atleast_1d(axes), workloads):
    for eng, color in [("baseline", "#8C8C8C"), ("pooled", "#4C72B0")]:
        h = hist[(hist.workload == wl) & (hist.engine == eng)]
        ccdf = 1.0 - np.cumsum(h["count"]) / h["count"].sum()
        ax.plot(h.bin_hi_ns[:-1], ccdf[:-1].clip(lower=1e-7), color=color, label=eng)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_title(wl, fontsize=9)
    ax.set_xlabel("latency (ns)")
np.atleast_1d(axes)[0].set_ylabel("P(latency > x)")
np.atleast_1d(axes)[0].legend(fontsize=8)
finish(fig, "compare_distribution.png", "Latency tail (complementary CDF)")

fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4))
col = "heap_in_use_bytes" if has_heap else "peak_live_bytes"
for i, (eng, color) in enumerate([("baseline", "#8C8C8C"), ("pooled", "#4C72B0")]):
    a1.bar(x + (i - 0.5) * w, [S(summ, wl, eng)[col] / 2**20 for wl in workloads], w, label=eng, color=color)
    a2.bar(x + (i - 0.5) * w, [S(summ, wl, eng)[col] / S(summ, wl, eng).final_resting_orders for wl in workloads], w,
           label=eng, color=color)
for a, t in ((a1, "heap in use at end of run (MiB)"), (a2, "heap bytes per resting order")):
    a.set_xticks(x, workloads)
    a.tick_params(axis="x", labelrotation=20)
    a.set_title(t, fontsize=9)
a1.legend()
finish(fig, "compare_memory.png", "Memory: baseline vs pooled (malloc view incl. headers; pooled counts reserved chunks)")
print("wrote compare_table.md and compare_*.png ->", dst)
