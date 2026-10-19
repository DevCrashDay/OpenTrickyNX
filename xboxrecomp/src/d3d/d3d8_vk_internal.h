/*
 * d3d8_vk_internal.h -- shared state of the Vulkan backend (non-Windows,
 * XBOX_VULKAN=ON): the Nintendo Switch's NVK (mesa-switch, linked without a
 * loader) and, for testing, any Vulkan 1.3 driver on Linux (lavapipe).
 *
 * The backend mirrors the OpenGL one file for file:
 *   d3d8_vk_core.c       instance, device, swapchain, frames in flight,
 *                        upload rings, memory, GLSL -> SPIR-V (glslang),
 *                        pipelines.
 *   d3d8_vk_device.c     the IDirect3DDevice8 vtable, the factory, the scene
 *                        target and the present chain (d3d8_gl_device.c).
 *   d3d8_vk_resources.c  textures and vertex/index buffers.
 *   d3d8_vk_draw.c       render states, the fixed-function path, the NV2A
 *                        draws and vertex programs (d3d8_gl_draw.c).
 * Shaders are the OpenGL backend's GLSL (nv2a_psh.c, d3d8_gl_vsh.c), given
 * Vulkan bindings by vkd_shader_glsl and compiled with glslang at run time.
 *
 * Images are top-down, as D3D's: unlike the GL backend nothing is flipped.
 * Every image stays in VK_IMAGE_LAYOUT_GENERAL (swapchain images aside) and
 * ordering is by full memory barriers, as the NVK ports do.
 *
 * Every Vulkan call happens on the thread that created the device (the PFIFO
 * pump thread). Resources may be created and released on other threads (the
 * HD texture workers): those paths only touch CPU memory and queue the GPU
 * work for the render thread.
 */
#ifndef D3D8_VK_INTERNAL_H
#define D3D8_VK_INTERNAL_H

#include "d3d8_xbox.h"
#if defined(__SWITCH__)
#  define VK_USE_PLATFORM_VI_NN 1
#endif
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>

#define VKD_MAX_RS        256
#define VKD_MAX_STAGES    4
#define VKD_MAX_TSS       32
#define VKD_MAX_XFORMS    512
#define VKD_MAX_LIGHTS    8
#define VKD_MAX_LEVELS    16

/* ── Memory and images (d3d8_vk_core.c) ────────────────────────────── */
typedef struct VkdMem {
    VkDeviceMemory mem;
    VkDeviceSize   off, size;
    int            block;             /* sub-allocation block, -1 dedicated */
    uint8_t       *map;               /* host-visible memory, else NULL */
} VkdMem;

typedef struct VkdImage {
    VkImage     image;
    VkImageView view;
    VkdMem      mem;
    VkFormat    format;
    uint32_t    w, h, levels;
    VkSampleCountFlagBits samples;
    VkImageAspectFlags    aspect;
} VkdImage;

typedef struct VkdCore {
    VkInstance       inst;
    VkPhysicalDevice phys;
    VkDevice         dev;
    VkQueue          queue;
    uint32_t         qfam;
    VkPhysicalDeviceProperties props;
    int              has_bc, has_aniso, has_mirror_once, has_depth_clamp, has_fill_line;
    int              has_host_query_reset, has_precise_occ;
    float            max_aniso;
    VkDescriptorSetLayout dsl;        /* the one push-descriptor set */
    VkPipelineLayout layout;          /* that set + 16 bytes of push constants */
    PFN_vkCmdPushDescriptorSetKHR push_desc;
    PFN_vkCmdSetVertexInputEXT    set_vertex_input;
    VkFormat         sc_format;       /* swapchain */
    unsigned         cb_gen;          /* bumps when command-buffer state is lost */
    unsigned         ring_gen[4];     /* bumps when a ring starts over */
} VkdCore;

extern VkdCore g_vk;

/* Binding numbers of the set every shader uses. */
enum { VKD_B_UB0 = 0, VKD_B_UB1 = 1, VKD_B_UB2 = 2, VKD_B_TEX0 = 3, VKD_NBIND = 7 };

