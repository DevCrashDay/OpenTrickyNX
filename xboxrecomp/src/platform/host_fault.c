/*
 * host_fault.c - trapped MMIO apertures on POSIX and the Switch.
 * See host_fault.h.
 *
 * Linux: a SIGSEGV/SIGBUS handler (sigaction, SA_SIGINFO) reads the faulting
 * instruction from the ucontext, x86-64 or AArch64.
 * Switch: host_fault_nx.S replaces libnx's __libnx_exception_entry; it calls
 * host_fault_nx_dispatch below with the kernel's exception frame and the
 * registers it saved, then returns from the exception with them.
 */
#if !defined(_WIN32)

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "host_fault.h"

#include <stdio.h>
#include <string.h>

#if defined(__SWITCH__)
#include <switch.h>
#include <unistd.h>
#else
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#endif

/* ===================================================================== */
/* Registry                                                              */
/* ===================================================================== */

#define HOST_TRAP_MAX 8

typedef struct {
    uintptr_t    host;
    uint32_t     guest_va;
    size_t       size;
    host_mmio_fn fn;
} host_trap;

static host_trap      s_traps[HOST_TRAP_MAX];
static volatile int   s_trap_count;
static unsigned long  s_decode_fail;

static const host_trap *trap_find(uintptr_t addr)
{
    int i, n = s_trap_count;
    for (i = 0; i < n; i++)
        if (addr - s_traps[i].host < s_traps[i].size) return &s_traps[i];
    return NULL;
}

bool host_trap_covers(uintptr_t addr) { return trap_find(addr) != NULL; }

bool host_trap_access(uintptr_t fault_addr, unsigned size, int is_write, uint64_t *val)
{
    const host_trap *t = trap_find(fault_addr);
    if (!t) return false;
    return t->fn(t->guest_va + (uint32_t)(fault_addr - t->host), size, is_write, val);
}

static bool host_fault_install(void);

static bool protect_none(void *host, size_t size)
{
#if defined(__SWITCH__)
    return R_SUCCEEDED(svcSetMemoryPermission(host, size, Perm_None));
#else
    return mprotect(host, size, PROT_NONE) == 0;
#endif
}

