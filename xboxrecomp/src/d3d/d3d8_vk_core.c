/*
 * d3d8_vk_core.c -- the Vulkan backend's machinery (see d3d8_vk_internal.h).
 *
 *   - Instance, device and swapchain: VK_NN_vi_surface on the Switch (NVK
 *     exports every entry point, no loader), SDL_Vulkan on Linux. Vulkan
 *     1.3 with dynamic rendering, push descriptors and dynamic vertex input,
 *     as the NVK ports of other Xbox titles use.
 *   - Frames in flight: VKD_FRAMES slots, each with its command pool and its
 *     upload rings (vertices, indices, uniforms, staging). Every submission
 *     signals a timeline semaphore with its serial; a slot is reused once
 *     the GPU is past its last serial, and anything released is destroyed
 *     once the GPU is past the serial it was last used in.
 *   - Each submission is two command buffers: "pre" (texture uploads, image
 *     setup, recorded whenever needed) and "main" (the frame's rendering).
 *   - Memory: images are sub-allocated from 64 MB blocks (first fit); rings
 *     and staging buffers get memory of their own.
 *   - Shaders: GLSL -> SPIR-V with glslang on a thread with a 4 MB stack
 *     (glslang recurses deeply; the pump thread's stack is smaller on the
 *     Switch). The OpenGL backend's GLSL gets Vulkan bindings first.
 *   - Pipelines: cached by VkdPipeKey; depth, stencil, cull, winding,
 *     topology and vertex layout are dynamic state.
 */
#include "d3d8_vk_internal.h"
#include <SDL.h>
#if !defined(__SWITCH__)
#include <SDL_vulkan.h>
#else
#include <switch.h>
#endif
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VkdCore g_vk;
void (*vkd_on_render_end)(void);

#define VKD_FRAMES 2
#define VKD_CBS    64                   /* command buffers per slot */

static const size_t k_ring_cap[VKD_NRING] = {
    32u << 20,                          /* vertices */
    8u << 20,                           /* indices */
    8u << 20,                           /* uniforms */
    32u << 20,                          /* staging */
};

typedef struct {
    VkBuffer buf;
    VkdMem   mem;
    size_t   cap, off;
} Ring;

typedef struct {
    VkCommandPool   pool;
    VkCommandBuffer cb[VKD_CBS];
    int             ncb, used;
    VkSemaphore     acquire;
    Ring            ring[VKD_NRING];
    uint64_t        last;               /* last serial submitted from this slot */
} Frame;

static void         *s_window;
static SDL_threadID  s_tid;
static int           s_has_tid;
static VkSurfaceKHR  s_surf;
static VkSwapchainKHR s_sc;
static VkImage       s_sc_img[8];
static VkImageView   s_sc_view[8];
static VkSemaphore   s_sc_done[8];
static uint32_t      s_sc_n, s_sc_idx;
static VkExtent2D    s_sc_ext;
static int           s_sync = 1, s_sc_sync = -9, s_sc_stale, s_acquired;
static VkSemaphore   s_tl;
static uint64_t      s_submitted, s_completed;
static Frame         s_fr[VKD_FRAMES];
static int           s_cur = -1;        /* slot of the frame being recorded */
static unsigned      s_frame_no;
static VkCommandBuffer s_main, s_pre;
static int           s_open;            /* s_main / s_pre recording */
static int           s_rendering;       /* 0 none, 1 scene, 2 swapchain */
static VkPipelineCache s_pcache;
static VkPhysicalDeviceMemoryProperties s_memprops;

static void vk_fail(const char *what, VkResult r)
{
    fprintf(stderr, "D3D8: Vulkan %s failed (%d)\n", what, (int)r);
    fflush(stderr);
}

int vkd_on_render_thread(void) { return s_has_tid && SDL_ThreadID() == s_tid; }

/* ================================================================
 * Memory
 * ================================================================ */
#define BLOCK_SIZE ((VkDeviceSize)64 << 20)
typedef struct { VkDeviceSize off, size; } Span;
typedef struct {
    VkDeviceMemory mem;
    uint32_t type;
    VkDeviceSize size;
    Span *free;
    int nfree, capfree;
} Block;
static Block *s_blk;
static int s_nblk;

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static int mem_dedicated(VkDeviceSize size, int type, VkdMem *out)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkResult r;
    ai.allocationSize = size;
    ai.memoryTypeIndex = (uint32_t)type;
    r = vkAllocateMemory(g_vk.dev, &ai, NULL, &out->mem);
    if (r != VK_SUCCESS) { vk_fail("vkAllocateMemory", r); return 0; }
    out->off = 0;
    out->size = size;
    out->block = -1;
    out->map = NULL;
    if (s_memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void *p = NULL;
        if (vkMapMemory(g_vk.dev, out->mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) out->map = (uint8_t *)p;
    }
    return 1;
}

