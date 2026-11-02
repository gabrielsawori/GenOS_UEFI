#include <assert.h>
#include <stdio.h>
#include "../drivers/input.h"
#include "../drivers/keyboard.h"
#include "../drivers/ps2.h"

static struct genos_input_event take(void) {
    struct genos_input_event e;
    assert(input_peek(&e) == 1);
    input_consume();
    return e;
}
int main(void) {
    struct genos_input_event e;
    assert(!input_peek(&e));
    keyboard_handler(0x1e); keyboard_handler(0x9e);
    e = take(); assert(e.type == GENOS_INPUT_KEY && e.code == 0x1e && e.value == 1);
    e = take(); assert(e.code == 0x1e && e.value == 0);
    assert(keyboard_get_char() == 'a' && !keyboard_get_char());
    keyboard_handler(0xe0); keyboard_handler(0x48);
    keyboard_handler(0xe0); keyboard_handler(0xc8);
    e = take(); assert(e.code == 0x148 && e.value == 1);
    e = take(); assert(e.code == 0x148 && !e.value);
    const unsigned char pause[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
    for (unsigned i = 0; i < sizeof(pause); i++) keyboard_handler(pause[i]);
    assert(!input_peek(&e));
    ps2_mouse_byte(0); /* Lost synchronization; discard until packet bit 3. */
    ps2_mouse_byte(0x39); ps2_mouse_byte(1); ps2_mouse_byte(255);
    e = take(); assert(e.type == GENOS_INPUT_MOUSE && e.code == 1 && e.dx == -255 && e.dy == 1);
    ps2_mouse_byte(0xc8); ps2_mouse_byte(100); ps2_mouse_byte(100);
    assert(!input_peek(&e)); /* Overflow discards whole packet. */
    ps2_mouse_byte(0x08); ps2_mouse_byte(255); ps2_mouse_byte(1);
    e = take(); assert(e.dx == 255 && e.dy == -1);
    for (unsigned i = 0; i < 300; i++) {
        e = (struct genos_input_event){.type = GENOS_INPUT_KEY, .code = i};
        input_push(&e);
    }
    assert(input_dropped() == 45);
    for (unsigned i = 0; i < 255; i++) { e = take(); assert(e.code == i); }
    assert(!input_peek(&e));
    for (unsigned i = 0; i < 1000; i++) {
        e.code = i; input_push(&e); e = take(); assert(e.code == i);
    }
    puts("Input PASS: key make/break, E0/E1, mouse sign/overflow/resync, queue wrap/full.");
}
