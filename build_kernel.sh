#!/bin/bash

cd kernel-5.15
python scripts/gen_build_config.py --kernel-defconfig a06x_00_defconfig --kernel-defconfig-overlays "entry_level.config" -m user -o ../out/target/product/a06x/obj/KERNEL_OBJ/build.config

export ARCH=arm64
export PLATFORM_VERSION=13
export CROSS_COMPILE="aarch64-linux-gnu-"
export CROSS_COMPILE_COMPAT="arm-linux-gnueabi-"
export OUT_DIR="../out/target/product/a06x/obj/KERNEL_OBJ"
export DIST_DIR="../out/target/product/a06x/obj/KERNEL_OBJ"
export BUILD_CONFIG="../out/target/product/a06x/obj/KERNEL_OBJ/build.config"

cd ../kernel
./build/build.sh