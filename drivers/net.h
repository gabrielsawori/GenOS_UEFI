#pragma once
#include <stddef.h>
#include <stdint.h>
#include "device.h"
#include "virtio_irq.h"

#define NET_FRAME_MAX 1514 /* Ethernet header + MTU 1500, without FCS/VLAN. */
struct net_info {
    uint64_t transmitted, received, dropped, errors;
    uint8_t mac[6], online, link_up;
    struct virtio_irq_info irq;
};
void net_init(void);
/* Task context only. RX/TX share one device lock per NIC; init/shutdown also
 * uses the global mutation guard. Caller buffers must be kernel-accessible.
 * Async TX yields to the blocking callers' admission FIFO (BUSY). */
int net_submit(unsigned interface, const void* frame, size_t length, struct driver_token* token);
int net_collect(struct driver_token token);
int net_cancel(struct driver_token token);
void net_service(void);
unsigned net_count(void);
int net_get_info(unsigned interface, struct net_info* out);
/* Synchronous TX, kernel-owned bounce buffer. Pads frames shorter than 60 bytes. */
int net_send(unsigned interface, const void* frame, size_t length);
/* Nonblocking RX: positive length, zero if empty, negative driver_result.
 * A malformed frame or a too-small destination is consumed and counted as dropped. */
int net_receive(unsigned interface, void* frame, size_t capacity);
/* Blocking RX using an absolute PIT deadline. Reset cancels existing waiters. */
int net_receive_wait(unsigned interface, void* frame, size_t capacity, uint64_t deadline);
int net_shutdown(void);
