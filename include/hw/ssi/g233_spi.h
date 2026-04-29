/*
 * QEMU G233 SPI Controller
 *
 * Simple SPI master with 4 registers and two on-board Flash chips
 * (W25X16 on CS0, W25X32 on CS1).  Byte-level transfer with TXE/RXNE
 * status bits, overrun detection, and PLIC interrupt.
 *
 * Register map (base 0x10018000):
 *   0x00  SPI_CR1   bit0: SPE, bit2: MSTR, bit5: ERRIE,
 *                    bit6: RXNEIE, bit7: TXEIE
 *   0x04  SPI_CR2   bits[1:0]: CS select (0=CS0, 1=CS1)
 *   0x08  SPI_SR    bit0: RXNE, bit1: TXE, bit4: OVERRUN (w1c)
 *   0x0C  SPI_DR    data register (write=tx, read=rx)
 *
 * PLIC IRQ: 5
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_G233_SPI_H
#define HW_G233_SPI_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_G233_SPI "g233.spi"

typedef struct G233SPIState G233SPIState;
DECLARE_INSTANCE_CHECKER(G233SPIState, G233_SPI, TYPE_G233_SPI)

/* Flash chip model */
typedef struct {
    uint8_t *storage;        /* flash memory array */
    uint32_t size;           /* size in bytes */
    uint32_t jedec;          /* packed JEDEC ID (e.g. 0xEF3015) */

    /* Command state machine */
    uint8_t cmd;             /* current command byte */
    uint8_t state;           /* 0=IDLE, 1=JEDEC_ID, 2=RDSR, ... */
    int data_idx;            /* position within data phase */
    int data_len;            /* expected data bytes for current cmd */
    uint32_t addr;           /* accumulated address */
    int addr_count;          /* address bytes received */
    int addr_needed;         /* how many address bytes expected */

    /* Status */
    uint8_t sr;              /* status register (bit0=BUSY) */
    bool wel;                /* write enable latch */

    /* Erase/program timing */
    QEMUTimer *busy_timer;
    G233SPIState *spi;
    int cs_idx;
} G233SPIFlash;

struct G233SPIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t cr1;            /* SPE (bit0), MSTR (bit2), ERRIE (bit5),
                                RXNEIE (bit6), TXEIE (bit7) */
    uint32_t cr2;            /* CS select bits[1:0] */
    uint32_t sr;             /* RXNE (bit0), TXE (bit1), OVERRUN (bit4) */
    uint32_t dr;             /* data register */

    /* Transfer state */
    bool tx_pending;         /* byte in progress */
    uint8_t tx_byte;         /* byte being transmitted */
    QEMUTimer *transfer_timer;

    G233SPIFlash flash[2];   /* CS0: W25X16, CS1: W25X32 */
};

/* CR1 bits */
#define SPI_CR1_SPE     (1u << 0)
#define SPI_CR1_MSTR    (1u << 2)
#define SPI_CR1_ERRIE   (1u << 5)

/* SR bits */
#define SPI_SR_RXNE     (1u << 0)
#define SPI_SR_TXE      (1u << 1)
#define SPI_SR_OVERRUN  (1u << 4)

#endif
