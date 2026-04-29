/*
 * QEMU G233 GPIO Controller
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_G233_GPIO_H
#define HW_G233_GPIO_H

#include "hw/core/sysbus.h"

#define TYPE_G233_GPIO "g233.gpio"
typedef struct G233GPIOState G233GPIOState;
DECLARE_INSTANCE_CHECKER(G233GPIOState, G233_GPIO, TYPE_G233_GPIO)

struct G233GPIOState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t dir;       /* 0x00: 方向 0=输入/1=输出 */
    uint32_t out;       /* 0x04: 输出数据 */
    /* 0x08: IN 只读，输出模式时回读 OUT */
    uint32_t ie;        /* 0x0C: 中断使能 */
    uint32_t is_;       /* 0x10: 中断状态 (w1c) */
    uint32_t trig;      /* 0x14: 触发类型 0=边沿/1=电平 */
    uint32_t pol;       /* 0x18: 极性 */
    uint32_t prev_pin;  /* 上一次的引脚值，用于边沿检测 */
};

#endif
