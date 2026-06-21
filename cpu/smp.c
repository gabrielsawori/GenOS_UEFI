#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "percpu.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"
#include "../mm/pmm.h"

/*
 * SMP — Full Multi-Core Support via Limine goto_address
 *
 * Boot sequence for Application Processors (APs):
 *   1. BSP allocates per-AP stacks (8KB each from PMM)
 *   2. BSP stores AP boot data in extra_argument
 *   3. BSP writes ap_entry address to goto_address
 *   4. AP wakes up, loads BSP's GDT + IDT
 *   5. AP signals alive via atomic counter
 *   6. AP enters idle loop (cli; hlt) until per-CPU scheduling is ready
 *
 * Limine guarantees:
 *   - AP is in 64-bit long mode
 *   - AP has paging enabled (same CR3 as BSP)
 *   - AP has a temporary stack
 *   - AP receives struct limine_smp_info* in RDI
 */

#define AP_STACK_PAGES 2  /* 8KB per AP */

static struct cpu_info cpus[MAX_CPUS];
static uint32_t cpu_count = 0;
static uint32_t bsp_lapic_id = 0;

/* Atomic counter: how many APs have booted successfully */
static volatile uint32_t aps_booted = 0;

/* Per-AP boot data passed via extra_argument */
struct ap_boot_info {
    uint64_t stack_top;   /* AP's allocated stack top */
    uint32_t cpu_index;   /* Index into cpus[] array */
    uint32_t lapic_id;    /* This AP's LAPIC ID */
};

static struct ap_boot_info ap_boot_data[MAX_CPUS];

/* Read CPUID */
static void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                  uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

/* Read Model-Specific Register */
static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * ap_entry() — Entry point for each Application Processor.
 *
 * Called by Limine when BSP writes to goto_address.
 * The AP arrives here with:
 *   - RDI = pointer to struct limine_smp_info
 *   - Paging active (same CR3 as BSP)
 *   - 64-bit long mode
 *   - Interrupts disabled
 *   - Limine's temporary GDT/stack
 *
 * We load BSP's GDT + IDT, signal alive, and enter idle.
 */
static void ap_entry(struct limine_smp_info* info) {
    /* 1. Get our boot data from extra_argument */
    struct ap_boot_info* boot = (struct ap_boot_info*)info->extra_argument;

    /* 2. Switch to our allocated stack */
    uint64_t new_stack = boot->stack_top;
    asm volatile("mov %0, %%rsp" : : "r"(new_stack) : "memory");

    /* 3. Initialize per-CPU GDT+TSS (gives this AP its own TSS with RSP0)
     * This REPLACES gdt_load_on_ap() — we load a DIFFERENT GDT per AP. */
    percpu_init_ap(boot->cpu_index, boot->lapic_id, boot->stack_top);
    idt_load_on_ap();

    /* 4. Initialize LAPIC on this AP */
    lapic_init();

    /* 5. Start LAPIC periodic timer at ~1000 Hz */
    lapic_timer_init(1000);

    /* 6. Mark this CPU as active */
    cpus[boot->cpu_index].active = 1;

    /* 7. Signal to BSP that we're alive */
    __atomic_add_fetch(&aps_booted, 1, __ATOMIC_SEQ_CST);

    /* 8. Enable interrupts and enter idle loop.
     * LAPIC timer fires vector 48 → isr_handler → schedule().
     * When scheduler picks a task for this CPU, it will context-switch
     * to that task via iretq. The AP is now a fully active CPU! */
    asm volatile("sti");
    while (1) {
        asm volatile("hlt");
    }
}

/*
 * print_cpuid_info() — Detect and print CPU features via CPUID.
 */
