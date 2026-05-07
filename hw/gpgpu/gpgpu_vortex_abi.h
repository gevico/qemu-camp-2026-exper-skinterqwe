/*
 * QEMU GPGPU - Vortex simx C ABI
 *
 * This header is the stable boundary between QEMU C code and an out-of-tree
 * C++ shim that owns Vortex simx/CoreSim objects.
 */

#ifndef HW_GPGPU_VORTEX_ABI_H
#define HW_GPGPU_VORTEX_ABI_H

#include <stdint.h>

#define GPGPU_VORTEX_ABI_VERSION 1

typedef void (*VortexQEMUDoneCB)(void *opaque, int status);

typedef struct VortexQEMUConfig {
    uint32_t abi_version;
    uint32_t num_cus;
    uint32_t warps_per_cu;
    uint32_t warp_size;
    uint64_t vram_size;
    void *opaque;
    VortexQEMUDoneCB done_cb;
} VortexQEMUConfig;

typedef struct VortexQEMUDispatch {
    uint32_t abi_version;
    uint64_t kernel_pc;
    uint64_t kernel_args;
    uint64_t vram_base;
    uint32_t grid_dim[3];
    uint32_t block_dim[3];
    uint32_t shared_mem_size;
    uint8_t *vram;
    uint64_t vram_size;
} VortexQEMUDispatch;

typedef void *(*VortexQEMUCreateFn)(const VortexQEMUConfig *cfg);
typedef void (*VortexQEMUDestroyFn)(void *sim);
typedef int (*VortexQEMUDispatchFn)(void *sim,
                                    const VortexQEMUDispatch *dispatch);

#endif /* HW_GPGPU_VORTEX_ABI_H */
