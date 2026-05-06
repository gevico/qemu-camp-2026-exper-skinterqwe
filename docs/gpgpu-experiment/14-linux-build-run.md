# 14. Linux 构建与运行

## 用户态构建

如果系统交叉工具链完整：

```bash
make -C tests/gevico/firmware/gpgpu/linux user
```

如果缺 RISC-V libc headers，使用本地 sysroot：

```bash
make -C tests/gevico/firmware/gpgpu/linux user \
    SYSROOT=$PWD/tests/gevico/firmware/gpgpu/linux/sysroot
```

本地 sysroot 可通过无 root 方式准备：

```bash
apt-get download \
    libc6-riscv64-cross \
    libc6-dev-riscv64-cross \
    linux-libc-dev-riscv64-cross

mkdir -p tests/gevico/firmware/gpgpu/linux/sysroot
dpkg-deb -x libc6-riscv64-cross_*_all.deb tests/gevico/firmware/gpgpu/linux/sysroot
dpkg-deb -x libc6-dev-riscv64-cross_*_all.deb tests/gevico/firmware/gpgpu/linux/sysroot
dpkg-deb -x linux-libc-dev-riscv64-cross_*_all.deb tests/gevico/firmware/gpgpu/linux/sysroot
```

验证：

```bash
file tests/gevico/firmware/gpgpu/linux/demo/build/gpgpu_linux_demo
```

期望：

```text
ELF 64-bit LSB executable, UCB RISC-V, statically linked
```

## 内核模块构建

目标内核：

```text
Linux 6.6.75
```

准备源码：

```bash
tar -C /tmp/linux-6.6.75-gpgpu --strip-components=1 \
    -xf /tmp/linux-6.6.75.tar.xz
```

提取目标 `Image` 的真实配置：

```bash
/tmp/linux-6.6.75-gpgpu/scripts/extract-ikconfig \
    tests/gevico/firmware/Image > /tmp/linux-6.6.75-gpgpu/.config
```

准备外部模块构建环境：

```bash
make -C /tmp/linux-6.6.75-gpgpu \
    ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- \
    olddefconfig modules_prepare
```

构建模块：

```bash
make -C tests/gevico/firmware/gpgpu/linux/gpgpu_kmod \
    KERNEL_SRC=/tmp/linux-6.6.75-gpgpu \
    KBUILD_MODPOST_WARN=1
```

为什么需要 `KBUILD_MODPOST_WARN=1`：

- `modules_prepare` 不生成完整 `Module.symvers`。
- 该内核配置没有开启 `CONFIG_MODVERSIONS`。
- 没有模块签名要求。
- 因此可以把缺 `Module.symvers` 导致的 unresolved symbol 从 error 降为 warning，仍然生成 `.ko`。

验证：

```bash
file tests/gevico/firmware/gpgpu/linux/gpgpu_kmod/gpgpu.ko
strings tests/gevico/firmware/gpgpu/linux/gpgpu_kmod/gpgpu.ko | grep vermagic
```

期望：

```text
ELF 64-bit LSB relocatable, UCB RISC-V
vermagic=6.6.75 SMP mod_unload riscv
```

## rootfs 打包

需要 host cpio。如果系统没安装，可无 root 下载：

```bash
apt-get download cpio
mkdir -p /tmp/gpgpu_tools
dpkg-deb -x cpio_*_amd64.deb /tmp/gpgpu_tools
```

打包：

```bash
make -C tests/gevico/firmware/gpgpu/linux rootfs \
    SYSROOT=$PWD/tests/gevico/firmware/gpgpu/linux/sysroot \
    CPIO=/tmp/gpgpu_tools/usr/bin/cpio
```

产物：

```text
tests/gevico/firmware/gpgpu/linux/build/gpgpu_linux.cpio.gz
```

## Linux 端到端运行

```bash
timeout 60s ./build/qemu-system-riscv64 \
    -M virt \
    -m 2G \
    -display none \
    -serial mon:stdio \
    -kernel tests/gevico/firmware/Image \
    -initrd tests/gevico/firmware/gpgpu/linux/build/gpgpu_linux.cpio.gz \
    -append "console=ttyS0" \
    -device gpgpu
```

关键成功日志：

```text
pci 0000:00:01.0: [1234:1337] type 00 class 0x030200
gpgpu 0000:00:01.0: GPGPU ready: 4 CUs, 4 warps/CU, 32 threads/warp, 67108864 bytes VRAM
GPGPU Linux Demo
=== Vector Add ===
  PASS (N=64)
=== ReLU ===
  PASS (N=32)
=== MatMul ===
  PASS (4x4 @ 4x4)
All demos completed.
reboot: Power down
```
