# 18. Vortex shim ABI 与内存模型

## C 和 C++ 为什么需要 ABI 桥

QEMU 侧是 C 代码，Vortex simx 侧是 C++ 代码。C++ 函数名会被 name mangling 处理，不同编译器和版本还可能有 ABI 差异。为了让 QEMU 稳定调用 Vortex，需要在中间放一层 C ABI。

QEMU 侧头文件是：

```text
hw/gpgpu/gpgpu_vortex_abi.h
```

shim 需要导出三个 C 符号：

```c
extern "C" void *vortex_qemu_create(const VortexQEMUConfig *cfg);
extern "C" void vortex_qemu_destroy(void *sim);
extern "C" int vortex_qemu_dispatch(void *sim,
                                     const VortexQEMUDispatch *dispatch);
```

这三个函数分别对应：

| 函数 | 作用 |
| --- | --- |
| `create` | 创建 Vortex simx 设备实例 |
| `destroy` | 销毁实例，释放资源 |
| `dispatch` | 执行一次 kernel |

## 配置结构体

`VortexQEMUConfig` 在创建时传入：

```text
abi_version
num_cus
warps_per_cu
warp_size
vram_size
opaque
done_cb
```

其中 `opaque` 和 `done_cb` 用于完成通知。当前实现还是同步 dispatch，但保留回调可以让后续改成异步模型。

## dispatch 结构体

每次启动 kernel 时，QEMU 会填一个 `VortexQEMUDispatch`：

```text
kernel_pc
kernel_args
vram_base
grid_dim[3]
block_dim[3]
shared_mem_size
vram
vram_size
```

这里最关键的是三组地址：

| 字段 | 含义 |
| --- | --- |
| `kernel_pc` | Vortex device address，kernel 入口 |
| `kernel_args` | Vortex device address，参数块地址 |
| `vram_base` | QEMU BAR2 offset 0 对应的 Vortex device address |

例如本次 sgemm 测试使用：

```text
vram_base   = 0x80000000
kernel_pc   = 0x80000000
kernel_args = 0x80010300
```

也就是说：

```text
QEMU BAR2 offset 0x00000  ↔  Vortex address 0x80000000
QEMU BAR2 offset 0x10300  ↔  Vortex address 0x80010300
```

## 当前零拷贝内存方式

当前 shim 已经从最初的全量复制升级为页级零拷贝映射：

```text
QEMU BAR2 backing memory
  ↓ 按页映射到
Vortex RAM::pages_[vram_base >> page_bits ...]

Vortex simx 执行 kernel

kernel load/store
  ↓ Vortex cache / memory coalescer / RAM 路径
QEMU BAR2 backing memory
```

也就是说，QEMU QTest 或 guest 先把 kernel、参数和数据写入 BAR2；dispatch 时 shim 把 Vortex `RAM` 中覆盖这段 device address 的页指针替换为 QEMU BAR2 的 host 指针。kernel 写输出矩阵时，结果直接写入 QEMU VRAM，不需要执行完成后再 download。

这个实现仍然保留 Vortex simx 的核心执行路径：

- 指令 fetch 和数据 load/store 仍由 Vortex core 发起。
- cache、memory coalescer 和调度行为仍在 simx 内部生效。
- 只有最终 RAM backing store 被替换为 QEMU BAR2 memory。

当前版本为了避免修改 Vortex 仓库，在 shim 包含 `runtime/simx/vortex.cpp` 时打开了必要的私有字段访问，从而能更新 `RAM::pages_`。更干净的长期接口应该由 Vortex 正式暴露外部 `MemDevice` 或 memory backend 注入能力。

## 为什么要设置 mem_reserve

Vortex runtime 对 device memory 有访问权限控制。正常 Vortex 程序会调用：

```text
vx_mem_reserve()
vx_mem_access()
vx_copy_to_dev()
vx_start()
```

QEMU shim 直接操作 Vortex `vx_device`，如果只映射 host page 但不声明这段地址可访问，simx 执行时仍可能触发内存访问错误。因此 shim 在第一次 dispatch 时会对整段 BAR2 映射执行：

```text
mem_reserve(vram_base, vram_size, VX_MEM_READ_WRITE)
```

这相当于告诉 Vortex：

```text
0x80000000 开始的 64MB 是可读写 device memory
```

## MPM class 初始化

Vortex 原生 runtime 在 `vx_dev_open()` 时会初始化若干 DCR 寄存器，其中包括：

```text
VX_DCR_BASE_MPM_CLASS = 0
```

本次 QEMU shim 不走完整 stub runtime，而是直接创建 simx `vx_device`，因此也需要显式写这个 DCR。否则 kernel 结束查询性能计数器时会出现类似错误：

```text
Error: invalid MPM CLASS: value=8
```

这类问题很典型：当我们绕过一层 runtime 直接调用更底层 API 时，要补齐 runtime 原本帮我们做的初始化。

## 和最初全量复制方案的区别

最初 bring-up 版本采用：

```text
dispatch 前: QEMU BAR2 -> Vortex RAM
dispatch 后: Vortex RAM -> QEMU BAR2
```

现在改为：

```text
dispatch 前: 建立 Vortex RAM page -> QEMU BAR2 page 映射
dispatch 后: 不需要回拷，结果已经在 QEMU BAR2 中
```

这减少了整块 VRAM 的 upload/download，更接近真实设备直接访问显存的模型，也为后续研究 DMA、cache flush、内存一致性和异步中断完成时序打下基础。
