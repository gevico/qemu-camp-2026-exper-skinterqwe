# GPGPU Vortex simx 后端接入说明

当前 QEMU 侧已经保留 `hw/gpgpu/gpgpu.c` 作为 PCIe/MMIO/VRAM/MSI-X 前端，并通过可选后端把 kernel dispatch 转交给外部 Vortex simx 桥接库。

## QEMU 启动参数

默认仍使用仓库内手写 cmodel：

```bash
-device gpgpu
```

强制使用 Vortex simx：

```bash
-device gpgpu,backend=vortex,vortex-lib=/path/to/libvortex_qemu_simx.so
```

也可以通过环境变量指定库路径：

```bash
GPGPU_VORTEX_SIMX_LIB=/path/to/libvortex_qemu_simx.so \
  qemu-system-riscv64 ... -device gpgpu,backend=vortex
```

`backend=auto` 会在提供 `vortex-lib` 或 `GPGPU_VORTEX_SIMX_LIB` 时尝试加载 Vortex，失败后回退到 cmodel。

如果 guest 侧使用 Vortex 默认高地址 kernel 入口，例如 `STARTUP_ADDR=0x80000000`，需要声明 BAR2 偏移 0 对应的 Vortex device address：

```bash
-device gpgpu,backend=vortex,vortex-vram-base=0x80000000,\
vortex-lib=/path/to/libqemu-vortex-simx.so
```

## 外部 shim ABI

Vortex C++ shim 需要包含 `hw/gpgpu/gpgpu_vortex_abi.h`，并导出以下 C 符号：

```c
extern "C" void *vortex_qemu_create(const VortexQEMUConfig *cfg);
extern "C" void vortex_qemu_destroy(void *sim);
extern "C" int vortex_qemu_dispatch(void *sim,
                                     const VortexQEMUDispatch *dispatch);
```

`VortexQEMUDispatch` 提供：

- `kernel_pc`: kernel 入口的 Vortex device address。
- `kernel_args`: kernel 参数块的 Vortex device address。
- `vram_base`: QEMU BAR2 offset 0 对应的 Vortex device address。
- `grid_dim` / `block_dim` / `shared_mem_size`: QEMU 设备寄存器写入的 dispatch 参数。
- `vram` / `vram_size`: QEMU BAR2 后端内存视图，simx 可以直接读写。

如果 shim 异步执行 kernel，完成时调用 `cfg->done_cb(cfg->opaque, status)`；当前 QEMU 设备仍以同步 dispatch 语义完成 MMIO 写入，并在 `vortex_qemu_dispatch()` 返回后按既有逻辑设置 `KERNEL_DONE` 和触发 MSI-X。

## 参考 shim

仓库提供了一个最小 shim：

```bash
cd /home/yiyed/project/qemu-camp-2026-exper-skinterqwe
make -f contrib/gpgpu/Makefile.vortex \
  VORTEX_HOME=/home/yiyed/project/vortex \
  OUT=/tmp/libqemu-vortex-simx.so
```

该 shim 直接复用 Vortex 的 `runtime/simx/vortex.cpp`，并在 dispatch 时把 Vortex `RAM` 中覆盖 BAR2 地址范围的页映射到 QEMU BAR2 backing memory。这样 Vortex simx 的 load/store 仍经过原有 cache、memory coalescer 和 RAM 访问路径，但最终读写会直接落到 QEMU VRAM 指针上，不再对整块 BAR2 做 dispatch 前 upload 和 dispatch 后 download。

当前实现为了避免修改 Vortex 仓库，shim 在包含 Vortex runtime 实现时打开了必要的私有字段访问，用于替换 `RAM::pages_` 中对应页的 host 指针。长期更干净的方案是在 Vortex 侧提供正式的外部 memory backend 或 `MemDevice` 注入接口。

## QEMU 侧 smoke test

默认 GPGPU QTest 仍只覆盖 cmodel 后端，避免影响实验评分路径。若要验证 QEMU 能真实加载并初始化 Vortex simx shim，可以显式设置 shim 路径后运行可选节点：

```bash
GPGPU_QTEST_VORTEX_LIB=/tmp/libqemu-vortex-simx.so \
  build/tests/qtest/qos-test \
  -p /riscv64/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu-vortex/gpgpu-vortex-tests/simx-load
```

该用例只做设备 realize 和基础寄存器读取。Vortex 后端的 kernel dispatch 正确性应由后续裸机 kernel 或 OpenCL workload 验证。

若已生成 Vortex regression sgemm 的 `kernel.vxbin`，可以运行更完整的 QEMU+simx kernel dispatch 测试：

```bash
QTEST_QEMU_BINARY=build/qemu-system-riscv64 \
GPGPU_QTEST_VORTEX_LIB=/tmp/libqemu-vortex-simx.so \
GPGPU_QTEST_VORTEX_SGEMM_VXBIN=/home/yiyed/project/vortex/tests/regression/sgemm/kernel.vxbin \
  build/tests/qtest/qos-test \
  -p /riscv64/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu-vortex/gpgpu-vortex-tests/sgemm-n4
```

该测试把 `kernel.vxbin` 的 payload、kernel 参数块和 4x4 矩阵数据写入 QEMU BAR2，再通过 `backend=vortex` dispatch 到 simx，最后从 BAR2 读回输出矩阵并与 CPU 参考结果比较。

## Vortex 原生 sgemm 准备状态

Vortex regression sgemm 位于：

```text
/home/yiyed/project/vortex/tests/regression/sgemm
```

host runtime 需要先构建：

```bash
make -C /home/yiyed/project/vortex/runtime stub simx
```

再运行小尺寸 simx：

```bash
make -C /home/yiyed/project/vortex/tests/regression/sgemm run-simx OPTS=-n4
```

当前本机 `config.mk` 指向的 kernel 编译器为 `/home/yiyed/tools/llvm-vortex/bin/clang++`。如果该路径不存在，sgemm 会停在 `kernel.elf` 编译阶段；需要先安装或修正 `LLVM_VORTEX`、`LIBC_VORTEX`、`LIBCRT_VORTEX` 和 `RISCV_TOOLCHAIN_PATH` 后才能生成 `kernel.vxbin`。

如果缺少 `/home/yiyed/project/vortex/kernel/libvortex.a`，先构建 Vortex 裸机 kernel runtime：

```bash
make -C /home/yiyed/project/vortex/kernel
```

## 后续工作

1. 在 Vortex 仓库侧提供正式的外部 `MemDevice` / memory backend 注入接口，替代 shim 当前的私有字段访问。
2. 将 QEMU dispatch 升级为异步执行模型，kernel 完成后由 simx 回调触发 MSI-X。
3. 对齐进阶实验一的 `libgpgpu`/驱动 ABI 与 Vortex POCL/OpenCL runtime，最小目标是 OpenCL `sgemm`。
4. 在 ArceOS 和 rCore 中验证 PCI 发现、BAR 映射、MSI-X、中断完成和 VRAM 数据正确性。
