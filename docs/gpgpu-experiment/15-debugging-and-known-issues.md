# 15. 调试问题与后续改进

## QEMU 构建失败：孤立调试代码

现象：

```text
left-hand operand of comma expression has no effect
```

原因：

- 删除 `fprintf` 时留下了参数表达式。

解决：

- 检查 `hw/gpgpu/gpgpu_core.c` 的 `exec_warp` 附近。
- 删除残留调试片段。

## QTest 链接失败：undefined reference to main

现象：

```text
undefined reference to `main`
```

原因：

- `tests/gevico/qtest/meson.build` 注册了测试文件。
- 但对应 `.c` 文件是 0 字节。

解决：

- 补最小 `g_test_init()` 和 `g_test_run()` 入口。

## vector_add 第二个 block 全 0

原因：

- kernel 中 `mhartid` 被 `andi` 覆盖。
- `bid` 计算永远为 0。

解决：

```asm
csrrs x5, mhartid, x0
andi  x6, x5, 0x1f
srli  x7, x5, 13
```

## relu 正数没有写回

原因：

- 分支偏移跳到了 `ebreak`。

解决：

- 正数分支跳过 `sw zero` 和 `ebreak`，进入 `sw val`。

## matmul 循环错误

原因：

- 回跳偏移跳过了循环条件检查。
- 退出分支偏移也不对。

解决：

- 回跳到 `bge k, K, exit`。
- 重新计算 branch offset。

## Linux 外部模块 `-march=` 为空

现象：

```text
riscv64-linux-gnu-gcc: error: missing argument to '-march='
```

原因：

- 使用了错误的 kernel build tree。
- `/tmp/linux-6.6.75` 里的 `.config` 是 x86 配置。

解决：

- 新建干净源码树。
- 从 `tests/gevico/firmware/Image` 用 `extract-ikconfig` 提取 RISC-V 配置。

## Linux 6.6 `vm_flags` 只读

现象：

```text
assignment of read-only member 'vm_flags'
```

原因：

- Linux 新版本不允许直接写 `vma->vm_flags`。

解决：

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
#else
    vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
#endif
```

## demo 作为 PID 1 退出导致 panic

现象：

```text
Kernel panic - not syncing: Attempted to kill init!
```

原因：

- `/init` 直接 exec demo。
- demo 是 PID 1。
- PID 1 退出时 Linux 认为 init 死亡。

解决：

- `init_loader` 保持 PID 1。
- fork 子进程运行 demo。
- wait demo 退出后 poweroff。

## 后续改进方向

1. 引入 kernel 汇编源文件和自动机器码生成流程。
2. 支持异步 command queue 和中断完成通知。
3. 实现真正的 VRAM allocator。
4. 增加多 warp、多 block 的更复杂调度验证。
5. 支持更多 RV32F 比较和转换指令。
6. 将 Linux 驱动的 ioctl ABI 扩展为更接近真实 GPU runtime 的 command buffer。
7. 添加 QTest 覆盖 Linux 设备路径中的 ABI 结构体和寄存器配置。
