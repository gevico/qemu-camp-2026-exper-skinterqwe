/*
 * QEMU G233 PWM Controller
 *
 * 4-channel PWM with per-channel PERIOD/DUTY/CNT configuration.
 * Counter increments at 1MHz via virtual clock; DONE flag latches on wrap.
 *
 * Register map (base 0x10015000):
 *   0x00  PWM_GLB       bits[3:0] CHn_EN mirror, bits[7:4] CHn_DONE (w1c)
 *   Per channel n (at 0x10 + n * 0x10):
 *     +0x00 CHn_CTRL    bit0: EN, bit1: POL
 *     +0x04 CHn_PERIOD  period value
 *     +0x08 CHn_DUTY    duty cycle
 *     +0x0C CHn_CNT     counter (read-only, computed from elapsed time)
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/g233_pwm.h"

/* PWM_GLB bit positions */
#define GLB_CH_EN(n)    (1u << (n))       /* bits[3:0] */
#define GLB_CH_DONE(n)  (1u << (4 + (n))) /* bits[7:4] */

/* CHn_CTRL bit fields */
#define CTRL_EN    (1u << 0)
#define CTRL_POL   (1u << 1)

#define FREQ_HZ_DEFAULT 1000000ULL  /* 1MHz: 1 tick per μs */

/* Convert elapsed ns to counter ticks at the current frequency */
static inline uint64_t ns_to_ticks(uint64_t ns, uint64_t freq_hz)
{
    return muldiv64(ns, freq_hz, NANOSECONDS_PER_SECOND);
}

/* Convert ticks to ns at the current frequency */
static inline uint64_t ticks_to_ns(uint64_t ticks, uint64_t freq_hz)
{
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, freq_hz);
}

