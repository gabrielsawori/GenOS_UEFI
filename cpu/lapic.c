#include "lapic.h"
#include "../limine.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"

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
 * Uses a rough calibration: on QEMU, bus freq ~1GHz, div 16 = ~62.5MHz.
 * Count = freq / hz. For 1000Hz: ~62500.
 * Real hardware would need PIT-based calibration.
 */
void lapic_timer_init(uint32_t hz) {
    /* Divide by 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /*
     * Calibrate: measure LAPIC ticks per ~1ms using a busy-wait.
     * Set a large initial count, wait ~1ms via port 0x80 delay, read remainder.
     */
    lapic_write(LAPIC_LVT_TIMER, TIMER_MASKED); /* Mask during calibration */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);    /* Max count */

    /* Wait ~1ms using PIT channel 2 one-shot (1193 ticks = ~1ms) */
    /* Simple busy-wait: ~1ms at typical CPU speeds */
    for (volatile int i = 0; i < 100000; i++) {
        asm volatile("pause");
    }

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    uint32_t ticks_per_ms = elapsed; /* Approximate ticks in ~1ms */
    
    /* Sanity: if calibration seems off, use default */
    if (ticks_per_ms < 1000) ticks_per_ms = 62500;
    
    uint32_t count = (ticks_per_ms * 1000) / hz; /* Scale to desired Hz */
    if (count < 100) count = 62500; /* Safety floor */

    /* Set periodic timer with our vector */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, count);
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
