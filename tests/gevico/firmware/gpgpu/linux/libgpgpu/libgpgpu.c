#include "libgpgpu.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../common/gpgpu_regs.h"
#include "gpgpu_ioctl.h"

static int g_fd = -1;
static uint8_t *g_vram;
static uint64_t g_vram_size;
static uint32_t g_vram_next;
static gpgpuDeviceInfo g_info;

static int gpgpu_check_range(uint32_t off, size_t size)
{
    return g_vram && off <= g_vram_size && size <= g_vram_size - off;
}

int gpgpuInit(void)
{
    struct gpgpu_info_data info;

    if (g_fd >= 0)
        return 0;

    g_fd = open("/dev/gpgpu", O_RDWR);
    if (g_fd < 0)
        return -1;

    if (ioctl(g_fd, GPGPU_IOCTL_GET_INFO, &info) < 0)
        goto err_close;

    g_vram_size = info.vram_size;
    g_vram = mmap(NULL, g_vram_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  g_fd, 0);
    if (g_vram == MAP_FAILED) {
        g_vram = NULL;
        goto err_close;
    }

    g_info.num_cus = info.num_cus;
    g_info.warps_per_cu = info.warps_per_cu;
    g_info.warp_size = info.warp_size;
    g_info.vram_size = info.vram_size;
    g_vram_next = GPGPU_DATA_BASE;

    return 0;

err_close:
    close(g_fd);
    g_fd = -1;
    return -1;
}

void gpgpuShutdown(void)
{
    if (g_vram) {
        munmap(g_vram, g_vram_size);
        g_vram = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_vram_size = 0;
    g_vram_next = 0;
}

void gpgpuGetDeviceInfo(gpgpuDeviceInfo *info)
{
    if (info)
        *info = g_info;
}

uint32_t gpgpuMalloc(size_t size)
{
    uint32_t addr;

    size = (size + 3) & ~3u;
    if (!g_vram || g_vram_next > g_vram_size ||
        size > g_vram_size - g_vram_next) {
        errno = ENOMEM;
        return 0;
    }

    addr = g_vram_next;
    g_vram_next += size;
    return addr;
}

void gpgpuMemcpyH2D(uint32_t dst_vram, const void *src_host, size_t n)
{
    if (!gpgpu_check_range(dst_vram, n) || !src_host)
        return;

    memcpy(g_vram + dst_vram, src_host, n);
    __sync_synchronize();
}

void gpgpuMemcpyD2H(void *dst_host, uint32_t src_vram, size_t n)
{
    if (!gpgpu_check_range(src_vram, n) || !dst_host)
        return;

    __sync_synchronize();
    memcpy(dst_host, g_vram + src_vram, n);
}

void gpgpuFree(uint32_t ptr)
{
    (void)ptr;
}

void gpgpuLaunchKernel(const uint32_t *code, size_t code_words,
                       dim3 grid, dim3 block,
                       uint32_t *args, int num_args)
{
    struct gpgpu_launch_data launch;
    size_t code_size = code_words * sizeof(uint32_t);
    size_t args_size = (size_t)num_args * sizeof(uint32_t);

    if (!g_vram || g_fd < 0 || !code || !args)
        return;
    if (code_size > GPGPU_KERNEL_CODE_MAX)
        return;

    memcpy(g_vram + GPGPU_KERNEL_CODE_BASE, code, code_size);
    memcpy(g_vram + GPGPU_KERNEL_ARGS_BASE, args, args_size);
    __sync_synchronize();

    memset(&launch, 0, sizeof(launch));
    launch.kernel_addr = GPGPU_KERNEL_CODE_BASE;
    launch.args_addr = GPGPU_KERNEL_ARGS_BASE;
    launch.grid[0] = grid.x;
    launch.grid[1] = grid.y;
    launch.grid[2] = grid.z;
    launch.block[0] = block.x;
    launch.block[1] = block.y;
    launch.block[2] = block.z;

    ioctl(g_fd, GPGPU_IOCTL_LAUNCH, &launch);
    __sync_synchronize();
}

void gpgpuDeviceSynchronize(void)
{
    __sync_synchronize();
}
