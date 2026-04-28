// workload_mixed.cc — iSchedular evaluation workload
//
// Simulates a mixed environment with latency-critical "server" threads and
// background "batch" threads. Demonstrates that when applications send hints
// via shared memory, a hint-aware scheduler can significantly reduce tail
// latency for server threads without materially hurting batch throughput.
//
// Workload structure:
//   - N "server" threads: sleep, wake on simulated request, do short CPU burst,
//     record wakeup-to-completion latency. Write HINT_LATENCY_SENSITIVE.
//   - M "batch" threads: continuous CPU work (matrix multiply), write
//     HINT_BATCH. Measure total work units completed.
//
// The key insight: CFS treats all threads equally, so a waking server thread
// must wait for the batch thread's time slice to expire. A hint-aware
// scheduler can preempt batch threads immediately when a server thread wakes.
//
// Usage:
//   1. Start agent:  bazel-bin/agent_cfs_mem --ghost_cpus 0-3
//   2. Run workload: bazel-bin/workload_mixed [flags]
//   3. Results are printed to stdout and saved to /tmp/ischeduler_results.csv
//
// Build: add to BUILD file (see README)

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/ghost.h"
#include "lib/logging.h"
#include "schedulers/cfs_mem/cfs_mem_scheduler.h"

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
ABSL_FLAG(int, server_threads, 2,
          "Number of latency-critical server threads");
ABSL_FLAG(int, batch_threads, 4,
          "Number of background batch threads");
ABSL_FLAG(int, requests_per_server, 500,
          "Number of requests each server thread processes");
ABSL_FLAG(int, server_work_us, 50,
          "Microseconds of CPU work per server request");
ABSL_FLAG(int, server_sleep_us, 200,
          "Average microseconds between server requests (exponential dist)");
ABSL_FLAG(int, duration_sec, 10,
          "Duration in seconds for batch threads to run");
ABSL_FLAG(bool, send_hints, true,
          "Whether to send scheduling hints via shared memory");
ABSL_FLAG(std::string, output_file, "/tmp/ischeduler_results.csv",
          "Path to write latency results as CSV");

namespace ghost {

// ---------------------------------------------------------------------------
// Hint protocol -- first byte of the 48-byte message field
// ---------------------------------------------------------------------------
// Use the canonical constants from cfs_mem_scheduler.h so senders and the
// scheduler can never disagree on the byte values.
static constexpr char HINT_NONE = kHintNone;
static constexpr char HINT_LATENCY_SENSITIVE = kHintLatencySensitive;
static constexpr char HINT_BATCH = kHintBatch;

// ---------------------------------------------------------------------------
// Shared memory helpers (same as test_mem_sender.cc)
// ---------------------------------------------------------------------------
static MemCueRegion* OpenRegion() {
  int fd = shm_open(kShmName, O_RDWR, 0);
  if (fd < 0) {
    fprintf(stderr, "workload: shm_open failed (is agent_cfs_mem running?)\n");
    return nullptr;
  }
  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    perror("workload: mmap");
    return nullptr;
  }
  return static_cast<MemCueRegion*>(ptr);
}

static uint32_t AllocSlot(MemCueRegion* region) {
  uint32_t slot =
      region->next_slot.fetch_add(1, std::memory_order_relaxed);
  CHECK_LT(slot, static_cast<uint32_t>(kMemMaxSlots));
  // Publish our ghost gtid so the agent can resolve cues to a CfsTask*
  // without an extra TID->Gtid syscall on the hot path.
  region->slot_gtid[slot] = Gtid::Current().id();
  return slot;
}

static void WriteCue(MemCueRegion* region, uint32_t slot_idx,
                     char hint_type, const char* detail) {
  MemCueSlot& slot = region->slots[slot_idx];
  slot.sent_ns = absl::ToUnixNanos(absl::Now());
  slot.message[0] = hint_type;
  size_t dlen = std::min(strlen(detail), sizeof(slot.message) - 2);
  memcpy(slot.message + 1, detail, dlen);
  slot.message[1 + dlen] = '\0';
  slot.seq.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// CPU busy-work functions
// ---------------------------------------------------------------------------

// Spin for approximately `us` microseconds doing real CPU work.
// Uses volatile to prevent compiler from optimizing away.
static void BurnCpuMicroseconds(int us) {
  auto start = std::chrono::steady_clock::now();
  volatile double x = 1.0;
  while (true) {
    // Do some floating-point work so the CPU is genuinely busy
    for (int i = 0; i < 100; ++i) {
      x = std::sin(x) * std::cos(x) + 0.001;
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
        >= us) {
      break;
    }
  }
}

// One unit of batch work: a small matrix multiply.
// Returns an arbitrary checksum to prevent optimization.
static double BatchWorkUnit() {
  constexpr int N = 32;
  static thread_local double A[N][N], B[N][N], C[N][N];

  // Initialize on first call
  static thread_local bool initialized = false;
  if (!initialized) {
    for (int i = 0; i < N; ++i)
      for (int j = 0; j < N; ++j) {
        A[i][j] = 0.01 * (i + j);
        B[i][j] = 0.01 * (i - j + 1);
        C[i][j] = 0.0;
      }
    initialized = true;
  }

  // C = A * B
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) {
      double sum = 0.0;
      for (int k = 0; k < N; ++k)
        sum += A[i][k] * B[k][j];
      C[i][j] = sum;
    }
  return C[0][0];
}