/* Host-visible memory is mapped (and coherent when asked for). */
static int mem_alloc(const VkMemoryRequirements *rq, VkMemoryPropertyFlags want, int dedicated, VkdMem *out)
{
    int type = mem_type(rq->memoryTypeBits, want), b, i;
    if (type < 0 && (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        type = mem_type(rq->memoryTypeBits, want & ~(VkMemoryPropertyFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        type = mem_type(rq->memoryTypeBits, want & ~(VkMemoryPropertyFlags)VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { fprintf(stderr, "D3D8: no Vulkan memory type for 0x%X\n", rq->memoryTypeBits); return 0; }
    if (dedicated || rq->size > BLOCK_SIZE / 2) return mem_dedicated(rq->size, type, out);
    for (b = 0;; b++) {
        Block *k;
        if (b == s_nblk) {              /* a new block */
            VkdMem m;
            Block *n = (Block *)realloc(s_blk, sizeof *s_blk * (size_t)(s_nblk + 1));
            if (!n) return 0;
            s_blk = n;
            if (!mem_dedicated(BLOCK_SIZE, type, &m)) return 0;
            k = &s_blk[s_nblk++];
            memset(k, 0, sizeof *k);
            k->mem = m.mem;
            k->type = (uint32_t)type;
            k->size = BLOCK_SIZE;
            k->free = (Span *)malloc(sizeof(Span) * 16);
            k->capfree = 16;
            k->nfree = 1;
            k->free[0].off = 0;
            k->free[0].size = BLOCK_SIZE;
        }
        k = &s_blk[b];
        if (k->type != (uint32_t)type) continue;
        for (i = 0; i < k->nfree; i++) {
            VkDeviceSize o = (k->free[i].off + rq->alignment - 1) / rq->alignment * rq->alignment;
            VkDeviceSize end = k->free[i].off + k->free[i].size;
            if (o + rq->size > end) continue;
            /* split: [off, o) stays free, [o + size, end) stays free */
            if (o + rq->size < end) {
                if (o > k->free[i].off) {
                    if (k->nfree == k->capfree) {
                        Span *n = (Span *)realloc(k->free, sizeof(Span) * (size_t)k->capfree * 2);
                        if (!n) return 0;
                        k->free = n;
                        k->capfree *= 2;
                    }
                    memmove(&k->free[i + 2], &k->free[i + 1], sizeof(Span) * (size_t)(k->nfree - i - 1));
                    k->free[i + 1].off = o + rq->size;
                    k->free[i + 1].size = end - (o + rq->size);
                    k->free[i].size = o - k->free[i].off;
                    k->nfree++;
                } else {
                    k->free[i].off = o + rq->size;
                    k->free[i].size = end - (o + rq->size);
                }
            } else if (o > k->free[i].off) {
                k->free[i].size = o - k->free[i].off;
            } else {
                memmove(&k->free[i], &k->free[i + 1], sizeof(Span) * (size_t)(k->nfree - i - 1));
                k->nfree--;
            }
            out->mem = k->mem;
            out->off = o;
            out->size = rq->size;
            out->block = b;
            out->map = NULL;
            return 1;
        }
    }
}

static void mem_free(VkdMem *m)
{
    Block *k;
    int i;
    if (!m->mem) return;
    if (m->block < 0) {
        if (m->map) vkUnmapMemory(g_vk.dev, m->mem);
        vkFreeMemory(g_vk.dev, m->mem, NULL);
        memset(m, 0, sizeof *m);
        return;
    }
    k = &s_blk[m->block];
    for (i = 0; i < k->nfree && k->free[i].off < m->off; i++) {}
    /* merge with the span before and/or after */
    if (i > 0 && k->free[i - 1].off + k->free[i - 1].size == m->off) {
        k->free[i - 1].size += m->size;
        if (i < k->nfree && k->free[i - 1].off + k->free[i - 1].size == k->free[i].off) {
            k->free[i - 1].size += k->free[i].size;
            memmove(&k->free[i], &k->free[i + 1], sizeof(Span) * (size_t)(k->nfree - i - 1));
            k->nfree--;
        }
    } else if (i < k->nfree && m->off + m->size == k->free[i].off) {
        k->free[i].off = m->off;
        k->free[i].size += m->size;
    } else {
        if (k->nfree == k->capfree) {
            Span *n = (Span *)realloc(k->free, sizeof(Span) * (size_t)k->capfree * 2);
            if (!n) { memset(m, 0, sizeof *m); return; }       /* leaked */
            k->free = n;
            k->capfree *= 2;
        }
        memmove(&k->free[i + 1], &k->free[i], sizeof(Span) * (size_t)(k->nfree - i));
        k->free[i].off = m->off;
        k->free[i].size = m->size;
        k->nfree++;
    }
    memset(m, 0, sizeof *m);
}

static int buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                         VkBuffer *buf, VkdMem *mem)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements rq;
    VkResult r;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(g_vk.dev, &bi, NULL, buf);
    if (r != VK_SUCCESS) { vk_fail("vkCreateBuffer", r); return 0; }
    vkGetBufferMemoryRequirements(g_vk.dev, *buf, &rq);
    if (!mem_alloc(&rq, want, 1, mem)) { vkDestroyBuffer(g_vk.dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(g_vk.dev, *buf, mem->mem, mem->off);
    return 1;
}

/* ================================================================
 * Deferred destruction
 * ================================================================ */
typedef struct {
    uint64_t serial;
    VkImage image;
    VkImageView view;
    VkBuffer buffer;
    VkdMem mem;
} Retired;
static Retired *s_ret;
static size_t s_nret, s_capret;
static SDL_SpinLock s_xret_lock;        /* releases from other threads */
static Retired *s_xret;
static size_t s_nxret, s_capxret;

static void retire_push(Retired **v, size_t *n, size_t *cap, const Retired *r)
{
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        Retired *nv = (Retired *)realloc(*v, c * sizeof *nv);
        if (!nv) return;                /* leaked, never freed twice */
        *v = nv;
        *cap = c;
    }
    (*v)[(*n)++] = *r;
}

static void retire_destroy(Retired *r)
{
    if (r->view) vkDestroyImageView(g_vk.dev, r->view, NULL);
    if (r->image) vkDestroyImage(g_vk.dev, r->image, NULL);
    if (r->buffer) vkDestroyBuffer(g_vk.dev, r->buffer, NULL);
    mem_free(&r->mem);
}

static void retire_process(void)
{
    size_t i, k = 0;
    if (s_nxret) {
        SDL_AtomicLock(&s_xret_lock);
        for (i = 0; i < s_nxret; i++) {
            /* released elsewhere: the open submission may still use it */
            if (s_xret[i].serial < s_submitted + 1) s_xret[i].serial = s_submitted + 1;
            retire_push(&s_ret, &s_nret, &s_capret, &s_xret[i]);
        }
        s_nxret = 0;
        SDL_AtomicUnlock(&s_xret_lock);
    }
    for (i = 0; i < s_nret; i++) {
        if (vkd_done(s_ret[i].serial)) retire_destroy(&s_ret[i]);
        else s_ret[k++] = s_ret[i];
    }
    s_nret = k;
}

void vkd_image_retire_at(const VkdImage *im, uint64_t serial)
{
    Retired r;
    if (!im || !im->image) return;
    memset(&r, 0, sizeof r);
    r.serial = serial;
    r.image = im->image;
    r.view = im->view;
    r.mem = im->mem;
    if (vkd_on_render_thread()) {
        /* never before the open submission, which may have recorded it */
        if (r.serial < s_submitted + 1) r.serial = s_submitted + 1;
        retire_push(&s_ret, &s_nret, &s_capret, &r);
        return;
    }
    SDL_AtomicLock(&s_xret_lock);
    retire_push(&s_xret, &s_nxret, &s_capxret, &r);
    SDL_AtomicUnlock(&s_xret_lock);
}

void vkd_image_retire(VkdImage *im)
{
    vkd_image_retire_at(im, s_submitted + 1);
    memset(im, 0, sizeof *im);
}

void vkd_buffer_retire(VkBuffer buf, VkdMem *mem)
{
    Retired r;
    memset(&r, 0, sizeof r);
    r.serial = s_submitted + 1;
    r.buffer = buf;
    r.mem = *mem;
    retire_push(&s_ret, &s_nret, &s_capret, &r);
    memset(mem, 0, sizeof *mem);
}

/* ================================================================
 * Submissions
 * ================================================================ */
uint64_t vkd_serial(void) { return s_submitted + 1; }

int vkd_done(uint64_t serial)
{
    if (serial <= s_completed) return 1;
    if (serial > s_submitted) return 0;
    if (vkGetSemaphoreCounterValue(g_vk.dev, s_tl, &s_completed) != VK_SUCCESS) return 0;
    return serial <= s_completed;
}

static void wait_serial(uint64_t serial)
{
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    if (!serial || vkd_done(serial)) return;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s_tl;
    wi.pValues = &serial;
    vkWaitSemaphores(g_vk.dev, &wi, UINT64_MAX);
    if (serial > s_completed) s_completed = serial;
}

static void rings_reset(Frame *f)
{
    int i;
    for (i = 0; i < VKD_NRING; i++) { f->ring[i].off = 0; g_vk.ring_gen[i]++; }
}

static VkCommandBuffer take_cb(Frame *f)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkCommandBuffer cb;
    if (f->used == f->ncb) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cb[f->ncb]) != VK_SUCCESS) return VK_NULL_HANDLE;
        f->ncb++;
    }
    cb = f->cb[f->used++];
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void frame_open(void)
{
    Frame *f;
    if (s_open) return;
    if (s_cur < 0) {                    /* a new frame: recycle its slot */
        s_cur = (int)(s_frame_no % VKD_FRAMES);
        f = &s_fr[s_cur];
        wait_serial(f->last);
        vkResetCommandPool(g_vk.dev, f->pool, 0);
        f->used = 0;
        rings_reset(f);
        retire_process();
    }
    f = &s_fr[s_cur];
    if (f->used + 2 > VKD_CBS) {        /* out of command buffers: drain the slot */
        wait_serial(f->last);
        vkResetCommandPool(g_vk.dev, f->pool, 0);
        f->used = 0;
    }
    s_pre = take_cb(f);
    s_main = take_cb(f);
    s_open = 1;
    g_vk.cb_gen++;
}

VkCommandBuffer vkd_cmd(void) { frame_open(); return s_main; }
VkCommandBuffer vkd_cmd_pre(void) { frame_open(); return s_pre; }
int vkd_rendering(void) { return s_rendering == 1; }

void vkd_render_end(void)
{
    if (!s_rendering) return;
    if (s_rendering == 1 && vkd_on_render_end) vkd_on_render_end();
    vkCmdEndRendering(s_main);
    s_rendering = 0;
    vkd_barrier(s_main);
}

VkCommandBuffer vkd_cmd_outside(void)
{
    frame_open();
    vkd_render_end();
    return s_main;
}

VkCommandBuffer vkd_cmd_render(void)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkdState *s = &g_vkd;
    int ms = s->msaa > 1 && s->ms_color.image;
    frame_open();
    if (s_rendering == 1) return s_main;
    vkd_render_end();
    ca.imageView = ms ? s->ms_color.view : s->scene_color.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.imageView = ms ? s->ms_ds.view : s->scene_ds.view;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = s->width;
    ri.renderArea.extent.height = s->height;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    vkCmdBeginRendering(s_main, &ri);
    s_rendering = 1;
    s->scene_dirty = 1;
    return s_main;
}

