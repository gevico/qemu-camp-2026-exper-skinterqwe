/*
 * QEMU G233 SPI Controller
 *
 * Simple 4-register SPI master with two on-board Flash chips:
 *   CS0 — W25X16 (2MB, JEDEC 0xEF3015)
 *   CS1 — W25X32 (4MB, JEDEC 0xEF3016)
 *
 * Register map (base 0x10018000):
 *   0x00  SPI_CR1   bit0: SPE, bit2: MSTR, bit5: ERRIE,
 *                    bit6: RXNEIE, bit7: TXEIE
 *   0x04  SPI_CR2   bits[1:0]: CS select
 *   0x08  SPI_SR    bit0: RXNE, bit1: TXE, bit4: OVERRUN (w1c)
 *   0x0C  SPI_DR    data register (write=tx, read=rx)
 *
 * PLIC IRQ: 5
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/ssi/g233_spi.h"

#define TRANSFER_NS      1000ULL   /* 1μs per byte transfer */

/* Flash commands */
#define FLASH_CMD_WRITE_ENABLE  0x06
#define FLASH_CMD_READ_STATUS   0x05
#define FLASH_CMD_READ_DATA     0x03
#define FLASH_CMD_PAGE_PROGRAM  0x02
#define FLASH_CMD_SECTOR_ERASE  0x20
#define FLASH_CMD_JEDEC_ID      0x9F

/* Flash state machine states */
enum {
    FLASH_IDLE = 0,
    FLASH_JEDEC_ID,
    FLASH_RDSR,
    FLASH_READ,
    FLASH_PROGRAM,
    FLASH_ERASE,
};

/* ------------------------------------------------------------------ */
/*  Flash model                                                       */
/* ------------------------------------------------------------------ */

static uint8_t flash_process_byte(G233SPIFlash *f, uint8_t tx)
{
    switch (f->state) {
    case FLASH_IDLE:
        /* First byte is always a command */
        f->cmd = tx;
        f->data_idx = 0;
        f->addr = 0;
        f->addr_count = 0;
        f->addr_needed = 0;

        switch (tx) {
        case FLASH_CMD_JEDEC_ID:
            f->state = FLASH_JEDEC_ID;
            f->data_idx = 0;
            return 0x00;   /* command byte — no data */

        case FLASH_CMD_READ_STATUS:
            f->state = FLASH_RDSR;
            return 0x00;   /* command byte — status follows */

        case FLASH_CMD_WRITE_ENABLE:
            f->wel = true;
            f->state = FLASH_IDLE;
            return 0x00;

        case FLASH_CMD_READ_DATA:
            f->state = FLASH_READ;
            f->addr_needed = 3;
            return 0x00;

        case FLASH_CMD_PAGE_PROGRAM:
            f->state = FLASH_PROGRAM;
            f->addr_needed = 3;
            f->data_len = 256;
            return 0x00;

        case FLASH_CMD_SECTOR_ERASE:
            f->state = FLASH_ERASE;
            f->addr_needed = 3;
            return 0x00;

        default:
            f->state = FLASH_IDLE;
            return 0xFF;
        }

    case FLASH_JEDEC_ID:
        {
            uint8_t val;
            if (f->data_idx == 0) {
                val = (f->jedec >> 16) & 0xFF;  /* 0xEF: manufacturer */
            } else if (f->data_idx == 1) {
                val = (f->jedec >> 8) & 0xFF;   /* 0x30: device type */
            } else if (f->data_idx == 2) {
                val = f->jedec & 0xFF;          /* 0x15: capacity */
            } else {
                f->state = FLASH_IDLE;
                return 0xFF;
            }
            f->data_idx++;
            return val;
        }

    case FLASH_RDSR:
        f->state = FLASH_IDLE;
        return f->sr;

    case FLASH_READ:
        if (f->addr_count < f->addr_needed) {
            f->addr = (f->addr << 8) | tx;
            f->addr_count++;
            if (f->addr_count >= f->addr_needed) {
                f->addr %= f->size;
            }
            return 0x00;
        }
        /* Reading data */
        {
            uint8_t val = f->storage ? f->storage[f->addr] : 0xFF;
            f->addr = (f->addr + 1) % f->size;
            return val;
        }

    case FLASH_PROGRAM:
        if (f->addr_count < f->addr_needed) {
            f->addr = (f->addr << 8) | tx;
            f->addr_count++;
            if (f->addr_count >= f->addr_needed) {
                f->addr %= f->size;
            }
            return 0x00;
        }
        /* Programming data byte */
        if (f->storage && f->wel) {
            f->storage[f->addr] = tx;
        }
        f->addr = (f->addr + 1) % f->size;
        return 0x00;

    case FLASH_ERASE:
        if (f->addr_count < f->addr_needed) {
            f->addr = (f->addr << 8) | tx;
            f->addr_count++;
            if (f->addr_count >= f->addr_needed) {
                uint32_t sector = f->addr & ~0xFFF;
                if (f->storage && f->wel && sector < f->size) {
                    memset(f->storage + sector, 0xFF,
                           MIN(4096, f->size - sector));
                }
                f->wel = false;
                f->state = FLASH_IDLE;
            }
            return 0x00;
        }
        f->state = FLASH_IDLE;
        return 0x00;

    default:
        f->state = FLASH_IDLE;
        return 0xFF;
    }
}

