#!/usr/bin/env python3
"""plot_3slo.py -- render the 3-SLO baseline (Phase A) results.

Reads the per-event CSV emitted by `workload_3slo` and produces three PNGs:

  <basename>_latency.png    Grouped per-class scheduling latency, one bar
                            group per workload class. The metric is class-
                            specific:
                              deadline   -> per-frame wakeup_delay (ns)
                              throughput -> per-op wall time (ns)
                              latency    -> per-request start_delay (ns)
                            All three measure "how late was this unit of work
                            scheduled" and are plotted together on a log axis.

  <basename>_throughput.png Total ops completed by each workload class in the
                            run, on a log axis.

  <basename>_deadline.png   Per-frame slack for the deadline workload, with
                            the budget line and miss rate annotated.

Usage:
  python3 plot_3slo.py <csv_path> [--target-us 50] [--budget-ms 100]
                       [--out-dir /tmp]
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def load(path):
    rows = defaultdict(list)
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            rows[row["type"]].append(row)
    return rows


def percentile(values, p):
    if not values:
        return 0.0
    arr = np.array(sorted(values), dtype=np.float64)
    return float(np.percentile(arr, p))


# ---------------------------------------------------------------------------
# Grouped per-workload latency chart
# ---------------------------------------------------------------------------
def plot_latency(rows, target_us, out_path):
    # Pull a comparable "scheduling latency in microseconds" sample per class.
    dl_us = [int(r["c_ns"]) / 1000.0 for r in rows.get("deadline", [])
             if int(r.get("c_ns", 0)) > 0]
    tp_us = [int(r["a_ns"]) / 1000.0 for r in rows.get("throughput_op", [])]
    lat_us = [int(r["a_ns"]) / 1000.0 for r in rows.get("latency", [])]

    classes = [
        ("Deadline\n(frame wakeup delay)", dl_us, "#5bc0de"),
        ("Throughput\n(per-op wall time)", tp_us, "#5cb85c"),
        ("Latency\n(request start delay)", lat_us, "#d9534f"),
    ]
    pcts = [50, 90, 99, 99.9]
    labels = ["p50", "p90", "p99", "p99.9", "max"]

    fig, ax = plt.subplots(figsize=(10, 5.5))
    width = 0.25
    x = np.arange(len(labels))
    for i, (name, samples, color) in enumerate(classes):
        if not samples:
            continue
        vals = [percentile(samples, p) for p in pcts] + [max(samples)]
        offset = (i - 1) * width
        bars = ax.bar(x + offset, vals, width, label=name, color=color,
                      edgecolor="black")
        for b, v in zip(bars, vals):
            txt = f"{v:,.0f}" if v >= 10 else f"{v:.1f}"
            ax.text(b.get_x() + b.get_width() / 2,
                    v * 1.10 if v > 0 else 0.5,
                    txt, ha="center", va="bottom", fontsize=7, rotation=0)

    ax.axhline(target_us, color="green", linestyle="--",
               label=f"latency target {target_us} us")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Per-event scheduling latency (us, log scale)")
    ax.set_yscale("log")
    ax.set_title("Per-class scheduling latency (Phase A baseline, "
                 "equal worker counts)")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(axis="y", which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


# ---------------------------------------------------------------------------
# Throughput chart -- ops completed per workload class
# ---------------------------------------------------------------------------
def plot_throughput(rows, out_path):
    dl = len(rows.get("deadline", []))
    lat = len(rows.get("latency", []))
    # For throughput, sum ops_completed across throughput_sum rows (one per
    # worker). Falls back to counting throughput_op rows for older CSVs.
    tp_rows = rows.get("throughput_sum", [])
    if tp_rows:
        tp = sum(int(r["a_ns"]) for r in tp_rows)
    else:
        tp = len(rows.get("throughput_op", []))

    labels = ["Deadline\n(per-class)", "Throughput\n(per-class)",
              "Latency\n(per-class)"]
    vals = [dl, tp, lat]
    colors = ["#5bc0de", "#5cb85c", "#d9534f"]

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, vals, color=colors, edgecolor="black")
    ax.set_ylabel("Total matrix multiplies completed in the run")
    ax.set_yscale("log")
    ax.set_title("Throughput per workload class (Phase A baseline, "
                 "equal worker counts)")
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2,
                v * 1.05 if v > 0 else 0.5,
                f"{v:,}",
                ha="center", va="bottom", fontsize=10)
    ax.grid(axis="y", which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


# ---------------------------------------------------------------------------
# Deadline slack chart
# ---------------------------------------------------------------------------
def plot_deadline(rows, budget_ms, out_path):
    events = rows.get("deadline", [])
    if not events:
        print("no deadline events", file=sys.stderr)
        return
    actual_ms = [int(r["a_ns"]) / 1.0e6 for r in events]
    budget_each_ms = [int(r["b_ns"]) / 1.0e6 for r in events]
    slack_ms = [b - a for a, b in zip(actual_ms, budget_each_ms)]
    missed = sum(1 for r in events if int(r["met_or_target"]) == 0)
    miss_rate = 100.0 * missed / len(events)
    frames = list(range(len(events)))

    fig, ax = plt.subplots(figsize=(10, 5))
    bar_colors = ["#d9534f" if s < 0 else "#5cb85c" for s in slack_ms]
    bars = ax.bar(frames, slack_ms, color=bar_colors, edgecolor="black")
    ax.axhline(0, color="black", linewidth=0.8)
    ax.set_xlabel("Frame index (across all deadline workers, ordered by "
                  "completion)")
    ax.set_ylabel("Slack (ms)  -- positive = met with margin")
    ax.set_title(f"Deadline workload ({budget_ms} ms budget)  --  "
                 f"miss rate {miss_rate:.0f}%  ({missed}/{len(events)})")
    # Only label every Nth bar to avoid clutter when there are many frames.
    step = max(1, len(slack_ms) // 20)
    for i, (b, v) in enumerate(zip(bars, slack_ms)):
        if i % step != 0:
            continue
        ax.text(b.get_x() + b.get_width() / 2,
                v - 8 if v < 0 else v + 2,
                f"{v:.0f}",
                ha="center",
                va="top" if v < 0 else "bottom",
                fontsize=7)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--target-us", type=int, default=50)
    ap.add_argument("--budget-ms", type=int, default=100)
    ap.add_argument("--out-dir", default="/tmp")
    args = ap.parse_args()

    rows = load(args.csv)
    base = os.path.splitext(os.path.basename(args.csv))[0]
    plot_latency(rows, args.target_us,
                 os.path.join(args.out_dir, f"{base}_latency.png"))
    plot_throughput(rows,
                    os.path.join(args.out_dir, f"{base}_throughput.png"))
    plot_deadline(rows, args.budget_ms,
                  os.path.join(args.out_dir, f"{base}_deadline.png"))


if __name__ == "__main__":
    main()