static void submit(VkSemaphore wait, VkSemaphore signal)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkTimelineSemaphoreSubmitInfo ti = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore sig[2];
    uint64_t vals[2], serial = s_submitted + 1;
    VkCommandBuffer cbs[2];
    VkResult r;
    vkd_barrier(s_pre);                 /* uploads before everything after */
    vkEndCommandBuffer(s_pre);
    vkEndCommandBuffer(s_main);
    cbs[0] = s_pre;
    cbs[1] = s_main;
    sig[0] = s_tl;
    vals[0] = serial;
    sig[1] = signal;
    vals[1] = 0;
    ti.signalSemaphoreValueCount = signal ? 2 : 1;
    ti.pSignalSemaphoreValues = vals;
    si.pNext = &ti;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &ws;
    }
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = signal ? 2 : 1;
    si.pSignalSemaphores = sig;
    r = vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        vk_fail("vkQueueSubmit", r);
        if (r == VK_ERROR_DEVICE_LOST) { fprintf(stderr, "D3D8: the GPU was lost\n"); exit(1); }
    }
    s_submitted = serial;
    s_fr[s_cur].last = serial;
    s_open = 0;
    s_main = s_pre = VK_NULL_HANDLE;
}

void vkd_flush(int wait)
{
    if (s_open) {
        vkd_render_end();
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    if (wait && s_cur >= 0) {
        Frame *f = &s_fr[s_cur];
        wait_serial(s_submitted);
        vkResetCommandPool(g_vk.dev, f->pool, 0);
        f->used = 0;
        rings_reset(f);
        retire_process();
    }
}

void vkd_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* ================================================================
 * Rings
 * ================================================================ */
size_t vkd_ring_cap(int ring) { return k_ring_cap[ring]; }
VkDeviceSize vkd_ub_align(void)
{
    VkDeviceSize a = g_vk.props.limits.minUniformBufferOffsetAlignment;
    return a < 16 ? 16 : a;
}

int vkd_ring_need(int ring, size_t total, size_t align)
{
    Ring *r;
    size_t o;
    frame_open();
    r = &s_fr[s_cur].ring[ring];
    if (total > r->cap) return 0;
    o = (r->off + align - 1) / align * align;
    if (o + total > r->cap) vkd_flush(1);
    return 1;
}

int vkd_ring_alloc(int ring, size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off, void **ptr)
{
    Ring *r;
    size_t o;
    frame_open();
    r = &s_fr[s_cur].ring[ring];
    if (align < 1) align = 1;
    if (ring == VKD_RING_STAGE && size > r->cap / 2) {
        VkBuffer b;
        VkdMem m;
        if (!buffer_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &b, &m) ||
            !m.map)
            return 0;
        *buf = b;
        *off = 0;
        *ptr = m.map;
        vkd_buffer_retire(b, &m);       /* gone once this submission completes */
        return 1;
    }
    if (size > r->cap) return 0;
    o = (r->off + align - 1) / align * align;
    if (o + size > r->cap) {
        vkd_flush(1);
        frame_open();
        r = &s_fr[s_cur].ring[ring];
        o = 0;
    }
    r->off = o + size;
    *buf = r->buf;
    *off = o;
    *ptr = r->mem.map + o;
    return 1;
}

/* ================================================================
 * Images
 * ================================================================ */
