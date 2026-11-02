#include "task.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/shm.h"
#include "../mm/cow.h"
#include "../fs/elf.h"
#include "../fs/elf_validate.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../cpu/gdt.h"
#include "../cpu/spinlock.h"
#include "../cpu/preempt.h"
#include "../cpu/percpu.h"
#include "../cpu/lapic.h"
#include "../cpu/smp.h"
#include "../limine.h"
#include "utils.h"
#include <stddef.h>

/* Task metadata is protected by the recursive kernel mutation guard. */

/* Borrow from pmm.c for HHDM physical page access during fork copy */
extern volatile struct limine_hhdm_request hhdm_request;

/* Boot kernel stack from kernel.c — used as init task's kernel stack */
extern uint8_t boot_kernel_stack[];
#define BOOT_KERNEL_STACK_SIZE 16384

#define current_task (percpu_current()->current)
_Static_assert(offsetof(struct task, fpu) % 16 == 0, "task FXSAVE alignment");
static struct task ap_idle[MAX_CPUS];
static uint32_t placement_cursor;
static struct task* task_list_head = NULL;
static struct task* task_list_tail = NULL;
static uint32_t next_pid = 1;

/* Quantum lookup table indexed by priority level */
static const uint8_t quantum_table[PRIO_COUNT] = {
    QUANTUM_IDLE,    /* PRIO_IDLE   = 0 */
    QUANTUM_LOW,     /* PRIO_LOW    = 1 */
    QUANTUM_NORMAL,  /* PRIO_NORMAL = 2 */
    QUANTUM_HIGH     /* PRIO_HIGH   = 3 */
};



/* Helper: clamp priority to valid range */
static inline int8_t clamp_priority(int p) {
    if (p < PRIO_IDLE) return PRIO_IDLE;
    if (p > PRIO_HIGH) return PRIO_HIGH;
    return (int8_t)p;
}

/* Helper: initialize priority fields for a new task */
static void task_init_priority(struct task* t, int8_t prio) {
    t->base_priority = prio;
    t->priority      = prio;
    t->nice          = 0;
    t->quantum_max   = quantum_table[prio];
    t->quantum       = t->quantum_max;
    t->cpu_ticks     = 0;
    t->sleep_count   = 0;
}

#define KERNEL_STACK_PAGES 4
static struct task* reaper_task;
static struct task_resource_stats resource_stats;
static void task_reaper_main(void);

/* All callers either hold PREEMPT_GUARD or run before interrupts start. */
static struct task* task_alloc(void) {
    struct task* t = kmalloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    fpu_state_init(&t->fpu);
    user_memory_init(t);
    t->state = TASK_READY;
    t->cpu_id = smp_get_current_cpu_id();
    t->ready_since = timer_get_ticks();
    t->thread_count = 1;
    t->uid = current_task ? current_task->uid : 0;
    t->gid = current_task ? current_task->gid : 0;
    task_init_priority(t, PRIO_NORMAL);
    for (int i = 0; i < VFS_MAX_FDS; i++) t->fds[i].tmpfs_idx = -1;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) t->shm_attached[i].shmid = -1;
    return t;
}

static int task_alloc_stack(struct task* t) {
    uint64_t phys = (uint64_t)pmm_alloc_contiguous_pages(KERNEL_STACK_PAGES);
    if (!phys) {
        resource_stats.stack_alloc_failures++;
        return 0;
    }
    t->kernel_stack_phys = phys;
    t->kernel_stack_pages = KERNEL_STACK_PAGES;
    t->kernel_stack = phys + hhdm_request.response->offset +
                      KERNEL_STACK_PAGES * PAGE_SIZE;
    resource_stats.kernel_stack_pages += KERNEL_STACK_PAGES;
    return 1;
}

static void task_free_stack(struct task* t) {
    for (uint32_t i = 0; i < t->kernel_stack_pages; i++) {
        pmm_free_page((void*)(t->kernel_stack_phys + i * PAGE_SIZE));
    }
    resource_stats.kernel_stack_pages -= t->kernel_stack_pages;
    t->kernel_stack_phys = 0;
    t->kernel_stack_pages = 0;
    t->kernel_stack = 0;
}

