#pragma once
#include "../include/genos/input.h"
/* Single producer: BSP PS/2 IRQs, IF clear. Readers hold PREEMPT_GUARD. */
void input_push(const struct genos_input_event* event);
int input_peek(struct genos_input_event* event);
void input_consume(void);
uint64_t input_dropped(void);