/* ------------------------------------------------------------------ */
/*  SPI controller                                                    */
/* ------------------------------------------------------------------ */

static void g233_spi_update_irq(G233SPIState *s)
{
    bool irq = false;

    /* ERRIE + OVERRUN → interrupt */
    if ((s->cr1 & SPI_CR1_ERRIE) && (s->sr & SPI_SR_OVERRUN)) {
        irq = true;
    }
    /* RXNEIE + RXNE → interrupt */
    if ((s->cr1 & (1u << 6)) && (s->sr & SPI_SR_RXNE)) {
        irq = true;
    }
    /* TXEIE + TXE → interrupt */
    if ((s->cr1 & (1u << 7)) && (s->sr & SPI_SR_TXE)) {
        irq = true;
    }
    qemu_set_irq(s->irq, irq);
}

static void g233_spi_transfer_done(G233SPIState *s)
{
    int cs = s->cr2 & 0x3;
    G233SPIFlash *f = &s->flash[cs];

    s->tx_pending = false;

    /* Process the byte through the flash state machine */
    uint8_t rx = flash_process_byte(f, s->tx_byte);

    /* If RXNE was already set → overrun */
    if (s->sr & SPI_SR_RXNE) {
        s->sr |= SPI_SR_OVERRUN;
    }

    s->dr = rx;
    s->sr |= SPI_SR_RXNE;
    s->sr |= SPI_SR_TXE;
    g233_spi_update_irq(s);
}

static void g233_spi_timer_cb(void *opaque)
{
    g233_spi_transfer_done((G233SPIState *)opaque);
}

