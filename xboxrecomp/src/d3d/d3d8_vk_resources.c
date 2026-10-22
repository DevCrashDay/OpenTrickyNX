/*
 * d3d8_vk_resources.c -- D3D8 textures and buffers on Vulkan (XBOX_VULKAN).
 *
 * d3d8_gl_resources.c with Vulkan images: a texture keeps a CPU copy of
 * level 0 (LockRect hands it out, and readers such as the button-icon
 * builder read it back); other levels are staged until uploaded. Formats
 * map to the channel layout DXGI gives the D3D11 backend (B8G8R8A8, B5G6R5,
 * DXT1/3/5 as BC1/2/3, R8 / R8G8 for L8 / A8L8 ...), through component
 * swizzles where Vulkan's packing differs; a 16-bit format the device
 * cannot sample is widened to B8G8R8A8 on upload. Swizzled Xbox formats
 * are unswizzled on upload.
 *
 * Any thread may create, lock, unlock or release a texture (the HD texture
 * workers make theirs off the render thread): images are only created,
 * filled and destroyed on the render thread -- immediately when the caller
 * is on it, else at the texture's next bind. An upload lands ahead of the
 * frame's draws, or between them when this frame already drew with it.
 */
#include "d3d8_vk_internal.h"
#include "d3d8_swizzle.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vkd_resources_flush_deletes(void) {}   /* retired images go with their serial */

/* ================================================================
 * Formats
 * ================================================================ */
static UINT fmt_row_pitch(const VkdTexture *t, UINT width)
{
    if (t->compressed) return ((width + 3) / 4) * (t->fmt == D3DFMT_DXT1 ? 8u : 16u);
    return width * t->bytes_pp;
}

static void fmt_setup(VkdTexture *t)
{
    static const VkFormatFeatureFlags samp = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                             VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkComponentMapping id = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    t->compressed = 0;
    t->convert = VKD_CONV_NONE;
    t->swz = id;
    switch ((int)t->fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_LIN_A8R8G8B8:
        t->vkfmt = VK_FORMAT_B8G8R8A8_UNORM; t->bytes_pp = 4; break;
    case D3DFMT_X8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
        t->vkfmt = VK_FORMAT_B8G8R8A8_UNORM; t->bytes_pp = 4; t->swz.a = VK_COMPONENT_SWIZZLE_ONE; break;
    case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
        t->vkfmt = VK_FORMAT_R5G6B5_UNORM_PACK16; t->bytes_pp = 2; break;
    case D3DFMT_A1R5G5B5: case D3DFMT_LIN_A1R5G5B5:
        t->vkfmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16; t->bytes_pp = 2; break;
    case D3DFMT_A4R4G4B4: case D3DFMT_LIN_A4R4G4B4:
        /* B4G4R4A4 holds A R G B from the top bit down as "B G R A" */
        t->vkfmt = VK_FORMAT_B4G4R4A4_UNORM_PACK16; t->bytes_pp = 2;
        t->swz.r = VK_COMPONENT_SWIZZLE_G; t->swz.g = VK_COMPONENT_SWIZZLE_R;
        t->swz.b = VK_COMPONENT_SWIZZLE_A; t->swz.a = VK_COMPONENT_SWIZZLE_B;
        break;
    case D3DFMT_DXT1:
        t->compressed = 1; t->vkfmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; t->bytes_pp = 0; break;
    case D3DFMT_DXT3:
        t->compressed = 1; t->vkfmt = VK_FORMAT_BC2_UNORM_BLOCK; t->bytes_pp = 0; break;
    case D3DFMT_DXT5:
        t->compressed = 1; t->vkfmt = VK_FORMAT_BC3_UNORM_BLOCK; t->bytes_pp = 0; break;
    case D3DFMT_A8:
        t->vkfmt = VK_FORMAT_R8_UNORM; t->bytes_pp = 1;
        t->swz.r = t->swz.g = t->swz.b = VK_COMPONENT_SWIZZLE_ZERO; t->swz.a = VK_COMPONENT_SWIZZLE_R;
        break;
    case D3DFMT_L8:                     /* DXGI R8_UNORM: (L, 0, 0, 1), as D3D11 */
    case D3DFMT_P8:
        t->vkfmt = VK_FORMAT_R8_UNORM; t->bytes_pp = 1; break;
    case D3DFMT_A8L8:                   /* DXGI R8G8_UNORM */
        t->vkfmt = VK_FORMAT_R8G8_UNORM; t->bytes_pp = 2; break;
    default:
        fprintf(stderr, "D3D8: Unknown format 0x%X, using R8G8B8A8\n", (unsigned)t->fmt);
        t->vkfmt = VK_FORMAT_R8G8B8A8_UNORM; t->bytes_pp = 4; break;
    }
    if (t->bytes_pp == 2 && t->vkfmt != VK_FORMAT_R8G8_UNORM && g_vk.dev && !vkd_format_ok(t->vkfmt, samp, 0)) {
        t->vkfmt = VK_FORMAT_B8G8R8A8_UNORM;
        t->convert = VKD_CONV_TO_BGRA8;
        t->swz = id;
    }
    if (t->compressed && g_vk.dev && !g_vk.has_bc) {
        static int told;
        if (!told++) fprintf(stderr, "[VK] warning: no BC texture support; DXT textures will be black\n");
    }
    t->swizzled = !t->compressed && d3d8_format_is_swizzled((uint32_t)t->fmt);
}

