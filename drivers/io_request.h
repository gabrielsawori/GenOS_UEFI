#pragma once
#include "device.h"
#include "../kernel/wait.h"

#define IO_WAITERS_MAX 32U
struct io_admission { uint64_t id, deadline; };

/* Protected by the owning device lock, held only for short transitions. Objects
 * are permanent; reinitializing a device must never clear its event sequence. */
struct io_request {
    struct wait_queue changed;
    uint64_t id, started;
    unsigned bytes, operation;
    uint32_t owner_pid; /* Blocking adapter only; async tokens remain caller-owned. */
    int pending, result;
    /* FIFO admission for blocking callers. No pointers into task stacks.
     * Dead callers' tickets expire at their original slot-wait deadline. */
    struct io_admission waiters[IO_WAITERS_MAX];
    unsigned waiter_count;
};
int io_request_begin(struct io_request* request, unsigned device, struct driver_token* token);
int io_request_begin_owned(struct io_request* request, unsigned device, struct driver_token* token,
                           uint32_t owner, uint64_t* admission, uint64_t deadline);
void io_request_cancel_waiter(struct io_request* request, uint64_t admission);
void io_request_expire_waiters(struct io_request* request);
void io_request_cancel_waiters(struct io_request* request);
void io_request_complete(struct io_request* request, int result);
/* Invalidate even a completed but uncollected result before releasing DMA. */
void io_request_abort(struct io_request* request, int error);
void io_request_release(struct io_request* request);
int io_request_matches(const struct io_request* request, struct driver_token token);
int io_can_block(void);
int io_service_start(void);
int io_service_ready(void);
#define IO_WATCHDOG_TICKS 50U
void io_service_notify(void); /* IRQ-safe event publication. */
