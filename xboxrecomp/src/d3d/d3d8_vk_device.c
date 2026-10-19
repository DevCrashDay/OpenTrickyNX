/*
 * d3d8_vk_device.c -- the Xbox D3D8 device on Vulkan (XBOX_VULKAN).
 *
 * d3d8_gl_device.c on Vulkan: the title renders into an offscreen scene
 * target at the configured render size; every present scales that image,
 * at its display aspect and through the DAC palette when the title loaded
 * one, into the swapchain. Video frames written by the title's movie player
 * into the guest framebuffer are uploaded into the scene target at present
 * time, with the same freshness rules as the other backends.
 *
 * Unlike the GL backend the scene is stored top-down, as D3D's: viewports,
 * scissors, clear rectangles and readbacks need no flip.
 *
 * On the Switch there is no SDL window: the surface is the default NWindow
 * (VK_NN_vi_surface) and the applet loop stands in for SDL_QUIT.
 *
 * Everything that touches Vulkan runs on the thread that created the device.
 */
#include "d3d8_vk_internal.h"
#include "d3d8_gpuprof.h"
#include "../kernel/xbox_perf.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(__SWITCH__)
#include <switch.h>
#else
#include <unistd.h>
#endif

VkdState g_vkd;

static IDirect3DDevice8 g_device;
static const IDirect3DDevice8Vtbl g_device_vtbl;
static SDL_Window    *s_win;            /* NULL on the Switch */

static void scene_clear_all(void);

/* ================================================================
 * Host configuration (set by the launcher, read on the render thread)
 * ================================================================ */
static UINT   s_cfg_render_w, s_cfg_render_h;
static int    s_cfg_widescreen, s_cfg_fullscreen;
static double s_cfg_aspect;
static volatile LONG s_present_box;
static double s_present_box_shape = 16.0 / 9.0;
static float  s_point_zoom = 1.0f;
static volatile LONG s_want_msaa = 1;
static volatile LONG s_show_fps;
static volatile LONG s_frames;
static int    s_fullscreen;
static volatile LONG s_req_fullscreen = -1;      /* applied on the render thread */
static volatile LONG s_req_client_w, s_req_client_h;
static D3D8HostUiHooks s_ui_hooks;
static int    s_post_on, s_smaa, s_post_split;

/* Texture decode time, filled in by the push-buffer translator. */
volatile long g_tex_uploads_log = 0;
volatile double g_tex_upload_ms = 0;

void d3d8_SetHostDisplay(unsigned render_w, unsigned render_h, int widescreen, int fullscreen)
{
    s_cfg_render_w = render_w;
    s_cfg_render_h = render_h;
    s_cfg_widescreen = widescreen ? 1 : 0;
    s_cfg_fullscreen = fullscreen ? 1 : 0;
}

void d3d8_SetHostAspect(double aspect) { s_cfg_aspect = aspect > 1.0 ? aspect : 0.0; }

double d3d8_HostAspect(void)
{
    if (s_cfg_aspect > 0.0) return s_cfg_aspect;
    return s_cfg_widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
}

void d3d8_SetPresentBox(int on) { InterlockedExchange(&s_present_box, on ? 1 : 0); }
void d3d8_SetPresentBoxShape(double shape) { s_present_box_shape = shape > 1.0 ? shape : 16.0 / 9.0; }
void  d3d8_SetPointZoom(float zoom) { s_point_zoom = zoom > 0.0f ? zoom : 1.0f; }
float d3d8_PointZoom(void) { return s_point_zoom; }

void d3d8_SetHostUiHooks(const D3D8HostUiHooks *hooks)
{
    if (hooks) s_ui_hooks = *hooks;
    else memset(&s_ui_hooks, 0, sizeof s_ui_hooks);
}

void d3d8_SetMsaa(int samples)
{
    InterlockedExchange(&s_want_msaa, (samples == 2 || samples == 4 || samples == 8) ? samples : 1);
}
int  d3d8_GetMsaa(void)          { return (int)s_want_msaa; }
void d3d8_SetShowFps(int on)     { InterlockedExchange(&s_show_fps, on ? 1 : 0); }
int  d3d8_GetShowFps(void)       { return (int)s_show_fps; }
int  d3d8_HostIsWidescreen(void) { return s_cfg_widescreen; }
int  d3d8_HostIsFullscreen(void) { return s_fullscreen; }

/* Post chain, SMAA and the 3D/overlay split are D3D11-only for now: the
 * settings are kept so the menu reads back what it set, and have no effect. */
void d3d8_SetPostProcess(int on) { s_post_on = on ? 1 : 0; }
int  d3d8_GetPostProcess(void)   { return s_post_on; }
void d3d8_SetSmaa(int preset)    { s_smaa = preset >= 0 && preset <= 4 ? preset : 0; }
int  d3d8_GetSmaa(void)          { return s_smaa; }
void d3d8_SetPostSplit(int on)   { s_post_split = on ? 1 : 0; }
int  d3d8_GetPostSplit(void)     { return s_post_split; }
int  d3d8_post_split_wanted(void) { return 0; }   /* no post chain on Vulkan */
void d3d8_PassPhaseChanged(int old_phase, int new_phase, unsigned tag)
{ (void)old_phase; (void)new_phase; (void)tag; } /* no post chain on Vulkan */

/* Soft shadows are D3D11-only: the title's own stencil quad is kept. */
int d3d8_SoftShadowOn(void) { return 0; }
int d3d8_SoftShadowDraw(float shade, unsigned cmp, unsigned ref, unsigned rmask,
                        float fx0, float fy0, float fx1, float fy1)
{ (void)shade; (void)cmp; (void)ref; (void)rmask; (void)fx0; (void)fy0; (void)fx1; (void)fy1; return 0; }

/* GPU profiling (XBOX_GPUPROF) is D3D11-only: inert here. */
int g_gpuprof_on = 0;
void gpuprof_init(void) {}
void gpuprof_set_phase(int phase, int group) { (void)phase; (void)group; }
int  gpuprof_draw_cat(void) { return GP_OTHER; }
void gpuprof_begin(int cat) { (void)cat; }
void gpuprof_end(void) {}
void gpuprof_draw_begin(void) {}
void gpuprof_draw_end(void) {}
void gpuprof_count_flush(void) {}
void gpuprof_note(unsigned bits) { (void)bits; }
void gpuprof_frame(void) {}

/* Presentation sync: XBOX_SYNC / XBOX_VSYNC pick the swap interval (see
 * vkd_swap_interval); a frame-rate pace is recorded but not enforced. */
void d3d8_sync_set_pace(int fps);       /* d3d8_sync.h (D3D11/DXGI header) */
int  d3d8_monitor_hz(HWND hwnd);
static volatile LONG s_pace_fps;
void d3d8_sync_set_pace(int fps) { InterlockedExchange(&s_pace_fps, fps > 0 && fps <= 1000 ? fps : 0); }

int d3d8_monitor_hz(HWND hwnd)
{
    SDL_DisplayMode m;
    int idx = 0;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    if (hwnd) idx = SDL_GetWindowDisplayIndex((SDL_Window *)hwnd);
    if (idx < 0) idx = 0;
    if (SDL_GetDesktopDisplayMode(idx, &m) != 0) return 0;
    return m.refresh_rate > 0 ? m.refresh_rate : 0;
}

/* ================================================================
 * Screenshot request, toast (any thread)
 * ================================================================ */
static SDL_SpinLock s_shot_lock;
static wchar_t s_shot_path[MAX_PATH];
static volatile LONG s_shot_pending;
static SDL_SpinLock s_toast_lock;
static char s_toast[256];
static volatile LONG s_toast_tick;

void d3d8_RequestScreenshot(const wchar_t *path)
{
    SDL_AtomicLock(&s_shot_lock);
    if (path) {
        wcsncpy(s_shot_path, path, MAX_PATH - 1);
        s_shot_path[MAX_PATH - 1] = 0;
    } else {
        s_shot_path[0] = 0;
    }
    InterlockedExchange(&s_shot_pending, s_shot_path[0] ? 1 : 0);
    SDL_AtomicUnlock(&s_shot_lock);
}

int d3d8_ScreenshotPending(void) { return (int)s_shot_pending; }

