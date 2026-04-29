#!/usr/bin/env python3
"""plot_hetero_simple.py -- minimal version of plot_hetero.py.

Same per-class KPI bars, but:
  - no chart title
  - no dashed reference lines (target / budget)
  - x-axis labels read "Latency SLO", "Throughput SLO", etc. (no "policy")

Usage:
  python3 plot_hetero_simple.py [--in-dir /tmp] [--out-dir results]
"""
import argparse
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

POLICIES = ["baseline", "latency", "throughput", "deadline"]
LABELS = {
    "baseline":   "Baseline\n(no hints)",
    "latency":    "Latency\nSLO",
    "throughput": "Throughput\nSLO",
    "deadline":   "Deadline\nSLO",
}
COLORS = {
    "baseline":   "#888888",
    "latency":    "#d9534f",
    "throughput": "#5cb85c",
    "deadline":   "#5bc0de",
}


def load(path):
    if not os.path.exists(path):
        return None
    out = []
    with open(path) as f:
        for row in csv.DictReader(f):
            out.append({
                "class":   row["class"],
                "delay":   int(row["wakeup_delay_ns"]) / 1000.0,
                "wall":    int(row["wall_ns"]) / 1000.0,
                "budget":  int(row["budget_ms"]),
                "met":     int(row["met"]) == 1,
            })
    return out


def percentile(vals, p):
    if not vals:
        return 0.0
    return float(np.percentile(np.array(vals, dtype=np.float64), p))


def annotate(ax, bars, vals, fmt):
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2,
                v if v > 0 else 0,
                fmt.format(v),
                ha="center", va="bottom", fontsize=9)


def plot_latency(by_policy, out_path):
    p50 = []
    p99 = []
    labels = []
    colors = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        delays = [e["delay"] for e in evs if e["class"] == "latency"]
        if not delays:
            continue
        p50.append(percentile(delays, 50))
        p99.append(percentile(delays, 99))
        labels.append(LABELS[pol])
        colors.append(COLORS[pol])

    fig, ax = plt.subplots(figsize=(9, 5.5))
    width = 0.4
    x = np.arange(len(labels))
    bars50 = ax.bar(x - width / 2, p50, width, label="p50",
                    color=colors, edgecolor="black", alpha=0.5)
    bars99 = ax.bar(x + width / 2, p99, width, label="p99",
                    color=colors, edgecolor="black")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Wakeup-to-start delay (us, log scale)")
    ax.set_yscale("log")
    annotate(ax, bars50, p50, "{:.0f} us")
    annotate(ax, bars99, p99, "{:.0f} us")
    ax.legend(loc="upper right", fontsize=9)
    ax.grid(axis="y", which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_throughput(by_policy, out_path):
    counts = []
    labels = []
    colors = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        n = sum(1 for e in evs if e["class"] == "throughput")
        counts.append(n)
        labels.append(LABELS[pol])
        colors.append(COLORS[pol])

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, counts, color=colors, edgecolor="black")
    ax.set_ylabel("Throughput-class ops completed")
    annotate(ax, bars, counts, "{:.0f}")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_deadline(by_policy, out_path):
    # Loose-budget miss is essentially always 0 % (the sanity check) so we
    # only plot the tight-budget bar -- one bar per SLO, no legend, no
    # companion loose bars.
    tight = []
    labels = []
    colors = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        t = [e for e in evs if e["class"] == "deadline_tight"]
        if not t:
            continue
        t_miss = sum(1 for e in t if not e["met"]) / len(t) * 100
        tight.append(t_miss)
        labels.append(LABELS[pol])
        colors.append(COLORS[pol])

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, tight, color=colors, edgecolor="black")
    ax.set_ylabel("Tight-deadline miss rate (%)")
    annotate(ax, bars, tight, "{:.1f}%")
    ax.set_ylim(0, max(100, max(tight) * 1.2 if tight else 100))
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", default="/tmp")
    ap.add_argument("--out-dir", default="results")
    args = ap.parse_args()

    by_policy = {}
    for pol in POLICIES:
        path = os.path.join(args.in_dir, f"hetero_{pol}.csv")
        evs = load(path)
        if evs is None:
            print(f"warning: {path} not found, skipping {pol}",
                  file=sys.stderr)
            continue
        by_policy[pol] = evs

    os.makedirs(args.out_dir, exist_ok=True)
    plot_latency(by_policy,
                 os.path.join(args.out_dir, "hetero_latency.png"))
    plot_throughput(by_policy,
                    os.path.join(args.out_dir, "hetero_throughput.png"))
    plot_deadline(by_policy,
                  os.path.join(args.out_dir, "hetero_deadline.png"))


if __name__ == "__main__":
    main()
