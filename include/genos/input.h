#pragma once
#include <stdint.h>
#define GENOS_INPUT_KEY 1
#define GENOS_INPUT_MOUSE 2
struct genos_input_event {
    uint32_t type;
    uint32_t code; /* key: set-1 scancode, bit 8 = E0; mouse: button bitmask */
    int32_t value; /* key: 1 press, 0 release */
    int32_t dx, dy; /* mouse: relative movement, positive Y downward */
    uint32_t reserved;
};