/* wchar_t path -> UTF-8 (wchar_t is UTF-32 on these hosts). */
static void wide_to_utf8(const wchar_t *w, char *out, size_t n)
{
    size_t o = 0;
    for (; *w && o + 4 < n; w++) {
        unsigned long c = (unsigned long)*w;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

void d3d8_HostToast(const wchar_t *text)
{
    SDL_AtomicLock(&s_toast_lock);
    wide_to_utf8(text ? text : L"", s_toast, sizeof s_toast);
    SDL_AtomicUnlock(&s_toast_lock);
    InterlockedExchange(&s_toast_tick, (LONG)GetTickCount());
}

/* Closing the window ends the game at once (the title has no quit path). */
void d3d8_HostExit(void)
{
    fflush(stdout);
    fflush(stderr);
#if defined(__SWITCH__)
    exit(0);                            /* libnx needs its exit path */
#else
    _exit(0);
#endif
}

/* SDL_Window * of the game window (opaque; NULL before the device exists). */
HWND d3d8_GetHostWindow(void) { return (HWND)s_win; }

void d3d8_HostSetFullscreen(int on) { InterlockedExchange(&s_req_fullscreen, on ? 1 : 0); }

void d3d8_HostSetClientSize(unsigned w, unsigned h)
{
    if (!w || !h) return;
    InterlockedExchange(&s_req_client_h, (LONG)h);
    InterlockedExchange(&s_req_client_w, (LONG)w);
}

/* ================================================================
 * Window events (render thread)
 * ================================================================ */
static void host_set_fullscreen(int on)
{
#if defined(__SWITCH__)
    (void)on;                           /* always fullscreen */
#else
    if (!s_win || on == s_fullscreen) return;
    if (SDL_SetWindowFullscreen(s_win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) {
        s_fullscreen = on;
        SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
    }
#endif
}

static UINT sdl_key_to_vk(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return (UINT)('A' + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9) return (UINT)('0' + (k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70u + (UINT)(k - SDLK_F1);
    switch (k) {
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_RETURN: return 0x0D;
    case SDLK_SPACE:  return 0x20;
    case SDLK_TAB:    return 0x09;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_LEFT:   return 0x25;
    case SDLK_UP:     return 0x26;
    case SDLK_RIGHT:  return 0x27;
    case SDLK_DOWN:   return 0x28;
    case SDLK_PAUSE:  return 0x13;
    case SDLK_PRINTSCREEN: return 0x2C;
    default: return 0;
    }
}

static void host_pump_events(void)
{
    SDL_Event ev;
    LONG f, cw;
#if defined(__SWITCH__)
    if (!vkd_on_render_thread()) return;
    if (!appletMainLoop()) d3d8_HostExit();
#else
    if (!s_win || !vkd_on_render_thread()) return;
#endif
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            d3d8_HostExit();
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) d3d8_HostExit();
            break;
        case SDL_KEYDOWN:
            if (ev.key.repeat) break;
            if (s_ui_hooks.on_key) {
                UINT vk = sdl_key_to_vk(ev.key.keysym.sym);
                if (vk && s_ui_hooks.on_key((HWND)s_win, vk)) break;
            }
            if ((ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)) ||
                ev.key.keysym.sym == SDLK_F11)
                host_set_fullscreen(!s_fullscreen);
            break;
        default:
            break;
        }
    }
    f = InterlockedExchange(&s_req_fullscreen, -1);
    if (f >= 0) host_set_fullscreen((int)f);
    cw = InterlockedExchange(&s_req_client_w, 0);
    if (cw > 0 && s_win) {
        LONG ch = s_req_client_h;
        if (s_fullscreen) host_set_fullscreen(0);
        SDL_RestoreWindow(s_win);
        SDL_SetWindowSize(s_win, (int)cw, (int)ch);
    }
}

static void host_update_title(void)
{
#if !defined(__SWITCH__)
    static LONG last_frames;
    static DWORD last_tick;
    static UINT fps;
    static char cur[384];
    char title[384];
    DWORD now = GetTickCount();
    LONG frames = s_frames;
    if (!s_win) return;
    if (last_tick && now - last_tick >= 900) {
        fps = (UINT)((frames - last_frames) * 1000u / (now - last_tick));
        last_frames = frames;
        last_tick = now;
    } else if (!last_tick) {
        last_tick = now;
        last_frames = frames;
    }
    snprintf(title, sizeof title, "SSX Tricky");
    if (s_show_fps) {
        size_t l = strlen(title);
        snprintf(title + l, sizeof title - l, "  \xC2\xB7  %u fps", fps);
    }
    if (s_toast_tick && now - (DWORD)s_toast_tick < 3000) {
        size_t l = strlen(title);
        SDL_AtomicLock(&s_toast_lock);
        if (s_toast[0]) snprintf(title + l, sizeof title - l, "  \xC2\xB7  %s", s_toast);
        SDL_AtomicUnlock(&s_toast_lock);
    }
    if (strcmp(cur, title)) {
        memcpy(cur, title, sizeof cur);
        SDL_SetWindowTitle(s_win, title);
    }
#endif
}

void d3d8_PumpMessages(void)
{
    host_pump_events();
}

/* ================================================================
 * DAC palette (gamma ramp), from the translator's DAC emulation
 * ================================================================ */
static SDL_SpinLock     s_dac_lock;
static unsigned char    s_dac_lut[256][3];
static volatile LONG    s_dac_dirty, s_dac_have;
static int              s_dac_identity = 1;
static VkdImage         s_lut;

int d3d8_GammaFixEnabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_GAMMA"); on = !(e && e[0] == '0'); }
    return on;
}

void d3d8_SetDacPalette(const unsigned char lut[256][3])
{
    unsigned i;
    int ident = 1;
    SDL_AtomicLock(&s_dac_lock);
    memcpy(s_dac_lut, lut, sizeof s_dac_lut);
    for (i = 0; i < 256; i++)
        if (lut[i][0] != i || lut[i][1] != i || lut[i][2] != i) ident = 0;
    s_dac_identity = ident;
    SDL_AtomicUnlock(&s_dac_lock);
    InterlockedExchange(&s_dac_have, 1);
    InterlockedExchange(&s_dac_dirty, 1);
}

static int dac_active(void)
{
    return d3d8_GammaFixEnabled() && s_dac_have && !s_dac_identity;
}

static void dac_lut_copy(unsigned char out[256][3])
{
    SDL_AtomicLock(&s_dac_lock);
    memcpy(out, s_dac_lut, sizeof s_dac_lut);
    SDL_AtomicUnlock(&s_dac_lock);
}

/* The palette as a 256x1 RGBA8 image; NULL when off or identity. Uploads,
 * so it is taken before the present's rendering starts. */
static VkdImage *dac_lut_img(void)
{
    if (!dac_active()) return NULL;
    if (!s_lut.image) {
        if (!vkd_image_create(&s_lut, 256, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SAMPLE_COUNT_1_BIT, NULL))
            return NULL;
        InterlockedExchange(&s_dac_dirty, 1);
    }
    if (InterlockedExchange(&s_dac_dirty, 0)) {
        unsigned char lut[256][3], rgba[256 * 4];
        unsigned i;
        dac_lut_copy(lut);
        for (i = 0; i < 256; i++) {
            rgba[i * 4 + 0] = lut[i][0]; rgba[i * 4 + 1] = lut[i][1];
            rgba[i * 4 + 2] = lut[i][2]; rgba[i * 4 + 3] = 255;
        }
        vkd_image_upload(1, &s_lut, 0, 0, 0, 256, 1, rgba, sizeof rgba, 0);
    }
    return &s_lut;
}

/* ================================================================
 * Scene target
 * ================================================================ */
static void scene_targets_release(void)
{
    VkdState *s = &g_vkd;
    vkd_render_end();
    if (s->ms_color.image) vkd_image_retire(&s->ms_color);
    if (s->ms_ds.image) vkd_image_retire(&s->ms_ds);
    if (s->scene_color.image) vkd_image_retire(&s->scene_color);
    if (s->scene_ds.image) vkd_image_retire(&s->scene_ds);
}

/* Colour (0, 0, 0, 1), depth 1, stencil 0, as the GL backend starts too. */
static void clear_rect(DWORD flags, D3DCOLOR color, float z, DWORD stencil, LONG l, LONG t, LONG r, LONG b);
static void scene_clear_all(void)
{
    clear_rect(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000u, 1.0f, 0,
               0, 0, (LONG)g_vkd.width, (LONG)g_vkd.height);
}

static int scene_targets_create(UINT msaa)
{
    VkdState *s = &g_vkd;
    const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags du = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    UINT max = vkd_max_samples();
    s->color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    s->ds_fmt = vkd_depth_format();
    if (!vkd_image_create(&s->scene_color, s->width, s->height, 1, s->color_fmt, cu, VK_SAMPLE_COUNT_1_BIT, NULL) ||
        !vkd_image_create(&s->scene_ds, s->width, s->height, 1, s->ds_fmt, du, VK_SAMPLE_COUNT_1_BIT, NULL))
        return 0;
    while (msaa > 1 && msaa > max) msaa >>= 1;
    s->msaa = msaa > 1 ? msaa : 1;
    if (s->msaa > 1) {
        if (!vkd_image_create(&s->ms_color, s->width, s->height, 1, s->color_fmt,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              (VkSampleCountFlagBits)s->msaa, NULL) ||
            !vkd_image_create(&s->ms_ds, s->width, s->height, 1, s->ds_fmt, du,
                              (VkSampleCountFlagBits)s->msaa, NULL))
            return 0;
    }
    s->scene_dirty = 0;
    if (s->initialized) scene_clear_all();
    vkd_state_invalidate();
    return 1;
}

