/* Real PMM/page-table tests. Replace task.o only in the instrumented kernel. */
#define task_init production_task_init
#include "../kernel/task.c"
#undef task_init
#include "elf_fixture.h"

static int page_fail_after = -1, ref_fail_after = -1;
static int map_fail_after = -1;
static unsigned page_calls, map_calls, reports, protection_faults, user_cow, kernel_cow;
static int muted;
static uint64_t runtime_free, runtime_reaped;
static struct { uint32_t pid; uint64_t va; unsigned kind; } expected[2];

static void qemu_exit(uint32_t status) {
    asm volatile("outl %0, %1" : : "a"(status), "Nd"((uint16_t)0xf4));
}
void __real_serial_write_string(const char* text);
void __wrap_serial_write_string(const char* text) {
    if (!muted) __real_serial_write_string(text);
}
static void __attribute__((noreturn)) fail(const char* reason) {
    asm volatile("cli");
    __real_serial_write_string("[MEM TEST FAIL] ");
    __real_serial_write_string(reason);
    __real_serial_write_string("\n");
    qemu_exit(1);
    for (;;) asm volatile("hlt");
}
#define CHECK(expr, reason) do { if (!(expr)) fail(reason); } while (0)

void* __real_pmm_alloc_page(void);
void* __wrap_pmm_alloc_page(void) {
    page_calls++;
    if (page_fail_after == 0) return NULL;
    if (page_fail_after > 0) page_fail_after--;
    return __real_pmm_alloc_page();
}
int __real_pmm_ref_page(void* ptr);
int __wrap_pmm_ref_page(void* ptr) {
    if (ref_fail_after == 0) return 0;
    if (ref_fail_after > 0) ref_fail_after--;
    return __real_pmm_ref_page(ptr);
}
int __real_vmm_map_page_in(uint64_t*, uint64_t, uint64_t, uint64_t);
int __wrap_vmm_map_page_in(uint64_t* root, uint64_t va, uint64_t phys, uint64_t flags) {
    map_calls++;
    if (map_fail_after == 0) return 0;
    if (map_fail_after > 0) map_fail_after--;
    return __real_vmm_map_page_in(root, va, phys, flags);
}
int __real_vmm_map_pages_in(uint64_t*, uint64_t, const uint64_t*, uint32_t, uint64_t);
int __wrap_vmm_map_pages_in(uint64_t* root, uint64_t va, const uint64_t* frames,
                           uint32_t count, uint64_t flags) {
    if (map_fail_after >= 0 && (unsigned)map_fail_after < count) {
        uint64_t* entry = (uint64_t*)frames + map_fail_after;
        uint64_t saved = *entry;
        *entry |= 1;
        int result = __real_vmm_map_pages_in(root, va, frames, count, flags);
        *entry = saved;
        return result;
    }
    if (map_fail_after >= 0) map_fail_after -= count;
    return __real_vmm_map_pages_in(root, va, frames, count, flags);
}
static void unused_entry(void) { fail("fixture task ran"); }

static struct task* process(void) {
    struct task* t = create_task(unused_entry);
    CHECK(t, "process stack allocation");
    t->pml4 = vmm_create_address_space();
    CHECK(t->pml4, "process PML4 allocation");
    t->state = TASK_WAITING;
    return t;
}
static void private_page(struct task* t, uint64_t va, uint64_t flags) {
    uint64_t phys = (uint64_t)pmm_alloc_page();
    CHECK(phys && t->user_page_count < 256, "private page allocation");
    CHECK(vmm_map_page_in(t->pml4, va, phys, flags), "private page mapping");
    uint32_t i = t->user_page_count++;
    t->user_vaddrs[i] = va;
    t->user_pages[i] = phys;
    memset((void*)(phys + hhdm_request.response->offset), 0x11, PAGE_SIZE);
}
static uint64_t* contents(struct task* t, uint64_t va) {
    uint64_t phys = vmm_get_physical_in(t->pml4, va, NULL);
    CHECK(phys, "contents mapping absent");
    return (uint64_t*)(phys + hhdm_request.response->offset);
}
static const struct registers user_frame = {
    .cs = 0x23, .ss = 0x1b, .rflags = 0x202, .rip = 0x40002000, .rsp = 0x40000fe0
};

