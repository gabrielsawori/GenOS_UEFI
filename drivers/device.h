#pragma once
#include <stdint.h>

/* Kernel-only I/O APIs. Call from task context with interrupts enabled.
 * Buffers must be kernel-owned, valid for the call and not concurrently changed.
 * No userspace pointer or address-space pin is a DMA/page pin. */
enum driver_result {
    DRIVER_OK = 0,
    DRIVER_INVALID = -1,
    DRIVER_OFFLINE = -2,
    DRIVER_READ_ONLY = -3,
    DRIVER_IO = -4,
    DRIVER_TIMEOUT = -5,
    DRIVER_UNSUPPORTED = -6,
    DRIVER_BUFFER_SMALL = -7,
    DRIVER_BUSY = -8,
    DRIVER_CANCELED = -9,
    DRIVER_PENDING = 1,
};

/* Kernel capability for one request. IDs are never reused, even across reset.
 * The caller must collect or cancel accepted requests. No caller buffer or
 * task pointer is retained by an async submission. */
struct driver_token { uint64_t id; unsigned device; };
