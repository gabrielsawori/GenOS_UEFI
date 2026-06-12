/*
 * GenOS Buffer Cache Implementation
 *
 * Block-level cache with hash table lookup and LRU eviction.
 * Pool of 128 x 4KB blocks (~512 KB total) in static BSS.
 *
 * Flow:  VFS  →  cache_read()  →  [hit] return cached data
 *                                →  [miss] read from src, cache it, return
 */
#include "cache.h"
#include "../drivers/serial.h"
#include "../libc/string.h"

/* ─── Internal Structures ──────────────────────────────────────── */

typedef struct cache_entry {
    int      valid;                          /* Slot in use? */
    char     name[CACHE_NAME_MAX];           /* Filename tag */
    uint32_t block_index;                    /* Block number within file */
    uint8_t  data[CACHE_BLOCK_SIZE];         /* Cached block data */
    uint32_t bytes_valid;                    /* Valid bytes (last block partial) */

    /* LRU doubly-linked list */
    struct cache_entry* lru_prev;
    struct cache_entry* lru_next;

    /* Hash chain (separate chaining) */
    struct cache_entry* hash_next;
} cache_entry_t;

/* ─── Static Pool & Metadata ───────────────────────────────────── */

static cache_entry_t pool[CACHE_MAX_BLOCKS];
static cache_entry_t* hash_table[CACHE_HASH_BUCKETS];
static cache_entry_t* lru_head;   /* Most recently used */
static cache_entry_t* lru_tail;   /* Least recently used (evict candidate) */
static uint32_t free_hint;        /* Next pool index to try for allocation */

static cache_stats_t stats;

/* ─── Hash Function ────────────────────────────────────────────── */

static uint32_t cache_hash(const char* name, uint32_t block_index) {
    uint32_t h = block_index * 2654435761u;   /* Knuth multiplicative hash */
    for (int i = 0; name[i] != '\0'; i++) {
        h = h * 31 + (uint8_t)name[i];
    }
    return h & (CACHE_HASH_BUCKETS - 1);       /* Mask to bucket range */
}

/* ─── LRU Operations ──────────────────────────────────────────── */

/* Remove entry from its current position in the LRU list */
static void lru_remove(cache_entry_t* e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else             lru_head = e->lru_next;

    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else             lru_tail = e->lru_prev;

    e->lru_prev = NULL;
    e->lru_next = NULL;
}

/* Push entry to the head (most recently used) */
static void lru_push_head(cache_entry_t* e) {
    e->lru_prev = NULL;
    e->lru_next = lru_head;
    if (lru_head) lru_head->lru_prev = e;
    lru_head = e;
    if (!lru_tail) lru_tail = e;
}

/* Promote entry: remove from current position, push to head */
static void lru_promote(cache_entry_t* e) {
    lru_remove(e);
    lru_push_head(e);
}

/* ─── Hash Table Operations ────────────────────────────────────── */

