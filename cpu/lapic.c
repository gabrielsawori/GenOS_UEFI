#include "lapic.h"
#include "../limine.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"
#include "../drivers/io.h"

/*
 * Local APIC Driver
 *
 * LAPIC MMIO registers at physical 0xFEE00000 (accessed via HHDM).
 * Each CPU has its own LAPIC mapped at the same physical address.
 */

/* LAPIC register offsets */
#define LAPIC_ID        0x020  /* LAPIC ID (bits 24-31) */
#define LAPIC_VER       0x030  /* Version */
#define LAPIC_TPR       0x080  /* Task Priority */
#define LAPIC_EOI       0x0B0  /* End of Interrupt (write 0) */
#define LAPIC_SVR       0x0F0  /* Spurious Interrupt Vector */
#define LAPIC_ICR_LO    0x300  /* Interrupt Command (low) */
#define LAPIC_ICR_HI    0x310  /* Interrupt Command (high) */
#define LAPIC_LVT_TIMER 0x320  /* LVT Timer */
#define LAPIC_TIMER_ICR 0x380  /* Timer Initial Count */
#define LAPIC_TIMER_CCR 0x390  /* Timer Current Count */
#define LAPIC_TIMER_DCR 0x3E0  /* Timer Divide Config */

/* SVR bits */
#define SVR_ENABLE      0x100  /* APIC Software Enable */
#define SVR_VECTOR      0xFF   /* Spurious vector = 0xFF */

/* LVT Timer bits */
#define TIMER_PERIODIC  (1 << 17)  /* Periodic mode */
#define TIMER_MASKED    (1 << 16)  /* Masked */

static uint64_t lapic_base_virt = 0;
static uint32_t timer_counts_per_second;

extern volatile struct limine_hhdm_request hhdm_request;

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(lapic_base_virt + reg) = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)(lapic_base_virt + reg);
}

/*
 * lapic_init() — Enable the Local APIC on the current CPU.
 */
extern uint32_t smp_get_bsp_lapic_id(void);

void lapic_init(void) {
    if (lapic_base_virt == 0) {
        /* First call: compute virtual address from HHDM */
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
        uint64_t phys = ((uint64_t)hi << 32 | lo) & 0xFFFFF000ULL;
        lapic_base_virt = phys + hhdm_request.response->offset;
    }

    /* Enable LAPIC via SVR: set enable bit + spurious vector 0xFF */
    lapic_write(LAPIC_SVR, SVR_ENABLE | 0xFF);

    /* Set task priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* BUG FIX (Bare Metal): Route legacy 8259 PIC interrupts to the BSP.
     * When LAPIC is enabled, it takes over interrupt routing. If LINT0
     * is not configured as ExtINT, PIC interrupts (like the mouse on IRQ12)
     * will be dropped. Keyboard (IRQ1) might sometimes work if BIOS left it
     * routed, but IRQ12 typically fails. */
    if (lapic_get_id() == smp_get_bsp_lapic_id()) {
        lapic_write(0x350, 0x00000700); /* LINT0: Delivery Mode = ExtINT, Unmasked */
        lapic_write(0x360, 0x00000400); /* LINT1: Delivery Mode = NMI, Unmasked */
    }
}

/*
 * lapic_timer_init() — Start periodic LAPIC timer.
 *
 * BSP calibrates with PIT channel 2 before releasing APs. All QEMU CPUs
 * use that rate; PIT channel 0 remains the monotonic time source.
 */
void lapic_timer_init(uint32_t hz) {
    if (!hz || hz > 1000000) return;
    /* Divide by 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /* BSP calibrates once against PIT channel 2 (~10 ms). AP gate publishes
     * the result; APs never reprogram the shared PIT. QEMU xAPIC target. */
    if (!timer_counts_per_second) {
        uint8_t speaker = inb(0x61);
        outb(0x61, speaker & ~3U);
        outb(0x43, 0xb0); /* channel 2, low/high bytes, one-shot */
        outb(0x42, 11932 & 255);
        outb(0x42, 11932 >> 8);
        lapic_write(LAPIC_LVT_TIMER, TIMER_MASKED);
        lapic_write(LAPIC_TIMER_ICR, UINT32_MAX);
        outb(0x61, (speaker & ~2U) | 1);
        uint32_t limit = 100000000;
        while (!(inb(0x61) & 0x20) && --limit) asm volatile("pause");
        uint64_t elapsed = UINT32_MAX - lapic_read(LAPIC_TIMER_CCR);
        outb(0x61, speaker);
        uint64_t rate = elapsed * 1193182ULL / 11932;
        if (!limit || !rate || rate > UINT32_MAX) {
            serial_write_string("[FATAL] LAPIC timer calibration failed\n");
            asm volatile("cli");
            for (;;) asm volatile("hlt");
        }
        timer_counts_per_second = (uint32_t)rate;
    }
    uint32_t count = timer_counts_per_second / hz;
    if (!count) count = 1;

    /* Set periodic timer with our vector */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, count);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    while (lapic_read(LAPIC_ICR_LO) & (1U << 12)) asm volatile("pause");
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, vector);
    if (flags & 512) asm volatile("sti" ::: "memory");
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

uint64_t lapic_get_base_virt(void) {
    return lapic_base_virt;
}
