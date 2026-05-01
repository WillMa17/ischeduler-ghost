# cfs_mem -- CFS scheduler with shared-memory hint channel

The agent (`agent_cfs_mem`) extends the ghost-userspace CFS scheduler
with a hint-aware dispatch path. Threads scheduled by this agent can
publish per-task SLO intent through a shared memory region; the agent
reads cues every `Schedule()` pass and adjusts per-task scheduler
state.

## Shared memory layout

`/dev/shm/ghost_mem_cues` is created by the agent on startup and
mapped read/write by both the agent and any ghost-class sender.

```
MemCueRegion
  next_slot       atomic uint32         slot allocator
  slot_gtid[256]  int64                 ghost gtid of the thread that owns slot i
  slots[256]      MemCueSlot (64 B)     per-thread cue slot

MemCueSlot (64 B, cache-line aligned)
  seq      atomic uint32   release-store on every WriteCue
  _pad     uint32
  sent_ns  int64           agent uses this to measure cue propagation latency
  message  char[48]        message[0] = kind; message[1..16] = HintPayload
```

A ghost thread calls `AllocSlot()` once to get a permanent slot index
and to publish its `Gtid::Current().id()` into `slot_gtid[i]`. From
then on it just writes to its slot and bumps `seq` (release).
The agent does an acquire load on `seq` to detect a new cue.

## Hint kinds

Defined in `cfs_mem_scheduler.h`:

| kind | value | payload | scheduler effect |
|------|-------|---------|------------------|
| `kHintNone` | 0 | -- | ignored |
| `kHintLatencySensitive` | 1 | -- | sticky `latency_class=true` + `BoostTask` |
| `kHintBatch` | 2 | -- | no-op (batch is the default) |
| `kHintThroughput` | 3 | `slice_us` | `SetCustomSlice(slice)` + sticky `latency_class` |
| `kHintDeadline` | 4 | `deadline_unix_ns` | `SetDeadline` + (if urgent) sticky `latency_class` + `BoostTask` |

`HintPayload` (16 B, packed):

```c
struct HintPayload {
  uint32_t slice_us;         // for kHintThroughput
  uint32_t reserved;
  int64_t  deadline_unix_ns; // for kHintDeadline
};
```

## Per-task scheduler state added on top of CFS

`CfsTask` (in `userspace/schedulers/cfs/cfs_scheduler.h`) gains:

  - `latency_class` (bool) -- when set, every `EnqueueTask` of this task
    front-places it (`vruntime = leftmost - 1ns`).
  - `custom_slice` (Duration) -- when non-zero, `MinPreemptionGranularity`
    returns this for the task instead of the default CFS computation,
    capped at `4 * latency_`.
  - `deadline_ns` (int64) -- absolute Unix-ns deadline. `EnqueueTask`
    front-places tasks with `0 < deadline_ns - now < 30 ms` (the urgency
    window) and `RescueDeadlineTasks` periodically scans and boosts them.

## Files

  - `cfs_mem_scheduler.{h,cc}` -- shared-memory region, `MemCuePoll`, agent
    dispatch loop, hint -> scheduler-API mapping
  - `cfs_mem_agent.cc` -- `main()` that builds the agent process
  - `workload_hetero.cc` -- demo workload (3-class heterogeneous)
  - `workload_{mixed,3slo,unified}.cc` -- earlier exploratory workloads
  - `test_mem_sender.cc` -- tiny tool that writes raw cues, useful for
    validating the cue protocol without a full workload

## Overhead-experiment switch

If `GHOST_DISABLE_HINT_DISPATCH=1` is set in the agent's environment,
the agent loop skips both `Poll()` and `RescueDeadlineTasks()`, leaving
it equivalent to a plain CFS userspace agent. Used to measure the
always-on cost of the hint dispatch path.
