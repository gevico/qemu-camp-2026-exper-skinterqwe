/*
 * RISC-V translation routines for the Xg233ai ISA extensions
 *
 * Copyright (c) 2026 Gevico
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define REQUIRE_XG233(ctx) do {               \
    if (!ctx->cfg_ptr->ext_xg233) {           \
        return false;                         \
    }                                         \
} while (0)

static bool trans_vrelu(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_vrelu(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_vadd(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_vadd(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_vscale(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_vscale(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_vmax(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv_i64 ret = tcg_temp_new_i64();
    gen_helper_xg233_vmax(ret, tcg_env, cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    tcg_gen_mov_i64(cpu_gpr[a->rd], ret);
    return true;
}

static bool trans_sort(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_sort(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_crush(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_crush(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_expand(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_expand(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_vdot(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv_i64 ret = tcg_temp_new_i64();
    gen_helper_xg233_vdot(ret, tcg_env, cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    tcg_gen_mov_i64(cpu_gpr[a->rd], ret);
    return true;
}

static bool trans_gemm(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_gemm(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}

static bool trans_dma(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    gen_helper_xg233_dma(tcg_env, cpu_gpr[a->rd], cpu_gpr[a->rs1], cpu_gpr[a->rs2]);
    return true;
}