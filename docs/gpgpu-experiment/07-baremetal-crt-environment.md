# 07. 裸机 CRT 运行环境

## CRT 在本实验中的位置

CRT 是 C runtime 的启动环境。Linux 程序通常由内核、动态链接器和 libc 准备进程环境；裸机程序没有这些基础设施，因此必须自己提供最小启动代码。

GPGPU 裸机 demo 使用的 CRT 来自：

```text
tests/gevico/tcg/riscv64/crt/
├── crt.S
├── crt.h
├── console.c
└── memory.c
```

GPGPU 裸机侧通过 `tests/gevico/firmware/gpgpu/Makefile` 复用这些对象：

```make
CRT_DIR  := $(realpath ../../tcg/riscv64/crt)

CRT_OBJS := build/crt_crt.o \
            build/crt_console.o \
            build/crt_memory.o
```

最终链接到：

```text
tests/gevico/firmware/gpgpu/build/gpgpu_demo
```

## 裸机程序为什么需要 CRT

裸机 demo 运行在 QEMU `g233` 板上：

```bash
qemu-system-riscv64 -M g233 -m 2G -display none \
    -semihosting \
    -device loader,file=tests/gevico/firmware/gpgpu/build/gpgpu_demo \
    -device gpgpu
```

这里没有 Linux，也没有用户态进程概念。QEMU loader 只负责把 ELF 加载到 guest 物理内存并跳到入口地址。入口之后的事情都要由程序自己完成，包括：

- 提供 `_start` 符号。
- 设置 trap vector。
- 设置机器态 CSR。
- 准备栈。
- 调用 `main()`。
- 在 `main()` 返回后进入 semihosting exit。
- 提供 `printf()`、`memset()`、`memcpy()` 等最小 libc 能力。

所以本实验的裸机软件栈不是从 `main()` 自然开始的，而是：

```text
QEMU loader
  ↓
ELF entry: _start
  ↓
CRT 机器态初始化
  ↓
main()
  ↓
GPGPU runtime / driver / PCI
  ↓
semihosting exit
```

## 编译和链接方式

GPGPU 裸机 demo 使用 freestanding 编译：

```make
CFLAGS  := -O2 -g -march=rv64g -mabi=lp64d -mcmodel=medany \
           -nostdlib -ffreestanding -Wall -Werror
LDFLAGS := -nostdlib -static -mcmodel=medany
```

关键含义：

| 选项 | 作用 |
| --- | --- |
| `-ffreestanding` | 告诉编译器当前不是 hosted C 环境，不能假设标准库完整存在 |
| `-nostdlib` | 不自动链接系统启动文件和 libc |
| `-static` | 生成静态 ELF，不依赖动态链接器 |
| `-mcmodel=medany` | 生成适合 RISC-V 裸机高地址加载的 PC-relative 代码 |
| `-march=rv64g -mabi=lp64d` | 目标是 64 位 RISC-V，启用通用整数、原子、浮点等扩展 |

因为使用 `-nostdlib`，链接器不会自动加入常规的 `crt0.o`、`libc.a` 或 `libgcc` 启动路径。本实验必须显式编译并链接：

```text
crt.S      -> build/crt_crt.o
console.c  -> build/crt_console.o
memory.c   -> build/crt_memory.o
```

## `_start` 启动流程

`crt.S` 提供真正入口：

```asm
.global _start

_start:
    lla     t0, trap
    csrw    mtvec, t0

    li      sp, 0x84000000
    call    _init_bsp
    call    main

    li      a0, 0
    j       _exit
```

实际流程可以拆成四步：

1. 设置 `mtvec`，让机器态 trap 进入本 CRT 提供的 `trap`。
2. 设置栈指针 `sp = 0x84000000`。
3. 调用 `_init_bsp` 配置机器态 CSR。
4. 调用 C 语言入口 `main()`。

这里的栈地址和程序链接地址是配套的：

```text
程序起始地址: 0x80000000
printf buffer: 0x82000000
栈顶地址:     0x84000000
```

demo 使用 `-m 2G`，这些地址都落在 guest RAM 内。

## 机器态初始化

`_init_bsp` 做的是最小机器态环境配置：

```asm
li      t2, 0x800000000024112d
csrw    0x301, t2

li      t0, 0x2200
csrr    t1, mstatus
or      t1, t1, t0
csrw    mstatus, t1
```

