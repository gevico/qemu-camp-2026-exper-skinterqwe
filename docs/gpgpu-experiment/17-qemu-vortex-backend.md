# 17. QEMU 侧后端切换机制

## 原来的结构

基础实验中，`gpgpu.c` 在收到 dispatch 寄存器写入后，直接调用手写执行器：

```text
guest 写 GPGPU_REG_DISPATCH
  ↓
gpgpu_ctrl_write()
  ↓
gpgpu_core_exec_kernel()
  ↓
gpgpu_core.c 解释 RV32 指令
```

这条路径简单直接，但执行后端被固定死了。进阶实验要支持 Vortex，就需要把“设备前端”和“执行后端”拆开。

## 新的 backend 属性

现在 GPGPU 设备新增了几个属性：

```text
backend=cmodel|vortex|auto
vortex-lib=/path/to/libqemu-vortex-simx.so
vortex-vram-base=0x80000000
```

含义如下：

| 属性 | 作用 |
| --- | --- |
| `backend=cmodel` | 使用原来的手写解释器，默认值 |
| `backend=vortex` | 强制加载 Vortex simx shim |
| `backend=auto` | 如果提供 shim 就尝试 Vortex，否则回退 cmodel |
| `vortex-lib` | 外部 C++ shim 动态库路径 |
| `vortex-vram-base` | QEMU BAR2 offset 0 对应的 Vortex device address |

默认路径保持不变：

```bash
-device gpgpu
```

Vortex 路径显式开启：

```bash
-device gpgpu,backend=vortex,\
vortex-lib=/tmp/libqemu-vortex-simx.so,\
vortex-vram-base=0x80000000
```

## 后端初始化

设备 realize 时，QEMU 会根据 `backend` 初始化执行后端：

```text
gpgpu_realize()
  ↓
分配 VRAM
  ↓
gpgpu_backend_init()
  ├─ cmodel: 不需要额外初始化
  ├─ vortex: gpgpu_vortex_init()
  └─ auto: 有 shim 时尝试 Vortex，失败则警告并回退
```

`gpgpu_vortex_init()` 的核心工作是：

1. 找到动态库路径。
2. 用 `g_module_open()` 加载动态库。
3. 查找三个 C ABI 符号：
   - `vortex_qemu_create`
   - `vortex_qemu_destroy`
   - `vortex_qemu_dispatch`
4. 调用 `vortex_qemu_create()` 创建 Vortex simx 实例。
5. 把返回的后端对象保存在 `s->vortex_backend`。

## dispatch 时如何选择后端

写 `GPGPU_REG_DISPATCH` 时，QEMU 仍然执行原来的设备状态流程：

```text
检查 GPGPU_CTRL_ENABLE
  ↓
设置 BUSY，清 READY
  ↓
执行 kernel
  ↓
清 BUSY，置 READY
  ↓
如果开启中断，触发 MSI-X
```

不同的是“执行 kernel”现在通过统一入口：

```text
gpgpu_exec_kernel()
  ├─ backend=vortex 或 s->vortex_backend 存在
  │    ↓
  │  gpgpu_vortex_exec_kernel()
  └─ 否则
       ↓
     gpgpu_core_exec_kernel()
```

这样做的好处是前端逻辑只有一份。无论后端是 cmodel 还是 Vortex，guest 看到的 PCI 设备、BAR 和寄存器行为都是同一个模型。

## 为什么使用动态库

QEMU 主体是 C 项目，而 Vortex simx 是 C++ 项目。直接把大量 C++ 源码编进 QEMU 会带来几个问题：

- QEMU 的构建系统要处理 C++ ABI、链接顺序和外部依赖。
- Vortex 仓库会引入 ramulator、softfloat 等第三方库。
- 实验仓库和 Vortex 仓库的生命周期不同。

因此这里使用动态库隔离：

```text
QEMU C 代码
  ↓ C ABI
libqemu-vortex-simx.so
  ↓ C++ API
Vortex runtime/simx
```

QEMU 只关心稳定的 C ABI，Vortex 相关依赖都留在 shim 动态库里。
