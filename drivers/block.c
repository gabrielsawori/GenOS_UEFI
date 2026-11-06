#include "block.h"
#include "virtio.h"
#include "serial.h"
#include "io_request.h"
#include "timer.h"
#include "../cpu/preempt.h"
#include "../cpu/device_lock.h"
#include "../kernel/utils.h"
#include "../kernel/task.h"

#define BLK_RO (1U << 5)
#define BLK_FLUSH (1U << 9)
struct block_device {
    struct virtio_device transport;
    struct block_info info;
    uint64_t physical;
    uint8_t* dma;
};
struct block_request { uint32_t type, reserved; uint64_t sector; };
_Static_assert(sizeof(struct block_request) == 16, "virtio-blk header ABI");
static struct block_device disks[2];
static struct io_request requests[2];
static spinlock_t locks[2]; /* Permanent, including across shutdown/reinit. */
static unsigned disk_count;
static int initialized;
static void service_disk(unsigned index);
static void check_configuration(unsigned index);

static int stop(struct block_device* disk) {
    struct io_request* r = &requests[disk - disks];
    io_request_cancel_waiters(r);
    if (r->result == DRIVER_OK) io_request_abort(r, DRIVER_CANCELED);
    else io_request_complete(r, DRIVER_CANCELED);
    disk->info.online = 0;
    if (!virtio_stop(&disk->transport)) return 0;
    virtio_dma_free(disk->physical, 2);
    disk->physical = 0; disk->dma = NULL;
    return 1;
}

void block_init(void) {
    PREEMPT_GUARD;
    if (initialized) return;
    initialized = 1;
    for (unsigned i = 0; i < pci_count() && block_count() < 2; i++) {
        struct pci_device pci;
        if (pci_get(i, &pci) || pci.vendor != 0x1af4 || pci.device != 0x1001) continue;
        unsigned index = block_count();
        DEVICE_LOCK_GUARD(&locks[index]);
        struct block_device* disk = &disks[index];
        memset(disk, 0, sizeof(*disk));
        struct virtio_device* dev = &disk->transport;
        if (!virtio_open(dev, &pci, BLK_RO | BLK_FLUSH) || !virtio_queue_init(dev, 0)) goto failed;
        disk->info.sectors = virtio_read_config64(dev, 0);
        if (!disk->info.sectors) goto failed;
        disk->dma = virtio_dma_alloc(2, &disk->physical);
        if (!disk->dma) goto failed;
        disk->info.read_only = !!(dev->features & BLK_RO);
        disk->info.can_flush = !!(dev->features & BLK_FLUSH);
        virtio_ready(dev);
        disk->info.online = 1;
        __atomic_store_n(&disk_count, index + 1, __ATOMIC_RELEASE);
        serial_write_string("[OK] virtio-blk: disk online (512-byte sectors)\n");
        continue;
failed:
        stop(disk);
        __atomic_store_n(&disk_count, index + 1, __ATOMIC_RELEASE);
        serial_write_string("[WARN] virtio-blk: disk initialization failed\n");
    }
}

unsigned block_count(void) { return __atomic_load_n(&disk_count, __ATOMIC_ACQUIRE); }
int block_get_info(unsigned index, struct block_info* out) {
    if (index >= 2 || !out) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[index]);
    if (index >= block_count()) return DRIVER_INVALID;
    *out = disks[index].info;
    virtio_irq_get_info(disks[index].transport.io, &out->irq);
    return DRIVER_OK;
}

static int submit_owned(unsigned index, enum block_operation type, uint64_t sector, unsigned count,
                        const void* buffer, struct driver_token* token, uint32_t owner,
                        uint64_t* admission, uint64_t deadline) {
    if (!virtio_irqs_enabled()) return DRIVER_INVALID;
    if (index >= 2 || !token || (type != 0 && type != 1 && type != 4)) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[index]);
    if (index >= block_count()) return DRIVER_INVALID;
    check_configuration(index);
    struct block_device* disk = &disks[index];
    if (!disk->info.online || !io_service_ready()) return DRIVER_OFFLINE;
    if (type != 4 && ((type == 1 && !buffer) || !count || count > BLOCK_MAX_SECTORS ||
        sector >= disk->info.sectors || count > disk->info.sectors - sector)) return DRIVER_INVALID;
    if (type == 4 && (sector || count || buffer)) return DRIVER_INVALID;
    if (type == 1 && disk->info.read_only) return DRIVER_READ_ONLY;
    if (type == 4 && !disk->info.can_flush) return DRIVER_UNSUPPORTED;
    struct io_request* r = &requests[index];
    int result = io_request_begin_owned(r, index, token, owner, admission, deadline);
    if (result) return result;
    r->operation = type; r->bytes = count * BLOCK_SECTOR_SIZE;
    struct virtio_device* dev = &disk->transport;
    struct virtio_queue* q = &dev->queue[0];
    struct block_request* header = (void*)disk->dma;
    *header = (struct block_request){.type = type, .sector = sector};
    volatile uint8_t* status = disk->dma + sizeof(*header);
    *status = 0xff;
    unsigned bytes = count * BLOCK_SECTOR_SIZE;
    if (type == 1) memcpy(disk->dma + 4096, buffer, bytes);
    q->descriptors[0] = (struct virtio_desc){disk->physical, sizeof(*header), VIRTIO_DESC_NEXT, type == 4 ? 2 : 1};
    q->descriptors[1] = (struct virtio_desc){disk->physical + 4096, bytes,
        VIRTIO_DESC_NEXT | (type == 0 ? VIRTIO_DESC_WRITE : 0), 2};
    q->descriptors[2] = (struct virtio_desc){disk->physical + sizeof(*header), 1, VIRTIO_DESC_WRITE, 0};
    virtio_submit(q, 0);
    virtio_notify(dev, 0);
    disk->info.requests++;
    return DRIVER_OK;
}
int block_submit(unsigned index, enum block_operation type, uint64_t sector, unsigned count,
                 const void* buffer, struct driver_token* token) {
    return submit_owned(index, type, sector, count, buffer, token, 0, NULL, 0);
}

