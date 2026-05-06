# 05. 裸机 GPGPU 驱动层

## 概述

GPGPU 驱动层建立在 PCI 驱动之上，封装了对 GPGPU 设备的所有硬件操作：MMIO 寄存器读写、VRAM 数据传输、设备控制和内核调度。这一层直接与 QEMU 中 `hw/gpgpu/gpgpu.c` 的 MMIO handler 对话。

## 设备结构体

```c
typedef struct {
    volatile uint32_t *bar0;   // 控制寄存器基地址（MMIO 映射后的主机虚拟地址）
    volatile uint8_t  *bar2;   // VRAM 基地址（逐字节可访问）
    uint64_t           vram_size;
    uint32_t           num_cus;        // 计算单元数量
    uint32_t           warps_per_cu;   // 每个 CU 的 warp 数量
    uint32_t           warp_size;      // 每个 warp 的线程数（= 32）
} gpgpu_dev_t;
```

`volatile` 关键字至关重要——这些指针指向的是硬件 MMIO 区域而非普通内存，编译器不能优化掉对它们的读写。

## 初始化流程

`gpgpu_drv_init()` 串联了从发现设备到就绪的全部步骤：

```
pci_find_device(0x1234, 0x1337)   ← 扫描 bus 0 找 GPGPU
        ↓
pci_probe_bars(bdf)                ← 探测所有 6 个 BAR 的大小
        ↓
pci_assign_bar(bdf, 0)             ← BAR0 = 0x40000000（控制寄存器, 64KB）
pci_assign_bar(bdf, 2)             ← BAR2 = 0x44000000（VRAM, 64MB）
        ↓
pci_enable_device(bdf)             ← 设置 Memory Space + Bus Master
        ↓
读取 DEV_CAPS 寄存器               ← 解析 num_cus / warps_per_cu / warp_size
读取 VRAM_SIZE 寄存器              ← 获取 VRAM 总容量
```

### 设备能力解析

DEV_CAPS 寄存器（偏移 0x0008）将多个字段打包在一个 32 位字中：

```
bit [7:0]    num_cus        — 计算单元数量（GPGPU 中为 4）
bit [15:8]   warps_per_cu   — 每个 CU 的 warp 数（GPGPU 中为 4）
bit [23:16]  warp_size      — 每个 warp 的线程数（GPGPU 中为 32）
```

```c
uint32_t caps = gpgpu_reg_read(dev, GPGPU_REG_DEV_CAPS);
dev->num_cus      = caps & 0xFF;
dev->warps_per_cu  = (caps >> 8) & 0xFF;
dev->warp_size     = (caps >> 16) & 0xFF;
```

VRAM 大小是一个 64 位值，需要读两个 32 位寄存器：

```c
uint32_t vram_lo = gpgpu_reg_read(dev, GPGPU_REG_VRAM_SIZE_LO);
uint32_t vram_hi = gpgpu_reg_read(dev, GPGPU_REG_VRAM_SIZE_HI);
dev->vram_size = ((uint64_t)vram_hi << 32) | vram_lo;
```

## MMIO 寄存器访问

BAR0 映射了 GPGPU 的控制寄存器空间。寄存器按 4 字节对齐排列，所以偏移量除以 4 就是数组索引：

```c
uint32_t gpgpu_reg_read(gpgpu_dev_t *dev, uint32_t offset)
{
    return dev->bar0[offset / 4];  // 读 32 位 MMIO
}

void gpgpu_reg_write(gpgpu_dev_t *dev, uint32_t offset, uint32_t val)
{
    dev->bar0[offset / 4] = val;  // 写 32 位 MMIO
}
```

每个寄存器写入都会触发 QEMU 中 `gpgpu_ctrl_write()` 的对应 `case` 分支。

## VRAM 访问

BAR2 映射了整个 VRAM 空间（64MB）。由于 BAR2 声明为 `volatile uint8_t *`，可以按字节寻址：

```c
void gpgpu_vram_write(gpgpu_dev_t *dev, uint32_t offset,
                      const void *src, size_t n)
{
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        dev->bar2[offset + i] = s[i];   // 逐字节写入 VRAM
    }
}

void gpgpu_vram_read(gpgpu_dev_t *dev, uint32_t offset,
                     void *dst, size_t n)
{
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = dev->bar2[offset + i];   // 逐字节读取 VRAM
    }
}
```