static uint64_t g233_pwm_get_cnt(G233PWMChannel *ch, uint64_t freq_hz)
{
    if (!(ch->ctrl & CTRL_EN)) {
        return ch->cnt;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = (now > ch->start_ns) ? (now - ch->start_ns) : 0;
    uint64_t ticks = ns_to_ticks(elapsed, freq_hz);
    /*
     * Counter wraps at period+1: 0, 1, ..., period, 0, 1, ...
     * Special case: period == 0 means counter is always 0.
     */
    if (ch->period > 0) {
        ticks %= (ch->period + 1);
    }
    return ticks;
}

/*
 * (Re)arm the one-shot timer to fire when the counter next wraps
 * to 0.  The wrap happens at (period + 1 - current_ticks) ticks
 * in the future, relative to the current counter value within the
 * current period cycle.
 */
static void g233_pwm_rearm_timer(G233PWMChannel *ch, uint64_t freq_hz)
{
    if (!(ch->ctrl & CTRL_EN) || ch->period == 0) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = (now > ch->start_ns) ? (now - ch->start_ns) : 0;
    uint64_t ticks = ns_to_ticks(elapsed, freq_hz);
    uint64_t period_ticks = ch->period + 1;
    uint64_t ticks_in_cycle = ticks % period_ticks;
    uint64_t ticks_until_wrap = period_ticks - ticks_in_cycle;
    uint64_t ns_until_wrap = ticks_to_ns(ticks_until_wrap, freq_hz);
    timer_mod(ch->timer, now + ns_until_wrap);
}

static void g233_pwm_timer_cb(void *opaque)
{
    G233PWMChannel *ch = opaque;
    G233PWMState *s = ch->parent;

    /* Counter wrapped — latch DONE flag */
    s->done_flags |= (1u << ch->index);

    /* Reset the cycle start and re-arm for the next period */
    ch->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    g233_pwm_rearm_timer(ch, s->freq_hz);
}

static uint64_t g233_pwm_read(void *opaque, hwaddr addr, unsigned size)
{
    G233PWMState *s = G233_PWM(opaque);

    if (addr == 0x00) {
        /* GLB: [3:0]=CHn_EN mirror, [7:4]=DONE flags */
        uint32_t glb = s->done_flags << 4;
        for (int i = 0; i < G233_PWM_CHANNELS; i++) {
            if (s->ch[i].ctrl & CTRL_EN) {
                glb |= GLB_CH_EN(i);
            }
        }
        return glb;
    }

    /* Per-channel registers starting at 0x10 */
    if (addr < 0x10) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_pwm: bad read offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }

    int ch_idx = (addr - 0x10) / 0x10;
    int ch_off = (addr - 0x10) % 0x10;

    if (ch_idx >= G233_PWM_CHANNELS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_pwm: bad read channel %d\n", ch_idx);
        return 0;
    }

    G233PWMChannel *ch = &s->ch[ch_idx];

    switch (ch_off) {
    case 0x00: return ch->ctrl;
    case 0x04: return ch->period;
    case 0x08: return ch->duty;
    case 0x0C: return g233_pwm_get_cnt(ch, s->freq_hz);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_pwm: bad read offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void g233_pwm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    G233PWMState *s = G233_PWM(opaque);

    if (addr == 0x00) {
        /* GLB: only DONE bits [7:4] are writable (w1c) */
        uint32_t done_w1c = (val >> 4) & 0xF;
        s->done_flags &= ~done_w1c;
        return;
    }

    if (addr < 0x10) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_pwm: bad write offset 0x%" HWADDR_PRIx "\n", addr);
        return;
    }

    int ch_idx = (addr - 0x10) / 0x10;
    int ch_off = (addr - 0x10) % 0x10;

    if (ch_idx >= G233_PWM_CHANNELS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_pwm: bad write channel %d\n", ch_idx);
        return;
    }

    G233PWMChannel *ch = &s->ch[ch_idx];

    switch (ch_off) {
    case 0x00: /* CTRL */
    {
        uint32_t old_ctrl = ch->ctrl;
        ch->ctrl = val;
        if (!(old_ctrl & CTRL_EN) && (val & CTRL_EN)) {
            /* EN 0→1: start counter */
            ch->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            g233_pwm_rearm_timer(ch, s->freq_hz);
        } else if ((old_ctrl & CTRL_EN) && !(val & CTRL_EN)) {
            /* EN 1→0: stop counter, save snapshot */
            ch->cnt = g233_pwm_get_cnt(ch, s->freq_hz);
            timer_del(ch->timer);
        } else if (val & CTRL_EN) {
            g233_pwm_rearm_timer(ch, s->freq_hz);
        }
        break;
    }
    case 0x04:
        ch->period = val;
        if (ch->ctrl & CTRL_EN) {
            /* Restart cycle from now with new period */
            ch->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            g233_pwm_rearm_timer(ch, s->freq_hz);
        }
        break;
    case 0x08:
        ch->duty = val;
        break;
    case 0x0C:
        /* CNT is read-only, ignore writes */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_pwm: bad write offset 0x%" HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps g233_pwm_ops = {
    .read  = g233_pwm_read,
    .write = g233_pwm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_pwm_reset(DeviceState *dev)
{
    G233PWMState *s = G233_PWM(dev);
    for (int i = 0; i < G233_PWM_CHANNELS; i++) {
        G233PWMChannel *ch = &s->ch[i];
        timer_del(ch->timer);
        ch->ctrl = 0;
        ch->period = 0;
        ch->duty = 0;
        ch->cnt = 0;
        ch->start_ns = 0;
    }
    s->done_flags = 0;
}

static void g233_pwm_realize(DeviceState *dev, Error **errp)
{
    G233PWMState *s = G233_PWM(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_pwm_ops, s,
                          TYPE_G233_PWM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    for (int i = 0; i < G233_PWM_CHANNELS; i++) {
        G233PWMChannel *ch = &s->ch[i];
        ch->parent = s;
        ch->index = i;
        ch->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                 g233_pwm_timer_cb, ch);
    }
}

static void g233_pwm_unrealize(DeviceState *dev)
{
    G233PWMState *s = G233_PWM(dev);
    for (int i = 0; i < G233_PWM_CHANNELS; i++) {
        timer_free(s->ch[i].timer);
        s->ch[i].timer = NULL;
    }
}

static const Property g233_pwm_properties[] = {
    DEFINE_PROP_UINT64("clock-frequency", G233PWMState,
                       freq_hz, FREQ_HZ_DEFAULT),
};

static void g233_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = g233_pwm_realize;
    dc->unrealize = g233_pwm_unrealize;
    device_class_set_legacy_reset(dc, g233_pwm_reset);
    device_class_set_props(dc, g233_pwm_properties);
    dc->desc = "G233 PWM Controller";
}

static const TypeInfo g233_pwm_info = {
    .name          = TYPE_G233_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233PWMState),
    .class_init    = g233_pwm_class_init,
};

static void g233_pwm_register_types(void)
{
    type_register_static(&g233_pwm_info);
}
type_init(g233_pwm_register_types);
