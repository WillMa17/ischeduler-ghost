#!/usr/bin/env python3
"""plot_unified.py -- compare scheduler policies on the unified workload.

Reads four per-event CSVs (one per --hint_mode) and produces three charts,
one per KPI. Each chart has four bars (one per scheduler policy) so the
reader can directly see "which policy wins which KPI."

Inputs (defaults assume the layout written by run_unified_experiment.sh):
  /tmp/unified_none.csv         baseline (no hints)
  /tmp/unified_latency.csv      latency-prioritising policy
  /tmp/unified_throughput.csv   throughput-prioritising policy
  /tmp/unified_deadline.csv     deadline-prioritising policy

Outputs:
  unified_latency.png    p99 wakeup-to-start delay per policy (us, log)
  unified_throughput.png total events completed per policy (count, linear)
  unified_deadline.png   deadline miss rate per policy (%)

Usage:
  python3 plot_unified.py [--in-dir /tmp] [--out-dir results]
"""
import argparse
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

POLICIES = ["none", "latency", "throughput", "deadline"]
POLICY_LABELS = {
    "none":       "Baseline\n(no hints)",
    "latency":    "Latency\npolicy",
    "throughput": "Throughput\npolicy",
    "deadline":   "Deadline\npolicy",
}
POLICY_COLORS = {
    "none":       "#888888",
    "latency":    "#d9534f",
    "throughput": "#5cb85c",
    "deadline":   "#5bc0de",
}


def load(path):
    if not os.path.exists(path):
        return None
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({
                "wakeup_us": int(row["wakeup_delay_ns"]) / 1000.0,
                "wall_us":   int(row["wall_ns"]) / 1000.0,
                "met":       int(row["met"]) == 1,
            })
    return rows


def percentile(vals, p):
    if not vals:
        return 0.0
    return float(np.percentile(np.array(vals, dtype=np.float64), p))


def annotate_value(ax, bars, vals, fmt):
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2,
                v if v > 0 else 0,
                fmt.format(v),
                ha="center", va="bottom", fontsize=9)


def plot_latency(by_policy, target_us, out_path):
    """One bar per policy, height = p99 wakeup-to-start delay (us, log)."""
    p50 = []
    p99 = []
    on_time_pct = []
    labels = []
    colors = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        wks = [e["wakeup_us"] for e in evs]
        p50.append(percentile(wks, 50))
        p99.append(percentile(wks, 99))
        ot = sum(1 for v in wks if v <= target_us) / len(wks) * 100
        on_time_pct.append(ot)
        labels.append(POLICY_LABELS[pol])
        colors.append(POLICY_COLORS[pol])

    fig, ax = plt.subplots(figsize=(9, 5.5))
    width = 0.4
    x = np.arange(len(labels))
    bars50 = ax.bar(x - width / 2, p50, width, label="p50",
                    color=[c for c in colors],
                    edgecolor="black", alpha=0.5)
    bars99 = ax.bar(x + width / 2, p99, width, label="p99",
                    color=colors, edgecolor="black")
    ax.axhline(target_us, color="green", linestyle="--",
               label=f"latency target {target_us} us")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Wakeup-to-start delay (us, log scale)")
    ax.set_yscale("log")
    ax.set_title("Latency KPI per scheduler policy "
                 "(uniform workload, lower is better)")
    annotate_value(ax, bars50, p50, "{:.0f} us")
    annotate_value(ax, bars99, p99, "{:.0f} us")
    # Overlay the on-time rate as text under each policy.
    for i, (xi, ot) in enumerate(zip(x, on_time_pct)):
        ax.text(xi, ax.get_ylim()[0] * 1.5, f"on-time {ot:.1f}%",
                ha="center", va="bottom", fontsize=8, color="dimgray")
    ax.legend(loc="upper right", fontsize=9)
    ax.grid(axis="y", which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_throughput(by_policy, out_path):
    """One bar per policy, height = total events completed."""
    counts = []
    labels = []
    colors = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        counts.append(len(evs))
        labels.append(POLICY_LABELS[pol])
        colors.append(POLICY_COLORS[pol])

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, counts, color=colors, edgecolor="black")
    ax.set_ylabel("Total events completed in the run (higher is better)")
    ax.set_title("Throughput KPI per scheduler policy "
                 "(uniform workload)")
    annotate_value(ax, bars, counts, "{:.0f}")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_deadline(by_policy, budget_ms, out_path):
    """One bar per policy, height = % of events that missed the deadline."""
    miss_pct = []
    labels = []
    colors = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        missed = sum(1 for e in evs if not e["met"])
        miss_pct.append(100.0 * missed / len(evs))
        labels.append(POLICY_LABELS[pol])
        colors.append(POLICY_COLORS[pol])

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, miss_pct, color=colors, edgecolor="black")
    ax.set_ylabel(f"Deadline miss rate (%, lower is better; {budget_ms} ms "
                  f"budget)")
    ax.set_title("Deadline KPI per scheduler policy "
                 "(uniform workload)")
    annotate_value(ax, bars, miss_pct, "{:.1f}%")
    ax.set_ylim(0, max(100, max(miss_pct) * 1.2 if miss_pct else 100))
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", default="/tmp",
                    help="Where the per-policy CSVs live.")
    ap.add_argument("--out-dir", default="results",
                    help="Where to write the PNG charts.")
    ap.add_argument("--target-us", type=int, default=50)
    ap.add_argument("--budget-ms", type=int, default=35)
    args = ap.parse_args()

    by_policy = {}
    for pol in POLICIES:
        path = os.path.join(args.in_dir, f"unified_{pol}.csv")
        evs = load(path)
        if evs is None:
            print(f"warning: {path} not found, skipping {pol}",
                  file=sys.stderr)
            continue
        by_policy[pol] = evs

    os.makedirs(args.out_dir, exist_ok=True)
    plot_latency(by_policy, args.target_us,
                 os.path.join(args.out_dir, "unified_latency.png"))
    plot_throughput(by_policy,
                    os.path.join(args.out_dir, "unified_throughput.png"))
    plot_deadline(by_policy, args.budget_ms,
                  os.path.join(args.out_dir, "unified_deadline.png"))


if __name__ == "__main__":
    main()
