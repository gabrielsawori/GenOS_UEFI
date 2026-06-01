#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include <stddef.h>

void tar_init(void *address);
char *tar_read_file(const char *filename, size_t *size);

#endif
