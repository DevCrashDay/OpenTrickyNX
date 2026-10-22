/*
 * d3d8_gl_resources.c -- D3D8 textures and buffers on OpenGL (non-Windows).
 *
 * Mirrors d3d8_resources.c: a texture keeps a CPU copy of level 0 (LockRect
 * hands it out, and readers such as the button-icon builder read it back);
 * other levels are staged until uploaded. Formats map to the same channel
 * layout DXGI gives the D3D11 backend (B8G8R8A8, B5G6R5, DXT1/3/5 as S3TC,
 * R8 / R8G8 for L8 / A8L8 ...). Swizzled Xbox formats are unswizzled on
 * upload, exactly as d3d8_resources.c does.
 *
 * Any thread may create, lock, unlock or release a texture (the HD texture
 * workers make theirs off the render thread): GL objects are only created,
 * filled and deleted on the render thread -- immediately when the caller is
 * on it, else at the texture's next bind or the next present.
 */
#include "d3d8_gl_internal.h"
#include "d3d8_swizzle.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Deferred deletes (releases on other threads)
 * ================================================================ */
static SDL_SpinLock s_del_lock;
static GLuint *s_del;
static size_t  s_del_n, s_del_cap;

void gld_delete_texture_name(GLuint name)
{
    if (!name) return;
    if (gld_on_gl_thread()) { glDeleteTextures(1, &name); gld_state_invalidate(); return; }
    SDL_AtomicLock(&s_del_lock);
    if (s_del_n == s_del_cap) {
        size_t cap = s_del_cap ? s_del_cap * 2 : 64;
        GLuint *n = (GLuint *)realloc(s_del, cap * sizeof *n);
        if (n) { s_del = n; s_del_cap = cap; }
    }
    if (s_del_n < s_del_cap) s_del[s_del_n++] = name;  /* else leaked, never freed twice */
    SDL_AtomicUnlock(&s_del_lock);
}

void gld_resources_flush_deletes(void)
{
    GLuint *v = NULL;
    size_t n = 0;
    if (!s_del_n) return;
    SDL_AtomicLock(&s_del_lock);
    v = s_del; n = s_del_n;
    s_del = NULL; s_del_n = s_del_cap = 0;
    SDL_AtomicUnlock(&s_del_lock);
    if (n) { glDeleteTextures((GLsizei)n, v); gld_state_invalidate(); }
    free(v);
}

/* ================================================================
 * Formats
 * ================================================================ */
/* Bytes of one row of `width` texels (rows of 4x4 blocks when compressed). */
static UINT fmt_row_pitch(const GldTexture *t, UINT width)
{
    if (t->compressed) return ((width + 3) / 4) * (t->fmt == D3DFMT_DXT1 ? 8u : 16u);
    return width * t->bytes_pp;
}

static void fmt_setup(GldTexture *t)
{
    t->compressed = 0;
    t->swz = GLD_SWZ_NONE;
    t->ptype = GL_UNSIGNED_BYTE;
    switch ((int)t->fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_LIN_A8R8G8B8:
        t->ifmt = GL_RGBA8; t->pfmt = GL_BGRA; t->bytes_pp = 4; break;
    case D3DFMT_X8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
        t->ifmt = GL_RGBA8; t->pfmt = GL_BGRA; t->bytes_pp = 4; t->swz = GLD_SWZ_X1; break;
    case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
        t->ifmt = GL_RGB8; t->pfmt = GL_RGB; t->ptype = GL_UNSIGNED_SHORT_5_6_5; t->bytes_pp = 2; break;
    case D3DFMT_A1R5G5B5: case D3DFMT_LIN_A1R5G5B5:
        t->ifmt = GL_RGB5_A1; t->pfmt = GL_BGRA; t->ptype = GL_UNSIGNED_SHORT_1_5_5_5_REV; t->bytes_pp = 2; break;
    case D3DFMT_A4R4G4B4: case D3DFMT_LIN_A4R4G4B4:
        t->ifmt = GL_RGBA4; t->pfmt = GL_BGRA; t->ptype = GL_UNSIGNED_SHORT_4_4_4_4_REV; t->bytes_pp = 2; break;
    case D3DFMT_DXT1:
        t->compressed = 1; t->ifmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; t->bytes_pp = 0; break;
    case D3DFMT_DXT3:
        t->compressed = 1; t->ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; t->bytes_pp = 0; break;
    case D3DFMT_DXT5:
        t->compressed = 1; t->ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; t->bytes_pp = 0; break;
    case D3DFMT_A8:
        t->ifmt = GL_R8; t->pfmt = GL_RED; t->bytes_pp = 1; t->swz = GLD_SWZ_A8; break;
    case D3DFMT_L8:                     /* DXGI R8_UNORM: (L, 0, 0, 1), as D3D11 */
    case D3DFMT_P8:
        t->ifmt = GL_R8; t->pfmt = GL_RED; t->bytes_pp = 1; break;
    case D3DFMT_A8L8:                   /* DXGI R8G8_UNORM */
        t->ifmt = GL_RG8; t->pfmt = GL_RG; t->bytes_pp = 2; break;
    default:
        fprintf(stderr, "D3D8: Unknown format 0x%X, using R8G8B8A8\n", (unsigned)t->fmt);
        t->ifmt = GL_RGBA8; t->pfmt = GL_RGBA; t->bytes_pp = 4; break;
    }
    t->swizzled = !t->compressed && d3d8_format_is_swizzled((uint32_t)t->fmt);
}

