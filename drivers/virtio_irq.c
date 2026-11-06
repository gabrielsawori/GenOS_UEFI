#include "virtio_irq.h"
#include "io.h"
#include "io_request.h"
#include "../cpu/irq_lock.h"
#include "../cpu/preempt.h"
#include "../cpu/pic.h"

static struct irq_route {
    uint16_t io;
    uint8_t irq, config_pending;
    uint64_t queues, config;
} routes[4];
static spinlock_t route_lock = SPINLOCK_INIT;
static struct virtio_irq_stats stats;

int virtio_irq_attach(uint16_t io, uint8_t irq) {
    if (!preempt_depth() && !preempt_local_depth()) __builtin_trap();
    if (!io || !pic_enable_pci_irq(irq)) return 0;
    IRQ_LOCK_GUARD(&route_lock);
    struct irq_route* free = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (routes[i].io == io) return 0;
        if (!routes[i].io) free = &routes[i];
    }
    if (!free) return 0;
    *free = (struct irq_route){.io = io, .irq = irq};
    stats.registered++;
    return 1;
}
void virtio_irq_detach(uint16_t io) {
    if (!preempt_depth() && !preempt_local_depth()) __builtin_trap();
    IRQ_LOCK_GUARD(&route_lock);
    for (unsigned i = 0; i < 4; i++) if (io && routes[i].io == io) {
        (void)inb(io + 19); /* Drain the latched status with PCI INTx masked. */
        routes[i] = (struct irq_route){0};
        stats.registered--;
    }
    /* Do not mask a PIC line: another registered device may still share it. */
}
void virtio_irq_dispatch(uint8_t irq) {
    unsigned matched = 0, claimed = 0;
    {
        IRQ_LOCK_GUARD(&route_lock);
        stats.vectors++;
        for (unsigned i = 0; i < 4; i++) {
            struct irq_route* route = &routes[i];
            if (!route->io || route->irq != irq) continue;
            matched++;
            uint8_t status = inb(route->io + 19); /* Read acknowledges/deasserts INTx. */
            if (status & 1) { route->queues++; stats.queue_events++; }
            if (status & 2) {
                route->config++; stats.config_events++; route->config_pending = 1;
            }
            if (status & 3) claimed++;
        }
        if (matched > 1) stats.shared++;
        if (claimed) stats.claimed++;
        else stats.unclaimed++;
    }
    /* Only atomic event publication here: no allocation or mutation guard. */
    if (claimed) io_service_notify();
}
void virtio_irq_get_info(uint16_t io, struct virtio_irq_info* out) {
    IRQ_LOCK_GUARD(&route_lock);
    if (!out) return;
    *out = (struct virtio_irq_info){0};
    for (unsigned i = 0; i < 4; i++) if (io && routes[i].io == io)
        *out = (struct virtio_irq_info){routes[i].queues, routes[i].config, 1, routes[i].irq};
}
int virtio_irq_take_config(uint16_t io) {
    IRQ_LOCK_GUARD(&route_lock);
    for (unsigned i = 0; i < 4; i++) if (io && routes[i].io == io) {
        int pending = routes[i].config_pending;
        routes[i].config_pending = 0;
        return pending;
    }
    return 0;
}
void virtio_irq_get_stats(struct virtio_irq_stats* out) {
    IRQ_LOCK_GUARD(&route_lock);
    if (out) *out = stats;
}
