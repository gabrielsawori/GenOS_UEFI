#include "input.h"
#define INPUT_CAPACITY 256U
static struct genos_input_event events[INPUT_CAPACITY];
static uint32_t head, tail;
static uint64_t dropped;
void input_push(const struct genos_input_event* event) {
    uint32_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);
    uint32_t next = (h + 1) % INPUT_CAPACITY;
    if (next == __atomic_load_n(&tail, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    events[h] = *event;
    __atomic_store_n(&head, next, __ATOMIC_RELEASE);
}
int input_peek(struct genos_input_event* event) {
    uint32_t t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
    if (t == __atomic_load_n(&head, __ATOMIC_ACQUIRE)) return 0;
    *event = events[t];
    return 1;
}
void input_consume(void) {
    uint32_t t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
    if (t != __atomic_load_n(&head, __ATOMIC_ACQUIRE))
        __atomic_store_n(&tail, (t + 1) % INPUT_CAPACITY, __ATOMIC_RELEASE);
}
uint64_t input_dropped(void) { return __atomic_load_n(&dropped, __ATOMIC_RELAXED); }
