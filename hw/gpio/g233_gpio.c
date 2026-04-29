/*
 * QEMU G233 GPIO Controller
 *
 * Register map (base 0x10012000):
 *   0x00  GPIO_DIR   — direction (0=input, 1=output)
 *   0x04  GPIO_OUT   — output data
 *   0x08  GPIO_IN    — input data (read-only; reflects OUT when DIR=output)
 *   0x0C  GPIO_IE    — interrupt enable
 *   0x10  GPIO_IS    — interrupt status (write-1-to-clear)
 *   0x14  GPIO_TRIG  — trigger type (0=edge, 1=level)
 *   0x18  GPIO_POL   — polarity (0=low/falling, 1=high/rising)
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/gpio/g233_gpio.h"

static void g233_gpio_update_irq(G233GPIOState *s)
{
    qemu_set_irq(s->irq, (s->is_ & s->ie) != 0);
}

/*
 * Compute current pin values and update IS register.
 * pin[i] = DIR[i] ? OUT[i] : 0  (output-mode pins are driven; inputs read 0)
 *
 * Edge-triggered (TRIG=0): IS bit sets on pin transition to POL value, if IE=1
 * Level-triggered (TRIG=1): IS bit follows (pin == POL), if IE=1
 */
static void g233_gpio_update_int(G233GPIOState *s)
{
    uint32_t cur_pin = s->dir & s->out;  /* 输出模式时 pin=OUT, 输入模式 pin=0 */
    uint32_t prev = s->prev_pin;

    for (int i = 0; i < 32; i++) {
        bool ie_bit  = (s->ie   >> i) & 1;
        bool trig_bit = (s->trig >> i) & 1;
        bool pol_bit  = (s->pol  >> i) & 1;
        bool cur_bit  = (cur_pin >> i) & 1;
        bool prev_bit = (prev   >> i) & 1;

        if (trig_bit == 0) {
            /* Edge-triggered: IE=1 AND transition to POL value */
            if (ie_bit && prev_bit != cur_bit && cur_bit == pol_bit) {
                s->is_ |= (1u << i);
            }
        } else {
            /* Level-triggered: IE=1 AND pin matches POL */
            if (ie_bit && cur_bit == pol_bit) {
                s->is_ |= (1u << i);
            } else {
                s->is_ &= ~(1u << i);
            }
        }
    }

    s->prev_pin = cur_pin;
    g233_gpio_update_irq(s);
}

static uint64_t g233_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (addr) {
    case 0x00: return s->dir;
    case 0x04: return s->out;
    case 0x08:
        /* IN: 输出时回读 OUT，输入时读 0 (per-pin) */
        return s->dir & s->out;
    case 0x0C: return s->ie;
    case 0x10: return s->is_;
    case 0x14: return s->trig;
    case 0x18: return s->pol;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_gpio: bad read offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void g233_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (addr) {
    case 0x00: /* DIR */
        s->dir = val;
        g233_gpio_update_int(s);
        break;
    case 0x04: /* OUT */
        s->out = val;
        g233_gpio_update_int(s);
        break;
    case 0x08:
        /* IN is read-only, ignore writes */
        break;
    case 0x0C: /* IE */
        s->ie = val;
        g233_gpio_update_int(s);
        break;
    case 0x10:
        /* IS: write-1-to-clear */
        s->is_ &= ~val;
        g233_gpio_update_irq(s);
        break;
    case 0x14:
        s->trig = val;
        g233_gpio_update_int(s);
        break;
    case 0x18:
        s->pol = val;
        g233_gpio_update_int(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_gpio: bad write offset 0x%" HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps g233_gpio_ops = {
    .read  = g233_gpio_read,
    .write = g233_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_gpio_reset(DeviceState *dev)
{
    G233GPIOState *s = G233_GPIO(dev);
    s->dir  = 0;
    s->out  = 0;
    s->ie   = 0;
    s->is_  = 0;
    s->trig = 0;
    s->pol  = 0;
    s->prev_pin = 0;
}

static void g233_gpio_realize(DeviceState *dev, Error **errp)
{
    G233GPIOState *s = G233_GPIO(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_gpio_ops, s,
                          TYPE_G233_GPIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = g233_gpio_realize;
    device_class_set_legacy_reset(dc, g233_gpio_reset);
    dc->desc = "G233 GPIO Controller";
}

static const TypeInfo g233_gpio_info = {
    .name          = TYPE_G233_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GPIOState),
    .class_init    = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}
type_init(g233_gpio_register_types);
