# 03. 裸机软件栈

## 裸机软件栈结构

目录：

```text
tests/gevico/firmware/gpgpu/
├── common/gpgpu_regs.h
├── driver/pci.c
├── driver/pci.h
├── driver/gpgpu_drv.c
├── driver/gpgpu_drv.h
├── runtime/libgpgpu.c
├── runtime/libgpgpu.h
├── kernels/kernels.h
├── demo/main.c
├── semihost.ld
└── Makefile
```

分层：

```text
demo
  ↓
runtime/libgpgpu
  ↓
driver/gpgpu_drv
  ↓
driver/pci
  ↓
PCI ECAM + BAR0/BAR2
```

本文是裸机栈总览。更细的专题文档包括：

- [裸机 PCI 驱动层](04-baremetal-pci-driver.md)
- [裸机 GPGPU 驱动层](05-baremetal-gpgpu-driver.md)
- [裸机运行时库](06-baremetal-runtime.md)
- [SIMT Core 与 Kernel 机器码](02-simt-core-and-kernels.md)
- [裸机 CRT 运行环境](07-baremetal-crt-environment.md)
- [裸机 Demo 程序](08-baremetal-demo.md)
- [裸机构建与运行](09-baremetal-build-run.md)

## PCI 枚举

裸机没有 Linux PCI 子系统，因此需要手动扫描 PCI 配置空间。

核心函数：

```c
int pci_find_device(uint16_t vendor, uint16_t device, pci_bdf_t *out);
```

它扫描 bus 0 上的 device/function：

```text
bus 0
  device 0..31
    function 0..7
```

通过读取：

```text
PCI_CFG_VENDOR_ID = 0x00
PCI_CFG_DEVICE_ID = 0x02
```

匹配 GPGPU：

```text
vendor = 0x1234
device = 0x1337
```

## BAR 探测和分配

裸机环境中，QEMU PCI 设备的 BAR 可能初始为 0。软件必须自己完成：

1. 写 `0xffffffff` 到 BAR。
2. 读回 mask。
3. 根据 mask 计算 BAR 大小。
4. 分配 MMIO 地址。
5. 写回 BAR base。

这是标准 PCI BAR size probing 流程。

本实验中分配：

```text
BAR0 = 0x40000000
BAR2 = 0x44000000
```

裸机 demo 输出：

```text
PCI: BAR0=0x40000000 (size=0x00100000) BAR2=0x44000000 (size=0x04000000)
```

## GPGPU 驱动层

`driver/gpgpu_drv.c` 封装：

- `gpgpu_drv_init()`
- `gpgpu_reg_read()`
- `gpgpu_reg_write()`
- `gpgpu_vram_write()`
- `gpgpu_vram_read()`
- `gpgpu_enable()`
- `gpgpu_dispatch()`

驱动层负责理解 BAR0/BAR2：

```text
BAR0 base + register offset
BAR2 base + vram offset
```

runtime 只需要传 VRAM offset，不需要知道 PCI BAR 物理地址。

## runtime API

`runtime/libgpgpu.h` 暴露类 CUDA API：

```c
int  gpgpuInit(void);
void gpgpuGetDeviceInfo(gpgpuDeviceInfo *info);
uint32_t gpgpuMalloc(size_t size);
void gpgpuMemcpyH2D(uint32_t dst_vram, const void *src_host, size_t n);
void gpgpuMemcpyD2H(void *dst_host, uint32_t src_vram, size_t n);
void gpgpuLaunchKernel(const uint32_t *code, size_t code_words,
                       dim3 grid, dim3 block,
                       uint32_t *args, int num_args);
void gpgpuDeviceSynchronize(void);
```

它隐藏了底层细节：

- 初始化 PCI 设备。
- 启用 GPGPU。
- 管理 VRAM bump allocator。
- 上传 kernel code。
- 上传 kernel args。
- 写 dispatch 寄存器。

## bump allocator

VRAM 布局：

```text
0x0000 kernel code
0x1000 kernel args
0x2000 data
```

`gpgpuMalloc()` 从 `0x2000` 开始线性分配：

```c
size = (size + 3) & ~3;
addr = g_vram_next;
g_vram_next += size;
return addr;
```

这个 allocator 不支持真正的 free。`gpgpuFree()` 当前是 no-op。

为什么可以这样做：

- demo 生命周期短。
- 分配数量少。
- 每次运行从头初始化。

真实 runtime 需要：

- free list。
- 对齐管理。
- 内存复用。
- 越界检查。

## kernel launch

runtime launch 分三步：

1. 写 kernel code 到 `GPGPU_KERNEL_CODE_BASE`。
2. 写 args 到 `GPGPU_KERNEL_ARGS_BASE`。
3. 写 BAR0 dispatch 寄存器。

伪代码：

```c
gpgpu_vram_write(code_base, code, code_words * 4);
gpgpu_vram_write(args_base, args, num_args * 4);
gpgpu_dispatch(code_base, args_base, grid, block);
```

kernel 中通过：

```asm
lui x3, 1        # x3 = 0x1000
lw  x10, 0(x3)
lw  x11, 4(x3)
...
```

读取参数。

## 链接脚本问题

裸机程序使用：

```text
semihost.ld
```

曾经遇到的关键问题：

```text
_start 不在 0x80000000
```

原因是链接器脚本没有保证 CRT startup object 的 `.text` 放在最前。

解决：

```ld
*crt_crt.o(.text)
```

需要放在 `.text` 最前。这样 QEMU loader 加载后，入口地址就是 CRT `_start`。

CRT 启动、栈地址、semihosting 输出和退出路径的完整解释见 [07-baremetal-crt-environment.md](07-baremetal-crt-environment.md)。

## 裸机运行命令

构建：

```bash
cd tests/gevico/firmware/gpgpu
make clean && make
```

运行：

```bash
cd /home/yiyed/project/qemu-camp-2026-exper-skinterqwe
./build/qemu-system-riscv64 -M g233 -m 2G -display none \
    -semihosting \
    -device loader,file=tests/gevico/firmware/gpgpu/build/gpgpu_demo \
    -device gpgpu,addr=04.0
```

期望：

```text
GPGPU Advanced Demo
PCI: Found GPGPU at 0:4:0
PCI: BAR0=0x40000000 ...
=== Vector Add ===
  PASS (N=64)
=== ReLU ===
  PASS (N=32)
=== MatMul ===
  PASS (4x4 @ 4x4)
All demos completed.
```

## 裸机栈的学习重点

1. PCI 配置空间不是普通 MMIO 寄存器，需要通过 ECAM 访问。
2. BAR 初始值不一定可用，裸机必须自己探测和分配。
3. BAR 地址是 CPU 侧物理地址，kernel 参数中传的是 GPU VRAM offset。
4. runtime API 的价值在于隐藏硬件访问细节。
5. 简单 bump allocator 足够支撑教学 demo，但不是通用内存管理。
