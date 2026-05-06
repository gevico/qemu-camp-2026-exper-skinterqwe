# 02. SIMT Core 与 Kernel 机器码

## SIMT 模型

SIMT 是 Single Instruction Multiple Threads。GPU 中多个线程以 warp 为单位执行同一条指令，但每个 lane 有自己的寄存器和线程 ID。

本实验的核心抽象：

```text
Kernel
  └── Grid
      └── Block
          └── Warp
              └── Lane
```

当前默认参数：

| 项 | 值 |
| --- | --- |
| CU 数量 | 4 |
| 每个 CU warp 数 | 4 |
| warp size | 32 |
| VRAM | 64MB |

代码结构：

```c
typedef struct GPGPULane {
    uint32_t gpr[32];
    uint32_t fpr[32];
    uint32_t pc;
    uint32_t mhartid;
    bool active;
    float_status fp_status;
} GPGPULane;

typedef struct GPGPUWarp {
    GPGPULane lanes[32];
    uint32_t active_mask;
    uint32_t warp_id;
    uint32_t thread_id_base;
    uint32_t block_id[3];
} GPGPUWarp;
```

每个 lane 有独立的：

- 通用寄存器 `x0-x31`
- 浮点寄存器 `f0-f31`
- `pc`
- `mhartid`
- 浮点状态

## mhartid 编码

kernel 通过读 `mhartid` 获得线程身份。

本实验编码：

```text
mhartid = block_id_linear << 13 | warp_id << 5 | thread_id
```

也可以理解为：

| 位段 | 含义 |
| --- | --- |
| `[4:0]` | thread id in warp |
| `[12:5]` | warp id |
| `[31:13]` | block id linear |

典型 kernel 开头：

```asm
csrrs x5, mhartid, x0
andi  x6, x5, 0x1f     # tid
srli  x7, x5, 13       # bid
```

注意：`x5` 必须保留原始 `mhartid`，不能先 `andi` 再 `srli`。之前的 vector add block 1 错误就是因为覆盖了 `x5`。

错误写法：

```asm
csrrs x5, mhartid, x0
andi  x5, x5, 0x1f
srli  x6, x5, 13       # 此时 x5 已经只剩 tid，bid 永远是 0
```

正确写法：

```asm
csrrs x5, mhartid, x0
andi  x6, x5, 0x1f
srli  x7, x5, 13
```

## RV32I/RV32F 解释器

GPU kernel 使用 RV32 指令编码。解释器在 `hw/gpgpu/gpgpu_core.c` 中实现。

主循环：

```text
while cycle < max_cycles:
  for lane in active lanes:
    inst = read32(vram, lane.pc)
    decode opcode/funct3/funct7/rd/rs1/rs2
    execute instruction
    lane.pc = next_pc
```

已支持的主要指令类别：

| 类别 | opcode | 示例 |
| --- | --- | --- |
| U-type | `0x37/0x17` | `lui`, `auipc` |
| J-type | `0x6f/0x67` | `jal`, `jalr` |
| B-type | `0x63` | `beq`, `bne`, `blt`, `bge` |
| LOAD | `0x03` | `lb`, `lh`, `lw` |
| STORE | `0x23` | `sb`, `sh`, `sw` |
| OP-IMM | `0x13` | `addi`, `andi`, `srli` |
| OP | `0x33` | `add`, `sub`, `mul` |
| SYSTEM | `0x73` | `csrrs`, `ebreak` |
| LOAD-FP | `0x07` | `flw` |
| STORE-FP | `0x27` | `fsw` |
| OP-FP | `0x53` | `fadd.s`, `fsub.s`, `fmul.s` |

## active_mask 和 ebreak

每个 warp 有一个 `active_mask`，表示哪些 lane 仍在执行。

当 lane 执行：

```asm
ebreak
```

解释器会清除该 lane 对应 bit：

```c
lane->active = false;
warp->active_mask &= ~(1u << i);
```

当所有 lane 都执行结束：

```c
if (warp->active_mask == 0) {
    return 0;
}
```

这是一种简单的 kernel 退出协议。真实 GPU 会有更复杂的 block/warp 调度和陷入机制，本实验用 `ebreak` 表示当前 lane 完成。

## 分支偏移是核心易错点

RISC-V branch offset 是相对当前 PC，而不是下一条 PC。手写机器码时很容易错。

本实验修过两个重要 bug：

### ReLU 正数分支

目标逻辑：

```asm
if val >= 0:
    out = val
else:
    out = 0
```

错误是 `bge x18, x0, +8` 跳到了 `ebreak`，没有执行正数路径的 `sw`。

正确应跳过：

```asm
sw x0, 0(x22)
ebreak
```

到：

```asm
sw x18, 0(x22)
ebreak
```

因此需要 `+12`。

### MatMul 循环回跳

目标逻辑：

```c
for (k = 0; k < K; k++) {
    sum += A[row*K+k] * B[k*N+col];
}
```

错误回跳跳过了 `bge x16, x14, exit` 条件检查，导致循环控制不正确。

正确回跳要回到循环条件检查位置，而不是回到循环体中间。

## 三个 kernel

### vector_add

功能：

```c
C[i] = A[i] + B[i]
```

参数：

```text
args[0] = A_addr
args[1] = B_addr
args[2] = C_addr
args[3] = N
args[4] = blockDim
```

global id：

```asm
gid = bid * blockDim + tid
```

核心指令：

```asm
flw    f1, 0(A + gid*4)
flw    f2, 0(B + gid*4)
fadd.s f3, f1, f2
fsw    f3, 0(C + gid*4)
```

### relu

功能：

```c
out[i] = input[i] > 0 ? input[i] : 0
```

这里为了简化，用整数比较 float bit pattern。测试数据是 `-16..15` 的 float，负数符号位为 1，对有符号整数比较仍然能区分正负。

这不是通用 float 比较方法。更严谨的实现应该使用 RV32F 比较指令或处理 IEEE 754 特殊值。

### matmul

功能：

```c
C[row*N + col] = sum(A[row*K + k] * B[k*N + col])
```

demo 中 `A` 是单位矩阵，所以期望输出等于 `B`。

launch：

```text
grid = (M, 1, 1)
block = (N, 1, 1)
```

映射：

```text
row = block id
col = thread id
```

## 为什么 kernel 用机器码数组

当前实验直接在 `kernels/kernels.h` 中保存机器码数组：

```c
static const uint32_t kernel_vector_add[] = {
    0xF14022F3,
    ...
};
```

优点：

- 不依赖单独的 GPU 编译工具链。
- demo 可以直接上传机器码到 VRAM。
- 便于 QTest 和手工验证。

缺点：

- 可读性差。
- 分支偏移容易错。
- 难以维护复杂 kernel。

后续改进可以引入：

- RV32 汇编源文件。
- `riscv64-unknown-elf-as` 生成 object。
- `objcopy` 提取 `.text`。
- 自动生成 `kernels.h`。