static VkImageAspectFlags format_aspect(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

int vkd_image_create(VkdImage *im, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt,
                     VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                     const VkComponentMapping *swz)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkMemoryRequirements rq;
    VkResult r;
    memset(im, 0, sizeof *im);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = levels ? levels : 1;
    ci.arrayLayers = 1;
    ci.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(g_vk.dev, &ci, NULL, &im->image);
    if (r != VK_SUCCESS) { vk_fail("vkCreateImage", r); im->image = VK_NULL_HANDLE; return 0; }
    vkGetImageMemoryRequirements(g_vk.dev, im->image, &rq);
    if (!mem_alloc(&rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &im->mem)) {
        vkDestroyImage(g_vk.dev, im->image, NULL);
        im->image = VK_NULL_HANDLE;
        return 0;
    }
    vkBindImageMemory(g_vk.dev, im->image, im->mem.mem, im->mem.off);
    im->format = fmt;
    im->w = w;
    im->h = h;
    im->levels = ci.mipLevels;
    im->samples = ci.samples;
    im->aspect = format_aspect(fmt);
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    if (swz) vi.components = *swz;
    /* a depth/stencil view is only an attachment here */
    vi.subresourceRange.aspectMask = im->aspect;
    vi.subresourceRange.levelCount = im->levels;
    vi.subresourceRange.layerCount = 1;
    r = vkCreateImageView(g_vk.dev, &vi, NULL, &im->view);
    if (r != VK_SUCCESS) {
        vk_fail("vkCreateImageView", r);
        vkDestroyImage(g_vk.dev, im->image, NULL);
        mem_free(&im->mem);
        memset(im, 0, sizeof *im);
        return 0;
    }
    /* UNDEFINED -> GENERAL ahead of anything this submission records */
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange = vi.subresourceRange;
    vkCmdPipelineBarrier(vkd_cmd_pre(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    return 1;
}

int vkd_image_upload(int in_main, VkdImage *im, uint32_t level, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, const void *data, size_t size, uint32_t row_texels)
{
    VkCommandBuffer cb;
    VkBuffer buf;
    VkDeviceSize off;
    void *p;
    VkBufferImageCopy c;
    if (!im->image || !size) return 0;
    if (!vkd_ring_alloc(VKD_RING_STAGE, size, 16, &buf, &off, &p)) return 0;
    memcpy(p, data, size);
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.bufferRowLength = row_texels;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.mipLevel = level;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)x;
    c.imageOffset.y = (int32_t)y;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    /* taken after the staging allocation, which may have flushed */
    cb = in_main ? vkd_cmd_outside() : vkd_cmd_pre();
    vkd_barrier(cb);
    vkCmdCopyBufferToImage(cb, buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vkd_barrier(cb);
    return 1;
}

unsigned char *vkd_read_image(VkdImage *im, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkBufferImageCopy c;
    VkCommandBuffer cb;
    VkBuffer buf;
    VkdMem mem;
    unsigned char *out;
    size_t n = (size_t)w * h * 4u;
    if (!im->image || !w || !h || x + w > im->w || y + h > im->h) return NULL;
    if (!buffer_create(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &buf, &mem))
        return NULL;
    if (!mem.map || !(out = (unsigned char *)malloc(n))) {
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        mem_free(&mem);
        return NULL;
    }
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)x;
    c.imageOffset.y = (int32_t)y;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    cb = vkd_cmd_outside();
    vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkd_flush(1);
    memcpy(out, mem.map, n);
    vkDestroyBuffer(g_vk.dev, buf, NULL);
    mem_free(&mem);
    return out;
}

int vkd_format_ok(VkFormat f, VkFormatFeatureFlags need, int buffer)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return ((buffer ? p.bufferFeatures : p.optimalTilingFeatures) & need) == need;
}

VkFormat vkd_depth_format(void)
{
    static const VkFormat c[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    unsigned i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++)
        if (vkd_format_ok(c[i], VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, 0)) return c[i];
    return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

uint32_t vkd_max_samples(void)
{
    VkSampleCountFlags f = g_vk.props.limits.framebufferColorSampleCounts &
                           g_vk.props.limits.framebufferDepthSampleCounts &
                           g_vk.props.limits.framebufferStencilSampleCounts;
    if (f & VK_SAMPLE_COUNT_8_BIT) return 8;
    if (f & VK_SAMPLE_COUNT_4_BIT) return 4;
    if (f & VK_SAMPLE_COUNT_2_BIT) return 2;
    return 1;
}

/* ================================================================
 * Shaders: GLSL -> SPIR-V
 * ================================================================ */
typedef struct { char *p; size_t n, cap; } Str;

static void str_add(Str *s, const char *t, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        size_t c = (s->cap ? s->cap * 2 : 4096);
        char *np;
        while (c < s->n + n + 1) c *= 2;
        np = (char *)realloc(s->p, c);
        if (!np) return;
        s->p = np;
        s->cap = c;
    }
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}

static void str_addz(Str *s, const char *t) { str_add(s, t, strlen(t)); }

static int starts(const char *line, size_t n, const char *pre)
{
    size_t k = strlen(pre);
    return n >= k && !memcmp(line, pre, k);
}

/* The OpenGL backend's GLSL 330 with Vulkan bindings: uniform blocks and
 * samplers in set 0 (PshConsts 0, NvConsts 1, NvParams 2, texN 3 + N), the
 * NvVary blocks at location 0, gl_FragCoord already upper-left, and (vertex
 * shaders) D3D's top-down image and [0, w] depth from the GL clip space. */
static char *glsl_for_vulkan(const char *src, int len, int fragment)
{
    Str o = { NULL, 0, 0 };
    const char *p = src, *end = src + len;
    if (len >= 12 && !memcmp(src, "#version 450", 12)) {
        str_add(&o, src, (size_t)len);
        return o.p;
    }
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (starts(p, n, "#version")) {
            str_addz(&o, "#version 450\n");
        } else if (starts(p, n, "layout(origin_upper_left) in vec4 gl_FragCoord;")) {
            /* Vulkan's origin is the upper left */
        } else if (starts(p, n, "layout(std140) uniform ")) {
            const char *name = p + strlen("layout(std140) uniform ");
            int binding = starts(name, (size_t)(p + n - name), "NvConsts") ? 1
                        : starts(name, (size_t)(p + n - name), "NvParams") ? 2 : 0;
            char b[96];
            snprintf(b, sizeof b, "layout(std140, set = 0, binding = %d) uniform ", binding);
            str_addz(&o, b);
            str_add(&o, name, (size_t)(p + n - name));
            str_addz(&o, "\n");
        } else if (starts(p, n, "uniform sampler2D tex") && n > 21 && p[21] >= '0' && p[21] <= '3') {
            char b[96];
            snprintf(b, sizeof b, "layout(set = 0, binding = %d) ", VKD_B_TEX0 + (p[21] - '0'));
            str_addz(&o, b);
            str_add(&o, p, n);
            str_addz(&o, "\n");
        } else if (starts(p, n, "in NvVary {") || starts(p, n, "out NvVary {")) {
            str_addz(&o, "layout(location = 0) ");
            str_add(&o, p, n);
            str_addz(&o, "\n");
        } else {
            str_add(&o, p, n);
            str_addz(&o, "\n");
        }
        p += n + 1;
    }
    if (!fragment && o.p) {
        char *last = strrchr(o.p, '}');
        if (last) {
            size_t at = (size_t)(last - o.p);
            static const char conv[] =
                "    gl_Position.y = -gl_Position.y;\n"
                "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n"
                "    gl_PointSize = 1.0;\n";
            Str t = { NULL, 0, 0 };
            str_add(&t, o.p, at);
            str_addz(&t, conv);
            str_addz(&t, o.p + at);
            free(o.p);
            o = t;
        }
    }
    return o.p;
}

