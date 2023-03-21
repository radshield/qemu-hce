#!/bin/bash
set -e -u

# Run QEMU and load to root shell
qemu-hce/build/qemu-system-arm -s -M virt -m 2G -smp cpus=4 -nographic \
                               -drive file="debian-11-rt/debian.qcow2",id=hd,if=none,media=disk \
                               -device virtio-blk-device,drive=hd \
                               -kernel debian-11-rt/vmlinuz \
                               -initrd debian-11-rt/initrd.img \
                               -append "root=/dev/vda2" \
                               -plugin ./qemu-hce/build/contrib/plugins/libcache.so,cores=4,dcachesize=2048,dassoc=4,dblksize=64,icachesize=2048,iassoc=4,iblksize=64,l2cachesize=2097152,l2assoc=4,l2blksize=64 \
                               -d plugin \
                               -loadvm root_shell
