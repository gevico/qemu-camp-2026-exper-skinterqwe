# 11. Linux 内核模块

## 模块结构

```text
gpgpu_kmod/
├── gpgpu_module.c
├── gpgpu_pci.c
├── gpgpu_pci.h
├── gpgpu_char.c
├── gpgpu_char.h
├── gpgpu_regs.h
├── Kbuild
└── Makefile
```

职责划分：

| 文件 | 职责 |
| --- | --- |
| `gpgpu_module.c` | module init/exit |
| `gpgpu_pci.c` | PCI probe/remove |
| `gpgpu_char.c` | `/dev/gpgpu`、ioctl、mmap |
| `gpgpu_regs.h` | 复用寄存器定义 |

## PCI probe 流程

核心流程：

```text
pci_enable_device()
  ↓
pci_request_regions()
  ↓
pci_iomap(pdev, 0, 0)
  ↓
pci_resource_start(pdev, 2)
  ↓
pci_set_master()
  ↓
读取设备能力寄存器
  ↓
写 GLOBAL_CTRL enable
  ↓
gpgpu_char_set_dev(dev)
```

`pci_enable_device()` 启用 PCI 设备，允许内核访问设备资源。`pci_request_regions()` 声明驱动拥有该设备 BAR 资源，避免其他驱动冲突访问同一资源。

## BAR0 控制寄存器

BAR0 是控制寄存器。内核模块通过：

```c
dev->bar0 = pci_iomap(pdev, 0, 0);
```

获得 `void __iomem *`。

访问寄存器必须使用：

```c
ioread32()
iowrite32()
```

不能把 `__iomem` 当普通内存指针直接解引用。

## BAR2 VRAM

BAR2 是 VRAM。驱动保存：

```c
dev->bar2_phys = pci_resource_start(pdev, 2);
dev->bar2_size = pci_resource_len(pdev, 2);
```

BAR2 不在 probe 阶段长期 ioremap 给内核主路径使用，而是在 `mmap()` 中映射给用户态。

## chardev 和 ioctl

用户态通过：

```c
open("/dev/gpgpu", O_RDWR)
```

访问设备。

ioctl 定义在：

```text
linux/libgpgpu/gpgpu_ioctl.h
```

命令：

| ioctl | 方向 | 用途 |
| --- | --- | --- |
| `GPGPU_IOCTL_GET_INFO` | kernel -> user | 返回 CU/warp/VRAM 信息 |
| `GPGPU_IOCTL_MEMCPY_H2D` | user -> kernel | 备用 copy 路径 |
| `GPGPU_IOCTL_MEMCPY_D2H` | kernel -> user | 备用 copy 路径 |
| `GPGPU_IOCTL_LAUNCH` | user -> kernel | 写 BAR0 寄存器并 dispatch |

当前用户态主路径：

- H2D/D2H 通过 mmap 直接访问 BAR2。
- launch 通过 ioctl 写 BAR0。

## mmap BAR2

`gpgpu_mmap()` 将 BAR2 物理地址映射到用户进程地址空间：

```c
pfn = (dev->bar2_phys + off) >> PAGE_SHIFT;
vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
io_remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
```

关键点：

- `VM_IO` 表示这是 IO 映射。
- `VM_PFNMAP` 表示直接映射 PFN，不是普通匿名页。
- `pgprot_noncached()` 避免缓存一致性问题。
- Linux 6.6 中不能直接写 `vma->vm_flags`，要用 `vm_flags_set()`。