static void mapping_tests(void) {
    uint64_t baseline = pmm_get_free_ram();
    uint64_t* root = vmm_create_address_space();
    uint64_t phys = (uint64_t)pmm_alloc_page();
    CHECK(root && phys, "mapping fixture");
    uint64_t free_before = pmm_get_free_ram();
    for (int n = 0; n < 3; n++) {
        page_fail_after = n;
        CHECK(!vmm_map_page_in(root, 0x40000000, phys, VMM_FLAGS_USER_DATA),
              "mapping OOM accepted");
        CHECK(root[0] == 0 && pmm_get_free_ram() == free_before, "page table rollback");
    }
    page_fail_after = -1;
    CHECK(!vmm_map_page_in(root, 0x40000001, phys, VMM_FLAGS_USER_DATA) &&
          !vmm_map_page_in(root, 0x800000000000ULL, phys, VMM_FLAGS_USER_DATA) &&
          !vmm_map_page_in(root, 0x40000000, phys | PTE_NX, VMM_FLAGS_USER_DATA),
          "invalid mapping accepted");
    CHECK(vmm_map_page_in(root, 0x40000000, phys, VMM_FLAGS_USER_DATA), "mapping failed");
    uint64_t flags;
    CHECK(vmm_get_physical_in(root, 0x40000000, &flags) == phys &&
          flags == VMM_FLAGS_USER_DATA, "NX address/flags separation");
    CHECK(!vmm_map_page_in(root, 0x40000000, phys, VMM_FLAGS_USER_CODE), "overwrite accepted");
    uint64_t range_frames[2] = {phys, phys};
    CHECK(vmm_map_page_in(root, 0x40002000, phys, VMM_FLAGS_USER_DATA), "range collision fixture");
    CHECK(!vmm_map_pages_in(root, 0x40001000, range_frames, 2, VMM_FLAGS_USER_DATA) &&
          !vmm_pte_in(root, 0x40001000) &&
          vmm_get_physical_in(root, 0x40002000, NULL) == phys,
          "range rollback overwrote collision");
    CHECK(vmm_unmap_pages_in(root, 0x40001000, 2) == 1 &&
          vmm_get_physical_in(root, 0x40000000, NULL) == phys,
          "sparse range removal damaged neighbor");
    CHECK(!vmm_map_pages_in(root, 0x800000000000ULL, range_frames, 2, VMM_FLAGS_USER_DATA) &&
          !vmm_map_pages_in(root, UINT64_MAX - PAGE_SIZE + 1, range_frames, 2, VMM_FLAGS_KERNEL) &&
          !vmm_map_pages_in(root, 0x40001001, range_frames, 2, VMM_FLAGS_USER_DATA),
          "invalid range accepted");
    /* Alias one owned frame across 17 PTs to exercise bounded table retirement
     * without requiring tens of MiB of extra RAM in the low-memory cases. */
    uint64_t range_baseline = pmm_get_free_ram();
    uint64_t array_phys = (uint64_t)pmm_alloc_contiguous_pages(17);
    CHECK(array_phys, "large range vector");
    uint64_t* many = (void*)(array_phys + hhdm_request.response->offset);
    for (unsigned i = 0; i < 8193; i++) many[i] = phys;
    CHECK(vmm_map_pages_in(root, 0x200000000ULL, many, 8193, VMM_FLAGS_USER_DATA),
          "large range mapping");
    CHECK(vmm_unmap_pages_in(root, 0x200000000ULL, 8193) == 8193 &&
          !vmm_pte_in(root, 0x200000000ULL) && !vmm_pte_in(root, 0x202000000ULL) &&
          vmm_get_physical_in(root, 0x40000000, NULL) == phys,
          "large table retirement damaged mappings");
    for (unsigned i = 0; i < 17; i++) pmm_free_page((void*)(array_phys + i * PAGE_SIZE));
    CHECK(pmm_get_free_ram() == range_baseline, "large range table retirement leak");
    uint64_t offset = hhdm_request.response->offset;
    uint64_t* pdpt = (uint64_t*)(vmm_pte_address(root[0]) + offset);
    uint64_t* pd = (uint64_t*)(vmm_pte_address(pdpt[1]) + offset);
    uint64_t saved = pd[0];
    pd[0] = 0x200000 | PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_LARGE;
    CHECK(!vmm_pte_in(root, 0x40000000) && !vmm_unmap_page_in(root, 0x40000000) &&
          !vmm_map_page_in(root, 0x40000000, phys, VMM_FLAGS_USER_DATA), "huge leaf walked");
    pd[0] = saved;
    CHECK(vmm_unmap_page_in(root, 0x40000000) == phys && root[0] == 0,
          "unmap address or table pruning");
    pmm_free_page((void*)phys);
    vmm_destroy_address_space(root);
    CHECK(pmm_get_free_ram() == baseline, "mapping lifecycle leak");
    serial_write_string("[MEM TEST PASS] mappings and rollback\n");
}