static void level_dims(const VkdTexture *t, UINT level, UINT *w, UINT *h, UINT *pitch, UINT *rows)
{
    UINT lw = t->width >> level, lh = t->height >> level;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    *w = lw; *h = lh;
    *pitch = fmt_row_pitch(t, lw);
    *rows = t->compressed ? (lh + 3) / 4 : lh;
}

static unsigned char x5(unsigned v) { return (unsigned char)((v << 3) | (v >> 2)); }
static unsigned char x6(unsigned v) { return (unsigned char)((v << 2) | (v >> 4)); }
static unsigned char x4(unsigned v) { return (unsigned char)(v * 17u); }

/* 16-bit D3D texels -> B G R A bytes. */
static void widen16(const VkdTexture *t, const BYTE *src, BYTE *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v = (unsigned)src[i * 2] | ((unsigned)src[i * 2 + 1] << 8);
        BYTE *d = dst + i * 4;
        switch ((int)t->fmt) {
        case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
            d[0] = x5(v & 31); d[1] = x6((v >> 5) & 63); d[2] = x5(v >> 11); d[3] = 255; break;
        case D3DFMT_A1R5G5B5: case D3DFMT_LIN_A1R5G5B5:
            d[0] = x5(v & 31); d[1] = x5((v >> 5) & 31); d[2] = x5((v >> 10) & 31); d[3] = (v & 0x8000) ? 255 : 0; break;
        default:                        /* A4R4G4B4 */
            d[0] = x4(v & 15); d[1] = x4((v >> 4) & 15); d[2] = x4((v >> 8) & 15); d[3] = x4(v >> 12); break;
        }
    }
}

/* ================================================================
 * GPU side (render thread)
 * ================================================================ */