每个字节写入对应一次 MMIO 访问，由 QEMU 的 MemoryRegionOps 处理。虽然效率不高（没有用 DMA 或 burst 传输），但对于 demo 规模的数据量完全够用。

## 设备控制

### 使能与复位

```c
void gpgpu_enable(gpgpu_dev_t *dev)
{
    gpgpu_reg_write(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);  // bit 0
}

void gpgpu_reset(gpgpu_dev_t *dev)
{
    gpgpu_reg_write(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);   // bit 1
}
```

写入 RESET 位会触发 QEMU 中的软复位逻辑，清除所有状态（包括 SIMT 引擎和 VRAM 内容）。

### 状态查询

```c
uint32_t gpgpu_get_status(gpgpu_dev_t *dev)
{
    return gpgpu_reg_read(dev, GPGPU_REG_GLOBAL_STATUS);
    // bit 0: READY  — 设备空闲，可以接受新 kernel
    // bit 1: BUSY   — kernel 正在执行
    // bit 2: ERROR  — 执行出错
}
```

## 内核调度（Dispatch）

`gpgpu_dispatch()` 是驱动层最核心的函数，它将内核执行所需的所有信息写入设备寄存器，然后触发执行：

```c
void gpgpu_dispatch(gpgpu_dev_t *dev, uint32_t kernel_addr,
                    uint32_t args_addr, uint32_t grid[3], uint32_t block[3])
{
    // 1. 内核代码在 VRAM 中的起始地址
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ADDR_LO, kernel_addr);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ADDR_HI, 0);

    // 2. 内核参数在 VRAM 中的地址
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ARGS_LO, args_addr);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ARGS_HI, 0);

    // 3. Grid 维度（有多少个 Block）
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_X, grid[0]);
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_Y, grid[1]);
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_Z, grid[2]);

    // 4. Block 维度（每个 Block 有多少个线程）
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_X, block[0]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_Y, block[1]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_Z, block[2]);

    // 5. 触发执行！
    gpgpu_reg_write(dev, GPGPU_REG_DISPATCH, 1);
}
```

### 寄存器与 QEMU 侧的对应

| 驱动写入 | QEMU 侧处理 |
|----------|-------------|
| `KERNEL_ADDR_LO` | 存入 `s->kernel_addr` |
| `KERNEL_ARGS_LO` | 存入 `s->kernel_args` |
| `GRID_DIM_X/Y/Z` | 存入 `s->grid_dim[0/1/2]` |
| `BLOCK_DIM_X/Y/Z` | 存入 `s->block_dim[0/1/2]` |
| `DISPATCH` | 设 BUSY → 调用 `gpgpu_core_exec_kernel()` → 设 READY → 触发中断 |

### 同步执行模型

当前 GPGPU 设备采用**同步执行**模型：写入 DISPATCH 寄存器后，QEMU 在 MMIO 回调函数内立即执行完整的 kernel，然后才返回。这意味着当 `gpgpu_reg_write(DISPATCH)` 返回时，kernel 已经执行完毕，STATUS 已经回到 READY。

这简化了驱动设计——不需要轮询状态或等待中断，但代价是 kernel 执行期间 CPU 被阻塞。

## 关键寄存器地址一览

```
偏移      名称                 读/写    说明
────────────────────────────────────────────────────
0x0000    DEV_ID               R       设备标识 "GPGPU"
0x0004    DEV_VERSION          R       版本号
0x0008    DEV_CAPS             R       能力字段（CU/warp/线程数）
0x000C    VRAM_SIZE_LO         R       VRAM 大小低 32 位
0x0010    VRAM_SIZE_HI         R       VRAM 大小高 32 位
0x0100    GLOBAL_CTRL          W       控制（Enable/Reset）
0x0104    GLOBAL_STATUS        R       状态（Ready/Busy/Error）
0x0300    KERNEL_ADDR_LO       W       内核代码地址
0x0308    KERNEL_ARGS_LO       W       内核参数地址
0x0310    GRID_DIM_X           W       Grid X 维度
0x031C    BLOCK_DIM_X          W       Block X 维度
0x0330    DISPATCH             W       触发执行（写 1）
```
