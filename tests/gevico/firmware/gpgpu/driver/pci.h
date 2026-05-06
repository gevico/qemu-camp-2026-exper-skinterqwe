#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* RISC-V virt/g233 ECAM base address */
#define PCI_ECAM_BASE       0x30000000UL
#define PCI_ECAM_SIZE       0x10000000UL

/* PCI Config Space Offsets */
#define PCI_CFG_VENDOR_ID   0x00
#define PCI_CFG_DEVICE_ID   0x02
#define PCI_CFG_COMMAND     0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_BAR1        0x14
#define PCI_CFG_BAR2        0x18
#define PCI_CFG_BAR3        0x1C
#define PCI_CFG_BAR4        0x20
#define PCI_CFG_BAR5        0x24

/* PCI Command Register Bits */
#define PCI_CMD_MEMORY      (1 << 1)   /* Memory Space Enable */
#define PCI_CMD_BUS_MASTER  (1 << 2)   /* Bus Master Enable */

typedef struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
} pci_bdf_t;

/* Read 32-bit value from PCI config space */
uint32_t pci_cfg_read32(pci_bdf_t bdf, uint8_t offset);

/* Write 32-bit value to PCI config space */
void pci_cfg_write32(pci_bdf_t bdf, uint8_t offset, uint32_t val);

/* Read 16-bit value from PCI config space */
uint16_t pci_cfg_read16(pci_bdf_t bdf, uint8_t offset);

/* Write 16-bit value to PCI config space */
void pci_cfg_write16(pci_bdf_t bdf, uint8_t offset, uint16_t val);

/* Find a PCI device by vendor/device ID (scans bus 0) */
int pci_find_device(uint16_t vendor, uint16_t device, pci_bdf_t *out);

/* Enable memory space access and bus mastering */
void pci_enable_device(pci_bdf_t bdf);

/* Read BAR address (handles 64-bit BARs) */
uint64_t pci_read_bar(pci_bdf_t bdf, int bar_num);

#endif /* PCI_H */
