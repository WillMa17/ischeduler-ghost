#include "schedulers/cfs_mem/cfs_mem_scheduler.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "absl/time/clock.h"
#include "lib/scheduler.h"

namespace ghost {

int MemCuePoll::Poll(const MemCueRegion* region, std::vector<MemCue>& out) {
  absl::MutexLock l(&mu);
  uint32_t n = region->next_slot.load(std::memory_order_acquire);
  if (n > static_cast<uint32_t>(kMemMaxSlots)) n = kMemMaxSlots;
  int found = 0;
  absl::Time now = absl::Now();
  for (uint32_t i = 0; i < n; ++i) {
    const MemCueSlot& slot = region->slots[i];
    uint32_t seq = slot.seq.load(std::memory_order_acquire);
    if (seq == last_seq[i]) continue;
    last_seq[i] = seq;
    // Reads after the acquire load of seq are ordered after the sender's
    // release store, so sent_ns and message are consistent.
    out.push_back({i, slot.sent_ns,
                   std::string(slot.message,
                               strnlen(slot.message, sizeof(slot.message))),
                   now});
    ++found;
  }
  return found;
}

FullMemCfsAgent::FullMemCfsAgent(CfsConfig config)
    : FullAgent<LocalEnclave>(config) {
  shm_unlink(kShmName);
  shm_fd_ = shm_open(kShmName, O_CREAT | O_RDWR, 0666);
  CHECK_GE(shm_fd_, 0);
  CHECK_EQ(ftruncate(shm_fd_, sizeof(MemCueRegion)), 0);

  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, shm_fd_, 0);
  CHECK_NE(ptr, MAP_FAILED);
  // Placement-new over the zero-initialized mmap'd pages to construct atomics.
  region_ = new (ptr) MemCueRegion();

  printf("ghost_mem: shm region ready at %s (%zu bytes, %d slots)\n",
         kShmName, sizeof(MemCueRegion), kMemMaxSlots);
  fflush(stdout);

  scheduler_ =
      MultiThreadedCfsScheduler(&this->enclave_, *this->enclave_.cpus(),
                                config.min_granularity_, config.latency_);
  this->StartAgentTasks();
  this->enclave_.Ready();
}

FullMemCfsAgent::~FullMemCfsAgent() {
  this->TerminateAgentTasks();
  if (region_) {
    region_->~MemCueRegion();
    munmap(region_, sizeof(MemCueRegion));
  }
  if (shm_fd_ >= 0) close(shm_fd_);
  shm_unlink(kShmName);
}

