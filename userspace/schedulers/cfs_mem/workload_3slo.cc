// workload_3slo.cc -- Three-SLO co-location workload.
//
// Spawns three classes of ghost threads on the same set of CPUs and reports
// SLO-specific KPIs for each. Designed to expose scheduling tradeoffs that a
// hint-aware scheduler can later resolve.
//
//   1. Deadline workers
//      Each worker submits one "frame" every `--deadline_period_ms`, computes
//      a `--deadline_dim`x`--deadline_dim` matrix multiply, and reports
//      whether it finished before `submit_time + deadline_budget_ms`.
//      KPI: miss rate, slack distribution (positive = met with margin).
//
//   2. Throughput workers
//      Each worker runs a tight loop of `--throughput_dim`x`--throughput_dim`
//      multiplies for `--duration_sec` and reports total ops completed.
//      KPI: total ops, ops/sec.
//
//   3. Latency workers
//      Each worker sleeps for an exponential interval (mean
//      `--latency_sleep_us`), wakes, and immediately performs a
//      `--latency_dim`x`--latency_dim` multiply. Records the gap between the
//      intended wake time and the actual wake time -- this is the
//      submission-to-start delay.
//      KPI: start-latency P50/P99/P999 and the share of requests that started
//      within `--latency_target_us` of submission.
//
// Phase A baseline: pass `--send_hints=false` so the workload publishes no
// cues and the scheduler is the unmodified ghOSt CFS. Phase B+ will set
// `--send_hints=true` and exercise the hint protocol.

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
// Default to N=3 of every class so the three classes contribute equally to
// CPU pressure.  9 ghost threads on 4 vCPUs gives ~2.25x oversubscription.
ABSL_FLAG(int, deadline_workers, 3,
          "Number of deadline workers (one matmul per period).");
ABSL_FLAG(int, deadline_dim, 256,
          "Side length of the deadline-job matrix.");
ABSL_FLAG(int, deadline_budget_ms, 100,
          "Per-frame deadline budget in milliseconds.");
ABSL_FLAG(int, deadline_period_ms, 250,
          "Inter-frame submission period in milliseconds.");

ABSL_FLAG(int, throughput_workers, 3,
          "Number of throughput workers.");
ABSL_FLAG(int, throughput_dim, 128,
          "Side length of the throughput-job matrix.");

ABSL_FLAG(int, latency_workers, 3,
          "Number of latency workers.");
ABSL_FLAG(int, latency_dim, 64,
          "Side length of the latency-job matrix.");
ABSL_FLAG(int, latency_sleep_us, 1000,
          "Mean inter-request sleep (exponential dist) in microseconds.");
ABSL_FLAG(int, latency_target_us, 50,
          "Target start-latency in microseconds (used for hit-rate KPI).");

ABSL_FLAG(int, duration_sec, 10,
          "Total experiment duration in seconds.");
ABSL_FLAG(bool, send_hints, false,
          "Publish hints to /ghost_mem_cues. Phase A leaves this off.");
ABSL_FLAG(std::string, output_file, "/tmp/3slo_results.csv",
          "Per-event CSV output.");

