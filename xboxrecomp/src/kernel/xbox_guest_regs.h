/*
 * xbox_guest_regs.h - the guest x86 registers in host CPU registers
 *
 * The translated code reads and writes eax..esp as the globals g_eax..g_esp.
 * By default they are __thread variables (xbox_memory_layout.c): every access
 * is a load or store through the thread pointer, and since a guest memory store
 * may alias any of them, the compiler reloads them after each one. On the
 * Switch that is most of the game thread's time.
 *
 * With XBOX_GUEST_REGS (GCC on AArch64) they are global register variables
 * instead, pinned to the callee-saved registers w19-w25. Each OS thread has its
 * own register file, so they stay per-thread like the __thread copies, and a
 * register move costs nothing. The build force-includes this header into every
 * C file of the program (port/posix.cmake), so none of our code uses those
 * registers for anything else; the libraries we link (libnx, newlib, SDL, Mesa)
 * see ordinary callee-saved registers and preserve them.
 *
 * The rest of the guest's per-thread state (the x87 stack, the split-compare
 * operands, ...) moves into one __thread struct, xbox_gtls. Its old names are
 * macros for its fields: host code reaches it as a __thread variable, the
 * translated code through x26, which holds its address on a thread running
 * guest code (recomp_types.h switches XBOX_GTLS over). That is one load with
 * an offset instead of asking for the thread pointer every time, which on the
 * Switch is a function call (-mtp=soft). The x87 stack top, touched by every
 * floating-point instruction, gets a register of its own: w27.
 *
 * What changes with them:
 *  - a thread that runs guest code must start through xbox_guest_regs_call(),
 *    which zeroes the eight registers (the __thread copies started at 0), sets
 *    x26 and restores the caller's x19-x27 for whatever started the thread;
 *  - longjmp() restores them to their setjmp() values;
 *  - their address cannot be taken.
 */
#ifndef XBOX_GUEST_REGS_H
#define XBOX_GUEST_REGS_H

#if defined(XBOX_GUEST_REGS) && defined(__aarch64__) && defined(__GNUC__) && !defined(__clang__) \
    && !defined(__cplusplus) && !defined(__ASSEMBLER__)
#define XBOX_GUEST_REGS_IN_CPU 1

/* No system header here: this is included ahead of a file's own
 * _GNU_SOURCE and the like. __UINT32_TYPE__ is uint32_t's type. */
register __UINT32_TYPE__ g_eax __asm__("w19");
register __UINT32_TYPE__ g_ecx __asm__("w20");
register __UINT32_TYPE__ g_edx __asm__("w21");
register __UINT32_TYPE__ g_ebx __asm__("w22");
register __UINT32_TYPE__ g_esi __asm__("w23");
register __UINT32_TYPE__ g_edi __asm__("w24");
register __UINT32_TYPE__ g_esp __asm__("w25");
register struct xbox_gtls *g_xbox_gtls __asm__("x26");   /* only on guest threads */
register int g_fp_top __asm__("w27");                    /* the x87 stack top */

/* fn(arg) with the guest registers zeroed and x26 = this thread's xbox_gtls,
 * and the caller's x19-x27 back afterwards (xbox_memory_layout.c). */
void *xbox_guest_regs_call(void *(*fn)(void *), void *arg);

#define XBOX_GTLS_ICALL_N 2048u

/* What were separate __thread variables (recomp_types.h describes each). */
struct xbox_gtls {
    double fp_stack[8];
    double x87_st0;
    double ftol_arg;
    double ftol_fp_stack[8];
    int ftol_fp_top, fpu_cmp, recomp_cf_in, str_ne, cmp_test;
    __UINT32_TYPE__ seh_ebp, xbox_tib_va, cmp_a, cmp_b;
    __UINT16_TYPE__ x87_cw;
    __UINT64_TYPE__ mm[8];
    unsigned char xmm[8][16] __attribute__((aligned(16)));  /* recomp_xmm_t */
    /* recomp_manual.c's indirect-call target cache, reached through x26
     * (it only runs on guest threads) instead of the thread pointer. */
    struct { __UINT32_TYPE__ va; void (*fn)(void); } icall[XBOX_GTLS_ICALL_N];
};
extern __thread struct xbox_gtls t_xbox_gtls;

#define XBOX_GTLS (&t_xbox_gtls)            /* the translated code: g_xbox_gtls */
#define g_fp_stack      (XBOX_GTLS->fp_stack)
#define g_x87_st0       (XBOX_GTLS->x87_st0)
#define g_ftol_arg      (XBOX_GTLS->ftol_arg)
#define g_ftol_fp_stack (XBOX_GTLS->ftol_fp_stack)
#define g_ftol_fp_top   (XBOX_GTLS->ftol_fp_top)
#define g_fpu_cmp       (XBOX_GTLS->fpu_cmp)
#define g_recomp_cf_in  (XBOX_GTLS->recomp_cf_in)
#define g_str_ne        (XBOX_GTLS->str_ne)
#define g_cmp_test      (XBOX_GTLS->cmp_test)
#define g_seh_ebp       (XBOX_GTLS->seh_ebp)
#define g_xbox_tib_va   (XBOX_GTLS->xbox_tib_va)
#define g_cmp_a         (XBOX_GTLS->cmp_a)
#define g_cmp_b         (XBOX_GTLS->cmp_b)
#define g_x87_cw        (XBOX_GTLS->x87_cw)
#define g_mm            (XBOX_GTLS->mm)
#define g_xmm           (*(recomp_xmm_t (*)[8])XBOX_GTLS->xmm)

#endif
#endif /* XBOX_GUEST_REGS_H */
