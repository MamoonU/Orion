// pci.c - Minimal PCI bus driver

#include "pci.h"
#include "ioport.h"
#include "kprintf.h"

// build 32-bit address value written to CONFIG_ADDRESS
static uint32_t pci_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1Fu) << 11) | ((uint32_t)(func & 0x07u) << 8) | ((uint32_t)(offset) & 0xFCu);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, dev, func, offset & ~3u);
    return (uint16_t)(v >> ((offset & 2u) * 8u));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, dev, func, offset & ~3u);
    return (uint8_t)(v >> ((offset & 3u) * 8u));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t v     = pci_read32(bus, dev, func, offset & ~3u);
    uint32_t shift = (offset & 2u) * 8u;
    v &= ~(0xFFFFu << shift);
    v |= ((uint32_t)val << shift);
    pci_write32(bus, dev, func, offset & ~3u, v);
}

// scan bus 0-7, device 0-31, function 0 only
int pci_find_device(uint16_t vendor, uint16_t device_id, pci_device_t *out) {

    for (uint8_t bus = 0; bus < 8; bus++) {                             // for bus = 0 - 7
        for (uint8_t dev = 0; dev < 32; dev++) {                        // for dev = 0 - 31

            uint32_t id = pci_read32(bus, dev, 0, PCI_VENDOR_ID);       // read vendor + device ID

            if ((id & 0xFFFF) == 0xFFFF) continue;                      // slot empty

            uint16_t vid = (uint16_t)(id & 0xFFFF);                     // vendor id
            uint16_t did = (uint16_t)(id >> 16);                        // device id

            if (vid != vendor || did != device_id) continue;            // match target device

            out->bus       = bus;                                       // fill output struct
            out->device    = dev;
            out->function  = 0;
            out->vendor_id = vid;
            out->device_id = did;

            out->irq_line  = pci_read8(bus, dev, 0, PCI_INTERRUPT_LINE);    // read IRQ line 

            for (int i = 0; i < 6; i++) {                                   // read BARS
                out->bar[i] = pci_read32(bus, dev, 0, PCI_BAR0 + (uint8_t)(i * 4));
            }

            return 1;
        }
    }
    return 0;
}

// enable I/O space + bus mastering: must be called before using the device
void pci_enable_device(const pci_device_t *d) {
    uint16_t cmd = pci_read16(d->bus, d->device, d->function, PCI_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_BUS_MASTER;
    pci_write16(d->bus, d->device, d->function, PCI_COMMAND, cmd);
}

void pci_init(void) {
    kprintf("PCI: config space access ready (ports 0xCF8/0xCFC)\n");
}