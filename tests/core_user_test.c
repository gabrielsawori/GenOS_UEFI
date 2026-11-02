#include "../libc/syscall.h"
#include "../include/genos/system.h"
#include "../include/genos/input.h"

static unsigned done, started, seen, go;
static unsigned expected;
static uint8_t stacks[16][8192] __attribute__((aligned(16)));
static volatile unsigned private_data = 7;

static void fail(void) {
    syscall(61, 0, 0, 0);
    for (;;) asm volatile("pause");
}
void thread_entry(void);
void user_main(void);
void thread_main(void);
asm(".text\n.global _start\n_start:\ncall user_main\nud2\n"
    ".global thread_entry\nthread_entry:\ncall thread_main\nud2\n");

void thread_main(void) {
    unsigned cpu = syscall(50, 0, 0, 0);
    if (cpu >= 16) fail();
    __atomic_fetch_or(&seen, 1U << cpu, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&started, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) asm volatile("pause");
    for (unsigned i = 0; i < 200; i++) {
        if (syscall(50, 0, 0, 0) != cpu) fail();
        uint64_t tick = syscall(33, 0, 0, 0);
        syscall(4, 1, 0, 0);
        if (syscall(33, 0, 0, 0) <= tick) fail();
    }
    __atomic_add_fetch(&done, 1, __ATOMIC_SEQ_CST);
    syscall(2, 0, 0, 0);
    fail();
}

void user_main(void) {
    struct genos_system_info info;
    do {
        if (syscall(52, (uint64_t)&info, 0, 0)) fail();
    } while (info.online_cpus != info.detected_cpus);
    if (syscall(52, (uint64_t)&info, 0, 0) || info.version != 1 ||
        !info.online_cpus || info.online_cpus > 16) fail();
    expected = info.online_cpus;
    syscall(63, 0, 0, 0); /* Ask the host runner for actual emulated input. */
    unsigned keys = 0, buttons = 0;
    int dx = 0, dy = 0;
    uint64_t input_start = syscall(33, 0, 0, 0);
    while (keys != 3 || !buttons || dx != 12 || dy != -7) {
        struct genos_input_event event;
        int got = syscall(51, (uint64_t)&event, 0, 0);
        if (got < 0 || syscall(33, 0, 0, 0) - input_start > 10000) fail();
        if (got) {
            if (event.type == GENOS_INPUT_KEY && event.code == 0x1e)
                keys |= event.value ? 1 : 2;
            if (event.type == GENOS_INPUT_MOUSE) {
                buttons |= event.code & 1;
                dx += event.dx; dy += event.dy;
            }
        } else syscall(4, 1, 0, 0);
    }
    syscall(64, 0, 0, 0);
    int child = (int)syscall(22, 0, 0, 0);
    if (child < 0) fail();
    if (!child) {
        private_data = 11;
        if (private_data != 11) fail();
        syscall(60, 2, syscall(50, 0, 0, 0), 0);
        syscall(2, 0, 0, 0);
        fail();
    }
    syscall(11, child, 0, 0);
    if (private_data != 7) fail();
    int tids[16];
    for (unsigned i = 0; i < expected; i++) {
        tids[i] = syscall(28, (uint64_t)thread_entry, (uint64_t)&stacks[i][8192], 0);
        if (tids[i] < 0) fail();
    }
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) != expected) asm volatile("pause");
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < expected; i++) syscall(11, tids[i], 0, 0);
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != expected ||
        __atomic_load_n(&seen, __ATOMIC_ACQUIRE) != (1U << expected) - 1) fail();
    syscall(60, 1, seen, 0);
    syscall(2, 0, 0, 0);
    fail();
}
