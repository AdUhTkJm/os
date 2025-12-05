#!/bin/zsh
while [[ $# -ne 0 ]]; do
  case "$1" in
    -s|--static) static=1; shift 1;;
    *) echo "unknown option: $1"; exit 1;;
  esac
done

build=../build/rootfs

dd if=/dev/zero of=$build/rootfs.ext2 bs=5M count=32
mkfs.ext2 -F $build/rootfs.ext2
sudo mount -o loop $build/rootfs.ext2 ../mnt
sudo cp -a rootfs/. ../mnt/
if [[ -n $static ]]; then
  cp ../temp/busybox-riscv64-linux-gnu ../mnt/bin/busybox
fi
sudo umount ../mnt
