/*
 * QEMU GPGPU - Vortex simx backend loader
 *
 * The real Vortex simulator is C++.  QEMU keeps this file in C and talks to a
 * small out-of-tree shared library through the ABI in gpgpu_vortex_abi.h.
 */

#include "qemu/osdep.h"
#include <gmodule.h>
#include "qapi/error.h"
#include "qemu/log.h"

#include "gpgpu.h"
#include "gpgpu_vortex.h"
#include "gpgpu_vortex_abi.h"

typedef struct GPGPUVortexBackend {
    GModule *module;
    void *sim;
    VortexQEMUDestroyFn destroy;
    VortexQEMUDispatchFn dispatch;
    bool done;
    int done_status;
} GPGPUVortexBackend;

static void gpgpu_vortex_done(void *opaque, int status)
{
    GPGPUVortexBackend *backend = opaque;

    backend->done = true;
    backend->done_status = status;
}

static bool gpgpu_vortex_symbol(GModule *module, const char *name,
                                gpointer *symbol, Error **errp)
{
    if (g_module_symbol(module, name, symbol)) {
        return true;
    }

    error_setg(errp, "GPGPU Vortex: missing symbol %s: %s",
               name, g_module_error());
    return false;
}

bool gpgpu_vortex_init(GPGPUState *s, Error **errp)
{
    const char *path = s->vortex_lib;
    VortexQEMUCreateFn create;
    GPGPUVortexBackend *backend;
    VortexQEMUConfig cfg;

    if (!path || !path[0]) {
        path = g_getenv("GPGPU_VORTEX_SIMX_LIB");
    }
    if (!path || !path[0]) {
        error_setg(errp, "GPGPU Vortex: set vortex-lib=PATH or "
                   "GPGPU_VORTEX_SIMX_LIB");
        return false;
    }

    backend = g_new0(GPGPUVortexBackend, 1);
    backend->module = g_module_open(path, G_MODULE_BIND_LOCAL);
    if (!backend->module) {
        error_setg(errp, "GPGPU Vortex: failed to open %s: %s",
                   path, g_module_error());
        g_free(backend);
        return false;
    }

    if (!gpgpu_vortex_symbol(backend->module, "vortex_qemu_create",
                             (gpointer *)&create, errp) ||
        !gpgpu_vortex_symbol(backend->module, "vortex_qemu_destroy",
                             (gpointer *)&backend->destroy, errp) ||
        !gpgpu_vortex_symbol(backend->module, "vortex_qemu_dispatch",
                             (gpointer *)&backend->dispatch, errp)) {
        g_module_close(backend->module);
        g_free(backend);
        return false;
    }

    cfg = (VortexQEMUConfig) {
        .abi_version = GPGPU_VORTEX_ABI_VERSION,
        .num_cus = s->num_cus,
        .warps_per_cu = s->warps_per_cu,
        .warp_size = s->warp_size,
        .vram_size = s->vram_size,
        .opaque = backend,
        .done_cb = gpgpu_vortex_done,
    };

    backend->sim = create(&cfg);
    if (!backend->sim) {
        error_setg(errp, "GPGPU Vortex: vortex_qemu_create failed");
        g_module_close(backend->module);
        g_free(backend);
        return false;
    }

    s->vortex_backend = backend;
    return true;
}

void gpgpu_vortex_cleanup(GPGPUState *s)
{
    GPGPUVortexBackend *backend = s->vortex_backend;

    if (!backend) {
        return;
    }

    if (backend->sim) {
        backend->destroy(backend->sim);
    }
    if (backend->module) {
        g_module_close(backend->module);
    }
    g_free(backend);
    s->vortex_backend = NULL;
}

int gpgpu_vortex_exec_kernel(GPGPUState *s)
{
    GPGPUVortexBackend *backend = s->vortex_backend;
    VortexQEMUDispatch dispatch;
    int ret;

    if (!backend) {
        return -1;
    }

    backend->done = false;
    backend->done_status = 0;

    dispatch = (VortexQEMUDispatch) {
        .abi_version = GPGPU_VORTEX_ABI_VERSION,
        .kernel_pc = s->kernel.kernel_addr,
        .kernel_args = s->kernel.kernel_args,
        .vram_base = s->vortex_vram_base,
        .grid_dim = {
            s->kernel.grid_dim[0] ?: 1,
            s->kernel.grid_dim[1] ?: 1,
            s->kernel.grid_dim[2] ?: 1,
        },
        .block_dim = {
            s->kernel.block_dim[0] ?: 1,
            s->kernel.block_dim[1] ?: 1,
            s->kernel.block_dim[2] ?: 1,
        },
        .shared_mem_size = s->kernel.shared_mem_size,
        .vram = s->vram_ptr,
        .vram_size = s->vram_size,
    };

    ret = backend->dispatch(backend->sim, &dispatch);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "GPGPU Vortex: dispatch failed: %d\n", ret);
        return -1;
    }

    if (backend->done && backend->done_status < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "GPGPU Vortex: kernel failed: %d\n",
                      backend->done_status);
        return -1;
    }

    return 0;
}
