/*
 * GenOS Shared Memory (SHM) Implementation
 *
 * Manages kernel-side shared memory segments for IPC.
 * Each segment owns one or more physical pages (via PMM).
 * shm_attach() maps those pages into a process's PML4 at a
 * kernel-assigned virtual address (bump allocator from 0x90000000).
 */
#include "shm.h"
#include "pmm.h"
#include "vmm.h"
#include "../kernel/task.h"
#include "../drivers/serial.h"

/* Global SHM segment table (kernel BSS) */
static shm_segment_t shm_segments[SHM_MAX_SEGMENTS];

/* VA bump allocator: each attach gets a unique VA range */
static uint64_t next_shm_vaddr = 0x90000000;

/* Auto-incrementing segment ID counter */
static int next_shmid = 1;

/* ─── Initialization ────────────────────────────────────────────── */

void shm_init(void) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_segments[i].in_use = 0;
    }
    serial_write_string("[OK] Shared Memory (SHM) manager initialized.\n");
}

/* ─── Create ───────────────────────────────────────────────────── */

int shm_create(uint32_t pid, uint32_t size) {
    if (size == 0 || size > SHM_MAX_PAGES * PAGE_SIZE) return -1;

    /* Calculate number of pages needed */
    uint32_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Find a free segment slot */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_segments[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1; /* All slots used */

    /* Allocate physical pages and zero them */
    shm_segments[slot].page_count = 0;
    for (uint32_t p = 0; p < pages_needed; p++) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) {
            /* Rollback: free pages already allocated */
            for (uint32_t r = 0; r < shm_segments[slot].page_count; r++) {
                pmm_free_page((void*)shm_segments[slot].phys_pages[r]);
            }
            return -1;
        }
        /* Zero the page via HHDM */
        uint64_t offset = 0xFFFF800000000000ULL; /* HHDM base */
        uint8_t* page_virt = (uint8_t*)(phys + offset);
        for (int b = 0; b < PAGE_SIZE; b++) page_virt[b] = 0;

        shm_segments[slot].phys_pages[p] = phys;
        shm_segments[slot].page_count++;
    }

    shm_segments[slot].in_use       = 1;
    shm_segments[slot].shmid        = next_shmid++;
    shm_segments[slot].size         = size;
    shm_segments[slot].refcount     = 0;
    shm_segments[slot].creator_pid  = pid;

    serial_write_string("[SHM] Created segment ");
    char buf[16];
    /* Simple itoa for shmid */
    int id = shm_segments[slot].shmid;
    int bi = 0;
    if (id == 0) { buf[bi++] = '0'; }
    else { while (id > 0) { buf[bi++] = '0' + (id % 10); id /= 10; } }
    /* Reverse */
    for (int i = 0; i < bi / 2; i++) {
        char tmp = buf[i]; buf[i] = buf[bi - 1 - i]; buf[bi - 1 - i] = tmp;
    }
    buf[bi] = '\0';
    serial_write_string(buf);
    serial_write_string("\n");

    return shm_segments[slot].shmid;
}

/* ─── Attach ───────────────────────────────────────────────────── */

uint64_t shm_attach(int shmid, uint32_t pid, uint64_t* pml4) {
    (void)pid;

    /* Find segment by shmid */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].in_use && shm_segments[i].shmid == shmid) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || !pml4) return 0;

    /* Get current task's shm_attached[] to record this attachment */
    struct task* current = task_find_by_pid(pid);
    if (!current) return 0;

    /* Find a free attachment slot */
    int aslot = -1;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (current->shm_attached[i].shmid == -1) {
            aslot = i;
            break;
        }
    }
    if (aslot < 0) return 0; /* No free attachment slots */

    /* Assign a virtual address via bump allocator */
    uint64_t vaddr = next_shm_vaddr;
    next_shm_vaddr += shm_segments[slot].page_count * PAGE_SIZE;

    /* Map each physical page into the process's PML4.
     * Flags: Present (0x01) | Writable (0x02) | User (0x04) = 0x07
     */
    for (uint32_t p = 0; p < shm_segments[slot].page_count; p++) {
        vmm_map_page_in(pml4,
                        vaddr + p * PAGE_SIZE,
                        shm_segments[slot].phys_pages[p],
                        0x07);
    }

    /* Increment refcount */
    shm_segments[slot].refcount++;

    /* Record attachment in task struct */
    current->shm_attached[aslot].shmid      = shmid;
    current->shm_attached[aslot].vaddr      = vaddr;
    current->shm_attached[aslot].page_count = shm_segments[slot].page_count;

    serial_write_string("[SHM] Attached segment ");
    char buf[16];
    int id = shmid, bi = 0;
    if (id == 0) { buf[bi++] = '0'; }
    else { while (id > 0) { buf[bi++] = '0' + (id % 10); id /= 10; } }
    for (int i = 0; i < bi / 2; i++) {
        char tmp = buf[i]; buf[i] = buf[bi - 1 - i]; buf[bi - 1 - i] = tmp;
    }
    buf[bi] = '\0';
    serial_write_string(buf);
    serial_write_string(" at VA\n");

    return vaddr;
}

