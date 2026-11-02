/* Freestanding Ring 3 probe. No test-only syscall performs the user copies. */
#include <stdint.h>
#include <stddef.h>
#include "../libc/stdio.h"

static long call(unsigned n, uint64_t a, uint64_t b, uint64_t c) {
    long result;
    asm volatile("syscall" : "=a"(result) : "a"((uint64_t)n), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return result;
}
#define U(p) ((uint64_t)(p))
#define CHECK(test) do { if (!(test)) { call(0x7fff, 0, __LINE__, 0); for (;;) {} } } while (0)
#define BAD 0x60000000ULL
static uint8_t buffer[8192] __attribute__((aligned(4096)));
static volatile uint32_t cow_output[2] __attribute__((aligned(4096))) = {11, 12};
static const uint8_t readonly[128] __attribute__((aligned(4096))) = {0x55};
static const char filename[] = "uaccess-test";
static const char abc[] = "abc";
static const uint8_t abc_hash[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
};
struct aes_params {
    const void* key; const void* iv; const void* input; size_t in_len;
    void* output; size_t out_max;
};
struct store_params {
    int op; const char* name; void* value; size_t val_len; int* out_len;
};

__attribute__((noreturn)) void user_main(void) {
    const uint64_t invalid[] = {0, BAD, 0x800000000000ULL, 0xffff800000000000ULL,
                               0x7ffffffffffcULL};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint64_t p = invalid[i];
        CHECK(call(1, p, 0, 0) == -1);
        CHECK(call(6, p, 0, 0) == -1);
        CHECK(call(8, p, U(buffer), 16) == -1);
        CHECK(call(9, p, 0, 0) == -1);
        CHECK(call(12, p, 0, 0) == -1);
        CHECK(call(21, p, 0, 0) == -1);
        CHECK(call(24, p, 0, 0) == -1);
        CHECK(call(25, p, 0, 0) == -1);
        CHECK(call(27, 0, p, 0) == -1);
        CHECK(call(31, p, 0, 0) == -1);
        CHECK(call(37, p, 16, 0) == -1);
        CHECK(call(38, p, 16, U(buffer)) == -1);
        CHECK(call(39, p, 0, 0) == -1);
        CHECK(call(40, p, 0, 0) == -1);
        CHECK(call(41, p, U(abc), 0) == -1);
        CHECK(call(41, U(abc), p, 0) == -1);
        CHECK(call(44, p, 0, 0) == -1);
    }
    CHECK(call(31, U(readonly), 0, 0) == -1 && readonly[0] == 0x55);
    CHECK(call(21, U(readonly), 0, 0) == -1);
    CHECK(call(27, 0, U(readonly), 0) == -1);
    CHECK(call(42, BAD, 32, 0) == -1);
    CHECK(call(31, U(buffer), 0, 0) == 0 && *(uint32_t*)buffer != 0);
    CHECK(call(27, 0, U(buffer), 0) == 0);
    CHECK(call(21, U(buffer), 0, 0) == 0);
    CHECK(call(42, U(buffer), 32, 0) >= 0);
    CHECK(call(8, U("missing-file"), U(buffer), 32) == 0);

    /* Place a terminator at the very end of a mapping, then remove it. */
    long id = call(17, 4096, 0, 0);
    CHECK(id >= 0);
    char* page = (char*)call(18, id, 0, 0);
    CHECK(page != NULL);
    page[4094] = '/'; page[4095] = 0;
    long dir = call(12, U(page + 4094), 0, 0);
    CHECK(dir >= 0);
    CHECK(call(14, dir, 0, 0) == 0);
    page[4095] = 'x';
    CHECK(call(12, U(page + 4094), 0, 0) == -1);
    CHECK(call(31, U(page + 4092), 0, 0) == -1 && page[4094] == '/');
    CHECK(call(19, U(page), 0, 0) == 0);
    CHECK(call(31, U(page), 0, 0) == -1);

    CHECK(call(24, U(filename), 0, 0) == 0);
    long fd = call(12, U(filename), 2, 0);
    CHECK(fd >= 0);
    for (unsigned i = 0; i < 3000; i++) buffer[i] = (uint8_t)i;
    CHECK(call(23, fd, U(buffer), 3000) == 3000);
    CHECK(call(15, fd, 0, 0) == 0);
    CHECK(call(13, fd, U(readonly), 16) == -1);
    CHECK(call(13, fd, BAD, 16) == -1);
    CHECK(call(23, fd, BAD, 16) == -1);
    CHECK(call(23, fd, U(buffer), UINT64_MAX) == -1);
    CHECK(call(15, fd, 0, 1) == 0);
    CHECK(call(13, fd, U(buffer + 4096), 3000) == 3000);
    for (unsigned i = 0; i < 3000; i++) CHECK(buffer[i + 4096] == (uint8_t)i);
    CHECK(call(13, fd, 0, 0) == 0 && call(23, fd, 0, 0) == 0);
    CHECK(call(14, fd, 0, 0) == 0);
    CHECK(call(25, U(filename), 0, 0) == 0);
    dir = call(12, U("/"), 0, 0);
    size_t file_size = 0;
    CHECK(dir >= 0);
    CHECK(call(16, dir, U(buffer), BAD) == -1);
    CHECK(call(16, dir, BAD, U(&file_size)) == -1);
    CHECK(call(16, dir, U(buffer), U(&file_size)) == 1 && buffer[0] == 's');
    CHECK(call(14, dir, 0, 0) == 0);

    dir = call(12, U("/"), 0, 0);
    struct { int size; volatile uint32_t canary; } entry = {-1, 0x12345678};
    CHECK(dir >= 0 && readdir((int)dir, (char*)buffer, &entry.size) == 1);
    CHECK(entry.size == (int)file_size && entry.canary == 0x12345678);
    CHECK(readdir(-1, (char*)buffer, &entry.size) == -1 && entry.canary == 0x12345678);
    CHECK(call(14, dir, 0, 0) == 0);

    CHECK(call(37, U(buffer), 32, 0) == 0);
    CHECK(call(37, U(readonly), 32, 0) == -1);
    CHECK(call(38, U(abc), 3, U(readonly)) == -1);
    CHECK(call(38, U(abc), 3, U(buffer)) == 0);
    for (unsigned i = 0; i < 32; i++) CHECK(buffer[i] == abc_hash[i]);
    uint8_t key[32] = {0}, iv[16] = {0};
    struct aes_params aes = {key, iv, abc, 3, buffer, 32};
    CHECK(call(39, U(&aes), 0, 0) == 16);
    aes.input = buffer; aes.in_len = 16; aes.output = buffer + 32;
    CHECK(call(40, U(&aes), 0, 0) == 3 && buffer[32] == 'a' && buffer[34] == 'c');
    aes.key = (void*)BAD;
    CHECK(call(39, U(&aes), 0, 0) == -1);
    aes.key = key; aes.iv = (void*)BAD;
    CHECK(call(40, U(&aes), 0, 0) == -1);
    aes.iv = iv; aes.input = (void*)BAD;
    CHECK(call(39, U(&aes), 0, 0) == -1);
    aes.input = abc; aes.in_len = 3; aes.output = (void*)readonly;
    CHECK(call(39, U(&aes), 0, 0) == -1);
    aes.output = buffer; aes.in_len = SIZE_MAX;
    CHECK(call(39, U(&aes), 0, 0) == -1);

    int out_len = 0;
    struct store_params store = {4, "test-passphrase", (void*)abc, 3, &out_len};
    CHECK(call(44, U(&store), 0, 0) == 0);
    store.op = 0; store.name = "test-key";
    CHECK(call(44, U(&store), 0, 0) == 0);
    store.op = 1; store.value = buffer; store.val_len = 32; store.out_len = (void*)BAD;
    CHECK(call(44, U(&store), 0, 0) == -1);
    store.out_len = &out_len;
    CHECK(call(44, U(&store), 0, 0) == 0 && out_len == 3 && buffer[0] == 'a');
    store.op = 0; store.value = (void*)BAD;
    CHECK(call(44, U(&store), 0, 0) == -1);
    store.op = 4; store.name = (void*)BAD;
    CHECK(call(44, U(&store), 0, 0) == -1);
    store.op = 2; store.name = "test-key";
    CHECK(call(44, U(&store), 0, 0) == 0);

    long child = call(22, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        CHECK(call(0x7ffd, U(cow_output), 0, 0) == -1 && cow_output[0] == 11);
        CHECK(call(31, U(cow_output), 0, 0) == 0 && cow_output[0] != 11);
        call(0x7fff, 2, 0, 0);
    } else {
        CHECK(call(11, child, 0, 0) == 1 && cow_output[0] == 11);
        call(0x7fff, 1, 0, 0);
    }
    call(2, 0, 0, 0);
    for (;;) {}
}

/* Kernel starts ELF at an aligned stack without a caller's return address. */
asm(".global _start\n_start:\ncall user_main\nud2\n");
