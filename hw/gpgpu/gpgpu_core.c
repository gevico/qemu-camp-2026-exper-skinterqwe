/*
 * QEMU GPGPU - RISC-V SIMT Core Implementation
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "gpgpu.h"
#include "gpgpu_core.h"

void gpgpu_core_init_warp(GPGPUWarp *warp, uint32_t pc,
                          uint32_t thread_id_base, const uint32_t block_id[3],
                          uint32_t num_threads,
                          uint32_t warp_id, uint32_t block_id_linear)
{
    memset(warp, 0, sizeof(*warp));

    warp->warp_id = warp_id;
    warp->thread_id_base = thread_id_base;
    warp->block_id[0] = block_id[0];
    warp->block_id[1] = block_id[1];
    warp->block_id[2] = block_id[2];

    /* active_mask: 低 num_threads 位为 1 */
    if (num_threads >= GPGPU_WARP_SIZE) {
        warp->active_mask = 0xFFFFFFFF;
    } else {
        warp->active_mask = (1u << num_threads) - 1;
    }

    for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
        GPGPULane *lane = &warp->lanes[i];
        lane->pc = pc;
        lane->gpr[0] = 0;  /* x0 硬连线为 0 */
        lane->active = (uint32_t)i < num_threads;
        /* mhartid = block(19) | warp(8) | thread(5) */
        lane->mhartid = MHARTID_ENCODE(block_id_linear, warp_id,
                                       thread_id_base + i);
        /* 初始化浮点舍入模式为最近偶数 (IEEE 754 默认) */
        set_float_rounding_mode(float_round_nearest_even, &lane->fp_status);
    }
}

static uint32_t gpu_mem_read32(GPGPUState *s, uint32_t addr)
{
    if (addr < s->vram_size) {
        return (uint32_t)s->vram_ptr[addr]
             | ((uint32_t)s->vram_ptr[addr + 1] << 8)
             | ((uint32_t)s->vram_ptr[addr + 2] << 16)
             | ((uint32_t)s->vram_ptr[addr + 3] << 24);
    }
    return 0;
}

static void gpu_mem_write32(GPGPUState *s, uint32_t addr, uint32_t val)
{
    if (addr < s->vram_size) {
        s->vram_ptr[addr]     = (uint8_t)(val);
        s->vram_ptr[addr + 1] = (uint8_t)(val >> 8);
        s->vram_ptr[addr + 2] = (uint8_t)(val >> 16);
        s->vram_ptr[addr + 3] = (uint8_t)(val >> 24);
    }
}

static uint8_t gpu_mem_read8(GPGPUState *s, uint32_t addr)
{
    if (addr < s->vram_size) {
        return s->vram_ptr[addr];
    }
    return 0;
}

static uint16_t gpu_mem_read16(GPGPUState *s, uint32_t addr)
{
    if (addr + 1 < s->vram_size) {
        return (uint16_t)s->vram_ptr[addr]
             | ((uint16_t)s->vram_ptr[addr + 1] << 8);
    }
    return 0;
}

static void gpu_mem_write8(GPGPUState *s, uint32_t addr, uint8_t val)
{
    if (addr < s->vram_size) {
        s->vram_ptr[addr] = val;
    }
}

static void gpu_mem_write16(GPGPUState *s, uint32_t addr, uint16_t val)
{
    if (addr + 1 < s->vram_size) {
        s->vram_ptr[addr]     = (uint8_t)val;
        s->vram_ptr[addr + 1] = (uint8_t)(val >> 8);
    }
}

