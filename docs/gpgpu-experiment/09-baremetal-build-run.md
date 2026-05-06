# 09. 裸机构建与运行

## 概述

GPGPU Demo 程序是一个裸机 RISC-V 二进制，通过 QEMU 的 `-semihosting` 机制实现 I/O。构建系统由 `Makefile` 和 `semihost.ld` 链接器脚本组成，使用 `riscv64-unknown-elf-gcc` 交叉编译。

## Makefile

```makefile
CROSS    ?= riscv64-unknown-elf-
CC       := $(CROSS)gcc
LDFLAGS  := -nostdlib -static -mcmodel=medany
CFLAGS   := -O2 -g -march=rv64g -mabi=lp64d -mcmodel=medany -nostdlib -ffreestanding -Wall -Werror
```

### 编译选项详解

| 选项 | 含义 |
|------|------|
| `-nostdlib` | 不链接标准 C 库，使用自己的 CRT |
| `-ffreestanding` | 告诉编译器不在宿主 OS 环境运行，不假设标准库存在 |
| `-mcmodel=medany` | 中等代码模型，允许代码和数据在 ±2GB 范围内，适合裸机 |
| `-march=rv64g` | 目标架构：RV64G（I+M+A+F+D），包含浮点 |
| `-mabi=lp64d` | ABI：64 位整数 + 双精度浮点通过寄存器传参 |
| `-Wall -Werror` | 开启所有警告，警告视为错误 |
| `-O2` | 优化级别 2（不使用 -Os，因为裸机空间充足） |
| `-g` | 生成调试信息 |

### 链接选项

| 选项 | 含义 |
|------|------|
| `-nostdlib` | 不链接标准启动文件和库 |
| `-static` | 静态链接（裸机总是静态的） |
| `-T semihost.ld` | 使用自定义链接器脚本 |

### 源文件编译

```makefile
SRCS := demo/main.c \
        runtime/libgpgpu.c \
        driver/gpgpu_drv.c \
        driver/pci.c

CRT_OBJS := build/crt_crt.o \
            build/crt_console.o \
            build/crt_memory.o
```

CRT 文件来自 `tests/gevico/tcg/riscv64/crt/` 目录，提供：
- `crt.S` — `_start` 入口、trap handler、semihosting exit
- `console.c` — `printf()` 实现（通过 semihosting）
- `memory.c` — `memset()` / `memcpy()` 实现

### 构建目标

```makefile
TARGET := build/gpgpu_demo

all: $(TARGET)

$(TARGET): $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS)
```

最终产物 `build/gpgpu_demo` 是一个 ELF 可执行文件，QEMU 通过 `-device loader,file=...` 直接加载到内存中。

## 链接器脚本 (semihost.ld)

```ld
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text : {
        *crt_crt.o(.text .text.*)    /* CRT 启动代码必须在最前面 */
        *(.text .text.*)             /* 其他所有代码 */
    }
    .rodata : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(1 << 21);
    .data : {
        *(.data .data.*)
    }
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
}
```

### 关键设计

#### 入口地址 0x80000000

QEMU 的 g233 板子将 MROM/Flash 映射到 `0x80000000`。`-device loader` 会将 ELF 的 `.text` section 加载到这个地址。CPU 复位后从 `0x80000000` 取第一条指令执行。

#### CRT 优先放置

```
*crt_crt.o(.text .text.*)    ← 第 8 行：确保 _start 在 0x80000000
*(.text .text.*)             ← 第 9 行：其余代码跟在后面
```

**这是最关键的配置**。如果 CRT 的 `.text` 不排在最前面，`main()` 函数可能会被放在 `0x80000000`，而 `_start` 在后面。CPU 复位后直接执行 `main()`，跳过了栈指针初始化和 mstatus 设置，导致崩溃。

#### 2MB 对齐

```
. = ALIGN(1 << 21);    /* 2MB 对齐 */
```

`.data` section 对齐到 2MB 边界。这是为了匹配 RISC-V 页表的大页映射（如果需要开启 MMU）。在纯裸机场景下不影响功能。

#### 内存布局

```
0x80000000  ┌──────────────────────┐
            │ .text                │  代码段（CRT _start + main + 驱动 + 运行时）
            ├──────────────────────┤
            │ .rodata              │  只读数据（kernel 机器码等）
            ├──────────────────────┤  (2MB 对齐)
            │ .data                │  已初始化数据（全局变量）
            ├──────────────────────┤
            │ .bss                 │  未初始化数据（清零）
            └──────────────────────┘
0x84000000  ┌──────────────────────┐
            │ 栈 (4MB per hart)    │  sp 指向这里
            └──────────────────────┘
```

### 栈空间

CRT 中 `_start` 设置栈指针：

```asm
csrr    a0, mhartid       # hart ID
slli    a1, a0, 22        # a1 = hartid * 4MB
li      sp, 0x84000000    # 栈基地址
add     sp, sp, a1        # sp = 0x84000000 + hartid * 4MB
```

每个 hart 有 4MB 栈空间。裸机环境下只有 hart 0，所以 `sp = 0x84000000`。

## QEMU 运行命令

```bash
./build/qemu-system-riscv64 \
    -M g233 \                          # G233 开发板模型
    -m 2G \                            # 2GB 系统内存
    -display none \                    # 无图形输出
    -semihosting \                     # 启用 semihosting（printf 使用）
    -device loader,file=build/gpgpu_demo \  # 加载裸机 ELF
    -device gpgpu,addr=04.0            # 挂载 GPGPU PCI 设备到 0:4:0
```

### 参数解析

| 参数 | 含义 |
|------|------|
| `-M g233` | 使用 G233 板子模型，包含 ECAM at 0x30000000、MMIO 窗口 at 0x40000000 |
| `-m 2G` | 系统 RAM，用于栈和主机侧数据 |
| `-semihosting` | 允许 guest 使用 semihosting 协议调用宿主机 I/O |
| `-device loader,file=...` | 将 ELF 文件加载到 guest 内存（不通过 BIOS/UEFI） |
| `-device gpgpu,addr=04.0` | 将 GPGPU 设备挂在 PCI bus 0 的 slot 4 function 0 |

### 为什么需要 `addr=04.0`？

PCI 设备需要指定 BDF（Bus:Device:Function）。`addr=04.0` 表示 device=4, function=0, 默认 bus=0。驱动中的 `pci_find_device()` 会扫描 bus 0 的所有 slot 找到这个设备。

## 调试技巧

### 1. 检查 ELF 入口

```bash
riscv64-unknown-elf-readelf -h build/gpgpu_demo | grep "Entry point"
# 应该显示 0x80000000
```

### 2. 检查 _start 位置

```bash
riscv64-unknown-elf-nm build/gpgpu_demo | grep _start
# 应该显示 80000000 T _start
```

### 3. 反汇编

```bash
riscv64-unknown-elf-objdump -d build/gpgpu_demo | less
# 查看指令布局是否正确
```

### 4. QEMU 单步调试

```bash
./build/qemu-system-riscv64 -M g233 -m 2G -display none \
    -semihosting -S -s \
    -device loader,file=build/gpgpu_demo \
    -device gpgpu,addr=04.0

# 另一个终端
riscv64-unknown-elf-gdb build/gpgpu_demo
(gdb) target remote :1234
(gdb) b main
(gdb) c
```

`-S` 让 QEMU 启动时暂停，`-s` 开启 GDB server（端口 1234）。
