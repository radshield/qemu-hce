#!/bin/bash
set -e -u
# use this command: set {int}0x800000 = N
# -s kernel

# Run QEMU and load to root shell
qemu-hce/build/qemu-system-arm -s -M virt -m 1G -nographic \
                               -drive file="debian-11-rt/debian.qcow2",id=hd,if=none,media=disk \
                               -device virtio-blk-device,drive=hd \
                               -kernel debian-11-rt/vmlinuz -initrd debian-11-rt/initrd.img \
                               -append "root=/dev/vda2" \
                               -plugin ./qemu-hce/build/contrib/plugins/libcache.so -d plugin \
                               -loadvm root_shell

HERE="$(dirname "$0")"
gdb-multiarch -ex 'target remote :1234' \
              -ex 'maintenance packet Qqemu.PhyMemMode:1' \
              -ex 'set pagination off' \
              -ex "source $HERE/ctrl.py""" \
              -ex 'log_inject ../injections.csv' \
              "$@"
