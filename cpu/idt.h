#pragma once
#include <stdint.h>

void idt_init(void);

/*
 * idt_load_on_ap() - Load BSP's IDT on an Application Processor.
 * APs share the same interrupt descriptor table as BSP.
 */
void idt_load_on_ap(void);