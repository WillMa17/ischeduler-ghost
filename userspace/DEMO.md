# Demo recipe

Single command, runs inside the ghOSt VM. Total runtime ~1 minute.

## Prerequisites

VM must be running and reachable on port 2222 (see top-level README's
`vm/run-vm.sh`). The userspace binaries must already be built:

```bash
cd /path/to/userspace
bazel build -c opt //:agent_cfs_mem //:workload_hetero
```

## One-pane demo

```bash
bash demo.sh
```

`demo.sh`:

  1. Prints an intro banner and waits for Enter (skip the wait by piping
     stdin from `/dev/null`).
  2. Runs the heterogeneous workload four times -- once each under
     `--hint_mode = baseline | latency | throughput | deadline`. Each
     run takes 10 seconds and prints a per-second progress ticker
     showing the latency / deadline / throughput class counts.
  3. Prints a final comparison table with the winning policy starred on
     each KPI.

CSV output (per-event records, tagged by class) goes to
`/tmp/hetero_<policy>.csv`. Render the three bar charts with:

```bash
python3 plot_hetero_simple.py --in-dir /tmp --out-dir results
```

## Optional second pane

In a separate ssh session into the VM:

| Command | Shows |
|---|---|
| `htop -d 5` | All 4 vCPUs saturated during each run; brief drops between policies |
| `watch -n 0.5 'ls /sys/fs/ghost/'` | `enclave_N` directory appearing / disappearing as each policy starts / stops its agent |

Neither is required; they make the recording more visually obvious that
ghOSt enclaves are independent and CPUs are saturated.

## Tunables

`demo.sh` honours environment overrides for the workload parameters:

```bash
DURATION=15 LAT_WORKERS=4 LAT_DIM=64 LAT_PERIOD=1000 \
DL_WORKERS=4 DL_DIM=128 DL_PERIOD=8 DL_TIGHT=5 DL_LOOSE=60 \
TP_WORKERS=4 TP_DIM=128 TP_SLICE=20000 \
  bash demo.sh
```

Defaults target a 4-vCPU VM and produce the numbers in the README.

## Expected result shape

Each policy should win its own KPI:

  - **Latency policy**: lowest wakeup p99 / highest on-time rate
  - **Throughput policy**: highest total ops
  - **Deadline policy**: lowest tight-deadline miss rate

Trade-offs on the other KPIs are expected -- prioritising one class
necessarily costs the others CPU.
