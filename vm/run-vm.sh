#!/bin/bash
# Boot the ghOSt VM with QEMU + KVM.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="$SCRIPT_DIR/bzImage"
DISK="$SCRIPT_DIR/ghost.img"

# Resolve the real home directory even when invoked via sudo
if [ -n "$SUDO_USER" ]; then
    HOST_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
else
    HOST_HOME="$HOME"
fi

if [ ! -f "$KERNEL" ]; then
    echo "ERROR: Kernel not found at $KERNEL"
    echo "       Make sure vm/bzImage is present in the repository."
    exit 1
fi

if [ ! -f "$DISK" ]; then
    echo "ERROR: Disk image not found at $DISK"
    echo "       Run: sudo bash $SCRIPT_DIR/setup-vm.sh"
    exit 1
fi

exec qemu-system-x86_64 \
    -enable-kvm \
    -m 4G \
    -smp 4 \
    -kernel "$KERNEL" \
    -drive file="$DISK",format=raw,if=virtio \
    -append "root=/dev/vda rw console=ttyS0 nokaslr" \
    -virtfs local,path="$HOST_HOME",security_model=none,mount_tag=host \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -nographic
