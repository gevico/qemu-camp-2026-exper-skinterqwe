# 04. 裸机 PCI 驱动层

## 概述

PCI 驱动层负责在裸机环境中发现 GPGPU 设备、探测其 BAR（Base Address Register）大小、分配 MMIO 地址并使能设备。这一层是所有 PCI 设备驱动的基础，也是裸机环境下最容易踩坑的地方。

## ECAM（Enhanced Configuration Access Mechanism）

### 原理

在有 BIOS/UEFI 的系统中，PCI 配置空间由固件预配置。但在 QEMU 裸机环境下，QEMU 的 PCIe ECAM 控制器提供了一种通过内存映射直接访问 PCI 配置空间的机制。

ECAM 将 PCI 的 `Bus:Device:Function` + `Register Offset` 编码为一个线性物理地址：

```
ECAM 地址 = ECAM_BASE | (bus << 20) | (dev << 15) | (func << 12) | offset
```

QEMU g233 板子的 ECAM 基地址为 `0x30000000`，覆盖 256MB 空间。

### 实现

```c
// driver/pci.c
static inline volatile uint32_t *ecam_addr(pci_bdf_t bdf, uint8_t offset)
{
    uint64_t addr = PCI_ECAM_BASE                     // 0x30000000
                  | ((uint64_t)bdf.bus << 20)          // bus 0~255
                  | ((uint64_t)bdf.dev << 15)          // device 0~31
                  | ((uint64_t)bdf.func << 12)         // function 0~7
                  | offset;                             // 寄存器偏移
    return (volatile uint32_t *)(uintptr_t)addr;
}
```

将计算出的地址强转为 `volatile uint32_t *` 指针后，直接读写即可操作 PCI 配置空间。

### 16 位读写

PCI 配置空间的 Vendor ID 和 Device ID 都是 16 位字段。由于 ECAM 只支持 32 位对齐访问，16 位读写需要手动处理对齐：

```c
uint16_t pci_cfg_read16(pci_bdf_t bdf, uint8_t offset)
{
    uint32_t val = pci_cfg_read32(bdf, offset);  // 读 32 位（自动对齐到 4 字节边界）
    return (offset & 2) ? (uint16_t)(val >> 16)  // 偏移量 bit1=1：取高 16 位
                        : (uint16_t)val;          // 偏移量 bit1=0：取低 16 位
}
```

## PCI 设备枚举

### 扫描策略

GPGPU 设备位于 bus 0，因此只扫描 bus 0 的 32 个 slot。对于每个 slot，读取 Vendor ID：如果为 `0xFFFF` 表示该位置无设备，跳过。

```c
int pci_find_device(uint16_t vendor, uint16_t device, pci_bdf_t *out)
{
    pci_bdf_t bdf;
    for (bdf.bus = 0; bdf.bus < 1; bdf.bus++) {       // 只扫描 bus 0
        for (bdf.dev = 0; bdf.dev < 32; bdf.dev++) {   // 32 个设备槽
            bdf.func = 0;                               // 只看 function 0
            uint16_t vid = pci_cfg_read16(bdf, PCI_CFG_VENDOR_ID);
            if (vid == 0xFFFF) continue;                // 无设备
            uint16_t did = pci_cfg_read16(bdf, PCI_CFG_DEVICE_ID);
            if (vid == vendor && did == device) {
                *out = bdf;
                return 0;
            }
        }
    }
    return -1;
}
```

GPGPU 设备的 Vendor ID = `0x1234`，Device ID = `0x1337`。

### 设备使能

找到设备后，需要设置 PCI Command 寄存器的两个关键位：

```c
void pci_enable_device(pci_bdf_t bdf)
{
    uint16_t cmd = pci_cfg_read16(bdf, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEMORY      // bit 1: Memory Space Enable — 允许访问 BAR 映射的 MMIO 区域
         | PCI_CMD_BUS_MASTER;  // bit 2: Bus Master Enable — 允许设备发起 DMA（本例中用于 VRAM 访问）
    pci_cfg_write16(bdf, PCI_CFG_COMMAND, cmd);
}
```

