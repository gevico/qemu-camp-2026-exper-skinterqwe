/*
 * QEMU G233 Watchdog Timer
 *
 * Decrementing counter; times out when VAL reaches 0.
 * Supports feeding (reload), locking (CTRL-write protection),
 * and PLIC interrupt on timeout.
 *
 * Register map (base 0x10010000):
 *   0x00  WDT_CTRL  bit0: EN, bit1: INTEN
 *   0x04  WDT_LOAD  reload value
 *   0x08  WDT_VAL   current counter (read-only)
 *   0x0C  WDT_KEY   write: 0x5A5A5A5A=feed, 0x1ACCE551=lock
 *   0x10  WDT_SR    bit0: TIMEOUT (write-1-to-clear)
 *
 * PLIC IRQ: 4
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_G233_WDT_H
#define HW_G233_WDT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_G233_WDT "g233.wdt"

typedef struct G233WDTState G233WDTState;
DECLARE_INSTANCE_CHECKER(G233WDTState, G233_WDT, TYPE_G233_WDT)

struct G233WDTState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t ctrl;       /* bit0=EN, bit1=INTEN */
    uint32_t load;       /* reload value */
    uint64_t val_snap;   /* VAL snapshot when EN=0 */
    int64_t start_ns;    /* VIRTUAL clock ns when counter started */
    bool locked;         /* CTRL writes blocked after lock key */
    uint32_t sr;         /* bit0=TIMEOUT, w1c */
    QEMUTimer *timer;    /* one-shot for timeout */
    uint64_t freq_hz;    /* counter tick frequency, default 1MHz */
};

#endif
