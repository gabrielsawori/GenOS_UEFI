#pragma once
#include <stdint.h>
#include <stddef.h>

/* User-facing adapters. Subsystems receive kernel-owned buffers only. */
int sys_user_read(int fd, void* buf, size_t count);
int sys_user_write(int fd, const void* buf, size_t count);
int sys_user_readdir(int fd, char* name, size_t* size);
int sys_user_random(void* buf, size_t len);
int sys_user_sha256(const void* data, size_t len, void* digest);
int sys_user_aes(const void* params, int decrypt);
int sys_user_keystore(const void* params);