/* glslang, and NVK's own shader compiler (pipelines, device setup), run on
 * this thread: they need far more stack than the Switch game thread has left
 * by the time the game reaches them. */
static struct {
    SDL_Thread *thr;
    SDL_sem    *go, *done;
    void      (*fn)(void *);
    void       *arg;
    int         fragment;
    const char *src;
    uint32_t   *spv;
    size_t      nwords;
    char        log[4096];
} s_job;

static void compile_job(void *unused)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr = NULL;
    const glslang_stage_t st = s_job.fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    (void)unused;
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = st;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = s_job.src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    in.resource = glslang_default_resource();
    s_job.spv = NULL;
    s_job.nwords = 0;
    s_job.log[0] = 0;
    sh = glslang_shader_create(&in);
    if (!sh) return;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        snprintf(s_job.log, sizeof s_job.log, "%s", glslang_shader_get_info_log(sh));
        glslang_shader_delete(sh);
        return;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        snprintf(s_job.log, sizeof s_job.log, "%s", glslang_program_get_info_log(pr));
    } else {
        glslang_program_SPIRV_generate(pr, st);
        s_job.nwords = glslang_program_SPIRV_get_size(pr);
        s_job.spv = (uint32_t *)malloc(s_job.nwords * 4);
        if (s_job.spv) glslang_program_SPIRV_get(pr, s_job.spv);
        else s_job.nwords = 0;
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
}

static int compile_thread(void *arg)
{
    (void)arg;
    glslang_initialize_process();
    for (;;) {
        SDL_SemWait(s_job.go);
        s_job.fn(s_job.arg);
        SDL_SemPost(s_job.done);
    }
    return 0;
}

/* Runs fn(arg) on the compiler thread and waits for it. */
static int on_big_stack(void (*fn)(void *), void *arg)
{
    if (!s_job.thr) {
        s_job.go = SDL_CreateSemaphore(0);
        s_job.done = SDL_CreateSemaphore(0);
        s_job.thr = SDL_CreateThreadWithStackSize(compile_thread, "vk-compile", 8u << 20, NULL);
        if (!s_job.thr) { fprintf(stderr, "D3D8: no shader compiler thread\n"); return 0; }
    }
    s_job.fn = fn;
    s_job.arg = arg;
    SDL_SemPost(s_job.go);
    SDL_SemWait(s_job.done);
    return 1;
}

VkShaderModule vkd_shader_glsl(int fragment, const char *src, int len, const char *name)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    char *text;
    if (len < 0) len = (int)strlen(src);
    text = glsl_for_vulkan(src, len, fragment);
    if (!text) return VK_NULL_HANDLE;
    s_job.fragment = fragment;
    s_job.src = text;
    if (!on_big_stack(compile_job, NULL)) { free(text); return VK_NULL_HANDLE; }
    if (!s_job.spv) {
        static int failures;
        if (failures++ < 6) {
            fprintf(stderr, "[VK] %s shader failed:\n%s\n", name, s_job.log);
            if (getenv("XBOX_VK_DEBUG")) fprintf(stderr, "%s\n", text);
        }
        free(text);
        return VK_NULL_HANDLE;
    }
    free(text);
    ci.codeSize = s_job.nwords * 4;
    ci.pCode = s_job.spv;
    if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    free(s_job.spv);
    s_job.spv = NULL;
    return m;
}

void vkd_shader_destroy(VkShaderModule m) { if (m) vkDestroyShaderModule(g_vk.dev, m, NULL); }

/* ================================================================
 * Pipelines
 * ================================================================ */
#define PIPE_SLOTS 8192
static struct { VkdPipeKey k; VkPipeline p; int used; } s_pipe[PIPE_SLOTS];
static int s_npipe;

static unsigned pipe_hash(const VkdPipeKey *k)
{
    const unsigned char *b = (const unsigned char *)k;
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof *k; i++) { h ^= b[i]; h *= 16777619u; }
    return h & (PIPE_SLOTS - 1);
}

struct pipe_call { const VkGraphicsPipelineCreateInfo *pi; VkPipeline *p; VkResult r; };

static void pipe_call_run(void *arg)
{
    struct pipe_call *c = (struct pipe_call *)arg;
    c->r = vkCreateGraphicsPipelines(g_vk.dev, s_pcache, 1, c->pi, NULL, c->p);
}