static void elf_oom_tests(void) {
    test_elf_t elf = test_elf_valid();
    elf.header.phnum = 3;
    elf.segments[1] = (elf_phdr_t){.type = ELF_PT_LOAD, .flags = ELF_PF_R | ELF_PF_W,
        .offset = offsetof(test_elf_t, payload) + 16,
        .vaddr = 0x40002000 + offsetof(test_elf_t, payload) + 16,
        .filesz = 16, .memsz = PAGE_SIZE - offsetof(test_elf_t, payload), .align = PAGE_SIZE};
    elf.segments[2] = (elf_phdr_t){.type = ELF_PT_LOAD, .flags = ELF_PF_R | ELF_PF_W,
        .vaddr = 0x40005000, .memsz = PAGE_SIZE, .align = PAGE_SIZE};
    page_calls = map_calls = 0;
    struct task* t = create_user_task((const uint8_t*)&elf, sizeof(elf));
    CHECK(t, "ELF OOM baseline");
    unsigned calls = page_calls, mappings = map_calls;
    CHECK(t->user_page_count == 8 && mappings == 8, "ELF page budget or stack");
    uint64_t next_stack = t->stack_base;
    uint64_t flags;
    CHECK(vmm_get_physical_in(t->pml4, 0x40000000, &flags) && flags == VMM_FLAGS_USER_CODE,
          "ELF executable flags");
    const uint64_t addresses[] = {0x40000000, 0x40002000, 0x40003000, 0x40005000};
    for (unsigned page = 0; page < 4; page++) {
        uint64_t phys = vmm_get_physical_in(t->pml4, addresses[page], &flags);
        CHECK(phys && t->user_vaddrs[page] == addresses[page], "ELF VA tracking");
        if (page) CHECK(flags == VMM_FLAGS_USER_DATA, "ELF data NX/write flags");
        uint8_t* bytes = (uint8_t*)(phys + hhdm_request.response->offset);
        for (unsigned j = 0; j < PAGE_SIZE; j++) {
            unsigned start = offsetof(test_elf_t, payload) + (page == 1 ? 16 : 0);
            unsigned length = page == 0 ? 32 : page == 1 ? 16 : 0;
            uint8_t expected_byte = 0;
            if (j >= start && j < start + length)
                expected_byte = (uint8_t)(0x80 + j - offsetof(test_elf_t, payload));
            CHECK(bytes[j] == expected_byte, "ELF unaligned copy/BSS/padding");
        }
    }
    t->state = TASK_DEAD;
    task_reap_dead();
    uint64_t baseline = pmm_get_free_ram();
    uint64_t stacks = resource_stats.kernel_stack_pages;
    for (unsigned n = 0; n < TEST_ELF_INVALID_CASES; n++) {
        test_elf_t invalid;
        size_t size = test_elf_invalid(&invalid, n, USER_MAX_PAGES - USER_STACK_PAGES);
        uint32_t pid = next_pid;
        page_calls = 0;
        CHECK(!create_user_task((const uint8_t*)&invalid, size), "invalid ELF published");
        CHECK(!page_calls && next_pid == pid && pmm_get_free_ram() == baseline &&
              resource_stats.kernel_stack_pages == stacks, "ELF validation allocated/leaked");
    }
    /* A malformed later segment must also fail before the loader maps anything. */
    test_elf_t invalid = elf;
    invalid.segments[2].flags = 7;
    uint64_t* root = vmm_create_address_space();
    CHECK(root, "loader rejection root");
    uint64_t phys[4], va[4];
    uint32_t count = 0;
    page_calls = 0;
    CHECK(!elf_load((const uint8_t*)&invalid, sizeof(invalid), root, phys, va, &count, 4) &&
          !count && !root[0] && !page_calls, "loader skipped complete validation");
    vmm_destroy_address_space(root);
    serial_write_string("[MEM TEST PASS] ELF validation and layout\n");

    for (unsigned n = 0; n < calls; n++) {
        uint32_t pid = next_pid;
        page_fail_after = n;
        CHECK(!create_user_task((const uint8_t*)&elf, sizeof(elf)), "ELF OOM accepted");
        page_fail_after = -1;
        CHECK(next_pid == pid && pmm_get_free_ram() == baseline &&
              resource_stats.kernel_stack_pages == stacks, "ELF allocation rollback leak");
    }
    for (unsigned n = 0; n < mappings; n++) {
        uint32_t pid = next_pid;
        map_fail_after = n;
        CHECK(!create_user_task((const uint8_t*)&elf, sizeof(elf)), "ELF map failure accepted");
        map_fail_after = -1;
        CHECK(next_pid == pid && pmm_get_free_ram() == baseline &&
              resource_stats.kernel_stack_pages == stacks, "ELF mapping rollback leak");
    }
    elf.header.phnum = 2;
    elf.segments[1].vaddr = next_stack + elf.segments[1].offset;
    uint32_t pid = next_pid;
    CHECK(!create_user_task((const uint8_t*)&elf, sizeof(elf)), "ELF stack collision accepted");
    CHECK(next_pid == pid && pmm_get_free_ram() == baseline &&
          resource_stats.kernel_stack_pages == stacks, "ELF stack collision rollback leak");
    serial_write_string("[MEM TEST PASS] ELF allocation failures\n");
}

