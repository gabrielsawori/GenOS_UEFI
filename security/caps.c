#include "caps.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"

/*
 * Capability-Based Access Control — Implementation
 *
 * Sistem capability ini ringan (hanya bitmask integer) tapi efektif
 * untuk membatasi apa yang bisa dilakukan proses Ring 3.
 */

void caps_init(void) {
    serial_write_string("[INFO] Capability system initialized.\n");
    serial_write_string("[INFO]   Shell/init: CAP_ALL (0x7FF)\n");
    serial_write_string("[INFO]   Default:    CAP_DEFAULT (FS+EXEC+CRYPTO+SHM+DISPLAY+PROC)\n");
}

int caps_check(uint32_t task_caps, uint32_t required) {
    /*
     * Cek bahwa SEMUA bit yang diminta ada di task_caps.
     * Misalnya: required = CAP_FS_WRITE | CAP_EXEC
     *           task_caps harus memiliki KEDUA bit tersebut.
     */
    return (task_caps & required) == required;
}

void caps_grant(uint32_t *current_caps, uint32_t new_cap) {
    *current_caps |= new_cap;
}

void caps_revoke(uint32_t *current_caps, uint32_t remove_cap) {
    *current_caps &= ~remove_cap;
}

uint32_t caps_inherit(uint32_t parent_caps, uint32_t mask) {
    /*
     * Child mendapat intersection dari parent caps dan mask.
     * Ini memastikan child tidak bisa memiliki lebih banyak
     * capabilities dari parent-nya.
     */
    return parent_caps & mask;
}

/*
 * Helper: append string to buffer with bounds tracking.
 */
static int sappend(char *buf, int pos, const char *str) {
    while (*str) {
        buf[pos++] = *str++;
    }
    return pos;
}

void caps_to_string(uint32_t caps, char *buf) {
    int pos = 0;
    buf[0] = '\0';

    if (caps == 0) {
        pos = sappend(buf, pos, "NONE");
        buf[pos] = '\0';
        return;
    }

    if (caps == CAP_ALL) {
        pos = sappend(buf, pos, "ALL");
        buf[pos] = '\0';
        return;
    }

    int first = 1;
    struct { uint32_t bit; const char *name; } cap_names[] = {
        { CAP_FS_READ,    "FS_READ" },
        { CAP_FS_WRITE,   "FS_WRITE" },
        { CAP_EXEC,       "EXEC" },
        { CAP_CRYPTO,     "CRYPTO" },
        { CAP_SHM,        "SHM" },
        { CAP_NET,        "NET" },
        { CAP_DISPLAY,    "DISPLAY" },
        { CAP_PROC,       "PROC" },
        { CAP_IO,         "IO" },
        { CAP_ADMIN,      "ADMIN" },
        { CAP_RAW_DEVICE, "RAW_DEV" },
        { 0, 0 }
    };

    for (int i = 0; cap_names[i].name; i++) {
        if (caps & cap_names[i].bit) {
            if (!first) {
                pos = sappend(buf, pos, "|");
            }
            pos = sappend(buf, pos, cap_names[i].name);
            first = 0;
        }
    }

    buf[pos] = '\0';
}
