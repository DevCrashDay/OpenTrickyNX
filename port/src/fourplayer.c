/*
 * fourplayer -- four-player split screen, a first test. See fourplayer.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "recomp/recomp_types.h"
#include "fourplayer.h"

void sub_000AC2AD(void);    /* level: cameras per view (the part 0xAC240 falls into) */
void sub_000AB270(void);    /* level: view rectangles (thiscall) */
void sub_000AC9B0(void);    /* level: init (thiscall, ret) */
void sub_000AC960(void);    /* level: deleting destructor (thiscall, ret 4) */
void sub_000697A0(void);    /* setup: pad port -> player slot (thiscall, ret 4) */
void sub_0007B450(void);    /* Camera: set mode (thiscall, ret 4) */
void sub_000E2450(void);    /* LensFX: Draw (thiscall, ret) */
void sub_000D2C20(void);    /* Overlays: draw for the current view (thiscall, ret) */
void sub_000CB470(void);    /* Overlays: arm a view's entry (thiscall, ret 4) */
void sub_000C3950(void);    /* race HUD: layout mode for a view (thiscall, ret 4) */

#define APP_GLOBAL   0x001E3C7Cu
#define SETUP        0x001DBF50u
#define SLOT_PORT    (SETUP + 0x3344u)        /* 4 slots, 0x44 each: pad port (signed byte) */
#define RIDER_COUNT  0x001DE8FCu              /* = SETUP + 0x29AC */
#define RIDER_REC    0x001DE900u              /* = SETUP + 0x29B0, 0x98 each, 6 */
#define REC_PORT     0x7Du
#define CAM_MODE_P2  0x001DD868u
#define HUD_MODE     0x001E9418u

int g_fourplayer;

/* Computer rider records turned into humans for this level, and the setup
 * slot each was given (its 0x44 bytes as they were). */
static struct { int rec, port, slot; uint8_t save[0x44]; } s_inj[2];
static int s_ninj;

static uint32_t level(void)
{
    uint32_t g = MEM32(APP_GLOBAL);
    return g ? MEM32(g + 0x72Cu) : 0;
}

/* Views of the level running, 0 outside a race. */
static uint32_t views(uint32_t lvl) { return lvl ? MEM32(lvl + 0x298u) : 0; }

int fourplayer_skip_view(void)
{
    uint32_t lvl;
    if (!g_fourplayer || views(lvl = level()) <= 2) return 0;
    return MEM32(lvl + 0x290u) >= 2;
}

/* ── Joining ───────────────────────────────────────────────────────── */

static void restore(void)
{
    int k;
    uint32_t b;
    for (k = 0; k < s_ninj; k++) {
        uint32_t p = RIDER_REC + (uint32_t)s_inj[k].rec * 0x98u + REC_PORT;
        if ((int8_t)MEM8(p) == s_inj[k].port) MEM8(p) = 0xFF;
        for (b = 0; b < 0x44u; b++) MEM8(SLOT_PORT + (uint32_t)s_inj[k].slot * 0x44u + b) = s_inj[k].save[b];
    }
    if (s_ninj) { fprintf(stderr, "[4P] %d extra player(s) back to computer riders\n", s_ninj); fflush(stderr); }
    s_ninj = 0;
}

