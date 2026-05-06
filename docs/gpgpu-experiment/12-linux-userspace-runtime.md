# 12. Linux 用户态 Runtime

## 目录结构

```text
linux/libgpgpu/
├── libgpgpu.c
├── libgpgpu.h
├── gpgpu_ioctl.h
└── Makefile
```

Linux 用户态 runtime 保持与裸机 runtime 相同的 API。demo 层不需要关心当前运行在裸机还是 Linux，只需要使用统一的 `gpgpuInit()`、`gpgpuMalloc()`、`gpgpuMemcpy*()` 和 `gpgpuLaunchKernel()`。

## 初始化流程

```c
g_fd = open("/dev/gpgpu", O_RDWR);
ioctl(g_fd, GPGPU_IOCTL_GET_INFO, &info);
g_vram_base = mmap(NULL, info.vram_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_fd, 0);
```

初始化阶段完成三件事：

1. 打开字符设备。
2. 通过 ioctl 获取 CU、warp、VRAM 大小等设备信息。
3. 把 BAR2 VRAM 映射到用户进程地址空间。

## VRAM 分配

Linux runtime 复用裸机侧的简单布局：

```text
0x0000 kernel code
0x1000 kernel args
0x2000 data
```

`gpgpuMalloc()` 从 `0x2000` 开始线性分配。当前 demo 生命周期短、分配数量少，因此 bump allocator 足够验证端到端路径。

## 数据传输

H2D/D2H 主路径直接访问 mmap 后的 BAR2：

```c
memcpy(g_vram_base + dst_vram, src_host, n);
memcpy(dst_host, g_vram_base + src_vram, n);
```

这样大块数据不需要每次通过 ioctl copy 进入内核。

## kernel launch

launch 前先把 kernel code 和参数写到固定 VRAM 区域：

```c
memcpy(g_vram_base + 0x0000, code, code_words * 4);
memcpy(g_vram_base + 0x1000, args, num_args * 4);
ioctl(g_fd, GPGPU_IOCTL_LAUNCH, &launch);
```

`GPGPU_IOCTL_LAUNCH` 进入内核模块后写 BAR0 dispatch 寄存器。QEMU 设备模型从 BAR2 的 code/args 区域取机器码和参数，执行结束后把结果写回 BAR2。

当前 QEMU 设备模型是同步 dispatch，`gpgpuDeviceSynchronize()` 在 Linux runtime 中主要保留 API 语义。
