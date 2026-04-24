// Test process for the IPC-CFS scheduler.
//
// Spawns ghost-scheduled threads that periodically send cues to a running
// agent_cfs_ipc instance via /tmp/ghost_ipc.sock. Each cue embeds the send
// timestamp so the agent's "received at / processed at" output can be compared
// against it to measure end-to-end socket delivery + queue-sit latency.
//
// Usage:
//   1. Start the agent:  bazel-bin/agent_cfs_ipc --ghost_cpus 0-3
//   2. Run this test:    bazel-bin/test_ghost_sender

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/ghost.h"
#include "lib/logging.h"

ABSL_FLAG(int, num_threads, 3, "Number of ghost threads to spawn");
ABSL_FLAG(int, cues_per_thread, 10, "Number of cues each thread sends");
ABSL_FLAG(absl::Duration, cue_interval, absl::Milliseconds(100),
          "Time between cues");

static constexpr char kSocketPath[] = "/tmp/ghost_ipc.sock";

// Sends a single newline-terminated cue. Returns the timestamp recorded
// immediately before the socket write, in nanoseconds since Unix epoch.
static int64_t SendCue(const std::string& msg) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("test: socket");
    return -1;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    perror("test: connect (is agent_cfs_ipc running?)");
    close(fd);
    return -1;
  }

  int64_t sent_ns = absl::ToUnixNanos(absl::Now());
  std::string wire = absl::StrFormat("%s sent_ns=%d\n", msg, sent_ns);
  write(fd, wire.c_str(), wire.size());
  close(fd);

  return sent_ns;
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  int num_threads = absl::GetFlag(FLAGS_num_threads);
  int cues_per_thread = absl::GetFlag(FLAGS_cues_per_thread);
  absl::Duration interval = absl::GetFlag(FLAGS_cue_interval);

  std::vector<std::unique_ptr<ghost::GhostThread>> threads;
  threads.reserve(num_threads);

  for (int t = 0; t < num_threads; t++) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [t, cues_per_thread, interval] {
          for (int i = 0; i < cues_per_thread; i++) {
            absl::SleepFor(interval);

            std::string msg =
                absl::StrFormat("cue thread=%d seq=%d pid=%d", t, i, getpid());
            int64_t sent_ns = SendCue(msg);

            if (sent_ns > 0) {
              printf("thread=%d seq=%d sent_ns=%d\n", t, i, sent_ns);
              fflush(stdout);
            }
          }
        }));
  }

  for (auto& thread : threads) {
    thread->Join();
  }

  printf("All threads done.\n");
  return 0;
}