static void level_dims(const GldTexture *t, UINT level, UINT *w, UINT *h, UINT *pitch, UINT *rows)
{
    UINT lw = t->width >> level, lh = t->height >> level;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    *w = lw; *h = lh;
    *pitch = fmt_row_pitch(t, lw);
    *rows = t->compressed ? (lh + 3) / 4 : lh;
}

/* ================================================================
 * GL side (render thread)
 * ================================================================ */
static void tex_make_storage(GldTexture *t)
{
    UINT i;
    glGenTextures(1, &t->name);
    gld_scratch_bind(t->name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(t->levels - 1));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    t->levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (t->swz == GLD_SWZ_X1) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    } else if (t->swz == GLD_SWZ_A8) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (i = 0; i < t->levels; i++) {
        UINT w, h, pitch, rows;
        level_dims(t, i, &w, &h, &pitch, &rows);
        if (t->compressed) {
            /* Storage must be defined with data: zeros until the level is written. */
            void *z = (t->dirty & (1u << i)) && t->lvl[i] ? NULL : calloc(1, (size_t)pitch * rows);
            if (z) {
                glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)i, t->ifmt, (GLsizei)w, (GLsizei)h, 0,
                                       (GLsizei)(pitch * rows), z);
                free(z);
            } else if (!((t->dirty & (1u << i)) && t->lvl[i])) {
                break;
            }
        } else {
            glTexImage2D(GL_TEXTURE_2D, (GLint)i, (GLint)t->ifmt, (GLsizei)w, (GLsizei)h, 0,
                         t->pfmt, t->ptype, NULL);
        }
    }
}

static void tex_upload_level(GldTexture *t, UINT level)
{
    UINT w, h, pitch, rows;
    const BYTE *src = t->lvl[level];
    BYTE *unswz = NULL;
    if (!src) return;
    level_dims(t, level, &w, &h, &pitch, &rows);
    if (t->swizzled) {
        unswz = (BYTE *)malloc((size_t)w * h * t->bytes_pp);
        if (unswz) { xbox_unswizzle_rect(unswz, src, w, h, t->bytes_pp); src = unswz; }
    }
    gld_scratch_bind(t->name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (t->compressed)
        glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)level, t->ifmt, (GLsizei)w, (GLsizei)h, 0,
                               (GLsizei)(pitch * rows), src);
    else
        glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h, t->pfmt, t->ptype, src);
    free(unswz);
}

/* Upload every pending level; free the staged copies that are not kept. */
static void tex_flush(GldTexture *t)
{
    UINT i;
    unsigned dirty;
    SDL_AtomicLock((SDL_SpinLock *)&t->pending_lock);
    if (!t->name && !t->gpu_only) tex_make_storage(t);
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

GLuint gld_texture_ensure(IDirect3DBaseTexture8 *bt)
{
    GldTexture *t = (GldTexture *)bt;
    if (!t) return 0;
    if (!t->name || t->dirty) tex_flush(t);
    return t->name;
}

/* ================================================================
 * IDirect3DTexture8
 * ================================================================ */
static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *self, const IID *riid, void **ppv)
{ (void)self; (void)riid; (void)ppv; return E_NOINTERFACE; }