static uint32_t placement_cpu(void) {
    uint32_t count = smp_get_cpu_count();
    if (!count) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cpu = placement_cursor++ % count;
        if (percpu_get(cpu)->idle_task) return cpu;
    }
    return 0;
}

static int task_active(struct task* t) {
    for (uint32_t i = 0; i < smp_get_cpu_count(); i++)
        if (percpu_get(i)->current == t ||
            __atomic_load_n(&percpu_get(i)->retiring, __ATOMIC_ACQUIRE) == t) return 1;
    return 0;
}

/* Changing current does not mean IRET has left the outgoing stack yet.
 * The next entry on a different stack acknowledges that it can be reaped.
 * This also handles a nested exception before the outgoing IRET safely. */
void task_context_enter(void) {
    struct percpu_data* p = percpu_current();
    struct task* t = __atomic_load_n(&p->retiring, __ATOMIC_ACQUIRE);
    if (!t) return;
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    uint64_t low = t->kernel_stack - t->kernel_stack_pages * PAGE_SIZE;
    if (rsp >= low && rsp < t->kernel_stack) return;
    __atomic_store_n(&p->retiring, NULL, __ATOMIC_RELEASE);
}

static void task_publish(struct task* t) {
    t->pid = next_pid++;
    if (!t->is_thread) t->tgid = t->pid;
    t->next = NULL;
    task_list_tail->next = t;
    task_list_tail = t;
    struct percpu_data* p = percpu_get(t->cpu_id);
    if (__atomic_load_n(&p->scheduler_online, __ATOMIC_ACQUIRE) &&
        t->cpu_id != smp_get_current_cpu_id())
        lapic_send_ipi(p->lapic_id, LAPIC_RESCHEDULE_VECTOR);
}

/* Release an unpublished process after a failed constructor. */
static void task_discard(struct task* t) {
    shm_cleanup_process(t);
    user_memory_release(t);
    if (t->pml4 && !t->is_thread) vmm_destroy_address_space(t->pml4);
    task_free_stack(t);
    kfree(t);
}

void task_get_resource_stats(struct task_resource_stats* out) {
    PREEMPT_GUARD;
    if (out) *out = resource_stats;
}

static void __attribute__((noreturn)) task_kernel_entry(void) {
    current_task->entry_point();
    exit_current_task();
    __builtin_unreachable();
}

void task_init(void) {
    PREEMPT_GUARD;
    serial_write_string("[INFO] Initializing Multitasking Engine...\n");
    struct task* init_task = task_alloc();
    if (!init_task) {
        serial_write_string("[FATAL] Cannot allocate idle task\n");
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    }
    init_task->state = TASK_RUNNING;
    init_task->kernel_stack = (uint64_t)(boot_kernel_stack + BOOT_KERNEL_STACK_SIZE);
    task_init_priority(init_task, PRIO_IDLE);
    init_task->regs.cs = 0x08;
    init_task->regs.ss = 0x10;
    init_task->regs.rflags = 0x202;
    init_task->regs.rsp = init_task->kernel_stack;
    current_task = task_list_head = task_list_tail = init_task;
    percpu_get(0)->idle_task = init_task;
    for (uint32_t i = 1; i < smp_get_cpu_count(); i++) {
        if (!__atomic_load_n(&smp_get_cpus()[i].active, __ATOMIC_ACQUIRE)) continue;
        struct task* idle = &ap_idle[i];
        memset(idle, 0, sizeof(*idle));
        fpu_state_init(&idle->fpu);
        idle->cpu_id = i;
        idle->state = TASK_RUNNING;
        idle->kernel_stack = percpu_get(i)->kernel_stack_top;
        task_init_priority(idle, PRIO_IDLE);
        percpu_get(i)->current = percpu_get(i)->idle_task = idle;
    }

    reaper_task = create_task(task_reaper_main);
    if (!reaper_task) {
        serial_write_string("[FATAL] Cannot allocate reaper task\n");
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    }
    task_init_priority(reaper_task, PRIO_HIGH);
    reaper_task->state = TASK_WAITING;
    serial_write_string("[OK] Multitasking Engine Active.\n");
}

