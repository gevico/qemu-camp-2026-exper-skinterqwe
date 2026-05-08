/*
 * QEMU GPGPU - Vortex simx shim
 *
 * Build this as a shared object outside the normal QEMU build, then pass it to
 * QEMU with -device gpgpu,backend=vortex,vortex-lib=/path/to/libqemu-vortex-simx.so.
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include <mem.h>

#include "hw/gpgpu/gpgpu_vortex_abi.h"

/*
 * Vortex currently keeps vx_device in runtime/simx/vortex.cpp rather than a
 * public C++ header.  Including the implementation gives this shim direct
 * access to the same runtime wrapper used by libvortex-simx.so.
 */
#include "runtime/simx/vortex.cpp"
#undef private

namespace {

struct MappedRamPage {
    uint64_t index = 0;
    uint8_t *previous = nullptr;
    bool had_previous = false;
};

struct QemuVortexSim {
    ~QemuVortexSim()
    {
        unmap_qemu_vram();
    }

    void unmap_qemu_vram()
    {
        for (const auto &page : mapped_pages) {
            if (page.had_previous) {
                device.ram_.pages_[page.index] = page.previous;
            } else {
                device.ram_.pages_.erase(page.index);
            }
        }
        mapped_pages.clear();
        device.ram_.last_page_ = nullptr;
        device.ram_.last_page_index_ = 0;
        mapped_base = 0;
        mapped_size = 0;
        mapped_host = nullptr;
    }

    vx_device device;
    void *opaque = nullptr;
    VortexQEMUDoneCB done_cb = nullptr;
    uint64_t reserved_base = 0;
    uint64_t reserved_size = 0;
    bool reserved = false;
    uint64_t mapped_base = 0;
    uint64_t mapped_size = 0;
    uint8_t *mapped_host = nullptr;
    std::vector<MappedRamPage> mapped_pages;
};

int reserve_qemu_vram(QemuVortexSim *sim, const VortexQEMUDispatch *dispatch)
{
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

    return 0;
}

int map_qemu_vram(QemuVortexSim *sim, const VortexQEMUDispatch *dispatch)
{
    if (dispatch->vram_size == 0) {
        return 0;
    }

    if (sim->mapped_base == dispatch->vram_base &&
        sim->mapped_size == dispatch->vram_size &&
        sim->mapped_host == dispatch->vram) {
        return 0;
    }

    uint64_t page_size = uint64_t{1} << sim->device.ram_.page_bits_;
    if ((dispatch->vram_base & (page_size - 1)) != 0 ||
        (dispatch->vram_size & (page_size - 1)) != 0) {
        return -1;
    }

    sim->unmap_qemu_vram();

    uint8_t *host = static_cast<uint8_t *>(dispatch->vram);
    for (uint64_t off = 0; off < dispatch->vram_size; off += page_size) {
        uint64_t index = (dispatch->vram_base + off) >> sim->device.ram_.page_bits_;
        auto it = sim->device.ram_.pages_.find(index);
        MappedRamPage page = {
            .index = index,
            .previous = it == sim->device.ram_.pages_.end() ? nullptr : it->second,
            .had_previous = it != sim->device.ram_.pages_.end(),
        };

        sim->mapped_pages.push_back(page);
        sim->device.ram_.pages_[index] = host + off;
    }

    sim->device.ram_.last_page_ = nullptr;
    sim->device.ram_.last_page_index_ = 0;
    sim->mapped_base = dispatch->vram_base;
    sim->mapped_size = dispatch->vram_size;
    sim->mapped_host = host;
    return 0;
}

int prepare_qemu_vram(QemuVortexSim *sim, const VortexQEMUDispatch *dispatch)
{
    int ret = reserve_qemu_vram(sim, dispatch);

    if (ret != 0) {
        return ret;
    }
    return map_qemu_vram(sim, dispatch);
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
        status = prepare_qemu_vram(sim, dispatch);
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