static uint64_t g233_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);
    uint32_t r;

    switch (addr) {
    case 0x00:
        return s->cr1;
    case 0x04:
        return s->cr2;
    case 0x08:
        return s->sr;
    case 0x0C:
        /* Reading DR clears RXNE */
        r = s->dr;
        s->sr &= ~SPI_SR_RXNE;
        g233_spi_update_irq(s);
        return r;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_spi: bad read offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void g233_spi_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);

    switch (addr) {
    case 0x00: /* CR1 */
        {
            uint32_t old = s->cr1;
            s->cr1 = val;
            /* SPE 0→1: initialise TXE */
            if (!(old & SPI_CR1_SPE) && (val & SPI_CR1_SPE)) {
                s->sr |= SPI_SR_TXE;
                g233_spi_update_irq(s);
            } else if ((old & SPI_CR1_SPE) && !(val & SPI_CR1_SPE)) {
                s->sr &= ~SPI_SR_TXE;
                s->sr &= ~SPI_SR_RXNE;
                s->tx_pending = false;
                timer_del(s->transfer_timer);
                g233_spi_update_irq(s);
            }
        }
        break;

    case 0x04: /* CR2 — CS select */
        {
            uint32_t new_cs = val & 0x3;
            if ((s->cr2 & 0x3) != new_cs) {
                /* CS changed: deselect old chip → reset to IDLE */
                s->flash[s->cr2 & 0x3].state = FLASH_IDLE;
                s->flash[s->cr2 & 0x3].addr_count = 0;
            }
            s->cr2 = new_cs;
        }
        break;

    case 0x08: /* SR — w1c for OVERRUN, RXNE/TXE read-only */
        s->sr &= ~(val & SPI_SR_OVERRUN);
        g233_spi_update_irq(s);
        break;

    case 0x0C: /* DR */
        if (!(s->cr1 & SPI_CR1_SPE)) {
            return;   /* SPI not enabled */
        }
        s->tx_byte = (uint8_t)val;
        s->sr &= ~SPI_SR_TXE;   /* TX buffer now full */
        /* RXNE stays set until DR is read — overrun if new rx arrives */
        s->tx_pending = true;
        timer_mod(s->transfer_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TRANSFER_NS);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233_spi: bad write offset 0x%" HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps g233_spi_ops = {
    .read  = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_spi_reset(DeviceState *dev)
{
    G233SPIState *s = G233_SPI(dev);

    timer_del(s->transfer_timer);
    s->cr1 = 0;
    s->cr2 = 0;
    s->sr  = 0;
    s->dr  = 0;
    s->tx_pending = false;
    s->tx_byte = 0;
    qemu_set_irq(s->irq, 0);

    /* Reset flash chips */
    for (int i = 0; i < 2; i++) {
        G233SPIFlash *f = &s->flash[i];
        f->state = FLASH_IDLE;
        f->cmd = 0;
        f->data_idx = 0;
        f->data_len = 0;
        f->addr = 0;
        f->addr_count = 0;
        f->addr_needed = 0;
        f->sr = 0;
        f->wel = false;
    }
}

static void g233_spi_flash_init(G233SPIFlash *f, uint32_t size,
                                 uint32_t jedec, G233SPIState *spi, int cs)
{
    f->size = size;
    f->jedec = jedec;
    f->spi = spi;
    f->cs_idx = cs;
    f->state = FLASH_IDLE;
    f->sr = 0;
    f->wel = false;
    f->storage = g_malloc0(size);
    /* Flash memory initialises to all 0xFF (erased) */
    memset(f->storage, 0xFF, size);
}

static void g233_spi_flash_free(G233SPIFlash *f)
{
    g_free(f->storage);
    f->storage = NULL;
}

static void g233_spi_realize(DeviceState *dev, Error **errp)
{
    G233SPIState *s = G233_SPI(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_spi_ops, s,
                          TYPE_G233_SPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->transfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      g233_spi_timer_cb, s);

    /* CS0: W25X16 — 2MB, JEDEC 0xEF3015 */
    g233_spi_flash_init(&s->flash[0], 2 * 1024 * 1024, 0xEF3015, s, 0);
    /* CS1: W25X32 — 4MB, JEDEC 0xEF3016 */
    g233_spi_flash_init(&s->flash[1], 4 * 1024 * 1024, 0xEF3016, s, 1);
}

static void g233_spi_unrealize(DeviceState *dev)
{
    G233SPIState *s = G233_SPI(dev);
    timer_free(s->transfer_timer);
    for (int i = 0; i < 2; i++) {
        g233_spi_flash_free(&s->flash[i]);
    }
}

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = g233_spi_realize;
    dc->unrealize = g233_spi_unrealize;
    device_class_set_legacy_reset(dc, g233_spi_reset);
    dc->desc = "G233 SPI Controller";
}

static const TypeInfo g233_spi_info = {
    .name          = TYPE_G233_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SPIState),
    .class_init    = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}
type_init(g233_spi_register_types);