static void heap_oom_tests(void) {
    uint8_t* marker = kmalloc(32);
    CHECK(marker, "heap marker allocation");
    marker[0] = 0x5a;
    uint64_t baseline = pmm_get_free_ram();
    for (int n = 0; n < 16; n++) {
        page_fail_after = n;
        CHECK(!kmalloc(128 * 1024), "heap OOM accepted");
        page_fail_after = -1;
        CHECK(pmm_get_free_ram() == baseline && marker[0] == 0x5a,
              "heap growth rollback/corruption");
    }
    kfree(marker);
    serial_write_string("[MEM TEST PASS] heap growth rollback\n");
}

static void fork_tests(void) {
    struct task* idle = current_task;
    uint64_t baseline = pmm_get_free_ram();
    struct task* parent = process();
    private_page(parent, 0x40000000, VMM_FLAGS_USER_DATA);
    private_page(parent, 0x40001000, VMM_FLAGS_USER_RO | PTE_NX);
    private_page(parent, 0x40002000, VMM_FLAGS_USER_CODE);
    current_task = parent;
    page_calls = 0;
    int32_t pid = task_fork(&user_frame);
    unsigned calls = page_calls;
    CHECK(pid > 0, "fork baseline");
    struct task* child = task_find_by_pid(pid);
    child->state = TASK_DEAD;
    task_reap_dead();
    CHECK(cow_resolve(0x40000000, 7), "last owner resolve");
    uint64_t before[3];
    for (int i = 0; i < 3; i++) before[i] = *vmm_pte_in(parent->pml4, 0x40000000 + i * PAGE_SIZE);
    uint64_t free_before = pmm_get_free_ram();
    for (unsigned n = 0; n < calls; n++) {
        page_fail_after = n;
        CHECK(task_fork(&user_frame) == -1, "fork OOM accepted");
        page_fail_after = -1;
        CHECK(pmm_get_free_ram() == free_before, "fork OOM leak");
        for (int i = 0; i < 3; i++) {
            CHECK(*vmm_pte_in(parent->pml4, 0x40000000 + i * PAGE_SIZE) == before[i] &&
                  pmm_get_refcount((void*)parent->user_pages[i]) == 1, "parent changed on failed fork");
        }
    }
    for (int n = 0; n < 3; n++) {
        ref_fail_after = n;
        CHECK(task_fork(&user_frame) == -1, "ref acquisition failure ignored");
        ref_fail_after = -1;
        CHECK(pmm_get_free_ram() == free_before, "ref failure leak");
        map_fail_after = n;
        CHECK(task_fork(&user_frame) == -1, "fork partial mapping failure ignored");
        map_fail_after = -1;
        CHECK(pmm_get_free_ram() == free_before, "fork partial mapping leak");
    }
    child = task_find_by_pid(task_fork(&user_frame));
    CHECK(child, "fork child");
    uint64_t flags;
    vmm_get_physical_in(child->pml4, 0x40001000, &flags);
    CHECK(flags == (VMM_FLAGS_USER_RO | PTE_NX), "read-only data became CoW");
    vmm_get_physical_in(child->pml4, 0x40002000, &flags);
    CHECK(flags == VMM_FLAGS_USER_CODE, "executable code changed permissions");
    current_task = child;
    page_fail_after = 0;
    CHECK(!cow_resolve(0x40000000, 7), "CoW OOM accepted");
    page_fail_after = -1;
    CHECK(child->user_pages[0] == parent->user_pages[0] &&
          pmm_get_refcount((void*)parent->user_pages[0]) == 2, "CoW OOM mutated ownership");
    CHECK(cow_resolve(0x40000000, 7), "child CoW copy");
    *contents(child, 0x40000000) = 99;
    CHECK(*contents(parent, 0x40000000) == 0x1111111111111111ULL &&
          child->user_page_count == 3 && child->user_pages[0] != parent->user_pages[0],
          "private data isolation/tracking");
    CHECK(!cow_resolve(0x40001000, 7) && !cow_resolve(0x40000000, 0x15), "RO/NX fault resolved");
    struct task* grandchild = task_find_by_pid(task_fork(&user_frame));
    CHECK(grandchild, "repeated fork");
    struct task* thread = task_find_by_pid(task_clone(0x40002000, 0x40001000));
    CHECK(thread, "thread fixture");
    current_task = thread;
    CHECK(cow_resolve(0x40000000, 3) && child->user_page_count == 3 &&
          thread->user_page_count == 0 && child->user_pages[0] != grandchild->user_pages[0],
          "thread/kernel CoW owner");
    current_task = idle;
    parent->state = child->state = grandchild->state = TASK_DEAD;
    task_reap_dead();
    task_reap_dead();
    CHECK(pmm_get_free_ram() == baseline && preempt_depth() == 0, "fork lifecycle leak");
    serial_write_string("[MEM TEST PASS] CoW ownership and fork rollback\n");
}

