/* Actual concurrent CPU execution, mapping replacement, allocator stress and
 * native Ring3 syscalls. Test hooks are link-time wrappers, never production. */
#include "../kernel/task.c"
#include "../cpu/tlb.h"
#include "../drivers/io.h"
#include "../drivers/pci.h"
#include "../drivers/ps2.h"

#define TEST_VA 0xffffc00002000000ULL
static uint32_t arrived, phase, observed, finished, reports, expected_mask;
static uint64_t frames[2];
static uint64_t start_tick;
static uint32_t child_returns[MAX_CPUS];

static void short_lived_child(void) {
    __atomic_add_fetch(&child_returns[smp_get_current_cpu_id()], 1, __ATOMIC_RELAXED);
}

static void __attribute__((noreturn)) fail(const char* message) {
    serial_write_string("[CORE TEST FAIL] ");
    serial_write_string(message);
    serial_write_string("\n");
    outl(0xf4, 1);
    for (;;) asm volatile("cli; hlt");
}
#define CHECK(c, m) do { if (!(c)) fail(m); } while (0)

static void wait_mask(uint32_t* value, uint32_t wanted) {
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != wanted) {
        CHECK(timer_get_ticks() - start_tick < 20000, "barrier timed out");
        asm volatile("pause");
    }
}

static void worker(void) {
    uint32_t cpu = smp_get_current_cpu_id();
    CHECK(current_task->cpu_id == cpu, "wrong pinned CPU");
    CHECK(*(volatile uint64_t*)TEST_VA == 0xa0, "initial shared kernel mapping");
    __atomic_fetch_or(&arrived, 1U << cpu, __ATOMIC_RELEASE);
    if (cpu == 0) wait_mask(&arrived, expected_mask);
    for (uint32_t turn = 1; turn <= 128; turn++) {
        if (cpu == 0) {
            PREEMPT_GUARD;
            CHECK(vmm_unmap_page(TEST_VA) == frames[(turn - 1) & 1], "unmap old frame");
            CHECK(vmm_map_page(TEST_VA, frames[turn & 1], PTE_PRESENT | PTE_WRITE | PTE_NX),
                  "replace mapping");
            __atomic_store_n(&observed, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&phase, turn, __ATOMIC_RELEASE);
        } else wait_mask(&phase, turn);
        CHECK(*(volatile uint64_t*)TEST_VA == 0xa0 + (turn & 1), "stale remote TLB");
        __atomic_fetch_or(&observed, 1U << cpu, __ATOMIC_RELEASE);
        wait_mask(&observed, expected_mask);
        /* Do not let the coordinator reset observed before peers saw it. */
        if (cpu != 0) {
            __atomic_fetch_or(&finished, 1U << cpu, __ATOMIC_RELEASE);
        } else {
            wait_mask(&finished, expected_mask & ~1U);
            __atomic_store_n(&finished, 0, __ATOMIC_RELEASE);
        }
    }
    for (unsigned i = 0; i < 1000; i++) {
        void* page = pmm_alloc_page();
        CHECK(page && pmm_ref_page(page), "concurrent PMM allocation/ref");
        uint64_t* alias = (void*)((uint64_t)page + hhdm_request.response->offset);
        alias[0] = ((uint64_t)cpu << 32) | i;
        void* block = kmalloc(17 + (i % 97));
        CHECK(block && !((uint64_t)block & 15), "heap alignment/OOM");
        CHECK(alias[0] == (((uint64_t)cpu << 32) | i), "allocator ownership race");
        kfree(block);
        pmm_free_page(page);
        pmm_free_page(page);
    }
    for (unsigned i = 0; i < 32; i++) {
        uint32_t pid;
        {
            PREEMPT_GUARD;
            struct task* child = create_task_on_cpu(short_lived_child, cpu);
            CHECK(child, "concurrent task creation");
            pid = child->pid;
        }
        while (!task_is_dead(pid)) {
            CHECK(timer_get_ticks() - start_tick < 20000, "child exit stalled");
            asm volatile("hlt");
        }
    }
    CHECK(__atomic_load_n(&child_returns[cpu], __ATOMIC_RELAXED) == 32,
          "child returned on wrong CPU");
    __atomic_fetch_or(&arrived, 1U << (cpu + 16), __ATOMIC_RELEASE);
    if (cpu != 0) return;
    wait_mask(&arrived, expected_mask | (expected_mask << 16));
    serial_write_string("[CORE TEST PASS] concurrent CPUs, TLB replacement and allocators\n");
    wait_mask(&reports, 6);
    {
        PREEMPT_GUARD;
        CHECK(vmm_unmap_page(TEST_VA) == frames[0], "final unmap");
        pmm_free_page((void*)frames[0]);
        pmm_free_page((void*)frames[1]);
    }
    /* Reaper must remove every user thread and exited worker. */
    for (;;) {
        unsigned live = 0;
        {
            PREEMPT_GUARD;
            for (struct task* t = task_list_head; t; t = t->next)
                if (t->pid && t != current_task && t != reaper_task) live++;
        }
        if (!live) break;
        CHECK(timer_get_ticks() - start_tick < 20000, "reaper stalled");
        asm volatile("hlt");
    }
    serial_write_string("[CORE TEST PASS] Ring3 multicore fork, clone, sleep and cleanup\n");
    CHECK(resource_stats.kernel_stack_pages == 8, "retired kernel stacks leaked");
    outl(0xf4, 0);
    for (;;) asm volatile("hlt");
}

