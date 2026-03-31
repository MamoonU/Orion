#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI config space I/O ports (x86)
#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

// PCI config space register offsets

// identity
#define PCI_VENDOR_ID       0x00    // 16-bit
#define PCI_DEVICE_ID       0x02    // 16-bit

// control/status
#define PCI_COMMAND         0x04    // 16-bit
#define PCI_STATUS          0x06    // 16-bit

// header info
#define PCI_HEADER_TYPE     0x0E    // 8-bit

// base address registers
#define PCI_BAR0            0x10    // 32-bit
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24

// interrupts
#define PCI_INTERRUPT_LINE  0x3C    // 8-bit - IRQ number
#define PCI_INTERRUPT_PIN   0x3D    // 8-bit

// PCI command register bits
#define PCI_CMD_IO          (1u << 0)   // enable I/O space access
#define PCI_CMD_BUS_MASTER  (1u << 2)   // enable bus mastering (DMA)

// PCI device
typedef struct {
    uint8_t  bus;               // unique address
    uint8_t  device;
    uint8_t  function;

    uint16_t vendor_id;         // what device is
    uint16_t device_id;

    uint8_t  irq_line;          // interrupt line

    uint32_t bar[6];            // raw BAR values (bit 0 set = I/O space)
} pci_device_t;

// config space access
uint32_t pci_read32 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_read8  (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val);

// scan PCI buses for vendor+device ID match
int  pci_find_device(uint16_t vendor, uint16_t device_id, pci_device_t *out);

// enable I/O space + bus mastering on a device (required before using it)
void pci_enable_device(const pci_device_t *dev);

// initialise PCI subsystem
void pci_init(void);

#endif