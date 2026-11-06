#include "io_request.h"
#include "block.h"
#include "net.h"
#include "virtio.h"
#include "timer.h"
#include "../kernel/task.h"
#include "../cpu/preempt.h"
#include "../cpu/percpu.h"

static uint64_t next_id;
static int service_ready;
static struct wait_queue service_event;
void io_service_notify(void) { wait_queue_signal(&service_event); }
static uint64_t allocate_id(void) {
    uint64_t previous = __atomic_load_n(&next_id, __ATOMIC_RELAXED);
    do {
        if (previous == UINT64_MAX) return 0;
    } while (!__atomic_compare_exchange_n(&next_id, &previous, previous + 1, 0,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return previous + 1;
}
void io_request_cancel_waiter(struct io_request* r, uint64_t admission) {
    if (!preempt_local_depth()) __builtin_trap();
    for (unsigned i = 0; i < r->waiter_count; i++) if (r->waiters[i].id == admission) {
        for (unsigned j = i + 1; j < r->waiter_count; j++) r->waiters[j - 1] = r->waiters[j];
        r->waiter_count--;
        wait_queue_signal(&r->changed);
        return;
    }
}
void io_request_expire_waiters(struct io_request* r) {
    if (!preempt_local_depth()) __builtin_trap();
    uint64_t now = timer_get_ticks();
    for (unsigned i = 0; i < r->waiter_count;) {
        if (now >= r->waiters[i].deadline) io_request_cancel_waiter(r, r->waiters[i].id);
        else i++;
    }
}
void io_request_cancel_waiters(struct io_request* r) {
    if (!preempt_local_depth()) __builtin_trap();
    r->waiter_count = 0;
    wait_queue_signal(&r->changed);
}
int io_request_begin_owned(struct io_request* r, unsigned device, struct driver_token* token,
                           uint32_t owner, uint64_t* admission, uint64_t deadline) {
    if (!preempt_local_depth()) __builtin_trap();
    if (!token || (!!owner != !!admission)) return DRIVER_INVALID;
    io_request_expire_waiters(r);
    if (admission && timer_get_ticks() >= deadline) {
        io_request_cancel_waiter(r, *admission);
        return DRIVER_TIMEOUT;
    }
    uint64_t reserved = admission ? *admission : 0;
    if (reserved) {
        unsigned i = 0;
        while (i < r->waiter_count && r->waiters[i].id != reserved) i++;
        if (i == r->waiter_count) return DRIVER_CANCELED;
    }
    if (r->id || (r->waiter_count && r->waiters[0].id != reserved)) {
        if (admission && !reserved && r->waiter_count < IO_WAITERS_MAX) {
            reserved = allocate_id();
            if (!reserved) return DRIVER_INVALID;
            *admission = reserved;
            r->waiters[r->waiter_count++] = (struct io_admission){reserved, deadline};
            io_service_notify(); /* Expire even a ticket whose task dies before waiting. */
        }
        return DRIVER_BUSY;
    }
    uint64_t id = reserved ? reserved : allocate_id();
    if (!id) return DRIVER_INVALID;
    if (reserved) { io_request_cancel_waiter(r, reserved); *admission = 0; }
    r->id = id;
    r->started = timer_get_ticks();
    r->owner_pid = owner;
    r->pending = 1; r->result = DRIVER_PENDING;
    *token = (struct driver_token){r->id, device};
    io_service_notify(); /* Also arms timeout service if the device never interrupts. */
    return DRIVER_OK;
}
int io_request_begin(struct io_request* r, unsigned device, struct driver_token* token) {
    return io_request_begin_owned(r, device, token, 0, NULL, 0);
}
void io_request_complete(struct io_request* r, int result) {
    if (!preempt_local_depth()) __builtin_trap();
    if (!r->pending) return;
    r->result = result; r->pending = 0;
    wait_queue_signal(&r->changed);
}
void io_request_abort(struct io_request* r, int error) {
    if (!preempt_local_depth() || error >= 0) __builtin_trap();
    if (!r->id) return;
    r->result = error; r->pending = 0;
    wait_queue_signal(&r->changed);
}
void io_request_release(struct io_request* r) {
    if (!preempt_local_depth()) __builtin_trap();
    r->id = 0; r->pending = 0; r->owner_pid = 0;
    wait_queue_signal(&r->changed);
}
int io_request_matches(const struct io_request* r, struct driver_token token) {
    return token.id && r->id == token.id;
}
int io_can_block(void) {
    if (!virtio_irqs_enabled() || preempt_depth() || preempt_local_depth()) return 0;
    PREEMPT_GUARD;
    return percpu_current()->current &&
           percpu_current()->current->pid && percpu_current()->current->state != TASK_DEAD;
}
static void service(void) {
    for (;;) {
        uint64_t observed = wait_queue_snapshot(&service_event);
        block_service();
        net_service();
        wait_queue_wait(&service_event, observed, timer_get_ticks() + IO_WATCHDOG_TICKS);
    }
}
int io_service_start(void) {
    PREEMPT_GUARD;
    if (io_service_ready()) return 1;
    if (!block_count() && !net_count()) return 1;
    if (!create_task_on_cpu(service, 0)) return 0;
    __atomic_store_n(&service_ready, 1, __ATOMIC_RELEASE);
    return 1;
}
int io_service_ready(void) { return __atomic_load_n(&service_ready, __ATOMIC_ACQUIRE); }
