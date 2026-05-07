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

## 当前内存同步方式

当前 shim 是 bring-up 版本，采用全量复制：

```text
dispatch 前:
  QEMU BAR2 backing memory
    ↓ upload
  Vortex RAM[vram_base, vram_base + vram_size)

Vortex simx 执行 kernel

dispatch 后:
  Vortex RAM[vram_base, vram_base + vram_size)
    ↓ download
  QEMU BAR2 backing memory
```

这样做性能不是最优，但概念简单，适合第一阶段验证：

- QEMU 不需要暴露复杂内存接口给 Vortex。
- Vortex simx 仍然使用自己的 RAM、cache 和 memory coalescer。
- 测试可以直接通过 BAR2 观察 kernel 输出。

## 为什么要设置 mem_reserve

Vortex runtime 对 device memory 有访问权限控制。正常 Vortex 程序会调用：

```text
vx_mem_reserve()
vx_mem_access()
vx_copy_to_dev()
vx_start()
```

QEMU shim 直接操作 Vortex `vx_device`，如果只 upload 数据但不声明这段地址可访问，simx 执行时可能触发内存访问错误。因此 shim 在第一次 dispatch 时会对整段 BAR2 映射执行：

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

## 后续更好的内存模型

全量复制适合教学和 bring-up，但后续可以改进为：

```text
QEMU BAR2 backing memory
  ↓ 封装为 Vortex MemDevice
Vortex cache/memory system 直接访问
```

这会减少拷贝，并能更精确地研究 DMA、cache flush、内存一致性和中断完成时序。