static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *self)
{ return (ULONG)InterlockedIncrement(&((GldTexture *)self)->ref); }

static ULONG __stdcall tex_Release(IDirect3DTexture8 *self)
{
    GldTexture *t = (GldTexture *)self;
    LONG ref = InterlockedDecrement(&t->ref);
    if (ref <= 0) {
        UINT i;
        if (g_gld.initialized)
            for (i = 0; i < GLD_MAX_STAGES; i++)
                if (g_gld.tex[i] == (IDirect3DBaseTexture8 *)self) g_gld.tex[i] = NULL;
        gld_delete_texture_name(t->name);
        for (i = 0; i < GLD_MAX_LEVELS; i++) free(t->lvl[i]);
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
static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *self) { return ((GldTexture *)self)->levels; }

static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *self, UINT level, D3DSURFACE_DESC *d)
{
    GldTexture *t = (GldTexture *)self;
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
    GldTexture *t = (GldTexture *)self;
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
    GldTexture *t = (GldTexture *)self;
    if (!t->locked || level != t->lock_level) return E_FAIL;
    SDL_AtomicLock((SDL_SpinLock *)&t->pending_lock);
    t->locked = 0;
    t->dirty |= 1u << level;
    SDL_AtomicUnlock((SDL_SpinLock *)&t->pending_lock);
    /* On the render thread the level goes up now, like UpdateSubresource. */
    if (gld_on_gl_thread() && g_gld.initialized) tex_flush(t);
    return S_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

static GldTexture *tex_alloc(UINT w, UINT h, UINT levels, D3DFORMAT fmt)
{
    GldTexture *t;
    if (!w || !h) return NULL;
    t = (GldTexture *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->iface.lpVtbl = &g_tex_vtbl;
    t->ref = 1;
    t->width = w;
    t->height = h;
    t->levels = levels ? levels : 1;
    if (t->levels > GLD_MAX_LEVELS) t->levels = GLD_MAX_LEVELS;
    t->fmt = fmt;
    fmt_setup(t);
    return t;
}

HRESULT gld_texture_create(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture8 **out)
{
    GldTexture *t;
    UINT lw, lh, pitch, rows;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    t = tex_alloc(w, h, levels, fmt);
    if (!t) return E_OUTOFMEMORY;
    level_dims(t, 0, &lw, &lh, &pitch, &rows);
    t->pitch0 = pitch;
    t->lvl[0] = (BYTE *)calloc(1, (size_t)pitch * rows);   /* the long-lived level-0 copy */
    if (!t->lvl[0]) { free(t); return E_OUTOFMEMORY; }
    if (gld_on_gl_thread() && g_gld.initialized) tex_make_storage(t);
    *out = &t->iface;
    return S_OK;
}

/* See d3d8_xbox.h: BGRA levels, tightly packed; never lockable. */
HRESULT d3d8_CreateTextureFromLevels(UINT Width, UINT Height, UINT Levels,
                                     const void *const *level_bits, IDirect3DTexture8 **ppTex)
{
    GldTexture *t;
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
    if (gld_on_gl_thread() && g_gld.initialized) tex_flush(t);
    *ppTex = &t->iface;
    return S_OK;
}

HRESULT gld_texture_create_gpu(UINT w, UINT h, IDirect3DTexture8 **out)
{
    GldTexture *t;
    if (!out || !gld_on_gl_thread()) return E_FAIL;
    t = tex_alloc(w, h, 1, D3DFMT_A8R8G8B8);
    if (!t) return E_OUTOFMEMORY;
    t->gpu_only = 1;
    glGenTextures(1, &t->name);
    gld_scratch_bind(t->name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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
} GldVB;

typedef struct {
    IDirect3DIndexBuffer8 iface;
    volatile LONG ref;
    UINT  size;
    D3DFORMAT fmt;
    DWORD usage;
    BYTE *mem;
    int   locked;
} GldIB;

static HRESULT __stdcall vb_QueryInterface(IDirect3DVertexBuffer8 *s, const IID *r, void **p)
{ (void)s; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall vb_AddRef(IDirect3DVertexBuffer8 *s) { return (ULONG)InterlockedIncrement(&((GldVB *)s)->ref); }
static ULONG __stdcall vb_Release(IDirect3DVertexBuffer8 *s)
{
    GldVB *b = (GldVB *)s;
    LONG ref = InterlockedDecrement(&b->ref);
    if (ref <= 0) {
        if (g_gld.vb == s) g_gld.vb = NULL;
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
    GldVB *b = (GldVB *)s;
    (void)size; (void)flags;
    if (!pp || off > b->size) return E_INVALIDARG;
    if (b->locked) return E_FAIL;
    *pp = b->mem + off;
    b->locked = 1;
    return S_OK;
}
static HRESULT __stdcall vb_Unlock(IDirect3DVertexBuffer8 *s)
{
    GldVB *b = (GldVB *)s;
    if (!b->locked) return E_FAIL;
    b->locked = 0;
    return S_OK;
}
static HRESULT __stdcall vb_GetDesc(IDirect3DVertexBuffer8 *s, void *d) { (void)s; (void)d; return E_NOTIMPL; }

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    vb_QueryInterface, vb_AddRef, vb_Release, vb_GetDevice, vb_SetPriority, vb_GetPriority,
    vb_PreLoad, vb_GetType, vb_Lock, vb_Unlock, vb_GetDesc,
};

HRESULT gld_create_vertex_buffer(UINT len, DWORD usage, DWORD fvf, IDirect3DVertexBuffer8 **out)
{
    GldVB *b;
    if (!out) return E_INVALIDARG;
    b = (GldVB *)calloc(1, sizeof *b);
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

const BYTE *gld_vb_data(IDirect3DVertexBuffer8 *vb, UINT *size)
{
    GldVB *b = (GldVB *)vb;
    if (!b) return NULL;
    *size = b->size;
    return b->mem;
}

static HRESULT __stdcall ib_QueryInterface(IDirect3DIndexBuffer8 *s, const IID *r, void **p)
{ (void)s; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall ib_AddRef(IDirect3DIndexBuffer8 *s) { return (ULONG)InterlockedIncrement(&((GldIB *)s)->ref); }
static ULONG __stdcall ib_Release(IDirect3DIndexBuffer8 *s)
{
    GldIB *b = (GldIB *)s;
    LONG ref = InterlockedDecrement(&b->ref);
    if (ref <= 0) {
        if (g_gld.ib == s) g_gld.ib = NULL;
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
    GldIB *b = (GldIB *)s;
    (void)size; (void)flags;
    if (!pp || off > b->size) return E_INVALIDARG;
    if (b->locked) return E_FAIL;
    *pp = b->mem + off;
    b->locked = 1;
    return S_OK;
}
static HRESULT __stdcall ib_Unlock(IDirect3DIndexBuffer8 *s)
{
    GldIB *b = (GldIB *)s;
    if (!b->locked) return E_FAIL;
    b->locked = 0;
    return S_OK;
}
static HRESULT __stdcall ib_GetDesc(IDirect3DIndexBuffer8 *s, void *d) { (void)s; (void)d; return E_NOTIMPL; }

static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    ib_QueryInterface, ib_AddRef, ib_Release, ib_GetDevice, ib_SetPriority, ib_GetPriority,
    ib_PreLoad, ib_GetType, ib_Lock, ib_Unlock, ib_GetDesc,
};

HRESULT gld_create_index_buffer(UINT len, DWORD usage, D3DFORMAT fmt, IDirect3DIndexBuffer8 **out)
{
    GldIB *b;
    if (!out) return E_INVALIDARG;
    b = (GldIB *)calloc(1, sizeof *b);
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

const BYTE *gld_ib_data(IDirect3DIndexBuffer8 *ib, UINT *size, D3DFORMAT *fmt)
{
    GldIB *b = (GldIB *)ib;
    if (!b) return NULL;
    *size = b->size;
    *fmt = b->fmt;
    return b->mem;
}
