// workload_unified.cc -- one workload, three policies.
//
// All worker threads run the SAME body: every `period_ms`, wake up, do one
// matmul that has a deadline of `submit + budget_ms`. Each event records
// three things, one per SLO:
//
//   - wakeup_delay_ns  = actual_wake - target_wake          (latency KPI)
//   - completed event count                                 (throughput KPI)
//   - met = (completion <= deadline)                        (deadline KPI)
//
// The benchmark is run multiple times with `--hint_mode` selecting which
// policy hint the workload publishes via the shared-memory cue channel.
// The four supported modes are:
//
//   none        -> publish nothing (Phase A baseline)
//   latency     -> WriteCue(kHintLatencySensitive) on every wakeup
//   throughput  -> WriteCue(kHintThroughput, slice_us=20000) once at start
//   deadline    -> WriteCue(kHintDeadline, deadline_unix_ns) on every wakeup
//
// The agent_cfs_mem scheduler reacts to whichever hint kind it sees:
//   latency   -> immediate vruntime boost
//   throughput-> per-task custom_slice override of MinPreemptionGranularity
//   deadline  -> SetDeadline + boost on cue + RescueDeadlineTasks scan
//
// So the same workload, run four times under four policies, lets us
// observe how each policy moves *exactly the SLO it targets* without
// changing what the workload itself does.
//
// Result CSV is per-event (one row per matmul completion) so the
// downstream plotter can produce a 4-policy-by-3-KPI comparison chart.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
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
ABSL_FLAG(int, workers, 6,
          "Number of uniform ghost workers.");
ABSL_FLAG(int, dim, 256,
          "Side length of the per-event matmul.");
ABSL_FLAG(int, period_ms, 40,
          "Inter-event wake-up period (each worker submits at this rate).");
ABSL_FLAG(int, budget_ms, 35,
          "Deadline budget for each event (deadline = submit + budget_ms).");
ABSL_FLAG(int, throughput_slice_us, 20000,
          "If hint_mode=throughput, the requested per-task slice in us.");
ABSL_FLAG(int, latency_target_us, 50,
          "On-time-rate target for the latency KPI in us.");
ABSL_FLAG(int, duration_sec, 10,
          "Total experiment duration.");
ABSL_FLAG(std::string, hint_mode, "none",
          "Which hint to publish: none | latency | throughput | deadline");
ABSL_FLAG(std::string, output_file, "/tmp/unified_results.csv",
          "Per-event CSV output.");

