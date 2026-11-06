#include "net.h"
#include "virtio.h"
#include "io.h"
#include "serial.h"
#include "io_request.h"
#include "timer.h"
#include "../cpu/preempt.h"
#include "../cpu/device_lock.h"
#include "../kernel/utils.h"
#include "../kernel/task.h"

#define NET_MAC (1U << 5)
#define NET_STATUS (1U << 16)
#define RX_SLOTS 32
#define RX_BYTES 2048
#define RX_PAGES (RX_SLOTS * RX_BYTES / 4096)
#define HEADER_BYTES 10 /* Legacy, no mergeable RX buffers or offload features. */
struct net_device {
    struct virtio_device transport;
    struct net_info info;
    uint64_t rx_physical, tx_physical;
    uint8_t *rx, *tx;
    unsigned slots;
};
static struct net_device interfaces[2];
static struct io_request requests[2];
static spinlock_t locks[2];
static struct wait_queue rx_changed[2];
static uint64_t rx_epoch[2];
static uint16_t rx_seen[2];
static unsigned interface_count;
static int initialized;

static int stop(struct net_device* nic) {
    unsigned index = nic - interfaces;
    io_request_cancel_waiters(&requests[index]);
    io_request_complete(&requests[index], DRIVER_CANCELED);
    rx_epoch[index]++;
    wait_queue_signal(&rx_changed[index]);
    nic->info.online = nic->info.link_up = 0;
    if (!virtio_stop(&nic->transport)) return 0;
    virtio_dma_free(nic->rx_physical, RX_PAGES);
    virtio_dma_free(nic->tx_physical, 1);
    nic->rx_physical = nic->tx_physical = 0;
    nic->rx = nic->tx = NULL;
    return 1;
}
static void link_status(struct net_device* nic) {
    nic->info.link_up = nic->info.online && (!(nic->transport.features & NET_STATUS) ||
        (inw(nic->transport.io + 26) & 1));
}
void net_init(void) {
    PREEMPT_GUARD;
    if (initialized) return;
    initialized = 1;
    for (unsigned i = 0; i < pci_count() && net_count() < 2; i++) {
        struct pci_device pci;
        if (pci_get(i, &pci) || pci.vendor != 0x1af4 || pci.device != 0x1000) continue;
        unsigned index = net_count();
        DEVICE_LOCK_GUARD(&locks[index]);
        struct net_device* nic = &interfaces[index];
        memset(nic, 0, sizeof(*nic));
        struct virtio_device* dev = &nic->transport;
        if (!virtio_open(dev, &pci, NET_MAC | NET_STATUS) || !(dev->features & NET_MAC) ||
            !virtio_queue_init(dev, 0) || !virtio_queue_init(dev, 1)) goto failed;
        nic->rx = virtio_dma_alloc(RX_PAGES, &nic->rx_physical);
        nic->tx = virtio_dma_alloc(1, &nic->tx_physical);
        if (!nic->rx || !nic->tx) goto failed;
        for (unsigned j = 0; j < 6; j++) nic->info.mac[j] = inb(dev->io + 20 + j);
        struct virtio_queue* rx = &dev->queue[0];
        nic->slots = rx->size < RX_SLOTS ? rx->size : RX_SLOTS;
        for (unsigned j = 0; j < nic->slots; j++) {
            rx->descriptors[j] = (struct virtio_desc){nic->rx_physical + j * RX_BYTES,
                RX_BYTES, VIRTIO_DESC_WRITE, 0};
            virtio_submit(rx, j);
        }
        virtio_ready(dev);
        virtio_notify(dev, 0);
        nic->info.online = 1;
        rx_seen[nic - interfaces] = 0;
        link_status(nic);
        __atomic_store_n(&interface_count, index + 1, __ATOMIC_RELEASE);
        serial_write_string("[OK] virtio-net: Ethernet interface online\n");
        continue;
failed:
        stop(nic);
        __atomic_store_n(&interface_count, index + 1, __ATOMIC_RELEASE);
        serial_write_string("[WARN] virtio-net: interface initialization failed\n");
    }
}
unsigned net_count(void) { return __atomic_load_n(&interface_count, __ATOMIC_ACQUIRE); }
int net_get_info(unsigned index, struct net_info* out) {
    if (!out || index >= 2) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[index]);
    if (index >= net_count()) return DRIVER_INVALID;
    link_status(&interfaces[index]);
    *out = interfaces[index].info;
    virtio_irq_get_info(interfaces[index].transport.io, &out->irq);
    return DRIVER_OK;
}

