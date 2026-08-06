#pragma once
#include <stdint.h>
#include "../limine.h"

/*
 * SMP (Symmetric Multi-Processing) Support
 *
 * Uses Limine bootloader's SMP response, which is backed by ACPI MADT
 * (Multiple APIC Description Table) parsing. This gives us accurate
 * per-CPU information without needing to parse ACPI tables ourselves.
 *
 * SMP Initialization Levels:
 *   ✅ Level 0: CPUID-based detection (vendor, features)
 *   ✅ Level 1: ACPI MADT via Limine SMP (accurate CPU enumeration)
 *   🔴 Level 2: AP boot via goto_address (future)
 *   🔴 Level 3: Per-CPU schedulers + IPI (future)
 */

#define MAX_CPUS 16  /* Maximum supported CPUs */

/* Per-CPU information structure */
struct cpu_info {
    uint32_t processor_id; /* ACPI processor UID */
    uint32_t apic_id;      /* Local APIC ID (from MADT) */
    uint8_t  is_bsp;       /* 1 = Bootstrap Processor */
    uint8_t  active;       /* 1 = CPU is running */
    uint8_t  reserved[2];
};

/* ACPI table structures for RSDP/MADT info display */
struct acpi_rsdp {
    char     signature[8]; /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;     /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* ACPI 2.0+ fields */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/*
 * Initialize SMP detection.
 * @param smp_response: Limine SMP response (MADT-backed CPU list)
 * @param rsdp_addr:    ACPI RSDP address (for ACPI version info)
 */
void smp_init(struct limine_smp_response* smp_response, void* rsdp_addr);

/* Get number of detected CPUs */
uint32_t smp_get_cpu_count(void);

/* Get current CPU's APIC ID */
uint8_t smp_get_apic_id(void);

/* Get CPU info array */
struct cpu_info* smp_get_cpus(void);

/* Get BSP's LAPIC ID */
uint32_t smp_get_bsp_lapic_id(void);

/*
 * smp_start_timers() — Start LAPIC timers on all CPUs.
 * MUST be called AFTER task_init + syscall_init + shell loaded.
 * Releases APs from their spin-wait gate.
 */
void smp_start_timers(void);