struct task* create_task(void (*entry_point)(void)) {
    PREEMPT_GUARD;
    if (!entry_point || !task_list_tail) return NULL;
    struct task* t = task_alloc();
    if (!t) return NULL;
    if (!task_alloc_stack(t)) {
        kfree(t);
        return NULL;
    }
    t->entry_point = entry_point;
    t->stack_base = t->kernel_stack - KERNEL_STACK_PAGES * PAGE_SIZE;
    t->regs.rip = (uint64_t)task_kernel_entry;
    t->regs.cs = 0x08;
    t->regs.ss = 0x10;
    t->regs.rflags = 0x202;
    /* iret enters a function directly: emulate the CALL return-address slot. */
    t->regs.rsp = t->kernel_stack - sizeof(uint64_t);
    *(uint64_t*)t->regs.rsp = 0;
    task_publish(t);
    return t;
}

struct task* create_user_task(const uint8_t* binary_data, size_t binary_size) {
    PREEMPT_GUARD;
    /* Reject malformed images and reserve stack tracking before any allocation. */
    elf_image_info_t image;
    if (!elf_validate(binary_data, binary_size, USER_MAX_PAGES - USER_STACK_PAGES, &image))
        return NULL;
    struct task* t = task_alloc();
    if (!t) return NULL;
    if (!task_alloc_stack(t)) {
        kfree(t);
        return NULL;
    }
    t->pml4 = vmm_create_address_space();
    if (!t->pml4) goto fail;
    if (!user_pages_reserve(t, image.page_count + USER_STACK_PAGES)) goto fail;
    uint64_t entry = elf_load(binary_data, binary_size, t->pml4,
                              t->user_pages, t->user_vaddrs, &t->user_page_count,
                              t->user_page_capacity - USER_STACK_PAGES);
    if (!entry) goto fail;

    /* Each address space reuses the same stack VA, with an unmapped page at
     * both ends. Reject ELF collisions including either guard. */
    uint64_t stack_base = USER_STACK_BASE;
    for (uint64_t va = stack_base - PAGE_SIZE;
         va <= stack_base + USER_STACK_PAGES * PAGE_SIZE; va += PAGE_SIZE)
        if (vmm_pte_in(t->pml4, va)) goto fail;
    for (unsigned i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) goto fail;
        t->user_vaddrs[t->user_page_count] = stack_base + i * PAGE_SIZE;
        t->user_pages[t->user_page_count++] = phys;
        memset((void*)(phys + hhdm_request.response->offset), 0, PAGE_SIZE);
        if (!vmm_map_page_in(t->pml4, stack_base + i * PAGE_SIZE, phys,
                             VMM_FLAGS_USER_STACK)) goto fail;
    }
    t->stack_base = stack_base;
    t->regs.rip = entry;
    t->regs.cs = 0x23;
    t->regs.ss = 0x1B;
    t->regs.rflags = 0x202;
    t->regs.rsp = stack_base + USER_STACK_PAGES * PAGE_SIZE - 16;
    /* PID 1 belongs to the kernel reaper; the first user task is the shell. */
    if (next_pid == 2) task_init_priority(t, PRIO_HIGH);
    t->cpu_id = placement_cpu();
    task_publish(t);
    return t;
fail:
    task_discard(t);
    return NULL;
}

/* Keep a dead group leader (the address-space owner) until all threads have
 * been removed. Active and retiring stacks on every CPU remain pinned.
 */
static void task_reap_dead(void) {
    PREEMPT_GUARD;
    struct task* prev = NULL;
    struct task* t = task_list_head;
    while (t) {
        struct task* next = t->next;
        int can_reap = t->state == TASK_DEAD && !task_active(t) && t->pid != 0;
        if (can_reap && !t->is_thread && t->pml4) {
            for (struct task* th = task_list_head; th; th = th->next) {
                if (th != t && th->pml4 == t->pml4) {
                    th->state = TASK_DEAD;
                    can_reap = 0;
                }
            }
        }
        if (!can_reap) {
            prev = t;
            t = next;
            continue;
        }
        if (prev) prev->next = next;
        else task_list_head = next;
        if (task_list_tail == t) task_list_tail = prev;
        for (struct task* w = task_list_head; w; w = w->next) {
            if (w->state == TASK_WAITING && w->wait_target_pid == t->pid)
                w->state = TASK_READY;
        }
        if (t->is_thread) {
            struct task* leader = task_find_by_pid(t->tgid);
            if (leader && leader->thread_count) leader->thread_count--;
        } else {
            shm_cleanup_process(t);
            user_memory_release(t);
            if (t->pml4) vmm_destroy_address_space(t->pml4);
        }
        task_free_stack(t);
        kfree(t);
        resource_stats.tasks_reaped++;
        t = next;
    }
}

