#!/bin/bash
# run_hetero_experiment.sh
#
# Runs the heterogeneous 3-class workload (workload_hetero) four times,
# once per scheduler policy (--hint_mode={baseline,latency,throughput,
# deadline}). Each policy flags only ONE class of workers, leaving the
# other two classes untouched.
set -e

DURATION=${DURATION:-10}
LAT_WORKERS=${LAT_WORKERS:-4}
LAT_DIM=${LAT_DIM:-64}
LAT_PERIOD=${LAT_PERIOD:-1000}      # us
DL_WORKERS=${DL_WORKERS:-4}
DL_DIM=${DL_DIM:-128}
DL_PERIOD=${DL_PERIOD:-8}           # ms
DL_TIGHT=${DL_TIGHT:-5}
DL_LOOSE=${DL_LOOSE:-60}
TP_WORKERS=${TP_WORKERS:-4}
TP_DIM=${TP_DIM:-128}
TP_SLICE=${TP_SLICE:-20000}         # us

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
  bazel-bin/agent_cfs_mem --ghost_cpus 0-3 > "/tmp/agent_h_${mode}.log" 2>&1 &
  local apid=$!
  sleep 3
  echo ""
  echo "=================================================="
  echo "  hint_mode = ${mode}"
  echo "=================================================="
  bazel-bin/workload_hetero \
    --latency_workers="$LAT_WORKERS" \
    --latency_dim="$LAT_DIM" \
    --latency_period_us="$LAT_PERIOD" \
    --deadline_workers="$DL_WORKERS" \
    --deadline_dim="$DL_DIM" \
    --deadline_period_ms="$DL_PERIOD" \
    --deadline_tight_budget_ms="$DL_TIGHT" \
    --deadline_loose_budget_ms="$DL_LOOSE" \
    --throughput_workers="$TP_WORKERS" \
    --throughput_dim="$TP_DIM" \
    --throughput_slice_us="$TP_SLICE" \
    --duration_sec="$DURATION" \
    --hint_mode="$mode" \
    --output_file="/tmp/hetero_${mode}.csv"
  kill "$apid" 2>/dev/null || true
  wait "$apid" 2>/dev/null || true
  cleanup
}

for mode in baseline latency throughput deadline; do
  run_one "$mode"
done

echo ""
echo "=================================================="
echo "  All runs complete."
ls -l /tmp/hetero_*.csv
