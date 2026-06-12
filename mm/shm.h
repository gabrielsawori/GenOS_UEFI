#pragma once
#include <stdint.h>
#include <stddef.h>

/* Forward declaration for task struct (defined in kernel/task.h) */
struct task;

/*
 * GenOS Shared Memory (SHM) Manager
 *
 * Kernel-managed shared memory segments for IPC between processes.
 * A process creates a segment (gets an ID), then other processes
 * attach the same segment to their address space. Both processes
 * map the same physical pages, so writes are immediately visible.
 *
 * Limits:
 *   SHM_MAX_SEGMENTS = 8 global segments
 *   SHM_MAX_PAGES    = 4 pages per segment (16KB max)
 *   SHM_MAX_ATTACH   = 4 segments per process
 */

#define SHM_MAX_SEGMENTS  8
#define SHM_MAX_PAGES     4
#define SHM_MAX_ATTACH    4

/*
 * Global shared memory segment descriptor.
 * Lives in kernel BSS; never exposed to user-space.
 */
typedef struct {
    int      in_use;
    int      shmid;
    uint64_t phys_pages[SHM_MAX_PAGES];  /* Physical page addresses */
    uint32_t page_count;                  /* Number of allocated pages */
    uint32_t size;                        /* Requested size in bytes */
    uint32_t refcount;                    /* Number of attached processes */
    uint32_t creator_pid;                 /* PID of the creator */
} shm_segment_t;

/* Per-task SHM attachment record (embedded in struct task) */
typedef struct {
    int      shmid;       /* Segment ID (-1 = unused slot) */
    uint64_t vaddr;       /* Virtual address in this process */
    uint32_t page_count;  /* Number of pages mapped */
} shm_attach_record_t;

/* Initialize SHM subsystem (call once at boot) */
void shm_init(void);

/* Create a new shared memory segment; returns shmid (>=0) or -1 */
int shm_create(uint32_t pid, uint32_t size);

/*
 * Attach a segment to a process's address space.
 * Maps physical pages into pml4 at a kernel-assigned VA.
 * Records the attachment in task->shm_attached[].
 * Returns virtual address (>0) or 0 on error.
 */
uint64_t shm_attach(int shmid, uint32_t pid, uint64_t* pml4);

/*
 * Detach a shared memory region from a process.
 * Unmaps pages from pml4, decrements refcount.
 * If refcount reaches 0, frees all physical pages.
 * Returns 0 on success, -1 on error.
 */
int shm_detach(uint64_t addr, uint32_t pid, uint64_t* pml4);

/*
 * Destroy a segment by shmid. Frees physical pages if refcount is 0.
 * Returns 0 on success, -1 if segment not found or still attached.
 */
int shm_destroy(int shmid);

/*
 * Cleanup all SHM attachments for a dying process.
 * Called by the reaper in schedule() before destroying the address space.
 * Uses the task's shm_attached[] to find and detach all segments.
 */
void shm_cleanup_process(struct task* t);