void MemCfsAgent::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished() || !scheduler_->Empty(cpu())) {
    std::vector<MemCue> cues;
    if (poll_->Poll(region_, cues) > 0) {
      // Hint-aware dispatch. message[0] selects the action; remaining
      // bytes are an optional HintPayload struct (slice_us / deadline_ns).
      //   kHintLatencySensitive -> immediate vruntime boost.
      //   kHintDeadline         -> record absolute deadline AND boost on
      //                            arrival (treat as latency for one frame).
      //   kHintThroughput       -> install a per-task custom slice so CFS
      //                            stops preempting the task on the default
      //                            period boundary.
      //   kHintBatch / kHintNone -> no-op.
      for (const auto& cue : cues) {
        if (cue.message.empty()) continue;
        char kind = cue.message[0];
        int64_t raw_gtid = region_->slot_gtid[cue.slot_idx];
        if (raw_gtid <= 0) continue;
        Gtid gtid(raw_gtid);

        HintPayload payload{};
        if (cue.message.size() >= 1 + sizeof(HintPayload)) {
          memcpy(&payload, cue.message.data() + 1, sizeof(HintPayload));
        }

        switch (kind) {
          case kHintLatencySensitive:
            // Sticky: every future enqueue of this task lands at the
            // front of the rq. We also boost the current state in case
            // the task is on rq right now.
            scheduler_->SetLatencyClass(gtid, true);
            scheduler_->BoostTask(gtid);
            break;
          case kHintDeadline: {
            // Deadline policy. Two effects per cue:
            //
            // 1. Track the absolute deadline so RescueDeadlineTasks can
            //    find this task on every Schedule() pass.
            //
            // 2. If the deadline is "tight" (already within the urgency
            //    window), also set latency_class=true so EVERY future
            //    EnqueueTask of this task front-places it. The sticky
            //    flag is what actually moves the deadline KPI:
            //
            //      - the cue arrives WHILE the worker is mid-frame, so
            //        both BoostTask and the per-event urgency check in
            //        EnqueueTask only catch the boost on a FUTURE wake;
            //      - by the time the next wake happens, the previous
            //        deadline_ns has already passed (= in the past), so
            //        the urgency check skips it and the wake gets
            //        default placement, defeating the purpose;
            //      - the sticky latency_class flag bypasses that timing
            //        issue: it persists across wakes and front-places
            //        every enqueue without depending on a fresh cue.
            //
            //    We restrict the sticky flag to TIGHT tasks (deadline
            //    closer than 30 ms) so loose-deadline workers in the
            //    same class don't get spuriously prioritised.
            scheduler_->SetDeadline(gtid, payload.deadline_unix_ns);
            int64_t now_ns = absl::ToUnixNanos(absl::Now());
            if (payload.deadline_unix_ns - now_ns <
                absl::ToInt64Nanoseconds(absl::Milliseconds(30))) {
              scheduler_->SetLatencyClass(gtid, true);
              scheduler_->BoostTask(gtid);
            }
            break;
          }
          case kHintThroughput:
            // Throughput policy. Two effects per cue:
            //
            // 1. Install a custom slice (typically tens of ms) so CFS's
            //    periodic preemption check does not yank this task off
            //    the CPU on the default ~1 ms boundary. This amortises
            //    context-switch overhead and lets the matmul keep its
            //    hot cache state across more iterations.
            //
            // 2. Set latency_class=true so the task is sticky
            //    front-of-queue. This matters when the throughput task
            //    DOES eventually get preempted (e.g. by CFS at the end
            //    of its slice, or by a TASK_WAKEUP). Without the sticky
            //    flag, the task would land back in default-CFS position
            //    and have to wait its turn behind every other thread on
            //    the rq before getting CPU again. The sticky flag pulls
            //    it back to the front so it resumes promptly. Net
            //    effect: throughput-class wall time per slice goes up
            //    (better cache reuse) AND idle-between-slice time goes
            //    down (back on CPU faster), so total ops increase.
            //
            //    Side effect: latency / deadline classes get worse
            //    KPIs in throughput mode. That is the intended trade-
            //    off -- throughput mode optimises throughput, not the
            //    other two SLOs.
            if (payload.slice_us > 0) {
              scheduler_->SetCustomSlice(
                  gtid, absl::Microseconds(payload.slice_us));
            }
            scheduler_->SetLatencyClass(gtid, true);
            break;
          default:
            break;  // kHintNone, kHintBatch, unknown -> no action
        }
      }
    }

    // Independent of cues: every Schedule() pass, rescue any deadline-
    // tagged task whose deadline is closer than 30 ms. This window
    // matches the urgency threshold used inside CfsRq::EnqueueTask so
    // a task that wasn't on-rq at boost time is still rescued before
    // its deadline expires.
    scheduler_->RescueDeadlineTasks(absl::Milliseconds(30));

    scheduler_->Schedule(cpu(), status_word());

    if (verbose() && debug_out.Edge()) {
      static const int flags =
          verbose() > 1 ? Scheduler::kDumpStateEmptyRQ : 0;
      if (scheduler_->debug_runqueue_) {
        scheduler_->debug_runqueue_ = false;
        scheduler_->DumpState(cpu(), Scheduler::kDumpAllTasks);
      } else {
        scheduler_->DumpState(cpu(), flags);
      }
    }
  }
}

}  // namespace ghost
