#!/bin/bash
set -e -u

# Run QEMU and load to root shell
qemu-hce/build/qemu-system-arm -s -M virt -m 2G -nographic \
                               -drive file="debian-11-rt/debian.qcow2",id=hd,if=none,media=disk \
                               -device virtio-blk-device,drive=hd \
                               -kernel debian-11-rt/vmlinuz -initrd debian-11-rt/initrd.img \
                               -append "root=/dev/vda2" \
                               -plugin ./qemu-hce/build/contrib/plugins/libcache.so -d plugin \
                               -loadvm root_shell
