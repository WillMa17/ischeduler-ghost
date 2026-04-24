#include <string>

#include "absl/debugging/symbolize.h"
#include "absl/flags/parse.h"
#include "lib/agent.h"
#include "lib/enclave.h"
#include "schedulers/cfs_mem/cfs_mem_scheduler.h"

ABSL_FLAG(std::string, ghost_cpus, "1-5", "cpulist");
ABSL_FLAG(std::string, enclave, "", "Connect to preexisting enclave directory");
ABSL_FLAG(absl::Duration, min_granularity, absl::Milliseconds(1),
          "Minimum time a task runs before being preempted");
ABSL_FLAG(absl::Duration, latency, absl::Milliseconds(10),
          "Target period in which all tasks run at least once");

namespace ghost {

static void ParseAgentConfig(CfsConfig* config) {
  CpuList ghost_cpus =
      MachineTopology()->ParseCpuStr(absl::GetFlag(FLAGS_ghost_cpus));
  CHECK(!ghost_cpus.Empty());

  config->topology_ = MachineTopology();
  config->cpus_ = ghost_cpus;
  std::string enclave = absl::GetFlag(FLAGS_enclave);
  if (!enclave.empty()) {
    int fd = open(enclave.c_str(), O_PATH);
    CHECK_GE(fd, 0);
    config->enclave_fd_ = fd;
  }
  config->min_granularity_ = absl::GetFlag(FLAGS_min_granularity);
  config->latency_ = absl::GetFlag(FLAGS_latency);
}

}  // namespace ghost

int main(int argc, char* argv[]) {
  absl::InitializeSymbolizer(argv[0]);
  absl::ParseCommandLine(argc, argv);

  ghost::CfsConfig config;
  ghost::ParseAgentConfig(&config);

  printf("Initializing...\n");

  auto uap =
      new ghost::AgentProcess<ghost::FullMemCfsAgent, ghost::CfsConfig>(config);

  ghost::GhostHelper()->InitCore();
  printf("Initialization complete, ghOSt active.\n");
  fflush(stdout);

  ghost::Notification exit;
  ghost::GhostSignals::AddHandler(SIGINT, [&exit](int) {
    static bool first = true;
    if (first) {
      exit.Notify();
      first = false;
      return false;
    }
    return true;
  });

  ghost::GhostSignals::AddHandler(SIGUSR1, [uap](int) {
    uap->Rpc(ghost::CfsScheduler::kDebugRunqueue);
    return false;
  });

  exit.WaitForNotification();
  delete uap;

  printf("\nDone!\n");
  return 0;
}
