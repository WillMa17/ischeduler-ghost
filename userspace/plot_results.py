#!/usr/bin/env python3
"""
plot_results.py — Generate comparison graphs for iSchedular evaluation.

Reads the CSV files produced by workload_mixed and generates:
  1. CDF of server thread latencies (baseline vs hint-aware)
  2. Bar chart of percentile comparisons (p50, p90, p95, p99, p999)
  3. Summary statistics table

Usage:
  python3 plot_results.py /tmp/ischeduler_eval/

Requires: pip install matplotlib pandas (install inside the VM or copy CSVs out)
"""

import sys
import os

try:
    import pandas as pd
    import matplotlib
    matplotlib.use('Agg')  # headless
    import matplotlib.pyplot as plt
    import numpy as np
    HAS_PLOT = True
except ImportError:
    HAS_PLOT = False
    print("matplotlib/pandas not available; printing text summary only.")


def load_data(results_dir):
    baseline_path = os.path.join(results_dir, "baseline_no_hints.csv")
    hints_path = os.path.join(results_dir, "with_hints.csv")

    if not os.path.exists(baseline_path):
        print(f"ERROR: {baseline_path} not found")
        sys.exit(1)
    if not os.path.exists(hints_path):
        print(f"ERROR: {hints_path} not found")
        sys.exit(1)

    baseline = pd.read_csv(baseline_path)
    hints = pd.read_csv(hints_path)

    # Convert ns to microseconds
    baseline["latency_us"] = baseline["latency_ns"] / 1000.0
    hints["latency_us"] = hints["latency_ns"] / 1000.0

    return baseline, hints


def compute_percentiles(data, column="latency_us"):
    percentiles = [50, 90, 95, 99, 99.9]
    results = {}
    for p in percentiles:
        results[f"p{p}"] = np.percentile(data[column], p)
    results["mean"] = data[column].mean()
    results["max"] = data[column].max()
    results["count"] = len(data)
    return results


def print_comparison(baseline_stats, hints_stats):
    print()
    print("=" * 70)
    print("  iSchedular: Baseline vs Hint-Aware Scheduling")
    print("=" * 70)
    print()
    print(f"{'Metric':<20} {'Baseline (us)':<20} {'Hints (us)':<20} {'Improvement':<15}")
    print("-" * 70)

    for key in ["p50", "p90", "p95", "p99", "p99.9", "mean", "max"]:
        b = baseline_stats.get(key, 0)
        h = hints_stats.get(key, 0)
        if b > 0:
            improvement = ((b - h) / b) * 100
            print(f"{key:<20} {b:<20.2f} {h:<20.2f} {improvement:>+.1f}%")
        else:
            print(f"{key:<20} {b:<20.2f} {h:<20.2f} {'N/A':<15}")

    print("-" * 70)
    print(f"{'Requests':<20} {baseline_stats['count']:<20} {hints_stats['count']:<20}")
    print("=" * 70)
    print()


def plot_cdf(baseline, hints, output_dir):
    """Plot CDF of latencies for both modes."""
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))

    for data, label, color in [(baseline, "Baseline (no hints)", "#e74c3c"),
                                (hints, "Hint-Aware", "#2ecc71")]:
        sorted_lat = np.sort(data["latency_us"])
        cdf = np.arange(1, len(sorted_lat) + 1) / len(sorted_lat)
        ax.plot(sorted_lat, cdf, label=label, color=color, linewidth=2)

    ax.set_xlabel("Latency (microseconds)", fontsize=12)
    ax.set_ylabel("CDF", fontsize=12)
    ax.set_title("Server Thread Latency: Baseline vs Hint-Aware Scheduling",
                 fontsize=14)
    ax.legend(fontsize=12)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(0.0, 1.01)

    # Use log scale if the range is large
    lat_max = max(baseline["latency_us"].max(), hints["latency_us"].max())
    lat_min = min(baseline["latency_us"].min(), hints["latency_us"].min())
    if lat_max / max(lat_min, 1) > 100:
        ax.set_xscale("log")

    path = os.path.join(output_dir, "latency_cdf.png")
    fig.savefig(path, dpi=150, bbox_inches="tight")
    print(f"Saved CDF plot to {path}")
    plt.close(fig)


def plot_percentile_bars(baseline_stats, hints_stats, output_dir):
    """Bar chart comparing percentiles."""
    metrics = ["p50", "p90", "p95", "p99", "p99.9"]
    baseline_vals = [baseline_stats[m] for m in metrics]
    hints_vals = [hints_stats[m] for m in metrics]

    x = np.arange(len(metrics))
    width = 0.35

    fig, ax = plt.subplots(1, 1, figsize=(10, 6))
    bars1 = ax.bar(x - width/2, baseline_vals, width, label="Baseline (no hints)",
                   color="#e74c3c", alpha=0.8)
    bars2 = ax.bar(x + width/2, hints_vals, width, label="Hint-Aware",
                   color="#2ecc71", alpha=0.8)

    ax.set_xlabel("Percentile", fontsize=12)
    ax.set_ylabel("Latency (microseconds)", fontsize=12)
    ax.set_title("Tail Latency Comparison: Baseline vs Hint-Aware", fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(metrics)
    ax.legend(fontsize=12)
    ax.grid(True, alpha=0.3, axis="y")

    # Add value labels on bars
    for bar in bars1:
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                f"{bar.get_height():.0f}", ha="center", va="bottom", fontsize=9)
    for bar in bars2:
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                f"{bar.get_height():.0f}", ha="center", va="bottom", fontsize=9)

    path = os.path.join(output_dir, "percentile_comparison.png")
    fig.savefig(path, dpi=150, bbox_inches="tight")
    print(f"Saved percentile bar chart to {path}")
    plt.close(fig)


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_results.py <results_dir>")
        print("  e.g.: python3 plot_results.py /tmp/ischeduler_eval/")
        sys.exit(1)

    results_dir = sys.argv[1]
    baseline, hints = load_data(results_dir)

    baseline_stats = compute_percentiles(baseline)
    hints_stats = compute_percentiles(hints)

    print_comparison(baseline_stats, hints_stats)

    if HAS_PLOT:
        plot_cdf(baseline, hints, results_dir)
        plot_percentile_bars(baseline_stats, hints_stats, results_dir)
    else:
        print("Install matplotlib and pandas to generate graphs:")
        print("  pip install matplotlib pandas")


if __name__ == "__main__":
    main()
