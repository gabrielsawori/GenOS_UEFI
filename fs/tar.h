#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include <stddef.h>

void tar_init(void *address);
char *tar_read_file(const char *filename, size_t *size);
void tar_list_files(void (*callback)(const char *filename, size_t size));

/*
 * tar_get_entry() - Get TAR entry by index (for VFS readdir).
 * Returns 1 if entry at `index` exists, 0 if past end of archive.
 * Copies entry name to name_buf and size to *size_buf.
 */
int tar_get_entry(int index, char *name_buf, size_t *size_buf);

#endif