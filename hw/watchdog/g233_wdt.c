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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/watchdog/g233_wdt.h"

#define WDT_CTRL_EN     (1u << 0)
#define WDT_CTRL_INTEN  (1u << 1)

#define WDT_KEY_FEED    0x5A5A5A5A
#define WDT_KEY_LOCK    0x1ACCE551

#define WDT_SR_TIMEOUT  (1u << 0)

#define FREQ_HZ_DEFAULT 1000000ULL  /* 1MHz: 1 tick per μs */

static void g233_wdt_update_irq(G233WDTState *s)
{
    bool irq = (s->sr & WDT_SR_TIMEOUT) && (s->ctrl & WDT_CTRL_INTEN);
    qemu_set_irq(s->irq, irq);
}

static uint64_t g233_wdt_get_val(G233WDTState *s)
{
    if (!(s->ctrl & WDT_CTRL_EN)) {
        return s->val_snap;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = (now > s->start_ns) ? (now - s->start_ns) : 0;
    uint64_t ticks = muldiv64(elapsed, s->freq_hz, NANOSECONDS_PER_SECOND);
    if (ticks >= s->load) {
        return 0;
    }
    return s->load - ticks;
}

static void g233_wdt_rearm_timer(G233WDTState *s)
{
    if (!(s->ctrl & WDT_CTRL_EN) || s->load == 0) {
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = (now > s->start_ns) ? (now - s->start_ns) : 0;
    uint64_t ticks = muldiv64(elapsed, s->freq_hz, NANOSECONDS_PER_SECOND);

    if (ticks >= s->load) {
        /* Already past timeout — fire immediately */
        s->sr |= WDT_SR_TIMEOUT;
        g233_wdt_update_irq(s);
        return;
    }

    uint64_t remaining = s->load - ticks;
    uint64_t ns_until = muldiv64(remaining, NANOSECONDS_PER_SECOND,
                                 s->freq_hz);
    timer_mod(s->timer, now + ns_until);
}

static void g233_wdt_timer_cb(void *opaque)
{
    G233WDTState *s = opaque;

    s->sr |= WDT_SR_TIMEOUT;
    g233_wdt_update_irq(s);
}

static uint64_t g233_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    G233WDTState *s = G233_WDT(opaque);

    switch (addr) {
    case 0x00: return s->ctrl;
    case 0x04: return s->load;
    case 0x08: return g233_wdt_get_val(s);
    case 0x0C:
        /* KEY is write-only */
        return 0;
    case 0x10: return s->sr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_wdt: bad read offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void g233_wdt_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    G233WDTState *s = G233_WDT(opaque);

    switch (addr) {
    case 0x00: /* CTRL */
        if (s->locked) {
            return;
        }
        {
            uint32_t old_ctrl = s->ctrl;
            s->ctrl = val;
            if (!(old_ctrl & WDT_CTRL_EN) && (val & WDT_CTRL_EN)) {
                /* EN 0→1: start counter, clear timeout */
                s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                s->sr = 0;
                g233_wdt_update_irq(s);
                g233_wdt_rearm_timer(s);
            } else if ((old_ctrl & WDT_CTRL_EN) && !(val & WDT_CTRL_EN)) {
                /* EN 1→0: stop, save snapshot */
                s->val_snap = g233_wdt_get_val(s);
                timer_del(s->timer);
            } else if (val & WDT_CTRL_EN) {
                /* EN stays 1, INTEN may have changed */
                g233_wdt_rearm_timer(s);
                g233_wdt_update_irq(s);
            }
        }
        break;

    case 0x04: /* LOAD */
        s->load = val;
        break;

    case 0x08:
        /* VAL is read-only, ignore writes */
        break;

    case 0x0C: /* KEY */
        if (val == WDT_KEY_FEED) {
            /* Feed: reload from LOAD, clear timeout, restart counter */
            s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->sr = 0;
            g233_wdt_update_irq(s);
            if (s->ctrl & WDT_CTRL_EN) {
                g233_wdt_rearm_timer(s);
            }
        } else if (val == WDT_KEY_LOCK) {
            s->locked = true;
        }
        break;

    case 0x10: /* SR — w1c */
        s->sr &= ~val;
        g233_wdt_update_irq(s);
        /* If EN=1 and counter had timed out, restart after clearing timeout */
        if ((s->ctrl & WDT_CTRL_EN) && !(s->sr & WDT_SR_TIMEOUT)) {
            s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            g233_wdt_rearm_timer(s);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_wdt: bad write offset 0x%" HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps g233_wdt_ops = {
    .read  = g233_wdt_read,
    .write = g233_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_wdt_reset(DeviceState *dev)
{
    G233WDTState *s = G233_WDT(dev);
    timer_del(s->timer);
    s->ctrl = 0;
    s->load = 0;
    s->val_snap = 0;
    s->start_ns = 0;
    s->locked = false;
    s->sr = 0;
    qemu_set_irq(s->irq, 0);
}

static void g233_wdt_realize(DeviceState *dev, Error **errp)
{
    G233WDTState *s = G233_WDT(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_wdt_ops, s,
                          TYPE_G233_WDT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, g233_wdt_timer_cb, s);
}

static void g233_wdt_unrealize(DeviceState *dev)
{
    G233WDTState *s = G233_WDT(dev);
    timer_free(s->timer);
    s->timer = NULL;
}

static const Property g233_wdt_properties[] = {
    DEFINE_PROP_UINT64("clock-frequency", G233WDTState,
                       freq_hz, FREQ_HZ_DEFAULT),
};

static void g233_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = g233_wdt_realize;
    dc->unrealize = g233_wdt_unrealize;
    device_class_set_legacy_reset(dc, g233_wdt_reset);
    device_class_set_props(dc, g233_wdt_properties);
    dc->desc = "G233 Watchdog Timer";
}

static const TypeInfo g233_wdt_info = {
    .name          = TYPE_G233_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233WDTState),
    .class_init    = g233_wdt_class_init,
};

static void g233_wdt_register_types(void)
{
    type_register_static(&g233_wdt_info);
}
type_init(g233_wdt_register_types);
