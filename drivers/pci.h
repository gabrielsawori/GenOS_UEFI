#pragma once
#include <stdint.h>
#define PCI_MAX_DEVICES 128
struct pci_device {
    uint16_t vendor, device;
    uint8_t bus, slot, function, class_code, subclass, prog_if, irq, header;
    uint32_t bar[6]; /* Raw BARs; enumeration does not probe or modify devices. */
};
void pci_init(void);
uint32_t pci_count(void);
int pci_get(uint32_t index, struct pci_device* out);
/* Kernel driver command update; writes only 16-bit Command, never W1C Status. */
void pci_update_command(const struct pci_device* device, uint16_t set, uint16_t clear);