/* ── Device state ──────────────────────────────────────────────────── */
typedef struct VkdState {
    int      initialized;
    UINT     width, height;           /* scene target (render size) */
    UINT     guest_w, guest_h;        /* the title's own backbuffer (640x480) */
    UINT     msaa;                    /* samples of the scene target (1 = none) */

    DWORD    rs[VKD_MAX_RS];
    DWORD    tss[VKD_MAX_STAGES][VKD_MAX_TSS];
    D3DMATRIX xforms[VKD_MAX_XFORMS];
    D3DVIEWPORT8 viewport;
    D3DMATERIAL8 material;
    D3DLIGHT8    lights[VKD_MAX_LIGHTS];
    BOOL         light_enable[VKD_MAX_LIGHTS];
    DWORD    vertex_shader, pixel_shader;
    IDirect3DBaseTexture8 *tex[VKD_MAX_STAGES];
    IDirect3DVertexBuffer8 *vb;  UINT vb_stride;
    IDirect3DIndexBuffer8  *ib;  UINT ib_base;
    volatile LONG ref;

    /* Scene target: the title renders into ms_* when msaa > 1 (resolved
     * into scene_color), else straight into scene_color / scene_ds. */
    VkdImage scene_color, scene_ds, ms_color, ms_ds;
    VkFormat color_fmt, ds_fmt;
    int      scene_dirty;             /* drawn since the last resolve */
} VkdState;

extern VkdState g_vkd;

/* ── Core (d3d8_vk_core.c) ─────────────────────────────────────────── */
/* Create everything up to the swapchain on `window` (an SDL_Window *, NULL
 * on the Switch). Returns 1 on success. */
int  vkd_core_init(void *window);
void vkd_core_shutdown(void);
/* 1 on the thread that created the device. */
int  vkd_on_render_thread(void);

/* The open command buffer, in any state. */
VkCommandBuffer vkd_cmd(void);
/* The open command buffer outside dynamic rendering (ends it if active). */
VkCommandBuffer vkd_cmd_outside(void);
/* The open command buffer rendering into the scene target. */
VkCommandBuffer vkd_cmd_render(void);
/* The command buffer submitted just before vkd_cmd()'s (uploads, layout
 * setup); never inside rendering. */
VkCommandBuffer vkd_cmd_pre(void);
int  vkd_rendering(void);             /* scene rendering active */
void vkd_render_end(void);
/* Called just before rendering ends (occlusion queries end there). */
extern void (*vkd_on_render_end)(void);

/* Submission serials: the open one, and whether one has completed. */
uint64_t vkd_serial(void);
int  vkd_done(uint64_t serial);
/* Submit what is recorded; wait for the GPU when `wait`. Recording goes on
 * in a fresh command buffer. */
void vkd_flush(int wait);

/* Upload rings, one set per frame in flight. */
enum { VKD_RING_VB, VKD_RING_IB, VKD_RING_UB, VKD_RING_STAGE, VKD_NRING };
/* `size` bytes at a multiple of `align`; flushes (and waits) when the ring
 * is full. Staging larger than half the ring gets a buffer of its own. */
int  vkd_ring_alloc(int ring, size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off, void **ptr);
/* Make sure `total` bytes (in `n` allocations of `align`) fit in the ring
 * without a flush, so a draw's allocations share one stretch. */
int  vkd_ring_need(int ring, size_t total, size_t align);
size_t vkd_ring_cap(int ring);
VkDeviceSize vkd_ub_align(void);

/* Images (GENERAL layout from the open submission on). */
int  vkd_image_create(VkdImage *im, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt,
                      VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                      const VkComponentMapping *swz);
/* Destroy once the GPU is past everything recorded so far (render thread). */
void vkd_image_retire(VkdImage *im);
void vkd_buffer_retire(VkBuffer buf, VkdMem *mem);
/* Destroy after the GPU is past `serial` and the open submission; any thread. */
void vkd_image_retire_at(const VkdImage *im, uint64_t serial);
/* Copy rows of `row_texels` texels (0: tightly packed) into a level: in the
 * main command buffer (ending rendering) when `in_main`, so it lands between
 * the draws recorded so far and the next ones, else ahead of them all. */
int  vkd_image_upload(int in_main, VkdImage *im, uint32_t level, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, const void *data, size_t size, uint32_t row_texels);
/* Texels of a colour image region (top-down rows, the image's own bytes),
 * malloc'd; waits for the GPU. NULL on failure. */