static VkPipeline pipe_build(const VkdPipeKey *k)
{
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
    };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkFormat cf = k->target == VKD_TARGET_SCENE ? g_vkd.color_fmt : g_vk.sc_format;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = k->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = k->fs;
    st[1].pName = "main";
    ia.topology = k->topo == VKD_TOPO_LINE ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : k->topo == VKD_TOPO_POINT ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.depthClampEnable = k->depth_clamp && g_vk.has_depth_clamp;
    rs.polygonMode = k->poly_line && g_vk.has_fill_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1);
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend;
    ba.srcColorBlendFactor = (VkBlendFactor)k->bs;
    ba.dstColorBlendFactor = (VkBlendFactor)k->bd;
    ba.colorBlendOp = (VkBlendOp)k->bop;
    ba.srcAlphaBlendFactor = (VkBlendFactor)k->bsa;
    ba.dstAlphaBlendFactor = (VkBlendFactor)k->bda;
    ba.alphaBlendOp = (VkBlendOp)k->bop;
    ba.colorWriteMask = k->cmask;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &cf;
    if (k->target == VKD_TARGET_SCENE) {
        ri.depthAttachmentFormat = g_vkd.ds_fmt;
        ri.stencilAttachmentFormat = g_vkd.ds_fmt;
    }
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = NULL;        /* dynamic */
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = g_vk.layout;
    {
        struct pipe_call c = { &pi, &p, VK_ERROR_INITIALIZATION_FAILED };
        on_big_stack(pipe_call_run, &c);
        r = c.r;
    }
    if (r != VK_SUCCESS) {
        static int failures;
        if (failures++ < 6) vk_fail("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    return p;
}

VkPipeline vkd_pipeline(const VkdPipeKey *k)
{
    unsigned h = pipe_hash(k);
    int n;
    for (n = 0; n < PIPE_SLOTS; n++, h = (h + 1) & (PIPE_SLOTS - 1)) {
        if (!s_pipe[h].used) break;
        if (!memcmp(&s_pipe[h].k, k, sizeof *k)) return s_pipe[h].p;
    }
    if (n == PIPE_SLOTS || s_npipe >= PIPE_SLOTS * 3 / 4) {
        /* full: start over (rare; the old ones may still be in flight) */
        vkd_flush(1);
        vkd_pipelines_drop();
        h = pipe_hash(k);
    }
    s_pipe[h].k = *k;
    s_pipe[h].p = pipe_build(k);
    s_pipe[h].used = 1;
    s_npipe++;
    return s_pipe[h].p;
}

void vkd_pipelines_drop(void)
{
    int i;
    vkDeviceWaitIdle(g_vk.dev);
    for (i = 0; i < PIPE_SLOTS; i++)
        if (s_pipe[i].used && s_pipe[i].p) vkDestroyPipeline(g_vk.dev, s_pipe[i].p, NULL);
    memset(s_pipe, 0, sizeof s_pipe);
    s_npipe = 0;
}

/* The driver's pipeline cache on disk (XBOX_VK_PIPECACHE=<path>, default
 * vk_pipeline.cache beside the executable or .nro; "0" for none): NVK compiles
 * every new pipeline from NIR, tens of milliseconds each on the console. */
static char s_pcache_path[512];
static unsigned s_pcache_saved_at;

static void pcache_load(void)
{
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    const char *e = getenv("XBOX_VK_PIPECACHE");
    void *data = NULL;
    long n = 0;
    FILE *f;
    if (e && !strcmp(e, "0")) s_pcache_path[0] = 0;
    else if (e && *e) snprintf(s_pcache_path, sizeof s_pcache_path, "%s", e);
    else {
        char exe[400], *slash;
        if (GetModuleFileNameA(NULL, exe, sizeof exe) && (slash = strrchr(exe, '/')) != NULL) {
            slash[1] = 0;
            snprintf(s_pcache_path, sizeof s_pcache_path, "%svk_pipeline.cache", exe);
        } else {
            snprintf(s_pcache_path, sizeof s_pcache_path, "vk_pipeline.cache");
        }
    }
    if (s_pcache_path[0] && (f = fopen(s_pcache_path, "rb"))) {
        if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) != (size_t)n) {
            free(data);
            data = NULL;
        }
        fclose(f);
    }
    ci.initialDataSize = data ? (size_t)n : 0;
    ci.pInitialData = data;
    if (vkCreatePipelineCache(g_vk.dev, &ci, NULL, &s_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vkCreatePipelineCache(g_vk.dev, &ci, NULL, &s_pcache);
    }
    if (data) fprintf(stderr, "D3D8: Vulkan pipeline cache %s (%ld bytes)\n", s_pcache_path, n);
    free(data);
}

/* Written every 64 new pipelines (there is no clean exit to save at). */
static void pcache_save(int force)
{
    size_t n = 0;
    void *data;
    char tmp[600];
    FILE *f;
    if (!s_pcache || !s_pcache_path[0]) return;
    if (!force && (unsigned)s_npipe < s_pcache_saved_at + 64) return;
    s_pcache_saved_at = (unsigned)s_npipe;
    if (vkGetPipelineCacheData(g_vk.dev, s_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    if (!(data = malloc(n))) return;
    if (vkGetPipelineCacheData(g_vk.dev, s_pcache, &n, data) == VK_SUCCESS) {
        snprintf(tmp, sizeof tmp, "%s.part", s_pcache_path);
        if ((f = fopen(tmp, "wb"))) {
            int ok = fwrite(data, 1, n, f) == n;
            if (fclose(f) != 0) ok = 0;
            if (ok) { remove(s_pcache_path); ok = rename(tmp, s_pcache_path) == 0; }
            if (!ok) remove(tmp);
        }
    }
    free(data);
}

/* ================================================================
 * Swapchain
 * ================================================================ */
void vkd_set_sync(int mode) { s_sync = mode; }

void vkd_drawable_size(int *w, int *h)
{
#if !defined(__SWITCH__)
    if (s_window) { SDL_Vulkan_GetDrawableSize((SDL_Window *)s_window, w, h); return; }
#endif
    *w = (int)s_sc_ext.width;
    *h = (int)s_sc_ext.height;
}

static void swapchain_destroy_views(void)
{
    uint32_t i;
    for (i = 0; i < s_sc_n; i++) {
        if (s_sc_view[i]) vkDestroyImageView(g_vk.dev, s_sc_view[i], NULL);
        s_sc_view[i] = VK_NULL_HANDLE;
    }
}

static int swapchain_build(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkPresentModeKHR modes[16], mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceFormatKHR fmts[64];
    uint32_t nm = 16, nf = 64, i, n;
    VkSwapchainKHR old = s_sc;
    VkResult r;
    int dw = 0, dh = 0;

    vkDeviceWaitIdle(g_vk.dev);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, s_surf, &caps);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, s_surf, &nm, modes);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, s_surf, &nf, fmts);
    if (!g_vk.sc_format) {
        g_vk.sc_format = fmts[0].format;
        for (i = 0; i < nf; i++)
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                g_vk.sc_format = fmts[i].format;
                ci.imageColorSpace = fmts[i].colorSpace;
                break;
            }
        if (g_vk.sc_format == VK_FORMAT_UNDEFINED) g_vk.sc_format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    for (i = 0; i < nf; i++) if (fmts[i].format == g_vk.sc_format) ci.imageColorSpace = fmts[i].colorSpace;
    if (s_sync == 0) {
        for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    } else if (s_sync < 0) {
        for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR) mode = modes[i];
    }
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        s_sc_ext = caps.currentExtent;
    } else {
#if !defined(__SWITCH__)
        if (s_window) SDL_Vulkan_GetDrawableSize((SDL_Window *)s_window, &dw, &dh);
#endif
        s_sc_ext.width = dw > 0 ? (uint32_t)dw : 1280u;
        s_sc_ext.height = dh > 0 ? (uint32_t)dh : 720u;
    }
    if (!s_sc_ext.width || !s_sc_ext.height) return 0;      /* minimised */
    n = caps.minImageCount + 1;
    if (n < 2) n = 2;
    if (n > 3) n = 3;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    if (n < caps.minImageCount) n = caps.minImageCount;
    ci.surface = s_surf;
    ci.minImageCount = n;
    ci.imageFormat = g_vk.sc_format;
    ci.imageExtent = s_sc_ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                    ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &s_sc);
    swapchain_destroy_views();
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    if (r != VK_SUCCESS) { vk_fail("vkCreateSwapchainKHR", r); s_sc = VK_NULL_HANDLE; s_sc_n = 0; return 0; }
    s_sc_n = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, s_sc, &s_sc_n, s_sc_img);
    for (i = 0; i < s_sc_n; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = s_sc_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_vk.sc_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(g_vk.dev, &vi, NULL, &s_sc_view[i]);
        if (!s_sc_done[i]) {
            VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(g_vk.dev, &si, NULL, &s_sc_done[i]);
        }
    }
    s_sc_sync = s_sync;
    s_sc_stale = 0;
    {
        static const char *const names[] = { "immediate", "mailbox", "fifo", "fifo relaxed" };
        fprintf(stderr, "D3D8: Vulkan swapchain %ux%u, %u images, %s, format %d (of %u), transform 0x%X\n",
                s_sc_ext.width, s_sc_ext.height, s_sc_n, (unsigned)mode < 4 ? names[mode] : "?",
                (int)g_vk.sc_format, nf, (unsigned)ci.preTransform);
        fflush(stderr);
    }
    return 1;
}

