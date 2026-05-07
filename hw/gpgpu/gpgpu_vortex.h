/*
 * QEMU GPGPU - Vortex simx backend loader
 */

#ifndef HW_GPGPU_VORTEX_H
#define HW_GPGPU_VORTEX_H

#include "qapi/error.h"

typedef struct GPGPUState GPGPUState;

bool gpgpu_vortex_init(GPGPUState *s, Error **errp);
void gpgpu_vortex_cleanup(GPGPUState *s);
int gpgpu_vortex_exec_kernel(GPGPUState *s);

#endif /* HW_GPGPU_VORTEX_H */
