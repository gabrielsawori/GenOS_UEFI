/* Instrumented kernel only: exercise real page faults and production syscalls. */
#define task_init production_task_init
#include "../kernel/task.c"
#undef task_init
#include "../cpu/uaccess.h"

static int allocation_fails;
static unsigned recovered, reports;
static uint64_t runtime_reaped;

static void finish(unsigned code) {
    asm volatile("outl %0, %1" : : "a"(code), "Nd"((uint16_t)0xf4));
    for (;;) asm volatile("hlt");
}
static void fail(const char* reason) {
    serial_write_string("[UACCESS TEST FAIL] ");
    serial_write_string(reason);
    serial_write_string("\n");
    finish(1);
}
#define CHECK(test, why) do { if (!(test)) fail(why); } while (0)
void* __real_pmm_alloc_page(void);
void* __wrap_pmm_alloc_page(void) {
    return allocation_fails ? NULL : __real_pmm_alloc_page();
}
extern int uaccess_copy_from_asm(void*, const void*, size_t);
extern int uaccess_copy_to_asm(void*, const void*, size_t);
extern const struct { uint64_t instruction, recovery, write; } uaccess_fixups_start[];
static void unused(void) { fail("fixture task scheduled"); }

void task_init(void) {
    production_task_init();
    uint64_t baseline = pmm_get_free_ram();
    struct task* idle = current_task;
    struct task* parent = create_task(unused);
    CHECK(parent, "parent allocation");
    parent->state = TASK_WAITING;
    parent->pml4 = vmm_create_address_space();
    CHECK(parent->pml4, "PML4 allocation");
    uint64_t offset = hhdm_request.response->offset;
    for (unsigned i = 0; i < 2; i++) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        CHECK(phys, "private allocation");
        parent->user_pages[i] = phys;
        parent->user_vaddrs[i] = 0x40000000 + i * PAGE_SIZE;
        parent->user_page_count++;
        memset((void*)(phys + offset), 'x', PAGE_SIZE);
        CHECK(vmm_map_page_in(parent->pml4, parent->user_vaddrs[i], phys,
              i ? VMM_FLAGS_USER_RO | PTE_NX : VMM_FLAGS_USER_DATA), "private map");
    }
    current_task = parent;
    vmm_switch_pml4(parent->pml4);
    char output[16] = {0}, input[] = "hello";
    void* rw = (void*)0x40000000;
    void* ro = (void*)0x40001000;
    CHECK(!copy_to_user(rw, input, sizeof(input)) &&
          !copy_from_user(output, rw, sizeof(input)) && output[0] == 'h', "valid copies");
    CHECK(copy_from_user(output, NULL, 1) == -1 &&
          copy_from_user(output, (void*)0x60000000, 1) == -1 &&
          copy_to_user(ro, input, 1) == -1 &&
          copy_from_user(output, (void*)0xffff800000000000ULL, 1) == -1 &&
          copy_from_user(output, (void*)0x800000000000ULL, 1) == -1 &&
          copy_from_user(output, rw, SIZE_MAX) == -1, "invalid ranges");
    CHECK(!copy_to_user(NULL, NULL, 0) && !copy_from_user(NULL, NULL, 0), "zero length");
    CHECK(copy_to_user((void*)0x40000fff, input, 2) == -1 &&
          *(char*)(parent->user_pages[0] + offset + 4095) == 'x', "cross-page RO preflight");
    output[0] = 'z';
    CHECK(copy_from_user(output, (void*)0x40001fff, 2) == -1 && output[0] == 'z',
          "cross-page missing preflight");
    CHECK(copy_user_string(output, (char*)0x40001fff, sizeof(output)) == -1,
          "unterminated cross-page string");
    *(char*)(parent->user_pages[1] + offset + 4095) = 0;
    CHECK(copy_user_string(output, (char*)0x40001fff, sizeof(output)) == 0,
          "string read past NUL");
    CHECK(copy_user_string(output, (char*)rw, 3) == -1, "string truncated silently");
    uint64_t* table = parent->pml4;
    for (int level = 3; level >= 0; level--) {
        uint64_t* slot = &table[(0x40000000ULL >> (12 + 9 * level)) & 511];
        uint64_t saved = *slot;
        *slot &= ~PTE_USER;
        CHECK(!user_access_ok(rw, 1, 0), "supervisor ancestor/leaf accepted");
        *slot = saved & ~PTE_WRITE;
        CHECK(!user_access_ok(rw, 1, 1), "read-only ancestor/leaf accepted");
        *slot = saved;
        table = (uint64_t*)(vmm_pte_address(saved) + offset);
    }
    serial_write_string("[UACCESS TEST PASS] ranges, permissions and strings\n");
    /* Bypass the preflight only in this fixture to exercise CPU recovery. */
    {
        PREEMPT_GUARD;
        CHECK(uaccess_copy_from_asm(output, (void*)0x60000000, 1) == -1 &&
              uaccess_copy_to_asm(ro, input, 1) == -1, "annotated copy recovery");
        struct registers fake = {.vector = 14, .cs = 8, .rip = (uint64_t)unused,
                                 .rsi = 0x60000000};
        CHECK(!uaccess_fixup(&fake, 0x60000000), "unrelated fault concealed");
        fake.rip = uaccess_fixups_start[0].instruction;
        fake.cs = 0x23;
        CHECK(!uaccess_fixup(&fake, 0x60000000), "user fault concealed");
        fake.cs = 8;
        fake.err_code = 8;
        CHECK(!uaccess_fixup(&fake, 0x60000000), "reserved-bit fault concealed");
        fake.err_code = 2;
        CHECK(!uaccess_fixup(&fake, 0x60000000), "wrong copy direction recovered");
        fake.err_code = 0;
        CHECK(!uaccess_fixup(&fake, 0x60001000), "wrong operand recovered");
        fake.rsi = 0xffff800000000000ULL;
        CHECK(!uaccess_fixup(&fake, fake.rsi), "kernel operand recovered");
    }
    serial_write_string("[UACCESS TEST PASS] annotated copy recovery\n");

    const struct registers frame = {.cs = 0x23, .ss = 0x1b, .rip = 0x40000000,
                                    .rsp = 0x40000ff0, .rflags = 0x202};
    struct task* child = task_find_by_pid(task_fork(&frame));
    CHECK(child, "CoW fork");
    child->state = TASK_WAITING;
    uint64_t before = pmm_get_free_ram(), phys = parent->user_pages[0];
    uint64_t pte = *vmm_pte_in(parent->pml4, (uint64_t)rw);
    allocation_fails = 1;
    for (unsigned i = 0; i < 1000; i++) {
        CHECK(copy_to_user(rw, input, sizeof(input)) == -1 &&
              !preempt_depth(), "CoW OOM recovery/guard");
    }
    allocation_fails = 0;
    CHECK(pmm_get_free_ram() == before && parent->user_pages[0] == phys &&
          pmm_get_refcount((void*)phys) == 2 &&
          *vmm_pte_in(parent->pml4, (uint64_t)rw) == pte, "CoW OOM changed ownership");
    input[0] = 'H';
    CHECK(!copy_to_user(rw, input, sizeof(input)) && parent->user_pages[0] != phys &&
          *(char*)(phys + offset) == 'h', "CoW retry isolation");
    current_task = idle;
    vmm_switch_pml4(NULL);
    parent->state = child->state = TASK_DEAD;
    task_reap_dead();
    CHECK(pmm_get_free_ram() == baseline && recovered >= 1002, "copy lifecycle leak");
    serial_write_string("[UACCESS TEST PASS] 1000 CoW OOM retries\n");
}