static void print_cpuid_info(void) {
    uint32_t eax, ebx, ecx, edx;
    char buf[16];

    /* Vendor string */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    char vendor[13];
    *((uint32_t*)&vendor[0]) = ebx;
    *((uint32_t*)&vendor[4]) = edx;
    *((uint32_t*)&vendor[8]) = ecx;
    vendor[12] = '\0';
    serial_write_string("  CPU Vendor: ");
    serial_write_string(vendor);
    serial_write_string("\n");

    /* Features (CPUID leaf 1) */
    cpuid(1, &eax, &ebx, &ecx, &edx);

    uint32_t stepping = eax & 0xF;
    uint32_t model    = (eax >> 4) & 0xF;
    uint32_t family   = (eax >> 8) & 0xF;
    uint32_t ext_model = (eax >> 16) & 0xF;
    uint32_t ext_family = (eax >> 20) & 0xFF;
    if (family == 0xF) family += ext_family;
    if (family == 0x6 || family == 0xF) model += (ext_model << 4);

    serial_write_string("  CPU Family: ");
    itoa(family, buf, 10);
    serial_write_string(buf);
    serial_write_string(" Model: ");
    itoa(model, buf, 10);
    serial_write_string(buf);
    serial_write_string(" Stepping: ");
    itoa(stepping, buf, 10);
    serial_write_string(buf);
    serial_write_string("\n");

    int has_apic  = (edx >> 9)  & 1;
    int has_msr   = (edx >> 5)  & 1;
    int has_sse2  = (edx >> 26) & 1;
    int has_sse3  = (ecx >> 0)  & 1;
    int has_ssse3 = (ecx >> 9)  & 1;
    int has_sse41 = (ecx >> 19) & 1;
    int has_avx   = (ecx >> 28) & 1;
    int has_x2apic = (ecx >> 21) & 1;

    serial_write_string("  Features: ");
    if (has_apic)   serial_write_string("APIC ");
    if (has_x2apic) serial_write_string("x2APIC ");
    if (has_msr)    serial_write_string("MSR ");
    if (has_sse2)   serial_write_string("SSE2 ");
    if (has_sse3)   serial_write_string("SSE3 ");
    if (has_ssse3)  serial_write_string("SSSE3 ");
    if (has_sse41)  serial_write_string("SSE4.1 ");
    if (has_avx)    serial_write_string("AVX ");
    serial_write_string("\n");

    if (has_apic && has_msr) {
        uint64_t lapic_msr = rdmsr(0x1B);
        uint64_t lapic_base = lapic_msr & 0xFFFFF000ULL;
        int enabled = (lapic_msr >> 11) & 1;
        serial_write_string("  LAPIC Base: 0x");
        itoa(lapic_base, buf, 16);
        serial_write_string(buf);
        serial_write_string(enabled ? " (enabled)\n" : " (disabled)\n");
    }
}

/*
 * print_acpi_info() — Print ACPI RSDP information.
 */
static void print_acpi_info(void* rsdp_addr) {
    if (!rsdp_addr) {
        serial_write_string("  ACPI: RSDP not available\n");
        return;
    }

    struct acpi_rsdp* rsdp = (struct acpi_rsdp*)rsdp_addr;

    int valid = 1;
    const char* expected = "RSD PTR ";
    for (int i = 0; i < 8; i++) {
        if (rsdp->signature[i] != expected[i]) { valid = 0; break; }
    }
    if (!valid) {
        serial_write_string("  ACPI: Invalid RSDP signature!\n");
        return;
    }

    serial_write_string("  ACPI OEM: ");
    char oem[7];
    for (int i = 0; i < 6; i++) oem[i] = rsdp->oem_id[i];
    oem[6] = '\0';
    serial_write_string(oem);

    char buf[16];
    serial_write_string(" Rev: ");
    if (rsdp->revision >= 2) {
        serial_write_string("2.0+ (XSDT)\n");
        serial_write_string("  XSDT Address: 0x");
        itoa(rsdp->xsdt_address, buf, 16);
        serial_write_string(buf);
        serial_write_string("\n");
    } else {
        serial_write_string("1.0 (RSDT)\n");
    }
    serial_write_string("  RSDT Address: 0x");
    itoa(rsdp->rsdt_address, buf, 16);
    serial_write_string(buf);
    serial_write_string("\n");
}

/*
 * smp_init() — Full SMP initialization with AP boot.
 */
