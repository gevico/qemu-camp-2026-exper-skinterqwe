#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "gpgpu_char.h"
#include "gpgpu_pci.h"
#include "gpgpu_regs.h"

static const struct pci_device_id gpgpu_pci_ids[] = {
    { PCI_DEVICE(GPGPU_VENDOR_ID, GPGPU_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, gpgpu_pci_ids);

static int gpgpu_pci_probe(struct pci_dev *pdev,
                           const struct pci_device_id *id)
{
    struct gpgpu_dev *dev;
    u32 caps;
    u32 vram_lo;
    u32 vram_hi;
    int ret;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->pdev = pdev;

    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free;

    ret = pci_request_regions(pdev, "gpgpu");
    if (ret)
        goto err_disable;

    dev->bar0 = pci_iomap(pdev, 0, 0);
    if (!dev->bar0) {
        ret = -ENOMEM;
        goto err_regions;
    }

    dev->bar0_size = pci_resource_len(pdev, 0);
    dev->bar2_phys = pci_resource_start(pdev, 2);
    dev->bar2_size = pci_resource_len(pdev, 2);
    if (!dev->bar2_phys || !dev->bar2_size) {
        ret = -ENODEV;
        goto err_iounmap;
    }

    pci_set_master(pdev);

    caps = gpgpu_reg_read(dev, GPGPU_REG_DEV_CAPS);
    vram_lo = gpgpu_reg_read(dev, GPGPU_REG_VRAM_SIZE_LO);
    vram_hi = gpgpu_reg_read(dev, GPGPU_REG_VRAM_SIZE_HI);

    dev->num_cus = caps & 0xff;
    dev->warps_per_cu = (caps >> 8) & 0xff;
    dev->warp_size = (caps >> 16) & 0xff;
    dev->vram_size = ((u64)vram_hi << 32) | vram_lo;
    if (!dev->vram_size || dev->vram_size > dev->bar2_size)
        dev->vram_size = dev->bar2_size;

    gpgpu_reg_write(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);

    pci_set_drvdata(pdev, dev);
    gpgpu_char_set_dev(dev);

    dev_info(&pdev->dev,
             "GPGPU ready: %u CUs, %u warps/CU, %u threads/warp, %llu bytes VRAM\n",
             dev->num_cus, dev->warps_per_cu, dev->warp_size,
             (unsigned long long)dev->vram_size);
    return 0;

err_iounmap:
    pci_iounmap(pdev, dev->bar0);
err_regions:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
err_free:
    kfree(dev);
    return ret;
}

static void gpgpu_pci_remove(struct pci_dev *pdev)
{
    struct gpgpu_dev *dev = pci_get_drvdata(pdev);

    gpgpu_char_set_dev(NULL);
    if (!dev)
        return;

    gpgpu_reg_write(dev, GPGPU_REG_GLOBAL_CTRL, 0);
    pci_iounmap(pdev, dev->bar0);
    pci_clear_master(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    kfree(dev);
}

static struct pci_driver gpgpu_pci_driver = {
    .name = "gpgpu",
    .id_table = gpgpu_pci_ids,
    .probe = gpgpu_pci_probe,
    .remove = gpgpu_pci_remove,
};

int gpgpu_pci_register(void)
{
    return pci_register_driver(&gpgpu_pci_driver);
}

void gpgpu_pci_unregister(void)
{
    pci_unregister_driver(&gpgpu_pci_driver);
}
