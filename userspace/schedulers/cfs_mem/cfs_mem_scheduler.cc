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
            scheduler_->BoostTask(gtid);
            break;
          case kHintDeadline:
            scheduler_->SetDeadline(gtid, payload.deadline_unix_ns);
            scheduler_->BoostTask(gtid);
            break;
          case kHintThroughput:
            if (payload.slice_us > 0) {
              scheduler_->SetCustomSlice(
                  gtid, absl::Microseconds(payload.slice_us));
            }
            break;
          default:
            break;  // kHintNone, kHintBatch, unknown -> no action
        }
      }
    }

    // Independent of cues: every Schedule() pass, rescue any deadline-
    // tagged task whose deadline is closer than 5 ms. This catches the
    // case where a deadline task was recorded earlier but did not get
    // CPU and the wall-clock has now drifted into the urgency window.
    scheduler_->RescueDeadlineTasks(absl::Milliseconds(5));

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