static void tex_make_storage(VkdTexture *t)
{
    UINT i;
    if (!vkd_image_create(&t->img, t->width, t->height, t->levels, t->vkfmt,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, &t->swz))
        return;
    t->have_img = 1;
    if (!t->compressed) {               /* zeros until a level is written */
        VkClearColorValue z;
        VkImageSubresourceRange r;
        memset(&z, 0, sizeof z);
        memset(&r, 0, sizeof r);
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.levelCount = t->levels;
        r.layerCount = 1;
        vkCmdClearColorImage(vkd_cmd_pre(), t->img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
        return;
    }
    for (i = 0; i < t->levels; i++) {
        UINT w, h, pitch, rows;
        void *z;
        if ((t->dirty & (1u << i)) && t->lvl[i]) continue;
        level_dims(t, i, &w, &h, &pitch, &rows);
        z = calloc(1, (size_t)pitch * rows);
        if (z) vkd_image_upload(0, &t->img, i, 0, 0, w, h, z, (size_t)pitch * rows, 0);
        free(z);
    }
}

static void tex_upload_level(VkdTexture *t, UINT level)
{
    UINT w, h, pitch, rows;
    const BYTE *src = t->lvl[level];
    BYTE *unswz = NULL, *wide = NULL;
    size_t size;
    if (!src || !t->have_img) return;
    level_dims(t, level, &w, &h, &pitch, &rows);
    size = (size_t)pitch * rows;
    if (t->swizzled) {
        unswz = (BYTE *)malloc((size_t)w * h * t->bytes_pp);
        if (unswz) { xbox_unswizzle_rect(unswz, src, w, h, t->bytes_pp); src = unswz; }
    }
    if (t->convert == VKD_CONV_TO_BGRA8 && (wide = (BYTE *)malloc((size_t)w * h * 4u))) {
        widen16(t, src, wide, (size_t)w * h);
        src = wide;
        size = (size_t)w * h * 4u;
    }
    /* drawn with already in this submission: between those draws and the next */
    vkd_image_upload(t->last_use == vkd_serial(), &t->img, level, 0, 0, w, h, src, size, 0);
    free(unswz);
    free(wide);
}

/* Upload every pending level; free the staged copies that are not kept. */
static void tex_flush(VkdTexture *t)
{
    UINT i;
    unsigned dirty;
    SDL_AtomicLock((SDL_SpinLock *)&t->pending_lock);
    if (!t->have_img && !t->gpu_only) tex_make_storage(t);
    dirty = t->dirty;
    t->dirty = 0;
    for (i = 0; i < t->levels && dirty; i++) {
        if (!(dirty & (1u << i))) continue;
        dirty &= ~(1u << i);
        if (t->locked && t->lock_level == i) { t->dirty |= 1u << i; continue; }
        tex_upload_level(t, i);
        if (i > 0 || t->immutable) { free(t->lvl[i]); t->lvl[i] = NULL; }
    }
    SDL_AtomicUnlock((SDL_SpinLock *)&t->pending_lock);
}

VkImageView vkd_texture_ensure(IDirect3DBaseTexture8 *bt)
{
    VkdTexture *t = (VkdTexture *)bt;
    if (!t) return VK_NULL_HANDLE;
    if (!t->have_img || t->dirty) tex_flush(t);
    if (!t->have_img) return VK_NULL_HANDLE;
    t->last_use = vkd_serial();
    return t->img.view;
}

VkdImage *vkd_texture_image(IDirect3DTexture8 *tex)
{
    VkdTexture *t = (VkdTexture *)tex;
    return t && t->have_img ? &t->img : NULL;
}

/* ================================================================
 * IDirect3DTexture8
 * ================================================================ */
static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *self, const IID *riid, void **ppv)
{ (void)self; (void)riid; (void)ppv; return E_NOINTERFACE; }

static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *self)
{ return (ULONG)InterlockedIncrement(&((VkdTexture *)self)->ref); }

static ULONG __stdcall tex_Release(IDirect3DTexture8 *self)
{
    VkdTexture *t = (VkdTexture *)self;
    LONG ref = InterlockedDecrement(&t->ref);
    if (ref <= 0) {
        UINT i;
        if (g_vkd.initialized)
            for (i = 0; i < VKD_MAX_STAGES; i++)
                if (g_vkd.tex[i] == (IDirect3DBaseTexture8 *)self) g_vkd.tex[i] = NULL;
        if (t->have_img) vkd_image_retire_at(&t->img, t->last_use);
        for (i = 0; i < VKD_MAX_LEVELS; i++) free(t->lvl[i]);
        free(t);
        return 0;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall tex_GetDevice(IDirect3DTexture8 *self, IDirect3DDevice8 **pp)
{ (void)self; if (pp) *pp = xbox_GetD3DDevice(); return S_OK; }
static DWORD __stdcall tex_SetPriority(IDirect3DTexture8 *self, DWORD p) { (void)self; (void)p; return 0; }
static DWORD __stdcall tex_GetPriority(IDirect3DTexture8 *self) { (void)self; return 0; }
static void  __stdcall tex_PreLoad(IDirect3DTexture8 *self) { (void)self; }
static DWORD __stdcall tex_GetType(IDirect3DTexture8 *self) { (void)self; return 5; /* D3DRTYPE_TEXTURE */ }
static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *self) { return ((VkdTexture *)self)->levels; }

static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *self, UINT level, D3DSURFACE_DESC *d)
{
    VkdTexture *t = (VkdTexture *)self;
    if (!d || level >= t->levels) return E_INVALIDARG;
    memset(d, 0, sizeof *d);
    d->Format = t->fmt;
    d->Width = t->width >> level;
    d->Height = t->height >> level;
    if (d->Width < 1) d->Width = 1;
    if (d->Height < 1) d->Height = 1;
    d->Pool = D3DPOOL_DEFAULT;
    return S_OK;
}

