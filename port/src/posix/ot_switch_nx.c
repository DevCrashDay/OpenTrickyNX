/*
 * ot_switch_nx.c -- Switch only: the settings file beside the .nro and the
 * CPU, GPU and memory clocks.
 *
 * The clock code follows flatout2-sw / fo2x by sp00nz (MIT,
 * https://github.com/sp00nznet/flatout2-sw, src/switch_nx.c): the same
 * services (clkrst on 8.0.0+, pcv before), the same caps and the same
 * courtesy to clock managers.
 */
#ifdef __SWITCH__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "compat/ot_posix.h"

/* ── SSX Tricky env.txt ─────────────────────────────────────────────
 *
 * KEY=VALUE per line beside the .nro, '#' starts a comment. Each line becomes
 * an environment variable before the game reads its settings, so it wins
 * over settings.ini (launcher.c fork_set keeps what the environment has):
 * XBOX_PERF=1, OT_CPU_MHZ=1020 and the like, without rebuilding. */
void ot_switch_env_load(void)
{
    char dir[1024], path[1100], line[512];
    FILE *f;
    int n = 0;
    if (!ot_exe_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s%sSSX Tricky env.txt", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
    f = fopen(path, "r");
    if (!f) {
        printf("[switch] no %s: default settings\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *k = line, *eq, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == '\r' || *k == '\n' || !*k) continue;
        if (!(eq = strchr(k, '='))) continue;
        *eq = '\0';
        for (e = eq; e > k && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';
        for (e = eq + 1; *e && *e != '\r' && *e != '\n'; e++) {}
        *e = '\0';
        if (!*k) continue;
        setenv(k, eq + 1, 1);
        printf("[switch] env %s=%s\n", k, eq + 1);
        n++;
    }
    fclose(f);
    printf("[switch] %d setting(s) from %s\n", n, path);
}

/* ── Clocks ─────────────────────────────────────────────────────────
 *
 * OT_CPU_MHZ / OT_GPU_MHZ / OT_MEM_MHZ: run the CPU, GPU and memory at that
 * clock (the closest the hardware lists, never above the request; capped at
 * 1785 / 921.6 / 1600 MHz). Unset: 1785 / 768 / 1600, the rates fo2x ships
 * docked; 0 = the system's (CPU 1020 MHz). The translated game is CPU-bound,
 * so the CPU clock matters most. More heat and battery than the system's.
 *
 * The system puts its own clocks back on dock/undock, after sleep and when
 * the game comes back from the HOME menu, so a thread applies them again
 * every second while the game has focus. When a clock manager runs (sys-clk,
 * Horizon OC), its profile rules and nothing is set here (OT_CLOCK_FORCE=1
 * overrides); a rate already at or above the request is left alone. The old
 * rates come back on exit. */
static struct {
    const char *env, *name;
    PcvModule mod;
    PcvModuleId id;
    u32 cap_hz, def_mhz, old_hz, set_hz;
    int open;
    ClkrstSession s;
} s_clk[3] = {
    { "OT_CPU_MHZ", "CPU",    PcvModule_CpuBus, PcvModuleId_CpuBus, 1785000000u, 1785 },
    { "OT_GPU_MHZ", "GPU",    PcvModule_GPU,    PcvModuleId_GPU,    921600000u,  768 },
    { "OT_MEM_MHZ", "memory", PcvModule_EMC,    PcvModuleId_EMC,    1600000000u, 1600 },
};
static int s_clk_rst = -1;                 /* 1 clkrst, 0 pcv, -1 not up */
static volatile int s_clk_stop;
static Thread s_clk_thread;
static int s_clk_thread_up;

static Result clk_get(int i, u32 *hz)
{
    return s_clk_rst ? clkrstGetClockRate(&s_clk[i].s, hz) : pcvGetClockRate(s_clk[i].mod, hz);
}

static Result clk_set(int i, u32 hz)
{
    return s_clk_rst ? clkrstSetClockRate(&s_clk[i].s, hz) : pcvSetClockRate(s_clk[i].mod, hz);
}

/* The highest listed rate not above want (want itself when there is no list). */
static u32 clk_pick(int i, u32 want)
{
    u32 rates[64], best = 0;
    s32 n = 0;
    PcvClockRatesListType type;
    Result rc = s_clk_rst
        ? clkrstGetPossibleClockRates(&s_clk[i].s, rates, 64, &type, &n)
        : pcvGetPossibleClockRates(s_clk[i].mod, rates, 64, &type, &n);
    if (R_FAILED(rc) || n <= 0 || type != PcvClockRatesListType_Discrete) return want;
    for (s32 k = 0; k < n; k++)
        if (rates[k] <= want && rates[k] > best) best = rates[k];
    return best ? best : want;
}

/* Is another process serving this name? Registering it fails with "already
 * registered" when one is. */
static int clk_service_running(const char *name)
{
    Handle h;
    Result rc = smRegisterService(&h, smEncodeName(name), false, 1);
    if (R_SUCCEEDED(rc)) {
        smUnregisterService(smEncodeName(name));
        svcCloseHandle(h);
        return 0;
    }
    return rc == MAKERESULT(21, 4);
}

static void clk_restore(void)
{
    int i;
    if (s_clk_rst < 0) return;
    s_clk_stop = 1;
    if (s_clk_thread_up) {
        threadWaitForExit(&s_clk_thread);
        threadClose(&s_clk_thread);
        s_clk_thread_up = 0;
    }
    for (i = 0; i < 3; i++)
        if (s_clk[i].set_hz && s_clk[i].old_hz) clk_set(i, s_clk[i].old_hz);
    for (i = 0; i < 3; i++)
        if (s_clk[i].open) clkrstCloseSession(&s_clk[i].s);
    if (s_clk_rst) clkrstExit();
    else pcvExit();
    s_clk_rst = -1;
}

static void clk_keeper(void *arg)
{
    unsigned logged = 0;
    (void)arg;
    while (!s_clk_stop) {
        svcSleepThread(1000000000LL);
        if (s_clk_stop || appletGetFocusState() != AppletFocusState_InFocus) continue;
        for (int i = 0; i < 3; i++) {
            u32 hz = 0;
            if (!s_clk[i].set_hz || R_FAILED(clk_get(i, &hz)) || hz == s_clk[i].set_hz) continue;
            if (hz > s_clk[i].set_hz) {
                printf("[clock] %s raised to %u MHz by someone else: left to them\n",
                       s_clk[i].name, hz / 1000000u);
                s_clk[i].set_hz = 0;
                continue;
            }
            clk_set(i, s_clk[i].set_hz);
            if (logged++ < 8)
                printf("[clock] %s was reset to %u MHz, back to %u MHz\n", s_clk[i].name,
                       hz / 1000000u, s_clk[i].set_hz / 1000000u);
        }
    }
}

void ot_switch_clocks(void)
{
    u32 want[3];
    int i, any = 0;
    for (i = 0; i < 3; i++) {
        const char *e = getenv(s_clk[i].env);
        double mhz = (e && *e) ? atof(e) : s_clk[i].def_mhz;
        want[i] = mhz > 0 ? (u32)(mhz * 1e6 + 0.5) : 0;
        if (want[i] > s_clk[i].cap_hz) want[i] = s_clk[i].cap_hz;
        any |= want[i] != 0;
    }
    if (!any) {
        printf("[clock] OT_*_MHZ=0: the system's clocks\n");
        return;
    }
    {
        const char *f = getenv("OT_CLOCK_FORCE");
        const char *tool = clk_service_running("hoc:clk") ? "Horizon OC"
                         : clk_service_running("sys:clk") ? "sys-clk" : NULL;
        if (tool && !(f && *f == '1')) {
            printf("[clock] %s is running: clocks are left to its profile "
                   "(OT_CLOCK_FORCE=1 overrides)\n", tool);
            return;
        }
    }
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) s_clk_rst = 1;
    else if (R_SUCCEEDED(pcvInitialize())) s_clk_rst = 0;
    else {
        printf("[clock] neither clkrst nor pcv is available: clocks unchanged\n");
        return;
    }
    for (i = 0; i < 3; i++) {
        u32 hz, now = 0;
        Result rc;
        if (!want[i]) continue;
        if (s_clk_rst) {
            if (R_FAILED(rc = clkrstOpenSession(&s_clk[i].s, s_clk[i].id, 3))) {
                printf("[clock] %s: no clkrst session (0x%X)\n", s_clk[i].name, rc);
                continue;
            }
            s_clk[i].open = 1;
        }
        clk_get(i, &s_clk[i].old_hz);
        hz = clk_pick(i, want[i]);
        if (s_clk[i].old_hz >= hz) {
            printf("[clock] %s already at %u MHz (asked %u): left alone\n", s_clk[i].name,
                   s_clk[i].old_hz / 1000000u, want[i] / 1000000u);
            continue;
        }
        rc = clk_set(i, hz);
        clk_get(i, &now);
        if (R_SUCCEEDED(rc)) s_clk[i].set_hz = now ? now : hz;
        printf("[clock] %s %u -> %u MHz (asked %u)%s\n", s_clk[i].name,
               s_clk[i].old_hz / 1000000u, now / 1000000u, want[i] / 1000000u,
               R_FAILED(rc) ? " -- refused" : "");
    }
    atexit(clk_restore);
    /* High priority: at the lowest, busy game threads on its core starve it. */
    if (R_SUCCEEDED(threadCreate(&s_clk_thread, clk_keeper, NULL, NULL, 0x4000, 0x2C, -2))
        && R_SUCCEEDED(threadStart(&s_clk_thread)))
        s_clk_thread_up = 1;
}
#endif /* __SWITCH__ */
