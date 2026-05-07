# 16. Vortex simx 集成总览

## 为什么要接入 Vortex

前面基础实验里的 `hw/gpgpu/gpgpu_core.c` 是一个教学用 SIMT 解释器。它的优点是代码短、容易读，适合理解 GPGPU 设备的基本路径：

```text
写 BAR2 上传 kernel 和数据
  ↓
写 BAR0 dispatch 寄存器
  ↓
QEMU 手写解释器执行 RV32 指令
  ↓
从 BAR2 读回结果
```

但它和真实 GPGPU 还有明显差距：

- 没有 cycle-level 时序模型。
- 没有真实 cache 行为。
- 没有成熟的 warp scheduler。
- divergence、barrier、memory coalescing 等机制非常简化。
- 指令覆盖主要围绕实验 kernel 手写补齐。

Vortex 是一个开源 RISC-V GPGPU 项目。它的 `simx` 是 C++ 写的 cycle-level 模拟器，已经包含更接近真实 GPU 的核心结构，例如 core、warp、scheduler、cache、memory coalescer 和性能计数器。把 Vortex simx 接到 QEMU 后，QEMU 继续负责 PCIe 设备外壳，Vortex 负责真正的 GPGPU 执行后端。

## 本次实验做到了什么

本次进阶实验没有重写整个 GPGPU 设备，而是保留 QEMU 侧已经稳定的 PCI 设备模型：

- `hw/gpgpu/gpgpu.c`：PCI 设备、BAR、MMIO 寄存器、VRAM、MSI-X。
- `hw/gpgpu/gpgpu.h`：寄存器和设备状态。
- `tests/qtest/gpgpu-test.c`：默认 cmodel 测试和新增 Vortex 可选测试。

新增的是一个可选后端：

```text
QEMU GPGPU 前端
  ├─ backend=cmodel  → 原手写解释器 gpgpu_core.c
  └─ backend=vortex  → 外部 libqemu-vortex-simx.so → Vortex simx
```

默认 `-device gpgpu` 仍使用 cmodel，因此原实验评分路径不变。只有显式传入：

```bash
-device gpgpu,backend=vortex,vortex-lib=/tmp/libqemu-vortex-simx.so
```

才会启用 Vortex 后端。

## 端到端执行路径

以新增的 `sgemm-n4` 测试为例，完整路径如下：

```text
Vortex 仓库生成 kernel.vxbin
  ↓
QTest 读取 kernel.vxbin
  ↓
QTest 把 kernel payload、参数块、矩阵 A/B/C 写入 QEMU BAR2
  ↓
QTest 写 BAR0:
  - kernel_addr = 0x80000000
  - kernel_args = 0x80010300
  - grid/block 维度
  - dispatch = 1
  ↓
QEMU gpgpu.c 发现 backend=vortex
  ↓
gpgpu_vortex.c 调用动态库 ABI
  ↓
libqemu-vortex-simx.so 把 QEMU VRAM 上传到 Vortex RAM
  ↓
Vortex simx 执行 sgemm kernel
  ↓
shim 把 Vortex RAM 下载回 QEMU VRAM
  ↓
QTest 从 BAR2 读 C 矩阵并和 CPU 参考结果比较
```

这里最重要的思想是分层：

- QEMU 设备前端不理解 sgemm 算法。
- Vortex simx 不理解 PCI BAR。
- shim 负责把两边的内存和启动参数翻译到同一个模型中。

## 当前验证结果

默认 cmodel 评分测试仍通过：

```text
GPGPU experiment: 17/17 tests passed, score 100/100
```

Vortex 原生 regression sgemm 已通过：

```text
PASSED!
```

QEMU + Vortex simx 的 sgemm 集成测试也已通过：

```text
ok 1 /riscv64/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu-vortex/gpgpu-vortex-tests/sgemm-n4
```

这说明不是只完成了动态库加载，而是已经能让 QEMU 的 PCI 设备前端驱动 Vortex simx 执行真实 Vortex kernel。
