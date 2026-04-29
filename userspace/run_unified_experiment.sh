#!/bin/bash
# run_unified_experiment.sh
#
# Runs the same uniform workload four times under four different scheduler
# policies (selected by --hint_mode):
#   none        -> Phase A baseline (no hints)
#   latency     -> immediate boost on every wake
#   throughput  -> long custom slice, no preemption on default boundary
#   deadline    -> deadline tracking + boost on cue + urgency rescue
#
# After each run we restart agent_cfs_mem to clear any leftover state
# (custom_slice / deadline_ns) on tasks from the previous run.
#
# Output:
#   /tmp/unified_<mode>.csv            per-event records
#   /tmp/agent_<mode>.log              agent log
set -e

DURATION=${DURATION:-10}
WORKERS=${WORKERS:-6}
DIM=${DIM:-256}
PERIOD=${PERIOD:-40}
BUDGET=${BUDGET:-35}
SLICE=${SLICE:-20000}
TARGET=${TARGET:-50}

cd "$(dirname "$0")"

cleanup() {
  pkill -9 -f agent_cfs_mem 2>/dev/null || true
  sleep 1
  for i in /sys/fs/ghost/enclave_*/ctl; do
    echo destroy > "$i" 2>/dev/null || true
  done
}

run_one() {
  local mode="$1"
  cleanup
  bazel-bin/agent_cfs_mem --ghost_cpus 0-3 > "/tmp/agent_${mode}.log" 2>&1 &
  local apid=$!
  sleep 3
  echo ""
  echo "=================================================="
  echo "  hint_mode = ${mode}"
  echo "=================================================="
  bazel-bin/workload_unified \
    --workers="$WORKERS" \
    --dim="$DIM" \
    --period_ms="$PERIOD" \
    --budget_ms="$BUDGET" \
    --throughput_slice_us="$SLICE" \
    --latency_target_us="$TARGET" \
    --duration_sec="$DURATION" \
    --hint_mode="$mode" \
    --output_file="/tmp/unified_${mode}.csv"
  kill "$apid" 2>/dev/null || true
  wait "$apid" 2>/dev/null || true
  cleanup
}

for mode in none latency throughput deadline; do
  run_one "$mode"
done

echo ""
echo "=================================================="
echo "  All runs complete. CSVs:"
ls -l /tmp/unified_*.csv
