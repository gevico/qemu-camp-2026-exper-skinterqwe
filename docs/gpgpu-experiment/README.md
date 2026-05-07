# GPGPU 实验总结文档

本文档集总结本仓库 GPGPU 实验从 QEMU 设备建模、SIMT 执行核心、裸机软件栈到 Linux 内核模块和用户态运行时的完整实现过程。所有文档都放在本目录顶层，按编号阅读。

## 阅读顺序

### 共通基础

1. [00. 实验总览](00-overview.md)
   实验目标、最终成果、两套软件栈差异、共同地址空间和端到端流程。

2. [01. QEMU GPGPU 设备模型](01-qemu-device-model.md)
   QEMU PCI 设备建模、BAR 空间、MMIO 寄存器、VRAM 访问和 kernel dispatch。

3. [02. SIMT Core 与 Kernel 机器码](02-simt-core-and-kernels.md)
   SIMT warp/lane 模型、`mhartid` 编码、RV32I/RV32F 解释器、kernel 机器码与关键 bug。

### 裸机路径

4. [03. 裸机软件栈](03-baremetal-stack.md)
   裸机栈分层、PCI 枚举、BAR 分配、驱动层、runtime API 和运行入口。

5. [04. 裸机 PCI 驱动层](04-baremetal-pci-driver.md)
   ECAM 访问、PCI 设备扫描、BAR 探测和地址分配。

6. [05. 裸机 GPGPU 驱动层](05-baremetal-gpgpu-driver.md)
   BAR0/BAR2 封装、寄存器访问、VRAM 读写、设备 enable/reset 和 dispatch。

7. [06. 裸机运行时库](06-baremetal-runtime.md)
   CUDA 风格 API、VRAM bump allocator、数据传输、kernel launch 和同步语义。

8. [07. 裸机 CRT 运行环境](07-baremetal-crt-environment.md)
   `_start`、机器态初始化、trap handler、semihosting 输出/退出和链接脚本。

9. [08. 裸机 Demo 程序](08-baremetal-demo.md)
   `vector_add`、`relu`、`matmul` 三个端到端 demo 的数据准备、启动和校验。

10. [09. 裸机构建与运行](09-baremetal-build-run.md)
    交叉编译选项、链接布局、QEMU 启动参数和裸机调试方法。

### Linux 路径

11. [10. Linux 软件栈](10-linux-stack.md)
    Linux 版目录结构、为什么使用 `virt` 板、整体数据流和核心知识点。

12. [11. Linux 内核模块](11-linux-kernel-module.md)
    PCI probe、BAR0/BAR2、chardev、ioctl、mmap 和内核态 MMIO 访问规则。

13. [12. Linux 用户态 Runtime](12-linux-userspace-runtime.md)
    `libgpgpu` API、`/dev/gpgpu` 初始化、mmap 数据路径和 ioctl launch 路径。

14. [13. Linux initramfs 集成](13-linux-initramfs.md)
    `init_loader`、模块加载、rootfs overlay append、PID 1 和 poweroff 流程。

15. [14. Linux 构建与运行](14-linux-build-run.md)
    用户态程序、内核模块、rootfs 打包和 Linux 端到端 QEMU 运行。

### 调试记录

16. [15. 调试问题与后续改进](15-debugging-and-known-issues.md)
    典型错误、现象、原因、解决方法和后续改进方向。

### Vortex simx 进阶路径

17. [16. Vortex simx 集成总览](16-vortex-simx-overview.md)
    为什么要把手写 cmodel 替换为 Vortex simx、当前完成的集成边界和端到端执行路径。

18. [17. QEMU 侧后端切换机制](17-qemu-vortex-backend.md)
    `backend=cmodel/vortex/auto`、动态库加载、dispatch 入口和为什么使用外部 shim。

19. [18. Vortex shim ABI 与内存模型](18-vortex-shim-abi-and-memory.md)
    C/C++ ABI 桥接、`VortexQEMUDispatch`、BAR2 与 Vortex device address 映射、全量 VRAM 同步。

20. [19. Vortex sgemm 验证流程](19-vortex-sgemm-validation.md)
    从 Vortex 原生 `kernel.vxbin` 到 QEMU+simx `sgemm-n4` QTest 的完整验证流程。

21. [20. Vortex 集成学习路线与后续改进](20-vortex-learning-roadmap.md)
    面向初学者的阅读路线、核心概念复盘、常见问题和下一阶段方向。

## 当前验证结果

- 裸机 demo：`vector_add / relu / matmul` 全部 PASS。
- Linux demo：`gpgpu.ko` 加载成功，`/dev/gpgpu` 可用，三个 kernel 全部 PASS。
- Linux QEMU 启动后 demo 结束可正常 poweroff，不再因为 PID 1 退出触发 panic。
- Vortex simx：原生 `sgemm -n4` PASS，QEMU+Vortex `gpgpu-vortex-tests/sgemm-n4` PASS。