void vkd_scene_resolve(void)
{
    VkdState *s = &g_vkd;
    VkImageResolve r;
    VkCommandBuffer cb;
    if (s->msaa <= 1 || !s->ms_color.image || !s->scene_dirty) return;
    cb = vkd_cmd_outside();
    memset(&r, 0, sizeof r);
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
    r.extent.width = s->width;
    r.extent.height = s->height;
    r.extent.depth = 1;
    vkCmdResolveImage(cb, s->ms_color.image, VK_IMAGE_LAYOUT_GENERAL, s->scene_color.image,
                      VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    vkd_barrier(cb);
    s->scene_dirty = 0;
}

void d3d8_GetGuestScale(float *sx, float *sy)
{
    const VkdState *s = &g_vkd;
    *sx = (s->guest_w && s->width)  ? (float)s->width  / (float)s->guest_w : 1.0f;
    *sy = (s->guest_h && s->height) ? (float)s->height / (float)s->guest_h : 1.0f;
}

UINT d3d8_GetBackbufferWidth(void)  { return g_vkd.guest_w ? g_vkd.guest_w : g_vkd.width; }
UINT d3d8_GetBackbufferHeight(void) { return g_vkd.guest_h ? g_vkd.guest_h : g_vkd.height; }

/* Scene pixels, top-down rows, B G R A bytes; NULL on failure (free it). */
static unsigned char *scene_read_bgra(UINT *w_out, UINT *h_out)
{
    VkdState *s = &g_vkd;
    unsigned char *px;
    if (!s->scene_color.image || !s->width || !s->height) return NULL;
    vkd_scene_resolve();
    px = vkd_read_image(&s->scene_color, 0, 0, s->width, s->height);
    if (!px) return NULL;
    *w_out = s->width; *h_out = s->height;
    return px;
}

/* Diagnostic: one pixel of the scene, (fx, fy) in 0..1 from the top left,
 * as 0xAARRGGBB. Synchronous readback. */
unsigned d3d8_DebugPeekScene(float fx, float fy)
{
    VkdState *s = &g_vkd;
    unsigned char *p;
    unsigned x, y, v;
    if (!s->scene_color.image || !vkd_on_render_thread()) return 0xDEADBEEFu;
    x = (unsigned)(fx * (float)s->width);  if (x >= s->width)  x = s->width - 1;
    y = (unsigned)(fy * (float)s->height); if (y >= s->height) y = s->height - 1;
    vkd_scene_resolve();
    if (!(p = vkd_read_image(&s->scene_color, x, y, 1, 1))) return 0xDEADBEEFu;
    v = ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
    free(p);
    return v;
}

unsigned d3d8_BackbufferHash(void)
{
    UINT w, h, x, y;
    unsigned hash = 2166136261u;
    unsigned char *px;
    if (!vkd_on_render_thread() || !(px = scene_read_bgra(&w, &h))) return 0;
    for (y = 0; y < h; y += 16)
        for (x = 0; x < w; x += 16) {
            const unsigned char *p = px + ((size_t)y * w + x) * 4u;
            hash ^= p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
            hash *= 16777619u;
        }
    free(px);
    return hash ? hash : 1u;
}

void d3d8_DumpBackbuffer(const char *path)
{
    UINT w, h, y;
    unsigned char *px, hdr[54];
    FILE *f;
    size_t rowb, size;
    if (!path || !vkd_on_render_thread() || !(px = scene_read_bgra(&w, &h))) return;
    rowb = ((size_t)w * 3u + 3u) & ~(size_t)3u;
    size = 54 + rowb * h;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8);
    hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    f = fopen(path, "wb");
    if (f) {
        unsigned char *row = (unsigned char *)calloc(1, rowb);
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = 0; row && y < h; y++) {    /* BMP rows are bottom-up */
            const unsigned char *src = px + (size_t)(h - 1 - y) * w * 4u;
            UINT x;
            for (x = 0; x < w; x++) {
                row[x * 3 + 0] = src[x * 4 + 0];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 2];
            }
            fwrite(row, 1, rowb, f);
        }
        free(row);
        fclose(f);
    }
    free(px);
}

void d3d8_DebugSampleBackbuffer(const char *tag)
{
    UINT w, h;
    unsigned char *px;
    if (!vkd_on_render_thread() || !(px = scene_read_bgra(&w, &h))) return;
    {
        const unsigned char *tl = px, *c = px + ((size_t)(h / 2) * w + w / 2) * 4u;
        fprintf(stderr, "  [D3D] %s: scene %ux%u tl=%02X%02X%02X ctr=%02X%02X%02X\n",
                tag ? tag : "", w, h, tl[2], tl[1], tl[0], c[2], c[1], c[0]);
    }
    free(px);
}

/* ================================================================
 * Present bookkeeping, guest framebuffer
 * ================================================================ */
static DWORD g_last_present_tick;

DWORD d3d8_MsSincePresent(void)
{
    DWORD now = GetTickCount();
    if (g_last_present_tick == 0) g_last_present_tick = now;
    return now - g_last_present_tick;
}

static const void *g_guest_fb_src, *g_guest_fb_alt;
static unsigned    g_guest_fb_pitch, g_guest_fb_w, g_guest_fb_h;
static int         s_video_frame;

void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h)
{
    g_guest_fb_src = src; g_guest_fb_pitch = pitch;
    g_guest_fb_w = w;     g_guest_fb_h = h;
}

void d3d8_SetGuestFramebufferAlt(const void *alt) { g_guest_fb_alt = alt; }

/* See d3d8_device.c: upload the video buffer only while the player locks it. */
#define GUEST_FB_IDLE_PRESENTS 30u
static unsigned g_present_seq, g_guest_fb_lock_seq, g_guest_fb_locks;

void d3d8_NoteGuestFramebufferLock(void)
{
    g_guest_fb_lock_seq = g_present_seq;
    g_guest_fb_locks++;
}

unsigned d3d8_GuestFramebufferLockCount(void) { return *(volatile unsigned *)&g_guest_fb_locks; }

static int guest_fb_active(void)
{
    static int was_active = 0;
    int active = g_guest_fb_locks != 0
              && g_present_seq - g_guest_fb_lock_seq <= GUEST_FB_IDLE_PRESENTS;
    if (active != was_active) {
        fprintf(stderr, "  [FB] video framebuffer %s after %u locks (present %u)\n",
                active ? "active: presenting the video" : "idle: presenting rendered frames",
                g_guest_fb_locks, g_present_seq);
        fflush(stderr);
        was_active = active;
    }
    return active;
}

static unsigned guest_fb_sig(const void *base, unsigned pitch, unsigned h)
{
    const unsigned char *p = (const unsigned char *)base;
    unsigned sig = 2166136261u, y;
    if (!p) return 0;
    for (y = 0; y < h; y += 16) {
        const unsigned char *r = p + (size_t)y * pitch;
        unsigned k;
        for (k = 0; k < 64; k += 4) { sig ^= r[(pitch / 2) + k]; sig *= 16777619u; }
    }
    return sig;
}

/* The newest idle buffer of the two, never going backwards (d3d8_device.c). */
static const void *guest_fb_freshest(void)
{
    static unsigned sig_a, sig_b;
    static int seeded;
    static unsigned gen_a, gen_b, now, shown;
    unsigned a, b, cand_gen;
    int a_busy, b_busy;
    const void *pick;

    if (!g_guest_fb_alt) return g_guest_fb_src;
    a = guest_fb_sig(g_guest_fb_src, g_guest_fb_pitch, g_guest_fb_h);
    b = guest_fb_sig(g_guest_fb_alt, g_guest_fb_pitch, g_guest_fb_h);
    if (!seeded) { seeded = 1; sig_a = a; sig_b = b; return g_guest_fb_src; }
    now++;
    if (a != sig_a) gen_a = now;
    if (b != sig_b) gen_b = now;
    sig_a = a; sig_b = b;
    a_busy = (gen_a == now);
    b_busy = (gen_b == now);
    if (a_busy && b_busy)      { pick = NULL; cand_gen = 0; }
    else if (a_busy)           { pick = g_guest_fb_alt; cand_gen = gen_b; }
    else if (b_busy)           { pick = g_guest_fb_src; cand_gen = gen_a; }
    else if (gen_a >= gen_b)   { pick = g_guest_fb_src; cand_gen = gen_a; }
    else                       { pick = g_guest_fb_alt; cand_gen = gen_b; }
    if (pick && cand_gen < shown) pick = NULL;
    else if (pick)                shown = cand_gen;
    return pick;
}

