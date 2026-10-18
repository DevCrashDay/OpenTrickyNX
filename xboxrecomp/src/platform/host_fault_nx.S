/*
 * host_fault_nx.S - Switch user-exception entry for trapped MMIO.
 *
 * Replaces libnx's weak __libnx_exception_entry. The kernel enters here with
 * x0 = exception type and x1 = the saved frame (x0-x8, lr, sp, pc, pstate,
 * esr, far); every other register still holds its value from the faulting
 * instruction. We save x9-x29 and the FP/SIMD state, let
 * host_fault_nx_dispatch (host_fault.c) emulate the access, restore what it
 * may have changed and resume with svcReturnFromException(0), which reloads
 * the frame (including the advanced pc).
 *
 * Anything that is not a trapped access returns 0xF801 ("not handled"), the
 * same as libnx's own entry without a handler: the kernel then treats it as
 * an ordinary crash. libnx's crash dump (__libnx_exception_handler) is not
 * used by this program.
 */
    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type   __libnx_exception_entry, %function
    .align  2
__libnx_exception_entry:
    cbz     x1, 2f

    /* 21 GPRs (x9-x29) + x0/x1 + fpcr/fpsr = 25 slots -> 26 for alignment,
     * then 32 Q registers. */
    sub     sp, sp, #(26 * 8 + 32 * 16)
    stp     x9,  x10, [sp, #0]
    stp     x11, x12, [sp, #16]
    stp     x13, x14, [sp, #32]
    stp     x15, x16, [sp, #48]
    stp     x17, x18, [sp, #64]
    stp     x19, x20, [sp, #80]
    stp     x21, x22, [sp, #96]
    stp     x23, x24, [sp, #112]
    stp     x25, x26, [sp, #128]
    stp     x27, x28, [sp, #144]
    str     x29,      [sp, #160]
    stp     x0,  x1,  [sp, #168]
    mrs     x9, fpcr
    mrs     x10, fpsr
    stp     x9,  x10, [sp, #184]
    add     x9, sp, #(26 * 8)
    stp     q0,  q1,  [x9, #0]
    stp     q2,  q3,  [x9, #32]
    stp     q4,  q5,  [x9, #64]
    stp     q6,  q7,  [x9, #96]
    stp     q8,  q9,  [x9, #128]
    stp     q10, q11, [x9, #160]
    stp     q12, q13, [x9, #192]
    stp     q14, q15, [x9, #224]
    stp     q16, q17, [x9, #256]
    stp     q18, q19, [x9, #288]
    stp     q20, q21, [x9, #320]
    stp     q22, q23, [x9, #352]
    stp     q24, q25, [x9, #384]
    stp     q26, q27, [x9, #416]
    stp     q28, q29, [x9, #448]
    stp     q30, q31, [x9, #480]

    mov     x2, sp                  /* saved x9..x29 */
    bl      host_fault_nx_dispatch
    mov     w11, w0

    add     x9, sp, #(26 * 8)
    ldp     q0,  q1,  [x9, #0]
    ldp     q2,  q3,  [x9, #32]
    ldp     q4,  q5,  [x9, #64]
    ldp     q6,  q7,  [x9, #96]
    ldp     q8,  q9,  [x9, #128]
    ldp     q10, q11, [x9, #160]
    ldp     q12, q13, [x9, #192]
    ldp     q14, q15, [x9, #224]
    ldp     q16, q17, [x9, #256]
    ldp     q18, q19, [x9, #288]
    ldp     q20, q21, [x9, #320]
    ldp     q22, q23, [x9, #352]
    ldp     q24, q25, [x9, #384]
    ldp     q26, q27, [x9, #416]
    ldp     q28, q29, [x9, #448]
    ldp     q30, q31, [x9, #480]
    ldp     x9,  x10, [sp, #184]
    msr     fpcr, x9
    msr     fpsr, x10
    cbz     w11, 1f

    ldp     x9,  x10, [sp, #0]
    ldp     x11, x12, [sp, #16]
    ldp     x13, x14, [sp, #32]
    ldp     x15, x16, [sp, #48]
    ldp     x17, x18, [sp, #64]
    ldp     x19, x20, [sp, #80]
    ldp     x21, x22, [sp, #96]
    ldp     x23, x24, [sp, #112]
    ldp     x25, x26, [sp, #128]
    ldp     x27, x28, [sp, #144]
    ldr     x29,      [sp, #160]
    add     sp, sp, #(26 * 8 + 32 * 16)
    mov     w0, #0
    b       svcReturnFromException

1:  /* not ours: restore and report it unhandled */
    ldp     x9,  x10, [sp, #0]
    ldp     x11, x12, [sp, #16]
    ldp     x13, x14, [sp, #32]
    ldp     x15, x16, [sp, #48]
    ldp     x17, x18, [sp, #64]
    ldp     x19, x20, [sp, #80]
    ldp     x21, x22, [sp, #96]
    ldp     x23, x24, [sp, #112]
    ldp     x25, x26, [sp, #128]
    ldp     x27, x28, [sp, #144]
    ldr     x29,      [sp, #160]
    add     sp, sp, #(26 * 8 + 32 * 16)
2:  mov     w0, #0xF801
    b       svcReturnFromException
    .size   __libnx_exception_entry, . - __libnx_exception_entry
