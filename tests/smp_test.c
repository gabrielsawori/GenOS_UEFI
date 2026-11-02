/* Host regression tests. Link with --gc-sections to discard hardware-only code.
 * See documentation/smp_bugfix.md for the build command.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../cpu/smp.c"

static char serial_output[512];
static size_t serial_used;
static uint32_t current_apic_id;
static unsigned lapic_reads;

void serial_write_string(const char* text) {
    size_t length = strlen(text);
    assert(serial_used + length < sizeof(serial_output));
    memcpy(serial_output + serial_used, text, length + 1);
    serial_used += length;
}

uint32_t lapic_get_id(void) {
    lapic_reads++;
    return current_apic_id;
}

static void test_cpu_order(void) {
    enum { TOTAL = MAX_CPUS + 4 };
    struct limine_smp_info infos[TOTAL] = {0};
    struct limine_smp_info* entries[TOTAL];
    struct limine_smp_info* selected[MAX_CPUS];
    for (uint32_t i = 0; i < TOTAL; i++) {
        infos[i].processor_id = 100 + i;
        infos[i].lapic_id = 2 * i + 7;
        entries[i] = &infos[i];
    }

    /* Every BSP position, including beyond MAX_CPUS, and 1..TOTAL CPUs. */
    for (uint32_t total = 1; total <= TOTAL; total++) {
        for (uint32_t bsp = 0; bsp < total; bsp++) {
            struct limine_smp_response response = {
                .cpu_count = total,
                .bsp_lapic_id = infos[bsp].lapic_id,
                .cpus = entries,
            };
            uint32_t count = select_cpus(&response, selected);
            assert(count == (total < MAX_CPUS ? total : MAX_CPUS));
            assert(selected[0] == &infos[bsp]);
            uint32_t expected = 0;
            for (uint32_t i = 1; i < count; i++) {
                if (expected == bsp) expected++;
                assert(selected[i] == &infos[expected++]);
            }
            for (uint32_t i = 0; i < TOTAL; i++) {
                assert(entries[i] == &infos[i]); /* Do not reorder Limine's table. */
                assert(infos[i].goto_address == NULL);
            }
        }
    }
}

static void test_unavailable_topology(void) {
    struct limine_smp_info* selected[MAX_CPUS];
    struct limine_smp_response response = {0};
    assert(select_cpus(NULL, selected) == 0);
    assert(select_cpus(&response, selected) == 0);
    response.cpu_count = 1;
    assert(select_cpus(&response, selected) == 0);

    struct limine_smp_info ap = {.lapic_id = 7};
    struct limine_smp_info* entries[] = {NULL, &ap};
    response.cpus = entries;
    response.cpu_count = 2;
    assert(select_cpus(&response, selected) == 0); /* Missing BSP. */
    response.bsp_lapic_id = 7;
    assert(select_cpus(&response, selected) == 1);
    assert(selected[0] == &ap);
}

static void test_current_cpu(void) {
    cpu_count = 0;
    assert(smp_get_current_cpu_id() == 0);
    cpu_count = 1;
    assert(smp_get_current_cpu_id() == 0);
    assert(lapic_reads == 0); /* No MMIO before LAPIC initialization. */

    cpu_count = 3;
    cpus[0].apic_id = 42;
    cpus[1].apic_id = 7;
    cpus[2].apic_id = 201;
    for (uint32_t i = 0; i < cpu_count; i++) {
        current_apic_id = cpus[i].apic_id;
        assert(smp_get_current_cpu_id() == i);
    }
}

static void test_full_width_acpi_address(void) {
    struct acpi_rsdp rsdp = {
        .signature = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '},
        .oem_id = {'T', 'E', 'S', 'T', ' ', ' '},
        .revision = 2,
        .xsdt_address = UINT64_MAX,
    };
    print_acpi_info(&rsdp);
    assert(strstr(serial_output, "XSDT Address: 0xffffffffffffffff\n"));
}

int main(void) {
    test_cpu_order();
    test_unavailable_topology();
    test_current_cpu();
    test_full_width_acpi_address();
    puts("SMP regression tests passed (210 topologies, fallback, CPU IDs, ACPI).");
    return 0;
}