void task_init(void) {
    production_task_init();
    mapping_tests();
    elf_oom_tests();
    heap_oom_tests();
    fork_tests();
}

void __real_shm_init(void);
void __wrap_shm_init(void) {
    __real_shm_init();
    struct task* idle = current_task;
    uint64_t baseline = pmm_get_free_ram();
    muted = 1;
    for (unsigned i = 0; i < 1000; i++) {
        struct task* parent = process();
        private_page(parent, 0x40000000, VMM_FLAGS_USER_DATA);
        current_task = parent;
        int id = shm_create(parent->pid, 4 * PAGE_SIZE);
        CHECK(id >= 0, "SHM create");
        if (i == 0) {
            uint64_t free_before = pmm_get_free_ram();
            for (int n = 0; n < 2; n++) {
                page_fail_after = n;
                CHECK(!shm_attach(id, parent->pid, parent->pml4), "SHM attach OOM");
                page_fail_after = -1;
                CHECK(pmm_get_free_ram() == free_before &&
                      parent->shm_attached[0].shmid == -1, "SHM attach rollback");
            }
            for (int n = 1; n < 4; n++) {
                map_fail_after = n;
                CHECK(!shm_attach(id, parent->pid, parent->pml4), "partial attach accepted");
                map_fail_after = -1;
                CHECK(pmm_get_free_ram() == free_before &&
                      parent->shm_attached[0].shmid == -1, "partial attach leak");
            }
        }
        uint64_t va = shm_attach(id, parent->pid, parent->pml4);
        CHECK(va, "SHM attach");
        struct task* child = task_find_by_pid(task_fork(&user_frame));
        CHECK(child && child->shm_attached[0].vaddr == va, "SHM inheritance");
        uint64_t flags;
        vmm_get_physical_in(child->pml4, va, &flags);
        CHECK(flags == VMM_FLAGS_USER_DATA, "SHM became CoW");
        *contents(child, va) = 123;
        CHECK(*contents(parent, va) == 123, "shared visibility");
        current_task = child;
        CHECK(cow_resolve(0x40000000, 7), "cycle private CoW");
        CHECK(shm_destroy(id) == -1, "destroy attached segment");
        current_task = idle;
        parent->state = child->state = TASK_DEAD;
        task_reap_dead();
        CHECK(pmm_get_free_ram() == baseline, "SHM/fork cycle leak");
    }
    /* Thread attach metadata must outlive that thread, and be detachable by
     * another member of the address space. Created-only segments die at exit. */
    struct task* owner = process();
    current_task = owner;
    struct task* thread = task_find_by_pid(task_clone(0x40000000, 0x80000000));
    CHECK(thread, "SHM thread fixture");
    current_task = thread;
    int id = shm_create(thread->pid, PAGE_SIZE);
    uint64_t va = shm_attach(id, thread->pid, thread->pml4);
    CHECK(va && owner->shm_attached[0].shmid == id && thread->shm_attached[0].shmid == -1,
          "SHM metadata not shared by group");
    current_task = owner;
    thread->state = TASK_DEAD;
    task_reap_dead();
    CHECK(shm_detach(va, owner->pid, owner->pml4) == 0, "leader detach thread attachment");
    CHECK(shm_create(owner->pid, PAGE_SIZE) >= 0, "created-only segment");
    current_task = idle;
    owner->state = TASK_DEAD;
    task_reap_dead();
    muted = 0;
    CHECK(pmm_get_free_ram() == baseline && preempt_depth() == 0, "SHM group cleanup leak");
    serial_write_string("[MEM TEST PASS] 1000 fork/write/SHM lifecycles\n");
}

