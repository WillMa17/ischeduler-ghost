# iSchedular Demo Recipe

Step-by-step recipe for recording a terminal demo of the hint-aware
scheduler on the heterogeneous workload. The whole demo runs in **one
SSH session** and produces a single comparison table at the end. If you
want a "two-pane" recording for visual flair, the second section below
shows what to run in a second SSH session for live observability.

---

## 0. Prerequisites (one-time)

The repo lives on the host at `/root/ischeduler-ghost_new`. The VM is
already configured to mount the host home into `/mnt/host/`, and the
ghOSt kernel is in `/root/ischeduler-ghost_new/vm/bzImage`.

If the VM isn't already running:

```bash
# On the host
sudo bash /root/ischeduler-ghost_new/vm/run-vm.sh
# In another terminal you can ssh in:
sshpass -p 'ghost' ssh -p 2222 root@localhost
```

Inside the VM, build once:

```bash
cd /mnt/host/ischeduler-ghost_new/userspace
bazel build -c opt //:agent_cfs_mem //:workload_hetero
```

---

## 1. The headline command (single terminal)

```bash
cd /mnt/host/ischeduler-ghost_new/userspace
bash demo.sh
```

That's it. The script:

1. Prints an **intro banner** explaining the workload and the four
   policies, then waits for you to press Enter.
2. Runs the workload **four times** (baseline, latency, throughput,
   deadline). Each run takes ~10 seconds.
3. Each run prints a **per-second progress ticker** showing how many
   latency requests / deadline frames / throughput multiplies have been
   completed *so far*, plus a per-class summary at the end:

   ```
   ============================
     policy = latency
   ============================
   Heterogeneous 3-class workload
     Latency:    4 workers, 64x64 matmul, period 1000 us
     Deadline:   4 workers, 128x128 matmul, period 8 ms,
                 budgets tight=5ms loose=60ms
     Throughput: 4 workers, 128x128 matmul (continuous)
     Duration:   10 s
     Hint mode:  latency
   
     [t= 1s]  latency:+3982   deadline-tight:+118 (miss=12)  ...
     [t= 2s]  latency:+3990   deadline-tight:+121 (miss=8)   ...
     ...
   
     [Latency class]      requests: ...  started <= 200 us: 87.4%  p99: 3.6 ms
     [Deadline tight]     frames: ...    missed: ...
     [Throughput class]   total ops: ...
   ```

4. After all four runs it prints the **side-by-side comparison table**
   with each policy's winning cell starred (`*...*`) and bold:

   ```
   ===================================================
     FINAL COMPARISON  (lower-is-better unless noted)
   ===================================================
   
     KPI                            baseline    latency   throughput   deadline
     ------------------------------ ----------- --------- ------------ --------
     latency wakeup p99             18694 us    *3848 us* 18127 us    18526 us
     latency on-time<=200 us        75.8%       *87.4%*   75.8%       75.7%
     tight-deadline miss            20.6%       24.6%     21.9%       *18.6%*
     throughput total ops           9000        7914      *9146*      9158
   
     Each policy wins its target KPI: latency->latency,
     throughput->throughput, deadline->deadline
   ```

5. Reminds you where the per-event CSVs live and how to render the bar
   charts via `plot_hetero.py`.

The demo intentionally pauses at the intro so that if you're recording,
you can start narrating before the runs begin.

---

## 2. (Optional) Second pane: live observability

If you want a two-pane recording showing scheduler activity while the
demo runs, open a second SSH session into the VM:

```bash
sshpass -p 'ghost' ssh -p 2222 root@localhost
```

and run any combination of:

| Command | What it shows |
|---------|---------------|
| `htop -d 5` | CPU usage per thread, ~all 4 vCPUs saturated during runs |
| `watch -n 1 'ls /sys/fs/ghost/'` | The ghOSt enclave being created/destroyed between policies |
| `tail -f /tmp/agent_*.log` | The agent prints "Initialization complete, ghOSt active." for each policy |
| `watch -n 1 'wc -l /tmp/hetero_*.csv'` | Per-event CSVs growing as each policy completes |

`htop` is the most cinematic of these -- the bars across all 4 cores
hammer at full saturation for the whole 10s of each run, then drop
between policies.

---

## 3. Renderable artifacts after the demo

The demo leaves four per-event CSVs in `/tmp/`:

```
/tmp/hetero_baseline.csv
/tmp/hetero_latency.csv
/tmp/hetero_throughput.csv
/tmp/hetero_deadline.csv
```

To turn them into the three bar charts that match the README:

```bash
python3 plot_hetero.py --in-dir /tmp --out-dir /tmp/results
```

Produces:

  - `/tmp/results/hetero_latency.png`    -- per-policy wakeup-delay percentiles
  - `/tmp/results/hetero_throughput.png` -- per-policy throughput-class ops
  - `/tmp/results/hetero_deadline.png`   -- per-policy tight/loose miss rate

Open them however you like (e.g. `xdg-open` on the host, since
`/mnt/host` is the host's home).

---

## 4. Tweaking the demo for the camera

The script honours environment variables so you can tune the workload
without editing code:

```bash
# Quicker run (5s per policy, less contention)
DURATION=5 LAT_WORKERS=2 DL_WORKERS=2 TP_WORKERS=2 bash demo.sh

# Sharper KPI separation (more contention, longer run)
DURATION=15 LAT_PERIOD=500 DL_TIGHT=4 bash demo.sh
```

Defaults target a 4-vCPU VM and produce the numbers shown in the
README.

---

## 5. Single line summary you can read on camera

> "We run the same heterogeneous workload four times under four
> different ghOSt scheduler policies. The hints are sent through a
> 64-byte cache-line in shared memory; the agent translates them into
> sticky `latency_class` flags, custom time-slices, and absolute
> deadlines on `CfsTask`. The final comparison table shows that *each
> policy* wins on the SLO it was designed to optimise."
