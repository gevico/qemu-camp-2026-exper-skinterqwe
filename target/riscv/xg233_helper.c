/*
 * RISC-V Xg233ai Extension Helpers for QEMU.
 *
 * Copyright (c) 2026 Gevico
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/memop.h"
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-ldst.h"

void HELPER(vrelu)(CPURISCVState *env, target_ulong dst,
                   target_ulong src, target_ulong n)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    MemOpIdx oi = make_memop_idx(MO_TESL, mmu_idx);

    for (target_ulong i = 0; i < n; i++) {
        target_ulong addr = src + i * 4;
        int32_t val = cpu_ldl_mmu(env, addr, oi, ra);
        val = (val > 0) ? val : 0;
        cpu_stl_mmu(env, dst + i * 4, val, oi, ra);
    }
}

void HELPER(xg233_vadd)(CPURISCVState *env, target_ulong dst,
                        target_ulong src1, target_ulong src2)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    MemOpIdx oi = make_memop_idx(MO_TESL, mmu_idx);
    target_ulong n = 16;

    for (target_ulong i = 0; i < n; i++) {
        int32_t val1 = cpu_ldl_mmu(env, src1 + i * 4, oi, ra);
        int32_t val2 = cpu_ldl_mmu(env, src2 + i * 4, oi, ra);
        int32_t result = val1 + val2;
        cpu_stl_mmu(env, dst + i * 4, result, oi, ra);
    }
}

void HELPER(xg233_vscale)(CPURISCVState *env, target_ulong dst,
                         target_ulong src, target_ulong scale)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    MemOpIdx oi = make_memop_idx(MO_TESL, mmu_idx);
    target_ulong n = 16;

    for (target_ulong i = 0; i < n; i++) {
        int32_t val = cpu_ldl_mmu(env, src + i * 4, oi, ra);
        int64_t result = (int64_t)val * (int64_t)scale;
        int32_t result32 = (int32_t)result;
        cpu_stl_mmu(env, dst + i * 4, result32, oi, ra);
    }
}

void HELPER(xg233_vmax)(CPURISCVState *env, target_ulong dst,
                      target_ulong src, target_ulong n)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    MemOpIdx oi = make_memop_idx(MO_TESL, mmu_idx);

    int32_t max_val = cpu_ldl_mmu(env, src, oi, ra);
    for (target_ulong i = 1; i < n; i++) {
        int32_t val = cpu_ldl_mmu(env, src + i * 4, oi, ra);
        if (val > max_val) {
            max_val = val;
        }
    }
    cpu_stl_mmu(env, dst, (int64_t)(int32_t)max_val, oi, ra);
}

void HELPER(xg233_sort)(CPURISCVState *env, target_ulong k,
                       target_ulong arr, target_ulong n)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    MemOpIdx oi = make_memop_idx(MO_TESL, mmu_idx);

    int32_t *local_arr = g_new(int32_t, n);
    for (target_ulong i = 0; i < n; i++) {
        local_arr[i] = cpu_ldl_mmu(env, arr + i * 4, oi, ra);
    }

    for (target_ulong i = 0; i < k - 1; i++) {
        for (target_ulong j = 0; j < k - i - 1; j++) {
            if (local_arr[j] > local_arr[j + 1]) {
                int32_t tmp = local_arr[j];
                local_arr[j] = local_arr[j + 1];
                local_arr[j + 1] = tmp;
            }
        }
    }

    for (target_ulong i = 0; i < n; i++) {
        cpu_stl_mmu(env, arr + i * 4, local_arr[i], oi, ra);
    }

    g_free(local_arr);
}

void HELPER(xg233_crush)(CPURISCVState *env, target_ulong dst,
                         target_ulong src, target_ulong n)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);

    target_ulong out_len = (n + 1) / 2;

    for (target_ulong i = 0; i < n / 2; i++) {
        uint8_t val1 = cpu_ldub_mmuidx_ra(env, src + 2 * i, mmu_idx, ra);
        uint8_t val2 = cpu_ldub_mmuidx_ra(env, src + 2 * i + 1, mmu_idx, ra);
        uint8_t packed = (val1 & 0x0F) | ((val2 & 0x0F) << 4);
        cpu_stb_mmu(env, dst + i, packed, mmu_idx, ra);
    }

    if (n & 1) {
        uint8_t val = cpu_ldub_mmuidx_ra(env, src + n - 1, mmu_idx, ra);
        cpu_stb_mmu(env, dst + out_len - 1, val & 0x0F, mmu_idx, ra);
    }
}

void HELPER(xg233_expand)(CPURISCVState *env, target_ulong dst,
                          target_ulong src, target_ulong n)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);

    for (target_ulong i = 0; i < n; i++) {
        uint8_t val = cpu_ldub_mmuidx_ra(env, src + i, mmu_idx, ra);
        cpu_stb_mmu(env, dst + 2 * i, val & 0x0F, mmu_idx, ra);
        cpu_stb_mmu(env, dst + 2 * i + 1, (val >> 4) & 0x0F, mmu_idx, ra);
    }
}

void HELPER(xg233_vdot)(CPURISCVState *env, target_ulong dst,
                        target_ulong src1, target_ulong src2)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    target_ulong n = 16;

    int64_t acc = 0;
    for (target_ulong i = 0; i < n; i++) {
        int32_t val1 = cpu_ldl_mmu(env, src1 + i * 4, mmu_idx, ra);
        int32_t val2 = cpu_ldl_mmu(env, src2 + i * 4, mmu_idx, ra);
        acc += (int64_t)val1 * (int64_t)val2;
    }
    cpu_stq_mmu(env, dst, acc, mmu_idx, ra);
}

void HELPER(xg233_gemm)(CPURISCVState *env, target_ulong dst,
                        target_ulong src1, target_ulong src2)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);
    int dim = 4;

    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            int64_t acc = 0;
            for (int k = 0; k < dim; k++) {
                int32_t val_a = cpu_ldl_mmu(env, src1 + (i * dim + k) * 4, mmu_idx, ra);
                int32_t val_b = cpu_ldl_mmu(env, src2 + (k * dim + j) * 4, mmu_idx, ra);
                acc += (int64_t)val_a * (int64_t)val_b;
            }
            cpu_stl_mmu(env, dst + (i * dim + j) * 4, (int32_t)acc, mmu_idx, ra);
        }
    }
}

void HELPER(xg233_dma)(CPURISCVState *env, target_ulong dst,
                       target_ulong src, target_ulong grain)
{
    uintptr_t ra = GETPC();
    CPUState *cs = env_cpu(env);
    int mmu_idx = cpu_mmu_index(cs, false);

    int n;
    switch (grain) {
    case 0: n = 8; break;
    case 1: n = 16; break;
    case 2: n = 32; break;
    default: return;
    }

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            uint32_t val = cpu_ldl_mmu(env, src + (i * n + j) * 4, mmu_idx, ra);
            cpu_stl_mmu(env, dst + (j * n + i) * 4, val, mmu_idx, ra);
        }
    }
}