#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="$SCRIPT_DIR/bzImage"
DISK="$SCRIPT_DIR/ghost.img"

if [ ! -f "$KERNEL" ]; then
    echo "ERROR: Kernel not found at $KERNEL"
    exit 1
fi

if [ ! -f "$DISK" ]; then
    echo "ERROR: Disk image not found at $DISK"
    exit 1
fi

exec qemu-system-x86_64 \
    -cpu qemu64\
    -m 6G \
    -smp 4 \
    -kernel "$KERNEL" \
    -drive file="$DISK",format=raw,if=virtio \
    -append "root=/dev/vda rw console=ttyS0 nokaslr" \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -nographic