namespace ghost {

enum class HintMode { kNone, kLatency, kThroughput, kDeadline };

static HintMode ParseHintMode(const std::string& s) {
  if (s == "none") return HintMode::kNone;
  if (s == "latency") return HintMode::kLatency;
  if (s == "throughput") return HintMode::kThroughput;
  if (s == "deadline") return HintMode::kDeadline;
  return HintMode::kNone;
}

static const char* HintModeName(HintMode m) {
  switch (m) {
    case HintMode::kNone:       return "none";
    case HintMode::kLatency:    return "latency";
    case HintMode::kThroughput: return "throughput";
    case HintMode::kDeadline:   return "deadline";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Shared memory helpers
// ---------------------------------------------------------------------------
static MemCueRegion* OpenRegion() {
  int fd = shm_open(kShmName, O_RDWR, 0);
  if (fd < 0) {
    fprintf(stderr, "workload_unified: shm_open failed (is agent_cfs_mem "
                    "running?). Continuing without hint channel.\n");
    return nullptr;
  }
  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    perror("workload_unified: mmap");
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

static void WriteCueKindOnly(MemCueRegion* region, uint32_t slot_idx,
                             char kind) {
  MemCueSlot& slot = region->slots[slot_idx];
  slot.sent_ns = absl::ToUnixNanos(absl::Now());
  slot.message[0] = kind;
  slot.message[1] = 0;
  slot.seq.fetch_add(1, std::memory_order_release);
}

static void WriteCueWithPayload(MemCueRegion* region, uint32_t slot_idx,
                                char kind, const HintPayload& p) {
  MemCueSlot& slot = region->slots[slot_idx];
  slot.sent_ns = absl::ToUnixNanos(absl::Now());
  slot.message[0] = kind;
  static_assert(sizeof(HintPayload) + 1 <= sizeof(slot.message));
  memcpy(slot.message + 1, &p, sizeof(HintPayload));
  slot.seq.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Naive matmul (per-thread storage)
// ---------------------------------------------------------------------------
struct Matrix {
  int dim;
  std::vector<float> a, b, c;
  Matrix(int n) : dim(n), a(n * n), b(n * n), c(n * n) {
    for (int i = 0; i < n * n; ++i) {
      a[i] = 0.001f * static_cast<float>(i % 1000);
      b[i] = 0.002f * static_cast<float>((i + 7) % 1000);
    }
  }
};

static void Matmul(Matrix& m) {
  const int n = m.dim;
  const float* A = m.a.data();
  const float* B = m.b.data();
  float* C = m.c.data();
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      float s = 0.f;
      for (int k = 0; k < n; ++k) {
        s += A[i * n + k] * B[k * n + j];
      }
      C[i * n + j] = s;
    }
  }
  volatile float sink = C[0];
  (void)sink;
}

// ---------------------------------------------------------------------------
// Event record (one row per matmul completion). All KPIs derive from this.
// ---------------------------------------------------------------------------
struct Event {
  int worker;
  int idx;
  int64_t wakeup_delay_ns;  // actual_wake - target_wake
  int64_t wall_ns;          // matmul wall time
  bool met;                 // completion <= deadline
};

static std::mutex g_mu;
static std::vector<Event> g_events;
static std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------------------
// Uniform worker
// ---------------------------------------------------------------------------
static void UnifiedWorker(int id, MemCueRegion* region, HintMode mode,
                          int dim, int period_ms, int budget_ms,
                          int throughput_slice_us) {
  uint32_t slot = 0;
  bool have_slot = false;
  if (region && mode != HintMode::kNone) {
    slot = AllocSlot(region);
    have_slot = true;
    // Latency and throughput hints are sticky in the scheduler (set once,
    // affect every subsequent enqueue). Send them at startup so the
    // first wake-up already gets the right treatment. Deadline cues are
    // per-event because the deadline value changes every period.
    switch (mode) {
      case HintMode::kLatency:
        WriteCueKindOnly(region, slot, kHintLatencySensitive);
        break;
      case HintMode::kThroughput: {
        HintPayload p{};
        p.slice_us = throughput_slice_us;
        WriteCueWithPayload(region, slot, kHintThroughput, p);
        break;
      }
      case HintMode::kDeadline:
      case HintMode::kNone:
        break;
    }
  }

  Matrix m(dim);
  std::vector<Event> local;
  local.reserve(1024);
  int idx = 0;

  auto next_target = std::chrono::steady_clock::now();
  while (!g_stop.load(std::memory_order_relaxed)) {
    auto target = next_target;
    std::this_thread::sleep_until(target);
    if (g_stop.load(std::memory_order_relaxed)) break;

    auto actual = std::chrono::steady_clock::now();
    int64_t wakeup_delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            actual - target).count();
    if (wakeup_delay_ns < 0) wakeup_delay_ns = 0;

    auto submit = actual;
    auto deadline = submit + std::chrono::milliseconds(budget_ms);

    if (have_slot && mode == HintMode::kDeadline) {
      // Per-event cue: the deadline_unix_ns moves forward every period.
      HintPayload p{};
      p.deadline_unix_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              deadline.time_since_epoch()).count();
      WriteCueWithPayload(region, slot, kHintDeadline, p);
    }

    Matmul(m);

    auto done = std::chrono::steady_clock::now();
    int64_t wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            done - submit).count();
    bool met = (done <= deadline);

    Event ev{id, idx++, wakeup_delay_ns, wall_ns, met};
    local.push_back(ev);

    next_target += std::chrono::milliseconds(period_ms);
    auto now = std::chrono::steady_clock::now();
    if (next_target < now) next_target = now;
  }

  std::lock_guard<std::mutex> lk(g_mu);
  g_events.insert(g_events.end(), local.begin(), local.end());
}