namespace ghost {

// ---------------------------------------------------------------------------
// Shared memory helpers (only used when --send_hints is set)
// ---------------------------------------------------------------------------
static MemCueRegion* OpenRegion() {
  int fd = shm_open(kShmName, O_RDWR, 0);
  if (fd < 0) {
    fprintf(stderr, "workload_3slo: shm_open failed (is agent_cfs_mem "
                    "running?). Continuing without hint channel.\n");
    return nullptr;
  }
  void* ptr = mmap(nullptr, sizeof(MemCueRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    perror("workload_3slo: mmap");
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

// Plain-text variant: kind byte followed by a null-terminated debug string.
// Used by latency / batch hints whose semantics are fully captured by the
// kind byte (no extra parameters required).
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

// Structured variant: kind byte followed by a packed HintPayload. Used for
// kHintThroughput (slice_us) and kHintDeadline (deadline_unix_ns).
static void WriteCueWithPayload(MemCueRegion* region, uint32_t slot_idx,
                                char hint_type, const HintPayload& p) {
  MemCueSlot& slot = region->slots[slot_idx];
  slot.sent_ns = absl::ToUnixNanos(absl::Now());
  slot.message[0] = hint_type;
  static_assert(sizeof(HintPayload) + 1 <= sizeof(slot.message));
  memcpy(slot.message + 1, &p, sizeof(HintPayload));
  slot.seq.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Matrix multiply -- naive O(N^3), per-thread storage so workers don't share
// matrix state across cache lines.
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

// Computes C = A * B in place. Writes a single element back through volatile
// to defeat dead-store elimination.
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
// Result records (one mutex per type to keep contention low across writers)
// ---------------------------------------------------------------------------
// Each deadline frame carries both a "scheduling latency" (actual_wake -
// target_wake, i.e. how late the period boundary was honored) and the
// per-frame wall-clock duration so we can correlate misses with starvation.
struct DeadlineEvent {
  int worker;
  int frame;
  int64_t submit_unix_ns;
  int64_t complete_unix_ns;
  int64_t deadline_unix_ns;
  int64_t wakeup_delay_ns;  // actual_wake - target_wake at frame start
  bool met;                 // complete <= deadline
};

struct LatencyEvent {
  int worker;
  int request;
  int64_t start_delay_ns;   // actual_wake - intended_wake
};

// Throughput workers cannot have a "wakeup delay" because they never sleep.
// Their per-iteration wall time is the comparable scheduling-impact signal:
// uncontested it should equal the intrinsic matmul cost; under preemption
// it grows by the time the worker spent off-CPU mid-iteration.
struct ThroughputEvent {
  int worker;
  int op;
  int64_t per_op_ns;
};

struct ThroughputResult {
  int worker;
  int64_t ops_completed;
  int64_t wall_ns;
};

static std::mutex g_dl_mu;
static std::vector<DeadlineEvent> g_dl_events;
static std::mutex g_lat_mu;
static std::vector<LatencyEvent> g_lat_events;
static std::mutex g_tp_mu;
static std::vector<ThroughputResult> g_tp_results;
static std::vector<ThroughputEvent> g_tp_events;

static std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------------------
// Deadline worker
// ---------------------------------------------------------------------------
static void DeadlineWorker(int id, MemCueRegion* region, bool send_hints,
                           int dim, int budget_ms, int period_ms) {
  uint32_t slot = 0;
  if (region && send_hints) slot = AllocSlot(region);

  Matrix m(dim);
  std::vector<DeadlineEvent> local;
  int frame = 0;
  auto next_submit = std::chrono::steady_clock::now();

  while (!g_stop.load(std::memory_order_relaxed)) {
    auto target_wake = next_submit;
    std::this_thread::sleep_until(target_wake);
    if (g_stop.load(std::memory_order_relaxed)) break;

    auto submit = std::chrono::steady_clock::now();
    auto deadline = submit + std::chrono::milliseconds(budget_ms);
    int64_t wakeup_delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            submit - target_wake).count();
    if (wakeup_delay_ns < 0) wakeup_delay_ns = 0;

    if (region && send_hints) {
      // Phase C: publish the absolute deadline so the agent can boost the
      // task on arrival AND rescue it later if the wall-clock drifts into
      // the urgency window.
      HintPayload p{};
      p.deadline_unix_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              deadline.time_since_epoch()).count();
      WriteCueWithPayload(region, slot, kHintDeadline, p);
    }

    Matmul(m);

    auto done = std::chrono::steady_clock::now();
    DeadlineEvent ev;
    ev.worker = id;
    ev.frame = frame++;
    ev.submit_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            submit.time_since_epoch()).count();
    ev.complete_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            done.time_since_epoch()).count();
    ev.deadline_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count();
    ev.wakeup_delay_ns = wakeup_delay_ns;
    ev.met = (done <= deadline);
    local.push_back(ev);

    next_submit += std::chrono::milliseconds(period_ms);
    // If we ran past the next submission slot, snap forward so we don't
    // run-away catch-up after a long stall.
    auto now = std::chrono::steady_clock::now();
    if (next_submit < now) next_submit = now;
  }

  std::lock_guard<std::mutex> lk(g_dl_mu);
  g_dl_events.insert(g_dl_events.end(), local.begin(), local.end());
}

// ---------------------------------------------------------------------------
// Throughput worker
// ---------------------------------------------------------------------------
static void ThroughputWorker(int id, MemCueRegion* region, bool send_hints,
                             int dim) {
  uint32_t slot = 0;
  if (region && send_hints) {
    slot = AllocSlot(region);
    // Phase B: ask for a longer time slice (20 ms) so CFS doesn't preempt
    // us on every default period boundary. The cap inside the scheduler
    // limits this further.
    HintPayload p{};
    p.slice_us = 20000;
    WriteCueWithPayload(region, slot, kHintThroughput, p);
  }

  Matrix m(dim);
  int64_t ops = 0;
  std::vector<ThroughputEvent> local_events;
  local_events.reserve(8192);
  auto t0 = std::chrono::steady_clock::now();
  auto last = t0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    Matmul(m);
    auto now = std::chrono::steady_clock::now();
    int64_t per_op_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last)
            .count();
    last = now;
    local_events.push_back({id, static_cast<int>(ops), per_op_ns});
    ++ops;
  }
  auto t1 = std::chrono::steady_clock::now();

  ThroughputResult r;
  r.worker = id;
  r.ops_completed = ops;
  r.wall_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

  std::lock_guard<std::mutex> lk(g_tp_mu);
  g_tp_results.push_back(r);
  g_tp_events.insert(g_tp_events.end(), local_events.begin(),
                     local_events.end());
}

