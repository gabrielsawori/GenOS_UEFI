#include "pci.h"
#include "io.h"
#include "serial.h"
#include "../cpu/preempt.h"
#include "../cpu/irq_lock.h"
#include "../kernel/utils.h"

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t count;
static spinlock_t config_lock = SPINLOCK_INIT;
/* PCI configuration mechanism #1 on the QEMU PC. Serialize address/data pair. */
static uint32_t config_unlocked(uint32_t bus, uint32_t slot, uint32_t fn, uint32_t reg) {
    outl(0xcf8, 0x80000000U | (bus << 16) | (slot << 11) | (fn << 8) | (reg & 252));
    return inl(0xcfc);
}
static uint32_t config(uint32_t bus, uint32_t slot, uint32_t fn, uint32_t reg) {
    IRQ_LOCK_GUARD(&config_lock);
    return config_unlocked(bus, slot, fn, reg);
}

void pci_init(void) {
    PREEMPT_GUARD;
    count = 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned slot = 0; slot < 32; slot++) {
            if ((config(bus, slot, 0, 0) & 65535) == 65535) continue;
            unsigned functions = config(bus, slot, 0, 12) & (1U << 23) ? 8 : 1;
            for (unsigned fn = 0; fn < functions; fn++) {
                uint32_t id = config(bus, slot, fn, 0);
                if ((id & 65535) == 65535) continue;
                if (count == PCI_MAX_DEVICES) {
                    serial_write_string("[WARN] PCI inventory full\n");
                    return;
                }
                uint32_t cls = config(bus, slot, fn, 8);
                struct pci_device* d = &devices[count++];
                *d = (struct pci_device){.vendor = id, .device = id >> 16,
                    .bus = bus, .slot = slot, .function = fn,
                    .class_code = cls >> 24, .subclass = cls >> 16, .prog_if = cls >> 8,
                    .header = (config(bus, slot, fn, 12) >> 16) & 127,
                    .irq = config(bus, slot, fn, 60)};
                unsigned bars = d->header == 0 ? 6 : d->header == 1 ? 2 : 0;
                for (unsigned i = 0; i < bars; i++) d->bar[i] = config(bus, slot, fn, 16 + 4 * i);
            }
        }
    }
    char number[24];
    itoa(count, number, 10);
    serial_write_string("[OK] PCI inventory: ");
    serial_write_string(number);
    serial_write_string(" devices\n");
}
uint32_t pci_count(void) { return count; }
int pci_get(uint32_t index, struct pci_device* out) {
    if (!out || index >= count) return -1;
    *out = devices[index];
    return 0;
}

void pci_update_command(const struct pci_device* d, uint16_t set, uint16_t clear) {
    IRQ_LOCK_GUARD(&config_lock);
    uint16_t command = config_unlocked(d->bus, d->slot, d->function, 4);
    outw(0xcfc, (command | set) & ~clear);
}
