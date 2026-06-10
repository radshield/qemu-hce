#!/bin/bash
set -e -u

# Fault injection smoke test runner.
# This script mirrors what the CI workflow does locally.
#
# Prerequisites:
#   - QEMU built with: ./configure --target-list=riscv64-softmmu --enable-plugins && make -j$(nproc)
#   - SGL kernel Image at $KERNEL
#   - SGL rootfs qcow2 with a "root_shell" snapshot at $ROOTFS
#
# Usage:
#   KERNEL=meta-sgl/build/tmp-glibc/deploy/images/qemuriscv64/Image ROOTFS=qemu-images/core-image-minimal-qemuriscv64.rootfs.qcow2 ./fault-injection/run.sh

: "${KERNEL:?Set KERNEL to the path to the riscv64 kernel Image}"
: "${ROOTFS:?Set ROOTFS to the path to the rootfs qcow2 (with root_shell snapshot)}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
QEMU="$QEMU_DIR/build/qemu-system-riscv64"

if [ ! -x "$QEMU" ]; then
    echo "ERROR: QEMU not found at $QEMU. Build it first."
    exit 1
fi

exec "$QEMU" \
    -M virt -m 512M -nographic \
    -kernel "$KERNEL" \
    -append "root=/dev/vda rw console=ttyS0" \
    -drive "file=$ROOTFS,format=qcow2,id=hd0,if=none" \
    -device virtio-blk-device,drive=hd0 \
    -plugin "$QEMU_DIR/build/contrib/plugins/libcache.so,cores=4,dcachesize=32768,dassoc=8,dblksize=64,icachesize=32768,iassoc=8,iblksize=64,l2cachesize=2097152,l2assoc=32,l2blksize=64" \
    -plugin "$QEMU_DIR/build/contrib/plugins/libfault_injection.so,l1d_flip_chance=1000000000,l1i_flip_chance=1000000000,l2_flip_chance=1000000000,mem_flip_chance=1000000000" \
    -d plugin \
    -loadvm root_shell
