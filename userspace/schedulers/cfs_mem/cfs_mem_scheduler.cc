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
      for (const auto& cue : cues) {
        absl::Time sent_at = absl::FromUnixNanos(cue.sent_ns);
        absl::Duration latency = cue.observed_at - sent_at;
        printf("Slot %u: \"%s\" written at %s, processed at %s (latency %s)\n",
               cue.slot_idx, cue.message.c_str(),
               absl::FormatTime(sent_at).c_str(),
               absl::FormatTime(cue.observed_at).c_str(),
               absl::FormatDuration(latency).c_str());
      }
      fflush(stdout);
    }

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