int vkd_present_begin(uint32_t *w, uint32_t *h)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkResult r;
    int tries;
    s_acquired = 0;
    vkd_cmd_outside();
    if (s_sc_sync != s_sync) s_sc_stale = 1;
    for (tries = 0; tries < 2; tries++) {
        if ((s_sc_stale || !s_sc) && !swapchain_build()) return 0;
        r = vkAcquireNextImageKHR(g_vk.dev, s_sc, UINT64_MAX, s_fr[s_cur].acquire, VK_NULL_HANDLE, &s_sc_idx);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) break;
        if (r != VK_ERROR_OUT_OF_DATE_KHR) { vk_fail("vkAcquireNextImageKHR", r); return 0; }
        s_sc_stale = 1;
    }
    if (tries == 2) return 0;
    s_acquired = 1;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_sc_img[s_sc_idx];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_main, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    ca.imageView = s_sc_view[s_sc_idx];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    ri.renderArea.extent = s_sc_ext;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(s_main, &ri);
    s_rendering = 2;
    *w = s_sc_ext.width;
    *h = s_sc_ext.height;
    return 1;
}

void vkd_present_end(void)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkResult r;
    if (!s_acquired) {                  /* nothing to show: submit the frame's work */
        vkd_flush(0);
        s_frame_no++;
        s_cur = -1;
        return;
    }
    {
        /* XBOX_VK_TESTCOLOR=1: paint the left half magenta, to tell a frame
         * that never reaches the screen from one that arrives black. */
        static int test = -1;
        if (test < 0) { const char *e = getenv("XBOX_VK_TESTCOLOR"); test = e && *e == '1'; }
        if (test) {
            VkClearAttachment a = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { { { 1.0f, 0.0f, 1.0f, 1.0f } } } };
            VkClearRect cr = { { { 0, 0 }, { s_sc_ext.width / 2, s_sc_ext.height } }, 0, 1 };
            vkCmdClearAttachments(s_main, 1, &a, 1, &cr);
        }
    }
    vkCmdEndRendering(s_main);
    s_rendering = 0;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_sc_img[s_sc_idx];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_main, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    submit(s_fr[s_cur].acquire, s_sc_done[s_sc_idx]);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s_sc_done[s_sc_idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_sc;
    pi.pImageIndices = &s_sc_idx;
    r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) s_sc_stale = 1;
    else if (r != VK_SUCCESS) vk_fail("vkQueuePresentKHR", r);
    s_acquired = 0;
    s_frame_no++;
    s_cur = -1;
    pcache_save(0);
}

/* ================================================================
 * Init
 * ================================================================ */
static int has_name(const char *const *v, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (!strcmp(v[i], name)) return 1;
    return 0;
}

static int instance_create(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *exts[16];
    const char *layers[1];
    uint32_t nexts = 0;
    VkResult r;
    const char *val = getenv("XBOX_VK_VALIDATE");
#if defined(__SWITCH__)
    exts[nexts++] = VK_KHR_SURFACE_EXTENSION_NAME;
    exts[nexts++] = VK_NN_VI_SURFACE_EXTENSION_NAME;
#else
    {
        unsigned n = 0;
        const char *sdl[12];
        if (!SDL_Vulkan_GetInstanceExtensions((SDL_Window *)s_window, &n, NULL) || n > 12) {
            fprintf(stderr, "D3D8: SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
            return 0;
        }
        SDL_Vulkan_GetInstanceExtensions((SDL_Window *)s_window, &n, sdl);
        for (; nexts < n; nexts++) exts[nexts] = sdl[nexts];
    }
#endif
    app.pApplicationName = "SSX Tricky";
    app.pEngineName = "OpenTricky";
    app.apiVersion = VK_API_VERSION_1_3;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = nexts;
    ci.ppEnabledExtensionNames = exts;
    if (val && val[0] == '1') {
        layers[0] = "VK_LAYER_KHRONOS_validation";
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
    }
    r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && ci.enabledLayerCount) {
        fprintf(stderr, "D3D8: no Vulkan validation layer\n");
        ci.enabledLayerCount = 0;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) { vk_fail("vkCreateInstance", r); return 0; }
    return 1;
}