其中 `0x301` 是 `misa` CSR。这里写入的值用于声明当前机器支持的 ISA 能力。随后设置 `mstatus` 中和浮点状态相关的位，避免 C 代码或 demo 中的浮点操作因为 FPU 状态不可用而触发非法指令。

GPGPU demo 中 host 侧测试数据使用 `float`，例如：

```c
float A[64], B[64], C[64];
```

因此 CRT 必须让 guest CPU 处于可以执行浮点指令的状态。否则程序可能还没进入 GPGPU runtime，就在 CPU 侧数据准备或校验阶段 trap。

## trap 处理

CRT 的 `trap` 不是完整异常处理器，而是为 TCG 指令测试准备的最小逻辑：

```asm
trap:
    csrr    t0, mepc
    csrr    t1, mtval
    lwu     t2, 0(t0)
    bne     t1, t2, fail

    addi    t0, t0, 4
    csrw    mepc, t0
    mret
```

它的行为是：

- 读取发生 trap 的 PC。
- 读取 `mtval`。
- 从 PC 位置取出原始指令。
- 如果 `mtval` 等于该指令，说明这是预期的非法指令类测试，跳过该指令继续执行。
- 否则进入失败路径。

对 GPGPU 裸机 demo 来说，这段 trap 逻辑通常不是主路径。demo 的关键路径是 PCI 枚举、BAR 配置、MMIO 和 VRAM 访问。但复用这套 CRT 后，异常出口和失败处理仍然可用。

## `main()` 返回和 semihosting 退出

`main()` 返回后，当前 `crt.S` 会执行：

```asm
li      a0, 0
j       _exit
```

也就是说当前启动代码没有保留 `main()` 的返回值，而是固定按成功退出。失败路径会通过 `crt_abort()` 或 `fail` 设置：

```asm
li    a0, 1
```

然后进入 `_exit`。

`_exit` 使用 RISC-V semihosting 约定通知 QEMU 退出：

```asm
li    a0, 0x20    # TARGET_SYS_EXIT_EXTENDED

.balign    16
slli    zero, zero, 0x1f
ebreak
srai    zero, zero, 0x7
```

这要求 QEMU 启动参数带上：

```text
-semihosting
```

没有 `-semihosting` 时，`ebreak` 不会被 QEMU 当作 host 服务调用处理，程序可能停在异常或调试路径中。

## `printf()` 和 semihosting 输出

裸机环境没有 UART 驱动，也没有 stdout 文件描述符。`console.c` 自己实现了一个精简 `printf()`，最后通过 semihosting `SYS_WRITE0` 输出字符串：

```c
static void semihosting_write0(const char *str)
{
    register long a0 asm("a0") = 0x04; /* SYS_WRITE0 */
    register long a1 asm("a1") = (long)str;

    asm volatile(
        " .option push\n"
        " .option norvc\n"
        " .balign 16\n"
        " slli zero, zero, 0x1f\n"
        " ebreak\n"
        " srai zero, zero, 0x7\n"
        " .option pop\n"
        : : "r"(a0), "r"(a1) : "memory"
    );
}
```

打印缓冲区固定放在：

```c
static char *printbuf = (char *)0x82000000UL;
```

`printf()` 根据 `mhartid` 给每个 hart 分配一段 buffer：

```c
char *core_printbuf = HARTID * BUFLEN + printbuf;
```

本实验 demo 实际只按单核主路径运行，因此这个 per-hart buffer 主要来自原 TCG testcase CRT 的通用设计。

## `memset()` 和 `memcpy()`

`memory.c` 提供最基础的内存函数：

```c
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
```

这些函数有两个作用：

- 满足 C 代码显式调用，例如 runtime 和 demo 中的数据拷贝。
- 满足编译器在优化时可能生成的隐式调用。

因为使用 `-nostdlib`，如果没有这些符号，链接阶段就可能报 undefined reference。

需要注意的是，这里的 `memcpy()` 和 `memset()` 是普通 CPU 内存访问。访问 GPGPU VRAM 时，runtime 仍然会通过 BAR2 映射后的地址逐字节或逐字访问。二者概念不同：

```text
memcpy/memset: CPU 普通内存 helper
gpgpu_vram_write/read: PCI BAR2 指向的设备 VRAM 访问
```

## 链接脚本和入口地址

GPGPU 裸机程序使用：

```text
tests/gevico/firmware/gpgpu/semihost.ld
```

核心内容：

