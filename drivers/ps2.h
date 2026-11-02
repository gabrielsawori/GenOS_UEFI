#pragma once
#include <stdint.h>
/* Boot before enabling IRQs. Absent/failed mouse is a nonfatal condition. */
void ps2_init(void);
void ps2_irq(void);
int ps2_mouse_present(void);
void ps2_mouse_byte(uint8_t data);
