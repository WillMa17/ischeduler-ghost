#!/bin/bash
# demo.sh -- single-command live demo of the iSchedular hint framework.
#
# Runs the heterogeneous 3-class workload four times under four scheduler
# policies (baseline / latency / throughput / deadline) and prints a final
# side-by-side KPI comparison table. Uses ANSI colors when stdout is a tty.
#
# Run inside the VM, with the bazel binaries already built:
#   cd /mnt/host/ischeduler-ghost_new/userspace
#   bazel build -c opt //:agent_cfs_mem //:workload_hetero
#   bash demo.sh
#
# Optional environment overrides (with reasonable defaults for a 4-vCPU VM):
#   DURATION=10 LAT_WORKERS=4 LAT_DIM=64 LAT_PERIOD=1000 \
#   DL_WORKERS=4 DL_DIM=128 DL_PERIOD=8 DL_TIGHT=5 DL_LOOSE=60 \
#   TP_WORKERS=4 TP_DIM=128 TP_SLICE=20000 \
#     bash demo.sh
# Note: deliberately NOT using `set -e`. We want to keep going across the
# four policies even if one run hits a transient error (e.g. an enclave
# left over from a previous experiment) -- the cleanup helper handles
# those between runs.

# -------- ANSI color helpers ------------------------------------------------
if [ -t 1 ]; then
  BOLD=$(printf '\033[1m'); DIM=$(printf '\033[2m')
  RED=$(printf '\033[31m'); GREEN=$(printf '\033[32m')
  YELLOW=$(printf '\033[33m'); BLUE=$(printf '\033[34m')
  MAGENTA=$(printf '\033[35m'); CYAN=$(printf '\033[36m')
  RESET=$(printf '\033[0m')
else
  BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; BLUE=""
  MAGENTA=""; CYAN=""; RESET=""
fi

