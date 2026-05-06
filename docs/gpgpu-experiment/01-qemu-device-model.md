# 01. QEMU GPGPU 设备模型

## 设备模型的职责

QEMU 设备模型模拟一块 PCI GPGPU 卡。它需要完成三件事：

1. 在 PCI 总线上暴露一个标准设备。
2. 提供 MMIO 寄存器和 VRAM 供软件访问。
3. 在软件写 dispatch 寄存器时解释执行 GPU kernel。

核心文件：

- `hw/gpgpu/gpgpu.c`
- `hw/gpgpu/gpgpu.h`
- `hw/gpgpu/gpgpu_core.c`
- `hw/gpgpu/gpgpu_core.h`

## PCI 设备身份

设备 ID 定义在 `hw/gpgpu/gpgpu.h` 和固件侧 `common/gpgpu_regs.h`：

```c
#define GPGPU_VENDOR_ID 0x1234
#define GPGPU_DEVICE_ID 0x1337
```

Linux 启动时能看到：

```text
pci 0000:00:01.0: [1234:1337] type 00 class 0x030200
```

其中 `0x030200` 表示 3D Controller class。这样 Linux PCI 驱动可以通过：

```c
{ PCI_DEVICE(GPGPU_VENDOR_ID, GPGPU_DEVICE_ID) }
```

匹配设备。

## BAR 空间设计

### BAR0：控制寄存器

BAR0 是控制平面。软件通过 BAR0：

- 查询设备信息。
- 设置 kernel 地址。
- 设置 kernel 参数地址。
- 设置 grid/block 维度。
- 触发 dispatch。
- 查询状态和错误。

关键寄存器：

| 偏移 | 名称 | 说明 |
| --- | --- | --- |
| `0x0000` | `GPGPU_REG_DEV_ID` | 设备标识 |
| `0x0008` | `GPGPU_REG_DEV_CAPS` | CU/warp/thread 能力 |
| `0x000c` | `GPGPU_REG_VRAM_SIZE_LO` | VRAM 大小低 32 位 |
| `0x0010` | `GPGPU_REG_VRAM_SIZE_HI` | VRAM 大小高 32 位 |
| `0x0100` | `GPGPU_REG_GLOBAL_CTRL` | enable/reset |
| `0x0104` | `GPGPU_REG_GLOBAL_STATUS` | ready/busy/error |
| `0x0300` | `GPGPU_REG_KERNEL_ADDR_LO` | kernel 代码地址 |
| `0x0308` | `GPGPU_REG_KERNEL_ARGS_LO` | kernel 参数地址 |
| `0x0310` | `GPGPU_REG_GRID_DIM_X` | grid.x |
| `0x031c` | `GPGPU_REG_BLOCK_DIM_X` | block.x |
| `0x0330` | `GPGPU_REG_DISPATCH` | 写入后启动执行 |

### BAR2：VRAM

BAR2 是数据平面。它表示 GPU 可访问的显存，默认 64MB。

软件在 BAR2 中放：

- kernel 机器码。
- kernel 参数。
- 输入和输出数组。

设备解释器执行 load/store 指令时，访问的是 `s->vram_ptr`，也就是 BAR2 后端内存。

### BAR4：doorbell 预留

BAR4 当前没有作为核心路径使用。它保留给以后扩展异步队列、doorbell 或多队列提交模型。

## MMIO read/write

`gpgpu_ctrl_read()` 根据寄存器偏移返回设备状态。

典型例子：

```c
case GPGPU_REG_DEV_CAPS:
    return s->num_cus | (s->warps_per_cu << 8) | (s->warp_size << 16);
```

这个寄存器把三类能力打包进一个 32 位值：

- bit `[7:0]`：CU 数量。
- bit `[15:8]`：每个 CU 的 warp 数。
- bit `[23:16]`：每个 warp 的线程数。

`gpgpu_ctrl_write()` 处理控制动作。最关键的是 dispatch：

```c
case GPGPU_REG_DISPATCH:
    if (!(s->global_ctrl & GPGPU_CTRL_ENABLE)) {
        s->error_status |= GPGPU_ERR_INVALID_CMD;
        break;
    }
    s->global_status |= GPGPU_STATUS_BUSY;
    s->global_status &= ~GPGPU_STATUS_READY;
    if (gpgpu_core_exec_kernel(s) < 0) {
        s->error_status |= GPGPU_ERR_KERNEL_FAULT;
    }
    s->global_status &= ~GPGPU_STATUS_BUSY;
    s->global_status |= GPGPU_STATUS_READY;
    break;
```

这里的模型是同步执行：写 dispatch 后，QEMU 在 MMIO 回调里直接把 kernel 跑完。真实 GPU 一般是异步执行，但同步模型更适合教学和测试。

## VRAM 访问

BAR2 访问由 `gpgpu_vram_read()` 和 `gpgpu_vram_write()` 实现。它们按访问宽度处理 1/2/4/8 字节读写。

解释器内部也有 VRAM helper：

```c
static uint32_t gpu_mem_read32(GPGPUState *s, uint32_t addr);
static void gpu_mem_write32(GPGPUState *s, uint32_t addr, uint32_t val);
```

这些 helper 处理 GPU kernel 中的 `lw/sw/flw/fsw` 等指令。

## 状态机

设备状态主要由 `GLOBAL_CTRL` 和 `GLOBAL_STATUS` 管理。

控制位：

| 位 | 名称 | 说明 |
| --- | --- | --- |
| bit 0 | `GPGPU_CTRL_ENABLE` | 设备启用 |
| bit 1 | `GPGPU_CTRL_RESET` | 软复位 |

状态位：

| 位 | 名称 | 说明 |
| --- | --- | --- |
| bit 0 | `GPGPU_STATUS_READY` | 可接受任务 |
| bit 1 | `GPGPU_STATUS_BUSY` | 正在执行 |
| bit 2 | `GPGPU_STATUS_ERROR` | 出错 |

当前同步模型里，`BUSY` 只在 dispatch 回调内部短暂置位。Linux 用户态无需轮询等待，`gpgpuDeviceSynchronize()` 只是一个内存屏障语义的空操作。

## QEMU 设备模型的关键知识点

### PCI BAR 和设备内部地址不是一回事

Linux 里 BAR2 可能被分配到：

```text
0x400000000-0x403ffffff
```

但 GPU kernel 看到的是 VRAM 内部偏移：

```text
0x00002000
```

runtime 传给 kernel 的参数必须是 VRAM 偏移，而不是 host 物理地址或 Linux 虚拟地址。

### MMIO 控制平面和 VRAM 数据平面要分离

控制寄存器适合：

- 少量状态读写。
- 触发命令。
- 配置维度。

VRAM 适合：

- 大块数据。
- kernel code。
- kernel args。

这个分离让 Linux runtime 可以采用 `ioctl + mmap`：控制走 ioctl，数据走 mmap。

### 同步 dispatch 简化了验证

同步执行的好处：

- QTest 和 demo 容易写。
- 不需要中断等待。
- 不需要 command queue。
- 每次写 dispatch 后结果已经在 VRAM 中。

缺点：

- 不像真实 GPU。
- 长 kernel 会阻塞 QEMU vCPU。
- 后续要扩展异步模型时，需要引入队列、中断和状态轮询。
