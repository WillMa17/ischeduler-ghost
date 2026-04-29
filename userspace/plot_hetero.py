#!/usr/bin/env python3
"""plot_hetero.py -- charts for the heterogeneous 3-class workload.

Reads four per-event CSVs (one per --hint_mode) and produces three charts.
Each chart focuses on one workload class's KPI and shows it under all
four policies, so the reader can directly see "which policy wins which
class's SLO":

  hetero_latency.png    -- p50 / p99 wakeup-to-start delay for the
                           latency-class workers (us, log scale).
  hetero_throughput.png -- total ops completed by the throughput-class
                           workers in the run.
  hetero_deadline.png   -- deadline miss rate for the tight-budget half
                           of the deadline-class workers (the loose half
                           is always trivially met and is shown as a
                           sanity-check overlay).

Usage:
  python3 plot_hetero.py [--in-dir /tmp] [--out-dir results]
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
POLICY_LABELS = {
    "baseline":   "Baseline\n(no hints)",
    "latency":    "Latency\npolicy",
    "throughput": "Throughput\npolicy",
    "deadline":   "Deadline\npolicy",
}
POLICY_COLORS = {
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


# ---------------------------------------------------------------------------
# Latency chart -- p50 + p99 wakeup-to-start delay for the latency class
# ---------------------------------------------------------------------------
def plot_latency(by_policy, target_us, out_path):
    p50 = []
    p99 = []
    on_time = []
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
        ot = sum(1 for v in delays if v <= target_us) / len(delays) * 100
        on_time.append(ot)
        labels.append(POLICY_LABELS[pol])
        colors.append(POLICY_COLORS[pol])

    fig, ax = plt.subplots(figsize=(9, 5.5))
    width = 0.4
    x = np.arange(len(labels))
    bars50 = ax.bar(x - width / 2, p50, width, label="p50",
                    color=colors, edgecolor="black", alpha=0.5)
    bars99 = ax.bar(x + width / 2, p99, width, label="p99",
                    color=colors, edgecolor="black")
    ax.axhline(target_us, color="green", linestyle="--",
               label=f"target {target_us} us")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Wakeup-to-start delay (us, log scale)")
    ax.set_yscale("log")
    ax.set_title("Latency class wakeup delay  --  per scheduler policy "
                 "(lower is better)")
    annotate(ax, bars50, p50, "{:.0f} us")
    annotate(ax, bars99, p99, "{:.0f} us")
    for xi, ot in zip(x, on_time):
        ax.text(xi, ax.get_ylim()[0] * 1.5,
                f"on-time {ot:.1f}%", ha="center", va="bottom",
                fontsize=8, color="dimgray")
    ax.legend(loc="upper right", fontsize=9)
    ax.grid(axis="y", which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


# ---------------------------------------------------------------------------
# Throughput chart -- total ops completed by the throughput class
# ---------------------------------------------------------------------------
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
        labels.append(POLICY_LABELS[pol])
        colors.append(POLICY_COLORS[pol])

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, counts, color=colors, edgecolor="black")
    ax.set_ylabel("Throughput-class ops completed in run (higher is better)")
    ax.set_title("Throughput class ops  --  per scheduler policy")
    annotate(ax, bars, counts, "{:.0f}")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


# ---------------------------------------------------------------------------
# Deadline chart -- tight + loose miss rate per policy
# ---------------------------------------------------------------------------
def plot_deadline(by_policy, out_path):
    tight = []
    loose = []
    labels = []
    colors = []
    tight_n = []
    for pol in POLICIES:
        evs = by_policy.get(pol)
        if not evs:
            continue
        t = [e for e in evs if e["class"] == "deadline_tight"]
        l = [e for e in evs if e["class"] == "deadline_loose"]
        if not t and not l:
            continue
        t_miss = (sum(1 for e in t if not e["met"]) / len(t) * 100) if t else 0
        l_miss = (sum(1 for e in l if not e["met"]) / len(l) * 100) if l else 0
        tight.append(t_miss)
        loose.append(l_miss)
        tight_n.append(len(t))
        labels.append(POLICY_LABELS[pol])
        colors.append(POLICY_COLORS[pol])

    fig, ax = plt.subplots(figsize=(9, 5.5))
    width = 0.4
    x = np.arange(len(labels))
    bars_t = ax.bar(x - width / 2, tight, width,
                    label="tight-budget miss %", color=colors,
                    edgecolor="black")
    bars_l = ax.bar(x + width / 2, loose, width,
                    label="loose-budget miss %", color=colors,
                    edgecolor="black", alpha=0.4)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Deadline miss rate (%, lower is better)")
    ax.set_title("Deadline class miss rate  --  per scheduler policy")
    annotate(ax, bars_t, tight, "{:.1f}%")
    annotate(ax, bars_l, loose, "{:.1f}%")
    ax.set_ylim(0, max(100, max(tight + loose) * 1.2 if tight else 100))
    ax.legend(loc="upper right", fontsize=9)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", default="/tmp")
    ap.add_argument("--out-dir", default="results")
    ap.add_argument("--target-us", type=int, default=200)
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
    plot_latency(by_policy, args.target_us,
                 os.path.join(args.out_dir, "hetero_latency.png"))
    plot_throughput(by_policy,
                    os.path.join(args.out_dir, "hetero_throughput.png"))
    plot_deadline(by_policy,
                  os.path.join(args.out_dir, "hetero_deadline.png"))


if __name__ == "__main__":
    main()