struct task* __real_create_user_task(const uint8_t*, size_t);
struct task* __wrap_create_user_task(const uint8_t* binary, size_t size) {
    runtime_reaped = resource_stats.tasks_reaped;
    return __real_create_user_task(binary, size);
}
uint64_t __real_syscall_handler(uint64_t, uint64_t, uint64_t, uint64_t, const struct registers*);
uint64_t __wrap_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                              const struct registers* frame) {
    if (n == 0x7fff) {
        if (!a) {
            char number[24];
            itoa(b, number, 10);
            fail(number);
        }
        CHECK((a == 1 || a == 2) && !(reports & (1u << a)), "duplicate user report");
        reports |= 1u << a;
        return 0;
    }
    if (n == 0x7ffd) {
        allocation_fails = 1;
        uint64_t result = __real_syscall_handler(31, a, 0, 0, frame);
        allocation_fails = 0;
        return result;
    }
    return __real_syscall_handler(n, a, b, c, frame);
}
void __real_isr_handler(struct registers*);
void __wrap_isr_handler(struct registers* regs) {
    uint64_t vector = regs->vector;
    if (vector == 14 && !(regs->cs & 3)) recovered++;
    __real_isr_handler(regs);
    if (vector != 32 || timer_get_ticks() < 1000) return;
    CHECK(reports == 6 && !preempt_depth(), "runtime probes/guard incomplete");
    CHECK(resource_stats.tasks_reaped == runtime_reaped + 2 &&
          resource_stats.kernel_stack_pages == 4, "runtime task cleanup");
    serial_write_string("[UACCESS TEST PASS] Ring3 syscall buffers and nested pointers\n");
    finish(0);
}
