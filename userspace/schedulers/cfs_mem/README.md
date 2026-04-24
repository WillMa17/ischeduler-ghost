# cfs_mem — ghOSt CFS Scheduler with Shared-Memory Cues

A ghOSt scheduler that lets user-space processes running under ghOSt write
hints directly into shared memory, which the scheduler reads on every
scheduling decision. Scheduling policy is identical to stock CFS; the cue
path is additive and does not currently alter scheduling — it is the
foundation for doing so.

---

## How it works

### The shared memory region

When `agent_cfs_mem` starts it creates a POSIX shared memory object at
`/ghost_mem_cues` (visible under `/dev/shm/ghost_mem_cues`). Its layout is:

```
MemCueRegion
├── next_slot  (atomic uint32, 4 bytes + 60 bytes padding = 64 bytes)
└── slots[256] (one MemCueSlot per thread, 64 bytes each)
```

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
`next_slot` and receives back a permanent slot index. No other thread will
ever share that index. After this one-time call the thread communicates with
the scheduler purely through its slot — no system calls, no locks.

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
is delivered to exactly one of them. Once captured, the agent prints:

```
Slot 0: "thread=0 seq=3" written at 2026-04-22T02:10:46.243988212+00:00, processed at 2026-04-22T02:10:46.244053073+00:00 (latency 64.861us)
```

The reported latency is purely **scheduling-call latency**: time from the
`WriteCue` call to the next `Schedule()` iteration that observes the change.
There is no socket delivery leg.

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

## Customising the cue message

The `message` field is a `char[48]` null-terminated string. You can put
anything there that fits. The natural next step is a structured format so
the scheduler can act on specific hint types.

### Defining hint types

In `cfs_mem_scheduler.h`, add an enum alongside the existing structs:

```cpp
enum class CueHint : uint8_t {
  kNone   = 0,
  kBoost  = 1,   // request higher scheduling priority
  kYield  = 2,   // voluntarily give up the CPU
  kIoWait = 3,   // about to block on I/O
};
```

Then encode the hint type into the first byte of `message` and keep a
human-readable description in the rest:

```cpp
// Sender side
static int64_t WriteCue(MemCueRegion* region, uint32_t slot_idx,
                        CueHint hint, const char* detail = "") {
  MemCueSlot& slot = region->slots[slot_idx];
  int64_t sent_ns = absl::ToUnixNanos(absl::Now());
  slot.sent_ns = sent_ns;
  slot.message[0] = static_cast<char>(hint);
  strncpy(slot.message + 1, detail, sizeof(slot.message) - 2);
  slot.message[sizeof(slot.message) - 1] = '\0';
  slot.seq.fetch_add(1, std::memory_order_release);
  return sent_ns;
}
```

### Acting on hints in the scheduler

In `MemCfsAgent::AgentThread()`, after `Poll()` returns cues, parse the
hint byte and act on it before calling `scheduler_->Schedule()`:

```cpp
for (const auto& cue : cues) {
  auto hint = static_cast<CueHint>(cue.message[0]);
  switch (hint) {
    case CueHint::kBoost: {
      // Look up the task by TID stored alongside the slot (see below),
      // then reduce its vruntime to move it toward the front of the
      // run queue.
      CfsTask* task = scheduler_->FindTaskByTid(slot_tid[cue.slot_idx]);
      if (task) task->vruntime -= absl::ToInt64Nanoseconds(absl::Milliseconds(5));
      break;
    }
    case CueHint::kYield:
      // Force a preemption on the next Schedule() pass.
      scheduler_->Preempt(cue.slot_idx);
      break;
    default:
      break;
  }
}
```

### Associating slots with tasks

Currently the agent does not know which `Gtid` owns which slot. The cleanest
way to add this: store the sender's TID into a parallel array at `AllocSlot`
time.

**Sender side** — pass the TID when allocating:

```cpp
uint32_t slot = AllocSlot(region, gettid());
```

**`MemCueRegion`** — add a TID table to the header:

```cpp
struct MemCueRegion {
  std::atomic<uint32_t> next_slot{0};
  char _hpad[60]{};
  pid_t slot_tid[kMemMaxSlots]{};   // written once by each sender at AllocSlot
  MemCueSlot slots[kMemMaxSlots];
};
```

**`AllocSlot`**:

```cpp
static uint32_t AllocSlot(MemCueRegion* region, pid_t tid) {
  uint32_t slot = region->next_slot.fetch_add(1, std::memory_order_relaxed);
  CHECK_LT(slot, static_cast<uint32_t>(kMemMaxSlots));
  region->slot_tid[slot] = tid;
  return slot;
}
```

The agent can then call `allocator()->GetTask(Gtid(region->slot_tid[i]))`
to retrieve the `CfsTask*` directly from the scheduler's internal state,
without parsing any strings.