struct task* __real_create_user_task(const uint8_t* binary, size_t size);
struct task* __wrap_create_user_task(const uint8_t* binary, size_t size) {
    static int started;
    if (!started) {
        started = 1;
        runtime_free = pmm_get_free_ram();
        runtime_reaped = resource_stats.tasks_reaped;
    }
    return __real_create_user_task(binary, size);
}
uint64_t __real_syscall_handler(uint64_t, uint64_t, uint64_t, uint64_t,
                              const struct registers*);
uint64_t __wrap_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                              const struct registers* frame) {
    if (n == 0x7fff) {
        CHECK((a == 1 || a == 2) && !(reports & (1u << a)), "Ring3 private/shared data");
        __atomic_fetch_or(&reports, 1u << a, __ATOMIC_SEQ_CST);
        return 0;
    }
    if (n == 0x7ffe) {
        CHECK(a == 1 || a == 2, "protection probe kind");
        expected[a - 1].pid = get_current_pid();
        expected[a - 1].va = b;
        expected[a - 1].kind = a;
        return 0;
    }
    return __real_syscall_handler(n, a, b, c, frame);
}

void __real_isr_handler(struct registers* regs);
void __wrap_isr_handler(struct registers* regs) {
    uint64_t vector = regs->vector;
    if (vector == 14) {
        uint64_t va;
        asm volatile("mov %%cr2, %0" : "=r"(va));
        for (unsigned i = 0; i < 2; i++) {
            /* CALL to NX data first writes its return address to a CoW stack;
             * that separate fault must be resolved before checking NX. */
            if (expected[i].kind && expected[i].pid == get_current_pid() && va == expected[i].va) {
                CHECK((regs->err_code & 5) == 5 &&
                      (i == 0 ? (regs->err_code & 2) : (regs->err_code & 16)) &&
                      !cow_resolve(va, regs->err_code), "hardware RO/NX protection");
                expected[i].kind = 0;
                __atomic_add_fetch(&protection_faults, 1, __ATOMIC_SEQ_CST);
                exit_current_task();
            }
        }
        if (regs->err_code & 4) __atomic_add_fetch(&user_cow, 1, __ATOMIC_SEQ_CST);
        else __atomic_add_fetch(&kernel_cow, 1, __ATOMIC_SEQ_CST);
    }
    __real_isr_handler(regs);
    if (vector != 32 || timer_get_ticks() < 1000) return;
    CHECK(reports == 6 && protection_faults == 2 && user_cow && kernel_cow,
          "missing runtime CoW/protection probes");
    CHECK(resource_stats.tasks_reaped == runtime_reaped + 4 &&
          resource_stats.kernel_stack_pages == 4 && pmm_get_free_ram() == runtime_free,
          "runtime memory leak");
    CHECK(preempt_depth() == 0, "runtime guard leak");
    serial_write_string("[MEM TEST PASS] Ring3 CoW, shared data, RO and NX\n");
    qemu_exit(0);
    for (;;) asm volatile("hlt");
}
