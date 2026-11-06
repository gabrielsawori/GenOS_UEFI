#include "virtio.h"
#include "virtio_irq.h"
#include "io.h"
#include "serial.h"
#include "../mm/pmm.h"
#include "../cpu/preempt.h"
#include "../kernel/utils.h"
#include "../limine.h"

extern volatile struct limine_hhdm_request hhdm_request;
_Static_assert(sizeof(struct virtio_desc) == 16, "virtqueue descriptor ABI");

void* virtio_dma_alloc(unsigned pages, uint64_t* physical) {
    *physical = (uint64_t)pmm_alloc_contiguous_pages(pages);
    if (!*physical) return NULL;
    void* mapped = (void*)(*physical + hhdm_request.response->offset);
    memset(mapped, 0, (size_t)pages * PAGE_SIZE);
    return mapped;
}
void virtio_dma_free(uint64_t physical, unsigned pages) {
    if (!physical) return;
    for (unsigned i = 0; i < pages; i++) pmm_free_page((void*)(physical + i * PAGE_SIZE));
}

static int reset(struct virtio_device* dev) {
    outb(dev->io + 18, 0);
    for (unsigned i = 0; i < 100000; i++) {
        if (!inb(dev->io + 18)) return 1;
        asm volatile("pause" ::: "memory");
    }
    return 0;
}

int virtio_open(struct virtio_device* dev, const struct pci_device* pci, uint32_t accepted) {
    if (!preempt_local_depth()) __builtin_trap();
    memset(dev, 0, sizeof(*dev));
    uint32_t bar = pci->bar[0];
    if (pci->vendor != 0x1af4 || !(bar & 1) || !(bar & ~3U) ||
        (bar & ~3U) > 0xffc0 || pci->header != 0) return 0;
    dev->pci = *pci;
    dev->io = bar & ~3U;
    /* Keep INTx masked until the IRQ registry and queues are ready. */
    pci_update_command(pci, 1 | (1U << 10), 4);
    if (!reset(dev)) { dev->quarantined = 1; return 0; }
    outb(dev->io + 18, 1);
    outb(dev->io + 18, 3);
    dev->features = inl(dev->io) & accepted;
    outl(dev->io + 4, dev->features);
    return 1;
}

int virtio_queue_init(struct virtio_device* dev, unsigned index) {
    if (index >= 2 || dev->queue[index].physical) return 0;
    outw(dev->io + 14, index);
    uint16_t size = inw(dev->io + 12);
    if (size < 4 || size > 1024 || (size & (size - 1)) || inl(dev->io + 8)) return 0;
    struct virtio_queue* q = &dev->queue[index];
    unsigned used_offset = (16U * size + 6U + 2U * size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    q->pages = (used_offset + 6U + 8U * size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t* memory = virtio_dma_alloc(q->pages, &q->physical);
    if (!memory) return 0;
    if ((q->physical >> 12) > UINT32_MAX) {
        virtio_dma_free(q->physical, q->pages); memset(q, 0, sizeof(*q)); return 0;
    }
    q->index = index; q->size = size;
    q->descriptors = (void*)memory;
    uint16_t* avail = (void*)(memory + 16U * size);
    avail[0] = 1; /* VIRTQ_AVAIL_F_NO_INTERRUPT */
    q->avail_idx = avail + 1; q->avail_ring = avail + 2;
    q->used_idx = (void*)(memory + used_offset + 2);
    q->used_ring = (void*)(memory + used_offset + 4);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    outl(dev->io + 8, q->physical >> 12);
    return 1;
}

void virtio_ready(struct virtio_device* dev) {
    dev->irq_enabled = virtio_irq_attach(dev->io, dev->pci.irq);
    for (unsigned i = 0; i < 2; i++) {
        struct virtio_queue* q = &dev->queue[i];
        if (q->physical) __atomic_store_n(q->avail_idx - 1, dev->irq_enabled ? 0 : 1, __ATOMIC_RELEASE);
    }
    pci_update_command(&dev->pci, 4 | (dev->irq_enabled ? 0 : 1U << 10),
                       dev->irq_enabled ? 1U << 10 : 0);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    outb(dev->io + 18, 7); /* Legacy transport does not use FEATURES_OK. */
    dev->ready = 1;
}

int virtio_stop(struct virtio_device* dev) {
    dev->ready = 0;
    if (!dev->io) return 1;
    pci_update_command(&dev->pci, 1U << 10, 0);
    virtio_irq_detach(dev->io);
    dev->irq_enabled = 0;
    int stopped = reset(dev);
    pci_update_command(&dev->pci, 1U << 10, 4);
    if (!stopped) {
        dev->quarantined = 1;
        serial_write_string("[WARN] virtio reset unacknowledged; retaining DMA buffers\n");
        return 0;
    }
    for (unsigned i = 0; i < 2; i++) {
        virtio_dma_free(dev->queue[i].physical, dev->queue[i].pages);
        memset(&dev->queue[i], 0, sizeof(dev->queue[i]));
    }
    return 1;
}

void virtio_submit(struct virtio_queue* q, uint16_t descriptor) {
    if (descriptor >= q->size || q->outstanding >= q->size) __builtin_trap();
    q->outstanding++;
    q->avail_ring[q->available % q->size] = descriptor;
    __atomic_store_n(q->avail_idx, ++q->available, __ATOMIC_RELEASE);
}
void virtio_notify(struct virtio_device* dev, unsigned queue) {
    asm volatile("" ::: "memory");
    outw(dev->io + 16, queue);
}
int virtio_pop(struct virtio_queue* q, struct virtio_used* used) {
    uint16_t available = __atomic_load_n(q->used_idx, __ATOMIC_ACQUIRE);
    uint16_t count = available - q->consumed;
    if (!count) return 0;
    if (count > q->outstanding) return -1;
    volatile struct virtio_used* entry = &q->used_ring[q->consumed % q->size];
    *used = (struct virtio_used){entry->id, entry->length};
    if (used->id >= q->size) return -1;
    q->consumed++;
    q->outstanding--;
    return 1;
}
uint64_t virtio_read_config64(struct virtio_device* dev, unsigned offset) {
    uint64_t old = UINT64_MAX;
    for (unsigned i = 0; i < 16; i++) {
        uint64_t value = inl(dev->io + 20 + offset);
        value |= (uint64_t)inl(dev->io + 24 + offset) << 32;
        if (value == old) return value;
        old = value;
    }
    return 0;
}
int virtio_config_changed(struct virtio_device* dev) {
    if (!preempt_local_depth()) __builtin_trap();
    return dev->irq_enabled ? virtio_irq_take_config(dev->io) : !!(inb(dev->io + 19) & 2);
}
