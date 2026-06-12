#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * GenOS Buffer Cache Subsystem
 *
 * Block-level cache between VFS and storage backend (TAR ramdisk).
 * Caches 4KB blocks of file data to avoid repeated TAR traversal.
 * Uses hash table for O(1) lookup + LRU doubly-linked list for eviction.
 *
 * Future-ready: when a disk driver is added, only the "fetch" path
 * changes (TAR → ATA/NVMe); the cache logic stays the same.
 */

#define CACHE_BLOCK_SIZE  4096   /* 4 KB per block (one page) */
#define CACHE_MAX_BLOCKS  128    /* Max cached blocks (~512 KB) */
#define CACHE_HASH_BUCKETS 64    /* Hash table size (power of 2) */
#define CACHE_NAME_MAX    64     /* Max filename length for tag */

/* Cache statistics for monitoring */
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint32_t used_blocks;
    uint32_t total_blocks;
} cache_stats_t;

/* Initialize cache subsystem (call once at boot, after heap_init) */
void cache_init(void);

/*
 * Read `count` bytes from file through the cache layer.
 *
 * @param name      Filename (used as cache key component)
 * @param src       Pointer to file data in ramdisk (for cache fill on miss)
 * @param file_size Total size of the file
 * @param offset    Read offset within the file
 * @param buf       Destination buffer
 * @param count     Bytes to read
 * @return          Bytes actually read, or -1 on error
 */
int cache_read(const char* name, const uint8_t* src, size_t file_size,
               size_t offset, void* buf, size_t count);

/* Invalidate (drop) all cached blocks for a specific file */
void cache_invalidate(const char* name);

/* Flush entire cache (drop all entries) */
void cache_flush_all(void);

/* Retrieve cache performance statistics */
cache_stats_t cache_get_stats(void);
