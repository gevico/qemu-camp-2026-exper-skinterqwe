# 08. 裸机 Demo 程序

## 概述

`demo/main.c` 是端到端测试程序，运行三个 GPU 计算场景并验证结果正确性。程序在 QEMU 裸机环境下通过 semihosting 输出结果。

## 执行流程

```
_start (crt.S)
  ├── 设置 mtvec trap handler
  ├── 设置栈指针 sp
  ├── 初始化 mstatus（浮点使能等）
  └── call main
        ├── printf("GPGPU Advanced Demo")
        ├── gpgpuInit()                    ← PCI 枚举 + BAR 分配 + 设备使能
        ├── gpgpuGetDeviceInfo()           ← 打印设备信息
        ├── test_vector_add()              ← 测试 1：向量加法
        ├── test_relu()                    ← 测试 2：ReLU 激活
        ├── test_matmul()                  ← 测试 3：矩阵乘法
        └── printf("All demos completed.")
```

## 测试 1: Vector Add

**目标**：`C[i] = A[i] + B[i]`，N=64 个 float 元素

```c
static void test_vector_add(void)
{
    const int N = 64;
    float A[64], B[64], C[64];

    // 初始化输入：A[i]=i, B[i]=2i
    for (int i = 0; i < N; i++) {
        A[i] = (float)i;
        B[i] = (float)(i * 2);
    }

    // 分配 VRAM
    uint32_t dA = gpgpuMalloc(N * sizeof(float));   // VRAM 中的偏移量
    uint32_t dB = gpgpuMalloc(N * sizeof(float));
    uint32_t dC = gpgpuMalloc(N * sizeof(float));

    // Host → Device
    gpgpuMemcpyH2D(dA, A, N * sizeof(float));
    gpgpuMemcpyH2D(dB, B, N * sizeof(float));

    // 配置并启动 kernel
    uint32_t args[] = { dA, dB, dC, (uint32_t)N, (uint32_t)BLOCK_SIZE };
    dim3 grid = { (N + BLOCK_SIZE - 1) / BLOCK_SIZE, 1, 1 };  // = {2, 1, 1}
    dim3 block = { BLOCK_SIZE, 1, 1 };                         // = {32, 1, 1}
    gpgpuLaunchKernel(kernel_vector_add,
                      sizeof(kernel_vector_add) / sizeof(uint32_t),
                      grid, block, args, 5);
    gpgpuDeviceSynchronize();

    // Device → Host
    gpgpuMemcpyD2H(C, dC, N * sizeof(float));

    // 验证：C[i] 应该等于 A[i] + B[i] = i + 2i = 3i
    int pass = 1;
    for (int i = 0; i < N; i++) {
        float expected = (float)(i + i * 2);    // 3i
        if ((int)C[i] != (int)expected) {       // 粗略比较（截断为整数）
            printf("  FAIL at %d: got %d, expected %d\n",
                   i, (int)C[i], (int)expected);
            pass = 0;
        }
    }
    if (pass) printf("  PASS (N=%d)\n", N);
}
```

### 线程映射

```
Block 0: tid  0~31 → global_id  0~31 → 处理 C[ 0]~C[31]
Block 1: tid  0~31 → global_id 32~63 → 处理 C[32]~C[63]
```

每个线程计算一个 `C[global_id] = A[global_id] + B[global_id]`。

## 测试 2: ReLU

**目标**：`output[i] = max(0, input[i])`，N=32 个 float 元素

```c
static void test_relu(void)
{
    const int N = 32;
    float input[32], output[32];

    // 输入：-16, -15, ..., -1, 0, 1, ..., 15
    for (int i = 0; i < N; i++) {
        input[i] = (float)(i - 16);
    }

    uint32_t d_in  = gpgpuMalloc(N * sizeof(float));
    uint32_t d_out = gpgpuMalloc(N * sizeof(float));

    gpgpuMemcpyH2D(d_in, input, N * sizeof(float));

    uint32_t args[] = { d_in, d_out, (uint32_t)N, (uint32_t)BLOCK_SIZE };
    dim3 grid = { 1, 1, 1 };       // N=32 刚好一个 block
    dim3 block = { 32, 1, 1 };
    gpgpuLaunchKernel(kernel_relu, ..., grid, block, args, 4);
    gpgpuDeviceSynchronize();

    gpgpuMemcpyD2H(output, d_out, N * sizeof(float));

    // 验证
    int pass = 1;
    for (int i = 0; i < N; i++) {
        float expected = (input[i] > 0) ? input[i] : 0;
        if ((int)output[i] != (int)expected) { ... }
    }
    if (pass) printf("  PASS (N=%d)\n", N);
}
```

### ReLU 的 GPU 实现特点

- 只用 1 个 Block（N=32 = warp_size），每个线程处理一个元素
- 使用整数 `lw`/`sw` 代替浮点 `flw`/`fsw`，因为 ReLU 只需比较符号位
- `val >= 0` 的 IEEE754 float 转为整数后符号位为 0，整数比较等价于浮点比较

