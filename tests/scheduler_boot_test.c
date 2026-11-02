/* Test-only replacement for task.o: exercise private lifecycle helpers without
 * adding production test hooks. All allocations and page tables are real.
 */
#define task_init production_task_init
#include "../kernel/task.c"
#undef task_init
#include "elf_fixture.h"
#include "../drivers/io.h"

_Static_assert(sizeof(struct registers) == 176, "assembly frame size");
_Static_assert(offsetof(struct registers, rax) == 112, "syscall result slot");
_Static_assert(offsetof(struct registers, rip) == 136, "iret frame offset");

static int fail_stack;
static int page_fail_after = -1;
static int mute_serial;
static unsigned workers_done;
static unsigned user_reports;
static uint64_t pit_irqs;
static uint64_t lapic_irqs[MAX_CPUS];
static uint64_t active_vector[MAX_CPUS];
static uint64_t runtime_free;
static uint64_t runtime_reaped;

static void qemu_exit(uint32_t status) {
    asm volatile("outl %0, %1" : : "a"(status), "Nd"((uint16_t)0xf4));
}

void __real_serial_write_string(const char* s);
void __wrap_serial_write_string(const char* s) {
    if (!mute_serial) __real_serial_write_string(s);
}

static void __attribute__((noreturn)) fail(const char* message) {
    asm volatile("cli");
    __real_serial_write_string("[SCHED TEST FAIL] ");
    __real_serial_write_string(message);
    __real_serial_write_string("\n");
    qemu_exit(1);
    for (;;) asm volatile("hlt");
}

#define CHECK(test, message) do { if (!(test)) fail(message); } while (0)

void* __real_pmm_alloc_contiguous_pages(uint64_t count);
void* __wrap_pmm_alloc_contiguous_pages(uint64_t count) {
    if (fail_stack) return NULL;
    return __real_pmm_alloc_contiguous_pages(count);
}

void* __real_pmm_alloc_page(void);
void* __wrap_pmm_alloc_page(void) {
    if (page_fail_after == 0) return NULL;
    if (page_fail_after > 0) page_fail_after--;
    return __real_pmm_alloc_page();
}

static void worker(void) {
    workers_done++;
    struct task* self = current_task;
    uint64_t start = timer_get_ticks();
    preempt_disable();
    while (timer_get_ticks() - start < 8) asm volatile("hlt");
    CHECK(current_task == self, "timer switched a protected worker");
    preempt_enable();
    /* Returning must exit via task_kernel_entry, never jump to address zero. */
}

static void test_stack_cycles(void) {
    uint64_t free_before = pmm_get_free_ram();
    uint64_t stacks = resource_stats.kernel_stack_pages;
    for (unsigned i = 0; i < 1000; i++) {
        struct task* t = create_task(worker);
        CHECK(t && t->kernel_stack_pages == 4, "create owned kernel stack");
        CHECK(t->kernel_stack == t->kernel_stack_phys +
              hhdm_request.response->offset + 4 * PAGE_SIZE, "stack contiguous");
        CHECK((t->regs.rsp & 15) == 8, "kernel C entry stack alignment");
        t->state = TASK_DEAD;
        task_reap_dead();
        CHECK(resource_stats.kernel_stack_pages == stacks, "stack page leak");
        CHECK(pmm_get_free_ram() == free_before, "create/reap physical leak");
        CHECK(preempt_depth() == 0, "create/reap guard leak");
    }
    serial_write_string("[SCHED TEST PASS] 1000 stack lifecycles\n");
}