// ---------------------------------------------------------------------------
// Stats helpers
// ---------------------------------------------------------------------------
static int64_t Percentile(std::vector<int64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  double idx = p / 100.0 * (sorted.size() - 1);
  size_t lo = static_cast<size_t>(std::floor(idx));
  size_t hi = static_cast<size_t>(std::ceil(idx));
  if (lo == hi) return sorted[lo];
  double f = idx - lo;
  return static_cast<int64_t>(sorted[lo] * (1.0 - f) + sorted[hi] * f);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void ReportAndSave(HintMode mode, int latency_target_us, int budget_ms,
                          const std::string& path) {
  printf("\n=============================================================\n");
  printf("  Unified workload  --  hint_mode = %s\n", HintModeName(mode));
  printf("=============================================================\n");

  if (g_events.empty()) {
    printf("  no events recorded\n");
    return;
  }

  std::vector<int64_t> wakeup;
  std::vector<int64_t> wall;
  int missed = 0;
  for (const auto& e : g_events) {
    wakeup.push_back(e.wakeup_delay_ns);
    wall.push_back(e.wall_ns);
    if (!e.met) ++missed;
  }
  std::sort(wakeup.begin(), wakeup.end());
  std::sort(wall.begin(), wall.end());

  int64_t total = static_cast<int>(g_events.size());
  int on_time =
      static_cast<int>(std::count_if(wakeup.begin(), wakeup.end(),
                                     [&](int64_t v) {
                                       return v <= int64_t{latency_target_us} *
                                                       1000;
                                     }));

  printf("  Events completed (throughput KPI): %ld\n", total);
  printf("\n  [Latency KPI: wakeup-to-start delay]\n");
  printf("    Started <= %d us:   %d  (%.2f%%)\n", latency_target_us,
         on_time, 100.0 * on_time / total);
  printf("    p50:   %ld us\n", Percentile(wakeup, 50.0)  / 1000);
  printf("    p90:   %ld us\n", Percentile(wakeup, 90.0)  / 1000);
  printf("    p99:   %ld us\n", Percentile(wakeup, 99.0)  / 1000);
  printf("    p999:  %ld us\n", Percentile(wakeup, 99.9)  / 1000);
  printf("    max:   %ld us\n", wakeup.back() / 1000);

  printf("\n  [Deadline KPI: %d ms budget]\n", budget_ms);
  printf("    Missed:  %d / %ld  (%.1f%%)\n", missed, total,
         100.0 * missed / total);

  printf("\n  [Per-event wall time (interference signal)]\n");
  printf("    p50:   %ld us\n", Percentile(wall, 50.0)  / 1000);
  printf("    p99:   %ld us\n", Percentile(wall, 99.0)  / 1000);
  printf("    max:   %ld us\n", wall.back() / 1000);

  if (!path.empty()) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "mode,worker,event,wakeup_delay_ns,wall_ns,met\n");
    for (const auto& e : g_events) {
      fprintf(f, "%s,%d,%d,%ld,%ld,%d\n",
              HintModeName(mode), e.worker, e.idx,
              e.wakeup_delay_ns, e.wall_ns, e.met ? 1 : 0);
    }
    fclose(f);
    printf("\n  Per-event CSV saved to %s\n", path.c_str());
  }
}

}  // namespace ghost

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  int n        = absl::GetFlag(FLAGS_workers);
  int dim      = absl::GetFlag(FLAGS_dim);
  int period   = absl::GetFlag(FLAGS_period_ms);
  int budget   = absl::GetFlag(FLAGS_budget_ms);
  int slice_us = absl::GetFlag(FLAGS_throughput_slice_us);
  int target   = absl::GetFlag(FLAGS_latency_target_us);
  int dur      = absl::GetFlag(FLAGS_duration_sec);
  std::string mode_s = absl::GetFlag(FLAGS_hint_mode);
  std::string out    = absl::GetFlag(FLAGS_output_file);

  ghost::HintMode mode = ghost::ParseHintMode(mode_s);

  printf("Unified workload\n");
  printf("  Workers:           %d\n", n);
  printf("  Per-event matmul:  %dx%d\n", dim, dim);
  printf("  Period / budget:   %d / %d ms\n", period, budget);
  printf("  Hint mode:         %s\n", ghost::HintModeName(mode));
  if (mode == ghost::HintMode::kThroughput) {
    printf("  Throughput slice:  %d us\n", slice_us);
  }
  printf("  Duration:          %d s\n", dur);
  printf("\n");

  ghost::MemCueRegion* region = nullptr;
  if (mode != ghost::HintMode::kNone) {
    region = ghost::OpenRegion();
    if (!region) {
      fprintf(stderr,
              "ERROR: hints requested but no agent_cfs_mem region found.\n");
      return 1;
    }
  }

  ghost::g_stop.store(false);
  std::vector<std::unique_ptr<ghost::GhostThread>> threads;
  for (int i = 0; i < n; ++i) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, mode, dim, period, budget, slice_us] {
          ghost::UnifiedWorker(i, region, mode, dim, period, budget,
                               slice_us);
        }));
  }

  absl::SleepFor(absl::Seconds(dur));
  ghost::g_stop.store(true);
  for (auto& t : threads) t->Join();

  ghost::ReportAndSave(mode, target, budget, out);

  if (region) munmap(region, sizeof(ghost::MemCueRegion));
  return 0;
}
