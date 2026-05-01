// workload_hetero.cc -- 3-class heterogeneous workload + 4 hint_mode runs.
//
// Classes (run concurrently on the same CPU set):
//   latency    : sleep + tiny matmul on a short period
//   deadline   : sleep + medium matmul with a budget (half tight, half loose)
//   throughput : back-to-back matmul, no sleep
//
// --hint_mode selects which class publishes a cue this run:
//   baseline | latency | throughput | deadline
// Only the matching class flags itself; the other two are untouched.

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
ABSL_FLAG(int, latency_workers, 4,
          "Number of latency-class workers.");
ABSL_FLAG(int, latency_dim, 64,
          "Matmul size for latency-class workers.");
ABSL_FLAG(int, latency_period_us, 1000,
          "Inter-event period for latency-class workers (us).");
ABSL_FLAG(int, latency_target_us, 200,
          "Target start-delay (us) for the on-time-rate KPI.");

ABSL_FLAG(int, deadline_workers, 4,
          "Number of deadline-class workers (split half tight / half loose).");
ABSL_FLAG(int, deadline_dim, 128,
          "Matmul size for deadline-class workers.");
ABSL_FLAG(int, deadline_period_ms, 8,
          "Period between deadline-class submissions (ms).");
ABSL_FLAG(int, deadline_tight_budget_ms, 5,
          "Budget (ms) for the 'tight' half of deadline workers.");
ABSL_FLAG(int, deadline_loose_budget_ms, 60,
          "Budget (ms) for the 'loose' half of deadline workers.");

ABSL_FLAG(int, throughput_workers, 4,
          "Number of throughput-class workers.");
ABSL_FLAG(int, throughput_dim, 128,
          "Matmul size for throughput-class workers.");
ABSL_FLAG(int, throughput_slice_us, 20000,
          "Custom slice (us) requested by throughput-class workers when "
          "hint_mode=throughput.");

ABSL_FLAG(int, duration_sec, 10,
          "Total experiment duration.");
ABSL_FLAG(std::string, hint_mode, "baseline",
          "Policy to test: baseline | latency | throughput | deadline.");
ABSL_FLAG(std::string, output_file, "/tmp/hetero_results.csv",
          "Per-event CSV output (one row per event, tagged by class).");

