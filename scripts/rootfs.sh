#!/bin/zsh
build="build/rootfs"

cd ..
mkdir -p $build
cd $build
mkdir -p bin dev proc sys tmp etc 
cd ../../
rsync -avr scripts/busybox/_install/* $build/root

dd if=/dev/zero of=$build/rootfs.ext2 bs=1M count=32
mkfs.ext2 -F $build/rootfs.ext2
sudo mount -o loop $build/rootfs.ext2 /mnt
sudo cp -a $build/. /mnt/
sudo umount /mnt