static void test_constructors(void) {
    struct task* idle = current_task;
    struct task* parent = create_task(worker);
    CHECK(parent, "parent allocation");
    parent->pml4 = vmm_create_address_space();
    CHECK(parent->pml4, "parent address space");
    current_task = parent;
    struct registers frame = {.cs = 0x23, .ss = 0x1b, .rflags = 0x202,
        .rip = 0x401234, .rsp = 0x80000fe0, .rax = 22, .r12 = 0x12345678};
    uint64_t free_before = pmm_get_free_ram();
    uint64_t stacks = resource_stats.kernel_stack_pages;
    uint64_t failures = resource_stats.stack_alloc_failures;
    uint32_t pid_before = next_pid;
    test_elf_t valid = test_elf_valid();
    fail_stack = 1;
    CHECK(!create_task(worker), "kernel constructor OOM");
    CHECK(!create_user_task((const uint8_t*)&valid, sizeof(valid)), "user constructor stack OOM");
    CHECK(task_fork(&frame) == -1, "fork stack OOM");
    CHECK(task_clone(0x400000, 0x80000000) == -1, "clone stack OOM");
    fail_stack = 0;
    CHECK(resource_stats.stack_alloc_failures == failures + 4, "OOM accounting");
    CHECK(next_pid == pid_before && task_list_tail == parent &&
          parent->thread_count == 1, "failed task was published");
    CHECK(resource_stats.kernel_stack_pages == stacks &&
          pmm_get_free_ram() == free_before, "constructor OOM leak");
    page_fail_after = 0;
    CHECK(task_fork(&frame) == -1, "fork PML4 OOM");
    CHECK(!create_user_task((const uint8_t*)&valid, sizeof(valid)), "user PML4 OOM");
    page_fail_after = -1;
    CHECK(!create_user_task((const uint8_t*)&frame, sizeof(frame)), "invalid ELF accepted");
    CHECK(resource_stats.kernel_stack_pages == stacks &&
          pmm_get_free_ram() == free_before, "post-stack failure leak");

    /* Reject images exceeding the ownership budget before allocation. */
    test_elf_t oversized = test_elf_valid();
    oversized.segments[0].memsz = (USER_MAX_PAGES + 1ULL) * PAGE_SIZE;
    CHECK(!create_user_task((const uint8_t*)&oversized, sizeof(oversized)), "ELF tracking overflow");
    oversized.segments[0].memsz = (USER_MAX_PAGES - USER_STACK_PAGES + 1ULL) * PAGE_SIZE;
    CHECK(!create_user_task((const uint8_t*)&oversized, sizeof(oversized)), "user stack tracking overflow");
    CHECK(resource_stats.kernel_stack_pages == stacks &&
          pmm_get_free_ram() == free_before, "tracking rejection leak");

    int32_t child_pid = task_fork(&frame);
    struct task* child = task_find_by_pid(child_pid);
    CHECK(child && child->regs.rip == frame.rip && child->regs.rsp == frame.rsp &&
          child->regs.r12 == frame.r12 && child->regs.rax == 0,
          "fork must use live user frame");
    CHECK(child->tgid == child->pid && !child->is_thread &&
          child->kernel_stack_pages == 4, "fork initialization");
    child->state = TASK_DEAD;
    task_reap_dead();

    int32_t tid = task_clone(0x400000, 0x80000000);
    struct task* thread = task_find_by_pid(tid);
    CHECK(thread && thread->pml4 == parent->pml4 &&
          thread->kernel_stack_phys != parent->kernel_stack_phys &&
          parent->thread_count == 2, "clone identity/stack");
    uint64_t pml4_phys = (uint64_t)parent->pml4 - hhdm_request.response->offset;
    uint32_t parent_pid = parent->pid;
    current_task = thread;
    parent->state = TASK_DEAD;
    task_reap_dead();
    CHECK(task_find_by_pid(parent_pid) == parent &&
          pmm_get_refcount((void*)pml4_phys) == 1 &&
          task_find_by_pid(tid) == thread, "freed active thread/owner PML4");
    current_task = idle;
    vmm_switch_pml4(NULL);
    task_reap_dead();
    task_reap_dead();
    CHECK(!task_find_by_pid(parent_pid) && !task_find_by_pid(tid) &&
          pmm_get_refcount((void*)pml4_phys) == 0, "thread group reclamation");
    CHECK(preempt_depth() == 0, "constructor guard leak");
    serial_write_string("[SCHED TEST PASS] OOM and thread lifetime\n");
}

static void test_quantum(void) {
    timer_init(0);
    timer_init(1);
    timer_init(1193181);
    struct task* idle = current_task;
    struct task* a = create_task(worker);
    struct task* b = create_task(worker);
    CHECK(a && b, "quantum fixture allocation");
    a->state = TASK_RUNNING;
    current_task = a;
    struct registers frame = a->regs;
    preempt_disable();
    schedule(&frame);
    CHECK(current_task == a && a->quantum == QUANTUM_NORMAL,
          "schedule inside protected mutation");
    preempt_enable();
    schedule(&frame);
    CHECK(current_task == a && a->quantum == 1 && a->cpu_ticks == 1,
          "first quantum tick");
    schedule(&frame);
    CHECK(current_task == b && a->cpu_ticks == 2, "quantum/accounting off by one");
    current_task = idle;
    idle->state = TASK_RUNNING;
    percpu_set_kernel_stack(0, idle->kernel_stack);
    set_kernel_stack(idle->kernel_stack);
    a->state = b->state = TASK_DEAD;
    task_context_enter(); /* Synthetic frame switch did not execute IRET. */
    task_reap_dead();
    serial_write_string("[SCHED TEST PASS] quantum and preemption guard\n");
}

