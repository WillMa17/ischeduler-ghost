#!/bin/bash
# run_experiment.sh — Run the iSchedular A/B comparison experiment
#
# This script runs the mixed workload twice:
#   1. With hints DISABLED (baseline: scheduler ignores application intent)
#   2. With hints ENABLED (scheduler uses hints to prioritize server threads)
#
# Results are saved to CSV files and a summary is printed at the end.
#
# Prerequisites:
#   - agent_cfs_mem must NOT be running (this script starts/stops it)
#   - Build both: bazel build -c opt //:agent_cfs_mem //:workload_mixed
#
# Usage: bash run_experiment.sh [--server_threads N] [--batch_threads N]

set -e

# Defaults
SERVER_THREADS=2
BATCH_THREADS=4
REQUESTS=500
SERVER_WORK_US=50
SERVER_SLEEP_US=200
GHOST_CPUS="0-3"
RESULTS_DIR="/tmp/ischeduler_eval"

# Parse args
while [[ $# -gt 0 ]]; do
  case $1 in
    --server_threads) SERVER_THREADS="$2"; shift 2 ;;
    --batch_threads) BATCH_THREADS="$2"; shift 2 ;;
    --requests) REQUESTS="$2"; shift 2 ;;
    --ghost_cpus) GHOST_CPUS="$2"; shift 2 ;;
    *) echo "Unknown flag: $1"; exit 1 ;;
  esac
done

mkdir -p "$RESULTS_DIR"

echo "============================================================"
echo "  iSchedular Evaluation Experiment"
echo "============================================================"
echo "  Server threads:  $SERVER_THREADS"
echo "  Batch threads:   $BATCH_THREADS"
echo "  Requests/server: $REQUESTS"
echo "  Ghost CPUs:      $GHOST_CPUS"
echo "  Results dir:     $RESULTS_DIR"
echo "============================================================"
echo ""

# Helper: clean up any existing enclaves
cleanup_enclaves() {
  for e in /sys/fs/ghost/enclave_*/ctl/destroy; do
    echo 0 > "$e" 2>/dev/null || true
  done
  sleep 1
}

# ---------------------------------------------------------------------------
# Run 1: HINTS DISABLED (baseline)
# ---------------------------------------------------------------------------
echo ">>> RUN 1: Hints DISABLED (CFS baseline behavior)"
echo ""

cleanup_enclaves

# Start agent in background
bazel-bin/agent_cfs_mem --ghost_cpus "$GHOST_CPUS" &
AGENT_PID=$!
sleep 2  # Let agent initialize

# Run workload without hints
bazel-bin/workload_mixed \
  --server_threads "$SERVER_THREADS" \
  --batch_threads "$BATCH_THREADS" \
  --requests_per_server "$REQUESTS" \
  --server_work_us "$SERVER_WORK_US" \
  --server_sleep_us "$SERVER_SLEEP_US" \
  --send_hints=false \
  --output_file "$RESULTS_DIR/baseline_no_hints.csv"

# Stop agent
kill "$AGENT_PID" 2>/dev/null || true
wait "$AGENT_PID" 2>/dev/null || true
sleep 2

# ---------------------------------------------------------------------------
# Run 2: HINTS ENABLED
# ---------------------------------------------------------------------------
echo ""
echo ">>> RUN 2: Hints ENABLED (hint-aware scheduling)"
echo ""

cleanup_enclaves

# Start agent again
bazel-bin/agent_cfs_mem --ghost_cpus "$GHOST_CPUS" &
AGENT_PID=$!
sleep 2

# Run workload with hints
bazel-bin/workload_mixed \
  --server_threads "$SERVER_THREADS" \
  --batch_threads "$BATCH_THREADS" \
  --requests_per_server "$REQUESTS" \
  --server_work_us "$SERVER_WORK_US" \
  --server_sleep_us "$SERVER_SLEEP_US" \
  --send_hints=true \
  --output_file "$RESULTS_DIR/with_hints.csv"

# Stop agent
kill "$AGENT_PID" 2>/dev/null || true
wait "$AGENT_PID" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
echo "  Experiment Complete"
echo "============================================================"
echo ""
echo "Results saved to:"
echo "  $RESULTS_DIR/baseline_no_hints.csv"
echo "  $RESULTS_DIR/with_hints.csv"
echo ""
echo "To compare, look at the p99 and p999 latency numbers printed above."
echo "The hint-enabled run should show significantly lower tail latency"
echo "for server threads, with similar batch throughput."
echo ""
echo "To generate graphs, copy the CSVs and run:"
echo "  python3 plot_results.py $RESULTS_DIR/"