static int submit_owned(unsigned index, const void* frame, size_t length,
                        struct driver_token* token, uint32_t owner, uint64_t* admission, uint64_t deadline) {
    if (!virtio_irqs_enabled() || !frame || length < 14 || length > NET_FRAME_MAX) return DRIVER_INVALID;
    if (index >= 2 || !token) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[index]);
    if (index >= net_count()) return DRIVER_INVALID;
    struct net_device* nic = &interfaces[index];
    link_status(nic);
    if (!nic->info.online || !nic->info.link_up || !io_service_ready()) return DRIVER_OFFLINE;
    int result = io_request_begin_owned(&requests[index], index, token, owner, admission, deadline);
    if (result) return result;
    size_t bytes = length < 60 ? 60 : length;
    memset(nic->tx, 0, HEADER_BYTES + bytes);
    memcpy(nic->tx + HEADER_BYTES, frame, length);
    struct virtio_device* dev = &nic->transport;
    struct virtio_queue* tx = &dev->queue[1];
    tx->descriptors[0] = (struct virtio_desc){nic->tx_physical, HEADER_BYTES + bytes, 0, 0};
    virtio_submit(tx, 0);
    virtio_notify(dev, 1);
    return DRIVER_OK;
}
int net_submit(unsigned index, const void* frame, size_t length, struct driver_token* token) {
    return submit_owned(index, frame, length, token, 0, NULL, 0);
}
static void poll_request(unsigned index) {
    struct io_request* r = &requests[index];
    if (!r->pending) return;
    struct net_device* nic = &interfaces[index];
    struct virtio_used used;
    int result = virtio_pop(&nic->transport.queue[1], &used);
    if (!result && timer_get_ticks() - r->started < 500) return;
    if (result != 1 || used.id != 0) {
        nic->info.errors++;
        io_request_complete(r, result == 0 ? DRIVER_TIMEOUT : DRIVER_IO);
        stop(nic);
        return;
    }
    nic->info.transmitted++;
    io_request_complete(r, DRIVER_OK);
}
void net_service(void) {
    unsigned count = net_count();
    for (unsigned i = 0; i < count; i++) {
        uint64_t id;
        uint32_t owner;
        {
            DEVICE_TRY_GUARD(&locks[i]);
            if (!device_guard.lock) continue;
            io_request_expire_waiters(&requests[i]);
            id = requests[i].id; owner = requests[i].owner_pid;
        }
        int dead = owner && task_is_dead(owner);
        DEVICE_TRY_GUARD(&locks[i]);
        if (!device_guard.lock) continue;
        if (dead && requests[i].id == id && requests[i].owner_pid == owner) {
            if (requests[i].pending) stop(&interfaces[i]);
            io_request_release(&requests[i]);
        }
        poll_request(i);
        if (!interfaces[i].info.online) continue;
        if (virtio_config_changed(&interfaces[i].transport)) {
            link_status(&interfaces[i]);
            wait_queue_signal(&rx_changed[i]);
        }
        uint16_t used = __atomic_load_n(interfaces[i].transport.queue[0].used_idx, __ATOMIC_ACQUIRE);
        if (used != rx_seen[i]) {
            rx_seen[i] = used;
            wait_queue_signal(&rx_changed[i]);
        }
    }
}
int net_collect(struct driver_token token) {
    if (token.device >= 2 || !token.id) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[token.device]);
    struct io_request* r = &requests[token.device];
    if (!io_request_matches(r, token)) return DRIVER_CANCELED;
    poll_request(token.device);
    if (!io_request_matches(r, token)) return DRIVER_CANCELED;
    if (r->pending) return DRIVER_PENDING;
    int result = r->result;
    io_request_release(r);
    return result;
}
int net_cancel(struct driver_token token) {
    if (token.device >= 2 || !token.id) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[token.device]);
    struct io_request* r = &requests[token.device];
    if (!io_request_matches(r, token)) return DRIVER_CANCELED;
    if (r->pending) stop(&interfaces[token.device]);
    io_request_release(r);
    return DRIVER_OK;
}
int net_send(unsigned index, const void* frame, size_t length) {
    if (!io_can_block() || index >= 2) return DRIVER_INVALID;
    struct wait_queue* changed = &requests[index].changed;
    struct driver_token token;
    uint32_t owner = get_current_pid();
    uint64_t admission = 0;
    uint64_t deadline = timer_get_ticks() + 1000;
    for (;;) {
        uint64_t observed = wait_queue_snapshot(changed);
        int result = submit_owned(index, frame, length, &token, owner, &admission, deadline);
        if (result == DRIVER_BUSY && wait_queue_wait(changed, observed, deadline) == WAIT_TIMEOUT)
            result = DRIVER_TIMEOUT;
        if (result != DRIVER_BUSY) {
            if (admission) {
                DEVICE_LOCK_GUARD(&locks[index]);
                io_request_cancel_waiter(&requests[index], admission);
            }
            if (result) return result;
            break;
        }
    }
    for (;;) {
        uint64_t observed = wait_queue_snapshot(changed);
        int result = net_collect(token);
        if (result != DRIVER_PENDING) return result;
        wait_queue_wait(changed, observed, UINT64_MAX);
    }
}