```ld
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text : {
        *crt_crt.o(.text .text.*)
        *(.text .text.*)
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

这里有两个关键点：

- `ENTRY(_start)` 告诉 ELF 入口符号是 `_start`。
- `*crt_crt.o(.text .text.*)` 强制 CRT 启动代码排在 `.text` 最前。

之前遇到过 `_start` 不在 `0x80000000` 的问题，根因是普通 `.text` 输入段排序可能让 `demo/main.o` 或其他对象排在 `crt.S` 前面。QEMU loader 跳到 ELF entry 时虽然会找 `_start`，但教学场景里把启动代码稳定放在镜像最前更容易调试，也避免不同链接器排序导致入口和反汇编视图不一致。

`.data` 前使用：

```ld
. = ALIGN(1 << 21);
```

这会把 `.data` 对齐到 2MB 边界，降低代码、只读数据、可写数据互相挤压造成定位困难的概率。当前脚本没有显式 `__bss_start` / `__bss_end`，CRT 也没有清零 `.bss` 的循环，因此裸机 C 代码不应该依赖复杂的全局可变对象初始化语义。简单的全局静态变量在 QEMU ELF loader 加载段后通常可用，但如果后续扩展 CRT，建议显式补充 `.bss` 清零。

## 与 Linux 运行环境的区别

| 项目 | 裸机 CRT 环境 | Linux 环境 |
| --- | --- | --- |
| 入口 | `_start` | 内核加载 ELF 后进入用户态 `_start`，再由 libc 调 `main` |
| 标准库 | 手写最小函数 | sysroot 中的 libc |
| 输出 | semihosting `SYS_WRITE0` | console / stdout |
| 退出 | semihosting `SYS_EXIT_EXTENDED` | `exit()` 系统调用 |
| PCI 枚举 | 裸机驱动手写 ECAM 扫描 | Linux PCI subsystem |
| BAR 分配 | 裸机驱动手动探测和写 BAR | 内核自动分配资源 |
| 内存映射 | 直接物理地址访问 | `mmap()` 设备 BAR |

因此，CRT 文档主要解释的是裸机 host 程序如何跑起来；它不解释 GPU kernel 本身的启动。GPU kernel 是由 QEMU GPGPU 设备模型从 VRAM 中取 RV32 指令解释执行，属于 `02-simt-core-and-kernels.md` 的范围。

## 常见问题

### 程序没有任何输出

优先检查：

- QEMU 命令是否带了 `-semihosting`。
- ELF 是否链接了 `crt_console.o`。
- `printf()` 是否能访问 `0x82000000`。
- 程序是否在进入 `main()` 前 trap。

### 链接时报 undefined reference

常见缺失符号：

```text
_start
printf
memcpy
memset
crt_abort
```

处理方式：

- 确认 `CRT_OBJS` 都在 `OBJS` 中。
- 确认 `CRT_DIR` 能正确解析到 `tests/gevico/tcg/riscv64/crt`。
- 确认没有误删 `-T semihost.ld`。

### 进入 main 前卡住

可以检查反汇编：

```bash
riscv64-unknown-elf-objdump -d tests/gevico/firmware/gpgpu/build/gpgpu_demo | head -40
```

期望最前面能看到 `_start`，并且第一段逻辑是写 `mtvec`、设置 `sp`、调用 `_init_bsp`。

### 浮点指令非法

如果 GPGPU demo 在初始化测试数组时触发非法指令，重点检查 `_init_bsp` 是否设置了 `mstatus` 浮点状态位，以及 QEMU CPU 是否支持 `rv64g` 中的 F/D 扩展。

### BSS 相关问题

当前 CRT 没有显式清零 `.bss`。如果后续添加更多全局状态，建议在链接脚本中增加：

```ld
__bss_start = .;
*(.bss .bss.*)
*(COMMON)
__bss_end = .;
```

并在 `_start` 调用 `main()` 前用汇编循环清零 `[__bss_start, __bss_end)`。

## 本实验中的设计取舍

这套 CRT 的目标不是实现完整 libc，而是满足教学实验需要：

- 让裸机 C 程序可启动。
- 让 `printf()` 能输出调试信息。
- 让成功和失败能通过 QEMU semihosting 返回。
- 让 CPU 侧 demo 可以准备输入数据、调用 runtime、校验输出。

它刻意保持很小，因此适合解释裸机程序最小运行条件。GPGPU 实验真正的设备逻辑仍然集中在 PCI 设备模型、SIMT core、runtime 和 Linux 驱动中。
