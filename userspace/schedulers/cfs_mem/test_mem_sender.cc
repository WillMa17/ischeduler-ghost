// Test process for the mem-CFS scheduler.
//
// Spawns ghost-scheduled threads that communicate with a running agent_cfs_mem
// instance via shared memory. Each thread allocates a slot once at startup,
// then writes cues directly into it — no kernel involvement after registration.
//
// Usage:
//   1. Start the agent:  bazel-bin/agent_cfs_mem --ghost_cpus 0-3
//   2. Run this test:    bazel-bin/test_mem_sender

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/ghost.h"
#include "lib/logging.h"
#include "schedulers/cfs_mem/cfs_mem_scheduler.h"

ABSL_FLAG(int, num_threads, 3, "Number of ghost threads to spawn");
ABSL_FLAG(int, cues_per_thread, 10, "Number of cues each thread sends");
ABSL_FLAG(absl::Duration, cue_interval, absl::Milliseconds(100),
          "Time between cues");

namespace ghost {

static MemCueRegion* OpenRegion() {
  int fd = shm_open(kShmName, O_RDWR, 0);
  if (fd < 0) {
    perror("test_mem: shm_open (is agent_cfs_mem running?)");
    return nullptr;
  }
  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    perror("test_mem: mmap");
    return nullptr;
  }
  return static_cast<MemCueRegion*>(ptr);
}

// Called once per thread: atomically grab the next free slot index.
static uint32_t AllocSlot(MemCueRegion* region) {
  uint32_t slot =
      region->next_slot.fetch_add(1, std::memory_order_relaxed);
  CHECK_LT(slot, static_cast<uint32_t>(kMemMaxSlots));
  region->slot_gtid[slot] = Gtid::Current().id();
  return slot;
}

// Write a cue into the thread's dedicated slot.
// The release store on seq ensures the agent observes sent_ns and message
// consistently after its acquire load of seq.
static int64_t WriteCue(MemCueRegion* region, uint32_t slot_idx,
                        const char* msg) {
  MemCueSlot& slot = region->slots[slot_idx];
  int64_t sent_ns = absl::ToUnixNanos(absl::Now());
  slot.sent_ns = sent_ns;
  size_t len = std::min(strlen(msg), sizeof(slot.message) - 1);
  memcpy(slot.message, msg, len);
  slot.message[len] = '\0';
  slot.seq.fetch_add(1, std::memory_order_release);
  return sent_ns;
}

}  // namespace ghost

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  ghost::MemCueRegion* region = ghost::OpenRegion();
  if (!region) return 1;

  int num_threads = absl::GetFlag(FLAGS_num_threads);
  int cues_per_thread = absl::GetFlag(FLAGS_cues_per_thread);
  absl::Duration interval = absl::GetFlag(FLAGS_cue_interval);

  std::vector<std::unique_ptr<ghost::GhostThread>> threads;
  threads.reserve(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [t, cues_per_thread, interval, region] {
          uint32_t slot = ghost::AllocSlot(region);
          printf("thread=%d allocated slot=%u\n", t, slot);
          fflush(stdout);

          for (int i = 0; i < cues_per_thread; ++i) {
            absl::SleepFor(interval);
            std::string msg = absl::StrFormat("thread=%d seq=%d", t, i);
            int64_t sent_ns = ghost::WriteCue(region, slot, msg.c_str());
            printf("thread=%d seq=%d slot=%u sent_ns=%ld\n", t, i, slot,
                   sent_ns);
            fflush(stdout);
          }
        }));
  }

  for (auto& thread : threads) thread->Join();

  munmap(region, sizeof(ghost::MemCueRegion));
  printf("All threads done.\n");
  return 0;
}