void d3d8_PlagShot(void) {}             /* XBOX_PUMPLAG_SHOTS: D3D11 only */

unsigned d3d8_PresentSeq(void) { return g_present_seq; }

/* Video presents never wait for the GPU: the serial of each movie present,
 * and the next timed present is skipped until the GPU is past it. */
static uint64_t s_vid_serial;
static unsigned s_vid_skips;

static void video_present_mark(void)
{
    s_vid_serial = vkd_serial() - 1;    /* the present just submitted */
}

int d3d8_VideoPresentReady(void)
{
    if (!s_vid_serial || !vkd_on_render_thread()) return 1;
    if (!vkd_done(s_vid_serial)) {
        if (++s_vid_skips == 1 || s_vid_skips % 500 == 0) {
            fprintf(stderr, "[FB] video present held: GPU still on the previous frame (%u so far)\n",
                    s_vid_skips);
            fflush(stderr);
        }
        return 0;
    }
    s_vid_serial = 0;
    return 1;
}

/* Copy guest video memory (B G R A, top-down) into the scene target. src
 * NULL re-presents the frame already uploaded. */
void d3d8_PresentGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h)
{
    static VkdImage vimg;
    static unsigned vw, vh;
    VkdState *s = &g_vkd;
    if (!w || !h || !s->initialized || !vkd_on_render_thread()) return;
    if (!src && !vimg.image) return;
    if (src && (pitch < w * 4u || (pitch & 3u))) return;
    s_video_frame = 1;
    if (vimg.image && (vw != w || vh != h)) vkd_image_retire(&vimg);
    if (!vimg.image) {
        /* The video has no alpha: show it opaque. */
        VkComponentMapping m = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                 VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE };
        if (!src) return;
        if (!vkd_image_create(&vimg, w, h, 1, VK_FORMAT_B8G8R8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SAMPLE_COUNT_1_BIT, &m))
            return;
        vw = w; vh = h;
    }
    if (src)
        vkd_image_upload(1, &vimg, 0, 0, 0, w, h, src, (size_t)pitch * (h - 1) + (size_t)w * 4u, pitch / 4u);
    vkd_blit(&vimg, 0, 0, (int)s->width, (int)s->height, NULL, VKD_TARGET_SCENE);
    vkd_state_invalidate();
    if (src) {
        static int told;
        if (!told) {
            unsigned nz = 0, sy, sx;
            for (sy = 0; sy < h; sy += 8) {
                const unsigned char *r = (const unsigned char *)src + (size_t)sy * pitch;
                for (sx = 0; sx < w; sx += 8)
                    if (r[sx * 4] | r[sx * 4 + 1] | r[sx * 4 + 2]) nz++;
            }
            if (nz) {
                told = 1;
                fprintf(stderr, "  [FB] first video frame: %u/%u non-black samples (%ux%u pitch %u)\n",
                        nz, (h / 8) * (w / 8), w, h, pitch);
                fflush(stderr);
            }
        }
    }
}

int d3d8_HasGuestFramebuffer(void) { return g_guest_fb_src != NULL; }

int d3d8_GuestFramebufferActive(void)
{
    return g_guest_fb_src != NULL && g_guest_fb_locks != 0
        && g_present_seq - g_guest_fb_lock_seq <= GUEST_FB_IDLE_PRESENTS;
}

/* ================================================================
 * Occlusion queries (NV2A zpass counter), render thread only
 * ================================================================ */
#define OCC_POOL 256
static VkQueryPool    s_occ_pool;
static int            s_occ_state;      /* 0 untried, 1 ready, -1 unavailable */
static unsigned char  s_occ_busy[OCC_POOL];
static uint64_t       s_occ_serial[OCC_POOL];   /* the submission that counts it */
static int            s_occ_active = -1;        /* one query at a time */

/* Rendering is ending (a texture upload, a flush): the open query ends there. */
static void occ_on_render_end(void)
{
    if (s_occ_active < 0) return;
    vkCmdEndQuery(vkd_cmd(), s_occ_pool, (uint32_t)s_occ_active);
    s_occ_active = -1;
}

static int occ_ready(void)
{
    VkQueryPoolCreateInfo ci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    if (s_occ_state) return s_occ_state > 0;
    s_occ_state = -1;
    ci.queryType = VK_QUERY_TYPE_OCCLUSION;
    ci.queryCount = OCC_POOL;
    if (vkCreateQueryPool(g_vk.dev, &ci, NULL, &s_occ_pool) != VK_SUCCESS) return 0;
    vkResetQueryPool(g_vk.dev, s_occ_pool, 0, OCC_POOL);
    vkd_on_render_end = occ_on_render_end;
    s_occ_state = 1;
    return 1;
}

