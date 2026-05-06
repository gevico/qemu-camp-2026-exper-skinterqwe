#ifndef GPGPU_DRV_H
#define GPGPU_DRV_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    volatile uint32_t *bar0;  /* Control registers */
    volatile uint8_t  *bar2;  /* VRAM */
    uint64_t           vram_size;
    uint32_t           num_cus;
    uint32_t           warps_per_cu;
    uint32_t           warp_size;
} gpgpu_dev_t;

/* Initialize GPGPU device (PCI enum + BAR mapping) */
int gpgpu_drv_init(gpgpu_dev_t *dev);

/* MMIO register access */
uint32_t gpgpu_reg_read(gpgpu_dev_t *dev, uint32_t offset);
void gpgpu_reg_write(gpgpu_dev_t *dev, uint32_t offset, uint32_t val);

/* VRAM access */
void gpgpu_vram_write(gpgpu_dev_t *dev, uint32_t offset,
                      const void *src, size_t n);
void gpgpu_vram_read(gpgpu_dev_t *dev, uint32_t offset,
                     void *dst, size_t n);

/* Device control */
void gpgpu_enable(gpgpu_dev_t *dev);
void gpgpu_reset(gpgpu_dev_t *dev);
uint32_t gpgpu_get_status(gpgpu_dev_t *dev);

/* Kernel dispatch */
void gpgpu_dispatch(gpgpu_dev_t *dev, uint32_t kernel_addr,
                    uint32_t args_addr, uint32_t grid[3], uint32_t block[3]);

#endif /* GPGPU_DRV_H */
