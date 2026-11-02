/* Real allocator/page-table failure injection plus native multicore libc use. */
#include "../kernel/task.c"
#include "../drivers/io.h"
#include "elf_fixture.h"

static int page_fail = -1, map_fail = -1, heap_fail = -1;
static unsigned page_calls, reported, shootdowns;
static unsigned progress[MAX_CPUS];
static uint64_t runtime_free;

static void __attribute__((noreturn)) fail(const char* why) {
    serial_write_string("[UMEM TEST FAIL] ");
    serial_write_string(why);
    serial_write_string("\n");
    outl(0xf4, 1);
    for (;;) asm volatile("cli; hlt");
}
#define CHECK(c, m) do { if (!(c)) fail(m); } while (0)
void* __real_pmm_alloc_page(void);
void* __wrap_pmm_alloc_page(void) {
    page_calls++;
    if (!page_fail) return NULL;
    if (page_fail > 0) page_fail--;
    return __real_pmm_alloc_page();
}
void* __real_kmalloc(size_t);
void* __wrap_kmalloc(size_t size) {
    if (!heap_fail) return NULL;
    if (heap_fail > 0) heap_fail--;
    return __real_kmalloc(size);
}
int __real_vmm_map_page_in(uint64_t*, uint64_t, uint64_t, uint64_t);
int __wrap_vmm_map_page_in(uint64_t* root, uint64_t va, uint64_t phys, uint64_t flags) {
    if (!map_fail) return 0;
    if (map_fail > 0) map_fail--;
    return __real_vmm_map_page_in(root, va, phys, flags);
}
int __real_vmm_map_pages_in(uint64_t*, uint64_t, const uint64_t*, uint32_t, uint64_t);
int __wrap_vmm_map_pages_in(uint64_t* root, uint64_t va, const uint64_t* frames,
                           uint32_t count, uint64_t flags) {
    /* Inject an invalid physical address at a selected leaf. This exercises
     * the production range mapper's own partial rollback, without bypassing it. */
    if (map_fail >= 0 && (unsigned)map_fail < count) {
        uint64_t* entry = (uint64_t*)frames + map_fail;
        uint64_t saved = *entry;
        *entry |= 1;
        int result = __real_vmm_map_pages_in(root, va, frames, count, flags);
        *entry = saved;
        return result;
    }
    if (map_fail >= 0) map_fail -= count;
    return __real_vmm_map_pages_in(root, va, frames, count, flags);
}
void __real_tlb_shootdown(uint64_t*, uint64_t);
void __wrap_tlb_shootdown(uint64_t* root, uint64_t va) {
    shootdowns++;
    __real_tlb_shootdown(root, va);
}
static void unused(void) { fail("fixture scheduled"); }
static struct task* process(void) {
    struct task* t = create_task(unused);
    CHECK(t, "fixture task");
    t->pml4 = vmm_create_address_space();
    CHECK(t->pml4, "fixture address space");
    t->state = TASK_WAITING;
    return t;
}
static uint8_t* bytes(struct task* t, uint64_t va) {
    uint64_t phys = vmm_get_physical_in(t->pml4, va, NULL);
    CHECK(phys, "missing mapping");
    return (void*)(phys + hhdm_request.response->offset + (va & (PAGE_SIZE - 1)));
}
static const struct registers frame = {.cs=0x23, .ss=0x1b, .rflags=0x202,
    .rip=0x40000000, .rsp=0x80004ff0};

