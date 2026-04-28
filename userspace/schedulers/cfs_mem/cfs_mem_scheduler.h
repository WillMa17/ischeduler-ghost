#ifndef GHOST_SCHEDULERS_CFS_MEM_CFS_MEM_SCHEDULER_H_
#define GHOST_SCHEDULERS_CFS_MEM_CFS_MEM_SCHEDULER_H_

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/enclave.h"
#include "schedulers/cfs/cfs_scheduler.h"

namespace ghost {

static constexpr int kMemMaxSlots = 256;
static constexpr char kShmName[] = "/ghost_mem_cues";

// Hint protocol: the first byte of MemCueSlot::message encodes the hint type.
// Senders write one of these constants; the agent reads message[0] and acts
// accordingly. Keep these stable -- both senders and the scheduler depend on
// the exact byte values.
//   kHintNone:             no hint, ignored by the scheduler.
//   kHintLatencySensitive: boost this thread; the scheduler will reduce its
//                          vruntime so it becomes the leftmost task in its
//                          per-CPU rq and preempt the current task.
//   kHintBatch:            no-op for the scheduler; batch is the default
//                          behavior, no demotion is needed.
static constexpr char kHintNone = 0;
static constexpr char kHintLatencySensitive = 1;
static constexpr char kHintBatch = 2;

// One cache-line per ghost thread. Written by the thread; polled by the agent.
// seq is incremented (release) after each write so the agent can detect a new
// cue with a single acquire load, without any locks.
struct alignas(64) MemCueSlot {
  std::atomic<uint32_t> seq{0};
  uint32_t _pad{0};
  int64_t sent_ns{0};
  char message[48]{};
};
static_assert(sizeof(MemCueSlot) == 64);

// Shared memory layout: 64-byte header, gtid table, then the slot array.
// mmap'd by both the agent (O_CREAT|O_RDWR) and senders (O_RDWR).
//
// `slot_gtid[i]` is the ghost thread Gtid (Gtid::Current().id()) of the
// thread that owns slot i. Senders write it once at AllocSlot time so the
// agent can resolve cues to CfsTask* without an extra syscall.
struct MemCueRegion {
  std::atomic<uint32_t> next_slot{0};  // slot allocator; senders fetch_add once
  char _hpad[60]{};                    // pad header to 64 bytes
  int64_t slot_gtid[kMemMaxSlots]{};   // populated at AllocSlot, read by agent
  MemCueSlot slots[kMemMaxSlots];
};

// A single cue snapshot captured by the polling loop.
struct MemCue {
  uint32_t slot_idx;
  int64_t sent_ns;
  std::string message;
  absl::Time observed_at;
};

// Shared polling state across all per-CPU agent threads. Mirrors IpcCueQueue:
// whichever CPU polls first claims the new cues; others skip them.
struct MemCuePoll {
  // Scans all allocated slots, appends newly-written cues to `out`.
  // Thread-safe: multiple CPUs may call concurrently; each cue is delivered
  // exactly once.
  int Poll(const MemCueRegion* region, std::vector<MemCue>& out);

  absl::Mutex mu;
  std::array<uint32_t, kMemMaxSlots> last_seq ABSL_GUARDED_BY(mu) = {};
};

// Per-CPU agent thread. Mirrors IpcCfsAgent but polls shared memory instead
// of draining a socket queue.
class MemCfsAgent : public LocalAgent {
 public:
  MemCfsAgent(Enclave* enclave, Cpu cpu, CfsScheduler* scheduler,
              MemCueRegion* region, MemCuePoll* poll)
      : LocalAgent(enclave, cpu),
        scheduler_(scheduler),
        region_(region),
        poll_(poll) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return scheduler_; }

 private:
  CfsScheduler* scheduler_;
  MemCueRegion* region_;
  MemCuePoll* poll_;
};

// Full agent: owns the CFS scheduler, the shared memory region, and MemCuePoll.
// No background threads — cues are read directly in Schedule().
class FullMemCfsAgent : public FullAgent<LocalEnclave> {
 public:
  explicit FullMemCfsAgent(CfsConfig config);
  ~FullMemCfsAgent() override;

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<MemCfsAgent>(&this->enclave_, cpu, scheduler_.get(),
                                         region_, &poll_);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case CfsScheduler::kDebugRunqueue:
        scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      case CfsScheduler::kCountAllTasks:
        response.response_code = scheduler_->CountAllTasks();
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<CfsScheduler> scheduler_;
  int shm_fd_{-1};
  MemCueRegion* region_{nullptr};
  MemCuePoll poll_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_CFS_MEM_CFS_MEM_SCHEDULER_H_
