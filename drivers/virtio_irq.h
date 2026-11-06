#pragma once
#include <stdint.h>

/* Registry copies ports/IRQ numbers; top half never follows a device/DMA/task
 * pointer. Task-context attach/detach holds the mutation guard. The registry
 * has its own IRQ-save leaf lock, also serializing detach against in-flight
 * port reads. PCI INTx must be masked before detach. */
struct virtio_irq_info { uint64_t queues, config; uint8_t enabled, line; };
struct virtio_irq_stats {
    uint64_t vectors, claimed, shared, unclaimed, queue_events, config_events;
    uint32_t registered;
};
int virtio_irq_attach(uint16_t io, uint8_t irq);
void virtio_irq_detach(uint16_t io);
void virtio_irq_dispatch(uint8_t irq);
void virtio_irq_get_info(uint16_t io, struct virtio_irq_info* out);
void virtio_irq_get_stats(struct virtio_irq_stats* out);
/* Task-context worker consumes config notification, never in IRQ context. */
int virtio_irq_take_config(uint16_t io);