/* Remove entry from its hash chain */
static void hash_remove(cache_entry_t* e) {
    uint32_t bucket = cache_hash(e->name, e->block_index);
    cache_entry_t** pp = &hash_table[bucket];
    while (*pp) {
        if (*pp == e) {
            *pp = e->hash_next;
            e->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/* Insert entry into hash table */
static void hash_insert(cache_entry_t* e) {
    uint32_t bucket = cache_hash(e->name, e->block_index);
    e->hash_next = hash_table[bucket];
    hash_table[bucket] = e;
}

/* Lookup entry by name + block_index; returns NULL if not found */
static cache_entry_t* hash_lookup(const char* name, uint32_t block_index) {
    uint32_t bucket = cache_hash(name, block_index);
    cache_entry_t* e = hash_table[bucket];
    while (e) {
        if (e->block_index == block_index && strcmp(e->name, name) == 0) {
            return e;
        }
        e = e->hash_next;
    }
    return NULL;
}

/* ─── Block Allocation & Eviction ──────────────────────────────── */

/* Find a free pool slot; returns NULL if all slots are used */
static cache_entry_t* alloc_block(void) {
    /* Try from free_hint onwards (fast path) */
    for (uint32_t i = 0; i < CACHE_MAX_BLOCKS; i++) {
        uint32_t idx = (free_hint + i) % CACHE_MAX_BLOCKS;
        if (!pool[idx].valid) {
            free_hint = (idx + 1) % CACHE_MAX_BLOCKS;
            pool[idx].valid = 1;
            stats.used_blocks++;
            return &pool[idx];
        }
    }
    return NULL;  /* Pool exhausted */
}

/* Evict the LRU tail entry; returns the recycled slot */
static cache_entry_t* evict_lru(void) {
    if (!lru_tail) return NULL;

    cache_entry_t* victim = lru_tail;

    /* Remove from hash chain and LRU list */
    hash_remove(victim);
    lru_remove(victim);

    /* Clear metadata (data buffer will be overwritten) */
    victim->valid       = 1;   /* Still allocated, just recycled */
    victim->hash_next   = NULL;
    victim->lru_prev    = NULL;
    victim->lru_next    = NULL;

    stats.evictions++;
    return victim;
}

/* Get or create a cache block for (name, block_index) */
static cache_entry_t* cache_get_block(const char* name, uint32_t block_index) {
    /* Fast path: cache hit */
    cache_entry_t* e = hash_lookup(name, block_index);
    if (e) {
        stats.hits++;
        lru_promote(e);
        return e;
    }

    /* Cache miss */
    stats.misses++;

    /* Try to allocate a free slot */
    e = alloc_block();
    if (!e) {
        /* Pool full → evict LRU tail */
        e = evict_lru();
        if (!e) return NULL;  /* Should never happen if MAX_BLOCKS > 0 */
    }

    /* Initialize the new entry */
    e->block_index = block_index;
    e->bytes_valid = 0;
    e->hash_next   = NULL;
    e->lru_prev    = NULL;
    e->lru_next    = NULL;

    /* Copy filename tag */
    int i;
    for (i = 0; i < CACHE_NAME_MAX - 1 && name[i] != '\0'; i++) {
        e->name[i] = name[i];
    }
    e->name[i] = '\0';

    /* Insert into hash table and LRU head */
    hash_insert(e);
    lru_push_head(e);

    return e;
}

/* ─── Public API ───────────────────────────────────────────────── */

void cache_init(void) {
    /* Zero out the pool and hash table (BSS is already zero, but be explicit) */
    for (uint32_t i = 0; i < CACHE_MAX_BLOCKS; i++) {
        pool[i].valid      = 0;
        pool[i].hash_next  = NULL;
        pool[i].lru_prev   = NULL;
        pool[i].lru_next   = NULL;
    }
    for (uint32_t i = 0; i < CACHE_HASH_BUCKETS; i++) {
        hash_table[i] = NULL;
    }

    lru_head  = NULL;
    lru_tail  = NULL;
    free_hint = 0;

    stats.hits       = 0;
    stats.misses     = 0;
    stats.evictions  = 0;
    stats.used_blocks = 0;
    stats.total_blocks = CACHE_MAX_BLOCKS;

    serial_write_string("[OK] Buffer Cache initialized (128 blocks x 4KB = 512KB).\n");
}

int cache_read(const char* name, const uint8_t* src, size_t file_size,
               size_t offset, void* buf, size_t count) {
    if (!name || !src || !buf) return -1;
    if (offset >= file_size) return 0;

    /* Clamp to remaining bytes */
    size_t remaining = file_size - offset;
    if (count > remaining) count = remaining;

    uint8_t* dst = (uint8_t*)buf;
    size_t bytes_done = 0;

    while (bytes_done < count) {
        /* Which block does the current offset fall into? */
        size_t   cur_offset  = offset + bytes_done;
        uint32_t block_idx   = (uint32_t)(cur_offset / CACHE_BLOCK_SIZE);
        size_t   block_off   = cur_offset % CACHE_BLOCK_SIZE;

        /* How many bytes to copy from this block? */
        size_t chunk = CACHE_BLOCK_SIZE - block_off;
        if (chunk > count - bytes_done) chunk = count - bytes_done;

        /* Get or create cache entry */
        cache_entry_t* e = cache_get_block(name, block_idx);
        if (!e) return -1;  /* Allocation failure */

        /* If block is newly allocated (miss), fill it from source */
        if (e->bytes_valid == 0) {
            size_t file_off    = (size_t)block_idx * CACHE_BLOCK_SIZE;
            size_t avail       = (file_off < file_size) ? (file_size - file_off) : 0;
            size_t to_fill     = (avail < CACHE_BLOCK_SIZE) ? avail : CACHE_BLOCK_SIZE;

            if (to_fill > 0) {
                for (size_t i = 0; i < to_fill; i++) {
                    e->data[i] = src[file_off + i];
                }
            }
            e->bytes_valid = (uint32_t)to_fill;
        }

        /* Clamp chunk to valid bytes in block */
        if (block_off >= e->bytes_valid) break;  /* Past end of file data */
        if (block_off + chunk > e->bytes_valid) {
            chunk = e->bytes_valid - block_off;
        }

        /* Copy from cache to caller buffer */
        for (size_t i = 0; i < chunk; i++) {
            dst[bytes_done + i] = e->data[block_off + i];
        }

        bytes_done += chunk;
    }

    return (int)bytes_done;
}

void cache_invalidate(const char* name) {
    if (!name) return;

    /* Scan all hash buckets for entries matching this filename */
    for (uint32_t b = 0; b < CACHE_HASH_BUCKETS; b++) {
        cache_entry_t** pp = &hash_table[b];
        while (*pp) {
            cache_entry_t* e = *pp;
            if (strcmp(e->name, name) == 0) {
                /* Remove from hash chain */
                *pp = e->hash_next;

                /* Remove from LRU */
                lru_remove(e);

                /* Mark slot as free */
                e->valid     = 0;
                e->hash_next = NULL;
                stats.used_blocks--;
            } else {
                pp = &e->hash_next;
            }
        }
    }
}

void cache_flush_all(void) {
    for (uint32_t i = 0; i < CACHE_MAX_BLOCKS; i++) {
        pool[i].valid     = 0;
        pool[i].hash_next = NULL;
        pool[i].lru_prev  = NULL;
        pool[i].lru_next  = NULL;
    }
    for (uint32_t b = 0; b < CACHE_HASH_BUCKETS; b++) {
        hash_table[b] = NULL;
    }

    lru_head  = NULL;
    lru_tail  = NULL;
    free_hint = 0;
    stats.used_blocks = 0;
}

cache_stats_t cache_get_stats(void) {
    return stats;
}