## 测试 3: MatMul

**目标**：`C = A × B`，4×4 矩阵乘法

```c
static void test_matmul(void)
{
    const int M = 4, N = 4, K = 4;
    float A[16], B[16], C[16];

    // A = 4×4 单位矩阵（I）
    for (int i = 0; i < M; i++)
        for (int j = 0; j < K; j++)
            A[i * K + j] = (float)(i == j ? 1 : 0);

    // B = 4×4 矩阵，B[i][j] = i*N + j
    for (int i = 0; i < K; i++)
        for (int j = 0; j < N; j++)
            B[i * N + j] = (float)(i * N + j);

    // ... 分配 VRAM，传输数据 ...

    uint32_t args[] = { dA, dB, dC, (uint32_t)N, (uint32_t)K, (uint32_t)N };
    dim3 grid = { M, 1, 1 };   // = {4, 1, 1}：4 个 block（每行一个）
    dim3 block = { N, 1, 1 };  // = {4, 1, 1}：4 个线程/每 block（每列一个）

    gpgpuLaunchKernel(kernel_matmul, ..., grid, block, args, 6);
    gpgpuDeviceSynchronize();

    gpgpuMemcpyD2H(C, dC, M * N * sizeof(float));

    // 验证：I × B = B，C 应该等于 B
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++)
            if (C[i * N + j] != B[i * N + j]) { ... }
}
```

### 线程映射

```
Block 0 (row=0): tid 0,1,2,3 → 计算 C[0][0], C[0][1], C[0][2], C[0][3]
Block 1 (row=1): tid 0,1,2,3 → 计算 C[1][0], C[1][1], C[1][2], C[1][3]
Block 2 (row=2): tid 0,1,2,3 → 计算 C[2][0], C[2][1], C[2][2], C[2][3]
Block 3 (row=3): tid 0,1,2,3 → 计算 C[3][0], C[3][1], C[3][2], C[3][3]
```

每个线程计算 C 的一个元素，需要遍历 K 维做内积。

### 验证策略

选择单位矩阵 A = I 作为输入，这样 `I × B = B`，验证时只需比较 C 是否等于 B。这比随机矩阵乘法更容易发现错误。

## CRT 环境

### 启动流程 (crt.S)

1. 设置 `mtvec` 指向 trap handler（处理 ebreak 等异常）
2. 根据 `mhartid` 设置栈指针（裸机只有一个 hart）
3. 初始化 `mstatus`（使能浮点单元等）
4. 调用 `main()`

### Semihosting

程序通过 RISC-V semihosting 协议与 QEMU 通信：

- `printf()` 通过 semihosting 的 `SYS_WRITEC` 输出字符到 QEMU 控制台
- `_exit()` 通过 semihosting 的 `TARGET_SYS_EXIT_EXTENDED` 退出

Semihosting 调用序列：
```asm
slli zero, zero, 0x1f    # magic sequence part 1
ebreak                    # trap → QEMU 捕获
srai zero, zero, 0x7     # magic sequence part 2
```

QEMU 识别这个特殊序列后执行宿主机侧的 I/O 操作。

### ebreak 与 trap handler

GPU kernel 执行 `ebreak` 结束每个线程。在 SIMT 引擎中这是正常的控制流（不是异常）。

Demo 程序本身如果遇到 `ebreak`（例如 CRT 测试中的非法指令陷阱），trap handler 会比较 `mepc` 和 `mtval`，如果匹配则跳过该指令继续执行。

## 运行与验证

```bash
# 构建
cd tests/gevico/firmware/gpgpu && make clean && make

# 运行
./build/qemu-system-riscv64 -M g233 -m 2G -display none \
    -semihosting \
    -device loader,file=tests/gevico/firmware/gpgpu/build/gpgpu_demo \
    -device gpgpu,addr=04.0
```

### 成功输出

```
GPGPU Advanced Demo
PCI: Found GPGPU at 0:4:0
PCI: BAR0=0x40000000 (size=0x10000) BAR2=0x44000000 (size=0x4000000)
GPGPU: 4 CUs, 4 warps/CU, 32 threads/warp, 64MB VRAM
=== Vector Add ===
  PASS (N=64)
=== ReLU ===
  PASS (N=32)
=== MatMul ===
  PASS (4x4 @ 4x4)
All demos completed.
```

### 常见失败模式

| 现象 | 可能原因 |
|------|---------|
| `PCI: GPGPU device not found` | QEMU 命令行缺少 `-device gpgpu,addr=04.0` |
| `BAR0=0x0` | BAR 探测逻辑错误或 MMIO 池地址不对 |
| Vector Add Block 0 PASS, Block 1 全为 0 | kernel 中 mhartid 被覆写，global_id 计算错误 |
| ReLU 全部为 0 | kernel 的 `bge` 跳转偏移量计算错误 |
| MatMul 全部错误 | `fmul.s` 或 `fadd.s` 的 funct7 编码错误 |
