#pragma once
#include <stdint.h>

void timer_init(uint32_t frequency);
void timer_tick(void);
uint64_t timer_get_ticks(void);
void sleep(uint32_t ms);