struct task* __real_create_user_task(const uint8_t*, size_t);
struct task* __wrap_create_user_task(const uint8_t* data, size_t size) {
    PREEMPT_GUARD;
    CHECK(pci_count() >= 3 && ps2_mouse_present(), "QEMU PCI/PS2 initialization");
    struct pci_device device;
    CHECK(!pci_get(0, &device) && device.vendor != 0xffff && pci_get(PCI_MAX_DEVICES, &device),
          "PCI snapshot bounds");
    serial_write_string("[CORE TEST PASS] PCI and PS2 initialization\n");
    frames[0] = (uint64_t)pmm_alloc_page();
    frames[1] = (uint64_t)pmm_alloc_page();
    CHECK(frames[0] && frames[1], "shootdown fixture frames");
    *(uint64_t*)(frames[0] + hhdm_request.response->offset) = 0xa0;
    *(uint64_t*)(frames[1] + hhdm_request.response->offset) = 0xa1;
    CHECK(vmm_map_page(TEST_VA, frames[0], PTE_PRESENT | PTE_WRITE | PTE_NX), "fixture mapping");
    for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
        CHECK(create_task_on_cpu(worker, i), "CPU worker allocation");
        expected_mask |= 1U << i;
    }
    CHECK(!kmalloc(SIZE_MAX), "heap size overflow");
    start_tick = timer_get_ticks();
    return __real_create_user_task(data, size);
}

uint64_t __real_syscall_handler(uint64_t, uint64_t, uint64_t, uint64_t, const struct registers*);
uint64_t __wrap_syscall_handler(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
                                const struct registers* frame) {
    if (n == 61) fail("userspace assertion");
    if (n == 63) {
        serial_write_string("[CORE TEST READY] input\n");
        return 0;
    }
    if (n == 64) {
        serial_write_string("[CORE TEST PASS] PS2 IRQ to Ring3 input events\n");
        return 0;
    }
    if (n == 60) {
        CHECK(a == 1 || a == 2, "invalid user report");
        if (a == 1) CHECK(b == expected_mask, "Ring3 did not execute on every CPU");
        __atomic_fetch_or(&reports, 1U << a, __ATOMIC_RELEASE);
        return 0;
    }
    return __real_syscall_handler(n, a, b, c, frame);
}
