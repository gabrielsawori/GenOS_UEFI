#include "../libc/stdlib.h"
#include "../libc/syscall.h"

static unsigned started, done, seen, go;
static int shared_id;
static void* stacks[16];
static unsigned expected;

static void __attribute__((noreturn)) fail(void) {
    syscall(61, 0, 0, 0);
    for (;;) asm volatile("pause");
}
#define CHECK(c) do { if (!(c)) fail(); } while (0)
void user_main(void);
void thread_main(void);
void thread_entry(void);
asm(".text\n.global _start\n_start:\ncall user_main\nud2\n"
    ".global thread_entry\nthread_entry:\ncall thread_main\nud2\n");

void thread_main(void) {
    unsigned cpu = get_cpu_id();
    CHECK(cpu < 16);
    __atomic_fetch_or(&seen, 1U << cpu, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&started, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) asm volatile("pause");
    for (unsigned i = 0; i < 64; i++) {
        size_t n = 17 + (i % 8) * 4096;
        unsigned char* p = malloc(n);
        CHECK(p && !((uint64_t)p & 15) && !p[0] && !p[n - 1]);
        p[0] = (unsigned char)(cpu + 1);
        p[n - 1] = (unsigned char)(i + 1);
        user_sleep(1);
        unsigned char* q = realloc(p, n + 4096);
        CHECK(q && q[0] == cpu + 1 && q[n - 1] == i + 1 && !q[n + 4095]);
        free(q);
        volatile unsigned* shared;
        while (!(shared = shm_attach(shared_id))) user_sleep(1);
        shared[cpu + 16] = i + 1;
        CHECK(shared[cpu + 16] == i + 1 && shm_detach((void*)shared) == 0);
        if ((i & 15) == 15) syscall(62, cpu, i + 1, 0);
    }
    __atomic_add_fetch(&done, 1, __ATOMIC_RELEASE);
    exit(0);
}

void user_main(void) {
    struct genos_system_info info;
    do { CHECK(!get_system_info(&info)); } while (info.online_cpus != info.detected_cpus);
    expected = info.online_cpus;
    CHECK(expected && expected <= 16);
    CHECK(!malloc(0) && !malloc(SIZE_MAX));
    free(NULL);
    CHECK((int64_t)syscall(55, 0xffff800000000000ULL, 0, 0) == -1 &&
          (int64_t)syscall(47, 4096, 0, 0) == -1 &&
          (int64_t)syscall(17, 0x100001000ULL, 0, 0) == -1);
    size_t length = 2 * 1024 * 1024 + 37;
    unsigned char* p = malloc(length);
    CHECK(p && !((uint64_t)p & 15));
    for (size_t i = 0; i < length; i += 4096) {
        CHECK(!p[i]);
        p[i] = (unsigned char)(i / 4096 + 1);
    }
    p[length - 1] = 73;
    shared_id = shm_create(2 * 1024 * 1024 + 17);
    volatile unsigned* shared = shm_attach(shared_id);
    volatile unsigned* alias = shm_attach(shared_id);
    CHECK(shared_id >= 0 && shared && alias && shared != alias);
    shared[0] = 23;
    CHECK(alias[0] == 23);
    int child = fork();
    CHECK(child >= 0);
    if (!child) {
        p[0] = 99;
        shared[0] = 42;
        unsigned char* grown = realloc(p, length + 8192);
        CHECK(grown && grown[0] == 99 && grown[length - 1] == 73);
        free(grown);
        CHECK(!shm_detach((void*)shared) && !shm_detach((void*)alias));
        exit(0);
    }
    CHECK(wait_pid(child) == 1 && shared[0] == 42 && p[0] == 1);
    for (size_t i = 0; i < length; i += 4096) CHECK(p[i] == (unsigned char)(i / 4096 + 1));
    CHECK(!realloc(p, SIZE_MAX) && p[length - 1] == 73);
    unsigned char* small = realloc(p, 17);
    CHECK(small && small[0] == 1);
    free(small);
    CHECK(!shm_detach((void*)alias));
    int tids[16];
    for (unsigned i = 0; i < expected; i++) {
        stacks[i] = malloc(8192);
        CHECK(stacks[i]);
        tids[i] = syscall(28, (uint64_t)thread_entry, (uint64_t)stacks[i] + 8192, 0);
        CHECK(tids[i] > 0);
    }
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) != expected) asm volatile("pause");
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < expected; i++) {
        CHECK(wait_pid(tids[i]) == 1);
        free(stacks[i]);
    }
    CHECK(__atomic_load_n(&done, __ATOMIC_ACQUIRE) == expected &&
          __atomic_load_n(&seen, __ATOMIC_ACQUIRE) == (1U << expected) - 1);
    for (unsigned i = 0; i < expected; i++) CHECK(shared[i + 16] == 64);
    CHECK(!shm_detach((void*)shared));
    syscall(60, 0, 0, 0);
    exit(0);
}
