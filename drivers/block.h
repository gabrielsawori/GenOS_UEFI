#pragma once
#include <stdint.h>
#include "device.h"
#include "virtio_irq.h"

#define BLOCK_SECTOR_SIZE 512
#define BLOCK_MAX_SECTORS 8
struct block_info {
    uint64_t sectors, requests, errors;
    uint8_t online, read_only, can_flush;
    struct virtio_irq_info irq;
};
void block_init(void);
enum block_operation { BLOCK_READ = 0, BLOCK_WRITE = 1, BLOCK_FLUSH = 4 };
/* Task context only. Data path takes one device lock, with IRQs enabled;
 * lifecycle init/shutdown also takes the global mutation guard.
 * One reserved slot per disk until collect/cancel. Async submit yields to the
 * blocking callers' admission FIFO (BUSY). Write data is copied now;
 * reads provide the destination at collect. No request waits under a lock. */
int block_submit(unsigned disk, enum block_operation operation, uint64_t sector,
                 unsigned count, const void* write_data, struct driver_token* token);
/* PENDING, final error or OK. A small read buffer preserves the result for retry. */
int block_collect(struct driver_token token, void* buffer, unsigned capacity);
int block_cancel(struct driver_token token);
void block_service(void); /* Completion worker; bounded, never sleeps. */
/* Count includes recognized devices that failed initialization (online == 0). */
unsigned block_count(void);
int block_get_info(unsigned disk, struct block_info* out);
int block_read(unsigned disk, uint64_t sector, unsigned count, void* buffer);
int block_write(unsigned disk, uint64_t sector, unsigned count, const void* buffer);
int block_flush(unsigned disk);
/* Frees DMA only after reset acknowledgment; failure leaves buffers quarantined. */
int block_shutdown(void);
