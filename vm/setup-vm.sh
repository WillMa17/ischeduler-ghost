#!/bin/bash
# Sets up a Debian bookworm root filesystem image for the ghOSt QEMU VM.
# Run as root: sudo bash setup-vm.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="$SCRIPT_DIR/ghost.img"
MNT="/mnt/ghost-rootfs"
IMG_SIZE="12G"

echo "==> Installing debootstrap..."
apt install -y debootstrap

echo "==> Creating ${IMG_SIZE} raw disk image at $IMG..."
qemu-img create -f raw "$IMG" "$IMG_SIZE"
mkfs.ext4 -L ghost-root "$IMG"

echo "==> Mounting image at $MNT..."
mkdir -p "$MNT"
mount -o loop "$IMG" "$MNT"

echo "==> Running debootstrap (Debian bookworm)..."
debootstrap --arch=amd64 bookworm "$MNT"

echo "==> Configuring guest system..."

# fstab
cat > "$MNT/etc/fstab" <<'EOF'
/dev/vda    /           ext4    errors=remount-ro   0 1
host        /mnt/host   9p      trans=virtio,version=9p2000.L,msize=104857600   0 0
EOF

# hostname
echo "ghost-vm" > "$MNT/etc/hostname"

# hosts
cat > "$MNT/etc/hosts" <<'EOF'
127.0.0.1   localhost
127.0.1.1   ghost-vm
EOF

# Enable serial console for -nographic QEMU
mkdir -p "$MNT/etc/systemd/system/getty.target.wants"
ln -sf /lib/systemd/system/serial-getty@.service \
    "$MNT/etc/systemd/system/getty.target.wants/serial-getty@ttyS0.service"

# Install base packages and ghOSt build dependencies inside chroot
chroot "$MNT" bash -c "
    apt update
    apt install -y --no-install-recommends \
        systemd systemd-sysv udev \
        openssh-server sudo curl wget git \
        build-essential python3 python-is-python3 \
        libnuma-dev libcap-dev libelf-dev libbfd-dev \
        gcc clang llvm zlib1g-dev \
        iproute2 iputils-ping ifupdown vim less
"

# Set root password to 'ghost' and allow root ssh login
chroot "$MNT" bash -c "echo 'root:ghost' | chpasswd"
sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' \
    "$MNT/etc/ssh/sshd_config" 2>/dev/null || true

# Create /mnt/host mountpoint for 9p shared folder
mkdir -p "$MNT/mnt/host"

# Install Bazelisk as 'bazel'
echo "==> Installing Bazelisk..."
curl -fsSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
    -o "$MNT/usr/local/bin/bazel"
chmod +x "$MNT/usr/local/bin/bazel"

# Network: static IP for QEMU user-mode networking (10.0.2.x)
cat >> "$MNT/etc/network/interfaces" <<'EOF'

auto ens4
iface ens4 inet static
    address 10.0.2.15
    netmask 255.255.255.0
    gateway 10.0.2.2
    dns-nameservers 8.8.8.8
EOF

# DNS
echo "nameserver 8.8.8.8" > "$MNT/etc/resolv.conf"

# Mask some services that don't make sense in a VM
chroot "$MNT" systemctl mask \
    systemd-remount-fs.service \
    dev-hugepages.mount \
    sys-fs-fuse-connections.mount 2>/dev/null || true

echo "==> Unmounting image..."
umount "$MNT"
rmdir "$MNT"

echo ""
echo "Done. Image created at: $IMG"
echo "Boot with: bash $SCRIPT_DIR/run-vm.sh"
