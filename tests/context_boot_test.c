/* Native CPL3 probes exercise the production ISR and scheduler. */
#include "../kernel/task.c"
#include "../drivers/io.h"

static const uint8_t* user_image;
static size_t user_size;
static uint32_t reports, workers, expected_mask, fresh_pid;
static uint32_t trap_pid, trap_vector, trap_count, group_trap;
static uint64_t start_tick, baseline;

static void __attribute__((noreturn)) fail(const char* message) {
    serial_write_string("[CONTEXT TEST FAIL] ");
    serial_write_string(message); serial_write_string("\n");
    outl(0xf4, 1);
    for (;;) asm volatile("cli; hlt");
}
#define CHECK(c, m) do { if (!(c)) fail(m); } while (0)

static void worker(void) {
    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
    CHECK((cr0 & 0x2e) == 0x22 && (cr4 & 0x40600) == 0x600,
          "per-CPU FP enable/AVX restriction");
    struct fpu_state pattern, got;
    fpu_state_init(&pattern);
    uint32_t cpu = smp_get_current_cpu_id();
    pattern.mxcsr |= (cpu & 3) << 13;
    for (unsigned i = 128; i < 384; i++) pattern.registers[i] = cpu * 23 + i;
    fpu_restore(&pattern);
    for (unsigned turn = 0; turn < 64; turn++) {
        asm volatile("hlt");
        fpu_save(&got);
        CHECK(got.mxcsr == pattern.mxcsr, "kernel worker MXCSR changed");
        for (unsigned i = 128; i < 384; i++)
            CHECK(got.registers[i] == pattern.registers[i], "kernel worker XMM changed");
    }
    __atomic_fetch_or(&workers, 1U << cpu, __ATOMIC_RELEASE);
}

static void checker(void) {
    for (;;) {
        int finished = 0;
        {
            PREEMPT_GUARD;
            if (reports == 15 && workers == expected_mask) {
                finished = 1;
                for (struct task* t = task_list_head; t; t = t->next)
                    if (t->pid && t != current_task && t != reaper_task) finished = 0;
            }
        }
        if (finished) break;
        CHECK(timer_get_ticks() - start_tick < 30000, "runtime or reaper timed out");
        asm volatile("hlt");
    }
    CHECK(trap_count == 12 && group_trap, "missing native user trap");
    CHECK(resource_stats.kernel_stack_pages == 8, "faulted task stack leak");
    CHECK(pmm_get_free_ram() == baseline, "faulted address space/frame leak");
    serial_write_string("[CONTEXT TEST PASS] Ring3 traps, group termination and reclamation\n");
    serial_write_string("[CONTEXT TEST PASS] per-CPU kernel and user FP isolation\n");
    outl(0xf4, 0);
    for (;;) asm volatile("hlt");
}

struct task* __real_create_user_task(const uint8_t*, size_t);
struct task* __wrap_create_user_task(const uint8_t* data, size_t size) {
    PREEMPT_GUARD;
    user_image = data; user_size = size;
    /* Heap grows at high water. Warm enough metadata space before PMM count. */
    void* warm = kmalloc(1024 * 1024);
    CHECK(warm, "heap warmup"); kfree(warm);
    CHECK(create_task(checker), "checker allocation");
    baseline = pmm_get_free_ram();
    for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
        CHECK(create_task_on_cpu(worker, i), "FP worker allocation");
        expected_mask |= 1U << i;
    }
    start_tick = timer_get_ticks();
    return __real_create_user_task(data, size);
}

void __real_isr_handler(struct registers*);
void __wrap_isr_handler(struct registers* regs) {
    if (regs->vector < 32 && (regs->cs & 3) == 3) {
        /* CoW faults remain recoverable and must reach production resolution. */
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        PREEMPT_GUARD;
        uint64_t* pte = regs->vector == 14 ? vmm_pte_in(current_task->pml4, cr2) : NULL;
        if (!(pte && (*pte & PTE_COW) && (regs->err_code & 2))) {
            /* TCG 10.2 reports #GP for the noncanonical PUSH probe; real x86
             * may report #SS. Both must terminate the process with no retry. */
            int stack_gp = trap_vector == 12 && regs->vector == 13 &&
                           regs->rsp == 0x800000001000ULL && !regs->err_code;
            if (current_task->pid != trap_pid || (regs->vector != trap_vector && !stack_gp)) {
                char number[24];
                serial_write_string("[CONTEXT VECTOR] actual=");
                itoa(regs->vector, number, 10); serial_write_string(number);
                serial_write_string(" expected=");
                itoa(trap_vector, number, 10); serial_write_string(number);
                serial_write_string("\n");
            }
            CHECK(current_task->pid == trap_pid && (regs->vector == trap_vector || stack_gp),
                  "unexpected user vector/PID");
            trap_pid = 0;
            trap_count++;
        }
    }
    __real_isr_handler(regs); /* User termination runs without a fixture guard. */
}

uint64_t __real_syscall_handler(uint64_t, uint64_t, uint64_t, uint64_t, const struct registers*);
uint64_t __wrap_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                                const struct registers* frame) {
    if (n == 66) {
        CHECK(trap_vector == 19 && trap_pid == current_task->pid, "invalid XM injection");
        serial_write_string("[CONTEXT TEST NOTE] TCG #XM delivery unavailable; injecting dispatcher frame\n");
        struct registers injected = *frame;
        injected.vector = 19;
        injected.err_code = 0;
        __wrap_isr_handler(&injected);
        fail("XM injection returned");
    }
    if (n >= 60 && n <= 65) {
        PREEMPT_GUARD;
        if (n == 61) {
            char line[24]; itoa(a, line, 10); serial_write_string(line);
            fail("userspace assertion at preceding line");
        }
        if (n == 60) {
            CHECK(a == 1 || a == 2 || a == 4 || a == 8, "invalid report");
            if (a == 1) {
                CHECK(b == expected_mask, "missing CPU in Ring3 FP test");
                serial_write_string("[CONTEXT TEST PASS] x87/XMM/MXCSR across preemption and sleep\n");
            }
            if (a == 2) {
                CHECK(current_task->pid == fresh_pid, "wrong fresh task");
                serial_write_string("[CONTEXT TEST PASS] live fork/clone state and clean exec\n");
            }
            reports |= a;
            return 0;
        }
        if (n == 62) {
            CHECK(!trap_pid && a < 32, "overlapping trap registration");
            trap_pid = current_task->pid; trap_vector = a;
            if (b) {
                unsigned count = 0;
                for (struct task* t = task_list_head; t; t = t->next)
                    if (t->pml4 == current_task->pml4 && t->state != TASK_DEAD) count++;
                CHECK(current_task->is_thread && count == 4, "incomplete fault group");
                group_trap = 1;
            }
            return 0;
        }
        if (n == 63) {
            struct task* fresh = __real_create_user_task(user_image, user_size);
            CHECK(fresh, "fresh ELF allocation");
            fresh_pid = fresh->pid;
            return fresh_pid;
        }
        if (n == 64) return trap_count;
        if (n == 65) return current_task->pid == fresh_pid;
    }
    return __real_syscall_handler(n, a, b, c, frame);
}
