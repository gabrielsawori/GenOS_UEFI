/* Integer-only C keeps the register probes live across calls and interrupts. */
#include "../libc/syscall.h"
#include "../cpu/fpu.h"
#include "../include/genos/system.h"

static uint8_t stacks[32][8192] __attribute__((aligned(16)));
static unsigned started, go, done, seen, next_id;
static unsigned siblings;

static void __attribute__((noreturn)) fail(unsigned line) {
    syscall(61, line, 0, 0);
    for (;;) asm volatile("pause");
}
#define CHECK(c) do { if (!(c)) fail(__LINE__); } while (0)

void user_main(void), thread_main(void), bad_thread_main(void), sibling_main(void);
int context_math_test(void);
void thread_entry(void), bad_thread_entry(void), sibling_entry(void), bad_stack(void);
asm(".text\n.global _start\n_start:\ncall user_main\nud2\n"
    ".global thread_entry\nthread_entry:\ncall thread_main\nud2\n"
    ".global bad_thread_entry\nbad_thread_entry:\ncall bad_thread_main\nud2\n"
    ".global sibling_entry\nsibling_entry:\ncall sibling_main\nud2\n"
    ".global bad_stack\nbad_stack:\nmovabs $0x800000001000, %rsp\npush %rax\nud2\n");

static void save(struct fpu_state* state) {
    asm volatile("fxsave64 %0" : "=m"(*state) :: "memory");
}
static void restore(const struct fpu_state* state) {
    asm volatile("fxrstor64 %0" :: "m"(*state) : "memory");
}
static void pattern(struct fpu_state* state, unsigned seed) {
    uint8_t* bytes = (void*)state;
    for (unsigned i = 0; i < sizeof(*state); i++) bytes[i] = 0;
    state->control = 0x037f | ((seed & 3) << 10);
    state->mxcsr = 0x1f80 | ((seed & 3) << 13);
    state->tag = 0xff;
    for (unsigned r = 0; r < 8; r++) {
        uint64_t value = 0x8000000000000000ULL | ((uint64_t)seed << 32) | r;
        for (unsigned b = 0; b < 8; b++) state->registers[r * 16 + b] = value >> (b * 8);
        state->registers[r * 16 + 8] = 0xff;
        state->registers[r * 16 + 9] = 0x3f;
    }
    for (unsigned i = 128; i < 384; i++) state->registers[i] = seed * 19 + i * 7;
}
static void verify(const struct fpu_state* wanted) {
    struct fpu_state got;
    save(&got);
    CHECK(got.control == wanted->control && got.status == wanted->status &&
          got.tag == wanted->tag && got.mxcsr == wanted->mxcsr);
    for (unsigned r = 0; r < 8; r++)
        for (unsigned b = 0; b < 10; b++)
            CHECK(got.registers[r * 16 + b] == wanted->registers[r * 16 + b]);
    for (unsigned i = 128; i < 384; i++) CHECK(got.registers[i] == wanted->registers[i]);
}
static void clean_start(void) {
    static const struct fpu_state initial = { .control = 0x037f, .mxcsr = 0x1f80 };
    verify(&initial);
}

void thread_main(void) {
    struct fpu_state state;
    pattern(&state, 123);
    verify(&state); /* clone must use the live creator state */
    unsigned id = __atomic_add_fetch(&next_id, 1, __ATOMIC_SEQ_CST);
    unsigned cpu = syscall(50, 0, 0, 0);
    CHECK(cpu < 16);
    __atomic_fetch_or(&seen, 1U << cpu, __ATOMIC_SEQ_CST);
    pattern(&state, id);
    restore(&state);
    __atomic_add_fetch(&started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) asm volatile("pause");
    for (unsigned turn = 0; turn < 64; turn++) {
        uint64_t tick = syscall(33, 0, 0, 0);
        do {
            verify(&state);
            asm volatile("pause");
        } while (syscall(33, 0, 0, 0) - tick < 3);
        syscall(4, 1, 0, 0);
        verify(&state);
    }
    __atomic_add_fetch(&done, 1, __ATOMIC_RELEASE);
    syscall(2, 0, 0, 0);
    fail(__LINE__);
}

void sibling_main(void) {
    __atomic_add_fetch(&siblings, 1, __ATOMIC_RELEASE);
    for (;;) { syscall(4, 1, 0, 0); }
}
void bad_thread_main(void) {
    while (__atomic_load_n(&siblings, __ATOMIC_ACQUIRE) != 2) asm volatile("pause");
    syscall(62, 6, 1, 0); /* group contains a leader and three threads */
    asm volatile("ud2");
    fail(__LINE__);
}