static void task_reaper_main(void) {
    for (;;) {
        task_reap_dead();
        /* A later PIT tick wakes this worker whenever dead tasks remain. */
        {
            PREEMPT_GUARD;
            current_task->state = TASK_WAITING;
        }
        asm volatile("sti; hlt" ::: "memory");
    }
}

/* Tasks are pinned after placement; no task can be selected on two CPUs.
 * Task list scans are bounded by task count. Aging gives lower priorities a
 * turn; fine-grained run queues/load balancing are later optimizations. */
void schedule(struct registers* current_regs) {
    struct percpu_data* cpu = percpu_current();
    if (!current_task || !preempt_try_disable()) return;
    uint64_t now = timer_get_ticks();
    for (struct task* t = task_list_head; t; t = t->next) {
        if (t->state == TASK_SLEEPING && now >= t->sleep_until) {
            t->state = TASK_READY;
            t->ready_since = now;
        }
        if (t->state == TASK_DEAD && !task_active(t) && reaper_task &&
            reaper_task->state == TASK_WAITING) reaper_task->state = TASK_READY;
    }
    current_task->cpu_ticks++;
    if (current_task != cpu->idle_task && current_task->state == TASK_RUNNING) {
        if (current_task->quantum) current_task->quantum--;
        if (current_task->quantum) { preempt_enable(); return; }
    }
    struct task* previous = current_task;
    if (previous->state != TASK_DEAD) {
        previous->regs = *current_regs;
        if (previous->state == TASK_RUNNING) {
            previous->state = TASK_READY;
            previous->ready_since = now;
        }
    }
    struct task* best = NULL;
    int best_prio = -1;
    struct task* start = previous->next ? previous->next : task_list_head;
    struct task* t = start;
    do {
        if (t != cpu->idle_task && t->pid && t->cpu_id == cpu->cpu_id &&
            t->state == TASK_READY) {
            int prio = now - t->ready_since >= STARVATION_BOOST_INTERVAL ?
                       PRIO_HIGH + 1 : t->priority;
            if (prio > best_prio) { best = t; best_prio = prio; }
        }
        t = t->next ? t->next : task_list_head;
    } while (t != start);
    current_task = best ? best : cpu->idle_task;
    /* Eager switching also covers tasks preempted inside a blocking syscall.
     * Kernel C/assembly must not use SIMD except these explicit helpers. */
    if (current_task != previous) {
        fpu_save(&previous->fpu);
        fpu_restore(&current_task->fpu);
    }
    if (current_task != previous && previous->pid)
        __atomic_store_n(&cpu->retiring, previous, __ATOMIC_RELEASE);
    current_task->state = TASK_RUNNING;
    current_task->ready_since = now;
    current_task->quantum = current_task->quantum_max;
    vmm_switch_pml4(current_task->pml4);
    percpu_set_kernel_stack(cpu->cpu_id, current_task->kernel_stack);
    *current_regs = current_task->regs;
    cpu->switches++;
    /* Preserve the existing boot diagnostic without logging every tick. */
    if (cpu->cpu_id == 0 && cpu->switches == 1)
        serial_write_string("[SWITCH #1] BSP scheduler active\n");
    preempt_enable();
}

struct task* create_task_on_cpu(void (*entry)(void), uint32_t cpu) {
    PREEMPT_GUARD;
    if (cpu >= smp_get_cpu_count() || !percpu_get(cpu)->idle_task) return NULL;
    struct task* t = create_task(entry);
    if (t) t->cpu_id = cpu;
    return t;
}

int task_set_cpu(uint32_t pid, uint32_t cpu) {
    PREEMPT_GUARD;
    struct task* t = task_find_by_pid(pid);
    if (!t || !pid || t == reaper_task || cpu >= smp_get_cpu_count() ||
        !percpu_get(cpu)->idle_task || task_active(t) || t->state == TASK_DEAD) return -1;
    t->cpu_id = cpu;
    return 0;
}

