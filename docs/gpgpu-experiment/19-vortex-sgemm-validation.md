# 19. Vortex sgemm 验证流程

## 为什么选择 sgemm

`sgemm` 是单精度矩阵乘法，形式是：

```text
C = A x B
```

它适合作为 GPGPU 验证 workload，因为它同时覆盖：

- kernel 参数传递。
- 全局内存读写。
- 多维 grid。
- 浮点乘法和加法。
- 结果可用 CPU 版本直接校验。

本次使用的是 Vortex regression 里的 sgemm：

```text
/home/yiyed/project/vortex/tests/regression/sgemm
```

## 先跑 Vortex 原生 simx

在接入 QEMU 之前，先确认 Vortex 自己可以跑通：

```bash
make -C /home/yiyed/project/vortex/kernel
make -C /home/yiyed/project/vortex/runtime stub simx
make -C /home/yiyed/project/vortex/tests/regression/sgemm run-simx OPTS=-n4
```

通过时会看到：

```text
PASSED!
```

这一步验证的是：

- Vortex kernel runtime `libvortex.a` 可用。
- Vortex host runtime `libvortex.so` 可用。
- Vortex simx runtime `libvortex-simx.so` 可用。
- `llvm-vortex` 能编译 `kernel.cpp`。
- `kernel.vxbin` 能被 Vortex 原生 runtime 上传并执行。

## kernel.vxbin 是什么

Vortex 的 `kernel.vxbin` 不是普通裸 binary，它的前 16 字节是两个小端 64 位地址：

```text
offset 0x00: min_vma
offset 0x08: max_vma
offset 0x10: kernel payload
```

对本次 `sgemm`，`readelf` 可以看到 kernel 入口：

```text
Entry point 0x80000000
```

QEMU 测试读取 `kernel.vxbin` 后，会把 payload 写到：

```text
BAR2 offset = min_vma - vram_base
```

当 `vram_base=0x80000000` 且 `min_vma=0x80000000` 时，payload 就写到 BAR2 offset 0。

## QEMU 集成测试做了什么

新增的可选测试是：

```text
gpgpu-vortex/gpgpu-vortex-tests/sgemm-n4
```

它不会默认注册，只有设置两个环境变量才会出现：

```bash
GPGPU_QTEST_VORTEX_LIB=/tmp/libqemu-vortex-simx.so
GPGPU_QTEST_VORTEX_SGEMM_VXBIN=/home/yiyed/project/vortex/tests/regression/sgemm/kernel.vxbin
```

测试内部做这些事：

1. 读取 `kernel.vxbin`。
2. 解析 `min_vma`、`max_vma` 和 payload。
3. 把 payload 写入 BAR2。
4. 准备 4x4 矩阵 A 和 B。
5. 在 CPU 上计算参考矩阵 C。
6. 把 A、B、空 C 和 kernel 参数块写入 BAR2。
7. 写 BAR0 配置寄存器：
   - `kernel_addr = min_vma`
   - `kernel_args = 0x80010300`
   - `grid_dim = 4 x 4 x 1`
   - `block_dim = 1 x 1 x 1`
8. 写 `GPGPU_REG_DISPATCH`。
9. 等 QEMU 同步返回后检查错误寄存器。
10. 从 BAR2 读回 C，和 CPU 结果逐项比较。

运行命令：

```bash
QTEST_QEMU_BINARY=build/qemu-system-riscv64 \
GPGPU_QTEST_VORTEX_LIB=/tmp/libqemu-vortex-simx.so \
GPGPU_QTEST_VORTEX_SGEMM_VXBIN=/home/yiyed/project/vortex/tests/regression/sgemm/kernel.vxbin \
build/tests/qtest/qos-test \
  -p /riscv64/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu-vortex/gpgpu-vortex-tests/sgemm-n4
```

通过时输出：

```text
ok 1 /riscv64/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu-vortex/gpgpu-vortex-tests/sgemm-n4
```

## 地址布局

本测试使用的 BAR2 布局如下：

```text
Vortex address      BAR2 offset      内容
0x80000000          0x00000          kernel payload
0x80010000          0x10000          A 矩阵
0x80010100          0x10100          B 矩阵
0x80010200          0x10200          C 矩阵
0x80010300          0x10300          kernel_arg_t
```

`kernel_arg_t` 来自 Vortex sgemm 的 `common.h`：

```c
typedef struct {
  uint32_t grid_dim[2];
  uint32_t size;
  uint64_t A_addr;
  uint64_t B_addr;
  uint64_t C_addr;
} kernel_arg_t;
```

测试写入参数时要注意结构体对齐。`grid_dim[2]` 和 `size` 后面需要补 4 字节 padding，之后才是 64 位地址字段。

## 它验证了哪些能力

这个测试比单纯 `simx-load` 更有价值，因为它验证了：

- QEMU 能加载 Vortex shim。
- QEMU BAR2 数据能进入 Vortex RAM。
- Vortex kernel 能从 QEMU 设置的 `kernel_pc` 启动。
- `kernel_args` 能被 Vortex kernel 通过 `MSCRATCH` 使用。
- kernel 能读 A/B 并写 C。
- 执行结束后，结果能从 Vortex RAM 回写到 QEMU BAR2。
- QEMU 的 dispatch 状态和错误寄存器能反映执行结果。

这就是一个最小但完整的“QEMU 设备前端 + Vortex 执行后端 + 真实 AI kernel”闭环。
