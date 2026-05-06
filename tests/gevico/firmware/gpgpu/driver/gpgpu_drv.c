#include "gpgpu_drv.h"
#include "pci.h"
#include "../common/gpgpu_regs.h"

extern void printf(const char *fmt, ...);

/* PCI MMIO pool base (GPEX below-4G window) */
#define PCI_MMIO_BASE  0x40000000UL

static uint32_t bar_sizes[6];
static uint32_t next_mmio = PCI_MMIO_BASE;

static void pci_probe_bars(pci_bdf_t bdf)
{
    for (int i = 0; i < 6; i++) {
        uint8_t offset = PCI_CFG_BAR0 + i * 4;
        uint32_t old = pci_cfg_read32(bdf, offset);
        pci_cfg_write32(bdf, offset, 0xFFFFFFFF);
        uint32_t val = pci_cfg_read32(bdf, offset);
        pci_cfg_write32(bdf, offset, old);

        if (val == 0 || val == 0xFFFFFFFF) {
            bar_sizes[i] = 0;
            continue;
        }

        /* Mask off flag bits */
        uint32_t mask = (val & 1) ? 0xFFFFFFFC : 0xFFFFFFF0;
        uint32_t size = ~(val & mask) + 1;
        bar_sizes[i] = size;
    }
}

static uint64_t pci_assign_bar(pci_bdf_t bdf, int bar_idx)
{
    uint32_t size = bar_sizes[bar_idx];
    if (size == 0) return 0;

    /* Align to size (must be power of 2) */
    next_mmio = (next_mmio + size - 1) & ~(uint64_t)(size - 1);
    uint64_t addr = next_mmio;
    next_mmio += size;

    uint8_t offset = PCI_CFG_BAR0 + bar_idx * 4;
    uint32_t lo = pci_cfg_read32(bdf, offset);
    int is64 = ((lo & 0x7) == 0x4);

    pci_cfg_write32(bdf, offset, (uint32_t)addr);
    if (is64) {
        pci_cfg_write32(bdf, offset + 4, (uint32_t)(addr >> 32));
    }

    return addr;
}

int gpgpu_drv_init(gpgpu_dev_t *dev)
{
    pci_bdf_t bdf;
    if (pci_find_device(GPGPU_VENDOR_ID, GPGPU_DEVICE_ID, &bdf) != 0) {
        printf("PCI: GPGPU device not found\n");
        return -1;
    }

    printf("PCI: Found GPGPU at %d:%d:%d\n", bdf.bus, bdf.dev, bdf.func);

    /* Probe BAR sizes */
    pci_probe_bars(bdf);

    /* Assign BAR addresses from MMIO pool */
    uint64_t bar0_addr = pci_assign_bar(bdf, 0);
    uint64_t bar2_addr = pci_assign_bar(bdf, 2);

    printf("PCI: BAR0=0x%x (size=0x%x) BAR2=0x%x (size=0x%x)\n",
           (uint32_t)bar0_addr, bar_sizes[0],
           (uint32_t)bar2_addr, bar_sizes[2]);

    /* Enable Memory Space access */
    pci_enable_device(bdf);

    dev->bar0 = (volatile uint32_t *)(uintptr_t)bar0_addr;
    dev->bar2 = (volatile uint8_t *)(uintptr_t)bar2_addr;

    /* Read device capabilities */
    uint32_t caps = gpgpu_reg_read(dev, GPGPU_REG_DEV_CAPS);
    dev->num_cus = caps & 0xFF;
    dev->warps_per_cu = (caps >> 8) & 0xFF;
    dev->warp_size = (caps >> 16) & 0xFF;

    uint32_t vram_lo = gpgpu_reg_read(dev, GPGPU_REG_VRAM_SIZE_LO);
    uint32_t vram_hi = gpgpu_reg_read(dev, GPGPU_REG_VRAM_SIZE_HI);
    dev->vram_size = ((uint64_t)vram_hi << 32) | vram_lo;

    printf("GPGPU: %d CUs, %d warps/CU, %d threads/warp, %dMB VRAM\n",
           dev->num_cus, dev->warps_per_cu, dev->warp_size,
           (int)(dev->vram_size / (1024 * 1024)));

    return 0;
}

uint32_t gpgpu_reg_read(gpgpu_dev_t *dev, uint32_t offset)
{
    return dev->bar0[offset / 4];
}

void gpgpu_reg_write(gpgpu_dev_t *dev, uint32_t offset, uint32_t val)
{
    dev->bar0[offset / 4] = val;
}

void gpgpu_vram_write(gpgpu_dev_t *dev, uint32_t offset,
                      const void *src, size_t n)
{
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        dev->bar2[offset + i] = s[i];
    }
}

void gpgpu_vram_read(gpgpu_dev_t *dev, uint32_t offset,
                     void *dst, size_t n)
{
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = dev->bar2[offset + i];
    }
}

void gpgpu_enable(gpgpu_dev_t *dev)
{
    gpgpu_reg_write(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
}

void gpgpu_reset(gpgpu_dev_t *dev)
{
    gpgpu_reg_write(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
}

uint32_t gpgpu_get_status(gpgpu_dev_t *dev)
{
    return gpgpu_reg_read(dev, GPGPU_REG_GLOBAL_STATUS);
}

void gpgpu_dispatch(gpgpu_dev_t *dev, uint32_t kernel_addr,
                    uint32_t args_addr, uint32_t grid[3], uint32_t block[3])
{
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ADDR_LO, kernel_addr);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ADDR_HI, 0);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ARGS_LO, args_addr);
    gpgpu_reg_write(dev, GPGPU_REG_KERNEL_ARGS_HI, 0);

    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_X, grid[0]);
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_Y, grid[1]);
    gpgpu_reg_write(dev, GPGPU_REG_GRID_DIM_Z, grid[2]);

    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_X, block[0]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_Y, block[1]);
    gpgpu_reg_write(dev, GPGPU_REG_BLOCK_DIM_Z, block[2]);

    gpgpu_reg_write(dev, GPGPU_REG_DISPATCH, 1);
}
