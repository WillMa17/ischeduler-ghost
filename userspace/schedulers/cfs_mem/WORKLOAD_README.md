# workload_mixed — iSchedular Evaluation Workload

A benchmark that creates a realistic mixed environment to demonstrate that
application-provided scheduling hints improve tail latency for
latency-critical tasks without significantly hurting batch throughput.

## What it does

The workload runs two types of ghOSt-scheduled threads simultaneously:

**Server threads** (latency-critical):
- Sleep for a random interval (simulating waiting for a request to arrive)
- Wake up and do a short CPU burst (default 50 microseconds)
- Record the time from wakeup to completion as the request latency
- Send `HINT_LATENCY_SENSITIVE` via the shared memory channel

**Batch threads** (throughput-oriented):
- Run continuous CPU work (32x32 matrix multiplications)
- Count total work units completed
- Send `HINT_BATCH` via the shared memory channel

## Why this workload matters

CFS treats all threads equally. When batch threads saturate the CPUs and a
server thread wakes up, CFS won't immediately preempt the running batch
thread because it hasn't exhausted its time slice. The server thread waits,
causing tail latency spikes.

A hint-aware scheduler can read the hint byte and immediately boost
latency-sensitive tasks (by reducing their vruntime), preempting batch
work. Server threads get the CPU within microseconds; batch threads fill
the remaining time.

## Building

Inside the VM:

```bash
cd /root/ghost-userspace
bazel build -c opt //:workload_mixed
```

Requires the `.bazelrc` file with `build --cxxopt=-std=c++17`.

## Running

**Terminal 1** — start the agent:
```bash
sudo bazel-bin/agent_cfs_mem --ghost_cpus 0-3
```

**Terminal 2** — run the workload:
```bash
# Baseline (no hints sent)
bazel-bin/workload_mixed --send_hints=false --requests_per_server=100

# With hints (scheduler reads hints from shared memory)
bazel-bin/workload_mixed --send_hints=true --requests_per_server=100
```

## Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--server_threads` | 2 | Number of latency-critical server threads |
| `--batch_threads` | 4 | Number of background batch threads |
| `--requests_per_server` | 500 | Requests each server thread processes |
| `--server_work_us` | 50 | Microseconds of CPU work per request |
| `--server_sleep_us` | 200 | Average microseconds between requests |
| `--send_hints` | true | Whether to send scheduling hints |
| `--output_file` | /tmp/ischeduler_results.csv | Path for CSV output |

## Output

Prints tail latency percentiles (p50, p90, p95, p99, p999) and batch
throughput. Also saves per-request latency data to CSV for graphing.

## Evaluation scripts

- `run_experiment.sh` — Automates the A/B comparison (runs with and without hints)
- `plot_results.py` — Generates CDF and percentile bar charts from CSV output

## Initial baseline results (QEMU, no KVM, scheduler not yet acting on hints)

| Metric | No Hints | With Hints |
|--------|----------|------------|
| p99 | 446.84 us | 184.98 us |
| p999 | 10,315.15 us | 1,467.48 us |
| max | 10,385.00 us | 1,762.00 us |
| mean | 174.85 us | 94.75 us |
| Batch work | 96,678 units | 82,932 units |

Note: These numbers are from a software-emulated VM (no KVM) so absolute
values are not meaningful.

## Current results: hint-aware boost active (QEMU + KVM, 4 vCPUs)

After the scheduler was extended to act on `kHintLatencySensitive` cues
(see `cfs_mem/README.md` for the boost design), with a moderately
oversubscribed workload (2 server / 6 batch / 600 server requests /
sleep=50us / batch_duration=8s):

| Metric | No Hints | With Hints | Change |
|--------|----------|------------|--------|
| p50 | 52.66 us | 53.01 us | ~ |
| p90 | 52.74 us | 53.15 us | ~ |
| p95 | 52.78 us | 53.28 us | ~ |
| p99 | 52.96 us | 53.52 us | ~ |
| **p999** | **1,597.72 us** | **71.69 us** | **-95.5%** |
| **max** | **3,689.28 us** | **73.15 us** | **-98.0%** |
| mean | 58.96 us | 53.09 us | -10% |
| **Batch units** | **366,705** | **592,782** | **+62%** |

Interpretation:
  - p50-p99 don't move because the bulk of requests don't queue at all on
    a 4-CPU box with this load -- they hit the CPU on wakeup and run at the
    work-time floor (~52us).
  - The boost targets exactly the tail spikes (p999, max), where a request
    happens to wake when all 4 CPUs are mid-batch. Without boost, those
    requests wait for CFS's natural time slice (1-4ms); with boost they
    preempt within microseconds.
  - Batch throughput goes *up*, not down. Boosted server threads finish
    quickly and the batch threads then run uninterrupted, which is more
    cache-friendly than CFS's natural fine-grained alternation.
