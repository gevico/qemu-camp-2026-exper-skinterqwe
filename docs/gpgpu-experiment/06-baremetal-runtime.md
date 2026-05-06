# 06. 裸机运行时库

## 概述

`libgpgpu` 提供了一套类 CUDA 风格的 API，让 Demo 程序不需要直接操作 MMIO 寄存器和 VRAM 偏移量。它是驱动层之上的抽象层，将硬件细节隐藏在简洁的接口后面。

## API 设计

```c
// 设备管理
int  gpgpuInit(void);                           // 初始化，返回 0 成功
void gpgpuGetDeviceInfo(gpgpuDeviceInfo *info);  // 查询设备能力

// 内存管理（返回 VRAM 偏移量，不是主机指针）
uint32_t gpgpuMalloc(size_t size);
void gpgpuMemcpyH2D(uint32_t dst_vram, const void *src_host, size_t n);
void gpgpuMemcpyD2H(void *dst_host, uint32_t src_vram, size_t n);
void gpgpuFree(uint32_t ptr);                   // 当前为空操作

// 内核启动
void gpgpuLaunchKernel(const uint32_t *code, size_t code_words,
                       dim3 grid, dim3 block,
                       uint32_t *args, int num_args);
void gpgpuDeviceSynchronize(void);              // 当前为空（同步执行）
```

## 设备初始化

```c
static gpgpu_dev_t g_dev;       // 全局设备句柄
static uint32_t g_vram_next;    // bump allocator 的水位线
static int g_initialized = 0;   // 防止重复初始化

int gpgpuInit(void)
{
    if (g_initialized) return 0;
    if (gpgpu_drv_init(&g_dev) != 0) return -1;  // PCI 枚举 + BAR 映射
    gpgpu_enable(&g_dev);                          // 使能设备
    g_vram_next = GPGPU_DATA_BASE;                 // 0x2000
    g_initialized = 1;
    return 0;
}
```

初始化顺序：PCI 枚举 → BAR 分配 → 设备使能 → 初始化分配器。完成后 `g_dev` 包含了设备的所有状态信息。

## VRAM 内存管理

### Bump Allocator

GPGPU 的 VRAM 管理采用最简单的 bump allocator（线性推进分配器）：

```c
uint32_t gpgpuMalloc(size_t size)
{
    size = (size + 3) & ~3;     // 4 字节对齐
    uint32_t addr = g_vram_next;
    g_vram_next += size;
    return addr;                 // 返回 VRAM 偏移量
}
```

**设计考量**：

- `gpgpuMalloc` 返回的是 VRAM 中的偏移量（`uint32_t`），不是主机虚拟地址。这是因为 GPU 内核只能通过 VRAM 偏移量访问数据
- 4 字节对齐保证了 GPU 访问的自然对齐（`lw`/`flw` 指令要求 4 字节对齐）
- 从 `0x2000`（`GPGPU_DATA_BASE`）开始分配，避免覆盖 `0x0000-0x0FFF` 的内核代码区和 `0x1000-0x1FFF` 的参数区
- 当前实现没有 `gpgpuFree`，`gpgpuFree` 为空操作。在 demo 场景下这不是问题

### VRAM 布局约束

```
0x0000 ┌────────────────────┐
       │  内核代码 (≤4KB)    │  gpgpuLaunchKernel 写入
0x1000 ├────────────────────┤
       │  内核参数 (16B×n)   │  gpgpuLaunchKernel 写入
0x2000 ├────────────────────┤
       │  数据区...          │  gpgpuMalloc 从这里分配
       │  dA, dB, dC, ...   │
       └────────────────────┘
```

## 数据传输

### Host → Device

```c
void gpgpuMemcpyH2D(uint32_t dst_vram, const void *src_host, size_t n)
{
    gpgpu_vram_write(&g_dev, dst_vram, src_host, n);
}
```

底层通过 BAR2 的 MMIO 映射，逐字节将主机内存的数据写入 VRAM。

### Device → Host

```c
void gpgpuMemcpyD2H(void *dst_host, uint32_t src_vram, size_t n)
{
    gpgpu_vram_read(&g_dev, src_vram, dst_host, n);
}
```

从 VRAM 逐字节读取数据到主机内存。

## 内核启动

`gpgpuLaunchKernel()` 是运行时库最核心的函数，串联了内核代码上传、参数传递和调度执行：

```c
void gpgpuLaunchKernel(const uint32_t *code, size_t code_words,
                       dim3 grid, dim3 block,
                       uint32_t *args, int num_args)
{
    // 步骤 1：将 kernel 机器码写入 VRAM 的代码区
    gpgpu_vram_write(&g_dev, GPGPU_KERNEL_CODE_BASE, code,
                     code_words * sizeof(uint32_t));

    // 步骤 2：将参数写入 VRAM 的参数区
    uint32_t args_vram = GPGPU_KERNEL_ARGS_BASE;   // 0x1000
    gpgpu_vram_write(&g_dev, args_vram, args,
                     num_args * sizeof(uint32_t));

    // 步骤 3：配置 grid/block 维度，触发 dispatch
    uint32_t g[3] = { grid.x, grid.y, grid.z };
    uint32_t b[3] = { block.x, block.y, block.z };
    gpgpu_dispatch(&g_dev, GPGPU_KERNEL_CODE_BASE, args_vram, g, b);
}
```

### 参数传递机制

参数通过 VRAM 中的固定地址 `0x1000` 传递给 GPU 内核。GPU 内核通过以下方式读取参数：

```
主机侧：                     GPU 侧：
args[0] = dA                 lw x10, 0(x3)    // x3 = 0x1000, x10 = dA
args[1] = dB                 lw x11, 4(x3)    // x11 = dB
args[2] = dC                 lw x12, 8(x3)    // x12 = dC
args[3] = N                  lw x13, 12(x3)   // x13 = N
args[4] = blockDim           lw x14, 16(x3)   // x14 = blockDim
```

GPU 内核中通过 `lui x3, 1` 将 x3 设为 `0x1000`，然后用 `lw` 从 VRAM 加载参数。

### Grid/Block 模型

与 CUDA 类似：

- **Grid**：整个 kernel 的线程组织。`grid.x * grid.y * grid.z` 个 Block
- **Block**：一个线程块。`block.x * block.y * block.z` 个线程
- **线程总数** = Grid 中的 Block 数 × 每个 Block 的线程数

以 vector_add(N=64) 为例：
```
grid  = { (64+31)/32, 1, 1 } = { 2, 1, 1 }   ← 2 个 Block
block = { 32, 1, 1 }                            ← 每个 Block 32 个线程
总线程数 = 2 × 32 = 64                           ← 每个元素一个线程
```

## dim3 类型

```c
typedef struct { uint32_t x, y, z; } dim3;
```

与 CUDA 的 `dim3` 对应，支持 3D 组织。本 demo 只用 1D（y=z=1），但 API 设计为 3D 以保持通用性。

## gpgpuDeviceSynchronize

```c
void gpgpuDeviceSynchronize(void)
{
    /* 同步执行模型下，kernel 已在 dispatch 中完成 */
}
```

由于 QEMU 设备采用同步执行模型，kernel 在 `gpgpu_dispatch()` 写入 DISPATCH 寄存器时就已完成，所以 `gpgpuDeviceSynchronize()` 是空操作。在真实 GPU 或异步模型中，这里应该轮询状态寄存器或等待中断。