banner() {
  local color="$1"; local text="$2"
  local len=${#text}
  printf "%s" "$color"
  printf '%*s\n' "$((len + 4))" '' | tr ' ' '='
  printf "  %s\n" "$text"
  printf '%*s\n' "$((len + 4))" '' | tr ' ' '='
  printf "%s" "$RESET"
}

# -------- Tunables ----------------------------------------------------------
DURATION=${DURATION:-10}
LAT_WORKERS=${LAT_WORKERS:-4}
LAT_DIM=${LAT_DIM:-64}
LAT_PERIOD=${LAT_PERIOD:-1000}
DL_WORKERS=${DL_WORKERS:-4}
DL_DIM=${DL_DIM:-128}
DL_PERIOD=${DL_PERIOD:-8}
DL_TIGHT=${DL_TIGHT:-5}
DL_LOOSE=${DL_LOOSE:-60}
TP_WORKERS=${TP_WORKERS:-4}
TP_DIM=${TP_DIM:-128}
TP_SLICE=${TP_SLICE:-20000}
LAT_TARGET=${LAT_TARGET:-200}

cd "$(dirname "$0")"

# -------- Cleanup helper ----------------------------------------------------
cleanup() {
  pkill -9 -f agent_cfs_mem 2>/dev/null || true
  sleep 1
  shopt -s nullglob
  for i in /sys/fs/ghost/enclave_*/ctl; do
    echo destroy > "$i" 2>/dev/null || true
  done
  shopt -u nullglob
}
trap cleanup EXIT

# -------- Intro -------------------------------------------------------------
clear || true
banner "$CYAN$BOLD" "iSchedular hint-aware ghOSt scheduling demo"
cat <<EOF

${BOLD}Workload${RESET}: 3 thread classes co-located on the same set of CPUs.

  ${RED}latency  class${RESET}: ${LAT_WORKERS} workers x ${LAT_DIM}x${LAT_DIM} matmul,
                   wakes every ${LAT_PERIOD}us  -> wakeup-to-start delay KPI
  ${BLUE}deadline class${RESET}: ${DL_WORKERS} workers x ${DL_DIM}x${DL_DIM} matmul, period ${DL_PERIOD}ms,
                   half tight (${DL_TIGHT}ms budget) / half loose (${DL_LOOSE}ms)
                   -> tight-deadline miss-rate KPI
  ${GREEN}throughput class${RESET}: ${TP_WORKERS} workers x ${TP_DIM}x${TP_DIM} matmul, no sleep
                     -> total-ops KPI

The ${BOLD}same workload${RESET} is run 4 times. Each run uses one of these
scheduler policies (selected by which cue the workload publishes):

  ${DIM}baseline${RESET}   : no cues  (stock CFS)
  ${RED}latency${RESET}    : flag latency-class workers as sticky front-of-queue
  ${GREEN}throughput${RESET} : install ${TP_SLICE}us custom slice on throughput-class workers
  ${BLUE}deadline${RESET}   : publish each tight-deadline frame's deadline_ns

Expectation: each policy improves ${BOLD}its own${RESET} KPI without
materially harming the other two.

Each run takes ${DURATION}s.  Live counters tick every second.

EOF
# Pause for the camera; skip if stdin isn't a terminal (so the script is
# still runnable from CI / pipes / nohup).
if [ -t 0 ]; then
  read -r -p "  Press Enter to start the demo..." _
fi

# -------- Run a single policy ----------------------------------------------
run_one() {
  local mode="$1"; local color="$2"
  cleanup
  bazel-bin/agent_cfs_mem --ghost_cpus 0-3 > "/tmp/agent_${mode}.log" 2>&1 &
  local apid=$!
  sleep 3
  echo
  banner "$color$BOLD" "policy = ${mode}"
  bazel-bin/workload_hetero \
    --latency_workers="$LAT_WORKERS" \
    --latency_dim="$LAT_DIM" \
    --latency_period_us="$LAT_PERIOD" \
    --latency_target_us="$LAT_TARGET" \
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
}

run_one baseline   "$DIM"
run_one latency    "$RED"
run_one throughput "$GREEN"
run_one deadline   "$BLUE"

# -------- Final comparison --------------------------------------------------
echo
banner "$MAGENTA$BOLD" "FINAL COMPARISON  (lower-is-better unless noted)"
echo

python3 - <<PY
import csv
from collections import defaultdict

# (label, color)
POLICIES = [("baseline",   "${DIM}"),
            ("latency",    "${RED}"),
            ("throughput", "${GREEN}"),
            ("deadline",   "${BLUE}")]
RESET = "${RESET}"
BOLD  = "${BOLD}"

def load(p):
    by = defaultdict(list)
    try:
        for r in csv.DictReader(open(p)):
            by[r["class"]].append(r)
    except FileNotFoundError:
        pass
    return by

def percentile(vals, p):
    import math
    if not vals: return 0.0
    s = sorted(vals)
    idx = p/100.0 * (len(s)-1)
    lo = int(math.floor(idx)); hi = int(math.ceil(idx))
    if lo == hi: return s[lo]
    f = idx - lo
    return s[lo]*(1-f) + s[hi]*f

results = {}
for pol, _color in POLICIES:
    rows = load(f"/tmp/hetero_{pol}.csv")
    delays = [int(r["wakeup_delay_ns"])/1000 for r in rows.get("latency", [])]
    tight  = rows.get("deadline_tight", [])
    loose  = rows.get("deadline_loose", [])
    tp     = rows.get("throughput", [])
    miss_t = sum(1 for r in tight if int(r["met"]) == 0)
    on_time = sum(1 for d in delays if d <= ${LAT_TARGET})
    results[pol] = {
        "lat_p99":    percentile(delays, 99),
        "lat_on":     100.0 * on_time / len(delays) if delays else 0,
        "tight_miss": 100.0 * miss_t / len(tight) if tight else 0,
        "tp_total":   len(tp),
    }

# Find winners.
def best(metric, lower_is_better):
    vals = [(pol, results[pol][metric]) for pol, _ in POLICIES]
    if lower_is_better:
        return min(vals, key=lambda x: x[1])[0]
    return max(vals, key=lambda x: x[1])[0]

winners = {
    "lat_p99":    best("lat_p99",    True),
    "lat_on":     best("lat_on",     False),
    "tight_miss": best("tight_miss", True),
    "tp_total":   best("tp_total",   False),
}

print(f"  {'KPI':30s}  ", end="")
for pol, color in POLICIES:
    print(f"{color}{BOLD}{pol:>11}{RESET}  ", end="")
print()
print(f"  {'-'*30}  " + ("-"*13 + "  ") * 4)

def row(label, key, fmt, suffix=""):
    print(f"  {label:30s}  ", end="")
    for pol, color in POLICIES:
        v = results[pol][key]
        s = (fmt.format(v)) + suffix
        if pol == winners[key]:
            s = f"{BOLD}*{s}*{RESET}"
        else:
            s = f" {s} "
        print(f"{s:>13}  ", end="")
    print()

row("latency wakeup p99",  "lat_p99",    "{:.0f}us")
row("latency on-time<=200us", "lat_on",  "{:.1f}%")
row("tight-deadline miss",  "tight_miss", "{:.1f}%")
row("throughput total ops", "tp_total",   "{:d}")

print()
print(f"  ${BOLD}Winning policy per KPI${RESET}: ", end="")
print(f"wakeup-p99 -> {winners['lat_p99']}, ", end="")
print(f"on-time -> {winners['lat_on']}, ", end="")
print(f"tight-miss -> {winners['tight_miss']}, ", end="")
print(f"ops -> {winners['tp_total']}")
print()
PY

echo "  ${DIM}Per-event CSVs are in /tmp/hetero_<policy>.csv${RESET}"
echo "  ${DIM}Run plot_hetero.py against them to render the bar charts.${RESET}"
echo