static void poll_request(unsigned index) {
    struct io_request* r = &requests[index];
    if (!r->pending) return;
    struct block_device* disk = &disks[index];
    struct virtio_used used;
    int result = virtio_pop(&disk->transport.queue[0], &used);
    if (!result && timer_get_ticks() - r->started < 500) return;
    volatile uint8_t* status = disk->dma + sizeof(struct block_request);
    if (result != 1 || used.id != 0 || *status > 2) {
        disk->info.errors++;
        io_request_complete(r, result == 0 ? DRIVER_TIMEOUT : DRIVER_IO);
        stop(disk);
        return;
    }
    /* Legacy devices may report an inaccurate used length; status is authoritative. */
    if (*status) {
        disk->info.errors++;
        io_request_complete(r, *status == 2 ? DRIVER_UNSUPPORTED : DRIVER_IO);
        return;
    }
    io_request_complete(r, DRIVER_OK);
}
static void check_configuration(unsigned i) {
    struct block_device* disk = &disks[i];
    if (disk->info.online && virtio_config_changed(&disk->transport) &&
        virtio_read_config64(&disk->transport, 0) != disk->info.sectors) {
        /* Filesystem geometry changes need explicit revalidation later. */
        io_request_abort(&requests[i], DRIVER_IO);
        stop(disk);
    }
}
static void service_disk(unsigned i) {
    io_request_expire_waiters(&requests[i]);
    check_configuration(i);
    poll_request(i);
}
void block_service(void) {
    unsigned count = block_count();
    for (unsigned i = 0; i < count; i++) {
        uint64_t id;
        uint32_t owner;
        {
            DEVICE_TRY_GUARD(&locks[i]);
            if (!device_guard.lock) continue;
            id = requests[i].id; owner = requests[i].owner_pid;
            if (!owner) { service_disk(i); continue; }
        }
        /* Task list uses the global guard. Never take it under a device lock.
         * Token revalidation prevents a stale owner snapshot canceling new I/O. */
        int dead = task_is_dead(owner);
        DEVICE_TRY_GUARD(&locks[i]);
        if (!device_guard.lock) continue;
        if (dead && requests[i].id == id && requests[i].owner_pid == owner) {
            if (requests[i].pending) stop(&disks[i]);
            io_request_release(&requests[i]);
        }
        service_disk(i);
    }
}
int block_collect(struct driver_token token, void* buffer, unsigned capacity) {
    if (token.device >= 2 || !token.id) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[token.device]);
    struct io_request* r = &requests[token.device];
    if (!io_request_matches(r, token)) return DRIVER_CANCELED;
    service_disk(token.device);
    if (!io_request_matches(r, token)) return DRIVER_CANCELED;
    if (r->pending) return DRIVER_PENDING;
    int result = r->result;
    if (!result && r->operation == BLOCK_READ) {
        if (!buffer || capacity < r->bytes) return DRIVER_BUFFER_SMALL;
        memcpy(buffer, disks[token.device].dma + 4096, r->bytes);
    }
    io_request_release(r);
    return result;
}
int block_cancel(struct driver_token token) {
    if (token.device >= 2 || !token.id) return DRIVER_INVALID;
    DEVICE_LOCK_GUARD(&locks[token.device]);
    struct io_request* r = &requests[token.device];
    if (!io_request_matches(r, token)) return DRIVER_CANCELED;
    if (r->pending) stop(&disks[token.device]);
    io_request_release(r);
    return DRIVER_OK;
}

static int request(unsigned index, enum block_operation type, uint64_t sector, unsigned count, void* buffer) {
    if (!io_can_block() || (type != BLOCK_FLUSH && !buffer)) return DRIVER_INVALID;
    if (index >= 2) return DRIVER_INVALID;
    struct wait_queue* changed = &requests[index].changed;
    struct driver_token token;
    uint32_t owner = get_current_pid();
    uint64_t admission = 0;
    uint64_t deadline = timer_get_ticks() + 1000;
    for (;;) {
        uint64_t observed = wait_queue_snapshot(changed);
        int result = submit_owned(index, type, sector, count,
                                  type == BLOCK_WRITE ? buffer : NULL, &token, owner, &admission, deadline);
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
        int result = block_collect(token, buffer, count * BLOCK_SECTOR_SIZE);
        if (result != DRIVER_PENDING) return result;
        wait_queue_wait(changed, observed, UINT64_MAX);
    }
}
int block_read(unsigned disk, uint64_t sector, unsigned count, void* buffer) {
    return request(disk, 0, sector, count, buffer);
}
int block_write(unsigned disk, uint64_t sector, unsigned count, const void* buffer) {
    return request(disk, 1, sector, count, (void*)buffer);
}
int block_flush(unsigned disk) { return request(disk, 4, 0, 0, NULL); }
int block_shutdown(void) {
    PREEMPT_GUARD;
    int stopped = 1;
    for (unsigned i = 0; i < block_count(); i++) {
        DEVICE_LOCK_GUARD(&locks[i]);
        if (!stop(&disks[i])) stopped = 0;
        io_request_release(&requests[i]);
    }
    if (stopped) { initialized = 0; __atomic_store_n(&disk_count, 0, __ATOMIC_RELEASE); }
    return stopped ? DRIVER_OK : DRIVER_IO;
}