namespace ghost {

enum class HintMode { kBaseline, kLatency, kThroughput, kDeadline };

static HintMode ParseHintMode(const std::string& s) {
  if (s == "latency") return HintMode::kLatency;
  if (s == "throughput") return HintMode::kThroughput;
  if (s == "deadline") return HintMode::kDeadline;
  return HintMode::kBaseline;
}

static const char* HintModeName(HintMode m) {
  switch (m) {
    case HintMode::kBaseline:   return "baseline";
    case HintMode::kLatency:    return "latency";
    case HintMode::kThroughput: return "throughput";
    case HintMode::kDeadline:   return "deadline";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Shared-memory cue helpers
// ---------------------------------------------------------------------------
static MemCueRegion* OpenRegion() {
  int fd = shm_open(kShmName, O_RDWR, 0);
  if (fd < 0) {
    fprintf(stderr,
            "workload_hetero: shm_open failed (is agent_cfs_mem running?)\n");
    return nullptr;
  }
  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    perror("workload_hetero: mmap");
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
// Per-class event record. The class is captured as a small string so we can
// dump everything to one CSV and split by class downstream.
// ---------------------------------------------------------------------------
struct Event {
  const char* class_name;     // "latency", "deadline_tight", "deadline_loose", "throughput"
  int worker;
  int idx;
  int64_t wakeup_delay_ns;    // 0 for throughput class
  int64_t wall_ns;
  int64_t budget_ms;          // 0 for latency / throughput
  bool met;                   // for deadline class only; true for others
};

static std::mutex g_mu;
static std::vector<Event> g_events;
static std::atomic<bool> g_stop{false};

// Live counters for the per-second ticker so the demo terminal shows
// progress instead of going silent for the whole 10 second run.
static std::atomic<int64_t> g_tick_lat{0};
static std::atomic<int64_t> g_tick_dl_tight{0};
static std::atomic<int64_t> g_tick_dl_tight_miss{0};
static std::atomic<int64_t> g_tick_dl_loose{0};
static std::atomic<int64_t> g_tick_tp{0};

// ---------------------------------------------------------------------------
// Worker bodies
// ---------------------------------------------------------------------------
static void LatencyWorker(int id, MemCueRegion* region, HintMode mode,
                          int dim, int period_us) {
  if (region && mode == HintMode::kLatency) {
    uint32_t slot = AllocSlot(region);
    WriteCueKindOnly(region, slot, kHintLatencySensitive);
  }

  Matrix m(dim);
  std::vector<Event> local;
  local.reserve(8192);
  int idx = 0;
  auto next = std::chrono::steady_clock::now();
  while (!g_stop.load(std::memory_order_relaxed)) {
    auto target = next;
    std::this_thread::sleep_until(target);
    if (g_stop.load(std::memory_order_relaxed)) break;
    auto actual = std::chrono::steady_clock::now();
    int64_t delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            actual - target).count();
    if (delay_ns < 0) delay_ns = 0;

    Matmul(m);

    auto done = std::chrono::steady_clock::now();
    int64_t wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            done - actual).count();
    local.push_back({"latency", id, idx++, delay_ns, wall_ns, 0, true});
    g_tick_lat.fetch_add(1, std::memory_order_relaxed);

    next += std::chrono::microseconds(period_us);
    auto now = std::chrono::steady_clock::now();
    if (next < now) next = now;
  }

  std::lock_guard<std::mutex> lk(g_mu);
  g_events.insert(g_events.end(), local.begin(), local.end());
}

static void DeadlineWorker(int id, MemCueRegion* region, HintMode mode,
                           int dim, int period_ms, int budget_ms,
                           bool is_tight) {
  uint32_t slot = 0;
  bool have_slot = (region && mode == HintMode::kDeadline);
  if (have_slot) slot = AllocSlot(region);

  const char* class_name = is_tight ? "deadline_tight" : "deadline_loose";
  Matrix m(dim);
  std::vector<Event> local;
  local.reserve(2048);
  int idx = 0;
  auto next = std::chrono::steady_clock::now();
  while (!g_stop.load(std::memory_order_relaxed)) {
    auto target = next;
    std::this_thread::sleep_until(target);
    if (g_stop.load(std::memory_order_relaxed)) break;
    auto submit = std::chrono::steady_clock::now();
    auto deadline = submit + std::chrono::milliseconds(budget_ms);
    int64_t delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            submit - target).count();
    if (delay_ns < 0) delay_ns = 0;

    if (have_slot) {
      // EDF-style cue: publish the absolute deadline so the scheduler can
      // prioritise this task vs others by deadline.
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
    local.push_back({class_name, id, idx++, delay_ns, wall_ns,
                     static_cast<int64_t>(budget_ms), met});
    if (is_tight) {
      g_tick_dl_tight.fetch_add(1, std::memory_order_relaxed);
      if (!met) g_tick_dl_tight_miss.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_tick_dl_loose.fetch_add(1, std::memory_order_relaxed);
    }

    next += std::chrono::milliseconds(period_ms);
    auto now = std::chrono::steady_clock::now();
    if (next < now) next = now;
  }

  std::lock_guard<std::mutex> lk(g_mu);
  g_events.insert(g_events.end(), local.begin(), local.end());
}

static void ThroughputWorker(int id, MemCueRegion* region, HintMode mode,
                             int dim, int slice_us) {
  if (region && mode == HintMode::kThroughput) {
    uint32_t slot = AllocSlot(region);
    HintPayload p{};
    p.slice_us = slice_us;
    WriteCueWithPayload(region, slot, kHintThroughput, p);
  }

  Matrix m(dim);
  std::vector<Event> local;
  local.reserve(16384);
  int idx = 0;
  auto last = std::chrono::steady_clock::now();
  while (!g_stop.load(std::memory_order_relaxed)) {
    Matmul(m);
    auto now = std::chrono::steady_clock::now();
    int64_t wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last)
            .count();
    last = now;
    local.push_back({"throughput", id, idx++, 0, wall_ns, 0, true});
    g_tick_tp.fetch_add(1, std::memory_order_relaxed);
  }

  std::lock_guard<std::mutex> lk(g_mu);
  g_events.insert(g_events.end(), local.begin(), local.end());
}

// ---------------------------------------------------------------------------
// Stats helpers + reporting
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

static void Report(HintMode mode, int latency_target_us,
                   const std::string& path) {
  printf("\n=============================================================\n");
  printf("  Heterogeneous workload  --  hint_mode = %s\n", HintModeName(mode));
  printf("=============================================================\n");

  // Bucket events by class.
  std::vector<int64_t> lat_delay, dl_t_wall, dl_l_wall, tp_perop;
  int dl_t_total = 0, dl_t_missed = 0;
  int dl_l_total = 0, dl_l_missed = 0;
  int tp_total = 0;
  for (const auto& e : g_events) {
    if (!strcmp(e.class_name, "latency")) {
      lat_delay.push_back(e.wakeup_delay_ns);
    } else if (!strcmp(e.class_name, "deadline_tight")) {
      dl_t_wall.push_back(e.wall_ns);
      ++dl_t_total;
      if (!e.met) ++dl_t_missed;
    } else if (!strcmp(e.class_name, "deadline_loose")) {
      dl_l_wall.push_back(e.wall_ns);
      ++dl_l_total;
      if (!e.met) ++dl_l_missed;
    } else if (!strcmp(e.class_name, "throughput")) {
      tp_perop.push_back(e.wall_ns);
      ++tp_total;
    }
  }
  std::sort(lat_delay.begin(), lat_delay.end());
  std::sort(dl_t_wall.begin(), dl_t_wall.end());
  std::sort(dl_l_wall.begin(), dl_l_wall.end());
  std::sort(tp_perop.begin(), tp_perop.end());

  printf("\n  [Latency class]\n");
  if (lat_delay.empty()) {
    printf("    no events\n");
  } else {
    int on_time =
        static_cast<int>(std::count_if(lat_delay.begin(), lat_delay.end(),
                                       [&](int64_t v) {
                                         return v <=
                                                int64_t{latency_target_us} *
                                                    1000;
                                       }));
    printf("    requests:           %zu\n", lat_delay.size());
    printf("    started <= %d us:   %d (%.2f%%)\n", latency_target_us,
           on_time, 100.0 * on_time / lat_delay.size());
    printf("    wakeup p50:         %ld us\n",
           Percentile(lat_delay, 50.0) / 1000);
    printf("    wakeup p99:         %ld us\n",
           Percentile(lat_delay, 99.0) / 1000);
    printf("    wakeup p999:        %ld us\n",
           Percentile(lat_delay, 99.9) / 1000);
    printf("    wakeup max:         %ld us\n", lat_delay.back() / 1000);
  }

  printf("\n  [Deadline class -- tight budget]\n");
  if (!dl_t_total) {
    printf("    no events\n");
  } else {
    printf("    frames:             %d\n", dl_t_total);
    printf("    missed:             %d (%.1f%%)\n", dl_t_missed,
           100.0 * dl_t_missed / dl_t_total);
    printf("    wall p50:           %ld us\n",
           Percentile(dl_t_wall, 50.0) / 1000);
    printf("    wall p99:           %ld us\n",
           Percentile(dl_t_wall, 99.0) / 1000);
  }

  printf("\n  [Deadline class -- loose budget (sanity check)]\n");
  if (!dl_l_total) {
    printf("    no events\n");
  } else {
    printf("    frames:             %d\n", dl_l_total);
    printf("    missed:             %d (%.1f%%)\n", dl_l_missed,
           100.0 * dl_l_missed / dl_l_total);
  }

  printf("\n  [Throughput class]\n");
  if (!tp_total) {
    printf("    no events\n");
  } else {
    printf("    total ops:          %d\n", tp_total);
    printf("    per-op p50:         %ld us\n",
           Percentile(tp_perop, 50.0) / 1000);
    printf("    per-op p99:         %ld us\n",
           Percentile(tp_perop, 99.0) / 1000);
  }

  if (!path.empty()) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "mode,class,worker,event,wakeup_delay_ns,wall_ns,"
               "budget_ms,met\n");
    for (const auto& e : g_events) {
      fprintf(f, "%s,%s,%d,%d,%ld,%ld,%ld,%d\n",
              HintModeName(mode), e.class_name, e.worker, e.idx,
              e.wakeup_delay_ns, e.wall_ns, e.budget_ms, e.met ? 1 : 0);
    }
    fclose(f);
    printf("\n  CSV saved to %s\n", path.c_str());
  }
}

}  // namespace ghost

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  int n_lat        = absl::GetFlag(FLAGS_latency_workers);
  int lat_dim      = absl::GetFlag(FLAGS_latency_dim);
  int lat_period   = absl::GetFlag(FLAGS_latency_period_us);
  int lat_target   = absl::GetFlag(FLAGS_latency_target_us);

  int n_dl         = absl::GetFlag(FLAGS_deadline_workers);
  int dl_dim       = absl::GetFlag(FLAGS_deadline_dim);
  int dl_period    = absl::GetFlag(FLAGS_deadline_period_ms);
  int dl_tight     = absl::GetFlag(FLAGS_deadline_tight_budget_ms);
  int dl_loose     = absl::GetFlag(FLAGS_deadline_loose_budget_ms);

  int n_tp         = absl::GetFlag(FLAGS_throughput_workers);
  int tp_dim       = absl::GetFlag(FLAGS_throughput_dim);
  int tp_slice     = absl::GetFlag(FLAGS_throughput_slice_us);

  int dur          = absl::GetFlag(FLAGS_duration_sec);
  std::string mode_s = absl::GetFlag(FLAGS_hint_mode);
  std::string out    = absl::GetFlag(FLAGS_output_file);

  ghost::HintMode mode = ghost::ParseHintMode(mode_s);

  printf("Heterogeneous 3-class workload\n");
  printf("  Latency:    %d workers, %dx%d matmul, period %d us\n",
         n_lat, lat_dim, lat_dim, lat_period);
  printf("  Deadline:   %d workers, %dx%d matmul, period %d ms, "
         "budgets tight=%dms loose=%dms\n",
         n_dl, dl_dim, dl_dim, dl_period, dl_tight, dl_loose);
  printf("  Throughput: %d workers, %dx%d matmul (continuous)\n",
         n_tp, tp_dim, tp_dim);
  printf("  Duration:   %d s\n", dur);
  printf("  Hint mode:  %s\n", ghost::HintModeName(mode));
  if (mode == ghost::HintMode::kThroughput) {
    printf("  TP slice:   %d us\n", tp_slice);
  }
  printf("\n");

  ghost::MemCueRegion* region = nullptr;
  if (mode != ghost::HintMode::kBaseline) {
    region = ghost::OpenRegion();
    if (!region) {
      fprintf(stderr,
              "ERROR: hints requested but no agent_cfs_mem region found.\n");
      return 1;
    }
  }

  ghost::g_stop.store(false);
  std::vector<std::unique_ptr<ghost::GhostThread>> threads;

  for (int i = 0; i < n_tp; ++i) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, mode, tp_dim, tp_slice] {
          ghost::ThroughputWorker(i, region, mode, tp_dim, tp_slice);
        }));
  }
  // Brief warm-up so throughput workers reach steady state before we
  // start measuring latency / deadline.
  absl::SleepFor(absl::Milliseconds(200));
  for (int i = 0; i < n_lat; ++i) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, mode, lat_dim, lat_period] {
          ghost::LatencyWorker(i, region, mode, lat_dim, lat_period);
        }));
  }
  for (int i = 0; i < n_dl; ++i) {
    bool tight = (i < n_dl / 2);
    int budget = tight ? dl_tight : dl_loose;
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, mode, dl_dim, dl_period, budget, tight] {
          ghost::DeadlineWorker(i, region, mode, dl_dim, dl_period, budget,
                                tight);
        }));
  }

  // Per-second progress ticker so the demo terminal shows running totals
  // for every workload class. We snapshot the counters each second and
  // print a single line to stderr (so it doesn't pollute the CSV stdout
  // path if you ever pipe results).
  std::thread ticker([dur] {
    int64_t prev_lat = 0, prev_tight = 0, prev_loose = 0, prev_tp = 0;
    for (int s = 1; s <= dur; ++s) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (ghost::g_stop.load()) break;
      int64_t lat   = ghost::g_tick_lat.load();
      int64_t tight = ghost::g_tick_dl_tight.load();
      int64_t miss  = ghost::g_tick_dl_tight_miss.load();
      int64_t loose = ghost::g_tick_dl_loose.load();
      int64_t tp    = ghost::g_tick_tp.load();
      fprintf(stderr,
              "  [t=%2ds]  latency:+%-4ld  deadline-tight:+%-3ld (miss=%ld) "
              " deadline-loose:+%-3ld  throughput:+%-5ld\n",
              s, lat - prev_lat, tight - prev_tight, miss,
              loose - prev_loose, tp - prev_tp);
      fflush(stderr);
      prev_lat   = lat;
      prev_tight = tight;
      prev_loose = loose;
      prev_tp    = tp;
    }
  });

  absl::SleepFor(absl::Seconds(dur));
  ghost::g_stop.store(true);
  ticker.join();
  for (auto& t : threads) t->Join();

  ghost::Report(mode, lat_target, out);

  if (region) munmap(region, sizeof(ghost::MemCueRegion));
  return 0;
}