void smp_init(struct limine_smp_response* smp_response, void* rsdp_addr) {
    serial_write_string("[INFO] Detecting CPU topology (ACPI MADT + CPUID)...\n");

    /* Phase 1: CPUID feature detection */
    print_cpuid_info();

    /* Phase 2: ACPI RSDP info */
    print_acpi_info(rsdp_addr);

    /* Phase 3: Enumerate and BOOT CPUs via Limine SMP */
    if (!smp_response || smp_response->cpu_count == 0) {
        serial_write_string("  [WARN] Limine SMP unavailable, single-CPU fallback\n");
        uint32_t eax, ebx, ecx, edx;
        cpuid(1, &eax, &ebx, &ecx, &edx);
        cpus[0].processor_id = 0;
        cpus[0].apic_id = (ebx >> 24) & 0xFF;
        cpus[0].is_bsp = 1;
        cpus[0].active = 1;
        cpu_count = 1;
        bsp_lapic_id = cpus[0].apic_id;
        serial_write_string("[OK] SMP: 1 CPU (CPUID fallback)\n");
        return;
    }

    char buf[16];
    bsp_lapic_id = smp_response->bsp_lapic_id;

    serial_write_string("  BSP LAPIC ID: ");
    itoa(bsp_lapic_id, buf, 10);
    serial_write_string(buf);
    serial_write_string("\n");

    uint64_t total = smp_response->cpu_count;
    if (total > MAX_CPUS) total = MAX_CPUS;
    cpu_count = (uint32_t)total;

    /* HHDM offset for PMM physical-to-virtual conversion */
    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t hhdm_off = hhdm_request.response->offset;

    uint32_t ap_count = 0;

    serial_write_string("  MADT CPU Enumeration:\n");

    for (uint64_t i = 0; i < total; i++) {
        struct limine_smp_info* info = smp_response->cpus[i];
        int is_bsp = (info->lapic_id == bsp_lapic_id);

        cpus[i].processor_id = info->processor_id;
        cpus[i].apic_id      = info->lapic_id;
        cpus[i].is_bsp       = is_bsp ? 1 : 0;
        cpus[i].active       = is_bsp ? 1 : 0; /* APs set active in ap_entry */

        serial_write_string("    CPU #");
        itoa(i, buf, 10);
        serial_write_string(buf);
        serial_write_string(": LAPIC_ID=");
        itoa(info->lapic_id, buf, 10);
        serial_write_string(buf);

        if (is_bsp) {
            serial_write_string(" [BSP]\n");
            continue;
        }

        /* === BOOT THIS AP === */

        /* Allocate 8KB stack (2 pages) for this AP */
        uint64_t sphys1 = (uint64_t)pmm_alloc_page();
        uint64_t sphys2 = (uint64_t)pmm_alloc_page();
        if (!sphys1 || !sphys2) {
            serial_write_string(" [AP, NO MEMORY — skipped]\n");
            continue;
        }
        /* Stack grows down: top = end of second page */
        uint64_t stack_top = sphys2 + hhdm_off + 4096;

        /* Prepare boot data */
        ap_boot_data[i].stack_top  = stack_top;
        ap_boot_data[i].cpu_index  = (uint32_t)i;
        ap_boot_data[i].lapic_id   = info->lapic_id;

        /* Pass boot data via extra_argument */
        info->extra_argument = (uint64_t)&ap_boot_data[i];

        /* LAUNCH: Write goto_address — AP wakes up immediately! */
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);

        ap_count++;
        serial_write_string(" [AP, booting...]\n");
    }

    /* Phase 4: Wait for all APs to signal alive */
    if (ap_count > 0) {
        serial_write_string("  Waiting for ");
        itoa(ap_count, buf, 10);
        serial_write_string(buf);
        serial_write_string(" AP(s) to boot...\n");

        /* Spin-wait with timeout (~100ms max) */
        volatile uint32_t timeout = 10000000;
        while (__atomic_load_n(&aps_booted, __ATOMIC_SEQ_CST) < ap_count && timeout > 0) {
            asm volatile("pause");
            timeout--;
        }

        uint32_t booted = __atomic_load_n(&aps_booted, __ATOMIC_SEQ_CST);

        if (booted == ap_count) {
            serial_write_string("  [OK] All ");
            itoa(booted, buf, 10);
            serial_write_string(buf);
            serial_write_string(" AP(s) booted and idling!\n");
        } else {
            serial_write_string("  [WARN] Only ");
            itoa(booted, buf, 10);
            serial_write_string(buf);
            serial_write_string("/");
            itoa(ap_count, buf, 10);
            serial_write_string(buf);
            serial_write_string(" AP(s) responded\n");
        }
    }

    /* Phase 5: Per-CPU data + LAPIC on BSP */
    percpu_init_bsp();
    lapic_init();
    lapic_timer_init(1000);
    serial_write_string("  [OK] BSP per-CPU TSS + LAPIC timer ready\n");

    /* Summary */
    uint32_t active_count = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].active) active_count++;
    }

    serial_write_string("[OK] SMP: ");
    itoa(cpu_count, buf, 10);
    serial_write_string(buf);
    serial_write_string(" CPU(s) total, ");
    itoa(active_count, buf, 10);
    serial_write_string(buf);
    serial_write_string(" active (BSP + ");
    itoa(active_count - 1, buf, 10);
    serial_write_string(buf);
    serial_write_string(" AP(s) with LAPIC timers)\n");
}

uint32_t smp_get_cpu_count(void) {
    return cpu_count;
}

uint8_t smp_get_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF;
}

struct cpu_info* smp_get_cpus(void) {
    return cpus;
}

uint32_t smp_get_bsp_lapic_id(void) {
    return bsp_lapic_id;
}
