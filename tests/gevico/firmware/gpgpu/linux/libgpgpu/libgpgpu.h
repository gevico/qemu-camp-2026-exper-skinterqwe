#ifndef LINUX_LIB_GPGPU_H
#define LINUX_LIB_GPGPU_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t z;
} dim3;

typedef struct {
    uint32_t num_cus;
    uint32_t warps_per_cu;
    uint32_t warp_size;
    uint64_t vram_size;
} gpgpuDeviceInfo;

int gpgpuInit(void);
void gpgpuGetDeviceInfo(gpgpuDeviceInfo *info);

uint32_t gpgpuMalloc(size_t size);
void gpgpuMemcpyH2D(uint32_t dst_vram, const void *src_host, size_t n);
void gpgpuMemcpyD2H(void *dst_host, uint32_t src_vram, size_t n);
void gpgpuFree(uint32_t ptr);

void gpgpuLaunchKernel(const uint32_t *code, size_t code_words,
                       dim3 grid, dim3 block,
                       uint32_t *args, int num_args);
void gpgpuDeviceSynchronize(void);
void gpgpuShutdown(void);

#endif /* LINUX_LIB_GPGPU_H */