int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles)
{
    uint32_t cycle = 0;

    while (cycle < max_cycles) {
        if (warp->active_mask == 0) {
            return 0;
        }

        for (int i = 0; i < GPGPU_WARP_SIZE; i++) {
            if (!(warp->active_mask & (1u << i))) {
                continue;
            }

            GPGPULane *lane = &warp->lanes[i];
            uint32_t inst = gpu_mem_read32(s, lane->pc);
            uint32_t opcode = inst & 0x7F;
            uint32_t rd     = (inst >> 7) & 0x1F;
            uint32_t funct3 = (inst >> 12) & 0x7;
            uint32_t rs1    = (inst >> 15) & 0x1F;
            uint32_t rs2    = (inst >> 20) & 0x1F;
            uint32_t funct7 = (inst >> 25) & 0x7F;
            uint32_t npc = lane->pc + 4;

            switch (opcode) {
            /* ---- U-type ---- */
            case 0x37: /* LUI */
                lane->gpr[rd] = inst & 0xFFFFF000;
                break;
            case 0x17: /* AUIPC */
                lane->gpr[rd] = lane->pc + (inst & 0xFFFFF000);
                break;

            /* ---- J-type ---- */
            case 0x6F: { /* JAL */
                int32_t off = ((int32_t)(inst & 0x80000000) >> 11)
                            | (inst & 0xFF000)
                            | ((inst >> 9) & 0x800)
                            | ((inst >> 20) & 0x7FE);
                lane->gpr[rd] = lane->pc + 4;
                npc = lane->pc + off;
                break;
            }

            /* ---- I-type: JALR ---- */
            case 0x67: { /* JALR */
                int32_t imm = (int32_t)inst >> 20;
                uint32_t target = (lane->gpr[rs1] + imm) & ~1u;
                lane->gpr[rd] = lane->pc + 4;
                npc = target;
                break;
            }

            /* ---- B-type ---- */
            case 0x63: {
                int32_t off = ((int32_t)(inst & 0x80000000) >> 19)
                            | ((inst & 0x80) << 4)
                            | ((inst >> 20) & 0x7E0)
                            | ((inst >> 7) & 0x1E);
                int32_t a = (int32_t)lane->gpr[rs1];
                int32_t b = (int32_t)lane->gpr[rs2];
                uint32_t ua = lane->gpr[rs1];
                uint32_t ub = lane->gpr[rs2];
                bool taken = false;
                switch (funct3) {
                case 0: taken = (a == b); break;   /* BEQ  */
                case 1: taken = (a != b); break;   /* BNE  */
                case 4: taken = (a <  b); break;   /* BLT  */
                case 5: taken = (a >= b); break;   /* BGE  */
                case 6: taken = (ua <  ub); break;  /* BLTU */
                case 7: taken = (ua >= ub); break;  /* BGEU */
                }
                if (taken) {
                    npc = lane->pc + off;
                }
                break;
            }

            /* ---- I-type: LOAD ---- */
            case 0x03: {
                int32_t imm = (int32_t)inst >> 20;
                uint32_t addr = lane->gpr[rs1] + imm;
                uint32_t val = 0;
                switch (funct3) {
                case 0: { /* LB */
                    uint8_t b = gpu_mem_read8(s, addr);
                    val = (uint32_t)(int32_t)(int8_t)b;
                    break;
                }
                case 1: { /* LH */
                    uint16_t h = gpu_mem_read16(s, addr);
                    val = (uint32_t)(int32_t)(int16_t)h;
                    break;
                }
                case 2:   /* LW */
                    val = gpu_mem_read32(s, addr);
                    break;
                case 4:   /* LBU */
                    val = gpu_mem_read8(s, addr);
                    break;
                case 5:   /* LHU */
                    val = gpu_mem_read16(s, addr);
                    break;
                }
                lane->gpr[rd] = val;
                break;
            }

            /* ---- S-type: STORE ---- */
            case 0x23: {
                int32_t off = ((int32_t)(inst & 0xFE000000) >> 20)
                            | ((inst >> 7) & 0x1F);
                uint32_t addr = lane->gpr[rs1] + off;
                switch (funct3) {
                case 0: /* SB */
                    gpu_mem_write8(s, addr, (uint8_t)lane->gpr[rs2]);
                    break;
                case 1: /* SH */
                    gpu_mem_write16(s, addr, (uint16_t)lane->gpr[rs2]);
                    break;
                case 2: /* SW */
                    gpu_mem_write32(s, addr, lane->gpr[rs2]);
                    break;
                }
                break;
            }

            /* ---- LOAD-FP: FLW (opcode=0x07) ---- */
            case 0x07: {
                int32_t imm = (int32_t)inst >> 20;
                uint32_t addr = lane->gpr[rs1] + imm;
                lane->fpr[rd] = gpu_mem_read32(s, addr);
                break;
            }

            /* ---- STORE-FP: FSW (opcode=0x27) ---- */
            case 0x27: {
                int32_t off = ((int32_t)(inst & 0xFE000000) >> 20)
                            | ((inst >> 7) & 0x1F);
                uint32_t addr = lane->gpr[rs1] + off;
                gpu_mem_write32(s, addr, lane->fpr[rs2]);
                break;
            }

            /* ---- I-type: OP-IMM ---- */
            case 0x13: {
                int32_t imm = (int32_t)inst >> 20;
                uint32_t uimm = (uint32_t)imm;
                uint32_t shamt = (inst >> 20) & 0x1F;
                switch (funct3) {
                case 0: lane->gpr[rd] = lane->gpr[rs1] + uimm; break;         /* ADDI  */
                case 1: lane->gpr[rd] = lane->gpr[rs1] << shamt; break;        /* SLLI  */
                case 2: lane->gpr[rd] = (int32_t)lane->gpr[rs1] < imm; break;  /* SLTI  */
                case 3: lane->gpr[rd] = lane->gpr[rs1] < uimm; break;          /* SLTIU */
                case 4: lane->gpr[rd] = lane->gpr[rs1] ^ uimm; break;          /* XORI  */
                case 6: lane->gpr[rd] = lane->gpr[rs1] | uimm; break;          /* ORI   */
                case 7: lane->gpr[rd] = lane->gpr[rs1] & uimm; break;          /* ANDI  */
                case 5: /* SRLI / SRAI */
                    if (funct7 & 0x20) {
                        lane->gpr[rd] = (uint32_t)((int32_t)lane->gpr[rs1] >> shamt); /* SRAI */
                    } else {
                        lane->gpr[rd] = lane->gpr[rs1] >> shamt;                       /* SRLI */
                    }
                    break;
                }
                break;
            }

            /* ---- R-type: OP ---- */
            case 0x33: {
                uint32_t rs1v = lane->gpr[rs1];
                uint32_t rs2v = lane->gpr[rs2];
                if (funct7 == 0x01) { /* M extension */
                    switch (funct3) {
                    case 0: /* MUL */
                        lane->gpr[rd] = rs1v * rs2v;
                        break;
                    default:
                        lane->gpr[rd] = 0;
                        break;
                    }
                } else {
                    switch (funct3) {
                    case 0: /* ADD / SUB */
                        lane->gpr[rd] = (funct7 & 0x20)
                            ? rs1v - rs2v
                            : rs1v + rs2v;
                        break;
                    case 1: lane->gpr[rd] = rs1v << (rs2v & 0x1F); break;                /* SLL  */
                    case 2: lane->gpr[rd] = (int32_t)rs1v < (int32_t)rs2v; break;         /* SLT  */
                    case 3: lane->gpr[rd] = rs1v < rs2v; break;                            /* SLTU */
                    case 4: lane->gpr[rd] = rs1v ^ rs2v; break;                            /* XOR  */
                    case 5: /* SRL / SRA */
                        lane->gpr[rd] = (funct7 & 0x20)
                            ? (uint32_t)((int32_t)rs1v >> (rs2v & 0x1F))
                            : rs1v >> (rs2v & 0x1F);
                        break;
                    case 6: lane->gpr[rd] = rs1v | rs2v; break;                            /* OR   */
                    case 7: lane->gpr[rd] = rs1v & rs2v; break;                            /* AND  */
                    }
                }
                break;
            }

            /* ---- SYSTEM ---- */
            case 0x73: {
                if (funct3 == 0) { /* EBREAK / ECALL */
                    if (((inst >> 20) & 0xFFF) == 1) { /* EBREAK */
                        lane->active = false;
                        warp->active_mask &= ~(1u << i);
                    }
                    break;
                }
                /* CSR instructions */
                uint32_t csr = (inst >> 20) & 0xFFF;
                uint32_t csr_val = 0;
                switch (csr) {
                case CSR_MHARTID: csr_val = lane->mhartid; break;
                case CSR_FFLAGS:  csr_val = lane->fcsr & 0x1F; break;
                case CSR_FRM:     csr_val = (lane->fcsr >> 5) & 0x7; break;
                case CSR_FCSR:    csr_val = lane->fcsr & 0xFF; break;
                default:          csr_val = 0; break;
                }
                lane->gpr[rd] = csr_val;
                /* CSRRW: write rs1; CSRRS: OR with rs1; CSRRC: clear bits */
                if (csr != CSR_MHARTID) {
                    uint32_t wval = lane->gpr[rs1];
                    switch (funct3) {
                    case 1: /* CSRRW */
                        lane->fcsr = wval & 0xFF;
                        break;
                    case 2: /* CSRRS */
                        lane->fcsr = (csr_val | wval) & 0xFF;
                        break;
                    case 3: /* CSRRC */
                        lane->fcsr = (csr_val & ~wval) & 0xFF;
                        break;
                    }
                }
                break;
            }

            /* ---- OP-FP (RV32F 浮点指令) ---- */
            case 0x53: {
                uint32_t f7 = (inst >> 25) & 0x7F;
                uint32_t fs1 = rs1;
                uint32_t fs2 = rs2;
                uint32_t fd = rd;

                /* 更新浮点舍入模式 (funct3: 000=RNE, 001=RTZ, 010=RDN, 011=RUP, 100=RMM) */
                FloatRoundMode rm = float_round_nearest_even;
                if (funct3 <= 4) {
                    rm = (FloatRoundMode)funct3;
                }
                set_float_rounding_mode(rm, &lane->fp_status);

                switch (f7) {
                case 0x00: /* FADD.S: rs2 = 第二源浮点寄存器 */
                    lane->fpr[fd] = float32_add(lane->fpr[fs1],
                                                lane->fpr[fs2],
                                                &lane->fp_status);
                    break;
                case 0x04: /* FSUB.S: rs2 = 第二源浮点寄存器 */
                    lane->fpr[fd] = float32_sub(lane->fpr[fs1],
                                                lane->fpr[fs2],
                                                &lane->fp_status);
                    break;
                case 0x08: /* FMUL.S */
                    lane->fpr[fd] = float32_mul(lane->fpr[fs1],
                                                lane->fpr[fs2],
                                                &lane->fp_status);
                    break;
                case 0x0C: /* FDIV.S */
                    lane->fpr[fd] = float32_div(lane->fpr[fs1],
                                                lane->fpr[fs2],
                                                &lane->fp_status);
                    break;
                case 0x10: { /* FSGNJ.S / FSGNJN.S / FSGNJX.S */
                    uint32_t a = lane->fpr[fs1];
                    uint32_t sb = lane->fpr[fs2] & 0x80000000;
                    switch (funct3) {
                    case 0: /* FSGNJ:  sign = sign_b */
                        lane->fpr[fd] = (a & 0x7FFFFFFF) | sb;
                        break;
                    case 1: /* FSGNJN: sign = ~sign_b */
                        lane->fpr[fd] = (a & 0x7FFFFFFF) | (~sb & 0x80000000);
                        break;
                    case 2: /* FSGNJX: sign = sign_a ^ sign_b */
                        lane->fpr[fd] = a ^ sb;
                        break;
                    default:
                        lane->fpr[fd] = 0;
                        break;
                    }
                    break;
                }
                case 0x20: /* FMIN.S / FMAX.S */
                    if (funct3 == 0) {
                        lane->fpr[fd] = float32_min(lane->fpr[fs1],
                                                    lane->fpr[fs2],
                                                    &lane->fp_status);
                    } else {
                        lane->fpr[fd] = float32_max(lane->fpr[fs1],
                                                    lane->fpr[fs2],
                                                    &lane->fp_status);
                    }
                    break;
                case 0x60: { /* FCVT.W.S / FCVT.WU.S (float→int) */
                    float32 f = lane->fpr[fs1];
                    if (fs2 == 0) { /* FCVT.W.S: signed */
                        int32_t cvt_result = float32_to_int32(f, &lane->fp_status);
                        lane->gpr[rd] = (uint32_t)cvt_result;
                    } else { /* FCVT.WU.S: unsigned */
                        lane->gpr[rd] = float32_to_uint32(
                            f, &lane->fp_status);
                    }
                    break;
                }
                case 0x68: { /* FCVT.S.W / FCVT.S.WU (int→float) */
                    int32_t ival = (int32_t)lane->gpr[rs1];
                    if (fs2 == 0) { /* FCVT.S.W: signed input */
                        lane->fpr[fd] = int32_to_float32(
                            ival, &lane->fp_status);
                    } else { /* FCVT.S.WU: unsigned input */
                        lane->fpr[fd] = uint32_to_float32(
                            lane->gpr[rs1], &lane->fp_status);
                    }
                    break;
                }
                case 0x70: /* FMV.X.W / FCLASS.S */
                    if (funct3 == 0) { /* FMV.X.W: float bits → int reg */
                        lane->gpr[rd] = lane->fpr[fs1];
                    }
                    break;
                case 0x78: /* FMV.W.X: int bits → float reg */
                    lane->fpr[fd] = lane->gpr[rs1];
                    break;

                /* ---- 自定义低精度浮点转换 ---- */
                case 0x22: { /* BF16 转换 */
                    if (fs2 == 1) { /* fcvt.bf16.s: f32 → bf16 */
                        uint32_t bits = lane->fpr[fs1];
                        uint32_t sign = bits & 0x80000000;
                        int32_t exp = ((bits >> 23) & 0xFF) - 127;
                        if ((bits & 0x7FFFFFFF) == 0) {
                            lane->fpr[fd] = sign >> 16;
                        } else if (exp == 128 || exp > 127) {
                            lane->fpr[fd] = (sign >> 16) | 0x7F80;
                        } else if (exp < -133) {
                            lane->fpr[fd] = sign >> 16;
                        } else {
                            uint16_t bf16 = (uint16_t)(sign >> 16)
                                          | (uint16_t)((exp + 127) << 7)
                                          | (uint16_t)((bits >> 16) & 0x7F);
                            lane->fpr[fd] = (uint32_t)bf16 << 16;
                        }
                    } else { /* fcvt.s.bf16: bf16 → f32 */
                        lane->fpr[fd] = (uint32_t)(lane->fpr[fs1] >> 16) << 16;
                    }
                    break;
                }
                case 0x24: { /* E4M3 / E5M2 转换 */
                    if (fs2 == 0) { /* fcvt.s.e4m3: e4m3 → f32 */
                        uint8_t e4 = (uint8_t)(lane->fpr[fs1] & 0xFF);
                        uint32_t sign = ((uint32_t)(e4 >> 7)) << 31;
                        int32_t exp = ((e4 >> 3) & 0xF) - 7;
                        uint32_t mant = ((uint32_t)(e4 & 0x7)) << 20;
                        if ((e4 & 0x7F) == 0) {
                            lane->fpr[fd] = sign;
                        } else {
                            lane->fpr[fd] = sign
                                          | ((uint32_t)(exp + 127) << 23)
                                          | mant;
                        }
                    } else if (fs2 == 1) { /* fcvt.e4m3.s: f32 → e4m3 */
                        uint32_t bits = lane->fpr[fs1];
                        uint32_t sign = (bits >> 31) & 1;
                        int32_t exp = ((bits >> 23) & 0xFF) - 127;
                        if ((bits & 0x7FFFFFFF) == 0) {
                            lane->fpr[fd] = sign << 7;
                        } else if (exp == 128 || exp > 8) {
                            /* NaN/Inf/overflow → 饱和到最大正规数 0x7E (448.0) */
                            lane->fpr[fd] = (sign << 7) | 0x7E;
                        } else if (exp < -7) {
                            lane->fpr[fd] = sign << 7;
                        } else {
                            lane->fpr[fd] = (sign << 7)
                                          | ((exp + 7) << 3)
                                          | ((bits >> 20) & 0x7);
                        }
                    } else if (fs2 == 2) { /* fcvt.s.e5m2: e5m2 → f32 */
                        uint8_t e5 = (uint8_t)(lane->fpr[fs1] & 0xFF);
                        uint32_t sign = ((uint32_t)(e5 >> 7)) << 31;
                        int32_t exp = ((e5 >> 2) & 0x1F) - 15;
                        uint32_t mant = ((uint32_t)(e5 & 0x3)) << 21;
                        if ((e5 & 0x7F) == 0) {
                            lane->fpr[fd] = sign;
                        } else {
                            lane->fpr[fd] = sign
                                          | ((uint32_t)(exp + 127) << 23)
                                          | mant;
                        }
                    } else { /* fs2 == 3: fcvt.e5m2.s: f32 → e5m2 */
                        uint32_t bits = lane->fpr[fs1];
                        uint32_t sign = (bits >> 31) & 1;
                        int32_t exp = ((bits >> 23) & 0xFF) - 127;
                        if ((bits & 0x7FFFFFFF) == 0) {
                            lane->fpr[fd] = sign << 7;
                        } else if (exp == 128 || exp > 15) {
                            /* NaN/Inf/overflow → 饱和到最大正规数 0x7B (57344.0) */
                            lane->fpr[fd] = (sign << 7) | 0x7B;
                        } else if (exp < -15) {
                            lane->fpr[fd] = sign << 7;
                        } else {
                            lane->fpr[fd] = (sign << 7)
                                          | ((exp + 15) << 2)
                                          | ((bits >> 21) & 0x3);
                        }
                    }
                    break;
                }
                case 0x26: { /* E2M1 转换 */
                    if (fs2 == 0) { /* fcvt.s.e2m1: e2m1 → f32 */
                        uint8_t e2 = (uint8_t)(lane->fpr[fs1] & 0xF);
                        uint32_t sign = ((uint32_t)(e2 >> 3)) << 31;
                        int32_t exp = ((e2 >> 1) & 0x3) - 1;
                        uint32_t mant = ((uint32_t)(e2 & 0x1)) << 22;
                        if ((e2 & 0x7) == 0) {
                            lane->fpr[fd] = sign;
                        } else if (exp < 0) {
                            lane->fpr[fd] = sign
                                          | ((uint32_t)126 << 23) | mant;
                        } else {
                            lane->fpr[fd] = sign
                                          | ((uint32_t)(exp + 127) << 23)
                                          | mant;
                        }
                    } else { /* fs2 == 1: fcvt.e2m1.s: f32 → e2m1 */
                        uint32_t bits = lane->fpr[fs1];
                        uint32_t sign = (bits >> 31) & 1;
                        int32_t exp = ((bits >> 23) & 0xFF) - 127;
                        if ((bits & 0x7FFFFFFF) == 0) {
                            lane->fpr[fd] = sign << 3;
                        } else if (exp == 128) {
                            lane->fpr[fd] = (sign << 3) | 0x7;
                        } else if (exp > 2) {
                            lane->fpr[fd] = (sign << 3) | 0x7;
                        } else if (exp < -1) {
                            lane->fpr[fd] = sign << 3;
                        } else {
                            lane->fpr[fd] = (sign << 3)
                                          | ((exp + 1) << 1)
                                          | ((bits >> 22) & 0x1);
                        }
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }

            /* ---- FENCE (no-op in SIMT) ---- */
            case 0x0F:
                break;

            default:
                return -1;
            }

            lane->gpr[0] = 0;
            lane->pc = npc;
        }
        cycle++;
    }
    return 0;
}

int gpgpu_core_exec_kernel(GPGPUState *s)
{
    uint32_t gx = s->kernel.grid_dim[0] ?: 1;
    uint32_t gy = s->kernel.grid_dim[1] ?: 1;
    uint32_t gz = s->kernel.grid_dim[2] ?: 1;
    uint32_t bx = s->kernel.block_dim[0] ?: 1;
    uint32_t by = s->kernel.block_dim[1] ?: 1;
    uint32_t bz = s->kernel.block_dim[2] ?: 1;
    uint32_t total_threads = bx * by * bz;
    uint32_t ws = s->warp_size ?: GPGPU_WARP_SIZE;
    uint32_t num_warps = (total_threads + ws - 1) / ws;

    for (uint32_t gz_i = 0; gz_i < gz; gz_i++) {
        for (uint32_t gy_i = 0; gy_i < gy; gy_i++) {
            for (uint32_t gx_i = 0; gx_i < gx; gx_i++) {
                uint32_t block_id[3] = { gx_i, gy_i, gz_i };
                uint32_t block_id_linear =
                    (gz_i * gy + gy_i) * gx + gx_i;

                for (uint32_t w = 0; w < num_warps; w++) {
                    uint32_t tid_base = w * ws;
                    uint32_t n = total_threads - tid_base;
                    if (n > ws) {
                        n = ws;
                    }

                    GPGPUWarp warp;
                    gpgpu_core_init_warp(&warp,
                                         (uint32_t)s->kernel.kernel_addr,
                                         tid_base, block_id,
                                         n, w, block_id_linear);
                    int ret = gpgpu_core_exec_warp(s, &warp, 100000);
                    if (ret < 0) {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}
