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
values are not meaningful. The relative comparison is what matters, and
the real improvement will come after the scheduler is modified to act on
the hint byte.