static HRESULT __stdcall tex_GetSurfaceLevel(IDirect3DTexture8 *self, UINT level, IDirect3DSurface8 **pp)
{ (void)self; (void)level; (void)pp; return E_NOTIMPL; }

static HRESULT __stdcall tex_LockRect(IDirect3DTexture8 *self, UINT level, D3DLOCKED_RECT *lr,
                                      const RECT *rect, DWORD flags)
{
    VkdTexture *t = (VkdTexture *)self;
    UINT w, h, pitch, rows;
    (void)rect; (void)flags;
    if (!lr || level >= t->levels) return E_INVALIDARG;
    if (t->locked || t->immutable || t->gpu_only) return E_FAIL;
    level_dims(t, level, &w, &h, &pitch, &rows);
    SDL_AtomicLock((SDL_SpinLock *)&t->pending_lock);
    if (!t->lvl[level]) t->lvl[level] = (BYTE *)calloc(1, (size_t)pitch * rows);
    SDL_AtomicUnlock((SDL_SpinLock *)&t->pending_lock);
    if (!t->lvl[level]) return E_OUTOFMEMORY;
    lr->Pitch = (INT)pitch;
    lr->pBits = t->lvl[level];
    t->lock_level = level;
    t->locked = 1;
    return S_OK;
}

