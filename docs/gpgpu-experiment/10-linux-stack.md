# 10. Linux 软件栈

## Linux 软件栈结构

目录：

```text
tests/gevico/firmware/gpgpu/linux/
├── gpgpu_kmod/
├── libgpgpu/
├── demo/
├── init_loader.c
└── Makefile
```

分层：

```text
demo
  ↓
libgpgpu
  ↓
/dev/gpgpu
  ↓
gpgpu.ko
  ↓
Linux PCI subsystem
  ↓
QEMU GPGPU PCI device
```

Linux 路径的详细专题：

- [Linux 内核模块](11-linux-kernel-module.md)
- [Linux 用户态 Runtime](12-linux-userspace-runtime.md)
- [Linux initramfs 集成](13-linux-initramfs.md)
- [Linux 构建与运行](14-linux-build-run.md)

## 为什么 Linux 版使用 virt 板

裸机 demo 使用：

```text
-M g233
```

Linux demo 使用：

```text
-M virt
```

原因：

- `virt` 板有标准 PCI host bridge。
- Linux 对 `virt` 板支持成熟。
- GPGPU 可以作为普通 PCI 设备挂到 virt PCI bus。
- PCI BAR 分配由 Linux 内核完成。

QEMU 启动时可以看到：

```text
pci-host-generic 30000000.pci: PCI host bridge to bus 0000:00
pci 0000:00:01.0: [1234:1337] type 00 class 0x030200
```

## Linux 端到端路径

用户态 demo 不直接访问 PCI 配置空间，也不直接写控制寄存器。它只通过 `libgpgpu` 调用统一 API：

```text
gpgpuInit()
gpgpuMalloc()
gpgpuMemcpyH2D()
gpgpuLaunchKernel()
gpgpuMemcpyD2H()
```

Linux 侧把控制平面和数据平面拆开：

- 控制平面：`ioctl()` 进入 `gpgpu.ko`，由内核模块写 BAR0 寄存器并触发 dispatch。
- 数据平面：`mmap()` 把 BAR2 VRAM 映射到用户态，H2D/D2H 用普通 `memcpy()`。

这个设计保持了与裸机 runtime 相同的 API，同时符合 Linux 驱动模型。

## Linux 栈的核心知识点

1. Linux PCI 子系统负责枚举和 BAR 分配，驱动不要硬编码 BAR 地址。
2. 内核态访问 MMIO 必须用 `ioread32/iowrite32`。
3. 用户态不能直接写控制寄存器，控制路径应走 ioctl。
4. 大块 VRAM 数据传输适合 mmap，避免每次 copy 进入内核。
5. initramfs 环境可能没有 shell，要根据实际 rootfs 设计 init。
6. 外部模块构建必须匹配目标内核版本和配置。