## BAR 探测与地址分配

### 为什么需要手动处理？

这是裸机环境下最关键的一步。在有 BIOS/UEFI 的系统中，固件在启动时已经为所有 PCI 设备的 BAR 分配好了地址。但在裸机环境下，BAR 寄存器的值是 **0**（或未定义），意味着我们无法直接读取 BAR 来获得设备的 MMIO 地址。

### BAR 探测原理

PCI 规范定义了探测 BAR 大小的标准方法：

1. **保存** BAR 当前值
2. **写入** `0xFFFFFFFF` 到 BAR
3. **读回** — 设备只响应它实际实现的地址位，其余位被硬件强制为 0
4. **恢复** BAR 原始值

从读回值可以推算出 BAR 的大小：

```
写入 0xFFFFFFFF → 设备读回 0xFFF00000（20 个可写 bit）
→ 大小 = ~(0xFFF00000 & 0xFFFFFFF0) + 1 = 0x00100000 = 1MB
```

### 实现

```c
static void pci_probe_bars(pci_bdf_t bdf)
{
    for (int i = 0; i < 6; i++) {
        uint8_t offset = PCI_CFG_BAR0 + i * 4;
        uint32_t old = pci_cfg_read32(bdf, offset);     // 保存原始值
        pci_cfg_write32(bdf, offset, 0xFFFFFFFF);        // 写全 1
        uint32_t val = pci_cfg_read32(bdf, offset);      // 读回
        pci_cfg_write32(bdf, offset, old);                // 恢复

        if (val == 0 || val == 0xFFFFFFFF) {
            bar_sizes[i] = 0;                             // 该 BAR 未实现
            continue;
        }

        uint32_t mask = (val & 1) ? 0xFFFFFFFC : 0xFFFFFFF0;  // I/O BAR vs Memory BAR
        uint32_t size = ~(val & mask) + 1;                    // 计算大小
        bar_sizes[i] = size;
    }
}
```

### 地址分配

确定每个 BAR 的大小后，从 MMIO 池（`0x40000000` 起）顺序分配地址。每个 BAR 的起始地址必须是其大小的整数倍（PCI 规范要求）：

```c
static uint64_t pci_assign_bar(pci_bdf_t bdf, int bar_idx)
{
    uint32_t size = bar_sizes[bar_idx];
    if (size == 0) return 0;

    // 对齐到 size 边界（必须是 2 的幂）
    next_mmio = (next_mmio + size - 1) & ~(uint64_t)(size - 1);
    uint64_t addr = next_mmio;
    next_mmio += size;

    // 将分配的地址写入 BAR
    pci_cfg_write32(bdf, PCI_CFG_BAR0 + bar_idx * 4, (uint32_t)addr);

    // 64 位 BAR 需要写高位
    uint32_t lo = pci_cfg_read32(bdf, PCI_CFG_BAR0 + bar_idx * 4);
    if ((lo & 0x7) == 0x4) {
        pci_cfg_write32(bdf, PCI_CFG_BAR0 + bar_idx * 4 + 4, 0);
    }

    return addr;
}
```

### 典型分配结果

```
BAR0 (控制寄存器, 64KB)  → 0x40000000
BAR2 (VRAM, 64MB)        → 0x44000000  (跳过 BAR1 的 32KB)
```

## 关键设计决策

1. **只扫描 bus 0** — g233 板子的 GPGPU 直接挂在 bus 0 上，无需遍历多级 PCI 桥
2. **从 0x40000000 开始分配** — QEMU g233 板子的 GPEX 控制器在 `0x40000000` 处有一个大的 below-4G MMIO 窗口
3. **对齐要求** — BAR 地址必须是 BAR 大小的倍数，否则设备无法正确解码地址
4. **64 位 BAR 支持** — `pci_read_bar` 检查 bit[2:0] 是否为 `0b100`（64 位 BAR 标志），如果是则读取高位
