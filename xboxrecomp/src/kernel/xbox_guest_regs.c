/*
 * xbox_guest_regs.c - the per-thread state and the thread entry for
 * XBOX_GUEST_REGS (xbox_guest_regs.h): the guest registers in CPU registers.
 */
#ifdef XBOX_GUEST_REGS_IN_CPU
/* Here they live in w19-w25 instead (xbox_guest_regs.h). A thread that runs
 * guest code starts through this: x19-x25 zeroed like the __thread copies,
 * x26 = this thread's xbox_gtls, and the caller's values back on return,
 * since whatever started the thread (pthread, libnx) expects its
 * callee-saved registers kept. */
__thread struct xbox_gtls t_xbox_gtls = { .x87_cw = 0x027F };
struct xbox_gtls *xbox_gtls_self(void);
struct xbox_gtls *xbox_gtls_self(void) { return &t_xbox_gtls; }
__asm__(
    "    .text\n"
    "    .global xbox_guest_regs_call\n"
    "    .type   xbox_guest_regs_call, %function\n"
    "    .align  2\n"
    "xbox_guest_regs_call:\n"
    "    stp  x29, x30, [sp, #-96]!\n"
    "    mov  x29, sp\n"
    "    stp  x19, x20, [sp, #16]\n"
    "    stp  x21, x22, [sp, #32]\n"
    "    stp  x23, x24, [sp, #48]\n"
    "    stp  x25, x26, [sp, #64]\n"
    "    stp  x27, x0,  [sp, #80]\n"
    "    mov  x19, x1\n"
    "    bl   xbox_gtls_self\n"
    "    mov  x26, x0\n"
    "    mov  x0, x19\n"
    "    ldr  x16,      [sp, #88]\n"
    "    mov  x19, xzr\n"
    "    mov  x20, xzr\n"
    "    mov  x21, xzr\n"
    "    mov  x22, xzr\n"
    "    mov  x23, xzr\n"
    "    mov  x24, xzr\n"
    "    mov  x25, xzr\n"
    "    mov  x27, xzr\n"
    "    blr  x16\n"
    "    ldp  x19, x20, [sp, #16]\n"
    "    ldp  x21, x22, [sp, #32]\n"
    "    ldp  x23, x24, [sp, #48]\n"
    "    ldp  x25, x26, [sp, #64]\n"
    "    ldr  x27,      [sp, #80]\n"
    "    ldp  x29, x30, [sp], #96\n"
    "    ret\n"
    "    .size   xbox_guest_regs_call, . - xbox_guest_regs_call\n");
#endif
