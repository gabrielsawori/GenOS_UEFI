/*
 * GenOS tmpfs Implementation
 *
 * In-memory writeable filesystem using kernel heap.
 * Each file gets a kmalloc'd buffer that grows via krealloc on write.
 */
#include "tmpfs.h"
#include "../mm/heap.h"
#include "../drivers/serial.h"
#include "../libc/string.h"

#define TMPFS_INITIAL_CAP  4096  /* Initial allocation per file */

static tmpfs_file_t files[TMPFS_MAX_FILES];

void tmpfs_init(void) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        files[i].in_use   = 0;
        files[i].data     = NULL;
        files[i].size     = 0;
        files[i].capacity = 0;
    }
    serial_write_string("[OK] tmpfs initialized (32 files max, 64KB each).\n");
}

int tmpfs_open(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (files[i].in_use && strcmp(files[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int tmpfs_create(const char* name) {
    if (!name) return -1;

    /* Check if file already exists */
    int existing = tmpfs_open(name);
    if (existing >= 0) return existing;

    /* Find free slot */
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!files[i].in_use) {
            /* Allocate initial buffer */
            uint8_t* buf = (uint8_t*)kmalloc(TMPFS_INITIAL_CAP);
            if (!buf) return -1;

            /* Zero the buffer */
            for (size_t b = 0; b < TMPFS_INITIAL_CAP; b++) buf[b] = 0;

            /* Copy filename */
            int j;
            for (j = 0; j < 99 && name[j] != '\0'; j++) {
                files[i].name[j] = name[j];
            }
            files[i].name[j] = '\0';

            files[i].data     = buf;
            files[i].size     = 0;
            files[i].capacity = TMPFS_INITIAL_CAP;
            files[i].in_use   = 1;
            return i;
        }
    }
    return -1; /* No free slots */
}

int tmpfs_write(int index, size_t offset, const void* buf, size_t count) {
    if (index < 0 || index >= TMPFS_MAX_FILES) return -1;
    if (!files[index].in_use || !buf) return -1;

    tmpfs_file_t* f = &files[index];

    /* Calculate required size */
    size_t end_pos = offset + count;
    if (end_pos > TMPFS_MAX_SIZE) {
        count = (offset < TMPFS_MAX_SIZE) ? (TMPFS_MAX_SIZE - offset) : 0;
        end_pos = offset + count;
    }
    if (count == 0) return 0;

    /* Grow buffer if needed */
    if (end_pos > f->capacity) {
        size_t new_cap = f->capacity;
        while (new_cap < end_pos) {
            new_cap *= 2;
            if (new_cap > TMPFS_MAX_SIZE) new_cap = TMPFS_MAX_SIZE;
        }
        uint8_t* new_buf = (uint8_t*)krealloc(f->data, new_cap);
        if (!new_buf) return -1;

        /* Zero newly allocated region */
        for (size_t b = f->capacity; b < new_cap; b++) new_buf[b] = 0;

        f->data     = new_buf;
        f->capacity = new_cap;
    }

    /* Copy data */
    const uint8_t* src = (const uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        f->data[offset + i] = src[i];
    }

    /* Update size if we wrote past the current end */
    if (end_pos > f->size) {
        f->size = end_pos;
    }

    return (int)count;
}

int tmpfs_read(int index, size_t offset, void* buf, size_t count) {
    if (index < 0 || index >= TMPFS_MAX_FILES) return -1;
    if (!files[index].in_use || !buf) return -1;

    tmpfs_file_t* f = &files[index];
    if (offset >= f->size) return 0;

    /* Clamp to remaining bytes */
    size_t remaining = f->size - offset;
    if (count > remaining) count = remaining;

    /* Copy data */
    uint8_t* dst = (uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        dst[i] = f->data[offset + i];
    }

    return (int)count;
}

int tmpfs_unlink(const char* name) {
    if (!name) return -1;

    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (files[i].in_use && strcmp(files[i].name, name) == 0) {
            /* Free buffer */
            if (files[i].data) {
                kfree(files[i].data);
            }
            files[i].data     = NULL;
            files[i].size     = 0;
            files[i].capacity = 0;
            files[i].in_use   = 0;
            return 0;
        }
    }
    return -1; /* Not found */
}

int tmpfs_get_entry(int index, char* name_buf, size_t* size_buf) {
    if (index < 0 || index >= TMPFS_MAX_FILES) return 0;
    if (!files[index].in_use) return 0;

    /* Copy name */
    int i;
    for (i = 0; i < 99 && files[index].name[i] != '\0'; i++) {
        name_buf[i] = files[index].name[i];
    }
    name_buf[i] = '\0';
    if (size_buf) *size_buf = files[index].size;
    return 1;
}

int tmpfs_file_count(void) {
    int count = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (files[i].in_use) count++;
    }
    return count;
}