static HRESULT __stdcall tex_UnlockRect(IDirect3DTexture8 *self, UINT level)
{
    VkdTexture *t = (VkdTexture *)self;
    if (!t->locked || level != t->lock_level) return E_FAIL;
    SDL_AtomicLock((SDL_SpinLock *)&t->pending_lock);
    t->locked = 0;
    t->dirty |= 1u << level;
    SDL_AtomicUnlock((SDL_SpinLock *)&t->pending_lock);
    /* On the render thread the level goes up now, like UpdateSubresource. */
    if (vkd_on_render_thread() && g_vkd.initialized) tex_flush(t);
    return S_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

static VkdTexture *tex_alloc(UINT w, UINT h, UINT levels, D3DFORMAT fmt)
{
    VkdTexture *t;
    if (!w || !h) return NULL;
    t = (VkdTexture *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->iface.lpVtbl = &g_tex_vtbl;
    t->ref = 1;
    t->width = w;
    t->height = h;
    t->levels = levels ? levels : 1;
    if (t->levels > VKD_MAX_LEVELS) t->levels = VKD_MAX_LEVELS;
    t->fmt = fmt;
    fmt_setup(t);
    return t;
}

HRESULT vkd_texture_create(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture8 **out)
{
    VkdTexture *t;
    UINT lw, lh, pitch, rows;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    t = tex_alloc(w, h, levels, fmt);
    if (!t) return E_OUTOFMEMORY;
    level_dims(t, 0, &lw, &lh, &pitch, &rows);
    t->pitch0 = pitch;
    t->lvl[0] = (BYTE *)calloc(1, (size_t)pitch * rows);   /* the long-lived level-0 copy */
    if (!t->lvl[0]) { free(t); return E_OUTOFMEMORY; }
    if (vkd_on_render_thread() && g_vkd.initialized) tex_make_storage(t);
    *out = &t->iface;
    return S_OK;
}

/* See d3d8_xbox.h: BGRA levels, tightly packed; never lockable. */
HRESULT d3d8_CreateTextureFromLevels(UINT Width, UINT Height, UINT Levels,
                                     const void *const *level_bits, IDirect3DTexture8 **ppTex)
{
    VkdTexture *t;
    UINT i;
    if (!ppTex || !level_bits || !Width || !Height || !Levels || Levels > 16) return E_INVALIDARG;
    *ppTex = NULL;
    t = tex_alloc(Width, Height, Levels, D3DFMT_LIN_A8R8G8B8);
    if (!t) return E_OUTOFMEMORY;
    t->immutable = 1;
    for (i = 0; i < t->levels; i++) {
        UINT lw, lh, pitch, rows;
        level_dims(t, i, &lw, &lh, &pitch, &rows);
        if (!level_bits[i] || !(t->lvl[i] = (BYTE *)malloc((size_t)pitch * rows))) {
            tex_Release(&t->iface);
            return level_bits[i] ? E_OUTOFMEMORY : E_INVALIDARG;
        }
        memcpy(t->lvl[i], level_bits[i], (size_t)pitch * rows);
        t->dirty |= 1u << i;
    }
    if (vkd_on_render_thread() && g_vkd.initialized) tex_flush(t);
    *ppTex = &t->iface;
    return S_OK;
}

HRESULT vkd_texture_create_gpu(UINT w, UINT h, IDirect3DTexture8 **out)
{
    VkdTexture *t;
    if (!out || !vkd_on_render_thread()) return E_FAIL;
    t = tex_alloc(w, h, 1, D3DFMT_A8R8G8B8);
    if (!t) return E_OUTOFMEMORY;
    t->gpu_only = 1;
    t->vkfmt = g_vkd.color_fmt;
    if (!vkd_image_create(&t->img, w, h, 1, t->vkfmt,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, NULL)) {
        free(t);
        return E_OUTOFMEMORY;
    }
    t->have_img = 1;
    *out = &t->iface;
    return S_OK;
}

/* ================================================================
 * Vertex / index buffers: system memory only; draws go through the UP path
 * ================================================================ */
typedef struct {
    IDirect3DVertexBuffer8 iface;
    volatile LONG ref;
    UINT  size;
    DWORD fvf, usage;
    BYTE *mem;
    int   locked;
} VkdVB;

typedef struct {
    IDirect3DIndexBuffer8 iface;
    volatile LONG ref;
    UINT  size;
    D3DFORMAT fmt;
    DWORD usage;
    BYTE *mem;
    int   locked;
} VkdIB;

static HRESULT __stdcall vb_QueryInterface(IDirect3DVertexBuffer8 *s, const IID *r, void **p)
{ (void)s; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall vb_AddRef(IDirect3DVertexBuffer8 *s) { return (ULONG)InterlockedIncrement(&((VkdVB *)s)->ref); }
static ULONG __stdcall vb_Release(IDirect3DVertexBuffer8 *s)
{
    VkdVB *b = (VkdVB *)s;
    LONG ref = InterlockedDecrement(&b->ref);
    if (ref <= 0) {
        if (g_vkd.vb == s) g_vkd.vb = NULL;
        free(b->mem);
        free(b);
        return 0;
    }
    return (ULONG)ref;
}
static HRESULT __stdcall vb_GetDevice(IDirect3DVertexBuffer8 *s, IDirect3DDevice8 **pp)
{ (void)s; if (pp) *pp = xbox_GetD3DDevice(); return S_OK; }
static DWORD __stdcall vb_SetPriority(IDirect3DVertexBuffer8 *s, DWORD p) { (void)s; (void)p; return 0; }
static DWORD __stdcall vb_GetPriority(IDirect3DVertexBuffer8 *s) { (void)s; return 0; }
static void  __stdcall vb_PreLoad(IDirect3DVertexBuffer8 *s) { (void)s; }
static DWORD __stdcall vb_GetType(IDirect3DVertexBuffer8 *s) { (void)s; return 3; }
static HRESULT __stdcall vb_Lock(IDirect3DVertexBuffer8 *s, UINT off, UINT size, BYTE **pp, DWORD flags)
{
    VkdVB *b = (VkdVB *)s;
    (void)size; (void)flags;
    if (!pp || off > b->size) return E_INVALIDARG;
    if (b->locked) return E_FAIL;
    *pp = b->mem + off;
    b->locked = 1;
    return S_OK;
}
static HRESULT __stdcall vb_Unlock(IDirect3DVertexBuffer8 *s)
{
    VkdVB *b = (VkdVB *)s;
    if (!b->locked) return E_FAIL;
    b->locked = 0;
    return S_OK;
}
static HRESULT __stdcall vb_GetDesc(IDirect3DVertexBuffer8 *s, void *d) { (void)s; (void)d; return E_NOTIMPL; }

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    vb_QueryInterface, vb_AddRef, vb_Release, vb_GetDevice, vb_SetPriority, vb_GetPriority,
    vb_PreLoad, vb_GetType, vb_Lock, vb_Unlock, vb_GetDesc,
};

HRESULT vkd_create_vertex_buffer(UINT len, DWORD usage, DWORD fvf, IDirect3DVertexBuffer8 **out)
{
    VkdVB *b;
    if (!out) return E_INVALIDARG;
    b = (VkdVB *)calloc(1, sizeof *b);
    if (!b) return E_OUTOFMEMORY;
    b->mem = (BYTE *)calloc(1, len ? len : 1);
    if (!b->mem) { free(b); return E_OUTOFMEMORY; }
    b->iface.lpVtbl = &g_vb_vtbl;
    b->ref = 1;
    b->size = len;
    b->fvf = fvf;
    b->usage = usage;
    *out = &b->iface;
    return S_OK;
}

const BYTE *vkd_vb_data(IDirect3DVertexBuffer8 *vb, UINT *size)
{
    VkdVB *b = (VkdVB *)vb;
    if (!b) return NULL;
    *size = b->size;
    return b->mem;
}

static HRESULT __stdcall ib_QueryInterface(IDirect3DIndexBuffer8 *s, const IID *r, void **p)
{ (void)s; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall ib_AddRef(IDirect3DIndexBuffer8 *s) { return (ULONG)InterlockedIncrement(&((VkdIB *)s)->ref); }
static ULONG __stdcall ib_Release(IDirect3DIndexBuffer8 *s)
{
    VkdIB *b = (VkdIB *)s;
    LONG ref = InterlockedDecrement(&b->ref);
    if (ref <= 0) {
        if (g_vkd.ib == s) g_vkd.ib = NULL;
        free(b->mem);
        free(b);
        return 0;
    }
    return (ULONG)ref;
}
static HRESULT __stdcall ib_GetDevice(IDirect3DIndexBuffer8 *s, IDirect3DDevice8 **pp)
{ (void)s; if (pp) *pp = xbox_GetD3DDevice(); return S_OK; }
static DWORD __stdcall ib_SetPriority(IDirect3DIndexBuffer8 *s, DWORD p) { (void)s; (void)p; return 0; }
static DWORD __stdcall ib_GetPriority(IDirect3DIndexBuffer8 *s) { (void)s; return 0; }
static void  __stdcall ib_PreLoad(IDirect3DIndexBuffer8 *s) { (void)s; }
static DWORD __stdcall ib_GetType(IDirect3DIndexBuffer8 *s) { (void)s; return 4; }
static HRESULT __stdcall ib_Lock(IDirect3DIndexBuffer8 *s, UINT off, UINT size, BYTE **pp, DWORD flags)
{
    VkdIB *b = (VkdIB *)s;
    (void)size; (void)flags;
    if (!pp || off > b->size) return E_INVALIDARG;
    if (b->locked) return E_FAIL;
    *pp = b->mem + off;
    b->locked = 1;
    return S_OK;
}
static HRESULT __stdcall ib_Unlock(IDirect3DIndexBuffer8 *s)
{
    VkdIB *b = (VkdIB *)s;
    if (!b->locked) return E_FAIL;
    b->locked = 0;
    return S_OK;
}
static HRESULT __stdcall ib_GetDesc(IDirect3DIndexBuffer8 *s, void *d) { (void)s; (void)d; return E_NOTIMPL; }

static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    ib_QueryInterface, ib_AddRef, ib_Release, ib_GetDevice, ib_SetPriority, ib_GetPriority,
    ib_PreLoad, ib_GetType, ib_Lock, ib_Unlock, ib_GetDesc,
};

HRESULT vkd_create_index_buffer(UINT len, DWORD usage, D3DFORMAT fmt, IDirect3DIndexBuffer8 **out)
{
    VkdIB *b;
    if (!out) return E_INVALIDARG;
    b = (VkdIB *)calloc(1, sizeof *b);
    if (!b) return E_OUTOFMEMORY;
    b->mem = (BYTE *)calloc(1, len ? len : 1);
    if (!b->mem) { free(b); return E_OUTOFMEMORY; }
    b->iface.lpVtbl = &g_ib_vtbl;
    b->ref = 1;
    b->size = len;
    b->fmt = fmt;
    b->usage = usage;
    *out = &b->iface;
    return S_OK;
}

const BYTE *vkd_ib_data(IDirect3DIndexBuffer8 *ib, UINT *size, D3DFORMAT *fmt)
{
    VkdIB *b = (VkdIB *)ib;
    if (!b) return NULL;
    *size = b->size;
    *fmt = b->fmt;
    return b->mem;
}
