/*
 * QEMU GPGPU - Vortex simx shim
 *
 * Build this as a shared object outside the normal QEMU build, then pass it to
 * QEMU with -device gpgpu,backend=vortex,vortex-lib=/path/to/libqemu-vortex-simx.so.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>

#include "hw/gpgpu/gpgpu_vortex_abi.h"

/*
 * Vortex currently keeps vx_device in runtime/simx/vortex.cpp rather than a
 * public C++ header.  Including the implementation gives this shim direct
 * access to the same runtime wrapper used by libvortex-simx.so.
 */
#include "runtime/simx/vortex.cpp"

namespace {

struct QemuVortexSim {
    vx_device device;
    void *opaque = nullptr;
    VortexQEMUDoneCB done_cb = nullptr;
    uint64_t reserved_base = 0;
    uint64_t reserved_size = 0;
    bool reserved = false;
};

int copy_qemu_vram_to_vortex(QemuVortexSim *sim,
                             const VortexQEMUDispatch *dispatch)
{
    if (dispatch->vram_size == 0) {
        return 0;
    }

    if (!sim->reserved ||
        sim->reserved_base != dispatch->vram_base ||
        sim->reserved_size != dispatch->vram_size) {
        int ret = sim->device.mem_reserve(dispatch->vram_base,
                                          dispatch->vram_size,
                                          VX_MEM_READ_WRITE);
        if (ret != 0) {
            return ret;
        }
        sim->reserved_base = dispatch->vram_base;
        sim->reserved_size = dispatch->vram_size;
        sim->reserved = true;
    }

    return sim->device.upload(dispatch->vram_base,
                              dispatch->vram,
                              dispatch->vram_size);
}

int copy_vortex_vram_to_qemu(QemuVortexSim *sim,
                             const VortexQEMUDispatch *dispatch)
{
    if (dispatch->vram_size == 0) {
        return 0;
    }

    return sim->device.download(dispatch->vram,
                                dispatch->vram_base,
                                dispatch->vram_size);
}

} // namespace

extern "C" void *vortex_qemu_create(const VortexQEMUConfig *cfg)
{
    if (cfg == nullptr || cfg->abi_version != GPGPU_VORTEX_ABI_VERSION) {
        return nullptr;
    }

    try {
        auto *sim = new QemuVortexSim();
        sim->opaque = cfg->opaque;
        sim->done_cb = cfg->done_cb;
        if (sim->device.init() != 0) {
            delete sim;
            return nullptr;
        }
        if (sim->device.dcr_write(VX_DCR_BASE_MPM_CLASS, 0) != 0) {
            delete sim;
            return nullptr;
        }
        return sim;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "qemu-vortex-simx: create failed: %s\n", e.what());
        return nullptr;
    } catch (...) {
        std::fprintf(stderr, "qemu-vortex-simx: create failed\n");
        return nullptr;
    }
}

extern "C" void vortex_qemu_destroy(void *handle)
{
    delete static_cast<QemuVortexSim *>(handle);
}

extern "C" int vortex_qemu_dispatch(void *handle,
                                    const VortexQEMUDispatch *dispatch)
{
    if (handle == nullptr ||
        dispatch == nullptr ||
        dispatch->abi_version != GPGPU_VORTEX_ABI_VERSION ||
        dispatch->vram == nullptr) {
        return -1;
    }

    auto *sim = static_cast<QemuVortexSim *>(handle);
    int status = -1;

    try {
        status = copy_qemu_vram_to_vortex(sim, dispatch);
        if (status != 0) {
            goto out;
        }

        status = sim->device.start(dispatch->kernel_pc, dispatch->kernel_args);
        if (status != 0) {
            goto out;
        }

        status = sim->device.ready_wait(VX_MAX_TIMEOUT);
        if (status != 0) {
            goto out;
        }

        status = copy_vortex_vram_to_qemu(sim, dispatch);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "qemu-vortex-simx: dispatch failed: %s\n",
                     e.what());
        status = -1;
    } catch (...) {
        std::fprintf(stderr, "qemu-vortex-simx: dispatch failed\n");
        status = -1;
    }

out:
    if (sim->done_cb != nullptr) {
        sim->done_cb(sim->opaque, status);
    }
    return status;
}