int d3d8_OcclusionBegin(void)
{
    VkCommandBuffer cb;
    int i;
    if (!g_vkd.initialized || !vkd_on_render_thread() || s_occ_active >= 0 || !g_vk.has_host_query_reset ||
        !occ_ready())
        return -1;
    for (i = 0; i < OCC_POOL; i++) if (!s_occ_busy[i] && vkd_done(s_occ_serial[i])) break;
    if (i == OCC_POOL) return -1;
    vkResetQueryPool(g_vk.dev, s_occ_pool, (uint32_t)i, 1);
    cb = vkd_cmd_render();
    vkCmdBeginQuery(cb, s_occ_pool, (uint32_t)i, g_vk.has_precise_occ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_occ_busy[i] = 1;
    s_occ_serial[i] = vkd_serial();
    s_occ_active = i;
    return i;
}

void d3d8_OcclusionEnd(int h)
{
    if (h < 0 || h >= OCC_POOL || h != s_occ_active) return;
    if (vkd_rendering()) vkCmdEndQuery(vkd_cmd(), s_occ_pool, (uint32_t)h);
    s_occ_active = -1;
}

int d3d8_OcclusionPoll(int h, unsigned long long *samples)
{
    uint64_t n = 0;
    if (h < 0 || h >= OCC_POOL || s_occ_state <= 0 || !s_occ_busy[h] || h == s_occ_active) return -1;
    if (!vkd_done(s_occ_serial[h])) return 0;
    if (vkGetQueryPoolResults(g_vk.dev, s_occ_pool, (uint32_t)h, 1, sizeof n, &n, sizeof n,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return -1;
    *samples = n;
    return 1;
}

/* The title spins on a report: submit what is recorded (a query not yet
 * submitted would never complete before the present). */
void d3d8_OcclusionFlush(void)
{
    int i;
    if (!g_vkd.initialized || !vkd_on_render_thread() || s_occ_state <= 0) return;
    for (i = 0; i < OCC_POOL; i++)
        if (s_occ_busy[i] && i != s_occ_active && s_occ_serial[i] == vkd_serial()) {
            vkd_flush(0);
            vkd_state_invalidate();
            return;
        }
}

void d3d8_OcclusionRelease(int h)
{
    if (h < 0 || h >= OCC_POOL) return;
    if (h == s_occ_active) d3d8_OcclusionEnd(h);
    s_occ_busy[h] = 0;
}

float d3d8_OcclusionScale(void)
{
    float sx, sy;
    d3d8_GetGuestScale(&sx, &sy);
    return (float)(g_vkd.msaa ? g_vkd.msaa : 1) * sx * sy;
}

/* ================================================================
 * Previous frame as a texture
 * ================================================================ */
static IDirect3DTexture8 *g_prev_frame;
static unsigned           g_prev_frame_wanted;

static void prev_frame_update(void)
{
    VkdState *s = &g_vkd;
    VkdTexture *t;
    VkImageCopy c;
    VkCommandBuffer cb;
    if (!s->scene_color.image || !g_prev_frame_wanted || g_present_seq - g_prev_frame_wanted > 120)
        return;
    t = (VkdTexture *)g_prev_frame;
    if (t && (t->width != s->width || t->height != s->height)) {
        g_prev_frame->lpVtbl->Release(g_prev_frame);
        g_prev_frame = NULL;
        t = NULL;
    }
    if (!t) {
        if (FAILED(vkd_texture_create_gpu(s->width, s->height, &g_prev_frame))) return;
        t = (VkdTexture *)g_prev_frame;
    }
    memset(&c, 0, sizeof c);
    c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
    c.extent.width = s->width;
    c.extent.height = s->height;
    c.extent.depth = 1;
    cb = vkd_cmd_outside();             /* both top-down: a plain copy */
    vkCmdCopyImage(cb, s->scene_color.image, VK_IMAGE_LAYOUT_GENERAL, t->img.image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vkd_barrier(cb);
    vkd_state_invalidate();
}

IDirect3DTexture8 *d3d8_PrevFrameTexture(void)
{
    g_prev_frame_wanted = g_present_seq ? g_present_seq : 1;
    return g_prev_frame;
}

/* ================================================================
 * Display shape (d3d8_device.c semantics)
 * ================================================================ */
static int video_aspect_fix(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_VIDEO_ASPECT"); on = !(e && e[0] == '0'); }
    return on;
}

static double present_aspect(void)
{
    double a = d3d8_HostAspect();
    if (s_video_frame && g_guest_fb_w && g_guest_fb_h && video_aspect_fix()) {
        double va = (double)g_guest_fb_w / (double)g_guest_fb_h;
        if (a > va + 0.005) return va;
    }
    return s_present_box && a > s_present_box_shape + 0.005 ? s_present_box_shape : a;
}

static int present_box_cols(unsigned w, unsigned *x0, unsigned *bw)
{
    double a = d3d8_HostAspect(), pa = present_aspect();
    if (!(pa < a)) return 0;
    *bw = (unsigned)(w * pa / a + 0.5);
    if (*bw >= w || !*bw) return 0;
    *x0 = (w - *bw) / 2;
    return 1;
}

static void present_box_row(const unsigned char *src, unsigned char *dst, unsigned w,
                            unsigned x0, unsigned bw)
{
    unsigned x, c;
    for (x = 0; x < w; x++) {
        if (x < x0 || x >= x0 + bw) { dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = 0; continue; }
        {
            double u = ((double)(x - x0) + 0.5) * (double)w / (double)bw - 0.5;
            unsigned i0 = u <= 0.0 ? 0u : (unsigned)u, i1 = i0 + 1u < w ? i0 + 1u : i0;
            double t = u <= 0.0 ? 0.0 : u - (double)i0;
            for (c = 0; c < 3; c++)
                dst[x * 3 + c] = (unsigned char)(src[i0 * 3 + c] * (1.0 - t) + src[i1 * 3 + c] * t + 0.5);
        }
    }
}

/* ================================================================
 * Screenshots: PNG (zlib stored blocks, no dependency)
 * ================================================================ */
static unsigned long png_crc(unsigned long c, const BYTE *p, size_t n)
{
    static unsigned long t[256];
    size_t i;
    if (!t[1]) {
        unsigned long k, v;
        for (k = 0; k < 256; k++) {
            v = k;
            for (i = 0; i < 8; i++) v = (v & 1) ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
            t[k] = v;
        }
    }
    c ^= 0xFFFFFFFFUL;
    for (i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static BYTE *png_be32(BYTE *p, unsigned long v)
{
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v;
    return p + 4;
}

static BYTE *png_chunk_end(BYTE *p, const char *type, size_t len)
{
    png_be32(p, (unsigned long)len);
    memcpy(p + 4, type, 4);
    return png_be32(p + 8 + len, png_crc(0, p + 4, len + 4));
}

static int png_encode_own(const BYTE *bgr, UINT w, UINT h, UINT stride, BYTE **out, size_t *out_len)
{
    static const BYTE sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const size_t row = (size_t)w * 3 + 1, raw = row * h;
    const size_t blocks = (raw + 65534) / 65535;
    const size_t zlen = 2 + raw + blocks * 5 + 4;
    BYTE *png, *p, *z;
    size_t left = raw, y, x, done = 0;
    unsigned long a = 1, b = 0;

    *out = NULL; *out_len = 0;
    if (!w || !h) return 0;
    png = (BYTE *)malloc(8 + 25 + 12 + zlen + 12);
    if (!png) return 0;
    memcpy(png, sig, 8);
    p = png + 8;
    png_be32(p + 8, w); png_be32(p + 12, h);
    p[16] = 8; p[17] = 2; p[18] = 0; p[19] = 0; p[20] = 0;
    p = png_chunk_end(p, "IHDR", 13);
    z = p + 8;
    *z++ = 0x78; *z++ = 0x01;
    for (y = 0; y < h; y++) {
        const BYTE *src = bgr + y * stride;
        for (x = 0; x < row; x++, done++) {
            BYTE v = x == 0 ? 0 : src[(x - 1) / 3 * 3 + 2 - (x - 1) % 3];
            if (done % 65535 == 0) {
                size_t n = left < 65535 ? left : 65535;
                *z++ = (BYTE)(left == n);
                *z++ = (BYTE)n; *z++ = (BYTE)(n >> 8);
                *z++ = (BYTE)~n; *z++ = (BYTE)(~n >> 8);
                left -= n;
            }
            *z++ = v;
            a = (a + v) % 65521; b = (b + a) % 65521;
        }
    }
    z = png_be32(z, (b << 16) | a);
    p = png_chunk_end(p, "IDAT", (size_t)(z - (p + 8)));
    p = png_chunk_end(p, "IEND", 0);
    *out = png;
    *out_len = (size_t)(p - png);
    return 1;
}

/* The scene as shown (DAC palette, present box), written to <path>.part
 * and renamed into place. */
static int save_scene_png(const char *path)
{
    unsigned char slut[256][3];
    int slut_on = dac_active(), ok = 0;
    UINT w, h, x, y, stride;
    unsigned char *px, *bgr;
    if (slut_on) dac_lut_copy(slut);
    if (!(px = scene_read_bgra(&w, &h))) return 0;
    stride = (w * 3 + 3) & ~3u;
    bgr = (unsigned char *)malloc((size_t)stride * h);
    if (bgr) {
        for (y = 0; y < h; y++) {
            const unsigned char *src = px + (size_t)y * w * 4u;
            unsigned char *dst = bgr + (size_t)y * stride;
            for (x = 0; x < w; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
                if (slut_on) {
                    dst[x * 3 + 0] = slut[dst[x * 3 + 0]][2];
                    dst[x * 3 + 1] = slut[dst[x * 3 + 1]][1];
                    dst[x * 3 + 2] = slut[dst[x * 3 + 2]][0];
                }
            }
        }
        {
            unsigned bx0, bw;
            unsigned char *rb;
            if (present_box_cols(w, &bx0, &bw) && (rb = (unsigned char *)malloc((size_t)w * 3u))) {
                for (y = 0; y < h; y++) {
                    unsigned char *row = bgr + (size_t)y * stride;
                    present_box_row(row, rb, w, bx0, bw);
                    memcpy(row, rb, (size_t)w * 3u);
                }
                free(rb);
            }
        }
        {
            BYTE *png = NULL;
            size_t len = 0;
            if (png_encode_own(bgr, w, h, stride, &png, &len)) {
                char tmp[MAX_PATH * 4 + 8];
                FILE *f;
                snprintf(tmp, sizeof tmp, "%s.part", path);
                f = fopen(tmp, "wb");
                if (f) {
                    ok = fwrite(png, 1, len, f) == len;
                    if (fclose(f) != 0) ok = 0;
                    if (ok && rename(tmp, path) != 0) ok = 0;
                    if (!ok) remove(tmp);
                }
            }
            free(png);
        }
        free(bgr);
    }
    free(px);
    return ok;
}

/* ================================================================
 * Present
 * ================================================================ */
static int vkd_swap_interval(void)
{
    const char *e = getenv("XBOX_VSYNC");
    const char *m = getenv("XBOX_SYNC");
    if (m && *m) {
        if (!strcmp(m, "off")) return 0;
        if (!strcmp(m, "vsync")) return 1;
        if (!strcmp(m, "adaptive")) return -1;
    }
    if (e && *e) return e[0] == '1' ? 1 : 0;
#if defined(__SWITCH__)
    return 1;               /* no compositor: wait for the vertical blank */
#else
    return 0;               /* legacy: the title paces itself (d3d8_device.c) */
#endif
}

static void host_present_impl(void);

/* XBOX_PERF: the present (blit + swap) and the swap alone, then the pump's
 * per-frame record (no GPU timestamps on this backend). */
static double s_perf_swap_ms;
static void host_present(void)
{
    double t0;
    if (!g_perf_on) { host_present_impl(); return; }
    t0 = perf_now();
    s_perf_swap_ms = 0.0;
    host_present_impl();
    perf_add(PZ_PRESENT, perf_now() - t0);
    perf_add(PZ_DXGI, s_perf_swap_ms);
    perf_present_done(0.0);
}

static void host_present_impl(void)
{
    VkdState *s = &g_vkd;
    extern void d3d8_pump_frame_tick(void);
    d3d8_pump_frame_tick();
#if defined(__SWITCH__)
    if (!s->initialized || !vkd_on_render_thread()) return;     /* no SDL window there */
#else
    if (!s->initialized || !s_win || !vkd_on_render_thread()) return;
#endif

    vkd_resources_flush_deletes();
    vkd_scene_resolve();
    prev_frame_update();

    if (s_shot_pending) {
        wchar_t wpath[MAX_PATH];
        char path[MAX_PATH * 4];
        SDL_AtomicLock(&s_shot_lock);
        memcpy(wpath, s_shot_path, sizeof wpath);
        s_shot_path[0] = 0;
        SDL_AtomicUnlock(&s_shot_lock);
        if (wpath[0]) {
            int ok;
            wide_to_utf8(wpath, path, sizeof path);
            ok = save_scene_png(path);
            d3d8_HostToast(ok ? L"Screenshot saved" : L"Screenshot failed");
            fprintf(stderr, "D3D8: screenshot %s: %s\n", path, ok ? "ok" : "failed");
        }
        InterlockedExchange(&s_shot_pending, 0);
    }

    host_pump_events();
    {
        VkdImage *lut = dac_lut_img();
        uint32_t dw = 0, dh = 0;
        double ts;
        vkd_set_sync(vkd_swap_interval());
        if (vkd_present_begin(&dw, &dh) && dw > 0 && dh > 0) {
            const double aspect = present_aspect();
            int rw, rh, rx, ry;
            if ((double)dw / (double)dh > aspect) { rh = (int)dh; rw = (int)(dh * aspect + 0.5); }
            else                                  { rw = (int)dw; rh = (int)(dw / aspect + 0.5); }
            if (rw > (int)dw) rw = (int)dw;
            if (rh > (int)dh) rh = (int)dh;
            rx = ((int)dw - rw) / 2;
            ry = ((int)dh - rh) / 2;
            /* the swapchain image starts cleared to black: the bars are done */
            vkd_blit(&s->scene_color, rx, ry, rw, rh, lut, VKD_TARGET_SWAPCHAIN);
        }
        host_update_title();
        ts = g_perf_on ? perf_now() : 0.0;
        vkd_present_end();
        if (g_perf_on) s_perf_swap_ms = perf_now() - ts;
    }
    InterlockedIncrement(&s_frames);
    if (s_video_frame) video_present_mark();
    s_video_frame = 0;

    {   /* XBOX_FPS_LOG=1: frames per second every 2 s */
        static int on = -1;
        static DWORD t0;
        static LONG f0;
        if (on < 0) { const char *e = getenv("XBOX_FPS_LOG"); on = e && e[0] == '1'; }
        if (on) {
            DWORD now = GetTickCount();
            if (!t0) { t0 = now; f0 = s_frames; }
            else if (now - t0 >= 2000) {
                fprintf(stderr, "[FPS] %.1f; %ld shader compiles, %.0f ms; %ld texture uploads, %.0f ms\n",
                        (s_frames - f0) * 1000.0 / (now - t0), g_nv_compiles, g_nv_compile_ms,
                        g_tex_uploads_log, g_tex_upload_ms);
                g_nv_compiles = 0; g_nv_compile_ms = 0;
                g_tex_uploads_log = 0; g_tex_upload_ms = 0;
                t0 = now; f0 = s_frames;
            }
        }
    }

    /* Anti-aliasing changed from the menu: rebuild the targets between frames. */
    if ((UINT)s_want_msaa != (s->msaa ? s->msaa : 1)) {
        static UINT tried;
        if (tried != (UINT)s_want_msaa) {
            tried = (UINT)s_want_msaa;
            scene_targets_release();
            if (!scene_targets_create((UINT)s_want_msaa)) {
                scene_targets_release();
                scene_targets_create(1);
            }
            fprintf(stderr, "D3D8: anti-aliasing now %ux\n", s->msaa);
        }
    }
    vkd_state_invalidate();
}

void d3d8_PresentFrame(void)
{
    g_present_seq++;
    if (g_guest_fb_src && guest_fb_active()) {
        const void *fb = guest_fb_freshest();
        d3d8_PresentGuestFramebuffer(fb, g_guest_fb_pitch, g_guest_fb_w, g_guest_fb_h);
    }
    host_pump_events();
    {   /* XBOX_D3D_DUMP=<prefix>: every XBOX_D3D_DUMP_EVERY-th present as BMP */
        static unsigned seen, dn, cap, every, from = 0xFFFFFFFFu;
        const char *pre = getenv("XBOX_D3D_DUMP");
        if (pre) {
            if (!cap) { const char *m = getenv("XBOX_D3D_DUMP_MAX"); cap = (m && atoi(m) > 0) ? (unsigned)atoi(m) : 24u; }
            if (!every) { const char *e = getenv("XBOX_D3D_DUMP_EVERY"); every = (e && atoi(e) > 0) ? (unsigned)atoi(e) : 60u; }
            if (from == 0xFFFFFFFFu) { const char *f = getenv("XBOX_D3D_DUMP_FROM"); from = f ? (unsigned)atoi(f) : 0u; }
            if (seen >= from && ((seen - from) % every) == 0 && dn < cap) {
                char path[512];
                snprintf(path, sizeof path, "%s%03u_f%05u.bmp", pre, dn++, seen);
                d3d8_DumpBackbuffer(path);
            }
            seen++;
        }
    }
    g_last_present_tick = GetTickCount();
    {
        static unsigned c;
        if (++c == 1 || (c % 300) == 0) { fprintf(stderr, "  [PRESENT] PresentFrame #%u\n", c); fflush(stderr); }
    }
    host_present();
}

/* ================================================================
 * Window and context
 * ================================================================ */
static int host_window_create(UINT width, UINT height)
{
#if defined(__SWITCH__)
    (void)width; (void)height;
    s_fullscreen = 1;                   /* the default NWindow: 1280x720 / 1920x1080 */
    return vkd_core_init(NULL);
#else
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    int w = (int)width, h = (int)height;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "D3D8: SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (s_cfg_fullscreen) { flags |= SDL_WINDOW_FULLSCREEN_DESKTOP; s_fullscreen = 1; }
    {   /* shrink to fit the display, keeping the shape */
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(0, &r) == 0 && r.w > 0 && r.h > 0) {
            while (w > r.w || h > r.h) { w = w * 3 / 4; h = h * 3 / 4; }
        }
    }
    s_win = SDL_CreateWindow("SSX Tricky", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w > 0 ? w : 640, h > 0 ? h : 480, flags);
    if (!s_win) {
        fprintf(stderr, "D3D8: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!vkd_core_init(s_win)) {
        SDL_DestroyWindow(s_win);
        s_win = NULL;
        return 0;
    }
    if (s_fullscreen) SDL_ShowCursor(SDL_DISABLE);
    return 1;
#endif
}

static void init_default_states(void)
{
    VkdState *s = &g_vkd;
    int st, i;
    memset(s->rs, 0, sizeof s->rs);
    s->rs[D3DRS_ZENABLE]          = 1;
    s->rs[D3DRS_FILLMODE]         = D3DFILL_SOLID;
    s->rs[D3DRS_SHADEMODE]        = 2;
    s->rs[D3DRS_ZWRITEENABLE]     = TRUE;
    s->rs[D3DRS_ALPHATESTENABLE]  = FALSE;
    s->rs[D3DRS_SRCBLEND]         = D3DBLEND_ONE;
    s->rs[D3DRS_DESTBLEND]        = D3DBLEND_ZERO;
    s->rs[D3DRS_CULLMODE]         = D3DCULL_CCW;
    s->rs[D3DRS_ZFUNC]            = D3DCMP_LESSEQUAL;
    s->rs[D3DRS_ALPHAREF]         = 0;
    s->rs[D3DRS_ALPHAFUNC]        = D3DCMP_ALWAYS;
    s->rs[D3DRS_ALPHABLENDENABLE] = FALSE;
    s->rs[D3DRS_FOGENABLE]        = FALSE;
    s->rs[D3DRS_STENCILENABLE]    = FALSE;
    s->rs[D3DRS_COLORWRITEENABLE] = 0x0F;
    memset(s->tss, 0, sizeof s->tss);
    for (st = 0; st < VKD_MAX_STAGES; st++) {
        s->tss[st][D3DTSS_COLOROP]   = (st == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        s->tss[st][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        s->tss[st][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        s->tss[st][D3DTSS_ALPHAOP]   = (st == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        s->tss[st][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        s->tss[st][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        s->tss[st][D3DTSS_TEXCOORDINDEX] = (DWORD)st;
        s->tss[st][D3DTSS_ADDRESSU]  = 1;
        s->tss[st][D3DTSS_ADDRESSV]  = 1;
        s->tss[st][D3DTSS_MAGFILTER] = 1;
        s->tss[st][D3DTSS_MINFILTER] = 1;
        s->tss[st][D3DTSS_MIPFILTER] = 0;
    }
    s->viewport.X = 0;
    s->viewport.Y = 0;
    s->viewport.Width = s->guest_w;    /* title pixels; scaled when applied */
    s->viewport.Height = s->guest_h;
    s->viewport.MinZ = 0.0f;
    s->viewport.MaxZ = 1.0f;
    for (i = 0; i < VKD_MAX_XFORMS; i++) {
        memset(&s->xforms[i], 0, sizeof(D3DMATRIX));
        s->xforms[i]._11 = s->xforms[i]._22 = s->xforms[i]._33 = s->xforms[i]._44 = 1.0f;
    }
    s->vertex_shader = 0;
    s->pixel_shader = 0;
}

/* ================================================================
 * IDirect3DDevice8
 * ================================================================ */
static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{ (void)self; (void)riid; (void)ppv; return E_NOINTERFACE; }

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{ (void)self; return (ULONG)InterlockedIncrement(&g_vkd.ref); }

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    LONG ref = InterlockedDecrement(&g_vkd.ref);
    (void)self;
    if (ref <= 0 && g_vkd.initialized && vkd_on_render_thread()) {
        vkd_flush(1);
        vkd_draw_shutdown();
        scene_targets_release();
        if (s_lut.image) vkd_image_retire(&s_lut);
        vkd_core_shutdown();
        g_vkd.initialized = 0;
    }
    return (ULONG)(ref < 0 ? 0 : ref);
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **pp)
{ (void)self; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *p)
{ (void)self; (void)p; return S_OK; }
static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *p)
{ (void)self; (void)p; return S_OK; }
static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *p)
{ (void)self; (void)p; return S_OK; }
static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pp)
{ (void)self; (void)pp; return S_OK; }

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *dirty)
{
    static unsigned c;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)dirty;
    if (++c == 1 || (c % 300) == 0) { fprintf(stderr, "  [PRESENT] dev_Present #%u\n", c); fflush(stderr); }
    host_present();
    return S_OK;
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT i, DWORD t, IDirect3DSurface8 **pp)
{ (void)self; (void)i; (void)t; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self) { (void)self; return S_OK; }
static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)   { (void)self; return S_OK; }

/* A clear of [l, r) x [t, b) in scene pixels (D3D11's Clear ignores masks
 * and scissor; so does vkCmdClearAttachments). */
static void clear_rect(DWORD flags, D3DCOLOR color, float z, DWORD stencil, LONG l, LONG t, LONG r, LONG b)
{
    VkClearAttachment a[2];
    VkClearRect rc;
    VkCommandBuffer cb;
    uint32_t n = 0;
    if (r <= l || b <= t) return;
    memset(a, 0, sizeof a);
    if (flags & D3DCLEAR_TARGET) {
        a[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a[n].colorAttachment = 0;
        a[n].clearValue.color.float32[0] = ((color >> 16) & 0xFF) / 255.0f;
        a[n].clearValue.color.float32[1] = ((color >> 8) & 0xFF) / 255.0f;
        a[n].clearValue.color.float32[2] = (color & 0xFF) / 255.0f;
        a[n].clearValue.color.float32[3] = ((color >> 24) & 0xFF) / 255.0f;
        n++;
    }
    if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        if (flags & D3DCLEAR_ZBUFFER) a[n].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & D3DCLEAR_STENCIL) a[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        a[n].clearValue.depthStencil.depth = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
        a[n].clearValue.depthStencil.stencil = stencil & 0xFF;
        n++;
    }
    if (!n) return;
    rc.rect.offset.x = l;
    rc.rect.offset.y = t;
    rc.rect.extent.width = (uint32_t)(r - l);
    rc.rect.extent.height = (uint32_t)(b - t);
    rc.baseArrayLayer = 0;
    rc.layerCount = 1;
    cb = vkd_cmd_render();
    vkCmdClearAttachments(cb, n, a, 1, &rc);
}

/* Whole-target clear. */
static void clear_whole(DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (!g_vkd.initialized || !vkd_on_render_thread()) return;
    clear_rect(flags, color, z, stencil, 0, 0, (LONG)g_vkd.width, (LONG)g_vkd.height);
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags,
                                   D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects;
    clear_whole(Flags, Color, Z, Stencil);
    return S_OK;
}

/* The colour clear limited to the title's clear rectangle (inclusive, title
 * pixels); depth and stencil cleared whole, as the D3D11 backend does. */
void d3d8_ClearRect(DWORD flags, D3DCOLOR color, float z, DWORD stencil,
                    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    VkdState *s = &g_vkd;
    UINT gw = s->guest_w ? s->guest_w : s->width, gh = s->guest_h ? s->guest_h : s->height;
    if (!s->initialized || !vkd_on_render_thread()) return;
    if ((flags & D3DCLEAR_TARGET) && !(x0 == 0 && y0 == 0 && x1 + 1 >= gw && y1 + 1 >= gh)) {
        float sx, sy;
        LONG l, t, r, b;
        d3d8_GetGuestScale(&sx, &sy);
        if (x1 >= gw) x1 = gw - 1;
        if (y1 >= gh) y1 = gh - 1;
        l = (LONG)(x0 * sx + 0.5f);
        t = (LONG)(y0 * sy + 0.5f);
        r = (LONG)((x1 + 1) * sx + 0.5f);
        b = (LONG)((y1 + 1) * sy + 0.5f);
        if (x0 <= x1 && y0 <= y1 && r > l && b > t)
            clear_rect(D3DCLEAR_TARGET, color, z, stencil, l, t, r, b);
        flags &= ~(DWORD)D3DCLEAR_TARGET;
    }
    if (flags) clear_whole(flags, color, z, stencil);
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *m)
{
    (void)self;
    if ((DWORD)State < VKD_MAX_XFORMS && m) g_vkd.xforms[(DWORD)State] = *m;
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *m)
{
    (void)self;
    if ((DWORD)State < VKD_MAX_XFORMS && m) *m = g_vkd.xforms[(DWORD)State];
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    if ((DWORD)State < VKD_MAX_RS) g_vkd.rs[(DWORD)State] = Value;
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *v)
{
    (void)self;
    if ((DWORD)State < VKD_MAX_RS && v) *v = g_vkd.rs[(DWORD)State];
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < VKD_MAX_STAGES && (DWORD)Type < VKD_MAX_TSS) g_vkd.tss[Stage][(DWORD)Type] = Value;
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *v)
{
    (void)self;
    if (Stage < VKD_MAX_STAGES && (DWORD)Type < VKD_MAX_TSS && v) *v = g_vkd.tss[Stage][(DWORD)Type];
    return S_OK;
}

/* Binding is deferred to the draw (vkd_texture_ensure); as in D3D11, a NULL
 * texture disables the stage's colour op, a texture re-enables it. */
static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *t)
{
    (void)self;
    if (Stage >= VKD_MAX_STAGES) return E_INVALIDARG;
    g_vkd.tex[Stage] = t;
    if (t) {
        if (g_vkd.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_vkd.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        g_vkd.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

/* The bound texture, AddRef'd (D3D8 semantics; the D3D11 backend returns
 * E_NOTIMPL, whose only caller is a diagnostic log). */
static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **pp)
{
    (void)self;
    if (!pp) return E_INVALIDARG;
    *pp = NULL;
    if (Stage >= VKD_MAX_STAGES) return E_INVALIDARG;
    *pp = g_vkd.tex[Stage];
    if (*pp) ((IDirect3DTexture8 *)*pp)->lpVtbl->AddRef((IDirect3DTexture8 *)*pp);
    return S_OK;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT n, IDirect3DVertexBuffer8 *vb, UINT stride)
{
    (void)self;
    if (n != 0) return S_OK;            /* stream 0 only, as D3D11 */
    g_vkd.vb = vb;
    g_vkd.vb_stride = stride;
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT n, IDirect3DVertexBuffer8 **pp, UINT *stride)
{ (void)self; (void)n; (void)pp; (void)stride; return E_NOTIMPL; }

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *ib, UINT base)
{
    (void)self;
    g_vkd.ib = ib;
    g_vkd.ib_base = base;
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **pp, UINT *base)
{ (void)self; (void)pp; (void)base; return E_NOTIMPL; }

static UINT prim_vertex_count(D3DPRIMITIVETYPE pt, UINT count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  return count * 3;
    case D3DPT_TRIANGLESTRIP: return count + 2;
    case D3DPT_TRIANGLEFAN:   return count + 2;
    case D3DPT_LINELIST:      return count * 2;
    case D3DPT_LINESTRIP:     return count + 1;
    case D3DPT_POINTLIST:     return count;
    case D3DPT_QUADLIST:      return count * 4;
    default:                  return 0;
    }
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE pt, UINT start, UINT count)
{
    UINT size = 0, nv = prim_vertex_count(pt, count), stride = g_vkd.vb_stride;
    const BYTE *data = g_vkd.vb ? vkd_vb_data(g_vkd.vb, &size) : NULL;
    (void)self;
    if (!data || !nv || !stride || (size_t)(start + nv) * stride > size) return E_INVALIDARG;
    return vkd_draw_up(pt, count, data + (size_t)start * stride, stride, NULL, 0, 0);
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE pt, UINT minv, UINT nverts,
                                                  UINT start, UINT count)
{
    UINT vsize = 0, isize = 0, ni = prim_vertex_count(pt, count), stride = g_vkd.vb_stride, isz;
    D3DFORMAT ifmt = D3DFMT_INDEX16;
    const BYTE *v = g_vkd.vb ? vkd_vb_data(g_vkd.vb, &vsize) : NULL;
    const BYTE *idx = g_vkd.ib ? vkd_ib_data(g_vkd.ib, &isize, &ifmt) : NULL;
    (void)self; (void)minv; (void)nverts;
    isz = ifmt == D3DFMT_INDEX32 ? 4u : 2u;
    if (!v || !idx || !ni || !stride || (size_t)(start + ni) * isz > isize) return E_INVALIDARG;
    if ((size_t)g_vkd.ib_base * stride >= vsize) return E_INVALIDARG;
    return vkd_draw_up(pt, count, v + (size_t)g_vkd.ib_base * stride, stride,
                       idx + (size_t)start * isz, ifmt == D3DFMT_INDEX32, ni);
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE pt, UINT count,
                                             const void *verts, UINT stride)
{
    (void)self;
    if (!verts || !stride) return E_INVALIDARG;
    return vkd_draw_up(pt, count, verts, stride, NULL, 0, 0);
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE pt, UINT minv, UINT nverts,
                                                    UINT count, const void *idx, D3DFORMAT ifmt,
                                                    const void *verts, UINT stride)
{
    (void)self; (void)minv; (void)nverts;
    if (!verts || !idx || !stride) return E_INVALIDARG;
    return vkd_draw_up(pt, count, verts, stride, idx, ifmt == D3DFMT_INDEX32, prim_vertex_count(pt, count));
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT w, UINT h, UINT levels, DWORD usage,
                                           D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8 **pp)
{
    (void)self; (void)usage; (void)pool;
    return vkd_texture_create(w, h, levels, fmt, pp);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT len, DWORD usage, DWORD fvf,
                                                D3DPOOL pool, IDirect3DVertexBuffer8 **pp)
{ (void)self; (void)pool; return vkd_create_vertex_buffer(len, usage, fvf, pp); }

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT len, DWORD usage, D3DFORMAT fmt,
                                               D3DPOOL pool, IDirect3DIndexBuffer8 **pp)
{ (void)self; (void)pool; return vkd_create_index_buffer(len, usage, fmt, pp); }

/* Surfaces are not implemented by either backend (the translator draws
 * into the one scene target). */
static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT w, UINT h, D3DFORMAT f,
                                                D3DMULTISAMPLE_TYPE ms, BOOL lockable, IDirect3DSurface8 **pp)
{ (void)self; (void)w; (void)h; (void)f; (void)ms; (void)lockable; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT w, UINT h, D3DFORMAT f,
                                                       D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **pp)
{ (void)self; (void)w; (void)h; (void)f; (void)ms; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *rt, IDirect3DSurface8 *ds)
{ (void)self; (void)rt; (void)ds; return S_OK; }
static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **pp)
{ (void)self; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **pp)
{ (void)self; (void)pp; return E_NOTIMPL; }

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *vp)
{
    (void)self;
    if (vp) {
        g_vkd.viewport = *vp;               /* applied by the next draw */
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *vp)
{ (void)self; if (vp) *vp = g_vkd.viewport; return S_OK; }

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *m)
{ (void)self; if (m) g_vkd.material = *m; return S_OK; }
static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *m)
{ (void)self; if (m) *m = g_vkd.material; return S_OK; }
static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD i, const D3DLIGHT8 *l)
{ (void)self; if (i < VKD_MAX_LIGHTS && l) g_vkd.lights[i] = *l; return S_OK; }
static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD i, D3DLIGHT8 *l)
{ (void)self; if (i < VKD_MAX_LIGHTS && l) *l = g_vkd.lights[i]; return S_OK; }
static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD i, BOOL on)
{ (void)self; if (i < VKD_MAX_LIGHTS) g_vkd.light_enable[i] = on; return S_OK; }

