#ifndef GPGPU_PCI_H
#define GPGPU_PCI_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/types.h>

struct gpgpu_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
    resource_size_t bar0_size;
    resource_size_t bar2_phys;
    resource_size_t bar2_size;

    u32 num_cus;
    u32 warps_per_cu;
    u32 warp_size;
    u64 vram_size;
};

int gpgpu_pci_register(void);
void gpgpu_pci_unregister(void);

static inline u32 gpgpu_reg_read(struct gpgpu_dev *dev, u32 off)
{
    return ioread32(dev->bar0 + off);
}

static inline void gpgpu_reg_write(struct gpgpu_dev *dev, u32 off, u32 val)
{
    iowrite32(val, dev->bar0 + off);
}

#endif /* GPGPU_PCI_H */