/* A blocking syscall cannot return to userspace until wakeup. Periodic IRQs
 * also cover a wakeup just before HLT; no kernel lock is held while waiting. */
void task_wait_current(void) {
    for (;;) {
        int runnable;
        {
            PREEMPT_GUARD;
            runnable = current_task && (current_task->state == TASK_RUNNING ||
                                        current_task->state == TASK_READY);
        }
        if (runnable) return;
        asm volatile("sti; hlt" ::: "memory");
    }
}

void task_abort_current_group(void) {
    {
        PREEMPT_GUARD;
        struct task* faulting = current_task;
        if (faulting && faulting->pml4) {
            for (struct task* t = task_list_head; t; t = t->next) {
                if (t->pml4 == faulting->pml4) t->state = TASK_DEAD;
            }
            for (struct task* w = task_list_head; w; w = w->next) {
                if (w->state == TASK_WAITING && task_is_dead(w->wait_target_pid))
                    w->state = TASK_READY;
            }
        }
    }
    exit_current_task();
    __builtin_unreachable();
}

// Fungsi Pengeksekusi Mati
void exit_current_task(void) {
    {
        PREEMPT_GUARD;
        if (current_task != NULL) {
            current_task->state = TASK_DEAD;
            /* Wake waiters while the task list is stable. */
            for (struct task* w = task_list_head; w; w = w->next) {
                if (w->state == TASK_WAITING &&
                    w->wait_target_pid == current_task->pid) {
                    w->state = TASK_READY;
                }
            }
        }
    }
    // Tunggu scheduler untuk membersihkan
    while(1) { asm volatile ("sti; hlt"); }
}

/*
 * sleep_current_task() - Menidurkan task saat ini sampai tick tertentu.
 *
 * @param wake_tick: nilai timer tick di mana task harus dibangunkan.
 *
 * Fungsi ini TIDAK memblokir — hanya menandai task sebagai SLEEPING.
 * Scheduler akan skip task ini sampai timer_get_ticks() >= wake_tick,
 * lalu otomatis membangunkan (ubah state ke TASK_READY).
 */
void sleep_current_task(uint64_t wake_tick) {
    PREEMPT_GUARD;
    if (current_task != NULL) {
        current_task->sleep_until = wake_tick;
        current_task->state = TASK_SLEEPING;
        current_task->sleep_count++; /* Track I/O-bound behavior */
    }
}

/*
 * task_is_dead() - cek status task berdasarkan PID.
 *
 * Linked list task disusuri untuk menemukan PID yang diminta.
 * Dipakai syscall `wait_pid` agar shell di Ring 3 bisa polling berdasarkan PID,
 * tanpa menyimpan pointer task yang dapat dibebaskan oleh reaper.
 *
 * Return: 1 jika task TASK_DEAD atau PID tidak ditemukan, 0 bila masih hidup.
 */
int task_is_dead(uint32_t pid) {
    PREEMPT_GUARD;
    struct task* t = task_list_head;
    while (t != NULL) {
        if (t->pid == pid) {
            return (t->state == TASK_DEAD) ? 1 : 0;
        }
        t = t->next;
    }
    return 1; /* PID tidak ditemukan → anggap selesai */
}

/*
 * task_mark_running() - Tandai task saat ini sebagai TASK_RUNNING.
 *
 * Dipanggil di awal syscall_handler agar task yang sebelumnya tidur
 * (TASK_SLEEPING) tidak dilewati scheduler jika timer interrupt
 * menyala saat task sedang di dalam syscall berikutnya.
 * Tanpa ini, register task bisa disimpan paksa oleh scheduler ke
 * task->regs saat state masih SLEEPING → korupsi state.
 */
void task_mark_running(void) {
    PREEMPT_GUARD;
    if (current_task != NULL && current_task->state != TASK_DEAD) {
        current_task->state = TASK_RUNNING;
    }
}

