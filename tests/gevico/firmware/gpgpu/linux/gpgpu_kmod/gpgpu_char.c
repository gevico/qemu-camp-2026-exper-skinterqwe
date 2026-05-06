#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "../libgpgpu/gpgpu_ioctl.h"
#include "gpgpu_char.h"
#include "gpgpu_pci.h"
#include "gpgpu_regs.h"

static dev_t gpgpu_devt;
static struct cdev gpgpu_cdev;
static struct class *gpgpu_class;
static struct gpgpu_dev *g_dev;
static DEFINE_MUTEX(gpgpu_lock);

void gpgpu_char_set_dev(struct gpgpu_dev *dev)
{
    mutex_lock(&gpgpu_lock);
    g_dev = dev;
    mutex_unlock(&gpgpu_lock);
}

static struct gpgpu_dev *gpgpu_get_dev(void)
{
    struct gpgpu_dev *dev;

    mutex_lock(&gpgpu_lock);
    dev = g_dev;
    mutex_unlock(&gpgpu_lock);

    return dev;
}

static int gpgpu_open(struct inode *inode, struct file *filp)
{
    if (!gpgpu_get_dev())
        return -ENODEV;
    return 0;
}

static int gpgpu_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static bool gpgpu_range_ok(struct gpgpu_dev *dev, u64 off, u64 size)
{
    return off <= dev->vram_size && size <= dev->vram_size - off;
}

static long ioctl_get_info(struct gpgpu_dev *dev, void __user *arg)
{
    struct gpgpu_info_data info = {
        .num_cus = dev->num_cus,
        .warps_per_cu = dev->warps_per_cu,
        .warp_size = dev->warp_size,
        .vram_size = dev->vram_size,
    };

    if (copy_to_user(arg, &info, sizeof(info)))
        return -EFAULT;

    return 0;
}

static long ioctl_memcpy_h2d(struct gpgpu_dev *dev, void __user *arg)
{
    struct gpgpu_memcpy_data data;
    void __iomem *vram;
    void *buf;
    long ret = 0;

    if (copy_from_user(&data, arg, sizeof(data)))
        return -EFAULT;
    if (!gpgpu_range_ok(dev, data.vram_offset, data.size))
        return -EINVAL;
    if (!data.size)
        return 0;

    buf = memdup_user((void __user *)(uintptr_t)data.user_ptr, data.size);
    if (IS_ERR(buf))
        return PTR_ERR(buf);

    vram = ioremap_wc(dev->bar2_phys + data.vram_offset, data.size);
    if (!vram) {
        ret = -ENOMEM;
        goto out_free;
    }

    memcpy_toio(vram, buf, data.size);
    iounmap(vram);

out_free:
    kfree(buf);
    return ret;
}

static long ioctl_memcpy_d2h(struct gpgpu_dev *dev, void __user *arg)
{
    struct gpgpu_memcpy_data data;
    void __iomem *vram;
    void *buf;
    long ret = 0;

    if (copy_from_user(&data, arg, sizeof(data)))
        return -EFAULT;
    if (!gpgpu_range_ok(dev, data.vram_offset, data.size))
        return -EINVAL;
    if (!data.size)
        return 0;

    buf = kmalloc(data.size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    vram = ioremap_wc(dev->bar2_phys + data.vram_offset, data.size);
    if (!vram) {
        ret = -ENOMEM;
        goto out_free;
    }

    memcpy_fromio(buf, vram, data.size);
    iounmap(vram);

    if (copy_to_user((void __user *)(uintptr_t)data.user_ptr, buf, data.size))
        ret = -EFAULT;

out_free:
    kfree(buf);
    return ret;
}

static long ioctl_launch(struct gpgpu_dev *dev, void __user *arg)
{
    struct gpgpu_launch_data data;

    if (copy_from_user(&data, arg, sizeof(data)))
        return -EFAULT;

    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ADDR_LO, data.kernel_addr);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ADDR_HI, 0);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ARGS_LO, data.args_addr);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ARGS_HI, 0);

    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_X, data.grid[0]);
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_Y, data.grid[1]);
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_Z, data.grid[2]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_X, data.block[0]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_Y, data.block[1]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_Z, data.block[2]);

    gpgpu_reg_write(dev, GPGPU_REG_DISPATCH, 1);

    return gpgpu_reg_read(dev, GPGPU_REG_ERROR_STATUS) ? -EIO : 0;
}

static long gpgpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct gpgpu_dev *dev = gpgpu_get_dev();

    if (!dev)
        return -ENODEV;

    switch (cmd) {
    case GPGPU_IOCTL_GET_INFO:
        return ioctl_get_info(dev, (void __user *)arg);
    case GPGPU_IOCTL_MEMCPY_H2D:
        return ioctl_memcpy_h2d(dev, (void __user *)arg);
    case GPGPU_IOCTL_MEMCPY_D2H:
        return ioctl_memcpy_d2h(dev, (void __user *)arg);
    case GPGPU_IOCTL_LAUNCH:
        return ioctl_launch(dev, (void __user *)arg);
    default:
        return -ENOTTY;
    }
}

static int gpgpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct gpgpu_dev *dev = gpgpu_get_dev();
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long off = vma->vm_pgoff << PAGE_SHIFT;
    unsigned long pfn;

    if (!dev)
        return -ENODEV;
    if (!gpgpu_range_ok(dev, off, size))
        return -EINVAL;

    pfn = (dev->bar2_phys + off) >> PAGE_SHIFT;
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
#else
    vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
#endif

    return io_remap_pfn_range(vma, vma->vm_start, pfn, size,
                              vma->vm_page_prot);
}

static const struct file_operations gpgpu_fops = {
    .owner = THIS_MODULE,
    .open = gpgpu_open,
    .release = gpgpu_release,
    .unlocked_ioctl = gpgpu_ioctl,
    .mmap = gpgpu_mmap,
};

int gpgpu_char_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&gpgpu_devt, 0, 1, "gpgpu");
    if (ret)
        return ret;

    cdev_init(&gpgpu_cdev, &gpgpu_fops);
    ret = cdev_add(&gpgpu_cdev, gpgpu_devt, 1);
    if (ret)
        goto err_unregister;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    gpgpu_class = class_create("gpgpu");
#else
    gpgpu_class = class_create(THIS_MODULE, "gpgpu");
#endif
    if (IS_ERR(gpgpu_class)) {
        ret = PTR_ERR(gpgpu_class);
        goto err_cdev;
    }

    if (IS_ERR(device_create(gpgpu_class, NULL, gpgpu_devt, NULL, "gpgpu"))) {
        ret = -ENODEV;
        goto err_class;
    }

    return 0;

err_class:
    class_destroy(gpgpu_class);
err_cdev:
    cdev_del(&gpgpu_cdev);
err_unregister:
    unregister_chrdev_region(gpgpu_devt, 1);
    return ret;
}

void gpgpu_char_exit(void)
{
    device_destroy(gpgpu_class, gpgpu_devt);
    class_destroy(gpgpu_class);
    cdev_del(&gpgpu_cdev);
    unregister_chrdev_region(gpgpu_devt, 1);
}