static int receive_locked(unsigned index, void* frame, size_t capacity) {
    if (index >= net_count()) return DRIVER_INVALID;
    struct net_device* nic = &interfaces[index];
    if (!nic->info.online) return DRIVER_OFFLINE;
    struct virtio_device* dev = &nic->transport;
    struct virtio_queue* rx = &dev->queue[0];
    struct virtio_used used;
    int result = virtio_pop(rx, &used);
    if (!result) return 0;
    if (result < 0 || used.id >= nic->slots) {
        nic->info.errors++;
        stop(nic);
        return DRIVER_IO;
    }
    uint8_t* data = nic->rx + used.id * RX_BYTES;
    if (used.length < HEADER_BYTES + 14 || used.length > HEADER_BYTES + NET_FRAME_MAX ||
        data[0] || data[1]) {
        result = DRIVER_IO;
    } else if (used.length - HEADER_BYTES > capacity) {
        result = DRIVER_BUFFER_SMALL;
    } else {
        result = used.length - HEADER_BYTES;
        memcpy(frame, data + HEADER_BYTES, result);
        nic->info.received++;
    }
    if (result < 0) nic->info.dropped++;
    virtio_submit(rx, used.id);
    virtio_notify(dev, 0);
    return result;
}
int net_receive(unsigned index, void* frame, size_t capacity) {
    if (!virtio_irqs_enabled() || !frame || !capacity || index >= 2) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[index]);
    return receive_locked(index, frame, capacity);
}
int net_shutdown(void) {
    PREEMPT_GUARD;
    int stopped = 1;
    for (unsigned i = 0; i < net_count(); i++) {
        DEVICE_LOCK_GUARD(&locks[i]);
        if (!stop(&interfaces[i])) stopped = 0;
        io_request_release(&requests[i]);
    }
    if (stopped) { initialized = 0; __atomic_store_n(&interface_count, 0, __ATOMIC_RELEASE); }
    return stopped ? DRIVER_OK : DRIVER_IO;
}

int net_receive_wait(unsigned index, void* frame, size_t capacity, uint64_t deadline) {
    if (!io_can_block() || index >= 2 || !frame || !capacity) return DRIVER_INVALID;
    uint64_t epoch;
    { DEVICE_LOCK_GUARD(&locks[index]); epoch = rx_epoch[index]; }
    for (;;) {
        uint64_t observed;
        {
            DEVICE_LOCK_GUARD(&locks[index]);
            if (epoch != rx_epoch[index]) return DRIVER_CANCELED;
            observed = wait_queue_snapshot(&rx_changed[index]);
            int result = receive_locked(index, frame, capacity);
            if (result) return result;
            link_status(&interfaces[index]);
            if (!interfaces[index].info.link_up) return DRIVER_OFFLINE;
        }
        if (wait_queue_wait(&rx_changed[index], observed, deadline) == WAIT_TIMEOUT)
            return DRIVER_TIMEOUT;
    }
}