bool host_trap_register(void *host, uint32_t guest_va, size_t size, host_mmio_fn fn)
{
    host_trap *t;
    if (s_trap_count >= HOST_TRAP_MAX || !fn) return false;
    if (((uintptr_t)host & 0xFFF) || (size & 0xFFF)) return false;
    if (!host_fault_install()) return false;
    t = &s_traps[s_trap_count];
    t->host = (uintptr_t)host;
    t->guest_va = guest_va;
    t->size = size;
    t->fn = fn;
    __atomic_store_n(&s_trap_count, s_trap_count + 1, __ATOMIC_RELEASE);
    if (!protect_none(host, size)) {
        __atomic_store_n(&s_trap_count, s_trap_count - 1, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

static void report_decode_fail(uintptr_t pc, uintptr_t addr, const uint8_t *ip, int n)
{
    if (s_decode_fail++ < 8) {
        fprintf(stderr, "[HOSTFAULT] unhandled instruction at %p touching trapped %p:",
                (void *)pc, (void *)addr);
        for (int i = 0; i < n; i++) fprintf(stderr, " %02X", ip[i]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/* ===================================================================== */
/* AArch64 load/store decoder                                            */
/* ===================================================================== */

#if defined(__aarch64__)

/* Register access callbacks: the frame layouts differ per host. */
typedef struct {
    uint64_t *(*gpr)(void *ctx, int r);   /* x0..x30 */
    uint64_t *(*sp)(void *ctx);
    void *ctx;
} a64_regs;

static uint64_t a64_read(const a64_regs *rf, int r, int is_sp_ok)
{
    if (r == 31) return is_sp_ok ? *rf->sp(rf->ctx) : 0;
    return *rf->gpr(rf->ctx, r);
}

static void a64_write(const a64_regs *rf, int r, uint64_t v, int is_sp_ok)
{
    if (r == 31) { if (is_sp_ok) *rf->sp(rf->ctx) = v; return; }
    *rf->gpr(rf->ctx, r) = v;
}

/* Emulate one integer LDR/STR (all addressing forms except literal and
 * pair). The address is the fault address the CPU reported, so only the
 * register transfer and any base writeback need decoding. On success the
 * caller advances pc by 4. */
static bool a64_emulate(uint32_t insn, uintptr_t fault_addr, const a64_regs *rf)
{
    int size = (int)(insn >> 30);
    int v = (insn >> 26) & 1;
    int opc = (insn >> 22) & 3;
    int rn = (insn >> 5) & 31, rt = insn & 31;
    int bytes = 1 << size;
    int writeback = 0;
    int64_t wb_off = 0;
    int is_write, sext = 0, sext32 = 0;
    uint64_t val = 0;

    if (v) return false;                          /* SIMD/FP registers */
    if ((insn & 0x3B000000u) == 0x39000000u) {
        /* unsigned immediate: address already in fault_addr */
    } else if ((insn & 0x3B200000u) == 0x38000000u) {
        int idx = (insn >> 10) & 3;
        int64_t imm9 = (int64_t)((int32_t)(insn << 11) >> 23);
        if (idx == 1 || idx == 3) { writeback = idx; wb_off = imm9; }   /* post / pre */
    } else if ((insn & 0x3B200C00u) == 0x38200800u) {
        /* register offset */
    } else {
        return false;
    }

    if (opc == 0) is_write = 1;
    else if (opc == 1) is_write = 0;
    else {
        /* LDRS*: opc 2 -> 64-bit result, opc 3 -> 32-bit result */
        if (size == 3) return false;              /* PRFM / unallocated */
        if (size == 2 && opc == 3) return false;
        is_write = 0; sext = 1; sext32 = (opc == 3);
    }

    if (is_write) {
        val = a64_read(rf, rt, 0);
        if (bytes < 8) val &= (1ull << (bytes * 8)) - 1;
        if (!host_trap_access(fault_addr, (unsigned)bytes, 1, &val)) return false;
    } else {
        if (!host_trap_access(fault_addr, (unsigned)bytes, 0, &val)) return false;
        if (bytes < 8) val &= (1ull << (bytes * 8)) - 1;
        if (sext) {
            int sh = 64 - bytes * 8;
            val = (uint64_t)(((int64_t)(val << sh)) >> sh);
            if (sext32) val = (uint32_t)val;
        }
        a64_write(rf, rt, val, 0);
    }
    if (writeback) {
        uint64_t base = a64_read(rf, rn, 1);
        a64_write(rf, rn, base + (uint64_t)wb_off, 1);
    }
    return true;
}

#endif /* __aarch64__ */

/* ===================================================================== */
/* x86-64 MOV decoder (Linux PCs)                                        */
/* ===================================================================== */

#if defined(__x86_64__) && defined(__linux__)

static greg_t *x64_reg(ucontext_t *uc, int r)
{
    static const int map[16] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    };
    return &uc->uc_mcontext.gregs[map[r & 15]];
}

static int x64_modrm_len(const uint8_t *m)
{
    int mod = (m[0] >> 6) & 3, rm = m[0] & 7, len = 1;
    if (mod != 3 && rm == 4) len++;
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0) {
        if (rm == 5) len += 4;
        else if (rm == 4 && (m[1] & 7) == 5) len += 4;
    }
    return len;
}

/* The subset the Windows decoders handle: MOV r/m<->r, MOV r/m,imm,
 * MOVZX/MOVSX from memory. 8-bit registers without REX (AH..BH) are not
 * produced for these accesses by GCC and are rejected. */
static bool x64_emulate(ucontext_t *uc, uintptr_t addr)
{
    const uint8_t *ip = (const uint8_t *)uc->uc_mcontext.gregs[REG_RIP];
    int p = 0, has66 = 0, rex = 0;
    for (;;) {
        uint8_t b = ip[p];
        if (b == 0x66) { has66 = 1; p++; }
        else if (b == 0xF2 || b == 0xF3) p++;
        else if (b >= 0x40 && b <= 0x4F) { rex = b; p++; }
        else break;
    }
    const uint8_t *op = ip + p;
    int rex_w = rex & 8, rex_r = (rex & 4) ? 8 : 0;
    int size = rex_w ? 8 : has66 ? 2 : 4;
    uint64_t val;
    int len, reg;

    switch (op[0]) {
    case 0x88: case 0x89:
        if (op[0] == 0x88) { size = 1; if (!rex && ((op[1] >> 3) & 7) >= 4) return false; }
        reg = ((op[1] >> 3) & 7) | rex_r;
        len = 1 + x64_modrm_len(op + 1);
        val = (uint64_t)*x64_reg(uc, reg);
        if (size < 8) val &= (1ull << (size * 8)) - 1;
        if (!host_trap_access(addr, (unsigned)size, 1, &val)) return false;
        break;
    case 0xC6: case 0xC7: {
        int ml = x64_modrm_len(op + 1);
        if (op[0] == 0xC6) { size = 1; val = op[1 + ml]; len = 1 + ml + 1; }
        else if (size == 2) { val = *(const uint16_t *)(op + 1 + ml); len = 1 + ml + 2; }
        else { val = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml); len = 1 + ml + 4;
               if (size == 4) val &= 0xFFFFFFFFu; }
        if (!host_trap_access(addr, (unsigned)size, 1, &val)) return false;
        break;
    }
    case 0x8A: case 0x8B: {
        greg_t *dst;
        if (op[0] == 0x8A) { size = 1; if (!rex && ((op[1] >> 3) & 7) >= 4) return false; }
        reg = ((op[1] >> 3) & 7) | rex_r;
        len = 1 + x64_modrm_len(op + 1);
        if (!host_trap_access(addr, (unsigned)size, 0, &val)) return false;
        dst = x64_reg(uc, reg);
        if (size == 1) *dst = (greg_t)(((uint64_t)*dst & ~0xFFull) | (val & 0xFF));
        else if (size == 2) *dst = (greg_t)(((uint64_t)*dst & ~0xFFFFull) | (val & 0xFFFF));
        else if (size == 4) *dst = (greg_t)(val & 0xFFFFFFFFu);
        else *dst = (greg_t)val;
        break;
    }
    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7 || op[1] == 0xBE || op[1] == 0xBF) {
            int sz = (op[1] & 1) ? 2 : 1;
            int sx = op[1] >= 0xBE;
            int dsize = size;
            reg = ((op[2] >> 3) & 7) | rex_r;
            len = 2 + x64_modrm_len(op + 2);
            if (!host_trap_access(addr, (unsigned)sz, 0, &val)) return false;
            val &= (1ull << (sz * 8)) - 1;
            if (sx) val = (uint64_t)(((int64_t)(val << (64 - sz * 8))) >> (64 - sz * 8));
            if (dsize == 4) val &= 0xFFFFFFFFu;
            if (dsize == 2) {
                greg_t *d = x64_reg(uc, reg);
                *d = (greg_t)(((uint64_t)*d & ~0xFFFFull) | (val & 0xFFFF));
            } else {
                *x64_reg(uc, reg) = (greg_t)val;
            }
            break;
        }
        return false;
    default:
        return false;
    }
    uc->uc_mcontext.gregs[REG_RIP] += p + len;
    return true;
}

#endif /* x86-64 Linux */

/* ===================================================================== */
/* Linux: signal handler                                                 */
/* ===================================================================== */

#if !defined(__SWITCH__)

static struct sigaction s_prev_segv, s_prev_bus;

#if defined(__aarch64__) && defined(__linux__)
static uint64_t *lx_gpr(void *ctx, int r) { return (uint64_t *)&((ucontext_t *)ctx)->uc_mcontext.regs[r]; }
static uint64_t *lx_sp(void *ctx) { return (uint64_t *)&((ucontext_t *)ctx)->uc_mcontext.sp; }
#endif

static void chain(int sig, siginfo_t *si, void *uc_)
{
    struct sigaction *prev = (sig == SIGBUS) ? &s_prev_bus : &s_prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) { prev->sa_sigaction(sig, si, uc_); return; }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN && prev->sa_handler) {
        prev->sa_handler(sig);
        return;
    }
    /* Default action: re-raise with the default disposition. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_fault(int sig, siginfo_t *si, void *uc_)
{
    uintptr_t addr = (uintptr_t)si->si_addr;
    ucontext_t *uc = (ucontext_t *)uc_;
    if (host_trap_covers(addr)) {
#if defined(__x86_64__) && defined(__linux__)
        if (x64_emulate(uc, addr)) return;
        report_decode_fail((uintptr_t)uc->uc_mcontext.gregs[REG_RIP], addr,
                           (const uint8_t *)uc->uc_mcontext.gregs[REG_RIP], 8);
#elif defined(__aarch64__) && defined(__linux__)
        a64_regs rf = { lx_gpr, lx_sp, uc };
        uint32_t insn = *(const uint32_t *)uc->uc_mcontext.pc;
        if (a64_emulate(insn, addr, &rf)) { uc->uc_mcontext.pc += 4; return; }
        report_decode_fail((uintptr_t)uc->uc_mcontext.pc, addr,
                           (const uint8_t *)uc->uc_mcontext.pc, 4);
#else
        (void)uc;
#endif
    }
    chain(sig, si, uc_);
}

static bool host_fault_install(void)
{
    static int done;
    struct sigaction sa;
    if (done) return true;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_prev_segv) != 0) return false;
    sigaction(SIGBUS, &sa, &s_prev_bus);
    done = 1;
    return true;
}

#else /* __SWITCH__ */

/* ===================================================================== */
/* Switch: called from host_fault_nx.S                                   */
/* ===================================================================== */

/* x9..x29 as host_fault_nx.S saved them (index 0 = x9). */
typedef struct {
    ThreadExceptionFrameA64 *frame;
    uint64_t *saved;
} nx_ctx;

static uint64_t *nx_gpr(void *c, int r)
{
    nx_ctx *x = (nx_ctx *)c;
    if (r <= 8) return &x->frame->cpu_gprs[r];
    if (r == 30) return &x->frame->lr;
    return &x->saved[r - 9];
}

static uint64_t *nx_sp(void *c) { return &((nx_ctx *)c)->frame->sp; }

/* A crash that is not a trapped access: where it happened, in the log, before
 * the system ends the program. Addresses are also given as offsets into the
 * program (from _start), which addr2line maps to a function in the .elf. */
extern char _start[];
static void report_crash(uint32_t desc, const ThreadExceptionFrameA64 *frame, const uint64_t *saved)
{
    static int reported;
    const uintptr_t base = (uintptr_t)_start;
    int i;
    if (reported++) return;
    fprintf(stderr, "\n[SSX Tricky] crash: exception %u, pc 0x%llX (program +0x%llX), lr 0x%llX (program +0x%llX)\n"
                    "  fault address 0x%llX, esr 0x%llX, sp 0x%llX\n",
            desc, (unsigned long long)frame->elr_el1, (unsigned long long)(frame->elr_el1 - base),
            (unsigned long long)frame->lr, (unsigned long long)(frame->lr - base),
            (unsigned long long)frame->far, (unsigned long long)frame->esr,
            (unsigned long long)frame->sp);
    for (i = 0; i <= 8; i++)
        fprintf(stderr, "  x%d 0x%llX\n", i, (unsigned long long)frame->cpu_gprs[i]);
    for (i = 9; i <= 29; i++)
        fprintf(stderr, "  x%d 0x%llX\n", i, (unsigned long long)saved[i - 9]);
    /* A few return addresses from the stack (frame-pointer chain). */
    {
        const uint64_t *fp = (const uint64_t *)(uintptr_t)saved[29 - 9];
        for (i = 0; i < 16 && fp && ((uintptr_t)fp & 7) == 0; i++) {
            uint64_t ret = fp[1];
            if (!ret) break;
            fprintf(stderr, "  called from 0x%llX (program +0x%llX)\n",
                    (unsigned long long)ret, (unsigned long long)(ret - base));
            if ((const uint64_t *)(uintptr_t)fp[0] <= fp) break;
            fp = (const uint64_t *)(uintptr_t)fp[0];
        }
    }
    fflush(stderr);
    fsync(fileno(stderr));
}

/* Returns 1 when the exception was an access to a trapped range and has
 * been emulated (pc advanced); 0 to fall back to libnx's default. */
int host_fault_nx_dispatch(uint32_t desc, ThreadExceptionFrameA64 *frame, uint64_t *saved)
{
    nx_ctx c = { frame, saved };
    a64_regs rf = { nx_gpr, nx_sp, &c };
    uintptr_t addr;
    uint32_t insn;

    /* A data abort arrives as ThreadExceptionDesc_Other (EC 0x24). */
    addr = (uintptr_t)frame->far;
    if (desc != ThreadExceptionDesc_Other || !host_trap_covers(addr)) {
        report_crash(desc, frame, saved);
        return 0;
    }
    insn = *(const uint32_t *)frame->elr_el1;
    if (!a64_emulate(insn, addr, &rf)) {
        report_decode_fail((uintptr_t)frame->elr_el1, addr, (const uint8_t *)frame->elr_el1, 4);
        report_crash(desc, frame, saved);
        return 0;
    }
    frame->elr_el1 += 4;
    return 1;
}

static bool host_fault_install(void) { return true; }   /* the entry is linked in */

#endif /* __SWITCH__ */

#endif /* !_WIN32 */
