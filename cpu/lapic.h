#pragma once
#include <stdint.h>

#define LAPIC_TIMER_VECTOR 48  /* Dedicated vector for LAPIC timer */

void lapic_init(void);              /* Init LAPIC on current CPU */
void lapic_timer_init(uint32_t hz); /* Start periodic timer */
void lapic_eoi(void);               /* Send End-of-Interrupt */
uint32_t lapic_get_id(void);        /* Get current CPU's LAPIC ID */
uint64_t lapic_get_base_virt(void); /* Get LAPIC virtual address */
