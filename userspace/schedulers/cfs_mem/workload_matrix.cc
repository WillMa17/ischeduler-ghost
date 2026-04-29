// workload_matrix.cc — Matrix multiplication workload with diverse SLOs
//
// Demonstrates that iSchedular's hint framework is general-purpose by running
// the SAME computation (matrix multiplication) under three different SLOs,
// each sending a different hint to the scheduler:
//
//   1. DEADLINE jobs:    "Finish this large multiply before time T"
//   2. THROUGHPUT jobs:  "Complete as many multiplies as possible"
//   3. LATENCY jobs:     "Start each small multiply immediately when submitted"
//
// All three job types compete for CPU simultaneously. The scheduler uses hints
// to optimize for each job's specific SLO, rather than treating them all equally.
//
// Usage:
//   1. Start agent:   sudo bazel-bin/agent_cfs_mem --ghost_cpus 0-3
//   2. Run workload:  bazel-bin/workload_matrix [flags]
//
// Build: bazel build -c opt //:workload_matrix

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
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
ABSL_FLAG(int, deadline_jobs, 2,
          "Number of deadline-bound matrix multiply jobs");
ABSL_FLAG(int, throughput_jobs, 2,
          "Number of throughput-oriented matrix multiply jobs");
ABSL_FLAG(int, latency_jobs, 2,
          "Number of latency-sensitive matrix multiply jobs");
ABSL_FLAG(int, deadline_matrix_size, 256,
          "Matrix size (NxN) for deadline jobs");
ABSL_FLAG(int, throughput_matrix_size, 128,
          "Matrix size (NxN) for throughput jobs");
ABSL_FLAG(int, latency_matrix_size, 32,
          "Matrix size (NxN) for latency jobs");
ABSL_FLAG(int, deadline_ms, 500,
          "Deadline in milliseconds for deadline jobs to complete");
ABSL_FLAG(int, latency_requests, 200,
          "Number of multiply requests for latency jobs");
ABSL_FLAG(int, latency_sleep_us, 500,
          "Average sleep between latency job requests (us)");
ABSL_FLAG(int, duration_sec, 10,
          "Duration in seconds for throughput jobs to run");
ABSL_FLAG(bool, send_hints, true,
          "Whether to send scheduling hints via shared memory");
ABSL_FLAG(std::string, output_file, "/tmp/ischeduler_matrix_results.csv",
          "Path to write results as CSV");

namespace ghost {

// ---------------------------------------------------------------------------
// Hint constants (match cfs_mem_scheduler.h)
// ---------------------------------------------------------------------------
static constexpr char HINT_NONE = 0;
static constexpr char HINT_LATENCY_SENSITIVE = 1;
static constexpr char HINT_BATCH = 2;
static constexpr char HINT_DEADLINE = 3;
static constexpr char HINT_THROUGHPUT = 4;

// ---------------------------------------------------------------------------
// Shared memory helpers
// ---------------------------------------------------------------------------
static MemCueRegion* OpenRegion() {
  int fd = shm_open(kShmName, O_RDWR, 0);
  if (fd < 0) {
    fprintf(stderr, "workload_matrix: shm_open failed (is agent_cfs_mem running?)\n");
    return nullptr;
  }
  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    perror("workload_matrix: mmap");
    return nullptr;
  }
  return static_cast<MemCueRegion*>(ptr);
}

static uint32_t AllocSlot(MemCueRegion* region) {
  uint32_t slot =
      region->next_slot.fetch_add(1, std::memory_order_relaxed);
  CHECK_LT(slot, static_cast<uint32_t>(kMemMaxSlots));
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
// Matrix multiplication (real computation)
// ---------------------------------------------------------------------------
static void MatMul(int N, const double* A, const double* B, double* C) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      double sum = 0.0;
      for (int k = 0; k < N; ++k) {
        sum += A[i * N + k] * B[k * N + j];
      }
      C[i * N + j] = sum;
    }
  }
}

static void InitMatrix(int N, double* M, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  for (int i = 0; i < N * N; ++i) {
    M[i] = dist(rng);
  }
}

