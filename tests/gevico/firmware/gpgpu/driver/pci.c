#include "pci.h"

static inline volatile uint32_t *ecam_addr(pci_bdf_t bdf, uint8_t offset)
{
    uint64_t addr = PCI_ECAM_BASE
                  | ((uint64_t)bdf.bus << 20)
                  | ((uint64_t)bdf.dev << 15)
                  | ((uint64_t)bdf.func << 12)
                  | offset;
    return (volatile uint32_t *)(uintptr_t)addr;
}

uint32_t pci_cfg_read32(pci_bdf_t bdf, uint8_t offset)
{
    return *ecam_addr(bdf, offset & 0xFC);
}

void pci_cfg_write32(pci_bdf_t bdf, uint8_t offset, uint32_t val)
{
    *ecam_addr(bdf, offset & 0xFC) = val;
}

uint16_t pci_cfg_read16(pci_bdf_t bdf, uint8_t offset)
{
    uint32_t val = pci_cfg_read32(bdf, offset);
    return (offset & 2) ? (uint16_t)(val >> 16) : (uint16_t)val;
}

void pci_cfg_write16(pci_bdf_t bdf, uint8_t offset, uint16_t val)
{
    uint32_t aligned = pci_cfg_read32(bdf, offset);
    if (offset & 2) {
        aligned = (aligned & 0x0000FFFF) | ((uint32_t)val << 16);
    } else {
        aligned = (aligned & 0xFFFF0000) | val;
    }
    pci_cfg_write32(bdf, offset, aligned);
}

int pci_find_device(uint16_t vendor, uint16_t device, pci_bdf_t *out)
{
    pci_bdf_t bdf;
    for (bdf.bus = 0; bdf.bus < 1; bdf.bus++) {
        for (bdf.dev = 0; bdf.dev < 32; bdf.dev++) {
            bdf.func = 0;
            uint16_t vid = pci_cfg_read16(bdf, PCI_CFG_VENDOR_ID);
            if (vid == 0xFFFF) {
                continue;
            }
            uint16_t did = pci_cfg_read16(bdf, PCI_CFG_DEVICE_ID);
            if (vid == vendor && did == device) {
                *out = bdf;
                return 0;
            }
        }
    }
    return -1;
}

void pci_enable_device(pci_bdf_t bdf)
{
    uint16_t cmd = pci_cfg_read16(bdf, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER;
    pci_cfg_write16(bdf, PCI_CFG_COMMAND, cmd);
}

uint64_t pci_read_bar(pci_bdf_t bdf, int bar_num)
{
    uint8_t offset = PCI_CFG_BAR0 + bar_num * 4;
    uint32_t lo = pci_cfg_read32(bdf, offset);

    /* Check if 64-bit BAR (bit 1:0 == 10b) */
    if ((lo & 0x7) == 0x4) {
        uint32_t hi = pci_cfg_read32(bdf, offset + 4);
        return ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0);
    }
    return lo & 0xFFFFFFF0;
}
