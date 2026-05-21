#pragma once
#include <stdint.h>
#include <stddef.h>

void heap_init(void);

// Meminta potongan memori sebesar "size" byte
void* kmalloc(size_t size);

// Mengembalikan potongan memori yang sudah selesai dipakai
void kfree(void* ptr);