// ---------------------------------------------------------------------------
// Results collection
// ---------------------------------------------------------------------------
struct DeadlineResult {
  int job_id;
  int64_t elapsed_ms;
  int64_t deadline_ms;
  bool met_deadline;
};

struct LatencyResult {
  int job_id;
  int request_id;
  int64_t submit_to_done_ns;
};

static std::mutex g_mu;
static std::vector<DeadlineResult> g_deadline_results;
static std::vector<LatencyResult> g_latency_results;
static std::atomic<int64_t> g_throughput_count{0};
static std::atomic<bool> g_stop_throughput{false};

// ---------------------------------------------------------------------------
// DEADLINE JOB: Complete a large matrix multiply before a deadline
// ---------------------------------------------------------------------------
static void DeadlineJob(int id, int N, int deadline_ms,
                        MemCueRegion* region, bool send_hints) {
  uint32_t slot = 0;
  if (region && send_hints) {
    slot = AllocSlot(region);
    std::string detail = absl::StrFormat("deadline=%d N=%d ms=%d", id, N, deadline_ms);
    WriteCue(region, slot, HINT_DEADLINE, detail.c_str());
  }

  // Allocate matrices
  std::vector<double> A(N * N), B(N * N), C(N * N);
  InitMatrix(N, A.data(), 42 + id);
  InitMatrix(N, B.data(), 84 + id);

  auto start = std::chrono::steady_clock::now();

  // Send periodic hints as deadline approaches
  if (region && send_hints) {
    // Boost as we get closer to deadline
    std::string detail = absl::StrFormat("deadline=%d URGENT", id);
    WriteCue(region, slot, HINT_LATENCY_SENSITIVE, detail.c_str());
  }

  // Do the actual large matrix multiply
  MatMul(N, A.data(), B.data(), C.data());

  auto end = std::chrono::steady_clock::now();
  int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      end - start).count();

  bool met = (elapsed_ms <= deadline_ms);

  std::lock_guard<std::mutex> lock(g_mu);
  g_deadline_results.push_back({id, elapsed_ms, deadline_ms, met});
}

