#include "tar.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"

// Basic TAR header structure
struct tar_header {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
};

static uint8_t *tar_archive = NULL;

void tar_init(void *address) {
    tar_archive = (uint8_t *)address;
    serial_write_string("[OK] TAR archive initialized from RAM disk module.\n");
}

// Convert octal string to integer
static size_t octal_to_int(const char *str, size_t size) {
    size_t result = 0;
    for (size_t i = 0; i < size && str[i] != 0 && str[i] != ' '; i++) {
        result = result * 8 + (str[i] - '0');
    }
    return result;
}

char *tar_read_file(const char *filename, size_t *size) {
    if (!tar_archive) return NULL;

    uint8_t *ptr = tar_archive;
    while (1) {
        struct tar_header *header = (struct tar_header *)ptr;

        // If the filename is empty, we reached the end of the archive
        if (header->filename[0] == '\0') {
            break;
        }

        size_t file_size = octal_to_int(header->size, 11);

        if (strcmp(header->filename, filename) == 0) {
            if (size) *size = file_size;
            return (char *)(ptr + 512);
        }

        // Move to the next file in the archive (size + 512 for header, padded to 512 byte blocks)
        ptr += 512 + ((file_size + 511) / 512) * 512;
    }

    return NULL;
}

void tar_list_files(void (*callback)(const char *filename, size_t size)) {
    if (!tar_archive) return;
    
    uint8_t *ptr = tar_archive;
    while (1) {
        struct tar_header *header = (struct tar_header *)ptr;
        if (header->filename[0] == '\0') break;
        
        size_t file_size = octal_to_int(header->size, 11);
        
        if (callback) callback(header->filename, file_size);
        
        ptr += 512 + ((file_size + 511) / 512) * 512;
    }
}

/*
 * tar_get_entry() - Iterate TAR entries by index for VFS readdir.
 *
 * Walks the archive from the start, skipping `index` entries.
 * Returns 1 if the entry exists (fills name_buf and *size_buf),
 * or 0 if past end of archive.
 */
int tar_get_entry(int index, char *name_buf, size_t *size_buf) {
    if (!tar_archive) return 0;

    uint8_t *ptr = tar_archive;
    int current = 0;

    while (1) {
        struct tar_header *header = (struct tar_header *)ptr;
        if (header->filename[0] == '\0') return 0; /* End of archive */

        size_t file_size = octal_to_int(header->size, 11);

        if (current == index) {
            /* Copy filename */
            int i;
            for (i = 0; i < 99 && header->filename[i] != '\0'; i++) {
                name_buf[i] = header->filename[i];
            }
            name_buf[i] = '\0';
            if (size_buf) *size_buf = file_size;
            return 1;
        }

        current++;
        ptr += 512 + ((file_size + 511) / 512) * 512;
    }
}