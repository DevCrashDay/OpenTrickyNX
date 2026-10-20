/* The bench's runtime: guest memory, the globals the lifted code expects, and
 * a driver that calls run(iters) and prints eax (run.sh). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/mman.h>
typedef struct { union { float f[4]; double d[2]; uint32_t u[4]; uint64_t q[2]; }; } xmm_dummy;
ptrdiff_t g_xbox_mem_offset;
#ifndef XBOX_GUEST_REGS_IN_CPU
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
__thread uint32_t g_seh_ebp, g_xbox_tib_va, g_cmp_a, g_cmp_b;
__thread int g_cmp_test, g_recomp_cf_in, g_ftol_fp_top, g_fpu_cmp, g_str_ne, g_fp_top;
__thread double g_ftol_arg, g_ftol_fp_stack[8], g_x87_st0, g_fp_stack[8];
__thread uint16_t g_x87_cw = 0x027F;
__thread uint64_t g_mm[8];
__thread xmm_dummy g_xmm[8];
#endif /* else xboxrecomp/src/kernel/xbox_guest_regs.c */
int g_x87_compat, g_x87_status_lo, g_x87_ret_mode, g_lowacc_enabled;
uint32_t g_xbox_fake_hdr_va;
volatile uint32_t g_icall_trace[4096], g_icall_trace_idx;
volatile uint64_t g_icall_count;
void xbox_lowaccess_log(unsigned va, void *ra) { (void)va; (void)ra; }
void recomp_icall_miss_log_at(uint32_t va, const char *f, int l) { fprintf(stderr, "icall miss %08x %s:%d\n", va, f, l); exit(3); }
extern const unsigned harness_funcs[];
extern void (*const harness_ptrs[])(void);
extern const int harness_nfuncs;
void (*recomp_resolve_icall(uint32_t va))(void)
{
    for (int i = 0; i < harness_nfuncs; i++) if (harness_funcs[i] == va) return harness_ptrs[i];
    return 0;
}
void sub_000112D0(void);
static void *drive(void *arg)
{
    g_esp = 0x00800000;
    g_esp -= 4; *(uint32_t *)(g_xbox_mem_offset + g_esp) = (uint32_t)(uintptr_t)arg;
    g_esp -= 4; *(uint32_t *)(g_xbox_mem_offset + g_esp) = 0;
    sub_000112D0();
    return (void *)(uintptr_t)g_eax;
}
int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 100;
    uint8_t *m = mmap(0, 0x01000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    FILE *f = fopen("image.bin", "rb");
    if (!f || fread(m, 1, 0x00310000, f) != 0x00310000) { fprintf(stderr, "image.bin?\n"); return 2; }
    fclose(f);
    g_xbox_mem_offset = (ptrdiff_t)m;
#ifdef XBOX_GUEST_REGS_IN_CPU
    uint32_t r = (uint32_t)(uintptr_t)xbox_guest_regs_call(drive, (void *)(uintptr_t)iters);
#else
    uint32_t r = (uint32_t)(uintptr_t)drive((void *)(uintptr_t)iters);
#endif
    printf("%08x\n", r);
    return 0;
}