/* The handle is the FVF (no programmable vertex shaders are created here:
 * the translator runs NV2A programs on the CPU). */
static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD h)
{ (void)self; g_vkd.vertex_shader = h; return S_OK; }
static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *h)
{ (void)self; if (h) *h = g_vkd.vertex_shader; return S_OK; }
static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT r, const void *d, DWORD n)
{ (void)self; (void)r; (void)d; (void)n; return S_OK; }   /* no programmable VS here */
/* Pixel-shader handles (D3D8 combiner programs) are recorded only: the
 * translator never sets one, and the fixed-function path draws instead. */
static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD h)
{ (void)self; g_vkd.pixel_shader = h; return S_OK; }
static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *h)
{ (void)self; if (h) *h = g_vkd.pixel_shader; return S_OK; }
static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT r, const void *d, DWORD n)
{ (void)self; (void)r; (void)d; (void)n; return S_OK; }
static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD f, const D3DGAMMARAMP *r)
{ (void)self; (void)f; (void)r; }  /* the DAC palette path carries gamma */
static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *r)
{ (void)self; (void)r; }
static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD n, const void *e)
{ (void)self; (void)n; (void)e; return S_OK; }
static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD n, DWORD **pp)
{ (void)self; (void)n; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *p)
{ (void)self; (void)p; return E_NOTIMPL; }

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD flags)
{
    (void)self; (void)flags;
    host_present();
    return S_OK;
}

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
};