void task_init(void) {
    production_task_init();
    test_stack_cycles();
    test_constructors();
    test_quantum();
    for (unsigned i = 0; i < 2; i++) {
        struct task* t = create_task(worker);
        CHECK(t, "runtime worker allocation");
        task_init_priority(t, PRIO_HIGH);
    }
    runtime_reaped = resource_stats.tasks_reaped;
}

void __real_shm_init(void);
void __wrap_shm_init(void) {
    __real_shm_init();
    uint64_t free_before = pmm_get_free_ram();
    mute_serial = 1;
    for (unsigned i = 0; i < 1000; i++) {
        int id = shm_create(0, 4 * PAGE_SIZE);
        CHECK(id >= 0 && shm_destroy(id) == 0, "SHM create/destroy");
        CHECK(shm_destroy(id) == -1, "SHM double destroy");
        CHECK(pmm_get_free_ram() == free_before, "SHM backing leak");
    }
    for (int i = 0; i < 4; i++) {
        page_fail_after = i;
        CHECK(shm_create(0, 4 * PAGE_SIZE) == -1, "SHM partial OOM");
        CHECK(pmm_get_free_ram() == free_before, "SHM OOM rollback leak");
    }
    page_fail_after = -1;
    mute_serial = 0;
    CHECK(preempt_depth() == 0, "SHM guard leak");
    serial_write_string("[SCHED TEST PASS] 1000 SHM cycles and partial OOM\n");
}

struct task* __real_create_user_task(const uint8_t* data, size_t size);
struct task* __wrap_create_user_task(const uint8_t* data, size_t size) {
    /* Before boot's user program allocation; two workers still own 8 pages. */
    runtime_free = pmm_get_free_ram() + 8 * PAGE_SIZE;
    return __real_create_user_task(data, size);
}

uint64_t __real_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                              const struct registers* frame);
uint64_t __wrap_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                              const struct registers* frame) {
    CHECK(((uint64_t)frame & 15) == 0, "syscall frame alignment");
    CHECK(frame->cs == 0x23 && frame->ss == 0x1b &&
          frame->rip < 0x800000000000ULL, "syscall user frame");
    if (n == 0x7fff) {
        CHECK((a == 1 || a == 2) && !(user_reports & (1u << a)),
              "user register preservation or fork return");
        __atomic_fetch_or(&user_reports, 1u << a, __ATOMIC_SEQ_CST);
        return 0;
    }
    return __real_syscall_handler(n, a, b, c, frame);
}

void __real_timer_tick(void);
void __wrap_timer_tick(void) {
    uint32_t cpu = smp_get_current_cpu_id();
    CHECK(cpu == 0 && active_vector[cpu] == 32, "time advanced outside BSP PIT");
    __real_timer_tick();
    CHECK(timer_get_ticks() == pit_irqs, "global time differs from PIT IRQs");
}

void __real_isr_handler(struct registers* regs);
void __wrap_isr_handler(struct registers* regs) {
    uint32_t cpu = smp_get_current_cpu_id();
    uint64_t vector = regs->vector;
    active_vector[cpu] = vector;
    if (vector == 32) pit_irqs++;
    if (vector == 48) __atomic_add_fetch(&lapic_irqs[cpu], 1, __ATOMIC_RELAXED);
    __real_isr_handler(regs);
    if (vector != 32 || pit_irqs < 1000) return;
    CHECK(workers_done == 2 && user_reports == 6, "runtime tasks/syscalls missing");
    CHECK(resource_stats.tasks_reaped == runtime_reaped + 4 &&
          resource_stats.kernel_stack_pages == 4, "runtime reaper/stack leak");
    CHECK(pmm_get_free_ram() == runtime_free, "runtime physical leak");
    CHECK(preempt_depth() == 0, "runtime preemption leaked");
    CHECK(current_task == task_list_head && current_task->next == reaper_task &&
          !reaper_task->next, "runtime task list");
    for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
        CHECK(__atomic_load_n(&lapic_irqs[i], __ATOMIC_RELAXED) > 0,
              "LAPIC timer unobserved");
    }
    serial_write_string("[SCHED TEST PASS] PIT time and runtime reaper/syscalls\n");
    qemu_exit(0);
    for (;;) asm volatile("hlt");
}