// ---------------------------------------------------------------------------
// Results collection (thread-safe)
// ---------------------------------------------------------------------------
struct LatencyResult {
  int thread_id;
  int request_id;
  int64_t wakeup_to_done_ns;  // end-to-end latency for this request
};

static std::mutex g_results_mu;
static std::vector<LatencyResult> g_results;

static std::atomic<int64_t> g_batch_work_units{0};  // total across all batch threads
static std::atomic<bool> g_stop_batch{false};

// ---------------------------------------------------------------------------
// Server thread function
// ---------------------------------------------------------------------------
static void ServerThread(int id, MemCueRegion* region, bool send_hints,
                         int requests, int work_us, int sleep_us_mean) {
  uint32_t slot = 0;
  if (region && send_hints) {
    slot = AllocSlot(region);
  }

  std::mt19937 rng(42 + id);
  std::exponential_distribution<double> sleep_dist(
      1.0 / static_cast<double>(sleep_us_mean));

  std::vector<LatencyResult> local_results;
  local_results.reserve(requests);

  for (int r = 0; r < requests; ++r) {
    // Simulate waiting for a request to arrive
    int sleep_us = static_cast<int>(sleep_dist(rng));
    sleep_us = std::max(sleep_us, 10);  // minimum 10us sleep
    absl::SleepFor(absl::Microseconds(sleep_us));

    // Request arrived! Record wakeup time.
    auto wakeup = std::chrono::steady_clock::now();

    // Send hint: "I'm latency-sensitive, schedule me now!"
    if (region && send_hints) {
      std::string detail = absl::StrFormat("srv=%d req=%d", id, r);
      WriteCue(region, slot, HINT_LATENCY_SENSITIVE, detail.c_str());
    }

    // Do the actual work for this request
    BurnCpuMicroseconds(work_us);

    // Record completion time
    auto done = std::chrono::steady_clock::now();
    int64_t latency_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(done - wakeup)
            .count();

    local_results.push_back({id, r, latency_ns});
  }

  // Merge results
  std::lock_guard<std::mutex> lock(g_results_mu);
  g_results.insert(g_results.end(), local_results.begin(),
                   local_results.end());
}

