# 20. Vortex 集成学习路线与后续改进

## 初学者建议阅读顺序

如果刚开始接触这个实验，建议按下面顺序学习：

1. 先读 [00. 实验总览](00-overview.md)。
2. 再读 [01. QEMU GPGPU 设备模型](01-qemu-device-model.md)。
3. 读 [02. SIMT Core 与 Kernel 机器码](02-simt-core-and-kernels.md)，理解基础 cmodel。
4. 读 [16. Vortex simx 集成总览](16-vortex-simx-overview.md)，理解为什么要替换后端。
5. 读 [17. QEMU 侧后端切换机制](17-qemu-vortex-backend.md)，理解 QEMU 如何选择 cmodel 或 Vortex。
6. 读 [18. Vortex shim ABI 与内存模型](18-vortex-shim-abi-and-memory.md)，理解 C/C++ 边界和地址映射。
7. 最后读 [19. Vortex sgemm 验证流程](19-vortex-sgemm-validation.md)，照着命令跑 workload。

## 必须掌握的核心概念

### PCI BAR

BAR 是 PCI 设备暴露给 guest 的 MMIO 或 memory window。本实验里：

```text
BAR0: 控制寄存器
BAR2: VRAM
BAR4: doorbell 预留
```

guest 不直接调用 QEMU 函数，而是通过读写 BAR 来驱动设备。

### MMIO 寄存器

BAR0 里的寄存器定义了设备控制面，例如：

```text
GLOBAL_CTRL
GLOBAL_STATUS
KERNEL_ADDR
KERNEL_ARGS
GRID_DIM
BLOCK_DIM
DISPATCH
ERROR_STATUS
```

写 `DISPATCH` 是启动 kernel 的关键动作。

### Device address 与 BAR offset

QEMU BAR2 使用 offset，Vortex kernel 使用 device address。两者通过 `vortex-vram-base` 对齐：

```text
device address = vortex-vram-base + BAR2 offset
```

本次实验用：

```text
vortex-vram-base = 0x80000000
```

所以 BAR2 offset 0 对应 Vortex 地址 `0x80000000`。

### C ABI

C ABI 是 C 和 C++ 之间稳定交互的边界。QEMU 不直接链接 C++ 类，而是调用：

```text
vortex_qemu_create()
vortex_qemu_dispatch()
vortex_qemu_destroy()
```

这样 QEMU 主体不需要理解 Vortex 内部 C++ 类布局。

### bring-up shim

当前 shim 是为了先跑通功能，不是最终高性能实现。它的核心策略是：

```text
QEMU VRAM 全量复制到 Vortex RAM
执行
Vortex RAM 全量复制回 QEMU VRAM
```

优点是简单可靠；缺点是性能差，且不是真正的共享内存模型。

## 常见问题

### 为什么默认测试还是 17/17 cmodel

因为 `backend=cmodel` 是默认值。Vortex 后端依赖外部仓库、动态库和工具链，不适合强行加入默认评分路径。默认测试稳定，Vortex 测试通过环境变量显式开启。

### 为什么要先跑 Vortex 原生 sgemm

如果原生 Vortex simx 都跑不通，QEMU 集成失败时很难判断问题在 QEMU、shim、工具链还是 Vortex 本身。先跑原生 sgemm 可以把问题范围缩小。

### 为什么 sgemm 测试只用 4x4

这是集成测试，不是性能测试。4x4 足够覆盖 kernel 启动、参数传递、全局内存读写和浮点计算，同时运行时间短，适合放在 QTest 中。

### 为什么没有直接跑 OpenCL sgemm

当前已经跑通 Vortex regression sgemm。OpenCL 路径还需要继续对齐 Vortex POCL/OpenCL runtime 和本实验的 `libgpgpu`/驱动 ABI，涉及更多用户态运行时和编译链问题，适合作为下一阶段。

## 后续改进方向

### 1. 零拷贝内存后端

当前全量复制 BAR2。后续可以把 QEMU BAR2 backing memory 封装成 Vortex simx 的 memory backend，让 Vortex 直接访问 QEMU VRAM。

目标：

```text
减少 upload/download
保留 cache/memory 行为
更接近真实设备 DMA 模型
```

### 2. 异步 dispatch

当前 QEMU 写 `DISPATCH` 后同步等待 simx 执行结束。真实设备通常异步执行：

```text
guest 写 dispatch
QEMU 立即返回
simx 后台执行
完成后触发 MSI-X
```

这需要把 Vortex 执行放入 worker 线程，并仔细处理 QEMU BQL、设备状态和中断时序。

### 3. 更完整的 Vortex runtime 对齐

当前 QTest 手动解析 `kernel.vxbin` 并写 BAR2。后续可以让 guest 侧 runtime 更接近 Vortex 原生 API：

```text
vx_mem_alloc
vx_copy_to_dev
vx_upload_kernel_file
vx_start
vx_ready_wait
```

这样更容易迁移 Vortex regression 和 OpenCL 示例。

### 4. OpenCL/POCL 栈

最终目标是让 Vortex OpenCL 示例通过本实验设备运行，例如：

```text
tests/opencl/sgemm
tests/opencl/vecadd
```

这一步需要处理：

- OpenCL kernel 编译产物。
- POCL runtime 如何发现设备。
- 用户态 runtime 与内核驱动的 ABI。
- buffer allocation、kernel launch、同步和错误码。

### 5. 在真实 RISC-V OS 上验证

实验目标 OS 包括 ArceOS 和 rCore。后续验证重点是：

- PCI 设备发现。
- BAR 映射。
- MSI-X 或中断完成路径。
- VRAM 映射和 cache 属性。
- 用户态或内核态 runtime 能否稳定 dispatch Vortex kernel。

## 当前阶段结论

当前实现已经完成了进阶实验的关键闭环：

```text
QEMU PCI GPGPU 前端
  ↓
动态加载 Vortex simx shim
  ↓
通过 C ABI dispatch
  ↓
Vortex simx 执行真实 sgemm kernel
  ↓
QEMU BAR2 读回正确结果
```

这说明当前工作已经从“模拟一个简化 GPGPU”推进到了“用 QEMU 设备前端驱动外部 cycle-level GPGPU 模拟器”的阶段。