/* ─── Detach ───────────────────────────────────────────────────── */

int shm_detach(uint64_t addr, uint32_t pid, uint64_t* pml4) {
    struct task* current = task_find_by_pid(pid);
    if (!current) return -1;

    /* Find the attachment record matching this VA */
    int aslot = -1;
    int shmid = -1;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (current->shm_attached[i].shmid >= 0 &&
            current->shm_attached[i].vaddr == addr) {
            aslot = i;
            shmid = current->shm_attached[i].shmid;
            break;
        }
    }
    if (aslot < 0) return -1;

    /* Find the segment in the global table */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].in_use && shm_segments[i].shmid == shmid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Segment already destroyed — just clear the record */
        current->shm_attached[aslot].shmid = -1;
        return 0;
    }

    /* Unmap pages from this process's PML4 */
    if (pml4) {
        for (uint32_t p = 0; p < current->shm_attached[aslot].page_count; p++) {
            vmm_unmap_page_in(pml4, addr + p * PAGE_SIZE);
        }
    }

    /* Clear the attachment record */
    current->shm_attached[aslot].shmid = -1;

    /* Decrement refcount */
    if (shm_segments[slot].refcount > 0) {
        shm_segments[slot].refcount--;
    }

    /* If no one is attached anymore, free the physical pages */
    if (shm_segments[slot].refcount == 0) {
        for (uint32_t p = 0; p < shm_segments[slot].page_count; p++) {
            pmm_free_page((void*)shm_segments[slot].phys_pages[p]);
        }
        shm_segments[slot].in_use = 0;
        serial_write_string("[SHM] Segment auto-freed (refcount=0)\n");
    }

    return 0;
}

/* ─── Destroy ──────────────────────────────────────────────────── */

int shm_destroy(int shmid) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].in_use && shm_segments[i].shmid == shmid) {
            /* If still attached by someone, refuse to destroy */
            if (shm_segments[i].refcount > 0) {
                serial_write_string("[SHM] Cannot destroy: still attached\n");
                return -1;
            }
            shm_segments[i].in_use = 0;
            serial_write_string("[SHM] Segment destroyed\n");
            return 0;
        }
    }
    return -1;
}

/* ─── Process Cleanup (called by reaper) ───────────────────────── */

void shm_cleanup_process(struct task* t) {
    if (!t) return;

    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (t->shm_attached[i].shmid >= 0) {
            int shmid = t->shm_attached[i].shmid;
            uint64_t addr = t->shm_attached[i].vaddr;
            uint32_t pc = t->shm_attached[i].page_count;

            /* Find segment in global table */
            for (int s = 0; s < SHM_MAX_SEGMENTS; s++) {
                if (shm_segments[s].in_use &&
                    shm_segments[s].shmid == shmid) {
                    /* Unmap pages from the dying process's PML4 */
                    if (t->pml4) {
                        for (uint32_t p = 0; p < pc; p++) {
                            vmm_unmap_page_in(t->pml4,
                                              addr + p * PAGE_SIZE);
                        }
                    }
                    /* Decrement refcount */
                    if (shm_segments[s].refcount > 0) {
                        shm_segments[s].refcount--;
                    }
                    /* Auto-free if no one else is attached */
                    if (shm_segments[s].refcount == 0) {
                        for (uint32_t p = 0;
                             p < shm_segments[s].page_count; p++) {
                            pmm_free_page(
                                (void*)shm_segments[s].phys_pages[p]);
                        }
                        shm_segments[s].in_use = 0;
                        serial_write_string(
                            "[SHM] Segment auto-freed on process exit\n");
                    }
                    break;
                }
            }
            t->shm_attached[i].shmid = -1;
        }
    }
}
