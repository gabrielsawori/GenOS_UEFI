#include "shm.h"
#include "pmm.h"
#include "heap.h"
#include "vmm.h"
#include "user_memory.h"
#include "../cpu/preempt.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "../drivers/serial.h"
#include "../limine.h"

extern volatile struct limine_hhdm_request hhdm_request;
static shm_segment_t segments[SHM_MAX_SEGMENTS];
static uint32_t next_shmid = 1;

static shm_segment_t* find_segment(int id) {
    for (unsigned i = 0; i < SHM_MAX_SEGMENTS; i++)
        if (segments[i].in_use && segments[i].shmid == id) return &segments[i];
    return NULL;
}

static void release_segment(shm_segment_t* s) {
    for (uint32_t i = 0; i < s->page_count; i++) pmm_free_page((void*)s->phys_pages[i]);
    kfree(s->phys_pages);
    memset(s, 0, sizeof(*s));
}

void shm_init(void) {
    memset(segments, 0, sizeof(segments));
    serial_write_string("[OK] Shared Memory (SHM) manager initialized.\n");
}

int shm_create(uint32_t pid, uint32_t size) {
    PREEMPT_GUARD;
    if (!size || size > SHM_MAX_PAGES * PAGE_SIZE || next_shmid > INT32_MAX) return -1;
    struct task* owner = task_memory_owner(task_find_by_pid(pid));
    if (owner) pid = owner->pid;
    shm_segment_t* s = NULL;
    for (unsigned i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!segments[i].in_use) { s = &segments[i]; break; }
    }
    if (!s) return -1;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    s->phys_pages = kmalloc((size_t)pages * sizeof(uint64_t));
    if (!s->phys_pages) return -1;
    s->page_count = 0;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) { release_segment(s); return -1; }
        memset((void*)(phys + hhdm_request.response->offset), 0, PAGE_SIZE);
        s->phys_pages[s->page_count++] = phys;
    }
    s->shmid = (int)next_shmid++;
    s->size = size;
    s->creator_pid = pid;
    s->refcount = 0;
    s->in_use = 1;
    return s->shmid;
}

uint64_t shm_attach(int id, uint32_t pid, uint64_t* pml4) {
    PREEMPT_GUARD;
    shm_segment_t* s = find_segment(id);
    struct task* owner = task_memory_owner(task_find_by_pid(pid));
    if (!s || !owner || !pml4 || owner->pml4 != pml4 || s->refcount == UINT32_MAX) return 0;
    unsigned slot;
    for (slot = 0; slot < SHM_MAX_ATTACH; slot++)
        if (owner->shm_attached[slot].shmid < 0) break;
    if (slot == SHM_MAX_ATTACH) return 0;
    uint64_t base = user_find_free_range(owner, USER_SHM_BASE, USER_SHM_END, s->page_count);
    if (!base) return 0;
    if (!vmm_map_pages_in(pml4, base, s->phys_pages, s->page_count, VMM_FLAGS_USER_DATA))
        return 0;
    s->refcount++;
    owner->shm_attached[slot] = (shm_attach_record_t){
        .shmid = id, .vaddr = base, .page_count = s->page_count};
    return base;
}

static void detach_record(struct task* owner, unsigned slot) {
    shm_attach_record_t* a = &owner->shm_attached[slot];
    vmm_unmap_pages_in(owner->pml4, a->vaddr, a->page_count);
    shm_segment_t* s = find_segment(a->shmid);
    if (s && --s->refcount == 0) release_segment(s);
    *a = (shm_attach_record_t){.shmid = -1};
}

int shm_detach(uint64_t base, uint32_t pid, uint64_t* pml4) {
    PREEMPT_GUARD;
    struct task* owner = task_memory_owner(task_find_by_pid(pid));
    if (!owner || !pml4 || owner->pml4 != pml4) return -1;
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++) {
        if (owner->shm_attached[i].shmid >= 0 && owner->shm_attached[i].vaddr == base) {
            detach_record(owner, i);
            return 0;
        }
    }
    return -1;
}

int shm_destroy(int id) {
    PREEMPT_GUARD;
    shm_segment_t* s = find_segment(id);
    if (!s || s->refcount) return -1;
    release_segment(s);
    return 0;
}

void shm_cleanup_process(struct task* t) {
    PREEMPT_GUARD;
    if (!t || t->is_thread) return;
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++)
        if (t->shm_attached[i].shmid >= 0) detach_record(t, i);
    /* Unpublished constructors have PID zero and own no created segments. */
    if (t->pid) {
        for (unsigned i = 0; i < SHM_MAX_SEGMENTS; i++) {
            shm_segment_t* s = &segments[i];
            if (s->in_use && s->creator_pid == t->pid && !s->refcount) release_segment(s);
        }
    }
}

int shm_owns_mapping(struct task* owner, uint64_t va, uint64_t phys) {
    PREEMPT_GUARD;
    if (!owner) return 0;
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++) {
        shm_attach_record_t* a = &owner->shm_attached[i];
        if (a->shmid < 0 || va < a->vaddr || va - a->vaddr >= (uint64_t)a->page_count * PAGE_SIZE)
            continue;
        shm_segment_t* s = find_segment(a->shmid);
        if (s && s->refcount && s->page_count == a->page_count &&
            s->phys_pages[(va - a->vaddr) / PAGE_SIZE] == phys) return 1;
    }
    return 0;
}

int shm_inherit_attachments(struct task* parent, struct task* child) {
    PREEMPT_GUARD;
    uint32_t added[SHM_MAX_SEGMENTS] = {0};
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++) {
        shm_attach_record_t* a = &parent->shm_attached[i];
        if (a->shmid < 0) continue;
        shm_segment_t* s = find_segment(a->shmid);
        if (!s || s->page_count != a->page_count ||
            s->refcount > UINT32_MAX - ++added[s - segments]) return 0;
        for (uint32_t p = 0; p < a->page_count; p++)
            if (vmm_get_physical_in(child->pml4, a->vaddr + (uint64_t)p * PAGE_SIZE, NULL) !=
                s->phys_pages[p]) return 0;
    }
    for (unsigned i = 0; i < SHM_MAX_SEGMENTS; i++) segments[i].refcount += added[i];
    for (unsigned i = 0; i < SHM_MAX_ATTACH; i++) child->shm_attached[i] = parent->shm_attached[i];
    return 1;
}
