/*
 * d3d8_gl_internal.h -- shared state of the OpenGL backend (non-Windows).
 *
 * The backend is three files:
 *   d3d8_gl_device.c     window + context (SDL2), the IDirect3DDevice8 vtable,
 *                        the factory, the scene target and the present chain
 *                        (guest framebuffer, DAC palette, letterbox), and the
 *                        host-facing d3d8_* entry points.
 *   d3d8_gl_resources.c  textures (CreateTexture / LockRect / UnlockRect,
 *                        d3d8_CreateTextureFromLevels), vertex/index buffers.
 *   d3d8_gl_draw.c       render-state application, the fixed-function path
 *                        (DrawPrimitiveUP and friends), and the NV2A-native
 *                        draw with its GLSL pixel-shader cache.
 *
 * Every GL call happens on the thread that created the device (the PFIFO
 * pump thread, see nv2a_live_pb.c). Resources may be created and released
 * on other threads (the HD texture workers): those paths only touch CPU
 * memory and queue the GL work for the render thread.
 */
#ifndef D3D8_GL_INTERNAL_H
#define D3D8_GL_INTERNAL_H

#include "d3d8_xbox.h"
#include "d3d8_gl_loader.h"
#include <stddef.h>

#define GLD_MAX_RS        256
#define GLD_MAX_STAGES    4
#define GLD_MAX_TSS       32
#define GLD_MAX_XFORMS    512
#define GLD_MAX_LIGHTS    8
#define GLD_MAX_LEVELS    16

/* ── Device state ──────────────────────────────────────────────────── */
typedef struct GldState {
    int      initialized;
    UINT     width, height;           /* scene target (render size) */
    UINT     guest_w, guest_h;        /* the title's own backbuffer (640x480) */
    UINT     msaa;                    /* samples of the scene target (1 = none) */

    DWORD    rs[GLD_MAX_RS];
    DWORD    tss[GLD_MAX_STAGES][GLD_MAX_TSS];
    D3DMATRIX xforms[GLD_MAX_XFORMS];
    D3DVIEWPORT8 viewport;
    D3DMATERIAL8 material;
    D3DLIGHT8    lights[GLD_MAX_LIGHTS];
    BOOL         light_enable[GLD_MAX_LIGHTS];
    DWORD    vertex_shader, pixel_shader;
    IDirect3DBaseTexture8 *tex[GLD_MAX_STAGES];
    IDirect3DVertexBuffer8 *vb;  UINT vb_stride;
    IDirect3DIndexBuffer8  *ib;  UINT ib_base;
    volatile LONG ref;

    /* Scene target: draw_fbo is what the title renders into (multisampled
     * when msaa > 1), scene_fbo/scene_tex the resolved, sampleable image. */
    GLuint   draw_fbo, draw_color_rb, draw_ds_rb;
    GLuint   scene_fbo, scene_tex, scene_ds_rb;
} GldState;

extern GldState g_gld;

/* 1 on the thread that owns the GL context. */
int  gld_on_gl_thread(void);
/* Resolve the multisampled target into scene_tex (no-op without MSAA). */
void gld_scene_resolve(void);
/* Forget the GL state the draw path believes is set (after clears, blits,
 * present): the next draw re-applies everything. */
void gld_state_invalidate(void);

/* Uploads and texture setup bind on unit 7 so the draw cache's bindings of
 * units 0..3 stay valid. */
static inline void gld_scratch_bind(GLuint name)
{
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, name);
}

/* Defined in d3d8_gl_device.c (d3d8_internal.h declares them for _WIN32 only). */
UINT d3d8_GetBackbufferWidth(void);
UINT d3d8_GetBackbufferHeight(void);
/* Bind the title's render target and its viewport (after any blit). */
void gld_bind_draw_target(void);

/* ── Textures (d3d8_gl_resources.c) ────────────────────────────────── */
typedef struct GldTexture {
    IDirect3DTexture8 iface;          /* must be first */
    volatile LONG ref;
    UINT     width, height, levels;
    D3DFORMAT fmt;
    GLuint   name;                    /* 0 until created on the GL thread */
    int      compressed;
    GLenum   ifmt, pfmt, ptype;       /* internal format, upload format/type */
    int      swz;                     /* GLD_SWZ_* channel swizzle */
    UINT     bytes_pp;                /* bytes per pixel (uncompressed) */
    int      swizzled;                /* Xbox swizzled layout: unswizzle on upload */
    /* CPU copies. lvl[0] is the long-lived level-0 copy (LockRect reads it
     * back, as D3D11's sys_mem); other levels live until uploaded. An
     * immutable texture (d3d8_CreateTextureFromLevels) keeps nothing after
     * its upload and refuses LockRect. */
    BYTE    *lvl[GLD_MAX_LEVELS];
    UINT     pitch0;
    unsigned dirty;                   /* bit i: level i waits for upload */
    int      immutable;
    int      locked;
    UINT     lock_level;
    /* Render-to-texture (the previous-frame copy): owned GL storage that is
     * never uploaded from the CPU. */
    int      gpu_only;
    volatile LONG pending_lock;       /* guards lvl[]/dirty across threads */
} GldTexture;

enum { GLD_SWZ_NONE = 0, GLD_SWZ_X1 = 1, GLD_SWZ_A8 = 2 };

/* Create a texture with GL storage made on the GL thread; any thread. */
HRESULT gld_texture_create(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture8 **out);
/* Upload whatever is pending and return the GL name (GL thread only). */
GLuint  gld_texture_ensure(IDirect3DBaseTexture8 *t);
/* A GL-only RGBA8 texture wrapped as IDirect3DTexture8 (GL thread). */
HRESULT gld_texture_create_gpu(UINT w, UINT h, IDirect3DTexture8 **out);
/* Delete GL names queued by releases on other threads (GL thread). */
void    gld_resources_flush_deletes(void);
/* Queue or perform the deletion of a GL texture name (any thread). */
void    gld_delete_texture_name(GLuint name);

HRESULT gld_create_vertex_buffer(UINT len, DWORD usage, DWORD fvf, IDirect3DVertexBuffer8 **out);
HRESULT gld_create_index_buffer(UINT len, DWORD usage, D3DFORMAT fmt, IDirect3DIndexBuffer8 **out);
/* CPU copy of a buffer (stream sources are drawn through the UP path). */
const BYTE *gld_vb_data(IDirect3DVertexBuffer8 *vb, UINT *size);
const BYTE *gld_ib_data(IDirect3DIndexBuffer8 *ib, UINT *size, D3DFORMAT *fmt);

/* ── Drawing (d3d8_gl_draw.c) ──────────────────────────────────────── */
int     gld_draw_init(void);
void    gld_draw_shutdown(void);
/* Apply blend / depth / stencil / raster / scissor / samplers from the
 * render states (d3d8_states_apply's job). */
void    gld_states_apply(void);
/* Fixed-function draw of client vertices in FVF layout. */
HRESULT gld_draw_up(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                    const void *indices, int index32, UINT index_count_hint);
/* Full-screen textured blit into the currently bound framebuffer over the
 * given GL viewport, optionally through the DAC lookup texture. Restores
 * nothing; the caller rebinds. */
void    gld_blit(GLuint tex, int x, int y, int w, int h, GLuint lut_tex, int flip_y);

/* Diagnostics shared with the device. */
extern volatile long g_nv_compiles;
extern volatile double g_nv_compile_ms;

#endif /* D3D8_GL_INTERNAL_H */
