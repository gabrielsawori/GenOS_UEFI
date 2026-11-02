#pragma once
#include <stdint.h>
#define GENOS_MAX_CPUS 16
struct genos_system_info {
    uint32_t version, detected_cpus, online_cpus, current_cpu;
    uint32_t pci_devices, ps2_mouse;
    uint64_t ticks, free_bytes, input_dropped;
    uint64_t switches[GENOS_MAX_CPUS];
};