static void __attribute__((noreturn)) trigger(unsigned n) {
    switch (n) {
    case 0: asm volatile("xor %%edx, %%edx; mov $1, %%eax; xor %%ecx, %%ecx; div %%rcx"
                         ::: "rax", "rcx", "rdx"); break;
    case 1: asm volatile("ud2"); break;
    case 2: asm volatile("cli"); break;
    case 3: asm volatile("movabs $0x12345000, %%rax; mov (%%rax), %%rax" ::: "rax"); break;
    case 4: asm volatile("int3"); break;
    case 5: asm volatile("pushfq; orq $0x100, (%%rsp); popfq; nop" ::: "memory"); break;
    case 6: {
        uint16_t control = 0x037b; /* unmask x87 divide by zero */
        asm volatile("fninit; fldcw %0; fldz; fld1; fdiv %%st(1), %%st; fwait"
                     :: "m"(control) : "memory");
        break;
    }
    case 7: {
        uint32_t mxcsr = 0x1d80; /* unmask SSE divide by zero */
        asm volatile("ldmxcsr %0; mov $0x3f800000, %%eax; movd %%eax, %%xmm0;"
                     "pxor %%xmm1, %%xmm1; divss %%xmm1, %%xmm0"
                     :: "m"(mxcsr) : "rax", "memory");
        /* QEMU 10.2 TCG records ZE but does not deliver #XM. If hardware did
         * not trap, verify that exact condition before injecting the saved
         * user frame into the production dispatcher through a test hook. */
        asm volatile("stmxcsr %0" : "=m"(mxcsr));
        CHECK((mxcsr & 0x204) == 4);
        syscall(66, 0, 0, 0);
        break;
    }
    case 8: bad_stack(); break;
    case 9: asm volatile("int $0x20"); break; /* IRQ gates stay privileged */
    case 10: {
        uint8_t* page = (void*)syscall(54, 4096, 0, 0);
        CHECK(page); page[0] = 0xc3;
        ((void (*)(void))page)(); /* NX user allocation */
        break;
    }
    }
    fail(__LINE__);
}

void user_main(void) {
    clean_start();
    if (syscall(65, 0, 0, 0)) {
        syscall(60, 2, 0, 0);
        syscall(2, 0, 0, 0);
        fail(__LINE__);
    }
    struct genos_system_info info;
    do { CHECK(!syscall(52, (uint64_t)&info, 0, 0)); }
    while (info.online_cpus != info.detected_cpus);
    CHECK(info.online_cpus && info.online_cpus <= 16);
    CHECK(context_math_test());
    struct fpu_state parent;
    pattern(&parent, 123);
    restore(&parent);
    int child = syscall(22, 0, 0, 0);
    CHECK(child >= 0);
    verify(&parent);
    if (!child) {
        struct fpu_state changed;
        pattern(&changed, 222); restore(&changed);
        syscall(4, 2, 0, 0); verify(&changed);
        syscall(60, 4, 0, 0);
        syscall(2, 0, 0, 0); fail(__LINE__);
    }
    syscall(11, child, 0, 0);
    verify(&parent);
    int tids[32];
    for (unsigned i = 0; i < info.online_cpus * 2; i++) {
        tids[i] = syscall(28, (uint64_t)thread_entry, (uint64_t)&stacks[i][8192], 0);
        CHECK(tids[i] > 0);
    }
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) != info.online_cpus * 2)
        asm volatile("pause");
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < info.online_cpus * 2; i++) syscall(11, tids[i], 0, 0);
    CHECK(done == info.online_cpus * 2 && seen == (1U << info.online_cpus) - 1);
    verify(&parent);
    syscall(60, 1, seen, 0);
    /* A separately loaded ELF must start clean even after these workers. */
    int fresh = syscall(63, 0, 0, 0);
    CHECK(fresh > 0); syscall(11, fresh, 0, 0); verify(&parent);

    const unsigned vectors[] = {0, 6, 13, 14, 3, 1, 16, 19, 12, 13, 14};
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(*vectors); i++) {
        child = syscall(22, 0, 0, 0); CHECK(child >= 0);
        if (!child) { syscall(62, vectors[i], 0, 0); trigger(i); }
        syscall(11, child, 0, 0);
        verify(&parent);
        CHECK(syscall(64, 0, 0, 0) == i + 1);
    }
    child = syscall(22, 0, 0, 0); CHECK(child >= 0);
    if (!child) {
        for (unsigned i = 0; i < 2; i++)
            CHECK((int)syscall(28, (uint64_t)sibling_entry, (uint64_t)&stacks[i][8192], 0) > 0);
        CHECK((int)syscall(28, (uint64_t)bad_thread_entry, (uint64_t)&stacks[2][8192], 0) > 0);
        for (;;) syscall(4, 1, 0, 0);
    }
    syscall(11, child, 0, 0);
    CHECK(syscall(64, 0, 0, 0) == 12); verify(&parent);
    syscall(60, 8, 0, 0);
    syscall(2, 0, 0, 0); fail(__LINE__);
}