static void inject(void)
{
    uint32_t g = MEM32(APP_GLOBAL), mgr = g ? MEM32(g + 0x28u) : 0, ports = 0, slots = 0, npad, i;
    int n = (int)MEM32(RIDER_COUNT), humans = 0, r, k, tmpl = -1;
    if (n < 0 || n > 6 || !mgr) return;
    for (r = 0; r < n; r++) {
        int p = (int8_t)MEM8(RIDER_REC + (uint32_t)r * 0x98u + REC_PORT);
        if (p >= 0 && p < 4) { humans++; ports |= 1u << p; }
    }
    if (humans < 2) return;                 /* only a two-player race grows */
    for (k = 0; k < 4; k++)
        if ((int8_t)MEM8(SLOT_PORT + (uint32_t)k * 0x44u) >= 0) { slots |= 1u << k; tmpl = k; }
    if (tmpl < 0) return;
    npad = MEM32(mgr + 0x14u);
    for (i = 0; i < npad && i < 4u && humans < 4; i++) {
        uint32_t pad = MEM32(mgr + 0x18u + 4u * i), port;
        if (!pad || (port = MEM32(pad + 0x34u)) >= 4u || !MEM32(pad + 0x38u) || (ports & (1u << port)))
            continue;                       /* no such pad, not connected, or already playing */
        for (r = n - 1; r >= 0; r--)
            if ((int8_t)MEM8(RIDER_REC + (uint32_t)r * 0x98u + REC_PORT) < 0) break;
        for (k = 0; k < 4 && (slots & (1u << k)); k++) {}
        if (r < 0 || k == 4) break;         /* no computer rider or no slot number left */
        MEM8(RIDER_REC + (uint32_t)r * 0x98u + REC_PORT) = (uint8_t)port;
        s_inj[s_ninj].rec = r; s_inj[s_ninj].port = (int)port; s_inj[s_ninj].slot = k;
        /* The slot gets the last player's profile, so code reading a slot's
         * data finds some; its port stays -1, the frontend leaves it alone. */
        {
            uint32_t to = SLOT_PORT + (uint32_t)k * 0x44u, from = SLOT_PORT + (uint32_t)tmpl * 0x44u, b;
            for (b = 0; b < 0x44u; b++) s_inj[s_ninj].save[b] = MEM8(to + b);
            for (b = 1; b < 0x44u; b++) MEM8(to + b) = MEM8(from + b);
        }
        s_ninj++; humans++;
        ports |= 1u << port; slots |= 1u << k;
        fprintf(stderr, "[4P] pad %u joins as rider %d (slot %d)\n", port, r, k);
    }
    fflush(stderr);
}

/* Level init: before the roster, the vibration options and the replay
 * count the humans. */
void hook_fourplayer_000AC9B0(void)
{
    if (g_fourplayer) { restore(); inject(); }
    sub_000AC9B0();
}

void hook_fourplayer_000AC960(void)
{
    if (g_fourplayer) restore();
    sub_000AC960();
}

/* An extra player's port is in no setup slot: its own free slot number. */
void hook_fourplayer_000697A0(void)
{
    int port = (int)MEM32(g_esp + 4u), k;
    sub_000697A0();
    if (g_eax != 0xFFFFFFFFu) return;
    for (k = 0; k < s_ninj; k++)
        if (s_inj[k].port == port) { g_eax = (uint32_t)s_inj[k].slot; return; }
}

/* ── Views ─────────────────────────────────────────────────────────── */

static void cam_mode(uint32_t cam, uint32_t mode)
{
    uint32_t sv[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_seh_ebp };
    uint32_t esp0 = g_esp;
    g_ecx = cam;
    g_esp -= 4; MEM32(g_esp) = mode;
    g_esp -= 4; MEM32(g_esp) = 0;          /* dummy return address */
    sub_0007B450();
    g_esp = esp0;
    g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3]; g_esi = sv[4]; g_edi = sv[5]; g_seh_ebp = sv[6];
}

/* esi = the level, eax = its CamController; the view count is set. */
void hook_fourplayer_000AC2AD(void)
{
    uint32_t lvl = g_esi, ai, n, i;
    if (!g_fourplayer || !(ai = MEM32(lvl + 0x1Cu)) || MEM32(ai + 0x7Cu) < 3u) { sub_000AC2AD(); return; }
    n = 4;
    MEM32(lvl + 0x298u) = n;
    MEM32(lvl + 0x294u) = n;
    sub_000AC2AD();
    for (i = 2; i < n; i++) {
        uint32_t cam = MEM32(lvl + 0xA0u + (i << 7));
        if (cam) cam_mode(cam, MEM32(CAM_MODE_P2));
    }
    fprintf(stderr, "[4P] %u humans, %u views\n", MEM32(ai + 0x7Cu), n);
    fflush(stderr);
}