unsigned char *vkd_read_image(VkdImage *im, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* A full barrier: everything before against everything after. */
void vkd_barrier(VkCommandBuffer cb);
VkFormat vkd_depth_format(void);
int  vkd_format_ok(VkFormat f, VkFormatFeatureFlags need, int buffer);
uint32_t vkd_max_samples(void);

/* GLSL from the OpenGL backend's generators (or written for Vulkan when it
 * starts with "#version 450") -> shader module; 0 on failure. */
VkShaderModule vkd_shader_glsl(int fragment, const char *src, int len, const char *name);
void vkd_shader_destroy(VkShaderModule m);

/* Pipelines: everything else is dynamic state. */
enum { VKD_TOPO_TRI, VKD_TOPO_LINE, VKD_TOPO_POINT };
enum { VKD_TARGET_SCENE, VKD_TARGET_SWAPCHAIN };
/* memset a key before filling it: it is hashed and compared bytewise. */
typedef struct VkdPipeKey {
    VkShaderModule vs, fs;
    uint8_t topo, poly_line, depth_clamp, samples;
    uint8_t blend, bop, bs, bd, bsa, bda, cmask, target;
} VkdPipeKey;
VkPipeline vkd_pipeline(const VkdPipeKey *k);
void vkd_pipelines_drop(void);        /* on a sample-count change */

/* Present: end scene work, take a swapchain image and render into it. */
int  vkd_present_begin(uint32_t *w, uint32_t *h);
void vkd_present_end(void);
/* Swapchain sync mode: 0 off, 1 vsync, -1 adaptive. */
void vkd_set_sync(int mode);
void vkd_drawable_size(int *w, int *h);

/* ── Textures (d3d8_vk_resources.c) ────────────────────────────────── */
typedef struct VkdTexture {
    IDirect3DTexture8 iface;          /* must be first */
    volatile LONG ref;
    UINT     width, height, levels;
    D3DFORMAT fmt;
    VkdImage img;                     /* image == 0 until created on the render thread */
    int      have_img;
    int      compressed;
    VkFormat vkfmt;
    VkComponentMapping swz;
    UINT     bytes_pp;                /* bytes per pixel (uncompressed source) */
    int      convert;                 /* VKD_CONV_*: source -> image texels */
    int      swizzled;                /* Xbox swizzled layout: unswizzle on upload */
    BYTE    *lvl[VKD_MAX_LEVELS];     /* see GldTexture */
    UINT     pitch0;
    unsigned dirty;
    int      immutable;
    int      locked;
    UINT     lock_level;
    int      gpu_only;
    volatile LONG pending_lock;
    uint64_t last_use;                /* serial of the last draw that read it */
} VkdTexture;

enum { VKD_CONV_NONE = 0, VKD_CONV_TO_BGRA8 = 1 };

HRESULT vkd_texture_create(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture8 **out);
/* Upload whatever is pending; the image view, or VK_NULL_HANDLE. */
VkImageView vkd_texture_ensure(IDirect3DBaseTexture8 *t);
/* A GPU-only texture in the scene format (render thread). */
HRESULT vkd_texture_create_gpu(UINT w, UINT h, IDirect3DTexture8 **out);
VkdImage *vkd_texture_image(IDirect3DTexture8 *t);
/* Releases queued by other threads (render thread). */
void    vkd_resources_flush_deletes(void);

HRESULT vkd_create_vertex_buffer(UINT len, DWORD usage, DWORD fvf, IDirect3DVertexBuffer8 **out);
HRESULT vkd_create_index_buffer(UINT len, DWORD usage, D3DFORMAT fmt, IDirect3DIndexBuffer8 **out);
const BYTE *vkd_vb_data(IDirect3DVertexBuffer8 *vb, UINT *size);
const BYTE *vkd_ib_data(IDirect3DIndexBuffer8 *ib, UINT *size, D3DFORMAT *fmt);

/* ── Device (d3d8_vk_device.c) ─────────────────────────────────────── */
UINT d3d8_GetBackbufferWidth(void);
UINT d3d8_GetBackbufferHeight(void);
void d3d8_GetGuestScale(float *sx, float *sy);
/* Resolve the multisampled target into scene_color (no-op without MSAA). */
void vkd_scene_resolve(void);

/* ── Drawing (d3d8_vk_draw.c) ──────────────────────────────────────── */
int     vkd_draw_init(void);
void    vkd_draw_shutdown(void);
/* Forget the state the draw path believes is set. */
void    vkd_state_invalidate(void);
HRESULT vkd_draw_up(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                    const void *indices, int index32, UINT index_count_hint);
/* Textured blit over (x, y, w, h) of the current rendering (scene or
 * swapchain), optionally through the DAC lookup image. */
void    vkd_blit(VkdImage *src, int x, int y, int w, int h, VkdImage *lut, int target);

extern volatile long g_nv_compiles;
extern volatile double g_nv_compile_ms;

#endif /* D3D8_VK_INTERNAL_H */
