#!/bin/bash
# run_hetero_avg.sh -- run the heterogeneous workload N times under each
# of the four scheduler policies and concatenate the per-event CSVs so the
# downstream plotter sees averaged-over-iterations data.
#
# Single-run results have noticeable variance (cold-start scheduling
# decisions cascade into different steady states). Concatenating N
# iterations gives statistics that converge.
set -e

ITERS=${ITERS:-3}
DURATION=${DURATION:-15}
LAT_WORKERS=${LAT_WORKERS:-4}
LAT_DIM=${LAT_DIM:-64}
LAT_PERIOD=${LAT_PERIOD:-10000}
DL_WORKERS=${DL_WORKERS:-4}
DL_DIM=${DL_DIM:-128}
DL_PERIOD=${DL_PERIOD:-15}
DL_TIGHT=${DL_TIGHT:-6}
DL_LOOSE=${DL_LOOSE:-80}
TP_WORKERS=${TP_WORKERS:-4}
TP_DIM=${TP_DIM:-192}
TP_SLICE=${TP_SLICE:-20000}

cd "$(dirname "$0")"

cleanup() {
  pkill -9 -f agent_cfs_mem 2>/dev/null || true
  sleep 1
  for i in /sys/fs/ghost/enclave_*/ctl; do
    echo destroy > "$i" 2>/dev/null || true
  done
}

# Truncate the aggregated CSVs once.
for mode in baseline latency throughput deadline; do
  : > "/tmp/hetero_${mode}.csv"
done

run_one() {
  local mode="$1"
  cleanup
  bazel-bin/agent_cfs_mem --ghost_cpus 0-3 > "/tmp/agent_h_${mode}.log" 2>&1 &
  local apid=$!
  sleep 3
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
    --output_file="/tmp/hetero_${mode}_iter.csv" >/dev/null
  kill "$apid" 2>/dev/null || true
  wait "$apid" 2>/dev/null || true
  cleanup

  # Append iteration to aggregated CSV. Strip header from later iterations.
  if [ ! -s "/tmp/hetero_${mode}.csv" ]; then
    cp "/tmp/hetero_${mode}_iter.csv" "/tmp/hetero_${mode}.csv"
  else
    tail -n +2 "/tmp/hetero_${mode}_iter.csv" >> "/tmp/hetero_${mode}.csv"
  fi
}

for iter in $(seq 1 "$ITERS"); do
  echo ""
  echo "############### iteration $iter / $ITERS ###############"
  for mode in baseline latency throughput deadline; do
    echo "  --- iter $iter: hint_mode=$mode ---"
    run_one "$mode"
  done
done

echo ""
echo "Aggregated CSVs:"
wc -l /tmp/hetero_*.csv