static int surface_create(void)
{
#if defined(__SWITCH__)
    VkViSurfaceCreateInfoNN si = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN };
    VkResult r;
    si.window = nwindowGetDefault();
    r = vkCreateViSurfaceNN(g_vk.inst, &si, NULL, &s_surf);
    if (r != VK_SUCCESS) { vk_fail("vkCreateViSurfaceNN", r); return 0; }
    return 1;
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)s_window, g_vk.inst, &s_surf)) {
        fprintf(stderr, "D3D8: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
#endif
}

static int device_create(void)
{
    static const char *const want_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    };
    VkPhysicalDevice devs[16];
    uint32_t nd = 16, i, pick = 0xFFFFFFFFu, qf = 0;
    int best = -1;
    const char *want_dev = getenv("XBOX_VK_DEVICE");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 e2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan12Features e12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features e13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT evi = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkResult r;

    if (vkEnumeratePhysicalDevices(g_vk.inst, &nd, devs) < 0 || !nd) {
        fprintf(stderr, "D3D8: no Vulkan device\n");
        return 0;
    }
    for (i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties qp[16];
        /* static: 130 KB, more than the Switch game thread's stack has left */
        static VkExtensionProperties ep[512];
        static const char *names[512];
        uint32_t nq = 16, ne = 512, q, k;
        int score, ok = 1;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, ep);
        for (k = 0; k < ne; k++) names[k] = ep[k].extensionName;
        for (k = 0; k < sizeof want_exts / sizeof want_exts[0]; k++)
            if (!has_name(names, ne, want_exts[k])) ok = 0;
        if (!ok) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qp);
        for (q = 0; q < nq; q++) {
            VkBool32 pres = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, s_surf, &pres);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) break;
        }
        if (q == nq) continue;
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
        if (want_dev && atoi(want_dev) == (int)i) score = 100;
        if (score > best) { best = score; pick = i; qf = q; }
    }
    if (pick == 0xFFFFFFFFu) {
        fprintf(stderr, "D3D8: no Vulkan 1.3 device with push descriptors and dynamic vertex input\n");
        return 0;
    }
    g_vk.phys = devs[pick];
    g_vk.qfam = qf;
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &s_memprops);
    f2.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &fvi;
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    if (!f13.dynamicRendering || !f12.timelineSemaphore || !fvi.vertexInputDynamicState) {
        fprintf(stderr, "D3D8: the Vulkan device lacks dynamic rendering, timeline semaphores or dynamic vertex input\n");
        return 0;
    }
    e2.pNext = &e12;
    e12.pNext = &e13;
    e13.pNext = &evi;
    e2.features.textureCompressionBC = f2.features.textureCompressionBC;
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.depthClamp = f2.features.depthClamp;
    e2.features.fillModeNonSolid = f2.features.fillModeNonSolid;
    e12.timelineSemaphore = VK_TRUE;
    e12.hostQueryReset = f12.hostQueryReset;
    e2.features.occlusionQueryPrecise = f2.features.occlusionQueryPrecise;
    e12.samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge;
    e13.dynamicRendering = VK_TRUE;
    evi.vertexInputDynamicState = VK_TRUE;
    g_vk.has_bc = f2.features.textureCompressionBC;
    g_vk.has_aniso = f2.features.samplerAnisotropy;
    g_vk.has_depth_clamp = f2.features.depthClamp;
    g_vk.has_fill_line = f2.features.fillModeNonSolid;
    g_vk.has_mirror_once = f12.samplerMirrorClampToEdge;
    g_vk.max_aniso = g_vk.props.limits.maxSamplerAnisotropy;
    g_vk.has_host_query_reset = f12.hostQueryReset;
    g_vk.has_precise_occ = f2.features.occlusionQueryPrecise;
    if (!f12.hostQueryReset) fprintf(stderr, "D3D8: no host query reset: occlusion queries off\n");
    qi.queueFamilyIndex = qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    ci.pNext = &e2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)(sizeof want_exts / sizeof want_exts[0]);
    ci.ppEnabledExtensionNames = want_exts;
    r = vkCreateDevice(g_vk.phys, &ci, NULL, &g_vk.dev);
    if (r != VK_SUCCESS) { vk_fail("vkCreateDevice", r); return 0; }
    vkGetDeviceQueue(g_vk.dev, qf, 0, &g_vk.queue);
    g_vk.push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(g_vk.dev, "vkCmdPushDescriptorSetKHR");
    g_vk.set_vertex_input = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetVertexInputEXT");
    if (!g_vk.push_desc || !g_vk.set_vertex_input) { fprintf(stderr, "D3D8: missing Vulkan entry points\n"); return 0; }
    fprintf(stderr, "D3D8: Vulkan %u.%u.%u on %s (driver 0x%X)%s%s\n",
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
            VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.props.deviceName, g_vk.props.driverVersion,
            g_vk.has_bc ? "" : ", no BC textures", g_vk.has_depth_clamp ? "" : ", no depth clamp");
    return 1;
}

static int layout_create(void)
{
    VkDescriptorSetLayoutBinding b[VKD_NBIND];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange pc;
    int i;
    memset(b, 0, sizeof b);
    for (i = 0; i < VKD_NBIND; i++) {
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = i < VKD_B_TEX0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = VKD_NBIND;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_vk.dsl) != VK_SUCCESS) return 0;
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 16;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &g_vk.dsl;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    return vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &g_vk.layout) == VK_SUCCESS;
}

static int frames_create(void)
{
    VkSemaphoreTypeCreateInfo tt = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    int f, i;
    tt.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tt.initialValue = 0;
    si.pNext = &tt;
    if (vkCreateSemaphore(g_vk.dev, &si, NULL, &s_tl) != VK_SUCCESS) return 0;
    si.pNext = NULL;
    for (f = 0; f < VKD_FRAMES; f++) {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        Frame *fr = &s_fr[f];
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = g_vk.qfam;
        if (vkCreateCommandPool(g_vk.dev, &pi, NULL, &fr->pool) != VK_SUCCESS) return 0;
        if (vkCreateSemaphore(g_vk.dev, &si, NULL, &fr->acquire) != VK_SUCCESS) return 0;
        for (i = 0; i < VKD_NRING; i++) {
            static const VkBufferUsageFlags use[VKD_NRING] = {
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            };
            fr->ring[i].cap = k_ring_cap[i];
            if (!buffer_create(k_ring_cap[i], use[i],
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &fr->ring[i].buf, &fr->ring[i].mem) || !fr->ring[i].mem.map)
                return 0;
        }
    }
    return 1;
}

static int s_device_ok;
static void device_create_run(void *unused) { (void)unused; s_device_ok = device_create(); }

int vkd_core_init(void *window)
{
    s_window = window;
    s_tid = SDL_ThreadID();
    s_has_tid = 1;
    if (!instance_create() || !surface_create() || !on_big_stack(device_create_run, NULL) || !s_device_ok ||
        !layout_create() || !frames_create()) {
        fprintf(stderr, "D3D8: Vulkan setup failed\n");
        return 0;
    }
    pcache_load();
    if (!swapchain_build()) {
        fprintf(stderr, "D3D8: no Vulkan swapchain yet\n");
        s_sc_stale = 1;
    }
    return 1;
}

void vkd_core_shutdown(void)
{
    if (!g_vk.dev) return;
    vkd_flush(1);
    vkDeviceWaitIdle(g_vk.dev);
    pcache_save(1);
    vkd_pipelines_drop();
}