IDirect3DDevice8 *d3d8_GetDevice(void) { return &g_device; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return g_vkd.initialized ? &g_device : NULL; }

/* ================================================================
 * IDirect3D8 factory
 * ================================================================ */
static IDirect3D8 g_d3d8;
static volatile LONG g_d3d8_ref;

static HRESULT __stdcall d3d_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{ (void)self; (void)riid; (void)ppv; return E_NOINTERFACE; }
static ULONG __stdcall d3d_AddRef(IDirect3D8 *self)
{ (void)self; return (ULONG)InterlockedIncrement(&g_d3d8_ref); }
static ULONG __stdcall d3d_Release(IDirect3D8 *self)
{ (void)self; return (ULONG)InterlockedDecrement(&g_d3d8_ref); }

static HRESULT __stdcall d3d_CreateDevice(IDirect3D8 *self, UINT adapter, DWORD type, HWND focus, DWORD behavior,
                                          D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    VkdState *s = &g_vkd;
    (void)self; (void)adapter; (void)type; (void)focus; (void)behavior;
    if (!pp || !out) return E_INVALIDARG;
    if (s->initialized) {               /* one device; the same one again */
        InterlockedIncrement(&s->ref);
        *out = &g_device;
        return S_OK;
    }
    memset(s, 0, sizeof *s);
    s->ref = 1;
    s->guest_w = pp->BackBufferWidth  ? pp->BackBufferWidth  : 640;
    s->guest_h = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    s->width   = s_cfg_render_w ? s_cfg_render_w : s->guest_w;
    s->height  = s_cfg_render_h ? s_cfg_render_h : s->guest_h;

    if (!g_vk.dev && !host_window_create(s->width, s->height)) return E_FAIL;
    if (!scene_targets_create((UINT)s_want_msaa)) {
        scene_targets_release();
        if (!scene_targets_create(1)) {
            fprintf(stderr, "D3D8: could not create the %ux%u scene target\n", s->width, s->height);
            return E_FAIL;
        }
    }
    init_default_states();
    if (!vkd_draw_init()) {
        fprintf(stderr, "D3D8: Vulkan draw setup failed\n");
        return E_FAIL;
    }
    g_device.lpVtbl = &g_device_vtbl;
    s->initialized = 1;
    vkd_state_invalidate();
    scene_clear_all();
    *out = &g_device;
    {
        int dw = 0, dh = 0;
        vkd_drawable_size(&dw, &dh);
        fprintf(stderr, "D3D8: rendering %ux%u (title frame %ux%u), output %dx%d, %s, %ux AA (Vulkan)\n",
                s->width, s->height, s->guest_w, s->guest_h, dw, dh,
                s_cfg_aspect > 0.0 ? "wider than 16:9" : s_cfg_widescreen ? "16:9" : "4:3", s->msaa);
        fflush(stderr);
    }
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d_QueryInterface,
    d3d_AddRef,
    d3d_Release,
    d3d_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
