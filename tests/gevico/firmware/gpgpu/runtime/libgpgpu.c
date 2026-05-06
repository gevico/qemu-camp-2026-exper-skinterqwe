#include "libgpgpu.h"
#include "../driver/gpgpu_drv.h"
#include "../common/gpgpu_regs.h"

static gpgpu_dev_t g_dev;
static uint32_t g_vram_next;
static int g_initialized = 0;

int gpgpuInit(void)
{
    if (g_initialized) return 0;
    if (gpgpu_drv_init(&g_dev) != 0) return -1;
    gpgpu_enable(&g_dev);
    g_vram_next = GPGPU_DATA_BASE;
    g_initialized = 1;
    return 0;
}

void gpgpuGetDeviceInfo(gpgpuDeviceInfo *info)
{
    info->num_cus = g_dev.num_cus;
    info->warps_per_cu = g_dev.warps_per_cu;
    info->warp_size = g_dev.warp_size;
    info->vram_size = g_dev.vram_size;
}

uint32_t gpgpuMalloc(size_t size)
{
    size = (size + 3) & ~3;
    uint32_t addr = g_vram_next;
    g_vram_next += size;
    return addr;
}

void gpgpuMemcpyH2D(uint32_t dst_vram, const void *src_host, size_t n)
{
    gpgpu_vram_write(&g_dev, dst_vram, src_host, n);
}

void gpgpuMemcpyD2H(void *dst_host, uint32_t src_vram, size_t n)
{
    gpgpu_vram_read(&g_dev, src_vram, dst_host, n);
}

void gpgpuFree(uint32_t ptr)
{
    (void)ptr;
}

void gpgpuLaunchKernel(const uint32_t *code, size_t code_words,
                       dim3 grid, dim3 block,
                       uint32_t *args, int num_args)
{
    gpgpu_vram_write(&g_dev, GPGPU_KERNEL_CODE_BASE, code,
                     code_words * sizeof(uint32_t));

    uint32_t args_vram = GPGPU_KERNEL_ARGS_BASE;
    gpgpu_vram_write(&g_dev, args_vram, args,
                     num_args * sizeof(uint32_t));

    uint32_t g[3] = { grid.x, grid.y, grid.z };
    uint32_t b[3] = { block.x, block.y, block.z };
    gpgpu_dispatch(&g_dev, GPGPU_KERNEL_CODE_BASE, args_vram, g, b);
}

void gpgpuDeviceSynchronize(void)
{
    /* Synchronous — kernel already completed in DISPATCH */
}
