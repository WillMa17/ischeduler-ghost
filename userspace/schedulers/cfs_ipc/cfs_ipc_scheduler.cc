#include "schedulers/cfs_ipc/cfs_ipc_scheduler.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "absl/time/clock.h"
#include "lib/scheduler.h"

static constexpr char kSocketPath[] = "/tmp/ghost_ipc.sock";

namespace ghost {

void IpcCfsAgent::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished() || !scheduler_->Empty(cpu())) {
    // Drain any cues enqueued by the IPC listener since the last iteration.
    std::vector<IpcCue> cues;
    if (queue_->Drain(cues) > 0) {
      absl::Time now = absl::Now();
      for (const auto& cue : cues) {
        printf("Received message \"%s\" at %s, processed at %s (latency %s)\n",
               cue.message.c_str(),
               absl::FormatTime(cue.received_at).c_str(),
               absl::FormatTime(now).c_str(),
               absl::FormatDuration(now - cue.received_at).c_str());
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

void FullIpcCfsAgent::IpcListenerThread() {
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("ghost_ipc: socket");
    return;
  }

  unlink(kSocketPath);

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    perror("ghost_ipc: bind");
    close(server_fd);
    return;
  }

  if (listen(server_fd, /*backlog=*/8) < 0) {
    perror("ghost_ipc: listen");
    close(server_fd);
    return;
  }

  // 1-second accept timeout so we can check ipc_running_ and exit cleanly.
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  printf("ghost_ipc: listening on %s\n", kSocketPath);
  fflush(stdout);

  while (ipc_running_.load(std::memory_order_relaxed)) {
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) continue;  // timeout or signal — recheck ipc_running_

    // Read until newline or connection close.
    std::string msg;
    char c;
    while (read(client_fd, &c, 1) == 1 && c != '\n') msg += c;
    close(client_fd);

    if (msg.empty()) continue;

    // Enqueue the cue. It will be drained at the next natural Schedule()
    // iteration, which occurs within one scheduling round (~1 tick period).
    queue_.Push(msg);
  }

  close(server_fd);
  unlink(kSocketPath);
}

}  // namespace ghost