// ---------------------------------------------------------------------------
// THROUGHPUT JOB: Complete as many medium multiplies as possible
// ---------------------------------------------------------------------------
static void ThroughputJob(int id, int N,
                          MemCueRegion* region, bool send_hints) {
  uint32_t slot = 0;
  if (region && send_hints) {
    slot = AllocSlot(region);
    std::string detail = absl::StrFormat("throughput=%d N=%d", id, N);
    WriteCue(region, slot, HINT_THROUGHPUT, detail.c_str());
  }

  std::vector<double> A(N * N), B(N * N), C(N * N);
  InitMatrix(N, A.data(), 100 + id);
  InitMatrix(N, B.data(), 200 + id);

  int64_t count = 0;
  int cue_counter = 0;

  while (!g_stop_throughput.load(std::memory_order_relaxed)) {
    MatMul(N, A.data(), B.data(), C.data());
    ++count;

    // Re-send throughput hint periodically
    if (region && send_hints && (++cue_counter % 50 == 0)) {
      std::string detail = absl::StrFormat("throughput=%d done=%ld", id, count);
      WriteCue(region, slot, HINT_THROUGHPUT, detail.c_str());
    }
  }

  g_throughput_count.fetch_add(count, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// LATENCY JOB: Each small multiply should start immediately when submitted
// ---------------------------------------------------------------------------
static void LatencyJob(int id, int N, int requests, int sleep_us_mean,
                       MemCueRegion* region, bool send_hints) {
  uint32_t slot = 0;
  if (region && send_hints) {
    slot = AllocSlot(region);
  }

  std::mt19937 rng(300 + id);
  std::exponential_distribution<double> sleep_dist(
      1.0 / static_cast<double>(sleep_us_mean));

  std::vector<double> A(N * N), B(N * N), C(N * N);
  InitMatrix(N, A.data(), 400 + id);
  InitMatrix(N, B.data(), 500 + id);

  std::vector<LatencyResult> local_results;
  local_results.reserve(requests);

  for (int r = 0; r < requests; ++r) {
    // Sleep between requests
    int sleep_us = std::max(static_cast<int>(sleep_dist(rng)), 10);
    absl::SleepFor(absl::Microseconds(sleep_us));

    // Request submitted now
    auto submit = std::chrono::steady_clock::now();

    // Send hint: schedule me immediately
    if (region && send_hints) {
      std::string detail = absl::StrFormat("latency=%d req=%d", id, r);
      WriteCue(region, slot, HINT_LATENCY_SENSITIVE, detail.c_str());
    }

    // Do the small matrix multiply
    MatMul(N, A.data(), B.data(), C.data());

    auto done = std::chrono::steady_clock::now();
    int64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        done - submit).count();

    local_results.push_back({id, r, latency_ns});
  }

  std::lock_guard<std::mutex> lock(g_mu);
  g_latency_results.insert(g_latency_results.end(),
                           local_results.begin(), local_results.end());
}

// ---------------------------------------------------------------------------
// Percentile helper
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
// Print results
// ---------------------------------------------------------------------------
static void PrintResults(bool send_hints, const char* output_path) {
  printf("\n");
  printf("================================================================\n");
  printf("  iSchedular Matrix Workload Results\n");
  printf("  Mode: %s\n", send_hints ? "HINTS ENABLED" : "HINTS DISABLED (baseline)");
  printf("================================================================\n");

  // Deadline results
  printf("\n--- DEADLINE SLO ---\n");
  int deadline_met = 0;
  for (const auto& d : g_deadline_results) {
    const char* status = d.met_deadline ? "MET" : "MISSED";
    printf("  Job %d: %ldms / %ldms deadline  [%s]\n",
           d.job_id, d.elapsed_ms, d.deadline_ms, status);
    if (d.met_deadline) ++deadline_met;
  }
  printf("  Deadlines met: %d / %zu (%.0f%%)\n",
         deadline_met, g_deadline_results.size(),
         g_deadline_results.empty() ? 0.0 :
         100.0 * deadline_met / g_deadline_results.size());

  // Throughput results
  int64_t total_throughput = g_throughput_count.load();
  printf("\n--- THROUGHPUT SLO ---\n");
  printf("  Total multiplies completed: %ld\n", total_throughput);

  // Latency results
  printf("\n--- LATENCY SLO ---\n");
  std::vector<int64_t> latencies;
  latencies.reserve(g_latency_results.size());
  for (const auto& r : g_latency_results) {
    latencies.push_back(r.submit_to_done_ns);
  }
  std::sort(latencies.begin(), latencies.end());

  if (!latencies.empty()) {
    printf("  Total requests:  %zu\n", latencies.size());
    printf("  p50:             %.2f us\n", Percentile(latencies, 50) / 1000.0);
    printf("  p90:             %.2f us\n", Percentile(latencies, 90) / 1000.0);
    printf("  p95:             %.2f us\n", Percentile(latencies, 95) / 1000.0);
    printf("  p99:             %.2f us\n", Percentile(latencies, 99) / 1000.0);
    printf("  p999:            %.2f us\n", Percentile(latencies, 99.9) / 1000.0);
    printf("  max:             %.2f us\n", latencies.back() / 1000.0);
    printf("  mean:            %.2f us\n",
           std::accumulate(latencies.begin(), latencies.end(), 0LL) /
               (double)latencies.size() / 1000.0);
  }

  printf("\n================================================================\n");

  // Summary table
  printf("\n  SUMMARY:\n");
  printf("  %-20s %-15s\n", "SLO Type", "Result");
  printf("  %-20s %d/%zu met\n", "Deadline",
         deadline_met, g_deadline_results.size());
  printf("  %-20s %ld multiplies\n", "Throughput", total_throughput);
  printf("  %-20s p99=%.1fus\n", "Latency",
         latencies.empty() ? 0.0 : Percentile(latencies, 99) / 1000.0);
  printf("================================================================\n\n");

  // Save CSV
  FILE* f = fopen(output_path, "w");
  if (f) {
    fprintf(f, "slo_type,job_id,request_id,value_ns,deadline_ms,met_deadline,hints_enabled\n");

    for (const auto& d : g_deadline_results) {
      fprintf(f, "deadline,%d,0,%ld,%ld,%s,%s\n",
              d.job_id, d.elapsed_ms * 1000000,
              d.deadline_ms, d.met_deadline ? "true" : "false",
              send_hints ? "true" : "false");
    }

    for (const auto& r : g_latency_results) {
      fprintf(f, "latency,%d,%d,%ld,0,n/a,%s\n",
              r.job_id, r.request_id, r.submit_to_done_ns,
              send_hints ? "true" : "false");
    }

    fprintf(f, "throughput,0,0,%ld,0,n/a,%s\n",
            total_throughput, send_hints ? "true" : "false");

    fclose(f);
    printf("Results saved to %s\n", output_path);
  }
}

}  // namespace ghost

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  int n_deadline = absl::GetFlag(FLAGS_deadline_jobs);
  int n_throughput = absl::GetFlag(FLAGS_throughput_jobs);
  int n_latency = absl::GetFlag(FLAGS_latency_jobs);
  int deadline_N = absl::GetFlag(FLAGS_deadline_matrix_size);
  int throughput_N = absl::GetFlag(FLAGS_throughput_matrix_size);
  int latency_N = absl::GetFlag(FLAGS_latency_matrix_size);
  int deadline_ms = absl::GetFlag(FLAGS_deadline_ms);
  int latency_requests = absl::GetFlag(FLAGS_latency_requests);
  int latency_sleep = absl::GetFlag(FLAGS_latency_sleep_us);
  int duration_sec = absl::GetFlag(FLAGS_duration_sec);
  bool send_hints = absl::GetFlag(FLAGS_send_hints);
  std::string output_file = absl::GetFlag(FLAGS_output_file);

  printf("iSchedular Matrix Workload\n");
  printf("  Deadline jobs:    %d (size %dx%d, deadline %dms)\n",
         n_deadline, deadline_N, deadline_N, deadline_ms);
  printf("  Throughput jobs:  %d (size %dx%d, duration %ds)\n",
         n_throughput, throughput_N, throughput_N, duration_sec);
  printf("  Latency jobs:     %d (size %dx%d, %d requests)\n",
         n_latency, latency_N, latency_N, latency_requests);
  printf("  Hints:            %s\n", send_hints ? "ENABLED" : "DISABLED");
  printf("\n");

  ghost::MemCueRegion* region = ghost::OpenRegion();
  if (!region) {
    fprintf(stderr, "ERROR: Could not open shared memory.\n");
    return 1;
  }

  // Start throughput jobs first (they run continuously)
  ghost::g_stop_throughput.store(false);
  std::vector<std::unique_ptr<ghost::GhostThread>> throughput_threads;
  for (int i = 0; i < n_throughput; ++i) {
    throughput_threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, throughput_N, region, send_hints] {
          ghost::ThroughputJob(i, throughput_N, region, send_hints);
        }));
  }

  // Let throughput jobs warm up and saturate CPUs
  absl::SleepFor(absl::Milliseconds(500));
  printf("Throughput jobs saturating CPUs...\n");

  // Launch deadline jobs (they complete and exit)
  std::vector<std::unique_ptr<ghost::GhostThread>> deadline_threads;
  for (int i = 0; i < n_deadline; ++i) {
    deadline_threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, deadline_N, deadline_ms, region, send_hints] {
          ghost::DeadlineJob(i, deadline_N, deadline_ms, region, send_hints);
        }));
  }

  // Launch latency jobs (they process requests and exit)
  std::vector<std::unique_ptr<ghost::GhostThread>> latency_threads;
  for (int i = 0; i < n_latency; ++i) {
    latency_threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, latency_N, latency_requests, latency_sleep, region, send_hints] {
          ghost::LatencyJob(i, latency_N, latency_requests, latency_sleep,
                            region, send_hints);
        }));
  }

  // Wait for deadline and latency jobs to finish
  for (auto& t : deadline_threads) t->Join();
  printf("All deadline jobs completed.\n");

  for (auto& t : latency_threads) t->Join();
  printf("All latency jobs completed.\n");

  // Stop throughput jobs
  ghost::g_stop_throughput.store(true);
  for (auto& t : throughput_threads) t->Join();
  printf("Throughput jobs stopped.\n");

  // Print results
  ghost::PrintResults(send_hints, output_file.c_str());

  munmap(region, sizeof(ghost::MemCueRegion));
  return 0;
}