/*
 * wait_for_pid() - Blokir task saat ini sampai task target selesai.
 *
 * @param target_pid: PID task yang ditunggu.
 *
 * Jika target sudah TASK_DEAD atau tidak ditemukan, langsung return
 * tanpa memblokir. Jika target masih hidup, set state ke TASK_WAITING
 * dan scheduler akan skip task ini sampai target mati (dibangunkan
 * oleh exit_current_task atau worker reaper).
 */
void wait_for_pid(uint32_t target_pid) {
    PREEMPT_GUARD;
    if (!current_task) return;

    /* Cek apakah target sudah mati atau tidak ada */
    struct task* t = task_list_head;
    int found = 0;
    while (t != NULL) {
        if (t->pid == target_pid) {
            found = 1;
            if (t->state == TASK_DEAD) return; /* Sudah mati, tidak perlu tunggu */
            break;
        }
        t = t->next;
    }

    if (!found) return; /* PID tidak ditemukan, anggap selesai */

    /* Blokir task ini sampai target mati */
    current_task->state = TASK_WAITING;
    current_task->wait_target_pid = target_pid;
}

uint32_t get_current_pid(void) {
    return current_task ? current_task->pid : 0;
}

/*
 * task_get_current_fds() - Return the current task's FD array.
 * Used by VFS layer to access per-process file descriptors.
 */
vfs_fd_t* task_get_current_fds(void) {
    return current_task ? current_task->fds : NULL;
}

/*
 * task_find_by_pid() - Look up a task by PID in the linked list.
 */
struct task* task_find_by_pid(uint32_t pid) {
    PREEMPT_GUARD;
    struct task* t = task_list_head;
    while (t != NULL) {
        if (t->pid == pid) return t;
        t = t->next;
    }
    return NULL;
}

/*
 * task_get_current_pml4() - Return the current task's PML4 pointer.
 */
uint64_t* task_get_current_pml4(void) {
    return current_task ? current_task->pml4 : NULL;
}

struct task* task_memory_owner(struct task* t) {
    if (!t || !t->is_thread) return t;
    struct task* leader = task_find_by_pid(t->tgid);
    return leader && leader->pml4 == t->pml4 ? leader : NULL;
}

/*
 * task_fork() - Clone the current process using Copy-on-Write (CoW).
 *
 * Instead of copying every page physically, we:
 *   1. Map the SAME physical pages in both parent and child
 *   2. Mark writable private pages CoW; preserve RO and shared mappings
 *   3. Increment page reference count (pmm_ref_page)
 *   4. On write, page fault handler copies the page on demand
 *
 * Mapping/refcount changes are delegated to mm/cow.c; parent protection is
 * committed only after all child mappings and attachment references succeed.
 *
 * Returns child PID (>0) to parent, or -1 on failure.
 */

int32_t task_fork(const struct registers* user_frame) {
    PREEMPT_GUARD;
    if (!current_task || !user_frame || user_frame->cs != 0x23 ||
        user_frame->ss != 0x1B) return -1;
    if (!current_task->pml4) {
        serial_write_string("[FORK] Cannot fork kernel task!\n");
        return -1;
    }

    struct task* child = task_alloc();
    if (!child) return -1;
    if (!task_alloc_stack(child)) {
        kfree(child);
        return -1;
    }

    /* 1. Create child's address space (PML4 with shared upper half) */
    uint64_t* child_pml4 = vmm_create_address_space();
    if (!child_pml4) {
        serial_write_string("[FORK] Failed to create child address space!\n");
        task_discard(child);
        return -1;
    }
    child->pml4 = child_pml4;

    if (!cow_clone_address_space(current_task, child)) {
        task_discard(child);
        return -1;
    }

    /* The live syscall frame contains the actual user return RIP/RSP. */
    child->regs = *user_frame;
    child->regs.rax = 0;
    /* The saved task image may predate the current syscall. Snapshot the live
     * CPU while the mutation guard prevents a context switch. */
    fpu_save(&child->fpu);

    /* 5. Set up child task fields */
    child->state = TASK_READY;
    child->pml4 = child_pml4;
    child->stack_base = current_task->stack_base;
    child->sleep_until = 0;
    child->wait_target_pid = 0;

    /* 6. Copy FD table from parent */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        child->fds[i] = current_task->fds[i];
    }

    /* 8. Inherit parent's scheduling priority */
    child->base_priority = current_task->base_priority;
    child->priority      = current_task->priority;
    child->nice          = current_task->nice;
    child->quantum_max   = current_task->quantum_max;
    child->quantum       = child->quantum_max;
    child->cpu_ticks     = 0;
    child->sleep_count   = 0;

    /* 9. Place independent processes across active CPUs. */
    child->cpu_id = placement_cpu();
    task_publish(child);

    serial_write_string("[FORK-CoW] Child PID ");
    char pid_buf[16];
    itoa(child->pid, pid_buf, 10);
    serial_write_string(pid_buf);
    serial_write_string(" shares ");
    itoa(child->user_page_count, pid_buf, 10);
    serial_write_string(pid_buf);
    serial_write_string(" pages (0 copies)\n");

    return (int32_t)child->pid;

}