/* 2x2 with the title's 2-pixel gaps; it lays out one view to give the
 * screen size. */
void hook_fourplayer_000AB270(void)
{
    uint32_t lvl = g_ecx, n = MEM32(lvl + 0x298u), i;
    int32_t w, h, hw, hh;
    if (!g_fourplayer || n < 3 || n > 4) { sub_000AB270(); return; }
    MEM32(lvl + 0x298u) = 1;
    sub_000AB270();
    MEM32(lvl + 0x298u) = n;
    w = (int32_t)MEM32(lvl + 0x98u); h = (int32_t)MEM32(lvl + 0x9Cu);
    hw = w / 2; hh = h / 2;
    for (i = 0; i < 4; i++) {
        uint32_t r = lvl + 0x90u + (i << 7);
        MEM32(r + 0x0u) = (uint32_t)((i & 1u) ? hw + 2 : 0);
        MEM32(r + 0x4u) = (uint32_t)((i & 2u) ? hh + 2 : 0);
        MEM32(r + 0x8u) = (uint32_t)(hw - 2);
        MEM32(r + 0xCu) = (uint32_t)(hh - 2);
    }
}

/* Race HUD layout mode: the whole-screen layout in each quarter. */
void hook_fourplayer_000C3950(void)
{
    if (!g_fourplayer || views(level()) <= 2) { sub_000C3950(); return; }
    MEM32(HUD_MODE) = 0;
    g_esp += 8;                            /* ret 4 */
}

/* ── Structures of two views ───────────────────────────────────────── */

/* LensFX: views 3-4 skip it, views 1-2 record as two. */
void hook_fourplayer_000E2450(void)
{
    uint32_t lvl = level(), n = views(lvl);
    if (!g_fourplayer || n <= 2) { sub_000E2450(); return; }
    if (MEM32(lvl + 0x290u) >= 2) { g_esp += 4; return; }
    MEM32(lvl + 0x298u) = 2;
    sub_000E2450();
    MEM32(lvl + 0x298u) = n;
}

/* Overlays: no entry for views 3-4. */
void hook_fourplayer_000D2C20(void)
{
    if (!fourplayer_skip_view()) { sub_000D2C20(); return; }
    g_esp += 4;
}

void hook_fourplayer_000CB470(void)
{
    if (!g_fourplayer || views(level()) <= 2 || (int32_t)MEM32(g_esp + 4u) < 2) { sub_000CB470(); return; }
    g_esp += 8;
}

void fourplayer_init(void)
{
    const char *e = getenv("OT_FOUR_PLAYER");
    g_fourplayer = e && e[0] == '1';
    if (g_fourplayer) { fprintf(stderr, "[4P] OT_FOUR_PLAYER=1: up to four players in split screen\n"); fflush(stderr); }
}

void (*fourplayer_lookup(unsigned int xbox_va))(void)
{
    if (!g_fourplayer) return 0;
    if (xbox_va == 0x000AC2ADu) return hook_fourplayer_000AC2AD;
    if (xbox_va == 0x000AB270u) return hook_fourplayer_000AB270;
    if (xbox_va == 0x000AC9B0u) return hook_fourplayer_000AC9B0;
    if (xbox_va == 0x000AC960u) return hook_fourplayer_000AC960;
    if (xbox_va == 0x000697A0u) return hook_fourplayer_000697A0;
    if (xbox_va == 0x000C3950u) return hook_fourplayer_000C3950;
    if (xbox_va == 0x000E2450u) return hook_fourplayer_000E2450;
    if (xbox_va == 0x000D2C20u) return hook_fourplayer_000D2C20;
    if (xbox_va == 0x000CB470u) return hook_fourplayer_000CB470;
    return 0;
}
