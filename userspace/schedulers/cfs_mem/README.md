# cfs_mem -- ghOSt CFS Scheduler with Shared-Memory Cues

A ghOSt scheduler that lets user-space processes running under ghOSt write
hints directly into shared memory, which the scheduler reads on every
scheduling decision. The base policy is stock CFS; on top of that the agent
acts on `kHintLatencySensitive` cues by boosting the sender to the front of
its per-CPU run queue.

---

## How it works

### The shared memory region

When `agent_cfs_mem` starts it creates a POSIX shared memory object at
`/ghost_mem_cues` (visible under `/dev/shm/ghost_mem_cues`). Its layout is:

```
MemCueRegion
├── next_slot       (atomic uint32, 4 bytes + 60 bytes padding = 64 bytes)
├── slot_gtid[256]  (int64 ghost gtid per slot, written once at AllocSlot)
└── slots[256]      (one MemCueSlot per thread, 64 bytes each)
```

`slot_gtid[i]` lets the agent translate a cue back to its owning ghost
thread (i.e., a `CfsTask*`) with a single cache-line load, no syscall.

Each `MemCueSlot` is exactly one cache line:

```
MemCueSlot (64 bytes)
├── seq      atomic uint32  — incremented on every write; the agent's signal
├── _pad     uint32
├── sent_ns  int64          — nanoseconds since Unix epoch at write time
└── message  char[48]       — null-terminated cue string
```

### Thread registration (`AllocSlot`)

A ghost thread calls `AllocSlot` once at startup. It atomically increments
`next_slot`, receives back a permanent slot index, and publishes its
`Gtid::Current().id()` into `slot_gtid[index]` so the agent can later
identify it. No other thread will ever share that index. After this
one-time call the thread communicates with the scheduler purely through
its slot -- no system calls, no locks.

### Writing a cue (`WriteCue`)

```
WriteCue(region, slot_idx, "my hint")
  1. slot.sent_ns  = absl::ToUnixNanos(absl::Now())
  2. slot.message  = "my hint"   (memcpy, null-terminated)
  3. slot.seq     += 1           (atomic release store)
```

The release store on `seq` is the signal. It guarantees that any thread
which observes the new `seq` value (via an acquire load) will also see the
updated `sent_ns` and `message` — no additional synchronisation needed.

### How the agent reads cues (`MemCuePoll::Poll`)

On every `Schedule()` call, each per-CPU agent thread calls `Poll()`:

```
Poll()
  lock(mu)
  n = region->next_slot  (acquire load)
  for i in 0..n-1:
    seq = slots[i].seq   (acquire load)
    if seq == last_seq[i]: continue
    last_seq[i] = seq
    capture { slot_idx, sent_ns, message, observed_at=Now() }
  unlock(mu)
```

The mutex ensures that when multiple CPU agents run concurrently, each cue
is delivered to exactly one of them.

### Acting on cues (hint-aware boost)

After `Poll()` returns, the agent dispatches on the first byte of each cue:

```
for each cue:
  if cue.message[0] == kHintLatencySensitive:
    gtid = region->slot_gtid[cue.slot_idx]
    scheduler->BoostTask(Gtid(gtid))
```

`CfsScheduler::BoostTask` then:
  1. Resolves `Gtid` to a `CfsTask*` via the task allocator.
  2. Locates the task's owning CPU and acquires its `run_queue.mu_`.
  3. If the task is currently in the rq (`OnRq::kQueued`), calls
     `BoostTaskInRq`, which erases the task, sets its vruntime to
     `leftmost->vruntime - 1ns`, and re-inserts it. This makes the task
     the new leftmost (next to be picked).
  4. Sets `cs->preempt_curr = true`, so on the next `Schedule()` pass the
     currently running task is dropped and the boosted task takes the CPU.

Tasks not in the rq (running, blocked, migrating) are intentionally left
alone -- mutating them would risk breaking CFS's state-machine invariants.
They will pick up the boost the next time they enqueue at `min_vruntime_`.

---

## Building

Inside the VM, from `/root/ghost-userspace`:

```bash
bazel build //:agent_cfs_mem //:test_mem_sender
```

If the BUILD file or source files are out of date relative to the host:

```bash
cp /mnt/host/ghost/userspace/BUILD /root/ghost-userspace/BUILD
cp -r /mnt/host/ghost/userspace/schedulers/cfs_mem /root/ghost-userspace/schedulers/
bazel build //:agent_cfs_mem //:test_mem_sender
```

---

## Running

**Terminal 1** — start the agent:

```bash
cd /root/ghost-userspace
bazel-bin/agent_cfs_mem --ghost_cpus 0-3
```

**Terminal 2** — run the test sender (SSH in separately):

```bash
ssh -p 2222 root@localhost
cd /root/ghost-userspace
bazel-bin/test_mem_sender [--num_threads N] [--cues_per_thread N] [--cue_interval 100ms]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--num_threads` | 3 | Ghost threads to spawn |
| `--cues_per_thread` | 10 | Cues each thread sends |
| `--cue_interval` | 100ms | Delay between cues |

---

## Hint protocol

The first byte of `MemCueSlot::message` is the hint type. The canonical
constants live in `cfs_mem_scheduler.h`:

| Constant | Value | Effect |
|----------|-------|--------|
| `kHintNone` | 0 | Ignored. |
| `kHintLatencySensitive` | 1 | Agent calls `BoostTask` on the sender. |
| `kHintBatch` | 2 | No-op (batch is the default). |

The remaining 47 bytes are a free-form null-terminated string for human
debugging (the boost path doesn't read them).

## Adding new hint types

To extend the protocol:
  1. Add the new constant to `cfs_mem_scheduler.h`.
  2. Add a branch in `MemCfsAgent::AgentThread`'s dispatch loop.
  3. If the action needs new scheduler state, add a public method on
     `CfsScheduler` (mirror the `BoostTask` pattern: lock the per-CPU
     `run_queue.mu_`, mutate, set `preempt_curr` if needed).