static void kernel_tests(void) {
    /* Kernel heap keeps its high-water backing. Warm metadata capacity once
     * so exact PMM baselines measure owned pages/tables, not heap growth. */
    void* warm = kmalloc(512 * 1024);
    CHECK(warm, "metadata warmup");
    kfree(warm);
    struct task* idle = current_task;
    uint64_t baseline = pmm_get_free_ram();
    test_elf_t image = test_elf_valid();
    image.segments[0].memsz = 300 * PAGE_SIZE - offsetof(test_elf_t, payload);
    struct task* large = create_user_task((void*)&image, sizeof(image));
    CHECK(large && large->user_page_count == 300 + USER_STACK_PAGES,
          "ELF larger than old 256-page limit");
    CHECK(large->stack_base == USER_STACK_BASE &&
          !vmm_pte_in(large->pml4, USER_STACK_BASE - PAGE_SIZE) &&
          !vmm_pte_in(large->pml4, USER_STACK_BASE + USER_STACK_PAGES * PAGE_SIZE),
          "initial stack guards");
    large->state = TASK_DEAD;
    task_reap_dead();
    CHECK(pmm_get_free_ram() == baseline, "large ELF cleanup");

    struct task* p = process();
    current_task = p;
    uint64_t empty = pmm_get_free_ram();
    CHECK(!user_alloc(p, 0) && !user_alloc(p, SIZE_MAX) &&
          !user_alloc(p, USER_HEAP_END - USER_HEAP_BASE), "allocation bounds");
    uint64_t a = user_alloc(p, 3 * 1024 * 1024 + 17);
    CHECK(a == USER_HEAP_BASE && !(a & 15) && p->user_page_count == 769,
          "dynamic tracking/alignment");
    for (uint32_t i = 0; i < 769; i++) {
        uint64_t flags;
        CHECK(vmm_get_physical_in(p->pml4, a + (uint64_t)i * PAGE_SIZE, &flags) &&
              flags == VMM_FLAGS_USER_DATA, "heap user RW/NX flags");
        uint8_t* page = bytes(p, a + (uint64_t)i * PAGE_SIZE);
        for (unsigned j = 0; j < PAGE_SIZE; j++) CHECK(!page[j], "heap not zeroed");
        page[0] = (uint8_t)(i + 1);
    }
    CHECK(!vmm_pte_in(p->pml4, a + 769 * PAGE_SIZE), "heap trailing guard");
    uint64_t b = user_alloc(p, 32);
    CHECK(b == a + 770 * PAGE_SIZE, "allocator occupied guard");
    bytes(p, b)[0] = 0x6c;
    CHECK(user_free(p, a + 16) == -1 && user_free(p, (uint64_t)p) == -1 &&
          !user_realloc(p, a + 16, 64), "forged allocation base accepted");
    CHECK(!user_free(p, a) && user_free(p, a) == -1 && p->user_page_count == 1,
          "free ownership/count");
    CHECK(bytes(p, b)[0] == 0x6c, "range removal damaged adjacent allocation");
    uint64_t reused = user_alloc(p, PAGE_SIZE);
    CHECK(reused == a && !bytes(p, reused)[0], "VA reuse/old data exposure");
    CHECK(!user_free(p, reused) && !user_free(p, b) && pmm_get_free_ram() == empty,
          "private pages/tables leaked");
    serial_write_string("[UMEM TEST PASS] dynamic tracking, guards and VA reuse\n");

    uint64_t* old_tracking = p->user_pages;
    uint32_t capacity = p->user_page_capacity;
    heap_fail = 0;
    CHECK(!user_pages_reserve(p, capacity * 2) && p->user_pages == old_tracking &&
          p->user_page_capacity == capacity && !user_alloc(p, 16), "metadata OOM commit");
    heap_fail = -1;
    page_calls = 0;
    a = user_alloc(p, 8 * PAGE_SIZE);
    unsigned calls = page_calls;
    CHECK(a && !user_free(p, a), "OOM probe allocation");
    for (unsigned i = 0; i < calls; i++) {
        page_fail = i;
        CHECK(!user_alloc(p, 8 * PAGE_SIZE), "partial allocation succeeded");
        page_fail = -1;
        CHECK(!p->allocations && !p->user_page_count && pmm_get_free_ram() == empty,
              "allocation OOM rollback");
    }
    page_fail = 513 + 3; /* Fail the second PT after 512 leaves were installed. */
    CHECK(!user_alloc(p, 513 * PAGE_SIZE), "late page table OOM accepted");
    page_fail = -1;
    CHECK(!p->allocations && !p->user_page_count && pmm_get_free_ram() == empty,
          "late range rollback leaked pages/tables");
    shootdowns = 0;
    a = user_alloc(p, 513 * PAGE_SIZE);
    CHECK(a && !user_free(p, a) && shootdowns <= 8 && pmm_get_free_ram() == empty,
          "large range uses per-page shootdowns or leaks");
    for (unsigned i = 0; i < 8; i++) {
        map_fail = i;
        CHECK(!user_alloc(p, 8 * PAGE_SIZE), "partial mapping succeeded");
        map_fail = -1;
        CHECK(!p->allocations && !p->user_page_count && pmm_get_free_ram() == empty,
              "mapping rollback");
    }
    a = user_alloc(p, PAGE_SIZE + 17);
    CHECK(a, "resize fixture");
    bytes(p, a)[0] = 0x5a;
    bytes(p, a + PAGE_SIZE)[16] = 0xa5;
    uint64_t before = pmm_get_free_ram();
    for (int i = 0; i < 4; i++) {
        page_fail = i;
        CHECK(!user_realloc(p, a, 4 * PAGE_SIZE), "resize OOM accepted");
        page_fail = -1;
        CHECK(p->user_page_count == 2 && pmm_get_free_ram() == before &&
              bytes(p, a)[0] == 0x5a && bytes(p, a + PAGE_SIZE)[16] == 0xa5,
              "failed resize changed original");
    }
    b = user_realloc(p, a, 5 * PAGE_SIZE);
    CHECK(b && !vmm_pte_in(p->pml4, a) && bytes(p, b)[0] == 0x5a &&
          bytes(p, b + PAGE_SIZE)[16] == 0xa5 && !bytes(p, b + 4 * PAGE_SIZE)[0],
          "resize copy/zero tail");
    a = user_realloc(p, b, 1);
    CHECK(a && bytes(p, a)[0] == 0x5a && p->user_page_count == 1, "resize shrink");
    CHECK(!user_realloc(p, a, 0) && !p->user_page_count && pmm_get_free_ram() == empty,
          "resize to zero cleanup");
    serial_write_string("[UMEM TEST PASS] allocation and resize rollback\n");

    page_fail = 5;
    CHECK(shm_create(p->pid, 2 * 1024 * 1024) == -1, "large SHM OOM accepted");
    page_fail = -1;
    CHECK(pmm_get_free_ram() == empty, "large SHM allocation rollback");
    int id = shm_create(p->pid, 2 * 1024 * 1024 + 17);
    CHECK(id >= 0, "large SHM create");
    before = pmm_get_free_ram();
    map_fail = 511;
    CHECK(!shm_attach(id, p->pid, p->pml4), "large SHM partial map accepted");
    map_fail = -1;
    CHECK(p->shm_attached[0].shmid == -1 && pmm_get_free_ram() == before,
          "large SHM map rollback");
    uint64_t shared = shm_attach(id, p->pid, p->pml4);
    CHECK(shared == USER_SHM_BASE && !vmm_pte_in(p->pml4, shared + 513 * PAGE_SIZE),
          "large SHM VA/guard");
    CHECK(user_free(p, shared) == -1, "free accepted SHM");
    a = user_alloc(p, 2 * 1024 * 1024);
    CHECK(a, "fork allocation");
    bytes(p, a)[0] = 42;
    /* Fail after task and tracking allocation, while cloning allocation metadata. */
    before = pmm_get_free_ram();
    heap_fail = 2;
    CHECK(task_fork(&frame) == -1, "fork metadata failure accepted");
    heap_fail = -1;
    CHECK(pmm_get_free_ram() == before && bytes(p, a)[0] == 42, "fork metadata rollback");
    struct task* child = task_find_by_pid(task_fork(&frame));
    CHECK(child && child->allocations && child->allocations != p->allocations &&
          child->user_page_count == 512, "large fork metadata");
    current_task = child;
    CHECK(cow_resolve(a, 7), "large heap CoW");
    bytes(child, a)[0] = 99;
    bytes(child, shared)[0] = 123;
    CHECK(bytes(p, a)[0] == 42 && bytes(p, shared)[0] == 123, "private/shared fork semantics");
    CHECK(!user_free(child, a) && vmm_pte_in(p->pml4, a), "child free unmapped parent");
    current_task = p;
    child->state = TASK_DEAD;
    task_reap_dead();
    CHECK(!shm_detach(shared, p->pid, p->pml4), "large SHM detach");
    id = shm_create(p->pid, PAGE_SIZE);
    CHECK(id >= 0 && shm_attach(id, p->pid, p->pml4) == shared && !bytes(p, shared)[0],
          "SHM VA reuse/zeroing");
    CHECK(!shm_detach(shared, p->pid, p->pml4) && !user_free(p, a), "final free");
    CHECK(shm_create(p->pid, 0) == -1 &&
          shm_create(p->pid, SHM_MAX_PAGES * PAGE_SIZE + 1) == -1, "SHM size bounds");
    int ids[SHM_MAX_SEGMENTS];
    for (unsigned i = 0; i < SHM_MAX_SEGMENTS; i++) {
        ids[i] = shm_create(p->pid, PAGE_SIZE);
        CHECK(ids[i] >= 0, "SHM segment capacity");
    }
    CHECK(shm_create(p->pid, PAGE_SIZE) == -1, "SHM table overflow");
    for (unsigned i = 0; i < SHM_MAX_SEGMENTS; i++) CHECK(!shm_destroy(ids[i]), "SHM table cleanup");
    id = shm_create(p->pid, PAGE_SIZE);
    uint64_t aliases[SHM_MAX_ATTACH];
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++) {
        aliases[i] = shm_attach(id, p->pid, p->pml4);
        CHECK(aliases[i] == USER_SHM_BASE + (uint64_t)i * 2 * PAGE_SIZE,
              "SHM attachment guards/capacity");
    }
    CHECK(!shm_attach(id, p->pid, p->pml4), "SHM attachment overflow");
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++)
        CHECK(!shm_detach(aliases[i], p->pid, p->pml4), "SHM alias cleanup");
    current_task = idle;
    p->state = TASK_DEAD;
    task_reap_dead();
    CHECK(pmm_get_free_ram() == baseline, "dynamic memory lifecycle leak");
    serial_write_string("[UMEM TEST PASS] large SHM and fork ownership\n");
}