// ---------------------------------------------------------------------------
// Latency worker
// ---------------------------------------------------------------------------
static void LatencyWorker(int id, MemCueRegion* region, bool send_hints,
                          int dim, int sleep_us_mean) {
  uint32_t slot = 0;
  if (region && send_hints) slot = AllocSlot(region);

  Matrix m(dim);
  std::mt19937 rng(2024 + id);
  std::exponential_distribution<double> dist(
      1.0 / static_cast<double>(sleep_us_mean));
  std::vector<LatencyEvent> local;
  int req = 0;

  while (!g_stop.load(std::memory_order_relaxed)) {
    int us = static_cast<int>(dist(rng));
    us = std::max(us, 10);
    auto target_wake = std::chrono::steady_clock::now() +
                       std::chrono::microseconds(us);
    std::this_thread::sleep_until(target_wake);
    auto actual_wake = std::chrono::steady_clock::now();

    if (region && send_hints) {
      // Phase B: latency-sensitive boost on every wakeup.
      WriteCue(region, slot, kHintLatencySensitive,
               absl::StrFormat("lat=%d req=%d", id, req).c_str());
    }

    int64_t delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            actual_wake - target_wake).count();
    if (delay_ns < 0) delay_ns = 0;

    LatencyEvent ev{id, req++, delay_ns};
    local.push_back(ev);

    Matmul(m);
  }

  std::lock_guard<std::mutex> lk(g_lat_mu);
  g_lat_events.insert(g_lat_events.end(), local.begin(), local.end());
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
static void ReportAndSave(bool send_hints, const std::string& path,
                          int latency_target_us, int budget_ms) {
  printf("\n==============================================================\n");
  printf("  3-SLO Workload Results  (hints %s)\n",
         send_hints ? "ENABLED" : "DISABLED");
  printf("==============================================================\n");

  // -- Deadline KPIs
  printf("\n[Deadline jobs]\n");
  if (g_dl_events.empty()) {
    printf("  no events recorded\n");
  } else {
    int total = static_cast<int>(g_dl_events.size());
    int missed = 0;
    std::vector<int64_t> slack_ns;
    std::vector<int64_t> wakeup_delay_ns;
    slack_ns.reserve(total);
    wakeup_delay_ns.reserve(total);
    for (const auto& e : g_dl_events) {
      slack_ns.push_back(e.deadline_unix_ns - e.complete_unix_ns);
      wakeup_delay_ns.push_back(e.wakeup_delay_ns);
      if (!e.met) ++missed;
    }
    std::sort(slack_ns.begin(), slack_ns.end());
    std::sort(wakeup_delay_ns.begin(), wakeup_delay_ns.end());
    printf("  Total frames:       %d\n", total);
    printf("  Missed deadline:    %d  (%.1f%%)\n", missed,
           100.0 * missed / total);
    printf("  Slack p999:         %ld us  (positive=met, negative=missed by)\n",
           Percentile(slack_ns, 0.1) / 1000);
    printf("  Slack p50:          %ld us\n",
           Percentile(slack_ns, 50.0) / 1000);
    printf("  Slack worst (min):  %ld us\n", slack_ns.front() / 1000);
    printf("  Slack best  (max):  %ld us\n", slack_ns.back() / 1000);
    printf("  Budget:             %d ms\n", budget_ms);
    printf("  Frame wakeup delay p50:  %ld us\n",
           Percentile(wakeup_delay_ns, 50.0) / 1000);
    printf("  Frame wakeup delay p99:  %ld us\n",
           Percentile(wakeup_delay_ns, 99.0) / 1000);
    printf("  Frame wakeup delay max:  %ld us\n",
           wakeup_delay_ns.back() / 1000);
  }

  // -- Throughput KPIs
  printf("\n[Throughput jobs]\n");
  int64_t total_ops = 0;
  double total_wall_ms = 0;
  for (const auto& r : g_tp_results) {
    total_ops += r.ops_completed;
    total_wall_ms += r.wall_ns / 1.0e6;
  }
  printf("  Total ops:          %ld\n", total_ops);
  printf("  Workers:            %zu\n", g_tp_results.size());
  if (!g_tp_results.empty()) {
    printf("  Per-worker mean:    %ld ops\n",
           total_ops / static_cast<int64_t>(g_tp_results.size()));
    double agg_sec = (total_wall_ms / g_tp_results.size()) / 1000.0;
    printf("  Aggregate ops/sec:  %.0f\n",
           static_cast<double>(total_ops) / agg_sec);
  }
  // Per-op wall-time percentiles -- intrinsic cost lives at p1/p5; anything
  // above that is preemption interference.
  if (!g_tp_events.empty()) {
    std::vector<int64_t> per_op;
    per_op.reserve(g_tp_events.size());
    for (const auto& e : g_tp_events) per_op.push_back(e.per_op_ns);
    std::sort(per_op.begin(), per_op.end());
    printf("  Per-op wall p50:    %ld us\n",
           Percentile(per_op, 50.0) / 1000);
    printf("  Per-op wall p99:    %ld us\n",
           Percentile(per_op, 99.0) / 1000);
    printf("  Per-op wall p999:   %ld us\n",
           Percentile(per_op, 99.9) / 1000);
    printf("  Per-op wall max:    %ld us\n", per_op.back() / 1000);
  }

  // -- Latency KPIs
  printf("\n[Latency jobs]\n");
  if (g_lat_events.empty()) {
    printf("  no events recorded\n");
  } else {
    std::vector<int64_t> delays;
    delays.reserve(g_lat_events.size());
    int64_t target_ns =
        static_cast<int64_t>(latency_target_us) * 1000;
    int on_time = 0;
    for (const auto& e : g_lat_events) {
      delays.push_back(e.start_delay_ns);
      if (e.start_delay_ns <= target_ns) ++on_time;
    }
    std::sort(delays.begin(), delays.end());
    printf("  Total requests:     %zu\n", delays.size());
    printf("  Started <= %d us:   %d  (%.2f%%)\n", latency_target_us,
           on_time, 100.0 * on_time / delays.size());
    printf("  Start delay p50:    %ld us\n",
           Percentile(delays, 50.0) / 1000);
    printf("  Start delay p99:    %ld us\n",
           Percentile(delays, 99.0) / 1000);
    printf("  Start delay p999:   %ld us\n",
           Percentile(delays, 99.9) / 1000);
    printf("  Start delay max:    %ld us\n", delays.back() / 1000);
  }
  printf("\n");

  // -- CSV dump for off-line plotting.
  // Schema:
  //   deadline   : a_ns = wall (complete-submit), b_ns = budget,
  //                met_or_target = met ? 1 : 0, c_ns = wakeup_delay_ns
  //   latency    : a_ns = start_delay_ns
  //   throughput_op  : a_ns = per_op_ns (one row per op, sampled on every iter)
  //   throughput_sum : a_ns = total ops, b_ns = wall_ns
  if (!path.empty()) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "type,worker,event,a_ns,b_ns,met_or_target,c_ns\n");
    for (const auto& e : g_dl_events) {
      fprintf(f, "deadline,%d,%d,%ld,%ld,%d,%ld\n",
              e.worker, e.frame,
              e.complete_unix_ns - e.submit_unix_ns,
              e.deadline_unix_ns - e.submit_unix_ns,
              e.met ? 1 : 0,
              e.wakeup_delay_ns);
    }
    for (const auto& e : g_lat_events) {
      fprintf(f, "latency,%d,%d,%ld,0,0,0\n",
              e.worker, e.request, e.start_delay_ns);
    }
    for (const auto& e : g_tp_events) {
      fprintf(f, "throughput_op,%d,%d,%ld,0,0,0\n",
              e.worker, e.op, e.per_op_ns);
    }
    for (const auto& r : g_tp_results) {
      fprintf(f, "throughput_sum,%d,0,%ld,%ld,0,0\n",
              r.worker, r.ops_completed, r.wall_ns);
    }
    fclose(f);
    printf("Per-event CSV saved to %s\n", path.c_str());
  }
}

}  // namespace ghost

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  int n_dl = absl::GetFlag(FLAGS_deadline_workers);
  int dl_dim = absl::GetFlag(FLAGS_deadline_dim);
  int dl_budget = absl::GetFlag(FLAGS_deadline_budget_ms);
  int dl_period = absl::GetFlag(FLAGS_deadline_period_ms);

  int n_tp = absl::GetFlag(FLAGS_throughput_workers);
  int tp_dim = absl::GetFlag(FLAGS_throughput_dim);

  int n_lat = absl::GetFlag(FLAGS_latency_workers);
  int lat_dim = absl::GetFlag(FLAGS_latency_dim);
  int lat_sleep = absl::GetFlag(FLAGS_latency_sleep_us);
  int lat_target = absl::GetFlag(FLAGS_latency_target_us);

  int dur = absl::GetFlag(FLAGS_duration_sec);
  bool send_hints = absl::GetFlag(FLAGS_send_hints);
  std::string out = absl::GetFlag(FLAGS_output_file);

  printf("3-SLO Workload\n");
  printf("  Deadline:    %d worker(s), %dx%d matmul, %d ms budget every %d ms\n",
         n_dl, dl_dim, dl_dim, dl_budget, dl_period);
  printf("  Throughput:  %d worker(s), %dx%d matmul\n",
         n_tp, tp_dim, tp_dim);
  printf("  Latency:     %d worker(s), %dx%d matmul, sleep mean %d us, "
         "target start <= %d us\n",
         n_lat, lat_dim, lat_dim, lat_sleep, lat_target);
  printf("  Duration:    %d s\n", dur);
  printf("  Hints:       %s\n", send_hints ? "ENABLED" : "DISABLED");
  printf("\n");

  ghost::MemCueRegion* region = nullptr;
  if (send_hints) {
    region = ghost::OpenRegion();
    if (!region) {
      fprintf(stderr, "ERROR: hints requested but no agent_cfs_mem region "
                      "available.\n");
      return 1;
    }
  }

  ghost::g_stop.store(false);

  // Spawn workers.
  std::vector<std::unique_ptr<ghost::GhostThread>> threads;
  for (int i = 0; i < n_tp; ++i) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, send_hints, tp_dim] {
          ghost::ThroughputWorker(i, region, send_hints, tp_dim);
        }));
  }
  // Small warm-up so the throughput threads are settled before we start
  // measuring deadline / latency.
  absl::SleepFor(absl::Milliseconds(200));
  for (int i = 0; i < n_dl; ++i) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, send_hints, dl_dim, dl_budget, dl_period] {
          ghost::DeadlineWorker(i, region, send_hints, dl_dim, dl_budget,
                                dl_period);
        }));
  }
  for (int i = 0; i < n_lat; ++i) {
    threads.emplace_back(std::make_unique<ghost::GhostThread>(
        ghost::GhostThread::KernelScheduler::kGhost,
        [i, region, send_hints, lat_dim, lat_sleep] {
          ghost::LatencyWorker(i, region, send_hints, lat_dim, lat_sleep);
        }));
  }

  absl::SleepFor(absl::Seconds(dur));
  ghost::g_stop.store(true);
  for (auto& t : threads) t->Join();

  ghost::ReportAndSave(send_hints, out, lat_target, dl_budget);

  if (region) munmap(region, sizeof(ghost::MemCueRegion));
  return 0;
}