/*
 * task_set_nice() - Atur nice value untuk task saat ini.
 *
 * Nice value mempengaruhi prioritas efektif:
 *   effective_priority = base_priority - nice
 *
 * Nice negatif (-2, -1) = prioritas LEBIH TINGGI (lebih banyak CPU)
 * Nice positif (+1, +2) = prioritas LEBIH RENDAH (lebih sedikit CPU)
 * Nice 0               = default (tidak ada perubahan)
 *
 * Contoh: task PRIO_NORMAL (2) dengan nice=+1 → efektif PRIO_LOW (1)
 */
int task_set_nice(int nice_val) {
    PREEMPT_GUARD;
    if (!current_task) return -1;

    /* Clamp nice value to valid range */
    if (nice_val < -2) nice_val = -2;
    if (nice_val >  2) nice_val =  2;

    current_task->nice = (int8_t)nice_val;

    /* Recalculate effective priority */
    int eff = current_task->base_priority - nice_val;
    current_task->priority = clamp_priority(eff);
    current_task->quantum_max = quantum_table[current_task->priority];

    return 0;
}

/*
 * task_get_sched_info() - Dapatkan informasi scheduling untuk task tertentu.
 *
 * Format output: "PID=N PRI=N NICE=N Q=N/M CPU=N SLP=N ST=X\n"
 * Di mana ST = R(unning) | r(eady) | S(leeping) | W(aiting) | D(ead)
 */
int task_get_sched_info(uint32_t pid, char* out_buf) {
    PREEMPT_GUARD;
    struct task* t;
    if (pid == 0) {
        t = current_task;
    } else {
        t = task_find_by_pid(pid);
    }
    if (!t || !out_buf) return -1;

    char tmp[16];
    char* p = out_buf;

    /* Helper: append string */
    #define APPEND(s) do { const char* _s = (s); while (*_s) *p++ = *_s++; } while(0)

    APPEND("PID=");
    itoa(t->pid, tmp, 10); APPEND(tmp);
    APPEND(" PRI=");
    itoa(t->priority, tmp, 10); APPEND(tmp);
    APPEND(" NICE=");
    if (t->nice < 0) { *p++ = '-'; itoa(-t->nice, tmp, 10); }
    else { itoa(t->nice, tmp, 10); }
    APPEND(tmp);
    APPEND(" Q=");
    itoa(t->quantum, tmp, 10); APPEND(tmp);
    *p++ = '/';
    itoa(t->quantum_max, tmp, 10); APPEND(tmp);
    APPEND(" CPU=");
    itoa(t->cpu_ticks, tmp, 10); APPEND(tmp);
    APPEND(" SLP=");
    itoa(t->sleep_count, tmp, 10); APPEND(tmp);
    APPEND(" ST=");

    switch (t->state) {
        case TASK_RUNNING:  *p++ = 'R'; break;
        case TASK_READY:    *p++ = 'r'; break;
        case TASK_SLEEPING: *p++ = 'S'; break;
        case TASK_WAITING:  *p++ = 'W'; break;
        case TASK_DEAD:     *p++ = 'D'; break;
    }
    *p++ = '\n';
    *p = '\0';

    #undef APPEND
    return 0;
}

