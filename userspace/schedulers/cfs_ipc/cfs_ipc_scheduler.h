#ifndef GHOST_SCHEDULERS_CFS_IPC_CFS_IPC_SCHEDULER_H_
#define GHOST_SCHEDULERS_CFS_IPC_CFS_IPC_SCHEDULER_H_

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/enclave.h"
#include "schedulers/cfs/cfs_scheduler.h"

namespace ghost {

struct IpcCue {
  std::string message;
  absl::Time received_at;
};

// Thread-safe queue for IPC cues flowing from user processes into the
// scheduling loop. Push() is called by the IPC listener thread; Drain() is
// called by agent threads on each Schedule() iteration.
struct IpcCueQueue {
  void Push(std::string message) {
    absl::MutexLock l(&mu);
    cues.push_back({std::move(message), absl::Now()});
  }

  // Moves all pending cues into `out`. Returns the number drained.
  int Drain(std::vector<IpcCue>& out) {
    absl::MutexLock l(&mu);
    int n = static_cast<int>(cues.size());
    for (auto& c : cues) out.push_back(std::move(c));
    cues.clear();
    return n;
  }

  absl::Mutex mu;
  std::deque<IpcCue> cues ABSL_GUARDED_BY(mu);
};

// Per-CPU agent thread. Identical to CfsAgent except it drains pending IPC
// cues at the top of each scheduling iteration before delegating to the
// CfsScheduler.
class IpcCfsAgent : public LocalAgent {
 public:
  IpcCfsAgent(Enclave* enclave, Cpu cpu, CfsScheduler* scheduler,
              IpcCueQueue* queue)
      : LocalAgent(enclave, cpu), scheduler_(scheduler), queue_(queue) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return scheduler_; }

 private:
  CfsScheduler* scheduler_;
  IpcCueQueue* queue_;
};

// Full agent that owns the CFS scheduler, the per-CPU IpcCfsAgents, and a
// Unix-socket IPC listener thread. Scheduling policy is identical to
// FullCfsAgent; the IPC path is additive.
class FullIpcCfsAgent : public FullAgent<LocalEnclave> {
 public:
  explicit FullIpcCfsAgent(CfsConfig config) : FullAgent<LocalEnclave>(config) {
    scheduler_ =
        MultiThreadedCfsScheduler(&this->enclave_, *this->enclave_.cpus(),
                                  config.min_granularity_, config.latency_);
    this->StartAgentTasks();
    this->enclave_.Ready();

    ipc_running_.store(true, std::memory_order_relaxed);
    ipc_thread_ = std::thread(&FullIpcCfsAgent::IpcListenerThread, this);
  }

  ~FullIpcCfsAgent() override {
    this->TerminateAgentTasks();
    ipc_running_.store(false, std::memory_order_relaxed);
    ipc_thread_.join();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<IpcCfsAgent>(&this->enclave_, cpu, scheduler_.get(),
                                         &queue_);
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
  void IpcListenerThread();

  // Wakes all CPU agents so they process the queue promptly.
  void PingAllAgents() {
    for (const Cpu& cpu : *this->enclave_.cpus()) {
      Agent* agent = this->enclave_.GetAgent(cpu);
      if (agent) agent->Ping();
    }
  }

  std::unique_ptr<CfsScheduler> scheduler_;
  IpcCueQueue queue_;
  std::atomic<bool> ipc_running_{false};
  std::thread ipc_thread_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_CFS_IPC_CFS_IPC_SCHEDULER_H_
