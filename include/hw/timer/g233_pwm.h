/*
 * QEMU G233 PWM Controller
 *
 * 4-channel PWM with per-channel PERIOD/DUTY/CNT configuration.
 * Counter increments via virtual clock; DONE flag latches on wrap.
 *
 * Register map (base 0x10015000):
 *   0x00  PWM_GLB       bits[3:0] CHn_EN mirror, bits[7:4] CHn_DONE (w1c)
 *   Per channel n (at 0x10 + n * 0x10):
 *     +0x00 CHn_CTRL    bit0: EN, bit1: POL
 *     +0x04 CHn_PERIOD  period value
 *     +0x08 CHn_DUTY    duty cycle
 *     +0x0C CHn_CNT     counter (read-only)
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_G233_PWM_H
#define HW_G233_PWM_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_G233_PWM "g233.pwm"
#define G233_PWM_CHANNELS 4

typedef struct G233PWMState G233PWMState;
DECLARE_INSTANCE_CHECKER(G233PWMState, G233_PWM, TYPE_G233_PWM)

typedef struct {
    uint32_t ctrl;
    uint32_t period;
    uint32_t duty;
    uint64_t cnt;           /* computed from elapsed virtual time */
    int64_t start_ns;       /* VIRTUAL clock ns when EN was set */
    QEMUTimer *timer;       /* one-shot to fire at period completion */
    G233PWMState *parent;
    int index;
} G233PWMChannel;

struct G233PWMState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    G233PWMChannel ch[G233_PWM_CHANNELS];
    uint32_t done_flags;    /* bits[3:0] = CHn_DONE, w1c */
    uint64_t freq_hz;       /* counter tick frequency, default 1MHz */
};

#endif
