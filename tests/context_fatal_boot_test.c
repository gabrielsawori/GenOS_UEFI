/* Negative controls: kernel UD is native; platform CPL3 frames are injected
 * to prove they cannot be mistaken for recoverable application exceptions. */
#include "../kernel/task.c"
#include "../drivers/io.h"

struct task* __real_create_user_task(const uint8_t*, size_t);
struct task* __wrap_create_user_task(const uint8_t* data, size_t size) {
    serial_write_string("[CONTEXT FATAL ARMED]\n");
#if CONTEXT_FATAL_PROBE == 1
    (void)data; (void)size;
    asm volatile("ud2");
#else
    struct task* user = __real_create_user_task(data, size);
    if (!user) {
        serial_write_string("[CONTEXT TEST FAIL] negative fixture allocation\n");
        outl(0xf4, 1);
        for (;;) asm volatile("cli; hlt");
    }
    current_task = user;
    struct registers regs = user->regs;
#if CONTEXT_FATAL_PROBE == 2
    regs.vector = 2;
#elif CONTEXT_FATAL_PROBE == 3
    regs.vector = 18;
#elif CONTEXT_FATAL_PROBE == 4
    regs.vector = 14;
    regs.err_code = 12; /* User access with a reserved page-table bit. */
#endif
    isr_handler(&regs);
#endif
    serial_write_string("[CONTEXT TEST FAIL] fatal handler returned\n");
    outl(0xf4, 1);
    for (;;) asm volatile("cli; hlt");
}
