#pragma once
#include <stdint.h>

void idt_init(void);

/*
 * idt_set_descriptor() - Register one IDT entry with IST=0 (default).
 * Used for ordinary exceptions and IRQs that run on the current stack.
 */
void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags);

/*
 * idt_set_descriptor_ist() - Register one IDT entry with a custom IST.
 *
 * @param ist: IST index (1..7), or 0 for "no IST" (use current RSP).
 *
 * IST (Interrupt Stack Table) entries are pointed to by TSS.ist1..ist7.
 * When an exception with a non-zero IST fires, the CPU loads RSP from
 * that IST slot — IGNORING the current (possibly corrupted) RSP.
 *
 * This is essential for Double Fault (#DF, vector 8): if the kernel
 * stack is overflowed/corrupted, the CPU would fault again trying to
 * push the #DF frame, escalating to a Triple Fault (silent reboot).
 * With IST1, #DF lands on a known-good dedicated stack and we can
 * print diagnostics instead of rebooting silently.
 */
void idt_set_descriptor_ist(uint8_t vector, void* isr, uint8_t flags, uint8_t ist);

/*
 * idt_load_on_ap() - Load BSP's IDT on an Application Processor.
 * APs share the same interrupt descriptor table as BSP.
 */
void idt_load_on_ap(void);