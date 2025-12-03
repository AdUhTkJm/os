#!/bin/zsh
build=../build/rootfs

dd if=/dev/zero of=$build/rootfs.ext2 bs=5M count=32
mkfs.ext2 -F $build/rootfs.ext2
sudo mount -o loop $build/rootfs.ext2 /mnt
sudo cp -a rootfs/. /mnt/
sudo umount /mnt