/*
 * task_clone() - Create a new thread sharing the parent's address space.
 *
 * @param entry:     User-space function address where thread starts executing.
 * @param stack_top: Top of the thread's user-allocated stack.
 *
 * Unlike fork(), clone() does NOT copy the address space. The new thread:
 *   - SHARES parent's PML4 (same virtual memory, same code, same data)
 *   - SHARES parent's user_pages[] (no separate page tracking)
 *   - Copies parent's FD table (descriptor offsets remain per-thread)
 *   - Gets its OWN: PID/TID, registers, kernel stack, scheduler state
 *
 * Thread Group: all threads in a process share the same TGID (= leader's PID).
 * When the leader exits, all threads in the group are killed too.
 */
int32_t task_clone(uint64_t entry, uint64_t stack_top) {
    PREEMPT_GUARD;
    if (!current_task) return -1;
    if (!current_task->pml4) {
        serial_write_string("[CLONE] Cannot create thread in kernel task!\n");
        return -1;
    }

    struct task* leader = task_find_by_pid(current_task->tgid);
    if (!leader || leader->state == TASK_DEAD) return -1;
    if (!entry || entry >= 0x800000000000ULL || stack_top < PAGE_SIZE ||
        stack_top >= 0x800000000000ULL) return -1;
    struct task* thread = task_alloc();
    if (!thread) return -1;
    if (!task_alloc_stack(thread)) {
        kfree(thread);
        return -1;
    }

    /* 2. Set up thread registers */
    for (size_t i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&thread->regs)[i] = 0;
    }
    thread->regs.rip    = entry;
    thread->regs.cs     = 0x23;  /* User Code, Ring 3 */
    thread->regs.rflags = 0x202; /* IF=1, reserved=1 */
    thread->regs.rsp    = stack_top & ~0xFULL; /* 16-byte aligned */
    thread->regs.ss     = 0x1B;  /* User Data, Ring 3 */
    fpu_save(&thread->fpu); /* Inherit creator's live FP environment. */

    /* 3. Thread identity */
    thread->tgid = current_task->tgid; /* Same thread group as parent */
    thread->is_thread = 1;
    thread->thread_count = 0; /* Only leader tracks count */
    thread->state = TASK_READY;

    /* SHARE parent's address space — THIS IS THE KEY DIFFERENCE FROM FORK */
    thread->pml4 = current_task->pml4;
    thread->stack_base = stack_top - 4096; /* Approximate */

    /* Thread doesn't track user_pages — leader owns them */
    thread->user_page_count = 0;
    /* 5. Copy the parent's descriptors; FD table sharing is not implemented. */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        thread->fds[i] = current_task->fds[i];
    }

    /* 6. Clear SHM attachments */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        thread->shm_attached[i].shmid = -1;
        thread->shm_attached[i].vaddr = 0;
        thread->shm_attached[i].page_count = 0;
    }

    /* 7. Inherit scheduling priority */
    thread->base_priority = current_task->base_priority;
    thread->priority      = current_task->priority;
    thread->nice          = current_task->nice;
    thread->quantum_max   = current_task->quantum_max;
    thread->quantum       = thread->quantum_max;
    thread->cpu_ticks     = 0;
    thread->sleep_count   = 0;
    thread->sleep_until   = 0;
    thread->wait_target_pid = 0;

    /* 8. Increment parent's thread count */
    leader->thread_count++;

    /* 9. Append to task list */
    thread->cpu_id = placement_cpu();
    task_publish(thread);

    serial_write_string("[CLONE] Thread TID ");
    char buf[16];
    itoa(thread->pid, buf, 10);
    serial_write_string(buf);
    serial_write_string(" created in TGID ");
    itoa(thread->tgid, buf, 10);
    serial_write_string(buf);
    serial_write_string("\n");

    return (int32_t)thread->pid;
}

/*
 * task_get_thread_count() - Get thread count for current process.
 */
uint32_t task_get_thread_count(void) {
    PREEMPT_GUARD;
    if (!current_task) return 0;
    struct task* leader = task_find_by_pid(current_task->tgid);
    return leader ? leader->thread_count : 1;
}

void task_set_uid(uint32_t uid, uint32_t gid) {
    if (current_task) {
        current_task->uid = uid;
        current_task->gid = gid;
    }
}

uint32_t task_get_uid(void) {
    if (!current_task) return 0;
    return current_task->uid;
}
