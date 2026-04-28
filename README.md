# ghOSt Playground

Workspace for experimenting with ghOSt (userspace Linux scheduler delegation).
Paper: SOSP '21 — https://dl.acm.org/doi/10.1145/3477132.3483542

## Directory structure

```
ghost/
  kernel/     # ghost-kernel: Linux 5.11 + ghOSt scheduling class patches
  userspace/  # ghost-userspace: agent library + example policies
  vm/         # QEMU VM assets (disk image, run script) — to be set up
```

## Workflow overview

### Step 1 — Build the ghOSt kernel

```bash
cd kernel/
cp /boot/config-$(uname -r) .config   # start from current kernel config
make olddefconfig
make -j$(nproc)
```

The resulting kernel image will be at `arch/x86/boot/bzImage`.

### Step 2 — Set up a VM disk image

See vm/README.md (to be written) for how to create an Alpine or Debian image
and boot it with the ghOSt kernel under QEMU + KVM.

### Step 3 — Build the userspace agents

Must be done *inside* the VM running the ghOSt kernel (the userspace library
makes syscalls specific to the ghOSt scheduling class).

```bash
# Inside the VM:
sudo apt install libnuma-dev libcap-dev libelf-dev libbfd-dev gcc clang-12 llvm zlib1g-dev python-is-python3
# Install Bazel: https://docs.bazel.build/versions/main/install.html
bazel build ...
```

### Step 4 — Run an example agent

```bash
# Inside the VM, as root:
bazel run //agents/fifo:fifo_per_cpu_agent -- --ghost_cpus 0-1
```

## Key source locations (userspace/)

- `lib/`              — core ghOSt API (transactions, message queues, status words)
- `agents/`           — example scheduling policies (FIFO, CFS, Shinjuku, EDF...)
- `schedulers/`       — more complete scheduler implementations
- `tests/`            — unit + integration tests

## Key source locations (kernel/)

- `kernel/sched/ghost.c`   — ghOSt scheduling class (~3,777 LOC per paper)
- `kernel/sched/core.c`    — hooks into the main scheduler
- `include/uapi/linux/ghost.h` — kernel-userspace API (messages, syscalls)
