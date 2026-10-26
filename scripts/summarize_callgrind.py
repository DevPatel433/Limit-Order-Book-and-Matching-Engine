#!/usr/bin/env python3
"""Summarise callgrind output for lob_bench workloads into cost buckets.

usage: summarize_callgrind.py <measured_ops> <callgrind_*.out> [...]

Costs are *instructions executed* (callgrind 'Ir'), SELF cost per function, bucketed by
what the function is. This is a map of where the work is, not of where time goes:
callgrind models no cache misses, branch mispredictions or memory latency.
"""
import re
import subprocess
import sys
from collections import OrderedDict

BUCKETS = [  # first match wins
    ("allocator (malloc/free/new/delete/memset)", lambda n: any(k in n for k in (
        "malloc", "free", "operator new", "operator delete", "memset", "alloc_perturb", "tcache", "unlink_chunk",
        "_int_", "sysmalloc", "cfree", "brk", "SlabArena"))),
    ("hash index (unordered_map)", lambda n: n.startswith("std::_Hashtable") or "_Prime_rehash" in n
        or n.startswith("std::pair<std::__detail::_Node_iterator") or "_Hash_bytes" in n or n.startswith("std::__detail::_Map_base")),
    ("ordered map (std::map price levels)", lambda n: "_Rb_tree" in n),
    ("FIFO queue (std::list)", lambda n: "_List_node_base" in n or n.startswith("std::__cxx11::_List") or "std::list" in n),
    ("engine logic (MatchingEngine)", lambda n: "lob::MatchingEngine" in n or "lob::BasicMatchingEngine" in n),
    ("book logic (OrderBook, incl. inlined map/list code)", lambda n: "lob::OrderBook" in n or "lob::BasicOrderBook" in n or "lob::PriceLevel" in n or "lob::BasicPriceLevel" in n),
    ("benchmark loop", lambda n: "lob_measured_segment" in n or "lob::bench" in n),
]

LINE = re.compile(r"^\s*([\d,]+)\s+\(\s*([\d.]+)%\)\s+(.*)$")


def short(name: str, width: int = 70) -> str:
    name = re.sub(r"std::allocator<[^>]*(?:<[^>]*>[^>]*)*>", "", name)
    name = re.sub(r"\s+", " ", name)
    return name if len(name) <= width else name[: width - 1] + "…"


def parse(path):
    text = subprocess.run(["callgrind_annotate", "--threshold=100", path], capture_output=True, text=True).stdout
    total, funcs = 0, []
    in_table = False
    for line in text.splitlines():
        if "PROGRAM TOTALS" in line:
            m = LINE.match(line)
            if m:
                total = int(m.group(1).replace(",", ""))
            continue
        if re.search(r"file:function", line):
            in_table = True          # entries follow the dashed rule under this header
            continue
        if not in_table or line.startswith("-----"):
            continue
        if not line.strip():
            if funcs:
                break                # end of the function table; ignore annotated sources below
            continue
        m = LINE.match(line)
        if not m:
            continue
        ir = int(m.group(1).replace(",", ""))
        fm = re.match(r"^(\S+?):(.*)$", m.group(3))
        name = fm.group(2) if fm else m.group(3)
        name = re.sub(r"\s*\[[^\]]*\]\s*$", "", name)
        funcs.append((ir, name))
    return total, funcs


def main():
    ops = int(sys.argv[1])
    print(f"{'workload':<22}{'instr/op':>9}  " + "  ".join(f"{b.split(' (')[0][:11]:>11}" for b, _ in BUCKETS))
    details = []
    for path in sys.argv[2:]:
        wl = re.search(r"callgrind_(.+)\.out", path).group(1).replace("_baseline", " [base]").replace("_pooled", " [pool]")
        total, funcs = parse(path)
        agg = OrderedDict((b, 0) for b, _ in BUCKETS)
        agg["other"] = 0
        for ir, name in funcs:
            for b, pred in BUCKETS:
                if pred(name):
                    agg[b] += ir
                    break
            else:
                agg["other"] += ir
        row = "  ".join(f"{100 * v / total:>10.1f}%" for v in agg.values())
        print(f"{wl:<22}{total / ops:>9.0f}  {row}")
        details.append((wl, total, funcs))
    print("\ncolumns:", " | ".join(list(b for b, _ in BUCKETS) + ["other"]))
    for wl, total, funcs in details:
        print(f"\n== {wl}: top self-cost functions ==")
        for ir, name in sorted(funcs, reverse=True)[:8]:
            print(f"  {100 * ir / total:5.1f}%  {short(name)}")


if __name__ == "__main__":
    main()
