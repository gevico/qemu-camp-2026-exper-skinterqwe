# 00. 实验总览

## 实验目标

本实验的目标是在 QEMU 中实现一个教学用 GPGPU PCI 设备，并围绕该设备构建两套软件栈：

- 裸机软件栈：在 `g233` 板上通过手写 PCI 枚举访问 GPGPU。
- Linux 软件栈：在 `virt` 板上通过 Linux PCI 子系统、内核模块和用户态库访问 GPGPU。

最终希望展示一个完整的分层系统：

```text
用户程序
  ↓
libgpgpu runtime
  ↓
裸机驱动或 Linux /dev/gpgpu
  ↓
PCI BAR0 控制寄存器 + BAR2 VRAM
  ↓
QEMU GPGPU 设备模型
  ↓
SIMT RV32I/RV32F kernel 解释执行
```

## 最终成果

### QEMU 设备层

核心文件：

- `hw/gpgpu/gpgpu.c`
- `hw/gpgpu/gpgpu_core.c`
- `hw/gpgpu/gpgpu.h`
- `hw/gpgpu/gpgpu_core.h`

已实现能力：

- PCI 设备注册，vendor/device 为 `0x1234:0x1337`。
- BAR0 控制寄存器，大小 1MB。
- BAR2 VRAM，大小 64MB。
- BAR4 doorbell 区域，当前主要作为预留。
- MMIO 寄存器读写。
- VRAM 读写。
- kernel dispatch。
- RV32I/RV32F 指令解释器。
- 低精度浮点转换相关指令基础支持。

### 裸机软件栈

核心目录：

```text
tests/gevico/firmware/gpgpu/
├── common/
├── driver/
├── runtime/
├── kernels/
└── demo/
```

已验证：

```text
=== Vector Add ===
  PASS (N=64)
=== ReLU ===
  PASS (N=32)
=== MatMul ===
  PASS (4x4 @ 4x4)
```

### Linux 软件栈

核心目录：

```text
tests/gevico/firmware/gpgpu/linux/
├── gpgpu_kmod/
├── libgpgpu/
├── demo/
├── init_loader.c
└── Makefile
```

已验证：

- `gpgpu.ko` 可构建为 RISC-V 内核模块。
- Linux 启动时 PCI 子系统能识别设备：

```text
pci 0000:00:01.0: [1234:1337] type 00 class 0x030200
```

- 模块 probe 成功：

```text
gpgpu 0000:00:01.0: GPGPU ready: 4 CUs, 4 warps/CU, 32 threads/warp, 67108864 bytes VRAM
```

- 用户态 demo 全部 PASS：

```text
GPGPU Linux Demo
Device: 4 CUs, 4 warps/CU, 32 threads/warp, 64MB VRAM
=== Vector Add ===
  PASS (N=64)
=== ReLU ===
  PASS (N=32)
=== MatMul ===
  PASS (4x4 @ 4x4)
All demos completed.
```

## 核心地址空间

GPGPU 的地址空间分为 PCI BAR 和设备内部 VRAM 偏移两层。

PCI BAR：

| BAR | 用途 | 大小 |
| --- | --- | --- |
| BAR0 | 控制寄存器 MMIO | 1MB |
| BAR2 | VRAM | 64MB |
| BAR4 | doorbell 预留 | 64KB |

VRAM 内部布局：

```text
0x0000-0x0fff  kernel code
0x1000-0x1fff  kernel args
0x2000+        runtime data allocation
```

这个布局对裸机 runtime 和 Linux runtime 是一致的，因此同一份 kernel 机器码和 demo 逻辑可以复用。

## 端到端执行流程

以 `vector_add` 为例：

1. demo 在 host 内存中准备数组 `A/B/C`。
2. `gpgpuMalloc()` 返回 VRAM 偏移。
3. `gpgpuMemcpyH2D()` 把输入数组写入 BAR2 VRAM。
4. `gpgpuLaunchKernel()` 把 kernel 机器码写入 VRAM `0x0000`。
5. runtime 把参数数组写入 VRAM `0x1000`。
6. runtime 或内核模块写 BAR0 dispatch 寄存器。
7. QEMU 设备模型进入 `gpgpu_core_exec_kernel()`。
8. 每个 block/warp/lane 执行 RV32 指令。
9. kernel 用 VRAM 偏移访问输入输出。
10. demo 通过 `gpgpuMemcpyD2H()` 读回结果并校验。

## 为什么分裸机和 Linux 两套栈

裸机栈更适合说明硬件访问的底层机制：

- PCI 配置空间如何扫描。
- BAR 如何探测大小和分配地址。
- MMIO 寄存器如何读写。
- 没有 OS 时如何组织 runtime。

Linux 栈更接近真实驱动模型：

- PCI 资源由内核统一分配。
- 驱动通过 `pci_iomap()` 映射 BAR0。
- 用户态不能直接写 MMIO 控制寄存器，需要 ioctl。
- 大块显存访问适合通过 `mmap()` 映射 BAR2。
- 设备文件 `/dev/gpgpu` 是用户态和内核态之间的边界。

两套栈复用同一组 kernel 机器码，这验证了抽象边界是合理的：kernel 不关心运行在裸机还是 Linux，只关心 VRAM 布局和 dispatch 参数。
