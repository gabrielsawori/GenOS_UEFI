#pragma once
#include <stdint.h>
#include <stddef.h>

void heap_init(void);

// Meminta potongan memori sebesar "size" byte
void* kmalloc(size_t size);

// Mengembalikan potongan memori yang sudah selesai dipakai
void kfree(void* ptr);

// Mengubah ukuran potongan memori (resize) — bisa membesar atau mengecil
void* krealloc(void* ptr, size_t new_size);