static void checker(void) {
    uint64_t start = timer_get_ticks();
    for (;;) {
        unsigned live = 0;
        {
            PREEMPT_GUARD;
            for (struct task* t = task_list_head; t; t = t->next)
                if (t->pid && t != current_task && t != reaper_task) live++;
        }
        if (__atomic_load_n(&reported, __ATOMIC_ACQUIRE) && !live) break;
        CHECK(timer_get_ticks() - start < 30000, "runtime cleanup stalled");
        asm volatile("hlt");
    }
    CHECK(pmm_get_free_ram() == runtime_free && resource_stats.kernel_stack_pages == 8,
          "runtime pages or stacks leaked");
    serial_write_string("[UMEM TEST PASS] Ring3 malloc, fork, threads and cleanup\n");
    outl(0xf4, 0);
    for (;;) asm volatile("hlt");
}
void __real_shm_init(void);
void __wrap_shm_init(void) {
    __real_shm_init();
    kernel_tests();
    CHECK(create_task_on_cpu(checker, 0), "checker creation");
    runtime_free = pmm_get_free_ram();
}
uint64_t __real_syscall_handler(uint64_t, uint64_t, uint64_t, uint64_t, const struct registers*);
uint64_t __wrap_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                                const struct registers* frame_) {
    if (n == 61) fail("Ring3 assertion");
    if (n == 62) {
        PREEMPT_GUARD;
        CHECK(a < smp_get_cpu_count() && b > progress[a] && b <= 64, "invalid progress");
        progress[a] = b;
        char number[24];
        serial_write_string("[UMEM PROGRESS] CPU ");
        itoa(a, number, 10); serial_write_string(number);
        serial_write_string(" cycles ");
        itoa(b, number, 10); serial_write_string(number);
        serial_write_string(" ticks ");
        itoa(timer_get_ticks(), number, 10); serial_write_string(number);
        serial_write_string("\n");
        return 0;
    }
    if (n == 60) {
        PREEMPT_GUARD;
        CHECK(!task_memory_owner(current_task)->allocations, "libc allocations remain");
        for (unsigned i = 0; i < SHM_MAX_ATTACH; i++)
            CHECK(current_task->shm_attached[i].shmid < 0, "Ring3 SHM remains");
        __atomic_store_n(&reported, 1, __ATOMIC_RELEASE);
        return 0;
    }
    return __real_syscall_handler(n, a, b, c, frame_);
}
