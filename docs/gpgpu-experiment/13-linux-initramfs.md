# 13. Linux initramfs 集成

## 为什么需要 init_loader

原始 `rootfs.cpio.gz` 很小，只包含一个静态 `/init`，没有 shell 和 busybox。因此不能使用 shell 脚本作为 init。

解决方案是提供：

```text
init_loader.c
```

它是 RISC-V 静态可执行文件，作为 PID 1 运行。

## init_loader 职责

`init_loader` 的启动流程：

1. 挂载 `/proc`、`/sys`、`/dev`。
2. 读取 `/lib/modules/gpgpu.ko`。
3. 通过 `init_module` syscall 加载模块。
4. `fork()` 子进程执行 `/bin/gpgpu_demo`。
5. 等待 demo 退出。
6. 调用 `reboot(RB_POWER_OFF)` 关机。

## 为什么要 fork

如果 PID 1 直接 `execve()` 成 demo，demo 正常退出后 Linux 会 panic：

```text
Kernel panic - not syncing: Attempted to kill init!
```

让 PID 1 保持为 init loader，demo 作为子进程退出后由 init loader 处理，就不会 panic。demo 成功或失败后，init loader 都能收集退出状态并触发 poweroff。

## rootfs overlay append

不能直接解包原始 cpio 再重新打包，因为普通用户无法创建 device node。

采用 overlay append：

1. 解压原始 rootfs 为 `build/rootfs.cpio`。
2. 构建 overlay 目录：

```text
bin/gpgpu_demo
lib/modules/gpgpu.ko
init
```

3. 将 overlay 追加到 cpio：

```bash
cd build/rootfs-overlay
find . | cpio -o -H newc >> ../rootfs.cpio
```

cpio 中后出现的同名 `/init` 会覆盖前面的 `/init`。最终产物是：

```text
tests/gevico/firmware/gpgpu/linux/build/gpgpu_linux.cpio.gz
```
