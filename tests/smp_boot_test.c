/* Link-time instrumentation for run_smp_boot_tests.py; never in production. */
#include "../cpu/smp.h"
#include "../cpu/percpu.h"
#include "../cpu/lapic.h"
#include "../drivers/serial.h"
#include "../mm/pmm.h"

#ifndef SMP_TEST_MODE
#define SMP_TEST_MODE 0
#endif

static int allocating_ap_stacks;
static uint32_t expected_timers;
static uint32_t started_timers;

static void require(int condition, const char* message) {
    if (condition) return;
    serial_write_string("[SMP TEST FAIL] ");
    serial_write_string(message);
    serial_write_string("\n");
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

void __real_smp_init(struct limine_smp_response*, void*);
void __real_smp_start_timers(void);
void __real_percpu_init_ap(uint32_t, uint32_t, uint64_t);
void __real_lapic_timer_init(uint32_t);
void* __real_pmm_alloc_contiguous_pages(uint64_t);

/* Capture RSP before any compiler prologue so the actual call ABI is tested. */
asm(".text\n"
    ".global __wrap_percpu_init_ap\n"
    "__wrap_percpu_init_ap:\n"
    "mov %rsp, %rcx\n"
    "jmp smp_test_percpu_init_ap\n");

void smp_test_percpu_init_ap(uint32_t cpu, uint32_t apic, uint64_t top,
                           uint64_t entry_rsp) {
    require(cpu > 0 && cpu < MAX_CPUS, "AP must not use BSP's per-CPU slot");
    require((entry_rsp & 15) == 8, "AP C call stack alignment");
    require(entry_rsp < top && entry_rsp >= top - 2 * PAGE_SIZE,
            "AP must execute on its allocated stack");
    __real_percpu_init_ap(cpu, apic, top);
}

void* __wrap_pmm_alloc_contiguous_pages(uint64_t count) {
    if (SMP_TEST_MODE == 3 && allocating_ap_stacks) return NULL;
    return __real_pmm_alloc_contiguous_pages(count);
}

void __wrap_smp_init(struct limine_smp_response* response, void* rsdp) {
    struct limine_smp_response reordered;
    struct limine_smp_info* entries[32];
    if (SMP_TEST_MODE == 1) {
        response = NULL;
    } else if (SMP_TEST_MODE == 2) {
        require(response && response->cpu_count <= 32, "test topology size");
        reordered = *response;
        for (uint64_t i = 0; i < response->cpu_count; i++) {
            entries[i] = response->cpus[response->cpu_count - i - 1];
        }
        reordered.cpus = entries;
        response = &reordered;
    }
    uint32_t expected = response ? response->cpu_count : 1;
    if (expected > MAX_CPUS) expected = MAX_CPUS;
    expected_timers = SMP_TEST_MODE == 3 ? 1 : expected;

    allocating_ap_stacks = 1;
    __real_smp_init(response, rsdp);
    allocating_ap_stacks = 0;

    require(smp_get_cpu_count() == expected, "selected CPU count");
    require(lapic_get_base_virt() != 0, "BSP LAPIC initialization");
    require(smp_get_current_cpu_id() == 0, "BSP logical CPU ID");
    require(percpu_get(0)->lapic_id == smp_get_bsp_lapic_id(), "BSP metadata");
    struct cpu_info* cpus = smp_get_cpus();
    require(cpus[0].is_bsp && cpus[0].active, "BSP in slot zero");
    for (uint32_t i = 1; i < expected; i++) {
        require(!cpus[i].is_bsp, "AP slot identity");
        require(__atomic_load_n(&cpus[i].active, __ATOMIC_ACQUIRE) ==
                (SMP_TEST_MODE != 3), "AP boot status");
        if (SMP_TEST_MODE != 3) {
            require(percpu_get(i)->cpu_id == i, "AP logical CPU ID");
            require(percpu_get(i)->lapic_id == cpus[i].apic_id, "AP metadata");
        }
    }
    serial_write_string("[SMP TEST PASS] topology and stacks\n");
}

void __wrap_lapic_timer_init(uint32_t hz) {
    __real_lapic_timer_init(hz);
    __atomic_add_fetch(&started_timers, 1, __ATOMIC_SEQ_CST);
}

void __wrap_smp_start_timers(void) {
    __real_smp_start_timers();
    uint32_t remaining = 10000000;
    while (__atomic_load_n(&started_timers, __ATOMIC_SEQ_CST) < expected_timers &&
           remaining-- > 0) {
        asm volatile("pause");
    }
    require(__atomic_load_n(&started_timers, __ATOMIC_SEQ_CST) == expected_timers,
            "timers must start on every active CPU");
    /* Report from BSP so AP output cannot interleave with the boot log. */
    serial_write_string("[SMP TEST PASS] timers\n");
}
