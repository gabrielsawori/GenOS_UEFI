#include "keyboard.h"
#include "serial.h"
#include <stdbool.h>

// --- SISTEM RING BUFFER (Ember Antrean 256 Huruf) ---
#define BUFFER_SIZE 256
static char key_buffer[BUFFER_SIZE];
static volatile int buffer_head = 0; // Penunjuk tempat huruf masuk
static volatile int buffer_tail = 0; // Penunjuk tempat huruf diambil

// Fungsi rahasia memasukkan huruf ke ember
// FIX BUG #3: Tambahkan atomic operation untuk mencegah race condition
// FIX: Removed cli/sti — the interrupt gate (IDT type 0x8E) already clears IF
// on entry, so interrupts are disabled throughout the IRQ handler.
// Re-enabling IF with sti here would allow nested interrupts (e.g. timer
// firing during keyboard processing), causing stack corruption on bare metal.
static void buffer_push(char c) {
    int next_head = (buffer_head + 1) % BUFFER_SIZE;
    if (next_head != buffer_tail) { // Jika ember tidak penuh
        key_buffer[buffer_head] = c;
        buffer_head = next_head;
    }
}

// Fungsi yang akan dipanggil oleh Terminal untuk mengambil huruf
char keyboard_get_char(void) {
    if (buffer_head == buffer_tail) return 0; // Jika ember kosong, kembalikan 0
    char c = key_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    return c;
}

// --- KAMUS SCANCODE (Persis seperti yang kita buat sebelumnya) ---
const char scancode_to_ascii_base[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

const char scancode_to_ascii_shift[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

static bool shift_pressed = false;
static bool caps_lock_active = false;
static bool extended_key = false;  /* PS/2 extended scancode (0xE0 prefix) */

/*
 * Special key codes for non-ASCII keys (pushed to key buffer).
 * Desktop reads these to move cursor with arrow keys.
 */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83

// Fungsi yang terpicu setiap jari menekan tuts keyboard
void keyboard_handler(uint8_t scancode) {
    /*
     * PS/2 Extended Scancodes: Arrow keys, Home, End, etc.
     * send a 0xE0 prefix byte FIRST, then the actual scancode.
     * We set a flag and return; the NEXT IRQ delivers the real key.
     */
    if (scancode == 0xE0) {
        extended_key = true;
        return;
    }

    if (extended_key) {
        extended_key = false;
        /* Ignore key-up events for extended keys */
        if (scancode & 0x80) return;
        /* Map extended scancodes to special chars */
        switch (scancode) {
            case 0x48: buffer_push(KEY_UP);    return;
            case 0x50: buffer_push(KEY_DOWN);  return;
            case 0x4B: buffer_push(KEY_LEFT);  return;
            case 0x4D: buffer_push(KEY_RIGHT); return;
            default: return; /* Ignore other extended keys */
        }
    }

    // 1. Cek Key Up (Tombol Dilepas)
    if (scancode & 0x80) {
        uint8_t base_scancode = scancode & 0x7F;
        if (base_scancode == 0x2A || base_scancode == 0x36) shift_pressed = false;
        return;
    }

    // 2. Cek Modifier
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = true; return; }
    if (scancode == 0x3A) { caps_lock_active = !caps_lock_active; return; }

    // 3. Terjemahkan
    if (scancode < sizeof(scancode_to_ascii_base)) {
        char base_c = scancode_to_ascii_base[scancode];
        char shift_c = scancode_to_ascii_shift[scancode];
        char final_c = 0;

        if (base_c >= 'a' && base_c <= 'z') {
            bool uppercase = shift_pressed != caps_lock_active;
            final_c = uppercase ? shift_c : base_c;
        } else {
            final_c = shift_pressed ? shift_c : base_c;
        }

        // 4. MASUKKAN KE EMBER ANTREAN
        if (final_c != 0) {
            buffer_push(final_c);
        }
    }
}
