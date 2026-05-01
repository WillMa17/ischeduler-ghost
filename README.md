# iSchedular

Hint-aware extension of the ghOSt userspace CFS scheduler. Applications
publish per-thread SLO intent through a 64-byte cache-line in shared
memory (`/dev/shm/ghost_mem_cues`); the userspace scheduler agent reads
the cues every `Schedule()` pass and adjusts `vruntime`, custom slice,
and deadline tracking on the corresponding `CfsTask`.

## Repository layout

```
.
├── README.md                  This file.
├── userspace/                 Userspace agent + workloads + scripts.
│   ├── DEMO.md                How to record the demo (one-pane / two-pane).
│   ├── demo.sh                Single command: runs all 4 policies + prints
│   │                          the comparison table.
│   ├── plot_hetero_simple.py  Renders the 3 KPI bar charts from CSV output.
│   ├── run_hetero_experiment.sh
│   │                          Same as demo.sh but no banners, for scripting.
│   ├── BUILD                  Bazel build targets.
│   └── schedulers/
│       ├── cfs/cfs_scheduler.{h,cc}
│       │                      Stock ghOSt CFS, extended with:
│       │                        - CfsTask::latency_class (sticky front-of-rq)
│       │                        - CfsTask::custom_slice (per-task slice)
│       │                        - CfsTask::deadline_ns
│       │                        - SetLatencyClass / SetCustomSlice /
│       │                          SetDeadline / RescueDeadlineTasks APIs
│       │                        - EnqueueTask front-places latency_class
│       │                          tasks and tasks within the deadline
│       │                          urgency window.
│       └── cfs_mem/
│           ├── cfs_mem_scheduler.{h,cc}
│           │                  Hint protocol (cue layout + kHint* constants),
│           │                  shared-memory region, agent dispatch loop.
│           ├── cfs_mem_agent.cc
│           │                  agent_cfs_mem main(): builds the agent process.
│           ├── workload_hetero.cc
│           │                  3-class benchmark used by the demo. --hint_mode
│           │                  selects which class publishes a cue.
│           └── README.md      Lower-level notes on the cue protocol.
└── vm/
    ├── bzImage                Pre-built ghOSt kernel (Linux 5.11 + ghost
    │                          scheduling class).
    ├── setup-vm.sh            One-time Debian-bookworm rootfs setup.
    └── run-vm.sh              QEMU + KVM launcher (4 vCPU, 4 GB RAM,
                               port 2222 -> 22 forwarded for ssh).
```

## Building

Inside the VM (the ghost-userspace library only links against the ghOSt
kernel, so build inside the VM running that kernel):

```bash
cd /path/to/userspace
bazel build -c opt //:agent_cfs_mem //:workload_hetero
```

## Running the demo

Inside the VM:

```bash
cd /path/to/userspace
bash demo.sh
```

`demo.sh` runs the heterogeneous workload four times, once per
scheduler policy (baseline / latency / throughput / deadline), prints
per-second progress, and ends with a comparison table that stars the
winning policy on each KPI. Per-event CSVs land in `/tmp/hetero_*.csv`;
render the bar charts with:

```bash
python3 plot_hetero_simple.py --in-dir /tmp --out-dir results
```

See `userspace/DEMO.md` for the recording recipe (single-pane and
two-pane setups, htop / `watch ls /sys/fs/ghost/` overlays).

## Reproducing the kernel-level overhead experiment

To measure how much the hint-dispatch path costs when no cues are sent,
re-run baseline twice with the env switch built into the agent:

```bash
# With Poll() + RescueDeadlineTasks (current default)
bazel-bin/agent_cfs_mem --ghost_cpus 0-3 &
bazel-bin/workload_hetero --hint_mode=baseline --output_file=/tmp/with_dispatch.csv

# Without (loop equivalent to a stock CFS userspace agent)
GHOST_DISABLE_HINT_DISPATCH=1 bazel-bin/agent_cfs_mem --ghost_cpus 0-3 &
bazel-bin/workload_hetero --hint_mode=baseline --output_file=/tmp/no_dispatch.csv
```

In our 3-iteration measurement the always-on hint dispatch path costs
< 1 % on most KPIs (~7.6 % on latency p999 only).

## Branches

  - `main`         original ghost-userspace snapshot
  - `28april`      mid-development snapshots
  - `final`        this submission