// ---------------------------------------------------------------------------
// Batch thread function
// ---------------------------------------------------------------------------
static void BatchThread(int id, MemCueRegion* region, bool send_hints) {
  uint32_t slot = 0;
  if (region && send_hints) {
    slot = AllocSlot(region);
    // Send initial hint: "I'm a batch job, lower priority is fine"
    WriteCue(region, slot, HINT_BATCH,
             absl::StrFormat("batch=%d", id).c_str());
  }

  int64_t units = 0;
  int cue_counter = 0;

  while (!g_stop_batch.load(std::memory_order_relaxed)) {
    volatile double result = BatchWorkUnit();
    (void)result;
    ++units;

    // Periodically re-send the batch hint (every 1000 work units)
    if (region && send_hints && (++cue_counter % 1000 == 0)) {
      WriteCue(region, slot, HINT_BATCH,
               absl::StrFormat("batch=%d units=%ld", id, units).c_str());
    }
  }

  g_batch_work_units.fetch_add(units, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Percentile computation
// ---------------------------------------------------------------------------
static int64_t Percentile(std::vector<int64_t>& sorted_data, double p) {
  if (sorted_data.empty()) return 0;
  double idx = p / 100.0 * (sorted_data.size() - 1);
  size_t lo = static_cast<size_t>(std::floor(idx));
  size_t hi = static_cast<size_t>(std::ceil(idx));
  if (lo == hi) return sorted_data[lo];
  double frac = idx - lo;
  return static_cast<int64_t>(sorted_data[lo] * (1.0 - frac) +
                              sorted_data[hi] * frac);
}

// ---------------------------------------------------------------------------
// Print and save results
// ---------------------------------------------------------------------------
static void PrintResults(bool send_hints, const char* output_path) {
  // Extract latencies
  std::vector<int64_t> latencies;
  latencies.reserve(g_results.size());
  for (const auto& r : g_results) {
    latencies.push_back(r.wakeup_to_done_ns);
  }
  std::sort(latencies.begin(), latencies.end());

  int64_t batch_units = g_batch_work_units.load();

  printf("\n");
  printf("================================================================\n");
  printf("  iSchedular Evaluation Results\n");
  printf("  Mode: %s\n", send_hints ? "HINTS ENABLED" : "HINTS DISABLED (baseline)");
  printf("================================================================\n");
  printf("\n");
  printf("Server thread latency (wakeup to completion):\n");
  printf("  Total requests:  %zu\n", latencies.size());
  printf("  p50:             %.2f us\n", Percentile(latencies, 50) / 1000.0);
  printf("  p90:             %.2f us\n", Percentile(latencies, 90) / 1000.0);
  printf("  p95:             %.2f us\n", Percentile(latencies, 95) / 1000.0);
  printf("  p99:             %.2f us\n", Percentile(latencies, 99) / 1000.0);
  printf("  p999:            %.2f us\n", Percentile(latencies, 99.9) / 1000.0);
  printf("  max:             %.2f us\n",
         latencies.empty() ? 0.0 : latencies.back() / 1000.0);
  printf("  mean:            %.2f us\n",
         latencies.empty()
             ? 0.0
             : std::accumulate(latencies.begin(), latencies.end(), 0LL) /
                   (double)latencies.size() / 1000.0);
  printf("\n");
  printf("Batch thread throughput:\n");
  printf("  Total work units: %ld\n", batch_units);
  printf("================================================================\n");
  printf("\n");

  // Save CSV
  FILE* f = fopen(output_path, "w");
  if (f) {
    fprintf(f, "thread_id,request_id,latency_ns,hints_enabled\n");
    for (const auto& r : g_results) {
      fprintf(f, "%d,%d,%ld,%s\n", r.thread_id, r.request_id,
              r.wakeup_to_done_ns, send_hints ? "true" : "false");
    }
    fclose(f);
    printf("Results saved to %s\n", output_path);
  } else {
    fprintf(stderr, "Warning: could not open %s for writing\n", output_path);
  }
}

}  // namespace ghost

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  int num_servers = absl::GetFlag(FLAGS_server_threads);
  int num_batch = absl::GetFlag(FLAGS_batch_threads);
  int requests = absl::GetFlag(FLAGS_requests_per_server);
  int work_us = absl::GetFlag(FLAGS_server_work_us);
  int sleep_us = absl::GetFlag(FLAGS_server_sleep_us);
  int duration_sec = absl::GetFlag(FLAGS_duration_sec);
  bool send_hints = absl::GetFlag(FLAGS_send_hints);
  std::string output_file = absl::GetFlag(FLAGS_output_file);

  printf("iSchedular Workload\n");
  printf("  Server threads:   %d\n", num_servers);
  printf("  Batch threads:    %d\n", num_batch);
  printf("  Requests/server:  %d\n", requests);
  printf("  Server work:      %d us\n", work_us);
  printf("  Server sleep avg: %d us\n", sleep_us);
  printf("  Batch duration:   %d sec\n", duration_sec);
  printf("  Hints:            %s\n", send_hints ? "ENABLED" : "DISABLED");
  printf("\n");

  // Open shared memory region (agent must be running)
  ghost::MemCueRegion* region = ghost::OpenRegion();
  if (!region) {
    fprintf(stderr, "ERROR: Could not open shared memory. "
                    "Is agent_cfs_mem running?\n");
    return 1;
  }

  // Launch batch threads (run for duration_sec or until server threads finish)
  ghost::g_stop_batch.store(false);
  std::vector<std::unique_ptr<ghost::GhostThread>> batch_threads;
  for (int b = 0; b < num_batch; ++b) {
    batch_threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [b, region, send_hints] {
          ghost::BatchThread(b, region, send_hints);
        }));
  }

  // Let batch threads warm up
  absl::SleepFor(absl::Milliseconds(500));

  // Launch server threads
  std::vector<std::unique_ptr<ghost::GhostThread>> server_threads;
  for (int s = 0; s < num_servers; ++s) {
    server_threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [s, region, send_hints, requests, work_us, sleep_us] {
          ghost::ServerThread(s, region, send_hints, requests, work_us,
                              sleep_us);
        }));
  }

  // Wait for server threads to complete
  for (auto& t : server_threads) t->Join();
  printf("All server threads completed.\n");

  // Stop batch threads
  ghost::g_stop_batch.store(true);
  for (auto& t : batch_threads) t->Join();
  printf("All batch threads stopped.\n");

  // Print results
  ghost::PrintResults(send_hints, output_file.c_str());

  munmap(region, sizeof(ghost::MemCueRegion));
  return 0;
}
