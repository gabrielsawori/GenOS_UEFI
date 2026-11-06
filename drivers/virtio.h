#pragma once
#include <stdint.h>
#include "pci.h"

/* Initial QEMU transport: transitional PCI legacy I/O BAR, split queues.
 * No MSI-X, indirect descriptors, event-index, packed queues or offloads.
 * Queue operations hold PREEMPT_GUARD only for bounded state transitions.
 * INTx top half acknowledges and signals; the worker handles queues/timeout. */
#define VIRTIO_DESC_NEXT 1
#define VIRTIO_DESC_WRITE 2
struct virtio_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct virtio_used { uint32_t id, length; };
struct virtio_queue {
    uint64_t physical;
    uint32_t pages;
    uint16_t size, index, available, consumed, outstanding;
    struct virtio_desc* descriptors;
    uint16_t* avail_ring;
    uint16_t* avail_idx;
    volatile uint16_t* used_idx;
    volatile struct virtio_used* used_ring;
};
struct virtio_device {
    struct pci_device pci;
    uint16_t io;
    uint8_t ready, quarantined, irq_enabled;
    uint32_t features;
    struct virtio_queue queue[2];
};

int virtio_open(struct virtio_device* dev, const struct pci_device* pci, uint32_t accepted);
int virtio_queue_init(struct virtio_device* dev, unsigned index);
void virtio_ready(struct virtio_device* dev);
/* Reset acknowledgment precedes release. Failure retains DMA storage. */
int virtio_stop(struct virtio_device* dev);
void virtio_submit(struct virtio_queue* queue, uint16_t descriptor);
void virtio_notify(struct virtio_device* dev, unsigned queue);
int virtio_pop(struct virtio_queue* queue, struct virtio_used* used);
static inline int virtio_irqs_enabled(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    return !!(flags & 512);
}
void* virtio_dma_alloc(unsigned pages, uint64_t* physical);
void virtio_dma_free(uint64_t physical, unsigned pages);
uint64_t virtio_read_config64(struct virtio_device* dev, unsigned offset);
int virtio_config_changed(struct virtio_device* dev);
